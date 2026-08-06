#include "platform/Signal.h"

#include <csignal>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace holder::core {
namespace {

std::atomic<bool> g_signal_requested{false};
std::atomic<int> g_last_signal{0};
using HandlerType = void (*)(int);
#if defined(SIGINT)
HandlerType g_prev_int = nullptr;
#endif
#if defined(SIGTERM)
HandlerType g_prev_term = nullptr;
#endif

void request_global_stop(int signum) noexcept {
  g_signal_requested.store(true);
  if (signum != 0) {
    g_last_signal.store(signum);
  }
}

#if defined(_WIN32)
constexpr int kWindowsCtrlClose = -1;
constexpr int kWindowsCtrlBreak = -2;
constexpr int kWindowsCtrlLogoff = -3;
constexpr int kWindowsCtrlShutdown = -4;
std::atomic<bool> g_console_handler_installed{false};

BOOL WINAPI handle_console_control(DWORD event) {
  switch (event) {
    case CTRL_C_EVENT:
#if defined(SIGINT)
      request_global_stop(SIGINT);
#else
      request_global_stop(0);
#endif
      return TRUE;
    case CTRL_BREAK_EVENT:
      request_global_stop(kWindowsCtrlBreak);
      return TRUE;
    case CTRL_CLOSE_EVENT:
      request_global_stop(kWindowsCtrlClose);
      return TRUE;
    case CTRL_LOGOFF_EVENT:
      request_global_stop(kWindowsCtrlLogoff);
      return TRUE;
    case CTRL_SHUTDOWN_EVENT:
      request_global_stop(kWindowsCtrlShutdown);
      return TRUE;
    default:
      return FALSE;
  }
}
#endif

} // namespace

SignalHandler::SignalHandler() {
  g_signal_requested.store(false);
#if defined(SIGINT)
  g_prev_int = std::signal(SIGINT, &SignalHandler::handle);
#endif
#if defined(SIGTERM)
  g_prev_term = std::signal(SIGTERM, &SignalHandler::handle);
#endif
#if defined(_WIN32)
  if (SetConsoleCtrlHandler(&handle_console_control, TRUE)) {
    g_console_handler_installed.store(true);
  }
#endif
  installed_ = true;
}

SignalHandler::~SignalHandler() {
  if (!installed_) return;
#if defined(SIGINT)
  std::signal(SIGINT, g_prev_int);
#endif
#if defined(SIGTERM)
  std::signal(SIGTERM, g_prev_term);
#endif
#if defined(_WIN32)
  if (g_console_handler_installed.exchange(false)) {
    SetConsoleCtrlHandler(&handle_console_control, FALSE);
  }
#endif
} // LCOV_EXCL_LINE

bool SignalHandler::is_requested() const { return g_signal_requested.load(); }

int SignalHandler::last_signal() const { return g_last_signal.load(); }

void SignalHandler::request_stop(int signum) noexcept {
  request_global_stop(signum);
}

void SignalHandler::handle(int signum) {
  if (
#if defined(SIGINT)
      signum == SIGINT
#else
      false
#endif
#if defined(SIGTERM)
      || signum == SIGTERM
#endif
  ) {
    request_global_stop(signum);
  }
}

const char* signal_name(int signum) noexcept {
#if defined(SIGINT)
  if (signum == SIGINT) {
    return "SIGINT";
  }
#endif
#if defined(SIGTERM)
  if (signum == SIGTERM) {
    return "SIGTERM";
  }
#endif
#if defined(_WIN32)
  if (signum == kWindowsCtrlBreak) {
    return "CTRL_BREAK";
  }
  if (signum == kWindowsCtrlClose) {
    return "CTRL_CLOSE";
  }
  if (signum == kWindowsCtrlLogoff) {
    return "CTRL_LOGOFF";
  }
  if (signum == kWindowsCtrlShutdown) {
    return "CTRL_SHUTDOWN";
  }
#endif
  return "unknown";
}

} // namespace holder::core
