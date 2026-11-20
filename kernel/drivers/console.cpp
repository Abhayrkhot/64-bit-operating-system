#include "drivers/console.hpp"

#include "arch/x86_64/apic.hpp"
#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "drivers/serial.hpp"
#include "sched/sched.hpp"

namespace console {
namespace {

constexpr uint8_t kVectorSerial = 0x24;
constexpr uint32_t kRingSize = 1024;
char g_ring[kRingSize];
volatile uint32_t g_head, g_tail;  // head: next write, tail: next read
WaitQueue g_readers;

void on_rx(interrupts::Frame*) {
  while (serial::can_read()) {
    char c = serial::read_byte();
    if (c == '\r') c = '\n';
    uint32_t next = (g_head + 1) % kRingSize;
    if (next != g_tail) {
      g_ring[g_head] = c;
      g_head = next;
    }
  }
  apic::eoi();
  g_readers.wake_all();
}

int64_t con_read(File*, uint8_t* buf, uint64_t n) {
  if (n == 0) return 0;
  bool irq = arch::irq_save();
  while (g_head == g_tail) g_readers.wait();
  uint64_t got = 0;
  while (got < n && g_head != g_tail) {
    char c = g_ring[g_tail];
    g_tail = (g_tail + 1) % kRingSize;
    buf[got++] = static_cast<uint8_t>(c);
    if (c == '\n') break;  // line at a time, like a cooked tty
  }
  arch::irq_restore(irq);
  return static_cast<int64_t>(got);
}

int64_t con_write(File*, const uint8_t* buf, uint64_t n) {
  bool irq = arch::irq_save();
  serial::write(reinterpret_cast<const char*>(buf), n);
  arch::irq_restore(irq);
  return static_cast<int64_t>(n);
}

int64_t con_stat(File*, shale::sys::Stat* st) {
  *st = {};
  st->type = shale::sys::kTypeConsole;
  return 0;
}

const FileOps kOps = {con_read, con_write, con_stat, nullptr, nullptr, nullptr, false};

}  // namespace

void init() {
  interrupts::set_handler(kVectorSerial, on_rx);
  apic::route_isa_irq(4, kVectorSerial);
  serial::enable_rx_interrupt();
}

File* open() { return file_alloc(&kOps, shale::sys::kTypeConsole, shale::sys::kOReadWrite, nullptr); }

}  // namespace console
