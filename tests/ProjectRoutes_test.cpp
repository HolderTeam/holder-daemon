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

  void open_or_init(const std::filesystem::path&) override {}
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {}
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
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
