#include "arch/x86_64/apic.hpp"

#include "arch/x86_64/acpi.hpp"
#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "mm/vmm.hpp"

namespace apic {
namespace {

constexpr uint32_t kRegId = 0x020, kRegEoi = 0x0B0, kRegSvr = 0x0F0, kRegIcrLo = 0x300, kRegIcrHi = 0x310,
                   kRegLvtTimer = 0x320, kRegLvtLint0 = 0x350, kRegLvtLint1 = 0x360, kRegLvtError = 0x370,
                   kRegTimerInit = 0x380, kRegTimerCurrent = 0x390, kRegTimerDivide = 0x3E0;
constexpr uint32_t kLvtMasked = 1u << 16, kTimerPeriodic = 1u << 17;

volatile uint32_t* g_lapic;
volatile uint32_t* g_ioapic;
uint64_t g_timer_hz;

uint32_t rd(uint32_t reg) { return g_lapic[reg / 4]; }
void wr(uint32_t reg, uint32_t v) { g_lapic[reg / 4] = v; }

uint32_t io_rd(uint32_t reg) {
  g_ioapic[0] = reg;
  return g_ioapic[4];
}
void io_wr(uint32_t reg, uint32_t v) {
  g_ioapic[0] = reg;
  g_ioapic[4] = v;
}

void disable_pic() {
  // Remap to 0x20/0x28 first so a stray IRQ can't masquerade as an exception.
  arch::outb(0x20, 0x11);
  arch::outb(0xA0, 0x11);
  arch::outb(0x21, 0x20);
  arch::outb(0xA1, 0x28);
  arch::outb(0x21, 0x04);
  arch::outb(0xA1, 0x02);
  arch::outb(0x21, 0x01);
  arch::outb(0xA1, 0x01);
  arch::outb(0x21, 0xFF);
  arch::outb(0xA1, 0xFF);
}

void spurious(interrupts::Frame*) {}

}  // namespace

void init() {
  disable_pic();
  const acpi::Madt& m = acpi::madt();
  uint64_t base = arch::rdmsr(arch::kMsrApicBase);
  arch::wrmsr(arch::kMsrApicBase, base | (1u << 11));  // global enable
  g_lapic = static_cast<volatile uint32_t*>(vmm::map_mmio(m.lapic_phys, 4096));
  g_ioapic = static_cast<volatile uint32_t*>(vmm::map_mmio(m.ioapic_phys, 4096));
  wr(kRegLvtTimer, kLvtMasked);
  wr(kRegLvtLint0, kLvtMasked);
  wr(kRegLvtLint1, kLvtMasked);
  wr(kRegLvtError, kLvtMasked);
  wr(kRegSvr, 0x100 | interrupts::kVectorSpurious);  // software enable
  interrupts::set_handler(interrupts::kVectorSpurious, spurious);
  uint32_t redirs = ((io_rd(1) >> 16) & 0xFF) + 1;
  for (uint32_t i = 0; i < redirs; i++) mask_gsi(m.ioapic_gsi_base + i);
  wr(kRegEoi, 0);
  kprintf("apic: lapic id %u, ioapic with %u inputs\n", rd(kRegId) >> 24, redirs);
}

void eoi() { wr(kRegEoi, 0); }

void set_timer_one_shot_max() {
  wr(kRegTimerDivide, 0x3);  // divide by 16
  wr(kRegLvtTimer, kLvtMasked);
  wr(kRegTimerInit, 0xFFFFFFFF);
}
uint32_t timer_current_count() { return rd(kRegTimerCurrent); }

void start_timer(uint32_t hz, uint8_t vector) {
  g_timer_hz = time::lapic_timer_hz();
  wr(kRegTimerDivide, 0x3);
  wr(kRegLvtTimer, kTimerPeriodic | vector);
  wr(kRegTimerInit, static_cast<uint32_t>(g_timer_hz / hz));
}

void stop_timer() { wr(kRegLvtTimer, kLvtMasked); }
uint64_t timer_frequency() { return g_timer_hz; }

void send_self_ipi(uint8_t vector) {
  wr(kRegIcrHi, 0);
  wr(kRegIcrLo, (1u << 18) | vector);  // destination shorthand: self, fixed
}

void route_gsi(uint32_t gsi, uint8_t vector, bool active_low, bool level_triggered) {
  uint32_t pin = gsi - acpi::madt().ioapic_gsi_base;
  uint32_t lo = vector | (active_low ? 1u << 13 : 0) | (level_triggered ? 1u << 15 : 0);
  io_wr(0x11 + 2 * pin, (rd(kRegId) >> 24) << 24);  // destination: this LAPIC
  io_wr(0x10 + 2 * pin, lo);
}

void route_isa_irq(uint8_t irq, uint8_t vector) {
  acpi::IrqOverride o = acpi::isa_irq(irq);
  route_gsi(o.gsi, vector, o.active_low, o.level_triggered);
}

void mask_gsi(uint32_t gsi) {
  uint32_t pin = gsi - acpi::madt().ioapic_gsi_base;
  io_wr(0x10 + 2 * pin, 1u << 16);
}

}  // namespace apic
