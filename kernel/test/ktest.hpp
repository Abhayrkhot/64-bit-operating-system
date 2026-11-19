// In-kernel tests. Each KTEST lands in the .ktests section; `test=<filter>` on
// the command line runs every test whose name starts with the filter ("all"
// runs everything), prints one line per test and exits QEMU with the result.
//
//   KTEST(pmm_alloc_free) { KEXPECT(pmm::free_pages() > 0); }
#pragma once

#include <stdint.h>

namespace ktest {

struct Test {
  const char* name;
  void (*fn)();
};

void fail(const char* file, int line, const char* expr);
[[noreturn]] void run(const char* filter);
// Name of the test being run, or nullptr.
const char* current();

}  // namespace ktest

#define KTEST(name)                                                                        \
  static void ktest_fn_##name();                                                           \
  [[gnu::used, gnu::section(".ktests")]] static const ::ktest::Test ktest_desc_##name = { \
      #name, ktest_fn_##name};                                                             \
  static void ktest_fn_##name()

#define KEXPECT(cond)                                                \
  do {                                                               \
    if (!(cond)) ::ktest::fail(__FILE__, __LINE__, #cond);           \
  } while (0)

#define KEXPECT_EQ(a, b)                                                                        \
  do {                                                                                          \
    auto ktest_a_ = (a);                                                                        \
    auto ktest_b_ = (b);                                                                        \
    if (!(ktest_a_ == ktest_b_)) {                                                              \
      kprintf("    %s = %ld, %s = %ld\n", #a, static_cast<long>(ktest_a_), #b,                  \
              static_cast<long>(ktest_b_));                                                     \
      ::ktest::fail(__FILE__, __LINE__, #a " == " #b);                                          \
    }                                                                                           \
  } while (0)
