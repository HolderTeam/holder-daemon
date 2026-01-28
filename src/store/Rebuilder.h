#pragma once

#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "store/Db.h"

#include <cstddef>

namespace holder::store {

class Rebuilder {
public:
  Rebuilder(Db& db, holder::index::FtsIndexer* fts);

  struct RebuildStats {
    std::size_t cards = 0;
    std::size_t ai_messages = 0;
    std::size_t ai_threads = 0;
    std::size_t links = 0;
  };

  RebuildStats rebuild_project(const holder::model::Project& project);

private:
  Db& db_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::store
