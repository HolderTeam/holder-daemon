#include "store/CardStore.h"

#include "core/CardPaths.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace holder::store {
namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return {};
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data;
}

} // namespace

CardStore::CardStore(Db& db, holder::git::GitRepo& repo)
    : db_(db), repo_(repo), card_repo_(db) {}

void CardStore::create(holder::model::Card card, const std::string& content) {
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
  if (std::filesystem::exists(full_path)) {
    throw std::runtime_error("conflict: card file already exists");
  }

  repo_.write_file(card.rel_path, content);

  try {
    card_repo_.create(card);
  } catch (...) {
    std::filesystem::remove(repo_.repo_dir() / card.rel_path);
    throw;
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
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  const bool unchanged = (read_file(full_path) == content);

  if (!unchanged) {
    repo_.write_file(card.rel_path, content);
  }

  if (title.has_value()) {
    card_repo_.update_title(card_id, title.value(), updated_at);
  } else {
    card_repo_.touch_updated(card_id, updated_at);
  }

  if (!unchanged) {
    repo_.stage_path(card.rel_path);
    const std::string commit_title = title.has_value() ? title.value() : card.title;
    repo_.commit("Update card " + commit_title);
  }
}

std::optional<holder::model::Card> CardStore::get(const std::string& card_id) const {
  return card_repo_.get(card_id);
}

std::optional<std::string> CardStore::get_content(const holder::model::Card& card) const {
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = repo_.repo_dir() / card.rel_path;
  if (!std::filesystem::exists(full_path)) {
    return std::nullopt;
  }

  return read_file(full_path);
}

} // namespace holder::store
