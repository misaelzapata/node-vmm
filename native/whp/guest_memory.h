#pragma once

// Thin wrapper around the contiguous host buffer backing guest RAM. Used
// by every virtio device + boot-params writer to dereference guest
// physical addresses with bounds checking. No ownership: the buffer is
// managed by the partition's VirtualAllocMemory in backend.cc; this
// struct just gives device code a typed view.

#include "../common/bytes.h"

#include <cstdint>

namespace node_vmm::whp {

struct GuestMemory {
  uint8_t* data{nullptr};
  uint64_t len{0};

  uint8_t* ptr(uint64_t offset, uint64_t size) const {
    node_vmm::common::CheckRange(len, offset, size, "guest memory");
    return data + offset;
  }

  uint64_t size() const { return len; }
};

}  // namespace node_vmm::whp
