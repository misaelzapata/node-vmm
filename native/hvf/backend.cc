// Apple Hypervisor.framework backend for node-vmm (ARM64 macOS 15.0+)
#include <node_api.h>

#include <Hypervisor/Hypervisor.h>
#include <vmnet/vmnet.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <termios.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if __has_include(<libslirp.h>)
#include <libslirp.h>
#define NODE_VMM_HAVE_SLIRP 1
#elif __has_include(<slirp/libslirp.h>)
#include <slirp/libslirp.h>
#define NODE_VMM_HAVE_SLIRP 1
#else
#define NODE_VMM_HAVE_SLIRP 0
#endif

namespace {

// ─── QEMU ARM virt low MMIO layout ───────────────────────────────────────────
// Keep this table aligned with QEMU's hw/arm/virt.c for probe-sensitive devices.
constexpr uint64_t kGicDistBase       = 0x08000000ULL;
constexpr uint64_t kGicRedistBase     = 0x080A0000ULL;
constexpr uint64_t kUartBase          = 0x09000000ULL;
constexpr uint64_t kPl031RtcBase      = 0x09010000ULL;
constexpr uint64_t kFwCfgBase         = 0x09020000ULL;
constexpr uint64_t kPl061GpioBase     = 0x09030000ULL;
constexpr uint64_t kVirtioMmioBase    = 0x0A000000ULL;
constexpr uint64_t kVirtioMmioSize    = 0x200ULL;
constexpr uint64_t kVirtioMmioStride  = 0x200ULL;
constexpr uint32_t kVirtioMmioCount   = 32;
constexpr uint32_t kVirtioBlkSlot     = 0;
constexpr uint32_t kVirtioNetSlot     = 1;
constexpr uint64_t kVirtioBlkBase     = kVirtioMmioBase + kVirtioBlkSlot * kVirtioMmioStride;
constexpr uint64_t kVirtioNetBase     = kVirtioMmioBase + kVirtioNetSlot * kVirtioMmioStride;
constexpr uint64_t kExitDevBase       = 0x0B000000ULL;
constexpr uint64_t kPcieMmioBase      = 0x10000000ULL;
constexpr uint64_t kPcieMmioSize      = 0x2EFF0000ULL;
constexpr uint64_t kPciePioBase       = 0x3EFF0000ULL;
constexpr uint64_t kPciePioSize       = 0x00010000ULL;
constexpr uint64_t kPcieEcamBase      = 0x3F000000ULL;
constexpr uint64_t kPcieEcamSize      = 0x01000000ULL;
constexpr uint64_t kRamBase           = 0x40000000ULL;
constexpr uint64_t kDtbOffset         = 0x00000000ULL; // DTB at kRamBase + 0
constexpr uint64_t kKernelOffset      = 0x00400000ULL; // Kernel at kRamBase + 4MiB
constexpr uint32_t kGicRedistributorBytesPerCpu = 0x20000;

// GIC SPI interrupt IDs (INTID = 32 + SPI_N)
constexpr uint32_t kUartSpi        = 1;
constexpr uint32_t kRtcSpi         = 2;
constexpr uint32_t kPcieFirstSpi   = 3;
constexpr uint32_t kPcieIntxCount  = 4;
constexpr uint32_t kGpioSpi        = 7;
constexpr uint32_t kVirtioFirstSpi = 16;
constexpr uint32_t kUartIntid      = 32 + kUartSpi;
constexpr uint32_t kRtcIntid       = 32 + kRtcSpi;
constexpr uint32_t kGpioIntid      = 32 + kGpioSpi;
constexpr uint32_t kVirtioBlkIntid = 32 + kVirtioFirstSpi + kVirtioBlkSlot;
constexpr uint32_t kVirtioNetIntid = 32 + kVirtioFirstSpi + kVirtioNetSlot;
constexpr uint64_t kVtimerIntidBit = 1ULL << 27;

constexpr uint64_t VirtioMmioBaseForSlot(uint32_t slot) {
  return kVirtioMmioBase + static_cast<uint64_t>(slot) * kVirtioMmioStride;
}

constexpr uint32_t VirtioMmioIntidForSlot(uint32_t slot) {
  return 32 + kVirtioFirstSpi + slot;
}

constexpr uint32_t kPciRangeIoPort = 0x01000000;
constexpr uint32_t kPciRangeMmio32 = 0x02000000;

constexpr uint32_t kMaxQueueSize   = 256;
constexpr uint32_t kMaxFrameSize   = 1518;
constexpr uint32_t kVirtioStatusDeviceNeedsReset = 0x40;
constexpr int32_t kControlRun = 0;
constexpr int32_t kControlPause = 1;
constexpr int32_t kControlStop = 2;
constexpr int32_t kControlStateStarting = 0;
constexpr int32_t kControlStateRunning = 1;
constexpr int32_t kControlStatePaused = 2;
constexpr int32_t kControlStateStopping = 3;
constexpr int32_t kControlStateExited = 4;

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

bool HvfDebugEnabled() {
  const char* value = getenv("NODE_VMM_HVF_DEBUG");
  return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

bool HvfDebugUartEnabled() {
  const char* value = getenv("NODE_VMM_HVF_DEBUG_UART");
  return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

bool IsQemuVirtioMmioAddress(uint64_t ipa) {
  return ipa >= kVirtioMmioBase &&
         ipa < kVirtioMmioBase + uint64_t(kVirtioMmioCount) * kVirtioMmioStride;
}

uint64_t VirtioMmioOffset(uint64_t ipa) {
  return (ipa - kVirtioMmioBase) % kVirtioMmioStride;
}

uint32_t PcieBusCount() {
  return static_cast<uint32_t>(kPcieEcamSize / 0x100000ULL);
}

uint32_t GicRedistributorBytes(uint32_t cpus) {
  return kGicRedistributorBytesPerCpu * cpus;
}

void FillRandom(uint8_t* data, size_t len) {
  arc4random_buf(data, len);
}

uint64_t RandomU64() {
  uint8_t data[8]{};
  FillRandom(data, sizeof(data));
  uint64_t out = 0;
  for (size_t i = 0; i < sizeof(data); i++) {
    out |= uint64_t(data[i]) << (8 * i);
  }
  return out;
}

std::string Hex64(uint64_t value) {
  char buf[20];
  snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
  return buf;
}

std::string UnsupportedExceptionReason(const char* kind, uint32_t ec, uint64_t syndrome,
                                       uint64_t ipa, uint64_t pc) {
  return std::string("unsupported-") + kind +
         " ec=" + Hex64(ec) +
         " syndrome=" + Hex64(syndrome) +
         " ipa=" + Hex64(ipa) +
         " pc=" + Hex64(pc);
}

uint64_t MonotonicMicros() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000ULL + uint64_t(ts.tv_nsec) / 1000ULL;
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
	    uint64_t off = offset(gpa, len);
	    return data_ + off;
	  }
	  const uint8_t* ptr(uint64_t gpa, uint64_t len) const {
	    uint64_t off = offset(gpa, len);
	    return data_ + off;
	  }
	  uint8_t* guest_ptr(uint64_t gpa, uint64_t len) {
	    uint64_t off = guest_offset(gpa, len);
	    return data_ + off;
	  }
	  const uint8_t* guest_ptr(uint64_t gpa, uint64_t len) const {
	    uint64_t off = guest_offset(gpa, len);
	    return data_ + off;
	  }
	  bool contains_guest_range(uint64_t gpa, uint64_t len) const {
	    if (gpa < kRamBase) return false;
	    uint64_t off = gpa - kRamBase;
	    return off <= size_ && len <= size_ - off;
	  }
	  size_t size() const { return size_; }
	  uint8_t* data() const { return data_; }
	 private:
	  uint64_t offset(uint64_t gpa, uint64_t len) const {
	    uint64_t off = gpa;
    if (gpa >= kRamBase) {
      off = gpa - kRamBase;
    }
    Check(off <= size_ && len <= size_ - off,
	          "guest memory access out of bounds @" + std::to_string(gpa));
	    return off;
	  }

	  uint64_t guest_offset(uint64_t gpa, uint64_t len) const {
	    Check(contains_guest_range(gpa, len),
	          "guest physical memory access out of bounds @" + std::to_string(gpa));
	    return gpa - kRamBase;
	  }

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
	    (void)has_net;
	    buf_.clear();
	    str_buf_.clear();

	    constexpr uint32_t kHeaderSize = 40;
	    constexpr uint32_t kReserveMapSize = 16;
	    constexpr uint32_t kStructOffset = kHeaderSize + kReserveMapSize;

	    buf_.resize(kStructOffset, 0);

	    // FDT_BEGIN_NODE for root
	    AppendU32(FDT_BEGIN_NODE);
	    AppendStr(""); // root name = ""
    Align4();

    // root properties
	    AppendPropU32("#address-cells", 2);
	    AppendPropU32("#size-cells", 2);
	    AppendPropStr("compatible", "linux,dummy-virt");
	    AppendPropStr("model", "node-vmm-hvf");
	    AppendPropU32("interrupt-parent", 1);
	    AppendPropFlag("dma-coherent");

	    // /aliases
	    AppendU32(FDT_BEGIN_NODE);
	    AppendStr("aliases");
	    Align4();
	    AppendPropStr("serial0", "/pl011@9000000");
	    AppendU32(FDT_END_NODE);

	    // /chosen
	    AppendU32(FDT_BEGIN_NODE);
	    AppendStr("chosen");
	    Align4();
    AppendPropStr("bootargs", cmdline);
    AppendPropStr("stdout-path", "/pl011@9000000");
    AppendPropU64("kaslr-seed", RandomU64());
    {
      uint8_t seed[32];
      FillRandom(seed, sizeof(seed));
      AppendPropRaw("rng-seed", seed, sizeof(seed));
    }
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
	      AppendPropU32("reg", i);
	      if (cpus > 1) {
	        AppendPropStr("enable-method", "psci");
	      }
	      AppendU32(FDT_END_NODE);
	    }
	    AppendU32(FDT_END_NODE); // /cpus

    // /psci
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("psci");
    Align4();
    AppendPropStringList("compatible", {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"});
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
    AppendPropU32("#address-cells", 2);
    AppendPropU32("#size-cells", 2);
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
      WriteU32BE(regbuf+28, GicRedistributorBytes(cpus));
      AppendPropRaw("reg", regbuf, sizeof(regbuf));
	    }
	    AppendU32(FDT_END_NODE);

	    // ARM architected timer. HVF provides the virtual timer; Linux needs this
	    // node to program clock events instead of falling back to busy waits.
	    AppendU32(FDT_BEGIN_NODE);
	    AppendStr("timer");
	    Align4();
	    AppendPropStringList("compatible", {"arm,armv8-timer", "arm,armv7-timer"});
	    {
	      uint8_t irq[48];
      // <GIC_PPI N IRQ_TYPE_LEVEL_HIGH>. GICv3 does not use the GICv2 PPI CPU mask.
      const uint32_t ppis[] = {13, 14, 11, 10};
      for (size_t i = 0; i < 4; i++) {
        WriteU32BE(irq + i * 12 + 0, 1);
        WriteU32BE(irq + i * 12 + 4, ppis[i]);
        WriteU32BE(irq + i * 12 + 8, 4);
      }
	      AppendPropRaw("interrupts", irq, sizeof(irq));
	    }
	    AppendPropFlag("always-on");
	    AppendU32(FDT_END_NODE);

	    // /pl011 UART
	    AppendU32(FDT_BEGIN_NODE);
	    AppendStr("pl011@9000000");
	    Align4();
	    AppendPropStringList("compatible", {"arm,pl011", "arm,primecell"});
	    AppendPropReg("reg", kUartBase, 0x1000);
	    // interrupts = <GIC_SPI QEMU_UART_SPI IRQ_TYPE_LEVEL_HIGH>
	    {
      uint8_t irq[12];
      WriteU32BE(irq+0, 0); // GIC_SPI
      WriteU32BE(irq+4, kUartSpi);
      WriteU32BE(irq+8, 4); // level high
      AppendPropRaw("interrupts", irq, sizeof(irq));
	    }
	    AppendPropU32("interrupt-parent", 1);
	    AppendPropStringList("clock-names", {"uartclk", "apb_pclk"});
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

    // PL031 RTC
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("pl031@9010000");
    Align4();
    AppendPropStringList("compatible", {"arm,pl031", "arm,primecell"});
    AppendPropReg("reg", kPl031RtcBase, 0x1000);
    {
      uint8_t irq[12];
      WriteU32BE(irq+0, 0); // GIC_SPI
      WriteU32BE(irq+4, kRtcSpi);
      WriteU32BE(irq+8, 4); // level high
      AppendPropRaw("interrupts", irq, sizeof(irq));
    }
    AppendPropU32("interrupt-parent", 1);
    AppendPropU32("clocks", 3);
    AppendPropStr("clock-names", "apb_pclk");
    AppendU32(FDT_END_NODE);

    // PL061 GPIO and the QEMU virt power key child node.
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("pl061@9030000");
    Align4();
    AppendPropStringList("compatible", {"arm,pl061", "arm,primecell"});
    AppendPropReg("reg", kPl061GpioBase, 0x1000);
    AppendPropU32("#gpio-cells", 2);
    AppendPropFlag("gpio-controller");
    {
      uint8_t irq[12];
      WriteU32BE(irq+0, 0); // GIC_SPI
      WriteU32BE(irq+4, kGpioSpi);
      WriteU32BE(irq+8, 4); // level high
      AppendPropRaw("interrupts", irq, sizeof(irq));
    }
    AppendPropU32("interrupt-parent", 1);
    AppendPropU32("clocks", 3);
    AppendPropStr("clock-names", "apb_pclk");
    AppendPropU32("phandle", 4);
    AppendU32(FDT_END_NODE);

    AppendU32(FDT_BEGIN_NODE);
    AppendStr("gpio-keys");
    Align4();
    AppendPropStr("compatible", "gpio-keys");
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("poweroff");
    Align4();
    AppendPropStr("label", "GPIO Key Poweroff");
    AppendPropU32("linux,code", 116); // KEY_POWER
    {
      uint8_t gpios[12];
      WriteU32BE(gpios+0, 4);
      WriteU32BE(gpios+4, 3);
      WriteU32BE(gpios+8, 0);
      AppendPropRaw("gpios", gpios, sizeof(gpios));
    }
    AppendU32(FDT_END_NODE);
    AppendU32(FDT_END_NODE);

    // fw_cfg-mmio
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("fw-cfg@9020000");
    Align4();
    AppendPropStr("compatible", "qemu,fw-cfg-mmio");
    AppendPropReg("reg", kFwCfgBase, 0x18);
    AppendPropFlag("dma-coherent");
    AppendU32(FDT_END_NODE);

    // QEMU creates probe-safe virtio-mmio transports up front. Empty
    // transports report device ID 0; Linux probes them without binding.
    for (uint32_t slot = 0; slot < kVirtioMmioCount; slot++) {
      AppendVirtioNode(kVirtioMmioBase + slot * kVirtioMmioStride, kVirtioFirstSpi + slot);
    }

    // Empty generic PCIe ECAM host bridge. This exposes QEMU's virt bus layout
    // while keeping PCI enumeration probe-safe until virtio-pci/MSI exist.
    AppendU32(FDT_BEGIN_NODE);
    AppendStr("pcie@10000000");
    Align4();
    AppendPropStr("compatible", "pci-host-ecam-generic");
    AppendPropStr("device_type", "pci");
    AppendPropU32("#address-cells", 3);
    AppendPropU32("#size-cells", 2);
    AppendPropU32("linux,pci-domain", 0);
    {
      uint8_t bus_range[8];
      WriteU32BE(bus_range+0, 0);
      WriteU32BE(bus_range+4, PcieBusCount() - 1);
      AppendPropRaw("bus-range", bus_range, sizeof(bus_range));
    }
    AppendPropFlag("dma-coherent");
    AppendPropReg("reg", kPcieEcamBase, kPcieEcamSize);
    {
      uint8_t ranges[56];
      WriteU32BE(ranges+0,  kPciRangeIoPort);
      WriteU32BE(ranges+4,  0);
      WriteU32BE(ranges+8,  0);
      WriteU32BE(ranges+12, uint32_t(kPciePioBase >> 32));
      WriteU32BE(ranges+16, uint32_t(kPciePioBase));
      WriteU32BE(ranges+20, uint32_t(kPciePioSize >> 32));
      WriteU32BE(ranges+24, uint32_t(kPciePioSize));
      WriteU32BE(ranges+28, kPciRangeMmio32);
      WriteU32BE(ranges+32, uint32_t(kPcieMmioBase >> 32));
      WriteU32BE(ranges+36, uint32_t(kPcieMmioBase));
      WriteU32BE(ranges+40, uint32_t(kPcieMmioBase >> 32));
      WriteU32BE(ranges+44, uint32_t(kPcieMmioBase));
      WriteU32BE(ranges+48, uint32_t(kPcieMmioSize >> 32));
      WriteU32BE(ranges+52, uint32_t(kPcieMmioSize));
      AppendPropRaw("ranges", ranges, sizeof(ranges));
    }
    AppendPropU32("#interrupt-cells", 1);
    {
      uint8_t map[4 * kPcieIntxCount * 10 * sizeof(uint32_t)]{};
      size_t off = 0;
      for (uint32_t slot = 0; slot < 4; slot++) {
        for (uint32_t pin = 0; pin < kPcieIntxCount; pin++) {
          const uint32_t spi = kPcieFirstSpi + ((pin + slot) % kPcieIntxCount);
          const uint32_t cells[] = {
            slot << 11, 0, 0, pin + 1,
            1, 0, 0, 0, spi, 4,
          };
          for (uint32_t cell : cells) {
            WriteU32BE(map + off, cell);
            off += sizeof(uint32_t);
          }
        }
      }
      AppendPropRaw("interrupt-map", map, sizeof(map));
    }
    {
      uint8_t mask[16];
      WriteU32BE(mask+0,  0x1800); // devfn mask for slots 0..3
      WriteU32BE(mask+4,  0);
      WriteU32BE(mask+8,  0);
      WriteU32BE(mask+12, 0x7);    // PCI INTx pin mask
      AppendPropRaw("interrupt-map-mask", mask, sizeof(mask));
    }
    AppendU32(FDT_END_NODE);

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
	    uint32_t struct_size = static_cast<uint32_t>(buf_.size() - kStructOffset);
	    uint32_t strings_off = static_cast<uint32_t>(buf_.size());
	    buf_.insert(buf_.end(), str_buf_.begin(), str_buf_.end());
	    Align4buf();
	    uint32_t total = static_cast<uint32_t>(buf_.size());

    // Fill header
	    uint8_t* h = buf_.data();
	    WriteU32BE(h+0,  0xD00DFEED); // magic
	    WriteU32BE(h+4,  total);
	    WriteU32BE(h+8,  kStructOffset);
	    WriteU32BE(h+12, strings_off);
	    WriteU32BE(h+16, kHeaderSize);
	    WriteU32BE(h+20, 17);         // version
	    WriteU32BE(h+24, 16);         // last_comp_version
	    WriteU32BE(h+28, 0);          // boot_cpuid_phys
	    WriteU32BE(h+32, static_cast<uint32_t>(str_buf_.size())); // size_dt_strings
	    WriteU32BE(h+36, struct_size); // size_dt_struct

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
	    if (len > 0) {
	      Check(data != nullptr, "FDT property data is null");
	      buf_.insert(buf_.end(), data, data + len);
	    }
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
	    AppendPropRaw(name, reinterpret_cast<const uint8_t*>(value.c_str()), static_cast<uint32_t>(value.size() + 1));
	  }

