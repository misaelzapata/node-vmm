#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "win_console_ctrl.h"

#include <mutex>

namespace node_vmm::whp {
namespace {

std::mutex g_console_ctrl_mu;
std::function<void()> g_console_ctrl_callback;

BOOL WINAPI NodeVmmConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type != CTRL_C_EVENT && ctrl_type != CTRL_BREAK_EVENT) {
    return FALSE;
  }
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(g_console_ctrl_mu);
    callback = g_console_ctrl_callback;
  }
  if (!callback) {
    return FALSE;
  }
  callback();
  return TRUE;
}

}  // namespace

ScopedConsoleCtrlHandler::ScopedConsoleCtrlHandler(bool enabled, std::function<void()> callback)
    : enabled_(enabled) {
  if (!enabled_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_console_ctrl_mu);
    g_console_ctrl_callback = std::move(callback);
  }
  SetConsoleCtrlHandler(NodeVmmConsoleCtrlHandler, TRUE);
}

ScopedConsoleCtrlHandler::~ScopedConsoleCtrlHandler() {
  if (!enabled_) {
    return;
  }
  SetConsoleCtrlHandler(NodeVmmConsoleCtrlHandler, FALSE);
  std::lock_guard<std::mutex> lock(g_console_ctrl_mu);
  g_console_ctrl_callback = nullptr;
}

}  // namespace node_vmm::whp
