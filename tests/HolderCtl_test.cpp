#include "http_test_helpers.h"
#include "TestCommand.h"

#include "git/GitOps.h"
#include "git/GitRepo.h"
#include "model/Resource.h"
#include "resource/ResourceRepo.h"

#include <catch2/catch_test_macros.hpp>
#include <git2.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class HolderCtlTimeZoneGuard {
 public:
  explicit HolderCtlTimeZoneGuard(const std::string& value) {
    if (const char* current = std::getenv("TZ")) old_ = current;
    set(value);
  }

  ~HolderCtlTimeZoneGuard() {
    if (old_.has_value()) {
      set(*old_);
    } else {
#ifdef _WIN32
      _putenv_s("TZ", "");
      _tzset();
#else
      unsetenv("TZ");
      tzset();
#endif
    }
  }

 private:
  static void set(const std::string& value) {
#ifdef _WIN32
    _putenv_s("TZ", value.c_str());
    _tzset();
#else
    setenv("TZ", value.c_str(), 1);
    tzset();
#endif
  }

  std::optional<std::string> old_;
};

int run_command(const std::string& cmd) {
  return holder::test::run_system_command(cmd);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string created_card_id_from_output(const std::string& output) {
  const std::string prefix = "Created card: ";
  REQUIRE(output.rfind(prefix, 0) == 0);
  auto id = output.substr(prefix.size());
  if (!id.empty() && id.back() == '\n') {
    id.pop_back();
  }
  REQUIRE_FALSE(id.empty());
  return id;
}

std::string created_resource_id_from_output(const std::string& output) {
  const std::string prefix = "Created resource: ";
  REQUIRE(output.rfind(prefix, 0) == 0);
  auto id = output.substr(prefix.size());
  if (!id.empty() && id.back() == '\n') {
    id.pop_back();
  }
  REQUIRE_FALSE(id.empty());
  return id;
}

std::string history_collision_commit_content(const std::string& tree_oid, std::uint32_t nonce) {
  return "tree " + tree_oid +
         "\n"
         "author Holder <holder@example.invalid> 1 +0000\n"
         "committer Holder <holder@example.invalid> 1 +0000\n\n"
         "holderctl history collision " +
         std::to_string(nonce) + "\n";
}

std::string write_holderctl_ambiguous_revision_prefix(
    const std::filesystem::path& root,
    const std::string& tree_oid
) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, root.string().c_str()) == 0);
  git_odb* odb = nullptr;
  REQUIRE(git_repository_odb(&odb, raw) == 0);

  std::unordered_map<std::uint32_t, std::uint32_t> seen;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> collision;
  for (std::uint32_t nonce = 0; nonce < 500'000 && !collision.has_value(); ++nonce) {
    const auto content = history_collision_commit_content(tree_oid, nonce);
    git_oid oid{};
    if (git_odb_hash(&oid, content.data(), content.size(), GIT_OBJECT_COMMIT) != 0) {
      FAIL("git_odb_hash failed while constructing ambiguous holderctl revision prefixes");
    }
    const auto prefix = (static_cast<std::uint32_t>(oid.id[0]) << 24U) |
                        (static_cast<std::uint32_t>(oid.id[1]) << 16U) |
                        (static_cast<std::uint32_t>(oid.id[2]) << 8U) |
                        static_cast<std::uint32_t>(oid.id[3]);
    const auto [position, inserted] = seen.emplace(prefix, nonce);
    if (!inserted) collision = std::pair{position->second, nonce};
  }
  REQUIRE(collision.has_value());

  std::string prefix;
  for (const auto nonce : {collision->first, collision->second}) {
    const auto content = history_collision_commit_content(tree_oid, nonce);
    git_oid oid{};
    REQUIRE(git_odb_write(&oid, odb, content.data(), content.size(), GIT_OBJECT_COMMIT) == 0);
    const std::string oid_text = git_oid_tostr_s(&oid);
    if (prefix.empty())
      prefix = oid_text.substr(0, 8);
    else
      REQUIRE(oid_text.substr(0, 8) == prefix);
  }

  git_odb_free(odb);
  git_repository_free(raw);
  return prefix;
}

void write_server_info(
    const std::filesystem::path& path,
    int pid = 12345,
    int port = 11499,
    const std::string& token = "deadbeef"
) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"pid\": " << pid << ",\n"
      << "  \"bind\": \"127.0.0.1\",\n"
      << "  \"port\": " << port << ",\n"
      << "  \"api_version\": \"0.1\",\n"
      << "  \"server_version\": \"0.1.0\",\n"
      << "  \"auth_token\": \"" << token << "\"\n"
      << "}\n";
}

void write_server_info_without_token(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"pid\": 12345,\n"
      << "  \"bind\": \"127.0.0.1\",\n"
      << "  \"port\": 11499,\n"
      << "  \"api_version\": \"0.1\",\n"
      << "  \"server_version\": \"0.1.0\"\n"
      << "}\n";
}

std::filesystem::path prepare_xdg_tree() {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root / "data" / "holder" / "server");
  std::filesystem::create_directories(xdg_root / "config");
  std::filesystem::create_directories(xdg_root / "cache");
  return xdg_root;
}

#ifndef _WIN32
void write_fake_systemctl(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_SYSTEMCTL_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_SYSTEMCTL_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}

void write_fake_tail(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_TAIL_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_TAIL_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}

void write_fake_xdg_open(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_XDG_OPEN_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_XDG_OPEN_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}

std::string desktop_opener_name_for_tests() {
#if defined(__APPLE__)
  return "open";
#else
  return "xdg-open";
#endif
}

void write_fake_editor(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "if [ \"${HOLDERCTL_FAKE_EDITOR_MODE:-write}\" = write ]; then\n"
      << "  printf '%s' \"$HOLDERCTL_FAKE_EDITOR_CONTENT\" > \"$1\"\n"
      << "fi\n"
      << "exit \"${HOLDERCTL_FAKE_EDITOR_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}
#endif

class HolderCtlProjectGitOps final : public holder::git::GitOps {
 public:
  struct Snapshot {
    int open_count = 0;
    std::string remote_name;
    std::string remote_url;
    std::filesystem::path opened_repo;
  };

  void open_or_init(const std::filesystem::path& repo_dir) override {
    std::scoped_lock lock(mu_);
    ++open_count;
    opened_repo = repo_dir;
  }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string& name, const std::string& url) override {
    std::scoped_lock lock(mu_);
    remote_name = name;
    remote_url = url;
  }
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override {
    return {
        .status = holder::git::RemoteProbeStatus::Reachable,
        .remote_has_head = true,
        .error_message = {}
    };
  }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    return {
        .status = holder::git::PushStatus::Pushed,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = {},
        .error_message = {}
    };
  }
  std::filesystem::path repo_dir() const override {
    std::scoped_lock lock(mu_);
    return opened_repo;
  }

  Snapshot snapshot() const {
    std::scoped_lock lock(mu_);
    return {
        .open_count = open_count,
        .remote_name = remote_name,
        .remote_url = remote_url,
        .opened_repo = opened_repo
    };
  }

 private:
  mutable std::mutex mu_;
  int open_count = 0;
  std::string remote_name;
  std::string remote_url;
  std::filesystem::path opened_repo;
};

} // namespace

TEST_CASE("holderctl token prints token from secure server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const auto out_path = xdg_root / "token.out";
  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token > \"" +
                          out_path.string() + "\"";
  REQUIRE(run_command(cmd) == 0);
  REQUIRE(read_text(out_path) == "deadbeef\n");
}

TEST_CASE("holderctl token reports missing server info file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

#ifndef _WIN32
  const auto server_dir = xdg_root / "data" / "holder" / "server";
  ::chmod(server_dir.c_str(), S_IRWXU);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token refuses symlink server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto target_path = server_dir / "target.json";
  const auto info_path = server_dir / "holder.json";
  write_server_info(target_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(target_path.c_str(), S_IRUSR | S_IWUSR);
  REQUIRE(::symlink(target_path.c_str(), info_path.c_str()) == 0);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token refuses non-regular server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  std::filesystem::create_directory(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token refuses loose token file permissions", "[holderctl]") {
#ifdef _WIN32
  SKIP("POSIX permission-bit token-file test is not meaningful on Windows");
#else
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token";
  REQUIRE(run_command(cmd) == 1);
#endif
}

TEST_CASE("holderctl token refuses loose token directory permissions", "[holderctl]") {
#ifdef _WIN32
  SKIP("POSIX permission-bit token-directory test is not meaningful on Windows");
#else
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
  ::chmod(server_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
#endif
}

TEST_CASE("holderctl token requires auth token field", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info_without_token(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl status paths openapi and version smoke", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()));
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " status") == 0);
  REQUIRE(run_command(bin + " paths") == 0);
  REQUIRE(run_command(bin + " openapi --url") == 0);
  REQUIRE(run_command(bin + " logs --path") == 0);
  REQUIRE(run_command(bin + " --version") == 0);
  REQUIRE(run_command(bin + " --help") == 0);
  REQUIRE(run_command(bin + " nope") == 2);

  const auto json_error_path = xdg_root / "unknown-command.json";
  REQUIRE(
      run_command(bin + " nope --json >/dev/null 2> \"" + json_error_path.string() + "\"") == 2
  );
  const auto json_error = nlohmann::json::parse(read_text(json_error_path));
  REQUIRE(json_error["ok"] == false);
  REQUIRE(json_error["error"]["code"] == "unknown_command");
  REQUIRE(json_error["error"]["message"] == "Unknown command: nope");
  REQUIRE(json_error["error"]["details"]["command"] == "nope");
}

TEST_CASE("holderctl status reports missing or stopped daemon", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " status") == 1);
  REQUIRE(run_command(bin + " openapi --url") == 0);

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, -1);
  REQUIRE(run_command(bin + " status") == 1);
}

TEST_CASE("holderctl health checks metadata process token and HTTP endpoint", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  holder::platform::Db db;
  db.open(db_path);

  const std::string token = "healthtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" health";
  REQUIRE(run_command(cmd) == 0);

  write_server_info(
      info_path,
      static_cast<int>(::getpid()),
      static_cast<int>(bound.port),
      "wrongtoken"
  );
  REQUIRE(run_command(cmd + " >/dev/null 2>/dev/null") == 1);

  write_server_info(info_path, static_cast<int>(::getpid()), 1, token);
  REQUIRE(run_command(cmd + " >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl reindex requests daemon reindex", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string token = "reindextoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const auto out_path = xdg_root / "reindex.out";
  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" reindex > \"" +
                          out_path.string() + "\"";
  REQUIRE(run_command(cmd) == 0);
  REQUIRE(read_text(out_path) == "Reindex complete.\n");

  write_server_info(
      info_path,
      static_cast<int>(::getpid()),
      static_cast<int>(bound.port),
      "wrongtoken"
  );
  REQUIRE(
      run_command(std::string("\"") + HOLDER_CTL_PATH + "\" reindex >/dev/null 2>/dev/null") == 1
  );

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl reindex reports local metadata problems", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " reindex >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " reindex --bad >/dev/null 2>/dev/null") == 1);
}

TEST_CASE("holderctl projects lists daemon projects", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "project-root";
  std::filesystem::create_directories(project_root);
  holder::test::create_project(db, "proj-1", project_root.string());

  const std::string token = "projecttoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto list_path = xdg_root / "projects.out";
  REQUIRE(run_command(bin + " projects > \"" + list_path.string() + "\"") == 0);
  REQUIRE(
      read_text(list_path) ==
      "PROJECT_ID\tNAME\tROOT\nproj-1\tProject\t" + project_root.string() + "\n"
  );

  const auto count_path = xdg_root / "projects-count.out";
  REQUIRE(run_command(bin + " projects --count > \"" + count_path.string() + "\"") == 0);
  REQUIRE(
      read_text(count_path) == "PROJECT_ID\tNAME\tCARDS\tROOT_CARDS\tROOT\n"
                               "proj-1\tProject\t0\t0\t" +
                                   project_root.string() + "\n"
  );

  const auto json_path = xdg_root / "projects.json";
  REQUIRE(run_command(bin + " projects --json > \"" + json_path.string() + "\"") == 0);
  const auto payload = nlohmann::json::parse(read_text(json_path));
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"].is_array());
  REQUIRE(payload["data"][0]["project_id"] == "proj-1");

  write_server_info(
      info_path,
      static_cast<int>(::getpid()),
      static_cast<int>(bound.port),
      "wrongtoken"
  );
  REQUIRE(run_command(bin + " projects >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl projects reports an empty daemon project list", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string token = "emptyprojecttoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto list_path = xdg_root / "projects-empty.out";
  REQUIRE(run_command(bin + " projects > \"" + list_path.string() + "\"") == 0);
  REQUIRE(read_text(list_path) == "No projects.\n");

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl projects reports local metadata and option problems", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " projects >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " projects --bad >/dev/null 2>/dev/null") == 1);
}

