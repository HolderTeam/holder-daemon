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
  void pull_remote_ff_only(const std::string&) override {}
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

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(!fetched->git_remote_url.has_value());

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

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->git_remote_url.has_value());
  REQUIRE(fetched->git_remote_url.value() == "git@example.com:repo.git");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("Global recovery import keeps success when git remote setup fails", "[git][http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  holder::test::ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  holder::test::EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";

  // First server exports a token with remote hint.
  {
    holder::api::HttpServer export_server("127.0.0.1", 0, db, token, nullptr, nullptr);
    holder::api::HttpServer::BoundInfo bound;
    try {
      bound = export_server.start();
    } catch (const std::exception& ex) {
      SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
    }

    holder::core::SignalHandler signals;
    std::thread server_thread([&export_server, &signals]() { export_server.run(signals); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    holder::test::http_json_request(bound.bind,
                                    bound.port,
                                    token,
                                    boost::beast::http::verb::post,
                                    "/projects",
                                    {{"project_id", "proj-1"},
                                     {"name", "Project"},
                                     {"git_remote_url", "https://example.com/recovered.git"}},
                                    boost::beast::http::status::created);

    const auto exported = holder::test::http_json_request(bound.bind,
                                                          bound.port,
                                                          token,
                                                          boost::beast::http::verb::post,
                                                          "/projects/proj-1/recovery-token/export",
                                                          {{"pin", "1234"}},
                                                          boost::beast::http::status::ok);
    const std::string recovery_token = exported["data"]["recovery_token"].get<std::string>();

    holder::test::http_json_request(bound.bind,
                                    bound.port,
                                    token,
                                    boost::beast::http::verb::delete_,
                                    "/projects/proj-1",
                                    nlohmann::json::object(),
                                    boost::beast::http::status::ok);

    std::raise(SIGTERM);
    server_thread.join();

    // Second server forces set_remote failure during import.
    FailingGitOps git;
    git.fail_set_remote = true;
    holder::api::HttpServer import_server("127.0.0.1", 0, db, token, nullptr, nullptr, &git);
    holder::api::HttpServer::BoundInfo import_bound;
    try {
      import_bound = import_server.start();
    } catch (const std::exception& ex) {
      SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
    }

    holder::core::SignalHandler import_signals;
    std::thread import_thread([&import_server, &import_signals]() { import_server.run(import_signals); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto imported = holder::test::http_json_request(import_bound.bind,
                                                          import_bound.port,
                                                          token,
                                                          boost::beast::http::verb::post,
                                                          "/recovery-token/import",
                                                          {{"pin", "1234"}, {"recovery_token", recovery_token}},
                                                          boost::beast::http::status::created);
    REQUIRE(imported["ok"] == true);
    REQUIRE(imported["data"]["project_id"] == "proj-1");
    REQUIRE(imported["data"]["project_created"] == true);
    REQUIRE(imported["data"]["remote_hint_present"] == true);
    REQUIRE(imported["data"]["remote_configured"] == false);
    REQUIRE(imported["data"]["remote_error"].is_string());
    REQUIRE(imported["data"]["pull_status"] == "not_attempted");
    REQUIRE(imported["data"]["pull_error"].is_null());

    std::raise(SIGTERM);
    import_thread.join();
  }
}
