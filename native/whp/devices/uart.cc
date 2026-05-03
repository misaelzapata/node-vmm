#include "uart.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace node_vmm::whp {
namespace {

bool EqualsAsciiNoCase(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != '\0' && *right != '\0') {
    unsigned char a = static_cast<unsigned char>(*left++);
    unsigned char b = static_cast<unsigned char>(*right++);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return *left == '\0' && *right == '\0';
}

}  // namespace

Uart::Uart(size_t limit, bool echo_stdout)
    : limit_(limit), echo_stdout_(echo_stdout), stdout_writer_(echo_stdout) {
  set_terminal_query_mode_from_env();
}

void Uart::attach_irq_raiser(std::function<void(uint32_t)> raiser) {
  irq_raiser_ = std::move(raiser);
}

void Uart::enable_rx_debug() { rx_dbg_ = true; }

uint8_t Uart::read(uint16_t offset) {
  std::lock_guard<std::mutex> lock(mu_);
  bool dlab = (lcr_ & 0x80) != 0;
  switch (offset) {
    case 0:
      if (dlab) {
        return dll_;
      }
      {
        uint8_t value = 0;
        if (fifo_enabled_locked()) {
          if (!rx_fifo_.empty()) {
            value = rx_fifo_.front();
            rx_fifo_.pop_front();
          }
          timeout_ipending_ = false;
          drain_rx_staging_locked();
          if (rx_fifo_.empty()) {
            lsr_ = static_cast<uint8_t>(lsr_ & ~kLsrDr);
          }
        } else {
          if (lsr_ & kLsrDr) {
            value = rbr_;
            lsr_ = static_cast<uint8_t>(lsr_ & ~kLsrDr);
          }
          timeout_ipending_ = false;
          drain_rx_staging_locked();
        }
        update_interrupt_locked();
        return value;
      }
    case 1:
      return dlab ? dlh_ : ier_;
    case 2:
      {
        uint8_t value = static_cast<uint8_t>(iir_fifo_bits_locked() | iir_);
        if ((iir_ & 0x0F) == 0x02) {
          thr_ipending_ = false;
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
        uint8_t result = lsr_;
        static std::atomic<uint64_t> lsr_reads{0};
        uint64_t n = ++lsr_reads;
        size_t rx_size = fifo_enabled_locked()
            ? rx_fifo_.size()
            : ((lsr_ & kLsrDr) ? 1 : 0);
        if (rx_dbg_ && (n <= 25 || n % 50 == 0 || rx_size != 0)) {
          std::fprintf(stderr, "[node-vmm uart] LSR read #%llu = 0x%02x rx_size=%zu\n",
                       static_cast<unsigned long long>(n), result, rx_size);
        }
        if (lsr_ & (kLsrOe | kLsrPe | kLsrFe | kLsrBi)) {
          lsr_ = static_cast<uint8_t>(lsr_ & ~(kLsrOe | kLsrPe | kLsrFe | kLsrBi));
          update_interrupt_locked();
        }
        return result;
      }
    case 6:
      {
        uint8_t value = current_msr_locked();
        msr_ &= 0xF0;
        update_interrupt_locked();
        return value;
      }
    case 7:
      return scr_;
    default:
      return 0;
  }
}

void Uart::write(uint16_t offset, uint8_t value) {
  switch (offset) {
    case 0:
      {
        bool dlab = false;
        {
          std::lock_guard<std::mutex> lock(mu_);
          dlab = (lcr_ & 0x80) != 0;
          if (dlab) {
            dll_ = value;
          }
        }
        if (!dlab) {
          handle_tx_byte(value);
        }
        break;
      }
    case 1:
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (lcr_ & 0x80) {
          dlh_ = value;
        } else {
          uint8_t old_ier = ier_;
          if (rx_dbg_) {
            std::fprintf(stderr, "[node-vmm uart] IER write old=0x%02x new=0x%02x\n", ier_, value);
          }
          ier_ = value & 0x0F;
          if (((old_ier ^ ier_) & kIerThri) != 0) {
            thr_ipending_ = ((ier_ & kIerThri) != 0) && ((lsr_ & kLsrThre) != 0);
          }
          update_interrupt_locked();
        }
        break;
      }
    case 2:
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (rx_dbg_) {
          std::fprintf(stderr, "[node-vmm uart] FCR write 0x%02x\n", value);
        }
        fcr_ = static_cast<uint8_t>(value & (kFcrEnable | kFcrTriggerMask));
        if ((value & kFcrClearRx) != 0) {
          clear_rx_locked();
        } else {
          drain_rx_staging_locked();
        }
        if ((value & kFcrClearTx) != 0) {
          clear_tx_locked();
        }
        update_interrupt_locked();
        break;
      }
    case 3:
      {
        std::lock_guard<std::mutex> lock(mu_);
        uint8_t old_lcr = lcr_;
        lcr_ = value;
        if ((value & 0x40) != 0 && (old_lcr & 0x40) == 0) {
          lsr_ = static_cast<uint8_t>(lsr_ | kLsrBi | kLsrDr);
          rbr_ = 0;
        }
        update_interrupt_locked();
        break;
      }
    case 4:
      if (rx_dbg_) {
        std::fprintf(stderr, "[node-vmm uart] MCR write 0x%02x (out2=%d)\n", value, (value >> 3) & 1);
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        uint8_t old_msr = current_msr_locked();
        mcr_ = value & 0x1F;
        update_msr_delta_locked(old_msr, current_msr_locked());
        update_interrupt_locked();
      }
      break;
    case 7:
      {
        std::lock_guard<std::mutex> lock(mu_);
        scr_ = value;
      }
      break;
    default:
      break;
  }
}

