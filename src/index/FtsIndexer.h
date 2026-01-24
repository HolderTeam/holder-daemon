#pragma once

#include "store/Db.h"

#include <string>

namespace holder::index {

class FtsIndexer {
public:
  explicit FtsIndexer(holder::store::Db& db);

  void upsert_card(const std::string& card_id,
                   const std::string& project_id,
                   const std::string& title,
                   const std::string& body);
  void delete_card(const std::string& card_id);

  void upsert_message(const std::string& message_id,
                      const std::string& thread_id,
                      const std::string& project_id,
                      const std::string& content);
  void delete_message(const std::string& message_id);

private:
  holder::store::Db& db_;
};

} // namespace holder::index
