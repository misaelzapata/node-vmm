#include <node_api.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <linux/kvm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef NODE_VMM_HAVE_LIBSLIRP
extern "C" {
#if __has_include(<libslirp.h>)
#include <libslirp.h>
#else
#include <slirp/libslirp.h>
#endif
}
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace {

constexpr uint64_t kBootParamsAddr = 0x7000;
constexpr uint64_t kPageTableBase = 0x9000;
constexpr uint64_t kCmdlineAddr = 0x20000;
constexpr uint64_t kVirtioMmioBase = 0xD0000000;
constexpr uint64_t kVirtioStride = 0x1000;
constexpr uint32_t kVirtioMmioIrqBase = 5;
constexpr uint32_t kMaxIoApicPins = 24;
constexpr uint16_t kCom1Base = 0x3F8;
constexpr uint32_t kCom1IRQ = 4;
constexpr uint16_t kNodeVmmExitPort = 0x501;
constexpr uint16_t kNodeVmmConsolePort = 0x600;
constexpr uint64_t kGdtAddr = 0x500;
constexpr uint64_t kIdtAddr = 0x520;
constexpr uint32_t kMaxQueueSize = 256;
constexpr uint32_t kKernelCmdlineMax = 2048;
constexpr uint64_t kSnapshotPageSize = 4096;
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

std::string ErrnoMessage(const std::string& what) {
  return what + ": " + strerror(errno);
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
  snprintf(name, sizeof(name), "V%03u", index);
  return std::string(name, 4);
}

void CheckErr(int rc, const std::string& what) {
  if (rc < 0) {
    throw std::runtime_error(ErrnoMessage(what));
  }
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

int Ioctl(int fd, unsigned long request, unsigned long arg = 0) {
  int rc;
  do {
    rc = ioctl(fd, request, arg);
  } while (rc < 0 && errno == EINTR);
  return rc;
}

int IoctlPtr(int fd, unsigned long request, void* arg) {
  return Ioctl(fd, request, reinterpret_cast<unsigned long>(arg));
}

struct Fd {
  int fd{-1};
  Fd() = default;
  explicit Fd(int value) : fd(value) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }
  ~Fd() { reset(); }
  void reset(int next = -1) {
    if (fd >= 0) {
      close(fd);
    }
    fd = next;
  }
  int get() const { return fd; }
};

struct Mapping {
  void* addr{MAP_FAILED};
  size_t len{0};
  Mapping() = default;
  Mapping(void* value, size_t size) : addr(value), len(size) {}
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept : addr(other.addr), len(other.len) {
    other.addr = MAP_FAILED;
    other.len = 0;
  }
  Mapping& operator=(Mapping&& other) noexcept {
    if (this != &other) {
      reset();
      addr = other.addr;
      len = other.len;
      other.addr = MAP_FAILED;
      other.len = 0;
    }
    return *this;
  }
  ~Mapping() { reset(); }
  void reset() {
    if (addr != MAP_FAILED && len > 0) {
      munmap(addr, len);
    }
    addr = MAP_FAILED;
    len = 0;
  }
  uint8_t* bytes() const { return reinterpret_cast<uint8_t*>(addr); }
};

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
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Check(n >= 0, ErrnoMessage("read " + path));
    Check(n != 0, "short read " + path);
    done += static_cast<size_t>(n);
  }
  return data;
}

void PreadAll(int fd, uint8_t* dst, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread(fd, dst + done, len - done, off + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Check(n >= 0, ErrnoMessage("pread disk"));
    Check(n != 0, "short disk read");
    done += static_cast<size_t>(n);
  }
}

void PwriteAll(int fd, const uint8_t* src, size_t len, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pwrite(fd, src + done, len - done, off + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Check(n >= 0, ErrnoMessage("pwrite disk"));
    Check(n != 0, "short disk write");
    done += static_cast<size_t>(n);
  }
}

void WriteAll(int fd, const uint8_t* src, size_t len, const std::string& what) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(fd, src + done, len - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Check(n >= 0, ErrnoMessage(what));
    Check(n != 0, "short write " + what);
    done += static_cast<size_t>(n);
  }
}

bool IsZeroPage(const uint8_t* src, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (src[i] != 0) {
      return false;
    }
  }
  return true;
}

uint64_t WriteSparseMemoryFile(const std::string& path, const uint8_t* src, size_t len) {
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  CheckErr(fd.get(), "open sparse memory " + path);
  CheckErr(ftruncate(fd.get(), static_cast<off_t>(len)), "truncate sparse memory " + path);
  uint64_t pages_written = 0;
  for (size_t off = 0; off < len; off += kSnapshotPageSize) {
    size_t chunk = std::min<size_t>(kSnapshotPageSize, len - off);
    if (IsZeroPage(src + off, chunk)) {
      continue;
    }
    PwriteAll(fd.get(), src + off, chunk, static_cast<off_t>(off));
    pages_written++;
  }
  return pages_written;
}

void ReadAll(int fd, uint8_t* dst, size_t len, const std::string& what) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = read(fd, dst + done, len - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Check(n >= 0, ErrnoMessage(what));
    Check(n != 0, "short read " + what);
    done += static_cast<size_t>(n);
  }
}

uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

class GuestMemory {
 public:
  GuestMemory() = default;
  GuestMemory(uint8_t* data, size_t size) : data_(data), size_(size) {}

  uint8_t* ptr(uint64_t gpa, uint64_t len) {
    Check(gpa <= size_ && len <= size_ - gpa, "guest memory access out of bounds");
    return data_ + gpa;
  }
  const uint8_t* ptr(uint64_t gpa, uint64_t len) const {
    Check(gpa <= size_ && len <= size_ - gpa, "guest memory access out of bounds");
    return data_ + gpa;
  }
  size_t size() const { return size_; }
  uint8_t* data() const { return data_; }

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
};

struct KvmSystem {
  Fd kvm;
  Fd vm;
  Fd memfd;
  Mapping mem;
  size_t mem_size{0};
  int vcpu_mmap_size{0};
  bool dirty_log{false};

  GuestMemory guest() { return GuestMemory(mem.bytes(), mem_size); }
};

struct Vcpu {
  Fd fd;
  Mapping run_map;
  struct kvm_run* run{nullptr};
};

struct GuestExit {
  bool requested{false};
  uint32_t status{0};
};

constexpr char kRamSnapshotMagic[] = "NODEKVM-RAM-001";
constexpr char kDirtySnapshotMagic[] = "NODEKVM-DIRTY-001";
constexpr uint32_t kRamSnapshotVersion = 1;
constexpr uint32_t kDirtySnapshotVersion = 1;

struct RamSnapshotHeader {
  char magic[sizeof(kRamSnapshotMagic)];
  uint32_t version;
  uint32_t mem_mib;
  uint64_t mem_size;
};

struct VcpuCoreState {
  struct kvm_regs regs;
  struct kvm_sregs sregs;
  struct kvm_fpu fpu;
};

struct DirtySnapshotHeader {
  char magic[sizeof(kDirtySnapshotMagic)];
  uint32_t version;
  uint32_t page_size;
  uint64_t page_count;
  uint64_t dirty_pages;
};

VcpuCoreState CaptureVcpuCoreState(Vcpu& vcpu) {
  VcpuCoreState state {};
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_REGS, &state.regs), "KVM_GET_REGS snapshot");
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_SREGS, &state.sregs), "KVM_GET_SREGS snapshot");
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_FPU, &state.fpu), "KVM_GET_FPU snapshot");
  return state;
}

void RestoreVcpuCoreState(Vcpu& vcpu, const VcpuCoreState& state) {
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_SREGS, const_cast<struct kvm_sregs*>(&state.sregs)), "KVM_SET_SREGS restore");
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_FPU, const_cast<struct kvm_fpu*>(&state.fpu)), "KVM_SET_FPU restore");
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_REGS, const_cast<struct kvm_regs*>(&state.regs)), "KVM_SET_REGS restore");
}

void EnsureDirectory(const std::string& path) {
  if (mkdir(path.c_str(), 0700) == 0) {
    return;
  }
  if (errno == EEXIST) {
    struct stat st {};
    CheckErr(stat(path.c_str(), &st), "stat snapshot directory " + path);
    Check(S_ISDIR(st.st_mode), "snapshot path is not a directory: " + path);
    return;
  }
  throw std::runtime_error(ErrnoMessage("mkdir snapshot directory " + path));
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty() || dir == "/") {
    return dir + name;
  }
  return dir + "/" + name;
}

void WriteRamSnapshotFiles(
    const std::string& dir,
    uint32_t mem_mib,
    const KvmSystem& sys,
    const VcpuCoreState& state,
    std::string* ram_path,
    std::string* state_path) {
  EnsureDirectory(dir);
  *ram_path = JoinPath(dir, "mem.bin");
  *state_path = JoinPath(dir, "vcpu.bin");

  RamSnapshotHeader header {};
  memcpy(header.magic, kRamSnapshotMagic, sizeof(kRamSnapshotMagic));
  header.version = kRamSnapshotVersion;
  header.mem_mib = mem_mib;
  header.mem_size = sys.mem_size;

  Fd state_fd(open(state_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  CheckErr(state_fd.get(), "open snapshot state " + *state_path);
  WriteAll(state_fd.get(), reinterpret_cast<const uint8_t*>(&header), sizeof(header), "write snapshot state header");
  WriteAll(state_fd.get(), reinterpret_cast<const uint8_t*>(&state), sizeof(state), "write snapshot vCPU state");

  WriteSparseMemoryFile(*ram_path, sys.mem.bytes(), sys.mem_size);
}

VcpuCoreState ReadRamSnapshotState(const std::string& state_path, uint32_t* mem_mib, uint64_t* mem_size) {
  Fd state_fd(open(state_path.c_str(), O_RDONLY | O_CLOEXEC));
  CheckErr(state_fd.get(), "open snapshot state " + state_path);
  RamSnapshotHeader header {};
  ReadAll(state_fd.get(), reinterpret_cast<uint8_t*>(&header), sizeof(header), "read snapshot state header");
  Check(memcmp(header.magic, kRamSnapshotMagic, sizeof(kRamSnapshotMagic)) == 0, "invalid native RAM snapshot magic");
  Check(header.version == kRamSnapshotVersion, "unsupported native RAM snapshot version");
  VcpuCoreState state {};
  ReadAll(state_fd.get(), reinterpret_cast<uint8_t*>(&state), sizeof(state), "read snapshot vCPU state");
  *mem_mib = header.mem_mib;
  *mem_size = header.mem_size;
  return state;
}

std::vector<uint64_t> GetDirtyPages(KvmSystem& sys) {
  uint64_t page_count = sys.mem_size / kSnapshotPageSize;
  std::vector<uint64_t> bitmap(static_cast<size_t>((page_count + 63) / 64), 0);
  struct kvm_dirty_log dirty {};
  dirty.slot = 0;
  dirty.dirty_bitmap = bitmap.data();
  CheckErr(IoctlPtr(sys.vm.get(), KVM_GET_DIRTY_LOG, &dirty), "KVM_GET_DIRTY_LOG");
  std::vector<uint64_t> pages;
  for (uint64_t word_index = 0; word_index < bitmap.size(); word_index++) {
    uint64_t word = bitmap[static_cast<size_t>(word_index)];
    while (word != 0) {
      uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
      uint64_t page = word_index * 64 + bit;
      if (page < page_count) {
        pages.push_back(page);
      }
      word &= word - 1;
    }
  }
  return pages;
}

void WriteFullRamFile(const std::string& path, const KvmSystem& sys) {
  WriteSparseMemoryFile(path, sys.mem.bytes(), sys.mem_size);
}

void WriteVcpuStateFile(const std::string& path, const VcpuCoreState& state) {
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  CheckErr(fd.get(), "open dirty snapshot state " + path);
  WriteAll(fd.get(), reinterpret_cast<const uint8_t*>(&state), sizeof(state), "write dirty snapshot vCPU state");
}

VcpuCoreState ReadVcpuStateFile(const std::string& path) {
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  CheckErr(fd.get(), "open dirty snapshot state " + path);
  VcpuCoreState state {};
  ReadAll(fd.get(), reinterpret_cast<uint8_t*>(&state), sizeof(state), "read dirty snapshot vCPU state");
  return state;
}

void WriteDirtySnapshotFile(
    const std::string& path,
    const KvmSystem& sys,
    const std::vector<uint64_t>& dirty_pages) {
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  CheckErr(fd.get(), "open dirty RAM delta " + path);
  DirtySnapshotHeader header {};
  memcpy(header.magic, kDirtySnapshotMagic, sizeof(kDirtySnapshotMagic));
  header.version = kDirtySnapshotVersion;
  header.page_size = static_cast<uint32_t>(kSnapshotPageSize);
  header.page_count = sys.mem_size / kSnapshotPageSize;
  header.dirty_pages = dirty_pages.size();
  WriteAll(fd.get(), reinterpret_cast<const uint8_t*>(&header), sizeof(header), "write dirty snapshot header");
  for (uint64_t page : dirty_pages) {
    uint64_t offset = page * kSnapshotPageSize;
    WriteAll(fd.get(), reinterpret_cast<const uint8_t*>(&page), sizeof(page), "write dirty snapshot page index");
    WriteAll(fd.get(), sys.mem.bytes() + offset, kSnapshotPageSize, "write dirty snapshot page");
  }
}

uint64_t ApplyDirtySnapshotFile(const std::string& path, KvmSystem& sys) {
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  CheckErr(fd.get(), "open dirty RAM delta " + path);
  DirtySnapshotHeader header {};
  ReadAll(fd.get(), reinterpret_cast<uint8_t*>(&header), sizeof(header), "read dirty snapshot header");
  Check(memcmp(header.magic, kDirtySnapshotMagic, sizeof(kDirtySnapshotMagic)) == 0, "invalid dirty snapshot magic");
  Check(header.version == kDirtySnapshotVersion, "unsupported dirty snapshot version");
  Check(header.page_size == kSnapshotPageSize, "dirty snapshot page size mismatch");
  Check(header.page_count == sys.mem_size / kSnapshotPageSize, "dirty snapshot page count mismatch");
  for (uint64_t i = 0; i < header.dirty_pages; i++) {
    uint64_t page = 0;
    ReadAll(fd.get(), reinterpret_cast<uint8_t*>(&page), sizeof(page), "read dirty snapshot page index");
    Check(page < header.page_count, "dirty snapshot page index out of bounds");
    ReadAll(fd.get(), sys.mem.bytes() + page * kSnapshotPageSize, kSnapshotPageSize, "read dirty snapshot page");
  }
  return header.dirty_pages;
}

KvmSystem CreateVmWithMemory(
    uint32_t mem_mib,
    bool with_irqchip,
    const std::string& ram_path = "",
    bool private_ram = false,
    bool dirty_log = false) {
  KvmSystem sys;
  sys.kvm.reset(open("/dev/kvm", O_RDWR | O_CLOEXEC));
  CheckErr(sys.kvm.get(), "open /dev/kvm");
  int version = Ioctl(sys.kvm.get(), KVM_GET_API_VERSION);
  Check(version == 12, "unexpected KVM API version: " + std::to_string(version));

  int vm_fd = Ioctl(sys.kvm.get(), KVM_CREATE_VM);
  CheckErr(vm_fd, "KVM_CREATE_VM");
  sys.vm.reset(vm_fd);

  int mmap_size = Ioctl(sys.kvm.get(), KVM_GET_VCPU_MMAP_SIZE);
  CheckErr(mmap_size, "KVM_GET_VCPU_MMAP_SIZE");
  sys.vcpu_mmap_size = mmap_size;

  if (with_irqchip) {
    CheckErr(Ioctl(sys.vm.get(), KVM_SET_TSS_ADDR, 0xFFFBD000UL), "KVM_SET_TSS_ADDR");
    CheckErr(Ioctl(sys.vm.get(), KVM_CREATE_IRQCHIP), "KVM_CREATE_IRQCHIP");
    struct kvm_pit_config pit {};
    pit.flags = 1;
    CheckErr(IoctlPtr(sys.vm.get(), KVM_CREATE_PIT2, &pit), "KVM_CREATE_PIT2");
  }

  uint64_t bytes = uint64_t(mem_mib) * 1024ULL * 1024ULL;
  Check(bytes > 0 && bytes <= uint64_t(1) << 32, "invalid guest memory size");
  int map_flags = MAP_SHARED | MAP_NORESERVE;
  if (!ram_path.empty()) {
    Fd fd(open(ram_path.c_str(), O_RDONLY | O_CLOEXEC));
    CheckErr(fd.get(), "open snapshot RAM " + ram_path);
    struct stat st {};
    CheckErr(fstat(fd.get(), &st), "stat snapshot RAM " + ram_path);
    Check(st.st_size == static_cast<off_t>(bytes), "snapshot RAM size does not match memMiB");
    sys.memfd = std::move(fd);
    map_flags = (private_ram ? MAP_PRIVATE : MAP_SHARED) | MAP_NORESERVE;
  } else {
    int memfd = static_cast<int>(syscall(SYS_memfd_create, "node-vmm-guest-ram", MFD_CLOEXEC));
    CheckErr(memfd, "memfd_create guest memory");
    sys.memfd.reset(memfd);
    CheckErr(ftruncate(sys.memfd.get(), static_cast<off_t>(bytes)), "ftruncate guest memory");
  }
  void* mem = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, map_flags, sys.memfd.get(), 0);
  Check(mem != MAP_FAILED, ErrnoMessage("mmap guest memory"));
  sys.mem = Mapping(mem, static_cast<size_t>(bytes));
  sys.mem_size = static_cast<size_t>(bytes);
  sys.dirty_log = dirty_log;
  if (ram_path.empty()) {
    madvise(sys.mem.addr, sys.mem.len, MADV_HUGEPAGE);
  }

  struct kvm_userspace_memory_region region {};
  region.slot = 0;
  region.flags = dirty_log ? KVM_MEM_LOG_DIRTY_PAGES : 0;
  region.guest_phys_addr = 0;
  region.memory_size = bytes;
  region.userspace_addr = reinterpret_cast<uint64_t>(sys.mem.addr);
  CheckErr(IoctlPtr(sys.vm.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");
  return sys;
}

KvmSystem CreateVm(uint32_t mem_mib, bool with_irqchip) {
  return CreateVmWithMemory(mem_mib, with_irqchip);
}

Vcpu CreateVcpu(KvmSystem& sys, int id) {
  Vcpu vcpu;
  int fd = Ioctl(sys.vm.get(), KVM_CREATE_VCPU, static_cast<unsigned long>(id));
  CheckErr(fd, "KVM_CREATE_VCPU");
  vcpu.fd.reset(fd);
  void* run = mmap(nullptr, static_cast<size_t>(sys.vcpu_mmap_size), PROT_READ | PROT_WRITE, MAP_SHARED, vcpu.fd.get(), 0);
  Check(run != MAP_FAILED, ErrnoMessage("mmap vcpu"));
  vcpu.run_map = Mapping(run, static_cast<size_t>(sys.vcpu_mmap_size));
  vcpu.run = reinterpret_cast<struct kvm_run*>(run);
  return vcpu;
}

void CompletePendingIoBeforeSnapshot(Vcpu& vcpu) {
  vcpu.run->immediate_exit = 1;
  int rc = ioctl(vcpu.fd.get(), KVM_RUN, 0);
  vcpu.run->immediate_exit = 0;
  if (rc < 0 && (errno == EINTR || errno == EAGAIN)) {
    return;
  }
  CheckErr(rc, "KVM_RUN complete pending snapshot I/O");
}

void SetupRealMode(Vcpu& vcpu, uint64_t rip) {
  struct kvm_sregs sregs {};
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_SREGS, &sregs), "KVM_GET_SREGS");
  sregs.cs.base = 0;
  sregs.cs.selector = 0;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_SREGS, &sregs), "KVM_SET_SREGS");
  struct kvm_regs regs {};
  regs.rip = rip;
  regs.rflags = 0x2;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_REGS, &regs), "KVM_SET_REGS");
}

