#include "core/SerialExecutor.h"

#include <spdlog/spdlog.h>

namespace holder::core {

SerialExecutor::SerialExecutor(std::string name, std::size_t max_pending_tasks)
    : name_(std::move(name)),
      max_pending_tasks_(max_pending_tasks),
      worker_([this]() { run(); }) {}

SerialExecutor::~SerialExecutor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  cv_.notify_all();
  space_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void SerialExecutor::submit(std::function<void()> task) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (max_pending_tasks_ > 0) {
    space_cv_.wait(lock, [this]() {
      return stop_requested_ || tasks_.size() < max_pending_tasks_;
    });
  }
  if (stop_requested_) {
    return;
  }
  tasks_.emplace_back(std::move(task));
  lock.unlock();
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
    space_cv_.notify_all();

    try {
      task();
    } catch (const std::exception& ex) {
      spdlog::error("serial executor {} task failed: {}", name_, ex.what());
      throw;
    }
  }
}

} // namespace holder::core
