#include "arch/x86_64/gdt.hpp"

#include "common/mem.hpp"

namespace gdt {
namespace {

struct [[gnu::packed]] Tss {
  uint32_t reserved0;
  uint64_t rsp[3];
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap_base;
};
static_assert(sizeof(Tss) == 104, "TSS layout");

struct [[gnu::packed]] Pointer {
  uint16_t limit;
  uint64_t base;
};

constexpr uint64_t kIstStackSize = 16384;
alignas(16) uint8_t g_ist_stacks[3][kIstStackSize];
alignas(16) Tss g_tss;
alignas(16) uint64_t g_gdt[8];

}  // namespace

void init() {
  memset(&g_tss, 0, sizeof(g_tss));
  g_tss.iomap_base = sizeof(Tss);  // no I/O bitmap: ring 3 port I/O always faults
  for (int i = 0; i < 3; i++) g_tss.ist[i] = reinterpret_cast<uint64_t>(g_ist_stacks[i] + kIstStackSize);

  g_gdt[0] = 0;
  g_gdt[1] = 0x00AF9A000000FFFFull;  // kernel code: present, DPL0, exec/read, long mode
  g_gdt[2] = 0x00CF92000000FFFFull;  // kernel data
  g_gdt[3] = 0x00CFFA000000FFFFull;  // user code, 32-bit (sysret base slot)
  g_gdt[4] = 0x00CFF2000000FFFFull;  // user data, DPL3
  g_gdt[5] = 0x00AFFA000000FFFFull;  // user code, long mode, DPL3
  uint64_t base = reinterpret_cast<uint64_t>(&g_tss);
  uint64_t limit = sizeof(Tss) - 1;
  g_gdt[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((limit >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFF) << 56);  // available 64-bit TSS
  g_gdt[7] = base >> 32;

  Pointer ptr{sizeof(g_gdt) - 1, reinterpret_cast<uint64_t>(g_gdt)};
  asm volatile(
      "lgdt %0\n\t"
      "pushq %1\n\t"
      "leaq 1f(%%rip), %%rax\n\t"
      "pushq %%rax\n\t"
      "lretq\n"
      "1:\n\t"
      "mov %2, %%ds\n\t"
      "mov %2, %%es\n\t"
      "mov %2, %%ss\n\t"
      "xor %%eax, %%eax\n\t"
      "mov %%ax, %%fs\n\t"
      "mov %%ax, %%gs\n\t"
      "ltr %w3\n\t" ::"m"(ptr),
      "i"(static_cast<uint64_t>(kKernelCode)), "r"(static_cast<uint32_t>(kKernelData)), "r"(kTss)
      : "rax", "memory");
}

void set_kernel_stack(uint64_t rsp0) { g_tss.rsp[0] = rsp0; }
uint64_t kernel_stack() { return g_tss.rsp[0]; }

}  // namespace gdt
