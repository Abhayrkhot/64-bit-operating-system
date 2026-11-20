// Leaving the machine: QEMU's isa-debug-exit device (iobase 0xf4) turns a
// write of v into QEMU exit status (v << 1) | 1. On real hardware the write is
// ignored and the CPU halts.
#pragma once

#include "arch/x86_64/cpu.hpp"

namespace power {

enum class ExitCode : uint32_t {
  kSuccess = 0x10,  // QEMU exits with 33
  kFailure = 0x11,  // 35
  kPanic = 0x12,    // 37
};

[[noreturn]] inline void qemu_exit(ExitCode code) {
  arch::outl(0xf4, static_cast<uint32_t>(code));
  arch::halt_forever();
}

// Orderly exit: unmounts the file system first (core/main.cpp).
[[noreturn]] void shutdown(ExitCode code);

}  // namespace power
