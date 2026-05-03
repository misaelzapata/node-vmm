#pragma once

#include <functional>

namespace node_vmm::whp {

class ScopedConsoleCtrlHandler {
 public:
  ScopedConsoleCtrlHandler(bool enabled, std::function<void()> callback);
  ~ScopedConsoleCtrlHandler();

  ScopedConsoleCtrlHandler(const ScopedConsoleCtrlHandler&) = delete;
  ScopedConsoleCtrlHandler& operator=(const ScopedConsoleCtrlHandler&) = delete;

 private:
  bool enabled_{false};
};

}  // namespace node_vmm::whp
