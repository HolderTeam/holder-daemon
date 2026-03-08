#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/Signal.h"

#include <csignal>

TEST_CASE("SignalHandler sets flag on SIGTERM", "[signal]") {
  holder::core::SignalHandler handler;
  REQUIRE_FALSE(handler.is_requested());

  std::raise(SIGTERM);
  REQUIRE(handler.is_requested());
}

TEST_CASE("SignalHandler records last signal", "[signal]") {
  holder::core::SignalHandler handler;

  std::raise(SIGINT);
  REQUIRE(handler.is_requested());
  REQUIRE(handler.last_signal() == SIGINT);
}
