#pragma once

// Header-only support for virtio-mmio v2 device implementations. Holds
// the descriptor struct, the constants from the spec
// (qemu/include/standard-headers/linux/virtio_mmio.h + virtio_ring.h) we
// actually use, and a tiny helper for descriptor-chain walking.
//
// All virtio devices in this codebase (blk, rng, net) share these
// constants. Centralizing them here means a spec bump only needs to land
// in one place.

#include "../../common/bytes.h"

#include <array>
#include <cstdint>

namespace node_vmm::whp::virtio {

constexpr uint32_t kMagic = 0x74726976;        // "virt"
constexpr uint32_t kVendor = 0x554D4551;       // "QEMU" (matches host)
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxQueueSize = 256;

constexpr uint32_t kStatusAck = 0x01;
constexpr uint32_t kStatusDriver = 0x02;
constexpr uint32_t kStatusDriverOk = 0x04;
constexpr uint32_t kStatusFeaturesOk = 0x08;
constexpr uint32_t kStatusFailed = 0x80;

constexpr uint32_t kRingDescFNext = 1;
constexpr uint32_t kRingDescFWrite = 2;
constexpr uint32_t kRingDescFIndirect = 4;
constexpr uint16_t kRingAvailFNoInterrupt = 1;

constexpr uint64_t kFVersion1 = 1ULL << 32;
constexpr uint64_t kNetFMac = 1ULL << 5;
constexpr uint64_t kNetFStatus = 1ULL << 16;

constexpr uint32_t kInterruptVring = 0x1;
constexpr uint32_t kInterruptConfig = 0x2;

// virtio-mmio register offsets (from virtio_mmio.h).
constexpr uint16_t kMmioMagicValue = 0x000;
constexpr uint16_t kMmioVersion = 0x004;
constexpr uint16_t kMmioDeviceId = 0x008;
constexpr uint16_t kMmioVendorId = 0x00C;
constexpr uint16_t kMmioDeviceFeatures = 0x010;
constexpr uint16_t kMmioDeviceFeaturesSel = 0x014;
constexpr uint16_t kMmioDriverFeatures = 0x020;
constexpr uint16_t kMmioDriverFeaturesSel = 0x024;
constexpr uint16_t kMmioQueueSel = 0x030;
constexpr uint16_t kMmioQueueNumMax = 0x034;
constexpr uint16_t kMmioQueueNum = 0x038;
constexpr uint16_t kMmioQueueReady = 0x044;
constexpr uint16_t kMmioQueueNotify = 0x050;
constexpr uint16_t kMmioInterruptStatus = 0x060;
constexpr uint16_t kMmioInterruptAck = 0x064;
constexpr uint16_t kMmioStatus = 0x070;
constexpr uint16_t kMmioQueueDescLow = 0x080;
constexpr uint16_t kMmioQueueDescHigh = 0x084;
constexpr uint16_t kMmioQueueDriverLow = 0x090;
constexpr uint16_t kMmioQueueDriverHigh = 0x094;
constexpr uint16_t kMmioQueueDeviceLow = 0x0A0;
constexpr uint16_t kMmioQueueDeviceHigh = 0x0A4;
constexpr uint16_t kMmioShmSel = 0x0AC;
constexpr uint16_t kMmioShmLenLow = 0x0B0;
constexpr uint16_t kMmioShmLenHigh = 0x0B4;
constexpr uint16_t kMmioShmBaseLow = 0x0B8;
constexpr uint16_t kMmioShmBaseHigh = 0x0BC;
constexpr uint16_t kMmioConfigGeneration = 0x0FC;
constexpr uint16_t kMmioConfig = 0x100;

// virtq descriptor layout in host memory: addr (8) | len (4) | flags (2) | next (2).
struct Desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

// Walked descriptor chain. Bounded by kMaxQueueSize so we can stack-
// allocate without dynamic growth. push() throws on overflow rather
// than dropping descriptors.
struct DescChain {
  std::array<Desc, kMaxQueueSize> items{};
  size_t size{0};

  void push(Desc desc) {
    node_vmm::common::Check(size < items.size(), "virtio descriptor chain too long");
    items[size++] = desc;
  }

  Desc& operator[](size_t index) { return items[index]; }
  const Desc& operator[](size_t index) const { return items[index]; }
  bool empty() const { return size == 0; }
};

}  // namespace node_vmm::whp::virtio
