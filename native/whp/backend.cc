#include <node_api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <timeapi.h>

#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api.h"
#include "boot_params.h"
#include "console_writer.h"
#include "devices/acpi_pm_timer.h"
#include "devices/cmos.h"
#include "devices/hpet.h"
#include "devices/pic.h"
#include "devices/pit.h"
#include "devices/uart.h"
#include "elf_loader.h"
#include "guest_memory.h"
#include "irq.h"
#include "page_tables.h"
#include "virtio/blk.h"
#include "virtio/rng.h"
#include "win_console_ctrl.h"
#include "win_io.h"

namespace {

// Pull extracted module symbols into this anonymous namespace so existing
// call sites stay unqualified. All the new modules live in node_vmm::whp.
using node_vmm::whp::AcpiPmTimer;
using node_vmm::whp::ArmInterruptWindow;
using node_vmm::whp::BuildPageTables;
using node_vmm::whp::Cmos;
using node_vmm::whp::DisarmInterruptWindow;
using node_vmm::whp::Hpet;
using node_vmm::whp::InterruptibilitySnapshot;
using node_vmm::whp::KernelInfo;
using node_vmm::whp::KickVcpuOutOfHlt;
using node_vmm::whp::LoadElfKernel;
using node_vmm::whp::LongCodeSegment;
using node_vmm::whp::LongDataSegment;
using node_vmm::whp::Pic;
using node_vmm::whp::Pit;
using node_vmm::whp::Uart;
using node_vmm::whp::ReadInterruptibility;
using node_vmm::whp::RequestFixedInterrupt;
using node_vmm::whp::Segment;
using node_vmm::whp::SetPendingExtInt;
using node_vmm::whp::Table;
using node_vmm::whp::TryDeliverPendingExtInt;
using node_vmm::whp::UpdateVcpuFromExit;
using node_vmm::whp::VirtioBlk;
using node_vmm::whp::VirtioRng;
using node_vmm::whp::WhpApi;
using node_vmm::whp::WhpVcpuIrqState;
using node_vmm::whp::WriteBootParams;
// WriteMpTable is wrapped below to forward kPitIoApicPin from this TU.
}  // namespace

namespace {

constexpr uint64_t kGuestCodeAddr = 0x1000;
constexpr uint64_t kGuestWriteAddr = 0x2000;
constexpr uint64_t kGuestRamBytes = 0x10000;
constexpr uint64_t kBootParamsAddr = 0x7000;
constexpr uint64_t kPageTableBase = 0x9000;
constexpr uint64_t kCmdlineAddr = 0x20000;
// Layout matches qemu/hw/i386/microvm.c VIRTIO_MMIO_BASE plus a 512 byte
// stride per device. The virtio-mmio v2 spec only mandates the registers up
// through offset 0x100 plus a 256 byte config area, so 0x200 is enough room
// for one device and lets the kernel pack multiple virtio-mmio devices in a
// single 4 KiB page like QEMU does.
constexpr uint64_t kVirtioMmioBase = 0xD0000000;
constexpr uint64_t kVirtioStride = 0x200;
constexpr uint64_t kIoApicBase = 0xFEC00000;
constexpr uint64_t kIoApicSize = 0x1000;
constexpr uint64_t kHpetBase = 0xFED00000;
constexpr uint64_t kHpetSize = 0x400;
constexpr uint32_t kVirtioMmioIrqBase = 5;
constexpr uint32_t kMaxIoApicPins = 24;
constexpr uint32_t kPitIoApicPin = 2;
constexpr uint32_t kHpetTimer0IoApicPin = kPitIoApicPin;
constexpr uint16_t kCom1Base = 0x3F8;
constexpr uint16_t kAcpiPmTimerPort = 0x408;
constexpr uint32_t kCom1IRQ = 4;
constexpr uint16_t kNodeVmmExitPort = 0x501;
constexpr uint16_t kNodeVmmConsolePort = 0x600;
constexpr uint32_t kHvCpuidVendorAndMax = 0x40000000;
constexpr uint32_t kHvCpuidInterface = 0x40000001;
constexpr uint32_t kHvCpuidVersion = 0x40000002;
constexpr uint32_t kHvCpuidFeatures = 0x40000003;
constexpr uint32_t kHvCpuidEnlightenmentInfo = 0x40000004;
constexpr uint32_t kHvCpuidImplementationLimits = 0x40000005;
constexpr uint32_t kHvMsrGuestOsId = 0x40000000;
constexpr uint32_t kHvMsrHypercall = 0x40000001;
constexpr uint32_t kHvMsrVpIndex = 0x40000002;
constexpr uint32_t kHvMsrTimeRefCount = 0x40000020;
constexpr uint32_t kHvMsrTscFrequency = 0x40000022;
constexpr uint32_t kHvMsrApicFrequency = 0x40000023;
constexpr uint64_t kFallbackTscFrequencyHz = 2500000000ULL;
constexpr uint64_t kFallbackApicFrequencyHz = 200000000ULL;
constexpr uint32_t kVirtioMagic = 0x74726976;  // "virt"
constexpr uint32_t kVirtioStatusAck = 0x01;
constexpr uint32_t kVirtioStatusDriver = 0x02;
constexpr uint32_t kVirtioStatusDriverOk = 0x04;
constexpr uint32_t kVirtioStatusFeaturesOk = 0x08;
constexpr uint32_t kVirtioStatusFailed = 0x80;
constexpr uint32_t kVirtioRingDescFNext = 1;
constexpr uint32_t kVirtioRingDescFWrite = 2;
constexpr uint32_t kVirtioRingDescFIndirect = 4;
constexpr uint16_t kVirtioRingAvailFNoInterrupt = 1;
constexpr uint64_t kVirtioFVersion1 = 1ULL << 32;
constexpr uint64_t kVirtioNetFMac = 1ULL << 5;
constexpr uint64_t kVirtioNetFStatus = 1ULL << 16;
constexpr uint16_t kVirtioMmioMagicValue = 0x000;
constexpr uint16_t kVirtioMmioVersion = 0x004;
constexpr uint16_t kVirtioMmioDeviceId = 0x008;
constexpr uint16_t kVirtioMmioVendorId = 0x00C;
constexpr uint16_t kVirtioMmioDeviceFeatures = 0x010;
constexpr uint16_t kVirtioMmioDeviceFeaturesSel = 0x014;
constexpr uint16_t kVirtioMmioDriverFeatures = 0x020;
constexpr uint16_t kVirtioMmioDriverFeaturesSel = 0x024;
constexpr uint16_t kVirtioMmioQueueSel = 0x030;
constexpr uint16_t kVirtioMmioQueueNumMax = 0x034;
constexpr uint16_t kVirtioMmioQueueNum = 0x038;
constexpr uint16_t kVirtioMmioQueueReady = 0x044;
constexpr uint16_t kVirtioMmioQueueNotify = 0x050;
constexpr uint16_t kVirtioMmioInterruptStatus = 0x060;
constexpr uint16_t kVirtioMmioInterruptAck = 0x064;
constexpr uint16_t kVirtioMmioStatus = 0x070;
constexpr uint16_t kVirtioMmioQueueDescLow = 0x080;
constexpr uint16_t kVirtioMmioQueueDescHigh = 0x084;
constexpr uint16_t kVirtioMmioQueueDriverLow = 0x090;
constexpr uint16_t kVirtioMmioQueueDriverHigh = 0x094;
constexpr uint16_t kVirtioMmioQueueDeviceLow = 0x0A0;
constexpr uint16_t kVirtioMmioQueueDeviceHigh = 0x0A4;
constexpr uint16_t kVirtioMmioShmSel = 0x0AC;
constexpr uint16_t kVirtioMmioShmLenLow = 0x0B0;
constexpr uint16_t kVirtioMmioShmLenHigh = 0x0B4;
constexpr uint16_t kVirtioMmioShmBaseLow = 0x0B8;
constexpr uint16_t kVirtioMmioShmBaseHigh = 0x0BC;
constexpr uint16_t kVirtioMmioConfigGeneration = 0x0FC;
constexpr uint16_t kVirtioMmioConfig = 0x100;
constexpr uint32_t kVirtioInterruptVring = 0x1;
constexpr uint32_t kVirtioInterruptConfig = 0x2;
constexpr uint64_t kGdtAddr = 0x500;
constexpr uint64_t kIdtAddr = 0x520;
constexpr uint32_t kMaxQueueSize = 256;
constexpr uint32_t kKernelCmdlineMax = 2048;
constexpr uint32_t kMaxVcpus = 64;
constexpr int32_t kControlRun = 0;
constexpr int32_t kControlPause = 1;
constexpr int32_t kControlStop = 2;
constexpr int32_t kControlStateStarting = 0;
constexpr int32_t kControlStateRunning = 1;
constexpr int32_t kControlStatePaused = 2;
constexpr int32_t kControlStateStopping = 3;
constexpr int32_t kControlStateExited = 4;

constexpr uint64_t VirtioMmioBase(uint32_t index) {
  return kVirtioMmioBase + uint64_t(index) * kVirtioStride;
}

constexpr uint32_t VirtioMmioIrq(uint32_t index) {
  return kVirtioMmioIrqBase + index;
}

// ELF constants (kElfMagic, kElfClass64, kElfDataLe, kElfMachineX64,
// kElfPtLoad) plus the Elf64Ehdr/Elf64Phdr structs and the LoadElfKernel
// implementation moved to native/whp_elf_loader.{h,cc} as part of the
// PR-2 modularization. The KernelInfo struct + LoadElfKernel function are
// re-exported into this anonymous namespace via `using` declarations near
// the top of the file.

// Elf64Ehdr / Elf64Phdr / KernelInfo moved to whp_elf_loader.{h,cc}.
// `KernelInfo` is re-imported into this anonymous namespace at the top.

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

void CheckVirtioMmioDeviceCount(size_t count) {
  Check(count > 0, "at least one virtio-mmio device is required");
  Check(kVirtioMmioIrqBase + count - 1 < kMaxIoApicPins, "too many virtio-mmio devices");
}

std::string VirtioMmioDeviceName(uint32_t index) {
  Check(index <= 999, "too many virtio-mmio devices");
  char name[5];
  std::snprintf(name, sizeof(name), "V%03u", index);
  return std::string(name, 4);
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

std::string GetString(napi_env env, napi_value obj, const char* name) {
  napi_value v;
  bool has = false;
  napi_has_named_property(env, obj, name, &has);
  if (!has) {
    return "";
  }
  napi_get_named_property(env, obj, name, &v);
  napi_valuetype type;
  napi_typeof(env, v, &type);
  if (type == napi_undefined || type == napi_null) {
    return "";
  }
  if (type != napi_string) {
    throw std::runtime_error(std::string(name) + " must be a string");
  }
  size_t len = 0;
  napi_get_value_string_utf8(env, v, nullptr, 0, &len);
  std::vector<char> buffer(len + 1);
  napi_get_value_string_utf8(env, v, buffer.data(), buffer.size(), &len);
  std::string out(buffer.data(), len);
  return out;
}

uint32_t GetUint32(napi_env env, napi_value obj, const char* name, uint32_t fallback) {
  napi_value v;
  bool has = false;
  napi_has_named_property(env, obj, name, &has);
  if (!has) {
    return fallback;
  }
  napi_get_named_property(env, obj, name, &v);
  napi_valuetype type;
  napi_typeof(env, v, &type);
  if (type == napi_undefined || type == napi_null) {
    return fallback;
  }
  uint32_t out = 0;
  napi_get_value_uint32(env, v, &out);
  return out;
}

bool GetBool(napi_env env, napi_value obj, const char* name, bool fallback) {
  napi_value v;
  bool has = false;
  napi_has_named_property(env, obj, name, &has);
  if (!has) {
    return fallback;
  }
  napi_get_named_property(env, obj, name, &v);
  napi_valuetype type;
  napi_typeof(env, v, &type);
  if (type == napi_undefined || type == napi_null) {
    return fallback;
  }
  bool out = fallback;
  napi_get_value_bool(env, v, &out);
  return out;
}

struct AttachedDiskConfig {
  std::string path;
  bool read_only{false};
};

bool HasNonNullishNamed(napi_env env, napi_value obj, const char* name) {
  bool has = false;
  napi_has_named_property(env, obj, name, &has);
  if (!has) {
    return false;
  }
  napi_value value;
  napi_get_named_property(env, obj, name, &value);
  napi_valuetype type;
  napi_typeof(env, value, &type);
  return type != napi_undefined && type != napi_null;
}

std::vector<AttachedDiskConfig> GetAttachedDisks(napi_env env, napi_value obj) {
  bool has_disks = HasNonNullishNamed(env, obj, "disks");
  bool has_attached_disks = HasNonNullishNamed(env, obj, "attachedDisks");
  if (!has_disks && !has_attached_disks) {
    return {};
  }
  Check(!(has_disks && has_attached_disks), "use either disks or attachedDisks, not both");

  const char* name = has_disks ? "disks" : "attachedDisks";
  napi_value value;
  napi_get_named_property(env, obj, name, &value);
  bool is_array = false;
  napi_is_array(env, value, &is_array);
  Check(is_array, std::string(name) + " must be an array");

  uint32_t length = 0;
  napi_get_array_length(env, value, &length);
  std::vector<AttachedDiskConfig> out;
  out.reserve(length);
  for (uint32_t i = 0; i < length; i++) {
    napi_value entry;
    napi_get_element(env, value, i, &entry);
    napi_valuetype type = napi_undefined;
    napi_typeof(env, entry, &type);
    Check(type == napi_object, std::string(name) + " entries must be objects");
    AttachedDiskConfig disk;
    disk.path = GetString(env, entry, "path");
    Check(!disk.path.empty(), std::string(name) + " entries require path");
    disk.read_only = GetBool(env, entry, "readOnly", GetBool(env, entry, "readonly", false));
    out.push_back(std::move(disk));
  }
  return out;
}

napi_value Throw(napi_env env, const std::exception& err) {
  napi_throw_error(env, nullptr, err.what());
  return nullptr;
}

// WhpApi + LoadSymbol + WHv*Fn typedefs moved to whp/api.h.

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

void CheckRange(uint64_t total, uint64_t offset, uint64_t len, const std::string& what);

struct RunControl {
  int32_t* words{nullptr};
  size_t length{0};

  bool enabled() const { return words != nullptr && length >= 2; }

  int32_t command() const {
    if (!enabled()) {
      return kControlRun;
    }
    volatile LONG* ptr = reinterpret_cast<volatile LONG*>(&words[0]);
    return InterlockedCompareExchange(ptr, 0, 0);
  }

  void set_state(int32_t state) const {
    if (enabled()) {
      volatile LONG* ptr = reinterpret_cast<volatile LONG*>(&words[1]);
      InterlockedExchange(ptr, state);
    }
  }
};

RunControl GetRunControl(napi_env env, napi_value obj) {
  napi_value value;
  bool has = false;
  napi_has_named_property(env, obj, "control", &has);
  if (!has) {
    return {};
  }
  napi_get_named_property(env, obj, "control", &value);
  napi_valuetype value_type;
  napi_typeof(env, value, &value_type);
  if (value_type == napi_undefined || value_type == napi_null) {
    return {};
  }
  bool is_typedarray = false;
  napi_is_typedarray(env, value, &is_typedarray);
  Check(is_typedarray, "control must be an Int32Array");

  napi_typedarray_type type;
  size_t length = 0;
  void* data = nullptr;
  napi_value arraybuffer;
  size_t byte_offset = 0;
  Check(napi_get_typedarray_info(env, value, &type, &length, &data, &arraybuffer, &byte_offset) == napi_ok,
        "napi_get_typedarray_info control failed");
  Check(type == napi_int32_array, "control must be an Int32Array");
  Check(length >= 2, "control Int32Array must have at least 2 entries");
  return {reinterpret_cast<int32_t*>(data), length};
}

// GuestMemory moved to whp/guest_memory.h.
using node_vmm::whp::GuestMemory;

// WinHandle / PreadAll / PwriteAll / TruncateFileTo / MarkFileSparse
// moved to whp/win_io.h.
using node_vmm::whp::MarkFileSparse;
using node_vmm::whp::PreadAll;
using node_vmm::whp::PwriteAll;
using node_vmm::whp::TruncateFileTo;
using node_vmm::whp::WinHandle;

// Segment(selector, attributes) helper moved to whp_page_tables.{h,cc}.
// Re-imported via the `using` declaration at the top of the file.

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
    case WHvRunVpExitReasonX64ApicEoi:
      return "apic-eoi";
    case WHvRunVpExitReasonX64MsrAccess:
      return "msr-access";
    case WHvRunVpExitReasonX64Cpuid:
      return "cpuid";
    case WHvRunVpExitReasonCanceled:
      return "canceled";
    default:
      return "whp-exit-" + std::to_string(reason);
  }
}

bool WhpTraceEnabled() {
  char value[8]{};
  DWORD len = GetEnvironmentVariableA("NODE_VMM_WHP_TRACE", value, static_cast<DWORD>(sizeof(value)));
  return len > 0 && value[0] != '0';
}

std::string Hex(uint64_t value) {
  char buf[32]{};
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
  return buf;
}

std::string DescribeWhpExit(const WHV_RUN_VP_EXIT_CONTEXT& ctx) {
  std::string out = "exit=" + WhpExitReason(ctx.ExitReason) + " rip=" + Hex(ctx.VpContext.Rip);
  switch (ctx.ExitReason) {
    case WHvRunVpExitReasonX64IoPortAccess:
      out += " port=" + Hex(ctx.IoPortAccess.PortNumber);
      out += " size=" + std::to_string(ctx.IoPortAccess.AccessInfo.AccessSize);
      out += " write=" + std::to_string(ctx.IoPortAccess.AccessInfo.IsWrite);
      out += " len=" + std::to_string(ctx.IoPortAccess.InstructionByteCount);
      out += " rax=" + Hex(ctx.IoPortAccess.Rax);
      break;
    case WHvRunVpExitReasonMemoryAccess:
      out += " gpa=" + Hex(ctx.MemoryAccess.Gpa);
      out += " gva=" + Hex(ctx.MemoryAccess.Gva);
      out += " access=" + std::to_string(ctx.MemoryAccess.AccessInfo.AccessType);
      out += " len=" + std::to_string(ctx.MemoryAccess.InstructionByteCount);
      break;
    case WHvRunVpExitReasonUnrecoverableException:
      out += " exception=" + std::to_string(ctx.VpException.ExceptionType);
      out += " error=" + Hex(ctx.VpException.ErrorCode);
      out += " param=" + Hex(ctx.VpException.ExceptionParameter);
      break;
    case WHvRunVpExitReasonX64ApicEoi:
      out += " vector=" + Hex(ctx.ApicEoi.InterruptVector);
      break;
    case WHvRunVpExitReasonX64MsrAccess:
      out += " msr=" + Hex(ctx.MsrAccess.MsrNumber);
      out += " write=" + std::to_string(ctx.MsrAccess.AccessInfo.IsWrite);
      break;
    case WHvRunVpExitReasonX64Cpuid:
      out += " leaf=" + Hex(ctx.CpuidAccess.Rax & 0xFFFFFFFFULL);
      out += " subleaf=" + Hex(ctx.CpuidAccess.Rcx & 0xFFFFFFFFULL);
      break;
    default:
      break;
  }
  return out;
}

uint32_t CountBits(uint64_t value) {
  uint32_t count = 0;
  while (value) {
    value &= value - 1;
    count++;
  }
  return count;
}

uint64_t QueryWhpFrequency(
    WhpApi& api,
    WHV_CAPABILITY_CODE code,
    uint64_t fallback,
    uint64_t WHV_CAPABILITY::*field) {
  WHV_CAPABILITY capability{};
  UINT32 written = 0;
  HRESULT hr = api.get_capability(code, &capability, sizeof(capability), &written);
  if (FAILED(hr) || capability.*field == 0) {
    return fallback;
  }
  return capability.*field;
}

uint64_t CheckedAdd(uint64_t a, uint64_t b, const std::string& what) {
  Check(a <= UINT64_MAX - b, what + " overflows");
  return a + b;
}

uint64_t CheckedMul(uint64_t a, uint64_t b, const std::string& what) {
  if (a == 0 || b == 0) {
    return 0;
  }
  Check(a <= UINT64_MAX / b, what + " overflows");
  return a * b;
}

void CheckRange(uint64_t total, uint64_t offset, uint64_t len, const std::string& what) {
  Check(offset <= total && len <= total - offset, what + " out of bounds");
}

uint16_t ReadU16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
  return uint64_t(ReadU32(p)) | (uint64_t(ReadU32(p + 4)) << 32);
}

void WriteU16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

void WriteU32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

void WriteU64(uint8_t* p, uint64_t v) {
  WriteU32(p, uint32_t(v));
  WriteU32(p + 4, uint32_t(v >> 32));
}

// ReadLe / WriteLe / DepositBits moved to common/bytes.h.

