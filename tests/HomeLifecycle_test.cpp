#include "http_test_helpers.h"

#include "api/routes/ProjectRoutes.h"
#include "api/support/CloudQuota.h"
#include "app/Bootstrap.h"
#include "platform/DatabaseRecovery.h"
#include "platform/DeviceConfigStore.h"
#include "platform/Migrations.h"
#include "platform/Paths.h"
#include "platform/ProjectRegistry.h"
#include "privacy/ProjectPrivacy.h"
#include "project/StartupRecovery.h"

#include <iterator>

namespace {

class CwdGuard {
 public:
  explicit CwdGuard(const std::filesystem::path& next)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(next);
  }
  ~CwdGuard() { std::filesystem::current_path(previous_); }

 private:
  std::filesystem::path previous_;
};

std::string read_lifecycle_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct HomeLifecycle {
  // Bootstrap reads config/WELCOME.md; CTest starts in the build's tests directory.
  CwdGuard cwd{std::filesystem::path(__FILE__).parent_path().parent_path()};
  const std::filesystem::path dir = holder::test::make_temp_dir();
  holder::test::EnvGuard data_env{"XDG_DATA_HOME", (dir / "data").string()};
  holder::test::EnvGuard config_env{"XDG_CONFIG_HOME", (dir / "config").string()};
  holder::test::EnvGuard cache_env{"XDG_CACHE_HOME", (dir / "cache").string()};
  holder::test::EnvGuard root_env{"HOLDER_PROJECTS_ROOT", (dir / "projects").string()};
  holder::test::EnvGuard keys_env{"HOLDER_TEST_KEYSTORE_DIR", (dir / "keys").string()};
  const holder::core::Paths paths = holder::core::Paths::resolve("holder");
  holder::platform::Db db;

  HomeLifecycle() {
    paths.ensure_dirs();
    db.open(paths.db_path());
    holder::platform::Migrations::ensure_schema(db, SCHEMA_SQL_PATH);
  }

  holder::model::Project create_home() {
    const auto home = holder::app::bootstrap_default_home_project(db, nullptr);
    REQUIRE(home.has_value());
    REQUIRE(home->privacy_mode == "encrypted_git");
    REQUIRE(home->project_key_id.has_value());
    holder::core::ProjectRegistry(paths.project_registry_path()).remember({*home});
    return *home;
  }

  // Exercise the migration, discovery and bootstrap sequence with a newly opened
  // connection, retaining the on-disk project, registry and test keystore.
  void restart() {
    db.close();
    const bool existed = std::filesystem::exists(paths.db_path());
    db.open(paths.db_path());
    holder::platform::Migrations::ensure_schema(db, SCHEMA_SQL_PATH);
    holder::platform::Migrations::migrate_to_latest(db);
    holder::platform::Migrations::ensure_schema_version(
        db,
        holder::platform::Migrations::latest_schema_version
    );
    holder::app::recover_existing_projects(db, nullptr, paths, !existed);
    holder::app::bootstrap_default_home_project(db, nullptr);
    holder::core::ProjectRegistry(paths.project_registry_path())
        .remember(holder::project::ProjectRepo(db).list());
  }

  std::filesystem::path key_path(const holder::model::Project& home) const {
    return dir / "keys" / (home.project_key_id.value() + ".key");
  }

  void prepare_rebuild() {
    holder::core::initialize_device_config(db, paths.device_config_path());
    holder::api::support::initialize_cloud_usage_ledger(db, paths.cloud_usage_ledger_path());
    holder::core::audit_durable_database_ownership(db, paths);
    holder::core::mark_database_rebuild_ready(paths);
  }

  holder::core::DatabaseRebuildReport rebuild() {
    db.close();
    auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
    return holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false);
  }

  void remove_home(const holder::model::Project& home) {
    namespace http = boost::beast::http;
    const auto target = "/projects/" + home.project_id;
    http::request<http::string_body> req{http::verb::delete_, target, 11};
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::handle_project_routes(
        target,
        req,
        res,
        db,
        nullptr,
        holder::app::generate_uuid_v4,
        [](const std::string&) {
          return std::string();
        }
    ));
    REQUIRE(res.result() == http::status::ok);
    REQUIRE(holder::project::ProjectRepo(db).list().empty());
  }
};

} // namespace

