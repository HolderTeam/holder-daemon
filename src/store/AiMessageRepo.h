#pragma once

#include "core/Fs.h"
#include "index/FtsIndexer.h"
#include "git/GitRepo.h"
#include "model/AiMessage.h"
#include "store/AiThreadRepo.h"
#include "store/Db.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class AiMessageRepo {
public:
  AiMessageRepo(Db& db, holder::index::FtsIndexer* fts, holder::core::Fs* fs = nullptr);

  void append(const holder::model::AiMessage& message);
  std::optional<holder::model::AiMessage> get(const std::string& message_id) const;
  std::vector<holder::model::AiMessage> list_by_thread(const std::string& thread_id) const;
  void update_links(const std::string& message_id);
  void update(const holder::model::AiMessage& message);
  std::vector<holder::model::AiMessage> list_deleted_by_project(const std::string& project_id) const;
  void trash(const std::string& message_id, long long deleted_at);
  void restore(const std::string& message_id);
  void remove(const std::string& message_id);

private:
  Db& db_;
  holder::git::GitRepo repo_;
  holder::core::Fs* fs_ = nullptr;
  holder::store::LinkRepo link_repo_;
  holder::store::AiThreadRepo thread_repo_;
  holder::store::ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::store
