#include "proc/usercopy.hpp"

#include <abi/syscall.hpp>

#include "mm/vmm.hpp"

extern "C" int64_t user_copy_smap(void*, const void*, size_t);
extern "C" int64_t user_copy_nosmap(void*, const void*, size_t);
extern "C" int64_t user_strncpy_smap(char*, const char*, size_t);
extern "C" int64_t user_strncpy_nosmap(char*, const char*, size_t);

namespace usercopy {
namespace {
int64_t (*g_copy)(void*, const void*, size_t) = user_copy_nosmap;
int64_t (*g_strncpy)(char*, const char*, size_t) = user_strncpy_nosmap;
}  // namespace

void init() {
  if (vmm::features().smap) {
    g_copy = user_copy_smap;
    g_strncpy = user_strncpy_smap;
  }
}

bool range_ok(uint64_t addr, uint64_t n) {
  uint64_t end;
  if (__builtin_add_overflow(addr, n, &end)) return false;
  return end <= vmm::kUserEnd;
}

int64_t from_user(void* dst, uint64_t src, uint64_t n) {
  if (!range_ok(src, n)) return -os::sys::kEFAULT;
  if (n == 0) return 0;
  return g_copy(dst, reinterpret_cast<const void*>(src), n);
}

int64_t to_user(uint64_t dst, const void* src, uint64_t n) {
  if (!range_ok(dst, n)) return -os::sys::kEFAULT;
  if (n == 0) return 0;
  return g_copy(reinterpret_cast<void*>(dst), src, n);
}

int64_t strncpy_from_user(char* dst, uint64_t src, uint64_t max) {
  if (max == 0) return -os::sys::kENAMETOOLONG;
  // Clamp the scan to the user half so it can never walk into kernel memory.
  if (src >= vmm::kUserEnd) return -os::sys::kEFAULT;
  uint64_t limit = vmm::kUserEnd - src < max ? vmm::kUserEnd - src : max;
  int64_t n = g_strncpy(dst, reinterpret_cast<const char*>(src), limit);
  if (n == -os::sys::kENAMETOOLONG && limit < max) return -os::sys::kEFAULT;
  if (n >= 0 && static_cast<uint64_t>(n) >= max) return -os::sys::kENAMETOOLONG;
  return n;
}

}  // namespace usercopy
