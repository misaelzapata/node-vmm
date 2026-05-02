#pragma once

// 24/32-bit ACPI Power-Management timer at I/O port 0x408 (kAcpiPmTimerPort
// in backend.cc). Free-running counter at 3.579545 MHz (the canonical PM
// timer frequency). The Linux acpi_pm clocksource reads this counter to
// derive deltas; our value just has to advance monotonically at roughly
// the right rate. No state to write; the timer is read-only by design.

#include <chrono>
#include <cstdint>

namespace node_vmm::whp {

class AcpiPmTimer {
 public:
  // Canonical ACPI PM timer I/O port. Mirrors kAcpiPmTimerPort in the FADT
  // produced by acpi.cc; other modules can use this constant directly to
  // avoid stringly-typing the port number.
  static constexpr uint16_t kPort = 0x408;

  // Reads `size` bytes from `port`, where port ∈ [kPort, kPort+4). Returns
  // the partial counter value shifted to align with the byte offset within
  // the 32-bit register. Out-of-range reads return 0.
  uint32_t read(uint16_t port, uint8_t size) const;

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr uint64_t kPmTimerHz = 3579545;

  uint32_t counter() const;

  Clock::time_point started_at_{Clock::now()};
};

}  // namespace node_vmm::whp
