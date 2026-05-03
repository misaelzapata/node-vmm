#include "boot_params.h"

#include "../common/bytes.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace node_vmm::whp {

using node_vmm::common::CheckRange;
using node_vmm::common::WriteU16;
using node_vmm::common::WriteU32;
using node_vmm::common::WriteU64;

namespace {

// Layout constants. Subsequent refactor PRs may lift these into a shared
// layout header once a third or fourth module needs to know them.
constexpr uint64_t kBootParamsAddr = 0x7000;
constexpr uint64_t kCmdlineAddr = 0x20000;
constexpr uint64_t kE820Offset = 0x2D0;
constexpr uint64_t kE820EntrySize = 20;
constexpr uint64_t kMaxE820Entries = 4;

void PutE820(uint8_t* mem, uint64_t off, uint64_t addr, uint64_t size, uint32_t type) {
  WriteU64(mem + off, addr);
  WriteU64(mem + off + 8, size);
  WriteU32(mem + off + 16, type);
}

uint8_t Checksum(const std::vector<uint8_t>& data, size_t start, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum = static_cast<uint8_t>(sum + data[start + i]);
  }
  return static_cast<uint8_t>(~sum + 1);
}

}  // namespace

void WriteBootParams(uint8_t* mem, uint64_t mem_size, const std::string& cmdline) {
  if (cmdline.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("kernel cmdline is too large");
  }
  const uint64_t cmdline_len = static_cast<uint64_t>(cmdline.size()) + 1;
  CheckRange(mem_size, kCmdlineAddr, cmdline_len, "kernel cmdline");
  CheckRange(mem_size, kBootParamsAddr + 0x1E8, 1, "boot params e820 count");
  CheckRange(mem_size, kBootParamsAddr + 0x1FA, 2, "boot params video mode");
  CheckRange(mem_size, kBootParamsAddr + 0x1FE, 2, "boot params setup signature");
  CheckRange(mem_size, kBootParamsAddr + 0x202, 4, "boot params header signature");
  CheckRange(mem_size, kBootParamsAddr + 0x210, 2, "boot params loader flags");
  CheckRange(mem_size, kBootParamsAddr + 0x228, 4, "boot params cmdline pointer");
  CheckRange(mem_size, kBootParamsAddr + 0x238, 4, "boot params cmdline length");
  CheckRange(
      mem_size,
      kBootParamsAddr + kE820Offset,
      kE820EntrySize * kMaxE820Entries,
      "boot params e820 table");
  std::memcpy(mem + kCmdlineAddr, cmdline.c_str(), cmdline.size() + 1);
  mem[kBootParamsAddr + 0x1E8] = 4;
  PutE820(mem, kBootParamsAddr + kE820Offset, 0x00000000, 0x0009FC00, 1);
  PutE820(mem, kBootParamsAddr + kE820Offset + kE820EntrySize, 0x0009FC00, 0x00040400, 2);
  PutE820(mem, kBootParamsAddr + kE820Offset + (2 * kE820EntrySize), 0x000E0000, 0x00020000, 2);
  if (mem_size > 0x00100000) {
    PutE820(mem, kBootParamsAddr + kE820Offset + (3 * kE820EntrySize), 0x00100000, mem_size - 0x00100000, 1);
  }
  WriteU16(mem + kBootParamsAddr + 0x1FA, 0xFFFF);
  WriteU16(mem + kBootParamsAddr + 0x1FE, 0xAA55);
  WriteU32(mem + kBootParamsAddr + 0x202, 0x53726448);
  mem[kBootParamsAddr + 0x210] = 0xFF;
  mem[kBootParamsAddr + 0x211] |= 0x01;
  WriteU32(mem + kBootParamsAddr + 0x228, static_cast<uint32_t>(kCmdlineAddr));
  WriteU32(mem + kBootParamsAddr + 0x238, static_cast<uint32_t>(cmdline.size()));
}

