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

struct SummaryValidationResult {
  bool accepted = false;
  std::string summary;
  std::string reason;
  int extracted_items = 0;
  bool used_fallback_sections = false;
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

std::string build_structured_summary_refresh_prompt(
    const std::optional<std::string>& current_summary,
    const std::string& new_context);

SummaryValidationResult normalize_and_validate_rolling_summary(
    const std::string& candidate_summary,
    const std::optional<std::string>& previous_summary,
    long long max_summary_chars);

} // namespace holder::api::support
