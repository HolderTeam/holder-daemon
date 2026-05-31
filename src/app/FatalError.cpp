#include "app/FatalError.h"

#include <spdlog/spdlog.h>

#include <iostream>

namespace holder::app {
namespace {

const char* fatal_error_text(const char* message) noexcept {
  return (message != nullptr && message[0] != '\0') ? message : "unknown fatal error";
}

void log_critical(const char* message) { spdlog::critical("fatal error: {}", message); }

void write_stderr(const char* message) { std::cerr << "fatal error: " << message << "\n"; }

void shutdown_logger(const char*) { spdlog::shutdown(); }

void ignore_fatal_report_failure() noexcept {}

void run_fatal_operation(FatalErrorOps::Operation operation, const char* message) noexcept {
  try {
    operation(message);
  } catch (...) {
    ignore_fatal_report_failure();
  }
}

} // namespace

void report_fatal_error_with_ops(const char* message, FatalErrorOps ops) noexcept {
  const char* text = fatal_error_text(message);
  run_fatal_operation(ops.log_critical, text);
  run_fatal_operation(ops.write_stderr, text);
  run_fatal_operation(ops.shutdown_logger, text);
}

void report_fatal_error(const char* message) noexcept {
  report_fatal_error_with_ops(
      message,
      FatalErrorOps{
          log_critical,
          write_stderr,
          shutdown_logger,
      }
  );
}

} // namespace holder::app
