#pragma once

#include "model/AiMessage.h"
#include "store/Db.h"

#include <string>
#include <vector>

namespace holder::store {

class AiMessageRepo {
public:
  explicit AiMessageRepo(Db& db);

  void append(const holder::model::AiMessage& message);
  std::vector<holder::model::AiMessage> list_by_thread(const std::string& thread_id) const;

private:
  Db& db_;
};

} // namespace holder::store
