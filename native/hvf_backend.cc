// Apple Hypervisor.framework backend for node-vmm (ARM64 macOS 15.0+)
#include <node_api.h>

#include <Hypervisor/Hypervisor.h>
#include <vmnet/vmnet.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ─── Memory layout ────────────────────────────────────────────────────────────
constexpr uint64_t kGicDistBase    = 0x08000000ULL;
constexpr uint64_t kGicRedistBase  = 0x080A0000ULL;
constexpr uint64_t kUartBase       = 0x09000000ULL;
constexpr uint64_t kVirtioBlkBase  = 0x0A000000ULL;
constexpr uint64_t kVirtioNetBase  = 0x0A001000ULL;
constexpr uint64_t kExitDevBase    = 0x0B000000ULL;
constexpr uint64_t kRamBase        = 0x40000000ULL;
constexpr uint64_t kDtbOffset      = 0x00000000ULL; // DTB at kRamBase + 0
constexpr uint64_t kKernelOffset   = 0x00400000ULL; // Kernel at kRamBase + 4MiB
constexpr uint64_t kVirtioStride   = 0x1000ULL;

// GIC SPI interrupt IDs (INTID = 32 + SPI_N)
constexpr uint32_t kUartIntid      = 32;   // SPI 0
constexpr uint32_t kVirtioBlkIntid = 33;   // SPI 1
constexpr uint32_t kVirtioNetIntid = 34;   // SPI 2

constexpr uint32_t kMaxQueueSize   = 256;
constexpr uint32_t kMaxFrameSize   = 1518;

// ─── Helpers ──────────────────────────────────────────────────────────────────
std::string ErrnoMessage(const std::string& what) {
  return what + ": " + strerror(errno);
}

void Check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

void CheckErr(int rc, const std::string& what) {
  if (rc < 0) throw std::runtime_error(ErrnoMessage(what));
}

void CheckHv(hv_return_t rc, const std::string& what) {
  if (rc != HV_SUCCESS) {
    throw std::runtime_error(what + " failed: hv_return=" + std::to_string(rc));
  }
}

// ─── RAII helpers ─────────────────────────────────────────────────────────────
struct Fd {
  int fd{-1};
  Fd() = default;
  explicit Fd(int v) : fd(v) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) { reset(); fd = o.fd; o.fd = -1; }
    return *this;
  }
  ~Fd() { reset(); }
  void reset(int next = -1) { if (fd >= 0) close(fd); fd = next; }
  int get() const { return fd; }
};

struct Mapping {
  void* addr{MAP_FAILED};
  size_t len{0};
  Mapping() = default;
  Mapping(void* a, size_t s) : addr(a), len(s) {}
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& o) noexcept : addr(o.addr), len(o.len) { o.addr = MAP_FAILED; o.len = 0; }
  Mapping& operator=(Mapping&& o) noexcept {
    if (this != &o) { reset(); addr = o.addr; len = o.len; o.addr = MAP_FAILED; o.len = 0; }
    return *this;
  }
  ~Mapping() { reset(); }
  void reset() {
    if (addr != MAP_FAILED && len > 0) munmap(addr, len);
    addr = MAP_FAILED; len = 0;
  }
  uint8_t* bytes() const { return reinterpret_cast<uint8_t*>(addr); }
};

