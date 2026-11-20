// Process-level tests: each spawns user programs from the initrd and checks
// how they ended, then that the kernel is intact (allocator invariants, no
// leaked frames).
#include "core/cmdline.hpp"
#include "core/log.hpp"
#include "fmt/format.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"
#include "proc/process.hpp"
#include "sched/sched.hpp"
#include "test/ktest.hpp"
#include "bench/bench.hpp"

namespace {

using namespace os::sys;

int run(const char* const* argv) {
  char path[64];
  os::fmt::format(path, sizeof(path), "/bin/%s", argv[0]);
  int pid = process::spawn(path, argv, process::kernel());
  if (pid < 0) return pid - 1000;
  int status = -1;
  int r = process::wait(process::kernel(), pid, &status);
  return r == pid ? status : -2000;
}

// Lets the reaper and finish_switch catch up before measuring memory.
void settle() {
  for (int i = 0; i < 10; i++) sched::sleep_ms(1);
}

}  // namespace

KTEST(user_hello_runs_and_returns_status) {
  const char* a[] = {"hello", "7", nullptr};
  KEXPECT_EQ(run(a), 7);
  const char* b[] = {"hello", nullptr};
  KEXPECT_EQ(run(b), 0);
}

KTEST(user_faults_kill_only_the_offender) {
  struct Case {
    const char* what;
    int status;
  } cases[] = {
      {"null", 128 + kSigSegv},         {"kernel-read", 128 + kSigSegv}, {"kernel-write", 128 + kSigSegv},
      {"text-write", 128 + kSigSegv},   {"rodata-write", 128 + kSigSegv}, {"stack-exec", 128 + kSigSegv},
      {"heap-exec", 128 + kSigSegv},    {"cli", 128 + kSigSegv},          {"hlt", 128 + kSigSegv},
      {"port-io", 128 + kSigSegv},      {"cr3", 128 + kSigSegv},          {"stack-overflow", 128 + kSigSegv},
      {"ud", 128 + kSigIll},            {"div0", 128 + kSigFpe},          {"noncanonical-syscall", 3},
      {"sgdt", vmm::features().umip ? 128 + kSigSegv : 0},
  };
  for (const Case& c : cases) {
    const char* argv[] = {"fault", c.what, nullptr};
    int status = run(argv);
    if (status != c.status) kprintf("    fault %s: status %d, expected %d\n", c.what, status, c.status);
    KEXPECT_EQ(status, c.status);
  }
  KEXPECT(kheap_check());
}

KTEST(user_address_spaces_are_isolated) {
  const char* a[] = {"isolation", nullptr};
  KEXPECT_EQ(run(a), 0);
}

KTEST(user_syscall_edge_cases) {
  const char* a[] = {"usertest", nullptr};
  KEXPECT_EQ(run(a), 0);
}

KTEST(user_spawn_many_no_leak) {
  const char* a[] = {"hello", nullptr};
  for (int i = 0; i < 5; i++) run(a);  // warm slab caches and stack page tables
  settle();
  uint64_t before = pmm::free_bytes();
  for (int i = 0; i < 100; i++) KEXPECT_EQ(run(a), 0);
  settle();
  KEXPECT_EQ(pmm::free_bytes(), before);
}

KTEST(user_syscall_fuzz_no_crash_no_leak) {
  long iters = cmdline::get_long("fuzz_iters", 200000);
  long seeds = cmdline::get_long("fuzz_seeds", 3);
  char count[24];
  os::fmt::format(count, sizeof(count), "%ld", iters);
  const char* warm[] = {"sysfuzz", "100", "99", nullptr};
  run(warm);
  settle();
  uint64_t before = pmm::free_bytes();
  for (long s = 1; s <= seeds; s++) {
    char seed[24];
    os::fmt::format(seed, sizeof(seed), "%ld", s);
    const char* argv[] = {"sysfuzz", count, seed, nullptr};
    KEXPECT_EQ(run(argv), 0);
  }
  settle();
  KEXPECT(kheap_check());
  uint64_t after = pmm::free_bytes();
  if (after != before) kprintf("    free bytes before %lu after %lu\n", before, after);
  KEXPECT_EQ(after, before);
}

KBENCH(process_switch) {
  const char* a[] = {"ctxbench", "bench", "100000", nullptr};
  int status = run(a);
  if (status != 0) kprintf("BENCH process_switch failed status=%d\n", status);
}
