#pragma once

// Minimal CMOS/RTC at I/O ports 0x70 (index) and 0x71 (data). Linux probes
// status register A for the UIP (update-in-progress) bit during boot, and
// the rtc-cmos driver later reads dates and the shutdown-status register;
// without a CMOS responder, those reads return 0xFF on real hardware-style
// I/O and can mislead the kernel. We answer with stable defaults that look
// like a valid battery-backed clock in UTC, 24-hour mode, no UIP, clean
// shutdown. Date fields refresh from the host's UTC clock on every read so
// hwclock(8) reports something sensible.

#include <cstdint>

namespace node_vmm::whp {

class Cmos {
 public:
  Cmos();

  uint8_t read_port(uint16_t port);
  void write_port(uint16_t port, uint8_t value);

 private:
  static uint8_t to_bcd(uint16_t value);

  uint8_t encode_time_value(uint16_t value) const;
  void refresh_time();

  uint8_t selected_{0};
  uint8_t regs_[0x80]{};
};

}  // namespace node_vmm::whp
