#include "arch/x86_64/percpu.hpp"

#include "arch/x86_64/cpu.hpp"

namespace percpu {
namespace {
PerCpu g_cpu0;
}

// Must run after gdt::init(): loading GS there clears the GS base.
void init() {
  g_cpu0.self = &g_cpu0;
  arch::wrmsr(arch::kMsrGsBase, reinterpret_cast<uint64_t>(&g_cpu0));
  arch::wrmsr(arch::kMsrKernelGsBase, 0);
}

}  // namespace percpu
