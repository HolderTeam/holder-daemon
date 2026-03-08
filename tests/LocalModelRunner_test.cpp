#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "llm/LocalModelRunner.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

class EnvGuard {
public:
  EnvGuard(const char* key, const std::string& value) : key_(key) {
    const char* current = std::getenv(key_);
    if (current != nullptr) {
      had_old_ = true;
      old_ = current;
    }
    setenv(key_, value.c_str(), 1);
  }

  ~EnvGuard() {
    if (had_old_) {
      setenv(key_, old_.c_str(), 1);
    } else {
      unsetenv(key_);
    }
  }

private:
  const char* key_;
  bool had_old_ = false;
  std::string old_;
};

} // namespace

TEST_CASE("LocalModelRunner fake mode probe populates status", "[llm]") {
  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "1");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "192.0.2.10");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", "9999");
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "/tmp/does-not-matter");

  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  const auto status = runner.status();
  REQUIRE(status.available == true);
  REQUIRE(status.spawn_attempted == false);
  REQUIRE(status.version == "fake");
  REQUIRE(status.models.size() == 1);
  REQUIRE(status.models[0].name == "fake-echo");
  REQUIRE(status.models[0].size == 1);
}

TEST_CASE("LocalModelRunner retry and stream_generate fake-mode branches", "[llm]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);

  const auto status = runner.retry();
  REQUIRE(status.available == true);
  REQUIRE(status.version == "fake");
  REQUIRE(status.models.size() == 1);

  std::string out;
  std::string err;
  REQUIRE(runner.stream_generate("fake-echo", "hello", "", [&](const std::string& chunk) { out += chunk; }, &err));
  REQUIRE(out == "hello");
  REQUIRE(err.empty());

  out.clear();
  err.clear();
  REQUIRE_FALSE(runner.stream_generate("", "hello", "", [&](const std::string& chunk) { out += chunk; }, &err));
  REQUIRE(out.empty());
  REQUIRE(err == "missing model");
}

TEST_CASE("LocalModelRunner pull job lifecycle in fake mode", "[llm]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);

  auto missing = runner.start_pull("");
  REQUIRE(missing.status == "failed");
  REQUIRE(missing.error == "missing model");
  REQUIRE(missing.job_id.empty());

  auto job = runner.start_pull("llama3");
  REQUIRE_FALSE(job.job_id.empty());
  REQUIRE(job.model == "llama3");

  const auto listed_initial = runner.list_pulls();
  REQUIRE_FALSE(listed_initial.empty());

  // Fake mode completes asynchronously; poll briefly for completion.
  bool completed = false;
  for (int i = 0; i < 50; ++i) {
    auto fetched = runner.get_pull(job.job_id);
    REQUIRE(fetched.has_value());
    if (fetched->status == "completed") {
      completed = true;
      REQUIRE(fetched->progress.stage == "success");
      REQUIRE(fetched->progress.total == 1);
      REQUIRE(fetched->progress.completed == 1);
      REQUIRE(fetched->progress.percent == 100.0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(completed);

  REQUIRE_FALSE(runner.get_pull("missing-job-id").has_value());
}

TEST_CASE("LocalModelRunner stop without process is a no-op", "[llm]") {
  holder::llm::LocalModelRunner runner;
  REQUIRE_NOTHROW(runner.stop());
}

TEST_CASE("LocalModelRunner non-fake background probe runs once and sets status", "[llm]") {
  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "0");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", "9");
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "");

  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  runner.start_background_probe();

  holder::llm::RunnerStatus status;
  bool seen_check = false;
  for (int i = 0; i < 100; ++i) {
    status = runner.status();
    if (status.last_checked > 0) {
      seen_check = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  REQUIRE(seen_check);
  REQUIRE((status.available || !status.error.empty() || status.spawn_attempted));
}

TEST_CASE("LocalModelRunner retry non-fake returns status instead of throwing", "[llm]") {
  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "0");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", "9");
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "");

  holder::llm::LocalModelRunner runner;
  const auto status = runner.retry();
  REQUIRE(status.last_checked > 0);
}