// ─── Integer I/O helpers ──────────────────────────────────────────────────────
static inline uint16_t ReadU16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static inline uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static inline uint64_t ReadU64(const uint8_t* p) {
  return uint64_t(ReadU32(p)) | (uint64_t(ReadU32(p + 4)) << 32);
}
static inline void WriteU16(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static inline void WriteU32(uint8_t* p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static inline void WriteU64(uint8_t* p, uint64_t v) { WriteU32(p,uint32_t(v)); WriteU32(p+4,uint32_t(v>>32)); }

// Big-endian (for FDT)
static inline void WriteU32BE(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static inline void WriteU64BE(uint8_t* p, uint64_t v) { WriteU32BE(p,uint32_t(v>>32)); WriteU32BE(p+4,uint32_t(v)); }

// ─── File helpers ─────────────────────────────────────────────────────────────
std::vector<uint8_t> ReadFile(const std::string& path) {
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  CheckErr(fd.get(), "open " + path);
  struct stat st {};
  CheckErr(fstat(fd.get(), &st), "stat " + path);
  Check(st.st_size >= 0, "negative file size: " + path);
  std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = read(fd.get(), data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    Check(n > 0, ErrnoMessage("read " + path));
    done += static_cast<size_t>(n);
  }
  return data;
}

void PreadAll(int fd, uint8_t* dst, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread(fd, dst + done, len - done, off + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) continue;
    Check(n > 0, ErrnoMessage("pread disk"));
    done += static_cast<size_t>(n);
  }
}

void PwriteAll(int fd, const uint8_t* src, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pwrite(fd, src + done, len - done, off + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) continue;
    Check(n > 0, ErrnoMessage("pwrite disk"));
    done += static_cast<size_t>(n);
  }
}

// ─── GuestMemory ─────────────────────────────────────────────────────────────
class GuestMemory {
 public:
  GuestMemory() = default;
  GuestMemory(uint8_t* data, size_t size) : data_(data), size_(size) {}
  uint8_t* ptr(uint64_t gpa, uint64_t len) {
    // GPA is relative to kRamBase
    Check(gpa + len <= size_, "guest memory access out of bounds @" + std::to_string(gpa));
    return data_ + gpa;
  }
  const uint8_t* ptr(uint64_t gpa, uint64_t len) const {
    Check(gpa + len <= size_, "guest memory access out of bounds @" + std::to_string(gpa));
    return data_ + gpa;
  }
  size_t size() const { return size_; }
  uint8_t* data() const { return data_; }
 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
};

// ─── FDT Builder ─────────────────────────────────────────────────────────────
// Produces a minimal FDT blob for ARM64 Linux.
class FdtBuilder {
 public:
  std::vector<uint8_t> Build(
      uint64_t ram_base, uint64_t ram_size,
      uint32_t cpus, const std::string& cmdline,
      bool has_net)
  {
    buf_.clear();
    str_buf_.clear();

    // Reserve header space (48 bytes)
    buf_.resize(48, 0);

    // FDT_BEGIN_NODE for root
    AppendU32(FDT_BEGIN_NODE);
    AppendStr(""); // root name = ""
    Align4();

    // root properties
    AppendPropU32("#address-cells", 2);
    AppendPropU32("#size-cells", 2);
    AppendPropStr("compatible", "linux,dummy-virt");
    AppendPropStr("model", "node-vmm-hvf");

    // /chosen
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("chosen");
    Align4();
    AppendPropStr("bootargs", cmdline);
    AppendPropStr("stdout-path", "/pl011@9000000");
    AppendU32(FDT_END_NODE);

    // /cpus
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("cpus");
    Align4();
    AppendPropU32("#address-cells", 1);
    AppendPropU32("#size-cells", 0);
    for (uint32_t i = 0; i < cpus; i++) {
      std::string cpu_name = "cpu@" + ToHex(i);
      AppendU32(FDT_BEGIN_NODE);
      AppendStr(cpu_name);
      Align4();
      AppendPropStr("device_type", "cpu");
      AppendPropStr("compatible", "arm,arm-v8");
      AppendPropStr("enable-method", i == 0 ? "spin-table" : "psci");
      AppendPropU64("cpu-release-addr", 0);
      AppendPropU32("reg", i);
      AppendU32(FDT_END_NODE);
    }
    AppendU32(FDT_END_NODE); // /cpus

    // /psci
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("psci");
    Align4();
    AppendPropStr("compatible", "arm,psci-0.2");
    AppendPropStr("method", "hvc");
    AppendU32(FDT_END_NODE);

    // /memory
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("memory@" + ToHex64(ram_base));
    Align4();
    AppendPropStr("device_type", "memory");
    // reg = <addr_hi addr_lo size_hi size_lo>
    AppendPropReg("reg", ram_base, ram_size);
    AppendU32(FDT_END_NODE);

    // /intc (GICv3)
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("intc@8000000");
    Align4();
    AppendPropU32("phandle", 1);
    AppendPropStr("compatible", "arm,gic-v3");
    AppendPropU32("#interrupt-cells", 3);
    AppendPropFlag("interrupt-controller");
    AppendPropFlag("ranges");
    // reg = distributor + redistributor
    {
      uint8_t regbuf[32];
      WriteU32BE(regbuf+0,  uint32_t(kGicDistBase >> 32));
      WriteU32BE(regbuf+4,  uint32_t(kGicDistBase));
      WriteU32BE(regbuf+8,  0);
      WriteU32BE(regbuf+12, 0x10000);
      WriteU32BE(regbuf+16, uint32_t(kGicRedistBase >> 32));
      WriteU32BE(regbuf+20, uint32_t(kGicRedistBase));
      WriteU32BE(regbuf+24, 0);
      WriteU32BE(regbuf+28, 0x20000 * cpus);
      AppendPropRaw("reg", regbuf, sizeof(regbuf));
    }
    AppendU32(FDT_END_NODE);

    // /pl011 UART
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("pl011@9000000");
    Align4();
    AppendPropStr("compatible", "arm,pl011\0arm,primecell");
    AppendPropReg("reg", kUartBase, 0x1000);
    // interrupts = <SPI 0 IRQ_TYPE_LEVEL_HIGH> = <1 0 4>
    {
      uint8_t irq[12];
      WriteU32BE(irq+0, 0); // GIC_SPI
      WriteU32BE(irq+4, 0); // SPI 0
      WriteU32BE(irq+8, 4); // level high
      AppendPropRaw("interrupts", irq, sizeof(irq));
    }
    AppendPropU32("interrupt-parent", 1);
    AppendPropStr("clock-names", "uartclk\0apb_pclk");
    {
      uint8_t clks[8];
      WriteU32BE(clks+0, 2);
      WriteU32BE(clks+4, 3);
      AppendPropRaw("clocks", clks, sizeof(clks));
    }
    AppendU32(FDT_END_NODE);

    // clock nodes for PL011
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("uartclk");
    Align4();
    AppendPropU32("phandle", 2);
    AppendPropStr("compatible", "fixed-clock");
    AppendPropU32("#clock-cells", 0);
    AppendPropU32("clock-frequency", 24000000);
    AppendU32(FDT_END_NODE);

    AppendU32(FDT_BEGIN_NODE);
    AppendStr("apb-pclk");
    Align4();
    AppendPropU32("phandle", 3);
    AppendPropStr("compatible", "fixed-clock");
    AppendPropU32("#clock-cells", 0);
    AppendPropU32("clock-frequency", 24000000);
    AppendU32(FDT_END_NODE);

    // virtio-blk
    AppendVirtioNode(kVirtioBlkBase, kVirtioBlkIntid - 32);

    // virtio-net (optional)
    if (has_net) {
      AppendVirtioNode(kVirtioNetBase, kVirtioNetIntid - 32);
    }

    // /exit-device (custom MMIO to signal guest exit)
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("exit@b000000");
    Align4();
    AppendPropStr("compatible", "node-vmm,exit");
    AppendPropReg("reg", kExitDevBase, 0x1000);
    AppendU32(FDT_END_NODE);

    AppendU32(FDT_END_NODE); // root
    AppendU32(FDT_END);

    // Build final blob
    uint32_t struct_size = static_cast<uint32_t>(buf_.size() - 48);
    uint32_t strings_off = static_cast<uint32_t>(buf_.size());
    buf_.insert(buf_.end(), str_buf_.begin(), str_buf_.end());
    Align4buf();
    uint32_t total = static_cast<uint32_t>(buf_.size());

    // Fill header
    uint8_t* h = buf_.data();
    WriteU32BE(h+0,  0xD00DFEED); // magic
    WriteU32BE(h+4,  total);
    WriteU32BE(h+8,  48);         // off_dt_struct
    WriteU32BE(h+12, strings_off);
    WriteU32BE(h+16, 48);         // off_mem_rsvmap (empty, right before struct)
    WriteU32BE(h+20, 17);         // version
    WriteU32BE(h+24, 16);         // last_comp_version
    WriteU32BE(h+28, 0);          // boot_cpuid_phys
    WriteU32BE(h+32, static_cast<uint32_t>(str_buf_.size())); // size_dt_strings
    WriteU32BE(h+36, struct_size); // size_dt_struct
    // bytes 40-47 are memory reservation map (two 0-entries already written as part of header)
    // Actually we need to write the memory reservation block between header and struct.
    // Simplest: just leave zeros (header says off_dt_struct=48 which assumes no reservations).
    // off_mem_rsvmap=40 (right at end of 10-word header), two uint64 = 16 bytes = takes us to 56.
    // Let me fix: use off_dt_struct = 56 for the empty reservation map entry.
    WriteU32BE(h+8,  56);         // off_dt_struct (after 2 reservation words)
    // The reservation map at offset 40: one entry of (0,0) to terminate
    // bytes 40-55 are already zero from buf_.resize(48).
    // But buf_ now is larger. The struct was appended starting at index 48 in buf_.
    // We need to shift the struct by 8 bytes (insert 8 zero bytes at offset 48).
    buf_.insert(buf_.begin() + 48, 8, 0);
    // Update strings_off and total
    strings_off += 8;
    total = static_cast<uint32_t>(buf_.size());
    h = buf_.data();
    WriteU32BE(h+4,  total);
    WriteU32BE(h+12, strings_off);

    return buf_;
  }

 private:
  static constexpr uint32_t FDT_BEGIN_NODE = 0x00000001;
  static constexpr uint32_t FDT_END_NODE   = 0x00000002;
  static constexpr uint32_t FDT_PROP       = 0x00000003;
  static constexpr uint32_t FDT_END        = 0x00000009;

  std::vector<uint8_t> buf_;
  std::vector<uint8_t> str_buf_;

  void AppendU32(uint32_t v) {
    uint8_t b[4]; WriteU32BE(b, v);
    buf_.insert(buf_.end(), b, b+4);
  }

  void AppendStr(const std::string& s) {
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
  }

  void Align4() {
    while (buf_.size() % 4) buf_.push_back(0);
  }

  void Align4buf() {
    while (buf_.size() % 4) buf_.push_back(0);
  }

  uint32_t AddString(const char* s) {
    uint32_t off = static_cast<uint32_t>(str_buf_.size());
    size_t len = strlen(s) + 1;
    str_buf_.insert(str_buf_.end(), s, s + len);
    return off;
  }

  void AppendPropRaw(const char* name, const uint8_t* data, uint32_t len) {
    AppendU32(FDT_PROP);
    AppendU32(len);
    AppendU32(AddString(name));
    buf_.insert(buf_.end(), data, data + len);
    Align4();
  }

  void AppendPropU32(const char* name, uint32_t value) {
    uint8_t b[4]; WriteU32BE(b, value);
    AppendPropRaw(name, b, 4);
  }

  void AppendPropU64(const char* name, uint64_t value) {
    uint8_t b[8]; WriteU64BE(b, value);
    AppendPropRaw(name, b, 8);
  }

  void AppendPropStr(const char* name, const char* value) {
    size_t len = strlen(value) + 1;
    AppendPropRaw(name, reinterpret_cast<const uint8_t*>(value), static_cast<uint32_t>(len));
  }

  // For compatible with two strings (null-separated)
  void AppendPropStr(const char* name, const std::string& value) {
    AppendPropRaw(name, reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
  }

  void AppendPropFlag(const char* name) {
    AppendPropRaw(name, nullptr, 0);
  }

  void AppendPropReg(const char* name, uint64_t base, uint64_t size) {
    uint8_t b[16];
    WriteU32BE(b+0,  uint32_t(base >> 32));
    WriteU32BE(b+4,  uint32_t(base));
    WriteU32BE(b+8,  uint32_t(size >> 32));
    WriteU32BE(b+12, uint32_t(size));
    AppendPropRaw(name, b, 16);
  }

  void AppendVirtioNode(uint64_t base, uint32_t spi) {
    std::string name = "virtio_mmio@" + ToHex64(base);
    AppendU32(FDT_BEGIN_NODE);
    AppendStr(name);
    Align4();
    AppendPropStr("compatible", "virtio,mmio");
    AppendPropReg("reg", base, 0x200);
    uint8_t irq[12];
    WriteU32BE(irq+0, 0); // GIC_SPI
    WriteU32BE(irq+4, spi);
    WriteU32BE(irq+8, 4); // level high
    AppendPropRaw("interrupts", irq, sizeof(irq));
    AppendU32(FDT_END_NODE);
  }

  static std::string ToHex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%x", v);
    return buf;
  }

  static std::string ToHex64(uint64_t v) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)v);
    return buf;
  }
};

