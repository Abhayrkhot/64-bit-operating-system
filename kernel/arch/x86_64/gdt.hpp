// GDT and TSS. Layout is fixed by syscall/sysret, which derive selectors from
// the STAR MSR: kernel CS/SS = STAR[47:32] + {0, 8}; user SS/CS = STAR[63:48] + {8, 16}.
#pragma once

#include <stdint.h>

namespace gdt {

constexpr uint16_t kKernelCode = 0x08;
constexpr uint16_t kKernelData = 0x10;
constexpr uint16_t kUserBase = 0x18;  // 32-bit user code (unused), base for sysret (STAR uses 0x18 | 3)
constexpr uint16_t kUserData = 0x20 | 3;
constexpr uint16_t kUserCode = 0x28 | 3;
constexpr uint16_t kTss = 0x30;

// IST slots (1-based in the IDT gate).
constexpr uint8_t kIstDoubleFault = 1;
constexpr uint8_t kIstNmi = 2;
constexpr uint8_t kIstMachineCheck = 3;

void init();
// Stack the CPU switches to on an interrupt or exception from ring 3.
void set_kernel_stack(uint64_t rsp0);
uint64_t kernel_stack();

}  // namespace gdt
