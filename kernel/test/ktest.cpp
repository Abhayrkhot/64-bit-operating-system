#include "test/ktest.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/log.hpp"
#include "core/power.hpp"

extern "C" const ktest::Test __ktests_start[], __ktests_end[];

namespace ktest {
namespace {
const Test* g_current;
int g_failures_in_test;
}  // namespace

void fail(const char* file, int line, const char* expr) {
  g_failures_in_test++;
  kprintf("    FAILED %s at %s:%d\n", expr, file, line);
}

const char* current() { return g_current ? g_current->name : nullptr; }

void run(const char* filter) {
  bool all = strcmp(filter, "all") == 0;
  size_t flen = strlen(filter);
  int passed = 0, failed = 0;
  for (const Test* t = __ktests_start; t < __ktests_end; t++) {
    if (!all && strncmp(t->name, filter, flen) != 0) continue;
    g_current = t;
    g_failures_in_test = 0;
    uint64_t start = arch::rdtsc();
    t->fn();
    uint64_t cycles = arch::rdtsc() - start;
    if (g_failures_in_test) {
      failed++;
      kprintf("KTEST FAIL %s\n", t->name);
    } else {
      passed++;
      kprintf("KTEST PASS %s (%lu Mcycles)\n", t->name, cycles / 1000000);
    }
  }
  g_current = nullptr;
  kprintf("KTEST SUMMARY passed=%d failed=%d\n", passed, failed);
  power::qemu_exit(failed == 0 && passed > 0 ? power::ExitCode::kSuccess : power::ExitCode::kFailure);
}

}  // namespace ktest
