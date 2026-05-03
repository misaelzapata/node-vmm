#pragma once

// Minimal Intel 8254 Programmable Interval Timer. Mirrors the channels +
// modes that Linux's pit_hpet_ptimer_calibrate_cpu actually exercises:
//
//   * Channel 0: rate-generator at the kernel-programmed reload, used to
//     drive timer IRQ0 (kPitIoApicPin in backend.cc) once HPET legacy mode
//     and IOAPIC routing kick in. poll_irq0() returns true when the
//     scheduler thread should fire IRQ0 (idempotent against the next-irq
//     timestamp; coalesces missed ticks).
//   * Channel 2: gated by port 0x61 bit 0; OUT pin reflected via port 0x61
//     bit 5 so the kernel's TSC calibration loop converges. We approximate
//     mode 0 (interrupt-on-terminal-count) using elapsed wall time.
//
// The class holds no resources beyond std::array<Channel,3>; safe to
// stack-allocate per-VM. No locks: the dispatcher serializes access via
// device_mu in backend.cc.

#include <array>
#include <chrono>
#include <cstdint>

namespace node_vmm::whp {

class Pit {
 public:
  Pit();

  uint8_t read_port(uint16_t port);
  void write_port(uint16_t port, uint8_t value);

  bool poll_irq0();

  // True when channel 2's OUT line is asserted, used by the kernel through
  // port 0x61 bit 5 for TSC/PIT calibration loops.
  bool channel2_out_high();

  void set_channel2_gate(bool gated);
  bool channel2_gated() const;

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr uint64_t kPitHz = 1193182;

  struct Channel {
    uint16_t reload{0};
    uint16_t latch{0};
    bool latch_valid{false};
    uint8_t access{3};
    uint8_t mode{3};
    uint8_t write_phase{0};
    uint8_t read_phase{0};
    Clock::time_point start{};
    Clock::time_point next_irq{};
    bool irq_enabled{true};
  };

  static uint32_t divisor(const Channel& channel);
  static std::chrono::nanoseconds irq_interval(const Channel& channel);
  uint16_t current_count(const Channel& channel) const;
  void schedule_next_irq(Clock::time_point now);
  void reset_channel_timer(size_t index);
  void write_command(uint8_t value);
  void write_channel(size_t index, uint8_t value);
  uint8_t read_channel(Channel& channel);

  std::array<Channel, 3> channels_{};
  bool channel2_gated_{false};
};

}  // namespace node_vmm::whp
