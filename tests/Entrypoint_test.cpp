#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "app/Entrypoint.h"

#include <stdexcept>
#include <string>

namespace {

int observed_argc = 0;
char** observed_argv = nullptr;
std::string reported_message;

void reset_state() {
  observed_argc = 0;
  observed_argv = nullptr;
  reported_message.clear();
}

int successful_run(int argc, char* argv[]) {
  observed_argc = argc;
  observed_argv = argv;
  return 17;
}

int throwing_std_run(int, char*[]) { throw std::runtime_error("startup failed"); }

int throwing_unknown_run(int, char*[]) { throw 42; }

void record_fatal_error(const char* message) noexcept {
  reported_message = message == nullptr ? "" : message;
}

} // namespace

TEST_CASE("run_entrypoint returns daemon exit code", "[entrypoint]") {
  reset_state();
  char arg0[] = "holderd";
  char* argv[] = {arg0, nullptr};

  const int result = holder::app::run_entrypoint(1, argv, successful_run, record_fatal_error);

  REQUIRE(result == 17);
  REQUIRE(observed_argc == 1);
  REQUIRE(observed_argv == argv);
  REQUIRE(reported_message.empty());
}

TEST_CASE("run_entrypoint reports std exceptions", "[entrypoint]") {
  reset_state();

  const int result = holder::app::run_entrypoint(0, nullptr, throwing_std_run, record_fatal_error);

  REQUIRE(result == 1);
  REQUIRE(reported_message == "startup failed");
}

TEST_CASE("run_entrypoint reports unknown exceptions", "[entrypoint]") {
  reset_state();

  const int result =
      holder::app::run_entrypoint(0, nullptr, throwing_unknown_run, record_fatal_error);

  REQUIRE(result == 1);
  REQUIRE(reported_message == "unknown fatal error");
}
