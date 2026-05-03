#include "hpet.h"

#include <algorithm>
#include <cstring>

namespace node_vmm::whp {

using node_vmm::common::DepositBits;
using node_vmm::common::ReadLe;
using node_vmm::common::WriteLe;

Hpet::Hpet() {
  for (auto& timer : timers_) {
    timer.cmp = UINT64_MAX;
    timer.cmp64 = UINT64_MAX;
    timer.config = kTimerPeriodicCap | kTimerSizeCap | (uint64_t(kIntCap) << kTimerIntRouteCapShift);
  }
}

void Hpet::read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
  std::memset(data, 0, len);
  if (!valid_access(addr, len)) {
    return;
  }
  uint64_t off = addr - kBase;
  uint32_t shift = static_cast<uint32_t>((off & 4) * 8);
  uint64_t value = read_reg(off & ~uint64_t(4));
  WriteLe(data, len, value >> shift);
}

void Hpet::write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
  if (!valid_access(addr, len)) {
    return;
  }
  uint64_t off = addr - kBase;
  uint32_t shift = static_cast<uint32_t>((off & 4) * 8);
  uint32_t bits = std::min<uint32_t>(len * 8, 64 - shift);
  write_reg(off & ~uint64_t(4), shift, bits, ReadLe(data, len));
}

void Hpet::attach_irq_line(std::function<void(uint32_t, bool)> irq_line) {
  irq_line_ = std::move(irq_line);
}

std::vector<Hpet::Irq> Hpet::poll_expired() {
  std::vector<Irq> irqs;
  if ((config_ & kCfgEnable) == 0) {
    return irqs;
  }
  uint64_t now = counter();
  for (size_t i = 0; i < timers_.size(); i++) {
    Timer& timer = timers_[i];
    if (!timer.armed || (timer.config & kTimerEnable) == 0) {
      continue;
    }
    if (!counter_reached(now, timer.cmp64)) {
      continue;
    }
    Irq irq = route_for_timer(i);
    if (timer.config & kTimerTypeLevel) {
      isr_ |= uint64_t(1) << i;
      asserted_irqs_[i] = irq;
    }
    irqs.push_back(irq);
    if ((timer.config & kTimerPeriodic) != 0 && timer.period != 0) {
      do {
        timer.cmp64 += timer.period;
      } while (counter_reached(now, timer.cmp64));
      timer.cmp = (timer.config & kTimer32Bit) ? uint32_t(timer.cmp64) : timer.cmp64;
    } else {
      timer.armed = false;
    }
  }
  return irqs;
}

bool Hpet::legacy_mode() const { return (config_ & kCfgLegacy) != 0; }

bool Hpet::valid_access(uint64_t addr, uint32_t len) {
  return len != 0 && len <= 8 && addr >= kBase && addr < kBase + kSize &&
         len <= kBase + kSize - addr;
}

bool Hpet::counter_reached(uint64_t now, uint64_t target) {
  return static_cast<int64_t>(target - now) <= 0;
}

uint64_t Hpet::counter() const {
  if ((config_ & kCfgEnable) == 0) {
    return counter_base_;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     Clock::now() - counter_started_at_)
                     .count();
  uint64_t ticks = elapsed <= 0 ? 0 : static_cast<uint64_t>(elapsed) / kClockPeriodNs;
  return counter_base_ + ticks;
}

uint64_t Hpet::read_reg(uint64_t off) const {
  if (off <= 0xFF) {
    switch (off) {
      case 0x000:
        return kCapabilities;
      case 0x010:
        return config_;
      case 0x020:
        return isr_;
      case 0x0F0:
        return counter();
      default:
        return 0;
    }
  }
  if (off < 0x100) {
    return 0;
  }
  size_t timer_id = static_cast<size_t>((off - 0x100) / 0x20);
  if (timer_id >= timers_.size()) {
    return 0;
  }
  const Timer& timer = timers_[timer_id];
  switch (off & 0x18) {
    case 0x00:
      return timer.config;
    case 0x08:
      return timer.cmp;
    case 0x10:
      return 0;
    default:
      return 0;
  }
}

void Hpet::write_reg(uint64_t off, uint32_t shift, uint32_t bits, uint64_t value) {
  if (off <= 0xFF) {
    switch (off) {
      case 0x010:
        write_config(shift, bits, value);
        break;
      case 0x020: {
        uint64_t clear = DepositBits(0, shift, bits, value);
        uint64_t cleared = isr_ & clear;
        isr_ &= ~clear;
        deassert_cleared_irqs(cleared);
        break;
      }
      case 0x0F0:
        if ((config_ & kCfgEnable) == 0) {
          counter_base_ = DepositBits(counter_base_, shift, bits, value);
        }
        break;
      default:
        break;
    }
    return;
  }
  if (off < 0x100) {
    return;
  }
  size_t timer_id = static_cast<size_t>((off - 0x100) / 0x20);
  if (timer_id >= timers_.size()) {
    return;
  }
  switch (off & 0x18) {
    case 0x00:
      write_timer_config(timer_id, shift, bits, value);
      break;
    case 0x08:
      write_timer_cmp(timer_id, shift, bits, value);
      break;
    case 0x10:
      // FSB/MSI routing is intentionally not exposed (FSB cap stays clear),
      // so Linux should keep routing through the IOAPIC.
      break;
    default:
      break;
  }
}

