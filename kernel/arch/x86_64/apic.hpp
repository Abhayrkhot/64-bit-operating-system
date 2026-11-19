// Local APIC (timer, EOI, self-IPI) and I/O APIC (device IRQ routing). The
// legacy 8259 PICs are remapped and masked.
#pragma once

#include <stdint.h>

namespace apic {

void init();
void eoi();
// Starts the periodic LAPIC timer at `hz` on vector `vector`, using the
// calibrated LAPIC timer frequency.
void start_timer(uint32_t hz, uint8_t vector);
void stop_timer();
uint64_t timer_frequency();  // LAPIC timer ticks per second (after divide)
void send_self_ipi(uint8_t vector);

// Routes a global system interrupt to `vector` on this CPU.
void route_gsi(uint32_t gsi, uint8_t vector, bool active_low, bool level_triggered);
void route_isa_irq(uint8_t irq, uint8_t vector);
void mask_gsi(uint32_t gsi);

// Exposed for calibration in core/time.cpp.
void set_timer_one_shot_max();
uint32_t timer_current_count();

}  // namespace apic