// ReadWholeFile moved to whp_elf_loader.cc (TU-private helper for
// LoadElfKernel). FileSizeBytes stays here because it's also called from
// RunVm to size the rootfs image before partition mapping.
uint64_t FileSizeBytes(const std::string& path) {
  WinHandle file(CreateFileA(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  Check(file.valid(), "open " + path + " failed: " + WindowsErrorMessage(GetLastError()));
  LARGE_INTEGER size{};
  Check(GetFileSizeEx(file.get(), &size) != 0, "stat " + path + " failed: " + WindowsErrorMessage(GetLastError()));
  Check(size.QuadPart >= 0, "negative file size: " + path);
  return static_cast<uint64_t>(size.QuadPart);
}

// LoadElfKernel moved to whp_elf_loader.cc.

// PutE820 / WriteBootParams / Checksum / WriteMpTable moved to
// whp_boot_params.{h,cc}. WriteBootParams is re-imported via `using`
// above; WriteMpTable is wrapped below to forward kPitIoApicPin (which
// stays in this TU because ACPI MADT and the timer-irq delivery path
// also reference it).

void WriteMpTable(uint8_t* mem, uint64_t mem_size, int cpus) {
  node_vmm::whp::WriteMpTable(mem, mem_size, cpus, kPitIoApicPin);
}


uint64_t AlignUp(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

void AppendPkgLength(std::vector<uint8_t>& out, size_t payload_len, bool include_self) {
  size_t length_len = 1;
  if (payload_len >= (1U << 20) - 3) {
    length_len = 4;
  } else if (payload_len >= (1U << 12) - 2) {
    length_len = 3;
  } else if (payload_len >= (1U << 6) - 1) {
    length_len = 2;
  }
  size_t length = payload_len + (include_self ? length_len : 0);
  switch (length_len) {
    case 1:
      out.push_back(static_cast<uint8_t>(length));
      break;
    case 2:
      out.push_back(static_cast<uint8_t>((1U << 6) | (length & 0x0F)));
      out.push_back(static_cast<uint8_t>((length >> 4) & 0xFF));
      break;
    case 3:
      out.push_back(static_cast<uint8_t>((2U << 6) | (length & 0x0F)));
      out.push_back(static_cast<uint8_t>((length >> 4) & 0xFF));
      out.push_back(static_cast<uint8_t>((length >> 12) & 0xFF));
      break;
    default:
      out.push_back(static_cast<uint8_t>((3U << 6) | (length & 0x0F)));
      out.push_back(static_cast<uint8_t>((length >> 4) & 0xFF));
      out.push_back(static_cast<uint8_t>((length >> 12) & 0xFF));
      out.push_back(static_cast<uint8_t>((length >> 20) & 0xFF));
      break;
  }
}

std::vector<uint8_t> EncodePath(std::string name) {
  std::vector<uint8_t> out;
  bool root = !name.empty() && name[0] == '\\';
  if (root) {
    out.push_back('\\');
    name.erase(0, 1);
  }
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t dot = name.find('.', start);
    std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    Check(part.size() == 4, "AML path part must be four characters: " + part);
    parts.push_back(part);
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  if (parts.size() == 2) {
    out.push_back(0x2E);
  } else if (parts.size() > 2) {
    out.push_back(0x2F);
    out.push_back(static_cast<uint8_t>(parts.size()));
  }
  for (const auto& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

std::vector<uint8_t> EncodeString(const std::string& value) {
  std::vector<uint8_t> out;
  out.push_back(0x0D);
  out.insert(out.end(), value.begin(), value.end());
  out.push_back(0);
  return out;
}

std::vector<uint8_t> EncodeByte(uint8_t value) {
  return std::vector<uint8_t>{0x0A, value};
}

std::vector<uint8_t> EncodeWord(uint16_t value) {
  std::vector<uint8_t> out{0x0B, 0, 0};
  WriteU16(out.data() + 1, value);
  return out;
}

std::vector<uint8_t> EncodeDword(uint32_t value) {
  std::vector<uint8_t> out{0x0C, 0, 0, 0, 0};
  WriteU32(out.data() + 1, value);
  return out;
}

std::vector<uint8_t> EncodeInteger(uint64_t value) {
  if (value == 0) return std::vector<uint8_t>{0x00};
  if (value == 1) return std::vector<uint8_t>{0x01};
  if (value <= 0xFF) return EncodeByte(static_cast<uint8_t>(value));
  if (value <= 0xFFFF) return EncodeWord(static_cast<uint16_t>(value));
  if (value <= 0xFFFFFFFF) return EncodeDword(static_cast<uint32_t>(value));
  std::vector<uint8_t> out{0x0E, 0, 0, 0, 0, 0, 0, 0, 0};
  WriteU64(out.data() + 1, value);
  return out;
}

std::vector<uint8_t> AppendName(const std::string& path, const std::vector<uint8_t>& value) {
  std::vector<uint8_t> out{0x08};
  auto path_bytes = EncodePath(path);
  out.insert(out.end(), path_bytes.begin(), path_bytes.end());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

std::vector<uint8_t> ResMemory32Fixed(bool read_write, uint32_t base, uint32_t length) {
  std::vector<uint8_t> out(12);
  out[0] = 0x86;
  WriteU16(out.data() + 1, 9);
  out[3] = read_write ? 1 : 0;
  WriteU32(out.data() + 4, base);
  WriteU32(out.data() + 8, length);
  return out;
}

std::vector<uint8_t> ResInterrupt(bool consumer, bool edge_triggered, bool active_low, bool shared, uint32_t number) {
  std::vector<uint8_t> out(9);
  out[0] = 0x89;
  WriteU16(out.data() + 1, 6);
  uint8_t flags = 0;
  if (shared) flags |= 1 << 3;
  if (active_low) flags |= 1 << 2;
  if (edge_triggered) flags |= 1 << 1;
  if (consumer) flags |= 1;
  out[3] = flags;
  out[4] = 1;
  WriteU32(out.data() + 5, number);
  return out;
}

std::vector<uint8_t> ResIoPort(uint16_t base, uint8_t length) {
  std::vector<uint8_t> out(8);
  out[0] = 0x47;  // I/O port descriptor.
  out[1] = 0x01;  // Decode 16-bit addresses.
  WriteU16(out.data() + 2, base);
  WriteU16(out.data() + 4, base);
  out[6] = 0x01;  // Alignment.
  out[7] = length;
  return out;
}

std::vector<uint8_t> BuildResourceTemplate(const std::vector<std::vector<uint8_t>>& resources) {
  std::vector<uint8_t> payload;
  for (const auto& resource : resources) {
    payload.insert(payload.end(), resource.begin(), resource.end());
  }
  payload.push_back(0x79);
  payload.push_back(0x00);
  auto len_obj = EncodeInteger(payload.size());
  std::vector<uint8_t> tmp = len_obj;
  tmp.insert(tmp.end(), payload.begin(), payload.end());
  std::vector<uint8_t> out{0x11};
  AppendPkgLength(out, tmp.size(), true);
  out.insert(out.end(), tmp.begin(), tmp.end());
  return out;
}

std::vector<uint8_t> BuildResourceTemplate(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second) {
  return BuildResourceTemplate(std::vector<std::vector<uint8_t>>{first, second});
}

std::vector<uint8_t> AppendDevice(const std::string& name, const std::vector<std::vector<uint8_t>>& children) {
  std::vector<uint8_t> payload = EncodePath(name);
  for (const auto& child : children) {
    payload.insert(payload.end(), child.begin(), child.end());
  }
  std::vector<uint8_t> out{0x5B, 0x82};
  AppendPkgLength(out, payload.size(), true);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> AppendScope(const std::string& name, const std::vector<std::vector<uint8_t>>& children) {
  std::vector<uint8_t> payload = EncodePath(name);
  for (const auto& child : children) {
    payload.insert(payload.end(), child.begin(), child.end());
  }
  std::vector<uint8_t> out{0x10};
  AppendPkgLength(out, payload.size(), true);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> BuildVirtioMmioDevice(const std::string& name, uint32_t uid, uint64_t base, uint32_t irq) {
  std::vector<std::vector<uint8_t>> dev_children;
  dev_children.push_back(AppendName("_HID", EncodeString("LNRO0005")));
  dev_children.push_back(AppendName("_UID", EncodeInteger(uid)));
  dev_children.push_back(AppendName("_CCA", std::vector<uint8_t>{0x01}));
  dev_children.push_back(AppendName(
      "_CRS",
      BuildResourceTemplate(
          ResMemory32Fixed(true, static_cast<uint32_t>(base), static_cast<uint32_t>(kVirtioStride)),
          ResInterrupt(true, true, false, false, irq))));
  return AppendDevice(name, dev_children);
}

std::vector<uint8_t> BuildHpetDevice() {
  std::vector<std::vector<uint8_t>> dev_children;
  dev_children.push_back(AppendName("_HID", EncodeString("PNP0103")));
  dev_children.push_back(AppendName("_UID", EncodeInteger(0)));
  dev_children.push_back(AppendName(
      "_CRS",
      BuildResourceTemplate(std::vector<std::vector<uint8_t>>{
          ResMemory32Fixed(false, static_cast<uint32_t>(kHpetBase), static_cast<uint32_t>(kHpetSize)),
      })));
  return AppendDevice("HPET", dev_children);
}

std::vector<uint8_t> BuildCom1Device() {
  std::vector<std::vector<uint8_t>> dev_children;
  dev_children.push_back(AppendName("_HID", EncodeString("PNP0501")));
  dev_children.push_back(AppendName("_UID", EncodeInteger(0)));
  dev_children.push_back(AppendName(
      "_CRS",
      BuildResourceTemplate(std::vector<std::vector<uint8_t>>{
          ResIoPort(kCom1Base, 8),
          ResInterrupt(true, true, false, false, kCom1IRQ),
      })));
  return AppendDevice("COM1", dev_children);
}

std::vector<uint8_t> BuildDsdtBody(bool network_enabled, uint32_t disk_count) {
  uint32_t rng_index = disk_count + 1;
  CheckVirtioMmioDeviceCount(rng_index + 1);
  std::vector<std::vector<uint8_t>> children;
  children.push_back(BuildHpetDevice());
  children.push_back(BuildCom1Device());
  for (uint32_t index = 0; index < disk_count; index++) {
    children.push_back(BuildVirtioMmioDevice(
        VirtioMmioDeviceName(index), index, VirtioMmioBase(index), VirtioMmioIrq(index)));
  }
  children.push_back(BuildVirtioMmioDevice(
      VirtioMmioDeviceName(rng_index), rng_index, VirtioMmioBase(rng_index), VirtioMmioIrq(rng_index)));
  if (network_enabled) {
    uint32_t net_index = disk_count;
    children.push_back(BuildVirtioMmioDevice(
        VirtioMmioDeviceName(net_index), net_index, VirtioMmioBase(net_index), VirtioMmioIrq(net_index)));
  }
  return AppendScope("\\_SB_", children);
}

uint8_t TableChecksum(const std::vector<uint8_t>& table) {
  uint8_t sum = 0;
  for (uint8_t b : table) sum = static_cast<uint8_t>(sum + b);
  return static_cast<uint8_t>(~sum + 1);
}

std::vector<uint8_t> BuildSdtHeader(const char sig[4], uint32_t len, uint8_t rev, const char table_id[8]) {
  std::vector<uint8_t> out(36);
  std::memcpy(out.data(), sig, 4);
  WriteU32(out.data() + 4, len);
  out[8] = rev;
  std::memcpy(out.data() + 10, "GOCRKR", 6);
  std::memcpy(out.data() + 16, table_id, 8);
  WriteU32(out.data() + 28, 0x47434154);
  WriteU32(out.data() + 32, 0x20260402);
  return out;
}

void FinalizeSdt(std::vector<uint8_t>& table) {
  table[9] = 0;
  table[9] = TableChecksum(table);
}

std::vector<uint8_t> BuildDsdtTable(bool network_enabled, uint32_t disk_count) {
  auto body = BuildDsdtBody(network_enabled, disk_count);
  auto out = BuildSdtHeader("DSDT", static_cast<uint32_t>(36 + body.size()), 2, "GCDSDT01");
  out.insert(out.end(), body.begin(), body.end());
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildHpetTable() {
  auto out = BuildSdtHeader("HPET", 56, 1, "GCHPET01");
  out.resize(56);
  // Keep this event timer block ID in sync with Hpet::kCapabilities: Intel
  // vendor, legacy replacement capable, 3 timers, revision 1.
  WriteU32(out.data() + 36, 0x8086A201);
  out[40] = 0;   // Generic Address Structure: system memory.
  out[41] = 0;   // Register bit width, per QEMU's HPET table.
  out[42] = 0;   // Register bit offset.
  out[43] = 0;   // Access size.
  WriteU64(out.data() + 44, kHpetBase);
  out[52] = 0;   // HPET number.
  WriteU16(out.data() + 53, 0);  // Minimum tick in periodic mode.
  out[55] = 0;   // Page protection and OEM attribute.
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildFadt(uint64_t dsdt_addr) {
  std::vector<uint8_t> out(276);
  auto hdr = BuildSdtHeader("FACP", 276, 6, "GCFADT01");
  std::memcpy(out.data(), hdr.data(), hdr.size());
  WriteU32(out.data() + 40, static_cast<uint32_t>(dsdt_addr));
  WriteU32(out.data() + 76, kAcpiPmTimerPort);
  out[91] = 4;    // PM timer register length.
  out[108] = 0x32;  // Century register in CMOS/RTC.
  WriteU16(out.data() + 109, 1 << 2);
  WriteU32(out.data() + 112, (1 << 20) | (1 << 8) | (1 << 4) | (1 << 5));
  out[131] = 5;
  WriteU64(out.data() + 140, dsdt_addr);
  out[208] = 1;    // X_PM_TMR_BLK GAS: system I/O.
  out[209] = 32;   // 32-bit PM timer.
  out[210] = 0;
  out[211] = 3;    // DWord access.
  WriteU64(out.data() + 212, kAcpiPmTimerPort);
  std::memcpy(out.data() + 268, "GOCRKVM ", 8);
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildMadt(int cpus) {
  std::vector<uint8_t> body(8 + 12 + 10 + cpus * 8);
  WriteU32(body.data(), 0xFEE00000);
  WriteU32(body.data() + 4, 1);  // PCAT_COMPAT: dual 8259 PICs are present.
  uint8_t* ioapic = body.data() + 8;
  ioapic[0] = 1;
  ioapic[1] = 12;
  WriteU32(ioapic + 4, 0xFEC00000);
  uint8_t* irq0_override = body.data() + 20;
  irq0_override[0] = 2;   // Interrupt Source Override.
  irq0_override[1] = 10;
  irq0_override[2] = 0;   // ISA bus.
  irq0_override[3] = 0;   // ISA IRQ0 (PIT).
  WriteU32(irq0_override + 4, kPitIoApicPin);
  WriteU16(irq0_override + 8, 0);  // Bus-conforming polarity/trigger.
  size_t off = 30;
  for (int i = 0; i < cpus; i++) {
    uint8_t* lapic = body.data() + off;
    lapic[0] = 0;
    lapic[1] = 8;
    lapic[2] = static_cast<uint8_t>(i);
    lapic[3] = static_cast<uint8_t>(i);
    WriteU32(lapic + 4, 1);
    off += 8;
  }
  auto out = BuildSdtHeader("APIC", static_cast<uint32_t>(36 + body.size()), 6, "GCMADT01");
  out.insert(out.end(), body.begin(), body.end());
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildRsdt(const std::vector<uint64_t>& addrs) {
  std::vector<uint8_t> body(addrs.size() * 4);
  for (size_t i = 0; i < addrs.size(); i++) {
    WriteU32(body.data() + i * 4, static_cast<uint32_t>(addrs[i]));
  }
  auto out = BuildSdtHeader("RSDT", static_cast<uint32_t>(36 + body.size()), 1, "GCRSDT01");
  out.insert(out.end(), body.begin(), body.end());
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildXsdt(const std::vector<uint64_t>& addrs) {
  std::vector<uint8_t> body(addrs.size() * 8);
  for (size_t i = 0; i < addrs.size(); i++) {
    WriteU64(body.data() + i * 8, addrs[i]);
  }
  auto out = BuildSdtHeader("XSDT", static_cast<uint32_t>(36 + body.size()), 1, "GCXSDT01");
  out.insert(out.end(), body.begin(), body.end());
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildRsdp(uint64_t rsdt_addr, uint64_t xsdt_addr) {
  std::vector<uint8_t> out(36);
  std::memcpy(out.data(), "RSD PTR ", 8);
  std::memcpy(out.data() + 9, "GOCRKR", 6);
  out[15] = 2;
  WriteU32(out.data() + 16, static_cast<uint32_t>(rsdt_addr));
  WriteU32(out.data() + 20, 36);
  WriteU64(out.data() + 24, xsdt_addr);
  uint8_t sum20 = 0;
  for (size_t i = 0; i < 20; i++) sum20 = static_cast<uint8_t>(sum20 + out[i]);
  out[8] = static_cast<uint8_t>(~sum20 + 1);
  uint8_t sum36 = 0;
  for (uint8_t b : out) sum36 = static_cast<uint8_t>(sum36 + b);
  out[32] = static_cast<uint8_t>(~sum36 + 1);
  return out;
}

uint64_t CreateAcpiTables(uint8_t* mem, uint64_t mem_size, bool network_enabled, int cpus, uint32_t disk_count) {
  constexpr uint64_t rsdp_addr = 0x000E0000;
  uint64_t cursor = 0x000A0000;
  auto write_table = [&](const std::vector<uint8_t>& table) {
    cursor = AlignUp(cursor, 8);
    uint64_t addr = cursor;
    CheckRange(mem_size, addr, table.size(), "ACPI table");
    std::memcpy(mem + addr, table.data(), table.size());
    cursor += table.size();
    Check(cursor < rsdp_addr, "ACPI tables overflow low memory");
    return addr;
  };
  auto dsdt = BuildDsdtTable(network_enabled, disk_count);
  uint64_t dsdt_addr = write_table(dsdt);
  uint64_t fadt_addr = write_table(BuildFadt(dsdt_addr));
  uint64_t madt_addr = write_table(BuildMadt(cpus));
  uint64_t hpet_addr = write_table(BuildHpetTable());
  uint64_t rsdt_addr = write_table(BuildRsdt({fadt_addr, madt_addr, hpet_addr}));
  uint64_t xsdt_addr = write_table(BuildXsdt({fadt_addr, madt_addr, hpet_addr}));
  auto rsdp = BuildRsdp(rsdt_addr, xsdt_addr);
  CheckRange(mem_size, rsdp_addr, rsdp.size(), "RSDP");
  std::memcpy(mem + rsdp_addr, rsdp.data(), rsdp.size());
  return rsdp_addr;
}

// BuildPageTables / Table / LongCodeSegment / LongDataSegment moved to
// whp_page_tables.{h,cc}. Re-imported via `using` declarations at the
// top of this file so call sites in SetupLongMode below remain unchanged.

void SetupLongMode(WhpApi& api, WHV_PARTITION_HANDLE partition, UINT32 vp_index, uint8_t* mem, uint64_t entry) {
  BuildPageTables(mem, kPageTableBase);
  uint64_t gdt[] = {
      0x0000000000000000ULL,
      0x00AF9B000000FFFFULL,
      0x00CF93000000FFFFULL,
      0x008F8B000000FFFFULL,
  };
  for (size_t i = 0; i < 4; i++) {
    WriteU64(mem + kGdtAddr + i * 8, gdt[i]);
  }
  WriteU64(mem + kIdtAddr, 0);

  WHV_REGISTER_NAME names[] = {
      WHvX64RegisterRip, WHvX64RegisterRsi, WHvX64RegisterRdi, WHvX64RegisterRflags, WHvX64RegisterRsp, WHvX64RegisterRbp,
      WHvX64RegisterCr0, WHvX64RegisterCr3, WHvX64RegisterCr4, WHvX64RegisterEfer,
      WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterSs,
      WHvX64RegisterTr, WHvX64RegisterGdtr, WHvX64RegisterIdtr,
  };
  WHV_REGISTER_VALUE values[sizeof(names) / sizeof(names[0])] = {};
  values[0].Reg64 = entry;
  values[1].Reg64 = kBootParamsAddr;
  values[2].Reg64 = vp_index;
  values[3].Reg64 = 0x2;
  values[4].Reg64 = 0x8FF0;
  values[5].Reg64 = 0x8FF0;
  values[6].Reg64 = 0x80000001ULL;
  values[7].Reg64 = kPageTableBase;
  values[8].Reg64 = 0x20;
  values[9].Reg64 = 0x500;
  values[10].Segment = LongCodeSegment();
  values[11].Segment = LongDataSegment();
  values[12].Segment = LongDataSegment();
  values[13].Segment = LongDataSegment();
  values[14].Segment = LongDataSegment();
  values[15].Segment = LongDataSegment();
  values[16].Segment = Segment(0x18, 0x8b);
  values[16].Segment.Limit = 0xFFFFF;
  values[16].Segment.Present = 1;
  values[16].Segment.Granularity = 1;
  values[17].Table = Table(kGdtAddr, 31);
  values[18].Table = Table(kIdtAddr, 7);
  CheckHr(api.set_vp_registers(partition, vp_index, names, static_cast<UINT32>(sizeof(names) / sizeof(names[0])), values),
          "WHvSetVirtualProcessorRegisters(long mode)");
}

void SetupWhpBootstrapVcpu(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    uint8_t* mem,
    uint64_t entry,
    bool enable_apic,
    bool rootfs_smp_ap) {
  auto set_optional_reg = [&](WHV_REGISTER_NAME name, uint64_t value) {
    WHV_REGISTER_VALUE reg{};
    reg.Reg64 = value;
    (void)api.set_vp_registers(partition, vp_index, &name, 1, &reg);
  };

  // For Linux/rootfs SMP, only the BSP (vp_index == 0) is bootstrapped into
  // long mode at the kernel entry point. The APs are left in their reset
  // state (real mode, RIP=0); WHP's X2APIC emulation parks them and starts
  // them at the SIPI vector when the BSP issues INIT/SIPI through the LAPIC
  // ICR, mirroring qemu/target/i386/whpx/whpx-apic.c:241-247 ("WHPX does not
  // use wait_for_sipi") and qemu/hw/intc/apic.c:apic_sipi.
  if (!rootfs_smp_ap) {
    SetupLongMode(api, partition, vp_index, mem, entry);
    set_optional_reg(WHvX64RegisterPat, 0x0007040600070406ULL);
    set_optional_reg(WHvX64RegisterMsrMtrrDefType, (1U << 11) | 0x6);
  }
  if (enable_apic) {
    set_optional_reg(
        WHvX64RegisterApicBase,
        0xFEE00000ULL | (1ULL << 11) | (vp_index == 0 ? (1ULL << 8) : 0));
  }
}

void SetRip(WhpApi& api, WHV_PARTITION_HANDLE partition, UINT32 vp_index, uint64_t rip) {
  WHV_REGISTER_NAME name = WHvX64RegisterRip;
  WHV_REGISTER_VALUE value{};
  value.Reg64 = rip;
  CheckHr(api.set_vp_registers(partition, vp_index, &name, 1, &value), "WHvSetVirtualProcessorRegisters(RIP)");
}

void SetRaxRip(WhpApi& api, WHV_PARTITION_HANDLE partition, UINT32 vp_index, uint64_t rax, uint64_t rip) {
  WHV_REGISTER_NAME names[] = {WHvX64RegisterRax, WHvX64RegisterRip};
  WHV_REGISTER_VALUE values[2]{};
  values[0].Reg64 = rax;
  values[1].Reg64 = rip;
  CheckHr(api.set_vp_registers(partition, vp_index, names, 2, values), "WHvSetVirtualProcessorRegisters(RAX/RIP)");
}

uint64_t GetRegister64(WhpApi& api, WHV_PARTITION_HANDLE partition, UINT32 vp_index, WHV_REGISTER_NAME name) {
  WHV_REGISTER_VALUE value{};
  CheckHr(api.get_vp_registers(partition, vp_index, &name, 1, &value), "WHvGetVirtualProcessorRegisters");
  return value.Reg64;
}

// KickVcpuOutOfHlt + WhpVcpuIrqState + InterruptibilitySnapshot +
// ReadInterruptibility + SetPendingExtInt + ArmInterruptWindow +
// DisarmInterruptWindow + UpdateVcpuFromExit + TryDeliverPendingExtInt
// moved to whp/irq.{h,cc}.

struct GuestExit {
  bool requested{false};
  uint32_t status{0};
};

// =====================================================================
// SECTION: serial UART 8250/16550 emulation (qemu/hw/char/serial.c parity)
// =====================================================================
class IoApic;
// Uart class moved to whp/devices/uart.{h,cc}.

// =====================================================================
// SECTION: legacy interrupt controllers
//   IoApic, Pic 8259, Pit i8254, Cmos/RTC. The 8259/8254/CMOS layout follows
//   qemu/hw/intc/i8259.c, qemu/hw/timer/i8254.c, qemu/hw/rtc/mc146818rtc.c.
// =====================================================================
class IoApic {
  static constexpr uint64_t kRteVectorMask = 0xFF;
  static constexpr uint64_t kRteDestinationMode = 1ULL << 11;
  static constexpr uint64_t kRteRemoteIrr = 1ULL << 14;
  static constexpr uint64_t kRteTriggerMode = 1ULL << 15;
  static constexpr uint64_t kRteMask = 1ULL << 16;

 public:
  IoApic(WhpApi& api, WHV_PARTITION_HANDLE partition) : api_(api), partition_(partition) {
    for (auto& entry : redir_) {
      entry = kRteMask;
    }
  }

  void attach_waker(std::function<void()> waker) {
    std::lock_guard<std::mutex> lock(mu_);
    waker_ = std::move(waker);
  }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    std::memset(data, 0, len);
    if (len != 4) {
      return;
    }
    uint32_t off = static_cast<uint32_t>(addr - kIoApicBase);
    if (off == 0x00) {
      WriteU32(data, selected_);
      return;
    }
    if (off == 0x10) {
      WriteU32(data, read_reg(selected_));
    }
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    if (len != 4) {
      return;
    }
    uint32_t off = static_cast<uint32_t>(addr - kIoApicBase);
    uint32_t value = ReadU32(data);
    if (off == 0x00) {
      selected_ = value & 0xFF;
      return;
    }
    if (off == 0x10) {
      write_reg(selected_, value);
    }
  }

  void request_irq(uint32_t irq) {
    std::lock_guard<std::mutex> lock(mu_);
    request_irq_locked(irq);
  }

  void set_irq(uint32_t irq, bool level) {
    if (irq >= redir_.size()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    line_level_[irq] = level;
    if (level) {
      request_irq_locked(irq);
    }
  }

  uint32_t vector_for_irq(uint32_t irq) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (irq >= redir_.size()) {
      return 0x20 + irq;
    }
    return vector_for_irq_locked(irq);
  }

  bool irq_unmasked(uint32_t irq) const {
    std::lock_guard<std::mutex> lock(mu_);
    return irq < redir_.size() && (redir_[irq] & kRteMask) == 0;
  }

  void drain_pending() {
    std::lock_guard<std::mutex> lock(mu_);
    drain_pending_locked();
  }

  void eoi(uint32_t vector) {
    std::lock_guard<std::mutex> lock(mu_);
    if (boot_dbg_) {
      std::fprintf(stderr, "[node-vmm ioapic] eoi vector=0x%02x\n", vector & 0xFF);
    }
    for (uint32_t irq = 0; irq < redir_.size(); irq++) {
      uint64_t entry = redir_[irq];
      if ((entry & kRteTriggerMode) == 0 || (entry & kRteRemoteIrr) == 0) {
        continue;
      }
      if (vector_for_irq_locked(irq) != (vector & 0xFF)) {
        continue;
      }
      redir_[irq] &= ~kRteRemoteIrr;
      if (line_level_[irq] && (redir_[irq] & kRteMask) == 0) {
        request_irq_locked(irq);
      }
    }
    drain_pending_locked();
  }

  void enable_debug() {
    std::lock_guard<std::mutex> lock(mu_);
    boot_dbg_ = true;
  }

 private:
  uint32_t read_reg(uint32_t reg) const {
    if (reg == 0x00) {
      return 12U << 24;
    }
    if (reg == 0x01) {
      return 0x11 | ((static_cast<uint32_t>(redir_.size()) - 1) << 16);
    }
    if (reg >= 0x10 && reg < 0x10 + redir_.size() * 2) {
      uint32_t irq = (reg - 0x10) / 2;
      bool high = ((reg - 0x10) & 1) != 0;
      return high ? static_cast<uint32_t>(redir_[irq] >> 32) : static_cast<uint32_t>(redir_[irq]);
    }
    return 0;
  }

  void write_reg(uint32_t reg, uint32_t value) {
    if (reg >= 0x10 && reg < 0x10 + redir_.size() * 2) {
      uint32_t irq = (reg - 0x10) / 2;
      bool high = ((reg - 0x10) & 1) != 0;
      uint64_t old = redir_[irq];
      if (high) {
        redir_[irq] = (redir_[irq] & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
      } else {
        redir_[irq] = (redir_[irq] & 0xFFFFFFFF00000000ULL) | value;
        redir_[irq] = (redir_[irq] & ~kRteRemoteIrr) | (old & kRteRemoteIrr);
        if ((redir_[irq] & kRteTriggerMode) == 0) {
          redir_[irq] &= ~kRteRemoteIrr;
        }
      }
      if (boot_dbg_) {
        std::fprintf(stderr, "[node-vmm ioapic] rte[%u].%s=0x%08x entry=0x%016llx masked=%d\n",
                     irq,
                     high ? "high" : "low",
                     value,
                     (unsigned long long)redir_[irq],
                     (int)((redir_[irq] >> 16) & 1));
      }
      if (!high && (old & kRteMask) != 0 && (redir_[irq] & kRteMask) == 0 && line_level_[irq]) {
        request_irq_locked(irq);
      }
    }
  }

  uint32_t vector_for_irq_locked(uint32_t irq) const {
    uint32_t vector = static_cast<uint32_t>(redir_[irq] & kRteVectorMask);
    return vector == 0 ? 0x20 + irq : vector;
  }

  void request_irq_locked(uint32_t irq) {
    if (irq >= redir_.size()) {
      return;
    }
    uint64_t& entry = redir_[irq];
    if (boot_dbg_) {
      std::fprintf(stderr, "[node-vmm ioapic] request_irq(%u) entry=0x%016llx masked=%d remote_irr=%d\n",
                   irq,
                   (unsigned long long)entry,
                   (int)((entry & kRteMask) != 0),
                   (int)((entry & kRteRemoteIrr) != 0));
    }
    if (entry & kRteMask) {
      return;
    }
    bool level_triggered = (entry & kRteTriggerMode) != 0;
    if (level_triggered && (entry & kRteRemoteIrr) != 0) {
      return;
    }
    if (pending_.size() > 64) {
      return;
    }
    WHV_INTERRUPT_CONTROL control{};
    control.Type = WHvX64InterruptTypeFixed;
    control.DestinationMode = (entry & kRteDestinationMode) ? WHvX64InterruptDestinationModeLogical : WHvX64InterruptDestinationModePhysical;
    control.TriggerMode = level_triggered ? WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
    control.Destination = static_cast<UINT32>((entry >> 56) & 0xFF);
    control.Vector = vector_for_irq_locked(irq);
    if (level_triggered) {
      entry |= kRteRemoteIrr;
    }
    pending_.push_back(control);
    drain_pending_locked();
  }

  void drain_pending_locked() {
    bool delivered = false;
    while (!pending_.empty()) {
      WHV_INTERRUPT_CONTROL control = pending_.front();
      HRESULT hr = api_.request_interrupt(partition_, &control, sizeof(control));
      if (FAILED(hr)) {
        return;
      }
      pending_.pop_front();
      delivered = true;
    }
    if (delivered && waker_) {
      waker_();
    }
  }

  WhpApi& api_;
  WHV_PARTITION_HANDLE partition_{nullptr};
  mutable std::mutex mu_;
  uint32_t selected_{0};
  std::array<uint64_t, 24> redir_{};
  std::array<bool, 24> line_level_{};
  std::deque<WHV_INTERRUPT_CONTROL> pending_;
  std::function<void()> waker_;
  bool boot_dbg_{false};
};

// Uart::update_interrupt_locked moved to whp/devices/uart.cc.

// RequestFixedInterrupt moved to whp/irq.{h,cc}.
// Pic class moved to whp/devices/pic.{h,cc}.

// Pit / Hpet / Cmos / AcpiPmTimer classes moved to whp/devices/.

struct HyperVState {
  using Clock = std::chrono::steady_clock;
  uint64_t tsc_frequency_hz{kFallbackTscFrequencyHz};
  uint64_t apic_frequency_hz{kFallbackApicFrequencyHz};
  std::atomic<uint64_t> guest_os_id{0};
  std::atomic<uint64_t> hypercall{0};
  Clock::time_point started_at{Clock::now()};

  uint64_t time_ref_count() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       Clock::now() - started_at)
                       .count();
    return elapsed <= 0 ? 0 : static_cast<uint64_t>(elapsed) / 100;
  }
};

// Desc / DescChain moved to whp/virtio/desc.h.
// VirtioBlk moved to whp/virtio/blk.{h,cc}.

// VirtioRng moved to whp/virtio/rng.{h,cc}.

#ifdef NODE_VMM_HAVE_LIBSLIRP
extern "C" {
#include <libslirp.h>
}

class VirtioNet;

// Wraps a libslirp instance running on its own polling thread. Mirrors the
// QEMU adapter in qemu/net/slirp.c. The callbacks are designed for v6 of the
// libslirp ABI (`register_poll_socket` / SLIRP_CONFIG_VERSION_MAX = 6 from
// libslirp/src/libslirp.h:160-161).
class Slirp {
 public:
  struct HostFwd {
    bool udp{false};
    uint32_t host_ip{0};
    uint16_t host_port{0};
    uint16_t guest_port{0};
  };

  Slirp(VirtioNet* net, std::vector<HostFwd> host_fwds);
  ~Slirp();

  Slirp(const Slirp&) = delete;
  Slirp& operator=(const Slirp&) = delete;

  // Hand a packet from the guest virtio-net TX queue over to libslirp for
  // dispatch out the host network stack.
  void input_packet(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    if (slirp_ != nullptr) {
      slirp_input(slirp_, data, static_cast<int>(len));
    }
  }

 private:
  void poll_thread_main();

  static slirp_ssize_t SendPacketCb(const void* buf, size_t len, void* opaque) {
    return reinterpret_cast<Slirp*>(opaque)->on_send_packet(buf, len);
  }
  static void GuestErrorCb(const char* msg, void* opaque) {
    (void)opaque;
    std::fprintf(stderr, "[node-vmm slirp guest-error] %s\n", msg);
  }
  static int64_t ClockGetNsCb(void* opaque) {
    (void)opaque;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  }
  static void* TimerNewOpaqueCb(SlirpTimerId id, void* cb_opaque, void* opaque) {
    return reinterpret_cast<Slirp*>(opaque)->on_timer_new(id, cb_opaque);
  }
  static void TimerFreeCb(void* timer, void* opaque) {
    reinterpret_cast<Slirp*>(opaque)->on_timer_free(timer);
  }
  static void TimerModCb(void* timer, int64_t expire_ms, void* opaque) {
    reinterpret_cast<Slirp*>(opaque)->on_timer_mod(timer, expire_ms);
  }
  static void RegisterPollSocketCb(slirp_os_socket fd, void* opaque) {
    (void)fd;
    (void)opaque;
    // libslirp polls via slirp_pollfds_fill_socket; nothing to register.
  }
  static void UnregisterPollSocketCb(slirp_os_socket fd, void* opaque) {
    (void)fd;
    (void)opaque;
  }
  static void NotifyCb(void* opaque) {
    reinterpret_cast<Slirp*>(opaque)->on_notify();
  }

  slirp_ssize_t on_send_packet(const void* buf, size_t len);
  void* on_timer_new(SlirpTimerId id, void* cb_opaque);
  void on_timer_free(void* timer);
  void on_timer_mod(void* timer, int64_t expire_ms);
  void on_notify();

  struct Timer {
    SlirpTimerId id;
    void* cb_opaque;
    int64_t expire_ms{INT64_MAX};
  };

  struct PollEntry {
    slirp_os_socket fd;
    int events;
    int revents;
  };

  static int AddPollSocketCb(slirp_os_socket fd, int events, void* opaque);
  static int GetREventsCb(int idx, void* opaque);

  VirtioNet* net_{nullptr};
  std::vector<HostFwd> host_fwds_;

  std::mutex mu_;
  ::Slirp* slirp_{nullptr};
  std::vector<std::unique_ptr<Timer>> timers_;
  std::vector<PollEntry> poll_entries_;

  std::thread poll_thread_;
  std::atomic<bool> stop_{false};
  HANDLE wakeup_event_{nullptr};
};

// virtio-net device-config layout (offset relative to kVirtioMmioConfig).
constexpr uint16_t kVirtioNetCfgMac = 0x00;            // 6 bytes
constexpr uint16_t kVirtioNetCfgStatus = 0x06;         // 2 bytes (LE)
constexpr uint16_t kVirtioNetSLinkUp = 0x1;

// virtio-net device, MMIO transport. Two queues: 0=RX, 1=TX. The driver and
// device share descriptor/avail/used rings in guest memory. Layout follows
// the Virtio 1.0 spec, transport bits from qemu/hw/virtio/virtio-mmio.c
// switch tables.
class VirtioNet {
 public:
  VirtioNet(IoApic& ioapic, GuestMemory mem, uint64_t mmio_base, uint32_t irq, std::array<uint8_t, 6> mac)
      : ioapic_(ioapic), mem_(mem), mmio_base_(mmio_base), irq_(irq), mac_(mac) {}

  void attach_slirp(Slirp* slirp) { slirp_ = slirp; }
  uint64_t mmio_base() const { return mmio_base_; }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    std::memset(data, 0, len);
    if (len != 1 && len != 2 && len != 4) {
      return;
    }
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    uint32_t value = read_reg(off);
    for (uint32_t i = 0; i < len; i++) {
      data[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    if (len != 4) {
      return;
    }
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    uint32_t value = ReadU32(data);
    write_reg(off, value);
  }

  // Called from the slirp polling thread when a packet should reach the
  // guest. Pulls a descriptor chain off RX queue 0, prepends an empty
  // virtio_net_hdr_v1 (12 bytes), copies the payload, and notifies via IRQ.
  void deliver_rx_packet(const uint8_t* data, size_t len);

 private:
  static constexpr uint32_t kQueueSize = 256;
  static constexpr size_t kRxBacklogLimit = 256;
  // virtio_net_hdr_v1 is 12 bytes; with VIRTIO_NET_F_MRG_RXBUF negotiated the
  // guest expects an extra 2-byte num_buffers field, total 14. We pick the
  // size lazily from driver_features_ so the same device adapts when the
  // guest opts into mergeable rx buffers.
  static constexpr size_t kVirtioNetHdrLenBase = 12;
  static constexpr uint64_t kVirtioNetFMrgRxbuf = 1ULL << 15;
  size_t virtio_net_hdr_len_locked() const {
    return (driver_features_ & kVirtioNetFMrgRxbuf) ? 14 : kVirtioNetHdrLenBase;
  }

  struct Queue {
    bool ready{false};
    uint32_t num{kQueueSize};
    uint64_t desc_addr{0};
    uint64_t driver_addr{0};
    uint64_t device_addr{0};
    uint16_t last_avail_idx{0};
    uint16_t last_used_idx{0};
  };

  // Layout matches struct virtio_net_config from the Virtio 1.0 spec; we
  // expose the subset that linux/drivers/net/virtio_net.c queries (mac +
  // status + max_virtqueue_pairs + mtu).
#pragma pack(push, 1)
  struct VirtioNetConfig {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
  };
#pragma pack(pop)
  static_assert(sizeof(VirtioNetConfig) == 12, "virtio_net config drifted");

  uint32_t read_reg(uint32_t off);
  void write_reg(uint32_t off, uint32_t value);
  void process_tx_queue();
  bool deliver_rx_packet_now(const uint8_t* data, size_t len);
  void flush_rx_backlog();
  void raise_used_buffer_irq(uint32_t queue_index);
  void raise_config_change_irq_locked();
  void update_status_locked(uint32_t new_status);
  VirtioNetConfig build_net_config_locked() const;

  uint16_t read_u16(uint64_t gpa) {
    uint16_t value = 0;
    auto* ptr = ptr_or_null(gpa, 2);
    if (ptr != nullptr) {
      std::memcpy(&value, ptr, 2);
    }
    return value;
  }
  void write_u16(uint64_t gpa, uint16_t value) {
    auto* ptr = ptr_or_null(gpa, 2);
    if (ptr != nullptr) {
      std::memcpy(ptr, &value, 2);
    }
  }
  void write_u32(uint64_t gpa, uint32_t value) {
    auto* ptr = ptr_or_null(gpa, 4);
    if (ptr != nullptr) {
      std::memcpy(ptr, &value, 4);
    }
  }
  uint8_t* ptr_or_null(uint64_t gpa, uint64_t len) const {
    if (gpa > mem_.size() || len > mem_.size() - gpa) {
      return nullptr;
    }
    return mem_.data + gpa;
  }

  IoApic& ioapic_;
  GuestMemory mem_;
  uint64_t mmio_base_{0};
  uint32_t irq_{0};
  std::array<uint8_t, 6> mac_;
  Slirp* slirp_{nullptr};

  std::mutex mu_;  // protects ring state, status, interrupt_status
  uint32_t device_features_sel_{0};
  uint32_t driver_features_sel_{0};
  uint64_t driver_features_{0};
  uint32_t status_{0};
  uint32_t interrupt_status_{0};
  uint32_t queue_sel_{0};
  uint32_t config_generation_{0};
  bool link_up_{true};
  bool driver_ok_announced_{false};
  bool rx_flush_active_{false};
  Queue queues_[2];  // 0 = RX, 1 = TX
  std::deque<std::vector<uint8_t>> rx_backlog_;
};

inline uint64_t VirtioNetDeviceFeatures() {
  return kVirtioNetFMac | kVirtioNetFStatus | kVirtioFVersion1;
}

uint32_t VirtioNet::read_reg(uint32_t off) {
  std::lock_guard<std::mutex> lock(mu_);
  switch (off) {
    case kVirtioMmioMagicValue:
      return kVirtioMagic;
    case kVirtioMmioVersion:
      return 2;
    case kVirtioMmioDeviceId:
      return 1;  // virtio-net
    case kVirtioMmioVendorId:
      return 0x554D4551;  // "QEMU"
    case kVirtioMmioDeviceFeatures: {
      uint64_t feats = VirtioNetDeviceFeatures();
      return device_features_sel_ == 1
                 ? static_cast<uint32_t>(feats >> 32)
                 : static_cast<uint32_t>(feats & 0xFFFFFFFFu);
    }
    case kVirtioMmioQueueNumMax:
      return kQueueSize;
    case kVirtioMmioQueueReady:
      return queue_sel_ < 2 && queues_[queue_sel_].ready ? 1 : 0;
    case kVirtioMmioInterruptStatus:
      return interrupt_status_;
    case kVirtioMmioStatus:
      return status_;
    case kVirtioMmioConfigGeneration:
      return config_generation_;
    // virtio-mmio v2 shared-memory probe; we have no SHM regions, so report
    // -1 in both halves like qemu/hw/virtio/virtio-mmio.c:211-218.
    case kVirtioMmioShmLenLow:
    case kVirtioMmioShmLenHigh:
      return 0xFFFFFFFFu;
    case kVirtioMmioShmBaseLow:
    case kVirtioMmioShmBaseHigh:
      return 0xFFFFFFFFu;
    default:
      // Device config space. The driver may read individual bytes (MAC) or
      // 16-bit words (status), so we serve the request from a packed buffer
      // and let the byte-extraction in read_mmio() pick the right bytes.
      if (off >= kVirtioMmioConfig &&
          off < kVirtioMmioConfig + sizeof(VirtioNetConfig)) {
        VirtioNetConfig cfg = build_net_config_locked();
        const auto* bytes = reinterpret_cast<const uint8_t*>(&cfg);
        size_t cfg_off = off - kVirtioMmioConfig;
        uint32_t value = 0;
        for (size_t i = 0; i < 4 && cfg_off + i < sizeof(VirtioNetConfig); i++) {
          value |= static_cast<uint32_t>(bytes[cfg_off + i]) << (i * 8);
        }
        return value;
      }
      return 0;
  }
}

VirtioNet::VirtioNetConfig VirtioNet::build_net_config_locked() const {
  VirtioNetConfig cfg{};
  std::memcpy(cfg.mac, mac_.data(), 6);
  cfg.status = link_up_ ? kVirtioNetSLinkUp : 0;
  cfg.max_virtqueue_pairs = 1;
  cfg.mtu = 1500;
  return cfg;
}

void VirtioNet::write_reg(uint32_t off, uint32_t value) {
  bool kick_tx = false;
  bool kick_rx = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    switch (off) {
      case kVirtioMmioDeviceFeaturesSel:
        device_features_sel_ = value;
        return;
      case kVirtioMmioDriverFeatures:
        if (driver_features_sel_ == 0) {
          driver_features_ = (driver_features_ & 0xFFFFFFFF00000000ULL) | value;
        } else {
          driver_features_ = (driver_features_ & 0x00000000FFFFFFFFULL) |
                             (uint64_t(value) << 32);
        }
        return;
      case kVirtioMmioDriverFeaturesSel:
        driver_features_sel_ = value;
        return;
      case kVirtioMmioQueueSel:
        queue_sel_ = value;
        return;
      case kVirtioMmioQueueNum:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].num = value == 0 ? 0 : std::min<uint32_t>(value, kQueueSize);
        }
        return;
      case kVirtioMmioQueueReady:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].ready = value != 0;
        }
        return;
      case kVirtioMmioQueueNotify:
        kick_tx = (value == 1);
        kick_rx = (value == 0);
        break;
      case kVirtioMmioInterruptAck:
        interrupt_status_ &= ~value;
        return;
      case kVirtioMmioStatus:
        update_status_locked(value);
        return;
      case kVirtioMmioQueueDescLow:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].desc_addr =
              (queues_[queue_sel_].desc_addr & 0xFFFFFFFF00000000ULL) | value;
        }
        return;
      case kVirtioMmioQueueDescHigh:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].desc_addr =
              (queues_[queue_sel_].desc_addr & 0xFFFFFFFFULL) |
              (uint64_t(value) << 32);
        }
        return;
      case kVirtioMmioQueueDriverLow:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].driver_addr =
              (queues_[queue_sel_].driver_addr & 0xFFFFFFFF00000000ULL) | value;
        }
        return;
      case kVirtioMmioQueueDriverHigh:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].driver_addr =
              (queues_[queue_sel_].driver_addr & 0xFFFFFFFFULL) |
              (uint64_t(value) << 32);
        }
        return;
      case kVirtioMmioQueueDeviceLow:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].device_addr =
              (queues_[queue_sel_].device_addr & 0xFFFFFFFF00000000ULL) | value;
        }
        return;
      case kVirtioMmioQueueDeviceHigh:
        if (queue_sel_ < 2) {
          queues_[queue_sel_].device_addr =
              (queues_[queue_sel_].device_addr & 0xFFFFFFFFULL) |
              (uint64_t(value) << 32);
        }
        return;
      default:
        return;
    }
  }
  if (kick_tx) {
    process_tx_queue();
  } else if (kick_rx) {
    // RX kicks make new buffers available. QEMU's virtio-net backend can hold
    // packets until the guest posts descriptors; mirror that instead of
    // dropping slirp packets during bursts.
    flush_rx_backlog();
  }
}

