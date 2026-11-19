// Just enough ACPI to find interrupt controllers: RSDP -> XSDT/RSDT -> MADT.
#pragma once

#include <stdint.h>

namespace acpi {

struct IrqOverride {
  uint8_t source;  // ISA IRQ
  uint32_t gsi;    // global system interrupt it is wired to
  bool active_low;
  bool level_triggered;
};

struct Madt {
  uint64_t lapic_phys = 0;
  uint64_t ioapic_phys = 0;
  uint32_t ioapic_gsi_base = 0;
  uint32_t cpu_count = 0;
  IrqOverride overrides[16];
  uint32_t override_count = 0;
};

void init(uint64_t rsdp_phys);
const Madt& madt();
// GSI for an ISA IRQ, applying MADT overrides.
const IrqOverride isa_irq(uint8_t irq);

}  // namespace acpi
