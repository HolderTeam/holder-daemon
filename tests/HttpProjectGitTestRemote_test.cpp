#include "http_test_helpers.h"

#include "git/GitOps.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"

#include <filesystem>
#include <stdexcept>

namespace {

class ProbeGitOps final : public holder::git::GitOps {
public:
  std::filesystem::path repo_dir_;
  std::string last_remote_url;
  holder::git::RemoteProbeResult probe_result{
      .status = holder::git::RemoteProbeStatus::Reachable,
      .remote_has_head = false,
      .error_message = {},
  };

  void open_or_init(const std::filesystem::path& repo_dir) override {
    repo_dir_ = repo_dir;
    std::filesystem::create_directories(repo_dir_);
  }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string& url) override { last_remote_url = url; }
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override { return probe_result; }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    return {.status = holder::git::PushStatus::Pushed, .ahead_count = 0, .behind_count = 0, .error_message = {}};
  }
  std::filesystem::path repo_dir() const override { return repo_dir_; }
};

} // namespace

TEST_CASE("Project git test-remote returns remote_unset when no remote configured", "[git][http]") {
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

  ProbeGitOps git;
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

  const auto tested = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      boost::beast::http::verb::post,
                                                      "/projects/proj-1/git/test-remote",
                                                      nlohmann::json::object(),
                                                      boost::beast::http::status::ok);
  REQUIRE(tested["ok"] == true);
  REQUIRE(tested["data"]["status"] == "remote_unset");
  REQUIRE(tested["data"]["remote_url"].is_null());
  REQUIRE(tested["data"]["remote_has_head"] == false);
  REQUIRE(tested["data"]["error_code"] == "remote_unset");
  REQUIRE(tested["data"]["error_message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("Project git test-remote uses override remote and persists project remote", "[git][http]") {
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

  ProbeGitOps git;
  git.probe_result = {
      .status = holder::git::RemoteProbeStatus::NotFound,
      .remote_has_head = false,
      .error_message = "Repository not found",
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

  const std::string remote_url = "git@github.com:owner/repo.git";
  const auto tested = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      boost::beast::http::verb::post,
                                                      "/projects/proj-1/git/test-remote",
                                                      {{"remote_url", remote_url}, {"branch", "main"}},
                                                      boost::beast::http::status::ok);
  REQUIRE(tested["ok"] == true);
  REQUIRE(tested["data"]["status"] == "not_found");
  REQUIRE(tested["data"]["project_id"] == "proj-1");
  REQUIRE(tested["data"]["remote_url"] == remote_url);
  REQUIRE(tested["data"]["branch"] == "main");
  REQUIRE(tested["data"]["remote_has_head"] == false);
  REQUIRE(tested["data"]["error_code"] == "not_found");
  REQUIRE(tested["data"]["error_message"] == "Repository not found");
  REQUIRE(git.last_remote_url == remote_url);

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->git_remote_url.has_value());
  REQUIRE(fetched->git_remote_url.value() == remote_url);

  std::raise(SIGTERM);
  server_thread.join();
}
