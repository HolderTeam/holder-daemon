#pragma once

#include "platform/Signal.h"

#include <atomic>
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
    thread_.join();
  }

 private:
  holder::core::SignalHandler* signals_ = nullptr;
  std::thread thread_;
};

class StopFlagThreadGuard {
 public:
  StopFlagThreadGuard(std::atomic<bool>& stop_requested, std::thread thread) noexcept
      : stop_requested_(&stop_requested),
        thread_(std::move(thread)) {}

  ~StopFlagThreadGuard() noexcept { stop_and_join(); }

  StopFlagThreadGuard(const StopFlagThreadGuard&) = delete;
  StopFlagThreadGuard& operator=(const StopFlagThreadGuard&) = delete;

  void stop_and_join() noexcept {
    if (!thread_.joinable()) return;
    if (stop_requested_ != nullptr) {
      stop_requested_->store(true);
    }
    thread_.join();
  }

 private:
  std::atomic<bool>* stop_requested_ = nullptr;
  std::thread thread_;
};

} // namespace holder::app