// ─── ARM64 Image loader ───────────────────────────────────────────────────────
// ARM64 Image header (little-endian):
//  offset 0:  code0 (branch instruction)
//  offset 8:  text_offset (uint64_t)
//  offset 16: image_size (uint64_t)
//  offset 56: magic = 0x644D5241 ("ARM\x64")
struct Arm64ImageInfo {
  uint64_t load_offset; // text_offset from header (or 0x80000 default)
  uint64_t image_size;
  std::vector<uint8_t> data;
};

Arm64ImageInfo LoadArm64Image(const std::string& path, GuestMemory& mem) {
  std::vector<uint8_t> data = ReadFile(path);
  Check(data.size() >= 64, "ARM64 Image too small");
  constexpr uint32_t kArm64Magic = 0x644D5241;
  uint32_t magic = ReadU32(data.data() + 56);
  Check(magic == kArm64Magic, "not an ARM64 Image (bad magic at offset 56)");

  uint64_t text_offset = ReadU64(data.data() + 8);
  uint64_t image_size  = ReadU64(data.data() + 16);
  if (image_size == 0) image_size = static_cast<uint64_t>(data.size());
  if (text_offset == 0) text_offset = 0x80000; // default for 2MiB-aligned images

  // Load at kRamBase + kKernelOffset (we always use 4MiB offset)
  uint64_t load_gpa = kKernelOffset;
  Check(load_gpa + data.size() <= mem.size(), "ARM64 Image too large for guest RAM");
  memcpy(mem.ptr(load_gpa, data.size()), data.data(), data.size());

  Arm64ImageInfo info;
  info.load_offset = load_gpa;
  info.image_size  = image_size;
  info.data        = std::move(data);
  return info;
}

// ─── ESR decode helpers ───────────────────────────────────────────────────────
static inline uint32_t EsrEC(uint64_t esr)  { return uint32_t((esr >> 26) & 0x3F); }
static inline bool     EsrWnR(uint64_t esr) { return (esr >> 6) & 1; }
static inline uint32_t EsrSAS(uint64_t esr) { return uint32_t((esr >> 22) & 3); }
static inline uint32_t EsrSRT(uint64_t esr) { return uint32_t((esr >> 16) & 0x1F); }
static inline bool     EsrISV(uint64_t esr) { return (esr >> 24) & 1; }
static inline bool     EsrSSE(uint64_t esr) { return (esr >> 21) & 1; }

// ─── PL011 UART ───────────────────────────────────────────────────────────────
class Pl011Uart {
 public:
  Pl011Uart() = default;

  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len != 4) return;
    uint32_t val = 0;
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case 0x000: // DR
        if (!rx_.empty()) {
          val = rx_.front(); rx_.pop_front();
        }
        break;
      case 0x018: // FR
        val = 0x90; // TXFE | RXFE (TX empty, RX empty by default)
        if (!rx_.empty()) val &= ~0x10; // clear RXFE
        val |= 0x20; // TXFF never set (always room to write)
        break;
      case 0x02C: // IBRD
        val = 13; // 24MHz / (16 * 115200) ~ 13
        break;
      case 0x030: // CR
        val = cr_;
        break;
      case 0x038: // IMSC
        val = imsc_;
        break;
      case 0x03C: // RIS
        val = ris_;
        break;
      case 0x040: // MIS
        val = ris_ & imsc_;
        break;
      default:
        break;
    }
    WriteU32(data, val);
  }

  void write_mmio(uint64_t offset, const uint8_t* data, uint32_t len) {
    if (len != 4) return;
    uint32_t val = ReadU32(data);
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case 0x000: { // DR - transmit
        char c = static_cast<char>(val & 0xFF);
        if (console_limit_ == 0 || console_.size() < console_limit_) {
          console_.push_back(c);
        }
        if (echo_stdout_) {
          write(STDOUT_FILENO, &c, 1);
        }
        ris_ |= 0x20; // TXRIS
        update_irq_locked();
        break;
      }
      case 0x030: cr_ = val; break;
      case 0x038: imsc_ = val; update_irq_locked(); break;
      case 0x044: ris_ &= ~val; update_irq_locked(); break; // ICR
      default: break;
    }
  }

  void set_console_limit(size_t limit) { console_limit_ = limit; }
  void set_echo_stdout(bool echo) { echo_stdout_ = echo; }

  std::string console() const {
    std::lock_guard<std::mutex> lk(mu_);
    return console_;
  }

  void inject_char(char c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (rx_.size() < 4096) rx_.push_back(static_cast<uint8_t>(c));
    ris_ |= 0x10; // RXRIS
    update_irq_locked();
  }

  // Called without lock held — returns desired IRQ level
  bool irq_pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (ris_ & imsc_) != 0;
  }

  // Called from MMIO handler — fire GIC SPI after state update
  // (gic_inject is called after releasing the lock)
  bool consume_irq_level() {
    std::lock_guard<std::mutex> lk(mu_);
    bool level = (ris_ & imsc_) != 0;
    return level;
  }

  void set_gic_inject(std::function<void(uint32_t, bool)> fn) { gic_inject_ = std::move(fn); }

 private:
  void update_irq_locked() {
    bool level = (ris_ & imsc_) != 0;
    if (gic_inject_) gic_inject_(kUartIntid, level);
  }

  mutable std::mutex mu_;
  std::deque<uint8_t> rx_;
  std::string console_;
  size_t console_limit_{0};
  bool echo_stdout_{false};
  uint32_t cr_{0x300}; // UARTEN|TXE|RXE
  uint32_t imsc_{0};
  uint32_t ris_{0};
  std::function<void(uint32_t, bool)> gic_inject_;
};

// ─── Virtio descriptor helpers ────────────────────────────────────────────────
struct Desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct DescChain {
  std::array<Desc, kMaxQueueSize> items {};
  size_t size{0};
  void push(Desc d) {
    Check(size < items.size(), "virtio descriptor chain too long");
    items[size++] = d;
  }
  Desc& operator[](size_t i) { return items[i]; }
  const Desc& operator[](size_t i) const { return items[i]; }
  bool empty() const { return size == 0; }
};

struct VirtQueue {
  uint32_t size{kMaxQueueSize};
  bool ready{false};
  uint16_t last_avail{0};
  uint64_t desc_addr{0};
  uint64_t driver_addr{0};
  uint64_t device_addr{0};
};

// Virtio queue memory helpers (GPA offsets from kRamBase)
static Desc ReadDesc(GuestMemory& mem, const VirtQueue& q, uint16_t idx) {
  Check(idx < q.size, "virtio descriptor index out of bounds");
  uint8_t* p = mem.ptr(q.desc_addr + uint64_t(idx) * 16, 16);
  return Desc{ReadU64(p), ReadU32(p+8), ReadU16(p+12), ReadU16(p+14)};
}