void Hpet::write_config(uint32_t shift, uint32_t bits, uint64_t value) {
  uint64_t old = config_;
  uint64_t next = DepositBits(old, shift, bits, value);
  next = (next & kCfgWriteMask) | (old & ~kCfgWriteMask);
  bool was_enabled = (old & kCfgEnable) != 0;
  bool will_enable = (next & kCfgEnable) != 0;
  if (was_enabled && !will_enable) {
    counter_base_ = counter();
  }
  config_ = next;
  if (!was_enabled && will_enable) {
    counter_started_at_ = Clock::now();
  }
  if (will_enable) {
    reprogram_timers();
  } else {
    for (auto& timer : timers_) {
      timer.armed = false;
    }
    uint64_t cleared = isr_;
    isr_ = 0;
    deassert_cleared_irqs(cleared);
  }
}

void Hpet::write_timer_config(size_t timer_id, uint32_t shift, uint32_t bits, uint64_t value) {
  Timer& timer = timers_[timer_id];
  uint64_t old = timer.config;
  uint64_t next = DepositBits(old, shift, bits, value);
  next = (next & kTimerCfgWriteMask) | (old & ~kTimerCfgWriteMask);
  bool clear_level_irq =
      (old & kTimerTypeLevel) != 0 &&
      ((next & kTimerTypeLevel) == 0 || (next & kTimerEnable) == 0);
  if (clear_level_irq) {
    uint64_t clear = uint64_t(1) << timer_id;
    uint64_t cleared = isr_ & clear;
    isr_ &= ~clear;
    deassert_cleared_irqs(cleared);
  }
  timer.config = next;
  if (timer.config & kTimer32Bit) {
    timer.cmp = uint32_t(timer.cmp);
    timer.period = uint32_t(timer.period);
  }
  set_timer(timer_id);
}

void Hpet::write_timer_cmp(size_t timer_id, uint32_t shift, uint32_t bits, uint64_t value) {
  Timer& timer = timers_[timer_id];
  if (timer.config & kTimer32Bit) {
    if (shift != 0) {
      return;
    }
    bits = 64;
    value = uint32_t(value);
  }
  if ((timer.config & kTimerPeriodic) == 0 || (timer.config & kTimerSetVal) != 0) {
    timer.cmp = DepositBits(timer.cmp, shift, bits, value);
  }
  if (timer.config & kTimerPeriodic) {
    timer.period = DepositBits(timer.period, shift, bits, value);
  }
  timer.config &= ~kTimerSetVal;
  set_timer(timer_id);
}

void Hpet::reprogram_timers() {
  for (size_t i = 0; i < timers_.size(); i++) {
    set_timer(i);
  }
}

void Hpet::set_timer(size_t timer_id) {
  Timer& timer = timers_[timer_id];
  timer.armed = false;
  if ((config_ & kCfgEnable) == 0 || (timer.config & kTimerEnable) == 0) {
    return;
  }
  uint64_t now = counter();
  if (timer.config & kTimer32Bit) {
    timer.cmp64 = (now & ~0xFFFFFFFFULL) | uint32_t(timer.cmp);
    if (static_cast<int64_t>(timer.cmp64 - now) < 0) {
      timer.cmp64 += 0x100000000ULL;
    }
  } else {
    timer.cmp64 = timer.cmp;
  }
  timer.armed = true;
}

Hpet::Irq Hpet::route_for_timer(size_t timer_id) const {
  bool level = (timers_[timer_id].config & kTimerTypeLevel) != 0;
  if (timer_id <= 1 && (config_ & kCfgLegacy) != 0) {
    if (timer_id == 0) {
      return Irq{kTimer0IoApicPin, 0, level};
    }
    return Irq{8, 8, level};
  }
  uint32_t route = static_cast<uint32_t>(
      (timers_[timer_id].config & kTimerIntRouteMask) >> kTimerIntRouteShift);
  return Irq{route, 0xFF, level};
}

void Hpet::deassert_cleared_irqs(uint64_t cleared) {
  for (size_t i = 0; i < timers_.size(); i++) {
    if ((cleared & (uint64_t(1) << i)) == 0) {
      continue;
    }
    Irq irq = asserted_irqs_[i].value_or(route_for_timer(i));
    asserted_irqs_[i].reset();
    if (irq_line_ && irq.level && irq.ioapic_pin < 24) {
      irq_line_(irq.ioapic_pin, false);
    }
  }
}

}  // namespace node_vmm::whp
