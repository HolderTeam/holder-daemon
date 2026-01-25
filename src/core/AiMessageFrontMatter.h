#pragma once

#include "model/AiMessage.h"

#include <string>

namespace holder::core {

struct ParsedAiMessageFile {
  holder::model::AiMessage message;
  std::string project_id;
  std::string body;
  bool has_front_matter = false;
};

std::string render_ai_message_front_matter(const holder::model::AiMessage& message,
                                           const std::string& project_id);
ParsedAiMessageFile parse_ai_message_file(const std::string& raw);

} // namespace holder::core
