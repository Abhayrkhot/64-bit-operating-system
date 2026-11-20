// PCI configuration space (mechanism #1, ports 0xCF8/0xCFC) and enumeration.
#pragma once

#include <stdint.h>

namespace pci {

struct Device {
  uint8_t bus, slot, func;
  uint16_t vendor, device;
  uint8_t class_code, subclass;
  uint32_t bar[6];
  uint8_t irq_line;
};

uint32_t read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t read16(const Device& d, uint8_t offset);
void write16(const Device& d, uint8_t offset, uint16_t value);

// Scans bus 0 and every bus behind a PCI bridge.
void enumerate();
// Finds the n-th device with this vendor/device id.
bool find(uint16_t vendor, uint16_t device, int n, Device* out);
// Turns on I/O + memory decoding and bus mastering.
void enable(const Device& d);
int count();

}  // namespace pci
