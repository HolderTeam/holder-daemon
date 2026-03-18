#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ProjectRoutes.h"
#include "http_test_helpers.h"
#include "git/GitOps.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method,
                                              const std::string& target,
                                              const nlohmann::json& body = nlohmann::json()) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.is_null() && !body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();
  }
  return req;
}

class ProjectRoutesTestGitOps final : public holder::git::GitOps {
public:
  holder::git::RemoteProbeResult probe_result{
      .status = holder::git::RemoteProbeStatus::Reachable,
      .remote_has_head = true,
      .error_message = {},
  };
  holder::git::PushResult push_result{
      .status = holder::git::PushStatus::Pushed,
      .ahead_count = 0,
      .behind_count = 0,
      .error_message = {},
  };
  bool throw_on_set_remote = false;
  bool throw_on_pull = false;

  void open_or_init(const std::filesystem::path&) override {}
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {
    if (throw_on_set_remote) {
      throw std::runtime_error("set_remote failed");
    }
  }
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {
    if (throw_on_pull) {
      throw std::runtime_error("pull failed");
    }
  }
  holder::git::RemoteProbeResult probe_remote(const std::string&) override { return probe_result; }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override { return push_result; }
  std::filesystem::path repo_dir() const override { return {}; }
};

} // namespace

TEST_CASE("ProjectRoutes returns false when path does not match", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/not-projects");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/not-projects", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE_FALSE(handled);
}

TEST_CASE("ProjectRoutes global recovery import validates required fields", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::post, "/recovery-token/import", nlohmann::json::object());
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/recovery-token/import", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("ProjectRoutes rejects empty project id segment", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/projects/");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/projects/", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::not_found);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "not_found");
}

