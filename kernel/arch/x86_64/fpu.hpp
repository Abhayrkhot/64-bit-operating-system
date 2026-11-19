// x87/SSE state. The kernel never touches FPU registers (built with -mno-sse),
// so only threads that run user code save and restore it on context switch.
#pragma once

#include "sched/sched.hpp"

namespace fpu {

void init();
// A clean state (FCW 0x37F, MXCSR 0x1F80) for new user threads.
void init_state(FpuState* s);
inline void save(FpuState* s) { asm volatile("fxsave64 %0" : "=m"(*s)::"memory"); }
inline void restore(const FpuState* s) { asm volatile("fxrstor64 %0" ::"m"(*s) : "memory"); }

}  // namespace fpu