struct Cpuid256 {
  uint32_t nent;
  uint32_t padding;
  struct kvm_cpuid_entry2 entries[256];
};

uint32_t CeilLog2(uint32_t value) {
  uint32_t out = 0;
  uint32_t max = value > 0 ? value - 1 : 0;
  while (max > 0) {
    out++;
    max >>= 1;
  }
  return out;
}

void SetupCpuid(KvmSystem& sys, Vcpu& vcpu, uint32_t cpu_id = 0, uint32_t cpu_count = 1) {
  Cpuid256 cpuid {};
  cpuid.nent = 256;
  CheckErr(IoctlPtr(sys.kvm.get(), KVM_GET_SUPPORTED_CPUID, &cpuid), "KVM_GET_SUPPORTED_CPUID");
  uint32_t logical_cpus = std::min<uint32_t>(cpu_count, 255);
  uint32_t topology_shift = CeilLog2(cpu_count);
  for (uint32_t i = 0; i < cpuid.nent; i++) {
    kvm_cpuid_entry2& entry = cpuid.entries[i];
    if (entry.function == 1) {
      entry.ebx = (entry.ebx & 0x00FFFFFFU) | ((cpu_id & 0xFFU) << 24);
      entry.ebx = (entry.ebx & 0xFF00FFFFU) | (logical_cpus << 16);
      if (cpu_count > 1) {
        entry.edx |= (1U << 28);
      }
    } else if (entry.function == 0x0B || entry.function == 0x1F) {
      entry.edx = cpu_id;
      if (entry.index == 0) {
        entry.eax = 0;
        entry.ebx = 1;
        entry.ecx = (1U << 8) | entry.index;
      } else if (entry.index == 1) {
        entry.eax = topology_shift;
        entry.ebx = cpu_count;
        entry.ecx = (2U << 8) | entry.index;
      } else {
        entry.eax = 0;
        entry.ebx = 0;
        entry.ecx = entry.index;
      }
    }
  }
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_CPUID2, &cpuid), "KVM_SET_CPUID2");
}

struct Msrs12 {
  uint32_t nmsrs;
  uint32_t pad;
  struct kvm_msr_entry entries[12];
};

void SetupMsrs(Vcpu& vcpu) {
  Msrs12 msrs {};
  msrs.nmsrs = 12;
  msrs.entries[0].index = 0x174;
  msrs.entries[1].index = 0x175;
  msrs.entries[2].index = 0x176;
  msrs.entries[3].index = 0x10;
  msrs.entries[4].index = 0x1A0;
  msrs.entries[4].data = 1;
  msrs.entries[5].index = 0xC0000080;
  msrs.entries[5].data = 0x500;
  msrs.entries[6].index = 0xC0000081;
  msrs.entries[7].index = 0xC0000082;
  msrs.entries[8].index = 0xC0000083;
  msrs.entries[9].index = 0xC0000084;
  msrs.entries[10].index = 0xC0000102;
  msrs.entries[11].index = 0x2FF;
  msrs.entries[11].data = (1U << 11) | 0x6;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_MSRS, &msrs), "KVM_SET_MSRS");
}

void SetupFpu(Vcpu& vcpu) {
  struct kvm_fpu fpu {};
  fpu.fcw = 0x37F;
  fpu.mxcsr = 0x1F80;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_FPU, &fpu), "KVM_SET_FPU");
}

void SetupLapic(Vcpu& vcpu) {
  struct kvm_lapic_state lapic {};
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_LAPIC, &lapic), "KVM_GET_LAPIC");
  lapic.regs[0x350] = 0x00;
  lapic.regs[0x351] = 0x07;
  lapic.regs[0x352] = 0x00;
  lapic.regs[0x353] = 0x00;
  lapic.regs[0x360] = 0x00;
  lapic.regs[0x361] = 0x04;
  lapic.regs[0x362] = 0x00;
  lapic.regs[0x363] = 0x00;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_LAPIC, &lapic), "KVM_SET_LAPIC");
}

void SetMpState(Vcpu& vcpu, int state) {
  struct kvm_mp_state mp_state {};
  mp_state.mp_state = state;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_MP_STATE, &mp_state), "KVM_SET_MP_STATE");
}

void BuildPageTables(GuestMemory mem, uint64_t base) {
  constexpr uint64_t page_size = 0x1000;
  constexpr uint64_t flags = 0x07;
  constexpr uint64_t huge = 0x83;
  uint64_t pml4 = base;
  uint64_t pdpt = base + page_size;
  uint64_t pd = base + 2 * page_size;
  WriteU64(mem.ptr(pml4, 8), pdpt | flags);
  for (uint64_t i = 0; i < 4; i++) {
    uint64_t this_pd = pd + i * page_size;
    WriteU64(mem.ptr(pdpt + i * 8, 8), this_pd | flags);
    for (uint64_t j = 0; j < 512; j++) {
      uint64_t phys = (i * 512 + j) * 0x200000ULL;
      WriteU64(mem.ptr(this_pd + j * 8, 8), phys | huge);
    }
  }
}

void SetupLongMode(Vcpu& vcpu, GuestMemory mem, uint64_t entry) {
  BuildPageTables(mem, kPageTableBase);
  uint64_t gdt[] = {
      0x0000000000000000ULL,
      0x00AF9B000000FFFFULL,
      0x00CF93000000FFFFULL,
      0x008F8B000000FFFFULL,
  };
  for (size_t i = 0; i < 4; i++) {
    WriteU64(mem.ptr(kGdtAddr + i * 8, 8), gdt[i]);
  }
  WriteU64(mem.ptr(kIdtAddr, 8), 0);

  struct kvm_sregs sregs {};
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_SREGS, &sregs), "KVM_GET_SREGS");
  sregs.cs.base = 0;
  sregs.cs.limit = 0xFFFFF;
  sregs.cs.selector = 0x08;
  sregs.cs.type = 11;
  sregs.cs.present = 1;
  sregs.cs.s = 1;
  sregs.cs.l = 1;
  sregs.cs.g = 1;

  auto set_data = [](struct kvm_segment& seg) {
    seg.base = 0;
    seg.limit = 0xFFFFF;
    seg.selector = 0x10;
    seg.type = 3;
    seg.present = 1;
    seg.s = 1;
    seg.db = 1;
    seg.g = 1;
  };
  set_data(sregs.ds);
  set_data(sregs.es);
  set_data(sregs.fs);
  set_data(sregs.gs);
  set_data(sregs.ss);
  sregs.tr.base = 0;
  sregs.tr.limit = 0xFFFFF;
  sregs.tr.selector = 0x18;
  sregs.tr.type = 11;
  sregs.tr.present = 1;
  sregs.tr.g = 1;
  sregs.gdt.base = kGdtAddr;
  sregs.gdt.limit = 31;
  sregs.idt.base = kIdtAddr;
  sregs.idt.limit = 7;
  sregs.cr0 |= 0x80000001ULL;
  sregs.cr3 = kPageTableBase;
  sregs.cr4 |= 0x20;
  sregs.efer |= 0x500;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_SREGS, &sregs), "KVM_SET_SREGS");

  struct kvm_regs regs {};
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_GET_REGS, &regs), "KVM_GET_REGS");
  regs.rip = entry;
  regs.rsi = kBootParamsAddr;
  regs.rflags = 0x2;
  regs.rsp = 0x8FF0;
  regs.rbp = 0x8FF0;
  CheckErr(IoctlPtr(vcpu.fd.get(), KVM_SET_REGS, &regs), "KVM_SET_REGS");
}

void SetupBootstrapVcpu(KvmSystem& sys, Vcpu& vcpu, GuestMemory mem, uint64_t entry, uint32_t cpu_count) {
  SetupCpuid(sys, vcpu, 0, cpu_count);
  SetupMsrs(vcpu);
  SetupFpu(vcpu);
  SetupLongMode(vcpu, mem, entry);
  SetupLapic(vcpu);
}

void SetupApplicationVcpu(KvmSystem& sys, Vcpu& vcpu, uint32_t cpu_id, uint32_t cpu_count) {
  SetupCpuid(sys, vcpu, cpu_id, cpu_count);
  SetupMsrs(vcpu);
  SetupFpu(vcpu);
  SetupRealMode(vcpu, 0);
  SetupLapic(vcpu);
  SetMpState(vcpu, KVM_MP_STATE_UNINITIALIZED);
}

struct KernelInfo {
  uint64_t entry{0};
  uint64_t kernel_end{0};
};

KernelInfo LoadElfKernel(GuestMemory mem, const std::string& path) {
  std::vector<uint8_t> data = ReadFile(path);
  Check(data.size() >= sizeof(Elf64_Ehdr), "kernel is too small");
  auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data.data());
  Check(memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0, "kernel must be an ELF vmlinux for KVM v1");
  Check(eh->e_ident[EI_CLASS] == ELFCLASS64, "kernel must be ELF64");
  Check(eh->e_ident[EI_DATA] == ELFDATA2LSB, "kernel must be little-endian ELF");
  Check(eh->e_machine == EM_X86_64, "kernel must be x86_64 ELF");
  Check(eh->e_phentsize == sizeof(Elf64_Phdr), "kernel ELF program header size is unsupported");
  uint64_t ph_size = CheckedMul(uint64_t(eh->e_phnum), sizeof(Elf64_Phdr), "kernel program header table");
  CheckRange(data.size(), eh->e_phoff, ph_size, "kernel program header table");

  KernelInfo info {};
  info.entry = eh->e_entry;
  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    uint64_t ph_off = CheckedAdd(eh->e_phoff, CheckedMul(uint64_t(i), sizeof(Elf64_Phdr), "kernel program header offset"),
                                 "kernel program header offset");
    auto* ph = reinterpret_cast<const Elf64_Phdr*>(data.data() + ph_off);
    if (ph->p_type != PT_LOAD) {
      continue;
    }
    Check(ph->p_filesz <= ph->p_memsz, "kernel segment file size exceeds memory size");
    CheckRange(data.size(), ph->p_offset, ph->p_filesz, "kernel segment file range");
    CheckRange(mem.size(), ph->p_paddr, ph->p_memsz, "kernel segment guest range");
    memcpy(mem.ptr(ph->p_paddr, ph->p_filesz), data.data() + ph->p_offset, static_cast<size_t>(ph->p_filesz));
    if (ph->p_memsz > ph->p_filesz) {
      uint64_t zero_start = CheckedAdd(ph->p_paddr, ph->p_filesz, "kernel zero-fill range");
      memset(mem.ptr(zero_start, ph->p_memsz - ph->p_filesz), 0, static_cast<size_t>(ph->p_memsz - ph->p_filesz));
    }
    info.kernel_end = std::max(info.kernel_end, CheckedAdd(ph->p_paddr, ph->p_memsz, "kernel end"));
  }
  Check(info.entry != 0, "kernel ELF entrypoint is zero");
  return info;
}

void PutE820(GuestMemory mem, uint64_t off, uint64_t addr, uint64_t size, uint32_t typ) {
  WriteU64(mem.ptr(off, 20), addr);
  WriteU64(mem.ptr(off + 8, 12), size);
  WriteU32(mem.ptr(off + 16, 4), typ);
}

