#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/SerialExecutor.h"
#include "git/ExecutorGitOps.h"

#include <filesystem>
#include <string>

namespace {

class RecordingGitOps final : public holder::git::GitOps {
 public:
  std::filesystem::path opened_repo;
  std::filesystem::path last_path;
  std::string last_name;
  std::string last_url;
  std::string last_message;
  std::string last_branch;
  bool last_set_upstream = false;

  holder::git::RemoteProbeResult probe_result{
      .status = holder::git::RemoteProbeStatus::Reachable,
      .remote_has_head = true,
      .error_message = "",
  };
  holder::git::PushResult push_result{
      .status = holder::git::PushStatus::Pushed,
      .ahead_count = 1,
      .behind_count = 0,
      .local_head_commit = "abc123",
      .error_message = "",
  };

  void open_or_init(const std::filesystem::path& repo_dir) override { opened_repo = repo_dir; }
  void write_file(const std::filesystem::path& relative_path, const std::string&) override {
    last_path = relative_path;
  }
  void stage_path(const std::filesystem::path& relative_path) override {
    last_path = relative_path;
  }
  void remove_path(const std::filesystem::path& relative_path) override {
    last_path = relative_path;
  }
  void commit(const std::string& message) override { last_message = message; }
  void set_remote(const std::string& name, const std::string& url) override {
    last_name = name;
    last_url = url;
  }
  void remove_remote(const std::string& name) override { last_name = name; }
  void pull_remote_ff_only(const std::string& name) override { last_name = name; }
  holder::git::RemoteProbeResult probe_remote(const std::string& name) override {
    last_name = name;
    return probe_result;
  }
  holder::git::PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) override {
    last_name = name;
    last_branch = branch;
    last_set_upstream = set_upstream;
    return push_result;
  }
  std::filesystem::path repo_dir() const override { return opened_repo; }
};

} // namespace

TEST_CASE("ExecutorGitOps delegates all operations through SerialExecutor", "[git][executor]") {
  holder::core::SerialExecutor executor("git-executor-test");
  RecordingGitOps inner;
  holder::git::ExecutorGitOps git(inner, executor);

  git.open_or_init("/tmp/repo");
  REQUIRE(inner.opened_repo == std::filesystem::path("/tmp/repo"));

  git.write_file("cards/x.md", "hello");
  REQUIRE(inner.last_path == std::filesystem::path("cards/x.md"));

  git.stage_path("cards/x.md");
  REQUIRE(inner.last_path == std::filesystem::path("cards/x.md"));

  git.remove_path("cards/x.md");
  REQUIRE(inner.last_path == std::filesystem::path("cards/x.md"));

  git.commit("commit msg");
  REQUIRE(inner.last_message == "commit msg");

  git.set_remote("origin", "https://example.com/repo.git");
  REQUIRE(inner.last_name == "origin");
  REQUIRE(inner.last_url == "https://example.com/repo.git");

  git.remove_remote("origin");
  REQUIRE(inner.last_name == "origin");

  git.pull_remote_ff_only("origin");
  REQUIRE(inner.last_name == "origin");

  const auto probe = git.probe_remote("origin");
  REQUIRE(inner.last_name == "origin");
  REQUIRE(probe.status == holder::git::RemoteProbeStatus::Reachable);
  REQUIRE(probe.remote_has_head == true);

  const auto push = git.push_branch("origin", "cards", true);
  REQUIRE(inner.last_name == "origin");
  REQUIRE(inner.last_branch == "cards");
  REQUIRE(inner.last_set_upstream == true);
  REQUIRE(push.status == holder::git::PushStatus::Pushed);

  REQUIRE(git.repo_dir() == std::filesystem::path("/tmp/repo"));
}