void VirtioNet::update_status_locked(uint32_t new_status) {
  // Mirror QEMU's virtio_set_status semantics for the bits we care about
  // (qemu/hw/virtio/virtio-mmio.c:426-445 + qemu/hw/net/virtio-net.c handlers):
  //   * On RESET, drop ring state and re-arm DRIVER_OK announcement.
  //   * On the first DRIVER_OK transition, leave link state available through
  //     config space; don't inject a config IRQ before Linux has installed
  //     the virtio-net IRQ handler.
  //   * On FAILED, mark queues unready so subsequent kicks are dropped.
  uint32_t prev = status_;
  status_ = new_status;
  if (new_status == 0) {
    for (auto& q : queues_) {
      q = Queue{};
    }
    interrupt_status_ = 0;
    driver_ok_announced_ = false;
    return;
  }
  if ((new_status & kVirtioStatusFailed) != 0) {
    for (auto& q : queues_) {
      q.ready = false;
    }
    return;
  }
  // When the guest commits FEATURES_OK we must ensure the driver only acked
  // features we actually advertise; otherwise spec-compliant drivers expect
  // us to refuse the bit so they can fall back. See qemu/hw/virtio/virtio.c
  // virtio_set_features for the canonical handling.
  bool now_features_ok = (new_status & kVirtioStatusFeaturesOk) != 0;
  bool was_features_ok = (prev & kVirtioStatusFeaturesOk) != 0;
  if (now_features_ok && !was_features_ok) {
    uint64_t supported = VirtioNetDeviceFeatures();
    if ((driver_features_ & ~supported) != 0) {
      status_ &= ~kVirtioStatusFeaturesOk;
    }
  }
  bool now_driver_ok = (new_status & kVirtioStatusDriverOk) != 0;
  bool was_driver_ok = (prev & kVirtioStatusDriverOk) != 0;
  if (now_driver_ok && !was_driver_ok && !driver_ok_announced_) {
    driver_ok_announced_ = true;
  }
}