static DescChain WalkChain(GuestMemory& mem, const VirtQueue& q, uint16_t head) {
  DescChain chain;
  std::array<uint8_t, kMaxQueueSize> seen{};
  uint16_t idx = head;
  for (;;) {
    Check(idx < q.size, "descriptor next out of bounds");
    Check(seen[idx] == 0, "descriptor cycle");
    seen[idx] = 1;
    Desc d = ReadDesc(mem, q, idx);
    Check((d.flags & 4) == 0, "indirect descriptors not supported");
    chain.push(d);
    if ((d.flags & 1) == 0) break;
    idx = d.next;
  }
  return chain;
}

static void PushUsed(GuestMemory& mem, VirtQueue& q, uint32_t id, uint32_t written) {
  uint8_t* idxp = mem.ptr(q.device_addr + 2, 2);
  uint16_t used = ReadU16(idxp);
  uint64_t entry = q.device_addr + 4 + uint64_t(used % q.size) * 8;
  WriteU32(mem.ptr(entry, 8), id);
  WriteU32(mem.ptr(entry+4, 4), written);
  WriteU16(idxp, used + 1);
}

// ─── VirtioBlk ────────────────────────────────────────────────────────────────
class VirtioBlk {
 public:
  VirtioBlk(GuestMemory mem, const std::string& path, const std::string& overlay_path,
            std::function<void(uint32_t, bool)> gic_inject)
      : mem_(mem), gic_inject_(std::move(gic_inject)) {
    bool overlay = !overlay_path.empty();
    int flags = overlay ? (O_RDONLY | O_CLOEXEC) : (O_RDWR | O_CLOEXEC);
    base_fd_.reset(open(path.c_str(), flags));
    CheckErr(base_fd_.get(), "open rootfs " + path);
    struct stat st{};
    CheckErr(fstat(base_fd_.get(), &st), "stat rootfs " + path);
    Check(st.st_size >= 0, "negative rootfs size");
    disk_bytes_ = static_cast<uint64_t>(st.st_size);
    sectors_ = disk_bytes_ / 512;
    if (overlay) {
      Check(overlay_path != path, "overlayPath must not equal rootfsPath");
      overlay_fd_.reset(open(overlay_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
      CheckErr(overlay_fd_.get(), "open overlay " + overlay_path);
      CheckErr(ftruncate(overlay_fd_.get(), st.st_size), "truncate overlay");
      dirty_sectors_.assign(static_cast<size_t>((sectors_ + 7) / 8), 0);
    }
    queues_[0].size = kMaxQueueSize;
  }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    uint32_t off = static_cast<uint32_t>(addr - kVirtioBlkBase);
    if (off < 0x100) {
      if (len != 4) return;
      WriteU32(data, read_reg(off));
      return;
    }
    uint8_t cfg[16]{};
    WriteU64(cfg, sectors_);
    WriteU32(cfg+8, 0);
    WriteU32(cfg+12, 128);
    uint32_t cfg_off = off - 0x100;
    if (cfg_off < sizeof(cfg))
      memcpy(data, cfg+cfg_off, std::min<uint32_t>(len, sizeof(cfg)-cfg_off));
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    uint32_t off = static_cast<uint32_t>(addr - kVirtioBlkBase);
    if (off >= 0x100 || len != 4) return;
    write_reg(off, ReadU32(data));
  }

 private:
  uint32_t read_reg(uint32_t off) {
    switch (off) {
      case 0x000: return 0x74726976;
      case 0x004: return 2;
      case 0x008: return 2;
      case 0x00C: return 0x554D4551;
      case 0x010: {
        uint64_t f = (1ULL<<32)|(1ULL<<9);
        return dev_features_sel_ == 1 ? uint32_t(f>>32) : uint32_t(f);
      }
      case 0x034: return kMaxQueueSize;
      case 0x044: return queues_[queue_sel_].ready ? 1 : 0;
      case 0x060: return interrupt_status_;
      case 0x070: return status_;
      case 0x0FC: return 0;
      default: return 0;
    }
  }

  void write_reg(uint32_t off, uint32_t value) {
    VirtQueue& q = queues_[queue_sel_];
    switch (off) {
      case 0x014: dev_features_sel_ = value; break;
      case 0x024: drv_features_sel_ = value; break;
      case 0x020:
        if (drv_features_sel_ == 0) drv_features_ = (drv_features_ & ~0xFFFFFFFFULL) | value;
        else drv_features_ = (drv_features_ & 0xFFFFFFFFULL) | (uint64_t(value)<<32);
        break;
      case 0x030: if (value < 8) queue_sel_ = value; break;
      case 0x038: if (value >= 1 && value <= kMaxQueueSize) q.size = value; break;
      case 0x044: q.ready = value != 0; break;
      case 0x050:
        if (value < 8 && queues_[value].ready) {
          handle_queue(queues_[value]);
          interrupt_status_ |= 1;
          gic_inject_(kVirtioBlkIntid, true);
        }
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) gic_inject_(kVirtioBlkIntid, false);
        break;
      case 0x070:
        if (value == 0) reset();
        else status_ = value;
        break;
      case 0x080: q.desc_addr   = (q.desc_addr   & ~0xFFFFFFFFULL) | value; break;
      case 0x084: q.desc_addr   = (q.desc_addr   & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      case 0x090: q.driver_addr = (q.driver_addr & ~0xFFFFFFFFULL) | value; break;
      case 0x094: q.driver_addr = (q.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      case 0x0A0: q.device_addr = (q.device_addr & ~0xFFFFFFFFULL) | value; break;
      case 0x0A4: q.device_addr = (q.device_addr & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      default: break;
    }
  }

  void reset() {
    status_ = 0; drv_features_ = 0;
    dev_features_sel_ = 0; drv_features_sel_ = 0;
    queue_sel_ = 0; interrupt_status_ = 0;
    for (auto& q : queues_) q = VirtQueue{};
    gic_inject_(kVirtioBlkIntid, false);
  }

  void handle_queue(VirtQueue& q) {
    uint16_t avail = ReadU16(mem_.ptr(q.driver_addr+2, 2));
    while (q.last_avail != avail) {
      uint16_t head = ReadU16(mem_.ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
      q.last_avail++;
      process_request(q, head);
    }
  }

  void process_request(VirtQueue& q, uint16_t head) {
    uint8_t status = 0;
    uint32_t written = 0;
    try {
      DescChain chain = WalkChain(mem_, q, head);
      Check(chain.size >= 2, "virtio-blk chain too short");
      Desc hdr = chain[0];
      Desc stat_d = chain[chain.size-1];
      Check(hdr.len >= 16, "virtio-blk header too short");
      Check((stat_d.flags & 2) != 0 && stat_d.len >= 1, "virtio-blk status desc invalid");
      uint8_t h[16]; memcpy(h, mem_.ptr(hdr.addr, 16), 16);
      uint32_t type = ReadU32(h);
      uint64_t sector = ReadU64(h+8);
      if (type == 0) {
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          Check((d.flags & 2) != 0, "virtio-blk read desc must be writable");
          Check((d.len % 512) == 0, "virtio-blk read length not sector-aligned");
          ReadDisk(sector, mem_.ptr(d.addr, d.len), d.len);
          written += d.len; sector += d.len / 512;
        }
      } else if (type == 1) {
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          Check((d.flags & 2) == 0, "virtio-blk write desc must be read-only");
          Check((d.len % 512) == 0, "virtio-blk write length not sector-aligned");
          WriteDisk(sector, mem_.ptr(d.addr, d.len), d.len);
          sector += d.len / 512;
        }
      } else if (type == 4) {
        fsync(write_fd());
      } else if (type == 8) {
        const char id[] = "node-vmm";
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          uint32_t n = std::min<uint32_t>(d.len, sizeof(id));
          memcpy(mem_.ptr(d.addr, n), id, n);
          written += n; break;
        }
      } else {
        status = 2;
      }
      memcpy(mem_.ptr(stat_d.addr, 1), &status, 1);
      PushUsed(mem_, q, head, written+1);
    } catch (...) {
      status = 1;
      try {
        DescChain chain = WalkChain(mem_, q, head);
        if (!chain.empty()) {
          Desc sd = chain[chain.size-1];
          memcpy(mem_.ptr(sd.addr, 1), &status, 1);
        }
      } catch (...) {}
      PushUsed(mem_, q, head, written);
    }
  }

  bool has_overlay() const { return overlay_fd_.get() >= 0; }
  int write_fd() const { return has_overlay() ? overlay_fd_.get() : base_fd_.get(); }

  bool sector_dirty(uint64_t s) const {
    if (!has_overlay() || s >= sectors_) return false;
    return (dirty_sectors_[static_cast<size_t>(s/8)] & (uint8_t(1)<<(s%8))) != 0;
  }
  void mark_dirty(uint64_t s) {
    if (!has_overlay() || s >= sectors_) return;
    dirty_sectors_[static_cast<size_t>(s/8)] |= uint8_t(1)<<(s%8);
  }
  void mark_dirty_range(uint64_t off, size_t len) {
    if (!has_overlay() || len == 0) return;
    for (uint64_t s = off/512; s <= (off+len-1)/512; s++) mark_dirty(s);
  }
  void prepare_partial_overlay(uint64_t off, size_t len) {
    if (!has_overlay() || len == 0) return;
    uint64_t end = off + len;
    std::array<uint8_t,512> buf{};
    for (uint64_t s = off/512; s <= (end-1)/512; s++) {
      uint64_t ss = s*512;
      bool full = off<=ss && end>=ss+512;
      if (full || sector_dirty(s)) continue;
      PreadAll(base_fd_.get(), buf.data(), 512, static_cast<off_t>(ss));
      PwriteAll(overlay_fd_.get(), buf.data(), 512, static_cast<off_t>(ss));
    }
  }
  void ReadDisk(uint64_t sector, uint8_t* dst, size_t len) {
    uint64_t off = sector * 512;
    Check(off + len <= disk_bytes_, "virtio-blk read out of range");
    if (!has_overlay()) { PreadAll(base_fd_.get(), dst, len, static_cast<off_t>(off)); return; }
    size_t done = 0;
    while (done < len) {
      uint64_t cur = off + done;
      uint64_t s = cur / 512;
      size_t in = static_cast<size_t>(cur % 512);
      size_t chunk = std::min(len-done, size_t(512)-in);
      int fd = sector_dirty(s) ? overlay_fd_.get() : base_fd_.get();
      PreadAll(fd, dst+done, chunk, static_cast<off_t>(cur));
      done += chunk;
    }
  }
  void WriteDisk(uint64_t sector, const uint8_t* src, size_t len) {
    uint64_t off = sector * 512;
    Check(off + len <= disk_bytes_, "virtio-blk write out of range");
    prepare_partial_overlay(off, len);
    PwriteAll(write_fd(), src, len, static_cast<off_t>(off));
    mark_dirty_range(off, len);
  }

  GuestMemory mem_;
  Fd base_fd_;
  Fd overlay_fd_;
  std::vector<uint8_t> dirty_sectors_;
  uint64_t disk_bytes_{0};
  uint64_t sectors_{0};
  uint32_t status_{0};
  uint64_t drv_features_{0};
  uint32_t dev_features_sel_{0};
  uint32_t drv_features_sel_{0};
  uint32_t queue_sel_{0};
  uint32_t interrupt_status_{0};
  VirtQueue queues_[8];
  std::function<void(uint32_t, bool)> gic_inject_;
};

// ─── VirtioNet (vmnet.framework) ──────────────────────────────────────────────
std::array<uint8_t, 6> ParseMac(const std::string& mac) {
  std::array<uint8_t,6> out{};
  unsigned int p[6]{};
  Check(sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &p[0],&p[1],&p[2],&p[3],&p[4],&p[5]) == 6,
        "invalid MAC: " + mac);
  for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(p[i]);
  return out;
}

