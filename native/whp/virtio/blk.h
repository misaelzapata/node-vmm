#pragma once

// virtio-blk-mmio device. Backs Linux's virtio_blk driver: serves a
// rootfs.ext4 image via a single virtqueue, optionally with a
// copy-on-write overlay so the host filesystem stays clean. Mirrors
// qemu/hw/block/virtio-blk.c with the small subset Linux actually uses
// during boot:
//   * VIRTIO_BLK_T_IN  (read sectors)
//   * VIRTIO_BLK_T_OUT (write sectors)
//   * VIRTIO_BLK_T_FLUSH (FlushFileBuffers)
//   * VIRTIO_BLK_T_GET_ID (used by udev for stable device naming)
//
// Decoupled from IoApic by accepting a `raise_irq` callback at
// construction. Same pattern as VirtioRng (PR-5a).

#include "../guest_memory.h"
#include "../win_io.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace node_vmm::whp {

class VirtioBlk {
 public:
  // mmio_base: device's MMIO base address (e.g. 0xD0000000).
  // mem: guest RAM view used to dereference descriptor addresses.
  // path: rootfs.ext4 to serve.
  // overlay_path: optional copy-on-write overlay; empty disables.
  // raise_irq: invoked when the device wants to signal an interrupt.
  VirtioBlk(
      uint64_t mmio_base,
      GuestMemory mem,
      const std::string& path,
      const std::string& overlay_path,
      bool read_only,
      std::function<void()> raise_irq);

  uint64_t mmio_base() const { return mmio_base_; }
  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len);
  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len);

 private:
  struct Queue {
    uint32_t size{256};
    bool ready{false};
    uint16_t last_avail{0};
    uint64_t desc_addr{0};
    uint64_t driver_addr{0};
    uint64_t device_addr{0};
  };

  uint32_t read_reg(uint32_t off);
  void write_reg(uint32_t off, uint32_t value);
  void reset();

  void handle_queue(Queue& q);
  void process_request(Queue& q, uint16_t head);

  bool has_overlay() const;
  HANDLE write_handle() const;
  bool sector_dirty(uint64_t sector) const;
  void mark_dirty(uint64_t sector);
  void mark_dirty_range(uint64_t byte_off, size_t len);
  void prepare_partial_overlay_sectors(uint64_t byte_off, size_t len);
  void ReadDisk(uint64_t sector, uint8_t* dst, size_t len);
  void WriteDisk(uint64_t sector, const uint8_t* src, size_t len);

  uint64_t mmio_base_{0};
  GuestMemory mem_;
  std::function<void()> raise_irq_;
  WinHandle base_;
  WinHandle overlay_;
  std::vector<uint8_t> dirty_sectors_;
  uint64_t disk_bytes_{0};
  uint64_t sectors_{0};
  bool read_only_{false};
  uint32_t status_{0};
  uint64_t drv_features_{0};
  uint32_t dev_features_sel_{0};
  uint32_t drv_features_sel_{0};
  uint32_t queue_sel_{0};
  uint32_t interrupt_status_{0};
  Queue queues_[8];
};

}  // namespace node_vmm::whp
