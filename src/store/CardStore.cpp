#include "store/CardStore.h"

#include "core/CardFrontMatter.h"
#include "core/CardPaths.h"
#include "core/Fs.h"
#include "store/LinkRepo.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <stdexcept>
namespace holder::store {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
}

void write_card_file(holder::git::GitRepo& repo,
                     const holder::model::Card& card,
                     const std::vector<holder::model::CardLink>& links,
                     const std::string& content) {
  const auto front_matter = holder::core::render_card_front_matter(card, links);
  repo.write_file(card.rel_path, front_matter + content);
}

} // namespace

CardStore::CardStore(Db& db, holder::index::FtsIndexer* fts, holder::core::Fs* fs)
    : db_(db),
      repo_(),
      fs_(&resolve_fs(fs)),
      card_repo_(db),
      link_repo_(db),
      project_repo_(db),
      fts_(fts) {}

holder::model::Project CardStore::require_project(const std::string& project_id) {
  const auto project_opt = project_repo_.get(project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found: " + project_id);
  }
  repo_.open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    repo_.set_remote("origin", project_opt->git_remote_url.value());
  }
  return project_opt.value();
}

void CardStore::create(holder::model::Card card, const std::string& content) {
  require_project(card.project_id);

  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path.empty()) {
    card.rel_path = expected;
  } else if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  if (card_repo_.get(card.card_id).has_value()) {
    throw std::runtime_error("conflict: card_id already exists");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  if (fs_->exists(full_path)) {
    throw std::runtime_error("conflict: card file already exists");
  }

  const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);
  write_card_file(repo_, card, links, content);

  try {
    card_repo_.create(card);
  } catch (...) {
    std::filesystem::remove(repo_.repo_dir() / card.rel_path);
    throw;
  }

  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, card.title, content);
  }

  repo_.stage_path(card.rel_path);
  repo_.commit("Add card " + card.title);
}

void CardStore::update_content(const std::string& card_id,
                               const std::string& content,
                               const std::optional<std::string>& title,
                               long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  const auto& card = card_opt.value();
  require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  bool unchanged = false;
  if (fs_->exists(full_path)) {
    unchanged = (holder::core::parse_card_file(fs_->read_file(full_path)).body == content);
  }

  if (!unchanged) {
    auto updated_card = card;
    if (title.has_value()) {
      updated_card.title = title.value();
    }
    updated_card.updated_at = updated_at;
    const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);
    write_card_file(repo_, updated_card, links, content);
  }

  if (title.has_value()) {
    card_repo_.update_title(card_id, title.value(), updated_at);
  } else {
    card_repo_.touch_updated(card_id, updated_at);
  }

  const std::string fts_title = title.has_value() ? title.value() : card.title;
  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, fts_title, content);
  }

  if (!unchanged) {
    repo_.stage_path(card.rel_path);
    const std::string commit_title = title.has_value() ? title.value() : card.title;
    repo_.commit("Update card " + commit_title);
  }
}

void CardStore::update_links(const std::string& card_id, long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  auto card = card_opt.value();
  require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto raw = fs_->read_file(full_path);
  const auto parsed = holder::core::parse_card_file(raw);
  const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);

  card.updated_at = updated_at;
  const auto updated_raw = holder::core::render_card_front_matter(card, links) + parsed.body;

  if (updated_raw == raw) {
    return;
  }

  repo_.write_file(card.rel_path, updated_raw);
  card_repo_.touch_updated(card_id, updated_at);
  repo_.stage_path(card.rel_path);
  repo_.commit("Update links for " + card.title);
}

void CardStore::trash(const std::string& card_id, long long deleted_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (card.deleted_at.has_value()) {
    throw std::runtime_error("card already deleted");
  }

  require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto src_path = repo_.repo_dir() / card.rel_path;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("card content missing");
  }

  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto dst_path = repo_.repo_dir() / trash_rel;
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  card_repo_.soft_delete(card_id, deleted_at, deleted_at);
  if (fts_) {
    fts_->delete_card(card_id);
  }
  repo_.remove_path(card.rel_path);
  repo_.stage_path(trash_rel);
  repo_.commit("Delete card " + card.title);
}

void CardStore::restore(const std::string& card_id, long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (!card.deleted_at.has_value()) {
    throw std::runtime_error("card is not deleted");
  }

  require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto src_path = repo_.repo_dir() / trash_rel;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto dst_path = repo_.repo_dir() / card.rel_path;
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  card_repo_.restore(card_id, updated_at);
  if (fts_) {
    const auto raw = fs_->read_file(dst_path);
    const auto parsed = holder::core::parse_card_file(raw);
    fts_->upsert_card(card.card_id, card.project_id, card.title, parsed.body);
  }
  repo_.remove_path(trash_rel);
  repo_.stage_path(card.rel_path);
  repo_.commit("Restore card " + card.title);
}

void CardStore::hard_delete(const std::string& card_id) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (!card.deleted_at.has_value()) {
    throw std::runtime_error("card is not deleted");
  }

  require_project(card.project_id);
  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto trash_path = repo_.repo_dir() / trash_rel;
  if (fs_->exists(trash_path)) {
    fs_->remove(trash_path);
    repo_.remove_path(trash_rel);
  }

  link_repo_.delete_links_from(card.project_id, card.card_id);
  link_repo_.delete_links_to_typed(card.project_id, card.card_id, "card");
  card_repo_.remove(card.card_id);
  repo_.commit("Permanently delete card " + card.title);
}

std::optional<holder::model::Card> CardStore::get(const std::string& card_id) const {
  return card_repo_.get(card_id);
}

std::optional<std::string> CardStore::get_content(const holder::model::Card& card) {
  const auto project_opt = project_repo_.get(card.project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found: " + card.project_id);
  }
  repo_.open_or_init(project_opt->root_path);

  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    return std::nullopt;
  }

  return holder::core::parse_card_file(fs_->read_file(full_path)).body;
}

} // namespace holder::store
