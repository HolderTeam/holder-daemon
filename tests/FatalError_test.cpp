#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "app/FatalError.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> observed_operations;
std::vector<std::string> observed_messages;

void reset_observations() {
  observed_operations.clear();
  observed_messages.clear();
}

void record_log(const char* message) {
  observed_operations.emplace_back("log");
  observed_messages.emplace_back(message);
}

void record_stderr(const char* message) {
  observed_operations.emplace_back("stderr");
  observed_messages.emplace_back(message);
}

void record_shutdown(const char* message) {
  observed_operations.emplace_back("shutdown");
  observed_messages.emplace_back(message);
}

void throw_log(const char*) {
  observed_operations.emplace_back("log");
  throw std::runtime_error("log failed");
}

void throw_stderr(const char*) {
  observed_operations.emplace_back("stderr");
  throw std::runtime_error("stderr failed");
}

void throw_shutdown(const char*) {
  observed_operations.emplace_back("shutdown");
  throw std::runtime_error("shutdown failed");
}

holder::app::FatalErrorOps recording_ops() {
  return holder::app::FatalErrorOps{
      record_log,
      record_stderr,
      record_shutdown,
  };
}

holder::app::FatalErrorOps throwing_ops() {
  return holder::app::FatalErrorOps{
      throw_log,
      throw_stderr,
      throw_shutdown,
  };
}

} // namespace

TEST_CASE("report_fatal_error_with_ops sends normalized message to all operations", "[fatal]") {
  reset_observations();

  holder::app::report_fatal_error_with_ops("startup failed", recording_ops());

  REQUIRE(observed_operations == std::vector<std::string>{"log", "stderr", "shutdown"});
  REQUIRE(
      observed_messages ==
      std::vector<std::string>{
          "startup failed",
          "startup failed",
          "startup failed",
      }
  );
}

TEST_CASE("report_fatal_error_with_ops normalizes missing messages", "[fatal]") {
  reset_observations();

  holder::app::report_fatal_error_with_ops(nullptr, recording_ops());

  REQUIRE(
      observed_messages ==
      std::vector<std::string>{
          "unknown fatal error",
          "unknown fatal error",
          "unknown fatal error",
      }
  );

  reset_observations();

  holder::app::report_fatal_error_with_ops("", recording_ops());

  REQUIRE(
      observed_messages ==
      std::vector<std::string>{
          "unknown fatal error",
          "unknown fatal error",
          "unknown fatal error",
      }
  );
}

TEST_CASE("report_fatal_error_with_ops swallows operation failures and continues", "[fatal]") {
  reset_observations();

  REQUIRE_NOTHROW(holder::app::report_fatal_error_with_ops("boom", throwing_ops()));

  REQUIRE(observed_operations == std::vector<std::string>{"log", "stderr", "shutdown"});
}

TEST_CASE("report_fatal_error uses the default reporting operations", "[fatal]") {
  auto previous_logger = spdlog::default_logger();
  std::ostringstream log_output;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log_output);
  auto logger = std::make_shared<spdlog::logger>("fatal-error-test", sink);
  spdlog::set_default_logger(logger);

  std::ostringstream stderr_output;
  auto* previous_stderr = std::cerr.rdbuf(stderr_output.rdbuf());

  holder::app::report_fatal_error("default failure");

  std::cerr.rdbuf(previous_stderr);
  spdlog::set_default_logger(previous_logger);

  REQUIRE(log_output.str().find("fatal error: default failure") != std::string::npos);
  REQUIRE(stderr_output.str() == "fatal error: default failure\n");
}
