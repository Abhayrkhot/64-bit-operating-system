#include "bench/bench.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/log.hpp"
#include "core/power.hpp"
#include "core/time.hpp"
#include "fmt/format.hpp"
#include "util/sort.hpp"

extern "C" const bench::Bench __kbenches_start[], __kbenches_end[];

namespace bench {
namespace {

// cycles -> "123.4" (ns with one decimal).
void fmt_ns(char* out, size_t size, uint64_t cycles) {
  uint64_t tenths = static_cast<uint64_t>((static_cast<unsigned __int128>(cycles) * 10000000000ull) / time::tsc_hz());
  shale::fmt::format(out, size, "%lu.%lu", tenths / 10, tenths % 10);
}

}  // namespace

void report_cycles(const char* bench, const char* metric, uint64_t* cycles, size_t n) {
  if (n == 0) {
    kprintf("BENCH %s %s n=0\n", bench, metric);
    return;
  }
  shale::heap_sort(cycles, n);
  unsigned __int128 sum = 0;
  for (size_t i = 0; i < n; i++) sum += cycles[i];
  auto pct = [&](uint64_t per_mille) { return cycles[shale::min<size_t>(n - 1, n * per_mille / 1000)]; };
  char mean[24], p50[24], p90[24], p99[24], p999[24], mn[24], mx[24];
  fmt_ns(mean, sizeof(mean), static_cast<uint64_t>(sum / n));
  fmt_ns(p50, sizeof(p50), pct(500));
  fmt_ns(p90, sizeof(p90), pct(900));
  fmt_ns(p99, sizeof(p99), pct(990));
  fmt_ns(p999, sizeof(p999), pct(999));
  fmt_ns(mn, sizeof(mn), cycles[0]);
  fmt_ns(mx, sizeof(mx), cycles[n - 1]);
  kprintf("BENCH %s %s n=%zu mean_ns=%s p50_ns=%s p90_ns=%s p99_ns=%s p999_ns=%s min_ns=%s max_ns=%s\n", bench,
          metric, n, mean, p50, p90, p99, p999, mn, mx);
}

void report_value(const char* bench, const char* metric, const char* unit, uint64_t value) {
  kprintf("BENCH %s %s %s=%lu\n", bench, metric, unit, value);
}

void run(const char* filter) {
  bool all = strcmp(filter, "all") == 0;
  size_t flen = strlen(filter);
  int ran = 0;
  kprintf("BENCH env tsc_mhz=%lu hypervisor=%d\n", time::tsc_hz() / 1000000,
          (arch::cpuid(1).ecx >> 31) & 1);
  for (const Bench* b = __kbenches_start; b < __kbenches_end; b++) {
    if (!all && strncmp(b->name, filter, flen) != 0) continue;
    kprintf("BENCH begin %s\n", b->name);
    b->fn();
    ran++;
  }
  kprintf("BENCH done ran=%d\n", ran);
  power::qemu_exit(ran ? power::ExitCode::kSuccess : power::ExitCode::kFailure);
}

}  // namespace bench
