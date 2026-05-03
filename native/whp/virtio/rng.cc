#include "rng.h"

#include "desc.h"
#include "../../common/bytes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace node_vmm::whp {

using node_vmm::common::Check;
using node_vmm::common::ReadU16;
using node_vmm::common::ReadU32;
using node_vmm::common::ReadU64;
using node_vmm::common::WriteU16;
using node_vmm::common::WriteU32;

namespace v = node_vmm::whp::virtio;

VirtioRng::VirtioRng(uint64_t mmio_base, GuestMemory mem, std::function<void()> raise_irq)
    : mmio_base_(mmio_base), mem_(mem), raise_irq_(std::move(raise_irq)) {}

void VirtioRng::read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
  std::memset(data, 0, len);
  if (len != 4) {
    return;
  }
  WriteU32(data, read_reg(static_cast<uint32_t>(addr - mmio_base_)));
}

void VirtioRng::write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
  if (len != 4) {
    return;
  }
  write_reg(static_cast<uint32_t>(addr - mmio_base_), ReadU32(data));
}

uint32_t VirtioRng::read_reg(uint32_t off) const {
  switch (off) {
    case v::kMmioMagicValue:
      return v::kMagic;
    case v::kMmioVersion:
      return v::kVersion;
    case v::kMmioDeviceId:
      return 4;  // virtio-rng
    case v::kMmioVendorId:
      return v::kVendor;
    case v::kMmioDeviceFeatures:
      return dev_features_sel_ == 1 ? static_cast<uint32_t>(v::kFVersion1 >> 32) : 0;
    case v::kMmioQueueNumMax:
      return 64;
    case v::kMmioQueueReady:
      return queue_.ready ? 1 : 0;
    case v::kMmioInterruptStatus:
      return interrupt_status_;
    case v::kMmioStatus:
      return status_;
    case v::kMmioConfigGeneration:
      return 0;
    default:
      return 0;
  }
}