void WriteBootParams(GuestMemory mem, uint64_t mem_bytes, const std::string& cmdline) {
  uint64_t base = kBootParamsAddr;
  mem.ptr(base, 4096);
  mem.ptr(kCmdlineAddr, cmdline.size() + 1);
  memcpy(mem.ptr(kCmdlineAddr, cmdline.size() + 1), cmdline.c_str(), cmdline.size() + 1);

  mem.ptr(base + 0x2D0, 80);
  mem.ptr(base + 0x1E8, 1)[0] = 4;
  PutE820(mem, base + 0x2D0, 0x00000000, 0x0009FC00, 1);
  PutE820(mem, base + 0x2D0 + 20, 0x0009FC00, 0x00040400, 2);
  PutE820(mem, base + 0x2D0 + 40, 0x000E0000, 0x00020000, 2);
  if (mem_bytes > 0x00100000) {
    PutE820(mem, base + 0x2D0 + 60, 0x00100000, mem_bytes - 0x00100000, 1);
  }
  WriteU16(mem.ptr(base + 0x1FA, 2), 0xFFFF);
  WriteU16(mem.ptr(base + 0x1FE, 2), 0xAA55);
  WriteU32(mem.ptr(base + 0x202, 4), 0x53726448);
  mem.ptr(base + 0x210, 1)[0] = 0xFF;
  mem.ptr(base + 0x211, 1)[0] |= 0x01;
  WriteU32(mem.ptr(base + 0x228, 4), static_cast<uint32_t>(kCmdlineAddr));
  WriteU32(mem.ptr(base + 0x238, 4), static_cast<uint32_t>(cmdline.size()));
}

uint8_t Checksum(const std::vector<uint8_t>& data, size_t start, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum = static_cast<uint8_t>(sum + data[start + i]);
  }
  return static_cast<uint8_t>(~sum + 1);
}

