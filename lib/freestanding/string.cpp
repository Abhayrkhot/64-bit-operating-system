// libc memory/string routines for the kernel and user programs (not host
// builds, which use the real libc). The compiler emits calls to
// memcpy/memset for struct copies, so these must exist and must not recurse.
#include "common/mem.hpp"

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
  void* d = dst;
  asm volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n)::"memory");
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  void* d = dst;
  asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  auto* s = static_cast<const uint8_t*>(src);
  if (d == s || n == 0) return dst;
  if (d < s || d >= s + n) return memcpy(dst, src, n);
  // Overlapping with dst after src: copy backwards.
  const uint8_t* sp = s + n - 1;
  uint8_t* dp = d + n - 1;
  asm volatile("std\n\trep movsb\n\tcld" : "+D"(dp), "+S"(sp), "+c"(n)::"memory");
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  auto* x = static_cast<const uint8_t*>(a);
  auto* y = static_cast<const uint8_t*>(b);
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

int strncmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i] || !a[i]) return static_cast<unsigned char>(a[i]) - static_cast<unsigned char>(b[i]);
  }
  return 0;
}

// C++ runtime hooks. Kernel globals are never destroyed.
void* __dso_handle = nullptr;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
int atexit(void (*)()) { return 0; }
}
