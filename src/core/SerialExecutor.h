#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace holder::core {

class SerialExecutor {
 public:
  explicit SerialExecutor(std::string name);
  ~SerialExecutor();

  SerialExecutor(const SerialExecutor&) = delete;
  SerialExecutor& operator=(const SerialExecutor&) = delete;

  void submit(std::function<void()> task) const;

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
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable std::deque<std::function<void()>> tasks_;
  bool stop_requested_ = false;
  std::thread worker_;
};

} // namespace holder::core
