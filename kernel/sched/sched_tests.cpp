#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "mm/pmm.hpp"
#include "sched/sched.hpp"
#include "test/ktest.hpp"

namespace {

void return_after_flag(void* arg) {
  *static_cast<volatile int*>(arg) = 1;
  sched::exit(7);
}

struct Log {
  char buf[64];
  volatile int len;
};
struct YieldArg {
  Log* log;
  char id;
};
void yield_writer(void* p) {
  auto* a = static_cast<YieldArg*>(p);
  for (int i = 0; i < 4; i++) {
    a->log->buf[a->log->len++] = a->id;
    sched::yield();
  }
}

volatile bool g_stop_spin;
void counter_spin(void* p) {
  auto* c = static_cast<volatile uint64_t*>(p);
  while (!g_stop_spin) (*c)++;
}

struct Shared {
  Mutex m;
  uint64_t counter;
};
void mutex_worker(void* p) {
  auto* s = static_cast<Shared*>(p);
  for (int i = 0; i < 500; i++) {
    s->m.lock();
    uint64_t v = s->counter;
    if (i % 7 == 0) sched::yield();  // hold the lock across a switch
    s->counter = v + 1;
    s->m.unlock();
  }
}

void noop(void*) {}

}  // namespace

KTEST(sched_spawn_and_join) {
  volatile int flag = 0;
  Thread* t = sched::spawn("flag", return_after_flag, const_cast<int*>(&flag));
  KEXPECT(t != nullptr);
  KEXPECT_EQ(sched::join(t), 7);
  KEXPECT_EQ(flag, 1);
}

KTEST(sched_yield_is_round_robin) {
  Log log{};
  YieldArg args[3] = {{&log, 'a'}, {&log, 'b'}, {&log, 'c'}};
  Thread* ts[3];
  for (int i = 0; i < 3; i++) ts[i] = sched::create("rr", yield_writer, &args[i]);
  bool irq = arch::irq_save();  // queue all three before any runs
  for (Thread* t : ts) sched::make_runnable(t);
  arch::irq_restore(irq);
  for (Thread* t : ts) sched::join(t);
  log.buf[log.len] = 0;
  KEXPECT(strcmp(log.buf, "abcabcabcabc") == 0);
  if (strcmp(log.buf, "abcabcabcabc") != 0) kprintf("    log was %s\n", log.buf);
}

KTEST(sched_timer_preempts_spinning_threads) {
  volatile uint64_t c1 = 0, c2 = 0;
  g_stop_spin = false;
  uint64_t p0 = sched::stats().preemptions;
  Thread* a = sched::spawn("spin1", counter_spin, const_cast<uint64_t*>(&c1));
  Thread* b = sched::spawn("spin2", counter_spin, const_cast<uint64_t*>(&c2));
  sched::sleep_ms(100);
  g_stop_spin = true;
  sched::join(a);
  sched::join(b);
  KEXPECT(c1 > 0 && c2 > 0);
  // 100 ms of two spinners with 10 ms slices: several forced switches.
  KEXPECT(sched::stats().preemptions - p0 >= 5);
}

KTEST(sched_sleep_duration) {
  uint64_t t0 = time::now_ns();
  sched::sleep_ms(20);
  uint64_t dt = time::now_ns() - t0;
  KEXPECT(dt >= 20000000);
  KEXPECT(dt < 60000000);
}

KTEST(sched_mutex_excludes_across_switches) {
  Shared s{};
  Thread* ts[4];
  for (auto& t : ts) t = sched::spawn("mutex", mutex_worker, &s);
  for (auto* t : ts) sched::join(t);
  KEXPECT_EQ(s.counter, 2000u);
}

KTEST(sched_many_threads_free_their_stacks) {
  for (int i = 0; i < 8; i++) sched::join(sched::spawn("warm", noop, nullptr));
  uint64_t before = pmm::free_bytes();
  for (int round = 0; round < 4; round++) {
    Thread* ts[100];
    for (auto& t : ts) {
      t = sched::spawn("many", noop, nullptr);
      KEXPECT(t != nullptr);
    }
    for (auto* t : ts) sched::join(t);
  }
  // Thread structs come from a slab cache and stack page tables stay; data frames all return.
  KEXPECT(pmm::free_bytes() + 64 * 4096 >= before);
}

KTEST(time_calibration_matches_tick_rate) {
  uint64_t ticks0 = time::ticks(), ns0 = time::now_ns();
  sched::sleep_ms(200);
  uint64_t ticks = time::ticks() - ticks0, ns = time::now_ns() - ns0;
  // TSC and LAPIC timer were calibrated independently against the PIT.
  uint64_t expected = ns / 1000000;
  KEXPECT(ticks + 5 >= expected && ticks <= expected + 5);
}
