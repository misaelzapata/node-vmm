#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <WinHvPlatform.h>

#include <cstdint>

namespace node_vmm::whp {

// Builds a 4-level x86_64 identity-map page table starting at `base` in
// guest physical memory. Covers 4 GiB using 2 MiB huge pages, present +
// writable + huge (PD entry flags 0x83). Layout:
//   base + 0x0000   PML4 (one entry -> PDPT)
//   base + 0x1000   PDPT (4 entries -> 4 PDs)
//   base + 0x2000   PD0  (512 huge-page entries, [0..1 GiB))
//   base + 0x3000   PD1  ([1..2 GiB))
//   base + 0x4000   PD2  ([2..3 GiB))
//   base + 0x5000   PD3  ([3..4 GiB))
// Caller passes `kPageTableBase` (0x9000) as `base` and uses `base` as
// CR3 for the BSP.
void BuildPageTables(uint8_t* mem, uint64_t base);

// Helpers used by SetupLongMode to populate WHV_X64_*_REGISTER values.
// `Segment(selector, attributes)` builds a generic segment descriptor with
// base=0/limit=0xffff (used for AP-startup real-mode segments and as the
// scaffolding for the long-mode helpers below).
WHV_X64_SEGMENT_REGISTER Segment(uint16_t selector, uint16_t attributes);

// Long-mode CS (selector 0x08, type 11 = exec/read code, L=1).
WHV_X64_SEGMENT_REGISTER LongCodeSegment();

// Long-mode DS/ES/SS/FS/GS (selector 0x10, type 3 = read/write data, D=1).
WHV_X64_SEGMENT_REGISTER LongDataSegment();

// Builds a WHV_X64_TABLE_REGISTER (used for GDT/IDT registers) with the
// given base and limit.
WHV_X64_TABLE_REGISTER Table(uint64_t base, uint16_t limit);

}  // namespace node_vmm::whp
