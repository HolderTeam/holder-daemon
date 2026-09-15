#include "TestCommand.h"
#include "http_test_helpers.h"
#include "platform/Paths.h"
#include "platform/ServerInfo.h"
#include "project/ProjectSyncRepo.h"

#include <git2.h>
#include <memory>
#include <sstream>

namespace {
namespace http = boost::beast::http;
const std::string work_id = "12345678-1234-4234-8234-123456789abc";

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

struct CliResult {
  int code;
  std::string output;
  std::string error;
};

class SyncFixture {
 public:
  explicit SyncFixture(holder::git::GitOps* git = nullptr)
      : data_env("XDG_DATA_HOME", (dir / "data").string()),
        config_env("XDG_CONFIG_HOME", (dir / "config").string()),
        cache_env("XDG_CACHE_HOME", (dir / "cache").string()),
        keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string()),
        db(holder::test::open_db_with_schema(dir / "holder.db")),
        server("127.0.0.1", 0, db, token, nullptr, nullptr, git) {
    paths.ensure_dirs();
    holder::test::create_project(db, "home-id", (dir / "home").string());
    holder::test::create_project(db, work_id, (dir / "work").string());
    holder::project::ProjectRepo projects(db);
    projects.update_name("home-id", "Home", 1);
    projects.update_name(work_id, "Work Project", 1);
    bound = server.start();
    thread = std::make_unique<holder::test::HttpServerThreadGuard>(server, signals);
    REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));
    write_info(token);
  }

  void write_info(const std::string& client_token) {
    holder::core::ServerInfo info;
    info.pid = holder::core::current_pid();
    info.bind = bound.bind;
    info.port = static_cast<int>(bound.port);
    info.auth_token = client_token;
    holder::core::write_server_info(paths.info_path(), info);
  }

  CliResult run(const std::string& args, const std::string& name = "cli") {
    const auto out = dir / (name + ".out");
    const auto err = dir / (name + ".err");
    const auto command = std::string("\"") + HOLDER_CTL_PATH + "\" sync " + args + " > \"" +
                         out.string() + "\" 2> \"" + err.string() + "\"";
    const int code = holder::test::run_system_command(command);
    return {code, read_text(out), read_text(err)};
  }

  nlohmann::json request(
      http::verb method,
      const std::string& id,
      const nlohmann::json& body = nlohmann::json::object(),
      http::status status = http::status::ok
  ) {
    return holder::test::http_json_request(
        bound.bind,
        bound.port,
        token,
        method,
        "/projects/" + id,
        body,
        status
    );
  }

  const std::filesystem::path dir = holder::test::make_temp_dir();
  holder::test::EnvGuard data_env, config_env, cache_env, keystore_env;
  const holder::core::Paths paths = holder::core::Paths::resolve("holder");
  holder::platform::Db db;
  const std::string token = "private-daemon-token";
  holder::api::HttpServer server;
  holder::api::HttpServer::BoundInfo bound;
  holder::core::SignalHandler signals;
  std::unique_ptr<holder::test::HttpServerThreadGuard> thread;
};

std::optional<std::string> git_remote_url(const std::filesystem::path& root) {
  git_repository* repo = nullptr;
  REQUIRE(git_repository_open(&repo, root.string().c_str()) == 0);
  git_remote* remote = nullptr;
  const int result = git_remote_lookup(&remote, repo, "origin");
  std::optional<std::string> url;
  if (result == 0) url = ::git_remote_url(remote);
  git_remote_free(remote);
  git_repository_free(repo);
  REQUIRE((result == 0 || result == GIT_ENOTFOUND));
  return url;
}

class DiagnosticGitOps final : public holder::git::GitOps {
 public:
  std::string set_error;
  std::string remove_error;
  std::filesystem::path root;
  void open_or_init(const std::filesystem::path& path) override { root = path; }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {
    if (!set_error.empty()) throw std::runtime_error(set_error);
  }
  void remove_remote(const std::string&) override {
    if (!remove_error.empty()) throw std::runtime_error(remove_error);
  }
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override { return {}; }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    return {};
  }
  std::filesystem::path repo_dir() const override { return root; }
};

void check_mutation(const CliResult& result, bool changed) {
  REQUIRE(result.code == 0);
  CHECK(result.error.empty());
  CHECK(
      nlohmann::json::parse(result.output) ==
      nlohmann::json{
          {"ok", true},
          {"data", {{"project_id", work_id}, {"git_remote_changed", changed}}}
      }
  );
}
} // namespace

