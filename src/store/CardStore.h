#pragma once

#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "store/CardRepo.h"
#include "store/Db.h"
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
  std::optional<holder::model::Card> get(const std::string& card_id) const;
  std::optional<std::string> get_content(const holder::model::Card& card);

private:
  holder::model::Project require_project(const std::string& project_id);

  Db& db_;
  holder::git::GitRepo repo_;
  CardRepo card_repo_;
  ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::store
