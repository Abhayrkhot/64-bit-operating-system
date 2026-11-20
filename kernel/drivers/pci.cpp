#include "drivers/pci.hpp"

#include "arch/x86_64/cpu.hpp"
#include "core/log.hpp"

namespace pci {
namespace {
constexpr unsigned kMaxDevices = 64;
Device g_devices[kMaxDevices];
int g_count;

uint32_t address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
  return 0x80000000u | (static_cast<uint32_t>(bus) << 16) | (static_cast<uint32_t>(slot) << 11) |
         (static_cast<uint32_t>(func) << 8) | (offset & 0xFC);
}

void scan_bus(uint8_t bus, int depth);

void scan_function(uint8_t bus, uint8_t slot, uint8_t func, int depth) {
  uint32_t id = read32(bus, slot, func, 0);
  if ((id & 0xFFFF) == 0xFFFF) return;
  uint32_t cls = read32(bus, slot, func, 0x08);
  uint8_t header = static_cast<uint8_t>((read32(bus, slot, func, 0x0C) >> 16) & 0x7F);
  if (g_count < static_cast<int>(kMaxDevices)) {
    Device& d = g_devices[g_count++];
    d.bus = bus;
    d.slot = slot;
    d.func = func;
    d.vendor = static_cast<uint16_t>(id & 0xFFFF);
    d.device = static_cast<uint16_t>(id >> 16);
    d.class_code = static_cast<uint8_t>(cls >> 24);
    d.subclass = static_cast<uint8_t>(cls >> 16);
    for (int i = 0; i < 6; i++) d.bar[i] = header == 0 ? read32(bus, slot, func, static_cast<uint8_t>(0x10 + 4 * i)) : 0;
    d.irq_line = static_cast<uint8_t>(read32(bus, slot, func, 0x3C) & 0xFF);
  }
  if (header == 1 && depth < 8) {  // PCI-PCI bridge: walk its secondary bus
    uint8_t secondary = static_cast<uint8_t>((read32(bus, slot, func, 0x18) >> 8) & 0xFF);
    if (secondary && secondary != bus) scan_bus(secondary, depth + 1);
  }
}

void scan_bus(uint8_t bus, int depth) {
  for (uint8_t slot = 0; slot < 32; slot++) {
    if ((read32(bus, slot, 0, 0) & 0xFFFF) == 0xFFFF) continue;
    bool multi = (read32(bus, slot, 0, 0x0C) >> 16) & 0x80;
    for (uint8_t func = 0; func < (multi ? 8 : 1); func++) scan_function(bus, slot, func, depth);
  }
}
}  // namespace

uint32_t read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
  arch::outl(0xCF8, address(bus, slot, func, offset));
  return arch::inl(0xCFC);
}

void write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
  arch::outl(0xCF8, address(bus, slot, func, offset));
  arch::outl(0xCFC, value);
}

uint16_t read16(const Device& d, uint8_t offset) {
  uint32_t v = read32(d.bus, d.slot, d.func, offset & 0xFC);
  return static_cast<uint16_t>(v >> ((offset & 2) * 8));
}

void write16(const Device& d, uint8_t offset, uint16_t value) {
  uint32_t v = read32(d.bus, d.slot, d.func, offset & 0xFC);
  uint32_t shift = (offset & 2) * 8;
  v = (v & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
  write32(d.bus, d.slot, d.func, offset & 0xFC, v);
}

void enumerate() {
  g_count = 0;
  scan_bus(0, 0);
  kprintf("pci: %d function(s)\n", g_count);
  for (int i = 0; i < g_count; i++) {
    const Device& d = g_devices[i];
    kprintf("  %02x:%02x.%u %04x:%04x class %02x.%02x\n", d.bus, d.slot, d.func, d.vendor, d.device, d.class_code,
            d.subclass);
  }
}

bool find(uint16_t vendor, uint16_t device, int n, Device* out) {
  for (int i = 0; i < g_count; i++) {
    if (g_devices[i].vendor == vendor && g_devices[i].device == device && n-- == 0) {
      *out = g_devices[i];
      return true;
    }
  }
  return false;
}

void enable(const Device& d) { write16(d, 0x04, static_cast<uint16_t>(read16(d, 0x04) | 0x7)); }

int count() { return g_count; }

}  // namespace pci
