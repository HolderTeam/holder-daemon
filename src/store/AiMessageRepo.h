#pragma once

#include "index/FtsIndexer.h"
#include "git/GitRepo.h"
#include "model/AiMessage.h"
#include "store/AiThreadRepo.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"

#include <string>
#include <vector>

namespace holder::store {

class AiMessageRepo {
public:
  AiMessageRepo(Db& db, holder::index::FtsIndexer* fts);

  void append(const holder::model::AiMessage& message);
  std::vector<holder::model::AiMessage> list_by_thread(const std::string& thread_id) const;

private:
  Db& db_;
  holder::git::GitRepo repo_;
  holder::store::AiThreadRepo thread_repo_;
  holder::store::ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::store
