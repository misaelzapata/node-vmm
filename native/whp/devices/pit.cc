#include "pit.h"

#include <algorithm>

namespace node_vmm::whp {

Pit::Pit() {
  auto now = Clock::now();
  for (auto& channel : channels_) {
    channel.start = now;
  }
  schedule_next_irq(now);
}

uint8_t Pit::read_port(uint16_t port) {
  if (port < 0x40 || port > 0x43) {
    return 0;
  }
  if (port == 0x43) {
    return 0;
  }
  return read_channel(channels_[port - 0x40]);
}

void Pit::write_port(uint16_t port, uint8_t value) {
  if (port == 0x43) {
    write_command(value);
    return;
  }
  if (port >= 0x40 && port <= 0x42) {
    write_channel(static_cast<size_t>(port - 0x40), value);
  }
}

bool Pit::poll_irq0() {
  auto now = Clock::now();
  Channel& channel = channels_[0];
  if (!channel.irq_enabled || now < channel.next_irq) {
    return false;
  }
  auto interval = irq_interval(channel);
  do {
    channel.next_irq += interval;
  } while (now >= channel.next_irq);
  return true;
}

bool Pit::channel2_out_high() {
  if (!channel2_gated_) {
    return false;
  }
  const Channel& channel = channels_[2];
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     Clock::now() - channel.start)
                     .count();
  if (elapsed <= 0) {
    return false;
  }
  uint64_t ticks = (static_cast<uint64_t>(elapsed) * kPitHz) / 1000000000ULL;
  return ticks >= divisor(channel);
}

void Pit::set_channel2_gate(bool gated) {
  if (gated && !channel2_gated_) {
    channels_[2].start = Clock::now();
  }
  channel2_gated_ = gated;
}

bool Pit::channel2_gated() const { return channel2_gated_; }

uint32_t Pit::divisor(const Channel& channel) {
  return channel.reload == 0 ? 65536U : channel.reload;
}

std::chrono::nanoseconds Pit::irq_interval(const Channel& channel) {
  uint64_t div = divisor(channel);
  uint64_t nanos = std::max<uint64_t>((div * 1000000000ULL) / kPitHz, 1000000ULL);
  return std::chrono::nanoseconds(nanos);
}

uint16_t Pit::current_count(const Channel& channel) const {
  // Mirrors QEMU's pit_get_count (qemu/hw/timer/i8254.c:53-76): for modes
  // 0/1/4/5 the counter is `(count - d) & 0xffff`; for the rate generator
  // and square-wave modes (2/3) it wraps around the count value.
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     Clock::now() - channel.start)
                     .count();
  uint64_t ticks = elapsed <= 0
                       ? 0
                       : (static_cast<uint64_t>(elapsed) * kPitHz) / 1000000000ULL;
  uint32_t count = divisor(channel);
  uint32_t counter;
  if (channel.mode == 2 || channel.mode == 3) {
    counter = count - static_cast<uint32_t>(ticks % count);
  } else {
    counter = (count - static_cast<uint32_t>(ticks)) & 0xffff;
  }
  return static_cast<uint16_t>(counter & 0xFFFF);
}

void Pit::schedule_next_irq(Clock::time_point now) {
  channels_[0].next_irq = now + irq_interval(channels_[0]);
}

void Pit::reset_channel_timer(size_t index) {
  auto now = Clock::now();
  channels_[index].start = now;
  if (index == 0) {
    channels_[index].irq_enabled = true;
    schedule_next_irq(now);
  }
}

void Pit::write_command(uint8_t value) {
  if ((value & 0xC0) == 0xC0) {
    return;
  }
  size_t index = (value >> 6) & 0x03;
  if (index >= channels_.size()) {
    return;
  }
  Channel& channel = channels_[index];
  uint8_t access = (value >> 4) & 0x03;
  if (access == 0) {
    channel.latch = current_count(channel);
    channel.latch_valid = true;
    channel.read_phase = 0;
    return;
  }
  channel.access = access;
  channel.mode = (value >> 1) & 0x07;
  if (channel.mode > 5) {
    channel.mode -= 4;
  }
  channel.write_phase = 0;
  channel.read_phase = 0;
  channel.latch_valid = false;
}

void Pit::write_channel(size_t index, uint8_t value) {
  Channel& channel = channels_[index];
  if (channel.access == 1) {
    channel.reload = static_cast<uint16_t>((channel.reload & 0xFF00) | value);
    reset_channel_timer(index);
    return;
  }
  if (channel.access == 2) {
    channel.reload = static_cast<uint16_t>((channel.reload & 0x00FF) | (uint16_t(value) << 8));
    reset_channel_timer(index);
    return;
  }
  if (channel.write_phase == 0) {
    channel.reload = static_cast<uint16_t>((channel.reload & 0xFF00) | value);
    channel.write_phase = 1;
    return;
  }
  channel.reload = static_cast<uint16_t>((channel.reload & 0x00FF) | (uint16_t(value) << 8));
  channel.write_phase = 0;
  reset_channel_timer(index);
}

uint8_t Pit::read_channel(Channel& channel) {
  uint16_t value = channel.latch_valid ? channel.latch : current_count(channel);
  if (channel.access == 1) {
    channel.latch_valid = false;
    return static_cast<uint8_t>(value & 0xFF);
  }
  if (channel.access == 2) {
    channel.latch_valid = false;
    return static_cast<uint8_t>((value >> 8) & 0xFF);
  }
  if (channel.read_phase == 0) {
    channel.read_phase = 1;
    return static_cast<uint8_t>(value & 0xFF);
  }
  channel.read_phase = 0;
  channel.latch_valid = false;
  return static_cast<uint8_t>((value >> 8) & 0xFF);
}

}  // namespace node_vmm::whp