std::string Uart::console() const {
  std::lock_guard<std::mutex> lock(mu_);
  return console_;
}

bool Uart::contains(const std::string& needle) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (needle == "reboot: System halted") {
    return halted_seen_;
  }
  if (needle == "Restarting system") {
    return restarting_seen_;
  }
  return console_.find(needle) != std::string::npos;
}

void Uart::emit_bytes(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  emit_tx_locked(std::string(reinterpret_cast<const char*>(data), len));
}

size_t Uart::enqueue_rx(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  return enqueue_rx_locked(reinterpret_cast<const char*>(data), len);
}

void Uart::emit_tx_locked(const std::string& bytes) {
  if (bytes.empty()) {
    return;
  }
  track_console_markers(bytes);
  size_t available = limit_ > console_.size() ? limit_ - console_.size() : 0;
  if (available > 0) {
    console_.append(bytes.data(), std::min(available, bytes.size()));
  }
  if (echo_stdout_) {
    write_stdout(bytes);
  }
}

// Windows console treats a bare LF as "move down, keep column" when the
// guest line discipline has not converted LF to CRLF. Normalize bare LF
// here, but preserve CRLF that the guest already sent. The previous
// unconditional CR insertion fixed one progress-bar symptom while creating
// doubled carriage returns that confused ConPTY/VS Code redraw.
std::string Uart::NormalizeCrlf(const std::string& bytes, char& last_byte) {
  std::string normalized;
  normalized.reserve(bytes.size() * 2);
  for (char byte : bytes) {
    if (byte == '\n' && last_byte != '\r') {
      normalized.push_back('\r');
    }
    normalized.push_back(byte);
    last_byte = byte;
  }
  return normalized;
}

void Uart::write_stdout(const std::string& bytes) {
  if (bytes.empty()) {
    return;
  }
  std::string normalized = NormalizeCrlf(bytes, last_stdout_byte_);
  stdout_writer_.write(normalized);
}

