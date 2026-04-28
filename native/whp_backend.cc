#include <node_api.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <WinHvPlatform.h>

#include <stdint.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kGuestCodeAddr = 0x1000;
constexpr uint64_t kGuestWriteAddr = 0x2000;
constexpr uint64_t kGuestRamBytes = 0x10000;

std::string WindowsErrorMessage(DWORD code) {
  char* buffer = nullptr;
  DWORD size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer),
      0,
      nullptr);
  std::string message = size && buffer ? std::string(buffer, size) : "unknown Windows error";
  if (buffer) {
    LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
    message.pop_back();
  }
  return message;
}

std::string HresultMessage(HRESULT hr, const std::string& what) {
  char code[16];
  std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(hr));
  return what + " failed with HRESULT " + code + ": " + WindowsErrorMessage(static_cast<DWORD>(hr));
}

void Check(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

void CheckHr(HRESULT hr, const std::string& what) {
  if (FAILED(hr)) {
    throw std::runtime_error(HresultMessage(hr, what));
  }
}

napi_value MakeObject(napi_env env) {
  napi_value obj;
  napi_create_object(env, &obj);
  return obj;
}

void SetString(napi_env env, napi_value obj, const char* name, const std::string& value) {
  napi_value v;
  napi_create_string_utf8(env, value.c_str(), value.size(), &v);
  napi_set_named_property(env, obj, name, v);
}

void SetBool(napi_env env, napi_value obj, const char* name, bool value) {
  napi_value v;
  napi_get_boolean(env, value, &v);
  napi_set_named_property(env, obj, name, v);
}

void SetUint32(napi_env env, napi_value obj, const char* name, uint32_t value) {
  napi_value v;
  napi_create_uint32(env, value, &v);
  napi_set_named_property(env, obj, name, v);
}

void SetDouble(napi_env env, napi_value obj, const char* name, double value) {
  napi_value v;
  napi_create_double(env, value, &v);
  napi_set_named_property(env, obj, name, v);
}

napi_value Throw(napi_env env, const std::exception& err) {
  napi_throw_error(env, nullptr, err.what());
  return nullptr;
}

template <typename T>
T LoadSymbol(HMODULE dll, const char* name) {
  FARPROC proc = GetProcAddress(dll, name);
  if (!proc) {
    throw std::runtime_error(std::string("WinHvPlatform.dll does not export ") + name);
  }
  return reinterpret_cast<T>(proc);
}

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
using WHvRunVirtualProcessorFn = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, VOID*, UINT32);
using WHvSetVirtualProcessorRegistersFn =
    HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME*, UINT32, const WHV_REGISTER_VALUE*);

struct WhpApi {
  HMODULE dll{nullptr};
  WHvGetCapabilityFn get_capability{nullptr};
  WHvCreatePartitionFn create_partition{nullptr};
  WHvSetupPartitionFn setup_partition{nullptr};
  WHvDeletePartitionFn delete_partition{nullptr};
  WHvSetPartitionPropertyFn set_partition_property{nullptr};
  WHvCreateVirtualProcessorFn create_vp{nullptr};
  WHvDeleteVirtualProcessorFn delete_vp{nullptr};
  WHvMapGpaRangeFn map_gpa_range{nullptr};
  WHvQueryGpaRangeDirtyBitmapFn query_dirty_bitmap{nullptr};
  WHvRunVirtualProcessorFn run_vp{nullptr};
  WHvSetVirtualProcessorRegistersFn set_vp_registers{nullptr};

