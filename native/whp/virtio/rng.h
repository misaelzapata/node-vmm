#pragma once

// virtio-rng-mmio device. Backs Linux's hwrng-virtio driver: the guest
// posts an empty write-only buffer on virtqueue 0, we fill it with random
// bytes via BCryptGenRandom (Windows CNG), push the used entry, and raise
// an interrupt.
//
// Decoupled from IoApic by accepting a `raise_irq` callback at
// construction. Caller supplies the routing.

#include "../guest_memory.h"

#include <cstdint>
#include <functional>

namespace node_vmm::whp {

class VirtioRng {
 public:
  // mmio_base: device's MMIO base address (e.g. 0xD0000400).
  // mem: guest RAM view used to dereference descriptor addresses.
  // raise_irq: invoked when the device wants to signal an interrupt to
  //   the driver. Caller routes to the right IOAPIC pin / PIC line.
  VirtioRng(uint64_t mmio_base, GuestMemory mem, std::function<void()> raise_irq);

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len);
  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len);

 private:
  struct Queue {
    uint32_t size{64};
    bool ready{false};
    uint16_t last_avail{0};
    uint64_t desc_addr{0};
    uint64_t driver_addr{0};
    uint64_t device_addr{0};
  };

  uint32_t read_reg(uint32_t off) const;
  void write_reg(uint32_t off, uint32_t value);
  void reset();

  uint8_t* ptr_or_null(uint64_t gpa, uint64_t len) const;
  void fill_random(uint8_t* dst, uint32_t len);

  // Walks the descriptor chain starting at `head`, fills any write-only
  // descriptor with random bytes via fill_random, returns total bytes
  // written.
  uint32_t service_chain(uint16_t head);

  void push_used(uint16_t id, uint32_t written);
  void handle_queue();

  uint64_t mmio_base_{0};
  GuestMemory mem_;
  std::function<void()> raise_irq_;
  Queue queue_{};
  uint32_t status_{0};
  uint64_t drv_features_{0};
  uint32_t dev_features_sel_{0};
  uint32_t drv_features_sel_{0};
  uint32_t queue_sel_{0};
  uint32_t interrupt_status_{0};
};

}  // namespace node_vmm::whp