void WriteMpTable(uint8_t* mem, uint64_t mem_size, int cpus, uint32_t pit_io_apic_pin) {
  constexpr uint64_t base_ram_end = 0xA0000;
  constexpr uint32_t apic_default_base = 0xFEE00000;
  constexpr uint32_t io_apic_base = 0xFEC00000;
  constexpr uint8_t apic_version = 0x14;
  constexpr int max_legacy_gsi = 23;
  int size = 16 + 44 + (20 * cpus) + 8 + 8 + (8 * (max_legacy_gsi + 1)) + (8 * 2);
  uint64_t start = (base_ram_end - static_cast<uint64_t>(size)) & ~0xFULL;
  CheckRange(mem_size, start, static_cast<uint64_t>(size), "MP table");
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  uint32_t table_addr = static_cast<uint32_t>(start + 16);
  uint8_t io_apic_id = static_cast<uint8_t>(cpus + 1);

  std::memcpy(buf.data(), "_MP_", 4);
  WriteU32(buf.data() + 4, table_addr);
  buf[8] = 1;
  buf[9] = 4;
  buf[10] = Checksum(buf, 0, 16);

  std::vector<uint8_t> entries;
  uint16_t entry_count = 0;
  for (int cpu = 0; cpu < cpus; cpu++) {
    uint8_t entry[20]{};
    entry[0] = 0;
    entry[1] = static_cast<uint8_t>(cpu);
    entry[2] = apic_version;
    entry[3] = 0x01 | (cpu == 0 ? 0x02 : 0);
    WriteU32(entry + 4, 0x600);
    WriteU32(entry + 8, 0x200 | 0x001);
    entries.insert(entries.end(), entry, entry + sizeof(entry));
    entry_count++;
  }
  uint8_t bus[8]{};
  bus[0] = 1;
  std::memcpy(bus + 2, "ISA   ", 6);
  entries.insert(entries.end(), bus, bus + sizeof(bus));
  entry_count++;

  uint8_t ioapic[8]{};
  ioapic[0] = 2;
  ioapic[1] = io_apic_id;
  ioapic[2] = apic_version;
  ioapic[3] = 1;
  WriteU32(ioapic + 4, io_apic_base);
  entries.insert(entries.end(), ioapic, ioapic + sizeof(ioapic));
  entry_count++;

  for (int irq = 0; irq <= max_legacy_gsi; irq++) {
    uint8_t entry[8]{};
    entry[0] = 3;
    entry[1] = 0;
    entry[4] = 0;
    entry[5] = static_cast<uint8_t>(irq);
    entry[6] = io_apic_id;
    entry[7] = static_cast<uint8_t>(irq == 0 ? pit_io_apic_pin : static_cast<uint32_t>(irq));
    entries.insert(entries.end(), entry, entry + sizeof(entry));
    entry_count++;
  }
  uint8_t extint[8]{};
  extint[0] = 4;
  extint[1] = 3;
  entries.insert(entries.end(), extint, extint + sizeof(extint));
  entry_count++;

  uint8_t nmi[8]{};
  nmi[0] = 4;
  nmi[1] = 1;
  nmi[6] = 0xFF;
  nmi[7] = 1;
  entries.insert(entries.end(), nmi, nmi + sizeof(nmi));
  entry_count++;

  uint8_t* header = buf.data() + 16;
  std::memcpy(header, "PCMP", 4);
  WriteU16(header + 4, static_cast<uint16_t>(44 + entries.size()));
  header[6] = 4;
  std::memcpy(header + 8, "FC      ", 8);
  std::memcpy(header + 16, "000000000000", 12);
  WriteU16(header + 34, entry_count);
  WriteU32(header + 36, apic_default_base);
  std::memcpy(buf.data() + 16 + 44, entries.data(), entries.size());
  header[7] = Checksum(buf, 16, 44 + entries.size());
  std::memcpy(mem + start, buf.data(), buf.size());
}

}  // namespace node_vmm::whp
