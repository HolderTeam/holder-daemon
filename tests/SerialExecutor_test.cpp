#include "core/SerialExecutor.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

TEST_CASE("SerialExecutor blocks submit when pending queue is full", "[core][executor]") {
  holder::core::SerialExecutor executor("test-executor", 1);

  std::promise<void> release_first;
  std::shared_future<void> release_first_future(release_first.get_future());
  std::atomic<bool> first_started{false};
  std::atomic<bool> second_started{false};
  std::atomic<bool> third_ran{false};

  executor.submit([&]() {
    first_started.store(true);
    release_first_future.wait();
  });

  for (int i = 0; i < 50 && !first_started.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(first_started.load());

  executor.submit([&]() {
    second_started.store(true);
  });

  auto third_submit = std::async(std::launch::async, [&]() {
    executor.submit([&]() {
      third_ran.store(true);
    });
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(third_submit.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  REQUIRE_FALSE(second_started.load());
  REQUIRE_FALSE(third_ran.load());

  release_first.set_value();

  REQUIRE(third_submit.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);

  for (int i = 0; i < 50 && !third_ran.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(second_started.load());
  REQUIRE(third_ran.load());
}
