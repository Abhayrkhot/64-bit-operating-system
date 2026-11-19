#include "arch/x86_64/acpi.hpp"

#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"

namespace acpi {
namespace {

struct [[gnu::packed]] Rsdp {
  char signature[8];
  uint8_t checksum;
  char oem[6];
  uint8_t revision;
  uint32_t rsdt;
  uint32_t length;
  uint64_t xsdt;
  uint8_t ext_checksum;
  uint8_t reserved[3];
};

struct [[gnu::packed]] SdtHeader {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem[6];
  char oem_table[8];
  uint32_t oem_revision;
  uint32_t creator;
  uint32_t creator_revision;
};

Madt g_madt;

bool checksum_ok(const void* p, uint32_t len) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < len; i++) sum = static_cast<uint8_t>(sum + static_cast<const uint8_t*>(p)[i]);
  return sum == 0;
}

void parse_madt(const SdtHeader* h) {
  const auto* p = reinterpret_cast<const uint8_t*>(h);
  g_madt.lapic_phys = *reinterpret_cast<const uint32_t*>(p + sizeof(SdtHeader));
  for (uint32_t off = sizeof(SdtHeader) + 8; off + 2 <= h->length;) {
    uint8_t type = p[off], len = p[off + 1];
    if (len < 2) break;
    const uint8_t* e = p + off;
    switch (type) {
      case 0:  // processor local APIC
        if (*reinterpret_cast<const uint32_t*>(e + 4) & 1) g_madt.cpu_count++;
        break;
      case 1:  // I/O APIC
        if (!g_madt.ioapic_phys) {
          g_madt.ioapic_phys = *reinterpret_cast<const uint32_t*>(e + 4);
          g_madt.ioapic_gsi_base = *reinterpret_cast<const uint32_t*>(e + 8);
        }
        break;
      case 2:  // interrupt source override
        if (g_madt.override_count < 16) {
          uint16_t flags = *reinterpret_cast<const uint16_t*>(e + 8);
          g_madt.overrides[g_madt.override_count++] = {e[3], *reinterpret_cast<const uint32_t*>(e + 4),
                                                       (flags & 3) == 3, ((flags >> 2) & 3) == 3};
        }
        break;
      case 5:  // 64-bit local APIC address override
        g_madt.lapic_phys = *reinterpret_cast<const uint64_t*>(e + 4);
        break;
    }
    off += len;
  }
}

}  // namespace

void init(uint64_t rsdp_phys) {
  if (!rsdp_phys) panic("acpi: no RSDP");
  const auto* rsdp = phys_to_virt<const Rsdp>(rsdp_phys);
  if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) panic("acpi: bad RSDP");
  bool xsdt = rsdp->revision >= 2 && rsdp->xsdt;
  const auto* root = phys_to_virt<const SdtHeader>(xsdt ? rsdp->xsdt : rsdp->rsdt);
  uint32_t entry_size = xsdt ? 8 : 4;
  uint32_t entries = (root->length - sizeof(SdtHeader)) / entry_size;
  const auto* table = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);
  for (uint32_t i = 0; i < entries; i++) {
    uint64_t phys = xsdt ? *reinterpret_cast<const uint64_t*>(table + i * 8)
                         : *reinterpret_cast<const uint32_t*>(table + i * 4);
    const auto* h = phys_to_virt<const SdtHeader>(phys);
    if (memcmp(h->signature, "APIC", 4) == 0 && checksum_ok(h, h->length)) parse_madt(h);
  }
  if (!g_madt.lapic_phys || !g_madt.ioapic_phys) panic("acpi: MADT missing LAPIC/IOAPIC");
  kprintf("acpi: %u cpu(s), lapic %lx, ioapic %lx (gsi base %u), %u irq overrides\n", g_madt.cpu_count,
          g_madt.lapic_phys, g_madt.ioapic_phys, g_madt.ioapic_gsi_base, g_madt.override_count);
}

const Madt& madt() { return g_madt; }

const IrqOverride isa_irq(uint8_t irq) {
  for (uint32_t i = 0; i < g_madt.override_count; i++)
    if (g_madt.overrides[i].source == irq) return g_madt.overrides[i];
  return {irq, irq, false, false};
}

}  // namespace acpi
