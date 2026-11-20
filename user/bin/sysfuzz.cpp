// System call fuzzer: random syscall numbers (valid and invalid) with hostile
// arguments: kernel addresses, non-canonical addresses, unmapped and
// read-only user pages, ranges that wrap or straddle the user/kernel
// boundary, huge lengths, random descriptors. The kernel must neither crash
// nor leak; the in-kernel test checks free memory and allocator invariants
// once this process is gone.
//
//   sysfuzz <iterations> [seed]
//
// Calls that would end or stall the fuzzer itself (exit, blocking console
// reads, long sleeps), or make the kernel scribble over the fuzzer's own
// stack and globals, are steered to harmless variants; everything else hits
// the kernel as generated.
#include "shale.hpp"
#include "util/random.hpp"

namespace {

shale::Random* g_rng;
uint64_t g_mapped, g_readonly;
char g_buf[8192];

uint64_t hostile_pointer() {
  switch (g_rng->below(14)) {
    case 0: return 0;
    case 1: return 0xFFFFFFFF80000000ull + g_rng->below(1 << 20);  // kernel image
    case 2: return 0xFFFF800000000000ull + (g_rng->next() & 0xFFFFFFFFFull);  // direct map
    case 3: return 0x0000800000000000ull - g_rng->below(64);  // just below the boundary
    case 4: return 0x0000800000000000ull + g_rng->below(1 << 16);  // non-canonical
    case 5: return g_rng->next();  // anything
    case 6: return g_rng->below(1 << 16);  // low, unmapped
    case 7: return g_readonly + g_rng->below(4096);
    case 8: return (reinterpret_cast<uint64_t>(&hostile_pointer) & ~0xFFFull) + g_rng->below(4096);  // our text
    case 9: return 0x00007FFFFFF00000ull - g_rng->below(1 << 20);  // stack area
    case 10: return 0x00007FFFFFF00000ull - (64 + 1) * 4096 + g_rng->below(4096);  // guard page
    default: return reinterpret_cast<uint64_t>(g_buf) + g_rng->below(sizeof(g_buf) / 2);
  }
}

uint64_t hostile_length() {
  switch (g_rng->below(8)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return g_rng->below(64);
    case 3: return g_rng->below(8192);
    case 4: return ~0ull - g_rng->below(16);
    case 5: return 1ull << (20 + g_rng->below(44));
    case 6: return 0x0000800000000000ull;
    default: return g_rng->below(1 << 16);
  }
}

uint64_t hostile_value() {
  switch (g_rng->below(6)) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return g_rng->below(64);
    case 3: return g_rng->next();
    case 4: return static_cast<uint64_t>(-static_cast<int64_t>(g_rng->below(64)));
    default: return hostile_pointer();
  }
}

uint64_t arg() {
  switch (g_rng->below(3)) {
    case 0: return hostile_pointer();
    case 1: return hostile_length();
    default: return hostile_value();
  }
}

// Calls that make the kernel write to user memory may only target junk
// buffers or addresses that must fault; pointing them at the fuzzer's own
// stack or globals would only crash the fuzzer, not test the kernel.
uint64_t safe_out_pointer(uint64_t p) {
  bool own_writable = p >= 0x400000 && p < 0x0000800000000000ull &&
                      !(p >= g_readonly && p < g_readonly + 4096) &&
                      (p & ~0xFFFull) != (reinterpret_cast<uint64_t>(&hostile_pointer) & ~0xFFFull);
  return own_writable ? reinterpret_cast<uint64_t>(g_buf) + (p & 0xFFF) : p;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iters = argc > 1 ? static_cast<uint64_t>(atol(argv[1])) : 100000;
  uint64_t seed = argc > 2 ? static_cast<uint64_t>(atol(argv[2])) : 1;
  shale::Random rng(seed);
  g_rng = &rng;
  g_mapped = static_cast<uint64_t>(mmap(16 * 4096, sys::kProtRead | sys::kProtWrite));
  g_readonly = static_cast<uint64_t>(mmap(4096, sys::kProtRead));
  uint64_t by_result[64] = {};  // [0] = success, [n] = -errno n
  uint64_t t0 = clock_ns();
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t nr = rng.below(sys::kCount + 8);  // includes invalid numbers
    if (rng.chance(1, 50)) nr = rng.next();    // and wild ones
    uint64_t a[6];
    for (auto& x : a) x = arg();
    switch (nr) {
      case sys::kExit: continue;  // would end the run
      case sys::kRead:
        if (a[0] < 3) a[0] = 3 + rng.below(8);  // no blocking console reads
        a[1] = safe_out_pointer(a[1]);
        break;
      case sys::kWrite:
        if (a[0] < 3) a[0] += 3;  // keep the console clean
        break;
      case sys::kSleepNs: a[0] &= 0xFFF; break;
      case sys::kWait: a[0] = static_cast<uint64_t>(-1); break;  // never block: no children
      case sys::kMunmap:
        if (a[0] == g_mapped || a[0] == g_readonly) a[0] ^= 0x1000;
        break;
      case sys::kClose:
        if (a[0] < 3) a[0] += 3;  // keep stdio for the report
        break;
      case sys::kSysinfo: a[0] = safe_out_pointer(a[0]); break;
      case sys::kStat:
      case sys::kReaddir: a[1] = safe_out_pointer(a[1]); break;
      default: break;
    }
    int64_t r = syscall6(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    if (nr == sys::kSpawn && r > 0) {  // a random spawn that worked: reap it
      int st;
      wait(static_cast<int>(r), &st);
    }
    if (nr == sys::kOpen && r >= 3) close(static_cast<int>(r));
    uint64_t slot = r >= 0 ? 0 : static_cast<uint64_t>(-r);
    by_result[slot < 64 ? slot : 63]++;
  }
  uint64_t dt = clock_ns() - t0;
  printf("sysfuzz: %lu calls in %lu ms (%lu ns/call), seed %lu\n", iters, dt / 1000000, iters ? dt / iters : 0,
         seed);
  printf("sysfuzz: ok=%lu", by_result[0]);
  for (int e = 1; e < 64; e++)
    if (by_result[e]) printf(" %s=%lu", errno_name(-e), by_result[e]);
  printf("\n");
  return 0;
}
