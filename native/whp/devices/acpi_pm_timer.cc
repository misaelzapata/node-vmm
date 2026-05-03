#include "acpi_pm_timer.h"

namespace node_vmm::whp {

uint32_t AcpiPmTimer::read(uint16_t port, uint8_t size) const {
  if (port < kPort || port >= kPort + 4 || size == 0 || size > 4) {
    return 0;
  }
  uint8_t shift = static_cast<uint8_t>((port - kPort) * 8);
  return static_cast<uint32_t>(counter() >> shift);
}

uint32_t AcpiPmTimer::counter() const {
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     Clock::now() - started_at_)
                     .count();
  if (elapsed <= 0) {
    return 0;
  }
  uint64_t ticks = (static_cast<uint64_t>(elapsed) * kPmTimerHz) / 1000000000ULL;
  return static_cast<uint32_t>(ticks);
}

}  // namespace node_vmm::whp
