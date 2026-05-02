#pragma once

// 16550-style UART covering the registers Linux's serial8250 driver
// actually reads and writes during boot and runtime: THR/RBR, IER,
// IIR/FCR, LCR, MCR, LSR, MSR, SCR, plus the DLAB-gated divisor latch
// (DLL/DLH).
//
// Two host-facing IO paths:
//   * write_stdout - TX bytes to host stdout via ConsoleWriter.
//   * enqueue_rx   - host stdin / paste buffer to the guest RX FIFO with
//                    overrun + IRQ4 dispatch.
//
// IoApic decoupling: instead of holding an IoApic* and a fallback
// irq_router_, we accept a single std::function<void(uint32_t)> at
// attach_irq_raiser. Caller binds the IOAPIC-then-PIC routing logic.
// Mirrors the virtio extraction pattern from PR-5a.

#include "../console_writer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace node_vmm::whp {

class Uart {
 public:
  explicit Uart(size_t limit, bool echo_stdout = true);

  // Wires the UART's IRQ4 raise path. Caller decides whether to route
  // through the IOAPIC, PIC, or both. Must be called before any vCPU
  // starts; called only from RunVm after the IRQ controllers are
  // constructed. Idempotent: replacing an existing raiser is supported.
  void attach_irq_raiser(std::function<void(uint32_t)> raiser);

  // Enables verbose RX/IRQ debug output via stderr. Off by default;
  // RunVm flips it on when NODE_VMM_BOOT_TIME=1.
  void enable_rx_debug();

  uint8_t read(uint16_t offset);
  void write(uint16_t offset, uint8_t value);

  std::string console() const;
  bool contains(const std::string& needle) const;

  // Direct TX byte injection used by the paravirt console port (0x600).
  // Bypasses the LCR/THR-empty interrupt path; goes straight to the
  // console buffer + host stdout.
  void emit_bytes(const uint8_t* data, size_t len);

  // Push host bytes into the guest UART RX queue. Mirrors QEMU's
  // serial_receive. Returns the count actually accepted; caller can retry
  // any tail bytes instead of dropping them.
  size_t enqueue_rx(const uint8_t* data, size_t len);

  // Pure CRLF normalizer used by write_stdout. Exposed for unit tests
  // so we can assert the contract without spinning up a partition.
  // last_byte is updated in-place to reflect the last byte appended,
  // letting callers chain calls and preserve the no-double-CR property.
  static std::string NormalizeCrlf(const std::string& bytes, char& last_byte);

 private:
  static constexpr size_t kRxCapacity = 4096;
  static constexpr size_t kFifoSize = 16;

  static constexpr uint8_t kIerRdi = 0x01;
  static constexpr uint8_t kIerThri = 0x02;
  static constexpr uint8_t kIerRlsi = 0x04;
  static constexpr uint8_t kIerMsi = 0x08;

  static constexpr uint8_t kFcrEnable = 0x01;
  static constexpr uint8_t kFcrClearRx = 0x02;
  static constexpr uint8_t kFcrClearTx = 0x04;
  static constexpr uint8_t kFcrTriggerMask = 0xC0;

  static constexpr uint8_t kLsrDr = 0x01;
  static constexpr uint8_t kLsrOe = 0x02;
  static constexpr uint8_t kLsrPe = 0x04;
  static constexpr uint8_t kLsrFe = 0x08;
  static constexpr uint8_t kLsrBi = 0x10;
  static constexpr uint8_t kLsrThre = 0x20;
  static constexpr uint8_t kLsrTemt = 0x40;

  // Forwarded from emit_bytes / handle_tx_byte. Must be called with mu_
  // held.
  void emit_tx_locked(const std::string& bytes);
  // CRLF-normalizing write to host stdout. Why this is here: see the
  // long comment in the .cc - it keeps bare LF output sane on Windows.
  void write_stdout(const std::string& bytes);

  size_t enqueue_rx_locked(const char* data, size_t len, bool front = false);
  void handle_tx_byte(uint8_t value);
  bool fifo_enabled_locked() const;
  size_t rx_trigger_level_locked() const;
  void drain_rx_staging_locked();
  bool rx_ready_for_interrupt_locked() const;
  uint8_t iir_fifo_bits_locked() const;
  void clear_rx_locked();
  void clear_tx_locked();
  void transmit_byte_locked(uint8_t value);
  void set_terminal_query_mode_from_env();

  static size_t AdvanceMatch(const char* pattern, size_t pattern_len, size_t current, char byte);
  void track_console_markers(const std::string& bytes);
  uint8_t current_msr_locked() const;
  void update_msr_delta_locked(uint8_t old_msr, uint8_t new_msr);
  void update_interrupt_locked();

  mutable std::mutex mu_;
  uint8_t ier_{0};
  uint8_t iir_{0x01};
  uint8_t fcr_{0};
  uint8_t lcr_{0};
  uint8_t mcr_{0x08};
  uint8_t msr_{0xB0};
  uint8_t lsr_{static_cast<uint8_t>(kLsrThre | kLsrTemt)};
  uint8_t scr_{0};
  uint8_t dll_{0x0C};
  uint8_t dlh_{0};
  uint8_t rbr_{0};
  uint8_t thr_{0};
  uint8_t tsr_{0};
  std::deque<uint8_t> rx_fifo_;
  std::deque<uint8_t> rx_staging_;
  std::deque<uint8_t> tx_fifo_;
  bool thr_ipending_{false};
  bool timeout_ipending_{false};
  size_t limit_{1024 * 1024};
  bool echo_stdout_{true};
  ConsoleWriter stdout_writer_;
  std::string console_;
  std::string terminal_query_;
  // Tracks the last byte written to host stdout so we can synthesize CR
  // before bare LF without doubling up when the guest already sent CRLF.
  // See write_stdout() for the rationale.
  char last_stdout_byte_{0};
  bool dsr_passthrough_{false};
  bool halted_seen_{false};
  bool restarting_seen_{false};
  size_t halted_match_{0};
  size_t restarting_match_{0};
  std::function<void(uint32_t)> irq_raiser_;
  bool rx_dbg_{false};
};

}  // namespace node_vmm::whp
