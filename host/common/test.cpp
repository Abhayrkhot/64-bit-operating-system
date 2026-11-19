#include "common/test.hpp"

#include <chrono>
#include <cstring>

namespace htest {
namespace {
int g_failures;
}

std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

void fail(const char* file, int line, const std::string& what) {
  g_failures++;
  std::printf("    FAILED %s at %s:%d\n", what.c_str(), file, line);
}

int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0;
  for (const Case& c : registry()) {
    if (std::strncmp(c.name, filter, std::strlen(filter)) != 0) continue;
    int before = g_failures;
    auto start = std::chrono::steady_clock::now();
    c.fn();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (g_failures == before) {
      passed++;
      std::printf("HTEST PASS %s (%.0f ms)\n", c.name, ms);
    } else {
      failed++;
      std::printf("HTEST FAIL %s\n", c.name);
    }
  }
  std::printf("HTEST SUMMARY passed=%d failed=%d\n", passed, failed);
  return failed == 0 && passed > 0 ? 0 : 1;
}

}  // namespace htest