TEST_CASE("holderctl project new creates a project and can select it", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  HolderCtlProjectGitOps git;

  const std::string token = "projectnewtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr, &git);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto json_out = xdg_root / "project-new.json";
  REQUIRE(
      run_command(
          bin +
          " project new CLI Project --plain --remote https://example.com/repo.git "
          "--use --json > \"" +
          json_out.string() + "\""
      ) == 0
  );
  const auto created = nlohmann::json::parse(read_text(json_out));
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["name"] == "CLI Project");
  REQUIRE(created["data"]["privacy_mode"] == "plain");
  REQUIRE(created["data"]["git_remote_url"] == "https://example.com/repo.git");
  const auto project_id = created["data"]["project_id"].get<std::string>();
  const auto root_path = created["data"]["root_path"].get<std::string>();
  REQUIRE_FALSE(project_id.empty());
  REQUIRE_FALSE(root_path.empty());

  const auto git_snapshot = git.snapshot();
  // Project creation opens the repository to persist its durable manifest,
  // then opens it again to configure the requested remote.
  REQUIRE(git_snapshot.open_count == 2);
  REQUIRE(git_snapshot.remote_name == "origin");
  REQUIRE(git_snapshot.remote_url == "https://example.com/repo.git");
  REQUIRE(git_snapshot.opened_repo == std::filesystem::path(root_path));

  const auto config_path = xdg_root / "config" / "holder" / "holderctl.json";
  REQUIRE(nlohmann::json::parse(read_text(config_path))["current_project_id"] == project_id);

  const auto current_out = xdg_root / "project-new-current.out";
  REQUIRE(run_command(bin + " current > \"" + current_out.string() + "\"") == 0);
  REQUIRE(
      read_text(current_out) ==
      "Current project: CLI Project (" + project_id + ")\nRoot: " + root_path + "\n"
  );

  const auto text_out = xdg_root / "project-new-text.out";
  REQUIRE(
      run_command(bin + " project new Second Project --plain > \"" + text_out.string() + "\"") == 0
  );
  REQUIRE(read_text(text_out).rfind("Created project: ", 0) == 0);

  const auto use_text_out = xdg_root / "project-new-use-text.out";
  REQUIRE(
      run_command(
          bin + " project new Third Project --plain --use > \"" + use_text_out.string() + "\""
      ) == 0
  );
  const auto use_text = read_text(use_text_out);
  REQUIRE(use_text.rfind("Created project: ", 0) == 0);
  REQUIRE(use_text.find("\nCurrent project: Third Project (") != std::string::npos);

  const auto encrypted_json_out = xdg_root / "project-new-encrypted.json";
  REQUIRE(
      run_command(
          bin + " project new Encrypted Project --encrypted --json > \"" +
          encrypted_json_out.string() + "\""
      ) == 0
  );
  const auto encrypted = nlohmann::json::parse(read_text(encrypted_json_out));
  REQUIRE(encrypted["ok"] == true);
  REQUIRE(encrypted["data"]["name"] == "Encrypted Project");
  REQUIRE(encrypted["data"]["privacy_mode"] == "encrypted_git");

  write_server_info(
      info_path,
      static_cast<int>(::getpid()),
      static_cast<int>(bound.port),
      "wrongtoken"
  );
  REQUIRE(run_command(bin + " project new Broken --plain >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl parser errors do not require daemon metadata", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " current nope >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project nope >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new Name --plain --encrypted >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new Name --encrypted --plain >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new Name --remote >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " project new Name --remote '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " search >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " search --bad query >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " search --limit >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " search --limit nope query >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " search --limit 0 query >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --limit >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --limit nope >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --limit 0 >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --limit 2 >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --parent >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --parent '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards --recent --parent card-id >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " cards extra >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tags one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tags '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tags --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tag >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tag add one >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tag change one work >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " tag add one work --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " card >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " card --bad card-id >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " card one two >/dev/null 2>/dev/null") == 1);
  const auto json_error_path = xdg_root / "parser-error.json";
  REQUIRE(
      run_command(bin + " card --json >/dev/null 2> \"" + json_error_path.string() + "\"") == 1
  );
  const auto json_error = nlohmann::json::parse(read_text(json_error_path));
  REQUIRE(json_error["ok"] == false);
  REQUIRE(json_error["error"]["code"] == "cli_error");
  REQUIRE(json_error["error"]["message"] == "Usage: holderctl card [--json] <card-reference>");
  REQUIRE(run_command(bin + " edit >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " edit one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " links >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " links --bad one >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " links one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " backlinks >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " backlinks --bad one >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " backlinks one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two three >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two --kind >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two --kind '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two --label >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " link one two --label '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash --json >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash list extra >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash restore >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash restore one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash delete >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash delete one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash empty extra >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " restore >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " restore --json >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " restore one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " restore one --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " append >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource list --filter >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource list --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add one --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add one --kind >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add one --kind '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add one --label >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit one --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit one --kind >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit one --kind '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource show >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource show --json >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource open >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource open '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource open one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token nope >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token export --bad >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token export --pin >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token export --pin '' >/dev/null 2>/dev/null") == 1);
  REQUIRE(
      run_command(bin + " recovery-token import --pin 1234 --out x >/dev/null 2>/dev/null") == 1
  );
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 1234 --file x --out y >/dev/null 2>/dev/null"
      ) == 1
  );
}

