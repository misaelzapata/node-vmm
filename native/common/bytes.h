#pragma once

// Tiny shared helpers used by every WHP module: throw-on-false `Check`,
// little-endian byte readers/writers, range/overflow guards, and
// `WindowsErrorMessage` for HRESULT/Win32 error formatting. Each module
// previously carried a TU-local copy; consolidating them here means
// one source of truth and one less thing to read past when scanning a
// module's source.
//
// All functions are `inline` so the header is the implementation. No
// `.cc` file. Include from any C++ TU under native/whp/ or native/kvm/.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <stdexcept>
#include <string>

namespace node_vmm::common {

inline void Check(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

inline uint64_t CheckedAdd(uint64_t a, uint64_t b, const std::string& what) {
  Check(b == 0 || a <= UINT64_MAX - b, what + " overflow");
  return a + b;
}

inline uint64_t CheckedMul(uint64_t a, uint64_t b, const std::string& what) {
  if (a == 0 || b == 0) {
    return 0;
  }
  Check(a <= UINT64_MAX / b, what + " overflow");
  return a * b;
}

inline void CheckRange(uint64_t total, uint64_t offset, uint64_t len, const std::string& what) {
  Check(offset <= total, what + " offset is out of range");
  Check(len <= total - offset, what + " length is out of range");
}

inline void WriteU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

inline void WriteU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
  }
}

inline void WriteU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
  }
}

inline uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0])
      | (static_cast<uint32_t>(p[1]) << 8)
      | (static_cast<uint32_t>(p[2]) << 16)
      | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

// Generic little-endian load/store for partial-width MMIO accesses (HPET
// 32-bit halves of 64-bit registers, etc). `len` is clamped to 8.
inline uint64_t ReadLe(const uint8_t* p, uint32_t len) {
  uint64_t value = 0;
  for (uint32_t i = 0; i < len && i < 8; i++) {
    value |= static_cast<uint64_t>(p[i]) << (i * 8);
  }
  return value;
}

inline void WriteLe(uint8_t* p, uint32_t len, uint64_t value) {
  for (uint32_t i = 0; i < len && i < 8; i++) {
    p[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

// Replaces `bits` consecutive bits of `old_value` starting at `shift` with
// the low `bits` bits of `value`. Used by HPET register writes that span
// only a 32-bit half of a 64-bit register.
inline uint64_t DepositBits(uint64_t old_value, uint32_t shift, uint32_t bits, uint64_t value) {
  if (bits == 0 || shift >= 64) {
    return old_value;
  }
  uint32_t effective = bits > 64 - shift ? 64 - shift : bits;
  uint64_t mask = effective == 64 ? UINT64_MAX : ((uint64_t(1) << effective) - 1);
  return (old_value & ~(mask << shift)) | ((value & mask) << shift);
}

#ifdef _WIN32
// Windows-only: format a Win32 DWORD error code as a human-readable string.
inline std::string WindowsErrorMessage(DWORD code) {
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
#endif  // _WIN32

}  // namespace node_vmm::common
