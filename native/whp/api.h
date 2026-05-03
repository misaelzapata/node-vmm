#pragma once

// Dynamic-loader for WinHvPlatform.dll + WinHvEmulation.dll. Holds one
// function pointer per WHP API call we use. Two construction modes:
//
//   * `require_all = true`  : every symbol must resolve, otherwise throw.
//                             Used by RunVm before partition creation.
//   * `require_all = false` : best-effort, only loads what's needed by
//                             the probe / smoke entry points so we can
//                             tell users which features are available
//                             without requiring the full DLL surface.
//
// Header definition (no .cc) so header-only consumers like the new
// per-module .cc files can call into WhpApi without a circular include.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

#include <stdexcept>
#include <string>

#include "../common/bytes.h"

namespace node_vmm::whp {

using WHvGetCapabilityFn = HRESULT(WINAPI*)(WHV_CAPABILITY_CODE, VOID*, UINT32, UINT32*);
using WHvCreatePartitionFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE*);
using WHvSetupPartitionFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE);
using WHvDeletePartitionFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE);
using WHvSetPartitionPropertyFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, WHV_PARTITION_PROPERTY_CODE, const VOID*, UINT32);
using WHvCreateVirtualProcessorFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, UINT32);
using WHvDeleteVirtualProcessorFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32);
using WHvMapGpaRangeFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, VOID*, WHV_GUEST_PHYSICAL_ADDRESS, UINT64, WHV_MAP_GPA_RANGE_FLAGS);
using WHvQueryGpaRangeDirtyBitmapFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, WHV_GUEST_PHYSICAL_ADDRESS, UINT64, UINT64*, UINT32);
using WHvTranslateGvaFn = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE,
    UINT32,
    WHV_GUEST_VIRTUAL_ADDRESS,
    WHV_TRANSLATE_GVA_FLAGS,
    WHV_TRANSLATE_GVA_RESULT*,
    WHV_GUEST_PHYSICAL_ADDRESS*);
using WHvRunVirtualProcessorFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, VOID*, UINT32);
using WHvCancelRunVirtualProcessorFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, UINT32);
using WHvGetVirtualProcessorRegistersFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME*, UINT32, WHV_REGISTER_VALUE*);
using WHvSetVirtualProcessorRegistersFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME*, UINT32, const WHV_REGISTER_VALUE*);
using WHvRequestInterruptFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, const WHV_INTERRUPT_CONTROL*, UINT32);
using WHvEmulatorCreateEmulatorFn = HRESULT(WINAPI*)(const WHV_EMULATOR_CALLBACKS*, WHV_EMULATOR_HANDLE*);
using WHvEmulatorDestroyEmulatorFn = HRESULT(WINAPI*)(WHV_EMULATOR_HANDLE);
using WHvEmulatorTryIoEmulationFn = HRESULT(WINAPI*)(
    WHV_EMULATOR_HANDLE,
    VOID*,
    const WHV_VP_EXIT_CONTEXT*,
    const WHV_X64_IO_PORT_ACCESS_CONTEXT*,
    WHV_EMULATOR_STATUS*);
using WHvEmulatorTryMmioEmulationFn = HRESULT(WINAPI*)(
    WHV_EMULATOR_HANDLE,
    VOID*,
    const WHV_VP_EXIT_CONTEXT*,
    const WHV_MEMORY_ACCESS_CONTEXT*,
    WHV_EMULATOR_STATUS*);

template <typename T>
inline T LoadSymbol(HMODULE dll, const char* name) {
  FARPROC proc = GetProcAddress(dll, name);
  if (!proc) {
    throw std::runtime_error(std::string("WinHvPlatform.dll does not export ") + name);
  }
  return reinterpret_cast<T>(proc);
}

struct WhpApi {
  HMODULE dll{nullptr};
  HMODULE emu_dll{nullptr};
  WHvGetCapabilityFn get_capability{nullptr};
  WHvCreatePartitionFn create_partition{nullptr};
  WHvSetupPartitionFn setup_partition{nullptr};
  WHvDeletePartitionFn delete_partition{nullptr};
  WHvSetPartitionPropertyFn set_partition_property{nullptr};
  WHvCreateVirtualProcessorFn create_vp{nullptr};
  WHvDeleteVirtualProcessorFn delete_vp{nullptr};
  WHvMapGpaRangeFn map_gpa_range{nullptr};
  WHvQueryGpaRangeDirtyBitmapFn query_dirty_bitmap{nullptr};
  WHvTranslateGvaFn translate_gva{nullptr};
  WHvRunVirtualProcessorFn run_vp{nullptr};
  WHvCancelRunVirtualProcessorFn cancel_run_vp{nullptr};
  WHvGetVirtualProcessorRegistersFn get_vp_registers{nullptr};
  WHvSetVirtualProcessorRegistersFn set_vp_registers{nullptr};
  WHvRequestInterruptFn request_interrupt{nullptr};
  WHvEmulatorCreateEmulatorFn emulator_create{nullptr};
  WHvEmulatorDestroyEmulatorFn emulator_destroy{nullptr};
  WHvEmulatorTryIoEmulationFn emulator_try_io{nullptr};
  WHvEmulatorTryMmioEmulationFn emulator_try_mmio{nullptr};