TEST_CASE("holderctl use and current manage current project", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto home_root = xdg_root / "home-root";
  const auto alpha_root = xdg_root / "alpha-root";
  const auto beta_root = xdg_root / "beta-root";
  std::filesystem::create_directories(home_root);
  std::filesystem::create_directories(alpha_root);
  std::filesystem::create_directories(beta_root);
  holder::test::create_project(db, "home-id", home_root.string());
  holder::test::create_project(db, "alpha-id", alpha_root.string());
  holder::test::create_project(db, "beta-id", beta_root.string());
  {
    holder::project::ProjectRepo repo(db);
    repo.update_name("home-id", "Home", 3);
    repo.update_name("beta-id", "Beta Project", 2);
  }

  const std::string token = "usetoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto current_home_out = xdg_root / "current-home.out";
  REQUIRE(run_command(bin + " current > \"" + current_home_out.string() + "\"") == 0);
  REQUIRE(
      read_text(current_home_out) ==
      "Current project: Home (home-id)\nRoot: " + home_root.string() + "\n"
  );

  const auto use_name_out = xdg_root / "use-name.out";
  REQUIRE(run_command(bin + " use \"Beta Project\" > \"" + use_name_out.string() + "\"") == 0);
  REQUIRE(read_text(use_name_out) == "Current project: Beta Project (beta-id)\n");
  const auto config_path = xdg_root / "config" / "holder" / "holderctl.json";
  REQUIRE(nlohmann::json::parse(read_text(config_path))["current_project_id"] == "beta-id");

  const auto current_out = xdg_root / "current.out";
  REQUIRE(run_command(bin + " current > \"" + current_out.string() + "\"") == 0);
  REQUIRE(
      read_text(current_out) ==
      "Current project: Beta Project (beta-id)\nRoot: " + beta_root.string() + "\n"
  );

  const auto use_id_out = xdg_root / "use-id.out";
  REQUIRE(run_command(bin + " use alpha-id > \"" + use_id_out.string() + "\"") == 0);
  REQUIRE(read_text(use_id_out) == "Current project: Project (alpha-id)\n");
  REQUIRE(nlohmann::json::parse(read_text(config_path))["current_project_id"] == "alpha-id");

  const auto reset_out = xdg_root / "reset.out";
  REQUIRE(run_command(bin + " use > \"" + reset_out.string() + "\"") == 0);
  REQUIRE(read_text(reset_out) == "Current project: Home (home-id)\n");
  REQUIRE_FALSE(std::filesystem::exists(config_path));

  const auto use_home_name_out = xdg_root / "use-home-name.out";
  REQUIRE(run_command(bin + " use Home > \"" + use_home_name_out.string() + "\"") == 0);
  REQUIRE(read_text(use_home_name_out) == "Current project: Home (home-id)\n");
  REQUIRE_FALSE(std::filesystem::exists(config_path));

  std::filesystem::create_directories(config_path.parent_path());
  {
    std::ofstream out(config_path);
    out << "{}\n";
  }
  const auto empty_config_current_out = xdg_root / "empty-config-current.out";
  REQUIRE(run_command(bin + " current > \"" + empty_config_current_out.string() + "\"") == 0);
  REQUIRE(
      read_text(empty_config_current_out) ==
      "Current project: Home (home-id)\nRoot: " + home_root.string() + "\n"
  );

  {
    std::ofstream out(config_path);
    out << "{\"current_project_id\":\"missing-id\"}\n";
  }
  REQUIRE(run_command(bin + " current >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl use reports missing and ambiguous projects", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  holder::test::create_project(db, "first-id", (xdg_root / "first").string());
  holder::test::create_project(db, "second-id", (xdg_root / "second").string());

  const std::string token = "ambiguoususetoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " use Project >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use Missing >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use one two >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl search uses the current project", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "project-root";
  std::filesystem::create_directories(project_root);
  holder::test::create_project(db, "search-project", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  holder::model::Card card;
  card.card_id = "search-card";
  card.project_id = "search-project";
  card.title = "Searchable Card";
  card.created_at = 10;
  card.updated_at = 11;
  card_store.create(card, "unique holderctl search term");

  const std::string token = "searchtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " search unique >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use search-project >/dev/null") == 0);

  const auto search_out = xdg_root / "search.out";
  REQUIRE(run_command(bin + " search unique > \"" + search_out.string() + "\"") == 0);
  const auto output = read_text(search_out);
  REQUIRE(output.find("search-card\tSearchable Card\n") != std::string::npos);

  const auto search_empty_out = xdg_root / "search-empty.out";
  REQUIRE(run_command(bin + " search absentterm > \"" + search_empty_out.string() + "\"") == 0);
  REQUIRE(read_text(search_empty_out) == "No cards found.\n");

  const auto json_path = xdg_root / "search.json";
  REQUIRE(
      run_command(bin + " search --json --limit 5 \"unique\" > \"" + json_path.string() + "\"") == 0
  );
  const auto payload = nlohmann::json::parse(read_text(json_path));
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"].is_array());
  REQUIRE(payload["data"][0]["card_id"] == "search-card");

  REQUIRE(run_command(bin + " search \"unique holderctl\" >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl cards lists root and recent cards in the current project", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "cards-root";
  std::filesystem::create_directories(project_root);
  holder::test::create_project(db, "cards-project", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  constexpr const char* root_one_id = "12345678-1111-4111-8111-111111111111";
  constexpr const char* root_two_id = "12345678-2222-4222-8222-222222222222";
  constexpr const char* child_id = "87654321-3333-4333-8333-333333333333";

  holder::model::Card root_one;
  root_one.card_id = root_one_id;
  root_one.project_id = "cards-project";
  root_one.title = "Root One";
  root_one.created_at = 10;
  root_one.updated_at = 20;
  card_store.create(root_one, "root one body");

  holder::model::Card root_two;
  root_two.card_id = root_two_id;
  root_two.project_id = "cards-project";
  root_two.title = "Root Two";
  root_two.created_at = 11;
  root_two.updated_at = 30;
  card_store.create(root_two, "root two body");

  holder::model::Card child;
  child.card_id = child_id;
  child.project_id = "cards-project";
  child.parent_card_id = root_one_id;
  child.title = "Child One";
  child.created_at = 12;
  child.updated_at = 40;
  card_store.create(child, "child body");

  const std::string token = "cardstoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " cards >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use cards-project >/dev/null") == 0);

  const auto root_out = xdg_root / "cards-root.out";
  REQUIRE(run_command(bin + " cards > \"" + root_out.string() + "\"") == 0);
  const auto root_output = read_text(root_out);
  REQUIRE(root_output.find("CARD_ID\tTITLE\tCHILDREN\tUPDATED\n") == 0);
  REQUIRE(root_output.find("12345678-1\tRoot One\t1\t20\n") != std::string::npos);
  REQUIRE(root_output.find("12345678-2\tRoot Two\t0\t30\n") != std::string::npos);
  REQUIRE(root_output.find(child_id) == std::string::npos);

  const auto child_out = xdg_root / "cards-child.out";
  REQUIRE(run_command(bin + " cards --parent 'Root One' > \"" + child_out.string() + "\"") == 0);
  const auto child_output = read_text(child_out);
  REQUIRE(child_output.find("87654321\tChild One\t0\t40\n") != std::string::npos);
  REQUIRE(child_output.find(root_two_id) == std::string::npos);

  const auto empty_child_out = xdg_root / "cards-empty-child.out";
  REQUIRE(
      run_command(bin + " cards --parent 'Child One' > \"" + empty_child_out.string() + "\"") == 0
  );
  REQUIRE(read_text(empty_child_out) == "No root cards.\n");

  const auto recent_out = xdg_root / "cards-recent.out";
  REQUIRE(run_command(bin + " cards --recent --limit 2 > \"" + recent_out.string() + "\"") == 0);
  const auto recent_output = read_text(recent_out);
  REQUIRE(recent_output.find("87654321\tChild One\t0\t40\n") != std::string::npos);
  REQUIRE(recent_output.find("12345678\tRoot Two\t0\t30\n") != std::string::npos);
  REQUIRE(recent_output.find(root_one_id) == std::string::npos);

  const auto json_path = xdg_root / "cards.json";
  REQUIRE(run_command(bin + " cards --json > \"" + json_path.string() + "\"") == 0);
  const auto payload = nlohmann::json::parse(read_text(json_path));
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"].is_array());
  REQUIRE(payload["data"].size() == 2);
  REQUIRE(payload.dump().find(root_one_id) != std::string::npos);
  REQUIRE(payload.dump().find(root_two_id) != std::string::npos);

  write_server_info(
      info_path,
      static_cast<int>(::getpid()),
      static_cast<int>(bound.port),
      "wrongtoken"
  );
  REQUIRE(run_command(bin + " cards >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl tags query and mutate live card tags", "[holderctl][tags]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "tags-root";
  const auto other_root = xdg_root / "tags-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "tags-project", project_root.string());
  holder::test::create_project(db, "other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  constexpr const char* first_id = "12345678-1111-4111-8111-111111111111";
  constexpr const char* second_id = "12345678-2222-4222-8222-222222222222";
  constexpr const char* prose_id = "87654321-3333-4333-8333-333333333333";
  constexpr const char* other_id = "99999999-4444-4444-8444-444444444444";
  holder::test::create_card_fixture(
      card_store,
      first_id,
      "tags-project",
      "First Tagged",
      "First body\n\n#Android",
      10
  );
  holder::test::create_card_fixture(
      card_store,
      second_id,
      "tags-project",
      "Second Tagged",
      "Second body\n\n#android #sync",
      20
  );
  holder::test::create_card_fixture(
      card_store,
      prose_id,
      "tags-project",
      "Prose Tagged",
      "Keep #Prose in this sentence.",
      30
  );
  holder::test::create_card_fixture(
      card_store,
      other_id,
      "other-project",
      "Other Tagged",
      "Other body\n\n#android",
      40
  );

  const std::string token = "tagstoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " use tags-project >/dev/null") == 0);

  const auto list_path = xdg_root / "tags.out";
  REQUIRE(run_command(bin + " tags > \"" + list_path.string() + "\"") == 0);
  REQUIRE(read_text(list_path) == "TAG\tCARDS\nandroid\t2\nprose\t1\nsync\t1\n");

  const auto cards_path = xdg_root / "tagged-cards.out";
  REQUIRE(run_command(bin + " tags Android > \"" + cards_path.string() + "\"") == 0);
  REQUIRE(
      read_text(cards_path) ==
      "CARD_ID\tTITLE\n12345678-2\tSecond Tagged\n12345678-1\tFirst Tagged\n"
  );
  REQUIRE(read_text(cards_path).find(other_id) == std::string::npos);

  const auto cards_json_path = xdg_root / "tagged-cards.json";
  REQUIRE(
      run_command(bin + " tags android --json > \"" + cards_json_path.string() + "\"") == 0
  );
  const auto cards_json = nlohmann::json::parse(read_text(cards_json_path));
  REQUIRE(cards_json["ok"] == true);
  REQUIRE(cards_json["data"].size() == 2);
  REQUIRE(cards_json.dump().find(first_id) != std::string::npos);
  REQUIRE(cards_json.dump().find(second_id) != std::string::npos);
  REQUIRE(cards_json.dump().find(other_id) == std::string::npos);

  const auto tags_json_path = xdg_root / "tags.json";
  REQUIRE(run_command(bin + " tags --json > \"" + tags_json_path.string() + "\"") == 0);
  const auto tags_json = nlohmann::json::parse(read_text(tags_json_path));
  REQUIRE(tags_json["ok"] == true);
  REQUIRE(tags_json["data"][0]["tag"] == "android");
  REQUIRE(tags_json["data"][0]["card_count"] == 2);

  const auto add_path = xdg_root / "tag-add.out";
  REQUIRE(
      run_command(bin + " tag add 'First Tagged' Work > \"" + add_path.string() + "\"") == 0
  );
  REQUIRE(read_text(add_path) == "Added tag #work to 12345678.\n");

  const auto repeat_path = xdg_root / "tag-repeat.json";
  REQUIRE(
      run_command(
          bin + " tag add 'First Tagged' WORK --json > \"" + repeat_path.string() + "\""
      ) == 0
  );
  const auto repeated = nlohmann::json::parse(read_text(repeat_path));
  REQUIRE(repeated["ok"] == true);
  REQUIRE(repeated["data"]["card_id"] == first_id);
  REQUIRE(repeated["data"]["tag"] == "work");
  REQUIRE(repeated["data"]["outcome"] == "already_present");
  REQUIRE(repeated["data"]["changed"] == false);

  const auto remove_path = xdg_root / "tag-remove.json";
  REQUIRE(
      run_command(
          bin + " tag remove 'First Tagged' work --json > \"" + remove_path.string() + "\""
      ) == 0
  );
  const auto removed = nlohmann::json::parse(read_text(remove_path));
  REQUIRE(removed["data"]["card_id"] == first_id);
  REQUIRE(removed["data"]["outcome"] == "removed");
  REQUIRE(removed["data"]["changed"] == true);

  const auto missing_path = xdg_root / "tag-missing.out";
  REQUIRE(
      run_command(
          bin + " tag remove 'First Tagged' work > \"" + missing_path.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(missing_path) == "Tag #work is not present on 12345678.\n");

  const auto prose_path = xdg_root / "tag-prose.out";
  REQUIRE(
      run_command(
          bin + " tag remove 'Prose Tagged' PROSE > \"" + prose_path.string() + "\""
      ) == 0
  );
  REQUIRE(
      read_text(prose_path) ==
      "Tag #prose was not removed from 87654321: it appears outside the editable trailing tag "
      "line; edit the card text directly.\n"
  );
  REQUIRE(
      card_store.get_content(*card_store.get(prose_id)).value() ==
      "Keep #Prose in this sentence."
  );

  const auto invalid_path = xdg_root / "tag-invalid.json";
  REQUIRE(
      run_command(
          bin + " tag add 'First Tagged' 123invalid --json >/dev/null 2> \"" +
          invalid_path.string() + "\""
      ) == 1
  );
  const auto invalid = nlohmann::json::parse(read_text(invalid_path));
  REQUIRE(invalid["ok"] == false);
  REQUIRE(invalid["error"]["code"] == "invalid_tag");

  const auto ambiguous_path = xdg_root / "tag-ambiguous.json";
  REQUIRE(
      run_command(
          bin + " tag add 12345678 work --json >/dev/null 2> \"" +
          ambiguous_path.string() + "\""
      ) == 1
  );
  const auto ambiguous = nlohmann::json::parse(read_text(ambiguous_path));
  REQUIRE(ambiguous["error"]["code"] == "card_reference_ambiguous");
  REQUIRE(ambiguous.dump().find(first_id) != std::string::npos);
  REQUIRE(ambiguous.dump().find(second_id) != std::string::npos);

  REQUIRE(run_command(bin + " tag add 'Other Tagged' work >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE(
    "holderctl milestones and calendar expose the existing daemon API",
    "[holderctl][milestones][calendar]"
) {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  HolderCtlTimeZoneGuard timezone("UTC0");

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "milestone-root";
  const auto other_root = xdg_root / "milestone-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "milestone-project", project_root.string());
  holder::test::create_project(db, "other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  constexpr const char* card_id = "aaaaaaaa-1111-4111-8111-111111111111";
  constexpr const char* collision_id = "aaaaaaaa-3333-4333-8333-333333333333";
  constexpr const char* trashed_id = "cccccccc-4444-4444-8444-444444444444";
  constexpr const char* other_id = "bbbbbbbb-2222-4222-8222-222222222222";
  holder::test::create_card_fixture(
      card_store,
      card_id,
      "milestone-project",
      "Release Card",
      "Release notes",
      1780275600
  );
  holder::test::create_card_fixture(
      card_store,
      collision_id,
      "milestone-project",
      "Collision Release",
      "Collision notes",
      1
  );
  holder::test::create_card_fixture(
      card_store,
      trashed_id,
      "milestone-project",
      "Trashed Release",
      "Trashed notes",
      2
  );
  card_store.trash(trashed_id, 3);
  holder::test::create_card_fixture(
      card_store,
      other_id,
      "other-project",
      "Other Release",
      "Other notes",
      1780275600
  );

  const std::string token = "milestonestoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " use milestone-project >/dev/null") == 0);

  const auto empty_path = xdg_root / "milestones-empty.out";
  REQUIRE(run_command(bin + " milestones 'Release Card' > \"" + empty_path.string() + "\"") == 0);
  REQUIRE(read_text(empty_path) == "No milestones.\n");

  const auto all_day_path = xdg_root / "milestone-all-day.json";
  REQUIRE(
      run_command(
          bin +
          " milestone add 'Release Card' 2026-06-01 --end 2026-06-03 --kind release "
          "--description 'Release train' --all-day --json > \"" +
          all_day_path.string() + "\""
      ) == 0
  );
  const auto all_day = nlohmann::json::parse(read_text(all_day_path));
  REQUIRE(all_day["ok"] == true);
  REQUIRE(all_day["data"]["card_id"] == card_id);
  REQUIRE(all_day["data"]["start_at"] == 1780272000);
  REQUIRE(all_day["data"]["end_at"] == 1780444800);
  REQUIRE(all_day["data"]["all_day"] == true);
  REQUIRE(all_day["data"]["kind"] == "release");
  REQUIRE(all_day["data"]["description"] == "Release train");
  const auto all_day_id = all_day["data"]["milestone_id"].get<std::string>();
  REQUIRE(all_day_id.size() == 36);

  const auto timed_path = xdg_root / "milestone-timed.out";
  REQUIRE(
      run_command(
          bin +
          " milestone add 'Release Card' 2026-06-02T12:30:00+02:30 "
          "--end 2026-06-02T11:00:00Z --kind meeting > \"" +
          timed_path.string() + "\""
      ) == 0
  );
  const auto timed_output = read_text(timed_path);
  REQUIRE(timed_output.find("2026-06-02T10:00:00+00:00") != std::string::npos);
  REQUIRE(timed_output.find("2026-06-02T11:00:00+00:00") != std::string::npos);

  const auto list_path = xdg_root / "milestones.json";
  REQUIRE(
      run_command(
          bin + " milestones 'Release Card' --json > \"" + list_path.string() + "\""
      ) == 0
  );
  const auto listed = nlohmann::json::parse(read_text(list_path));
  REQUIRE(listed["data"].size() == 2);
  REQUIRE(listed.dump().find(all_day_id) != std::string::npos);
  const auto timed_id = listed["data"][1]["milestone_id"].get<std::string>();
  REQUIRE(timed_id.size() == 36);

  const auto edit_partial_path = xdg_root / "milestone-edit-partial.json";
  REQUIRE(
      run_command(
          bin + " milestone edit 'Release Card' " + timed_id +
          " --kind appointment --json > \"" + edit_partial_path.string() + "\""
      ) == 0
  );
  const auto partial = nlohmann::json::parse(read_text(edit_partial_path));
  REQUIRE(partial["data"]["milestone_id"] == timed_id);
  REQUIRE(partial["data"]["card_id"] == card_id);
  REQUIRE(partial["data"]["start_at"] == 1780394400);
  REQUIRE(partial["data"]["end_at"] == 1780398000);
  REQUIRE(partial["data"]["all_day"] == false);
  REQUIRE(partial["data"]["kind"] == "appointment");
  REQUIRE(partial["data"]["description"].is_null());

  const auto edit_all_day_path = xdg_root / "milestone-edit-all-day.out";
  REQUIRE(
      run_command(
          bin + " milestone edit 'Release Card' " + timed_id +
          " --start 2026-06-02 --end 2026-06-03 --all-day --clear-kind "
          "--description 'All day work' > \"" +
          edit_all_day_path.string() + "\""
      ) == 0
  );
  const auto edit_all_day = read_text(edit_all_day_path);
  REQUIRE(edit_all_day.find("Updated milestone " + timed_id) != std::string::npos);
  REQUIRE(edit_all_day.find("2026-06-02 – 2026-06-03") != std::string::npos);

  const auto after_edit_path = xdg_root / "milestones-after-edit.json";
  REQUIRE(
      run_command(
          bin + " milestones 'Release Card' --json > \"" + after_edit_path.string() + "\""
      ) == 0
  );
  const auto after_edit = nlohmann::json::parse(read_text(after_edit_path));
  REQUIRE(after_edit["data"].size() == 2);
  REQUIRE(after_edit["data"][1]["milestone_id"] == timed_id);
  REQUIRE(after_edit["data"][1]["start_at"] == 1780358400);
  REQUIRE(after_edit["data"][1]["end_at"] == 1780444800);
  REQUIRE(after_edit["data"][1]["all_day"] == true);
  REQUIRE(after_edit["data"][1]["kind"].is_null());
  REQUIRE(after_edit["data"][1]["description"] == "All day work");

  db.exec("UPDATE cards SET updated_at=1780398000 WHERE card_id="
          "'aaaaaaaa-1111-4111-8111-111111111111';");
  const auto calendar_path = xdg_root / "calendar.json";
  REQUIRE(
      run_command(
          bin + " calendar --from 2026-06-01 --to 2026-06-03 --json > \"" + calendar_path.string() +
          "\""
      ) == 0
  );
  const auto calendar = nlohmann::json::parse(read_text(calendar_path));
  REQUIRE(calendar["ok"] == true);
  REQUIRE(calendar["data"]["project_id"] == "milestone-project");
  REQUIRE(calendar["data"]["from"] == 1780272000);
  REQUIRE(calendar["data"]["to"] == 1780531199);
  REQUIRE(calendar["data"]["milestones"].size() == 2);
  REQUIRE(calendar["data"]["created_cards"].size() == 1);
  REQUIRE(calendar["data"]["updated_cards"].size() == 1);
  REQUIRE(calendar.dump().find(card_id) != std::string::npos);
  REQUIRE(calendar.dump().find(other_id) == std::string::npos);

  const auto calendar_human_path = xdg_root / "calendar.out";
  REQUIRE(
      run_command(
          bin + " calendar --from 2026-06-01 --to 2026-06-03 > \"" + calendar_human_path.string() +
          "\""
      ) == 0
  );
  const auto calendar_human = read_text(calendar_human_path);
  REQUIRE(calendar_human.find("\tmilestone\t" + all_day_id + "\t") != std::string::npos);
  REQUIRE(calendar_human.find("\tmilestone\t" + timed_id + "\t") != std::string::npos);
  REQUIRE(calendar_human.find("\tcard-created\t-\t") != std::string::npos);
  REQUIRE(calendar_human.find("\tcard-updated\t-\t") != std::string::npos);

  const auto invalid_path = xdg_root / "milestone-invalid.json";
  REQUIRE(
      run_command(
          bin + " milestone add 'Release Card' 06/01/2026 --json >/dev/null 2> \"" +
          invalid_path.string() + "\""
      ) == 1
  );
  const auto invalid = nlohmann::json::parse(read_text(invalid_path));
  REQUIRE(invalid["error"]["code"] == "invalid_milestone_time");

  const auto naive_path = xdg_root / "milestone-naive.json";
  REQUIRE(
      run_command(
          bin + " milestone add 'Release Card' 2026-06-01T12:00:00 --json >/dev/null 2> \"" +
          naive_path.string() + "\""
      ) == 1
  );
  const auto naive = nlohmann::json::parse(read_text(naive_path));
  REQUIRE(naive["error"]["code"] == "invalid_milestone_time");

  const auto range_path = xdg_root / "calendar-invalid.json";
  REQUIRE(
      run_command(
          bin + " calendar --from 2026-06-03 --to 2026-06-01 --json >/dev/null 2> \"" +
          range_path.string() + "\""
      ) == 1
  );
  const auto invalid_range = nlohmann::json::parse(read_text(range_path));
  REQUIRE(invalid_range["error"]["code"] == "invalid_calendar_range");

  const auto edit_mixed_path = xdg_root / "milestone-edit-mixed.json";
  REQUIRE(
      run_command(
          bin + " milestone edit 'Release Card' " + timed_id +
          " --start 2026-06-02 --end 2026-06-03T12:00:00Z --json >/dev/null 2> \"" +
          edit_mixed_path.string() + "\""
      ) == 1
  );
  const auto edit_mixed = nlohmann::json::parse(read_text(edit_mixed_path));
  REQUIRE(edit_mixed["error"]["code"] == "invalid_milestone_time");

  const auto missing_edit_path = xdg_root / "milestone-edit-missing.json";
  REQUIRE(
      run_command(
          bin +
          " milestone edit 'Release Card' 00000000-0000-4000-8000-000000000000 --kind x "
          "--json >/dev/null 2> \"" +
          missing_edit_path.string() + "\""
      ) == 1
  );
  const auto missing_edit = nlohmann::json::parse(read_text(missing_edit_path));
  REQUIRE(missing_edit["error"]["code"] == "not_found");

  const auto title_path = xdg_root / "milestone-title.err";
  REQUIRE(
      run_command(
          bin + " milestone add 'Release Card' 2026-06-01 --title Wrong >/dev/null 2> \"" +
          title_path.string() + "\""
      ) == 1
  );
  REQUIRE(read_text(title_path).find("Unknown milestone option: --title") != std::string::npos);

  const auto remove_path = xdg_root / "milestone-remove.json";
  REQUIRE(
      run_command(
          bin + " milestone remove 'Release Card' " + all_day_id + " --json > \"" +
          remove_path.string() + "\""
      ) == 0
  );
  const auto removed = nlohmann::json::parse(read_text(remove_path));
  REQUIRE(removed["data"]["milestone_id"] == all_day_id);
  REQUIRE(removed["data"]["removed"] == true);

  const auto repeat_path = xdg_root / "milestone-remove-repeat.out";
  REQUIRE(
      run_command(
          bin + " milestone remove 'Release Card' " + all_day_id + " > \"" + repeat_path.string() +
          "\""
      ) == 0
  );
  REQUIRE(read_text(repeat_path).find(all_day_id) != std::string::npos);
  REQUIRE(read_text(repeat_path).find("was not present") != std::string::npos);

  const auto ambiguous_path = xdg_root / "milestone-card-ambiguous.json";
  REQUIRE(
      run_command(
          bin + " milestones aaaaaaaa --json >/dev/null 2> \"" + ambiguous_path.string() + "\""
      ) == 1
  );
  const auto ambiguous = nlohmann::json::parse(read_text(ambiguous_path));
  REQUIRE(ambiguous["error"]["code"] == "card_reference_ambiguous");
  REQUIRE(ambiguous.dump().find(card_id) != std::string::npos);
  REQUIRE(ambiguous.dump().find(collision_id) != std::string::npos);

  const auto trashed_path = xdg_root / "milestone-card-trashed.json";
  REQUIRE(
      run_command(
          bin + " milestones 'Trashed Release' --json >/dev/null 2> \"" +
          trashed_path.string() + "\""
      ) == 1
  );
  const auto trashed = nlohmann::json::parse(read_text(trashed_path));
  REQUIRE(trashed["error"]["code"] == "card_reference_not_found");

  REQUIRE(run_command(bin + " milestones 'Other Release' >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl history exposes project and card revision workflows", "[holderctl][history]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "history-root";
  const auto other_root = xdg_root / "history-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "history-project", project_root.string());
  holder::test::create_project(db, "history-other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  constexpr const char* card_id = "12345678-1111-4111-8111-111111111111";
  constexpr const char* ambiguous_one = "aaaaaaaa-1111-4111-8111-111111111111";
  constexpr const char* ambiguous_two = "aaaaaaaa-2222-4222-8222-222222222222";
  constexpr const char* other_card_id = "87654321-3333-4333-8333-333333333333";

  holder::test::create_card_fixture(
      card_store,
      card_id,
      "history-project",
      "History Card",
      "First saved body\n",
      10
  );
  holder::git::GitRepo git;
  git.open_existing(project_root);
  const auto creation_oid = git.head_oid().value();
  card_store.update_content(card_id, "Second saved body\n", std::nullopt, 20);
  const auto edit_oid = git.head_oid().value();
  git.write_file("notes/from-another-tool.txt", "Project history note\n");
  git.stage_path("notes/from-another-tool.txt");
  git.commit("External history note");

  holder::test::create_card_fixture(
      card_store,
      ambiguous_one,
      "history-project",
      "Ambiguous History One",
      "one\n",
      21
  );
  holder::test::create_card_fixture(
      card_store,
      ambiguous_two,
      "history-project",
      "Ambiguous History Two",
      "two\n",
      22
  );
  holder::test::create_card_fixture(
      card_store,
      other_card_id,
      "history-other-project",
      "Other History Card",
      "other\n",
      23
  );

  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, project_root.string().c_str()) == 0);
  git_oid head_oid{};
  REQUIRE(git_reference_name_to_id(&head_oid, raw, "HEAD") == 0);
  git_commit* head = nullptr;
  REQUIRE(git_commit_lookup(&head, raw, &head_oid) == 0);
  const std::string tree_oid = git_oid_tostr_s(git_commit_tree_id(head));
  git_commit_free(head);
  git_repository_free(raw);
  const auto ambiguous_revision =
      write_holderctl_ambiguous_revision_prefix(project_root, tree_oid);

  const std::string token = "historytoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " use history-project >/dev/null") == 0);

  const auto help_path = xdg_root / "history-help.out";
  REQUIRE(run_command(bin + " history --help > \"" + help_path.string() + "\"") == 0);
  CHECK(read_text(help_path).find("holderctl history diff CARD REVISION [REVISION]") !=
        std::string::npos);

  const auto project_json_path = xdg_root / "project-history.json";
  REQUIRE(
      run_command(
          bin + " history --limit 1 --json > \"" + project_json_path.string() + "\""
      ) == 0
  );
  const auto project_page = nlohmann::json::parse(read_text(project_json_path));
  REQUIRE(project_page["ok"] == true);
  REQUIRE(project_page["data"]["activities"].size() == 1);
  REQUIRE(project_page["data"]["activities"][0]["oid"].get<std::string>().size() == 40);
  REQUIRE(project_page["data"]["next_cursor"].is_string());
  const auto next_cursor = project_page["data"]["next_cursor"].get<std::string>();

  const auto next_page_path = xdg_root / "project-history-next.json";
  REQUIRE(
      run_command(
          bin + " history --limit 1 --cursor " + next_cursor + " --json > \"" +
          next_page_path.string() + "\""
      ) == 0
  );
  const auto next_page = nlohmann::json::parse(read_text(next_page_path));
  REQUIRE(next_page["data"]["activities"].size() == 1);
  CHECK(next_page["data"]["activities"][0]["oid"] !=
        project_page["data"]["activities"][0]["oid"]);

  const auto filtered_path = xdg_root / "project-history-filtered.out";
  REQUIRE(
      run_command(
          bin + " history --kind unknown > \"" + filtered_path.string() + "\""
      ) == 0
  );
  const auto filtered = read_text(filtered_path);
  CHECK(filtered.find("REVISION\tAUTHOR\tCOMMITTED\tKINDS\tMESSAGE\n") == 0);
  CHECK(filtered.find("unknown\tExternal history note") != std::string::npos);

  const auto card_list_path = xdg_root / "card-history.out";
  REQUIRE(
      run_command(
          bin + " history 'History Card' --limit 1 > \"" + card_list_path.string() + "\""
      ) == 0
  );
  const auto card_list = read_text(card_list_path);
  CHECK(card_list.find("REVISION\tSAVES\tKIND\tUPDATED\tSUMMARY\n") == 0);
  CHECK(card_list.find(edit_oid.substr(0, 8)) != std::string::npos);
  CHECK(card_list.find("Next cursor: ") != std::string::npos);

  const auto show_path = xdg_root / "history-show.out";
  REQUIRE(
      run_command(
          bin + " history show 'History Card' " + creation_oid.substr(0, 8) + " > \"" +
          show_path.string() + "\""
      ) == 0
  );
  const auto shown = read_text(show_path);
  CHECK(shown.find("Revision: " + creation_oid + "\n") == 0);
  CHECK(shown.find("Title: History Card\n\nFirst saved body\n") != std::string::npos);

  const auto show_json_path = xdg_root / "history-show.json";
  REQUIRE(
      run_command(
          bin + " history show 'History Card' " + edit_oid.substr(0, 8) + " --json > \"" +
          show_json_path.string() + "\""
      ) == 0
  );
  const auto shown_json = nlohmann::json::parse(read_text(show_json_path));
  CHECK(shown_json["data"]["card_id"] == card_id);
  CHECK(shown_json["data"]["snapshot"]["oid"] == edit_oid);
  CHECK(shown_json["data"]["snapshot"]["body"] == "Second saved body\n");

  const auto change_path = xdg_root / "history-change.out";
  REQUIRE(
      run_command(
          bin + " history diff 'History Card' " + edit_oid.substr(0, 8) + " > \"" +
          change_path.string() + "\""
      ) == 0
  );
  const auto change_text = read_text(change_path);
  CHECK(change_text.find("From: " + creation_oid + "\nTo: " + edit_oid + "\n") !=
        std::string::npos);
  CHECK(change_text.find("-First saved body") != std::string::npos);
  CHECK(change_text.find("+Second saved body") != std::string::npos);
  CHECK(change_text.find("+# History Card") == std::string::npos);

  const auto since_path = xdg_root / "history-since.json";
  REQUIRE(
      run_command(
          bin + " history diff 'History Card' " + creation_oid + " " +
          edit_oid.substr(0, 8) + " --json > \"" + since_path.string() + "\""
      ) == 0
  );
  const auto since = nlohmann::json::parse(read_text(since_path));
  CHECK(since["data"]["from"]["oid"] == creation_oid);
  CHECK(since["data"]["to"]["oid"] == edit_oid);

  const auto missing_path = xdg_root / "history-missing.json";
  REQUIRE(
      run_command(
          bin + " history show 'History Card' 00000000 --json >/dev/null 2> \"" +
          missing_path.string() + "\""
      ) == 1
  );
  CHECK(nlohmann::json::parse(read_text(missing_path))["error"]["code"] ==
        "revision_not_found");

  const auto revision_ambiguous_path = xdg_root / "history-revision-ambiguous.json";
  REQUIRE(
      run_command(
          bin + " history diff 'History Card' " + ambiguous_revision +
          " --json >/dev/null 2> \"" + revision_ambiguous_path.string() + "\""
      ) == 1
  );
  CHECK(nlohmann::json::parse(read_text(revision_ambiguous_path))["error"]["code"] ==
        "revision_ambiguous");

  const auto card_ambiguous_path = xdg_root / "history-card-ambiguous.json";
  REQUIRE(
      run_command(
          bin + " history aaaaaaaa --json >/dev/null 2> \"" +
          card_ambiguous_path.string() + "\""
      ) == 1
  );
  const auto card_ambiguous = nlohmann::json::parse(read_text(card_ambiguous_path));
  CHECK(card_ambiguous["error"]["code"] == "card_reference_ambiguous");
  CHECK(card_ambiguous.dump().find(ambiguous_one) != std::string::npos);
  CHECK(card_ambiguous.dump().find(ambiguous_two) != std::string::npos);
  CHECK(run_command(bin + " history 'Other History Card' >/dev/null 2>/dev/null") == 1);

  card_store.trash(card_id, 30);
  const auto trashed_history_path = xdg_root / "trashed-card-history.json";
  REQUIRE(
      run_command(
          bin + " history 'History Card' --json > \"" + trashed_history_path.string() + "\""
      ) == 0
  );
  CHECK(nlohmann::json::parse(read_text(trashed_history_path))["data"]["entries"].size() >= 3);

  const auto restore_missing_path = xdg_root / "history-restore-missing.json";
  REQUIRE(
      run_command(
          bin + " history restore 'History Card' 00000000 --json >/dev/null 2> \"" +
          restore_missing_path.string() + "\""
      ) == 1
  );
  CHECK(nlohmann::json::parse(read_text(restore_missing_path))["error"]["code"] ==
        "revision_not_found");
  REQUIRE(card_store.get(card_id).has_value());
  CHECK(card_store.get(card_id)->deleted_at.has_value());

  const auto restore_json_path = xdg_root / "history-restore.json";
  REQUIRE(
      run_command(
          bin + " history restore 'History Card' " + creation_oid.substr(0, 8) +
          " --json > \"" + restore_json_path.string() + "\""
      ) == 0
  );
  const auto restored = nlohmann::json::parse(read_text(restore_json_path));
  CHECK(restored["data"]["card_id"] == card_id);
  CHECK(restored["data"]["restored_from_oid"] == creation_oid);
  CHECK(restored["data"]["result_oid"].get<std::string>().size() == 40);
  CHECK(restored["data"]["deleted_at"].is_null());
  REQUIRE(card_store.get(card_id).has_value());
  CHECK(card_store.get_content(*card_store.get(card_id)) == "First saved body\n");

  const auto restore_human_path = xdg_root / "history-restore.out";
  REQUIRE(
      run_command(
          bin + " history restore 'History Card' " + edit_oid.substr(0, 8) + " > \"" +
          restore_human_path.string() + "\""
      ) == 0
  );
  const auto restore_human = read_text(restore_human_path);
  CHECK(restore_human.find("Restored card 12345678: History Card\n") == 0);
  CHECK(restore_human.find("Source revision: " + edit_oid + "\n") != std::string::npos);
  CHECK(restore_human.find("Result revision: ") != std::string::npos);
  CHECK(restore_human.find("State: live\n") != std::string::npos);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl card prints a card from the current project", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "project-root";
  const auto other_root = xdg_root / "other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "card-project", project_root.string());
  holder::test::create_project(db, "other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  constexpr const char* card_id = "12345678-1111-4111-8111-111111111111";
  constexpr const char* other_card_id = "87654321-2222-4222-8222-222222222222";
  constexpr const char* ambiguous_card_id_one = "aaaaaaaa-1111-4111-8111-111111111111";
  constexpr const char* ambiguous_card_id_two = "aaaaaaaa-2222-4222-8222-222222222222";
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "card-project";
  card.title = "Card One";
  card.created_at = 10;
  card.updated_at = 11;
  card_store.create(card, "body from holderctl card");

  holder::model::Card other_card;
  other_card.card_id = other_card_id;
  other_card.project_id = "other-project";
  other_card.title = "Card Two";
  other_card.created_at = 12;
  other_card.updated_at = 13;
  card_store.create(other_card, "other body");

  holder::model::Card ambiguous_one;
  ambiguous_one.card_id = ambiguous_card_id_one;
  ambiguous_one.project_id = "card-project";
  ambiguous_one.title = "Collision One";
  ambiguous_one.created_at = 14;
  ambiguous_one.updated_at = 15;
  card_store.create(ambiguous_one, "first collision body");

  holder::model::Card ambiguous_two;
  ambiguous_two.card_id = ambiguous_card_id_two;
  ambiguous_two.project_id = "card-project";
  ambiguous_two.title = "Collision Two";
  ambiguous_two.created_at = 16;
  ambiguous_two.updated_at = 17;
  card_store.create(ambiguous_two, "second collision body");

  const std::string token = "cardtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " card " + card_id + " >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use card-project >/dev/null") == 0);

  const auto card_out = xdg_root / "card.out";
  REQUIRE(run_command(bin + " card " + card_id + " > \"" + card_out.string() + "\"") == 0);
  REQUIRE(read_text(card_out) == "body from holderctl card\n");

  const auto prefix_out = xdg_root / "card-prefix.out";
  REQUIRE(run_command(bin + " card 12345678 > \"" + prefix_out.string() + "\"") == 0);
  REQUIRE(read_text(prefix_out) == "body from holderctl card\n");

  const auto title_out = xdg_root / "card-title.out";
  REQUIRE(run_command(bin + " card \"Card One\" > \"" + title_out.string() + "\"") == 0);
  REQUIRE(read_text(title_out) == "body from holderctl card\n");

  const auto json_path = xdg_root / "card.json";
  REQUIRE(run_command(bin + " card --json 12345678 > \"" + json_path.string() + "\"") == 0);
  const auto payload = nlohmann::json::parse(read_text(json_path));
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["card_id"] == card_id);
  REQUIRE(payload["data"]["content"] == "body from holderctl card");

  const auto ambiguous_error_path = xdg_root / "card-ambiguous.err";
  REQUIRE(
      run_command(bin + " card aaaaaaaa >/dev/null 2> \"" + ambiguous_error_path.string() + "\"") ==
      1
  );
  const auto ambiguous_error = read_text(ambiguous_error_path);
  REQUIRE(ambiguous_error.find("Card reference is ambiguous") != std::string::npos);
  REQUIRE(ambiguous_error.find("aaaaaaaa-1\tCollision One") != std::string::npos);
  REQUIRE(ambiguous_error.find("Collision One") != std::string::npos);
  REQUIRE(ambiguous_error.find("aaaaaaaa-2\tCollision Two") != std::string::npos);
  REQUIRE(ambiguous_error.find("Collision Two") != std::string::npos);
  REQUIRE(ambiguous_error.find(ambiguous_card_id_one) == std::string::npos);
  REQUIRE(ambiguous_error.find(ambiguous_card_id_two) == std::string::npos);

  const auto ambiguous_json_path = xdg_root / "card-ambiguous.json";
  REQUIRE(
      run_command(
          bin + " card --json aaaaaaaa >/dev/null 2> \"" + ambiguous_json_path.string() + "\""
      ) == 1
  );
  const auto ambiguous_json = nlohmann::json::parse(read_text(ambiguous_json_path));
  REQUIRE(ambiguous_json["ok"] == false);
  REQUIRE(ambiguous_json["error"]["code"] == "card_reference_ambiguous");
  REQUIRE(ambiguous_json["error"]["details"]["reference"] == "aaaaaaaa");
  REQUIRE(ambiguous_json["error"]["details"]["candidates"].size() == 2);
  REQUIRE(ambiguous_json.dump().find(ambiguous_card_id_one) != std::string::npos);
  REQUIRE(ambiguous_json.dump().find(ambiguous_card_id_two) != std::string::npos);

  REQUIRE(run_command(bin + " card " + other_card_id + " >/dev/null 2>/dev/null") == 1);
  const auto missing_error_path = xdg_root / "card-missing.err";
  REQUIRE(
      run_command(
          bin + " card \"Missing card\" >/dev/null 2> \"" + missing_error_path.string() + "\""
      ) == 1
  );
  REQUIRE(
      read_text(missing_error_path).find("Card not found in current project: Missing card") !=
      std::string::npos
  );

  const auto missing_json_path = xdg_root / "card-missing.json";
  REQUIRE(
      run_command(
          bin + " card --json 'Missing card' >/dev/null 2> \"" + missing_json_path.string() + "\""
      ) == 1
  );
  const auto missing_json = nlohmann::json::parse(read_text(missing_json_path));
  REQUIRE(missing_json["ok"] == false);
  REQUIRE(missing_json["error"]["code"] == "card_reference_not_found");
  REQUIRE(missing_json["error"]["message"] == "Card was not found in the current project.");
  REQUIRE(missing_json["error"]["details"]["reference"] == "Missing card");
  REQUIRE(run_command(bin + " append " + other_card_id + " extra >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

#ifndef _WIN32
TEST_CASE("holderctl edit opens EDITOR and patches a card in the current project", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "ca'che").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "edit-root";
  const auto other_root = xdg_root / "edit-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "edit-project", project_root.string());
  holder::test::create_project(db, "edit-other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  holder::model::Card card;
  card.card_id = "editable.card";
  card.project_id = "edit-project";
  card.title = "Editable Card";
  card.created_at = 10;
  card.updated_at = 11;
  card_store.create(card, "original body\n");

  holder::model::Card other_card;
  other_card.card_id = "other-editable-card";
  other_card.project_id = "edit-other-project";
  other_card.title = "Other Editable Card";
  other_card.created_at = 12;
  other_card.updated_at = 13;
  card_store.create(other_card, "other body\n");

  const std::string token = "edittoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);

  const auto editor_path = xdg_root / "fake-editor";
  write_fake_editor(editor_path);

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " use edit-project >/dev/null") == 0);

  {
    holder::test::EnvGuard editor_env("EDITOR", editor_path.string());
    holder::test::EnvGuard editor_mode_env("HOLDERCTL_FAKE_EDITOR_MODE", "write");
    holder::test::EnvGuard editor_content_env(
        "HOLDERCTL_FAKE_EDITOR_CONTENT",
        "edited body\nsecond line\n"
    );
    holder::test::EnvGuard editor_exit_env("HOLDERCTL_FAKE_EDITOR_EXIT", "0");

    const auto edit_out = xdg_root / "edit.out";
    REQUIRE(run_command(bin + " edit 'Editable Card' > \"" + edit_out.string() + "\"") == 0);
    REQUIRE(read_text(edit_out) == "Updated card: editable.card\n");
  }

  const auto edited_card_out = xdg_root / "edited-card.out";
  REQUIRE(run_command(bin + " card \"Editable Card\" > \"" + edited_card_out.string() + "\"") == 0);
  REQUIRE(read_text(edited_card_out) == "edited body\nsecond line\n");

  {
    holder::test::EnvGuard editor_env("EDITOR", editor_path.string());
    holder::test::EnvGuard editor_mode_env("HOLDERCTL_FAKE_EDITOR_MODE", "noop");
    holder::test::EnvGuard editor_content_env("HOLDERCTL_FAKE_EDITOR_CONTENT", "");
    holder::test::EnvGuard editor_exit_env("HOLDERCTL_FAKE_EDITOR_EXIT", "0");

    const auto noop_out = xdg_root / "edit-noop.out";
    REQUIRE(run_command(bin + " edit 'Editable Card' > \"" + noop_out.string() + "\"") == 0);
    REQUIRE(read_text(noop_out) == "No changes.\n");
  }

  {
    holder::test::EnvGuard editor_env("EDITOR", editor_path.string());
    holder::test::EnvGuard editor_mode_env("HOLDERCTL_FAKE_EDITOR_MODE", "noop");
    holder::test::EnvGuard editor_content_env("HOLDERCTL_FAKE_EDITOR_CONTENT", "");
    holder::test::EnvGuard editor_exit_env("HOLDERCTL_FAKE_EDITOR_EXIT", "7");

    REQUIRE(run_command(bin + " edit 'Editable Card' >/dev/null 2>/dev/null") == 1);
  }

  {
    holder::test::EnvGuard editor_env("EDITOR", "");
    REQUIRE(run_command(bin + " edit 'Editable Card' >/dev/null 2>/dev/null") == 1);
  }

  REQUIRE(run_command(bin + " edit 'Other Editable Card' >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}
#endif

TEST_CASE("holderctl links backlinks and link expose card links", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "link-root";
  const auto other_root = xdg_root / "link-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "link-project", project_root.string());
  holder::test::create_project(db, "link-other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  holder::model::Card source;
  source.card_id = "source-card";
  source.project_id = "link-project";
  source.title = "Source Card";
  source.created_at = 10;
  source.updated_at = 11;
  card_store.create(source, "source body\n");

  holder::model::Card target;
  target.card_id = "target-card";
  target.project_id = "link-project";
  target.title = "Target Card";
  target.created_at = 12;
  target.updated_at = 13;
  card_store.create(target, "target body\n");

  holder::model::Card other_source;
  other_source.card_id = "other-source-card";
  other_source.project_id = "link-other-project";
  other_source.title = "Other Source Card";
  other_source.created_at = 14;
  other_source.updated_at = 15;
  card_store.create(other_source, "other source body\n");

  holder::model::Card other_target;
  other_target.card_id = "other-target-card";
  other_target.project_id = "link-other-project";
  other_target.title = "Other Target Card";
  other_target.created_at = 16;
  other_target.updated_at = 17;
  card_store.create(other_target, "other target body\n");

  const std::string token = "linktoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " links 'Source Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use link-project >/dev/null") == 0);

  const auto empty_links_out = xdg_root / "links-empty.out";
  REQUIRE(run_command(bin + " links 'Source Card' > \"" + empty_links_out.string() + "\"") == 0);
  REQUIRE(read_text(empty_links_out) == "No links.\n");

  const auto empty_backlinks_out = xdg_root / "backlinks-empty.out";
  REQUIRE(
      run_command(bin + " backlinks 'Target Card' > \"" + empty_backlinks_out.string() + "\"") == 0
  );
  REQUIRE(read_text(empty_backlinks_out) == "No backlinks.\n");

  const auto link_out = xdg_root / "link.out";
  REQUIRE(
      run_command(
          bin + " link 'Source Card' 'Target Card' --kind cite --label 'Related Card' > \"" +
          link_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(link_out) == "Linked card: source-card -> target-card\n");

  const auto links_out = xdg_root / "links.out";
  REQUIRE(run_command(bin + " links 'Source Card' > \"" + links_out.string() + "\"") == 0);
  const auto links_text = read_text(links_out);
  REQUIRE(links_text.find("TO_ID\tTYPE\tKIND\tLABEL\tCREATED\n") == 0);
  REQUIRE(links_text.find("target-card\tcard\tcite\tRelated Card\t") != std::string::npos);

  const auto backlinks_out = xdg_root / "backlinks.out";
  REQUIRE(run_command(bin + " backlinks 'Target Card' > \"" + backlinks_out.string() + "\"") == 0);
  const auto backlinks_text = read_text(backlinks_out);
  REQUIRE(backlinks_text.find("FROM_ID\tTYPE\tKIND\tLABEL\tCREATED\n") == 0);
  REQUIRE(backlinks_text.find("source-card\tcard\tcite\tRelated Card\t") != std::string::npos);

  const auto links_json_out = xdg_root / "links.json";
  REQUIRE(
      run_command(
          bin + " links --json --include-deleted 'Source Card' > \"" + links_json_out.string() +
          "\""
      ) == 0
  );
  const auto links_json = nlohmann::json::parse(read_text(links_json_out));
  REQUIRE(links_json["ok"] == true);
  REQUIRE(links_json["data"].is_array());
  REQUIRE(links_json["data"][0]["to_card_id"] == "target-card");

  const auto backlinks_json_out = xdg_root / "backlinks.json";
  REQUIRE(
      run_command(
          bin + " backlinks --json --include-deleted 'Target Card' > \"" +
          backlinks_json_out.string() + "\""
      ) == 0
  );
  const auto backlinks_json = nlohmann::json::parse(read_text(backlinks_json_out));
  REQUIRE(backlinks_json["ok"] == true);
  REQUIRE(backlinks_json["data"][0]["from_card_id"] == "source-card");

  const auto link_json_out = xdg_root / "link.json";
  REQUIRE(
      run_command(
          bin + " link 'Source Card' 'Target Card' --json > \"" + link_json_out.string() + "\""
      ) == 0
  );
  const auto link_json = nlohmann::json::parse(read_text(link_json_out));
  REQUIRE(link_json["ok"] == true);
  REQUIRE(link_json["data"]["from_card_id"] == "source-card");
  REQUIRE(link_json["data"]["to_card_id"] == "target-card");

  REQUIRE(
      run_command(bin + " link 'Other Source Card' 'Other Target Card' >/dev/null 2>/dev/null") == 1
  );
  REQUIRE(run_command(bin + " link 'Source Card' 'Other Target Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " links 'Other Source Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " backlinks 'Other Target Card' >/dev/null 2>/dev/null") == 1);

  REQUIRE(run_command(bin + " trash 'Source Card' >/dev/null") == 0);
  REQUIRE(run_command(bin + " links 'Source Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " links --include-deleted 'Source Card' >/dev/null") == 0);

  REQUIRE(run_command(bin + " trash 'Target Card' >/dev/null") == 0);
  REQUIRE(run_command(bin + " backlinks 'Target Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " backlinks --include-deleted 'Target Card' >/dev/null") == 0);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl trash and restore manage card deletion lifecycle", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = xdg_root / "trash-root";
  const auto other_root = xdg_root / "trash-other-root";
  std::filesystem::create_directories(project_root);
  std::filesystem::create_directories(other_root);
  holder::test::create_project(db, "trash-project", project_root.string());
  holder::test::create_project(db, "trash-other-project", other_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  auto create_card = [&](const std::string& card_id,
                         const std::string& project_id,
                         const std::string& title,
                         long long updated_at) {
    holder::model::Card card;
    card.card_id = card_id;
    card.project_id = project_id;
    card.title = title;
    card.created_at = updated_at - 1;
    card.updated_at = updated_at;
    card_store.create(card, title + " body\n");
  };

  create_card("trash-card", "trash-project", "Trash Card", 11);
  create_card("delete-card", "trash-project", "Delete Card", 13);
  create_card("json-trash-card", "trash-project", "JSON Trash Card", 14);
  create_card("empty-card-one", "trash-project", "Empty Card One", 15);
  create_card("empty-card-two", "trash-project", "Empty Card Two", 17);
  create_card("other-trash-card", "trash-other-project", "Other Trash Card", 19);

  const std::string token = "trashtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " trash 'Trash Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use trash-project >/dev/null") == 0);

  const auto empty_trash_out = xdg_root / "trash-empty.out";
  REQUIRE(run_command(bin + " trash list > \"" + empty_trash_out.string() + "\"") == 0);
  REQUIRE(read_text(empty_trash_out) == "Trash is empty.\n");

  const auto trash_out = xdg_root / "trash.out";
  REQUIRE(run_command(bin + " trash 'Trash Card' > \"" + trash_out.string() + "\"") == 0);
  REQUIRE(read_text(trash_out) == "Trashed card: trash-card\n");
  REQUIRE(run_command(bin + " card \"Trash Card\" >/dev/null 2>/dev/null") == 1);

  const auto trash_card_json_out = xdg_root / "trash-card.json";
  REQUIRE(
      run_command(
          bin + " trash 'JSON Trash Card' --json > \"" + trash_card_json_out.string() + "\""
      ) == 0
  );
  const auto trash_card_json = nlohmann::json::parse(read_text(trash_card_json_out));
  REQUIRE(trash_card_json["ok"] == true);
  REQUIRE(trash_card_json["data"]["card_id"] == "json-trash-card");

  const auto trash_list_out = xdg_root / "trash-list.out";
  REQUIRE(run_command(bin + " trash list > \"" + trash_list_out.string() + "\"") == 0);
  const auto trash_list = read_text(trash_list_out);
  REQUIRE(trash_list.find("CARD_ID\tTITLE\tDELETED\n") == 0);
  REQUIRE(trash_list.find("trash-card\tTrash Card\t") != std::string::npos);
  REQUIRE(trash_list.find("other-trash-card") == std::string::npos);

  const auto trash_json_out = xdg_root / "trash-list.json";
  REQUIRE(run_command(bin + " trash list --json > \"" + trash_json_out.string() + "\"") == 0);
  const auto trash_json = nlohmann::json::parse(read_text(trash_json_out));
  REQUIRE(trash_json["ok"] == true);
  REQUIRE(trash_json["data"].is_array());
  REQUIRE(trash_json["data"][0]["card_id"] == "trash-card");

  const auto restore_out = xdg_root / "restore.out";
  REQUIRE(run_command(bin + " restore 'Trash Card' > \"" + restore_out.string() + "\"") == 0);
  REQUIRE(read_text(restore_out) == "Restored card: trash-card\n");
  const auto restored_card_out = xdg_root / "restored-card.out";
  REQUIRE(run_command(bin + " card \"Trash Card\" > \"" + restored_card_out.string() + "\"") == 0);
  REQUIRE(read_text(restored_card_out) == "Trash Card body\n");

  REQUIRE(run_command(bin + " trash 'Trash Card' >/dev/null") == 0);
  const auto trash_restore_json_out = xdg_root / "trash-restore.json";
  REQUIRE(
      run_command(
          bin + " trash restore 'Trash Card' --json > \"" + trash_restore_json_out.string() + "\""
      ) == 0
  );
  REQUIRE(nlohmann::json::parse(read_text(trash_restore_json_out))["ok"] == true);

  REQUIRE(run_command(bin + " trash 'Trash Card' >/dev/null") == 0);
  const auto restore_json_out = xdg_root / "restore-json.out";
  REQUIRE(
      run_command(bin + " restore --json 'Trash Card' > \"" + restore_json_out.string() + "\"") == 0
  );
  REQUIRE(nlohmann::json::parse(read_text(restore_json_out))["ok"] == true);

  REQUIRE(run_command(bin + " trash delete 'Trash Card' >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " trash delete 'Other Trash Card' >/dev/null 2>/dev/null") == 1);

  REQUIRE(run_command(bin + " trash 'Delete Card' >/dev/null") == 0);
  const auto delete_json_out = xdg_root / "trash-delete.json";
  REQUIRE(
      run_command(
          bin + " trash delete 'Delete Card' --json > \"" + delete_json_out.string() + "\""
      ) == 0
  );
  REQUIRE(nlohmann::json::parse(read_text(delete_json_out))["ok"] == true);
  REQUIRE(run_command(bin + " restore 'Delete Card' >/dev/null 2>/dev/null") == 1);

  create_card("delete-card-two", "trash-project", "Delete Card Two", 21);
  REQUIRE(run_command(bin + " trash 'Delete Card Two' >/dev/null") == 0);
  const auto delete_out = xdg_root / "trash-delete.out";
  REQUIRE(
      run_command(bin + " trash delete 'Delete Card Two' > \"" + delete_out.string() + "\"") == 0
  );
  REQUIRE(read_text(delete_out) == "Deleted trashed card: delete-card-two\n");
  REQUIRE(run_command(bin + " restore 'Delete Card Two' >/dev/null 2>/dev/null") == 1);

  REQUIRE(run_command(bin + " trash 'Empty Card One' >/dev/null") == 0);
  REQUIRE(run_command(bin + " trash 'Empty Card Two' >/dev/null") == 0);
  const auto empty_json_out = xdg_root / "trash-empty.json";
  REQUIRE(run_command(bin + " trash empty --json > \"" + empty_json_out.string() + "\"") == 0);
  REQUIRE(nlohmann::json::parse(read_text(empty_json_out))["ok"] == true);

  create_card("empty-card-three", "trash-project", "Empty Card Three", 23);
  create_card("empty-card-four", "trash-project", "Empty Card Four", 25);
  REQUIRE(run_command(bin + " trash 'Empty Card Three' >/dev/null") == 0);
  REQUIRE(run_command(bin + " trash 'Empty Card Four' >/dev/null") == 0);
  const auto empty_out = xdg_root / "trash-empty-command.out";
  REQUIRE(run_command(bin + " trash empty > \"" + empty_out.string() + "\"") == 0);
  REQUIRE(read_text(empty_out) == "Emptied card trash.\n");
  const auto emptied_list_out = xdg_root / "trash-emptied-list.out";
  REQUIRE(run_command(bin + " trash list > \"" + emptied_list_out.string() + "\"") == 0);
  REQUIRE(read_text(emptied_list_out) == "Trash is empty.\n");

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl new and append capture cards in Home by default", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto home_root = xdg_root / "home-root";
  std::filesystem::create_directories(home_root);
  holder::test::create_project(db, "home-id", home_root.string());
  {
    holder::project::ProjectRepo repo(db);
    repo.update_name("home-id", "Home", 2);
  }

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const std::string token = "newappendtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto new_out = xdg_root / "new.out";
  REQUIRE(run_command(bin + " new Revise long division > \"" + new_out.string() + "\"") == 0);
  const auto first_card_id = created_card_id_from_output(read_text(new_out));
  REQUIRE(first_card_id.size() == 8);

  const std::string long_title(96, 'A');
  const auto long_new_out = xdg_root / "long-new.out";
  REQUIRE(run_command(bin + " new " + long_title + " > \"" + long_new_out.string() + "\"") == 0);
  const auto long_card_id = created_card_id_from_output(read_text(long_new_out));
  REQUIRE(long_card_id.size() == 8);
  const auto long_json_out = xdg_root / "long-card.json";
  REQUIRE(
      run_command(bin + " card --json " + long_card_id + " > \"" + long_json_out.string() + "\"") ==
      0
  );
  const auto long_card_json = nlohmann::json::parse(read_text(long_json_out));
  REQUIRE(long_card_json["data"]["title"].get<std::string>().size() == 80);
  const auto full_long_card_id = long_card_json["data"]["card_id"].get<std::string>();
  REQUIRE(full_long_card_id.size() == 36);
  REQUIRE(full_long_card_id.rfind(long_card_id, 0) == 0);

  const auto first_card_out = xdg_root / "first-card.out";
  REQUIRE(
      run_command(bin + " card " + first_card_id + " > \"" + first_card_out.string() + "\"") == 0
  );
  REQUIRE(read_text(first_card_out) == "Revise long division\n");

  const auto stdin_new_input = xdg_root / "stdin-new.in";
  {
    std::ofstream out(stdin_new_input);
    out << "Piped title\nbody line\n";
  }
  const auto stdin_new_out = xdg_root / "stdin-new.out";
  REQUIRE(
      run_command(
          bin + " new < \"" + stdin_new_input.string() + "\" > \"" + stdin_new_out.string() + "\""
      ) == 0
  );
  const auto second_card_id = created_card_id_from_output(read_text(stdin_new_out));

  const auto second_card_out = xdg_root / "second-card.out";
  REQUIRE(
      run_command(bin + " card " + second_card_id + " > \"" + second_card_out.string() + "\"") == 0
  );
  REQUIRE(read_text(second_card_out) == "Piped title\nbody line\n");

  const auto append_input = xdg_root / "append.in";
  {
    std::ofstream out(append_input);
    out << "extra line\n";
  }
  const auto append_out = xdg_root / "append.out";
  REQUIRE(
      run_command(
          bin + " append 'Revise long division' < \"" + append_input.string() + "\" > \"" +
          append_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(append_out) == "Appended to card: " + first_card_id + "\n");

  const auto appended_card_out = xdg_root / "appended-card.out";
  REQUIRE(
      run_command(bin + " card " + first_card_id + " > \"" + appended_card_out.string() + "\"") == 0
  );
  REQUIRE(read_text(appended_card_out) == "Revise long division\n\nextra line\n");

  const auto append_args_out = xdg_root / "append-args.out";
  const auto first_card_prefix = first_card_id.substr(0, 8);
  REQUIRE(
      run_command(
          bin + " append " + first_card_prefix + " Revise binary trees > \"" +
          append_args_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(append_args_out) == "Appended to card: " + first_card_id + "\n");

  const auto appended_args_card_out = xdg_root / "appended-args-card.out";
  REQUIRE(
      run_command(
          bin + " card " + first_card_id + " > \"" + appended_args_card_out.string() + "\""
      ) == 0
  );
  REQUIRE(
      read_text(appended_args_card_out) ==
      "Revise long division\n\nextra line\n\nRevise binary trees\n"
  );

  REQUIRE(run_command(bin + " new < /dev/null >/dev/null 2>/dev/null") == 1);
  REQUIRE(
      run_command(bin + " append " + first_card_id + " < /dev/null >/dev/null 2>/dev/null") == 1
  );
  const auto config_path = xdg_root / "config" / "holder" / "holderctl.json";
  std::filesystem::create_directories(config_path.parent_path());
  {
    std::ofstream out(config_path);
    out << "{\"current_project_id\":\"missing-id\"}\n";
  }
  REQUIRE(run_command(bin + " new Missing project card >/dev/null 2>/dev/null") == 1);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl resource manages resources in Home by default", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto home_root = xdg_root / "home-root";
  std::filesystem::create_directories(home_root);
  holder::test::create_project(db, "home-id", home_root.string());
  {
    holder::project::ProjectRepo repo(db);
    repo.update_name("home-id", "Home", 2);
  }

  holder::index::FtsIndexer setup_fts(db);
  holder::card::CardStore setup_cards(db, &setup_fts);
  holder::model::Card import_target;
  import_target.card_id = "abcdef12-3456-4789-8abc-def012345678";
  import_target.project_id = "home-id";
  import_target.title = "Import Target";
  import_target.created_at = 3;
  import_target.updated_at = 3;
  setup_cards.create(import_target, "Attach imported resources here.\n");

  const std::string token = "resourcetoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &setup_cards, &setup_fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto empty_list_out = xdg_root / "resources-empty.out";
  REQUIRE(run_command(bin + " resource list > \"" + empty_list_out.string() + "\"") == 0);
  REQUIRE(read_text(empty_list_out) == "No resources.\n");

  const auto add_out = xdg_root / "resource-add.out";
  REQUIRE(
      run_command(
          bin + " resource add https://example.com/docs --desc 'Docs link' > \"" +
          add_out.string() + "\""
      ) == 0
  );
  const auto resource_id = created_resource_id_from_output(read_text(add_out));

  const auto add_json_out = xdg_root / "resource-add-json.out";
  REQUIRE(
      run_command(
          bin + " resource add git@github.com:holderteam/example.git --json > \"" +
          add_json_out.string() + "\""
      ) == 0
  );
  const auto added_json = nlohmann::json::parse(read_text(add_json_out));
  REQUIRE(added_json["ok"] == true);
  const auto repo_resource_id = added_json["data"]["resource_id"].get<std::string>();
  REQUIRE_FALSE(repo_resource_id.empty());

  const auto local_dir = xdg_root / "local-dir";
  const auto local_file = xdg_root / "local-file.txt";
  const auto local_image = xdg_root / "diagram.png";
  std::filesystem::create_directories(local_dir);
  {
    std::ofstream out(local_file);
    out << "file\n";
  }
  {
    std::ofstream out(local_image);
    out << "png\n";
  }
  REQUIRE(run_command(bin + " resource add \"" + local_dir.string() + "\" >/dev/null") == 0);
  REQUIRE(run_command(bin + " resource add \"" + local_file.string() + "\" >/dev/null") == 0);
  REQUIRE(run_command(bin + " resource add \"" + local_image.string() + "\" >/dev/null") == 0);
  REQUIRE(run_command(bin + " resource add relative/path.txt >/dev/null") == 0);
  REQUIRE(run_command(bin + " resource add https://example.com/ >/dev/null") == 0);
  REQUIRE(run_command(bin + " resource add bareword >/dev/null") == 0);
  REQUIRE(
      run_command(bin + " resource add 'https://example.com/file.txt?download=1' >/dev/null") == 0
  );
  REQUIRE(run_command(bin + " resource add / >/dev/null") == 0);

  {
    holder::resource::ResourceRepo repo(db);
    holder::model::Resource empty_uri_resource;
    empty_uri_resource.resource_id = "empty-uri";
    empty_uri_resource.project_id = "home-id";
    empty_uri_resource.type = "website";
    empty_uri_resource.label = "empty uri";
    empty_uri_resource.created_at = 20;
    empty_uri_resource.updated_at = 20;
    repo.add(empty_uri_resource);
  }

  const auto list_out = xdg_root / "resources.out";
  REQUIRE(run_command(bin + " resource list > \"" + list_out.string() + "\"") == 0);
  const auto list_text = read_text(list_out);
  REQUIRE(list_text.find("RESOURCE_ID\tKIND\tLABEL\tURI\n") != std::string::npos);
  REQUIRE(
      list_text.find(resource_id + "\turl\tdocs\thttps://example.com/docs\n") != std::string::npos
  );
  REQUIRE(
      list_text.find(
          repo_resource_id + "\trepo\texample.git\tgit@github.com:holderteam/example.git\n"
      ) != std::string::npos
  );
  REQUIRE(list_text.find("\tdir\tlocal-dir\t" + local_dir.string() + "\n") != std::string::npos);
  REQUIRE(
      list_text.find("\tfile\tlocal-file.txt\t" + local_file.string() + "\n") != std::string::npos
  );
  REQUIRE(
      list_text.find("\timage\tdiagram.png\t" + local_image.string() + "\n") != std::string::npos
  );
  REQUIRE(list_text.find("\tfile\tpath.txt\trelative/path.txt\n") != std::string::npos);
  REQUIRE(list_text.find("\turl\texample.com\thttps://example.com/\n") != std::string::npos);
  REQUIRE(list_text.find("\turl\tbareword\tbareword\n") != std::string::npos);
  REQUIRE(
      list_text.find("\turl\tfile.txt\thttps://example.com/file.txt?download=1\n") !=
      std::string::npos
  );
  REQUIRE(list_text.find("\tdir\t/\t/\n") != std::string::npos);

  const auto list_json_out = xdg_root / "resources-list-json.out";
  REQUIRE(run_command(bin + " resource list --json > \"" + list_json_out.string() + "\"") == 0);
  REQUIRE(nlohmann::json::parse(read_text(list_json_out))["ok"] == true);

  const auto filtered_out = xdg_root / "resources-filtered.out";
  REQUIRE(
      run_command(bin + " resource list --filter github > \"" + filtered_out.string() + "\"") == 0
  );
  const auto filtered_text = read_text(filtered_out);
  REQUIRE(filtered_text.find(repo_resource_id) != std::string::npos);
  REQUIRE(filtered_text.find(resource_id) == std::string::npos);

  const auto filtered_json_out = xdg_root / "resources-filtered.json";
  REQUIRE(
      run_command(
          bin + " resource list --filter missing --json > \"" + filtered_json_out.string() + "\""
      ) == 0
  );
  const auto filtered_json = nlohmann::json::parse(read_text(filtered_json_out));
  REQUIRE(filtered_json["ok"] == true);
  REQUIRE(filtered_json["data"].empty());

  const auto show_out = xdg_root / "resource-show.out";
  REQUIRE(
      run_command(bin + " resource show " + resource_id + " > \"" + show_out.string() + "\"") == 0
  );
  REQUIRE(
      read_text(show_out) == "Resource: " + resource_id +
                                 "\n"
                                 "Type: url\n"
                                 "Label: docs\n"
                                 "Identifier: https://example.com/docs\n"
                                 "Desc: Docs link\n"
  );

  const auto show_json_out = xdg_root / "resource-show-json.out";
  REQUIRE(
      run_command(
          bin + " resource show --json " + resource_id + " > \"" + show_json_out.string() + "\""
      ) == 0
  );
  const auto show_json = nlohmann::json::parse(read_text(show_json_out));
  REQUIRE(show_json["ok"] == true);
  REQUIRE(show_json["data"]["project_id"] == "home-id");
  REQUIRE(show_json["data"]["label"] == "docs");

  const auto edit_out = xdg_root / "resource-edit.out";
  REQUIRE(
      run_command(
          bin + " resource edit " + resource_id +
          " --label Docs --kind url --uri https://example.com/reference --clear-desc > \"" +
          edit_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(edit_out) == "Updated resource: " + resource_id + "\n");

  const auto edited_json_out = xdg_root / "resource-edited.json";
  REQUIRE(
      run_command(
          bin + " resource show --json " + resource_id + " > \"" + edited_json_out.string() + "\""
      ) == 0
  );
  const auto edited_json = nlohmann::json::parse(read_text(edited_json_out));
  REQUIRE(edited_json["data"]["label"] == "Docs");
  REQUIRE(
      edited_json["data"]["metadata"]["identifier"] ==
      nlohmann::json::array({"https://example.com/reference"})
  );
  REQUIRE(!edited_json["data"]["metadata"].contains("description"));

  const auto edit_desc_json_out = xdg_root / "resource-edit-desc.json";
  REQUIRE(
      run_command(
          bin + " resource edit " + resource_id + " --desc 'Updated description' --json > \"" +
          edit_desc_json_out.string() + "\""
      ) == 0
  );
  REQUIRE(nlohmann::json::parse(read_text(edit_desc_json_out))["ok"] == true);

  const auto delete_out = xdg_root / "resource-delete.out";
  REQUIRE(
      run_command(bin + " resource delete " + resource_id + " > \"" + delete_out.string() + "\"") ==
      0
  );
  REQUIRE(read_text(delete_out) == "Deleted resource: " + resource_id + "\n");

  REQUIRE(run_command(bin + " resource show " + resource_id + " >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource open empty-uri >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource add >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " resource edit " + repo_resource_id + " >/dev/null 2>/dev/null") == 1);
  REQUIRE(
      run_command(
          bin + " resource edit " + repo_resource_id +
          " --desc x --clear-desc >/dev/null 2>/dev/null"
      ) == 1
  );
  REQUIRE(run_command(bin + " resource nope >/dev/null 2>/dev/null") == 1);

  const auto import_source = xdg_root / "import-source.txt";
  {
    std::ofstream out(import_source);
    out << "import me\n";
  }
  const auto import_objects = xdg_root / "import-objects";
  REQUIRE(
      run_command(
          bin + " resource location add-local Imports \"" + import_objects.string() +
          "\" >/dev/null"
      ) == 0
  );
  const auto import_out = xdg_root / "resource-import.out";
  REQUIRE(
      run_command(
          bin + " resource import 'Import Target' \"" + import_source.string() + "\" > \"" +
          import_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(import_out).find("Attached resource: ") == 0);
  REQUIRE(run_command(bin + " trash 'Import Target' >/dev/null") == 0);
  REQUIRE(
      run_command(
          bin + " resource import 'Import Target' \"" + import_source.string() +
          "\" >/dev/null 2>/dev/null"
      ) == 1
  );

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl recovery-token exports and imports encrypted project tokens", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string token = "recoverytoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const auto project_root = xdg_root / "encrypted-project";
  const auto created = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "encrypted-project"},
       {"name", "Encrypted Project"},
       {"root_path", project_root.string()},
       {"privacy_mode", "encrypted_git"},
       {"created_at", 10},
       {"updated_at", 10}},
      boost::beast::http::status::created
  );
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["project_key_id"].is_string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " recovery-token export --pin 1234 >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " use encrypted-project >/dev/null") == 0);

  const auto stdout_token_path = xdg_root / "stdout-token.hrk";
  REQUIRE(
      run_command(
          bin + " recovery-token export --pin 1234 > \"" + stdout_token_path.string() + "\""
      ) == 0
  );
  const auto stdout_token = nlohmann::json::parse(read_text(stdout_token_path));
  REQUIRE(stdout_token["version"] == 1);

  const auto token_path = xdg_root / "project.hrk";
  const auto export_file_out = xdg_root / "export-file.out";
  REQUIRE(
      run_command(
          bin + " recovery-token export --pin 1234 --out \"" + token_path.string() + "\" > \"" +
          export_file_out.string() + "\""
      ) == 0
  );
  REQUIRE(
      read_text(export_file_out).find("Recovery token exported: " + token_path.string()) !=
      std::string::npos
  );
  REQUIRE(nlohmann::json::parse(read_text(token_path))["version"] == 1);
#ifndef _WIN32
  struct stat file_stat {};
  REQUIRE(::stat(token_path.c_str(), &file_stat) == 0);
  REQUIRE((file_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0);
#endif

  const auto import_out = xdg_root / "import.out";
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 1234 --file \"" + token_path.string() + "\" > \"" +
          import_out.string() + "\""
      ) == 0
  );
  REQUIRE(read_text(import_out) == "Recovery token imported for project: encrypted-project\n");
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 9999 --file \"" + token_path.string() +
          "\" >/dev/null 2>/dev/null"
      ) == 1
  );

#ifndef _WIN32
  const auto token_arg = read_text(token_path);
  const auto import_token_out = xdg_root / "import-token.out";
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 1234 --token '" +
          token_arg.substr(0, token_arg.size() - 1) + "' > \"" + import_token_out.string() + "\""
      ) == 0
  );
  REQUIRE(
      read_text(import_token_out) == "Recovery token imported for project: encrypted-project\n"
  );
#endif

  const auto deleted = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/projects/encrypted-project",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(deleted["ok"] == true);

  const auto global_out = xdg_root / "global-import.out";
  REQUIRE(
      run_command(
          bin + " recovery-token import-global --pin 1234 --file \"" + token_path.string() +
          "\" > \"" + global_out.string() + "\""
      ) == 0
  );
  const auto global_text = read_text(global_out);
  REQUIRE(
      global_text.find("Recovery token imported for project: encrypted-project\n") !=
      std::string::npos
  );
  REQUIRE(global_text.find("Project created: yes\n") != std::string::npos);

  REQUIRE(run_command(bin + " recovery-token export >/dev/null 2>/dev/null") == 1);
  REQUIRE(run_command(bin + " recovery-token import --pin 1234 >/dev/null 2>/dev/null") == 1);
  REQUIRE(
      run_command(
          bin + " recovery-token import-global --pin 1234 --file \"" + token_path.string() +
          "\" --token x >/dev/null 2>/dev/null"
      ) == 1
  );
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 1234 --file \"" +
          (xdg_root / "missing.hrk").string() + "\" >/dev/null 2>/dev/null"
      ) == 1
  );
  const auto empty_token_path = xdg_root / "empty.hrk";
  {
    std::ofstream out(empty_token_path);
    out << " \n";
  }
  REQUIRE(
      run_command(
          bin + " recovery-token import --pin 1234 --file \"" + empty_token_path.string() +
          "\" >/dev/null 2>/dev/null"
      ) == 1
  );
  REQUIRE(
      run_command(bin + " recovery-token import --pin 1234 --token '   ' >/dev/null 2>/dev/null") ==
      1
  );
  const auto out_dir = xdg_root / "token-out-dir";
  std::filesystem::create_directories(out_dir);
  REQUIRE(
      run_command(
          bin + " recovery-token export --pin 1234 --out \"" + out_dir.string() +
          "\" >/dev/null 2>/dev/null"
      ) == 1
  );

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl health reports missing or insecure metadata", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " health >/dev/null 2>/dev/null") == 1);

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()));
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
#endif
  REQUIRE(run_command(bin + " health >/dev/null 2>/dev/null") == 1);

  write_server_info_without_token(info_path);
#ifndef _WIN32
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif
  REQUIRE(run_command(bin + " health >/dev/null 2>/dev/null") == 1);
}

TEST_CASE("holderctl logs prints daemon log file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto log_dir = xdg_root / "data" / "holder" / "server" / "logs";
  std::filesystem::create_directories(log_dir);
  const auto log_path = log_dir / "server.log";
  {
    std::ofstream out(log_path);
    out << "one\n"
        << "two\n";
  }

  const auto out_path = xdg_root / "logs.out";
  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" logs > \"" + out_path.string() +
                          "\"";
  REQUIRE(run_command(cmd) == 0);
  REQUIRE(read_text(out_path) == "one\ntwo\n");
}

TEST_CASE("holderctl logs reports missing daemon log file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" logs >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl logs rejects unknown options", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH +
                          "\" logs --bad >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

#ifndef _WIN32
TEST_CASE("holderctl resource open invokes desktop opener for resource URI", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto home_root = xdg_root / "home-root";
  std::filesystem::create_directories(home_root);
  holder::test::create_project(db, "home-id", home_root.string());
  {
    holder::project::ProjectRepo repo(db);
    repo.update_name("home-id", "Home", 2);
  }

  const std::string token = "resourceopentoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_xdg_open(fake_bin / desktop_opener_name_for_tests());

  const auto args_path = xdg_root / "xdg-open.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_XDG_OPEN_ARGS", args_path.string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  const auto add_out = xdg_root / "resource-open-add.out";
  REQUIRE(
      run_command(
          bin + " resource add https://example.com/open-me > \"" + add_out.string() + "\""
      ) == 0
  );
  const auto resource_id = created_resource_id_from_output(read_text(add_out));
  REQUIRE(run_command(bin + " resource open " + resource_id) == 0);
  REQUIRE(read_text(args_path) == "https://example.com/open-me\n");

  {
    holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_XDG_OPEN_EXIT", "9");
    const auto open_fail_out = xdg_root / "resource-open-fail.out";
    REQUIRE(
        run_command(
            bin + " resource open " + resource_id + " > \"" + open_fail_out.string() +
            "\" 2>/dev/null"
        ) == 1
    );
    REQUIRE(read_text(open_fail_out) == "https://example.com/open-me\n");
  }

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl openapi opens Swagger docs with desktop opener", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), 12345);

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_xdg_open(fake_bin / desktop_opener_name_for_tests());

  const auto args_path = xdg_root / "xdg-open.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_XDG_OPEN_ARGS", args_path.string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi") == 0);
  REQUIRE(read_text(args_path) == "http://127.0.0.1:12345/docs\n");
}

TEST_CASE("holderctl openapi reports desktop opener failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_xdg_open(fake_bin / desktop_opener_name_for_tests());

  const auto args_path = xdg_root / "xdg-open.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_XDG_OPEN_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_XDG_OPEN_EXIT", "9");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi >/dev/null 2>/dev/null") == 1);
  REQUIRE(read_text(args_path) == "http://127.0.0.1:11499/docs\n");
}

TEST_CASE("holderctl openapi rejects unknown options", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi --bad >/dev/null 2>/dev/null") == 1);
}

#if defined(__linux__)
TEST_CASE("holderctl restart invokes the Linux user service", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_systemctl(fake_bin / "systemctl");

  const auto args_path = xdg_root / "systemctl.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_SYSTEMCTL_ARGS", args_path.string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " restart") == 0);
  REQUIRE(read_text(args_path) == "--user\nrestart\nholder-daemon.service\n");
}

TEST_CASE("holderctl restart reports service manager failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_systemctl(fake_bin / "systemctl");

  const auto args_path = xdg_root / "systemctl.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_SYSTEMCTL_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_SYSTEMCTL_EXIT", "23");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " restart >/dev/null 2>/dev/null") == 1);
  REQUIRE(read_text(args_path) == "--user\nrestart\nholder-daemon.service\n");
}
#endif

TEST_CASE("holderctl logs follow invokes tail", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_tail(fake_bin / "tail");

  const auto args_path = xdg_root / "tail.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_TAIL_ARGS", args_path.string());

  const auto expected_log_path = xdg_root / "data" / "holder" / "server" / "logs" / "server.log";
  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " logs --follow") == 0);
  REQUIRE(read_text(args_path) == "-f\n" + expected_log_path.string() + "\n");
}

TEST_CASE("holderctl logs follow reports tail failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_tail(fake_bin / "tail");

  const auto args_path = xdg_root / "tail.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH",
      fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{})
  );
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_TAIL_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_TAIL_EXIT", "11");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " logs --follow >/dev/null 2>/dev/null") == 11);
}
#endif