class VirtioNet {
 public:
  VirtioNet(GuestMemory mem, const std::string& mac,
            std::function<void(uint32_t, bool)> gic_inject)
      : mem_(mem), mac_(ParseMac(mac)), gic_inject_(std::move(gic_inject)) {
    queues_[0].size = kMaxQueueSize;
    queues_[1].size = kMaxQueueSize;
    // Create notification pipe
    int pipefd[2];
    CheckErr(pipe(pipefd), "pipe for vmnet notification");
    notify_read_.reset(pipefd[0]);
    notify_write_.reset(pipefd[1]);
    // Set read end non-blocking
    int flags = fcntl(notify_read_.get(), F_GETFL);
    fcntl(notify_read_.get(), F_SETFL, flags | O_NONBLOCK);
  }

  ~VirtioNet() { stop_vmnet(); }

  // Start vmnet in shared (NAT) mode
  void start(const std::string& /*hint_tapname*/) {
    xpc_object_t opts = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(opts, vmnet_operation_mode_key, VMNET_SHARED_MODE);

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block vmnet_return_t status_out = VMNET_SUCCESS;
    __block interface_ref iface_out = nullptr;

    iface_ = vmnet_start_interface(opts, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
      ^(vmnet_return_t status, xpc_object_t params) {
        status_out = status;
        if (status == VMNET_SUCCESS) {
          iface_out = iface_;
          max_packet_size_ = static_cast<uint32_t>(
            xpc_dictionary_get_uint64(params, vmnet_max_packet_size_key));
          if (max_packet_size_ == 0) max_packet_size_ = kMaxFrameSize;
        }
        dispatch_semaphore_signal(sem);
      });

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    xpc_release(opts);

    Check(status_out == VMNET_SUCCESS, "vmnet_start_interface failed: " + std::to_string(status_out));
    iface_ = iface_out;

    // Register packet arrival callback
    int write_fd = notify_write_.get();
    vmnet_interface_set_event_callback(iface_, VMNET_INTERFACE_PACKETS_AVAILABLE,
      dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
      ^(interface_event_t /*event*/, xpc_object_t /*params*/) {
        uint8_t byte = 1;
        write(write_fd, &byte, 1);
      });
  }

  void stop_vmnet() {
    if (!iface_) return;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    vmnet_stop_interface(iface_, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
      ^(vmnet_return_t /*status*/) { dispatch_semaphore_signal(sem); });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    iface_ = nullptr;
  }

  bool notify_readable() const {
    struct pollfd pfd{};
    pfd.fd = notify_read_.get();
    pfd.events = POLLIN;
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
  }