	  void AppendPropStringList(const char* name, std::initializer_list<const char*> values) {
	    std::vector<uint8_t> data;
	    for (const char* value : values) {
	      size_t len = strlen(value) + 1;
	      data.insert(data.end(), reinterpret_cast<const uint8_t*>(value), reinterpret_cast<const uint8_t*>(value) + len);
	    }
	    AppendPropRaw(name, data.data(), static_cast<uint32_t>(data.size()));
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
    AppendPropReg("reg", base, kVirtioMmioSize);
    AppendPropU32("interrupt-parent", 1);
    uint8_t irq[12];
    WriteU32BE(irq+0, 0); // GIC_SPI
    WriteU32BE(irq+4, spi);
    WriteU32BE(irq+8, 1); // edge rising, matching QEMU virtio-mmio
    AppendPropRaw("interrupts", irq, sizeof(irq));
    AppendPropFlag("dma-coherent");
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

constexpr uint32_t kSysRegOp0Shift = 20;
constexpr uint32_t kSysRegOp1Shift = 14;
constexpr uint32_t kSysRegCrnShift = 10;
constexpr uint32_t kSysRegCrmShift = 1;
constexpr uint32_t kSysRegOp2Shift = 17;
constexpr uint32_t SysReg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
  return (op0 << kSysRegOp0Shift) |
         (op1 << kSysRegOp1Shift) |
         (crn << kSysRegCrnShift) |
         (crm << kSysRegCrmShift) |
         (op2 << kSysRegOp2Shift);
}

constexpr uint32_t kSysRegMask = SysReg(0x3, 0x7, 0xF, 0xF, 0x7);
constexpr uint32_t kSysRegOslArEl1 = SysReg(2, 0, 1, 0, 4);
constexpr uint32_t kSysRegOslSrEl1 = SysReg(2, 0, 1, 1, 4);
constexpr uint32_t kSysRegOsdLrEl1 = SysReg(2, 0, 1, 3, 4);
constexpr uint32_t kSysRegLorcEl1 = SysReg(3, 0, 10, 4, 3);
constexpr uint32_t kSysRegCntPctEl0 = SysReg(3, 3, 14, 0, 1);
constexpr uint32_t kSysRegCntpCtlEl0 = SysReg(3, 3, 14, 2, 1);

static bool HandleSystemRegisterTrap(hv_vcpu_t vcpu, uint64_t syndrome) {
  bool is_read = (syndrome & 1) != 0;
  uint32_t rt = static_cast<uint32_t>((syndrome >> 5) & 0x1F);
  uint32_t reg = static_cast<uint32_t>(syndrome) & kSysRegMask;
  uint64_t value = 0;
  bool handled = false;

  if (is_read) {
    switch (reg) {
      case kSysRegOslSrEl1:
      case kSysRegOsdLrEl1:
      case kSysRegLorcEl1:
        value = 0;
        handled = true;
        break;
      case kSysRegCntPctEl0:
        value = MonotonicMicros() * 1000ULL;
        handled = true;
        break;
      default:
        break;
    }
    if (handled && rt < 31) {
      CheckHv(hv_vcpu_set_reg(vcpu, static_cast<hv_reg_t>(rt), value),
              "hv_vcpu_set_reg sysreg read");
    }
    return handled;
  }

  switch (reg) {
    case kSysRegOslArEl1:
    case kSysRegOsdLrEl1:
    case kSysRegLorcEl1:
    case kSysRegCntpCtlEl0:
      return true;
    default:
      return false;
  }
}

constexpr uint64_t kPsciVersion       = 0x84000000ULL;
constexpr uint64_t kPsciCpuSuspend32  = 0x84000001ULL;
constexpr uint64_t kPsciCpuOff        = 0x84000002ULL;
constexpr uint64_t kPsciCpuOn32       = 0x84000003ULL;
constexpr uint64_t kPsciAffinityInfo32 = 0x84000004ULL;
constexpr uint64_t kPsciMigrateInfoType = 0x84000006ULL;
constexpr uint64_t kPsciSystemOff     = 0x84000008ULL;
constexpr uint64_t kPsciSystemReset   = 0x84000009ULL;
constexpr uint64_t kPsciFeatures      = 0x8400000AULL;
constexpr uint64_t kPsciCpuSuspend64  = 0xC4000001ULL;
constexpr uint64_t kPsciCpuOn64       = 0xC4000003ULL;
constexpr uint64_t kPsciAffinityInfo64 = 0xC4000004ULL;

constexpr int64_t kPsciSuccess = 0;
constexpr int64_t kPsciNotSupported = -1;
constexpr int64_t kPsciInvalidParams = -2;
constexpr int64_t kPsciDenied = -3;
constexpr int64_t kPsciAlreadyOn = -4;

static bool PsciFeatureSupported(uint64_t function_id) {
  switch (function_id) {
    case kPsciVersion:
    case kPsciCpuOff:
    case kPsciCpuOn32:
    case kPsciCpuOn64:
    case kPsciAffinityInfo32:
    case kPsciAffinityInfo64:
    case kPsciMigrateInfoType:
    case kPsciSystemOff:
    case kPsciSystemReset:
    case kPsciFeatures:
      return true;
    default:
      return false;
  }
}

// ─── PL011 UART ───────────────────────────────────────────────────────────────
class Pl011Uart {
 public:
  Pl011Uart() = default;

  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len == 0 || len > 4) return;
    uint32_t val = 0;
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case 0x000: // DR
        if (!rx_.empty()) {
          val = rx_.front(); rx_.pop_front();
          if (debug_uart_) {
            fprintf(stderr, "[node-vmm hvf] uart rx read 0x%02x remaining=%zu\n", val, rx_.size());
          }
          if (rx_.empty()) ris_ &= ~kRxInterrupts;
          update_irq_locked();
        }
        break;
      case 0x018: // FR
        val = 0x90; // TXFE | RXFE (TX empty, RX empty by default)
        if (!rx_.empty()) val &= ~0x10; // clear RXFE
        val &= ~0x20; // clear TXFF (always room to write)
        break;
      case 0x024: // IBRD
        val = ibrd_;
        break;
      case 0x028: // FBRD
        val = fbrd_;
        break;
      case 0x02C: // LCR_H
        val = lcr_h_;
        break;
      case 0x030: // CR
        val = cr_;
        break;
      case 0x034: // IFLS
        val = ifls_;
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
      case 0x048: // DMACR
        val = dmacr_;
        break;
      case 0xFE0: val = 0x11; break; // Peripheral ID0
      case 0xFE4: val = 0x10; break; // Peripheral ID1
      case 0xFE8: val = 0x14; break; // Peripheral ID2
      case 0xFEC: val = 0x00; break; // Peripheral ID3
      case 0xFF0: val = 0x0D; break; // PrimeCell ID0
      case 0xFF4: val = 0xF0; break; // PrimeCell ID1
      case 0xFF8: val = 0x05; break; // PrimeCell ID2
      case 0xFFC: val = 0xB1; break; // PrimeCell ID3
      default:
        break;
    }
    WriteU32(data, val);
  }

	  void write_mmio(uint64_t offset, const uint8_t* data, uint32_t len) {
	    if (len == 0 || len > 4) return;
	    std::lock_guard<std::mutex> lk(mu_);
	    switch (offset) {
	      case 0x000: { // DR - transmit
	        if (len < 1) return;
	        char c = static_cast<char>(data[0]);
	        handle_tx_locked(c);
	        ris_ |= 0x20; // TXRIS
	        update_irq_locked();
	        break;
	      }
	      default: break;
	    }
	    uint32_t val = ReadU32(data);
	    switch (offset) {
	      case 0x024: ibrd_ = val & 0xFFFF; break;
	      case 0x028: fbrd_ = val & 0x3F; break;
	      case 0x02C: lcr_h_ = val; break;
	      case 0x030: cr_ = val; break;
	      case 0x034: ifls_ = val & 0x3F; update_irq_locked(); break;
	      case 0x038: imsc_ = val; update_irq_locked(); break;
	      case 0x044: ris_ &= ~val; update_irq_locked(); break; // ICR
	      case 0x048: dmacr_ = val; break; // DMACR is accepted but not implemented.
	      default: break;
	    }
  }

  void set_console_limit(size_t limit) { console_limit_ = limit; }
  void set_echo_stdout(bool echo) { echo_stdout_ = echo; }
  void set_debug(bool debug) { debug_uart_ = debug; }
  void set_console_output_notify(std::function<void()> fn) { console_output_notify_ = std::move(fn); }

  std::string console() const {
    std::lock_guard<std::mutex> lk(mu_);
    return console_;
  }

  void inject_char(char c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (rx_.size() < 4096) rx_.push_back(static_cast<uint8_t>(c));
    ris_ |= kRxInterrupts;
    update_irq_locked();
  }

  void inject_bytes(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(mu_);
    if (debug_uart_) {
      fprintf(stderr, "[node-vmm hvf] uart inject len=%zu\n", len);
    }
    for (size_t i = 0; i < len && rx_.size() < 4096; i++) {
      rx_.push_back(data[i]);
    }
    if (len > 0) ris_ |= kRxInterrupts;
    update_irq_locked();
  }

  void refresh_irq() {
    std::lock_guard<std::mutex> lk(mu_);
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
	  void emit_tx_locked(char c) {
	    if (console_.size() < console_limit_) {
	      console_.push_back(c);
	    }
	    if (echo_stdout_) {
	      if (!console_output_seen_) {
	        console_output_seen_ = true;
	        if (console_output_notify_) {
	          console_output_notify_();
	        }
	      }
	      write(STDOUT_FILENO, &c, 1);
	    }
	  }

	  void handle_tx_locked(char c) {
	    if (echo_stdout_) {
	      emit_tx_locked(c);
	      return;
	    }
	    static const std::string query = "\x1b[6n";
	    if (!terminal_query_.empty() || c == 0x1B) {
	      terminal_query_.push_back(c);
	      if (query.rfind(terminal_query_, 0) == 0) {
	        if (terminal_query_ == query) {
	          static const char response[] = "\x1b[1;1R";
	          for (size_t i = sizeof(response) - 1; i > 0 && rx_.size() < 4096; i--) {
	            rx_.push_front(static_cast<uint8_t>(response[i - 1]));
	          }
	          ris_ |= kRxInterrupts;
	          terminal_query_.clear();
	        }
	        return;
	      }
	      for (char pending : terminal_query_) {
	        emit_tx_locked(pending);
	      }
	      terminal_query_.clear();
	      return;
	    }
	    emit_tx_locked(c);
	  }

	  void update_irq_locked() {
	    if (!rx_.empty()) {
	      ris_ |= kRxInterrupts;
	    } else {
	      ris_ &= ~kRxInterrupts;
	    }
	    bool level = (ris_ & imsc_) != 0;
	    if (gic_inject_) gic_inject_(kUartIntid, level);
	  }

  mutable std::mutex mu_;
	  std::deque<uint8_t> rx_;
	  std::string console_;
	  std::string terminal_query_;
  size_t console_limit_{0};
	  bool echo_stdout_{false};
	  bool console_output_seen_{false};
	  std::function<void()> console_output_notify_;
	  bool debug_uart_{false};
  uint32_t cr_{0x301}; // UARTEN|TXE|RXE
  uint32_t ibrd_{13}; // 24MHz / (16 * 115200) ~= 13.02
  uint32_t fbrd_{1};
  uint32_t lcr_h_{0x70}; // 8-bit words, FIFO enabled
  uint32_t ifls_{0x12};  // QEMU reset value: RX/TX interrupt at half FIFO
  uint32_t dmacr_{0};
  uint32_t imsc_{0};
  uint32_t ris_{0};
  std::function<void(uint32_t, bool)> gic_inject_;

  static constexpr uint32_t kRxInterrupts = 0x10 | 0x40; // RXRIS | RTRIS
};

