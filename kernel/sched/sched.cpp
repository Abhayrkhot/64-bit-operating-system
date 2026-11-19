#include "sched/sched.hpp"

#include "arch/x86_64/apic.hpp"
#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/fpu.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "arch/x86_64/percpu.hpp"
#include "common/mem.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "mm/kheap.hpp"
#include "mm/vmm.hpp"

extern "C" void context_switch(uint64_t* save_rsp, uint64_t load_rsp);
extern "C" void thread_trampoline();
extern "C" char boot_stack_top[];

namespace sched {
namespace {

Thread g_boot_thread;
Thread* g_idle;
Thread* g_rq_head;
Thread* g_rq_tail;
uint32_t g_rq_len;
Thread* g_sleepers;  // sorted by wake_ns
Thread* g_all;
Thread* g_switch_prev;  // the thread we just switched away from
int g_thread_cache = -1;
uint32_t g_next_id = 1;
uint32_t g_slice = kDefaultSliceTicks;
bool g_started;
Stats g_stats;

Thread* cur() { return percpu::get()->current; }

void rq_push(Thread* t) {
  t->next = nullptr;
  t->prev = g_rq_tail;
  if (g_rq_tail)
    g_rq_tail->next = t;
  else
    g_rq_head = t;
  g_rq_tail = t;
  g_rq_len++;
}

Thread* rq_pop() {
  Thread* t = g_rq_head;
  if (!t) return nullptr;
  g_rq_head = t->next;
  if (g_rq_head)
    g_rq_head->prev = nullptr;
  else
    g_rq_tail = nullptr;
  t->next = t->prev = nullptr;
  g_rq_len--;
  return t;
}

// Makes a blocked thread runnable. IRQs off.
void ready(Thread* t) {
  KASSERT(t->state == ThreadState::kBlocked);
  t->state = ThreadState::kReady;
  rq_push(t);
  if (cur() == g_idle) percpu::get()->preempt_pending = 1;
}

void finish_switch() {
  Thread* prev = g_switch_prev;
  g_switch_prev = nullptr;
  if (prev && prev->state == ThreadState::kDead && prev->kstack_top) {
    // Safe now: we are on the new thread's stack.
    vmm::free_kernel_stack(prev->kstack_top, kKernelStackPages);
    prev->kstack_top = 0;
    if (prev->fpu) {
      kfree(prev->fpu);
      prev->fpu = nullptr;
    }
  }
}

// Picks the next thread and switches to it. IRQs must be off.
void schedule() {
  KASSERT(!arch::interrupts_enabled());
  Thread* prev = cur();
  if (prev->state == ThreadState::kRunning) {
    prev->state = ThreadState::kReady;
    if (prev != g_idle) rq_push(prev);
  }
  Thread* next = rq_pop();
  if (!next) next = g_idle;
  next->state = ThreadState::kRunning;
  next->slice_left = g_slice;
  percpu::get()->preempt_pending = 0;
  if (next == prev) return;

  uint64_t now = arch::rdtsc();
  prev->run_ns += now - prev->last_run_start;  // cycles; converted when reported
  next->last_run_start = now;
  next->switches_in++;
  g_stats.context_switches++;

  switch_mm_and_stack(prev, next);
  percpu::get()->current = next;
  g_switch_prev = prev;
  context_switch(&prev->saved_rsp, next->saved_rsp);
  finish_switch();  // back on this thread: clean up whoever ran before us
}

void idle_loop(void*) {
  for (;;) asm volatile("sti\n\thlt\n\tcli" ::: "memory");
}

volatile uint64_t g_timer_irq_tsc;

void on_timer(interrupts::Frame*) {
  g_timer_irq_tsc = arch::rdtsc();
  apic::eoi();
  tick();
}

}  // namespace

void switch_mm_and_stack(Thread* prev, Thread* next) {
  if (prev->user_fpu && prev->fpu) fpu::save(prev->fpu);
  if (next->user_fpu && next->fpu) fpu::restore(next->fpu);
  if (next->address_space) {
    if (!next->address_space->active()) next->address_space->activate();
  } else if (prev->state == ThreadState::kDead && prev->address_space) {
    vmm::kernel_space().activate();  // never keep a dying address space loaded
  }
  gdt::set_kernel_stack(next->kstack_top);
  percpu::get()->kernel_rsp = next->kstack_top;
}

void init() {
  g_thread_cache = kcache_create(sizeof(Thread));
  Thread* b = &g_boot_thread;
  b->id = 0;
  b->state = ThreadState::kRunning;
  b->kstack_top = reinterpret_cast<uint64_t>(boot_stack_top);
  memcpy(b->name, "main", 5);
  b->last_run_start = arch::rdtsc();
  b->all_next = nullptr;
  g_all = b;
  percpu::get()->current = b;
  gdt::set_kernel_stack(b->kstack_top);
  percpu::get()->kernel_rsp = b->kstack_top;
  g_idle = create("idle", idle_loop, nullptr);
  g_idle->state = ThreadState::kReady;  // never queued: picked when the run queue is empty
}

void start() {
  interrupts::set_handler(interrupts::kVectorTimer, on_timer);
  apic::start_timer(time::kTickHz, interrupts::kVectorTimer);
  g_started = true;
  arch::sti();
  kprintf("sched: preemptive round-robin, %u ms slices, %u Hz tick\n", g_slice * 1000 / time::kTickHz,
          time::kTickHz);
}

Thread* create(const char* name, void (*entry)(void*), void* arg) {
  auto* t = static_cast<Thread*>(kcache_alloc(g_thread_cache));
  if (!t) return nullptr;
  memset(t, 0, sizeof(*t));
  t->kstack_top = vmm::alloc_kernel_stack(kKernelStackPages);
  if (!t->kstack_top) {
    kfree(t);
    return nullptr;
  }
  size_t n = strlen(name);
  if (n >= sizeof(t->name)) n = sizeof(t->name) - 1;
  memcpy(t->name, name, n);
  t->entry = entry;
  t->arg = arg;
  t->state = ThreadState::kBlocked;
  // Initial frame popped by context_switch, then `ret` into the trampoline.
  // saved_rsp + 56 must be 16-byte aligned when the trampoline starts.
  auto* sp = reinterpret_cast<uint64_t*>(t->kstack_top - 72);
  sp[0] = 0;                                         // r15
  sp[1] = 0;                                         // r14
  sp[2] = reinterpret_cast<uint64_t>(arg);           // r13
  sp[3] = reinterpret_cast<uint64_t>(entry);         // r12
  sp[4] = 0;                                         // rbx
  sp[5] = 0;                                         // rbp
  sp[6] = reinterpret_cast<uint64_t>(thread_trampoline);
  sp[7] = sp[8] = 0;  // end of backtraces
  t->saved_rsp = reinterpret_cast<uint64_t>(sp);
  bool irq = arch::irq_save();
  t->id = g_next_id++;
  t->all_next = g_all;
  g_all = t;
  g_stats.threads_created++;
  arch::irq_restore(irq);
  return t;
}

void make_runnable(Thread* t) {
  bool irq = arch::irq_save();
  ready(t);
  arch::irq_restore(irq);
}

Thread* spawn(const char* name, void (*entry)(void*), void* arg) {
  Thread* t = create(name, entry, arg);
  if (t) make_runnable(t);
  return t;
}

void exit(int code) {
  arch::cli();
  Thread* t = cur();
  KASSERT(t != &g_boot_thread && t != g_idle);
  t->exit_code = code;
  t->state = ThreadState::kDead;
  if (t->joiners) t->joiners->wake_all();
  schedule();
  panic("dead thread %u rescheduled", t->id);
}

int join(Thread* t) {
  WaitQueue wq;
  bool irq = arch::irq_save();
  t->joiners = &wq;
  while (t->state != ThreadState::kDead || t->kstack_top) {
    // kstack_top is cleared by finish_switch once the thread is fully off its stack.
    if (t->state == ThreadState::kDead) {
      arch::irq_restore(irq);
      yield();
      irq = arch::irq_save();
      continue;
    }
    wq.wait();
  }
  t->joiners = nullptr;
  int code = t->exit_code;
  // Unlink from the all-threads list and free.
  for (Thread** p = &g_all; *p; p = &(*p)->all_next) {
    if (*p == t) {
      *p = t->all_next;
      break;
    }
  }
  arch::irq_restore(irq);
  kfree(t);
  return code;
}

Thread* current() { return cur(); }

void yield() {
  bool irq = arch::irq_save();
  schedule();
  arch::irq_restore(irq);
}

void sleep_ns(uint64_t ns) {
  bool irq = arch::irq_save();
  Thread* t = cur();
  t->wake_ns = time::now_ns() + ns;
  Thread** p = &g_sleepers;
  while (*p && (*p)->wake_ns <= t->wake_ns) p = &(*p)->sleep_next;
  t->sleep_next = *p;
  *p = t;
  t->state = ThreadState::kBlocked;
  schedule();
  arch::irq_restore(irq);
}

void tick() {
  time::on_tick();
  if (!g_started) return;
  uint64_t now = time::now_ns();
  while (g_sleepers && g_sleepers->wake_ns <= now) {
    Thread* t = g_sleepers;
    g_sleepers = t->sleep_next;
    t->sleep_next = nullptr;
    ready(t);
  }
  Thread* c = cur();
  if (c == g_idle) {
    if (g_rq_head) percpu::get()->preempt_pending = 1;
  } else if (c->slice_left && --c->slice_left == 0 && g_rq_head) {
    percpu::get()->preempt_pending = 1;
  }
}

void preempt_if_needed() {
  if (!g_started || !percpu::get()->preempt_pending) return;
  if (cur() != g_idle) g_stats.preemptions++;
  schedule();
}

void block_current() { schedule(); }
void make_ready_locked(Thread* t) { ready(t); }
void finish_switch_external() { finish_switch(); }

void set_slice_ticks(uint32_t ticks) { g_slice = ticks ? ticks : 1; }
uint64_t last_timer_irq_tsc() { return g_timer_irq_tsc; }
uint32_t slice_ticks() { return g_slice; }

Stats stats() {
  bool irq = arch::irq_save();
  Stats s = g_stats;
  s.ready = g_rq_len;
  arch::irq_restore(irq);
  return s;
}

}  // namespace sched