TEST_CASE("ProjectRoutes covers additional uncovered branches", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Proj One";
  project.root_path = (dir / "repo1").string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto empty_param_get = [](const std::string&) { return std::string(); };
  ProjectRoutesTestGitOps git;

  auto call = [&](http::verb method,
                  const std::string& path,
                  const nlohmann::json& body = nlohmann::json()) {
    auto req = make_request(method, path, body);
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        path, req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  SECTION("global recovery import missing fields path") {
    auto [status, payload] = call(http::verb::post, "/recovery-token/import", {{"x", 1}});
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("projects list internal error catch with closed db") {
    db.close();
    auto [status, payload] = call(http::verb::get, "/projects");
    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(payload["error"]["code"] == "error");
  }
}

TEST_CASE("ProjectRoutes git and project route error/status branches", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Proj One";
  project.root_path = (dir / "repo1").string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  project.git_remote_url = std::string("git@example.com:test/repo.git");
  repo.create(project);

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto empty_param_get = [](const std::string&) { return std::string(); };
  ProjectRoutesTestGitOps git;

  auto call = [&](http::verb method,
                  const std::string& path,
                  const nlohmann::json& body = nlohmann::json()) {
    auto req = make_request(method, path, body);
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        path, req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  SECTION("git test-remote emits status_name and null remote override") {
    git.probe_result = {
        .status = holder::git::RemoteProbeStatus::AuthFailed,
        .remote_has_head = false,
        .error_message = "auth",
    };
    auto [status, payload] = call(http::verb::post,
                                  "/projects/proj-1/git/test-remote",
                                  {{"remote_url", nullptr}, {"branch", "main"}});
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["status"] == "remote_unset");
    REQUIRE(payload["data"]["error_code"] == "remote_unset");
    REQUIRE(payload["data"]["remote_has_head"] == false);
  }

  SECTION("git test-remote emits reachable status with null error_code") {
    git.probe_result = {
        .status = holder::git::RemoteProbeStatus::Reachable,
        .remote_has_head = true,
        .error_message = {},
    };
    auto [status, payload] = call(http::verb::post,
                                  "/projects/proj-1/git/test-remote",
                                  {{"branch", "main"}});
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["status"] == "reachable");
    REQUIRE(payload["data"]["error_code"].is_null());
    REQUIRE(payload["data"]["remote_has_head"] == true);
  }

  SECTION("git test-remote bad json catch") {
    auto req = make_request(http::verb::post, "/projects/proj-1/git/test-remote");
    req.body() = "{";
    req.prepare_payload();
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        "/projects/proj-1/git/test-remote", req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("git push emits push status name") {
    git.push_result = {
        .status = holder::git::PushStatus::AuthFailed,
        .ahead_count = 0,
        .behind_count = 0,
        .error_message = "auth failed",
    };
    auto [status, payload] = call(http::verb::post, "/projects/proj-1/git/push", nlohmann::json::object());
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["status"] == "auth_failed");
    REQUIRE(payload["data"]["error_code"] == "auth_failed");
  }

  SECTION("git push serializes empty local_head_commit as null") {
    git.push_result = {
        .status = holder::git::PushStatus::Pushed,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = "",
        .error_message = {},
    };
    auto [status, payload] = call(http::verb::post, "/projects/proj-1/git/push", nlohmann::json::object());
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["status"] == "pushed");
    REQUIRE(payload["data"]["local_head_commit"].is_null());
  }

  SECTION("git push serializes non-empty local_head_commit") {
    git.push_result = {
        .status = holder::git::PushStatus::Pushed,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = "abc123def456",
        .error_message = {},
    };
    auto [status, payload] = call(http::verb::post, "/projects/proj-1/git/push", nlohmann::json::object());
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["status"] == "pushed");
    REQUIRE(payload["data"]["local_head_commit"] == "abc123def456");
  }

  SECTION("git push bad json catch") {
    auto req = make_request(http::verb::post, "/projects/proj-1/git/push");
    req.body() = "{";
    req.prepare_payload();
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        "/projects/proj-1/git/push", req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("sync-status bad request catch with closed db") {
    db.close();
    auto [status, payload] = call(http::verb::get, "/projects/proj-1/git/sync-status");
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("sync-status returns default sync object when no row exists") {
    auto [status, payload] = call(http::verb::get, "/projects/proj-1/git/sync-status");
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["sync"]["uncommitted_changes_count"] == 0);
    REQUIRE(payload["data"]["sync"]["unpushed_commits_count"] == 0);
    REQUIRE(payload["data"]["sync"]["retry_count"] == 0);
    REQUIRE(payload["data"]["sync"]["pull_retry_count"] == 0);
    REQUIRE(payload["data"]["sync"]["updated_at"].is_null());
  }

  SECTION("sync-status maps existing zero updated_at to null") {
    holder::project::ProjectSyncRepo sync_repo(db);
    (void)sync_repo;
    db.exec(
        "INSERT INTO project_sync_state(project_id,uncommitted_changes_count,unpushed_commits_count,"
        "retry_count,pull_retry_count,updated_at) VALUES('proj-1',0,0,0,0,0) "
        "ON CONFLICT(project_id) DO UPDATE SET updated_at=0;");

    auto [status, payload] = call(http::verb::get, "/projects/proj-1/git/sync-status");
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"]["sync"]["updated_at"].is_null());
  }

  SECTION("git push best-effort metrics catch still returns payload") {
    // No repo exists at root_path; inspect_repo_sync_metrics will throw and be swallowed.
    git.push_result = {
        .status = holder::git::PushStatus::Pushed,
        .ahead_count = 0,
        .behind_count = 0,
        .error_message = {},
    };
    auto [status, payload] = call(http::verb::post, "/projects/proj-1/git/push", nlohmann::json::object());
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["status"] == "pushed");
  }

  SECTION("project get internal error catch with closed db") {
    db.close();
    auto [status, payload] = call(http::verb::get, "/projects/proj-1");
    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(payload["error"]["code"] == "error");
  }

  SECTION("project delete internal error catch with closed db") {
    db.close();
    auto [status, payload] = call(http::verb::delete_, "/projects/proj-1");
    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(payload["error"]["code"] == "error");
  }
}