void VirtioNet::raise_config_change_irq_locked() {
  // virtio-mmio defines bit 1 of INTERRUPT_STATUS as "configuration change",
  // see qemu/hw/virtio/virtio-mmio.c VIRTIO_MMIO_INT_CONFIG handling and the
  // Linux driver's vm_interrupt() in drivers/virtio/virtio_mmio.c.
  interrupt_status_ |= kVirtioInterruptConfig;
  ioapic_.request_irq(irq_);
}

void VirtioNet::process_tx_queue() {
  Slirp* slirp = slirp_;
  if (slirp == nullptr) {
    return;
  }
  Queue& q = queues_[1];
  bool processed = false;
  while (true) {
    uint16_t avail_idx;
    uint16_t desc_idx;
    uint32_t queue_num;
    uint64_t desc_addr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!q.ready || q.num == 0 || q.driver_addr == 0 || q.desc_addr == 0 ||
          q.device_addr == 0) {
        return;
      }
      queue_num = q.num;
      desc_addr = q.desc_addr;
      avail_idx = read_u16(q.driver_addr + 2);
      if (q.last_avail_idx == avail_idx) {
        break;
      }
      uint16_t ring_off = q.last_avail_idx % queue_num;
      desc_idx = read_u16(q.driver_addr + 4 + ring_off * 2);
      if (desc_idx >= queue_num) {
        q.last_avail_idx++;
        continue;
      }
      q.last_avail_idx++;
    }

    // Walk the descriptor chain into a contiguous buffer. Skip the leading
    // virtio_net_hdr_v1 - the host network stack only wants the L2 frame.
    std::vector<uint8_t> packet;
    packet.reserve(2048);
    uint16_t current = desc_idx;
    size_t hops = 0;
    bool truncated = false;
    while (true) {
      hops++;
      if (current >= queue_num || hops > queue_num) {
        truncated = true;
        break;
      }
      auto* desc = ptr_or_null(desc_addr + current * 16, 16);
      if (desc == nullptr) {
        truncated = true;
        break;
      }
      uint64_t addr;
      uint32_t segment_len;
      uint16_t flags, next;
      std::memcpy(&addr, desc, 8);
      std::memcpy(&segment_len, desc + 8, 4);
      std::memcpy(&flags, desc + 12, 2);
      std::memcpy(&next, desc + 14, 2);
      if ((flags & kVirtioRingDescFWrite) != 0) {
        // Driver-write segments shouldn't appear in TX chains; ignore.
      } else {
        auto* segment = ptr_or_null(addr, segment_len);
        if (segment != nullptr) {
          packet.insert(packet.end(), segment, segment + segment_len);
        }
      }
      if ((flags & kVirtioRingDescFNext) == 0) {
        break;
      }
      if (next >= queue_num) {
        truncated = true;
        break;
      }
      current = next;
    }
    (void)truncated;

    size_t hdr_len;
    {
      std::lock_guard<std::mutex> lock(mu_);
      hdr_len = virtio_net_hdr_len_locked();
    }
    if (packet.size() > hdr_len) {
      slirp->input_packet(packet.data() + hdr_len, packet.size() - hdr_len);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!q.ready || q.num == 0 || q.device_addr == 0) {
        continue;
      }
      uint16_t used_off = q.last_used_idx % q.num;
      uint64_t used_elem_addr = q.device_addr + 4 + used_off * 8;
      write_u32(used_elem_addr, desc_idx);
      write_u32(used_elem_addr + 4, 0);  // length: TX uses 0
      q.last_used_idx++;
      write_u16(q.device_addr + 2, q.last_used_idx);
    }
    processed = true;
  }
  if (processed) {
    raise_used_buffer_irq(1);
  }
}

void VirtioNet::deliver_rx_packet(const uint8_t* data, size_t len) {
  bool backlog_has_packets = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    backlog_has_packets = !rx_backlog_.empty();
  }
  if (!backlog_has_packets && deliver_rx_packet_now(data, len)) {
    flush_rx_backlog();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (rx_backlog_.size() < kRxBacklogLimit) {
      rx_backlog_.emplace_back(data, data + len);
    }
  }
  flush_rx_backlog();
}

void VirtioNet::flush_rx_backlog() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (rx_flush_active_) {
      return;
    }
    rx_flush_active_ = true;
  }
  for (;;) {
    std::vector<uint8_t> packet;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (rx_backlog_.empty()) {
        rx_flush_active_ = false;
        return;
      }
      packet = rx_backlog_.front();
    }
    if (!deliver_rx_packet_now(packet.data(), packet.size())) {
      std::lock_guard<std::mutex> lock(mu_);
      rx_flush_active_ = false;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!rx_backlog_.empty()) {
        rx_backlog_.pop_front();
      }
    }
  }
}

bool VirtioNet::deliver_rx_packet_now(const uint8_t* data, size_t len) {
  if (len > 1500 + 14 + 4) {
    return true;  // jumbo frames not supported by our defaults; drop.
  }
  Queue& q = queues_[0];
  uint16_t desc_idx;
  uint32_t queue_num;
  uint64_t desc_addr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!q.ready || q.num == 0 || q.driver_addr == 0 || q.desc_addr == 0 ||
        q.device_addr == 0) {
      return false;
    }
    queue_num = q.num;
    desc_addr = q.desc_addr;
    uint16_t avail_idx = read_u16(q.driver_addr + 2);
    if (q.last_avail_idx == avail_idx) {
      return false;  // no buffer available yet
    }
    uint16_t ring_off = q.last_avail_idx % queue_num;
    desc_idx = read_u16(q.driver_addr + 4 + ring_off * 2);
    if (desc_idx >= queue_num) {
      q.last_avail_idx++;
      return true;
    }
    q.last_avail_idx++;
  }

  // Walk descriptor chain and write virtio_net_hdr + payload into it.
  size_t hdr_len;
  {
    std::lock_guard<std::mutex> lock(mu_);
    hdr_len = virtio_net_hdr_len_locked();
  }
  std::vector<uint8_t> framed(hdr_len + len, 0);
  if (hdr_len >= 14) {
    // virtio_net_hdr_v1.num_buffers (LE u16) - we only ever use one buffer
    // per packet, so 1 is the right value when MRG_RXBUF is negotiated.
    framed[12] = 0x01;
    framed[13] = 0x00;
  }
  std::memcpy(framed.data() + hdr_len, data, len);
  size_t copied = 0;
  uint16_t current = desc_idx;
  size_t hops = 0;
  bool ok = true;
  while (copied < framed.size()) {
    hops++;
    if (current >= queue_num || hops > queue_num) {
      ok = false;
      break;
    }
    auto* desc = ptr_or_null(desc_addr + current * 16, 16);
    if (desc == nullptr) {
      ok = false;
      break;
    }
    uint64_t addr;
    uint32_t segment_len;
    uint16_t flags, next;
    std::memcpy(&addr, desc, 8);
    std::memcpy(&segment_len, desc + 8, 4);
    std::memcpy(&flags, desc + 12, 2);
    std::memcpy(&next, desc + 14, 2);
    if ((flags & kVirtioRingDescFWrite) == 0) {
      ok = false;
      break;
    }
    size_t take = std::min<size_t>(segment_len, framed.size() - copied);
    auto* segment = ptr_or_null(addr, take);
    if (segment == nullptr) {
      ok = false;
      break;
    }
    std::memcpy(segment, framed.data() + copied, take);
    copied += take;
    if ((flags & kVirtioRingDescFNext) == 0) {
      break;
    }
    if (next >= queue_num) {
      ok = false;
      break;
    }
    current = next;
  }
  if (!ok) {
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!q.ready || q.num == 0 || q.device_addr == 0) {
      return true;
    }
    uint16_t used_off = q.last_used_idx % q.num;
    uint64_t used_elem_addr = q.device_addr + 4 + used_off * 8;
    write_u32(used_elem_addr, desc_idx);
    write_u32(used_elem_addr + 4, static_cast<uint32_t>(framed.size()));
    q.last_used_idx++;
    write_u16(q.device_addr + 2, q.last_used_idx);
  }
  raise_used_buffer_irq(0);
  return true;
}

void VirtioNet::raise_used_buffer_irq(uint32_t queue_index) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_index >= 2) {
      return;
    }
    Queue& q = queues_[queue_index];
    if (!q.ready || q.driver_addr == 0) {
      return;
    }
    uint16_t avail_flags = read_u16(q.driver_addr);
    if ((avail_flags & kVirtioRingAvailFNoInterrupt) != 0) {
      return;
    }
    interrupt_status_ |= kVirtioInterruptVring;
  }
  ioapic_.request_irq(irq_);
}

Slirp::Slirp(VirtioNet* net, std::vector<HostFwd> host_fwds)
    : net_(net), host_fwds_(std::move(host_fwds)) {
  wakeup_event_ = WSACreateEvent();
  Check(wakeup_event_ != WSA_INVALID_EVENT,
        "WSACreateEvent failed for slirp wakeup");

  SlirpConfig cfg{};
  cfg.version = SLIRP_CONFIG_VERSION_MAX;
  cfg.in_enabled = true;
  cfg.vnetwork.s_addr = htonl(0x0A000200u);    // 10.0.2.0
  cfg.vnetmask.s_addr = htonl(0xFFFFFF00u);    // 255.255.255.0
  cfg.vhost.s_addr = htonl(0x0A000202u);       // 10.0.2.2
  cfg.vdhcp_start.s_addr = htonl(0x0A00020Fu); // 10.0.2.15
  cfg.vnameserver.s_addr = htonl(0x0A000203u); // 10.0.2.3
  cfg.if_mtu = 1500;
  cfg.if_mru = 1500;

  static const SlirpCb callbacks = {
      /* send_packet */ &Slirp::SendPacketCb,
      /* guest_error */ &Slirp::GuestErrorCb,
      /* clock_get_ns */ &Slirp::ClockGetNsCb,
      /* timer_new */ nullptr,
      /* timer_free */ &Slirp::TimerFreeCb,
      /* timer_mod */ &Slirp::TimerModCb,
      /* register_poll_fd */ nullptr,
      /* unregister_poll_fd */ nullptr,
      /* notify */ &Slirp::NotifyCb,
      /* init_completed */ nullptr,
      /* timer_new_opaque */ &Slirp::TimerNewOpaqueCb,
      /* register_poll_socket */ &Slirp::RegisterPollSocketCb,
      /* unregister_poll_socket */ &Slirp::UnregisterPollSocketCb,
  };

  slirp_ = slirp_new(&cfg, &callbacks, this);
  Check(slirp_ != nullptr, "slirp_new failed");

  for (const HostFwd& fwd : host_fwds_) {
    in_addr host{};
    in_addr guest{};
    host.s_addr = htonl(fwd.host_ip);
    guest.s_addr = htonl(0x0A00020Fu);  // forward to dhcp lease addr
    int rc = slirp_add_hostfwd(slirp_, fwd.udp ? 1 : 0, host, fwd.host_port,
                               guest, fwd.guest_port);
    if (rc != 0) {
      std::fprintf(stderr,
                   "[node-vmm slirp] hostfwd %s:%u -> guest:%u failed (rc=%d)\n",
                   fwd.udp ? "udp" : "tcp", fwd.host_port, fwd.guest_port, rc);
    }
  }

  poll_thread_ = std::thread(&Slirp::poll_thread_main, this);
}

Slirp::~Slirp() {
  stop_.store(true);
  if (wakeup_event_ != nullptr) {
    WSASetEvent(wakeup_event_);
  }
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (slirp_ != nullptr) {
      slirp_cleanup(slirp_);
      slirp_ = nullptr;
    }
    timers_.clear();
  }
  if (wakeup_event_ != nullptr) {
    WSACloseEvent(wakeup_event_);
    wakeup_event_ = nullptr;
  }
}

slirp_ssize_t Slirp::on_send_packet(const void* buf, size_t len) {
  if (net_ != nullptr) {
    // Mirror eth_pad_short_frame from qemu/net/slirp.c:net_slirp_send_packet:
    // Ethernet frames must be at least 60 bytes (excluding FCS) for the guest
    // NIC to accept them. ARP / DHCP replies routinely come in shorter than
    // that and were silently dropped on the guest side until we pad them.
    if (len < 60) {
      uint8_t padded[64]{};
      std::memcpy(padded, buf, len);
      net_->deliver_rx_packet(padded, 60);
      return static_cast<slirp_ssize_t>(len);
    }
    net_->deliver_rx_packet(reinterpret_cast<const uint8_t*>(buf), len);
  }
  return static_cast<slirp_ssize_t>(len);
}

void* Slirp::on_timer_new(SlirpTimerId id, void* cb_opaque) {
  std::lock_guard<std::mutex> lock(mu_);
  auto timer = std::make_unique<Timer>();
  timer->id = id;
  timer->cb_opaque = cb_opaque;
  timer->expire_ms = INT64_MAX;
  Timer* raw = timer.get();
  timers_.push_back(std::move(timer));
  return raw;
}

void Slirp::on_timer_free(void* timer) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->get() == timer) {
      timers_.erase(it);
      return;
    }
  }
}

void Slirp::on_timer_mod(void* timer, int64_t expire_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& t : timers_) {
    if (t.get() == timer) {
      t->expire_ms = expire_ms;
      break;
    }
  }
  if (wakeup_event_ != nullptr) {
    WSASetEvent(wakeup_event_);
  }
}

void Slirp::on_notify() {
  if (wakeup_event_ != nullptr) {
    WSASetEvent(wakeup_event_);
  }
}

int Slirp::AddPollSocketCb(slirp_os_socket fd, int events, void* opaque) {
  auto* self = reinterpret_cast<Slirp*>(opaque);
  PollEntry entry{};
  entry.fd = fd;
  entry.events = events;
  entry.revents = 0;
  self->poll_entries_.push_back(entry);
  return static_cast<int>(self->poll_entries_.size() - 1);
}

int Slirp::GetREventsCb(int idx, void* opaque) {
  auto* self = reinterpret_cast<Slirp*>(opaque);
  if (idx < 0 || static_cast<size_t>(idx) >= self->poll_entries_.size()) {
    return 0;
  }
  return self->poll_entries_[idx].revents;
}

void Slirp::poll_thread_main() {
  while (!stop_.load()) {
    poll_entries_.clear();
    uint32_t timeout_ms = 1000;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (slirp_ == nullptr) {
        return;
      }
      slirp_pollfds_fill_socket(slirp_, &timeout_ms, &Slirp::AddPollSocketCb,
                                this);
    }

    // Build WSAPOLLFD array. Always include our wakeup event via a dummy
    // fd we control by signaling wakeup_event_ from the foreign thread.
    std::vector<WSAPOLLFD> fds;
    fds.reserve(poll_entries_.size());
    for (auto& e : poll_entries_) {
      WSAPOLLFD pfd{};
      pfd.fd = e.fd;
      pfd.events = 0;
      if (e.events & SLIRP_POLL_IN) pfd.events |= POLLRDNORM;
      if (e.events & SLIRP_POLL_OUT) pfd.events |= POLLWRNORM;
      if (e.events & SLIRP_POLL_PRI) pfd.events |= POLLRDBAND;
      fds.push_back(pfd);
    }

    DWORD wait_ms = std::min<uint32_t>(timeout_ms, 50);
    if (!fds.empty()) {
      WSAPoll(fds.data(), static_cast<ULONG>(fds.size()),
              static_cast<INT>(wait_ms));
    } else {
      WaitForSingleObject(wakeup_event_, wait_ms);
    }
    WSAResetEvent(wakeup_event_);

    if (stop_.load()) {
      break;
    }

    int select_error = 0;
    for (size_t i = 0; i < fds.size(); i++) {
      const auto& pfd = fds[i];
      int re = 0;
      if (pfd.revents & (POLLRDNORM | POLLIN)) re |= SLIRP_POLL_IN;
      if (pfd.revents & (POLLWRNORM | POLLOUT)) re |= SLIRP_POLL_OUT;
      if (pfd.revents & POLLRDBAND) re |= SLIRP_POLL_PRI;
      if (pfd.revents & POLLERR) re |= SLIRP_POLL_ERR;
      if (pfd.revents & POLLHUP) re |= SLIRP_POLL_HUP;
      poll_entries_[i].revents = re;
      if (pfd.revents & POLLNVAL) {
        select_error = 1;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (slirp_ == nullptr) {
        break;
      }
      slirp_pollfds_poll(slirp_, select_error, &Slirp::GetREventsCb, this);

      // Fire any expired timers.
      auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
      for (auto& t : timers_) {
        if (t->expire_ms != INT64_MAX && t->expire_ms <= now_ms) {
          int64_t expired = t->expire_ms;
          t->expire_ms = INT64_MAX;
          slirp_handle_timer(slirp_, t->id, t->cb_opaque);
          (void)expired;
        }
      }
    }
  }
}
#endif  // NODE_VMM_HAVE_LIBSLIRP

struct EmulatorContext {
  WhpApi* api{nullptr};
  WHV_PARTITION_HANDLE partition{nullptr};
  UINT32 vp_index{0};
  GuestMemory mem{};
  Uart* uart{nullptr};
  GuestExit* guest_exit{nullptr};
  Pic* pic{nullptr};
  Pit* pit{nullptr};
  Hpet* hpet{nullptr};
  AcpiPmTimer* pm_timer{nullptr};
  IoApic* ioapic{nullptr};
  std::vector<VirtioBlk*> blks;
  VirtioRng* rng{nullptr};
  uint64_t rng_base{0};
  Cmos* cmos{nullptr};
#ifdef NODE_VMM_HAVE_LIBSLIRP
  VirtioNet* net{nullptr};
#endif
};

// =====================================================================
// SECTION: WHP emulator I/O + MMIO callbacks + RunVm orchestration
// =====================================================================
HRESULT CALLBACK EmulatorGetRegisters(
    VOID* context,
    const WHV_REGISTER_NAME* names,
    UINT32 count,
    WHV_REGISTER_VALUE* values) {
  auto* ctx = reinterpret_cast<EmulatorContext*>(context);
  return ctx->api->get_vp_registers(ctx->partition, ctx->vp_index, names, count, values);
}

