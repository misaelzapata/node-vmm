#include "cmos.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace node_vmm::whp {

Cmos::Cmos() {
  regs_[0x0A] = 0x26;  // 32.768 kHz divider, 1024 Hz periodic, UIP=0.
  regs_[0x0B] = 0x02;  // 24-hour mode, BCD numeric format.
  regs_[0x0D] = 0x80;  // VRT (battery valid).
  regs_[0x0F] = 0x00;  // Shutdown status: clean.
  refresh_time();
}

uint8_t Cmos::read_port(uint16_t port) {
  if (port == 0x71) {
    refresh_time();
    return regs_[selected_ & 0x7F];
  }
  return 0xFF;
}

void Cmos::write_port(uint16_t port, uint8_t value) {
  if (port == 0x70) {
    selected_ = value;
    return;
  }
  if (port == 0x71) {
    uint8_t reg = selected_ & 0x7F;
    // Only allow writes to status/shutdown registers; date fields are
    // intentionally read-only so the guest can't set our virtual clock.
    if (reg == 0x0A || reg == 0x0B || reg == 0x0C || reg == 0x0F) {
      regs_[reg] = value;
    }
  }
}

uint8_t Cmos::to_bcd(uint16_t value) {
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t Cmos::encode_time_value(uint16_t value) const {
  return (regs_[0x0B] & 0x04) != 0 ? static_cast<uint8_t>(value) : to_bcd(value);
}

void Cmos::refresh_time() {
  SYSTEMTIME now{};
  GetSystemTime(&now);
  regs_[0x00] = encode_time_value(now.wSecond);
  regs_[0x02] = encode_time_value(now.wMinute);
  regs_[0x04] = encode_time_value(now.wHour);
  regs_[0x06] = encode_time_value(now.wDayOfWeek + 1);  // RTC: Sunday=1.
  regs_[0x07] = encode_time_value(now.wDay);
  regs_[0x08] = encode_time_value(now.wMonth);
  regs_[0x09] = encode_time_value(now.wYear % 100);
  regs_[0x32] = encode_time_value(now.wYear / 100);
}

}  // namespace node_vmm::whp
