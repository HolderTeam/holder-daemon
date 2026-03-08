#include "privacy/ProjectPrivacy.h"
#include "http_test_helpers.h"
#include "git/GitOps.h"
#include "model/Project.h"
#include "privacy/CryptoService.h"
#include "project/ProjectRepo.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {

std::filesystem::path make_temp_dir_local() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto dir = base / ("holder_project_privacy_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << text;
}

} // namespace

TEST_CASE("Privacy safety check passes when cards directory is missing", "[privacy]") {
  const auto root = make_temp_dir_local();
  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == true);
  REQUIRE(check.checked_files == 0);
  REQUIRE(check.unsafe_paths.empty());
}

TEST_CASE("Privacy safety check reports plaintext card blobs", "[privacy]") {
  const auto root = make_temp_dir_local();
  write_file(root / "cards" / "ab" / "plain.md", "# title\nhello\n");

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == false);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.size() == 1);
  REQUIRE(check.unsafe_paths[0] == "cards/ab/plain.md");
  REQUIRE_THROWS(holder::privacy::assert_encryption_push_safe(root.string()));
}

TEST_CASE("Privacy safety check accepts HolderPriv1 envelope blobs", "[privacy]") {
  const auto root = make_temp_dir_local();
  write_file(root / "cards" / "ab" / "enc.md", "HolderPriv1\n{}\nAA==\n");

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == true);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.empty());
  REQUIRE_NOTHROW(holder::privacy::assert_encryption_push_safe(root.string()));
}

TEST_CASE("ensure_encrypted_project_ready stores 32-byte privacy key material", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() { return std::string("key-1"); });

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const auto key_path = dir / "keystore" / (fetched->project_key_id.value() + ".key");
  REQUIRE(std::filesystem::exists(key_path));

  std::ifstream in(key_path, std::ios::binary);
  REQUIRE(in.good());
  std::string key_b64((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  const auto decoded = holder::privacy::key_from_base64(key_b64);
  REQUIRE(decoded.size() == holder::privacy::kPrivacyKeyBytes);
}

TEST_CASE("staged card blob fails index safety check without explicit encryption", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() { return std::string("key-1"); });

  const std::string rel = "cards/aa/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa.md";
  git.write_file(rel, "# hello\nworld\n");
  git.stage_path(rel);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(project.root_path, {rel}),
      holder::privacy::PrivacyError);
}

TEST_CASE("staged plaintext card blob fails index safety check", "[privacy]") {
  const auto dir = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(dir);
  const std::string rel = "cards/aa/plain.md";
  git.write_file(rel, "# plain\n");
  git.stage_path(rel);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(dir.string(), {rel}),
      holder::privacy::PrivacyError);
}

TEST_CASE("recovery token import wrong PIN returns typed privacy error", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() { return std::string("key-1"); });

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const std::string key_id = fetched->project_key_id.value();

  const std::string token = holder::privacy::export_recovery_token("proj-1", key_id, "1234");
  try {
    holder::privacy::import_recovery_token(repo, "proj-1", "wrong", token, 3);
    FAIL("Expected privacy error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("export_recovery_token rejects empty PIN", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-pin-empty";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() { return std::string("key-empty-pin"); });

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  REQUIRE_THROWS(holder::privacy::export_recovery_token(project.project_id,
                                                        fetched->project_key_id.value(),
                                                        ""));
}

TEST_CASE("export_recovery_token fails when key material is missing from test keystore", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "empty-keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);

  REQUIRE_THROWS_AS(
      holder::privacy::export_recovery_token("proj-missing", "key-missing", "1234"),
      holder::privacy::PrivacyError);
}

TEST_CASE("inspect_recovery_token maps wrong PIN to RecoveryTokenInvalid", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-inspect";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() { return std::string("key-inspect"); });

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());

  const std::string token = holder::privacy::export_recovery_token(
      project.project_id,
      fetched->project_key_id.value(),
      "1234",
      std::optional<std::string>("Home"),
      std::optional<std::string>("git@example.com:org/repo.git"));

  try {
    (void)holder::privacy::inspect_recovery_token("wrong", token);
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("inspect_recovery_token maps malformed token JSON to RecoveryTokenInvalid", "[privacy]") {
  try {
    (void)holder::privacy::inspect_recovery_token("1234", "{not-json");
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("privacy safety check treats unreadable card file as unsafe", "[privacy]") {
  const auto root = make_temp_dir_local();
  const auto path = root / "cards" / "ab" / "locked.md";
  write_file(path, "HolderPriv1\n{}\nAA==\n");
  std::filesystem::permissions(path,
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == false);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.size() == 1);
}

TEST_CASE("index safety check ignores non-card paths and missing staged card paths", "[privacy]") {
  const auto root = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(root);

  REQUIRE_NOTHROW(holder::privacy::assert_encryption_index_paths_safe(
      root.string(),
      {"notes/a.md", "cards/aa/missing.md"}));
}

TEST_CASE("index safety check errors when repository cannot be opened", "[privacy]") {
  const auto root = make_temp_dir_local();
  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(root.string(), {"cards/aa/a.md"}),
      holder::privacy::PrivacyError);
}
