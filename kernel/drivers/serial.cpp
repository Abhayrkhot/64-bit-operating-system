#include "drivers/serial.hpp"

#include "arch/x86_64/cpu.hpp"

namespace serial {
namespace {
constexpr uint16_t kCom1 = 0x3F8;
constexpr uint16_t kData = 0, kIer = 1, kFcr = 2, kLcr = 3, kMcr = 4, kLsr = 5;
constexpr uint8_t kLsrDataReady = 0x01, kLsrThrEmpty = 0x20;

void put(char c) {
  while (!(arch::inb(kCom1 + kLsr) & kLsrThrEmpty)) arch::pause();
  arch::outb(kCom1 + kData, static_cast<uint8_t>(c));
}
}  // namespace

void init() {
  arch::outb(kCom1 + kIer, 0x00);
  arch::outb(kCom1 + kLcr, 0x80);  // DLAB
  arch::outb(kCom1 + kData, 0x01);  // divisor 1 = 115200 baud
  arch::outb(kCom1 + kIer, 0x00);
  arch::outb(kCom1 + kLcr, 0x03);  // 8N1
  arch::outb(kCom1 + kFcr, 0xC7);  // FIFO on, cleared, 14-byte threshold
  arch::outb(kCom1 + kMcr, 0x0B);  // DTR, RTS, OUT2 (IRQ gate)
}

void write(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\n') put('\r');
    put(s[i]);
  }
}

bool can_read() { return arch::inb(kCom1 + kLsr) & kLsrDataReady; }
char read_byte() { return static_cast<char>(arch::inb(kCom1 + kData)); }
void enable_rx_interrupt() { arch::outb(kCom1 + kIer, 0x01); }

}  // namespace serial
