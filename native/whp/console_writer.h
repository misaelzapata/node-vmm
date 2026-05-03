#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace node_vmm::whp {

class ConsoleWriter {
 public:
  explicit ConsoleWriter(bool enabled) : enabled_(enabled) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    output_is_console_ = enabled_ && out != INVALID_HANDLE_VALUE && out != nullptr && GetConsoleMode(out, &mode) != 0;
    if (output_is_console_) {
      old_output_cp_ = GetConsoleOutputCP();
      if (old_output_cp_ != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0) {
        changed_output_cp_ = true;
      }
    }
    if (enabled_) {
      writer_ = std::thread([this]() { writer_loop(); });
    }
  }

  ~ConsoleWriter() { stop(); }

  ConsoleWriter(const ConsoleWriter&) = delete;
  ConsoleWriter& operator=(const ConsoleWriter&) = delete;

  void write(const std::string& bytes) {
    if (bytes.empty()) {
      return;
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&]() {
      return stopping_ || !enabled_ || pending_.size() < kMaxPendingBytes;
    });
    if (!enabled_ || stopping_) {
      return;
    }
    if (pending_.size() + bytes.size() > kMaxPendingBytes) {
      flush_locked(lock);
      if (!enabled_ || stopping_) {
        return;
      }
    }
    pending_.append(bytes);
    lock.unlock();
    cv_.notify_one();
  }

  bool backed_up() {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size() >= kBackpressureBytes;
  }

  size_t pending_bytes() {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!enabled_ && !writer_.joinable()) {
        return;
      }
      stopping_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) {
      writer_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (changed_output_cp_) {
      SetConsoleOutputCP(old_output_cp_);
      changed_output_cp_ = false;
    }
    enabled_ = false;
  }

  void flush_pending() {
    std::unique_lock<std::mutex> lock(mu_);
    flush_locked(lock);
  }

  std::string cursor_position_report() {
    flush_pending();
    std::lock_guard<std::mutex> file_lock(file_mu_);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && out != nullptr && GetConsoleScreenBufferInfo(out, &info) != 0) {
      int row = static_cast<int>(info.dwCursorPosition.Y - info.srWindow.Top + 1);
      int col = static_cast<int>(info.dwCursorPosition.X - info.srWindow.Left + 1);
      if (row < 1) row = 1;
      if (col < 1) col = 1;
      char response[32];
      std::snprintf(response, sizeof(response), "\x1b[%d;%dR", row, col);
      return response;
    }
    return "\x1b[1;1R";
  }

  bool can_passthrough_terminal_query() const {
    return output_is_console_;
  }

  static std::string NormalizeHostTerminalBytesForTest(const std::string& bytes) {
    bool dec_restore_pending_clear = false;
    bool swallow_dec_clear = false;
    bool swallow_dec_clear_cr = false;
    return normalize_host_terminal_bytes_impl(
        bytes.data(),
        bytes.size(),
        dec_restore_pending_clear,
        swallow_dec_clear,
        swallow_dec_clear_cr);
  }

 private:
  enum class ParseState {
    Normal,
    Esc,
    Csi,
    Osc,
  };

  static constexpr size_t kFlushThreshold = 16 * 1024;
  static constexpr size_t kBackpressureBytes = 64 * 1024;
  static constexpr size_t kMaxPendingBytes = 256 * 1024;
  static constexpr auto kCoalesceDelay = std::chrono::milliseconds(6);
  static constexpr auto kRedrawFrameDelay = std::chrono::milliseconds(100);

  static bool is_csi_final(unsigned char byte) {
    return byte >= 0x40 && byte <= 0x7E;
  }

  static size_t terminal_safe_prefix(const std::string& bytes) {
    ParseState state = ParseState::Normal;
    bool osc_esc = false;
    size_t safe = 0;

    for (size_t i = 0; i < bytes.size(); i++) {
      unsigned char byte = static_cast<unsigned char>(bytes[i]);
      switch (state) {
        case ParseState::Normal:
          if (byte == 0x1B) {
            state = ParseState::Esc;
          } else if (byte == '\n' || byte == '\a') {
            safe = i + 1;
          }
          break;
        case ParseState::Esc:
          if (byte == '[') {
            state = ParseState::Csi;
          } else if (byte == ']') {
            state = ParseState::Osc;
            osc_esc = false;
          } else {
            state = ParseState::Normal;
            safe = i + 1;
          }
          break;
        case ParseState::Csi:
          if (is_csi_final(byte)) {
            state = ParseState::Normal;
            safe = i + 1;
          }
          break;
        case ParseState::Osc:
          if (byte == '\a') {
            state = ParseState::Normal;
            safe = i + 1;
            osc_esc = false;
          } else if (osc_esc && byte == '\\') {
            state = ParseState::Normal;
            safe = i + 1;
            osc_esc = false;
          } else {
            osc_esc = byte == 0x1B;
          }
          break;
      }
    }

    return safe;
  }

  static bool has_open_dec_redraw_frame(const std::string& bytes) {
    for (size_t i = 0; i + 1 < bytes.size(); i++) {
      if (static_cast<unsigned char>(bytes[i]) != 0x1B) {
        continue;
      }
      if (bytes[i + 1] == '7') {
        size_t restore = i + 2;
        while (restore + 1 < bytes.size()) {
          if (static_cast<unsigned char>(bytes[restore]) == 0x1B && bytes[restore + 1] == '8') {
            break;
          }
          restore++;
        }
        if (restore + 1 >= bytes.size()) {
          return true;
        }
        size_t after_restore = restore + 2;
        if (after_restore >= bytes.size()) {
          return true;
        }
        if (static_cast<unsigned char>(bytes[after_restore]) != 0x1B) {
          i = restore + 1;
          continue;
        }
        if (after_restore + 1 >= bytes.size()) {
          return true;
        }
        if (bytes[after_restore + 1] != '[') {
          i = restore + 1;
          continue;
        }
        size_t csi_end = after_restore + 2;
        while (csi_end < bytes.size() && !is_csi_final(static_cast<unsigned char>(bytes[csi_end]))) {
          csi_end++;
        }
        if (csi_end >= bytes.size()) {
          return true;
        }
        std::string params(bytes.data() + after_restore + 2, csi_end - after_restore - 2);
        if (bytes[csi_end] == 'K' && (params.empty() || params == "0")) {
          return false;
        }
        i = restore + 1;
      }
    }
    return false;
  }

  static bool has_control_byte(const char* data, size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
      unsigned char byte = static_cast<unsigned char>(data[i]);
      if (byte < 0x20 || byte == 0x7F) {
        return true;
      }
    }
    return false;
  }

  static size_t trim_right_spaces(const char* data, size_t start, size_t end) {
    while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) {
      end--;
    }
    return end;
  }

  static bool looks_like_progress_payload(const char* data, size_t start, size_t end) {
    while (start < end && data[start] == ' ') {
      start++;
    }
    size_t digits = 0;
    while (start < end && data[start] >= '0' && data[start] <= '9' && digits < 3) {
      start++;
      digits++;
    }
    return digits > 0 && start < end && data[start] == '%';
  }

  static bool parse_csi_final(
      const char* data,
      size_t size,
      size_t start,
      size_t* end,
      std::string* params,
      char* final_byte) {
    if (start + 2 > size || static_cast<unsigned char>(data[start]) != 0x1B || data[start + 1] != '[') {
      return false;
    }
    size_t pos = start + 2;
    while (pos < size && !is_csi_final(static_cast<unsigned char>(data[pos]))) {
      pos++;
    }
    if (pos >= size) {
      return false;
    }
    if (params) {
      params->assign(data + start + 2, pos - start - 2);
    }
    if (final_byte) {
      *final_byte = data[pos];
    }
    *end = pos + 1;
    return true;
  }

  static std::string normalize_host_terminal_bytes_impl(
      const char* data,
      size_t size,
      bool& dec_restore_pending_clear,
      bool& swallow_dec_clear,
      bool& swallow_dec_clear_cr) {
    std::string out;
    out.reserve(size + 32);
    for (size_t i = 0; i < size; i++) {
      if (swallow_dec_clear_cr) {
        swallow_dec_clear_cr = false;
        if (data[i] == '\r') {
          continue;
        }
      }
      if (static_cast<unsigned char>(data[i]) == 0x1B && i + 1 < size) {
        if (data[i + 1] == '7') {
          size_t restore = i + 2;
          while (restore + 1 < size) {
            if (static_cast<unsigned char>(data[restore]) == 0x1B && data[restore + 1] == '8') {
              break;
            }
            restore++;
          }
          if (restore + 1 < size) {
            size_t csi_end = restore + 2;
            std::string params;
            char final_byte = '\0';
            bool has_clear = parse_csi_final(data, size, csi_end, &csi_end, &params, &final_byte) &&
                             final_byte == 'K' &&
                             (params.empty() || params == "0");
            bool simple_payload = !has_control_byte(data, i + 2, restore);
            bool progress_payload = looks_like_progress_payload(data, i + 2, restore);
            if ((has_clear || progress_payload) && simple_payload) {
              size_t payload_end = trim_right_spaces(data, i + 2, restore);
              size_t next = restore + 2;
              if (has_clear) {
                next = csi_end;
                if (next < size && data[next] == '\r') {
                  next++;
                }
              } else {
                swallow_dec_clear = true;
                swallow_dec_clear_cr = false;
              }
              // BusyBox apk redraws progress as:
              //   ESC 7 + padded line + ESC 8 + CSI 0 K + CR
              //
              // ConPTY/VS Code can leave the visible cursor/artifact at the
              // padded right edge of that frame. Since the frame is just a
              // single-line redraw, translate it to the terminal primitive
              // QEMU's stdio path effectively relies on: carriage-return,
              // clear the current line, print the payload, and park the
              // cursor back at column 1. We trim the padding because the
              // clear-line already removes stale bytes.
              out.append("\x1b[?25l\r\x1b[2K", 11);
              out.append(data + i + 2, payload_end - (i + 2));
              out.append("\r\x1b[?25h", 7);
              dec_restore_pending_clear = false;
              i = next - 1;
              continue;
            }
          }
        }
        if (data[i + 1] == '8') {
          dec_restore_pending_clear = true;
          out.push_back(data[i]);
          out.push_back(data[i + 1]);
          i++;
          continue;
        }
        if (data[i + 1] == '[') {
          size_t end = 0;
          std::string params;
          char final_byte = '\0';
          if (parse_csi_final(data, size, i, &end, &params, &final_byte)) {
            if (swallow_dec_clear && final_byte == 'K' && (params.empty() || params == "0")) {
              swallow_dec_clear = false;
              swallow_dec_clear_cr = true;
              i = end - 1;
              continue;
            }
            swallow_dec_clear = false;
            out.append(data + i, end - i);
            if (dec_restore_pending_clear && final_byte == 'K' && (params.empty() || params == "0")) {
              out.push_back('\r');
              dec_restore_pending_clear = false;
            } else if (final_byte != 'K') {
              dec_restore_pending_clear = false;
            }
            i = end - 1;
            continue;
          }
        }
      } else if (data[i] != ' ' && data[i] != '\t') {
        dec_restore_pending_clear = false;
        swallow_dec_clear = false;
      }
      out.push_back(data[i]);
    }
    return out;
  }

  std::string normalize_host_terminal_bytes(const char* data, size_t size) {
    return normalize_host_terminal_bytes_impl(
        data,
        size,
        dec_restore_pending_clear_,
        swallow_dec_clear_,
        swallow_dec_clear_cr_);
  }

  void flush_locked(std::unique_lock<std::mutex>& lock) {
    std::string bytes;
    bytes.swap(pending_);
    lock.unlock();
    write_file(bytes.data(), bytes.size());
    lock.lock();
    cv_.notify_all();
  }

  void writer_loop() {
    for (;;) {
      std::string bytes;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&]() { return stopping_ || !pending_.empty(); });
        if (pending_.empty() && stopping_) {
          break;
        }
        auto now = std::chrono::steady_clock::now();
        auto deadline = now + kCoalesceDelay;
        auto redraw_deadline = now + kRedrawFrameDelay;
        size_t observed_pending_size = pending_.size();
        size_t safe = terminal_safe_prefix(pending_);
        bool open_redraw = has_open_dec_redraw_frame(pending_);
        while (!stopping_ && pending_.size() < kFlushThreshold && (safe == 0 || open_redraw)) {
          const auto wait_deadline = open_redraw ? redraw_deadline : deadline;
          if (cv_.wait_until(lock, wait_deadline) == std::cv_status::timeout) {
            if (open_redraw && !stopping_ && pending_.size() < kFlushThreshold) {
              cv_.wait(lock, [&]() {
                return stopping_ ||
                       pending_.size() != observed_pending_size ||
                       pending_.size() >= kFlushThreshold;
              });
              if (pending_.size() != observed_pending_size) {
                observed_pending_size = pending_.size();
                now = std::chrono::steady_clock::now();
                deadline = now + kCoalesceDelay;
                redraw_deadline = now + kRedrawFrameDelay;
              }
              safe = terminal_safe_prefix(pending_);
              open_redraw = has_open_dec_redraw_frame(pending_);
              continue;
            }
            break;
          }
          if (pending_.size() != observed_pending_size) {
            observed_pending_size = pending_.size();
            now = std::chrono::steady_clock::now();
            deadline = now + kCoalesceDelay;
            redraw_deadline = now + kRedrawFrameDelay;
          }
          safe = terminal_safe_prefix(pending_);
          open_redraw = has_open_dec_redraw_frame(pending_);
        }
        if (safe == 0 || open_redraw || pending_.size() >= kFlushThreshold || stopping_) {
          safe = pending_.size();
        }
        if (safe == pending_.size()) {
          bytes.swap(pending_);
        } else {
          bytes.assign(pending_.data(), safe);
          pending_.erase(0, safe);
        }
        cv_.notify_all();
      }
      write_file(bytes.data(), bytes.size());
    }
  }

  void write_file(const char* data, size_t size) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr || data == nullptr || size == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(file_mu_);
    // Mirrors QEMU's chardev/char-win-stdio.c win_stdio_chr_write loop:
    // ALWAYS loop until either every byte is consumed or WriteFile actually
    // fails. Our previous version broke on `written == 0` (success but zero
    // bytes), which silently dropped the tail of the buffer in some VT-mode
    // race conditions. The console can return success with zero bytes when
    // the VT engine is mid-parse on an ANSI sequence; QEMU's behavior of
    // re-trying eventually drains the buffer.
    std::string normalized = normalize_host_terminal_bytes(data, size);
    data = normalized.data();
    size = normalized.size();
    size_t remaining = size;
    while (remaining > 0) {
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 64 * 1024));
      DWORD written = 0;
      if (WriteFile(out, data, chunk, &written, nullptr) == 0) {
        break;  // hard failure: bail (no recovery)
      }
      if (written == 0) {
        // Zero-byte success means the console transiently rejected our
        // write. Yield once so the VT processor can drain, then retry.
        std::this_thread::yield();
        continue;
      }
      data += written;
      remaining -= written;
    }
  }

  bool enabled_{false};
  bool output_is_console_{false};
  bool changed_output_cp_{false};
  bool stopping_{false};
  UINT old_output_cp_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  std::string pending_;
  std::mutex file_mu_;
  std::thread writer_;
  bool dec_restore_pending_clear_{false};
  bool swallow_dec_clear_{false};
  bool swallow_dec_clear_cr_{false};
};

}  // namespace node_vmm::whp
