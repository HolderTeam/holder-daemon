#pragma once

#include "model/Card.h"

#include <string>

namespace holder::core {

struct ParsedCardFile {
  holder::model::Card card;
  std::string body;
  bool has_front_matter = false;
};

ParsedCardFile parse_card_file(const std::string& raw);

} // namespace holder::core
