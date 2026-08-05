#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Signal.h"

#include <csignal>
#include <string>

TEST_CASE("SignalHandler sets flag on SIGTERM", "[signal]") {
  holder::core::SignalHandler handler;
  REQUIRE_FALSE(handler.is_requested());

  std::raise(SIGTERM);
  REQUIRE(handler.is_requested());
  REQUIRE(std::string(holder::core::signal_name(handler.last_signal())) == "SIGTERM");
}

TEST_CASE("SignalHandler records last signal", "[signal]") {
  holder::core::SignalHandler handler;

  std::raise(SIGINT);
  REQUIRE(handler.is_requested());
  REQUIRE(handler.last_signal() == SIGINT);
  REQUIRE(std::string(holder::core::signal_name(handler.last_signal())) == "SIGINT");
  REQUIRE(std::string(holder::core::signal_name(0)) == "unknown");
}
