#include "http_test_helpers.h"

#include "git/GitOps.h"
#include "model/Project.h"
#include "store/ProjectRepo.h"

#include <filesystem>
#include <stdexcept>

namespace {

class FailingGitOps final : public holder::git::GitOps {
public:
  std::filesystem::path repo_dir_;
  bool fail_set_remote = false;
  bool fail_remove_remote = false;

  void open_or_init(const std::filesystem::path& repo_dir) override {
    repo_dir_ = repo_dir;
    std::filesystem::create_directories(repo_dir_);
  }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {
    if (fail_set_remote) throw std::runtime_error("set remote failed");
  }
  void remove_remote(const std::string&) override {
    if (fail_remove_remote) throw std::runtime_error("remove remote failed");
  }
  std::filesystem::path repo_dir() const override { return repo_dir_; }
};

} // namespace

TEST_CASE("Project patch propagates git remove_remote failure", "[git][http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.git_remote_url = std::string("git@example.com:repo.git");
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  FailingGitOps git;
  git.fail_remove_remote = true;

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

  nlohmann::json patch_body = {
      {"git_remote_url", nullptr},
      {"updated_at", 2}
  };

  const auto patch = holder::test::http_json_request(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::patch,
                                                     "/projects/proj-1",
                                                     patch_body,
                                                     boost::beast::http::status::bad_request);
  REQUIRE(patch["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("Project patch propagates git set_remote failure", "[git][http]") {
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

  FailingGitOps git;
  git.fail_set_remote = true;

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

  nlohmann::json patch_body = {
      {"git_remote_url", "git@example.com:repo.git"},
      {"updated_at", 2}
  };

  const auto patch = holder::test::http_json_request(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::patch,
                                                     "/projects/proj-1",
                                                     patch_body,
                                                     boost::beast::http::status::bad_request);
  REQUIRE(patch["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