  explicit WhpApi(bool require_all) {
    dll = LoadLibraryW(L"WinHvPlatform.dll");
    if (!dll) {
      if (require_all) {
        throw std::runtime_error("WinHvPlatform.dll is not available: "
            + node_vmm::common::WindowsErrorMessage(GetLastError()));
      }
      return;
    }
    get_capability = LoadSymbol<WHvGetCapabilityFn>(dll, "WHvGetCapability");
    if (!require_all) {
      query_dirty_bitmap =
          reinterpret_cast<WHvQueryGpaRangeDirtyBitmapFn>(GetProcAddress(dll, "WHvQueryGpaRangeDirtyBitmap"));
      create_partition = reinterpret_cast<WHvCreatePartitionFn>(GetProcAddress(dll, "WHvCreatePartition"));
      setup_partition = reinterpret_cast<WHvSetupPartitionFn>(GetProcAddress(dll, "WHvSetupPartition"));
      delete_partition = reinterpret_cast<WHvDeletePartitionFn>(GetProcAddress(dll, "WHvDeletePartition"));
      set_partition_property =
          reinterpret_cast<WHvSetPartitionPropertyFn>(GetProcAddress(dll, "WHvSetPartitionProperty"));
      return;
    }
    create_partition = LoadSymbol<WHvCreatePartitionFn>(dll, "WHvCreatePartition");
    setup_partition = LoadSymbol<WHvSetupPartitionFn>(dll, "WHvSetupPartition");
    delete_partition = LoadSymbol<WHvDeletePartitionFn>(dll, "WHvDeletePartition");
    set_partition_property = LoadSymbol<WHvSetPartitionPropertyFn>(dll, "WHvSetPartitionProperty");
    create_vp = LoadSymbol<WHvCreateVirtualProcessorFn>(dll, "WHvCreateVirtualProcessor");
    delete_vp = LoadSymbol<WHvDeleteVirtualProcessorFn>(dll, "WHvDeleteVirtualProcessor");
    map_gpa_range = LoadSymbol<WHvMapGpaRangeFn>(dll, "WHvMapGpaRange");
    query_dirty_bitmap = LoadSymbol<WHvQueryGpaRangeDirtyBitmapFn>(dll, "WHvQueryGpaRangeDirtyBitmap");
    translate_gva = LoadSymbol<WHvTranslateGvaFn>(dll, "WHvTranslateGva");
    run_vp = LoadSymbol<WHvRunVirtualProcessorFn>(dll, "WHvRunVirtualProcessor");
    cancel_run_vp = LoadSymbol<WHvCancelRunVirtualProcessorFn>(dll, "WHvCancelRunVirtualProcessor");
    get_vp_registers = LoadSymbol<WHvGetVirtualProcessorRegistersFn>(dll, "WHvGetVirtualProcessorRegisters");
    set_vp_registers = LoadSymbol<WHvSetVirtualProcessorRegistersFn>(dll, "WHvSetVirtualProcessorRegisters");
    request_interrupt = LoadSymbol<WHvRequestInterruptFn>(dll, "WHvRequestInterrupt");

    emu_dll = LoadLibraryW(L"WinHvEmulation.dll");
    if (!emu_dll) {
      throw std::runtime_error("WinHvEmulation.dll is not available: "
          + node_vmm::common::WindowsErrorMessage(GetLastError()));
    }
    emulator_create = LoadSymbol<WHvEmulatorCreateEmulatorFn>(emu_dll, "WHvEmulatorCreateEmulator");
    emulator_destroy = LoadSymbol<WHvEmulatorDestroyEmulatorFn>(emu_dll, "WHvEmulatorDestroyEmulator");
    emulator_try_io = LoadSymbol<WHvEmulatorTryIoEmulationFn>(emu_dll, "WHvEmulatorTryIoEmulation");
    emulator_try_mmio = LoadSymbol<WHvEmulatorTryMmioEmulationFn>(emu_dll, "WHvEmulatorTryMmioEmulation");
  }

  WhpApi(const WhpApi&) = delete;
  WhpApi& operator=(const WhpApi&) = delete;

  ~WhpApi() {
    if (emu_dll) {
      FreeLibrary(emu_dll);
    }
    if (dll) {
      FreeLibrary(dll);
    }
  }
};

}  // namespace node_vmm::whp
