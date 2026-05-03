#pragma once

// Small Windows file I/O helpers shared between the virtio-blk extracted
// module and backend.cc. RAII handle + positioned read/write + sparse-
// file marking. All header-only inline so multiple TUs can include
// without ODR concerns.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include "../common/bytes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace node_vmm::whp {

struct WinHandle {
  HANDLE handle{INVALID_HANDLE_VALUE};

  WinHandle() = default;
  explicit WinHandle(HANDLE value) : handle(value) {}
  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;
  WinHandle(WinHandle&& other) noexcept : handle(other.handle) { other.handle = INVALID_HANDLE_VALUE; }
  WinHandle& operator=(WinHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle = other.handle;
      other.handle = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  ~WinHandle() { reset(); }

  void reset(HANDLE next = INVALID_HANDLE_VALUE) {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
    handle = next;
  }

  HANDLE get() const { return handle; }
  bool valid() const { return handle != INVALID_HANDLE_VALUE; }
};

inline void PreadAll(HANDLE file, uint8_t* dst, size_t len, uint64_t off) {
  LARGE_INTEGER pos{};
  pos.QuadPart = static_cast<LONGLONG>(off);
  node_vmm::common::Check(
      SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) != 0,
      "seek disk read failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
  size_t done = 0;
  while (done < len) {
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(len - done, 1U << 20));
    node_vmm::common::Check(
        ReadFile(file, dst + done, want, &got, nullptr) != 0,
        "read disk failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
    node_vmm::common::Check(got != 0, "short disk read");
    done += got;
  }
}

inline void PwriteAll(HANDLE file, const uint8_t* src, size_t len, uint64_t off) {
  LARGE_INTEGER pos{};
  pos.QuadPart = static_cast<LONGLONG>(off);
  node_vmm::common::Check(
      SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) != 0,
      "seek disk write failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
  size_t done = 0;
  while (done < len) {
    DWORD wrote = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(len - done, 1U << 20));
    node_vmm::common::Check(
        WriteFile(file, src + done, want, &wrote, nullptr) != 0,
        "write disk failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
    node_vmm::common::Check(wrote != 0, "short disk write");
    done += wrote;
  }
}

inline void TruncateFileTo(HANDLE file, uint64_t size) {
  LARGE_INTEGER pos{};
  pos.QuadPart = static_cast<LONGLONG>(size);
  node_vmm::common::Check(
      SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) != 0,
      "seek truncate failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
  node_vmm::common::Check(
      SetEndOfFile(file) != 0,
      "truncate overlay failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
}

inline void MarkFileSparse(HANDLE file) {
  DWORD bytes_returned = 0;
  node_vmm::common::Check(
      DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned, nullptr) != 0,
      "mark overlay sparse failed: " + node_vmm::common::WindowsErrorMessage(GetLastError()));
}

}  // namespace node_vmm::whp