TEST_CASE(
    "holderctl sync remote and disconnect configure Git over HTTP",
    "[holderctl][sync][remote]"
) {
  SyncFixture fixture;
  const auto config = fixture.paths.config_dir / "holderctl.json";
  std::ofstream(config) << R"({"current_project_id":"home-id"})";
  const auto before_config = read_text(config);
  const std::string select = " --project 'Work Project'";
  auto result = fixture.run("remote" + select);
  REQUIRE(result.code == 0);
  CHECK(result.output == "Remote: none\n");
  CHECK(result.error.empty());
  result = fixture.run("remote --json" + select);
  REQUIRE(result.code == 0);
  CHECK(nlohmann::json::parse(result.output) == fixture.request(http::verb::get, work_id));

  const std::string first_url = "git@example.com:team/work.git";
  result = fixture.run("remote " + first_url + select);
  REQUIRE(result.code == 0);
  CHECK(result.output == "Project: " + work_id + "\nRemote set: " + first_url + "\n");
  CHECK(result.error.empty());
  CHECK(fixture.request(http::verb::get, work_id)["data"]["git_remote_url"] == first_url);
  CHECK(git_remote_url(fixture.dir / "work") == first_url);
  check_mutation(fixture.run("remote " + first_url + " --project " + work_id + " --json"), false);
  result = fixture.run("remote " + first_url + select);
  CHECK(result.output == "Project: " + work_id + "\nRemote unchanged: " + first_url + "\n");

  const std::string second_url = "https://example.com/team/work.git";
  check_mutation(fixture.run("--project 'Work Project' remote " + second_url + " --json"), true);
  result = fixture.run("remote" + select);
  CHECK(result.output == "Remote: " + second_url + "\n");
  result = fixture.run("remote --json" + select);
  CHECK(nlohmann::json::parse(result.output) == fixture.request(http::verb::get, work_id));
  CHECK(git_remote_url(fixture.dir / "work") == second_url);

  result = fixture.run("disconnect" + select);
  REQUIRE(result.code == 0);
  CHECK(result.output == "Project: " + work_id + "\nDisconnected.\n");
  CHECK_FALSE(git_remote_url(fixture.dir / "work").has_value());
  check_mutation(fixture.run("remote " + second_url + select + " --json"), true);
  check_mutation(fixture.run("disconnect --json" + select), true);
  CHECK_FALSE(git_remote_url(fixture.dir / "work").has_value());
  check_mutation(fixture.run("disconnect --json" + select), false);
  result = fixture.run("disconnect" + select);
  CHECK(result.output == "Project: " + work_id + "\nAlready disconnected.\n");
  CHECK(fixture.run("remote" + select).output == "Remote: none\n");
  CHECK(fixture.request(http::verb::get, "home-id")["data"]["git_remote_url"].is_null());
  CHECK_FALSE(std::filesystem::exists(fixture.dir / "home" / ".git"));
  CHECK(read_text(config) == before_config);
}

TEST_CASE(
    "holderctl remote mutations serialize concurrent requests",
    "[holderctl][sync][remote][listener]"
) {
  SyncFixture fixture;
  const std::string command = "remote https://example.com/work.git --project " + work_id +
                              " --json";
  CliResult first{}, second{};
  std::thread first_thread([&] {
    first = fixture.run(command, "first");
  });
  std::thread second_thread([&] {
    second = fixture.run(command, "second");
  });
  first_thread.join();
  second_thread.join();
  REQUIRE(first.code == 0);
  REQUIRE(second.code == 0);
  const bool first_changed = nlohmann::json::parse(first.output)["data"]["git_remote_changed"];
  check_mutation(first, first_changed);
  check_mutation(second, !first_changed);
  CHECK(git_remote_url(fixture.dir / "work") == "https://example.com/work.git");
}

