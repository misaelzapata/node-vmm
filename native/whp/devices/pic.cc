#include "pic.h"

#include "../irq.h"

#include <cstdio>
#include <windows.h>

namespace node_vmm::whp {

Pic::Pic(WhpApi& api, WHV_PARTITION_HANDLE partition)
    : api_(api), partition_(partition) {}

uint8_t Pic::read_port(uint16_t port) const {
  if (port == 0x21) {
    return master_.mask;
  }
  if (port == 0xA1) {
    return slave_.mask;
  }
  return 0;
}

void Pic::write_port(uint16_t port, uint8_t value) {
  switch (port) {
    case 0x20:
      write_command(master_, value);
      break;
    case 0x21:
      write_data(master_, value);
      break;
    case 0xA0:
      write_command(slave_, value);
      break;
    case 0xA1:
      write_data(slave_, value);
      break;
    default:
      break;
  }
}

bool Pic::request_irq(uint8_t irq) {
  if (irq < 8) {
    if ((master_.mask & (uint8_t(1) << irq)) != 0) {
      return false;
    }
    return RequestFixedInterrupt(api_, partition_, master_.vector + irq);
  }
  return false;
}

bool Pic::is_initialized() const { return master_.vector != 0x20; }

bool Pic::irq_unmasked(uint8_t irq) const {
  return irq < 8 && (master_.mask & (uint8_t(1) << irq)) == 0;
}

uint32_t Pic::vector_for_irq(uint8_t irq) const {
  if (irq < 8) {
    return master_.vector + irq;
  }
  return 0x20 + irq;
}

void Pic::write_command(Controller& controller, uint8_t value) {
  if ((value & 0x10) != 0) {
    controller.init_step = 2;
    controller.mask = 0xFF;
  }
  // EOI and other OCW commands are acknowledged by this minimal PIC.
}

void Pic::write_data(Controller& controller, uint8_t value) {
  if (controller.init_step == 2) {
    controller.vector = value;
    controller.init_step = 3;
    char env_check[8] = {0};
    GetEnvironmentVariableA("NODE_VMM_BOOT_TIME", env_check, sizeof(env_check));
    if (env_check[0] == '1') {
      std::fprintf(stderr, "[node-vmm pic] %s vector base programmed to 0x%02x\n",
                   &controller == &master_ ? "master" : "slave", value);
    }
    return;
  }
  if (controller.init_step == 3) {
    controller.init_step = 4;
    return;
  }
  if (controller.init_step == 4) {
    controller.init_step = 0;
    return;
  }
  controller.mask = value;
  char env_check[8] = {0};
  GetEnvironmentVariableA("NODE_VMM_BOOT_TIME", env_check, sizeof(env_check));
  if (env_check[0] == '1') {
    std::fprintf(stderr, "[node-vmm pic] %s mask=0x%02x\n",
                 &controller == &master_ ? "master" : "slave", value);
  }
}

}  // namespace node_vmm::whp
