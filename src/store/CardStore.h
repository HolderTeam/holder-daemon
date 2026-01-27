#pragma once

#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "store/CardRepo.h"
#include "store/Db.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"

#include <optional>
#include <string>

namespace holder::store {

class CardStore {
public:
  CardStore(Db& db, holder::index::FtsIndexer* fts);

  void create(holder::model::Card card, const std::string& content);
  void update_content(const std::string& card_id,
                      const std::string& content,
                      const std::optional<std::string>& title,
                      long long updated_at);
  void update_links(const std::string& card_id, long long updated_at);
  void trash(const std::string& card_id, long long deleted_at);
  void restore(const std::string& card_id, long long updated_at);
  void hard_delete(const std::string& card_id);
  std::optional<holder::model::Card> get(const std::string& card_id) const;
  std::optional<std::string> get_content(const holder::model::Card& card);

private:
  holder::model::Project require_project(const std::string& project_id);

  Db& db_;
  holder::git::GitRepo repo_;
  CardRepo card_repo_;
  LinkRepo link_repo_;
  ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::store
