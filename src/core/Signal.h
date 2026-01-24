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

private:
  static void handle(int signum);

  bool installed_ = false;
};

} // namespace holder::core
