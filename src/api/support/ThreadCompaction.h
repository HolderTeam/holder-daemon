#pragma once

#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::api::support {

struct ThreadCompactionState {
  std::string thread_id;
  std::optional<std::string> rolling_summary;
  std::optional<std::string> pinned_facts_json;
  std::optional<std::string> last_compacted_message_id;
  long long updated_at = 0;
};

std::optional<ThreadCompactionState> load_thread_compaction_state(holder::store::Db& db,
                                                                  const std::string& thread_id);

void upsert_thread_compaction_state(holder::store::Db& db, const ThreadCompactionState& state);

std::string build_compacted_context(const std::string& context_json,
                                    long long allowed_context_tokens,
                                    const std::optional<ThreadCompactionState>& state,
                                    bool* compacted,
                                    bool* used_summary,
                                    int* pinned_fact_count);

void roll_thread_compaction_state(holder::store::Db& db,
                                  const std::string& thread_id,
                                  const std::string& context_json,
                                  long long updated_at);

} // namespace holder::api::support
