#pragma once

// Minimal HPET (High Precision Event Timer) at MMIO 0xFED00000. Provides
// the registers and timers Linux's hpet_clocksource_register / hpet_enable
// actually exercises:
//
//   * Capabilities register (0x000), Configuration (0x010), ISR (0x020),
//     Main Counter (0x0F0).
//   * 3 timers (the minimum the spec allows). Each supports periodic mode,
//     32-bit / 64-bit operation, level/edge selection, and a programmable
//     IOAPIC route. FSB/MSI is intentionally not exposed (FSB cap stays
//     clear) so Linux always routes through the IOAPIC.
//   * Legacy mode: when bit 1 of CFG is set, timer 0 routes to IOAPIC pin
//     `kTimer0IoApicPin` (2 — same as the PIT) and timer 1 to pin 8.
//
// `attach_irq_line` is the up-call back into the dispatcher: a single
// `(uint32_t pin, bool level)` callback that level-de-asserts when the
// guest writes the matching ISR bit (qemu/hw/timer/hpet.c semantics).

#include "../../common/bytes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace node_vmm::whp {

class Hpet {
 public:
  struct Irq {
    uint32_t ioapic_pin{0};
    uint8_t pic_irq{0};
    bool level{false};
  };

  // MMIO base + length. Exported for the dispatcher and ACPI table builders.
  static constexpr uint64_t kBase = 0xFED00000ULL;
  static constexpr uint64_t kSize = 0x400ULL;

  Hpet();

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len);
  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len);

  void attach_irq_line(std::function<void(uint32_t, bool)> irq_line);

  std::vector<Irq> poll_expired();

  bool legacy_mode() const;

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr uint64_t kClockPeriodNs = 10;
  static constexpr uint64_t kClockPeriodFs = kClockPeriodNs * 1000000ULL;
  static constexpr size_t kNumTimers = 3;
  static constexpr uint32_t kTimer0IoApicPin = 2;
  static constexpr uint32_t kIntCap = 1U << kTimer0IoApicPin;
  static constexpr uint64_t kCapabilities =
      0x8086A001ULL |
      ((uint64_t(kNumTimers) - 1) << 8) |
      (kClockPeriodFs << 32);

  static constexpr uint64_t kCfgEnable = 0x001;
  static constexpr uint64_t kCfgLegacy = 0x002;
  static constexpr uint64_t kCfgWriteMask = 0x003;

  static constexpr uint64_t kTimerTypeLevel = 0x002;
  static constexpr uint64_t kTimerEnable = 0x004;
  static constexpr uint64_t kTimerPeriodic = 0x008;
  static constexpr uint64_t kTimerPeriodicCap = 0x010;
  static constexpr uint64_t kTimerSizeCap = 0x020;
  static constexpr uint64_t kTimerSetVal = 0x040;
  static constexpr uint64_t kTimer32Bit = 0x100;
  static constexpr uint64_t kTimerIntRouteMask = 0x3E00;
  static constexpr uint64_t kTimerCfgWriteMask = 0x7F4E;
  static constexpr uint32_t kTimerIntRouteShift = 9;
  static constexpr uint32_t kTimerIntRouteCapShift = 32;

  struct Timer {
    uint64_t config{0};
    uint64_t cmp{UINT64_MAX};
    uint64_t cmp64{UINT64_MAX};
    uint64_t period{0};
    bool armed{false};
  };

  static bool valid_access(uint64_t addr, uint32_t len);
  static bool counter_reached(uint64_t now, uint64_t target);

  uint64_t counter() const;
  uint64_t read_reg(uint64_t off) const;
  void write_reg(uint64_t off, uint32_t shift, uint32_t bits, uint64_t value);
  void write_config(uint32_t shift, uint32_t bits, uint64_t value);
  void write_timer_config(size_t timer_id, uint32_t shift, uint32_t bits, uint64_t value);
  void write_timer_cmp(size_t timer_id, uint32_t shift, uint32_t bits, uint64_t value);
  void reprogram_timers();
  void set_timer(size_t timer_id);
  Irq route_for_timer(size_t timer_id) const;
  void deassert_cleared_irqs(uint64_t cleared);

  std::array<Timer, kNumTimers> timers_{};
  std::array<std::optional<Irq>, kNumTimers> asserted_irqs_{};
  uint64_t config_{0};
  uint64_t isr_{0};
  uint64_t counter_base_{0};
  Clock::time_point counter_started_at_{Clock::now()};
  std::function<void(uint32_t, bool)> irq_line_;
};

}  // namespace node_vmm::whp
