#pragma once

#include "platform/Signal.h"

#include <thread>
#include <utility>

namespace holder::app {

class SyncThreadGuard {
 public:
  SyncThreadGuard(holder::core::SignalHandler& signals, std::thread thread) noexcept
      : signals_(&signals),
        thread_(std::move(thread)) {}

  ~SyncThreadGuard() noexcept { stop_and_join(); }

  SyncThreadGuard(const SyncThreadGuard&) = delete;
  SyncThreadGuard& operator=(const SyncThreadGuard&) = delete;

  void stop_and_join() noexcept {
    if (!thread_.joinable()) return;
    if (signals_ != nullptr) {
      signals_->request_stop();
    }
    try {
      thread_.join();
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
  }

 private:
  holder::core::SignalHandler* signals_ = nullptr;
  std::thread thread_;
};

} // namespace holder::app
