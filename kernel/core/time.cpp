#include "core/time.hpp"

#include "arch/x86_64/apic.hpp"
#include "arch/x86_64/cpu.hpp"
#include "core/log.hpp"

namespace time {
namespace {

constexpr uint32_t kPitHz = 1193182;
uint64_t g_tsc_hz, g_lapic_hz, g_tsc_base;
uint64_t g_ns_mult;  // ns = (cycles * g_ns_mult) >> 32
volatile uint64_t g_ticks;

// One PIT channel-2 countdown of `pit_ticks`, measuring TSC and LAPIC-timer
// progress across it. Channel 2's gate is software controlled through port
// 0x61 and its output is readable there, so no interrupt is needed.
void measure(uint32_t pit_ticks, uint64_t* tsc_delta, uint64_t* lapic_delta) {
  uint8_t port61 = arch::inb(0x61);
  arch::outb(0x61, static_cast<uint8_t>((port61 & ~0x02) & ~0x01));  // gate low, speaker off
  arch::outb(0x43, 0xB0);  // channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count)
  arch::outb(0x42, pit_ticks & 0xFF);
  arch::outb(0x42, static_cast<uint8_t>(pit_ticks >> 8));
  apic::set_timer_one_shot_max();
  uint32_t lapic0 = apic::timer_current_count();
  uint64_t tsc0 = arch::rdtsc_fenced();
  arch::outb(0x61, static_cast<uint8_t>((port61 & ~0x02) | 0x01));  // gate high: start counting
  while (!(arch::inb(0x61) & 0x20)) arch::pause();
  uint64_t tsc1 = arch::rdtsc_fenced();
  uint32_t lapic1 = apic::timer_current_count();
  *tsc_delta = tsc1 - tsc0;
  *lapic_delta = lapic0 - lapic1;
}

}  // namespace

void calibrate() {
  // Median of several 10 ms windows, so one window disturbed by the host
  // descheduling the vCPU doesn't skew the result.
  constexpr int kRounds = 5;
  constexpr uint32_t kPitTicks = kPitHz / 100;
  uint64_t tsc[kRounds], lapic[kRounds];
  for (int i = 0; i < kRounds; i++) measure(kPitTicks, &tsc[i], &lapic[i]);
  for (int i = 0; i < kRounds; i++)
    for (int j = i + 1; j < kRounds; j++) {
      if (tsc[j] < tsc[i]) {
        uint64_t t = tsc[i];
        tsc[i] = tsc[j];
        tsc[j] = t;
      }
      if (lapic[j] < lapic[i]) {
        uint64_t t = lapic[i];
        lapic[i] = lapic[j];
        lapic[j] = t;
      }
    }
  g_tsc_hz = tsc[kRounds / 2] * 100;
  g_lapic_hz = lapic[kRounds / 2] * 100;
  g_ns_mult = (1000000000ull << 32) / g_tsc_hz;
  g_tsc_base = arch::rdtsc();
  bool invariant = arch::cpuid(0x80000007).edx & (1u << 8);
  kprintf("time: tsc %lu.%03lu MHz%s, lapic timer %lu.%03lu MHz\n", g_tsc_hz / 1000000, (g_tsc_hz / 1000) % 1000,
          invariant ? " (invariant)" : "", g_lapic_hz / 1000000, (g_lapic_hz / 1000) % 1000);
}

uint64_t tsc_hz() { return g_tsc_hz; }
uint64_t lapic_timer_hz() { return g_lapic_hz; }

uint64_t cycles_to_ns(uint64_t cycles) {
  return static_cast<uint64_t>((static_cast<unsigned __int128>(cycles) * g_ns_mult) >> 32);
}

uint64_t now_ns() { return cycles_to_ns(arch::rdtsc() - g_tsc_base); }
uint64_t ticks() { return g_ticks; }
void on_tick() { g_ticks = g_ticks + 1; }

void spin_us(uint64_t us) {
  uint64_t end = arch::rdtsc() + g_tsc_hz / 1000000 * us;
  while (arch::rdtsc() < end) arch::pause();
}

}  // namespace time