TEST_CASE("Encrypted Home survives restart and schema upgrade", "[home-lifecycle][upgrade]") {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto key_before = read_lifecycle_file(fixture.key_path(original));
  const auto metadata = std::filesystem::path(original.root_path) / ".holder";
  const auto project_before = read_lifecycle_file(metadata / "project.json");
  const auto privacy_before = read_lifecycle_file(metadata / "privacy.json");
  const auto cards_before = holder::card::CardRepo(fixture.db).list_all(original.project_id);
  REQUIRE(cards_before.size() == 1);
  const auto card_before = cards_before.front();
  const auto content_before = holder::card::CardStore(fixture.db, nullptr).get_content(card_before);
  REQUIRE(content_before.has_value());
  const auto encrypted_before = holder::privacy::encrypt_project_blob(
      original.project_id,
      original.project_key_id.value(),
      "Content encrypted before upgrade"
  );

  SECTION("ordinary restart") {}
  SECTION("upgrade from schema v5") {
    // v6 adds only this index; remove it to construct the actual v5 schema.
    fixture.db.exec("DROP INDEX idx_cards_project_card_id;");
    fixture.db.exec("UPDATE schema_version SET version = 5;");
  }

  for (int restart = 0; restart < 2; ++restart) {
    fixture.restart();
    const auto projects = holder::project::ProjectRepo(fixture.db).list();
    REQUIRE(projects.size() == 1);
    const auto& home = projects.front();
    REQUIRE(home.project_id == original.project_id);
    REQUIRE(home.project_key_id == original.project_key_id);
    REQUIRE(home.root_path == original.root_path);
    REQUIRE(home.privacy_mode == "encrypted_git");
    REQUIRE(home.id_scheme == original.id_scheme);
    REQUIRE(read_lifecycle_file(fixture.key_path(home)) == key_before);
    REQUIRE(read_lifecycle_file(metadata / "project.json") == project_before);
    REQUIRE(read_lifecycle_file(metadata / "privacy.json") == privacy_before);
    REQUIRE(
        holder::privacy::decrypt_project_blob(
            home.project_id,
            home.project_key_id.value(),
            encrypted_before
        ) == "Content encrypted before upgrade"
    );
    const auto cards = holder::card::CardRepo(fixture.db).list_all(home.project_id);
    REQUIRE(cards.size() == 1);
    REQUIRE(cards.front().card_id == card_before.card_id);
    REQUIRE(
        holder::card::CardStore(fixture.db, nullptr).get_content(cards.front()) == content_before
    );
    REQUIRE_FALSE(holder::platform::Migrations::migrate_to_latest(fixture.db));
  }
}

