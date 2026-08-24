#pragma once

#include "platform/Db.h"

#include <cstddef>
#include <string>

namespace holder::ai {

bool persist_nudge_dismissal(holder::platform::Db& db, const std::string& nudge_id);
std::size_t backfill_nudge_dismissals(holder::platform::Db& db);
std::size_t restore_nudge_dismissals(holder::platform::Db& db);
bool all_nudge_dismissals_are_durable(holder::platform::Db& db);

} // namespace holder::ai
