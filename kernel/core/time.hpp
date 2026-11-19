// Time: TSC and LAPIC-timer calibration against the PIT, a monotonic clock in
// nanoseconds, and the scheduler tick.
#pragma once

#include <stdint.h>

namespace time {

constexpr uint32_t kTickHz = 1000;

void calibrate();
uint64_t tsc_hz();
uint64_t lapic_timer_hz();
// Nanoseconds since calibration (TSC-based, monotonic on a single CPU).
uint64_t now_ns();
uint64_t cycles_to_ns(uint64_t cycles);
// Scheduler ticks since the timer started.
uint64_t ticks();
void on_tick();
// Busy-waits (for drivers before the scheduler runs).
void spin_us(uint64_t us);

}  // namespace time
