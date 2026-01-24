#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/LockFile.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_lock_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

int run_helper(const std::filesystem::path& helper,
               const std::filesystem::path& lock_path,
               unsigned long hold_ms = 0) {
  std::string cmd;
  cmd.reserve(helper.string().size() + lock_path.string().size() + 16);
  cmd += "\"";
  cmd += helper.string();
  cmd += "\" \"";
  cmd += lock_path.string();
  cmd += "\"";
  if (hold_ms > 0) {
    cmd += " \"";
    cmd += std::to_string(hold_ms);
    cmd += "\"";
  }

  const int rc = std::system(cmd.c_str());
#ifdef _WIN32
  return rc;
#else
  if (rc == -1) return rc;
  return WEXITSTATUS(rc);
#endif
}

} // namespace

TEST_CASE("LockFile acquires and releases", "[lock]") {
  const auto dir = make_temp_dir();
  const auto lock_path = dir / "holder.lock";

  holder::core::LockFile lock(lock_path);
  REQUIRE(lock.try_acquire());
  REQUIRE(lock.is_locked());

  lock.release();
  REQUIRE_FALSE(lock.is_locked());

  holder::core::LockFile lock2(lock_path);
  REQUIRE(lock2.try_acquire());
  lock2.release();
}

TEST_CASE("LockFile blocks other process while held", "[lock]") {
  const auto dir = make_temp_dir();
  const auto lock_path = dir / "holder.lock";

  holder::core::LockFile lock(lock_path);
  REQUIRE(lock.try_acquire());

  const auto helper = std::filesystem::path(LOCK_HELPER_PATH);
  const int rc = run_helper(helper, lock_path);
  REQUIRE(rc == 1);

  lock.release();

  const int rc_after = run_helper(helper, lock_path);
  REQUIRE(rc_after == 0);
}

TEST_CASE("LockFile allows locking when file exists but no lock", "[lock]") {
  const auto dir = make_temp_dir();
  const auto lock_path = dir / "holder.lock";

  {
    std::ofstream out(lock_path);
    out << "stale\n";
  }

  holder::core::LockFile lock(lock_path);
  REQUIRE(lock.try_acquire());
  lock.release();
}

TEST_CASE("LockFile rejects parallel attempts while held", "[lock]") {
  const auto dir = make_temp_dir();
  const auto lock_path = dir / "holder.lock";

  holder::core::LockFile lock(lock_path);
  REQUIRE(lock.try_acquire());

  const auto helper = std::filesystem::path(LOCK_HELPER_PATH);
  const int attempts = 6;
  std::vector<int> results(attempts, -1);
  std::vector<std::thread> threads;
  threads.reserve(attempts);

  for (int i = 0; i < attempts; ++i) {
    threads.emplace_back([&results, i, &helper, &lock_path]() {
      results[i] = run_helper(helper, lock_path);
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  for (int rc : results) {
    REQUIRE(rc == 1);
  }

  lock.release();
}