// ─── PL031 RTC ────────────────────────────────────────────────────────────────
class Pl031Rtc {
 public:
  Pl031Rtc() = default;

  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len == 0 || len > 4) return;
    uint32_t val = 0;
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case 0x000: // DR
        val = current_seconds_locked();
        break;
      case 0x004: // MR
        val = match_;
        break;
      case 0x008: // LR
        val = current_seconds_locked();
        break;
      case 0x00C: // CR
        val = cr_;
        break;
      case 0x010: // IMSC
        val = imsc_;
        break;
      case 0x014: // RIS
        val = ris_;
        break;
      case 0x018: // MIS
        val = ris_ & imsc_;
        break;
      case 0xFE0: val = 0x31; break; // Peripheral ID0
      case 0xFE4: val = 0x10; break; // Peripheral ID1
      case 0xFE8: val = 0x14; break; // Peripheral ID2
      case 0xFEC: val = 0x00; break; // Peripheral ID3
      case 0xFF0: val = 0x0D; break; // PrimeCell ID0
      case 0xFF4: val = 0xF0; break; // PrimeCell ID1
      case 0xFF8: val = 0x05; break; // PrimeCell ID2
      case 0xFFC: val = 0xB1; break; // PrimeCell ID3
      default:
        break;
    }
    WriteU32(data, val);
  }

  void write_mmio(uint64_t offset, const uint8_t* data, uint32_t len) {
    if (len == 0 || len > 4) return;
    uint32_t val = ReadU32(data);
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case 0x004: // MR
        match_ = val;
        ris_ = (match_ != 0 && current_seconds_locked() >= match_) ? 1 : 0;
        break;
      case 0x008: { // LR
        time_t now = time(nullptr);
        offset_seconds_ = int64_t(val) - int64_t(now < 0 ? 0 : now);
        ris_ = (match_ != 0 && val >= match_) ? 1 : 0;
        break;
      }
      case 0x00C: // CR
        cr_ = val & 1;
        break;
      case 0x010: // IMSC
        imsc_ = val & 1;
        break;
      case 0x01C: // ICR
        ris_ &= ~val;
        break;
      default:
        break;
    }
  }

 private:
  uint32_t current_seconds_locked() const {
    time_t now = time(nullptr);
    int64_t seconds = int64_t(now < 0 ? 0 : now) + offset_seconds_;
    if (seconds < 0) return 0;
    if (seconds > UINT32_MAX) return UINT32_MAX;
    return static_cast<uint32_t>(seconds);
  }

  mutable std::mutex mu_;
  int64_t offset_seconds_{0};
  uint32_t match_{0};
  uint32_t cr_{1};
  uint32_t imsc_{0};
  uint32_t ris_{0};
};

// ─── fw_cfg MMIO ──────────────────────────────────────────────────────────────
class FwCfgMmio {
 public:
  explicit FwCfgMmio(uint16_t cpus = 1) : cpus_(cpus) {}

  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len == 0 || len > 8) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (offset < 8) {
      std::vector<uint8_t> item = item_data_locked();
      for (uint32_t i = 0; i < len; i++) {
        if (data_offset_ < item.size()) {
          data[i] = item[data_offset_];
        }
        data_offset_++;
      }
      return;
    }
    if (offset == 8 && len >= 2) {
      WriteU16(data, selector_);
      return;
    }
    // DMA is intentionally not implemented yet; FW_CFG_ID reports no DMA bit.
  }

  void write_mmio(uint64_t offset, const uint8_t* data, uint32_t len) {
    if (len == 0 || len > 8) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (offset == 8) {
      uint16_t raw = len >= 2 ? ReadU16(data) : data[0];
      selector_ = normalize_selector(raw);
      data_offset_ = 0;
    }
  }

 private:
  static uint16_t normalize_selector(uint16_t raw) {
    if ((raw & 0x00FF) == 0 && raw != 0) return raw >> 8;
    return raw;
  }

  std::vector<uint8_t> item_data_locked() const {
    switch (selector_) {
      case 0x0000: // FW_CFG_SIGNATURE
        return {'Q', 'E', 'M', 'U'};
      case 0x0001: // FW_CFG_ID: traditional interface, no DMA interface.
        return {1, 0, 0, 0};
      case 0x0005: // FW_CFG_NB_CPUS
        return {static_cast<uint8_t>(cpus_ & 0xFF), static_cast<uint8_t>(cpus_ >> 8)};
      case 0x0019: // FW_CFG_FILE_DIR with zero file entries.
        return {0, 0, 0, 0};
      default:
        return {};
    }
  }

  std::mutex mu_;
  uint16_t cpus_{1};
  uint16_t selector_{0};
  size_t data_offset_{0};
};

// ─── PL061 GPIO ───────────────────────────────────────────────────────────────
class Pl061Gpio {
 public:
  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len == 0 || len > 4) return;
    uint32_t val = 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (offset <= 0x3FC) {
      val = data_ & DataMask(offset);
    } else {
      switch (offset) {
        case 0x400: val = dir_; break;
        case 0x404: val = is_; break;
        case 0x408: val = ibe_; break;
        case 0x40C: val = iev_; break;
        case 0x410: val = ie_; break;
        case 0x414: val = ris_; break;
        case 0x418: val = ris_ & ie_; break;
        case 0x420: val = afsel_; break;
        case 0xFE0: val = 0x61; break; // Peripheral ID0
        case 0xFE4: val = 0x10; break; // Peripheral ID1
        case 0xFE8: val = 0x14; break; // Peripheral ID2
        case 0xFEC: val = 0x00; break; // Peripheral ID3
        case 0xFF0: val = 0x0D; break; // PrimeCell ID0
        case 0xFF4: val = 0xF0; break; // PrimeCell ID1
        case 0xFF8: val = 0x05; break; // PrimeCell ID2
        case 0xFFC: val = 0xB1; break; // PrimeCell ID3
        default: break;
      }
    }
    WriteU32(data, val);
  }

  void write_mmio(uint64_t offset, const uint8_t* data, uint32_t len) {
    if (len == 0 || len > 4) return;
    uint32_t val = ReadU32(data) & 0xFF;
    std::lock_guard<std::mutex> lk(mu_);
    if (offset <= 0x3FC) {
      uint32_t mask = DataMask(offset) & dir_;
      data_ = (data_ & ~mask) | (val & mask);
      return;
    }
    switch (offset) {
      case 0x400: dir_ = val; break;
      case 0x404: is_ = val; break;
      case 0x408: ibe_ = val; break;
      case 0x40C: iev_ = val; break;
      case 0x410: ie_ = val; break;
      case 0x41C: ris_ &= ~val; break;
      case 0x420: afsel_ = val; break;
      default: break;
    }
  }

 private:
  static uint32_t DataMask(uint64_t offset) {
    return static_cast<uint32_t>((offset >> 2) & 0xFF);
  }

  std::mutex mu_;
  uint32_t data_{0};
  uint32_t dir_{0};
  uint32_t is_{0};
  uint32_t ibe_{0};
  uint32_t iev_{0};
  uint32_t ie_{0};
  uint32_t ris_{0};
  uint32_t afsel_{0};
};

// ─── Empty PCIe ECAM Host ────────────────────────────────────────────────────
class EmptyPcieHost {
 public:
  void read_mmio(uint64_t /*offset*/, uint8_t* data, uint32_t len) {
    memset(data, 0xFF, len);
  }

  void write_mmio(uint64_t /*offset*/, const uint8_t* /*data*/, uint32_t /*len*/) {}
};

// ─── Empty virtio-mmio transport ──────────────────────────────────────────────
class EmptyVirtioMmio {
 public:
  void read_mmio(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    if (len != 4) return;
    uint32_t val = 0;
    switch (offset) {
      case 0x000: val = 0x74726976; break; // "virt"
      case 0x004: val = 2; break;
      case 0x008: val = 0; break; // no backend attached
      case 0x00C: val = 0x554D4551; break; // QEMU vendor ID
      case 0x034: val = kMaxQueueSize; break;
      case 0x070: val = 0; break;
      case 0x0FC: val = 0; break;
      default: break;
    }
    WriteU32(data, val);
  }

