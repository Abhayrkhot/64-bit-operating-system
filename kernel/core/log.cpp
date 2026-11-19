#include "core/log.hpp"

#include "arch/x86_64/cpu.hpp"
#include "core/power.hpp"
#include "drivers/serial.hpp"
#include "fmt/format.hpp"

extern "C" char __text_start[], __text_end[];

void kvprintf(const char* fmt, va_list ap) {
  char buf[512];
  int n = shale::fmt::vformat(buf, sizeof(buf), fmt, ap);
  if (n > static_cast<int>(sizeof(buf)) - 1) n = sizeof(buf) - 1;
  bool irq = arch::irq_save();
  serial::write(buf, static_cast<size_t>(n));
  arch::irq_restore(irq);
}

void kprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  kvprintf(fmt, ap);
  va_end(ap);
}

namespace {
bool in_text(uint64_t addr) {
  return addr >= reinterpret_cast<uint64_t>(__text_start) && addr < reinterpret_cast<uint64_t>(__text_end);
}
// RBP values must point into kernel memory (higher half) to be followed.
bool plausible_frame(uint64_t rbp) { return rbp >= 0xFFFF800000000000ull && (rbp & 7) == 0; }
int g_panicking = 0;
}  // namespace

void backtrace(uint64_t rbp, uint64_t rip) {
  kprintf("backtrace:\n");
  if (rip) kprintf("  [0] %p\n", reinterpret_cast<void*>(rip));
  for (int depth = 1; depth < 32 && plausible_frame(rbp); depth++) {
    const auto* frame = reinterpret_cast<const uint64_t*>(rbp);
    uint64_t ret = frame[1];
    if (!in_text(ret)) break;
    kprintf("  [%d] %p\n", depth, reinterpret_cast<void*>(ret));
    rbp = frame[0];
  }
}

void panic(const char* fmt, ...) {
  arch::cli();
  if (g_panicking++) {
    kprintf("\nnested panic\n");
    power::qemu_exit(power::ExitCode::kPanic);
  }
  kprintf("\nPANIC: ");
  va_list ap;
  va_start(ap, fmt);
  kvprintf(fmt, ap);
  va_end(ap);
  kprintf("\n");
  backtrace(reinterpret_cast<uint64_t>(__builtin_frame_address(0)), 0);
  power::qemu_exit(power::ExitCode::kPanic);
}
