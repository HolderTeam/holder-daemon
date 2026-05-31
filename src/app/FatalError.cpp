#include "app/FatalError.h"

#include <spdlog/spdlog.h>

#include <iostream>

namespace holder::app {

void report_fatal_error(const char* message) noexcept {
  const char* text = (message != nullptr && message[0] != '\0') ? message : "unknown fatal error";
  try {
    spdlog::critical("fatal error: {}", text);
  } catch (...) { // NOLINT(bugprone-empty-catch) LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
  try {
    std::cerr << "fatal error: " << text << "\n";
  } catch (...) { // NOLINT(bugprone-empty-catch) LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
  try {
    spdlog::shutdown();
  } catch (...) { // NOLINT(bugprone-empty-catch) LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
}

} // namespace holder::app
