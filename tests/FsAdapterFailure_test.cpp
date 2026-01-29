#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/AiMessagePaths.h"
#include "core/CardFrontMatter.h"
#include "core/CardPaths.h"
#include "core/Fs.h"
#include "index/FtsIndexer.h"
#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Card.h"
#include "model/Project.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/CardStore.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"
#include "store/Rebuilder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_fs_adapter_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::store::Db& db) {
  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::store::Db& db,
                    const std::string& project_id,
                    const std::string& root_path) {
  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

struct FailingFs final : holder::core::Fs {
  holder::core::RealFs real;
  std::optional<std::filesystem::path> fail_rename_from;
  std::optional<std::filesystem::path> fail_read_path;

  bool exists(const std::filesystem::path& path) const override {
    return real.exists(path);
  }
  void create_directories(const std::filesystem::path& path) const override {
    real.create_directories(path);
  }
  void rename(const std::filesystem::path& from,
              const std::filesystem::path& to) const override {
    if (fail_rename_from.has_value() && from == fail_rename_from.value()) {
      throw std::runtime_error("rename failed");
    }
    real.rename(from, to);
  }
  void remove(const std::filesystem::path& path) const override {
    real.remove(path);
  }
  long long last_write_time_seconds(const std::filesystem::path& path) const override {
    return real.last_write_time_seconds(path);
  }
  std::string read_file(const std::filesystem::path& path) const override {
    if (fail_read_path.has_value() && path == fail_read_path.value()) {
      throw std::runtime_error("read failed");
    }
    return real.read_file(path);
  }
  void write_file(const std::filesystem::path& path,
                  const std::string& content) const override {
    real.write_file(path, content);
  }
};

} // namespace

TEST_CASE("CardStore trash propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::store::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::store::CardStore store(db, &fts, &fs);

  holder::model::Card card;
  card.card_id = "deadbeef";
  card.project_id = "proj-1";
  card.title = "Fail";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto src_path = project_root / rel_path;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(store.trash(card.card_id, 10));
  REQUIRE(std::filesystem::exists(src_path));

  holder::store::CardRepo repo(db);
  const auto fetched = repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(!fetched->deleted_at.has_value());
}

TEST_CASE("AiMessageRepo trash propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::store::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::store::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::store::AiMessageRepo repo(db, &fts, &fs);

  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;
  repo.append(msg);

  const auto rel_path = holder::core::ai_message_rel_path(msg.message_id);
  const auto src_path = project_root / rel_path;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(repo.trash(msg.message_id, 10));
  REQUIRE(std::filesystem::exists(src_path));

  const auto fetched = repo.get(msg.message_id);
  REQUIRE(fetched.has_value());
  REQUIRE(!fetched->deleted_at.has_value());
}

TEST_CASE("Rebuilder propagates fs read failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::store::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  holder::model::Card card;
  card.card_id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  card.project_id = project_id;
  card.title = "Card";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 1;
  card.updated_at = 1;
  const auto raw = holder::core::render_card_front_matter(card, {}) + "body";
  const auto full_path = root / card.rel_path;
  std::filesystem::create_directories(full_path.parent_path());
  std::ofstream out(full_path);
  out << raw;
  out.close();

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  fs.fail_read_path = full_path;

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  holder::store::Rebuilder rebuilder(db, &fts, &fs);
  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}
