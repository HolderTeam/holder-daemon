#pragma once

namespace holder::app {

struct FatalErrorOps {
  using Operation = void (*)(const char* message);

  Operation log_critical;
  Operation write_stderr;
  Operation shutdown_logger;
};

void report_fatal_error_with_ops(const char* message, FatalErrorOps ops) noexcept;

void report_fatal_error(const char* message) noexcept;

} // namespace holder::app