  void drain_notify() {
    uint8_t buf[64];
    while (read(notify_read_.get(), buf, sizeof(buf)) > 0) {}
  }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    uint32_t off = static_cast<uint32_t>(addr - kVirtioNetBase);
    if (off < 0x100) {
      if (len != 4) return;
      WriteU32(data, read_reg(off));
      return;
    }
    uint8_t cfg[12]{};
    memcpy(cfg, mac_.data(), 6);
    WriteU16(cfg+6, 1);
    WriteU16(cfg+8, 1);
    WriteU16(cfg+10, 1500);
    uint32_t cfg_off = off - 0x100;
    if (cfg_off < sizeof(cfg))
      memcpy(data, cfg+cfg_off, std::min<uint32_t>(len, sizeof(cfg)-cfg_off));
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    uint32_t off = static_cast<uint32_t>(addr - kVirtioNetBase);
    if (off >= 0x100 || len != 4) return;
    write_reg(off, ReadU32(data));
  }

  void poll_rx() {
    if (!iface_ || !queues_[0].ready) return;
    drain_notify();
    for (int iter = 0; iter < 32; iter++) {
      if (!has_rx_buffer()) break;
      // Read packet from vmnet
      struct vmpktdesc pktdesc{};
      struct iovec iov{};
      std::array<uint8_t, kMaxFrameSize> frame_buf{};
      iov.iov_base = frame_buf.data();
      iov.iov_len = frame_buf.size();
      pktdesc.vm_pkt_iov = &iov;
      pktdesc.vm_pkt_iovcnt = 1;
      pktdesc.vm_flags = 0;
      int count = 1;
      vmnet_return_t ret = vmnet_read(iface_, &pktdesc, &count);
      if (ret != VMNET_SUCCESS || count == 0) break;
      inject_rx_frame(frame_buf.data(), pktdesc.vm_pkt_size);
    }
  }

  bool enabled() const { return iface_ != nullptr; }

 private:
  uint32_t read_reg(uint32_t off) {
    switch (off) {
      case 0x000: return 0x74726976;
      case 0x004: return 2;
      case 0x008: return 1;
      case 0x00C: return 0x554D4551;
      case 0x010: {
        uint64_t f = (1ULL<<32)|(1ULL<<5)|(1ULL<<16);
        return dev_features_sel_ == 1 ? uint32_t(f>>32) : uint32_t(f);
      }
      case 0x034: return queue_sel_ < 2 ? kMaxQueueSize : 0;
      case 0x044: return queue_sel_ < 2 && queues_[queue_sel_].ready ? 1 : 0;
      case 0x060: return interrupt_status_;
      case 0x070: return status_;
      case 0x0FC: return 0;
      default: return 0;
    }
  }

  void write_reg(uint32_t off, uint32_t value) {
    VirtQueue& q = queues_[queue_sel_ < 2 ? queue_sel_ : 0];
    switch (off) {
      case 0x014: dev_features_sel_ = value; break;
      case 0x024: drv_features_sel_ = value; break;
      case 0x020:
        if (drv_features_sel_ == 0) drv_features_ = (drv_features_ & ~0xFFFFFFFFULL) | value;
        else drv_features_ = (drv_features_ & 0xFFFFFFFFULL) | (uint64_t(value)<<32);
        break;
      case 0x030: if (value < 2) queue_sel_ = value; break;
      case 0x038: if (value >= 1 && value <= kMaxQueueSize) q.size = value; break;
      case 0x044: q.ready = value != 0; break;
      case 0x050:
        if (value == 0) poll_rx();
        else if (value == 1 && queues_[1].ready) handle_tx_queue();
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) gic_inject_(kVirtioNetIntid, false);
        break;
      case 0x070:
        if (value == 0) reset_dev();
        else status_ = value;
        break;
      case 0x080: q.desc_addr   = (q.desc_addr   & ~0xFFFFFFFFULL) | value; break;
      case 0x084: q.desc_addr   = (q.desc_addr   & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      case 0x090: q.driver_addr = (q.driver_addr & ~0xFFFFFFFFULL) | value; break;
      case 0x094: q.driver_addr = (q.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      case 0x0A0: q.device_addr = (q.device_addr & ~0xFFFFFFFFULL) | value; break;
      case 0x0A4: q.device_addr = (q.device_addr & 0xFFFFFFFFULL) | (uint64_t(value)<<32); break;
      default: break;
    }
  }

  void reset_dev() {
    status_ = 0; drv_features_ = 0;
    dev_features_sel_ = 0; drv_features_sel_ = 0;
    queue_sel_ = 0; interrupt_status_ = 0;
    for (auto& q : queues_) q = VirtQueue{};
    gic_inject_(kVirtioNetIntid, false);
  }

  bool has_rx_buffer() const {
    const VirtQueue& q = queues_[0];
    return q.ready && q.last_avail != ReadU16(mem_.ptr(q.driver_addr+2, 2));
  }

  void signal_net() {
    interrupt_status_ |= 1;
    gic_inject_(kVirtioNetIntid, true);
  }

  void inject_rx_frame(const uint8_t* frame, size_t len) {
    VirtQueue& q = queues_[0];
    uint16_t head = ReadU16(mem_.ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
    q.last_avail++;
    DescChain chain = WalkChain(mem_, q, head);
    size_t needed = 12 + len;
    size_t offset = 0;
    uint8_t header[12]{};
    for (size_t i = 0; i < chain.size; i++) {
      const Desc& d = chain[i];
      Check((d.flags & 2) != 0, "virtio-net rx desc must be writable");
      uint8_t* dst = mem_.ptr(d.addr, d.len);
      size_t desc_off = 0;
      if (offset < sizeof(header) && desc_off < d.len) {
        size_t n = std::min<size_t>(d.len-desc_off, sizeof(header)-offset);
        memcpy(dst+desc_off, header+offset, n);
        desc_off += n; offset += n;
      }
      if (offset >= sizeof(header) && desc_off < d.len && offset < needed) {
        size_t frame_off = offset - sizeof(header);
        size_t n = std::min<size_t>(d.len-desc_off, len-frame_off);
        memcpy(dst+desc_off, frame+frame_off, n);
        offset += n;
      }
      if (offset >= needed) break;
    }
    if (offset < needed) return;
    PushUsed(mem_, q, head, static_cast<uint32_t>(needed));
    signal_net();
  }

  void handle_tx_queue() {
    VirtQueue& q = queues_[1];
    uint16_t avail = ReadU16(mem_.ptr(q.driver_addr+2, 2));
    while (q.last_avail != avail) {
      uint16_t head = ReadU16(mem_.ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
      q.last_avail++;
      process_tx(head);
    }
    poll_rx();
  }

  void process_tx(uint16_t head) {
    VirtQueue& q = queues_[1];
    DescChain chain = WalkChain(mem_, q, head);
    std::vector<uint8_t> frame;
    frame.reserve(kMaxFrameSize);
    size_t skip = 12;
    for (size_t i = 0; i < chain.size; i++) {
      const Desc& d = chain[i];
      Check((d.flags & 2) == 0, "virtio-net tx desc must be read-only");
      const uint8_t* src = mem_.ptr(d.addr, d.len);
      if (skip >= d.len) { skip -= d.len; continue; }
      size_t pos = skip; skip = 0;
      frame.insert(frame.end(), src+pos, src+d.len);
    }
    if (iface_ && !frame.empty()) {
      struct vmpktdesc pktdesc{};
      struct iovec iov{};
      iov.iov_base = frame.data();
      iov.iov_len  = frame.size();
      pktdesc.vm_pkt_iov  = &iov;
      pktdesc.vm_pkt_iovcnt = 1;
      pktdesc.vm_pkt_size = frame.size();
      pktdesc.vm_flags    = 0;
      int count = 1;
      vmnet_write(iface_, &pktdesc, &count);
    }
    PushUsed(mem_, q, head, 0);
    signal_net();
  }

  GuestMemory mem_;
  std::array<uint8_t, 6> mac_;
  std::function<void(uint32_t, bool)> gic_inject_;
  interface_ref iface_{nullptr};
  uint32_t max_packet_size_{kMaxFrameSize};
  Fd notify_read_;
  Fd notify_write_;
  uint32_t status_{0};
  uint64_t drv_features_{0};
  uint32_t dev_features_sel_{0};
  uint32_t drv_features_sel_{0};
  uint32_t queue_sel_{0};
  uint32_t interrupt_status_{0};
  VirtQueue queues_[2];
};

// ─── NAPI helpers ─────────────────────────────────────────────────────────────
static napi_value MakeString(napi_env env, const std::string& s) {
  napi_value v;
  napi_create_string_utf8(env, s.c_str(), s.size(), &v);
  return v;
}
static napi_value MakeU32(napi_env env, uint32_t n) {
  napi_value v; napi_create_uint32(env, n, &v); return v;
}
static napi_value MakeBool(napi_env env, bool b) {
  napi_value v; napi_get_boolean(env, b, &v); return v;
}
static napi_value MakeObject(napi_env env) {
  napi_value v; napi_create_object(env, &v); return v;
}
static void SetProp(napi_env env, napi_value obj, const char* key, napi_value val) {
  napi_set_named_property(env, obj, key, val);
}
static std::string GetString(napi_env env, napi_value obj, const char* key) {
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_string) return "";
  size_t len; napi_get_value_string_utf8(env, v, nullptr, 0, &len);
  std::string s(len, '\0');
  napi_get_value_string_utf8(env, v, &s[0], len+1, nullptr);
  return s;
}
static uint32_t GetU32(napi_env env, napi_value obj, const char* key, uint32_t def = 0) {
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_number) return def;
  uint32_t n; napi_get_value_uint32(env, v, &n); return n;
}
static bool GetBool(napi_env env, napi_value obj, const char* key, bool def = false) {
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_boolean) return def;
  bool b; napi_get_value_bool(env, v, &b); return b;
}

