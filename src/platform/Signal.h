#pragma once

#include <atomic>

namespace holder::core {

class SignalHandler {
 public:
  SignalHandler();
  ~SignalHandler();

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

  bool is_requested() const;
  int last_signal() const;
  void request_stop(int signum = 0) noexcept;

 private:
  static void handle(int signum);

  bool installed_ = false;
};

const char* signal_name(int signum) noexcept;

} // namespace holder::core