TEST_CASE("Encrypted Home restart does not replace a missing key", "[home-lifecycle][upgrade]") {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto encrypted = holder::privacy::encrypt_project_blob(
      original.project_id,
      original.project_key_id.value(),
      "Existing encrypted content"
  );
  // Simulate an unavailable key without touching the real platform keychain.
  std::filesystem::rename(fixture.key_path(original), fixture.dir / "saved-key");
  fixture.restart();
  const auto projects = holder::project::ProjectRepo(fixture.db).list();
  REQUIRE(projects.size() == 1);
  REQUIRE(projects.front().project_id == original.project_id);
  REQUIRE(projects.front().project_key_id == original.project_key_id);
  REQUIRE_FALSE(std::filesystem::exists(fixture.key_path(original)));
  try {
    holder::privacy::decrypt_project_blob(original.project_id, *original.project_key_id, encrypted);
    FAIL("Expected the original missing key to be reported");
  } catch (const holder::privacy::PrivacyError& error) {
    REQUIRE(error.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
  }
  std::filesystem::rename(fixture.dir / "saved-key", fixture.key_path(original));
  REQUIRE(
      holder::privacy::decrypt_project_blob(
          original.project_id,
          *original.project_key_id,
          encrypted
      ) == "Existing encrypted content"
  );
}

TEST_CASE("Encrypted Home survives database reconstruction", "[home-lifecycle][recovery]") {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto old_key = read_lifecycle_file(fixture.key_path(original));
  const auto cards_before = holder::card::CardRepo(fixture.db).list_all(original.project_id);
  REQUIRE(cards_before.size() == 1);
  const auto content_before =
      holder::card::CardStore(fixture.db, nullptr).get_content(cards_before[0]);
  REQUIRE(content_before.has_value());
  fixture.prepare_rebuild();
  fixture.db.close();

  SECTION("explicit rebuild") {}
  SECTION("lost database after reinstall") {
    // Retain the old database as a backup outside the active database path.
    std::filesystem::rename(fixture.paths.db_path(), fixture.dir / "old-holder.db");
  }
  SECTION("damaged database") {
    std::ofstream damaged(fixture.paths.db_path(), std::ios::binary | std::ios::trunc);
    REQUIRE(damaged.is_open());
    damaged << "not a SQLite database";
  }

  const auto report = fixture.rebuild();
  REQUIRE(report.projects == 1);
  REQUIRE(report.cards == 1);
  fixture.restart();
  const auto projects = holder::project::ProjectRepo(fixture.db).list();
  REQUIRE(projects.size() == 1);
  REQUIRE(projects.front().project_id == original.project_id);
  REQUIRE(projects.front().project_key_id == original.project_key_id);
  REQUIRE(read_lifecycle_file(fixture.key_path(original)) == old_key);
  const auto cards = holder::card::CardRepo(fixture.db).list_all(original.project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards.front().card_id == cards_before.front().card_id);
  REQUIRE(
      holder::card::CardStore(fixture.db, nullptr).get_content(cards.front()) == content_before
  );
}

TEST_CASE(
    "Removing encrypted Home does not resurrect it on restart",
    "[home-lifecycle][recovery]"
) {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto old_key = read_lifecycle_file(fixture.key_path(original));
  fixture.prepare_rebuild();
  fixture.remove_home(original);

  SECTION("ordinary restart") {}
  SECTION("upgrade after removal") {
    fixture.db.exec("DROP INDEX idx_cards_project_card_id;");
    fixture.db.exec("UPDATE schema_version SET version = 5;");
  }
  SECTION("database lost before next start") {
    fixture.db.close();
    std::filesystem::rename(fixture.paths.db_path(), fixture.dir / "old-holder.db");
    REQUIRE(fixture.rebuild().projects == 0);
  }
  SECTION("corrupt database before next start") {
    fixture.db.close();
    {
      std::ofstream damaged(fixture.paths.db_path(), std::ios::binary | std::ios::trunc);
      REQUIRE(damaged.is_open());
      damaged << "not a SQLite database";
    }
    REQUIRE(fixture.rebuild().projects == 0);
  }
  fixture.restart();
  const auto projects = holder::project::ProjectRepo(fixture.db).list();
  REQUIRE(projects.size() == 1);
  REQUIRE(projects.front().name == "Home");
  REQUIRE(projects.front().project_id != original.project_id);
  REQUIRE(projects.front().project_key_id != original.project_key_id);
  const auto replacement = projects.front();
  fixture.restart();
  REQUIRE(holder::project::ProjectRepo(fixture.db).list().size() == 1);
  REQUIRE(holder::project::ProjectRepo(fixture.db).get(replacement.project_id).has_value());
  REQUIRE(std::filesystem::exists(
      std::filesystem::path(original.root_path) / ".holder" / "project.json"
  ));
  // Removal must not destroy the key needed to recover retained encrypted files.
  REQUIRE(read_lifecycle_file(fixture.key_path(original)) == old_key);
}

TEST_CASE("A new Home does not reuse a leftover encryption key", "[home-lifecycle][recovery]") {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto old_key = read_lifecycle_file(fixture.key_path(original));
  const auto encrypted_before = holder::privacy::encrypt_project_blob(
      original.project_id,
      *original.project_key_id,
      "Old Home content"
  );
  fixture.remove_home(original);
  // Model manually removing the old project directory while its key survives.
  // Keep the files outside the discovery paths so the test can verify preservation.
  std::filesystem::rename(original.root_path, fixture.dir / "old-home-backup");
  fixture.restart();
  const auto projects = holder::project::ProjectRepo(fixture.db).list();
  REQUIRE(projects.size() == 1);
  const auto& replacement = projects.front();
  REQUIRE(replacement.name == "Home");
  REQUIRE(replacement.project_id != original.project_id);
  REQUIRE(replacement.project_key_id.has_value());
  REQUIRE(replacement.project_key_id != original.project_key_id);
  REQUIRE(read_lifecycle_file(fixture.key_path(original)) == old_key);
  REQUIRE(read_lifecycle_file(fixture.key_path(replacement)) != old_key);
  REQUIRE(
      holder::privacy::decrypt_project_blob(
          original.project_id,
          *original.project_key_id,
          encrypted_before
      ) == "Old Home content"
  );
  REQUIRE_THROWS_AS(
      holder::privacy::decrypt_project_blob(
          replacement.project_id,
          *replacement.project_key_id,
          encrypted_before
      ),
      holder::privacy::PrivacyError
  );
}

TEST_CASE(
    "Failed removal preserves encrypted Home when its registry cannot be written",
    "[home-lifecycle][recovery]"
) {
  HomeLifecycle fixture;
  const auto home = fixture.create_home();
  const auto cards = holder::card::CardRepo(fixture.db).list_all(home.project_id);
  const auto key = read_lifecycle_file(fixture.key_path(home));
  // Make the registry's parent a regular file. This also fails under elevated tests.
  std::filesystem::rename(fixture.paths.config_dir, fixture.dir / "saved-config");
  { std::ofstream(fixture.paths.config_dir) << "blocked"; }
  namespace http = boost::beast::http;
  const auto target = "/projects/" + home.project_id;
  http::request<http::string_body> request{http::verb::delete_, target, 11};
  http::response<http::string_body> response;
  REQUIRE(holder::api::routes::handle_project_routes(
      target,
      request,
      response,
      fixture.db,
      nullptr,
      holder::app::generate_uuid_v4,
      [](const std::string&) {
        return std::string();
      }
  ));
  REQUIRE(response.result() == http::status::internal_server_error);
  REQUIRE(holder::project::ProjectRepo(fixture.db).get(home.project_id).has_value());
  REQUIRE(holder::card::CardRepo(fixture.db).list_all(home.project_id).size() == cards.size());
  REQUIRE(read_lifecycle_file(fixture.key_path(home)) == key);
}

TEST_CASE("Explicit recovery re-registers a removed encrypted Home", "[home-lifecycle][recovery]") {
  HomeLifecycle fixture;
  const auto original = fixture.create_home();
  const auto token = holder::privacy::export_recovery_token(
      original.project_id,
      *original.project_key_id,
      "1234",
      original.name
  );
  fixture.remove_home(original);
  namespace http = boost::beast::http;
  const std::string target = "/recovery-token/import";
  http::request<http::string_body> request{http::verb::post, target, 11};
  request.body() = nlohmann::json{{"pin", "1234"}, {"recovery_token", token}}.dump();
  request.prepare_payload();
  http::response<http::string_body> response;
  REQUIRE(holder::api::routes::handle_project_routes(
      target,
      request,
      response,
      fixture.db,
      nullptr,
      holder::app::generate_uuid_v4,
      [](const std::string&) {
        return std::string();
      }
  ));
  INFO(response.body());
  REQUIRE(response.result() == http::status::created);
  const auto restored = holder::project::ProjectRepo(fixture.db).get(original.project_id);
  REQUIRE(restored.has_value());
  REQUIRE(restored->project_key_id == original.project_key_id);
  const auto roots = holder::core::ProjectRegistry(fixture.paths.project_registry_path())
                         .discover_roots(fixture.dir / "projects");
  REQUIRE(roots.size() == 1);
  REQUIRE(roots.front() == std::filesystem::weakly_canonical(restored->root_path));
  fixture.restart();
  REQUIRE(holder::project::ProjectRepo(fixture.db).list().size() == 1);
  REQUIRE(holder::project::ProjectRepo(fixture.db).get(original.project_id).has_value());
}