TEST_CASE("ProjectRoutes recovery import and encryption-check branches", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto empty_param_get = [](const std::string&) { return std::string(); };
  ProjectRoutesTestGitOps git;

  auto call = [&](http::verb method,
                  const std::string& path,
                  const nlohmann::json& body = nlohmann::json()) {
    auto req = make_request(method, path, body);
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        path, req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  SECTION("global recovery import can mark pull succeeded with remote hint") {
    const auto repo_root = dir / "enc-repo";
    std::filesystem::create_directories(repo_root);

    auto [create_status, create_payload] = call(http::verb::post,
                                                "/projects",
                                                {{"project_id", "proj-enc"},
                                                 {"name", "Encrypted"},
                                                 {"root_path", repo_root.string()},
                                                 {"privacy_mode", "encrypted_git"},
                                                 {"git_remote_url", "https://example.com/recovered.git"},
                                                 {"created_at", 10},
                                                 {"updated_at", 10}});
    REQUIRE(create_status == http::status::created);
    REQUIRE(create_payload["ok"] == true);

    auto [export_status, export_payload] = call(http::verb::post,
                                                "/projects/proj-enc/recovery-token/export",
                                                {{"pin", "1234"}});
    REQUIRE(export_status == http::status::ok);
    const std::string token = export_payload["data"]["recovery_token"].get<std::string>();

    auto [del_status, del_payload] = call(http::verb::delete_, "/projects/proj-enc");
    REQUIRE(del_status == http::status::ok);
    REQUIRE(del_payload["ok"] == true);

    auto [import_status, import_payload] = call(http::verb::post,
                                                "/recovery-token/import",
                                                {{"pin", "1234"}, {"recovery_token", token}});
    REQUIRE(import_status == http::status::created);
    REQUIRE(import_payload["ok"] == true);
    REQUIRE(import_payload["data"]["remote_hint_present"] == true);
    REQUIRE(import_payload["data"]["remote_configured"] == true);
    REQUIRE(import_payload["data"]["pull_status"] == "succeeded");
  }

  SECTION("encryption-check plain and encrypted branches plus generic catch") {
    holder::project::ProjectRepo repo(db);

    holder::model::Project plain;
    plain.project_id = "proj-plain";
    plain.name = "Plain";
    plain.root_path = (dir / "plain").string();
    plain.privacy_mode = "plain";
    plain.created_at = 1;
    plain.updated_at = 1;
    repo.create(plain);

    auto [plain_status, plain_payload] =
        call(http::verb::get, "/projects/proj-plain/encryption-check");
    REQUIRE(plain_status == http::status::ok);
    REQUIRE(plain_payload["data"]["check"]["checked_files"] == 0);
    REQUIRE(plain_payload["data"]["check"]["unsafe_files"] == 0);

    holder::model::Project enc;
    enc.project_id = "proj-encrypted";
    enc.name = "Encrypted";
    enc.root_path = (dir / "encrypted").string();
    enc.privacy_mode = "encrypted_git";
    enc.created_at = 2;
    enc.updated_at = 2;
    repo.create(enc);
    std::filesystem::create_directories(enc.root_path);

    auto [enc_status, enc_payload] =
        call(http::verb::get, "/projects/proj-encrypted/encryption-check");
    REQUIRE(enc_status == http::status::ok);
    REQUIRE(enc_payload["data"]["check"].contains("unsafe_files"));
    const bool unsafe_files_is_int = enc_payload["data"]["check"]["unsafe_files"].is_number_unsigned() ||
                                     enc_payload["data"]["check"]["unsafe_files"].is_number_integer();
    REQUIRE(unsafe_files_is_int);

    db.close();
    auto [err_status, err_payload] =
        call(http::verb::get, "/projects/proj-encrypted/encryption-check");
    REQUIRE(err_status == http::status::bad_request);
    REQUIRE(err_payload["error"]["code"] == "bad_request");
  }
}

TEST_CASE("ProjectRoutes create and recovery required field branches", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto empty_param_get = [](const std::string&) { return std::string(); };
  ProjectRoutesTestGitOps git;

  auto call = [&](http::verb method,
                  const std::string& path,
                  const nlohmann::json& body = nlohmann::json()) {
    auto req = make_request(method, path, body);
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_project_routes(
        path, req, res, db, &git, uuid_v4, empty_param_get);
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  SECTION("projects post missing name") {
    auto [status, payload] = call(http::verb::post, "/projects", {{"x", 1}});
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("projects post parses project_key_id") {
    auto [status, payload] = call(http::verb::post,
                                  "/projects",
                                  {{"project_id", "proj-2"},
                                   {"name", "Two"},
                                   {"root_path", (dir / "repo2").string()},
                                   {"project_key_id", "k1"},
                                   {"privacy_mode", "plain"},
                                   {"created_at", 1},
                                   {"updated_at", 1}});
    REQUIRE(status == http::status::created);
    REQUIRE(payload["ok"] == true);
  }

  SECTION("project recovery export missing pin") {
    holder::project::ProjectRepo repo(db);
    holder::model::Project project;
    project.project_id = "proj-1";
    project.name = "One";
    project.root_path = (dir / "repo1").string();
    project.privacy_mode = "plain";
    project.project_key_id = std::string("kid");
    project.created_at = 1;
    project.updated_at = 1;
    repo.create(project);

    auto [status, payload] =
        call(http::verb::post, "/projects/proj-1/recovery-token/export", {{"x", 1}});
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("project recovery import missing fields") {
    holder::project::ProjectRepo repo(db);
    holder::model::Project project;
    project.project_id = "proj-1";
    project.name = "One";
    project.root_path = (dir / "repo1").string();
    project.privacy_mode = "plain";
    project.created_at = 1;
    project.updated_at = 1;
    repo.create(project);

    auto [status, payload] =
        call(http::verb::post, "/projects/proj-1/recovery-token/import", {{"pin", "1234"}});
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }
}