  explicit WhpApi(bool require_all) {
    dll = LoadLibraryW(L"WinHvPlatform.dll");
    if (!dll) {
      if (require_all) {
        throw std::runtime_error("WinHvPlatform.dll is not available: " + WindowsErrorMessage(GetLastError()));
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
    run_vp = LoadSymbol<WHvRunVirtualProcessorFn>(dll, "WHvRunVirtualProcessor");
    set_vp_registers = LoadSymbol<WHvSetVirtualProcessorRegistersFn>(dll, "WHvSetVirtualProcessorRegisters");
  }

  WhpApi(const WhpApi&) = delete;
  WhpApi& operator=(const WhpApi&) = delete;

  ~WhpApi() {
    if (dll) {
      FreeLibrary(dll);
    }
  }
};

struct Partition {
  WhpApi& api;
  WHV_PARTITION_HANDLE handle{nullptr};

  explicit Partition(WhpApi& api_ref) : api(api_ref) {
    CheckHr(api.create_partition(&handle), "WHvCreatePartition");
  }

  Partition(const Partition&) = delete;
  Partition& operator=(const Partition&) = delete;

  ~Partition() {
    if (handle) {
      api.delete_partition(handle);
    }
  }
};

struct VirtualProcessor {
  WhpApi& api;
  WHV_PARTITION_HANDLE partition{nullptr};
  UINT32 index{0};
  bool created{false};

  VirtualProcessor(WhpApi& api_ref, WHV_PARTITION_HANDLE partition_handle, UINT32 vp_index)
      : api(api_ref), partition(partition_handle), index(vp_index) {
    CheckHr(api.create_vp(partition, index, 0), "WHvCreateVirtualProcessor");
    created = true;
  }

  VirtualProcessor(const VirtualProcessor&) = delete;
  VirtualProcessor& operator=(const VirtualProcessor&) = delete;

  ~VirtualProcessor() {
    if (created) {
      api.delete_vp(partition, index);
    }
  }
};

struct VirtualAllocMemory {
  void* ptr{nullptr};

  explicit VirtualAllocMemory(size_t bytes) {
    ptr = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Check(ptr != nullptr, "VirtualAlloc guest RAM failed: " + WindowsErrorMessage(GetLastError()));
  }

  VirtualAllocMemory(const VirtualAllocMemory&) = delete;
  VirtualAllocMemory& operator=(const VirtualAllocMemory&) = delete;

  ~VirtualAllocMemory() {
    if (ptr) {
      VirtualFree(ptr, 0, MEM_RELEASE);
    }
  }

  uint8_t* bytes() const { return reinterpret_cast<uint8_t*>(ptr); }
};

WHV_X64_SEGMENT_REGISTER Segment(uint16_t selector, uint16_t attributes) {
  WHV_X64_SEGMENT_REGISTER segment{};
  segment.Base = 0;
  segment.Limit = 0xffff;
  segment.Selector = selector;
  segment.Attributes = attributes;
  return segment;
}

std::string WhpExitReason(uint32_t reason) {
  switch (reason) {
    case WHvRunVpExitReasonX64Halt:
      return "hlt";
    case WHvRunVpExitReasonMemoryAccess:
      return "memory-access";
    case WHvRunVpExitReasonX64IoPortAccess:
      return "io-port";
    case WHvRunVpExitReasonUnrecoverableException:
      return "unrecoverable-exception";
    case WHvRunVpExitReasonInvalidVpRegisterValue:
      return "invalid-vp-register";
    case WHvRunVpExitReasonUnsupportedFeature:
      return "unsupported-feature";
    case WHvRunVpExitReasonX64InterruptWindow:
      return "interrupt-window";
    case WHvRunVpExitReasonCanceled:
      return "canceled";
    default:
      return "whp-exit-" + std::to_string(reason);
  }
}

uint32_t CountBits(uint64_t value) {
  uint32_t count = 0;
  while (value) {
    value &= value - 1;
    count++;
  }
  return count;
}

bool ProbePartition(WhpApi& api, bool* setup_ok, std::string* reason) {
  *setup_ok = false;
  if (!api.create_partition || !api.set_partition_property || !api.setup_partition || !api.delete_partition) {
    *reason = "WinHvPlatform.dll is missing partition lifecycle exports";
    return false;
  }
  Partition partition(api);
  WHV_PARTITION_PROPERTY property{};
  property.ProcessorCount = 1;
  HRESULT hr = api.set_partition_property(
      partition.handle,
      WHvPartitionPropertyCodeProcessorCount,
      &property,
      sizeof(property));
  if (FAILED(hr)) {
    *reason = HresultMessage(hr, "WHvSetPartitionProperty(ProcessorCount)");
    return true;
  }
  hr = api.setup_partition(partition.handle);
  if (FAILED(hr)) {
    *reason = HresultMessage(hr, "WHvSetupPartition");
    return true;
  }
  *setup_ok = true;
  return true;
}

napi_value ProbeWhp(napi_env env, napi_callback_info) {
  napi_value out = MakeObject(env);
  SetString(env, out, "backend", "whp");
  SetString(env, out, "arch", "x86_64");
  SetBool(env, out, "available", false);
  SetBool(env, out, "hypervisorPresent", false);
  SetBool(env, out, "dirtyPageTracking", false);
  SetBool(env, out, "queryDirtyBitmapExport", false);
  SetBool(env, out, "partitionCreate", false);
  SetBool(env, out, "partitionSetup", false);

  try {
    WhpApi api(false);
    if (!api.dll) {
      SetString(env, out, "reason", "WinHvPlatform.dll is not available");
      return out;
    }
    SetBool(env, out, "queryDirtyBitmapExport", api.query_dirty_bitmap != nullptr);

    WHV_CAPABILITY present{};
    UINT32 written = 0;
    HRESULT hr = api.get_capability(WHvCapabilityCodeHypervisorPresent, &present, sizeof(present), &written);
    if (FAILED(hr)) {
      SetString(env, out, "reason", HresultMessage(hr, "WHvGetCapability(HypervisorPresent)"));
      return out;
    }
    SetBool(env, out, "hypervisorPresent", present.HypervisorPresent != FALSE);

    WHV_CAPABILITY features{};
    hr = api.get_capability(WHvCapabilityCodeFeatures, &features, sizeof(features), &written);
    if (SUCCEEDED(hr)) {
      SetBool(env, out, "dirtyPageTracking", features.Features.DirtyPageTracking != 0);
    }

    bool setup_ok = false;
    std::string reason;
    bool partition_ok = ProbePartition(api, &setup_ok, &reason);
    SetBool(env, out, "partitionCreate", partition_ok);
    SetBool(env, out, "partitionSetup", setup_ok);
    SetBool(env, out, "available", present.HypervisorPresent != FALSE && partition_ok && setup_ok);
    if (!reason.empty()) {
      SetString(env, out, "reason", reason);
    }
    return out;
  } catch (const std::exception& err) {
    SetString(env, out, "reason", err.what());
    return out;
  }
}

napi_value WhpSmokeHlt(napi_env env, napi_callback_info) {
  try {
    auto start = std::chrono::steady_clock::now();
    WhpApi api(true);
    Partition partition(api);

    WHV_PARTITION_PROPERTY property{};
    property.ProcessorCount = 1;
    CheckHr(
        api.set_partition_property(
            partition.handle,
            WHvPartitionPropertyCodeProcessorCount,
            &property,
            sizeof(property)),
        "WHvSetPartitionProperty(ProcessorCount)");
    CheckHr(api.setup_partition(partition.handle), "WHvSetupPartition");

    VirtualAllocMemory ram(kGuestRamBytes);
    uint8_t* bytes = ram.bytes();
    bytes[kGuestCodeAddr + 0] = 0xc6;  // mov byte ptr [0x2000], 0x41
    bytes[kGuestCodeAddr + 1] = 0x06;
    bytes[kGuestCodeAddr + 2] = static_cast<uint8_t>(kGuestWriteAddr & 0xff);
    bytes[kGuestCodeAddr + 3] = static_cast<uint8_t>((kGuestWriteAddr >> 8) & 0xff);
    bytes[kGuestCodeAddr + 4] = 0x41;
    bytes[kGuestCodeAddr + 5] = 0xf4;  // hlt

    WHV_MAP_GPA_RANGE_FLAGS flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        0x00000001 | 0x00000002 | 0x00000004 | 0x00000008);
    CheckHr(api.map_gpa_range(partition.handle, ram.ptr, 0, kGuestRamBytes, flags), "WHvMapGpaRange");

    VirtualProcessor vp(api, partition.handle, 0);

    WHV_REGISTER_NAME names[] = {
        WHvX64RegisterRip,
        WHvX64RegisterRflags,
        WHvX64RegisterCs,
        WHvX64RegisterDs,
        WHvX64RegisterEs,
        WHvX64RegisterSs,
    };
    WHV_REGISTER_VALUE values[sizeof(names) / sizeof(names[0])] = {};
    values[0].Reg64 = kGuestCodeAddr;
    values[1].Reg64 = 0x2;
    values[2].Segment = Segment(0, 0x9b);
    values[3].Segment = Segment(0, 0x93);
    values[4].Segment = Segment(0, 0x93);
    values[5].Segment = Segment(0, 0x93);
    CheckHr(
        api.set_vp_registers(
            partition.handle,
            0,
            names,
            static_cast<UINT32>(sizeof(names) / sizeof(names[0])),
            values),
        "WHvSetVirtualProcessorRegisters");

    WHV_RUN_VP_EXIT_CONTEXT exit_context{};
    uint32_t runs = 0;
    HRESULT hr = api.run_vp(partition.handle, 0, &exit_context, sizeof(exit_context));
    CheckHr(hr, "WHvRunVirtualProcessor");
    runs++;

    uint64_t dirty_bitmap = 0;
    CheckHr(
        api.query_dirty_bitmap(partition.handle, 0, kGuestRamBytes, &dirty_bitmap, sizeof(dirty_bitmap)),
        "WHvQueryGpaRangeDirtyBitmap");
    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    napi_value out = MakeObject(env);
    SetString(env, out, "backend", "whp");
    SetString(env, out, "exitReason", WhpExitReason(exit_context.ExitReason));
    SetUint32(env, out, "exitReasonCode", exit_context.ExitReason);
    SetUint32(env, out, "runs", runs);
    SetBool(env, out, "dirtyTracking", true);
    SetDouble(env, out, "dirtyPages", CountBits(dirty_bitmap));
    SetDouble(env, out, "totalMs", static_cast<double>(elapsed_us) / 1000.0);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value Unsupported(napi_env env, const char* method) {
  napi_throw_error(env, nullptr, (std::string(method) + " is only available in the Linux KVM backend").c_str());
  return nullptr;
}

napi_value ProbeKvm(napi_env env, napi_callback_info) { return Unsupported(env, "probeKvm"); }
napi_value SmokeHlt(napi_env env, napi_callback_info) { return Unsupported(env, "smokeHlt"); }
napi_value UartSmoke(napi_env env, napi_callback_info) { return Unsupported(env, "uartSmoke"); }
napi_value GuestExitSmoke(napi_env env, napi_callback_info) { return Unsupported(env, "guestExitSmoke"); }
napi_value RamSnapshotSmoke(napi_env env, napi_callback_info) { return Unsupported(env, "ramSnapshotSmoke"); }
napi_value DirtyRamSnapshotSmoke(napi_env env, napi_callback_info) { return Unsupported(env, "dirtyRamSnapshotSmoke"); }
napi_value RunVm(napi_env env, napi_callback_info) { return Unsupported(env, "runVm"); }

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"probeKvm", nullptr, ProbeKvm, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"probeWhp", nullptr, ProbeWhp, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"smokeHlt", nullptr, SmokeHlt, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpSmokeHlt", nullptr, WhpSmokeHlt, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"uartSmoke", nullptr, UartSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"guestExitSmoke", nullptr, GuestExitSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ramSnapshotSmoke", nullptr, RamSnapshotSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dirtyRamSnapshotSmoke", nullptr, DirtyRamSnapshotSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"runVm", nullptr, RunVm, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

}  // namespace
