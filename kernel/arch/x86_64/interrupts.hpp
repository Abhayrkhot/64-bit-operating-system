// IDT, exception handling and interrupt handler registration.
#pragma once

#include <stdint.h>

namespace interrupts {

// Layout must match isr.S.
struct Frame {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t vector, error;
  uint64_t rip, cs, rflags, rsp, ss;  // pushed by the CPU
  bool from_user() const { return (cs & 3) == 3; }
};

using Handler = void (*)(Frame*);

constexpr uint8_t kVectorPageFault = 14;
constexpr uint8_t kVectorIrqBase = 32;
constexpr uint8_t kVectorTimer = 32;
constexpr uint8_t kVectorReschedule = 0xF0;  // self-IPI used by yield()
constexpr uint8_t kVectorSpurious = 0xFF;

void init();
void set_handler(uint8_t vector, Handler h);
// Called for faults the kernel can't resolve; ring-3 faults go to this hook
// (set by the process layer) instead of panicking.
void set_user_fault_hook(void (*hook)(Frame*, const char* what));
void dump_frame(const Frame* f);
const char* exception_name(uint64_t vector);

// Interrupt nesting depth (0 = not in an interrupt handler).
uint32_t depth();

}  // namespace interrupts
