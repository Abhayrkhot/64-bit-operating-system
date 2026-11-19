#include "arch/x86_64/fpu.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"

namespace fpu {
namespace {
FpuState g_clean;
}

void init() {
  uint64_t cr0 = arch::read_cr0();
  cr0 &= ~((1ull << 2) | (1ull << 3));  // EM off, TS off
  cr0 |= 1ull << 1;                     // MP
  arch::write_cr0(cr0);
  arch::write_cr4(arch::read_cr4() | (1ull << 9) | (1ull << 10));  // OSFXSR, OSXMMEXCPT
  asm volatile("fninit");
  uint32_t mxcsr = 0x1F80;
  asm volatile("ldmxcsr %0" ::"m"(mxcsr));
  save(&g_clean);
}

void init_state(FpuState* s) { memcpy(s, &g_clean, sizeof(*s)); }

}  // namespace fpu