extern "C" void sched_thread_start() {
  sched::finish_switch_external();
  arch::sti();
}

extern "C" void sched_thread_exit(int code) { sched::exit(code); }

// ---- wait queues and mutexes ----------------------------------------------

void WaitQueue::wait() {
  KASSERT(!arch::interrupts_enabled());
  Thread* t = sched::current();
  t->state = ThreadState::kBlocked;
  t->next = nullptr;
  t->prev = tail_;
  if (tail_)
    tail_->next = t;
  else
    head_ = t;
  tail_ = t;
  sched::block_current();
}

int WaitQueue::wake_one() {
  bool irq = arch::irq_save();
  Thread* t = head_;
  if (t) {
    head_ = t->next;
    if (head_)
      head_->prev = nullptr;
    else
      tail_ = nullptr;
    t->next = t->prev = nullptr;
    sched::make_ready_locked(t);
  }
  arch::irq_restore(irq);
  return t ? 1 : 0;
}

int WaitQueue::wake_all() {
  int n = 0;
  while (wake_one()) n++;
  return n;
}

void Mutex::lock() {
  bool irq = arch::irq_save();
  KASSERT(owner_ != sched::current());
  while (owner_) waiters_.wait();
  owner_ = sched::current();
  arch::irq_restore(irq);
}

void Mutex::unlock() {
  bool irq = arch::irq_save();
  KASSERT(owner_ == sched::current());
  owner_ = nullptr;
  waiters_.wake_one();
  arch::irq_restore(irq);
}

bool Mutex::held_by_current() const { return owner_ == sched::current(); }
