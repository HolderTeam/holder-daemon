#pragma once

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace holder::core {

class SerialExecutor {
 public:
  explicit SerialExecutor(std::string name, std::size_t max_pending_tasks = 0);
  ~SerialExecutor();

  SerialExecutor(const SerialExecutor&) = delete;
  SerialExecutor& operator=(const SerialExecutor&) = delete;

  void submit(std::function<void()> task) const;
  void stop();

  template <typename Fn>
  auto call(Fn&& fn) const -> decltype(fn()) {
    using Result = decltype(fn());

    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    submit([task]() mutable { (*task)(); });

    if constexpr (std::is_void_v<Result>) {
      future.get();
    } else {
      return future.get();
    }
  }

 private:
  void run();

  std::string name_;
  std::size_t max_pending_tasks_ = 0;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable std::condition_variable space_cv_;
  mutable std::deque<std::function<void()>> tasks_;
  bool stop_requested_ = false;
  std::thread worker_;
};

} // namespace holder::core
