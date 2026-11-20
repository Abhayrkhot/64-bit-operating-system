// Process-level benchmarks, printed as BENCH lines like the in-kernel ones.
//
//   syscall_getpid     round trip of the cheapest system call
//   process_switch     two processes ping-pong with sys_yield: each switch
//                      is syscall entry + scheduler + CR3 switch + FXSAVE /
//                      FXRSTOR + sysret. One sample = round trip / 2.
#include "shale.hpp"
#include "util/sort.hpp"

namespace {

uint64_t g_tsc_hz;

void report(const char* bench, const char* metric, uint64_t* cycles, uint64_t n, uint64_t divisor) {
  shale::heap_sort(cycles, n);
  auto ns10 = [](uint64_t c, uint64_t div) {
    return static_cast<uint64_t>(static_cast<unsigned __int128>(c) * 10000000000ull / (g_tsc_hz * div));
  };
  unsigned __int128 sum = 0;
  for (uint64_t i = 0; i < n; i++) sum += cycles[i];
  uint64_t mean = ns10(static_cast<uint64_t>(sum / n), divisor), p50 = ns10(cycles[n / 2], divisor),
           p99 = ns10(cycles[n * 99 / 100], divisor), mn = ns10(cycles[0], divisor);
  printf("BENCH %s %s n=%lu mean_ns=%lu.%lu p50_ns=%lu.%lu p99_ns=%lu.%lu min_ns=%lu.%lu\n", bench, metric, n,
         mean / 10, mean % 10, p50 / 10, p50 % 10, p99 / 10, p99 % 10, mn / 10, mn % 10);
}

int partner(uint64_t iters) {
  for (uint64_t i = 0; i < iters + 1000; i++) yield();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iters = argc > 2 ? static_cast<uint64_t>(atol(argv[2])) : 100000;
  if (argc > 1 && !strcmp(argv[1], "partner")) return partner(iters);
  sys::SysInfo info;
  sysinfo(&info);
  g_tsc_hz = info.tsc_hz;
  auto* samples = static_cast<uint64_t*>(malloc(iters * sizeof(uint64_t)));

  // Control: the same timestamp pair around no work, to show the
  // measurement overhead on this machine/emulator.
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t t0 = rdtsc();
    samples[i] = rdtsc() - t0;
  }
  report("syscall_getpid", "timer_overhead", samples, iters, 1);
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t t0 = rdtsc();
    getpid();
    samples[i] = rdtsc() - t0;
  }
  report("syscall_getpid", "round_trip", samples, iters, 1);
  // Bulk timing with the kernel clock, independent of user-mode rdtsc.
  uint64_t c0 = clock_ns();
  for (uint64_t i = 0; i < iters; i++) getpid();
  uint64_t bulk = (clock_ns() - c0) * 10 / iters;
  printf("BENCH syscall_getpid bulk_mean ns=%lu.%lu\n", bulk / 10, bulk % 10);

  char count[24];
  snprintf(count, sizeof(count), "%lu", iters);
  const char* args[] = {"ctxbench", "partner", count, nullptr};
  int pid = spawn("/bin/ctxbench", args);
  if (pid < 0) return 1;
  yield();  // let the partner start
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t t0 = rdtsc();
    yield();  // -> partner -> back to us: two process switches
    samples[i] = rdtsc() - t0;
  }
  report("process_switch", "switch_latency", samples, iters, 2);
  int status;
  wait(pid, &status);
  free(samples);
  return 0;
}
