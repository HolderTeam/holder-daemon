#pragma once

#include "api/support/EventJournal.h"
#include "platform/Db.h"

#include <map>
#include <string>
#include <tuple>

namespace holder::api::support {

// An invalidation feed over committed snapshots. Intermediate states between
// observations may coalesce; this is deliberately not a mutation/audit journal.
// The observer's connection belongs to its dedicated thread, never an I/O thread.
class ChangeFeed {
 public:
  ChangeFeed(holder::platform::Db& db, EventJournal& journal);
  void poll();
  const nlohmann::json& revisions() const { return revisions_; }

 private:
  using Key = std::tuple<std::string, std::string, std::string>; // kind, id, project
  using Snapshot = std::map<Key, std::string>;
  Snapshot snapshot();
  long long data_version() const;
  nlohmann::json read_revisions();
  holder::platform::Db& db_;
  EventJournal& journal_;
  Snapshot previous_;
  long long version_ = 0;
  nlohmann::json revisions_;
};

} // namespace holder::api::support
