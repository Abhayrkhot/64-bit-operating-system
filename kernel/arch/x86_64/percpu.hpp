// Per-CPU block, reached through GS in kernel mode. The syscall entry path
// (M3) reads kernel_rsp/user_rsp at fixed offsets, so the layout is ABI.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct Thread;

struct PerCpu {
  PerCpu* self;           // 0x00
  uint64_t kernel_rsp;    // 0x08: top of the current thread's kernel stack
  uint64_t user_rsp;      // 0x10: scratch for the syscall entry path
  Thread* current;        // 0x18
  uint64_t preempt_pending;  // 0x20: set by the timer, consumed on interrupt exit
};
static_assert(offsetof(PerCpu, kernel_rsp) == 0x08, "syscall entry depends on this");
static_assert(offsetof(PerCpu, user_rsp) == 0x10, "syscall entry depends on this");

namespace percpu {

void init();
inline PerCpu* get() {
  PerCpu* p;
  asm volatile("mov %%gs:0, %0" : "=r"(p));
  return p;
}

}  // namespace percpu
