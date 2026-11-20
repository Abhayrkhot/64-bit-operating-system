// Threads and the preemptive scheduler (single CPU).
//
// Round-robin with a fixed time slice. The LAPIC timer ticks at 1 kHz; when
// the running thread's slice runs out, the tick marks a reschedule, and the
// outermost interrupt return switches threads (kernel code is preemptible
// whenever interrupts are enabled; spinlocks disable them).
//
// A context switch saves callee-saved registers on the old kernel stack,
// swaps stack pointers (switch.S), points the TSS and syscall stack at the new
// thread's kernel stack, switches CR3 if the address space differs and
// saves/restores FPU/SSE state for threads that run user code.
#pragma once

#include <stdint.h>

namespace vmm {
class AddressSpace;
}
struct Process;
class WaitQueue;

enum class ThreadState : uint8_t { kReady, kRunning, kBlocked, kDead };

struct alignas(16) FpuState {
  uint8_t bytes[512];  // FXSAVE image
};

struct Thread {
  uint64_t saved_rsp;  // must stay first: switch.S stores through &thread->saved_rsp
  uint64_t kstack_top;
  uint32_t id;
  ThreadState state;
  bool user_fpu;  // has user context: save/restore FPU on switch
  char name[22];
  Thread* next;  // run queue / wait queue link
  Thread* prev;
  uint64_t wake_ns;  // for sleepers
  Thread* sleep_next;
  uint32_t slice_left;
  int exit_code;
  vmm::AddressSpace* address_space;  // nullptr: kernel thread (borrows whatever CR3 is live)
  Process* process;
  FpuState* fpu;
  void (*entry)(void*);
  void* arg;
  uint64_t switches_in;
  uint64_t run_ns;
  uint64_t last_run_start;
  bool reaped;
  Thread* all_next;  // all-threads list
  WaitQueue* joiners;
};

class WaitQueue {
 public:
  // Blocks the current thread. Interrupts must be disabled by the caller,
  // which checks its condition in a loop around wait().
  void wait();
  // Wakes threads; returns how many. Safe with interrupts on or off.
  int wake_one();
  int wake_all();
  bool empty() const { return head_ == nullptr; }

 private:
  Thread* head_ = nullptr;
  Thread* tail_ = nullptr;
};

// Sleeping mutex (owner may block while holding it; not for IRQ context).
class Mutex {
 public:
  void lock();
  void unlock();
  bool held_by_current() const;

 private:
  Thread* owner_ = nullptr;
  WaitQueue waiters_;
};

namespace sched {

constexpr uint32_t kDefaultSliceTicks = 10;  // 10 ms at 1 kHz
constexpr unsigned kKernelStackPages = 4;    // 16 KiB + guard page

// Turns the boot context into thread 0 and creates the idle thread.
void init();
// Starts the timer; preemption begins.
void start();

Thread* spawn(const char* name, void (*entry)(void*), void* arg);
// Creates a thread without making it runnable (the process layer finishes setup).
Thread* create(const char* name, void (*entry)(void*), void* arg);
void make_runnable(Thread* t);
// Frees a thread from create() that was never made runnable.
void discard(Thread* t);
[[noreturn]] void exit(int code);
// Waits for `t` to exit and frees it. Returns its exit code.
int join(Thread* t);

Thread* current();
void yield();
void sleep_ns(uint64_t ns);
inline void sleep_ms(uint64_t ms) { sleep_ns(ms * 1000000); }

// Called from the timer interrupt and from interrupt exit.
void tick();
void preempt_if_needed();

void set_slice_ticks(uint32_t ticks);
// TSC at entry of the most recent timer interrupt handler (for benchmarks).
uint64_t last_timer_irq_tsc();
uint32_t slice_ticks();

struct Stats {
  uint64_t context_switches;
  uint64_t preemptions;
  uint64_t threads_created;
  uint32_t ready;
};
Stats stats();

// Called by the scheduler with IRQs off right before switching to `next`
// (address space and kernel stack setup lives here).
void switch_mm_and_stack(Thread* prev, Thread* next);

// Internal hooks for wait queues and the new-thread trampoline (IRQs off).
void block_current();               // current is already kBlocked and queued
void make_ready_locked(Thread* t);  // kBlocked -> run queue
void finish_switch_external();

}  // namespace sched