  void write_mmio(uint64_t /*offset*/, const uint8_t* /*data*/, uint32_t /*len*/) {}
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

// Virtio queue memory helpers. Queue addresses and descriptors are guest
// physical addresses, so they must be inside the mapped RAM IPA range.
static bool IsPowerOfTwo(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static bool ValidVirtQueue(const GuestMemory& mem, const VirtQueue& q) {
  if (!IsPowerOfTwo(q.size) || q.size > kMaxQueueSize) return false;
  if ((q.desc_addr % 16) != 0 || (q.driver_addr % 2) != 0 || (q.device_addr % 4) != 0) return false;
  if (!mem.contains_guest_range(q.desc_addr, uint64_t(q.size) * 16)) return false;
  if (!mem.contains_guest_range(q.driver_addr, 6 + uint64_t(q.size) * 2)) return false;
  if (!mem.contains_guest_range(q.device_addr, 6 + uint64_t(q.size) * 8)) return false;
  return true;
}

static Desc ReadDesc(GuestMemory& mem, const VirtQueue& q, uint16_t idx) {
  Check(idx < q.size, "virtio descriptor index out of bounds");
  uint8_t* p = mem.guest_ptr(q.desc_addr + uint64_t(idx) * 16, 16);
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
  uint8_t* idxp = mem.guest_ptr(q.device_addr + 2, 2);
  uint16_t used = ReadU16(idxp);
  uint64_t entry = q.device_addr + 4 + uint64_t(used % q.size) * 8;
  WriteU32(mem.guest_ptr(entry, 8), id);
  WriteU32(mem.guest_ptr(entry+4, 4), written);
  WriteU16(idxp, used + 1);
}

// ─── VirtioBlk ────────────────────────────────────────────────────────────────
class VirtioBlk {
 public:
  VirtioBlk(GuestMemory mem, uint32_t slot, const std::string& path,
            const std::string& overlay_path, bool read_only,
            std::function<void(uint32_t, bool)> gic_inject)
      : mem_(mem),
        base_(VirtioMmioBaseForSlot(slot)),
        intid_(VirtioMmioIntidForSlot(slot)),
        read_only_(read_only),
        gic_inject_(std::move(gic_inject)) {
    bool overlay = !overlay_path.empty();
    Check(!(read_only_ && overlay), "read-only disk cannot use an overlay");
    int flags = (overlay || read_only_) ? (O_RDONLY | O_CLOEXEC) : (O_RDWR | O_CLOEXEC);
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
    uint32_t off = static_cast<uint32_t>(addr - base_);
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
    uint32_t off = static_cast<uint32_t>(addr - base_);
    if (off >= 0x100 || len != 4) return;
    write_reg(off, ReadU32(data));
  }

  bool contains(uint64_t addr) const {
    return addr >= base_ && addr < base_ + kVirtioMmioSize;
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
      case 0x044:
        if (value == 0) {
          q.ready = false;
        } else if (ValidVirtQueue(mem_, q)) {
          q.ready = true;
        } else {
          q.ready = false;
          status_ |= kVirtioStatusDeviceNeedsReset;
        }
        break;
      case 0x050:
        if (value < 8 && queues_[value].ready) {
          handle_queue(queues_[value]);
          interrupt_status_ |= 1;
          gic_inject_(intid_, true);
        }
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) gic_inject_(intid_, false);
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
    gic_inject_(intid_, false);
  }

  void handle_queue(VirtQueue& q) {
    uint16_t avail = ReadU16(mem_.guest_ptr(q.driver_addr+2, 2));
    while (q.last_avail != avail) {
      uint16_t head = ReadU16(mem_.guest_ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
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
      uint8_t h[16]; memcpy(h, mem_.guest_ptr(hdr.addr, 16), 16);
      uint32_t type = ReadU32(h);
      uint64_t sector = ReadU64(h+8);
      if (type == 0) {
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          Check((d.flags & 2) != 0, "virtio-blk read desc must be writable");
          Check((d.len % 512) == 0, "virtio-blk read length not sector-aligned");
          ReadDisk(sector, mem_.guest_ptr(d.addr, d.len), d.len);
          Check(written <= std::numeric_limits<uint32_t>::max() - d.len,
                "virtio-blk read byte count overflow");
          written += d.len; sector += d.len / 512;
        }
      } else if (type == 1) {
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          Check((d.flags & 2) == 0, "virtio-blk write desc must be read-only");
          Check((d.len % 512) == 0, "virtio-blk write length not sector-aligned");
          WriteDisk(sector, mem_.guest_ptr(d.addr, d.len), d.len);
          sector += d.len / 512;
        }
      } else if (type == 4) {
        CheckErr(fsync(write_fd()), "virtio-blk flush");
      } else if (type == 8) {
        const char id[] = "node-vmm";
        for (size_t i = 1; i+1 < chain.size; i++) {
          Desc d = chain[i];
          uint32_t n = std::min<uint32_t>(d.len, sizeof(id));
          memcpy(mem_.guest_ptr(d.addr, n), id, n);
          written += n; break;
        }
      } else {
        status = 2;
      }
      memcpy(mem_.guest_ptr(stat_d.addr, 1), &status, 1);
      PushUsed(mem_, q, head, written+1);
    } catch (...) {
      status = 1;
      try {
        DescChain chain = WalkChain(mem_, q, head);
        if (!chain.empty()) {
          Desc sd = chain[chain.size-1];
          memcpy(mem_.guest_ptr(sd.addr, 1), &status, 1);
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
    uint64_t last = off + static_cast<uint64_t>(len) - 1;
    for (uint64_t s = off/512; s <= last/512; s++) mark_dirty(s);
  }
  void prepare_partial_overlay(uint64_t off, size_t len) {
    if (!has_overlay() || len == 0) return;
    uint64_t end = off + static_cast<uint64_t>(len);
    std::array<uint8_t,512> buf{};
    for (uint64_t s = off/512; s <= (end-1)/512; s++) {
      uint64_t ss = s*512;
      bool full = off<=ss && end>=ss+512;
      if (full || sector_dirty(s)) continue;
      PreadAll(base_fd_.get(), buf.data(), 512, static_cast<off_t>(ss));
      PwriteAll(overlay_fd_.get(), buf.data(), 512, static_cast<off_t>(ss));
    }
  }
  uint64_t DiskOffset(uint64_t sector, size_t len, const char* op) const {
    Check(sector <= std::numeric_limits<uint64_t>::max() / 512ULL,
          std::string("virtio-blk ") + op + " sector offset overflow");
    uint64_t off = sector * 512ULL;
    Check(off <= disk_bytes_ && uint64_t(len) <= disk_bytes_ - off,
          std::string("virtio-blk ") + op + " out of range");
    uint64_t max_off = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    Check(off <= max_off,
          std::string("virtio-blk ") + op + " offset exceeds host off_t");
    Check(len == 0 || uint64_t(len - 1) <= max_off - off,
          std::string("virtio-blk ") + op + " range exceeds host off_t");
    return off;
  }
  void ReadDisk(uint64_t sector, uint8_t* dst, size_t len) {
    uint64_t off = DiskOffset(sector, len, "read");
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
    Check(!read_only_, "virtio-blk write on read-only disk");
    uint64_t off = DiskOffset(sector, len, "write");
    prepare_partial_overlay(off, len);
    PwriteAll(write_fd(), src, len, static_cast<off_t>(off));
    mark_dirty_range(off, len);
  }

  GuestMemory mem_;
  uint64_t base_{kVirtioBlkBase};
  uint32_t intid_{kVirtioBlkIntid};
  bool read_only_{false};
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

struct NetPortForward {
  std::string host;
  uint16_t host_port{0};
  uint16_t guest_port{0};
};

struct AttachedDiskConfig {
  std::string path;
  bool read_only{false};
};

const char* VmnetStatusName(vmnet_return_t status) {
  switch (status) {
    case VMNET_SUCCESS: return "VMNET_SUCCESS";
    case VMNET_FAILURE: return "VMNET_FAILURE";
    case VMNET_MEM_FAILURE: return "VMNET_MEM_FAILURE";
    case VMNET_INVALID_ARGUMENT: return "VMNET_INVALID_ARGUMENT";
    case VMNET_SETUP_INCOMPLETE: return "VMNET_SETUP_INCOMPLETE";
    case VMNET_INVALID_ACCESS: return "VMNET_INVALID_ACCESS";
    case VMNET_PACKET_TOO_BIG: return "VMNET_PACKET_TOO_BIG";
    case VMNET_BUFFER_EXHAUSTED: return "VMNET_BUFFER_EXHAUSTED";
    case VMNET_TOO_MANY_PACKETS: return "VMNET_TOO_MANY_PACKETS";
    case VMNET_SHARING_SERVICE_BUSY: return "VMNET_SHARING_SERVICE_BUSY";
    case VMNET_NOT_AUTHORIZED: return "VMNET_NOT_AUTHORIZED";
    default: return "VMNET_UNKNOWN";
  }
}

std::string VmnetStartError(vmnet_return_t status) {
  std::string message = "vmnet_start_interface failed: ";
  message += VmnetStatusName(status);
  message += " (" + std::to_string(status) + ")";
  if (status == VMNET_FAILURE || status == VMNET_INVALID_ACCESS || status == VMNET_NOT_AUTHORIZED) {
    message += "; macOS vmnet requires root or a valid com.apple.vm.networking entitlement";
  }
  return message;
}

class VirtioNet {
 public:
  VirtioNet(GuestMemory mem, uint32_t slot, const std::string& mac,
            std::function<void(uint32_t, bool)> gic_inject)
      : mem_(mem),
        base_(VirtioMmioBaseForSlot(slot)),
        intid_(VirtioMmioIntidForSlot(slot)),
        mac_(ParseMac(mac)),
        gic_inject_(std::move(gic_inject)) {
    queues_[0].size = kMaxQueueSize;
    queues_[1].size = kMaxQueueSize;
    // Create notification pipe
    int pipefd[2];
    CheckErr(pipe(pipefd), "pipe for vmnet notification");
    notify_read_.reset(pipefd[0]);
    notify_write_.reset(pipefd[1]);
    int read_flags = fcntl(notify_read_.get(), F_GETFL);
    CheckErr(read_flags, "fcntl notify read flags");
    CheckErr(fcntl(notify_read_.get(), F_SETFL, read_flags | O_NONBLOCK),
             "fcntl notify read nonblock");
    int write_flags = fcntl(notify_write_.get(), F_GETFL);
    CheckErr(write_flags, "fcntl notify write flags");
    CheckErr(fcntl(notify_write_.get(), F_SETFL, write_flags | O_NONBLOCK),
             "fcntl notify write nonblock");
  }

  ~VirtioNet() { stop_vmnet(); }

  // Start vmnet in shared (NAT) mode
  void start(const std::string& hint_tapname,
             const std::string& host_ip,
             const std::string& guest_ip,
             const std::string& netmask,
             const std::string& dns_ip,
             const std::vector<NetPortForward>& port_forwards) {
    if (hint_tapname == "slirp" || hint_tapname.rfind("slirp:", 0) == 0) {
      start_slirp(host_ip, guest_ip, netmask, dns_ip, port_forwards);
      return;
    }
    if (hint_tapname.rfind("socket_vmnet:", 0) == 0) {
      start_socket_vmnet(hint_tapname.substr(strlen("socket_vmnet:")));
      return;
    }

    xpc_object_t opts = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(opts, vmnet_operation_mode_key, VMNET_SHARED_MODE);
    if (!host_ip.empty() && !guest_ip.empty() && !netmask.empty()) {
      xpc_dictionary_set_string(opts, vmnet_start_address_key, host_ip.c_str());
      xpc_dictionary_set_string(opts, vmnet_end_address_key, guest_ip.c_str());
      xpc_dictionary_set_string(opts, vmnet_subnet_mask_key, netmask.c_str());
    }

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

    Check(status_out == VMNET_SUCCESS, VmnetStartError(status_out));
    iface_ = iface_out;

    // Register packet arrival callback
    VirtioNet* self = this;
    vmnet_interface_set_event_callback(iface_, VMNET_INTERFACE_PACKETS_AVAILABLE,
      dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
      ^(interface_event_t /*event*/, xpc_object_t /*params*/) {
        self->notify_host();
      });
  }

  void stop_vmnet() {
    stop_slirp();
    stop_socket_vmnet();
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
    std::lock_guard<std::mutex> lk(notify_mu_);
    uint8_t buf[64];
    for (;;) {
      ssize_t n = read(notify_read_.get(), buf, sizeof(buf));
      if (n > 0) continue;
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    notify_pending_ = false;
  }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    uint32_t off = static_cast<uint32_t>(addr - base_);
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
    uint32_t off = static_cast<uint32_t>(addr - base_);
    if (off >= 0x100 || len != 4) return;
    write_reg(off, ReadU32(data));
  }

  bool contains(uint64_t addr) const {
    return addr >= base_ && addr < base_ + kVirtioMmioSize;
  }

  void poll_rx() {
    if (!enabled() || !queues_[0].ready) return;
    drain_notify();
    poll_slirp();
    if (socket_fd_.get() >= 0 || slirp_enabled()) {
      for (int iter = 0; iter < 32; iter++) {
        if (!has_rx_buffer()) break;
        std::vector<uint8_t> frame;
        {
          std::lock_guard<std::mutex> lk(rx_frames_mu_);
          if (rx_frames_.empty()) break;
          frame = std::move(rx_frames_.front());
          rx_frames_.pop_front();
        }
        if (!frame.empty()) inject_rx_frame(frame.data(), frame.size());
      }
      return;
    }
    if (!iface_) return;
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

  bool enabled() const { return iface_ != nullptr || socket_fd_.get() >= 0 || slirp_enabled(); }
  bool needs_poll() const { return slirp_enabled(); }
  void set_wake_callback(std::function<void()> wake_vcpus) {
    std::lock_guard<std::mutex> lk(wake_mu_);
    wake_vcpus_ = std::move(wake_vcpus);
  }

 private:
  void notify_host() {
    {
      std::lock_guard<std::mutex> lk(notify_mu_);
      if (!notify_pending_) {
        uint8_t byte = 1;
        for (;;) {
          ssize_t n = write(notify_write_.get(), &byte, 1);
          if (n == 1 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            notify_pending_ = true;
            break;
          }
          if (n < 0 && errno == EINTR) continue;
          break;
        }
      }
    }
    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> lk(wake_mu_);
      wake = wake_vcpus_;
    }
    if (wake) wake();
  }

  static bool read_exact(int fd, uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = read(fd, data + off, len - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  static bool write_exact(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = write(fd, data + off, len - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  void start_socket_vmnet(const std::string& path) {
    Check(!path.empty(), "socket_vmnet path is empty");
    Check(path.size() < sizeof(sockaddr_un::sun_path), "socket_vmnet path is too long");

    Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
    CheckErr(fd.get(), "socket_vmnet socket");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    CheckErr(connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), "connect socket_vmnet");

    socket_fd_ = std::move(fd);
    socket_done_.store(false, std::memory_order_relaxed);
    int read_fd = socket_fd_.get();
    socket_thread_ = std::thread([this, read_fd]() {
      for (;;) {
        if (socket_done_.load(std::memory_order_relaxed)) break;
        uint32_t len_be = 0;
        if (!read_exact(read_fd, reinterpret_cast<uint8_t*>(&len_be), sizeof(len_be))) break;
        uint32_t len = ntohl(len_be);
        if (len == 0 || len > 65536) break;
        std::vector<uint8_t> frame(len);
        if (!read_exact(read_fd, frame.data(), frame.size())) break;
        {
          std::lock_guard<std::mutex> lk(rx_frames_mu_);
          rx_frames_.push_back(std::move(frame));
        }
        notify_host();
      }
    });
  }

  void stop_socket_vmnet() {
    socket_done_.store(true, std::memory_order_relaxed);
    if (socket_fd_.get() >= 0) {
      shutdown(socket_fd_.get(), SHUT_RDWR);
      socket_fd_.reset();
    }
    if (socket_thread_.joinable()) socket_thread_.join();
  }

  bool slirp_enabled() const {
#if NODE_VMM_HAVE_SLIRP
    return slirp_ != nullptr;
#else
    return false;
#endif
  }

  void start_slirp(const std::string& host_ip,
                   const std::string& guest_ip,
                   const std::string& netmask,
                   const std::string& dns_ip,
                   const std::vector<NetPortForward>& port_forwards) {
#if NODE_VMM_HAVE_SLIRP
    in_addr host_addr{};
    in_addr guest_addr{};
    in_addr mask_addr{};
    in_addr dns_addr{};
    Check(inet_pton(AF_INET, host_ip.c_str(), &host_addr) == 1, "invalid slirp host IP: " + host_ip);
    Check(inet_pton(AF_INET, guest_ip.c_str(), &guest_addr) == 1, "invalid slirp guest IP: " + guest_ip);
    Check(inet_pton(AF_INET, netmask.c_str(), &mask_addr) == 1, "invalid slirp netmask: " + netmask);
    const std::string effective_dns = dns_ip.empty() ? host_ip : dns_ip;
    Check(inet_pton(AF_INET, effective_dns.c_str(), &dns_addr) == 1, "invalid slirp DNS IP: " + effective_dns);

    in_addr network_addr{};
    network_addr.s_addr = htonl(ntohl(host_addr.s_addr) & ntohl(mask_addr.s_addr));

    SlirpConfig cfg{};
    cfg.version = SLIRP_CONFIG_VERSION_MAX;
    cfg.restricted = 0;
    cfg.in_enabled = true;
    cfg.vnetwork = network_addr;
    cfg.vnetmask = mask_addr;
    cfg.vhost = host_addr;
    cfg.in6_enabled = false;
    cfg.vhostname = "node-vmm";
    cfg.vdhcp_start = guest_addr;
    cfg.vnameserver = dns_addr;
    cfg.if_mtu = 1500;
    cfg.if_mru = 1500;
    cfg.disable_host_loopback = false;

    memset(&slirp_cb_, 0, sizeof(slirp_cb_));
    slirp_cb_.send_packet = &VirtioNet::slirp_send_packet_cb;
    slirp_cb_.guest_error = &VirtioNet::slirp_guest_error_cb;
    slirp_cb_.clock_get_ns = &VirtioNet::slirp_clock_get_ns_cb;
    slirp_cb_.notify = &VirtioNet::slirp_notify_cb;
    slirp_cb_.register_poll_socket = &VirtioNet::slirp_register_poll_socket_cb;
    slirp_cb_.unregister_poll_socket = &VirtioNet::slirp_unregister_poll_socket_cb;

    slirp_ = slirp_new(&cfg, &slirp_cb_, this);
    Check(slirp_ != nullptr, "slirp_new failed");

    for (const NetPortForward& forward : port_forwards) {
      in_addr host_bind{};
      const std::string bind_host = forward.host.empty() ? "127.0.0.1" : forward.host;
      Check(inet_pton(AF_INET, bind_host.c_str(), &host_bind) == 1,
            "invalid slirp host forward bind IP: " + bind_host);
      int rc = slirp_add_hostfwd(slirp_, 0, host_bind, forward.host_port, guest_addr, forward.guest_port);
      Check(rc == 0, "slirp host forward failed: " + bind_host + ":" +
                     std::to_string(forward.host_port) + " -> " + guest_ip + ":" +
                     std::to_string(forward.guest_port));
    }
#else
    (void)host_ip;
    (void)guest_ip;
    (void)netmask;
    (void)dns_ip;
    (void)port_forwards;
    throw std::runtime_error("HVF slirp networking is not available in this build; install libslirp or use vmnet/socket_vmnet");
#endif
  }

  void stop_slirp() {
#if NODE_VMM_HAVE_SLIRP
    if (!slirp_) return;
    slirp_cleanup(slirp_);
    slirp_ = nullptr;
#endif
  }

#if NODE_VMM_HAVE_SLIRP
  struct SlirpPollSet {
    std::vector<pollfd> fds;
  };
#endif

  void poll_slirp() {
#if NODE_VMM_HAVE_SLIRP
    if (!slirp_) return;
    uint32_t timeout = 0;
    SlirpPollSet poll_set;
    slirp_pollfds_fill_socket(slirp_, &timeout, &VirtioNet::slirp_add_poll_cb, &poll_set);
    int rc = 0;
    if (!poll_set.fds.empty()) {
      rc = poll(poll_set.fds.data(), static_cast<nfds_t>(poll_set.fds.size()), 0);
      if (rc < 0 && errno == EINTR) rc = 0;
    }
    slirp_pollfds_poll(slirp_, rc < 0 ? 1 : 0, &VirtioNet::slirp_get_revents_cb, &poll_set);
#endif
  }

#if NODE_VMM_HAVE_SLIRP
  static short poll_events_from_slirp(int events) {
    short out = 0;
    if (events & SLIRP_POLL_IN) out |= POLLIN;
    if (events & SLIRP_POLL_OUT) out |= POLLOUT;
    if (events & SLIRP_POLL_PRI) out |= POLLPRI;
    if (events & SLIRP_POLL_ERR) out |= POLLERR;
    if (events & SLIRP_POLL_HUP) out |= POLLHUP;
    return out;
  }

  static int slirp_events_from_poll(short events) {
    int out = 0;
    if (events & POLLIN) out |= SLIRP_POLL_IN;
    if (events & POLLOUT) out |= SLIRP_POLL_OUT;
    if (events & POLLPRI) out |= SLIRP_POLL_PRI;
    if (events & POLLERR) out |= SLIRP_POLL_ERR;
    if (events & POLLHUP) out |= SLIRP_POLL_HUP;
    return out;
  }

  static int slirp_add_poll_cb(slirp_os_socket fd, int events, void* opaque) {
    auto* set = static_cast<SlirpPollSet*>(opaque);
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = poll_events_from_slirp(events);
    set->fds.push_back(pfd);
    return static_cast<int>(set->fds.size() - 1);
  }

  static int slirp_get_revents_cb(int idx, void* opaque) {
    auto* set = static_cast<SlirpPollSet*>(opaque);
    if (idx < 0 || static_cast<size_t>(idx) >= set->fds.size()) return 0;
    return slirp_events_from_poll(set->fds[static_cast<size_t>(idx)].revents);
  }

  static slirp_ssize_t slirp_send_packet_cb(const void* buf, size_t len, void* opaque) {
    return static_cast<VirtioNet*>(opaque)->slirp_send_packet(buf, len);
  }

  slirp_ssize_t slirp_send_packet(const void* buf, size_t len) {
    if (!buf || len == 0) return 0;
    const auto* bytes = static_cast<const uint8_t*>(buf);
    {
      std::lock_guard<std::mutex> lk(rx_frames_mu_);
      rx_frames_.emplace_back(bytes, bytes + len);
    }
    notify_host();
    return static_cast<slirp_ssize_t>(len);
  }

  static void slirp_guest_error_cb(const char* msg, void* /*opaque*/) {
    if (HvfDebugEnabled() && msg) {
      fprintf(stderr, "[node-vmm hvf] slirp guest error: %s\n", msg);
    }
  }

  static int64_t slirp_clock_get_ns_cb(void* /*opaque*/) {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + int64_t(ts.tv_nsec);
  }

  static void slirp_notify_cb(void* opaque) {
    auto* self = static_cast<VirtioNet*>(opaque);
    self->notify_host();
  }

  static void slirp_register_poll_socket_cb(slirp_os_socket /*socket*/, void* opaque) {
    slirp_notify_cb(opaque);
  }

  static void slirp_unregister_poll_socket_cb(slirp_os_socket /*socket*/, void* opaque) {
    slirp_notify_cb(opaque);
  }
#endif

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
      case 0x044:
        if (value == 0) {
          q.ready = false;
        } else if (ValidVirtQueue(mem_, q)) {
          q.ready = true;
        } else {
          q.ready = false;
          status_ |= kVirtioStatusDeviceNeedsReset;
        }
        break;
      case 0x050:
        if (value == 0) poll_rx();
        else if (value == 1 && queues_[1].ready) handle_tx_queue();
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) gic_inject_(intid_, false);
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
    gic_inject_(intid_, false);
  }

  bool has_rx_buffer() const {
    const VirtQueue& q = queues_[0];
    return q.ready && q.last_avail != ReadU16(mem_.guest_ptr(q.driver_addr+2, 2));
  }

  void signal_net() {
    interrupt_status_ |= 1;
    gic_inject_(intid_, true);
  }

  void inject_rx_frame(const uint8_t* frame, size_t len) {
    VirtQueue& q = queues_[0];
    Check(len <= kMaxFrameSize, "virtio-net rx frame too large");
    uint16_t head = ReadU16(mem_.guest_ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
    q.last_avail++;
    DescChain chain = WalkChain(mem_, q, head);
    size_t needed = 12 + len;
    size_t offset = 0;
    uint8_t header[12]{};
    for (size_t i = 0; i < chain.size; i++) {
      const Desc& d = chain[i];
      Check((d.flags & 2) != 0, "virtio-net rx desc must be writable");
      uint8_t* dst = mem_.guest_ptr(d.addr, d.len);
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
    uint16_t avail = ReadU16(mem_.guest_ptr(q.driver_addr+2, 2));
    while (q.last_avail != avail) {
      uint16_t head = ReadU16(mem_.guest_ptr(q.driver_addr+4+uint64_t(q.last_avail%q.size)*2, 2));
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
      const uint8_t* src = mem_.guest_ptr(d.addr, d.len);
      if (skip >= d.len) { skip -= d.len; continue; }
      size_t pos = skip; skip = 0;
      Check(frame.size() <= kMaxFrameSize &&
            static_cast<size_t>(d.len - pos) <= kMaxFrameSize - frame.size(),
            "virtio-net tx frame too large");
      frame.insert(frame.end(), src+pos, src+d.len);
    }
    if (slirp_enabled() && !frame.empty()) {
#if NODE_VMM_HAVE_SLIRP
      slirp_input(slirp_, frame.data(), static_cast<int>(frame.size()));
#endif
    } else if (socket_fd_.get() >= 0 && !frame.empty()) {
      uint32_t len_be = htonl(static_cast<uint32_t>(frame.size()));
      write_exact(socket_fd_.get(), reinterpret_cast<const uint8_t*>(&len_be), sizeof(len_be));
      write_exact(socket_fd_.get(), frame.data(), frame.size());
    } else if (iface_ && !frame.empty()) {
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
  uint64_t base_{kVirtioNetBase};
  uint32_t intid_{kVirtioNetIntid};
  std::array<uint8_t, 6> mac_;
  std::function<void(uint32_t, bool)> gic_inject_;
  interface_ref iface_{nullptr};
  Fd socket_fd_;
  std::atomic<bool> socket_done_{false};
  std::thread socket_thread_;
  std::mutex rx_frames_mu_;
  std::deque<std::vector<uint8_t>> rx_frames_;
#if NODE_VMM_HAVE_SLIRP
  Slirp* slirp_{nullptr};
  SlirpCb slirp_cb_{};
#endif
  uint32_t max_packet_size_{kMaxFrameSize};
  Fd notify_read_;
  Fd notify_write_;
  std::mutex notify_mu_;
  bool notify_pending_{false};
  std::mutex wake_mu_;
  std::function<void()> wake_vcpus_;
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
static bool HasNamed(napi_env env, napi_value obj, const char* key) {
  bool has = false;
  napi_has_named_property(env, obj, key, &has);
  return has;
}
static bool IsNullish(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  return type == napi_undefined || type == napi_null;
}
static std::string GetString(napi_env env, napi_value obj, const char* key) {
  if (!HasNamed(env, obj, key)) return "";
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_string) return "";
  size_t len; napi_get_value_string_utf8(env, v, nullptr, 0, &len);
  std::vector<char> buf(len + 1, '\0');
  napi_get_value_string_utf8(env, v, buf.data(), buf.size(), nullptr);
  return std::string(buf.data(), len);
}
static uint32_t GetU32(napi_env env, napi_value obj, const char* key, uint32_t def = 0) {
  if (!HasNamed(env, obj, key)) return def;
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_number) return def;
  uint32_t n; napi_get_value_uint32(env, v, &n); return n;
}
static bool GetBool(napi_env env, napi_value obj, const char* key, bool def = false) {
  if (!HasNamed(env, obj, key)) return def;
  napi_value v; napi_get_named_property(env, obj, key, &v);
  napi_valuetype t; napi_typeof(env, v, &t);
  if (t != napi_boolean) return def;
  bool b; napi_get_value_bool(env, v, &b); return b;
}

static std::vector<NetPortForward> GetPortForwards(napi_env env, napi_value obj, const char* key) {
  std::vector<NetPortForward> out;
  if (!HasNamed(env, obj, key)) return out;
  napi_value arr; napi_get_named_property(env, obj, key, &arr);
  bool is_array = false;
  napi_is_array(env, arr, &is_array);
  if (!is_array) return out;
  uint32_t len = 0;
  napi_get_array_length(env, arr, &len);
  for (uint32_t i = 0; i < len; i++) {
    napi_value item;
    napi_get_element(env, arr, i, &item);
    NetPortForward forward;
    forward.host = GetString(env, item, "host");
    uint32_t host_port = GetU32(env, item, "hostPort", 0);
    uint32_t guest_port = GetU32(env, item, "guestPort", 0);
    Check(host_port > 0 && host_port <= 65535, "invalid net hostPort");
    Check(guest_port > 0 && guest_port <= 65535, "invalid net guestPort");
    forward.host_port = static_cast<uint16_t>(host_port);
    forward.guest_port = static_cast<uint16_t>(guest_port);
    out.push_back(std::move(forward));
  }
  return out;
}

static bool HasNonNullishNamed(napi_env env, napi_value obj, const char* key) {
  if (!HasNamed(env, obj, key)) return false;
  napi_value value;
  napi_get_named_property(env, obj, key, &value);
  return !IsNullish(env, value);
}

static std::vector<AttachedDiskConfig> GetAttachedDisks(napi_env env, napi_value obj) {
  bool has_disks = HasNonNullishNamed(env, obj, "disks");
  bool has_attached_disks = HasNonNullishNamed(env, obj, "attachedDisks");
  if (!has_disks && !has_attached_disks) return {};
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

struct RunControl {
  int32_t* words{nullptr};
  size_t length{0};

  bool enabled() const { return words != nullptr && length >= 2; }

  int32_t command() const {
    if (!enabled()) return kControlRun;
    return __atomic_load_n(&words[0], __ATOMIC_SEQ_CST);
  }

  void set_state(int32_t state) const {
    if (enabled()) {
      __atomic_store_n(&words[1], state, __ATOMIC_SEQ_CST);
    }
  }

  void mark_console_output() const {
    if (words != nullptr && length >= 3) {
      __atomic_store_n(&words[2], 1, __ATOMIC_SEQ_CST);
    }
  }
};

static RunControl GetRunControl(napi_env env, napi_value obj) {
  if (!HasNamed(env, obj, "control")) return {};
  napi_value value; napi_get_named_property(env, obj, "control", &value);
  if (IsNullish(env, value)) return {};

  bool is_typedarray = false;
  Check(napi_is_typedarray(env, value, &is_typedarray) == napi_ok, "napi_is_typedarray control failed");
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

struct ControlExitGuard {
  RunControl control;
  ~ControlExitGuard() { control.set_state(kControlStateExited); }
};

struct HvMapGuard {
  uint64_t ipa{0};
  uint64_t size{0};
  bool active{false};

  ~HvMapGuard() { unmap(); }

  void unmap() {
    if (!active) return;
    hv_vm_unmap(ipa, size);
    active = false;
  }
};

struct VcpuGuard {
  hv_vcpu_t vcpu{};
  bool active{false};

  ~VcpuGuard() { destroy(); }

  void destroy() {
    if (!active) return;
    hv_vcpu_destroy(vcpu);
    active = false;
  }
};

struct MonitorThreadGuard {
  std::atomic<bool>& done;
  std::thread& thread;

  ~MonitorThreadGuard() { stop(); }

  void stop() {
    done.store(true, std::memory_order_relaxed);
    if (thread.joinable()) thread.join();
  }
};

struct InputThreadGuard {
  std::atomic<bool>& done;
  std::thread& thread;

  ~InputThreadGuard() { stop(); }

  void stop() {
    done.store(true, std::memory_order_relaxed);
    if (thread.joinable()) thread.join();
  }
};

class TerminalRawMode {
 public:
  explicit TerminalRawMode(bool enable) {
    if (!enable || !isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &old_) != 0) return;
    struct termios raw = old_;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag |= OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    raw_ = raw;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      active_ = true;
      struct sigaction action {};
      action.sa_handler = &TerminalRawMode::OnSigcont;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGCONT, &action, &old_sigcont_) == 0) {
        sigcont_active_ = true;
      }
    }
  }

  TerminalRawMode(const TerminalRawMode&) = delete;
  TerminalRawMode& operator=(const TerminalRawMode&) = delete;

  ~TerminalRawMode() {
    if (active_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &old_);
    }
    if (sigcont_active_) {
      sigaction(SIGCONT, &old_sigcont_, nullptr);
    }
  }

  void restore_after_sigcont() const {
    if (!active_) return;
    if (sigcont_pending_.exchange(false, std::memory_order_relaxed)) {
      tcsetattr(STDIN_FILENO, TCSANOW, &raw_);
    }
  }

 private:
  static void OnSigcont(int) {
    sigcont_pending_.store(true, std::memory_order_relaxed);
  }

  bool active_{false};
  bool sigcont_active_{false};
  struct termios old_ {};
  struct termios raw_ {};
  struct sigaction old_sigcont_ {};
  inline static std::atomic<bool> sigcont_pending_{false};
};

class SignalIgnoreGuard {
 public:
  SignalIgnoreGuard(int signum, bool enable) : signum_(signum) {
    if (!enable) return;
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(signum_, &action, &old_) == 0) {
      active_ = true;
    }
  }

  SignalIgnoreGuard(const SignalIgnoreGuard&) = delete;
  SignalIgnoreGuard& operator=(const SignalIgnoreGuard&) = delete;

  ~SignalIgnoreGuard() {
    if (active_) {
      sigaction(signum_, &old_, nullptr);
    }
  }

 private:
  int signum_{0};
  bool active_{false};
  struct sigaction old_ {};
};

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

template <typename Device>
static uint32_t ReadMmio32(Device& device, uint64_t offset) {
  uint8_t data[4]{};
  device.read_mmio(offset, data, sizeof(data));
  return ReadU32(data);
}

template <typename Device>
static void WriteMmio32(Device& device, uint64_t offset, uint32_t value) {
  uint8_t data[4]{};
  WriteU32(data, value);
  device.write_mmio(offset, data, sizeof(data));
}

static napi_value HvfFdtSmoke(napi_env env, napi_callback_info /*info*/) {
  try {
    FdtBuilder builder;
    constexpr uint32_t kSmokeCpus = 2;
    std::vector<uint8_t> dtb = builder.Build(
        kRamBase, 128ULL * 1024ULL * 1024ULL, kSmokeCpus,
        "console=ttyAMA0,115200 root=/dev/vda", true);

    EmptyVirtioMmio empty;
    uint8_t data[4]{};
    empty.read_mmio(0x000, data, sizeof(data));
    uint32_t empty_magic = ReadU32(data);
    empty.read_mmio(0x008, data, sizeof(data));
    uint32_t empty_device_id = ReadU32(data);

    napi_value result = MakeObject(env);
    SetProp(env, result, "backend", MakeString(env, "hvf"));
    SetProp(env, result, "dtbBytes", MakeU32(env, static_cast<uint32_t>(dtb.size())));
    SetProp(env, result, "rootInterruptParent", MakeU32(env, 1));
    SetProp(env, result, "rootDmaCoherent", MakeBool(env, true));
    SetProp(env, result, "gicPhandle", MakeU32(env, 1));
    SetProp(env, result, "cpuCount", MakeU32(env, kSmokeCpus));
    SetProp(env, result, "cpuEnableMethod", MakeString(env, "psci"));
    SetProp(env, result, "gicRedistributorBytes", MakeU32(env, GicRedistributorBytes(kSmokeCpus)));
    SetProp(env, result, "timerInterrupts", MakeU32(env, 4));
    SetProp(env, result, "timerIrqFlags", MakeU32(env, 4));
    SetProp(env, result, "chosenRandomness", MakeBool(env, true));
    SetProp(env, result, "serial0Alias", MakeString(env, "/pl011@9000000"));
    SetProp(env, result, "stdoutPath", MakeString(env, "/pl011@9000000"));
    SetProp(env, result, "uartBase", MakeU32(env, static_cast<uint32_t>(kUartBase)));
    SetProp(env, result, "uartSpi", MakeU32(env, kUartSpi));
    SetProp(env, result, "uartClockPhandles", MakeU32(env, 2));
    SetProp(env, result, "rtcBase", MakeU32(env, static_cast<uint32_t>(kPl031RtcBase)));
    SetProp(env, result, "rtcSpi", MakeU32(env, kRtcSpi));
    SetProp(env, result, "rtcIntid", MakeU32(env, kRtcIntid));
    SetProp(env, result, "gpioBase", MakeU32(env, static_cast<uint32_t>(kPl061GpioBase)));
    SetProp(env, result, "gpioSpi", MakeU32(env, kGpioSpi));
    SetProp(env, result, "gpioIntid", MakeU32(env, kGpioIntid));
    SetProp(env, result, "gpioPhandle", MakeU32(env, 4));
    SetProp(env, result, "gpioPowerKeyCode", MakeU32(env, 116));
    SetProp(env, result, "fwCfgBase", MakeU32(env, static_cast<uint32_t>(kFwCfgBase)));
    SetProp(env, result, "fwCfgSize", MakeU32(env, 0x18));
    SetProp(env, result, "fwCfgDmaCoherent", MakeBool(env, true));
    SetProp(env, result, "virtioBase", MakeU32(env, static_cast<uint32_t>(kVirtioMmioBase)));
    SetProp(env, result, "virtioStride", MakeU32(env, static_cast<uint32_t>(kVirtioMmioStride)));
    SetProp(env, result, "virtioCount", MakeU32(env, kVirtioMmioCount));
    SetProp(env, result, "virtioDmaCoherent", MakeBool(env, true));
    SetProp(env, result, "virtioFirstSpi", MakeU32(env, kVirtioFirstSpi));
    SetProp(env, result, "virtioBlkBase", MakeU32(env, static_cast<uint32_t>(kVirtioBlkBase)));
    SetProp(env, result, "virtioBlkIntid", MakeU32(env, kVirtioBlkIntid));
    SetProp(env, result, "virtioNetBase", MakeU32(env, static_cast<uint32_t>(kVirtioNetBase)));
    SetProp(env, result, "virtioNetIntid", MakeU32(env, kVirtioNetIntid));
    SetProp(env, result, "pcieBase", MakeU32(env, static_cast<uint32_t>(kPcieMmioBase)));
    SetProp(env, result, "pcieMmioSize", MakeU32(env, static_cast<uint32_t>(kPcieMmioSize)));
    SetProp(env, result, "pciePioBase", MakeU32(env, static_cast<uint32_t>(kPciePioBase)));
    SetProp(env, result, "pcieEcamBase", MakeU32(env, static_cast<uint32_t>(kPcieEcamBase)));
    SetProp(env, result, "pcieEcamSize", MakeU32(env, static_cast<uint32_t>(kPcieEcamSize)));
    SetProp(env, result, "pcieFirstSpi", MakeU32(env, kPcieFirstSpi));
    SetProp(env, result, "pcieIntxCount", MakeU32(env, kPcieIntxCount));
    SetProp(env, result, "pcieInterruptMapEntries", MakeU32(env, 4 * kPcieIntxCount));
    SetProp(env, result, "pcieInterruptMapMaskDevfn", MakeU32(env, 0x1800));
    SetProp(env, result, "pcieInterruptMapMaskPin", MakeU32(env, 0x7));
    SetProp(env, result, "pcieBusCount", MakeU32(env, PcieBusCount()));
    SetProp(env, result, "pcieDmaCoherent", MakeBool(env, true));
    SetProp(env, result, "emptyTransportMagic", MakeU32(env, empty_magic));
    SetProp(env, result, "emptyTransportDeviceId", MakeU32(env, empty_device_id));
    return result;
  } catch (const std::exception& e) {
    return ThrowError(env, std::string("hvfFdtSmoke: ") + e.what());
  }
}

static napi_value HvfPl011Smoke(napi_env env, napi_callback_info /*info*/) {
  try {
    Pl011Uart uart;
    uart.set_console_limit(1024);

    uint8_t one[4]{};
    WriteU32(one, 'A');
    uart.write_mmio(0x000, one, 1);
    const char cursor_query[] = "\x1b[6n";
    for (size_t i = 0; i + 1 < sizeof(cursor_query); i++) {
      WriteU32(one, static_cast<uint8_t>(cursor_query[i]));
      uart.write_mmio(0x000, one, 1);
    }

    std::string cursor_response;
    for (size_t i = 0; i < 6; i++) {
      uint8_t data[4]{};
      uart.read_mmio(0x000, data, sizeof(data));
      cursor_response.push_back(static_cast<char>(ReadU32(data) & 0xFF));
    }

    uint32_t fr_empty = ReadMmio32(uart, 0x018);
    uart.inject_char('z');
    uint32_t fr_with_rx = ReadMmio32(uart, 0x018);
    WriteMmio32(uart, 0x038, 0x50);
    uint32_t ris_before_clear = ReadMmio32(uart, 0x03C);
    uint32_t mis_before_clear = ReadMmio32(uart, 0x040);
    uint32_t rx_byte = ReadMmio32(uart, 0x000) & 0xFF;
    WriteMmio32(uart, 0x044, 0xFFFFFFFF);
    uint32_t ris_after_clear = ReadMmio32(uart, 0x03C);

    WriteMmio32(uart, 0x024, 26);
    WriteMmio32(uart, 0x028, 3);
    WriteMmio32(uart, 0x02C, 0x60);
    WriteMmio32(uart, 0x030, 0x301);
    WriteMmio32(uart, 0x034, 0x24);
    WriteMmio32(uart, 0x048, 0x7);

    napi_value result = MakeObject(env);
    SetProp(env, result, "backend", MakeString(env, "hvf"));
    SetProp(env, result, "console", MakeString(env, uart.console()));
    SetProp(env, result, "cursorResponse", MakeString(env, cursor_response));
    SetProp(env, result, "rxByte", MakeU32(env, rx_byte));
    SetProp(env, result, "frEmpty", MakeU32(env, fr_empty));
    SetProp(env, result, "frWithRx", MakeU32(env, fr_with_rx));
    SetProp(env, result, "risBeforeClear", MakeU32(env, ris_before_clear));
    SetProp(env, result, "misBeforeClear", MakeU32(env, mis_before_clear));
    SetProp(env, result, "risAfterClear", MakeU32(env, ris_after_clear));
    SetProp(env, result, "ibrd", MakeU32(env, ReadMmio32(uart, 0x024)));
    SetProp(env, result, "fbrd", MakeU32(env, ReadMmio32(uart, 0x028)));
    SetProp(env, result, "lcrH", MakeU32(env, ReadMmio32(uart, 0x02C)));
    SetProp(env, result, "cr", MakeU32(env, ReadMmio32(uart, 0x030)));
    SetProp(env, result, "ifls", MakeU32(env, ReadMmio32(uart, 0x034)));
    SetProp(env, result, "dmacr", MakeU32(env, ReadMmio32(uart, 0x048)));
    SetProp(env, result, "peripheralId0", MakeU32(env, ReadMmio32(uart, 0xFE0)));
    SetProp(env, result, "primeCellId0", MakeU32(env, ReadMmio32(uart, 0xFF0)));
    return result;
  } catch (const std::exception& e) {
    return ThrowError(env, std::string("hvfPl011Smoke: ") + e.what());
  }
}

static napi_value HvfDeviceSmoke(napi_env env, napi_callback_info /*info*/) {
  try {
    Pl031Rtc rtc;
    FwCfgMmio fw_cfg;
    Pl061Gpio gpio;
    EmptyPcieHost pcie;
    EmptyVirtioMmio empty_virtio;

    uint32_t rtc_now = ReadMmio32(rtc, 0x000);
    WriteMmio32(rtc, 0x008, 12345);
    uint32_t rtc_loaded = ReadMmio32(rtc, 0x000);
    uint32_t rtc_pid0 = ReadMmio32(rtc, 0xFE0);
    uint32_t rtc_cid0 = ReadMmio32(rtc, 0xFF0);

    WriteMmio32(gpio, 0x400, 0x0F);
    WriteMmio32(gpio, 0x3FC, 0x05);
    uint32_t gpio_data = ReadMmio32(gpio, 0x3FC);
    uint32_t gpio_dir = ReadMmio32(gpio, 0x400);
    uint32_t gpio_pid0 = ReadMmio32(gpio, 0xFE0);
    uint32_t gpio_cid0 = ReadMmio32(gpio, 0xFF0);

    uint8_t data[8]{};
    WriteU16(data, 0x0000);
    fw_cfg.write_mmio(0x008, data, 2);
    std::string signature;
    for (size_t i = 0; i < 4; i++) {
      uint8_t byte[1]{};
      fw_cfg.read_mmio(0x000, byte, 1);
      signature.push_back(static_cast<char>(byte[0]));
    }
    WriteU16(data, 0x0001);
    fw_cfg.write_mmio(0x008, data, 2);
    uint8_t id_bytes[4]{};
    fw_cfg.read_mmio(0x000, id_bytes, sizeof(id_bytes));
    uint32_t fw_cfg_id = ReadU32(id_bytes);
    WriteU16(data, 0x0005);
    fw_cfg.write_mmio(0x008, data, 2);
    uint8_t cpu_bytes[2]{};
    fw_cfg.read_mmio(0x000, cpu_bytes, sizeof(cpu_bytes));
    uint32_t fw_cfg_cpus = ReadU16(cpu_bytes);
    WriteU16(data, 0x0019);
    fw_cfg.write_mmio(0x008, data, 2);
    uint8_t dir_bytes[4]{};
    fw_cfg.read_mmio(0x000, dir_bytes, sizeof(dir_bytes));
    uint32_t fw_cfg_file_count = ReadU32(dir_bytes);

    uint32_t empty_magic = ReadMmio32(empty_virtio, 0x000);
    uint32_t empty_device_id = ReadMmio32(empty_virtio, 0x008);
    uint32_t empty_vendor = ReadMmio32(empty_virtio, 0x00C);
    uint32_t pcie_vendor = ReadMmio32(pcie, 0x000);

    napi_value result = MakeObject(env);
    SetProp(env, result, "backend", MakeString(env, "hvf"));
    SetProp(env, result, "rtcNow", MakeU32(env, rtc_now));
    SetProp(env, result, "rtcLoaded", MakeU32(env, rtc_loaded));
    SetProp(env, result, "rtcPeripheralId0", MakeU32(env, rtc_pid0));
    SetProp(env, result, "rtcPrimeCellId0", MakeU32(env, rtc_cid0));
    SetProp(env, result, "gpioData", MakeU32(env, gpio_data));
    SetProp(env, result, "gpioDirection", MakeU32(env, gpio_dir));
    SetProp(env, result, "gpioPeripheralId0", MakeU32(env, gpio_pid0));
    SetProp(env, result, "gpioPrimeCellId0", MakeU32(env, gpio_cid0));
    SetProp(env, result, "fwCfgSignature", MakeString(env, signature));
    SetProp(env, result, "fwCfgId", MakeU32(env, fw_cfg_id));
    SetProp(env, result, "fwCfgCpus", MakeU32(env, fw_cfg_cpus));
    SetProp(env, result, "fwCfgFileCount", MakeU32(env, fw_cfg_file_count));
    SetProp(env, result, "emptyVirtioMagic", MakeU32(env, empty_magic));
    SetProp(env, result, "emptyVirtioDeviceId", MakeU32(env, empty_device_id));
    SetProp(env, result, "emptyVirtioVendor", MakeU32(env, empty_vendor));
    SetProp(env, result, "pcieEmptyVendor", MakeU32(env, pcie_vendor));
    return result;
  } catch (const std::exception& e) {
    return ThrowError(env, std::string("hvfDeviceSmoke: ") + e.what());
  }
}

// ─── RunVm ────────────────────────────────────────────────────────────────────
struct GuestExitReq {
  bool requested{false};
  uint8_t status{0};
};

enum class PsciCpuState : uint8_t {
  Off,
  OnPending,
  On,
};

struct HvfCpuSlot {
  hv_vcpu_t vcpu{};
  hv_vcpu_exit_t* exit_info{nullptr};
  bool created{false};
  bool live{false};
  bool powered{false};
  bool start_requested{false};
  bool paused{false};
  PsciCpuState state{PsciCpuState::Off};
  uint64_t entry{0};
  uint64_t context{0};
};

static bool CpuIndexFromMpidr(uint64_t mpidr, uint32_t cpus, uint32_t* out) {
  if ((mpidr & ~0xFFULL) != 0) return false;
  uint32_t index = static_cast<uint32_t>(mpidr & 0xFF);
  if (index >= cpus) return false;
  *out = index;
  return true;
}

static uint32_t PoweredCpuCountLocked(const std::vector<HvfCpuSlot>& slots) {
  uint32_t count = 0;
  for (const auto& slot : slots) {
    if (slot.state != PsciCpuState::Off) count++;
  }
  return std::max<uint32_t>(count, 1);
}

static int64_t PsciAffinityInfoState(const std::vector<HvfCpuSlot>& slots, uint64_t mpidr, uint64_t level) {
  if (level == 0) {
    uint32_t target = 0;
    if (!CpuIndexFromMpidr(mpidr, static_cast<uint32_t>(slots.size()), &target)) {
      return kPsciInvalidParams;
    }
    switch (slots[target].state) {
      case PsciCpuState::On: return 0;
      case PsciCpuState::Off: return 1;
      case PsciCpuState::OnPending: return 2;
    }
  }
  if (level <= 3 && mpidr == 0) {
    bool pending = false;
    for (const auto& slot : slots) {
      if (slot.state == PsciCpuState::On) return 0;
      if (slot.state == PsciCpuState::OnPending) pending = true;
    }
    return pending ? 2 : 1;
  }
  return kPsciInvalidParams;
}

static void InitVcpuRegisters(hv_vcpu_t vcpu, uint32_t cpu_index, uint64_t pc, uint64_t x0) {
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_PC,   pc), "hv_vcpu_set_reg PC");
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_X0,   x0), "hv_vcpu_set_reg X0");
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_X1,   0),  "hv_vcpu_set_reg X1");
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_X2,   0),  "hv_vcpu_set_reg X2");
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_X3,   0),  "hv_vcpu_set_reg X3");
  CheckHv(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3C5), "hv_vcpu_set_reg CPSR");
  CheckHv(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, cpu_index),
          "hv_vcpu_set_sys_reg MPIDR_EL1");
}

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
	  std::string net_host_ip  = GetString(env, argv[0], "netHostIp");
	  std::string net_guest_ip = GetString(env, argv[0], "netGuestIp");
	  std::string net_netmask  = GetString(env, argv[0], "netNetmask");
	  std::string net_dns      = GetString(env, argv[0], "netDns");
	  std::vector<NetPortForward> net_port_forwards = GetPortForwards(env, argv[0], "netPortForwards");
	  std::vector<AttachedDiskConfig> attached_disks = GetAttachedDisks(env, argv[0]);
	  std::string cmdline      = GetString(env, argv[0], "cmdline");
	  uint32_t mem_mib         = GetU32(env, argv[0], "memMiB", 256);
	  uint32_t cpus            = GetU32(env, argv[0], "cpus", 1);
	  uint32_t timeout_ms      = GetU32(env, argv[0], "timeoutMs", 0);
	  uint32_t console_limit   = GetU32(env, argv[0], "consoleLimit", 1024 * 1024);
	  bool interactive         = GetBool(env, argv[0], "interactive", false);
	  RunControl control       = GetRunControl(env, argv[0]);
	  bool debug               = HvfDebugEnabled();

	  if (kernel_path.empty()) return ThrowError(env, "kernelPath is required");
		  if (rootfs_path.empty()) return ThrowError(env, "rootfsPath is required");
		  if (cpus < 1 || cpus > 64) return ThrowError(env, "HVF backend supports 1-64 vCPUs");
		  control.set_state(kControlStateStarting);
		  ControlExitGuard control_exit_guard{control};

	  bool has_net = !tap_name.empty() && tap_name != "none";
  bool use_net = has_net &&
      (tap_name == "auto" ||
       tap_name == "slirp" ||
       tap_name.rfind("slirp:", 0) == 0 ||
       tap_name.rfind("socket_vmnet:", 0) == 0 ||
       tap_name.find("vmnet") != std::string::npos);
  uint32_t disk_count = 1 + static_cast<uint32_t>(attached_disks.size());
  uint32_t transport_count = disk_count + (use_net ? 1U : 0U);
  Check(transport_count <= kVirtioMmioCount,
        "HVF backend supports at most " + std::to_string(kVirtioMmioCount) +
            " virtio-mmio transports for disks and network");

	  try {
	    // ── Create VM ──────────────────────────────────────────────────────────────
	    CheckHv(hv_vm_create(nullptr), "hv_vm_create");

    struct VmGuard {
      ~VmGuard() { hv_vm_destroy(); }
	    } vm_guard;

    size_t hvf_redist_region_size = 0;
    CheckHv(hv_gic_get_redistributor_region_size(&hvf_redist_region_size),
            "hv_gic_get_redistributor_region_size");
    Check(uint64_t(GicRedistributorBytes(cpus)) <= hvf_redist_region_size,
          "HVF GIC redistributor region is too small for " + std::to_string(cpus) + " vCPUs");

    // ── Allocate guest RAM ─────────────────────────────────────────────────────
    uint64_t mem_bytes = uint64_t(mem_mib) * 1024ULL * 1024ULL;
    void* ram = mmap(nullptr, static_cast<size_t>(mem_bytes),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(ram != MAP_FAILED, ErrnoMessage("mmap guest RAM"));
    Mapping ram_map(ram, static_cast<size_t>(mem_bytes));

	    CheckHv(hv_vm_map(ram, kRamBase, mem_bytes,
	                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
	            "hv_vm_map");
	    HvMapGuard map_guard{kRamBase, mem_bytes, true};

	    GuestMemory mem(static_cast<uint8_t*>(ram), static_cast<size_t>(mem_bytes));

    // ── Load ARM64 kernel ──────────────────────────────────────────────────────
    Arm64ImageInfo kernel = LoadArm64Image(kernel_path, mem);

    // ── Build DTB ─────────────────────────────────────────────────────────────
    FdtBuilder fdt_builder;
    std::vector<uint8_t> dtb = fdt_builder.Build(
        kRamBase, mem_bytes, cpus, cmdline, use_net);
    Check(dtb.size() < kKernelOffset, "DTB too large");
    memcpy(mem.ptr(kDtbOffset, dtb.size()), dtb.data(), dtb.size());

	    // ── Create GIC v3 ─────────────────────────────────────────────────────────
	    hv_gic_config_t gic_config = hv_gic_config_create();
	    CheckHv(hv_gic_config_set_distributor_base(gic_config, kGicDistBase),
              "hv_gic_config_set_distributor_base");
	    CheckHv(hv_gic_config_set_redistributor_base(gic_config, kGicRedistBase),
              "hv_gic_config_set_redistributor_base");
    CheckHv(hv_gic_create(gic_config), "hv_gic_create");
    os_release(gic_config);

    // GIC inject callback (called from MMIO handlers)
    auto gic_inject = [debug](uint32_t intid, bool level) {
      hv_return_t rc = hv_gic_set_spi(intid, level);
      if (debug && rc != HV_SUCCESS) {
        fprintf(stderr, "[node-vmm hvf] hv_gic_set_spi intid=%u level=%d rc=%d\n",
                intid, level ? 1 : 0, rc);
      }
    };

    // ── UART ──────────────────────────────────────────────────────────────────
	    Pl011Uart uart;
	    uart.set_console_limit(static_cast<size_t>(console_limit));
	    uart.set_echo_stdout(interactive);
	    uart.set_debug(HvfDebugUartEnabled());
	    uart.set_console_output_notify([control]() { control.mark_console_output(); });
	    uart.set_gic_inject(gic_inject);

    // ── QEMU virt low MMIO probe devices ─────────────────────────────────────
    Pl031Rtc rtc;
    FwCfgMmio fw_cfg(static_cast<uint16_t>(std::min<uint32_t>(cpus, UINT16_MAX)));
    Pl061Gpio gpio;
    EmptyPcieHost pcie;
    EmptyVirtioMmio empty_virtio;

	    // ── VirtioBlk ─────────────────────────────────────────────────────────────
	    std::vector<std::unique_ptr<VirtioBlk>> blks;
	    blks.reserve(disk_count);
	    blks.push_back(std::make_unique<VirtioBlk>(mem, 0, rootfs_path, overlay_path, false, gic_inject));
	    for (uint32_t i = 0; i < attached_disks.size(); i++) {
	      const AttachedDiskConfig& disk = attached_disks[i];
	      blks.push_back(std::make_unique<VirtioBlk>(mem, i + 1, disk.path, "", disk.read_only, gic_inject));
	    }

    // ── VirtioNet ─────────────────────────────────────────────────────────────
	    std::unique_ptr<VirtioNet> net_dev;
	    if (use_net) {
	      if (guest_mac.empty()) guest_mac = "52:54:00:12:34:56";
	      net_dev = std::make_unique<VirtioNet>(mem, disk_count, guest_mac, gic_inject);
	      net_dev->start(tap_name, net_host_ip, net_guest_ip, net_netmask, net_dns, net_port_forwards);
	    }

    uint64_t entry_ipa = kRamBase + kernel.load_offset;
	    uint64_t dtb_ipa   = kRamBase + kDtbOffset;

	    std::vector<HvfCpuSlot> cpu_slots(cpus);
	    std::vector<std::thread> vcpu_threads;
    vcpu_threads.reserve(cpus);
    struct ThreadJoinGuard {
      std::vector<std::thread>& threads;
      ~ThreadJoinGuard() {
        for (auto& thread : threads) {
          if (thread.joinable()) thread.join();
        }
      }
    } vcpu_join_guard{vcpu_threads};

    std::mutex lifecycle_mu;
	    std::condition_variable lifecycle_cv;
	    bool start_vcpus = false;
	    uint32_t created_vcpus = 0;
	    uint32_t paused_vcpus = 0;
	    std::string thread_error;
    std::mutex result_mu;
    std::string exit_reason_name = "unknown";
    int exit_reason_code = -1;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> result_set{false};
    std::atomic<int> cancel_reason{0}; // 1=timeout, 2=stop, 3=pause
    std::atomic<uint64_t> runs{0};
    std::mutex device_mu;
    SignalIgnoreGuard sigint_guard(SIGINT, interactive);
    TerminalRawMode raw_mode(interactive);

	    auto wake_all_vcpus = [&]() {
      std::vector<hv_vcpu_t> live_vcpus;
      {
        std::lock_guard<std::mutex> lk(lifecycle_mu);
        live_vcpus.reserve(cpu_slots.size());
        for (const auto& slot : cpu_slots) {
          if (slot.live && slot.vcpu != 0) {
            live_vcpus.push_back(slot.vcpu);
          }
        }
      }
      if (!live_vcpus.empty()) {
        hv_vcpus_exit(live_vcpus.data(), static_cast<uint32_t>(live_vcpus.size()));
      }
	    };
    if (net_dev) {
      net_dev->set_wake_callback(wake_all_vcpus);
    }

    auto request_stop = [&](const std::string& reason, int code) {
      bool expected = false;
      if (result_set.compare_exchange_strong(expected, true)) {
        std::lock_guard<std::mutex> result_lk(result_mu);
        exit_reason_name = reason;
        exit_reason_code = code;
      }
      stop_requested.store(true, std::memory_order_relaxed);
      lifecycle_cv.notify_all();
      wake_all_vcpus();
    };

    auto poll_devices = [&]() {
      std::lock_guard<std::mutex> device_lk(device_mu);
      uart.refresh_irq();
      if (net_dev && net_dev->enabled() && (net_dev->needs_poll() || net_dev->notify_readable())) {
        net_dev->poll_rx();
      }
    };

    auto handle_pause_if_requested = [&](uint32_t cpu_index) {
      if (!control.enabled() || control.command() != kControlPause) return false;
      {
        std::lock_guard<std::mutex> lk(lifecycle_mu);
        if (!cpu_slots[cpu_index].paused) {
          cpu_slots[cpu_index].paused = true;
          paused_vcpus++;
        }
        if (paused_vcpus >= PoweredCpuCountLocked(cpu_slots)) {
          control.set_state(kControlStatePaused);
        }
      }
      while (!stop_requested.load(std::memory_order_relaxed) && control.command() == kControlPause) {
        struct timespec ts{0, 10 * 1000000L};
        nanosleep(&ts, nullptr);
      }
      {
        std::lock_guard<std::mutex> lk(lifecycle_mu);
        if (cpu_slots[cpu_index].paused) {
          cpu_slots[cpu_index].paused = false;
          if (paused_vcpus > 0) paused_vcpus--;
        }
      }
      return stop_requested.load(std::memory_order_relaxed);
    };

    auto trap_reason = [](const char* prefix, uint32_t ec, uint64_t syndrome, uint64_t ipa, uint64_t pc) {
      return UnsupportedExceptionReason(prefix, ec, syndrome, ipa, pc);
    };

    auto dispatch_mmio = [&](uint64_t ipa, bool is_write, uint8_t* data, uint32_t len) -> bool {
      std::lock_guard<std::mutex> device_lk(device_mu);
      if (ipa >= kUartBase && ipa < kUartBase + 0x1000) {
        uint64_t off = ipa - kUartBase;
        if (is_write) uart.write_mmio(off, data, len);
        else uart.read_mmio(off, data, len);
      } else if (ipa >= kPl031RtcBase && ipa < kPl031RtcBase + 0x1000) {
        uint64_t off = ipa - kPl031RtcBase;
        if (is_write) rtc.write_mmio(off, data, len);
        else rtc.read_mmio(off, data, len);
      } else if (ipa >= kFwCfgBase && ipa < kFwCfgBase + 0x18) {
        uint64_t off = ipa - kFwCfgBase;
        if (is_write) fw_cfg.write_mmio(off, data, len);
        else fw_cfg.read_mmio(off, data, len);
      } else if (ipa >= kPl061GpioBase && ipa < kPl061GpioBase + 0x1000) {
        uint64_t off = ipa - kPl061GpioBase;
        if (is_write) gpio.write_mmio(off, data, len);
        else gpio.read_mmio(off, data, len);
      } else if (ipa >= kPcieEcamBase && ipa < kPcieEcamBase + kPcieEcamSize) {
        uint64_t off = ipa - kPcieEcamBase;
        if (is_write) pcie.write_mmio(off, data, len);
        else pcie.read_mmio(off, data, len);
      } else if (ipa >= kPciePioBase && ipa < kPciePioBase + kPciePioSize) {
        uint64_t off = ipa - kPciePioBase;
        if (is_write) pcie.write_mmio(off, data, len);
        else pcie.read_mmio(off, data, len);
      } else if ([&]() {
        for (const auto& blk : blks) {
          if (blk->contains(ipa)) {
            if (is_write) blk->write_mmio(ipa, data, len);
            else blk->read_mmio(ipa, data, len);
            return true;
          }
        }
        return false;
      }()) {
        // Handled by one of the virtio-blk transports above.
      } else if (net_dev && net_dev->contains(ipa)) {
        if (is_write) net_dev->write_mmio(ipa, data, len);
        else net_dev->read_mmio(ipa, data, len);
      } else if (IsQemuVirtioMmioAddress(ipa)) {
        uint64_t off = VirtioMmioOffset(ipa);
        if (is_write) empty_virtio.write_mmio(off, data, len);
        else empty_virtio.read_mmio(off, data, len);
      } else if (ipa >= kExitDevBase && ipa < kExitDevBase + 0x1000) {
        if (is_write) request_stop("guest-exit", data[0]);
      } else {
        return false;
      }
      return true;
    };

    auto vcpu_main = [&](uint32_t cpu_index) {
      hv_vcpu_t vcpu{};
      hv_vcpu_exit_t* exit_info = nullptr;
      bool active = false;
      try {
        CheckHv(hv_vcpu_create(&vcpu, &exit_info, nullptr), "hv_vcpu_create");
        active = true;
        CheckHv(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, cpu_index),
                "hv_vcpu_set_sys_reg MPIDR_EL1");
        {
          std::lock_guard<std::mutex> lk(lifecycle_mu);
	          cpu_slots[cpu_index].vcpu = vcpu;
	          cpu_slots[cpu_index].exit_info = exit_info;
	          cpu_slots[cpu_index].created = true;
	          cpu_slots[cpu_index].live = true;
	          cpu_slots[cpu_index].powered = (cpu_index == 0);
	          cpu_slots[cpu_index].state = cpu_index == 0 ? PsciCpuState::On : PsciCpuState::Off;
	          created_vcpus++;
        }
        lifecycle_cv.notify_all();

        {
          std::unique_lock<std::mutex> lk(lifecycle_mu);
          lifecycle_cv.wait(lk, [&]() {
            return start_vcpus || stop_requested.load(std::memory_order_relaxed);
          });
        }

        bool boot_cpu_started = false;
        for (;;) {
          uint64_t start_pc = 0;
          uint64_t start_x0 = 0;
          if (cpu_index == 0 && !boot_cpu_started) {
            boot_cpu_started = true;
            start_pc = entry_ipa;
            start_x0 = dtb_ipa;
          } else {
	            std::unique_lock<std::mutex> lk(lifecycle_mu);
	            if (!stop_requested.load(std::memory_order_relaxed)) {
	              cpu_slots[cpu_index].powered = false;
	              cpu_slots[cpu_index].state = PsciCpuState::Off;
	              cpu_slots[cpu_index].paused = false;
	              lifecycle_cv.notify_all();
	            }
            lifecycle_cv.wait(lk, [&]() {
              return stop_requested.load(std::memory_order_relaxed) || cpu_slots[cpu_index].start_requested;
            });
            if (stop_requested.load(std::memory_order_relaxed)) break;
            start_pc = cpu_slots[cpu_index].entry;
            start_x0 = cpu_slots[cpu_index].context;
	            cpu_slots[cpu_index].start_requested = false;
	            cpu_slots[cpu_index].powered = true;
	            cpu_slots[cpu_index].state = PsciCpuState::On;
	          }

          InitVcpuRegisters(vcpu, cpu_index, start_pc, start_x0);
          control.set_state(kControlStateRunning);

          uint32_t debug_exit_logs = 0;
          uint32_t debug_psci_logs = 0;
          bool debug_exit_seen = false;
	          uint32_t last_debug_reason = 0;
	          uint64_t last_debug_pc = 0;
	          uint64_t last_debug_syndrome = 0;
	          uint64_t last_debug_ipa = 0;
	          bool vtimer_pending = false;

	          for (;;) {
            if (stop_requested.load(std::memory_order_relaxed)) break;
            if (control.enabled()) {
              int32_t command = control.command();
              if (command == kControlStop) {
                cancel_reason.store(2, std::memory_order_relaxed);
                control.set_state(kControlStateStopping);
                request_stop("host-stop", 0);
                break;
              }
              if (handle_pause_if_requested(cpu_index)) break;
              control.set_state(kControlStateRunning);
            }

	            raw_mode.restore_after_sigcont();
	            poll_devices();
	            if (vtimer_pending) {
	              uint64_t pending = 0;
	              uint64_t active_gic = 0;
	              CheckHv(hv_gic_get_redistributor_reg(vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, &pending),
	                      "hv_gic_get_redistributor_reg GICR_ISPENDR0");
	              CheckHv(hv_gic_get_redistributor_reg(vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISACTIVER0, &active_gic),
	                      "hv_gic_get_redistributor_reg GICR_ISACTIVER0");
	              if (((pending | active_gic) & kVtimerIntidBit) == 0) {
	                CheckHv(hv_vcpu_set_vtimer_mask(vcpu, false), "hv_vcpu_set_vtimer_mask");
	                vtimer_pending = false;
	              }
	            }

	            hv_return_t run_rc = hv_vcpu_run(vcpu);
            if (run_rc != HV_SUCCESS) {
              request_stop("hv-error", static_cast<int>(run_rc));
              break;
            }
            runs.fetch_add(1, std::memory_order_relaxed);

            uint32_t reason = exit_info->reason;
            if (debug) {
              uint64_t pc = 0;
              hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
              uint64_t syndrome = exit_info->exception.syndrome;
              uint64_t ipa = exit_info->exception.physical_address;
              bool changed = !debug_exit_seen || reason != last_debug_reason ||
                pc != last_debug_pc || syndrome != last_debug_syndrome || ipa != last_debug_ipa;
              if (changed && debug_exit_logs < 128) {
                fprintf(stderr, "[node-vmm hvf] cpu=%u exit reason=%u pc=0x%llx syndrome=0x%llx ipa=0x%llx va=0x%llx\n",
                        cpu_index,
                        reason,
                        static_cast<unsigned long long>(pc),
                        static_cast<unsigned long long>(syndrome),
                        static_cast<unsigned long long>(ipa),
                        static_cast<unsigned long long>(exit_info->exception.virtual_address));
                debug_exit_logs++;
                debug_exit_seen = true;
                last_debug_reason = reason;
                last_debug_pc = pc;
                last_debug_syndrome = syndrome;
                last_debug_ipa = ipa;
              } else if (changed && debug_exit_logs == 128) {
                fprintf(stderr, "[node-vmm hvf] further exit logs suppressed for cpu=%u\n", cpu_index);
                debug_exit_logs++;
              }
            }

            if (reason == HV_EXIT_REASON_CANCELED) {
              if (stop_requested.load(std::memory_order_relaxed)) break;
              if (cancel_reason.load(std::memory_order_relaxed) == 3) continue;
              continue;
            }

	            if (reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
	              CheckHv(hv_gic_set_redistributor_reg(vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, kVtimerIntidBit),
	                      "hv_gic_set_redistributor_reg GICR_ISPENDR0");
	              vtimer_pending = true;
	              continue;
	            }

	            if (reason == HV_EXIT_REASON_EXCEPTION) {
	              uint64_t syndrome = exit_info->exception.syndrome;
	              uint32_t ec = EsrEC(syndrome);

	              if (ec == 0x17) {
	                hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(kPsciNotSupported));
	                uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
	                hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
	                continue;
	              }

	              if (ec == 0x16) {
                uint64_t x0 = 0;
                uint64_t x1 = 0;
                uint64_t x2 = 0;
                uint64_t x3 = 0;
                hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
                hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
                hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
                if (debug && debug_psci_logs < 64) {
                  fprintf(stderr, "[node-vmm hvf] cpu=%u psci hvc/smc x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                          cpu_index,
                          static_cast<unsigned long long>(x0),
                          static_cast<unsigned long long>(x1),
                          static_cast<unsigned long long>(x2),
                          static_cast<unsigned long long>(x3));
                  debug_psci_logs++;
                } else if (debug && debug_psci_logs == 64) {
                  fprintf(stderr, "[node-vmm hvf] further psci logs suppressed for cpu=%u\n", cpu_index);
                  debug_psci_logs++;
                }

                bool cpu_powered_off = false;
                if (x0 == kPsciVersion) {
                  hv_vcpu_set_reg(vcpu, HV_REG_X0, 0x00010000ULL); // PSCI 1.0
                } else if (x0 == kPsciFeatures) {
                  hv_vcpu_set_reg(vcpu, HV_REG_X0,
                                  PsciFeatureSupported(x1) ? uint64_t(kPsciSuccess) : static_cast<uint64_t>(kPsciNotSupported));
                } else if (x0 == kPsciMigrateInfoType) {
                  hv_vcpu_set_reg(vcpu, HV_REG_X0, 2);
	                } else if (x0 == kPsciAffinityInfo32 || x0 == kPsciAffinityInfo64) {
	                  int64_t ret = kPsciInvalidParams;
	                  {
	                    std::lock_guard<std::mutex> lk(lifecycle_mu);
	                    ret = PsciAffinityInfoState(cpu_slots, x1, x2);
	                  }
	                  hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(ret));
                } else if (x0 == kPsciCpuSuspend32 || x0 == kPsciCpuSuspend64) {
                  hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(kPsciDenied));
                } else if (x0 == kPsciCpuOn32 || x0 == kPsciCpuOn64) {
                  uint32_t target = 0;
                  int64_t ret = kPsciInvalidParams;
                  if (CpuIndexFromMpidr(x1, cpus, &target) && x2 != 0) {
                    std::lock_guard<std::mutex> lk(lifecycle_mu);
	                    if (target == 0 || cpu_slots[target].state != PsciCpuState::Off || cpu_slots[target].start_requested) {
	                      ret = kPsciAlreadyOn;
	                    } else {
	                      cpu_slots[target].entry = x2;
	                      cpu_slots[target].context = x3;
	                      cpu_slots[target].start_requested = true;
	                      cpu_slots[target].state = PsciCpuState::OnPending;
	                      ret = kPsciSuccess;
	                    }
                  }
                  lifecycle_cv.notify_all();
                  hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(ret));
                } else if (x0 == kPsciCpuOff) {
                  if (cpu_index == 0) {
                    request_stop("hlt", 0);
	                  } else {
	                    std::lock_guard<std::mutex> lk(lifecycle_mu);
	                    cpu_slots[cpu_index].powered = false;
	                    cpu_slots[cpu_index].state = PsciCpuState::Off;
	                    cpu_slots[cpu_index].paused = false;
                    cpu_powered_off = true;
                  }
                  lifecycle_cv.notify_all();
                } else if (x0 == kPsciSystemOff) {
                  request_stop("shutdown", 0);
                } else if (x0 == kPsciSystemReset) {
                  request_stop("reset", 0);
                } else {
                  hv_vcpu_set_reg(vcpu, HV_REG_X0, static_cast<uint64_t>(kPsciNotSupported));
                }
                if (cpu_powered_off) break;
                continue;
              }

              if (ec == 0x01) {
                uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
                hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
                {
                  std::lock_guard<std::mutex> device_lk(device_mu);
                  if (net_dev && net_dev->enabled()) net_dev->poll_rx();
                }
                struct timespec ts{0, 100000};
                nanosleep(&ts, nullptr);
                continue;
              }

              if (ec == 0x18) {
                uint64_t pc = 0;
                hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
                if (HandleSystemRegisterTrap(vcpu, syndrome)) {
                  hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
                  continue;
                }
                uint64_t ipa = exit_info->exception.physical_address;
                request_stop(trap_reason("unhandled-sysreg", ec, syndrome, ipa, pc), 1);
                break;
              }

              if (ec == 0x24 || ec == 0x20) {
                uint64_t ipa = exit_info->exception.physical_address;
                uint64_t pc = 0;
                hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);

                if (ec == 0x24 && EsrISV(syndrome)) {
                  bool is_write = EsrWnR(syndrome);
                  uint32_t sas   = EsrSAS(syndrome);
                  uint32_t len   = 1u << sas;
                  uint32_t srt   = EsrSRT(syndrome);

                  uint8_t data[8] = {};
                  if (is_write) {
                    uint64_t reg_val = 0;
                    hv_vcpu_get_reg(vcpu, static_cast<hv_reg_t>(srt), &reg_val);
                    for (uint32_t i = 0; i < len && i < 8; i++) {
                      data[i] = static_cast<uint8_t>(reg_val >> (8*i));
                    }
                  }

                  if (!dispatch_mmio(ipa, is_write, data, len)) {
                    request_stop(trap_reason("unhandled-mmio", ec, syndrome, ipa, pc), 1);
                    break;
                  }

                  if (!is_write) {
                    uint64_t reg_val = 0;
                    for (uint32_t i = 0; i < len && i < 8; i++) {
                      reg_val |= uint64_t(data[i]) << (8*i);
                    }
                    if (EsrSSE(syndrome)) {
                      uint64_t sign_bit = uint64_t(1) << (8*len - 1);
                      if (reg_val & sign_bit) reg_val |= ~((sign_bit << 1) - 1);
                    }
                    hv_vcpu_set_reg(vcpu, static_cast<hv_reg_t>(srt), reg_val);
                  }

                  hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
                  continue;
                }

                request_stop(trap_reason("unsupported-mmio-trap", ec, syndrome, ipa, pc), 1);
                break;
              }

              uint64_t pc = 0; hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
              uint64_t ipa = exit_info->exception.physical_address;
              request_stop(trap_reason("unhandled-trap", ec, syndrome, ipa, pc), 1);
              break;
            }

            {
              uint64_t pc = 0;
              hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
              request_stop("unknown-exit reason=" + std::to_string(reason) +
                           " syndrome=" + Hex64(exit_info->exception.syndrome) +
                           " ipa=" + Hex64(exit_info->exception.physical_address) +
                           " pc=" + Hex64(pc),
                           static_cast<int>(reason));
            }
            break;
          }
          if (stop_requested.load(std::memory_order_relaxed) || cpu_index == 0) break;
        }
      } catch (const std::exception& e) {
        {
          std::lock_guard<std::mutex> lk(lifecycle_mu);
          if (thread_error.empty()) thread_error = e.what();
        }
        request_stop("hv-error", -1);
	      }
	      if (active) {
	        {
	          std::lock_guard<std::mutex> lk(lifecycle_mu);
	          cpu_slots[cpu_index].live = false;
	          cpu_slots[cpu_index].vcpu = 0;
	          cpu_slots[cpu_index].exit_info = nullptr;
	          cpu_slots[cpu_index].powered = false;
	          cpu_slots[cpu_index].state = PsciCpuState::Off;
	        }
	        hv_vcpu_destroy(vcpu);
	      }
	    };

    for (uint32_t i = 0; i < cpus; i++) {
      vcpu_threads.emplace_back(vcpu_main, i);
    }

    {
      std::unique_lock<std::mutex> lk(lifecycle_mu);
      lifecycle_cv.wait(lk, [&]() {
        return created_vcpus == cpus || !thread_error.empty();
      });
      if (!thread_error.empty()) {
        throw std::runtime_error(thread_error);
      }
	      start_vcpus = true;
    }
    lifecycle_cv.notify_all();

    std::atomic<bool> monitor_done{false};
    std::thread monitor_thread;
    MonitorThreadGuard monitor_guard{monitor_done, monitor_thread};
    bool needs_network_wake = net_dev && net_dev->enabled();
    if (timeout_ms > 0 || control.enabled() || needs_network_wake) {
      uint64_t start_us = MonotonicMicros();
      monitor_thread = std::thread([timeout_ms, start_us, needs_network_wake, &control, &cancel_reason, &monitor_done, &request_stop, &wake_all_vcpus,
                                    timeout_extension_us = uint64_t(0), pause_active = false, pause_start_us = uint64_t(0)]() mutable {
        while (!monitor_done.load(std::memory_order_relaxed)) {
          uint64_t now_us = MonotonicMicros();
          if (control.enabled()) {
            int32_t command = control.command();
            if (command == kControlStop) {
              cancel_reason.store(2, std::memory_order_relaxed);
              control.set_state(kControlStateStopping);
              request_stop("host-stop", 0);
              return;
            }
            if (command == kControlPause) {
              if (!pause_active) {
                pause_active = true;
                pause_start_us = now_us;
              }
              cancel_reason.store(3, std::memory_order_relaxed);
              wake_all_vcpus();
            } else if (pause_active) {
              timeout_extension_us += now_us - pause_start_us;
              pause_active = false;
            }
          }
          if (timeout_ms > 0) {
            uint64_t paused_us = pause_active ? now_us - pause_start_us : 0;
            uint64_t deadline_us = start_us + uint64_t(timeout_ms) * 1000ULL +
                                   timeout_extension_us + paused_us;
            if (now_us >= deadline_us) {
              cancel_reason.store(1, std::memory_order_relaxed);
              request_stop("timeout", 124);
              return;
            }
          }
          if (needs_network_wake) {
            wake_all_vcpus();
          }
          struct timespec ts{0, 10 * 1000000L};
          nanosleep(&ts, nullptr);
        }
      });
    }

    std::atomic<bool> input_done{false};
    std::thread input_thread;
    InputThreadGuard input_guard{input_done, input_thread};
    if (interactive) {
      input_thread = std::thread([&uart, &input_done, debug, &wake_all_vcpus, &request_stop]() {
        uint8_t buf[256];
        while (!input_done.load(std::memory_order_relaxed)) {
          struct pollfd pfd{};
          pfd.fd = STDIN_FILENO;
          pfd.events = POLLIN;
          int prc = poll(&pfd, 1, 20);
          if (prc < 0) {
            if (errno == EINTR) continue;
            break;
          }
          if (prc == 0) continue;
          if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
          if ((pfd.revents & POLLIN) == 0) continue;
          ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
          if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
          }
          if (n == 0) break;
          if (debug) {
            fprintf(stderr, "[node-vmm hvf] stdin bytes=%zd\n", n);
          }
          size_t start = 0;
          const size_t count = static_cast<size_t>(n);
          for (size_t i = 0; i < count; i++) {
            if (buf[i] == 0x1D) { // Ctrl-], QEMU-style host escape for this backend.
              if (i > start) {
                uart.inject_bytes(buf + start, i - start);
              }
              request_stop("host-stop", 0);
              input_done.store(true, std::memory_order_relaxed);
              wake_all_vcpus();
              return;
            }
          }
          uart.inject_bytes(buf + start, count - start);
          wake_all_vcpus();
        }
      });
    }

    for (auto& thread : vcpu_threads) {
      if (thread.joinable()) thread.join();
    }
    input_guard.stop();
    monitor_guard.stop();
    if (!thread_error.empty()) {
      throw std::runtime_error(thread_error);
    }
	    if (net_dev) net_dev->stop_vmnet();
	    map_guard.unmap();

    // ── Build result ──────────────────────────────────────────────────────────
    napi_value result = MakeObject(env);
    SetProp(env, result, "exitReason",     MakeString(env, exit_reason_name));
    SetProp(env, result, "exitReasonCode", MakeU32(env, static_cast<uint32_t>(exit_reason_code)));
    SetProp(env, result, "runs",           MakeU32(env, static_cast<uint32_t>(std::min<uint64_t>(runs.load(), UINT32_MAX))));
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
  napi_value fn_probe, fn_run, fn_fdt_smoke, fn_pl011_smoke, fn_device_smoke;
  napi_create_function(env, "probeHvf", NAPI_AUTO_LENGTH, ProbeHvf, nullptr, &fn_probe);
  napi_create_function(env, "runVm",    NAPI_AUTO_LENGTH, RunVm,    nullptr, &fn_run);
  napi_create_function(env, "hvfFdtSmoke", NAPI_AUTO_LENGTH, HvfFdtSmoke, nullptr, &fn_fdt_smoke);
  napi_create_function(env, "hvfPl011Smoke", NAPI_AUTO_LENGTH, HvfPl011Smoke, nullptr, &fn_pl011_smoke);
  napi_create_function(env, "hvfDeviceSmoke", NAPI_AUTO_LENGTH, HvfDeviceSmoke, nullptr, &fn_device_smoke);
  napi_set_named_property(env, exports, "probeHvf", fn_probe);
  napi_set_named_property(env, exports, "runVm",    fn_run);
  napi_set_named_property(env, exports, "hvfFdtSmoke", fn_fdt_smoke);
  napi_set_named_property(env, exports, "hvfPl011Smoke", fn_pl011_smoke);
  napi_set_named_property(env, exports, "hvfDeviceSmoke", fn_device_smoke);
  return exports;
}

NAPI_MODULE(node_vmm_native, Init)

} // extern "C"