size_t Uart::enqueue_rx_locked(const char* data, size_t len, bool front) {
  if (data == nullptr || len == 0) {
    return 0;
  }

  auto queued = [&]() -> size_t {
    size_t n = rx_staging_.size();
    if (fifo_enabled_locked()) {
      n += rx_fifo_.size();
    } else if (lsr_ & kLsrDr) {
      n += 1;
    }
    return n;
  };

  size_t accepted = 0;
  if (front) {
    for (size_t i = len; i > 0 && queued() < kRxCapacity; i--) {
      rx_staging_.push_front(static_cast<uint8_t>(data[i - 1]));
      accepted++;
    }
  } else {
    for (size_t i = 0; i < len && queued() < kRxCapacity; i++) {
      rx_staging_.push_back(static_cast<uint8_t>(data[i]));
      accepted++;
    }
  }
  if (accepted < len) {
    lsr_ = static_cast<uint8_t>(lsr_ | kLsrOe);
  }
  drain_rx_staging_locked();
  update_interrupt_locked();
  return accepted;
}

void Uart::handle_tx_byte(uint8_t value) {
  static const std::string query = "\x1b[6n";
  std::lock_guard<std::mutex> lock(mu_);

  thr_ = value;
  lsr_ = static_cast<uint8_t>(lsr_ & ~(kLsrThre | kLsrTemt));

  if (mcr_ & 0x10) {
    transmit_byte_locked(value);
    update_interrupt_locked();
    return;
  }

  if (!terminal_query_.empty() || value == 0x1B) {
    terminal_query_.push_back(static_cast<char>(value));
    if (query.rfind(terminal_query_, 0) == 0) {
      if (terminal_query_ == query) {
        if (dsr_passthrough_ && stdout_writer_.can_passthrough_terminal_query()) {
          emit_tx_locked(terminal_query_);
        } else {
          std::string response = stdout_writer_.cursor_position_report();
          enqueue_rx_locked(response.data(), response.size(), false);
        }
        terminal_query_.clear();
      }
      lsr_ = static_cast<uint8_t>(lsr_ | kLsrThre | kLsrTemt);
      thr_ipending_ = true;
      update_interrupt_locked();
      return;
    }
    emit_tx_locked(terminal_query_);
    terminal_query_.clear();
    lsr_ = static_cast<uint8_t>(lsr_ | kLsrThre | kLsrTemt);
    thr_ipending_ = true;
    update_interrupt_locked();
    return;
  }

  transmit_byte_locked(value);
  update_interrupt_locked();
}

bool Uart::fifo_enabled_locked() const {
  return (fcr_ & kFcrEnable) != 0;
}

size_t Uart::rx_trigger_level_locked() const {
  switch (fcr_ & kFcrTriggerMask) {
    case 0x00:
      return 1;
    case 0x40:
      return 4;
    case 0x80:
      return 8;
    case 0xC0:
      return 14;
    default:
      return 1;
  }
}

void Uart::drain_rx_staging_locked() {
  if (fifo_enabled_locked()) {
    while (!rx_staging_.empty() && rx_fifo_.size() < kFifoSize) {
      rx_fifo_.push_back(rx_staging_.front());
      rx_staging_.pop_front();
    }
    if (rx_fifo_.empty()) {
      lsr_ = static_cast<uint8_t>(lsr_ & ~kLsrDr);
      timeout_ipending_ = false;
    } else {
      lsr_ = static_cast<uint8_t>(lsr_ | kLsrDr);
      timeout_ipending_ = rx_fifo_.size() < rx_trigger_level_locked();
    }
    return;
  }

  if ((lsr_ & kLsrDr) == 0 && !rx_staging_.empty()) {
    rbr_ = rx_staging_.front();
    rx_staging_.pop_front();
    lsr_ = static_cast<uint8_t>(lsr_ | kLsrDr);
  }
  timeout_ipending_ = false;
}

bool Uart::rx_ready_for_interrupt_locked() const {
  if (fifo_enabled_locked()) {
    return !rx_fifo_.empty() && rx_fifo_.size() >= rx_trigger_level_locked();
  }
  return (lsr_ & kLsrDr) != 0;
}

uint8_t Uart::iir_fifo_bits_locked() const {
  return fifo_enabled_locked() ? 0xC0 : 0x00;
}

void Uart::clear_rx_locked() {
  rx_fifo_.clear();
  rx_staging_.clear();
  rbr_ = 0;
  timeout_ipending_ = false;
  lsr_ = static_cast<uint8_t>(lsr_ & ~(kLsrDr | kLsrOe | kLsrPe | kLsrFe | kLsrBi));
}

