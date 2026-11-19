// Kernel console output, panics and assertions.
#pragma once

#include <stdarg.h>
#include <stdint.h>

void kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char* fmt, va_list ap);
[[noreturn]] void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Prints return addresses by walking the RBP chain (kernel is built with frame
// pointers). tools/symbolize.py turns them into file:line.
void backtrace(uint64_t rbp, uint64_t rip);

#define KASSERT(cond)                                                            \
  do {                                                                           \
    if (__builtin_expect(!(cond), 0))                                            \
      panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);          \
  } while (0)