HRESULT CALLBACK EmulatorSetRegisters(
    VOID* context,
    const WHV_REGISTER_NAME* names,
    UINT32 count,
    const WHV_REGISTER_VALUE* values) {
  auto* ctx = reinterpret_cast<EmulatorContext*>(context);
  return ctx->api->set_vp_registers(ctx->partition, ctx->vp_index, names, count, values);
}

HRESULT CALLBACK EmulatorTranslateGva(
    VOID* context,
    WHV_GUEST_VIRTUAL_ADDRESS gva,
    WHV_TRANSLATE_GVA_FLAGS flags,
    WHV_TRANSLATE_GVA_RESULT_CODE* result,
    WHV_GUEST_PHYSICAL_ADDRESS* gpa) {
  auto* ctx = reinterpret_cast<EmulatorContext*>(context);
  WHV_TRANSLATE_GVA_RESULT translation{};
  HRESULT hr = ctx->api->translate_gva(ctx->partition, ctx->vp_index, gva, flags, &translation, gpa);
  *result = translation.ResultCode;
  return hr;
}

HRESULT CALLBACK EmulatorIoPort(VOID* context, WHV_EMULATOR_IO_ACCESS_INFO* access) {
  auto* ctx = reinterpret_cast<EmulatorContext*>(context);
  bool write = access->Direction != 0;
  uint16_t port = access->Port;
  if (WhpTraceEnabled()) {
    std::fprintf(
        stderr,
        "[node-vmm whp emu-io] port=%s size=%u dir=%u data=%s write=%u\n",
        Hex(port).c_str(),
        access->AccessSize,
        access->Direction,
        Hex(access->Data).c_str(),
        write ? 1U : 0U);
  }
  if (port == kNodeVmmExitPort && access->AccessSize == 1) {
    if (write && ctx->guest_exit != nullptr) {
      ctx->guest_exit->requested = true;
      ctx->guest_exit->status = access->Data & 0xFF;
    } else if (!write) {
      access->Data = 0;
    }
    return S_OK;
  }
  if (port == kNodeVmmConsolePort && access->AccessSize == 1) {
    if (write) {
      uint8_t byte = static_cast<uint8_t>(access->Data & 0xFF);
      ctx->uart->emit_bytes(&byte, 1);
    } else {
      access->Data = 0;
    }
    return S_OK;
  }
  if (port >= kCom1Base && port < kCom1Base + 8 && access->AccessSize == 1) {
    uint16_t off = static_cast<uint16_t>(port - kCom1Base);
    if (write) {
      ctx->uart->write(off, static_cast<uint8_t>(access->Data & 0xFF));
    } else {
      access->Data = ctx->uart->read(off);
    }
    return S_OK;
  }
  if ((port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1) && access->AccessSize == 1) {
    if (write) {
      ctx->pic->write_port(port, static_cast<uint8_t>(access->Data & 0xFF));
    } else {
      access->Data = ctx->pic->read_port(port);
    }
    return S_OK;
  }
  if (port >= 0x40 && port <= 0x43 && access->AccessSize == 1) {
    if (write) {
      ctx->pit->write_port(port, static_cast<uint8_t>(access->Data & 0xFF));
    } else {
      access->Data = ctx->pit->read_port(port);
    }
    return S_OK;
  }
  if (port == 0x61 && access->AccessSize == 1) {
    // PC AT NMI Status / Speaker control: Linux uses bit 0 to gate PIT
    // channel 2 and reads bit 5 to detect terminal count for TSC calibration
    // (arch/x86/kernel/tsc.c:pit_calibrate_tsc).
    if (write) {
      uint8_t value = static_cast<uint8_t>(access->Data & 0xFF);
      ctx->pit->set_channel2_gate((value & 0x01) != 0);
    } else {
      // Layout matches qemu/hw/audio/pcspk.c:pcspk_io_read: gate(b0) |
      // data_on(b1) | dummy_refresh(b4) | ch2_out(b5). We don't model the
      // speaker; data_on stays 0. The refresh-clock toggles to satisfy any
      // probe that polls bit 4 for time progression.
      static std::atomic<uint8_t> refresh_toggle{0};
      uint8_t value = ctx->pit->channel2_gated() ? 0x01 : 0x00;
      value |= (refresh_toggle.fetch_xor(0x10) & 0x10);
      if (ctx->pit->channel2_out_high()) {
        value |= 0x20;
      }
      access->Data = value;
    }
    return S_OK;
  }
  if (port >= kAcpiPmTimerPort && port < kAcpiPmTimerPort + 4 && access->AccessSize <= 4) {
    if (!write && ctx->pm_timer != nullptr) {
      access->Data = ctx->pm_timer->read(port, static_cast<uint8_t>(access->AccessSize));
    }
    return S_OK;
  }
  if ((port == 0x70 || port == 0x71) && access->AccessSize == 1 && ctx->cmos != nullptr) {
    if (write) {
      ctx->cmos->write_port(port, static_cast<uint8_t>(access->Data & 0xFF));
    } else {
      access->Data = ctx->cmos->read_port(port);
    }
    return S_OK;
  }
  if (!write) {
    access->Data = 0;
  }
  return S_OK;
}

HRESULT CALLBACK EmulatorMemory(VOID* context, WHV_EMULATOR_MEMORY_ACCESS_INFO* access) {
  auto* ctx = reinterpret_cast<EmulatorContext*>(context);
  bool write = access->Direction != 0;
  uint64_t gpa = access->GpaAddress;
  uint32_t size = access->AccessSize;
  if (size == 0 || size > 8) {
    return E_INVALIDARG;
  }
  for (VirtioBlk* blk : ctx->blks) {
    if (blk != nullptr && gpa >= blk->mmio_base() && gpa < blk->mmio_base() + kVirtioStride) {
      if (write) {
        blk->write_mmio(gpa, access->Data, size);
      } else {
        blk->read_mmio(gpa, access->Data, size);
      }
      return S_OK;
    }
  }
  if (ctx->rng != nullptr && gpa >= ctx->rng_base && gpa < ctx->rng_base + kVirtioStride) {
    if (write) {
      ctx->rng->write_mmio(gpa, access->Data, size);
    } else {
      ctx->rng->read_mmio(gpa, access->Data, size);
    }
    return S_OK;
  }
  if (gpa >= kIoApicBase && gpa < kIoApicBase + kIoApicSize) {
    if (write) {
      ctx->ioapic->write_mmio(gpa, access->Data, size);
    } else {
      ctx->ioapic->read_mmio(gpa, access->Data, size);
    }
    return S_OK;
  }
  if (ctx->hpet != nullptr && gpa >= kHpetBase && gpa < kHpetBase + kHpetSize) {
    if (write) {
      ctx->hpet->write_mmio(gpa, access->Data, size);
    } else {
      ctx->hpet->read_mmio(gpa, access->Data, size);
    }
    return S_OK;
  }
#ifdef NODE_VMM_HAVE_LIBSLIRP
  if (ctx->net != nullptr && gpa >= ctx->net->mmio_base() &&
      gpa < ctx->net->mmio_base() + kVirtioStride) {
    if (write) {
      ctx->net->write_mmio(gpa, access->Data, size);
    } else {
      ctx->net->read_mmio(gpa, access->Data, size);
    }
    return S_OK;
  }
#endif
  if (gpa < ctx->mem.size() && size <= ctx->mem.size() - gpa) {
    if (write) {
      std::memcpy(ctx->mem.ptr(gpa, size), access->Data, size);
    } else {
      std::memcpy(access->Data, ctx->mem.ptr(gpa, size), size);
    }
    return S_OK;
  }
  if (!write) {
    std::memset(access->Data, 0, size);
  }
  return S_OK;
}

bool TranslateGuestAddress(WhpApi& api, WHV_PARTITION_HANDLE partition, UINT32 vp_index, uint64_t gva, uint64_t* gpa) {
  WHV_TRANSLATE_GVA_RESULT translation{};
  HRESULT hr = api.translate_gva(partition, vp_index, gva, WHvTranslateGvaFlagNone, &translation, gpa);
  return SUCCEEDED(hr) && translation.ResultCode == WHvTranslateGvaResultSuccess;
}

uint8_t FetchGuestInstructionByte(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    GuestMemory& guest,
    uint64_t gva) {
  uint64_t gpa = gva;
  (void)TranslateGuestAddress(api, partition, vp_index, gva, &gpa);
  return *guest.ptr(gpa, 1);
}

uint8_t DecodeIoInstructionLength(const WHV_X64_IO_PORT_ACCESS_CONTEXT& io) {
  if (io.InstructionByteCount != 0) {
    return io.InstructionByteCount;
  }
  uint8_t pos = 0;
  while (pos < sizeof(io.InstructionBytes)) {
    uint8_t byte = io.InstructionBytes[pos];
    bool prefix =
        byte == 0x66 || byte == 0x67 || byte == 0xF2 || byte == 0xF3 ||
        byte == 0x2E || byte == 0x36 || byte == 0x3E || byte == 0x26 ||
        byte == 0x64 || byte == 0x65 || (byte >= 0x40 && byte <= 0x4F);
    if (!prefix) {
      break;
    }
    pos++;
  }
  if (pos >= sizeof(io.InstructionBytes)) {
    return 0;
  }
  switch (io.InstructionBytes[pos]) {
    case 0xE4:
    case 0xE5:
    case 0xE6:
    case 0xE7:
      return pos + 2;
    case 0xEC:
    case 0xED:
    case 0xEE:
    case 0xEF:
      return pos + 1;
    default:
      return 0;
  }
}

WHV_X64_IO_PORT_ACCESS_CONTEXT PrepareWhpIoContext(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    GuestMemory& guest,
    const WHV_RUN_VP_EXIT_CONTEXT& exit_context) {
  WHV_X64_IO_PORT_ACCESS_CONTEXT io = exit_context.IoPortAccess;
  io.Rax = GetRegister64(api, partition, vp_index, WHvX64RegisterRax);
  uint64_t rdx = WhpTraceEnabled() ? GetRegister64(api, partition, vp_index, WHvX64RegisterRdx) : 0;
  if (WhpTraceEnabled()) {
    std::fprintf(stderr, "[node-vmm whp io-prep] rax=%s rdx=%s vplen=%u", Hex(io.Rax).c_str(), Hex(rdx).c_str(), exit_context.VpContext.InstructionLength);
  }
  if (io.InstructionByteCount == 0) {
    for (size_t i = 0; i < sizeof(io.InstructionBytes); i++) {
      io.InstructionBytes[i] = FetchGuestInstructionByte(api, partition, vp_index, guest, exit_context.VpContext.Rip + i);
    }
    io.InstructionByteCount = DecodeIoInstructionLength(io);
  }
  if (WhpTraceEnabled()) {
    std::fprintf(
        stderr,
        " len=%u bytes=%02x %02x %02x %02x\n",
        io.InstructionByteCount,
        io.InstructionBytes[0],
        io.InstructionBytes[1],
        io.InstructionBytes[2],
        io.InstructionBytes[3]);
  }
  return io;
}

void HandleWhpIo(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    uint64_t rip,
    const WHV_X64_IO_PORT_ACCESS_CONTEXT& io,
    Uart& uart,
    GuestExit& guest_exit,
    Cmos* cmos,
    AcpiPmTimer* pm_timer) {
  Check(io.AccessInfo.AccessSize == 1 || io.AccessInfo.AccessSize == 2 || io.AccessInfo.AccessSize == 4, "unsupported I/O access size");
  Check(!io.AccessInfo.StringOp && !io.AccessInfo.RepPrefix, "string I/O is not supported yet");
  Check(io.InstructionByteCount != 0, "WHP I/O exit did not include a decodable instruction length");
  uint16_t port = io.PortNumber;
  bool write = io.AccessInfo.IsWrite != 0;
  uint64_t rax = GetRegister64(api, partition, vp_index, WHvX64RegisterRax);
  uint32_t mask = io.AccessInfo.AccessSize == 1 ? 0xFFU : io.AccessInfo.AccessSize == 2 ? 0xFFFFU : 0xFFFFFFFFU;
  uint32_t data = static_cast<uint32_t>(rax) & mask;

  if (port == kNodeVmmExitPort && io.AccessInfo.AccessSize == 1) {
    if (write) {
      guest_exit.requested = true;
      guest_exit.status = data & 0xFF;
    } else {
      rax = (rax & ~uint64_t(mask)) | 0;
    }
  } else if (port == kNodeVmmConsolePort && io.AccessInfo.AccessSize == 1) {
    if (write) {
      uint8_t byte = static_cast<uint8_t>(data & 0xFF);
      uart.emit_bytes(&byte, 1);
    } else {
      rax = (rax & ~uint64_t(mask)) | 0;
    }
  } else if (port >= kCom1Base && port < kCom1Base + 8 && io.AccessInfo.AccessSize == 1) {
    uint16_t off = static_cast<uint16_t>(port - kCom1Base);
    if (write) {
      uart.write(off, static_cast<uint8_t>(data & 0xFF));
    } else {
      rax = (rax & ~uint64_t(mask)) | uart.read(off);
    }
  } else if (port == 0x61 && io.AccessInfo.AccessSize == 1) {
    // Fallback path matches the emulator-side handler above.
    if (write) {
      // No PIT pointer here; the value still settles via the emulator path.
    } else {
      rax = (rax & ~uint64_t(mask)) | 0;
    }
  } else if ((port == 0x70 || port == 0x71) && io.AccessInfo.AccessSize == 1 && cmos != nullptr) {
    if (write) {
      cmos->write_port(port, static_cast<uint8_t>(data & 0xFF));
    } else {
      rax = (rax & ~uint64_t(mask)) | cmos->read_port(port);
    }
  } else if (port >= kAcpiPmTimerPort && port < kAcpiPmTimerPort + 4 &&
             io.AccessInfo.AccessSize <= 4 && pm_timer != nullptr) {
    if (!write) {
      rax = (rax & ~uint64_t(mask)) |
            (pm_timer->read(port, static_cast<uint8_t>(io.AccessInfo.AccessSize)) & mask);
    }
  } else if (!write) {
    rax = (rax & ~uint64_t(mask)) | 0;
  }

  uint64_t next_rip = rip + io.InstructionByteCount;
  if (write) {
    SetRip(api, partition, vp_index, next_rip);
  } else {
    SetRaxRip(api, partition, vp_index, rax, next_rip);
  }
}

void HandleWhpCpuid(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    const WHV_RUN_VP_EXIT_CONTEXT& exit_context,
    uint32_t cpus) {
  const auto& cpuid = exit_context.CpuidAccess;
  uint32_t leaf = static_cast<uint32_t>(cpuid.Rax);
  uint64_t rax = uint32_t(cpuid.DefaultResultRax);
  uint64_t rbx = uint32_t(cpuid.DefaultResultRbx);
  uint64_t rcx = uint32_t(cpuid.DefaultResultRcx);
  uint64_t rdx = uint32_t(cpuid.DefaultResultRdx);

  switch (leaf) {
    case 0x00000001:
      rcx |= 1U << 31;   // Hypervisor present.
      rcx &= ~(1U << 24);  // Hide TSC deadline until we emulate it.
      break;
    case 0x00000006:
      rax &= ~(1U << 2);  // Hide ARAT; WHP's LAPIC timer frequency comes via Hyper-V MSR.
      break;
    case 0x80000007:
      rdx |= 1U << 8;  // Invariant TSC: keep Linux off the ACPI PM timer clocksource.
      break;
    case kHvCpuidVendorAndMax:
      rax = kHvCpuidImplementationLimits;
      rbx = 0x7263694D;  // "Micr"
      rcx = 0x666F736F;  // "osof"
      rdx = 0x76482074;  // "t Hv"
      break;
    case kHvCpuidInterface:
      rax = 0x31237648;  // "Hv#1"
      rbx = rcx = rdx = 0;
      break;
    case kHvCpuidVersion:
      rax = 0x00001DB1;  // Build number, informational only.
      rbx = 0x000A0000;
      rcx = rdx = 0;
      break;
    case kHvCpuidFeatures:
      rax = (1U << 5) | (1U << 6) | (1U << 11);  // Hypercall, VP index, frequency MSRs.
      rbx = 0;
      rcx = 0;
      rdx = 1U << 8;  // Frequency MSRs available.
      break;
    case kHvCpuidEnlightenmentInfo:
      rax = rbx = rcx = rdx = 0;
      break;
    case kHvCpuidImplementationLimits:
      rax = std::max<uint32_t>(1, cpus);
      rbx = 0;
      rcx = 0;
      rdx = 0;
      break;
    default:
      break;
  }

  uint64_t next_rip = exit_context.VpContext.Rip + exit_context.VpContext.InstructionLength;
  WHV_REGISTER_NAME names[] = {
      WHvX64RegisterRax,
      WHvX64RegisterRbx,
      WHvX64RegisterRcx,
      WHvX64RegisterRdx,
      WHvX64RegisterRip,
  };
  WHV_REGISTER_VALUE values[5]{};
  values[0].Reg64 = rax;
  values[1].Reg64 = rbx;
  values[2].Reg64 = rcx;
  values[3].Reg64 = rdx;
  values[4].Reg64 = next_rip;
  CheckHr(api.set_vp_registers(partition, vp_index, names, 5, values), "WHvSetVirtualProcessorRegisters(CPUID)");
}

void HandleWhpMsr(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    const WHV_RUN_VP_EXIT_CONTEXT& exit_context,
    HyperVState& hyperv) {
  const auto& msr = exit_context.MsrAccess;
  uint64_t next_rip = exit_context.VpContext.Rip + exit_context.VpContext.InstructionLength;
  uint64_t write_value = (msr.Rax & 0xFFFFFFFFULL) | ((msr.Rdx & 0xFFFFFFFFULL) << 32);
  if (msr.AccessInfo.IsWrite) {
    switch (msr.MsrNumber) {
      case kHvMsrGuestOsId:
        hyperv.guest_os_id.store(write_value);
        break;
      case kHvMsrHypercall:
        hyperv.hypercall.store(write_value);
        break;
      default:
        break;
    }
    SetRip(api, partition, vp_index, next_rip);
    return;
  }

  uint64_t value = 0;
  switch (msr.MsrNumber) {
    case kHvMsrGuestOsId:
      value = hyperv.guest_os_id.load();
      break;
    case kHvMsrHypercall:
      value = hyperv.hypercall.load();
      break;
    case kHvMsrVpIndex:
      value = vp_index;
      break;
    case kHvMsrTimeRefCount:
      value = hyperv.time_ref_count();
      break;
    case kHvMsrTscFrequency:
      value = hyperv.tsc_frequency_hz;
      break;
    case kHvMsrApicFrequency:
      value = hyperv.apic_frequency_hz;
      break;
    default:
      value = 0;
      break;
  }
  SetRaxRip(api, partition, vp_index, value & 0xFFFFFFFFULL, next_rip);
  WHV_REGISTER_NAME name = WHvX64RegisterRdx;
  WHV_REGISTER_VALUE reg{};
  reg.Reg64 = value >> 32;
  CheckHr(api.set_vp_registers(partition, vp_index, &name, 1, &reg), "WHvSetVirtualProcessorRegisters(MSR RDX)");
}

struct WhpEmulator {
  WhpApi& api;
  WHV_EMULATOR_HANDLE handle{nullptr};

  explicit WhpEmulator(WhpApi& api_ref) : api(api_ref) {
    WHV_EMULATOR_CALLBACKS callbacks{};
    callbacks.Size = sizeof(callbacks);
    callbacks.WHvEmulatorIoPortCallback = EmulatorIoPort;
    callbacks.WHvEmulatorMemoryCallback = EmulatorMemory;
    callbacks.WHvEmulatorGetVirtualProcessorRegisters = EmulatorGetRegisters;
    callbacks.WHvEmulatorSetVirtualProcessorRegisters = EmulatorSetRegisters;
    callbacks.WHvEmulatorTranslateGvaPage = EmulatorTranslateGva;
    CheckHr(api.emulator_create(&callbacks, &handle), "WHvEmulatorCreateEmulator");
  }

  WhpEmulator(const WhpEmulator&) = delete;
  WhpEmulator& operator=(const WhpEmulator&) = delete;

  ~WhpEmulator() {
    if (handle) {
      api.emulator_destroy(handle);
    }
  }
};

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

napi_value RunVm(napi_env env, napi_callback_info info) {
  const auto t_run_enter = std::chrono::steady_clock::now();
  bool boot_trace = false;
  {
    char v[8] = {0};
    DWORD n = GetEnvironmentVariableA("NODE_VMM_BOOT_TIME", v, sizeof(v));
    boot_trace = (n > 0 && v[0] == '1');
  }
  auto trace_phase = [&](const char* name) {
    if (!boot_trace) return;
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - t_run_enter).count();
    std::fprintf(stderr, "[node-vmm boot-time] %s @ %lld us\n", name, (long long)us);
  };
  trace_phase("run_enter");
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
      throw std::runtime_error("runVm requires a config object");
    }
    std::string kernel_path = GetString(env, argv[0], "kernelPath");
    std::string rootfs_path = GetString(env, argv[0], "rootfsPath");
    std::string overlay_path = GetString(env, argv[0], "overlayPath");
    std::string cmdline = GetString(env, argv[0], "cmdline");
    uint32_t mem_mib = GetUint32(env, argv[0], "memMiB", 256);
    uint32_t cpus = GetUint32(env, argv[0], "cpus", 1);
    uint32_t timeout_ms = GetUint32(env, argv[0], "timeoutMs", 60000);
    uint32_t console_limit = GetUint32(env, argv[0], "consoleLimit", 1024 * 1024);
    bool interactive = GetBool(env, argv[0], "interactive", false);
    std::string tap_name = GetString(env, argv[0], "netTapName");
    bool slirp_enabled = GetBool(env, argv[0], "netSlirpEnabled", false);
    std::vector<AttachedDiskConfig> attached_disks = GetAttachedDisks(env, argv[0]);
