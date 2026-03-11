#include "platform/Signal.h"

#include <csignal>

namespace holder::core {
namespace {

std::atomic<bool> g_signal_requested{false};
std::atomic<int> g_last_signal{0};
using HandlerType = void (*)(int);
HandlerType g_prev_int = nullptr;
HandlerType g_prev_term = nullptr;

} // namespace

SignalHandler::SignalHandler() {
  g_signal_requested.store(false);
  g_prev_int = std::signal(SIGINT, &SignalHandler::handle);
  g_prev_term = std::signal(SIGTERM, &SignalHandler::handle);
  installed_ = true;
}

SignalHandler::~SignalHandler() {
  if (!installed_) return;
  std::signal(SIGINT, g_prev_int);
  std::signal(SIGTERM, g_prev_term);
} // LCOV_EXCL_LINE

bool SignalHandler::is_requested() const {
  return g_signal_requested.load();
}

int SignalHandler::last_signal() const {
  return g_last_signal.load();
}

void SignalHandler::handle(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    g_signal_requested.store(true);
    g_last_signal.store(signum);
  }
}

} // namespace holder::core
