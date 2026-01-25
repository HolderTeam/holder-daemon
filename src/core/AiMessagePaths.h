#pragma once

#include <string>

namespace holder::core {

// Compute ai_messages/<first2>/<next2>/<uuid>.md
std::string ai_message_rel_path(const std::string& message_id);

} // namespace holder::core
