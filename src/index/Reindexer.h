#pragma once

#include "index/FtsIndexer.h"
#include "platform/Db.h"

namespace holder::index {

class Reindexer {
public:
  explicit Reindexer(holder::store::Db& db);

  // Full reindex from DB into FTS tables.
  void run();

private:
  holder::store::Db& db_;
};

} // namespace holder::index
