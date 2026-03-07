#include "http_test_helpers.h"

#include "git/GitOps.h"
#include "model/Project.h"
#include "store/ProjectRepo.h"

#include <filesystem>

namespace {

class PushGitOps final : public holder::git::GitOps {
public:
  std::filesystem::path repo_dir_;
  holder::git::PushResult push_result{
      .status = holder::git::PushStatus::Pushed,
      .ahead_count = 0,
      .behind_count = 0,
      .error_message = {},
  };
  std::string pushed_branch;
  bool pushed_set_upstream = false;

  void open_or_init(const std::filesystem::path& repo_dir) override {
    repo_dir_ = repo_dir;
    std::filesystem::create_directories(repo_dir_);
  }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {}
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override {
    return {.status = holder::git::RemoteProbeStatus::Reachable, .remote_has_head = true, .error_message = {}};
  }
  holder::git::PushResult push_branch(const std::string&,
                                      const std::string& branch,
                                      bool set_upstream) override {
    pushed_branch = branch;
    pushed_set_upstream = set_upstream;
    return push_result;
  }
  std::filesystem::path repo_dir() const override { return repo_dir_; }
};

} // namespace

TEST_CASE("Project git push returns remote_unset when no remote configured", "[git][http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  PushGitOps git;
  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr, &git);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto pushed = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      boost::beast::http::verb::post,
                                                      "/projects/proj-1/git/push",
                                                      nlohmann::json::object(),
                                                      boost::beast::http::status::ok);
  REQUIRE(pushed["ok"] == true);
  REQUIRE(pushed["data"]["status"] == "remote_unset");
  REQUIRE(pushed["data"]["error_code"] == "remote_unset");
  REQUIRE(pushed["data"]["next_action"].is_null());

  const auto sync = holder::test::http_json_request(bound.bind,
                                                    bound.port,
                                                    token,
                                                    boost::beast::http::verb::get,
                                                    "/projects/proj-1/git/sync-status",
                                                    nlohmann::json::object(),
                                                    boost::beast::http::status::ok);
  REQUIRE(sync["ok"] == true);
  REQUIRE(sync["data"]["project_id"] == "proj-1");
  REQUIRE(sync["data"]["sync"]["last_push_status"] == "remote_unset");
  REQUIRE(sync["data"]["sync"]["retry_count"] == 1);
  REQUIRE(sync["data"]["sync"]["next_retry_at"].is_number_integer());
  REQUIRE(sync["data"]["sync"]["pull_retry_count"] == 0);
  REQUIRE(sync["data"]["sync"]["next_pull_retry_at"].is_null());
  REQUIRE(sync["data"]["sync"]["last_sync_error"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("Project git push returns structured non_fast_forward result", "[git][http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.git_remote_url = std::string("git@github.com:owner/repo.git");
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  PushGitOps git;
  git.push_result = {
      .status = holder::git::PushStatus::NonFastForward,
      .ahead_count = 2,
      .behind_count = 1,
      .error_message = "non-fast-forward",
  };

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr, &git);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto pushed = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      boost::beast::http::verb::post,
                                                      "/projects/proj-1/git/push",
                                                      {{"branch", "main"}, {"set_upstream", false}},
                                                      boost::beast::http::status::ok);
  REQUIRE(pushed["ok"] == true);
  REQUIRE(pushed["data"]["status"] == "non_fast_forward");
  REQUIRE(pushed["data"]["error_code"] == "non_fast_forward");
  REQUIRE(pushed["data"]["next_action"] == "pull_then_retry");
  REQUIRE(pushed["data"]["ahead_count"] == 2);
  REQUIRE(pushed["data"]["behind_count"] == 1);
  REQUIRE(git.pushed_branch == "main");
  REQUIRE(git.pushed_set_upstream == false);

  const auto sync = holder::test::http_json_request(bound.bind,
                                                    bound.port,
                                                    token,
                                                    boost::beast::http::verb::get,
                                                    "/projects/proj-1/git/sync-status",
                                                    nlohmann::json::object(),
                                                    boost::beast::http::status::ok);
  REQUIRE(sync["ok"] == true);
  REQUIRE(sync["data"]["sync"]["last_push_status"] == "non_fast_forward");
  REQUIRE(sync["data"]["sync"]["retry_count"] == 1);
  REQUIRE(sync["data"]["sync"]["next_retry_at"].is_number_integer());
  REQUIRE(sync["data"]["sync"]["pull_retry_count"] == 0);
  REQUIRE(sync["data"]["sync"]["next_pull_retry_at"].is_null());
  REQUIRE(sync["data"]["sync"]["last_sync_error"] == "non-fast-forward");

  std::raise(SIGTERM);
  server_thread.join();
}