void WriteMpTable(GuestMemory mem, int cpus) {
  constexpr uint64_t base_ram_end = 0xA0000;
  constexpr uint32_t apic_default_base = 0xFEE00000;
  constexpr uint32_t io_apic_base = 0xFEC00000;
  constexpr uint8_t apic_version = 0x14;
  constexpr int max_legacy_gsi = 23;
  int size = 16 + 44 + (20 * cpus) + 8 + 8 + (8 * (max_legacy_gsi + 1)) + (8 * 2);
  uint64_t start = (base_ram_end - static_cast<uint64_t>(size)) & ~0xFULL;
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  uint32_t table_addr = static_cast<uint32_t>(start + 16);
  uint8_t io_apic_id = static_cast<uint8_t>(cpus + 1);

  memcpy(buf.data(), "_MP_", 4);
  WriteU32(buf.data() + 4, table_addr);
  buf[8] = 1;
  buf[9] = 4;
  buf[10] = Checksum(buf, 0, 16);

  std::vector<uint8_t> entries;
  uint16_t entry_count = 0;
  for (int cpu = 0; cpu < cpus; cpu++) {
    uint8_t entry[20] {};
    entry[0] = 0;
    entry[1] = static_cast<uint8_t>(cpu);
    entry[2] = apic_version;
    entry[3] = 0x01 | (cpu == 0 ? 0x02 : 0);
    WriteU32(entry + 4, 0x600);
    WriteU32(entry + 8, 0x200 | 0x001);
    entries.insert(entries.end(), entry, entry + sizeof(entry));
    entry_count++;
  }
  uint8_t bus[8] {};
  bus[0] = 1;
  memcpy(bus + 2, "ISA   ", 6);
  entries.insert(entries.end(), bus, bus + sizeof(bus));
  entry_count++;

  uint8_t ioapic[8] {};
  ioapic[0] = 2;
  ioapic[1] = io_apic_id;
  ioapic[2] = apic_version;
  ioapic[3] = 1;
  WriteU32(ioapic + 4, io_apic_base);
  entries.insert(entries.end(), ioapic, ioapic + sizeof(ioapic));
  entry_count++;

  for (int irq = 0; irq <= max_legacy_gsi; irq++) {
    uint8_t entry[8] {};
    entry[0] = 3;
    entry[1] = 0;
    entry[4] = 0;
    entry[5] = static_cast<uint8_t>(irq);
    entry[6] = io_apic_id;
    entry[7] = static_cast<uint8_t>(irq);
    entries.insert(entries.end(), entry, entry + sizeof(entry));
    entry_count++;
  }
  uint8_t extint[8] {};
  extint[0] = 4;
  extint[1] = 3;
  entries.insert(entries.end(), extint, extint + sizeof(extint));
  entry_count++;

  uint8_t nmi[8] {};
  nmi[0] = 4;
  nmi[1] = 1;
  nmi[6] = 0xFF;
  nmi[7] = 1;
  entries.insert(entries.end(), nmi, nmi + sizeof(nmi));
  entry_count++;

  uint8_t* header = buf.data() + 16;
  memcpy(header, "PCMP", 4);
  WriteU16(header + 4, static_cast<uint16_t>(44 + entries.size()));
  header[6] = 4;
  memcpy(header + 8, "FC      ", 8);
  memcpy(header + 16, "000000000000", 12);
  WriteU16(header + 34, entry_count);
  WriteU32(header + 36, apic_default_base);
  memcpy(buf.data() + 16 + 44, entries.data(), entries.size());
  header[7] = Checksum(buf, 16, 44 + entries.size());
  memcpy(mem.ptr(start, buf.size()), buf.data(), buf.size());
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

std::vector<uint8_t> BuildResourceTemplate(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second) {
  std::vector<uint8_t> payload;
  payload.insert(payload.end(), first.begin(), first.end());
  payload.insert(payload.end(), second.begin(), second.end());
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
  dev_children.push_back(AppendName("_CRS", BuildResourceTemplate(
      ResMemory32Fixed(true, static_cast<uint32_t>(base), static_cast<uint32_t>(kVirtioStride)),
      ResInterrupt(true, true, false, false, irq))));
  return AppendDevice(name, dev_children);
}

std::vector<uint8_t> BuildDsdtBody(bool network_enabled, uint32_t disk_count) {
  CheckVirtioMmioDeviceCount(disk_count + (network_enabled ? 1 : 0));
  std::vector<std::vector<uint8_t>> children;
  for (uint32_t index = 0; index < disk_count; index++) {
    children.push_back(BuildVirtioMmioDevice(
        VirtioMmioDeviceName(index), index, VirtioMmioBase(index), VirtioMmioIrq(index)));
  }
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
  memcpy(out.data(), sig, 4);
  WriteU32(out.data() + 4, len);
  out[8] = rev;
  memcpy(out.data() + 10, "GOCRKR", 6);
  memcpy(out.data() + 16, table_id, 8);
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

std::vector<uint8_t> BuildFadt(uint64_t dsdt_addr) {
  std::vector<uint8_t> out(276);
  auto hdr = BuildSdtHeader("FACP", 276, 6, "GCFADT01");
  memcpy(out.data(), hdr.data(), hdr.size());
  WriteU32(out.data() + 40, static_cast<uint32_t>(dsdt_addr));
  WriteU16(out.data() + 109, 1 << 2);
  WriteU32(out.data() + 112, (1 << 20) | (1 << 4) | (1 << 5));
  out[131] = 5;
  WriteU64(out.data() + 140, dsdt_addr);
  memcpy(out.data() + 268, "GOCRKVM ", 8);
  FinalizeSdt(out);
  return out;
}

std::vector<uint8_t> BuildMadt(int cpus) {
  std::vector<uint8_t> body(8 + 12 + cpus * 8);
  WriteU32(body.data(), 0xFEE00000);
  uint8_t* ioapic = body.data() + 8;
  ioapic[0] = 1;
  ioapic[1] = 12;
  WriteU32(ioapic + 4, 0xFEC00000);
  size_t off = 20;
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
  memcpy(out.data(), "RSD PTR ", 8);
  memcpy(out.data() + 9, "GOCRKR", 6);
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

uint64_t CreateAcpiTables(GuestMemory mem, bool network_enabled, int cpus, uint32_t disk_count) {
  constexpr uint64_t rsdp_addr = 0x000E0000;
  uint64_t cursor = 0x000A0000;
  auto write_table = [&](const std::vector<uint8_t>& table) {
    cursor = AlignUp(cursor, 8);
    uint64_t addr = cursor;
    memcpy(mem.ptr(addr, table.size()), table.data(), table.size());
    cursor += table.size();
    Check(cursor < rsdp_addr, "ACPI tables overflow low memory");
    return addr;
  };
  auto dsdt = BuildDsdtTable(network_enabled, disk_count);
  uint64_t dsdt_addr = write_table(dsdt);
  uint64_t fadt_addr = write_table(BuildFadt(dsdt_addr));
  uint64_t madt_addr = write_table(BuildMadt(cpus));
  uint64_t rsdt_addr = write_table(BuildRsdt({fadt_addr, madt_addr}));
  uint64_t xsdt_addr = write_table(BuildXsdt({fadt_addr, madt_addr}));
  auto rsdp = BuildRsdp(rsdt_addr, xsdt_addr);
  memcpy(mem.ptr(rsdp_addr, rsdp.size()), rsdp.data(), rsdp.size());
  return rsdp_addr;
}

struct IRQRoutingTable {
  uint32_t nr;
  uint32_t flags;
  struct kvm_irq_routing_entry entries[64];
};

void SetIrqRouting(KvmSystem& sys) {
  IRQRoutingTable table {};
  for (uint32_t irq = 0; irq < kMaxIoApicPins; irq++) {
    if (irq < 16) {
      struct kvm_irq_routing_entry& pic = table.entries[table.nr++];
      pic.gsi = irq;
      pic.type = KVM_IRQ_ROUTING_IRQCHIP;
      pic.u.irqchip.irqchip = irq < 8 ? KVM_IRQCHIP_PIC_MASTER : KVM_IRQCHIP_PIC_SLAVE;
      pic.u.irqchip.pin = irq < 8 ? irq : irq - 8;
    }

    struct kvm_irq_routing_entry& ioapic = table.entries[table.nr++];
    ioapic.gsi = irq;
    ioapic.type = KVM_IRQ_ROUTING_IRQCHIP;
    ioapic.u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
    ioapic.u.irqchip.pin = irq;
  }
  CheckErr(IoctlPtr(sys.vm.get(), KVM_SET_GSI_ROUTING, &table), "KVM_SET_GSI_ROUTING");
}

void IrqLine(KvmSystem& sys, uint32_t irq, bool level) {
  struct kvm_irq_level irq_level {};
  irq_level.irq = irq;
  irq_level.level = level ? 1 : 0;
  CheckErr(IoctlPtr(sys.vm.get(), KVM_IRQ_LINE, &irq_level), "KVM_IRQ_LINE");
}

class Uart {
 public:
  explicit Uart(
      size_t limit,
      bool echo_stdout = true,
      KvmSystem* sys = nullptr,
      std::function<void()> console_output_notify = {})
      : limit_(limit),
        echo_stdout_(echo_stdout),
        sys_(sys),
        console_output_notify_(std::move(console_output_notify)) {}

  uint8_t read(uint16_t offset) {
    bool dlab = (lcr_ & 0x80) != 0;
    switch (offset) {
      case 0:
        if (dlab) {
          return dll_;
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          uint8_t value = 0;
          if (!rx_.empty()) {
            value = rx_.front();
            rx_.pop_front();
          }
          update_interrupt_locked();
          return value;
        }
      case 1:
        return dlab ? dlh_ : ier_;
      case 2:
        {
          std::lock_guard<std::mutex> lock(mu_);
          uint8_t value = 0xC0 | iir_;
          if (iir_ == 0x02) {
            set_irq(false);
            update_interrupt_locked();
          }
          return value;
        }
      case 3:
        return lcr_;
      case 4:
        return mcr_;
      case 5:
        {
          std::lock_guard<std::mutex> lock(mu_);
          return static_cast<uint8_t>(0x60 | (rx_.empty() ? 0x00 : 0x01));
        }
      case 6:
        return 0xB0;
      case 7:
        return scr_;
      default:
        return 0;
    }
  }

  void write(uint16_t offset, uint8_t value) {
    bool dlab = (lcr_ & 0x80) != 0;
    switch (offset) {
      case 0:
        if (dlab) {
          dll_ = value;
        } else {
          handle_tx_byte(value);
          if (ier_ & 0x02) {
            raise_thr_empty();
          }
        }
        break;
      case 1:
        if (dlab) {
          dlh_ = value;
        } else {
          ier_ = value;
          {
            std::lock_guard<std::mutex> lock(mu_);
            update_interrupt_locked();
          }
        }
        break;
      case 3:
        lcr_ = value;
        break;
      case 4:
        mcr_ = value;
        break;
      case 7:
        scr_ = value;
        break;
      default:
        break;
    }
  }

	  std::string console() const {
	    std::lock_guard<std::mutex> lock(mu_);
	    return console_;
	  }
	
	  bool contains(const std::string& needle) const {
	    std::lock_guard<std::mutex> lock(mu_);
	    if (needle == "reboot: System halted") {
	      return halted_seen_;
	    }
	    if (needle == "Restarting system") {
	      return restarting_seen_;
	    }
	    return console_.find(needle) != std::string::npos;
	  }

  void enqueue_rx(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < len && rx_.size() < 4096; i++) {
      rx_.push_back(data[i]);
    }
    update_interrupt_locked();
  }

  void emit_bytes(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    emit_tx_locked(std::string(reinterpret_cast<const char*>(data), len));
  }

 private:
  mutable std::mutex mu_;
  uint8_t ier_{0};
  uint8_t iir_{0x01};
  uint8_t lcr_{0};
  uint8_t mcr_{0x03};
  uint8_t scr_{0};
  uint8_t dll_{0};
  uint8_t dlh_{0};
  size_t limit_{1024 * 1024};
  bool echo_stdout_{true};
  KvmSystem* sys_{nullptr};
  bool console_output_seen_{false};
  std::function<void()> console_output_notify_;
  std::string console_;
  char last_stdout_byte_{0};
	  std::deque<uint8_t> rx_;
	  std::string terminal_query_;
	  bool halted_seen_{false};
	  bool restarting_seen_{false};
	  size_t halted_match_{0};
	  size_t restarting_match_{0};

  void set_irq(bool level) {
    if (sys_ != nullptr) {
      IrqLine(*sys_, kCom1IRQ, level);
    }
  }

  void raise_thr_empty() {
    std::lock_guard<std::mutex> lock(mu_);
    update_interrupt_locked();
  }

	  void emit_tx_locked(const std::string& bytes) {
	    if (bytes.empty()) {
	      return;
	    }
	    track_console_markers(bytes);
	    size_t available = limit_ > console_.size() ? limit_ - console_.size() : 0;
	    if (available > 0) {
	      console_.append(bytes.data(), std::min(available, bytes.size()));
	    }
    if (echo_stdout_) {
      if (!console_output_seen_) {
        console_output_seen_ = true;
        if (console_output_notify_) {
          console_output_notify_();
        }
      }
      write_stdout(bytes);
    }
  }

  void write_stdout(const std::string& bytes) {
    std::string normalized;
    normalized.reserve(bytes.size() + 16);
    for (char byte : bytes) {
      if (byte == '\n' && last_stdout_byte_ != '\r') {
        normalized.push_back('\r');
      }
      normalized.push_back(byte);
      last_stdout_byte_ = byte;
    }
    ssize_t ignored = ::write(STDOUT_FILENO, normalized.data(), normalized.size());
    (void)ignored;
  }

  void enqueue_rx_locked(const char* data, size_t len, bool front = false) {
    if (front) {
      for (size_t i = len; i > 0 && rx_.size() < 4096; i--) {
        rx_.push_front(static_cast<uint8_t>(data[i - 1]));
      }
    } else {
      for (size_t i = 0; i < len && rx_.size() < 4096; i++) {
        rx_.push_back(static_cast<uint8_t>(data[i]));
      }
    }
    update_interrupt_locked();
  }

	  void handle_tx_byte(uint8_t value) {
    static const std::string query = "\x1b[6n";
    std::lock_guard<std::mutex> lock(mu_);
    if (!terminal_query_.empty() || value == 0x1B) {
      terminal_query_.push_back(static_cast<char>(value));
      if (query.rfind(terminal_query_, 0) == 0) {
        if (terminal_query_ == query) {
          static const char response[] = "\x1b[1;1R";
          enqueue_rx_locked(response, sizeof(response) - 1, true);
          terminal_query_.clear();
        }
        return;
      }
      emit_tx_locked(terminal_query_);
      terminal_query_.clear();
      return;
    }

	    char byte = static_cast<char>(value);
	    emit_tx_locked(std::string(&byte, 1));
	  }

	  static size_t AdvanceMatch(const char* pattern, size_t pattern_len, size_t current, char byte) {
	    while (current > 0 && byte != pattern[current]) {
	      current--;
	    }
	    if (byte == pattern[current]) {
	      current++;
	    }
	    if (current == pattern_len) {
	      return pattern_len;
	    }
	    return current;
	  }

	  void track_console_markers(const std::string& bytes) {
	    static constexpr char kHalted[] = "reboot: System halted";
	    static constexpr char kRestarting[] = "Restarting system";
	    for (char byte : bytes) {
	      if (!halted_seen_) {
	        halted_match_ = AdvanceMatch(kHalted, sizeof(kHalted) - 1, halted_match_, byte);
	        halted_seen_ = halted_match_ == sizeof(kHalted) - 1;
	      }
	      if (!restarting_seen_) {
	        restarting_match_ = AdvanceMatch(kRestarting, sizeof(kRestarting) - 1, restarting_match_, byte);
	        restarting_seen_ = restarting_match_ == sizeof(kRestarting) - 1;
	      }
	    }
	  }

  void update_interrupt_locked() {
    if ((ier_ & 0x01) && !rx_.empty()) {
      iir_ = 0x04;
      set_irq(true);
      return;
    }
    if (ier_ & 0x02) {
      iir_ = 0x02;
      set_irq(true);
      return;
    }
    iir_ = 0x01;
    set_irq(false);
  }
};

struct Desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct DescChain {
  std::array<Desc, kMaxQueueSize> items {};
  size_t size{0};

  void push(Desc desc) {
    Check(size < items.size(), "virtio descriptor chain too long");
    items[size++] = desc;
  }

  Desc& operator[](size_t index) { return items[index]; }
  const Desc& operator[](size_t index) const { return items[index]; }
  bool empty() const { return size == 0; }
};

class VirtioBlk {
 public:
  VirtioBlk(
      KvmSystem& sys,
      GuestMemory mem,
      uint64_t mmio_base,
      uint32_t irq,
      const std::string& path,
      const std::string& overlay_path,
      bool read_only)
      : sys_(sys), mem_(mem), mmio_base_(mmio_base), irq_(irq), read_only_(read_only) {
    bool overlay = !overlay_path.empty();
    Check(!(read_only_ && overlay), "read-only disk cannot use an overlay");
    int flags = (overlay || read_only_) ? (O_RDONLY | O_CLOEXEC) : (O_RDWR | O_CLOEXEC);
    base_fd_.reset(open(path.c_str(), flags));
    CheckErr(base_fd_.get(), "open rootfs " + path);
    struct stat st {};
    CheckErr(fstat(base_fd_.get(), &st), "stat rootfs " + path);
    Check(st.st_size >= 0, "negative rootfs size: " + path);
    disk_bytes_ = static_cast<uint64_t>(st.st_size);
    sectors_ = disk_bytes_ / 512;
    if (overlay) {
      Check(overlay_path != path, "overlayPath must not equal rootfsPath");
      overlay_fd_.reset(open(overlay_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
      CheckErr(overlay_fd_.get(), "open overlay " + overlay_path);
      CheckErr(ftruncate(overlay_fd_.get(), st.st_size), "truncate overlay " + overlay_path);
      dirty_sectors_.assign(static_cast<size_t>((sectors_ + 7) / 8), 0);
    }
    queues_[0].size = kMaxQueueSize;
  }

  uint64_t mmio_base() const { return mmio_base_; }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    if (off < 0x100) {
      if (len != 4) {
        return;
      }
      uint32_t value = read_reg(off);
      WriteU32(data, value);
      return;
    }
    uint8_t cfg[16] {};
    WriteU64(cfg, sectors_);
    WriteU32(cfg + 8, 0);
    WriteU32(cfg + 12, 128);
    uint32_t cfg_off = off - 0x100;
    if (cfg_off < sizeof(cfg)) {
      memcpy(data, cfg + cfg_off, std::min<uint32_t>(len, sizeof(cfg) - cfg_off));
    }
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    if (off >= 0x100 || len != 4) {
      return;
    }
    write_reg(off, ReadU32(data));
  }

 private:
  struct Queue {
    uint32_t size{kMaxQueueSize};
    bool ready{false};
    uint16_t last_avail{0};
    uint64_t desc_addr{0};
    uint64_t driver_addr{0};
    uint64_t device_addr{0};
  };

  uint32_t read_reg(uint32_t off) {
    switch (off) {
      case 0x000:
        return 0x74726976;
      case 0x004:
        return 2;
      case 0x008:
        return 2;
      case 0x00C:
        return 0x554D4551;
      case 0x010: {
        uint64_t features = (1ULL << 32) | (1ULL << 9);
        if (read_only_) {
          features |= 1ULL << 5;
        }
        return dev_features_sel_ == 1 ? uint32_t(features >> 32) : uint32_t(features);
      }
      case 0x034:
        return kMaxQueueSize;
      case 0x044:
        return queues_[queue_sel_].ready ? 1 : 0;
      case 0x060:
        return interrupt_status_;
      case 0x070:
        return status_;
      case 0x0FC:
        return 0;
      default:
        return 0;
    }
  }

  void write_reg(uint32_t off, uint32_t value) {
    Queue& q = queues_[queue_sel_];
    switch (off) {
      case 0x014:
        dev_features_sel_ = value;
        break;
      case 0x024:
        drv_features_sel_ = value;
        break;
      case 0x020:
        if (drv_features_sel_ == 0) {
          drv_features_ = (drv_features_ & ~0xFFFFFFFFULL) | value;
        } else {
          drv_features_ = (drv_features_ & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        }
        break;
      case 0x030:
        if (value < 8) {
          queue_sel_ = value;
        }
        break;
      case 0x038:
        if (value >= 1 && value <= kMaxQueueSize) {
          q.size = value;
        }
        break;
      case 0x044:
        q.ready = value != 0;
        break;
      case 0x050:
        if (value < 8 && queues_[value].ready) {
          handle_queue(queues_[value]);
          interrupt_status_ |= 1;
          IrqLine(sys_, irq_, true);
        }
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) {
          IrqLine(sys_, irq_, false);
        }
        break;
      case 0x070:
        if (value == 0) {
          reset();
        } else {
          status_ = value;
        }
        break;
      case 0x080:
        q.desc_addr = (q.desc_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x084:
        q.desc_addr = (q.desc_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      case 0x090:
        q.driver_addr = (q.driver_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x094:
        q.driver_addr = (q.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      case 0x0A0:
        q.device_addr = (q.device_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x0A4:
        q.device_addr = (q.device_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      default:
        break;
    }
  }

  void reset() {
    status_ = 0;
    drv_features_ = 0;
    dev_features_sel_ = 0;
    drv_features_sel_ = 0;
    queue_sel_ = 0;
    interrupt_status_ = 0;
    for (auto& q : queues_) {
      q = Queue {};
    }
    IrqLine(sys_, irq_, false);
  }

  Desc read_desc(const Queue& q, uint16_t idx) {
    Check(idx < q.size, "virtio descriptor index out of bounds");
    uint8_t* p = mem_.ptr(q.desc_addr + uint64_t(idx) * 16, 16);
    return Desc {ReadU64(p), ReadU32(p + 8), ReadU16(p + 12), ReadU16(p + 14)};
  }

  DescChain walk_chain(const Queue& q, uint16_t head) {
    DescChain chain;
    std::array<uint8_t, kMaxQueueSize> seen {};
    uint16_t idx = head;
    for (;;) {
      Check(idx < q.size, "virtio descriptor next out of bounds");
      Check(seen[idx] == 0, "virtio descriptor cycle");
      seen[idx] = 1;
      Desc d = read_desc(q, idx);
      Check((d.flags & 4) == 0, "virtio indirect descriptors are not supported");
      chain.push(d);
      if ((d.flags & 1) == 0) {
        break;
      }
      idx = d.next;
    }
    return chain;
  }

  void push_used(Queue& q, uint32_t id, uint32_t written) {
    uint8_t* idxp = mem_.ptr(q.device_addr + 2, 2);
    uint16_t used = ReadU16(idxp);
    uint64_t entry = q.device_addr + 4 + uint64_t(used % q.size) * 8;
    WriteU32(mem_.ptr(entry, 8), id);
    WriteU32(mem_.ptr(entry + 4, 4), written);
    WriteU16(idxp, used + 1);
  }

  void handle_queue(Queue& q) {
    uint16_t avail_idx = ReadU16(mem_.ptr(q.driver_addr + 2, 2));
    while (q.last_avail != avail_idx) {
      uint16_t head = ReadU16(mem_.ptr(q.driver_addr + 4 + uint64_t(q.last_avail % q.size) * 2, 2));
      q.last_avail++;
      process_request(q, head);
    }
  }

	  void process_request(Queue& q, uint16_t head) {
    uint8_t status = 0;
    uint32_t written = 0;
    try {
	      DescChain chain = walk_chain(q, head);
	      Check(chain.size >= 2, "virtio-blk descriptor chain too short");
	      Desc header = chain[0];
	      Desc status_desc = chain[chain.size - 1];
	      Check(header.len >= 16, "virtio-blk header too short");
	      Check((status_desc.flags & 2) != 0 && status_desc.len >= 1, "virtio-blk status descriptor invalid");
      uint8_t hdr[16];
      memcpy(hdr, mem_.ptr(header.addr, 16), sizeof(hdr));
      uint32_t type = ReadU32(hdr);
      uint64_t sector = ReadU64(hdr + 8);

	      if (type == 0) {
	        for (size_t i = 1; i + 1 < chain.size; i++) {
	          Desc d = chain[i];
	          Check((d.flags & 2) != 0, "virtio-blk read data descriptor must be writable");
	          Check((d.len % 512) == 0, "virtio-blk read length must be sector-aligned");
		          ReadDisk(sector, mem_.ptr(d.addr, d.len), d.len);
		          written += d.len;
		          sector += d.len / 512;
	        }
	      } else if (type == 1) {
	        for (size_t i = 1; i + 1 < chain.size; i++) {
	          Desc d = chain[i];
	          Check((d.flags & 2) == 0, "virtio-blk write data descriptor must be read-only");
	          Check((d.len % 512) == 0, "virtio-blk write length must be sector-aligned");
		          WriteDisk(sector, mem_.ptr(d.addr, d.len), d.len);
	          sector += d.len / 512;
	        }
      } else if (type == 4) {
	        if (!read_only_) {
	          CheckErr(fsync(write_fd()), "virtio-blk flush");
	        }
	      } else if (type == 8) {
	        const char id[] = "node-vmm";
	        for (size_t i = 1; i + 1 < chain.size; i++) {
	          Desc d = chain[i];
	          uint32_t n = std::min<uint32_t>(d.len, sizeof(id));
	          memcpy(mem_.ptr(d.addr, n), id, n);
          written += n;
          break;
        }
      } else {
        status = 2;
      }
      memcpy(mem_.ptr(status_desc.addr, 1), &status, 1);
      push_used(q, head, written + 1);
    } catch (...) {
      status = 1;
	      try {
	        DescChain chain = walk_chain(q, head);
	        if (!chain.empty()) {
	          Desc status_desc = chain[chain.size - 1];
	          memcpy(mem_.ptr(status_desc.addr, 1), &status, 1);
	        }
      } catch (...) {
      }
	      push_used(q, head, written);
	    }
	  }

	  bool has_overlay() const { return overlay_fd_.get() >= 0; }

	  int write_fd() const {
	    return has_overlay() ? overlay_fd_.get() : base_fd_.get();
	  }

	  bool sector_dirty(uint64_t sector) const {
	    if (!has_overlay() || sector >= sectors_) {
	      return false;
	    }
	    return (dirty_sectors_[static_cast<size_t>(sector / 8)] & (uint8_t(1) << (sector % 8))) != 0;
	  }

	  void mark_dirty(uint64_t sector) {
	    if (!has_overlay() || sector >= sectors_) {
	      return;
	    }
	    dirty_sectors_[static_cast<size_t>(sector / 8)] |= uint8_t(1) << (sector % 8);
	  }

	  void mark_dirty_range(uint64_t byte_off, size_t len) {
	    if (!has_overlay() || len == 0) {
	      return;
	    }
	    uint64_t first = byte_off / 512;
	    uint64_t last = (byte_off + len - 1) / 512;
	    for (uint64_t sector = first; sector <= last; sector++) {
	      mark_dirty(sector);
	    }
	  }

	  void prepare_partial_overlay_sectors(uint64_t byte_off, size_t len) {
	    if (!has_overlay() || len == 0) {
	      return;
	    }
	    CheckRange(disk_bytes_, byte_off, len, "virtio-blk overlay prepare");
	    uint64_t end = CheckedAdd(byte_off, len, "virtio-blk overlay range");
	    uint64_t first = byte_off / 512;
	    uint64_t last = (end - 1) / 512;
	    std::array<uint8_t, 512> sector_buf {};
	    for (uint64_t sector = first; sector <= last; sector++) {
	      uint64_t sector_start = sector * 512;
	      bool covers_full_sector = byte_off <= sector_start && end >= sector_start + 512;
	      if (covers_full_sector || sector_dirty(sector)) {
	        continue;
	      }
	      PreadAll(base_fd_.get(), sector_buf.data(), sector_buf.size(), static_cast<off_t>(sector_start));
	      PwriteAll(overlay_fd_.get(), sector_buf.data(), sector_buf.size(), static_cast<off_t>(sector_start));
	    }
	  }

	  void ReadDisk(uint64_t sector, uint8_t* dst, size_t len) {
	    uint64_t byte_off = CheckedMul(sector, 512, "virtio-blk read offset");
	    CheckRange(disk_bytes_, byte_off, len, "virtio-blk read");
	    if (!has_overlay()) {
	      PreadAll(base_fd_.get(), dst, len, static_cast<off_t>(byte_off));
	      return;
	    }
	    size_t done = 0;
	    while (done < len) {
	      uint64_t current_byte = byte_off + done;
	      uint64_t current_sector = current_byte / 512;
	      size_t in_sector = static_cast<size_t>(current_byte % 512);
	      size_t chunk = std::min(len - done, size_t(512) - in_sector);
	      int fd = sector_dirty(current_sector) ? overlay_fd_.get() : base_fd_.get();
	      PreadAll(fd, dst + done, chunk, static_cast<off_t>(current_byte));
	      done += chunk;
	    }
	  }

	  void WriteDisk(uint64_t sector, const uint8_t* src, size_t len) {
	    Check(!read_only_, "virtio-blk write to read-only disk");
	    uint64_t byte_off = CheckedMul(sector, 512, "virtio-blk write offset");
	    CheckRange(disk_bytes_, byte_off, len, "virtio-blk write");
	    prepare_partial_overlay_sectors(byte_off, len);
	    PwriteAll(write_fd(), src, len, static_cast<off_t>(byte_off));
	    mark_dirty_range(byte_off, len);
	  }

	  KvmSystem& sys_;
	  GuestMemory mem_;
	  uint64_t mmio_base_{0};
	  uint32_t irq_{0};
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
  Queue queues_[8];
};

std::array<uint8_t, 6> ParseMac(const std::string& mac) {
  std::array<uint8_t, 6> out {};
  unsigned int parts[6] {};
  Check(sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) == 6,
        "invalid MAC address: " + mac);
  for (size_t i = 0; i < out.size(); i++) {
    Check(parts[i] <= 0xFF, "invalid MAC address: " + mac);
    out[i] = static_cast<uint8_t>(parts[i]);
  }
  return out;
}

Fd OpenTap(const std::string& tap_name) {
  Fd fd(open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC));
  CheckErr(fd.get(), "open /dev/net/tun");
  struct ifreq ifr {};
  strncpy(ifr.ifr_name, tap_name.c_str(), IFNAMSIZ - 1);
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  CheckErr(IoctlPtr(fd.get(), TUNSETIFF, &ifr), "TUNSETIFF " + tap_name);
  return fd;
}

struct SlirpHostFwdConfig {
  bool udp{false};
  uint32_t host_ip{0};
  uint16_t host_port{0};
  uint16_t guest_port{0};
};

class VirtioNet {
 public:
  VirtioNet(
      KvmSystem& sys,
      GuestMemory mem,
      uint64_t mmio_base,
      uint32_t irq,
      const std::string& tap_name,
      const std::string& mac,
      bool slirp_enabled,
      const std::string& slirp_host_ip,
      const std::string& slirp_guest_ip,
      const std::string& slirp_netmask,
      const std::string& slirp_dns,
      const std::vector<SlirpHostFwdConfig>& slirp_host_fwds)
      : sys_(sys),
        mem_(mem),
        mmio_base_(mmio_base),
        irq_(irq),
        tap_(slirp_enabled ? Fd{} : OpenTap(tap_name)),
        mac_(ParseMac(mac)) {
    queues_[0].size = kMaxQueueSize;
    queues_[1].size = kMaxQueueSize;
    if (slirp_enabled) {
      start_slirp(slirp_host_ip, slirp_guest_ip, slirp_netmask, slirp_dns, slirp_host_fwds);
    }
  }

  ~VirtioNet() { stop_slirp(); }

  uint64_t mmio_base() const { return mmio_base_; }

  void read_mmio(uint64_t addr, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    if (off < 0x100) {
      if (len != 4) {
        return;
      }
      WriteU32(data, read_reg(off));
      return;
    }
    uint8_t cfg[12] {};
    memcpy(cfg, mac_.data(), mac_.size());
    WriteU16(cfg + 6, 1);
    WriteU16(cfg + 8, 1);
    WriteU16(cfg + 10, 1500);
    uint32_t cfg_off = off - 0x100;
    if (cfg_off < sizeof(cfg)) {
      memcpy(data, cfg + cfg_off, std::min<uint32_t>(len, sizeof(cfg) - cfg_off));
    }
  }

  void write_mmio(uint64_t addr, const uint8_t* data, uint32_t len) {
    uint32_t off = static_cast<uint32_t>(addr - mmio_base_);
    if (off >= 0x100 || len != 4) {
      return;
    }
    write_reg(off, ReadU32(data));
  }

  void poll_rx() {
    if (slirp_enabled()) {
      poll_slirp();
      flush_slirp_rx();
      return;
    }
    if (!queues_[0].ready) {
      drain_tap(false);
      return;
    }
    for (int i = 0; i < 16; i++) {
      if (!has_rx_buffer(queues_[0])) {
        break;
      }
      uint8_t frame[2048];
      ssize_t n = read(tap_.get(), frame, sizeof(frame));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        throw std::runtime_error(ErrnoMessage("read tap"));
      }
      if (n == 0) {
        break;
      }
      (void)inject_rx_frame(frame, static_cast<size_t>(n));
    }
  }

	  bool enabled() const { return tap_.get() >= 0 || slirp_enabled(); }

	  bool tap_readable() const {
    if (slirp_enabled()) {
      return true;
    }
	    struct pollfd pfd {};
	    pfd.fd = tap_.get();
	    pfd.events = POLLIN;
	    int rc = poll(&pfd, 1, 0);
	    return rc > 0 && (pfd.revents & POLLIN);
	  }
	
	 private:
  struct Queue {
    uint32_t size{kMaxQueueSize};
    bool ready{false};
    uint16_t last_avail{0};
    uint64_t desc_addr{0};
    uint64_t driver_addr{0};
    uint64_t device_addr{0};
  };

  uint32_t read_reg(uint32_t off) {
    switch (off) {
      case 0x000:
        return 0x74726976;
      case 0x004:
        return 2;
      case 0x008:
        return 1;
      case 0x00C:
        return 0x554D4551;
      case 0x010: {
        uint64_t features = (1ULL << 32) | (1ULL << 5) | (1ULL << 16);
        return dev_features_sel_ == 1 ? uint32_t(features >> 32) : uint32_t(features);
      }
      case 0x034:
        return queue_sel_ < 2 ? kMaxQueueSize : 0;
      case 0x044:
        return queue_sel_ < 2 && queues_[queue_sel_].ready ? 1 : 0;
      case 0x060:
        return interrupt_status_;
      case 0x070:
        return status_;
      case 0x0FC:
        return 0;
      default:
        return 0;
    }
  }

  void write_reg(uint32_t off, uint32_t value) {
    Queue& q = queues_[queue_sel_ < 2 ? queue_sel_ : 0];
    switch (off) {
      case 0x014:
        dev_features_sel_ = value;
        break;
      case 0x024:
        drv_features_sel_ = value;
        break;
      case 0x020:
        if (drv_features_sel_ == 0) {
          drv_features_ = (drv_features_ & ~0xFFFFFFFFULL) | value;
        } else {
          drv_features_ = (drv_features_ & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        }
        break;
      case 0x030:
        if (value < 2) {
          queue_sel_ = value;
        }
        break;
      case 0x038:
        if (value >= 1 && value <= kMaxQueueSize) {
          q.size = value;
        }
        break;
      case 0x044:
        q.ready = value != 0;
        break;
      case 0x050:
        if (value == 0) {
          poll_rx();
        } else if (value == 1 && queues_[1].ready) {
          handle_tx_queue();
        }
        break;
      case 0x064:
        interrupt_status_ &= ~value;
        if (interrupt_status_ == 0) {
          IrqLine(sys_, irq_, false);
        }
        break;
      case 0x070:
        if (value == 0) {
          reset();
        } else {
          status_ = value;
        }
        break;
      case 0x080:
        q.desc_addr = (q.desc_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x084:
        q.desc_addr = (q.desc_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      case 0x090:
        q.driver_addr = (q.driver_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x094:
        q.driver_addr = (q.driver_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      case 0x0A0:
        q.device_addr = (q.device_addr & ~0xFFFFFFFFULL) | value;
        break;
      case 0x0A4:
        q.device_addr = (q.device_addr & 0xFFFFFFFFULL) | (uint64_t(value) << 32);
        break;
      default:
        break;
    }
  }

  void reset() {
    status_ = 0;
    drv_features_ = 0;
    dev_features_sel_ = 0;
    drv_features_sel_ = 0;
    queue_sel_ = 0;
    interrupt_status_ = 0;
    for (auto& q : queues_) {
      q = Queue {};
    }
    IrqLine(sys_, irq_, false);
  }

  Desc read_desc(const Queue& q, uint16_t idx) {
    Check(idx < q.size, "virtio-net descriptor index out of bounds");
    uint8_t* p = mem_.ptr(q.desc_addr + uint64_t(idx) * 16, 16);
    return Desc {ReadU64(p), ReadU32(p + 8), ReadU16(p + 12), ReadU16(p + 14)};
  }

  DescChain walk_chain(const Queue& q, uint16_t head) {
    DescChain chain;
    std::array<uint8_t, kMaxQueueSize> seen {};
    uint16_t idx = head;
    for (;;) {
      Check(idx < q.size, "virtio-net descriptor next out of bounds");
      Check(seen[idx] == 0, "virtio-net descriptor cycle");
      seen[idx] = 1;
      Desc d = read_desc(q, idx);
      Check((d.flags & 4) == 0, "virtio-net indirect descriptors are not supported");
      chain.push(d);
      if ((d.flags & 1) == 0) {
        break;
      }
      idx = d.next;
    }
    return chain;
  }

  void push_used(Queue& q, uint32_t id, uint32_t written) {
    uint8_t* idxp = mem_.ptr(q.device_addr + 2, 2);
    uint16_t used = ReadU16(idxp);
    uint64_t entry = q.device_addr + 4 + uint64_t(used % q.size) * 8;
    WriteU32(mem_.ptr(entry, 8), id);
    WriteU32(mem_.ptr(entry + 4, 4), written);
    WriteU16(idxp, used + 1);
  }

  void signal_queue() {
    interrupt_status_ |= 1;
    IrqLine(sys_, irq_, true);
  }

  bool has_rx_buffer(const Queue& q) const {
    return q.ready && q.last_avail != ReadU16(mem_.ptr(q.driver_addr + 2, 2));
  }

  bool inject_rx_frame(const uint8_t* frame, size_t len) {
    Queue& q = queues_[0];
    if (!has_rx_buffer(q)) {
      return false;
    }
    uint16_t head = ReadU16(mem_.ptr(q.driver_addr + 4 + uint64_t(q.last_avail % q.size) * 2, 2));
    q.last_avail++;
	    DescChain chain = walk_chain(q, head);
	    size_t needed = 12 + len;
	    size_t offset = 0;
	    uint8_t header[12] {};
	    for (size_t i = 0; i < chain.size; i++) {
	      const Desc& d = chain[i];
	      Check((d.flags & 2) != 0, "virtio-net rx descriptor must be writable");
	      uint8_t* dst = mem_.ptr(d.addr, d.len);
	      size_t desc_off = 0;
	      if (offset < sizeof(header) && desc_off < d.len) {
	        size_t n = std::min<size_t>(d.len - desc_off, sizeof(header) - offset);
	        memcpy(dst + desc_off, header + offset, n);
	        desc_off += n;
	        offset += n;
	      }
	      if (offset >= sizeof(header) && desc_off < d.len && offset < needed) {
	        size_t frame_off = offset - sizeof(header);
	        size_t n = std::min<size_t>(d.len - desc_off, len - frame_off);
	        memcpy(dst + desc_off, frame + frame_off, n);
	        offset += n;
	      }
	      if (offset >= needed) {
	        break;
      }
    }
    if (offset < needed) {
      return false;
    }
    push_used(q, head, static_cast<uint32_t>(needed));
    signal_queue();
    return true;
  }

  void handle_tx_queue() {
    Queue& q = queues_[1];
    uint16_t avail_idx = ReadU16(mem_.ptr(q.driver_addr + 2, 2));
    while (q.last_avail != avail_idx) {
      uint16_t head = ReadU16(mem_.ptr(q.driver_addr + 4 + uint64_t(q.last_avail % q.size) * 2, 2));
      q.last_avail++;
      process_tx(head);
    }
    poll_rx();
  }

	  void process_tx(uint16_t head) {
	    Queue& q = queues_[1];
	    DescChain chain = walk_chain(q, head);
	    std::array<struct iovec, kMaxQueueSize> iov {};
	    int iov_len = 0;
    std::vector<uint8_t> packet;
	    size_t skip = 12;
	    for (size_t i = 0; i < chain.size; i++) {
	      const Desc& d = chain[i];
	      Check((d.flags & 2) == 0, "virtio-net tx descriptor must be read-only");
	      size_t pos = 0;
	      if (skip >= d.len) {
        skip -= d.len;
        continue;
	      }
	      pos = skip;
	      skip = 0;
	      if (d.len > pos && iov_len < static_cast<int>(iov.size())) {
        uint8_t* data = mem_.ptr(d.addr + pos, d.len - pos);
        if (slirp_enabled()) {
          packet.insert(packet.end(), data, data + d.len - pos);
        } else {
	        iov[iov_len].iov_base = data;
	        iov[iov_len].iov_len = d.len - pos;
	        iov_len++;
        }
	      }
	    }
    if (slirp_enabled()) {
      input_slirp_packet(packet.data(), packet.size());
      poll_slirp();
      flush_slirp_rx();
    } else if (iov_len > 0) {
	      ssize_t n = writev(tap_.get(), iov.data(), iov_len);
	      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
	        throw std::runtime_error(ErrnoMessage("write tap"));
	      }
    }
    push_used(q, head, 0);
    signal_queue();
  }

  void drain_tap(bool throw_errors) {
    if (tap_.get() < 0) {
      return;
    }
    uint8_t buf[2048];
    for (;;) {
      ssize_t n = read(tap_.get(), buf, sizeof(buf));
      if (n > 0) {
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && throw_errors && errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::runtime_error(ErrnoMessage("read tap"));
      }
      break;
    }
  }

  bool slirp_enabled() const {
#ifdef NODE_VMM_HAVE_LIBSLIRP
    return slirp_ != nullptr;
#else
    return false;
#endif
  }

  void start_slirp(
      const std::string& host_ip,
      const std::string& guest_ip,
      const std::string& netmask,
      const std::string& dns,
      const std::vector<SlirpHostFwdConfig>& host_fwds) {
#ifdef NODE_VMM_HAVE_LIBSLIRP
    in_addr host_addr{};
    in_addr guest_addr{};
    in_addr mask_addr{};
    in_addr dns_addr{};
    const std::string effective_host = host_ip.empty() ? "10.0.2.2" : host_ip;
    const std::string effective_guest = guest_ip.empty() ? "10.0.2.15" : guest_ip;
    const std::string effective_mask = netmask.empty() ? "255.255.255.0" : netmask;
    const std::string effective_dns = dns.empty() ? "10.0.2.3" : dns;
    Check(inet_pton(AF_INET, effective_host.c_str(), &host_addr) == 1, "invalid slirp host IP: " + effective_host);
    Check(inet_pton(AF_INET, effective_guest.c_str(), &guest_addr) == 1, "invalid slirp guest IP: " + effective_guest);
    Check(inet_pton(AF_INET, effective_mask.c_str(), &mask_addr) == 1, "invalid slirp netmask: " + effective_mask);
    Check(inet_pton(AF_INET, effective_dns.c_str(), &dns_addr) == 1, "invalid slirp DNS IP: " + effective_dns);

    in_addr network_addr{};
    network_addr.s_addr = htonl(ntohl(host_addr.s_addr) & ntohl(mask_addr.s_addr));

    SlirpConfig cfg{};
    cfg.version = SLIRP_CONFIG_VERSION_MAX;
    cfg.in_enabled = true;
    cfg.vnetwork = network_addr;
    cfg.vnetmask = mask_addr;
    cfg.vhost = host_addr;
    cfg.vdhcp_start = guest_addr;
    cfg.vnameserver = dns_addr;
    cfg.if_mtu = 1500;
    cfg.if_mru = 1500;

    memset(&slirp_cb_, 0, sizeof(slirp_cb_));
    slirp_cb_.send_packet = &VirtioNet::slirp_send_packet_cb;
    slirp_cb_.guest_error = &VirtioNet::slirp_guest_error_cb;
    slirp_cb_.clock_get_ns = &VirtioNet::slirp_clock_get_ns_cb;
    slirp_cb_.notify = &VirtioNet::slirp_notify_cb;
    slirp_cb_.register_poll_fd = &VirtioNet::slirp_register_poll_fd_cb;
    slirp_cb_.unregister_poll_fd = &VirtioNet::slirp_unregister_poll_fd_cb;

    slirp_ = slirp_new(&cfg, &slirp_cb_, this);
    Check(slirp_ != nullptr, "slirp_new failed");

    for (const SlirpHostFwdConfig& fwd : host_fwds) {
      in_addr host_bind{};
      in_addr guest_bind = guest_addr;
      host_bind.s_addr = htonl(fwd.host_ip);
      int rc = slirp_add_hostfwd(slirp_, fwd.udp ? 1 : 0, host_bind, fwd.host_port, guest_bind, fwd.guest_port);
      Check(rc == 0, "slirp host forward failed: " + std::to_string(fwd.host_port) + " -> " +
                     effective_guest + ":" + std::to_string(fwd.guest_port));
    }
#else
    (void)host_ip;
    (void)guest_ip;
    (void)netmask;
    (void)dns;
    (void)host_fwds;
    throw std::runtime_error("Linux/KVM slirp networking is not available in this build; install libslirp-dev and rebuild");
#endif
  }

  void stop_slirp() {
#ifdef NODE_VMM_HAVE_LIBSLIRP
    if (slirp_ != nullptr) {
      slirp_cleanup(slirp_);
      slirp_ = nullptr;
    }
#endif
  }

  void input_slirp_packet(const uint8_t* data, size_t len) {
#ifdef NODE_VMM_HAVE_LIBSLIRP
    if (slirp_ != nullptr && data != nullptr && len > 0) {
      slirp_input(slirp_, data, static_cast<int>(len));
    }
#else
    (void)data;
    (void)len;
#endif
  }

#ifdef NODE_VMM_HAVE_LIBSLIRP
  struct SlirpPollSet {
    std::vector<pollfd> fds;
  };
#endif

  void poll_slirp() {
#ifdef NODE_VMM_HAVE_LIBSLIRP
    if (slirp_ == nullptr) {
      return;
    }
    uint32_t timeout_ms = 0;
    SlirpPollSet poll_set;
    slirp_pollfds_fill(slirp_, &timeout_ms, &VirtioNet::slirp_add_poll_cb, &poll_set);
    int rc = 0;
    if (!poll_set.fds.empty()) {
      rc = poll(poll_set.fds.data(), static_cast<nfds_t>(poll_set.fds.size()), 0);
      if (rc < 0 && errno == EINTR) {
        rc = 0;
      }
    }
    slirp_pollfds_poll(slirp_, rc < 0 ? 1 : 0, &VirtioNet::slirp_get_revents_cb, &poll_set);
#endif
  }

  void enqueue_slirp_rx(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
      return;
    }
    slirp_rx_frames_.emplace_back(data, data + len);
  }

  void flush_slirp_rx() {
    while (!slirp_rx_frames_.empty()) {
      const std::vector<uint8_t>& frame = slirp_rx_frames_.front();
      if (!inject_rx_frame(frame.data(), frame.size())) {
        return;
      }
      slirp_rx_frames_.pop_front();
    }
  }

#ifdef NODE_VMM_HAVE_LIBSLIRP
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

  static int slirp_add_poll_cb(int fd, int events, void* opaque) {
    auto* set = static_cast<SlirpPollSet*>(opaque);
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = poll_events_from_slirp(events);
    set->fds.push_back(pfd);
    return static_cast<int>(set->fds.size() - 1);
  }

  static int slirp_get_revents_cb(int index, void* opaque) {
    auto* set = static_cast<SlirpPollSet*>(opaque);
    if (index < 0 || static_cast<size_t>(index) >= set->fds.size()) {
      return 0;
    }
    return slirp_events_from_poll(set->fds[static_cast<size_t>(index)].revents);
  }

  static ssize_t slirp_send_packet_cb(const void* buf, size_t len, void* opaque) {
    auto* self = static_cast<VirtioNet*>(opaque);
    if (buf == nullptr || len == 0) {
      return 0;
    }
    if (len < 60) {
      uint8_t padded[64]{};
      memcpy(padded, buf, len);
      self->enqueue_slirp_rx(padded, 60);
      return static_cast<ssize_t>(len);
    }
    self->enqueue_slirp_rx(static_cast<const uint8_t*>(buf), len);
    return static_cast<ssize_t>(len);
  }

  static void slirp_guest_error_cb(const char* msg, void* /*opaque*/) {
    if (msg != nullptr) {
      fprintf(stderr, "[node-vmm kvm] slirp guest error: %s\n", msg);
    }
  }

  static int64_t slirp_clock_get_ns_cb(void* /*opaque*/) {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + int64_t(ts.tv_nsec);
  }

  static void slirp_notify_cb(void* /*opaque*/) {}

  static void slirp_register_poll_fd_cb(int /*fd*/, void* opaque) {
    slirp_notify_cb(opaque);
  }

  static void slirp_unregister_poll_fd_cb(int /*fd*/, void* opaque) {
    slirp_notify_cb(opaque);
  }
#endif

  KvmSystem& sys_;
  GuestMemory mem_;
  uint64_t mmio_base_{0};
  uint32_t irq_{0};
  Fd tap_;
  std::array<uint8_t, 6> mac_;
  uint32_t status_{0};
  uint64_t drv_features_{0};
  uint32_t dev_features_sel_{0};
  uint32_t drv_features_sel_{0};
  uint32_t queue_sel_{0};
  uint32_t interrupt_status_{0};
  Queue queues_[2];
  std::deque<std::vector<uint8_t>> slirp_rx_frames_;
#ifdef NODE_VMM_HAVE_LIBSLIRP
  ::Slirp* slirp_{nullptr};
  SlirpCb slirp_cb_{};
#endif
};

void HandleIo(struct kvm_run* run, Uart& uart, GuestExit* guest_exit = nullptr) {
  uint8_t* data = reinterpret_cast<uint8_t*>(run) + run->io.data_offset;
  for (uint32_t i = 0; i < run->io.count; i++) {
    uint16_t port = run->io.port;
    uint8_t* item = data + i * run->io.size;
    if (port == kNodeVmmExitPort && run->io.size == 1) {
      if (run->io.direction == KVM_EXIT_IO_OUT && guest_exit != nullptr) {
        guest_exit->requested = true;
        guest_exit->status = item[0];
      } else if (run->io.direction == KVM_EXIT_IO_IN) {
        item[0] = 0;
      }
      continue;
    }
    if (port == kNodeVmmConsolePort && run->io.size == 1) {
      if (run->io.direction == KVM_EXIT_IO_OUT) {
        uart.emit_bytes(item, 1);
      } else {
        item[0] = 0;
      }
      continue;
    }
    if (port >= kCom1Base && port < kCom1Base + 8 && run->io.size == 1) {
      uint16_t off = port - kCom1Base;
      if (run->io.direction == KVM_EXIT_IO_OUT) {
        uart.write(off, item[0]);
      } else {
        item[0] = uart.read(off);
      }
      continue;
    }
    if (run->io.direction == KVM_EXIT_IO_IN) {
      memset(item, 0, run->io.size);
    }
  }
}

bool HandleMmio(struct kvm_run* run, const std::vector<std::unique_ptr<VirtioBlk>>& blks, VirtioNet* net) {
  uint64_t addr = run->mmio.phys_addr;
  for (const auto& blk : blks) {
    if (addr >= blk->mmio_base() && addr < blk->mmio_base() + kVirtioStride) {
      if (run->mmio.is_write) {
        blk->write_mmio(addr, run->mmio.data, run->mmio.len);
      } else {
        blk->read_mmio(addr, run->mmio.data, run->mmio.len);
      }
      return true;
    }
  }
  if (net != nullptr && addr >= net->mmio_base() && addr < net->mmio_base() + kVirtioStride) {
    if (run->mmio.is_write) {
      net->write_mmio(addr, run->mmio.data, run->mmio.len);
    } else {
      net->read_mmio(addr, run->mmio.data, run->mmio.len);
    }
    return true;
  }
  if (!run->mmio.is_write) {
    memset(run->mmio.data, 0, run->mmio.len);
  }
  return false;
}

std::string ExitReasonName(uint32_t reason) {
  switch (reason) {
    case KVM_EXIT_UNKNOWN:
      return "unknown";
    case KVM_EXIT_EXCEPTION:
      return "exception";
    case KVM_EXIT_IO:
      return "io";
    case KVM_EXIT_HLT:
      return "hlt";
    case KVM_EXIT_MMIO:
      return "mmio";
    case KVM_EXIT_IRQ_WINDOW_OPEN:
      return "irq-window-open";
    case KVM_EXIT_SHUTDOWN:
      return "shutdown";
    case KVM_EXIT_FAIL_ENTRY:
      return "fail-entry";
    case KVM_EXIT_INTERNAL_ERROR:
      return "internal-error";
    case KVM_EXIT_SYSTEM_EVENT:
      return "system-event";
    default:
      return "exit-" + std::to_string(reason);
  }
}

napi_value MakeString(napi_env env, const std::string& value) {
  napi_value out;
  napi_create_string_utf8(env, value.c_str(), value.size(), &out);
  return out;
}

napi_value MakeUint32(napi_env env, uint32_t value) {
  napi_value out;
  napi_create_uint32(env, value, &out);
  return out;
}

napi_value MakeDouble(napi_env env, double value) {
  napi_value out;
  napi_create_double(env, value, &out);
  return out;
}

napi_value MakeBool(napi_env env, bool value) {
  napi_value out;
  napi_get_boolean(env, value, &out);
  return out;
}

napi_value MakeObject(napi_env env) {
  napi_value out;
  napi_create_object(env, &out);
  return out;
}

void Set(napi_env env, napi_value obj, const char* name, napi_value value) {
  napi_set_named_property(env, obj, name, value);
}

void SetString(napi_env env, napi_value obj, const char* name, const std::string& value) {
  Set(env, obj, name, MakeString(env, value));
}

void SetUint32(napi_env env, napi_value obj, const char* name, uint32_t value) {
  Set(env, obj, name, MakeUint32(env, value));
}

void SetDouble(napi_env env, napi_value obj, const char* name, double value) {
  Set(env, obj, name, MakeDouble(env, value));
}

void SetBool(napi_env env, napi_value obj, const char* name, bool value) {
  Set(env, obj, name, MakeBool(env, value));
}

napi_value Throw(napi_env env, const std::exception& err) {
  napi_throw_error(env, nullptr, err.what());
  return nullptr;
}

napi_value GetNamed(napi_env env, napi_value obj, const char* name) {
  napi_value value;
  napi_status status = napi_get_named_property(env, obj, name, &value);
  if (status != napi_ok) {
    throw std::runtime_error(std::string("missing property: ") + name);
  }
  return value;
}

bool HasNamed(napi_env env, napi_value obj, const char* name) {
  bool has = false;
  napi_has_named_property(env, obj, name, &has);
  return has;
}

bool IsNullish(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  return type == napi_undefined || type == napi_null;
}

std::string GetString(napi_env env, napi_value obj, const char* name, const std::string& fallback = "") {
  if (!HasNamed(env, obj, name)) {
    return fallback;
  }
  napi_value value = GetNamed(env, obj, name);
  if (IsNullish(env, value)) {
    return fallback;
  }
  size_t len = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &len);
  std::vector<char> buf(len + 1);
  napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &len);
  return std::string(buf.data(), len);
}

uint32_t GetUint32(napi_env env, napi_value obj, const char* name, uint32_t fallback = 0) {
  if (!HasNamed(env, obj, name)) {
    return fallback;
  }
  napi_value value = GetNamed(env, obj, name);
  if (IsNullish(env, value)) {
    return fallback;
  }
  uint32_t out = 0;
  napi_get_value_uint32(env, value, &out);
  return out;
}

bool GetBool(napi_env env, napi_value obj, const char* name, bool fallback = false) {
  if (!HasNamed(env, obj, name)) {
    return fallback;
  }
  napi_value value = GetNamed(env, obj, name);
  if (IsNullish(env, value)) {
    return fallback;
  }
  bool out = fallback;
  napi_get_value_bool(env, value, &out);
  return out;
}

struct AttachedDiskConfig {
  std::string path;
  bool read_only{false};
};

bool HasNonNullishNamed(napi_env env, napi_value obj, const char* name) {
  if (!HasNamed(env, obj, name)) {
    return false;
  }
  return !IsNullish(env, GetNamed(env, obj, name));
}

std::vector<AttachedDiskConfig> GetAttachedDisks(napi_env env, napi_value obj) {
  bool has_disks = HasNonNullishNamed(env, obj, "disks");
  bool has_attached_disks = HasNonNullishNamed(env, obj, "attachedDisks");
  if (!has_disks && !has_attached_disks) {
    return {};
  }
  Check(!(has_disks && has_attached_disks), "use either disks or attachedDisks, not both");

  const char* name = has_disks ? "disks" : "attachedDisks";
  napi_value value = GetNamed(env, obj, name);
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

uint32_t ParseIpv4HostOrder(const std::string& input, const std::string& label) {
  in_addr addr {};
  Check(inet_pton(AF_INET, input.c_str(), &addr) == 1, "invalid IPv4 address for " + label + ": " + input);
  return ntohl(addr.s_addr);
}

std::vector<SlirpHostFwdConfig> GetSlirpHostFwds(napi_env env, napi_value obj) {
  if (!HasNonNullishNamed(env, obj, "netSlirpHostFwds")) {
    return {};
  }
  napi_value value = GetNamed(env, obj, "netSlirpHostFwds");
  bool is_array = false;
  napi_is_array(env, value, &is_array);
  Check(is_array, "netSlirpHostFwds must be an array");

  uint32_t length = 0;
  napi_get_array_length(env, value, &length);
  std::vector<SlirpHostFwdConfig> out;
  out.reserve(length);
  for (uint32_t i = 0; i < length; i++) {
    napi_value entry;
    napi_get_element(env, value, i, &entry);
    napi_valuetype type = napi_undefined;
    napi_typeof(env, entry, &type);
    Check(type == napi_object, "netSlirpHostFwds entries must be objects");
    SlirpHostFwdConfig fwd;
    fwd.udp = GetBool(env, entry, "udp", false);
    fwd.host_ip = ParseIpv4HostOrder(GetString(env, entry, "hostAddr", "127.0.0.1"), "netSlirpHostFwds.hostAddr");
    fwd.host_port = static_cast<uint16_t>(GetUint32(env, entry, "hostPort", 0));
    fwd.guest_port = static_cast<uint16_t>(GetUint32(env, entry, "guestPort", 0));
    Check(fwd.host_port > 0, "netSlirpHostFwds entries require hostPort");
    Check(fwd.guest_port > 0, "netSlirpHostFwds entries require guestPort");
    out.push_back(fwd);
  }
  return out;
}

struct RunControl {
  int32_t* words{nullptr};
  size_t length{0};

  bool enabled() const { return words != nullptr && length >= 2; }

  int32_t command() const {
    if (!enabled()) {
      return kControlRun;
    }
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

RunControl GetRunControl(napi_env env, napi_value obj) {
  if (!HasNamed(env, obj, "control")) {
    return {};
  }
  napi_value value = GetNamed(env, obj, "control");
  if (IsNullish(env, value)) {
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

class TerminalRawMode {
 public:
  explicit TerminalRawMode(bool enable) {
    if (!enable || !isatty(STDIN_FILENO)) {
      return;
    }
    if (tcgetattr(STDIN_FILENO, &old_) != 0) {
      return;
    }
    struct termios raw = old_;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag |= OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      active_ = true;
    }
  }

  TerminalRawMode(const TerminalRawMode&) = delete;
  TerminalRawMode& operator=(const TerminalRawMode&) = delete;

  ~TerminalRawMode() {
    if (active_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &old_);
    }
  }

 private:
  bool active_{false};
  struct termios old_ {};
};

napi_value ProbeKvm(napi_env env, napi_callback_info info) {
  (void)info;
  try {
    Fd kvm(open("/dev/kvm", O_RDWR | O_CLOEXEC));
    CheckErr(kvm.get(), "open /dev/kvm");
    int version = Ioctl(kvm.get(), KVM_GET_API_VERSION);
    CheckErr(version, "KVM_GET_API_VERSION");
    int mmap_size = Ioctl(kvm.get(), KVM_GET_VCPU_MMAP_SIZE);
    CheckErr(mmap_size, "KVM_GET_VCPU_MMAP_SIZE");
    napi_value out = MakeObject(env);
    SetUint32(env, out, "apiVersion", static_cast<uint32_t>(version));
    SetUint32(env, out, "mmapSize", static_cast<uint32_t>(mmap_size));
    SetString(env, out, "arch", "x86_64");
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value SmokeHlt(napi_env env, napi_callback_info info) {
  (void)info;
  try {
    KvmSystem sys = CreateVm(1, false);
    GuestMemory mem = sys.guest();
    mem.ptr(0, 1)[0] = 0xF4;
    Vcpu vcpu = CreateVcpu(sys, 0);
    SetupRealMode(vcpu, 0);
    CheckErr(Ioctl(vcpu.fd.get(), KVM_RUN), "KVM_RUN");
    napi_value out = MakeObject(env);
    SetString(env, out, "exitReason", ExitReasonName(vcpu.run->exit_reason));
    SetUint32(env, out, "exitReasonCode", vcpu.run->exit_reason);
    SetUint32(env, out, "runs", 1);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value UartSmoke(napi_env env, napi_callback_info info) {
  (void)info;
  try {
    KvmSystem sys = CreateVm(1, false);
    GuestMemory mem = sys.guest();
    const uint8_t code[] = {0xBA, 0xF8, 0x03, 0xB0, 0x41, 0xEE, 0xF4};
    memcpy(mem.ptr(0, sizeof(code)), code, sizeof(code));
    Vcpu vcpu = CreateVcpu(sys, 0);
    SetupRealMode(vcpu, 0);
    Uart uart(1024, false);
    uint32_t runs = 0;
    for (;;) {
      CheckErr(Ioctl(vcpu.fd.get(), KVM_RUN), "KVM_RUN");
      runs++;
      if (vcpu.run->exit_reason == KVM_EXIT_IO) {
        HandleIo(vcpu.run, uart);
        continue;
      }
      break;
    }
    napi_value out = MakeObject(env);
    SetString(env, out, "exitReason", ExitReasonName(vcpu.run->exit_reason));
    SetUint32(env, out, "exitReasonCode", vcpu.run->exit_reason);
    SetUint32(env, out, "runs", runs);
    SetString(env, out, "output", uart.console());
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value GuestExitSmoke(napi_env env, napi_callback_info info) {
  (void)info;
  try {
    KvmSystem sys = CreateVm(1, false);
    GuestMemory mem = sys.guest();
    const uint8_t code[] = {0xBA, 0x01, 0x05, 0xB0, 0x2A, 0xEE, 0xF4};
    memcpy(mem.ptr(0, sizeof(code)), code, sizeof(code));
    Vcpu vcpu = CreateVcpu(sys, 0);
    SetupRealMode(vcpu, 0);
    Uart uart(1024, false);
    GuestExit guest_exit;
    uint32_t runs = 0;
    for (;;) {
      CheckErr(Ioctl(vcpu.fd.get(), KVM_RUN), "KVM_RUN");
      runs++;
      if (vcpu.run->exit_reason == KVM_EXIT_IO) {
        HandleIo(vcpu.run, uart, &guest_exit);
        if (guest_exit.requested) {
          break;
        }
        continue;
      }
      break;
    }
    napi_value out = MakeObject(env);
    SetString(env, out, "exitReason", guest_exit.requested ? "guest-exit" : ExitReasonName(vcpu.run->exit_reason));
    SetUint32(env, out, "exitReasonCode", vcpu.run->exit_reason);
    SetUint32(env, out, "runs", runs);
    SetUint32(env, out, "status", guest_exit.status);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value RamSnapshotSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
      throw std::runtime_error("ramSnapshotSmoke requires a config object");
    }
    std::string snapshot_dir = GetString(env, argv[0], "snapshotDir");
    uint32_t mem_mib = GetUint32(env, argv[0], "memMiB", 1);
    Check(!snapshot_dir.empty(), "snapshotDir is required");
    Check(mem_mib > 0, "memMiB must be greater than zero");

    const uint8_t code[] = {
        0xBA, 0xF8, 0x03, 0xB0, 0x41, 0xEE,  // out COM1, 'A'
        0xBA, 0xF8, 0x03, 0xB0, 0x42, 0xEE,  // out COM1, 'B'
        0xBA, 0x01, 0x05, 0xB0, 0x2A, 0xEE,  // out node-vmm exit port, 42
        0xF4                                      // hlt fallback
    };

    uint64_t total_start = NowMicros();
    std::string ram_path;
    std::string state_path;
    std::string before_output;
    std::string after_output;
    uint32_t runs_before_snapshot = 0;
    uint32_t runs_after_restore = 0;
    uint32_t status = 0;
    uint64_t snapshot_write_us = 0;
    std::string exit_reason;

    {
      KvmSystem sys = CreateVm(mem_mib, false);
      GuestMemory mem = sys.guest();
      Check(sizeof(code) <= mem.size(), "snapshot smoke code exceeds guest memory");
      memcpy(mem.ptr(0, sizeof(code)), code, sizeof(code));
      Vcpu vcpu = CreateVcpu(sys, 0);
      SetupRealMode(vcpu, 0);
      Uart uart(1024, false);

      for (;;) {
        CheckErr(Ioctl(vcpu.fd.get(), KVM_RUN), "KVM_RUN snapshot source");
        runs_before_snapshot++;
        if (vcpu.run->exit_reason == KVM_EXIT_IO) {
          HandleIo(vcpu.run, uart);
          CompletePendingIoBeforeSnapshot(vcpu);
          break;
        }
        if (vcpu.run->exit_reason == KVM_EXIT_HLT) {
          throw std::runtime_error("snapshot smoke halted before snapshot point");
        }
        throw std::runtime_error("snapshot smoke unexpected source exit: " + ExitReasonName(vcpu.run->exit_reason));
      }

      before_output = uart.console();
      VcpuCoreState state = CaptureVcpuCoreState(vcpu);
      uint64_t write_start = NowMicros();
      WriteRamSnapshotFiles(snapshot_dir, mem_mib, sys, state, &ram_path, &state_path);
      uint64_t write_end = NowMicros();
      snapshot_write_us = write_end - write_start;
    }

    uint32_t restored_mem_mib = 0;
    uint64_t restored_mem_size = 0;
    uint64_t restore_start = NowMicros();
    VcpuCoreState restored_state = ReadRamSnapshotState(state_path, &restored_mem_mib, &restored_mem_size);
    Check(restored_mem_mib == mem_mib, "restored memMiB mismatch");
    KvmSystem restored_sys = CreateVmWithMemory(restored_mem_mib, false, ram_path, true);
    Check(restored_sys.mem_size == restored_mem_size, "restored RAM size mismatch");
    Vcpu restored_vcpu = CreateVcpu(restored_sys, 0);
    RestoreVcpuCoreState(restored_vcpu, restored_state);
    uint64_t restore_ready = NowMicros();

    Uart restored_uart(1024, false);
    GuestExit guest_exit;
    for (;;) {
      CheckErr(Ioctl(restored_vcpu.fd.get(), KVM_RUN), "KVM_RUN snapshot restore");
      runs_after_restore++;
      if (restored_vcpu.run->exit_reason == KVM_EXIT_IO) {
        HandleIo(restored_vcpu.run, restored_uart, &guest_exit);
        if (guest_exit.requested) {
          status = guest_exit.status;
          exit_reason = "guest-exit";
          break;
        }
        continue;
      }
      exit_reason = ExitReasonName(restored_vcpu.run->exit_reason);
      break;
    }
    after_output = restored_uart.console();
    uint64_t total_end = NowMicros();

    struct stat ram_stat {};
    CheckErr(stat(ram_path.c_str(), &ram_stat), "stat snapshot RAM " + ram_path);
    napi_value out = MakeObject(env);
    SetString(env, out, "exitReason", exit_reason);
    SetUint32(env, out, "exitReasonCode", restored_vcpu.run->exit_reason);
    SetUint32(env, out, "status", status);
    SetUint32(env, out, "runsBeforeSnapshot", runs_before_snapshot);
    SetUint32(env, out, "runsAfterRestore", runs_after_restore);
    SetString(env, out, "output", before_output + after_output);
    SetString(env, out, "snapshotDir", snapshot_dir);
    SetString(env, out, "ramPath", ram_path);
    SetString(env, out, "statePath", state_path);
    SetDouble(env, out, "ramBytes", static_cast<double>(ram_stat.st_size));
    SetDouble(env, out, "ramAllocatedBytes", static_cast<double>(ram_stat.st_blocks) * 512.0);
    SetDouble(env, out, "snapshotWriteMs", static_cast<double>(snapshot_write_us) / 1000.0);
    SetDouble(env, out, "restoreSetupMs", static_cast<double>(restore_ready - restore_start) / 1000.0);
    SetDouble(env, out, "totalMs", static_cast<double>(total_end - total_start) / 1000.0);
    SetBool(env, out, "privateRamMapping", true);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value DirtyRamSnapshotSmoke(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
      throw std::runtime_error("dirtyRamSnapshotSmoke requires a config object");
    }
    std::string snapshot_dir = GetString(env, argv[0], "snapshotDir");
    uint32_t mem_mib = GetUint32(env, argv[0], "memMiB", 256);
    uint32_t dirty_pages_target = GetUint32(env, argv[0], "dirtyPages", 8);
    Check(!snapshot_dir.empty(), "snapshotDir is required");
    Check(mem_mib > 0, "memMiB must be greater than zero");

    uint64_t page_count = (uint64_t(mem_mib) * 1024ULL * 1024ULL) / kSnapshotPageSize;
    Check(dirty_pages_target > 0 && dirty_pages_target <= 8 && dirty_pages_target < page_count, "dirtyPages must be between 1 and 8");

    std::string base_ram = JoinPath(snapshot_dir, "base.mem");
    std::string delta_ram = JoinPath(snapshot_dir, "dirty.mem");
    std::string state_path = JoinPath(snapshot_dir, "dirty-vcpu.bin");
    EnsureDirectory(snapshot_dir);

    uint64_t total_start = NowMicros();
    uint64_t base_write_us = 0;
    uint64_t dirty_write_us = 0;
    uint64_t restore_setup_us = 0;
    uint64_t dirty_pages_count = 0;
    std::string before_output;
    std::string after_output;
    uint32_t runs_before_snapshot = 0;
    uint32_t runs_after_restore = 0;
    uint32_t status = 0;
    std::string exit_reason;

    {
      KvmSystem sys = CreateVmWithMemory(mem_mib, false, "", false, true);
      GuestMemory mem = sys.guest();
      mem.ptr(0, 1)[0] = 0xF4;
      Vcpu vcpu = CreateVcpu(sys, 0);
      SetupRealMode(vcpu, 0);

      uint64_t base_start = NowMicros();
      WriteFullRamFile(base_ram, sys);
      base_write_us = NowMicros() - base_start;

      const uint64_t code_addr = 0x9000;
      std::vector<uint8_t> code;
      for (uint32_t i = 0; i < dirty_pages_target; i++) {
        uint16_t addr = static_cast<uint16_t>((1 + i) * kSnapshotPageSize);
        code.push_back(0xC6);  // mov byte ptr [addr], imm8
        code.push_back(0x06);
        code.push_back(static_cast<uint8_t>(addr));
        code.push_back(static_cast<uint8_t>(addr >> 8));
        code.push_back(static_cast<uint8_t>(0xA0 + (i & 0x1F)));
      }
      const uint8_t tail[] = {
          0xBA, 0xF8, 0x03, 0xB0, 0x43, 0xEE,  // out COM1, 'C'
          0xBA, 0xF8, 0x03, 0xB0, 0x44, 0xEE,  // out COM1, 'D'
          0xBA, 0x01, 0x05, 0xB0, 0x2A, 0xEE,  // out node-vmm exit port, 42
          0xF4
      };
      code.insert(code.end(), tail, tail + sizeof(tail));
      Check(code_addr + code.size() < mem.size(), "dirty snapshot code exceeds guest memory");
      memcpy(mem.ptr(code_addr, code.size()), code.data(), code.size());
      SetupRealMode(vcpu, code_addr);

      Uart uart(1024, false);
      for (;;) {
        CheckErr(Ioctl(vcpu.fd.get(), KVM_RUN), "KVM_RUN dirty snapshot source");
        runs_before_snapshot++;
        if (vcpu.run->exit_reason == KVM_EXIT_IO) {
          HandleIo(vcpu.run, uart);
          CompletePendingIoBeforeSnapshot(vcpu);
          break;
        }
        if (vcpu.run->exit_reason == KVM_EXIT_HLT) {
          throw std::runtime_error("dirty snapshot smoke halted before snapshot point");
        }
        throw std::runtime_error("dirty snapshot smoke unexpected source exit: " + ExitReasonName(vcpu.run->exit_reason));
      }
      before_output = uart.console();
      VcpuCoreState state = CaptureVcpuCoreState(vcpu);
      std::vector<uint64_t> dirty_pages = GetDirtyPages(sys);
      dirty_pages_count = dirty_pages.size();

      uint64_t dirty_start = NowMicros();
      WriteVcpuStateFile(state_path, state);
      WriteDirtySnapshotFile(delta_ram, sys, dirty_pages);
      dirty_write_us = NowMicros() - dirty_start;
    }

    uint64_t restore_start = NowMicros();
    KvmSystem restored_sys = CreateVmWithMemory(mem_mib, false, base_ram, true);
    uint64_t restored_dirty_pages = ApplyDirtySnapshotFile(delta_ram, restored_sys);
    Vcpu restored_vcpu = CreateVcpu(restored_sys, 0);
    VcpuCoreState restored_state = ReadVcpuStateFile(state_path);
    RestoreVcpuCoreState(restored_vcpu, restored_state);
    restore_setup_us = NowMicros() - restore_start;

    Uart uart(1024, false);
    GuestExit guest_exit;
    for (;;) {
      CheckErr(Ioctl(restored_vcpu.fd.get(), KVM_RUN), "KVM_RUN dirty snapshot restore");
      runs_after_restore++;
      if (restored_vcpu.run->exit_reason == KVM_EXIT_IO) {
        HandleIo(restored_vcpu.run, uart, &guest_exit);
        if (guest_exit.requested) {
          status = guest_exit.status;
          exit_reason = "guest-exit";
          break;
        }
        continue;
      }
      exit_reason = ExitReasonName(restored_vcpu.run->exit_reason);
      break;
    }
    after_output = uart.console();
    uint64_t total_end = NowMicros();

    struct stat base_stat {};
    struct stat delta_stat {};
    CheckErr(stat(base_ram.c_str(), &base_stat), "stat base RAM " + base_ram);
    CheckErr(stat(delta_ram.c_str(), &delta_stat), "stat dirty RAM " + delta_ram);
    napi_value out = MakeObject(env);
    SetString(env, out, "exitReason", exit_reason);
    SetUint32(env, out, "exitReasonCode", restored_vcpu.run->exit_reason);
    SetUint32(env, out, "status", status);
    SetUint32(env, out, "runsBeforeSnapshot", runs_before_snapshot);
    SetUint32(env, out, "runsAfterRestore", runs_after_restore);
    SetString(env, out, "output", before_output + after_output);
    SetString(env, out, "snapshotDir", snapshot_dir);
    SetString(env, out, "baseRamPath", base_ram);
    SetString(env, out, "deltaRamPath", delta_ram);
    SetString(env, out, "statePath", state_path);
    SetDouble(env, out, "baseRamBytes", static_cast<double>(base_stat.st_size));
    SetDouble(env, out, "baseRamAllocatedBytes", static_cast<double>(base_stat.st_blocks) * 512.0);
    SetDouble(env, out, "deltaRamBytes", static_cast<double>(delta_stat.st_size));
    SetDouble(env, out, "deltaRamAllocatedBytes", static_cast<double>(delta_stat.st_blocks) * 512.0);
    SetDouble(env, out, "dirtyPages", static_cast<double>(dirty_pages_count));
    SetDouble(env, out, "restoredDirtyPages", static_cast<double>(restored_dirty_pages));
    SetDouble(env, out, "baseWriteMs", static_cast<double>(base_write_us) / 1000.0);
    SetDouble(env, out, "dirtyWriteMs", static_cast<double>(dirty_write_us) / 1000.0);
    SetDouble(env, out, "restoreSetupMs", static_cast<double>(restore_setup_us) / 1000.0);
    SetDouble(env, out, "totalMs", static_cast<double>(total_end - total_start) / 1000.0);
    SetBool(env, out, "privateRamMapping", true);
    return out;
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value RunVm(napi_env env, napi_callback_info info) {
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
    std::string guest_mac = GetString(env, argv[0], "netGuestMac");
    bool slirp_enabled = GetBool(env, argv[0], "netSlirpEnabled", false);
    std::string slirp_host_ip = GetString(env, argv[0], "netHostIp", "10.0.2.2");
    std::string slirp_guest_ip = GetString(env, argv[0], "netGuestIp", "10.0.2.15");
    std::string slirp_netmask = GetString(env, argv[0], "netNetmask", "255.255.255.0");
    std::string slirp_dns = GetString(env, argv[0], "netDns", "10.0.2.3");
    std::vector<SlirpHostFwdConfig> slirp_host_fwds = GetSlirpHostFwds(env, argv[0]);
    std::vector<AttachedDiskConfig> attached_disks = GetAttachedDisks(env, argv[0]);
    RunControl control = GetRunControl(env, argv[0]);
    control.set_state(kControlStateStarting);
    bool network_enabled = !tap_name.empty() || slirp_enabled;
    Check(attached_disks.size() < kMaxIoApicPins, "too many attached disks");
    uint32_t disk_count = static_cast<uint32_t>(attached_disks.size() + 1);
    CheckVirtioMmioDeviceCount(disk_count + (network_enabled ? 1 : 0));
    Check(!kernel_path.empty(), "kernelPath is required");
    Check(!rootfs_path.empty(), "rootfsPath is required");
    Check(!cmdline.empty(), "cmdline is required");
    Check(cpus >= 1 && cpus <= kMaxVcpus, "cpus must be between 1 and 64");
    Check(!(slirp_enabled && !tap_name.empty()), "netSlirpEnabled cannot be combined with netTapName");
    Check(!network_enabled || !guest_mac.empty(), "netGuestMac is required when networking is enabled");
    Check(cmdline.size() + 1 <= kKernelCmdlineMax, "kernel cmdline is too long");

    KvmSystem sys = CreateVm(mem_mib, true);
    GuestMemory mem = sys.guest();
    KernelInfo kernel = LoadElfKernel(mem, kernel_path);
    uint64_t rsdp_addr = CreateAcpiTables(mem, network_enabled, static_cast<int>(cpus), disk_count);
    WriteBootParams(mem, uint64_t(mem_mib) * 1024ULL * 1024ULL, cmdline);
    WriteU64(mem.ptr(kBootParamsAddr + 0x70, 8), rsdp_addr);
    WriteMpTable(mem, static_cast<int>(cpus));
    SetIrqRouting(sys);
    std::vector<std::unique_ptr<VirtioBlk>> blks;
    blks.reserve(disk_count);
    blks.push_back(std::make_unique<VirtioBlk>(
        sys, mem, VirtioMmioBase(0), VirtioMmioIrq(0), rootfs_path, overlay_path, false));
    for (uint32_t i = 0; i < attached_disks.size(); i++) {
      const AttachedDiskConfig& disk = attached_disks[i];
      uint32_t index = i + 1;
      blks.push_back(std::make_unique<VirtioBlk>(
          sys, mem, VirtioMmioBase(index), VirtioMmioIrq(index), disk.path, "", disk.read_only));
    }
    std::unique_ptr<VirtioNet> net;
    if (network_enabled) {
      uint32_t net_index = disk_count;
      net = std::make_unique<VirtioNet>(
          sys,
          mem,
          VirtioMmioBase(net_index),
          VirtioMmioIrq(net_index),
          tap_name,
          guest_mac,
          slirp_enabled,
          slirp_host_ip,
          slirp_guest_ip,
          slirp_netmask,
          slirp_dns,
          slirp_host_fwds);
    }

    std::vector<Vcpu> vcpus;
    vcpus.reserve(cpus);
    for (uint32_t i = 0; i < cpus; i++) {
      vcpus.push_back(CreateVcpu(sys, static_cast<int>(i)));
    }
    SetupBootstrapVcpu(sys, vcpus[0], mem, kernel.entry, cpus);
    for (uint32_t i = 1; i < cpus; i++) {
      SetupApplicationVcpu(sys, vcpus[i], i, cpus);
    }

    Uart uart(console_limit, interactive, &sys, [control]() { control.mark_console_output(); });
    TerminalRawMode raw_mode(interactive);
    std::atomic<bool> input_done{false};
    std::atomic<bool> host_interrupt_requested{false};
    std::thread input_thread;
    if (interactive) {
      input_thread = std::thread([&]() {
        uint8_t buf[256];
        while (!input_done.load()) {
          struct pollfd pfd {};
          pfd.fd = STDIN_FILENO;
          pfd.events = POLLIN;
          int prc = poll(&pfd, 1, 20);
          if (prc < 0) {
            if (errno == EINTR) {
              continue;
            }
            break;
          }
          if (prc == 0) {
            continue;
          }
          if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
          }
          if (pfd.revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0) {
              if (errno == EINTR || errno == EAGAIN) {
                continue;
              }
              break;
            }
            if (n == 0) {
              break;
            }
            bool host_interrupt = false;
            for (ssize_t i = 0; i < n; i++) {
              if (buf[i] == 0x03) {
                host_interrupt = true;
                break;
              }
            }
            if (host_interrupt) {
              host_interrupt_requested = true;
              break;
            }
            uart.enqueue_rx(buf, static_cast<size_t>(n));
          }
        }
      });
    }
    auto stop_input = [&]() {
      input_done = true;
      if (input_thread.joinable()) {
        input_thread.join();
      }
    };

    bool timeout_enabled = timeout_ms > 0;
    auto base_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    struct sigaction sa {};
    struct sigaction old_sa {};
    sa.sa_handler = [](int) {};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old_sa);
    std::mutex device_mu;
    std::mutex result_mu;
    std::mutex runner_mu;
    std::string final_exit_reason = "host-stop";
    std::string error_message;
    uint32_t final_reason = KVM_EXIT_INTR;
    std::vector<pthread_t> runner_threads(cpus);
    std::vector<bool> runner_ready(cpus, false);
    std::atomic<bool> vm_done{false};
    std::atomic<bool> watchdog_done{false};
    std::atomic<bool> watchdog_timeout{false};
    std::atomic<bool> watchdog_halted{false};
    std::atomic<uint64_t> runs{0};
    std::atomic<uint32_t> paused_count{0};
    std::atomic<int64_t> timeout_extension_ms{0};
    auto set_runner_thread = [&](uint32_t cpu_index) {
      std::lock_guard<std::mutex> lock(runner_mu);
      runner_threads[cpu_index] = pthread_self();
      runner_ready[cpu_index] = true;
    };
    auto interrupt_all = [&]() {
      for (auto& cpu : vcpus) {
        cpu.run->immediate_exit = 1;
      }
      std::lock_guard<std::mutex> lock(runner_mu);
      for (uint32_t i = 0; i < cpus; i++) {
        if (runner_ready[i]) {
          pthread_kill(runner_threads[i], SIGUSR1);
        }
      }
    };
    auto finish = [&](const std::string& reason, uint32_t code) {
      bool expected = false;
      if (vm_done.compare_exchange_strong(expected, true)) {
        {
          std::lock_guard<std::mutex> lock(result_mu);
          final_exit_reason = reason;
          final_reason = code;
        }
        interrupt_all();
      }
    };
    auto record_error = [&](const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(result_mu);
        if (error_message.empty()) {
          error_message = message;
        }
      }
      finish("host-error", KVM_EXIT_INTERNAL_ERROR);
    };
    std::thread watchdog([&]() {
      while (!watchdog_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (host_interrupt_requested.load()) {
          finish("host-stop", KVM_EXIT_INTR);
          break;
        }
        if (control.command() != kControlRun) {
          interrupt_all();
        }
        if (net && net->tap_readable()) {
          interrupt_all();
        }
        if (control.command() == kControlPause) {
          continue;
        }
        auto deadline = base_deadline + std::chrono::milliseconds(timeout_extension_ms.load());
        if (timeout_enabled && std::chrono::steady_clock::now() > deadline) {
          watchdog_timeout = true;
          break;
        }
        if (uart.contains("reboot: System halted") || uart.contains("Restarting system")) {
          watchdog_halted = true;
          finish("halted-console", KVM_EXIT_HLT);
          break;
        }
      }
      if (!watchdog_done.load()) {
        interrupt_all();
      }
    });
    auto stop_watchdog = [&]() {
      watchdog_done = true;
      if (watchdog.joinable()) {
        watchdog.join();
      }
      sigaction(SIGUSR1, &old_sa, nullptr);
    };
    auto make_result = [&]() {
      control.set_state(kControlStateExited);
      stop_input();
      stop_watchdog();
      if (!error_message.empty()) {
        throw std::runtime_error(error_message);
      }
      std::string reason;
      uint32_t code = 0;
      {
        std::lock_guard<std::mutex> lock(result_mu);
        reason = final_exit_reason;
        code = final_reason;
      }
      uint64_t total_runs = runs.load();
      napi_value out = MakeObject(env);
      SetString(env, out, "exitReason", reason);
      SetUint32(env, out, "exitReasonCode", code);
      SetUint32(env, out, "runs", total_runs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total_runs));
      SetString(env, out, "console", uart.console());
      return out;
    };

    auto vcpu_loop = [&](uint32_t cpu_index) {
      Vcpu& cpu = vcpus[cpu_index];
      set_runner_thread(cpu_index);
      for (;;) {
        if (vm_done.load()) {
          return;
        }
        int32_t command = control.command();
        if (command == kControlStop) {
          control.set_state(kControlStateStopping);
          finish("host-stop", KVM_EXIT_INTR);
          return;
        }
        if (command == kControlPause) {
          auto paused_at = std::chrono::steady_clock::now();
          if (paused_count.fetch_add(1) + 1 == cpus) {
            control.set_state(kControlStatePaused);
          }
          while (control.command() == kControlPause && !vm_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          paused_count.fetch_sub(1);
          if (timeout_enabled && cpu_index == 0) {
            auto paused_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - paused_at)
                                 .count();
            timeout_extension_ms.fetch_add(paused_ms);
          }
          if (control.command() == kControlStop) {
            control.set_state(kControlStateStopping);
            finish("host-stop", KVM_EXIT_INTR);
            return;
          }
          if (!vm_done.load()) {
            control.set_state(kControlStateRunning);
          }
        }
        if (cpu_index == 0 && net) {
          std::lock_guard<std::mutex> lock(device_mu);
          net->poll_rx();
        }
        cpu.run->immediate_exit = 0;
        int rc = ioctl(cpu.fd.get(), KVM_RUN, 0);
        if (rc < 0 && (errno == EINTR || errno == EAGAIN)) {
          cpu.run->immediate_exit = 0;
          if (cpu_index == 0 && net) {
            std::lock_guard<std::mutex> lock(device_mu);
            net->poll_rx();
          }
          if (watchdog_halted.load()) {
            finish("halted-console", KVM_EXIT_HLT);
            return;
          }
          if (watchdog_timeout.load()) {
            throw std::runtime_error("VM timed out after " + std::to_string(timeout_ms) + "ms");
          }
          continue;
        }
        CheckErr(rc, "KVM_RUN");
        runs.fetch_add(1);
        uint32_t exit_reason = cpu.run->exit_reason;
        switch (cpu.run->exit_reason) {
          case KVM_EXIT_IO: {
            GuestExit guest_exit;
            {
              std::lock_guard<std::mutex> lock(device_mu);
              HandleIo(cpu.run, uart, &guest_exit);
            }
            if (guest_exit.requested) {
              finish("guest-exit", exit_reason);
              return;
            }
            if (uart.contains("reboot: System halted") || uart.contains("Restarting system")) {
              finish("halted-console", KVM_EXIT_HLT);
              return;
            }
            break;
          }
          case KVM_EXIT_MMIO:
            {
              std::lock_guard<std::mutex> lock(device_mu);
              HandleMmio(cpu.run, blks, net.get());
            }
            break;
          case KVM_EXIT_IRQ_WINDOW_OPEN:
            cpu.run->request_interrupt_window = 0;
            break;
          case KVM_EXIT_HLT:
            if (cpu_index == 0) {
              finish(ExitReasonName(exit_reason), exit_reason);
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
          case KVM_EXIT_SHUTDOWN:
          case KVM_EXIT_SYSTEM_EVENT:
            finish(ExitReasonName(exit_reason), exit_reason);
            return;
          case KVM_EXIT_INTERNAL_ERROR:
            throw std::runtime_error("KVM internal error");
          case KVM_EXIT_FAIL_ENTRY:
            throw std::runtime_error("KVM fail entry hardware_entry_failure_reason=" + std::to_string(cpu.run->fail_entry.hardware_entry_failure_reason));
          default:
            throw std::runtime_error("unhandled KVM exit: " + ExitReasonName(exit_reason));
        }
      }
    };

    std::vector<std::thread> ap_threads;
    ap_threads.reserve(cpus > 0 ? cpus - 1 : 0);
    try {
      control.set_state(kControlStateRunning);
      for (uint32_t i = 1; i < cpus; i++) {
        ap_threads.emplace_back([&, i]() {
          try {
            vcpu_loop(i);
          } catch (const std::exception& err) {
            record_error(err.what());
          } catch (...) {
            record_error("unknown vCPU runner error");
          }
        });
      }
      try {
        vcpu_loop(0);
      } catch (const std::exception& err) {
        record_error(err.what());
      } catch (...) {
        record_error("unknown vCPU runner error");
      }
      finish("host-stop", KVM_EXIT_INTR);
      for (auto& thread : ap_threads) {
        if (thread.joinable()) {
          thread.join();
        }
      }
      return make_result();
    } catch (...) {
      finish("host-error", KVM_EXIT_INTERNAL_ERROR);
      for (auto& thread : ap_threads) {
        if (thread.joinable()) {
          thread.join();
        }
      }
      stop_input();
      stop_watchdog();
      throw;
    }
  } catch (const std::exception& err) {
    return Throw(env, err);
  }
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"probeKvm", nullptr, ProbeKvm, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"smokeHlt", nullptr, SmokeHlt, nullptr, nullptr, nullptr, napi_default, nullptr},
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