void VirtioRng::write_reg(uint32_t off, uint32_t value) {
  switch (off) {
    case v::kMmioDeviceFeaturesSel:
      dev_features_sel_ = value;
      return;
    case v::kMmioDriverFeaturesSel:
      drv_features_sel_ = value;
      return;
    case v::kMmioDriverFeatures:
      if (drv_features_sel_ == 0) {
        drv_features_ = (drv_features_ & 0xFFFFFFFF00000000ULL) | value;
      } else {
        drv_features_ = (drv_features_ & 0x00000000FFFFFFFFULL) | (uint64_t(value) << 32);
      }
      return;
    case v::kMmioQueueSel:
      queue_sel_ = value;
      return;
    case v::kMmioQueueNum:
      if (queue_sel_ == 0 && value >= 1 && value <= 64) {
        queue_.size = value;
      }
      return;
    case v::kMmioQueueReady:
      if (queue_sel_ == 0) {
        queue_.ready = value != 0;
      }
      return;
    case v::kMmioQueueNotify:
      if (value == 0) {
        handle_queue();
      }
      return;
    case v::kMmioInterruptAck:
      interrupt_status_ &= ~value;
      return;
    case v::kMmioStatus:
      if (value == 0) {
        reset();
      } else {
        status_ = value;
      }
      return;
    case v::kMmioQueueDescLow:
      queue_.desc_addr = (queue_.desc_addr & 0xFFFFFFFF00000000ULL) | value;
      return;
    case v::kMmioQueueDescHigh:
      queue_.desc_addr = (queue_.desc_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      return;
    case v::kMmioQueueDriverLow:
      queue_.driver_addr = (queue_.driver_addr & 0xFFFFFFFF00000000ULL) | value;
      return;
    case v::kMmioQueueDriverHigh:
      queue_.driver_addr = (queue_.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      return;
    case v::kMmioQueueDeviceLow:
      queue_.device_addr = (queue_.device_addr & 0xFFFFFFFF00000000ULL) | value;
      return;
    case v::kMmioQueueDeviceHigh:
      queue_.device_addr = (queue_.device_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      return;
    default:
      return;
  }
}

void VirtioRng::reset() {
  status_ = 0;
  drv_features_ = 0;
  dev_features_sel_ = 0;
  drv_features_sel_ = 0;
  queue_sel_ = 0;
  interrupt_status_ = 0;
  queue_ = Queue{};
}

uint8_t* VirtioRng::ptr_or_null(uint64_t gpa, uint64_t len) const {
  if (gpa > mem_.size() || len > mem_.size() - gpa) {
    return nullptr;
  }
  return mem_.data + gpa;
}

void VirtioRng::fill_random(uint8_t* dst, uint32_t len) {
  while (len > 0) {
    ULONG chunk = std::min<uint32_t>(len, 1U << 20);
    NTSTATUS status = BCryptGenRandom(nullptr, dst, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    Check(status >= 0, "BCryptGenRandom failed");
    dst += chunk;
    len -= chunk;
  }
}

uint32_t VirtioRng::service_chain(uint16_t head) {
  uint32_t written = 0;
  std::array<uint8_t, 64> seen{};
  uint16_t idx = head;
  for (uint32_t hops = 0; hops < queue_.size; hops++) {
    if (idx >= queue_.size || seen[idx] != 0) {
      break;
    }
    seen[idx] = 1;
    uint8_t* p = ptr_or_null(queue_.desc_addr + uint64_t(idx) * 16, 16);
    if (p == nullptr) {
      break;
    }
    v::Desc desc{ReadU64(p), ReadU32(p + 8), ReadU16(p + 12), ReadU16(p + 14)};
    if ((desc.flags & v::kRingDescFWrite) != 0 && desc.len > 0) {
      uint8_t* dst = ptr_or_null(desc.addr, desc.len);
      if (dst != nullptr) {
        fill_random(dst, desc.len);
        written += desc.len;
      }
    }
    if ((desc.flags & v::kRingDescFNext) == 0) {
      break;
    }
    idx = desc.next;
  }
  return written;
}

void VirtioRng::push_used(uint16_t id, uint32_t written) {
  uint8_t* idxp = ptr_or_null(queue_.device_addr + 2, 2);
  if (idxp == nullptr || queue_.size == 0) {
    return;
  }
  uint16_t used = ReadU16(idxp);
  uint64_t entry = queue_.device_addr + 4 + uint64_t(used % queue_.size) * 8;
  uint8_t* elem = ptr_or_null(entry, 8);
  if (elem == nullptr) {
    return;
  }
  WriteU32(elem, id);
  WriteU32(elem + 4, written);
  WriteU16(idxp, used + 1);
}

void VirtioRng::handle_queue() {
  if (!queue_.ready || queue_.size == 0 || queue_.desc_addr == 0 ||
      queue_.driver_addr == 0 || queue_.device_addr == 0) {
    return;
  }
  uint8_t* avail_idx_ptr = ptr_or_null(queue_.driver_addr + 2, 2);
  if (avail_idx_ptr == nullptr) {
    return;
  }
  uint16_t avail_idx = ReadU16(avail_idx_ptr);
  bool processed = false;
  while (queue_.last_avail != avail_idx) {
    uint64_t ring_addr = queue_.driver_addr + 4 + uint64_t(queue_.last_avail % queue_.size) * 2;
    uint8_t* headp = ptr_or_null(ring_addr, 2);
    if (headp == nullptr) {
      break;
    }
    uint16_t head = ReadU16(headp);
    queue_.last_avail++;
    push_used(head, service_chain(head));
    processed = true;
  }
  if (!processed) {
    return;
  }
  uint16_t flags = 0;
  if (uint8_t* flagsp = ptr_or_null(queue_.driver_addr, 2)) {
    flags = ReadU16(flagsp);
  }
  if ((flags & v::kRingAvailFNoInterrupt) == 0) {
    interrupt_status_ |= v::kInterruptVring;
    if (raise_irq_) {
      raise_irq_();
    }
  }
}

}  // namespace node_vmm::whp