void Uart::clear_tx_locked() {
  tx_fifo_.clear();
  thr_ = 0;
  tsr_ = 0;
  terminal_query_.clear();
  lsr_ = static_cast<uint8_t>(lsr_ | kLsrThre | kLsrTemt);
  thr_ipending_ = true;
}

void Uart::transmit_byte_locked(uint8_t value) {
  if (fifo_enabled_locked()) {
    if (tx_fifo_.size() < kFifoSize) {
      tx_fifo_.push_back(value);
    }
    while (!tx_fifo_.empty()) {
      tsr_ = tx_fifo_.front();
      tx_fifo_.pop_front();
      if (mcr_ & 0x10) {
        enqueue_rx_locked(reinterpret_cast<const char*>(&tsr_), 1);
      } else {
        char byte = static_cast<char>(tsr_);
        emit_tx_locked(std::string(&byte, 1));
      }
    }
  } else {
    tsr_ = value;
    if (mcr_ & 0x10) {
      enqueue_rx_locked(reinterpret_cast<const char*>(&tsr_), 1);
    } else {
      char byte = static_cast<char>(tsr_);
      emit_tx_locked(std::string(&byte, 1));
    }
  }
  lsr_ = static_cast<uint8_t>(lsr_ | kLsrThre | kLsrTemt);
  thr_ipending_ = true;
}

void Uart::set_terminal_query_mode_from_env() {
  const char* mode = std::getenv("NODE_VMM_WHP_DSR");
  dsr_passthrough_ = EqualsAsciiNoCase(mode, "passthrough");
}

size_t Uart::AdvanceMatch(const char* pattern, size_t pattern_len, size_t current, char byte) {
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

void Uart::track_console_markers(const std::string& bytes) {
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

uint8_t Uart::current_msr_locked() const {
  if (mcr_ & 0x10) {
    uint8_t loop = msr_ & 0x0F;
    if (mcr_ & 0x02) loop |= 0x10;  // RTS -> CTS
    if (mcr_ & 0x01) loop |= 0x20;  // DTR -> DSR
    if (mcr_ & 0x04) loop |= 0x40;  // OUT1 -> RI
    if (mcr_ & 0x08) loop |= 0x80;  // OUT2 -> DCD
    return loop;
  }
  return msr_;
}

void Uart::update_msr_delta_locked(uint8_t old_msr, uint8_t new_msr) {
  uint8_t old_status = old_msr & 0xF0;
  uint8_t new_status = new_msr & 0xF0;
  uint8_t delta = 0;
  if ((old_status ^ new_status) & 0x10) delta |= 0x01;  // CTS
  if ((old_status ^ new_status) & 0x20) delta |= 0x02;  // DSR
  if ((old_status & 0x40) && !(new_status & 0x40)) delta |= 0x04;  // RI trailing edge
  if ((old_status ^ new_status) & 0x80) delta |= 0x08;  // DCD
  msr_ = static_cast<uint8_t>((new_msr & 0xF0) | ((msr_ | delta) & 0x0F));
}

void Uart::update_interrupt_locked() {
  bool raise = false;
  if ((ier_ & kIerRlsi) && (lsr_ & (kLsrOe | kLsrPe | kLsrFe | kLsrBi))) {
    iir_ = 0x06;
    raise = true;
  } else if ((ier_ & kIerRdi) && timeout_ipending_) {
    iir_ = 0x0C;
    raise = true;
  } else if ((ier_ & kIerRdi) && rx_ready_for_interrupt_locked()) {
    iir_ = 0x04;
    raise = true;
  } else if ((ier_ & kIerThri) && thr_ipending_ && (lsr_ & kLsrThre)) {
    iir_ = 0x02;
    raise = true;
  } else if ((ier_ & kIerMsi) && (current_msr_locked() & 0x0F)) {
    iir_ = 0x00;
    raise = true;
  } else {
    iir_ = 0x01;
  }
  if (raise && irq_raiser_) {
    irq_raiser_(4);
  }
}

}  // namespace node_vmm::whp
