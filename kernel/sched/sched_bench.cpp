// Context-switch benchmarks.
//
// ctxswitch_yield: two kernel threads hand the CPU back and forth with
//   yield(). Each sample is the time from the last instruction before one
//   thread's yield() to the first instruction after the other resumes: the
//   full voluntary switch path (scheduler + register/stack switch).
//
// ctxswitch_preempt: two threads spin without yielding under a 1-tick slice,
//   so every switch is forced by the timer. Each sample is the gap between
//   the last timestamp the preempted thread wrote and the first one the next
//   thread writes: interrupt delivery and entry, EOI, tick bookkeeping,
//   scheduling, the switch and the interrupt return. Under KVM this includes
//   the hypervisor's timer-interrupt injection and EOI exit.
#include "arch/x86_64/cpu.hpp"
#include "bench/bench.hpp"
#include "core/cmdline.hpp"
#include "core/log.hpp"
#include "mm/kheap.hpp"
#include "sched/sched.hpp"

namespace {

constexpr uint32_t kMaxSamples = 200000;
uint64_t* g_samples;
uint64_t* g_delivery;  // preempted thread's last stamp -> timer handler entry
uint64_t* g_kernel;    // timer handler entry -> next thread running
volatile uint32_t g_split_count;
volatile uint32_t g_count;

volatile uint64_t g_stamp;
uint32_t g_iters;

void pingpong(void*) {
  for (uint32_t i = 0; i < g_iters; i++) {
    uint64_t now = arch::rdtsc_fenced();
    uint64_t last = g_stamp;
    if (last && g_count < kMaxSamples) g_samples[g_count++] = now - last;
    g_stamp = arch::rdtsc_fenced();
    sched::yield();
  }
}

volatile uint64_t g_last_seen[2];
volatile int g_last_runner;
volatile bool g_stop;

void spinner(void* arg) {
  int me = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
  while (!g_stop) {
    // Read-compare-store with interrupts off so a preemption can only land
    // between samples (at the sti), never between taking `now` and using it.
    arch::cli();
    uint64_t now = arch::rdtsc_fenced();
    int last = g_last_runner;
    if (last != me) {
      if (last >= 0 && g_count < kMaxSamples) {
        uint64_t prev = g_last_seen[last], irq = sched::last_timer_irq_tsc();
        g_samples[g_count++] = now - prev;
        if (irq > prev && irq < now) {
          g_delivery[g_split_count] = irq - prev;
          g_kernel[g_split_count++] = now - irq;
        }
      }
      g_last_runner = me;
    }
    g_last_seen[me] = now;
    arch::sti();
  }
}

}  // namespace

KBENCH(ctxswitch_yield) {
  g_samples = static_cast<uint64_t*>(kmalloc(kMaxSamples * sizeof(uint64_t)));
  g_iters = static_cast<uint32_t>(cmdline::get_long("iters", 100000));
  g_count = 0;
  g_stamp = 0;
  Thread* a = sched::create("ping", pingpong, nullptr);
  Thread* b = sched::create("pong", pingpong, nullptr);
  uint64_t switches0 = sched::stats().context_switches;
  uint64_t t0 = arch::rdtsc_fenced();
  sched::make_runnable(a);
  sched::make_runnable(b);
  sched::join(a);
  sched::join(b);
  uint64_t elapsed = arch::rdtsc_fenced() - t0;
  uint64_t switches = sched::stats().context_switches - switches0;
  bench::report_value("ctxswitch_yield", "switches", "count", switches);
  bench::report_value("ctxswitch_yield", "elapsed_over_switches", "cycles", elapsed / switches);
  bench::report_cycles("ctxswitch_yield", "switch_latency", g_samples, g_count);
  kfree(g_samples);
}

KBENCH(ctxswitch_preempt) {
  g_samples = static_cast<uint64_t*>(kmalloc(kMaxSamples * sizeof(uint64_t)));
  g_delivery = static_cast<uint64_t*>(kmalloc(kMaxSamples * sizeof(uint64_t)));
  g_kernel = static_cast<uint64_t*>(kmalloc(kMaxSamples * sizeof(uint64_t)));
  g_split_count = 0;
  g_count = 0;
  g_last_runner = -1;
  g_stop = false;
  uint32_t saved_slice = sched::slice_ticks();
  sched::set_slice_ticks(1);
  uint64_t preempt0 = sched::stats().preemptions;
  Thread* a = sched::spawn("spin-a", spinner, reinterpret_cast<void*>(0));
  Thread* b = sched::spawn("spin-b", spinner, reinterpret_cast<void*>(1));
  sched::sleep_ms(static_cast<uint64_t>(cmdline::get_long("ms", 3000)));
  g_stop = true;
  sched::join(a);
  sched::join(b);
  sched::set_slice_ticks(saved_slice);
  // Drop the first and last samples: they straddle the benchmark thread itself.
  uint32_t n = g_count > 4 ? g_count - 2 : 0;
  bench::report_value("ctxswitch_preempt", "preemptions", "count", sched::stats().preemptions - preempt0);
  bench::report_cycles("ctxswitch_preempt", "switch_latency", g_samples + 1, n);
  bench::report_cycles("ctxswitch_preempt", "irq_delivery", g_delivery, g_split_count);
  bench::report_cycles("ctxswitch_preempt", "kernel_path", g_kernel, g_split_count);
  kfree(g_samples);
  kfree(g_delivery);
  kfree(g_kernel);
}
