#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "llm/LocalModelRunner.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

struct HttpResponseSpec {
  int status = 200;
  std::string body;
  std::string content_type = "application/json";
  int delay_ms = 0;
};

uint16_t reserve_loopback_port() {
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  return acceptor.local_endpoint().port();
}

class StubHttpServer {
public:
  StubHttpServer(uint16_t port, std::vector<HttpResponseSpec> responses)
      : port_(port), responses_(std::move(responses)) {}

  void start() {
    thread_ = std::thread([this]() {
      namespace http = boost::beast::http;
      using tcp = boost::asio::ip::tcp;

      try {
        boost::asio::io_context ioc;
        tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), port_});
        {
          std::lock_guard<std::mutex> lock(mu_);
          ready_ = true;
        }
        cv_.notify_one();
        for (const auto& spec : responses_) {
          tcp::socket socket(ioc);
          acceptor.accept(socket);

          boost::beast::flat_buffer buffer;
          http::request<http::string_body> req;
          http::read(socket, buffer, req);

          if (spec.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(spec.delay_ms));
          }

          auto status = static_cast<http::status>(spec.status);
          http::response<http::string_body> res(status, 11);
          res.set(http::field::server, "stub");
          res.set(http::field::content_type, spec.content_type);
          res.keep_alive(false);
          res.body() = spec.body;
          res.prepare_payload();
          http::write(socket, res);

          boost::system::error_code ec;
          socket.shutdown(tcp::socket::shutdown_both, ec);
        }
      } catch (...) {
        // Tests assert behavior through client results; server thread should not crash test process.
        {
          std::lock_guard<std::mutex> lock(mu_);
          ready_ = true;
        }
        cv_.notify_one();
      }
    });
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&]() { return ready_; });
  }

  ~StubHttpServer() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  uint16_t port_;
  std::vector<HttpResponseSpec> responses_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool ready_ = false;
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
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "/definitely/not-found-ollama");

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
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "/definitely/not-found-ollama");

  holder::llm::LocalModelRunner runner;
  const auto status = runner.retry();
  REQUIRE(status.last_checked > 0);
}

TEST_CASE("LocalModelRunner retry non-fake reads version and tags from HTTP", "[llm]") {
  const uint16_t port = reserve_loopback_port();
  StubHttpServer server(
      port,
      {
          HttpResponseSpec{200, R"({"version":"test-v1"})"},
          HttpResponseSpec{200, R"({"models":[{"name":"m1","digest":"d1","size":42,"modified_at":"now"}]})"},
      });
  server.start();

  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "0");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", std::to_string(port));
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "");

  holder::llm::LocalModelRunner runner;
  const auto status = runner.retry();
  REQUIRE(status.available == true);
  REQUIRE(status.version == "test-v1");
  REQUIRE(status.models.size() == 1);
  REQUIRE(status.models[0].name == "m1");
  REQUIRE(status.models[0].digest == "d1");
  REQUIRE(status.models[0].size == 42);
}

TEST_CASE("LocalModelRunner stream_generate parses streamed response lines", "[llm]") {
  const uint16_t port = reserve_loopback_port();
  StubHttpServer server(
      port,
      {
          HttpResponseSpec{200,
                           "  \n"
                           "not-json\n"
                           "{\"response\":\"hi\"}\n"
                           "{\"response\":\" there\",\"done\":true}\n"},
      });
  server.start();

  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "0");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", std::to_string(port));
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "");

  holder::llm::LocalModelRunner runner;
  std::string out;
  std::string err;
  const bool ok = runner.stream_generate(
      "model-a", "prompt", "", [&](const std::string& chunk) { out += chunk; }, &err);
  REQUIRE(ok);
  REQUIRE(err.empty());
  REQUIRE(out == "hi there");
}

TEST_CASE("LocalModelRunner start_pull non-fake marks job failed on HTTP error", "[llm]") {
  const uint16_t port = reserve_loopback_port();
  StubHttpServer server(port, {HttpResponseSpec{500, R"({"error":"nope"})"}});
  server.start();

  EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "0");
  EnvGuard host_env("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1");
  EnvGuard port_env("HOLDER_MODEL_RUNNER_PORT", std::to_string(port));
  EnvGuard bin_env("HOLDER_MODEL_RUNNER_BIN", "");

  holder::llm::LocalModelRunner runner;
  auto job = runner.start_pull("model-b");
  REQUIRE_FALSE(job.job_id.empty());

  bool failed = false;
  for (int i = 0; i < 100; ++i) {
    auto fetched = runner.get_pull(job.job_id);
    REQUIRE(fetched.has_value());
    if (fetched->status == "failed") {
      failed = true;
      REQUIRE_FALSE(fetched->error.empty());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(failed);
}

TEST_CASE("LocalModelRunner start_pull returns existing queued job for same model", "[llm]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);

  auto first = runner.start_pull("same-model");
  auto second = runner.start_pull("same-model");
  REQUIRE_FALSE(first.job_id.empty());
  REQUIRE(second.job_id == first.job_id);
}
