#pragma once

// Minimal 8259A PIC pair (master + slave). Mirrors qemu/hw/intc/i8259.c
// just enough to satisfy Linux's init_8259A: ICW1-4 reprogramming sequence
// with mask + vector base, EOI/OCW2 acknowledged silently, and an explicit
// `is_initialized()` predicate the dispatcher uses to gate ExtInt delivery
// until the kernel has actually remapped the vector base off 0x20.
//
// `request_irq` short-circuits the IOAPIC and goes through
// WHvRequestInterrupt (irq.h::RequestFixedInterrupt) on the master line.
// Slave-line IRQs (8..15) are not routed through this path.

#include "../api.h"

#include <cstdint>

namespace node_vmm::whp {

class Pic {
 public:
  Pic(WhpApi& api, WHV_PARTITION_HANDLE partition);

  uint8_t read_port(uint16_t port) const;
  void write_port(uint16_t port, uint8_t value);

  bool request_irq(uint8_t irq);

  // True once the kernel has written ICW2 (vector base) to the PIC. The
  // master starts with vector base 0x20 (BIOS reset value); Linux remaps
  // it to 0x30 in init_8259A, so a non-0x20 value confirms the kernel has
  // taken ownership and ExtInts are safe to deliver.
  bool is_initialized() const;

  bool irq_unmasked(uint8_t irq) const;

  uint32_t vector_for_irq(uint8_t irq) const;

 private:
  struct Controller {
    uint8_t vector{0x20};
    uint8_t mask{0xFF};
    uint8_t init_step{0};
  };

  void write_command(Controller& controller, uint8_t value);
  void write_data(Controller& controller, uint8_t value);

  WhpApi& api_;
  WHV_PARTITION_HANDLE partition_{nullptr};
  Controller master_{0x20, 0xFE, 0};
  Controller slave_{0x28, 0xFF, 0};
};

}  // namespace node_vmm::whp