// Throw a JS error from a C++ exception
static napi_value ThrowError(napi_env env, const std::string& msg) {
  napi_throw_error(env, nullptr, msg.c_str());
  return nullptr;
}

// ─── ProbeHvf ─────────────────────────────────────────────────────────────────
static napi_value ProbeHvf(napi_env env, napi_callback_info /*info*/) {
  napi_value result = MakeObject(env);
  try {
    // Test hv_vm_create
    hv_return_t rc = hv_vm_create(nullptr);
    bool vm_ok = (rc == HV_SUCCESS);
    if (vm_ok) hv_vm_destroy();

    SetProp(env, result, "backend", MakeString(env, "hvf"));
    SetProp(env, result, "available", MakeBool(env, vm_ok));
    SetProp(env, result, "arch", MakeString(env, "arm64"));
    if (!vm_ok) {
      SetProp(env, result, "reason", MakeString(env, "hv_vm_create failed: " + std::to_string(rc)));
    }
  } catch (const std::exception& e) {
    SetProp(env, result, "backend",   MakeString(env, "hvf"));
    SetProp(env, result, "available", MakeBool(env, false));
    SetProp(env, result, "arch",      MakeString(env, "arm64"));
    SetProp(env, result, "reason",    MakeString(env, e.what()));
  }
  return result;
}

// ─── RunVm ────────────────────────────────────────────────────────────────────
struct GuestExitReq {
  bool requested{false};
  uint8_t status{0};
};