#ifdef NODE_VMM_HAVE_LIBSLIRP
    std::vector<Slirp::HostFwd> slirp_host_fwds;
    {
      napi_value fwds_value;
      bool has_fwds = false;
      napi_has_named_property(env, argv[0], "netSlirpHostFwds", &has_fwds);
      if (has_fwds) {
        napi_get_named_property(env, argv[0], "netSlirpHostFwds", &fwds_value);
        bool is_array = false;
        napi_is_array(env, fwds_value, &is_array);
        if (is_array) {
          uint32_t length = 0;
          napi_get_array_length(env, fwds_value, &length);
          slirp_host_fwds.reserve(length);
          for (uint32_t i = 0; i < length; i++) {
            napi_value entry;
            napi_get_element(env, fwds_value, i, &entry);
            Slirp::HostFwd fwd{};
            fwd.udp = GetBool(env, entry, "udp", false);
            // hostAddr is "127.0.0.1" / "0.0.0.0" / "..." -> map to uint32 host order.
            std::string host_addr = GetString(env, entry, "hostAddr");
            if (host_addr.empty() || host_addr == "0.0.0.0") {
              fwd.host_ip = 0;  // INADDR_ANY (slirp listens on all host ifaces)
            } else {
              in_addr addr{};
              if (inet_pton(AF_INET, host_addr.c_str(), &addr) == 1) {
                fwd.host_ip = ntohl(addr.s_addr);
              }
            }
            fwd.host_port = static_cast<uint16_t>(GetUint32(env, entry, "hostPort", 0));
            fwd.guest_port = static_cast<uint16_t>(GetUint32(env, entry, "guestPort", 0));
            if (fwd.host_port != 0 && fwd.guest_port != 0) {
              slirp_host_fwds.push_back(fwd);
            }
          }
        }
      }
    }
#endif
    RunControl control = GetRunControl(env, argv[0]);
    control.set_state(kControlStateStarting);
    Check(attached_disks.size() < kMaxIoApicPins, "too many attached disks");
    uint32_t disk_count = static_cast<uint32_t>(attached_disks.size() + 1);
    uint32_t net_index = disk_count;
    uint32_t rng_index = disk_count + 1;
    CheckVirtioMmioDeviceCount(rng_index + 1);

    Check(!kernel_path.empty(), "kernelPath is required");
    Check(!rootfs_path.empty(), "rootfsPath is required");
    Check(!cmdline.empty(), "cmdline is required");
    Check(mem_mib > 0, "memMiB must be greater than zero");
    Check(cpus >= 1 && cpus <= kMaxVcpus, "cpus must be between 1 and 64");
    Check(tap_name.empty(),
          "Windows WHP backend currently supports --net none and --net slirp only");
#ifndef NODE_VMM_HAVE_LIBSLIRP
    Check(!slirp_enabled,
          "Windows WHP slirp networking requires libslirp; rebuild with vcpkg libslirp installed (see docs/windows.md)");
#endif
    Check(cmdline.size() + 1 <= kKernelCmdlineMax, "kernel cmdline is too long");

    uint64_t rootfs_bytes = FileSizeBytes(rootfs_path);
    bool rootfs_backed = rootfs_bytes > 0;

    WhpApi api(true);
    HyperVState hyperv;
    hyperv.tsc_frequency_hz = QueryWhpFrequency(
        api,
        WHvCapabilityCodeProcessorClockFrequency,
        kFallbackTscFrequencyHz,
        &WHV_CAPABILITY::ProcessorClockFrequency);
    hyperv.apic_frequency_hz = QueryWhpFrequency(
        api,
        WHvCapabilityCodeInterruptClockFrequency,
        kFallbackApicFrequencyHz,
        &WHV_CAPABILITY::InterruptClockFrequency);
    trace_phase("api_loaded");
    Partition partition(api);
    trace_phase("partition_created");
    if (rootfs_backed) {
      WHV_EXTENDED_VM_EXITS exits{};
      exits.X64CpuidExit = 1;
      exits.X64MsrExit = 1;
      CheckHr(
          api.set_partition_property(
              partition.handle,
              WHvPartitionPropertyCodeExtendedVmExits,
              &exits,
              sizeof(exits)),
          "WHvSetPartitionProperty(ExtendedVmExits)");
    }
    if (rootfs_backed) {
      WHV_X64_LOCAL_APIC_EMULATION_MODE apic_property = WHvX64LocalApicEmulationModeX2Apic;
      CheckHr(
          api.set_partition_property(
              partition.handle,
              WHvPartitionPropertyCodeLocalApicEmulationMode,
              &apic_property,
              sizeof(apic_property)),
          "WHvSetPartitionProperty(LocalApicEmulationMode)");
    }
    // Hyper-V synthetic processor features. Mirrors qemu/target/i386/whpx/
    // whpx-all.c:2313-2341 -- without this the guest sees no enlightenments
    // (HypervisorPresent CPUID bit clear, no SynIC, no synthetic timers,
    // no AccessVpRunTimeReg). Linux's "tsc=reliable" fallback works either
    // way but the kernel logs benign noise like "TSC doesn't count with P0
    // frequency" because we never advertise the canonical Hyper-V interface.
    // Best-effort: older Windows builds reject some banks, so we try the
    // full set first, then back off to the minimal "just say HypervisorPresent"
    // bank if the call fails. We swallow non-success here -- a partition
    // without enlightenments still boots, just with worse performance hints.
    if (rootfs_backed) {
      WHV_PARTITION_PROPERTY synth{};
      synth.SyntheticProcessorFeaturesBanks.BanksCount = 1;
      // Bit layout per WinHvPlatformDefs.h SyntheticProcessorFeatures1:
      //   bit  0 HypervisorPresent
      //   bit  1 Hv1
      //   bit  2 AccessVpRunTimeReg
      //   bit  3 AccessPartitionReferenceCounter
      //   bit  4 AccessSynicRegs
      //   bit  5 AccessSyntheticTimerRegs
      //   bit  6 AccessIntrCtrlRegs
      //   bit  7 AccessHypercallMsrs
      //   bit  8 AccessVpIndex
      //   bit 19 TbFlushHypercalls
      //   bit 28 SignalEvents
      synth.SyntheticProcessorFeaturesBanks.AsUINT64[0] =
          (1ULL << 0)  | (1ULL << 1)  | (1ULL << 2)  | (1ULL << 3)  |
          (1ULL << 7)  | (1ULL << 8)  | (1ULL << 19);
      HRESULT hr = api.set_partition_property(
          partition.handle,
          WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks,
          &synth,
          sizeof(synth));
      if (FAILED(hr)) {
        // Older Windows: try the absolute minimum (HypervisorPresent + Hv1).
        synth.SyntheticProcessorFeaturesBanks.AsUINT64[0] = (1ULL << 0) | (1ULL << 1);
        (void)api.set_partition_property(
            partition.handle,
            WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks,
            &synth,
            sizeof(synth));
      }
    }
    WHV_PARTITION_PROPERTY property{};
    property.ProcessorCount = cpus;
    CheckHr(
        api.set_partition_property(partition.handle, WHvPartitionPropertyCodeProcessorCount, &property, sizeof(property)),
        "WHvSetPartitionProperty(ProcessorCount)");
    CheckHr(api.setup_partition(partition.handle), "WHvSetupPartition");
    trace_phase("partition_setup");

    uint64_t ram_bytes = uint64_t(mem_mib) * 1024ULL * 1024ULL;
    Check(ram_bytes <= uint64_t(1) << 32, "invalid guest memory size");
    VirtualAllocMemory ram(static_cast<size_t>(ram_bytes));
    uint8_t* bytes = ram.bytes();
    GuestMemory guest{bytes, ram_bytes};
    trace_phase("ram_allocated");
    KernelInfo kernel = LoadElfKernel(bytes, ram_bytes, kernel_path);
    trace_phase("kernel_loaded");
    uint64_t rsdp_addr =
        CreateAcpiTables(bytes, ram_bytes, slirp_enabled, static_cast<int>(cpus), disk_count);
    WriteBootParams(bytes, ram_bytes, cmdline);
    WriteU64(guest.ptr(kBootParamsAddr + 0x70, 8), rsdp_addr);
    WriteMpTable(bytes, ram_bytes, static_cast<int>(cpus));
    trace_phase("acpi_built");

    WHV_MAP_GPA_RANGE_FLAGS flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(0x00000001 | 0x00000002 | 0x00000004);
    CheckHr(api.map_gpa_range(partition.handle, ram.ptr, 0, ram_bytes, flags), "WHvMapGpaRange");
    trace_phase("ram_mapped");
    std::vector<std::unique_ptr<VirtualProcessor>> vcpus;
    vcpus.reserve(cpus);
    for (uint32_t i = 0; i < cpus; i++) {
      vcpus.push_back(std::make_unique<VirtualProcessor>(api, partition.handle, i));
    }
    for (uint32_t i = 0; i < cpus; i++) {
      // For rootfs-backed Linux guests with multiple CPUs, only the BSP boots
      // straight into long mode at kernel.entry. APs are left parked in WHP's
      // default reset state so the X2APIC INIT/SIPI handshake can launch them
      // through the kernel's real-mode trampoline.
      bool is_rootfs_ap = rootfs_backed && i != 0;
      SetupWhpBootstrapVcpu(
          api, partition.handle, i, bytes, kernel.entry, rootfs_backed, is_rootfs_ap);
    }

    // Pin the Windows scheduler to 1ms resolution so std::this_thread::sleep_for
    // and WaitForSingleObject use 1ms granularity instead of the default 15.6ms.
    // Without this, the PIT timer thread can only deliver ~67 Hz at best, which
    // is below the HZ=100 / HZ=250 cadence Linux assumes during boot.
    struct HighResolutionTimerScope {
      HighResolutionTimerScope() { timeBeginPeriod(1); }
      ~HighResolutionTimerScope() { timeEndPeriod(1); }
    } high_res_timer;

    std::mutex run_wake_mu;
    std::condition_variable run_wake_cv;
    auto wake_halted_vcpus = [&]() {
      run_wake_cv.notify_all();
    };

    IoApic ioapic(api, partition.handle);
    Pic pic(api, partition.handle);
    Pit pit;
    Hpet hpet;
    hpet.attach_irq_line([&](uint32_t ioapic_pin, bool level) {
      if (ioapic_pin < 24) {
        ioapic.set_irq(ioapic_pin, level);
        wake_halted_vcpus();
      }
    });
    Cmos cmos;
    AcpiPmTimer pm_timer;
    std::vector<std::unique_ptr<VirtioBlk>> blks;
    blks.reserve(disk_count);
    blks.push_back(std::make_unique<VirtioBlk>(
        VirtioMmioBase(0),
        guest,
        rootfs_path,
        overlay_path,
        false,
        [&] { ioapic.request_irq(VirtioMmioIrq(0)); }));
    for (uint32_t i = 0; i < attached_disks.size(); i++) {
      const AttachedDiskConfig& disk = attached_disks[i];
      uint32_t index = i + 1;
      blks.push_back(std::make_unique<VirtioBlk>(
          VirtioMmioBase(index),
          guest,
          disk.path,
          std::string(),
          disk.read_only,
          [&, index] { ioapic.request_irq(VirtioMmioIrq(index)); }));
    }
    std::vector<VirtioBlk*> blk_ptrs;
    blk_ptrs.reserve(blks.size());
    for (const auto& blk : blks) {
      blk_ptrs.push_back(blk.get());
    }
    VirtioRng rng(VirtioMmioBase(rng_index), guest,
                  [&, rng_index] { ioapic.request_irq(VirtioMmioIrq(rng_index)); });
    Uart uart(console_limit, interactive);
    // Default IRQ4 raiser routes through the IOAPIC. Replaced below
    // (after `pic` exists) with the IOAPIC-then-PIC fallback router.
    uart.attach_irq_raiser([&](uint32_t irq) { ioapic.request_irq(irq); });
    if (boot_trace) {
      uart.enable_rx_debug();
      ioapic.enable_debug();
    }
    GuestExit guest_exit;
    WhpEmulator emulator(api);

#ifdef NODE_VMM_HAVE_LIBSLIRP
    std::unique_ptr<VirtioNet> virtio_net;
    std::unique_ptr<Slirp> slirp_runtime;
    if (slirp_enabled && rootfs_backed) {
      // Locally-administered MAC. Bit 1 of the first octet must be set so
      // the OUI doesn't collide with a registered manufacturer.
      std::array<uint8_t, 6> mac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
      virtio_net = std::make_unique<VirtioNet>(
          ioapic, guest, VirtioMmioBase(net_index), VirtioMmioIrq(net_index), mac);
      slirp_runtime = std::make_unique<Slirp>(virtio_net.get(), std::move(slirp_host_fwds));
      virtio_net->attach_slirp(slirp_runtime.get());
    }
#else
    (void)slirp_enabled;
    (void)net_index;
