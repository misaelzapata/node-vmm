#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace node_vmm::whp {

// Result of loading an ELF64 kernel into guest physical memory. `entry` is
// the kernel ELF entry point (passed to the BSP's RIP); `kernel_end` is the
// highest paddr+memsz across all PT_LOAD segments (used by the boot params
// builder to size the kernel image region).
struct KernelInfo {
  uint64_t entry{0};
  uint64_t kernel_end{0};
};

// Loads a vmlinux-style ELF64 kernel from `path` into the guest physical
// memory region `[mem, mem+mem_size)`. Each PT_LOAD segment is copied to
// its `paddr`, with the BSS region zero-filled. Throws std::runtime_error
// on any of: file too small, bad ELF magic, wrong class/endianness/machine,
// out-of-range program header offsets, segments exceeding guest memory.
//
// The returned KernelInfo is consumed by the BSP bootstrap path
// (SetupWhpBootstrapVcpu) to set RIP and by the boot params writer to
// place the cmdline / boot args after the kernel image.
KernelInfo LoadElfKernel(uint8_t* mem, uint64_t mem_size, const std::string& path);

}  // namespace node_vmm::whp
