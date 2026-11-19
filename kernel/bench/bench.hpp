// In-kernel benchmarks: KBENCH(name) { ... } lands in .kbenches;
// `bench=<prefix|all>` runs them and exits QEMU. Results are printed as
//   BENCH <bench> <metric> n=... mean_ns=... p50_ns=... p99_ns=... ...
// lines that tools/bench.py collects.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bench {

struct Bench {
  const char* name;
  void (*fn)();
};

[[noreturn]] void run(const char* filter);

// Sorts `cycles` in place and prints a summary line (TSC cycles -> ns).
void report_cycles(const char* bench, const char* metric, uint64_t* cycles, size_t n);
// Prints a single value.
void report_value(const char* bench, const char* metric, const char* unit, uint64_t value);

}  // namespace bench

#define KBENCH(name)                                                                            \
  static void kbench_fn_##name();                                                               \
  [[gnu::used, gnu::section(".kbenches")]] static const ::bench::Bench kbench_desc_##name = {  \
      #name, kbench_fn_##name};                                                                 \
  static void kbench_fn_##name()