#endif

    std::vector<EmulatorContext> emulator_contexts;
    emulator_contexts.reserve(cpus);
    for (uint32_t i = 0; i < cpus; i++) {
      EmulatorContext ctx{};
      ctx.api = &api;
      ctx.partition = partition.handle;
      ctx.vp_index = i;
      ctx.mem = guest;
      ctx.uart = &uart;
      ctx.guest_exit = &guest_exit;
      ctx.pic = &pic;
      ctx.pit = &pit;
      ctx.hpet = &hpet;
      ctx.pm_timer = &pm_timer;
      ctx.ioapic = &ioapic;
      ctx.blks = blk_ptrs;
      ctx.rng = &rng;
      ctx.rng_base = VirtioMmioBase(rng_index);
      ctx.cmos = &cmos;
#ifdef NODE_VMM_HAVE_LIBSLIRP
      ctx.net = virtio_net.get();
#endif
      emulator_contexts.push_back(ctx);
    }

    // Per-vCPU IRQ delivery state. Only vCPU0 receives ExtInts in the current
    // single-CPU rootfs path; the others get an entry for completeness so the
    // run loop can index by cpu_index without bounds checks.
    std::vector<std::unique_ptr<WhpVcpuIrqState>> vcpu_irq;
    vcpu_irq.reserve(cpus);
    for (uint32_t i = 0; i < cpus; i++) {
      auto state = std::make_unique<WhpVcpuIrqState>();
      state->index = i;
      vcpu_irq.push_back(std::move(state));
    }

    std::mutex device_mu;
    std::mutex result_mu;
    std::atomic<bool> vm_done{false};
    std::atomic<bool> watchdog_done{false};
    std::atomic<bool> watchdog_timeout{false};
    std::atomic<bool> timer_done{false};
    std::atomic<uint64_t> runs{0};
    std::atomic<uint32_t> paused_count{0};
    std::atomic<uint32_t> halted_count{0};
    std::atomic<int64_t> timeout_extension_ms{0};
    auto start = std::chrono::steady_clock::now();
    std::string final_exit_reason = "host-stop";
    std::string error_message;
    uint32_t final_exit_code = WHvRunVpExitReasonCanceled;
    auto cancel_all = [&]() {
      for (uint32_t i = 0; i < cpus; i++) {
        (void)api.cancel_run_vp(partition.handle, i, 0);
      }
    };
    // WHvRequestInterrupt is enough for fixed IOAPIC interrupts. The waker
    // only nudges vCPU loops that are parked outside WHvRunVirtualProcessor
    // after a HLT exit; it deliberately does not cancel every VP.
    ioapic.attach_waker(wake_halted_vcpus);
    auto finish = [&](const std::string& reason, uint32_t code) {
      bool expected = false;
      if (vm_done.compare_exchange_strong(expected, true)) {
        {
          std::lock_guard<std::mutex> lock(result_mu);
          final_exit_reason = reason;
          final_exit_code = code;
        }
        if (boot_trace) {
          auto now = std::chrono::steady_clock::now();
          auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - t_run_enter).count();
          std::fprintf(stderr, "[node-vmm boot-time] finish:%s @ %lld us\n", reason.c_str(), (long long)us);
        }
        cancel_all();
        wake_halted_vcpus();
      }
    };
    auto record_error = [&](const std::string& message) {
      std::string detail = message;
      if (detail.empty()) {
        detail = watchdog_timeout.load()
            ? "VM timed out after " + std::to_string(timeout_ms) + "ms"
            : "unknown WHP vCPU runner error";
      }
      {
        std::lock_guard<std::mutex> lock(result_mu);
        if (error_message.empty()) {
          error_message = detail;
        }
      }
      finish("host-error", WHvRunVpExitReasonCanceled);
    };

    // Interactive stdin pump: when the user passed --interactive, switch the
    // host console to virtual-terminal/raw mode so that keystrokes flow
    // through verbatim and pipe them into the guest UART RX queue. Mirrors
    // the POSIX path in native/kvm_backend.cc:3074-3130 - same Uart::
    // enqueue_rx, same Ctrl-C trap routed to the guest instead of killing
    // the host VM.
    std::atomic<bool> input_done{false};
    HANDLE stdin_handle = interactive ? GetStdHandle(STD_INPUT_HANDLE) : nullptr;
    DWORD original_in_mode = 0;
    DWORD original_out_mode = 0;
    bool captured_in_mode = false;
    bool captured_out_mode = false;
    HANDLE stdout_handle = interactive ? GetStdHandle(STD_OUTPUT_HANDLE) : nullptr;
    if (interactive && stdin_handle != INVALID_HANDLE_VALUE && stdin_handle != nullptr) {
      if (GetConsoleMode(stdin_handle, &original_in_mode)) {
        captured_in_mode = true;
        DWORD raw_in =
            (original_in_mode &
             ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) |
            ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(stdin_handle, raw_in);
      }
    }
    if (interactive && stdout_handle != INVALID_HANDLE_VALUE && stdout_handle != nullptr) {
      if (GetConsoleMode(stdout_handle, &original_out_mode)) {
        captured_out_mode = true;
        SetConsoleMode(stdout_handle,
                       original_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      }
    }
    auto restore_console = [&]() {
      if (interactive && stdin_handle != nullptr && stdin_handle != INVALID_HANDLE_VALUE && captured_in_mode) {
        SetConsoleMode(stdin_handle, original_in_mode);
      }
      if (interactive && stdout_handle != nullptr && stdout_handle != INVALID_HANDLE_VALUE && captured_out_mode) {
        SetConsoleMode(stdout_handle, original_out_mode);
      }
    };

    // Decide once whether stdin is a real console so the Ctrl handler only
    // attaches for terminals. Input bytes always flow through ReadFile: with
    // ENABLE_VIRTUAL_TERMINAL_INPUT set, Windows emits escape sequences for
    // arrows/function keys instead of KEY_EVENT records with UnicodeChar == 0.
    bool stdin_is_console = false;
    if (interactive && stdin_handle != INVALID_HANDLE_VALUE && stdin_handle != nullptr) {
      DWORD probe_mode = 0;
      stdin_is_console = GetConsoleMode(stdin_handle, &probe_mode) != 0;
    }
    // Per agent #1 finding: when the input thread enqueues bytes into the
    // UART RX queue, it must (a) take device_mu so its IRQ raise is
    // serialized with vCPU-side device accesses (otherwise update_interrupt
    // races with the guest reading IIR/LSR), and (b) call cancel_all() to
    // kick the BSP vCPU out of HLT/wait so it actually traps to read the
    // freshly delivered IRQ4. Without (b), bytes sit in the UART RX FIFO, IRQ4 fires
    // through ioapic.request_irq -> WHvRequestInterrupt, but if the vCPU is
    // halted in WHvRunVirtualProcessor at HLT it doesn't take the interrupt
    // until the next external wake -- which never comes from a foreground
    // shell that's already idle. Mirrors kvm_backend.cc:3074-3130.
    auto deliver_uart_input = [&](const uint8_t* buf, size_t n) {
      size_t delivered = 0;
      while (delivered < n && !input_done.load()) {
        size_t accepted = 0;
        {
          std::lock_guard<std::mutex> lock(device_mu);
          accepted = uart.enqueue_rx(buf + delivered, n - delivered);
        }
        if (accepted == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          cancel_all();
          wake_halted_vcpus();
          continue;
        }
        delivered += accepted;
        if (boot_trace) {
          std::fprintf(stderr, "[node-vmm input] enqueue_rx %zu/%zu bytes -> cancel_all\n", delivered, n);
        }
        cancel_all();
        wake_halted_vcpus();
      }
    };

    constexpr size_t kHostInputQueueLimit = 1024 * 1024;
    constexpr size_t kHostInputPumpChunk = 256;
    std::mutex host_input_mu;
    std::condition_variable host_input_cv;
    std::deque<uint8_t> host_input_queue;
    auto queue_host_input = [&](const uint8_t* buf, size_t n, bool front = false) {
      if (buf == nullptr || n == 0) {
        return;
      }
      std::unique_lock<std::mutex> lock(host_input_mu);
      if (front) {
        while (host_input_queue.size() + n > kHostInputQueueLimit && !host_input_queue.empty()) {
          host_input_queue.pop_back();
        }
        for (size_t i = n; i > 0; i--) {
          host_input_queue.push_front(buf[i - 1]);
        }
      } else {
        host_input_cv.wait(lock, [&]() {
          return input_done.load() || host_input_queue.size() + n <= kHostInputQueueLimit;
        });
        if (input_done.load()) {
          return;
        }
        for (size_t i = 0; i < n; i++) {
          host_input_queue.push_back(buf[i]);
        }
      }
      lock.unlock();
      host_input_cv.notify_all();
    };
    node_vmm::whp::ScopedConsoleCtrlHandler console_ctrl_handler(interactive && stdin_is_console, [&]() {
      const uint8_t intr = 0x03;
      queue_host_input(&intr, 1, true);
    });
    // Wire COM1 IRQ4 routing now that ioapic, pic, and vcpu_irq all exist.
    // Mirrors the IRQ0/PIT pattern at native/whp_backend.cc:4451: try the
    // IOAPIC if its RTE is unmasked; otherwise fall back to legacy 8259A
    // ExtInt + cancel_all() so the BSP wakes from HLT and notices the
    // pending interrupt. KVM's KVM_IRQ_LINE picks the same way internally.
    uart.attach_irq_raiser([&](uint32_t irq) {
      if (ioapic.irq_unmasked(irq)) {
        ioapic.request_irq(irq);
        return;
      }
      if (!pic.is_initialized() || !pic.irq_unmasked(static_cast<uint8_t>(irq))) {
        return;
      }
      WhpVcpuIrqState& bsp = *vcpu_irq[0];
      bsp.ext_int_vector.store(pic.vector_for_irq(irq));
      bsp.ext_int_pending.store(true);
      cancel_all();
      wake_halted_vcpus();
    });
    std::thread input_thread;
    std::thread input_pump_thread;
    if (interactive && stdin_handle != INVALID_HANDLE_VALUE && stdin_handle != nullptr) {
      input_pump_thread = std::thread([&]() {
        std::vector<uint8_t> chunk;
        chunk.reserve(kHostInputPumpChunk);
        for (;;) {
          chunk.clear();
          {
            std::unique_lock<std::mutex> lock(host_input_mu);
            host_input_cv.wait(lock, [&]() { return input_done.load() || !host_input_queue.empty(); });
            if (input_done.load()) {
              break;
            }
            size_t take = std::min(kHostInputPumpChunk, host_input_queue.size());
            for (size_t i = 0; i < take; i++) {
              chunk.push_back(host_input_queue.front());
              host_input_queue.pop_front();
            }
          }
          host_input_cv.notify_all();
          if (!chunk.empty()) {
            deliver_uart_input(chunk.data(), chunk.size());
          }
        }
      });
      input_thread = std::thread([&]() {
        auto deliver_console_bytes = [&](const uint8_t* data, size_t len) {
          std::vector<uint8_t> normalized;
          normalized.reserve(len);
          for (size_t i = 0; i < len; i++) {
            if (data[i] == '\r') {
              if (i + 1 < len && data[i + 1] == '\n') {
                normalized.push_back('\n');
                i++;
              } else {
                normalized.push_back('\r');
              }
              continue;
            }
            normalized.push_back(data[i]);
          }
          if (!normalized.empty()) {
            queue_host_input(normalized.data(), normalized.size());
          }
        };
        if (stdin_is_console) {
          uint8_t buf[4096];
          while (!input_done.load()) {
            DWORD n = 0;
            BOOL ok = ReadFile(stdin_handle, buf, sizeof(buf), &n, nullptr);
            if (!ok || n == 0) {
              if (input_done.load()) {
                break;
              }
              if (boot_trace) {
                std::fprintf(stderr, "[node-vmm input] ReadFile done ok=%d n=%lu err=%lu console=%d\n",
                             ok, n, ok ? 0 : GetLastError(), 1);
              }
              break;
            }
            if (boot_trace) {
              std::fprintf(stderr, "[node-vmm input] read %lu bytes console=1\n", n);
            }
            deliver_console_bytes(buf, static_cast<size_t>(n));
          }
        } else {
          uint8_t byte = 0;
          while (!input_done.load()) {
            DWORD n = 0;
            BOOL ok = ReadFile(stdin_handle, &byte, 1, &n, nullptr);
            if (!ok || n == 0) {
              if (input_done.load()) {
                break;
              }
              if (boot_trace) {
                std::fprintf(stderr, "[node-vmm input] ReadFile done ok=%d n=%lu err=%lu console=0\n",
                             ok, n, ok ? 0 : GetLastError());
              }
              break;
            }
            if (byte == '\r') {
              continue;
            }
            queue_host_input(&byte, 1);
          }
        }
      });
    }
    auto stop_input = [&]() {
      input_done = true;
      if (interactive && stdin_handle != INVALID_HANDLE_VALUE && stdin_handle != nullptr) {
        CancelIoEx(stdin_handle, nullptr);
      }
      if (input_thread.joinable()) {
        CancelSynchronousIo(input_thread.native_handle());
      }
      if (input_thread.joinable()) {
        input_thread.join();
      }
      host_input_cv.notify_all();
      if (input_pump_thread.joinable()) {
        input_pump_thread.join();
      }
      restore_console();
    };

    std::thread watchdog([&]() {
      while (!watchdog_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int32_t command = control.command();
        if (command == kControlStop) {
          control.set_state(kControlStateStopping);
          finish("host-stop", WHvRunVpExitReasonCanceled);
          break;
        }
        if (command == kControlPause) {
          cancel_all();
          wake_halted_vcpus();
          continue;
        }
        auto deadline =
            start + std::chrono::milliseconds(timeout_ms) + std::chrono::milliseconds(timeout_extension_ms.load());
        if (timeout_ms > 0 && std::chrono::steady_clock::now() > deadline) {
          watchdog_timeout = true;
          cancel_all();
          wake_halted_vcpus();
          break;
        }
      }
    });
    auto stop_watchdog = [&]() {
      watchdog_done = true;
      if (watchdog.joinable()) {
        watchdog.join();
      }
    };
    auto deliver_timer_irq = [&](WhpVcpuIrqState& bsp, uint32_t ioapic_pin, uint8_t pic_irq, bool level, const char* source) {
      if (ioapic_pin < 24 && ioapic.irq_unmasked(ioapic_pin)) {
        if (level) {
          ioapic.set_irq(ioapic_pin, true);
        } else {
          ioapic.request_irq(ioapic_pin);
        }
        return;
      }
      if (pic_irq < 8 && pic.is_initialized() && pic.irq_unmasked(pic_irq)) {
        bsp.ext_int_vector.store(pic.vector_for_irq(pic_irq));
        bsp.ext_int_pending.store(true);
        if (boot_trace) {
          static std::atomic<uint64_t> timer_extints{0};
          uint64_t n = ++timer_extints;
          if (n <= 40 || n % 100 == 0) {
            std::fprintf(stderr, "[node-vmm %s] extint irq%u vector=0x%02x tick=%llu\n",
                         source,
                         pic_irq,
                         pic.vector_for_irq(pic_irq) & 0xFF,
                         (unsigned long long)n);
          }
        }
        cancel_all();
        wake_halted_vcpus();
      }
    };
    // PIT timer thread: ~1 kHz polling cadence so HZ=100 / HZ=250 Linux kernels
    // see steady jiffies progress. Routing depends on the kernel boot phase:
    //   * Before setup_IO_APIC, IRQ0 is delivered through the legacy 8259A
    //     PIC and the kernel expects an ExtInt event - we deliver it via
    //     WHvRegisterPendingEvent.ExtIntEvent gated by the QEMU-style
    //     InterruptWindow dance in TryDeliverPendingExtInt.
    //   * Once setup_IO_APIC unmasks IRQ0 in the IOAPIC redirection entry,
    //     the kernel routes through the LAPIC and expects a fixed-vector
    //     interrupt; we deliver via WHvRequestInterrupt (IoApic::request_irq)
    //     so Hyper-V's virtual LAPIC handles vector dispatch and EOI.
    std::thread timer_thread([&]() {
      if (!rootfs_backed) {
        return;
      }
      WhpVcpuIrqState& bsp = *vcpu_irq[0];
      while (!timer_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (vm_done.load() || control.command() != kControlRun) {
          continue;
        }
        std::lock_guard<std::mutex> lock(device_mu);
        for (const auto& irq : hpet.poll_expired()) {
          deliver_timer_irq(bsp, irq.ioapic_pin, irq.pic_irq, irq.level, "hpet");
        }
        if (!hpet.legacy_mode() && pit.poll_irq0()) {
          // Two delivery paths:
          //   * IOAPIC unmasked PIT pin: kernel set up the IRQ handler and
          //     wants the timer through the LAPIC; route via WHvRequestInterrupt
          //     using whatever vector the kernel programmed in the RTE.
          //     PC-compatible IOAPIC wiring maps ISA IRQ0 to GSI 2, just like
          //     QEMU's ioapic_set_irq() and the MADT Type 2 override above.
          //   * IOAPIC masked: kernel hasn't programmed the IOAPIC for the PIT,
          //     fall back to legacy ExtInt with the PIC's vector. The kernel
          //     still depends on those early ticks for scheduler jiffies before
          //     the final IOAPIC route is ready.
          deliver_timer_irq(bsp, kPitIoApicPin, 0, false, "pit");
        }
      }
    });
    auto stop_timer = [&]() {
      timer_done = true;
      if (timer_thread.joinable()) {
        timer_thread.join();
      }
    };
    auto make_result = [&]() {
      control.set_state(kControlStateExited);
      stop_input();
      stop_watchdog();
      stop_timer();
      if (watchdog_timeout.load() && error_message.empty()) {
        throw std::runtime_error("VM timed out after " + std::to_string(timeout_ms) + "ms");
      }
      if (!error_message.empty()) {
        throw std::runtime_error(error_message);
      }
      std::string reason;
      uint32_t code = 0;
      {
        std::lock_guard<std::mutex> lock(result_mu);
        reason = final_exit_reason;
        code = final_exit_code;
      }
      uint64_t total_runs = runs.load();
      napi_value out = MakeObject(env);
      SetString(env, out, "exitReason", reason);
      SetUint32(env, out, "exitReasonCode", code);
      SetUint32(env, out, "runs", total_runs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total_runs));
      SetString(env, out, "console", uart.console());
      return out;
    };

    bool trace_whp = WhpTraceEnabled();
    auto vcpu_loop = [&](uint32_t cpu_index) {
      bool locally_halted = false;
      WHV_RUN_VP_EXIT_CONTEXT last_exit_context{};
      bool have_last_exit_context = false;
      for (;;) {
        if (vm_done.load()) {
          return;
        }
        int32_t command = control.command();
        if (command == kControlStop) {
          control.set_state(kControlStateStopping);
          finish("host-stop", WHvRunVpExitReasonCanceled);
          return;
        }
        if (command == kControlPause) {
          auto paused_at = std::chrono::steady_clock::now();
          if (paused_count.fetch_add(1) + 1 == cpus) {
            control.set_state(kControlStatePaused);
          }
          while (control.command() == kControlPause && !vm_done.load() && !watchdog_timeout.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          paused_count.fetch_sub(1);
          if (timeout_ms > 0 && cpu_index == 0) {
            auto paused_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - paused_at)
                                 .count();
            timeout_extension_ms.fetch_add(paused_ms);
          }
          if (control.command() == kControlStop) {
            control.set_state(kControlStateStopping);
            finish("host-stop", WHvRunVpExitReasonCanceled);
            return;
          }
          if (!vm_done.load()) {
            control.set_state(kControlStateRunning);
          }
        }
        if (locally_halted) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }

        {
          std::lock_guard<std::mutex> lock(device_mu);
          if (rootfs_backed && cpu_index == 0) {
            // Pre-run delivery follows whpx_vcpu_pre_run with kernel-irqchip
            // active (qemu/target/i386/whpx/whpx-all.c:1595-1642):
            // attempt the inject when the CPU is in a known-deliverable
            // state; otherwise arm a one-shot DeliverabilityNotifications
            // window so the next exit lets us inject.
            (void)TryDeliverPendingExtInt(api, partition.handle, *vcpu_irq[0]);
          }
          ioapic.drain_pending();
        }
        WHV_RUN_VP_EXIT_CONTEXT exit_context{};
        HRESULT hr = api.run_vp(partition.handle, cpu_index, &exit_context, sizeof(exit_context));
        if (FAILED(hr)) {
          if (watchdog_timeout.load()) {
            std::string detail = have_last_exit_context ? " last " + DescribeWhpExit(last_exit_context) : "";
            throw std::runtime_error("VM timed out after " + std::to_string(timeout_ms) + "ms" + detail);
          }
          CheckHr(hr, "WHvRunVirtualProcessor");
        }
        uint64_t run_no = runs.fetch_add(1) + 1;
        last_exit_context = exit_context;
        have_last_exit_context = true;
        // Refresh per-vCPU interruptibility from the exit context, mirroring
        // whpx_vcpu_post_run (qemu/target/i386/whpx/whpx-all.c:1658-1679).
        UpdateVcpuFromExit(*vcpu_irq[cpu_index], exit_context);
        if (trace_whp && run_no <= 65536) {
          std::fprintf(
              stderr,
              "[node-vmm whp] cpu=%u run=%llu %s\n",
              cpu_index,
              static_cast<unsigned long long>(run_no),
              DescribeWhpExit(exit_context).c_str());
        }
        if (watchdog_timeout.load()) {
          throw std::runtime_error(
              "VM timed out after " + std::to_string(timeout_ms) + "ms last " + DescribeWhpExit(exit_context));
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64Halt) {
          if (rootfs_backed) {
            // Mirror whpx_handle_halt (qemu/target/i386/whpx/whpx-all.c:1498):
            // a HLT with an interrupt already pending and IF=1 should not
            // park the vCPU - inject right now and continue.
            bool wait_for_interrupt = false;
            {
              std::lock_guard<std::mutex> lock(device_mu);
              WhpVcpuIrqState& vcpu = *vcpu_irq[cpu_index];
              // A HLT exit is a stable boundary where the guest is waiting for
              // the next external interrupt. If the PIT thread posts IRQ0 just
              // after this exit, the following pre-run pass must be allowed to
              // inject it instead of waiting forever for an InterruptWindow exit.
              vcpu.ready_for_pic_interrupt = vcpu.interruptable && vcpu.interrupt_flag;
              if (boot_trace && cpu_index == 0) {
                static std::atomic<uint64_t> hlt_count{0};
                uint64_t h = ++hlt_count;
                if (h <= 20 || h % 100 == 0) {
                  std::fprintf(stderr, "[node-vmm hlt] cpu0 ready_for_pic_interrupt=%d if=%d shadow_ok=%d pending=%d count=%llu\n",
                               (int)vcpu.ready_for_pic_interrupt,
                               (int)vcpu.interrupt_flag,
                               (int)vcpu.interruptable,
                               (int)vcpu.ext_int_pending.load(),
                               (unsigned long long)h);
                }
              }
              if (vcpu.ext_int_pending.load()) {
                TryDeliverPendingExtInt(api, partition.handle, vcpu);
              } else {
                wait_for_interrupt = true;
              }
            }
            if (wait_for_interrupt) {
              std::unique_lock<std::mutex> wait_lock(run_wake_mu);
              run_wake_cv.wait_for(wait_lock, std::chrono::milliseconds(50));
            }
            continue;
          }
          locally_halted = true;
          if (halted_count.fetch_add(1) + 1 == cpus) {
            finish("hlt", exit_context.ExitReason);
          }
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64IoPortAccess) {
          bool guest_requested = false;
          bool halted_console = false;
          {
            std::lock_guard<std::mutex> lock(device_mu);
            WHV_X64_IO_PORT_ACCESS_CONTEXT io_context =
                PrepareWhpIoContext(api, partition.handle, cpu_index, guest, exit_context);
            WHV_EMULATOR_STATUS status{};
            HRESULT emu_hr = api.emulator_try_io(
                emulator.handle,
                &emulator_contexts[cpu_index],
                &exit_context.VpContext,
                &io_context,
                &status);
            if (FAILED(emu_hr) || !status.EmulationSuccessful) {
              HandleWhpIo(
                  api,
                  partition.handle,
                  cpu_index,
                  exit_context.VpContext.Rip,
                  io_context,
                  uart,
                  guest_exit,
                  &cmos,
                  &pm_timer);
            }
            guest_requested = guest_exit.requested;
            halted_console = uart.contains("reboot: System halted") || uart.contains("Restarting system");
          }
          if (guest_requested) {
            finish("guest-exit", exit_context.ExitReason);
            return;
          }
          if (halted_console) {
            finish("halted-console", WHvRunVpExitReasonX64Halt);
            return;
          }
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonMemoryAccess) {
          WHV_EMULATOR_STATUS status{};
          {
            std::lock_guard<std::mutex> lock(device_mu);
            CheckHr(
                api.emulator_try_mmio(
                    emulator.handle,
                    &emulator_contexts[cpu_index],
                    &exit_context.VpContext,
                    &exit_context.MemoryAccess,
                    &status),
                "WHvEmulatorTryMmioEmulation");
          }
          if (!status.EmulationSuccessful) {
            throw std::runtime_error(
                "WHP MMIO emulation failed with status " + std::to_string(status.AsUINT32) +
                " gpa=0x" + std::to_string(exit_context.MemoryAccess.Gpa));
          }
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64ApicEoi) {
          // QEMU whpx-all.c handles this by broadcasting the EOI to its IOAPIC
          // model. Without this case WHP can surface a perfectly normal EOI as
          // an unknown VM-exit and we would stop a healthy guest.
          std::lock_guard<std::mutex> lock(device_mu);
          ioapic.eoi(exit_context.ApicEoi.InterruptVector);
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64InterruptWindow) {
          // QEMU just flags ready and clears the registered window; the next
          // pre-run call will inject (whpx-all.c:1864-1868). The IOAPIC drain
          // covers fixed interrupts that the guest may finally take.
          std::lock_guard<std::mutex> lock(device_mu);
          WhpVcpuIrqState& vcpu = *vcpu_irq[cpu_index];
          vcpu.ready_for_pic_interrupt = vcpu.interruptable && vcpu.interrupt_flag;
          vcpu.window_registered = false;
          if (vcpu.ext_int_pending.load()) {
            TryDeliverPendingExtInt(api, partition.handle, vcpu);
          }
          ioapic.drain_pending();
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64Cpuid) {
          HandleWhpCpuid(api, partition.handle, cpu_index, exit_context, cpus);
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonX64MsrAccess) {
          HandleWhpMsr(api, partition.handle, cpu_index, exit_context, hyperv);
          continue;
        }
        if (exit_context.ExitReason == WHvRunVpExitReasonCanceled) {
          if (boot_trace && cpu_index == 0) {
            static std::atomic<uint64_t> cancel_count{0};
            uint64_t c = ++cancel_count;
            if (c <= 30 || c % 100 == 0) {
              std::fprintf(stderr, "[node-vmm vcpu0] Canceled exit #%llu (rip=0x%llx)\n",
                           (unsigned long long)c,
                           (unsigned long long)exit_context.VpContext.Rip);
            }
          }
          if (watchdog_timeout.load()) {
            throw std::runtime_error("VM timed out after " + std::to_string(timeout_ms) + "ms");
          }
          if (vm_done.load()) {
            return;
          }
          continue;
        }
        finish(WhpExitReason(exit_context.ExitReason), exit_context.ExitReason);
        return;
      }
    };

    std::vector<std::thread> ap_threads;
    ap_threads.reserve(cpus > 0 ? cpus - 1 : 0);
    trace_phase("threads_about_to_start");
    try {
      control.set_state(kControlStateRunning);
      for (uint32_t i = 1; i < cpus; i++) {
        ap_threads.emplace_back([&, i]() {
          try {
            vcpu_loop(i);
          } catch (const std::exception& err) {
            record_error(err.what());
          } catch (...) {
            record_error("unknown WHP vCPU runner error");
          }
        });
      }
      try {
        vcpu_loop(0);
      } catch (const std::exception& err) {
        record_error(err.what());
      } catch (...) {
        record_error("unknown WHP vCPU runner error");
      }
      trace_phase("vcpu0_returned");
      finish("host-stop", WHvRunVpExitReasonCanceled);
      for (auto& thread : ap_threads) {
        if (thread.joinable()) {
          thread.join();
        }
      }
      return make_result();
    } catch (...) {
      finish("host-error", WHvRunVpExitReasonCanceled);
      for (auto& thread : ap_threads) {
        if (thread.joinable()) {
          thread.join();
        }
      }
      stop_input();
      stop_watchdog();
      stop_timer();
      control.set_state(kControlStateExited);
      throw;
    }
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// =====================================================================
// SECTION: NAPI module exports
// =====================================================================
// Returns {cols, rows} from the actual Windows console buffer. Used by the
// JS side to populate the guest TTY size cmdline arg. Direct Win32 query is
// more reliable than `process.stdout.columns`, which is undefined when
// stdout is piped (e.g. spawn from a parent Node) and falls back to the
// 80x24 default that breaks apk progress bars (issue: lines wrap because
// the guest thinks it has 80 cols when the host has 200+).
// Smoke-test entry point for native/whp_elf_loader.cc. Loads the ELF file
// at config.path into a temporary 256 MiB heap buffer and returns
// {entry, kernelEnd} on success, or throws via Throw on validation
// failures. Used by the unit tests in test/native.test.ts to exercise:
//   - rejection of truncated headers
//   - rejection of wrong magic / class / endianness / machine
//   - successful parsing of a hand-built minimal ELF64 vmlinux
napi_value WhpElfLoaderSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
      throw std::runtime_error("whpElfLoaderSmoke requires a config object");
    }
    std::string path = GetString(env, argv[0], "path");
    Check(!path.empty(), "path is required");
    constexpr size_t kBufBytes = 256ULL * 1024ULL * 1024ULL;
    std::vector<uint8_t> mem(kBufBytes, 0);
    KernelInfo info_result = LoadElfKernel(mem.data(), kBufBytes, path);
    napi_value out = MakeObject(env);
    napi_value entry;
    napi_create_bigint_uint64(env, info_result.entry, &entry);
    napi_set_named_property(env, out, "entry", entry);
    napi_value kernel_end;
    napi_create_bigint_uint64(env, info_result.kernel_end, &kernel_end);
    napi_set_named_property(env, out, "kernelEnd", kernel_end);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp_boot_params.cc. Builds boot_params + e820 +
