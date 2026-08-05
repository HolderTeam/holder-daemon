#include "platform/Signal.h"

#include <csignal>

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

} // namespace

SignalHandler::SignalHandler() {
  g_signal_requested.store(false);
#if defined(SIGINT)
  g_prev_int = std::signal(SIGINT, &SignalHandler::handle);
#endif
#if defined(SIGTERM)
  g_prev_term = std::signal(SIGTERM, &SignalHandler::handle);
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
} // LCOV_EXCL_LINE

bool SignalHandler::is_requested() const { return g_signal_requested.load(); }

int SignalHandler::last_signal() const { return g_last_signal.load(); }

void SignalHandler::request_stop(int signum) noexcept {
  g_signal_requested.store(true);
  if (signum != 0) {
    g_last_signal.store(signum);
  }
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
    g_signal_requested.store(true);
    g_last_signal.store(signum);
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
  return "unknown";
}

} // namespace holder::core
