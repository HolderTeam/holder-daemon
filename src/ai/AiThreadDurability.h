#pragma once

#include "model/AiThread.h"
#include "platform/Db.h"

#include <cstddef>
#include <string>

namespace holder::ai {

void persist_ai_thread(
    holder::platform::Db& db,
    const holder::model::AiThread& thread,
    const std::string& commit_message = "Update AI thread metadata"
);
void remove_ai_thread_manifest(holder::platform::Db& db, const holder::model::AiThread& thread);
std::size_t backfill_ai_thread_manifests(holder::platform::Db& db);

} // namespace holder::ai
