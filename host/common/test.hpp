// Minimal host unit-test harness: TEST(name) { EXPECT(...); }
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace htest {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& what);
struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
int run_all(int argc, char** argv);

}  // namespace htest

#define TEST(name)                                                  \
  static void htest_##name();                                       \
  static ::htest::Registrar htest_reg_##name(#name, htest_##name);  \
  static void htest_##name()

#define EXPECT(cond)                                                \
  do {                                                              \
    if (!(cond)) ::htest::fail(__FILE__, __LINE__, #cond);          \
  } while (0)

#define EXPECT_EQ(a, b)                                                                              \
  do {                                                                                               \
    auto htest_a_ = (a);                                                                             \
    auto htest_b_ = (b);                                                                             \
    if (!(htest_a_ == htest_b_))                                                                     \
      ::htest::fail(__FILE__, __LINE__,                                                              \
                    std::string(#a " == " #b " (") + std::to_string(static_cast<long long>(htest_a_)) + \
                        " vs " + std::to_string(static_cast<long long>(htest_b_)) + ")");           \
  } while (0)

// Stops the current test on failure (use where continuing would crash).
#define ASSERT(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      ::htest::fail(__FILE__, __LINE__, #cond);                     \
      return;                                                       \
    }                                                               \
  } while (0)
