#include "arch/x86_64/interrupts.hpp"

#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/extable.hpp"
#include "arch/x86_64/gdt.hpp"
#include "core/log.hpp"
#include "sched/sched.hpp"

extern "C" uint8_t isr_stub_table[];

namespace interrupts {
namespace {

struct [[gnu::packed]] Gate {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t reserved;
};
static_assert(sizeof(Gate) == 16, "IDT gate layout");

struct [[gnu::packed]] Pointer {
  uint16_t limit;
  uint64_t base;
};

alignas(16) Gate g_idt[256];
Handler g_handlers[256];
void (*g_user_fault_hook)(Frame*, const char*);
void (*g_return_to_user_hook)(Frame*);
uint32_t g_depth;

void set_gate(uint8_t vector, uint8_t ist, uint8_t dpl) {
  uint64_t addr = reinterpret_cast<uint64_t>(isr_stub_table + 16 * vector);
  Gate& g = g_idt[vector];
  g.offset_lo = addr & 0xFFFF;
  g.selector = gdt::kKernelCode;
  g.ist = ist;
  g.type_attr = static_cast<uint8_t>(0x8E | (dpl << 5));  // present, 64-bit interrupt gate
  g.offset_mid = (addr >> 16) & 0xFFFF;
  g.offset_hi = static_cast<uint32_t>(addr >> 32);
  g.reserved = 0;
}

const char* const kNames[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection", "page fault", "reserved", "x87 FP", "alignment check",
    "machine check", "SIMD FP", "virtualization", "control protection", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection", "VMM communication", "security", "reserved"};

[[noreturn]] void fatal(Frame* f) {
  uint64_t cr2 = arch::read_cr2();
  kprintf("\n*** %s (vector %lu, error %#lx) in %s mode\n", exception_name(f->vector), f->vector, f->error,
          f->from_user() ? "user" : "kernel");
  if (f->vector == kVectorPageFault)
    kprintf("    fault address %016lx (%s, %s, %s%s)\n", cr2, (f->error & 1) ? "protection" : "not present",
            (f->error & 2) ? "write" : "read", (f->error & 4) ? "user" : "supervisor",
            (f->error & 16) ? ", instruction fetch" : "");
  dump_frame(f);
  backtrace(f->rbp, f->rip);
  panic("unhandled %s", exception_name(f->vector));
}

}  // namespace

const char* exception_name(uint64_t vector) { return vector < 32 ? kNames[vector] : "interrupt"; }

void dump_frame(const Frame* f) {
  kprintf("    rip %016lx cs %04lx rflags %08lx rsp %016lx ss %04lx\n", f->rip, f->cs, f->rflags, f->rsp, f->ss);
  kprintf("    rax %016lx rbx %016lx rcx %016lx rdx %016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
  kprintf("    rsi %016lx rdi %016lx rbp %016lx r8  %016lx\n", f->rsi, f->rdi, f->rbp, f->r8);
  kprintf("    r9  %016lx r10 %016lx r11 %016lx r12 %016lx\n", f->r9, f->r10, f->r11, f->r12);
  kprintf("    r13 %016lx r14 %016lx r15 %016lx cr3 %016lx\n", f->r13, f->r14, f->r15, arch::read_cr3());
}

void init() {
  for (int v = 0; v < 256; v++) set_gate(static_cast<uint8_t>(v), 0, 0);
  set_gate(8, gdt::kIstDoubleFault, 0);
  set_gate(2, gdt::kIstNmi, 0);
  set_gate(18, gdt::kIstMachineCheck, 0);
  Pointer ptr{sizeof(g_idt) - 1, reinterpret_cast<uint64_t>(g_idt)};
  asm volatile("lidt %0" ::"m"(ptr));
}

void set_handler(uint8_t vector, Handler h) { g_handlers[vector] = h; }
void set_user_fault_hook(void (*hook)(Frame*, const char*)) { g_user_fault_hook = hook; }
void set_return_to_user_hook(void (*hook)(Frame*)) { g_return_to_user_hook = hook; }
uint32_t depth() { return g_depth; }

}  // namespace interrupts

extern "C" void interrupt_dispatch(interrupts::Frame* f) {
  using namespace interrupts;
  g_depth++;
  if (Handler h = g_handlers[f->vector]) {
    h(f);
  } else if (f->vector < 32) {
    // Kernel faults inside instructions listed in the exception table (user
    // copies, test probes) resume at their fixup instead of panicking.
    if (!f->from_user() && extable::fixup(f)) {
      // resumed
    } else if (f->from_user() && g_user_fault_hook) {
      g_depth--;  // the hook kills the process and never returns here
      g_user_fault_hook(f, exception_name(f->vector));
    } else {
      fatal(f);
    }
  } else if (f->vector != kVectorSpurious) {
    kprintf("unexpected interrupt vector %lu\n", f->vector);
  }
  g_depth--;
  // Preempt only from the outermost interrupt, and only if the interrupted
  // context could have been interrupted anyway (IF was set).
  if (g_depth == 0 && (f->rflags & (1u << 9))) sched::preempt_if_needed();
  if (f->from_user() && g_return_to_user_hook) g_return_to_user_hook(f);
}
