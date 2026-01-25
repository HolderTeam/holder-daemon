#include "core/CardFrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <string_view>

namespace holder::core {
namespace {

constexpr std::string_view kDelimiter = "---\n";

} // namespace

ParsedCardFile parse_card_file(const std::string& raw) {
  ParsedCardFile parsed;
  parsed.body = raw;

  if (raw.rfind(kDelimiter, 0) != 0) {
    return parsed;
  }

  const auto end = raw.find("\n---\n", kDelimiter.size());
  if (end == std::string::npos) {
    return parsed;
  }

  const std::string yaml_text = raw.substr(kDelimiter.size(), end - kDelimiter.size());
  try {
    const auto node = YAML::Load(yaml_text);
    if (!node || !node.IsMap()) {
      return parsed;
    }

    parsed.has_front_matter = true;
    auto& card = parsed.card;

    if (node["card_id"]) card.card_id = node["card_id"].as<std::string>();
    if (node["project_id"]) card.project_id = node["project_id"].as<std::string>();
    if (node["title"]) card.title = node["title"].as<std::string>();
    if (node["rel_path"]) card.rel_path = node["rel_path"].as<std::string>();
    if (node["created_at"]) card.created_at = node["created_at"].as<long long>();
    if (node["updated_at"]) card.updated_at = node["updated_at"].as<long long>();
    if (node["sort_key"]) card.sort_key = node["sort_key"].as<double>();

    if (node["parent_card_id"] && !node["parent_card_id"].IsNull()) {
      card.parent_card_id = node["parent_card_id"].as<std::string>();
    }
    if (node["deleted_at"] && !node["deleted_at"].IsNull()) {
      card.deleted_at = node["deleted_at"].as<long long>();
    }

    parsed.body = raw.substr(end + 5);
    return parsed;
  } catch (const std::exception&) {
    return parsed;
  }
}

} // namespace holder::core
