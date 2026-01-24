#pragma once

#include "git/GitRepo.h"
#include "model/Card.h"
#include "store/CardRepo.h"
#include "store/Db.h"

#include <optional>
#include <string>

namespace holder::store {

class CardStore {
public:
  CardStore(Db& db, holder::git::GitRepo& repo);

  void create(holder::model::Card card, const std::string& content);
  void update_content(const std::string& card_id,
                      const std::string& content,
                      const std::optional<std::string>& title,
                      long long updated_at);
  std::optional<holder::model::Card> get(const std::string& card_id) const;
  std::optional<std::string> get_content(const holder::model::Card& card) const;

private:
  Db& db_;
  holder::git::GitRepo& repo_;
  CardRepo card_repo_;
};

} // namespace holder::store
