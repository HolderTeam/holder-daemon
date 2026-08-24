#pragma once

#include "api/support/ThreadCompaction.h"
#include "platform/Db.h"

#include <cstddef>

namespace holder::ai {

// Returns false when the thread is not owned by a known project (primarily useful
// for isolated unit tests). Production thread state is always persisted.
bool persist_thread_compaction_state(
    holder::platform::Db& db,
    const holder::api::support::ThreadCompactionState& state
);

std::size_t backfill_thread_compaction_states(holder::platform::Db& db);
std::size_t restore_thread_compaction_states(holder::platform::Db& db);
bool all_thread_compaction_states_are_durable(holder::platform::Db& db);

} // namespace holder::ai