TEST_CASE("holderctl sync remote redacts URLs and diagnostics", "[holderctl][sync][remote]") {
  std::string url;
  SECTION("password in HTTP URL") { url = "https://user:private-secret@example.com/work.git"; }
  SECTION("token in HTTP userinfo") { url = "https://private-secret@example.com/work.git"; }
  SECTION("query string") { url = "https://example.com/work.git?access_token=private-secret"; }
  SECTION("fragment") { url = "https://example.com/work.git#private-secret"; }
  SECTION("nonstandard SCP username") { url = "private-secret@example.com:work.git"; }
  SECTION("terminal control characters") {
    url = "https://example.com/work.git\x1b[31mprivate-secret";
  }
  DiagnosticGitOps git;
  SyncFixture fixture(&git);
  auto projects = holder::project::ProjectRepo(fixture.db);
  projects.update_git_remote(work_id, url, 1);
  holder::project::ProjectSyncRepo(fixture.db)
      .record_push_result(work_id, "auth_failed", false, "Bearer private-secret", 1);
  auto result = fixture.run("remote --project " + work_id);
  REQUIRE(result.code == 0);
  CHECK(result.output == "Remote: [redacted]\n");
  result = fixture.run("remote --project " + work_id + " --json");
  REQUIRE(result.code == 0);
  auto expected = fixture.request(http::verb::get, work_id);
  expected["data"]["git_remote_url"] = "[redacted]";
  expected["data"]["sync"]["last_sync_error"] = "[redacted]";
  CHECK(nlohmann::json::parse(result.output) == expected);
  CHECK(result.output.find("private-secret") == std::string::npos);
  CHECK(result.output.find(fixture.token) == std::string::npos);
  CHECK(result.error.empty());
  // Use a fixed credential-bearing URL for mutation, avoiding terminal characters in shell input.
  result = fixture.run(
      "remote 'https://user:private-secret@example.com/new.git' --project " + work_id
  );
  REQUIRE(result.code == 0);
  CHECK(result.output == "Project: " + work_id + "\nRemote set: [redacted]\n");
  check_mutation(
      fixture.run(
          "remote 'https://user:private-secret@example.com/new.git' --project " + work_id +
          " --json"
      ),
      false
  );
}

TEST_CASE(
    "holderctl sync remote and disconnect preserve typed failures",
    "[holderctl][sync][remote]"
) {
  DiagnosticGitOps git;
  std::string operation = "remote https://example.com/new.git";
  std::string expected_code;
  bool wrong_token = false, stop_server = false;
  SECTION("remote authentication failure") {
    wrong_token = true;
    expected_code = "unauthorized";
  }
  SECTION("disconnect authentication failure") {
    wrong_token = true;
    operation = "disconnect";
    expected_code = "unauthorized";
  }
  SECTION("remote rejected by Git") {
    git.set_error = "Invalid remote https://user:private-secret@example.com/new.git";
    expected_code = "bad_request";
  }
  SECTION("disconnect rejected by Git") {
    git.remove_error = "Bearer private-secret";
    operation = "disconnect";
    expected_code = "bad_request";
  }
  SECTION("network failure") {
    stop_server = true;
    expected_code = "network_error";
  }
  SECTION("missing project") {
    operation += " --project Missing";
    expected_code = "not_found";
  }
  SECTION("disconnect missing project") {
    operation = "disconnect --project Missing";
    expected_code = "not_found";
  }
  SECTION("disconnect network failure") {
    operation = "disconnect";
    stop_server = true;
    expected_code = "network_error";
  }
  SyncFixture fixture(&git);
  holder::project::ProjectRepo(fixture.db)
      .update_git_remote("home-id", "https://example.com/original.git", 1);
  // A selected ID sends mutations straight to PATCH, exercising its auth gate.
  std::ofstream(fixture.paths.config_dir / "holderctl.json")
      << R"({"current_project_id":"home-id"})";
  const auto before = fixture.request(http::verb::get, "home-id");
  if (wrong_token) fixture.write_info("wrong-private-token");
  if (stop_server) fixture.thread->stop();
  for (const auto* mode : {"", " --json"}) {
    const auto result = fixture.run(operation + mode);
    REQUIRE(result.code == 1);
    CHECK(result.output.empty());
    CHECK_FALSE(result.error.empty());
    CHECK(result.error.find("private-secret") == std::string::npos);
    CHECK(result.error.find("wrong-private-token") == std::string::npos);
    CHECK(result.error.find(fixture.token) == std::string::npos);
    if (std::string(mode) == " --json")
      CHECK(nlohmann::json::parse(result.error)["error"]["code"] == expected_code);
  }
  if (!stop_server) CHECK(fixture.request(http::verb::get, "home-id") == before);
}

TEST_CASE("project remote update contract rejects malformed input", "[http][sync][remote]") {
  DiagnosticGitOps git;
  SyncFixture fixture(&git);
  const auto before = fixture.request(http::verb::get, work_id);
  for (const auto& body :
       {nlohmann::json{{"git_remote_url", 42}, {"updated_at", 2}},
        nlohmann::json{{"git_remote_url", "url"}},
        nlohmann::json{{"git_remote_url", false}, {"updated_at", 2}}}) {
    CHECK(
        fixture.request(http::verb::patch, work_id, body, http::status::bad_request)["ok"] == false
    );
    CHECK(fixture.request(http::verb::get, work_id) == before);
  }
  const auto name_only =
      fixture.request(http::verb::patch, work_id, {{"name", "Work"}, {"updated_at", 2}});
  CHECK_FALSE(name_only["data"].contains("git_remote_changed"));
}
