#pragma once

#include <cstdint>
#include <string>

namespace node_vmm::whp {

// Writes the Linux x86 boot protocol "zero page" (boot_params) at the
// canonical kBootParamsAddr (0x7000) of guest memory plus an e820 memory
// map covering low-RAM, the legacy 9FC00..A0000 reserved hole, the
// E0000..100000 reserved region, and main RAM above 1 MiB. Also copies
// the kernel command line to kCmdlineAddr (0x20000) and points the
// boot_params.cmd_line_ptr field at it. Mirrors qemu/hw/i386/x86-common.c.
//
// Layout written into boot_params:
//   +0x1E8  e820_entries (4)
//   +0x1FA  vid_mode      (0xFFFF, "ask BIOS")
//   +0x1FE  boot signature (0xAA55)
//   +0x202  kernel_signature ("HdrS")
//   +0x210  type_of_loader (0xFF == "unknown loader, vmlinux ELF entry")
//   +0x211  loadflags |= LOADED_HIGH (bit 0)
//   +0x228  cmd_line_ptr  (kCmdlineAddr)
//   +0x238  cmd_line_size (cmdline.size())
//   +0x2D0  e820 table (4 entries x 20 bytes)
void WriteBootParams(uint8_t* mem, uint64_t mem_size, const std::string& cmdline);

// Writes the Intel MultiProcessor Specification configuration table near
// the top of low RAM (under the legacy 0xA0000 boundary). Linux still
// scans this region during early SMP bring-up if MADT is missing or
// disagrees. Entries:
//   * one Processor entry per CPU (BSP gets bit 1 set in flags)
//   * one Bus entry  (ISA)
//   * one IO APIC entry
//   * one IO Interrupt entry per legacy GSI 0..max_legacy_gsi (24 by
//     default), with IRQ0 routed to the override pin (`pit_io_apic_pin`,
//     normally 2 to match ACPI MADT's Interrupt Source Override)
//   * one ExtINT and one NMI Local Interrupt entry
void WriteMpTable(uint8_t* mem, uint64_t mem_size, int cpus, uint32_t pit_io_apic_pin);

}  // namespace node_vmm::whp