// MP table into a fresh 16 MiB scratch buffer at the canonical layout
// addresses, then returns a Buffer view + a few key fields so the JS
// side can verify:
//   * boot signature 0xAA55 at offset 0x1FE inside boot_params
//   * kernel signature "HdrS" at offset 0x202
//   * e820 entry count at 0x1E8 == 4
//   * cmd_line_ptr at 0x228 == kCmdlineAddr
//   * MP floating-pointer signature "_MP_" near top of low RAM
napi_value WhpBootParamsSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string cmdline = "console=ttyS0 root=/dev/vda";
    int cpus = 2;
    if (argc >= 1) {
      napi_valuetype t;
      napi_typeof(env, argv[0], &t);
      if (t == napi_object) {
        std::string user_cmd = GetString(env, argv[0], "cmdline");
        if (!user_cmd.empty()) cmdline = user_cmd;
        cpus = static_cast<int>(GetUint32(env, argv[0], "cpus", 2));
      }
    }
    constexpr size_t kBufBytes = 16ULL * 1024ULL * 1024ULL;
    std::vector<uint8_t> mem(kBufBytes, 0);
    WriteBootParams(mem.data(), kBufBytes, cmdline);
    node_vmm::whp::WriteMpTable(mem.data(), kBufBytes, cpus, kPitIoApicPin);
    napi_value out = MakeObject(env);
    auto put32 = [&](const char* name, uint32_t value) {
      SetUint32(env, out, name, value);
    };
    auto read16 = [&](size_t off) -> uint32_t {
      return static_cast<uint32_t>(mem[off]) | (static_cast<uint32_t>(mem[off + 1]) << 8);
    };
    auto read32 = [&](size_t off) -> uint32_t {
      return static_cast<uint32_t>(mem[off])
          | (static_cast<uint32_t>(mem[off + 1]) << 8)
          | (static_cast<uint32_t>(mem[off + 2]) << 16)
          | (static_cast<uint32_t>(mem[off + 3]) << 24);
    };
    put32("e820_entries", mem[0x7000 + 0x1E8]);
    put32("vid_mode", read16(0x7000 + 0x1FA));
    put32("boot_sig", read16(0x7000 + 0x1FE));
    put32("kernel_sig", read32(0x7000 + 0x202));
    put32("type_of_loader", mem[0x7000 + 0x210]);
    put32("loadflags", mem[0x7000 + 0x211]);
    put32("cmd_line_ptr", read32(0x7000 + 0x228));
    put32("cmd_line_size", read32(0x7000 + 0x238));
    put32("e820_0_addr_lo", read32(0x7000 + 0x2D0));
    put32("e820_0_size_lo", read32(0x7000 + 0x2D0 + 8));
    put32("e820_0_type", read32(0x7000 + 0x2D0 + 16));
    // MP floating pointer is placed near the top of low RAM (under 0xA0000).
    // Scan 0x9F800..0xA0000 for the "_MP_" signature.
    int mp_off = -1;
    for (size_t off = 0x9F800; off < 0xA0000; off += 16) {
      if (off + 4 <= kBufBytes && mem[off] == '_' && mem[off + 1] == 'M' && mem[off + 2] == 'P' && mem[off + 3] == '_') {
        mp_off = static_cast<int>(off);
        break;
      }
    }
    put32("mp_signature_offset", mp_off >= 0 ? static_cast<uint32_t>(mp_off) : 0u);
    SetBool(env, out, "mp_signature_found", mp_off >= 0);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp_page_tables.cc. Builds a 4 GiB identity-map
// page table at offset 0x9000 of a fresh 64 MiB scratch buffer (large
// enough to hold the PML4 + PDPT + 4 PDs = 6 pages = 24 KiB starting at
// 0x9000, so the PD3 entry at 0xE000 + 0xFF8 = 0xEFF8 still fits). Returns
// the table-base + the first few entries so the JS side can verify:
//   * PML4[0] points to the PDPT (next page) with flags 0x07
//   * PDPT[0..3] each point to the right PD with flags 0x07
//   * PD0[0] is identity-mapped at phys=0 with huge-page flags 0x83
//   * PD3[511] is mapped at phys=0xFFE00000 (top of 4 GiB) with flags 0x83
napi_value WhpPageTablesSmoke(napi_env env, napi_callback_info /*info*/) {
  try {
    constexpr size_t kBufBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr uint64_t kBase = 0x9000;
    std::vector<uint8_t> mem(kBufBytes, 0);
    BuildPageTables(mem.data(), kBase);
    auto read64 = [&mem](uint64_t addr) -> uint64_t {
      uint64_t v = 0;
      for (int i = 0; i < 8; i++) {
        v |= uint64_t(mem[addr + i]) << (8 * i);
      }
      return v;
    };
    napi_value out = MakeObject(env);
    auto put = [&](const char* name, uint64_t value) {
      napi_value v;
      napi_create_bigint_uint64(env, value, &v);
      napi_set_named_property(env, out, name, v);
    };
    put("base", kBase);
    put("pml4_0", read64(kBase));
    put("pdpt_0", read64(kBase + 0x1000));
    put("pdpt_1", read64(kBase + 0x1000 + 8));
    put("pdpt_2", read64(kBase + 0x1000 + 16));
    put("pdpt_3", read64(kBase + 0x1000 + 24));
    put("pd0_0", read64(kBase + 0x2000));
    put("pd0_1", read64(kBase + 0x2000 + 8));
    put("pd3_511", read64(kBase + 0x5000 + 511 * 8));
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp/irq.cc. Drives every branch of the IRQ
// delivery state machine without touching WHP:
//   * UpdateVcpuFromExit reflects RFLAGS.IF / InterruptShadow /
//     InterruptionPending from a fabricated WHV_RUN_VP_EXIT_CONTEXT.
//   * EvaluateInjectDecision returns the matching enum
//     (kNoPending / kInject / kArmWindow) for every can_inject combination.
//   * Idempotence check: setting window_registered=true and toggling
//     ext_int_pending leaves window_registered alone (no double-arm).
//
// Inputs (config object):
//   rflags             : uint32 RFLAGS value (only bit 9 / IF matters)
//   interruptShadow    : bool   (1 → blocked by STI/MOV-SS shadow)
//   interruptionPending: bool   (1 → vCPU is mid-event-injection)
//   readyForPic        : bool   (set by InterruptWindow exit handler)
//   extIntPending      : bool   (set by timer/UART thread)
//   extIntVector       : uint32 (low 8 bits used)
//   windowRegistered   : bool   (initial state for idempotence test)
//
// Outputs:
//   { interruptable, interruptFlag, interruptionPending, readyForPic,
//     decision: 'noPending' | 'inject' | 'armWindow', windowRegistered }
napi_value WhpIrqStateSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Check(argc >= 1, "whpIrqStateSmoke requires a config object");

    uint32_t rflags = GetUint32(env, argv[0], "rflags", 0x202u);
    bool interrupt_shadow = GetBool(env, argv[0], "interruptShadow", false);
    bool interruption_pending = GetBool(env, argv[0], "interruptionPending", false);
    bool ready_for_pic = GetBool(env, argv[0], "readyForPic", false);
    bool ext_int_pending = GetBool(env, argv[0], "extIntPending", false);
    uint32_t ext_int_vector = GetUint32(env, argv[0], "extIntVector", 0x20u);
    bool window_registered = GetBool(env, argv[0], "windowRegistered", false);

    WhpVcpuIrqState vcpu;
    vcpu.index = 0;
    vcpu.window_registered = window_registered;
    vcpu.ext_int_pending.store(ext_int_pending);
    vcpu.ext_int_vector.store(ext_int_vector);
    vcpu.ready_for_pic_interrupt = ready_for_pic;

    WHV_RUN_VP_EXIT_CONTEXT ctx{};
    ctx.VpContext.Rflags = rflags;
    ctx.VpContext.ExecutionState.InterruptShadow = interrupt_shadow ? 1 : 0;
    ctx.VpContext.ExecutionState.InterruptionPending = interruption_pending ? 1 : 0;
    UpdateVcpuFromExit(vcpu, ctx);

    node_vmm::whp::InjectDecision decision = node_vmm::whp::EvaluateInjectDecision(vcpu);
    const char* decision_str = "noPending";
    if (decision == node_vmm::whp::InjectDecision::kInject) {
      decision_str = "inject";
    } else if (decision == node_vmm::whp::InjectDecision::kArmWindow) {
      decision_str = "armWindow";
    }

    napi_value out = MakeObject(env);
    SetBool(env, out, "interruptable", vcpu.interruptable);
    SetBool(env, out, "interruptFlag", vcpu.interrupt_flag);
    SetBool(env, out, "interruptionPending", vcpu.interruption_pending);
    SetBool(env, out, "readyForPic", vcpu.ready_for_pic_interrupt);
    SetBool(env, out, "windowRegistered", vcpu.window_registered);
    napi_value dec_v;
    napi_create_string_utf8(env, decision_str, NAPI_AUTO_LENGTH, &dec_v);
    napi_set_named_property(env, out, "decision", dec_v);
    SetUint32(env, out, "extIntVector", vcpu.ext_int_vector.load());
    SetBool(env, out, "extIntPending", vcpu.ext_int_pending.load());
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp/devices/pic.cc. Drives the master 8259 PIC
// through Linux's init_8259A reprogramming sequence (ICW1 → ICW2 → ICW3 →
// ICW4) and returns the post-init state so the JS side can assert:
//   * is_initialized() flips true once ICW2 (vector base) is written
//   * vector_for_irq(0) reflects the new vector base (e.g. 0x30, not 0x20)
//   * mask reads through port 0x21 reflect the OCW1 byte we wrote
//
// The Pic only calls into WHP from request_irq, which we don't exercise
// here — the smoke is purely a state-machine drive. We still need a
// WhpApi instance (Pic holds it by reference); `WhpApi(false)` skips any
// WHP DLL probe so the test runs on machines without Hyper-V too.
napi_value WhpPicSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    uint32_t vector_base = 0x30;
    uint32_t mask_after_init = 0xFB;  // unmask IRQ2 (slave) like Linux does
    if (argc >= 1) {
      napi_valuetype t;
      napi_typeof(env, argv[0], &t);
      if (t == napi_object) {
        vector_base = GetUint32(env, argv[0], "vectorBase", 0x30);
        mask_after_init = GetUint32(env, argv[0], "maskAfterInit", 0xFB);
      }
    }

    WhpApi api(false);
    Pic pic(api, nullptr);

    napi_value out = MakeObject(env);
    SetBool(env, out, "initializedBeforeIcw", pic.is_initialized());
    SetUint32(env, out, "vectorForIrq0Before", pic.vector_for_irq(0));

    // ICW1: 0x11 = ICW1_INIT | ICW1_ICW4_NEEDED. Master command port is 0x20.
    pic.write_port(0x20, 0x11);
    // ICW2 = vector base. Master data port is 0x21.
    pic.write_port(0x21, static_cast<uint8_t>(vector_base & 0xFF));
    // ICW3 = cascade pin map (slave on IRQ2 → bit 2 = 0x04).
    pic.write_port(0x21, 0x04);
    // ICW4 = 0x01 (8086 mode).
    pic.write_port(0x21, 0x01);
    // OCW1: write the mask.
    pic.write_port(0x21, static_cast<uint8_t>(mask_after_init & 0xFF));

    SetBool(env, out, "initializedAfterIcw", pic.is_initialized());
    SetUint32(env, out, "vectorForIrq0After", pic.vector_for_irq(0));
    SetUint32(env, out, "vectorForIrq3After", pic.vector_for_irq(3));
    SetUint32(env, out, "maskRead", pic.read_port(0x21));
    SetBool(env, out, "irq0Unmasked", pic.irq_unmasked(0));
    SetBool(env, out, "irq2Unmasked", pic.irq_unmasked(2));
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp/devices/pit.cc. Walks channel 2 through the
// PC speaker / TSC-calibration path: gate it via set_channel2_gate(true),
// program a tiny reload value via the lobyte/hibyte access mode (mirroring
// the kernel's pit_hpet_ptimer_calibrate_cpu sequence), wait long enough
// that elapsed_time × kPitHz exceeds the divisor, then read back the OUT
// pin state. Returns the gate state and OUT-pin truth values for both the
// pre-wait and post-wait reads.
napi_value WhpPitSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    // 0x000A = 10 PIT-clock ticks ≈ 8.4 µs. Sleeping 5 ms leaves us with
    // ~5957 ticks against the divisor 10 → guaranteed OUT high.
    uint32_t reload = 0x000A;
    uint32_t wait_ms = 5;
    if (argc >= 1) {
      napi_valuetype t;
      napi_typeof(env, argv[0], &t);
      if (t == napi_object) {
        reload = GetUint32(env, argv[0], "reload", reload);
        wait_ms = GetUint32(env, argv[0], "waitMs", wait_ms);
      }
    }

    Pit pit;
    napi_value out = MakeObject(env);

    // Before gating: OUT must be low.
    SetBool(env, out, "channel2OutBeforeGate", pit.channel2_out_high());
    SetBool(env, out, "channel2GatedBeforeGate", pit.channel2_gated());

    // Mode 0 (interrupt-on-terminal-count), access=3 (lobyte/hibyte).
    // Command byte: SC=10 (channel 2) | RW=11 (lo+hi) | M=000 (mode 0) | BCD=0
    //             = 0xB0.
    pit.write_port(0x43, 0xB0);
    // Now gate the channel — sets start = now.
    pit.set_channel2_gate(true);
    // Reload low + high byte through port 0x42.
    pit.write_port(0x42, static_cast<uint8_t>(reload & 0xFF));
    pit.write_port(0x42, static_cast<uint8_t>((reload >> 8) & 0xFF));

    SetBool(env, out, "channel2GatedAfterGate", pit.channel2_gated());

    Sleep(wait_ms);

    SetBool(env, out, "channel2OutAfterWait", pit.channel2_out_high());

    // Ungate and verify OUT drops back low.
    pit.set_channel2_gate(false);
    SetBool(env, out, "channel2OutAfterUngate", pit.channel2_out_high());

    SetUint32(env, out, "reload", reload);
    SetUint32(env, out, "waitMs", wait_ms);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp/devices/uart.cc — exercises the CRLF
// normalization in `write_stdout` end-to-end without spinning up a
// partition. The test uses `echo_stdout=false` so we don't actually
// touch the host console; instead we drive `emit_bytes` (the same
// internal path the paravirt port 0x600 uses) with mixed CRLF/LF input
// and verify the captured `console_` buffer matches.
//
// Why this exists: PR-1 fixed the apk progress-bar redraw bug by
// restoring CRLF normalization. PR-6 turned that fix into a module-
// level test so any future refactor of Uart::write_stdout can't lose
// it without a CI failure.
napi_value WhpUartCrlfSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string input = "a\nb\n";
    char initial_last_byte = 0;
    if (argc >= 1) {
      napi_valuetype t;
      napi_typeof(env, argv[0], &t);
      if (t == napi_object) {
        std::string user_input = GetString(env, argv[0], "input");
        if (!user_input.empty()) {
          input = user_input;
        }
        initial_last_byte = static_cast<char>(GetUint32(env, argv[0], "lastByte", 0));
      }
    }

    char last_byte = initial_last_byte;
    std::string normalized = Uart::NormalizeCrlf(input, last_byte);

    napi_value out = MakeObject(env);
    napi_value v;
    napi_create_string_utf8(env, input.c_str(), input.size(), &v);
    napi_set_named_property(env, out, "input", v);
    napi_create_string_utf8(env, normalized.c_str(), normalized.size(), &v);
    napi_set_named_property(env, out, "normalized", v);
    SetUint32(env, out, "lastByte", static_cast<uint32_t>(static_cast<unsigned char>(last_byte)));
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

// Smoke-test for native/whp/virtio/blk.cc — exercises the MMIO read
// path against the device-id / version / vendor / magic registers
// without spinning up a partition. Creates a 512-byte scratch file as
// the rootfs so the constructor's CreateFileA succeeds.
//
// Contract: virtio-mmio v2 magic 0x74726976 ("virt"), version 2,
// device id 2 (block), vendor "QEMU" (0x554D4551).
// Register-level UART smoke for the QEMU-like 16550 behavior used by WHP
// interactive ttyS0. This deliberately stays below the VM layer so failures
// point at the device model: FIFO trigger levels, IIR priority, overrun
// reporting/clearing, and THRE/TEMT transitions.
// Smoke-test for native/whp/console_writer.h. Exercises the host-only
// normalization that translates BusyBox apk progress redraw frames from
// DEC save/restore cursor form into a ConPTY-safe single-line redraw.
napi_value WhpConsoleWriterSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string input = "\x1b""7 33% ####        \x1b""8\x1b[0K\r";
    if (argc >= 1) {
      napi_valuetype t;
      napi_typeof(env, argv[0], &t);
      if (t == napi_object) {
        std::string user_input = GetString(env, argv[0], "input");
        if (!user_input.empty()) {
          input = user_input;
        }
      }
    }

    std::string normalized = node_vmm::whp::ConsoleWriter::NormalizeHostTerminalBytesForTest(input);

    napi_value out = MakeObject(env);
    SetString(env, out, "input", input);
    SetString(env, out, "normalized", normalized);
    SetBool(env, out, "containsDecSaveRestore",
            normalized.find("\x1b""7") != std::string::npos ||
                normalized.find("\x1b""8") != std::string::npos);
    SetBool(env, out, "hidesCursor", normalized.find("\x1b[?25l") != std::string::npos);
    SetBool(env, out, "showsCursor", normalized.find("\x1b[?25h") != std::string::npos);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value WhpUartRegisterSmoke(napi_env env, napi_callback_info /*info*/) {
  try {
    Uart uart(4096, false);
    uint32_t irq_count = 0;
    uart.attach_irq_raiser([&](uint32_t irq) {
      if (irq == 4) {
        irq_count++;
      }
    });

    uint8_t initial_lsr = uart.read(5);
    uart.write(2, 0xC7);  // FIFO enable, clear RX/TX, trigger level 14.
    uart.write(1, 0x07);  // RDI, THRI, RLSI.

    const uint8_t one = 'a';
    size_t accepted_one = uart.enqueue_rx(&one, 1);
    uint8_t iir_after_one = uart.read(2);

    uint8_t fill[13]{};
    for (size_t i = 0; i < sizeof(fill); i++) {
      fill[i] = static_cast<uint8_t>('b' + i);
    }
    size_t accepted_fill = uart.enqueue_rx(fill, sizeof(fill));
    uint8_t iir_at_trigger = uart.read(2);
    uint8_t first_rx = uart.read(0);

    uart.write(2, 0xC3);  // Keep FIFO enabled/trigger 14 and clear RX.
    std::vector<uint8_t> overflow(4097);
    for (size_t i = 0; i < overflow.size(); i++) {
      overflow[i] = static_cast<uint8_t>('A' + i);
    }
    size_t accepted_overflow = uart.enqueue_rx(overflow.data(), overflow.size());
    uint8_t overrun_lsr = uart.read(5);
    uint8_t overrun_lsr_after_clear = uart.read(5);

    uart.write(1, 0x02);  // THRI only, so TX-empty priority is observable.
    uart.write(0, 'x');
    uint8_t tx_iir = uart.read(2);
    uint8_t tx_lsr = uart.read(5);

    napi_value out = MakeObject(env);
    SetUint32(env, out, "initialLsr", initial_lsr);
    SetUint32(env, out, "acceptedOne", static_cast<uint32_t>(accepted_one));
    SetUint32(env, out, "iirAfterOne", iir_after_one);
    SetUint32(env, out, "acceptedFill", static_cast<uint32_t>(accepted_fill));
    SetUint32(env, out, "iirAtTrigger", iir_at_trigger);
    SetUint32(env, out, "firstRx", first_rx);
    SetUint32(env, out, "acceptedOverflow", static_cast<uint32_t>(accepted_overflow));
    SetUint32(env, out, "overrunLsr", overrun_lsr);
    SetUint32(env, out, "overrunLsrAfterClear", overrun_lsr_after_clear);
    SetUint32(env, out, "txIir", tx_iir);
    SetUint32(env, out, "txLsr", tx_lsr);
    SetUint32(env, out, "irqCount", irq_count);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value WhpVirtioBlkSmoke(napi_env env, napi_callback_info /*info*/) {
  try {
    char temp_path[MAX_PATH];
    DWORD got_temp = GetTempPathA(MAX_PATH, temp_path);
    Check(got_temp > 0 && got_temp < MAX_PATH, "GetTempPathA failed");
    char temp_file[MAX_PATH];
    Check(GetTempFileNameA(temp_path, "vmm", 0, temp_file) != 0, "GetTempFileNameA failed");
    {
      WinHandle h(CreateFileA(temp_file, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      Check(h.valid(), "open temp rootfs failed");
      DWORD wrote = 0;
      uint8_t zeros[512] = {};
      Check(WriteFile(h.get(), zeros, sizeof(zeros), &wrote, nullptr) != 0, "write temp rootfs failed");
    }

    constexpr size_t kRamBytes = 1ULL * 1024ULL * 1024ULL;
    std::vector<uint8_t> ram(kRamBytes, 0);
    GuestMemory mem{ram.data(), ram.size()};

    bool irq_called = false;
    VirtioBlk blk(0xD0000000ULL, mem, std::string(temp_file), std::string(), false,
                  [&] { irq_called = true; });

    napi_value out = MakeObject(env);
    auto read_reg32 = [&](uint64_t reg_off) -> uint32_t {
      uint8_t buf[4]{};
      blk.read_mmio(0xD0000000ULL + reg_off, buf, 4);
      return static_cast<uint32_t>(buf[0])
           | (static_cast<uint32_t>(buf[1]) << 8)
           | (static_cast<uint32_t>(buf[2]) << 16)
           | (static_cast<uint32_t>(buf[3]) << 24);
    };
    SetUint32(env, out, "magicValue", read_reg32(0x000));
    SetUint32(env, out, "version", read_reg32(0x004));
    SetUint32(env, out, "deviceId", read_reg32(0x008));
    SetUint32(env, out, "vendorId", read_reg32(0x00C));
    SetUint32(env, out, "queueNumMax", read_reg32(0x034));
    SetBool(env, out, "irqRaisedBeforeWrite", irq_called);
    DeleteFileA(temp_file);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value WhpHostConsoleSize(napi_env env, napi_callback_info /*info*/) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  unsigned cols = 0, rows = 0;
  if (out != INVALID_HANDLE_VALUE && out != nullptr) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(out, &info) != 0) {
      cols = static_cast<unsigned>(info.srWindow.Right - info.srWindow.Left + 1);
      rows = static_cast<unsigned>(info.srWindow.Bottom - info.srWindow.Top + 1);
    }
  }
  napi_value obj;
  napi_create_object(env, &obj);
  SetUint32(env, obj, "cols", cols);
  SetUint32(env, obj, "rows", rows);
  return obj;
}

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
      {"whpHostConsoleSize", nullptr, WhpHostConsoleSize, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpElfLoaderSmoke", nullptr, WhpElfLoaderSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpPageTablesSmoke", nullptr, WhpPageTablesSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpBootParamsSmoke", nullptr, WhpBootParamsSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpIrqStateSmoke", nullptr, WhpIrqStateSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpPicSmoke", nullptr, WhpPicSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpPitSmoke", nullptr, WhpPitSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpUartCrlfSmoke", nullptr, WhpUartCrlfSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpConsoleWriterSmoke", nullptr, WhpConsoleWriterSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpUartRegisterSmoke", nullptr, WhpUartRegisterSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"whpVirtioBlkSmoke", nullptr, WhpVirtioBlkSmoke, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"runVm", nullptr, RunVm, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

}  // namespace
