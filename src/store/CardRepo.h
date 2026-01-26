#pragma once

#include "model/Card.h"
#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class CardRepo {
public:
  explicit CardRepo(Db& db);

  void create(const holder::model::Card& card);
  std::optional<holder::model::Card> get(const std::string& card_id) const;

  std::vector<holder::model::Card> list(const std::string& project_id,
                                        const std::optional<std::string>& parent_card_id) const;

  void update_title(const std::string& card_id, const std::string& title, long long updated_at);
  void touch_updated(const std::string& card_id, long long updated_at);
  void soft_delete(const std::string& card_id, long long deleted_at, long long updated_at);
  void restore(const std::string& card_id, long long updated_at);
  void move(const std::string& card_id,
            const std::optional<std::string>& parent_card_id,
            double sort_key,
            long long updated_at);

private:
  Db& db_;
};

} // namespace holder::store
