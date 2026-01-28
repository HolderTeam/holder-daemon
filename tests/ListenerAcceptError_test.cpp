#include "api/Listener.h"
#include "api/Router.h"
#include "core/Signal.h"
#include "store/Db.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <chrono>
#include <filesystem>
#include <thread>

TEST_CASE("Listener accept error branch is exercised", "[listener]") {
  holder::store::Db db;
  db.open(std::filesystem::temp_directory_path() / "holder_listener_error.db");

  holder::api::Router router;
  holder::api::Listener listener("127.0.0.1",
                                 0,
                                 db,
                                 "token",
                                 router,
                                 std::chrono::steady_clock::now(),
                                 nullptr,
                                 nullptr);
  const auto bound = listener.start();
  REQUIRE(bound.port > 0);

  holder::core::SignalHandler signals;
  std::thread t([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  listener.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::raise(SIGTERM);
  t.join();
}
