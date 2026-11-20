// Shale user-space C library (tiny): system call wrappers, stdio, malloc.
#pragma once

#include <shale/syscall.hpp>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mem.hpp"

namespace sys = shale::sys;

// Programs define `int main(int argc, char** argv)`. Freestanding C++ doesn't
// treat main specially, so give it C linkage for crt0 to find.
extern "C" int main(int argc, char** argv);

// Raw system calls.
inline int64_t syscall6(uint64_t nr, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
                        uint64_t a4 = 0, uint64_t a5 = 0) {
  int64_t ret;
  register uint64_t r10 asm("r10") = a3;
  register uint64_t r8 asm("r8") = a4;
  register uint64_t r9 asm("r9") = a5;
  asm volatile("syscall"
               : "=a"(ret)
               : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
               : "rcx", "r11", "memory");
  return ret;
}

[[noreturn]] void exit(int code);
inline int64_t write(int fd, const void* buf, uint64_t n) {
  return syscall6(sys::kWrite, static_cast<uint64_t>(fd), reinterpret_cast<uint64_t>(buf), n);
}
inline int64_t read(int fd, void* buf, uint64_t n) {
  return syscall6(sys::kRead, static_cast<uint64_t>(fd), reinterpret_cast<uint64_t>(buf), n);
}
inline void yield() { syscall6(sys::kYield); }
inline int getpid() { return static_cast<int>(syscall6(sys::kGetpid)); }
inline int getppid() { return static_cast<int>(syscall6(sys::kGetppid)); }
inline void sleep_ns(uint64_t ns) { syscall6(sys::kSleepNs, ns); }
inline void sleep_ms(uint64_t ms) { sleep_ns(ms * 1000000); }
inline uint64_t clock_ns() { return static_cast<uint64_t>(syscall6(sys::kClockNs)); }
inline int spawn(const char* path, const char* const* argv) {
  return static_cast<int>(syscall6(sys::kSpawn, reinterpret_cast<uint64_t>(path), reinterpret_cast<uint64_t>(argv)));
}
inline int wait(int pid, int* status) {
  return static_cast<int>(syscall6(sys::kWait, static_cast<uint64_t>(pid), reinterpret_cast<uint64_t>(status)));
}
inline uint64_t brk(uint64_t end) { return static_cast<uint64_t>(syscall6(sys::kBrk, end)); }
inline int64_t mmap(uint64_t len, uint64_t prot) { return syscall6(sys::kMmap, len, prot); }
inline int64_t munmap(uint64_t addr, uint64_t len) { return syscall6(sys::kMunmap, addr, len); }
inline int open(const char* path, uint64_t flags) {
  return static_cast<int>(syscall6(sys::kOpen, reinterpret_cast<uint64_t>(path), flags));
}
inline int close(int fd) { return static_cast<int>(syscall6(sys::kClose, static_cast<uint64_t>(fd))); }
inline int64_t lseek(int fd, int64_t off, uint64_t whence) {
  return syscall6(sys::kLseek, static_cast<uint64_t>(fd), static_cast<uint64_t>(off), whence);
}
inline int stat(const char* path, sys::Stat* st) {
  return static_cast<int>(syscall6(sys::kStat, reinterpret_cast<uint64_t>(path), reinterpret_cast<uint64_t>(st)));
}
inline int mkdir(const char* path) { return static_cast<int>(syscall6(sys::kMkdir, reinterpret_cast<uint64_t>(path))); }
inline int unlink(const char* path) {
  return static_cast<int>(syscall6(sys::kUnlink, reinterpret_cast<uint64_t>(path)));
}
inline int rename(const char* a, const char* b) {
  return static_cast<int>(syscall6(sys::kRename, reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b)));
}
inline int truncate(const char* path, uint64_t len) {
  return static_cast<int>(syscall6(sys::kTruncate, reinterpret_cast<uint64_t>(path), len));
}
inline int fsync(int fd) { return static_cast<int>(syscall6(sys::kFsync, static_cast<uint64_t>(fd))); }
inline int readdir(int fd, sys::DirEntry* e, uint64_t index) {
  return static_cast<int>(syscall6(sys::kReaddir, static_cast<uint64_t>(fd), reinterpret_cast<uint64_t>(e), index));
}
inline int sync() { return static_cast<int>(syscall6(sys::kSync)); }
inline int kill(int pid) { return static_cast<int>(syscall6(sys::kKill, static_cast<uint64_t>(pid))); }
inline int sysinfo(sys::SysInfo* info) {
  return static_cast<int>(syscall6(sys::kSysinfo, reinterpret_cast<uint64_t>(info)));
}

inline uint64_t rdtsc() {
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

// stdio
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int puts(const char* s);
// Reads one line (without the newline) from fd; returns its length or -1 at EOF.
int64_t read_line(int fd, char* buf, uint64_t size);

// malloc
void* malloc(size_t n);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t n);
void free(void* p);

// strings
char* strcpy(char* dst, const char* src);
char* strchr(const char* s, int c);
long atol(const char* s);
const char* errno_name(int64_t err);  // "-ENOENT" style names for negative returns
