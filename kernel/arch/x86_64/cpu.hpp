// Thin wrappers around x86-64 instructions the kernel needs.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace arch {

inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
inline uint8_t inb(uint16_t port) {
  uint8_t v;
  asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
inline uint16_t inw(uint16_t port) {
  uint16_t v;
  asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
inline uint32_t inl(uint16_t port) {
  uint32_t v;
  asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

inline uint64_t rdtsc() {
  uint32_t lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

// rdtsc ordered against surrounding instructions, for benchmarks.
inline uint64_t rdtsc_fenced() {
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}
inline void wrmsr(uint32_t msr, uint64_t v) {
  asm volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(v)), "d"(static_cast<uint32_t>(v >> 32)));
}

struct CpuidRegs {
  uint32_t eax, ebx, ecx, edx;
};
inline CpuidRegs cpuid(uint32_t leaf, uint32_t subleaf = 0) {
  CpuidRegs r;
  asm volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
  return r;
}

#define CR_ACCESSORS(n)                                  \
  inline uint64_t read_cr##n() {                               \
    uint64_t v;                                                \
    asm volatile("mov %%cr" #n ", %0" : "=r"(v));              \
    return v;                                                  \
  }                                                            \
  inline void write_cr##n(uint64_t v) { asm volatile("mov %0, %%cr" #n ::"r"(v) : "memory"); }
CR_ACCESSORS(0)
CR_ACCESSORS(2)
CR_ACCESSORS(3)
CR_ACCESSORS(4)
#undef CR_ACCESSORS

inline void invlpg(uint64_t va) { asm volatile("invlpg (%0)" ::"r"(va) : "memory"); }
inline void cli() { asm volatile("cli" ::: "memory"); }
inline void sti() { asm volatile("sti" ::: "memory"); }
inline void hlt() { asm volatile("hlt" ::: "memory"); }
inline void pause() { asm volatile("pause"); }

inline uint64_t read_flags() {
  uint64_t f;
  asm volatile("pushfq\n\tpop %0" : "=r"(f)::"memory");
  return f;
}
inline bool interrupts_enabled() { return read_flags() & (1u << 9); }

// Disables interrupts and returns whether they were on.
inline bool irq_save() {
  bool was = interrupts_enabled();
  cli();
  return was;
}
inline void irq_restore(bool was) {
  if (was) sti();
}

[[noreturn]] inline void halt_forever() {
  for (;;) {
    cli();
    hlt();
  }
}

constexpr uint32_t kMsrEfer = 0xC0000080;
constexpr uint32_t kMsrStar = 0xC0000081;
constexpr uint32_t kMsrLstar = 0xC0000082;
constexpr uint32_t kMsrFmask = 0xC0000084;
constexpr uint32_t kMsrFsBase = 0xC0000100;
constexpr uint32_t kMsrGsBase = 0xC0000101;
constexpr uint32_t kMsrKernelGsBase = 0xC0000102;
constexpr uint32_t kMsrApicBase = 0x1B;

}  // namespace arch
