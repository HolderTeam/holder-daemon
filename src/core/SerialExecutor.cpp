#include "core/SerialExecutor.h"

#include <spdlog/spdlog.h>

namespace holder::core {

SerialExecutor::SerialExecutor(std::string name)
    : name_(std::move(name)),
      worker_([this]() { run(); }) {}

SerialExecutor::~SerialExecutor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void SerialExecutor::submit(std::function<void()> task) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.emplace_back(std::move(task));
  }
  cv_.notify_one();
}

void SerialExecutor::run() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stop_requested_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        if (stop_requested_) {
          return;
        }
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }

    try {
      task();
    } catch (const std::exception& ex) {
      spdlog::error("serial executor {} task failed: {}", name_, ex.what());
      throw;
    }
  }
}

} // namespace holder::core
