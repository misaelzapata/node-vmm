#include "page_tables.h"

#include "../common/bytes.h"

namespace node_vmm::whp {

using node_vmm::common::WriteU64;

void BuildPageTables(uint8_t* mem, uint64_t base) {
  constexpr uint64_t page_size = 0x1000;
  constexpr uint64_t flags = 0x07;  // P | RW | US
  constexpr uint64_t huge = 0x83;   // P | RW | PS (huge page)
  uint64_t pml4 = base;
  uint64_t pdpt = base + page_size;
  uint64_t pd = base + 2 * page_size;
  WriteU64(mem + pml4, pdpt | flags);
  for (uint64_t i = 0; i < 4; i++) {
    uint64_t this_pd = pd + i * page_size;
    WriteU64(mem + pdpt + i * 8, this_pd | flags);
    for (uint64_t j = 0; j < 512; j++) {
      uint64_t phys = (i * 512 + j) * 0x200000ULL;
      WriteU64(mem + this_pd + j * 8, phys | huge);
    }
  }
}

WHV_X64_SEGMENT_REGISTER Segment(uint16_t selector, uint16_t attributes) {
  WHV_X64_SEGMENT_REGISTER segment{};
  segment.Base = 0;
  segment.Limit = 0xffff;
  segment.Selector = selector;
  segment.Attributes = attributes;
  return segment;
}

WHV_X64_SEGMENT_REGISTER LongCodeSegment() {
  WHV_X64_SEGMENT_REGISTER segment = Segment(0x08, 0);
  segment.Limit = 0xFFFFF;
  segment.SegmentType = 11;
  segment.NonSystemSegment = 1;
  segment.Present = 1;
  segment.Long = 1;
  segment.Granularity = 1;
  return segment;
}

WHV_X64_SEGMENT_REGISTER LongDataSegment() {
  WHV_X64_SEGMENT_REGISTER segment = Segment(0x10, 0);
  segment.Limit = 0xFFFFF;
  segment.SegmentType = 3;
  segment.NonSystemSegment = 1;
  segment.Present = 1;
  segment.Default = 1;
  segment.Granularity = 1;
  return segment;
}

WHV_X64_TABLE_REGISTER Table(uint64_t base, uint16_t limit) {
  WHV_X64_TABLE_REGISTER table{};
  table.Base = base;
  table.Limit = limit;
  return table;
}

}  // namespace node_vmm::whp
