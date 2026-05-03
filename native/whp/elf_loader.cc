#include "elf_loader.h"

#include "../common/bytes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace node_vmm::whp {

using node_vmm::common::Check;
using node_vmm::common::CheckedAdd;
using node_vmm::common::CheckedMul;
using node_vmm::common::CheckRange;
using node_vmm::common::WindowsErrorMessage;

namespace {

// ELF64 layout constants. Mirrors the values originally inlined into
// native/whp/backend.cc; kept TU-local so we don't grow whp_common.h yet.
constexpr unsigned char kElfMagic[] = {0x7f, 'E', 'L', 'F'};
constexpr uint16_t kElfClass64 = 2;
constexpr uint16_t kElfDataLe = 1;
constexpr uint16_t kElfMachineX64 = 62;
constexpr uint32_t kElfPtLoad = 1;

#pragma pack(push, 1)
struct Elf64Ehdr {
  unsigned char ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
};

struct Elf64Phdr {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
};
#pragma pack(pop)

std::vector<uint8_t> ReadWholeFile(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "open " + path + " failed: " + WindowsErrorMessage(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) {
    DWORD error = GetLastError();
    CloseHandle(file);
    throw std::runtime_error("stat " + path + " failed: " + WindowsErrorMessage(error));
  }
  Check(size.QuadPart >= 0 && size.QuadPart <= INT32_MAX, "file is too large: " + path);
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < data.size()) {
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1U << 20));
    if (!ReadFile(file, data.data() + done, want, &got, nullptr)) {
      DWORD error = GetLastError();
      CloseHandle(file);
      throw std::runtime_error("read " + path + " failed: " + WindowsErrorMessage(error));
    }
    Check(got != 0, "short read " + path);
    done += got;
  }
  CloseHandle(file);
  return data;
}

}  // namespace

KernelInfo LoadElfKernel(uint8_t* mem, uint64_t mem_size, const std::string& path) {
  std::vector<uint8_t> data = ReadWholeFile(path);
  Check(data.size() >= sizeof(Elf64Ehdr), "kernel is too small");
  const auto* eh = reinterpret_cast<const Elf64Ehdr*>(data.data());
  Check(std::memcmp(eh->ident, kElfMagic, sizeof(kElfMagic)) == 0, "kernel must be an ELF vmlinux for WHP v1");
  Check(eh->ident[4] == kElfClass64, "kernel must be ELF64");
  Check(eh->ident[5] == kElfDataLe, "kernel must be little-endian ELF");
  Check(eh->machine == kElfMachineX64, "kernel must be x86_64 ELF");
  Check(eh->phentsize == sizeof(Elf64Phdr), "kernel ELF program header size is unsupported");
  uint64_t ph_size = CheckedMul(uint64_t(eh->phnum), sizeof(Elf64Phdr), "kernel program header table");
  CheckRange(data.size(), eh->phoff, ph_size, "kernel program header table");

  KernelInfo info{};
  info.entry = eh->entry;
  for (uint16_t i = 0; i < eh->phnum; i++) {
    uint64_t ph_off = CheckedAdd(eh->phoff, CheckedMul(uint64_t(i), sizeof(Elf64Phdr), "kernel program header offset"),
                                 "kernel program header offset");
    const auto* ph = reinterpret_cast<const Elf64Phdr*>(data.data() + ph_off);
    if (ph->type != kElfPtLoad) {
      continue;
    }
    Check(ph->filesz <= ph->memsz, "kernel segment file size exceeds memory size");
    CheckRange(data.size(), ph->offset, ph->filesz, "kernel segment file range");
    CheckRange(mem_size, ph->paddr, ph->memsz, "kernel segment guest range");
    std::memcpy(mem + ph->paddr, data.data() + ph->offset, static_cast<size_t>(ph->filesz));
    if (ph->memsz > ph->filesz) {
      uint64_t zero_start = CheckedAdd(ph->paddr, ph->filesz, "kernel zero-fill range");
      std::memset(mem + zero_start, 0, static_cast<size_t>(ph->memsz - ph->filesz));
    }
    info.kernel_end = std::max(info.kernel_end, CheckedAdd(ph->paddr, ph->memsz, "kernel end"));
  }
  Check(info.entry != 0, "kernel ELF entrypoint is zero");
  return info;
}

}  // namespace node_vmm::whp
