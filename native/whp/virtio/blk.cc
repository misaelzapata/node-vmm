#include "blk.h"

#include "desc.h"
#include "../../common/bytes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace node_vmm::whp {

using node_vmm::common::Check;
using node_vmm::common::CheckedAdd;
using node_vmm::common::CheckedMul;
using node_vmm::common::CheckRange;
using node_vmm::common::ReadU16;
using node_vmm::common::ReadU32;
using node_vmm::common::ReadU64;
using node_vmm::common::WindowsErrorMessage;
using node_vmm::common::WriteU16;
using node_vmm::common::WriteU32;
using node_vmm::common::WriteU64;

namespace v = node_vmm::whp::virtio;

VirtioBlk::VirtioBlk(
    uint64_t mmio_base,
    GuestMemory mem,
    const std::string& path,
    const std::string& overlay_path,
    bool read_only,
    std::function<void()> raise_irq)
    : mmio_base_(mmio_base), mem_(mem), raise_irq_(std::move(raise_irq)), read_only_(read_only) {
  bool overlay = !overlay_path.empty();
  Check(!(read_only_ && overlay), "read-only disk cannot use an overlay");
  DWORD access = (overlay || read_only_) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
  base_.reset(CreateFileA(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  Check(base_.valid(), "open rootfs " + path + " failed: " + WindowsErrorMessage(GetLastError()));
  LARGE_INTEGER size{};
  Check(GetFileSizeEx(base_.get(), &size) != 0, "stat rootfs " + path + " failed: " + WindowsErrorMessage(GetLastError()));
  Check(size.QuadPart >= 0, "negative rootfs size: " + path);
  disk_bytes_ = static_cast<uint64_t>(size.QuadPart);
  sectors_ = disk_bytes_ / 512;
  if (overlay) {
    Check(overlay_path != path, "overlayPath must not equal rootfsPath");
    overlay_.reset(CreateFileA(
        overlay_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    Check(overlay_.valid(), "open overlay " + overlay_path + " failed: " + WindowsErrorMessage(GetLastError()));
    MarkFileSparse(overlay_.get());
    TruncateFileTo(overlay_.get(), disk_bytes_);
    dirty_sectors_.assign(static_cast<size_t>((sectors_ + 7) / 8), 0);
  }
  queues_[0].size = v::kMaxQueueSize;
}

void VirtioBlk::read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
  std::memset(data, 0, len);
  uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
  if (off < 0x100) {
    if (len != 4) {
      return;
    }
    WriteU32(data, read_reg(off));
    return;
  }
  uint8_t cfg[16]{};
  WriteU64(cfg, sectors_);
  WriteU32(cfg + 8, 0);
  WriteU32(cfg + 12, 128);
  uint32_t cfg_off = off - 0x100;
  if (cfg_off < sizeof(cfg)) {
    std::memcpy(data, cfg + cfg_off, std::min<uint32_t>(len, sizeof(cfg) - cfg_off));
  }
}

void VirtioBlk::write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
  uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
  if (off >= 0x100 || len != 4) {
    return;
  }
  write_reg(off, ReadU32(data));
}

uint32_t VirtioBlk::read_reg(uint32_t off) {
  switch (off) {
    case v::kMmioMagicValue:
      return v::kMagic;
    case v::kMmioVersion:
      return v::kVersion;
    case v::kMmioDeviceId:
      return 2;  // virtio-blk
    case v::kMmioVendorId:
      return v::kVendor;
    case v::kMmioDeviceFeatures: {
      uint64_t features = v::kFVersion1 | (1ULL << 9);
      if (read_only_) {
        features |= 1ULL << 5;
      }
      return dev_features_sel_ == 1 ? uint32_t(features >> 32) : uint32_t(features);
    }
    case v::kMmioQueueNumMax:
      return v::kMaxQueueSize;
    case v::kMmioQueueReady:
      return queues_[queue_sel_].ready ? 1 : 0;
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

void VirtioBlk::write_reg(uint32_t off, uint32_t value) {
  Queue& q = queues_[queue_sel_];
  switch (off) {
    case v::kMmioDeviceFeaturesSel:
      dev_features_sel_ = value;
      break;
    case v::kMmioDriverFeaturesSel:
      drv_features_sel_ = value;
      break;
    case v::kMmioDriverFeatures:
      if (drv_features_sel_ == 0) {
        drv_features_ = (drv_features_ & ~0xFFFFFFFFULL) | value;
      } else {
        drv_features_ = (drv_features_ & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      }
      break;
    case v::kMmioQueueSel:
      if (value < 8) {
        queue_sel_ = value;
      }
      break;
    case v::kMmioQueueNum:
      if (value >= 1 && value <= v::kMaxQueueSize) {
        q.size = value;
      }
      break;
    case v::kMmioQueueReady:
      q.ready = value != 0;
      break;
    case v::kMmioQueueNotify:
      if (value < 8 && queues_[value].ready) {
        handle_queue(queues_[value]);
        interrupt_status_ |= v::kInterruptVring;
        if (raise_irq_) {
          raise_irq_();
        }
      }
      break;
    case v::kMmioInterruptAck:
      interrupt_status_ &= ~value;
      break;
    case v::kMmioStatus:
      if (value == 0) {
        reset();
      } else {
        status_ = value;
      }
      break;
    case v::kMmioQueueDescLow:
      q.desc_addr = (q.desc_addr & ~0xFFFFFFFFULL) | value;
      break;
    case v::kMmioQueueDescHigh:
      q.desc_addr = (q.desc_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      break;
    case v::kMmioQueueDriverLow:
      q.driver_addr = (q.driver_addr & ~0xFFFFFFFFULL) | value;
      break;
    case v::kMmioQueueDriverHigh:
      q.driver_addr = (q.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      break;
    case v::kMmioQueueDeviceLow:
      q.device_addr = (q.device_addr & ~0xFFFFFFFFULL) | value;
      break;
    case v::kMmioQueueDeviceHigh:
      q.device_addr = (q.device_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      break;
    default:
      break;
  }
}

void VirtioBlk::reset() {
  status_ = 0;
  drv_features_ = 0;
  dev_features_sel_ = 0;
  drv_features_sel_ = 0;
  queue_sel_ = 0;
  interrupt_status_ = 0;
  for (auto& q : queues_) {
    q = Queue{};
  }
}

// Walk a descriptor chain through the guest's virtq.
static v::DescChain walk_chain_inline(GuestMemory& mem, uint64_t desc_addr, uint32_t q_size, uint16_t head) {
  v::DescChain chain;
  std::array<uint8_t, v::kMaxQueueSize> seen{};
  uint16_t idx = head;
  for (;;) {
    Check(idx < q_size, "virtio descriptor next out of bounds");
    Check(seen[idx] == 0, "virtio descriptor cycle");
    seen[idx] = 1;
    Check(idx < q_size, "virtio descriptor index out of bounds");
    uint8_t* p = mem.ptr(desc_addr + uint64_t(idx) * 16, 16);
    v::Desc d{ReadU64(p), ReadU32(p + 8), ReadU16(p + 12), ReadU16(p + 14)};
    Check((d.flags & v::kRingDescFIndirect) == 0, "virtio indirect descriptors are not supported");
    chain.push(d);
    if ((d.flags & v::kRingDescFNext) == 0) {
      break;
    }
    idx = d.next;
  }
  return chain;
}

static void push_used_inline(GuestMemory& mem, uint64_t device_addr, uint32_t q_size, uint32_t id, uint32_t written) {
  uint8_t* idxp = mem.ptr(device_addr + 2, 2);
  uint16_t used = ReadU16(idxp);
  uint64_t entry = device_addr + 4 + uint64_t(used % q_size) * 8;
  WriteU32(mem.ptr(entry, 8), id);
  WriteU32(mem.ptr(entry + 4, 4), written);
  WriteU16(idxp, used + 1);
}

void VirtioBlk::handle_queue(Queue& q) {
  uint16_t avail_idx = ReadU16(mem_.ptr(q.driver_addr + 2, 2));
  while (q.last_avail != avail_idx) {
    uint16_t head = ReadU16(mem_.ptr(q.driver_addr + 4 + uint64_t(q.last_avail % q.size) * 2, 2));
    q.last_avail++;
    process_request(q, head);
  }
}

void VirtioBlk::process_request(Queue& q, uint16_t head) {
  uint8_t status = 0;
  uint32_t written = 0;
  try {
    v::DescChain chain = walk_chain_inline(mem_, q.desc_addr, q.size, head);
    Check(chain.size >= 2, "virtio-blk descriptor chain too short");
    v::Desc header = chain[0];
    v::Desc status_desc = chain[chain.size - 1];
    Check(header.len >= 16, "virtio-blk header too short");
    Check((status_desc.flags & v::kRingDescFWrite) != 0 && status_desc.len >= 1,
          "virtio-blk status descriptor invalid");
    uint8_t hdr[16];
    std::memcpy(hdr, mem_.ptr(header.addr, 16), sizeof(hdr));
    uint32_t type = ReadU32(hdr);
    uint64_t sector = ReadU64(hdr + 8);

    if (type == 0) {
      for (size_t i = 1; i + 1 < chain.size; i++) {
        v::Desc d = chain[i];
        Check((d.flags & v::kRingDescFWrite) != 0, "virtio-blk read data descriptor must be writable");
        Check((d.len % 512) == 0, "virtio-blk read length must be sector-aligned");
        ReadDisk(sector, mem_.ptr(d.addr, d.len), d.len);
        written += d.len;
        sector += d.len / 512;
      }
    } else if (type == 1) {
      for (size_t i = 1; i + 1 < chain.size; i++) {
        v::Desc d = chain[i];
        Check((d.flags & v::kRingDescFWrite) == 0, "virtio-blk write data descriptor must be read-only");
        Check((d.len % 512) == 0, "virtio-blk write length must be sector-aligned");
        WriteDisk(sector, mem_.ptr(d.addr, d.len), d.len);
        sector += d.len / 512;
      }
    } else if (type == 4) {
      if (!read_only_) {
        Check(FlushFileBuffers(write_handle()) != 0,
              "virtio-blk flush failed: " + WindowsErrorMessage(GetLastError()));
      }
    } else if (type == 8) {
      const char id[] = "node-vmm";
      for (size_t i = 1; i + 1 < chain.size; i++) {
        v::Desc d = chain[i];
        uint32_t n = std::min<uint32_t>(d.len, sizeof(id));
        std::memcpy(mem_.ptr(d.addr, n), id, n);
        written += n;
        break;
      }
    } else {
      status = 2;
    }
    std::memcpy(mem_.ptr(status_desc.addr, 1), &status, 1);
    push_used_inline(mem_, q.device_addr, q.size, head, written + 1);
  } catch (...) {
    status = 1;
    try {
      v::DescChain chain = walk_chain_inline(mem_, q.desc_addr, q.size, head);
      if (!chain.empty()) {
        v::Desc status_desc = chain[chain.size - 1];
        std::memcpy(mem_.ptr(status_desc.addr, 1), &status, 1);
      }
    } catch (...) {
    }
    push_used_inline(mem_, q.device_addr, q.size, head, written);
  }
}

bool VirtioBlk::has_overlay() const { return overlay_.valid(); }

HANDLE VirtioBlk::write_handle() const {
  return has_overlay() ? overlay_.get() : base_.get();
}

bool VirtioBlk::sector_dirty(uint64_t sector) const {
  if (!has_overlay() || sector >= sectors_) {
    return false;
  }
  return (dirty_sectors_[static_cast<size_t>(sector / 8)] & (uint8_t(1) << (sector % 8))) != 0;
}

void VirtioBlk::mark_dirty(uint64_t sector) {
  if (!has_overlay() || sector >= sectors_) {
    return;
  }
  dirty_sectors_[static_cast<size_t>(sector / 8)] |= uint8_t(1) << (sector % 8);
}

void VirtioBlk::mark_dirty_range(uint64_t byte_off, size_t len) {
  if (!has_overlay() || len == 0) {
    return;
  }
  uint64_t first = byte_off / 512;
  uint64_t last = (byte_off + len - 1) / 512;
  for (uint64_t sector = first; sector <= last; sector++) {
    mark_dirty(sector);
  }
}

void VirtioBlk::prepare_partial_overlay_sectors(uint64_t byte_off, size_t len) {
  if (!has_overlay() || len == 0) {
    return;
  }
  CheckRange(disk_bytes_, byte_off, len, "virtio-blk overlay prepare");
  uint64_t end = CheckedAdd(byte_off, len, "virtio-blk overlay range");
  uint64_t first = byte_off / 512;
  uint64_t last = (end - 1) / 512;
  std::array<uint8_t, 512> sector_buf{};
  for (uint64_t sector = first; sector <= last; sector++) {
    uint64_t sector_start = sector * 512;
    bool covers_full_sector = byte_off <= sector_start && end >= sector_start + 512;
    if (covers_full_sector || sector_dirty(sector)) {
      continue;
    }
    PreadAll(base_.get(), sector_buf.data(), sector_buf.size(), sector_start);
    PwriteAll(overlay_.get(), sector_buf.data(), sector_buf.size(), sector_start);
  }
}

void VirtioBlk::ReadDisk(uint64_t sector, uint8_t* dst, size_t len) {
  uint64_t byte_off = CheckedMul(sector, 512, "virtio-blk read offset");
  CheckRange(disk_bytes_, byte_off, len, "virtio-blk read");
  if (!has_overlay()) {
    PreadAll(base_.get(), dst, len, byte_off);
    return;
  }
  size_t done = 0;
  while (done < len) {
    uint64_t current_byte = byte_off + done;
    uint64_t current_sector = current_byte / 512;
    size_t in_sector = static_cast<size_t>(current_byte % 512);
    size_t chunk = std::min(len - done, size_t(512) - in_sector);
    HANDLE file = sector_dirty(current_sector) ? overlay_.get() : base_.get();
    PreadAll(file, dst + done, chunk, current_byte);
    done += chunk;
  }
}

void VirtioBlk::WriteDisk(uint64_t sector, const uint8_t* src, size_t len) {
  Check(!read_only_, "virtio-blk write to read-only disk");
  uint64_t byte_off = CheckedMul(sector, 512, "virtio-blk write offset");
  CheckRange(disk_bytes_, byte_off, len, "virtio-blk write");
  prepare_partial_overlay_sectors(byte_off, len);
  PwriteAll(write_handle(), src, len, byte_off);
  mark_dirty_range(byte_off, len);
}

}  // namespace node_vmm::whp
