#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/SerialExecutor.h"
#include "llm/ExecutorRunnerClient.h"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace {

class RecordingRunnerClient final : public holder::llm::RunnerClient {
public:
  std::atomic<int> background_probe_calls{0};
  std::atomic<int> status_calls{0};
  std::atomic<int> retry_calls{0};
  std::atomic<int> start_pull_calls{0};
  std::atomic<int> get_pull_calls{0};
  std::atomic<int> list_pulls_calls{0};
  std::atomic<int> stream_generate_calls{0};

  holder::llm::RunnerStatus status_result{};
  holder::llm::RunnerStatus retry_result{};
  holder::llm::RunnerPullJob pull_job{};
  std::optional<holder::llm::RunnerPullJob> get_pull_result{};
  std::vector<holder::llm::RunnerPullJob> list_pulls_result{};
  bool stream_generate_result = true;

  void start_background_probe() override { background_probe_calls.fetch_add(1); }
  holder::llm::RunnerStatus status() const override {
    const_cast<RecordingRunnerClient*>(this)->status_calls.fetch_add(1);
    return status_result;
  }
  holder::llm::RunnerStatus retry() override {
    retry_calls.fetch_add(1);
    return retry_result;
  }
  holder::llm::RunnerPullJob start_pull(const std::string&) override {
    start_pull_calls.fetch_add(1);
    return pull_job;
  }
  std::optional<holder::llm::RunnerPullJob> get_pull(const std::string&) const override {
    const_cast<RecordingRunnerClient*>(this)->get_pull_calls.fetch_add(1);
    return get_pull_result;
  }
  std::vector<holder::llm::RunnerPullJob> list_pulls() const override {
    const_cast<RecordingRunnerClient*>(this)->list_pulls_calls.fetch_add(1);
    return list_pulls_result;
  }
  bool stream_generate(const std::string&,
                       const std::string&,
                       const std::string&,
                       const std::function<void(const std::string&)>& on_chunk,
                       std::string* error) override {
    stream_generate_calls.fetch_add(1);
    on_chunk("chunk");
    if (error != nullptr) {
      *error = "none";
    }
    return stream_generate_result;
  }
};

} // namespace

TEST_CASE("ExecutorRunnerClient delegates retry and pull operations through SerialExecutor",
          "[llm][executor]") {
  holder::core::SerialExecutor executor("runner-executor-test");
  RecordingRunnerClient inner;

  holder::llm::RunnerStatus status;
  status.available = true;
  status.version = "0.1";
  inner.status_result = status;
  inner.retry_result = status;

  holder::llm::RunnerPullJob job;
  job.job_id = "job-1";
  job.model = "model-a";
  job.status = "queued";
  inner.pull_job = job;
  inner.get_pull_result = job;
  inner.list_pulls_result = {job};

  holder::llm::ExecutorRunnerClient client(inner, executor);

  client.start_background_probe();
  for (int i = 0; i < 50 && inner.background_probe_calls.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(inner.background_probe_calls.load() == 1);

  const auto current = client.status();
  REQUIRE(inner.status_calls.load() == 1);
  REQUIRE(current.available);
  REQUIRE(current.version == "0.1");

  const auto retried = client.retry();
  REQUIRE(inner.retry_calls.load() == 1);
  REQUIRE(retried.available);
  REQUIRE(retried.version == "0.1");

  const auto started = client.start_pull("model-a");
  REQUIRE(inner.start_pull_calls.load() == 1);
  REQUIRE(started.job_id == "job-1");

  const auto fetched = client.get_pull("job-1");
  REQUIRE(inner.get_pull_calls.load() == 1);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->job_id == "job-1");

  const auto pulls = client.list_pulls();
  REQUIRE(inner.list_pulls_calls.load() == 1);
  REQUIRE(pulls.size() == 1);
  REQUIRE(pulls.front().job_id == "job-1");

  std::string error;
  std::string chunk;
  const bool ok = client.stream_generate(
      "model-a", "prompt", "{}", [&](const std::string& piece) { chunk += piece; }, &error);
  REQUIRE(inner.stream_generate_calls.load() == 1);
  REQUIRE(ok);
  REQUIRE(chunk == "chunk");
  REQUIRE(error == "none");
}
