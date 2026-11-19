// Memory/string primitives and placement new for code shared between the
// kernel (freestanding, no libc) and host tools/tests (hosted libc).
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(SHALE_HOSTED)
#include <string.h>

#include <new>
#else
extern "C" {
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
}
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }
#endif

namespace shale {

template <typename T>
constexpr T min(T a, T b) {
  return a < b ? a : b;
}
template <typename T>
constexpr T max(T a, T b) {
  return a < b ? b : a;
}
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
constexpr uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
constexpr bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }

template <typename T>
struct RemoveReference {
  using Type = T;
};
template <typename T>
struct RemoveReference<T&> {
  using Type = T;
};
template <typename T>
struct RemoveReference<T&&> {
  using Type = T;
};
template <typename T>
constexpr typename RemoveReference<T>::Type&& move(T&& v) {
  return static_cast<typename RemoveReference<T>::Type&&>(v);
}
template <typename T>
void swap(T& a, T& b) {
  T t = move(a);
  a = move(b);
  b = move(t);
}

}  // namespace shale