static napi_value RunVm(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return ThrowError(env, "runVm requires config argument");

  std::string kernel_path  = GetString(env, argv[0], "kernelPath");
  std::string rootfs_path  = GetString(env, argv[0], "rootfsPath");
  std::string overlay_path = GetString(env, argv[0], "overlayPath");
  std::string tap_name     = GetString(env, argv[0], "netTapName");
  std::string guest_mac    = GetString(env, argv[0], "netGuestMac");
  std::string cmdline      = GetString(env, argv[0], "cmdline");
  uint32_t mem_mib         = GetU32(env, argv[0], "memMiB", 256);
  uint32_t cpus            = GetU32(env, argv[0], "cpus", 1);
  uint32_t timeout_ms      = GetU32(env, argv[0], "timeoutMs", 0);
  uint32_t console_limit   = GetU32(env, argv[0], "consoleLimit", 0);
  bool interactive         = GetBool(env, argv[0], "interactive", false);

  if (kernel_path.empty()) return ThrowError(env, "kernelPath is required");
  if (rootfs_path.empty()) return ThrowError(env, "rootfsPath is required");
  if (cpus < 1) cpus = 1;
  if (cpus > 4) cpus = 4; // HVF ARM64: practical limit

  bool has_net = !tap_name.empty() && tap_name != "none";
  // "vmnet:shared" sentinel from net.ts means use vmnet
  bool use_vmnet = has_net && (tap_name.find("vmnet") != std::string::npos || tap_name == "auto");

  try {
    // ── Create VM ──────────────────────────────────────────────────────────────
    CheckHv(hv_vm_create(nullptr), "hv_vm_create");

    struct VmGuard {
      ~VmGuard() { hv_vm_destroy(); }
    } vm_guard;

    // ── Allocate guest RAM ─────────────────────────────────────────────────────
    uint64_t mem_bytes = uint64_t(mem_mib) * 1024ULL * 1024ULL;
    void* ram = mmap(nullptr, static_cast<size_t>(mem_bytes),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(ram != MAP_FAILED, ErrnoMessage("mmap guest RAM"));
    Mapping ram_map(ram, static_cast<size_t>(mem_bytes));

    CheckHv(hv_vm_map(ram, kRamBase, mem_bytes,
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
            "hv_vm_map");

    GuestMemory mem(static_cast<uint8_t*>(ram), static_cast<size_t>(mem_bytes));

    // ── Load ARM64 kernel ──────────────────────────────────────────────────────
    Arm64ImageInfo kernel = LoadArm64Image(kernel_path, mem);

    // ── Build DTB ─────────────────────────────────────────────────────────────
    FdtBuilder fdt_builder;
    std::vector<uint8_t> dtb = fdt_builder.Build(
        kRamBase, mem_bytes, cpus, cmdline, use_vmnet);
    Check(dtb.size() < kKernelOffset, "DTB too large");
    memcpy(mem.ptr(kDtbOffset, dtb.size()), dtb.data(), dtb.size());

    // ── Create GIC v3 ─────────────────────────────────────────────────────────
    hv_gic_config_t gic_config = hv_gic_config_create();
    hv_gic_config_set_distributor_base(gic_config, kGicDistBase);
    hv_gic_config_set_redistributor_base(gic_config, kGicRedistBase);
    CheckHv(hv_gic_create(gic_config), "hv_gic_create");
    os_release(gic_config);

    // GIC inject callback (called from MMIO handlers)
    auto gic_inject = [](uint32_t intid, bool level) {
      if (level) {
        hv_gic_set_spi(intid, true);
      } else {
        hv_gic_set_spi(intid, false);
      }
    };

    // ── UART ──────────────────────────────────────────────────────────────────
    Pl011Uart uart;
    uart.set_console_limit(console_limit == 0 ? SIZE_MAX : static_cast<size_t>(console_limit));
    uart.set_echo_stdout(interactive);
    uart.set_gic_inject(gic_inject);

    // ── VirtioBlk ─────────────────────────────────────────────────────────────
    VirtioBlk blk(mem, rootfs_path, overlay_path, gic_inject);

    // ── VirtioNet ─────────────────────────────────────────────────────────────
    std::unique_ptr<VirtioNet> net_dev;
    if (use_vmnet) {
      if (guest_mac.empty()) guest_mac = "52:54:00:12:34:56";
      net_dev = std::make_unique<VirtioNet>(mem, guest_mac, gic_inject);
      net_dev->start(tap_name);
    }

    // ── Create vCPU(s) ────────────────────────────────────────────────────────
    // HVF requires one vCPU per thread; for simplicity we run 1 vCPU here.
    // Multi-vCPU support requires PSCI and is left as future work.
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t* exit_info = nullptr;
    CheckHv(hv_vcpu_create(&vcpu, &exit_info, nullptr), "hv_vcpu_create");

    // ── Set initial vCPU registers ────────────────────────────────────────────
    // PC = kernel load address (absolute IPA = kRamBase + kKernelOffset)
    // x0 = DTB IPA = kRamBase + kDtbOffset
    // x1 = x2 = x3 = 0
    // CPSR = EL1h (0x3C5)
    uint64_t entry_ipa = kRamBase + kernel.load_offset;
    uint64_t dtb_ipa   = kRamBase + kDtbOffset;
    hv_vcpu_set_reg(vcpu, HV_REG_PC,   entry_ipa);
    hv_vcpu_set_reg(vcpu, HV_REG_X0,   dtb_ipa);
    hv_vcpu_set_reg(vcpu, HV_REG_X1,   0);
    hv_vcpu_set_reg(vcpu, HV_REG_X2,   0);
    hv_vcpu_set_reg(vcpu, HV_REG_X3,   0);
    hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3C5); // EL1h, IRQs unmasked

    // Enable EL1 MMU / cache via system registers
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, 0x00C50078);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_HCR_EL2,   0x80000000); // RW=1 (AArch64 EL1)
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1,  0);

    // ── Run loop ──────────────────────────────────────────────────────────────
    GuestExitReq guest_exit;
    std::string exit_reason_name = "unknown";
    int exit_reason_code = -1;
    uint32_t runs = 0;

    uint64_t start_us = 0;
    {
      struct timespec ts{};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      start_us = uint64_t(ts.tv_sec)*1000000ULL + uint64_t(ts.tv_nsec)/1000ULL;
    }

    for (;;) {
      // Check timeout
      if (timeout_ms > 0) {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = uint64_t(ts.tv_sec)*1000000ULL + uint64_t(ts.tv_nsec)/1000ULL;
        if ((now - start_us) >= uint64_t(timeout_ms) * 1000ULL) {
          exit_reason_name = "timeout";
          exit_reason_code = 124;
          break;
        }
      }

      // Poll vmnet for incoming packets
      if (net_dev && net_dev->enabled() && net_dev->notify_readable()) {
        net_dev->poll_rx();
      }

      hv_return_t run_rc = hv_vcpu_run(vcpu);
      if (run_rc != HV_SUCCESS) {
        exit_reason_name = "hv-error";
        exit_reason_code = static_cast<int>(run_rc);
        break;
      }
      runs++;

      uint32_t reason = exit_info->reason;

      if (reason == HV_EXIT_REASON_CANCELED) {
        exit_reason_name = "canceled";
        exit_reason_code = 0;
        break;
      }

      if (reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
        // Deliver virtual timer interrupt (PPI 27 = INTID 27)
        hv_vcpu_set_vtimer_mask(vcpu, false);
        continue;
      }

      if (reason == HV_EXIT_REASON_EXCEPTION) {
        uint64_t syndrome = exit_info->exception.syndrome;
        uint32_t ec = EsrEC(syndrome);

        // EC=0x16: HVC / EC=0x17: SMC (PSCI)
        if (ec == 0x16 || ec == 0x17) {
          uint64_t x0 = 0;
          hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
          // PSCI CPU_OFF = 0x84000002 → exit
          if (x0 == 0x84000002ULL || x0 == 0x80000002ULL) {
            exit_reason_name = "hlt";
            exit_reason_code = 0;
            break;
          }
          // PSCI SYSTEM_OFF = 0x84000008
          if (x0 == 0x84000008ULL) {
            exit_reason_name = "shutdown";
            exit_reason_code = 0;
            break;
          }
          // Unsupported: return -1 (NOT_SUPPORTED)
          hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(-1));
          // Advance PC
          uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
          hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
          continue;
        }

        // EC=0x18: MSR/MRS / EC=0x1F: SError / EC=0x00: WFI
        if (ec == 0x01) {
          // WFI — advance PC and yield briefly
          uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
          hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
          // Poll vmnet while idle
          if (net_dev && net_dev->enabled()) net_dev->poll_rx();
          struct timespec ts{0, 100000}; // 100µs
          nanosleep(&ts, nullptr);
          continue;
        }

        // EC=0x24: Data abort (MMIO)
        // EC=0x20: Instruction abort
        if (ec == 0x24 || ec == 0x20) {
          uint64_t ipa = exit_info->exception.physical_address;

          if (ec == 0x24 && EsrISV(syndrome)) {
            bool is_write = EsrWnR(syndrome);
            uint32_t sas   = EsrSAS(syndrome);
            uint32_t len   = 1u << sas; // bytes
            uint32_t srt   = EsrSRT(syndrome);

            uint8_t data[8] = {};
            if (is_write) {
              uint64_t reg_val = 0;
              hv_vcpu_get_reg(vcpu, static_cast<hv_reg_t>(srt), &reg_val);
              for (uint32_t i = 0; i < len && i < 8; i++) {
                data[i] = static_cast<uint8_t>(reg_val >> (8*i));
              }
            }

            // Dispatch MMIO
            if (ipa >= kUartBase && ipa < kUartBase + 0x1000) {
              uint64_t off = ipa - kUartBase;
              if (is_write) uart.write_mmio(off, data, len);
              else uart.read_mmio(off, data, len);
            } else if (ipa >= kVirtioBlkBase && ipa < kVirtioBlkBase + kVirtioStride) {
              if (is_write) blk.write_mmio(ipa, data, len);
              else blk.read_mmio(ipa, data, len);
            } else if (net_dev && ipa >= kVirtioNetBase && ipa < kVirtioNetBase + kVirtioStride) {
              if (is_write) net_dev->write_mmio(ipa, data, len);
              else net_dev->read_mmio(ipa, data, len);
            } else if (ipa >= kExitDevBase && ipa < kExitDevBase + 0x1000) {
              if (is_write) {
                guest_exit.requested = true;
                guest_exit.status = data[0];
              }
            } else {
              // Unknown MMIO: return 0 on reads
              memset(data, 0, sizeof(data));
            }

            if (!is_write) {
              uint64_t reg_val = 0;
              for (uint32_t i = 0; i < len && i < 8; i++) {
                reg_val |= uint64_t(data[i]) << (8*i);
              }
              if (EsrSSE(syndrome)) {
                // Sign extend
                uint64_t sign_bit = uint64_t(1) << (8*len - 1);
                if (reg_val & sign_bit) reg_val |= ~((sign_bit << 1) - 1);
              }
              hv_vcpu_set_reg(vcpu, static_cast<hv_reg_t>(srt), reg_val);
            }

            uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);

            if (guest_exit.requested) {
              exit_reason_name = "guest-exit";
              exit_reason_code = static_cast<int>(guest_exit.status);
              break;
            }
            continue;
          }

          // No ISV: can't decode access size/register — skip instruction
          uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
          hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
          continue;
        }

        // Unknown exception: log and continue
        uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        continue;
      }

      // Unhandled exit
      exit_reason_name = "unknown-exit-" + std::to_string(reason);
      exit_reason_code = static_cast<int>(reason);
      break;
    } // run loop

    // ── Cleanup ───────────────────────────────────────────────────────────────
    hv_vcpu_destroy(vcpu);
    if (net_dev) net_dev->stop_vmnet();
    hv_vm_unmap(kRamBase, mem_bytes);

    // ── Build result ──────────────────────────────────────────────────────────
    napi_value result = MakeObject(env);
    SetProp(env, result, "exitReason",     MakeString(env, exit_reason_name));
    SetProp(env, result, "exitReasonCode", MakeU32(env, static_cast<uint32_t>(exit_reason_code)));
    SetProp(env, result, "runs",           MakeU32(env, runs));
    SetProp(env, result, "console",        MakeString(env, uart.console()));
    return result;

  } catch (const std::exception& e) {
    return ThrowError(env, std::string("hvf RunVm: ") + e.what());
  }
}

} // namespace

// ─── Module init ──────────────────────────────────────────────────────────────
extern "C" {

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn_probe, fn_run;
  napi_create_function(env, "probeHvf", NAPI_AUTO_LENGTH, ProbeHvf, nullptr, &fn_probe);
  napi_create_function(env, "runVm",    NAPI_AUTO_LENGTH, RunVm,    nullptr, &fn_run);
  napi_set_named_property(env, exports, "probeHvf", fn_probe);
  napi_set_named_property(env, exports, "runVm",    fn_run);
  return exports;
}

NAPI_MODULE(node_vmm_native, Init)

} // extern "C"
