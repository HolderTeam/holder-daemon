#include "api/support/LocalModelRouting.h"

#include "api/support/PathDiscovery.h"

#include "caste.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace holder::api::support {
namespace {

long long parse_size_bytes(const std::string& input) {
  std::string s;
  s.reserve(input.size());
  for (char ch : input) {
    if (!std::isspace(static_cast<unsigned char>(ch))) s.push_back(ch);
  }
  if (s.empty()) return 0;

  size_t pos = 0;
  while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.')) {
    ++pos;
  }
  if (pos == 0) return 0;
  const double value = std::stod(s.substr(0, pos));
  std::string unit = s.substr(pos);
  for (auto& ch : unit) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  double multiplier = 1.0;
  if (unit == "kb" || unit == "k") multiplier = 1024.0;
  else if (unit == "mb" || unit == "m") multiplier = 1024.0 * 1024.0;
  else if (unit == "gb" || unit == "g") multiplier = 1024.0 * 1024.0 * 1024.0;
  else if (unit == "tb" || unit == "t") multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;

  return static_cast<long long>(value * multiplier);
}

int caste_rank(const std::string& caste_name_value) {
  const std::string n = normalize_caste_name(caste_name_value);
  if (n == "mini") return 0;
  if (n == "user") return 1;
  if (n == "developer") return 2;
  if (n == "workstation") return 3;
  if (n == "rig") return 4;
  return -1;
}

int quality_rank(const std::string& quality) {
  const std::string q = lowercase_ascii(trim_ascii(quality));
  if (q == "high") return 3;
  if (q == "medium") return 2;
  if (q == "low") return 1;
  return 0;
}

int speed_rank(const std::string& speed) {
  const std::string s = lowercase_ascii(trim_ascii(speed));
  if (s == "fast") return 3;
  if (s == "medium") return 2;
  if (s == "slow") return 1;
  return 0;
}

std::optional<std::string> extract_json_array(const std::string& text) {
  const auto start = text.find('[');
  if (start == std::string::npos) return std::nullopt;
  const auto end = text.find_last_of(']');
  if (end == std::string::npos || end <= start) return std::nullopt;
  return text.substr(start, end - start + 1);
}

} // namespace

std::string trim_ascii(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::string lowercase_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

std::string normalize_caste_name(const std::string& raw) {
  const std::string key = lowercase_ascii(trim_ascii(raw));
  if (key.empty()) return {};
  if (key == "mini" || key == "user" || key == "developer" || key == "workstation" || key == "rig") {
    return key;
  }
  if (key == "tiny") return "mini";
  if (key == "budget") return "user";
  if (key == "laptop") return "developer";
  if (key == "gaming") return "workstation";
  return {};
}

bool caste_meets_or_exceeds(const std::string& machine_caste, const std::string& required_caste) {
  const int machine = caste_rank(machine_caste);
  const int required = caste_rank(required_caste);
  if (machine < 0 || required < 0) return false;
  return machine >= required;
}

std::optional<CasteInfo> detect_machine_caste() {
  try {
    const CasteResult result = detect_caste();
    CasteInfo out;
    out.name = normalize_caste_name(caste_name(result.caste));
    out.reason = result.reason;
    if (out.name.empty()) return std::nullopt;
    return out;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::unordered_map<std::string, LocalModelMeta> load_local_model_meta() {
  std::unordered_map<std::string, LocalModelMeta> meta;
  const auto ai_catalog_path = find_ai_catalog_path();
  if (!ai_catalog_path.has_value()) return meta;
  try {
    const YAML::Node root = YAML::LoadFile(ai_catalog_path->string());
    if (!root["models"] || !root["models"]["Models"] || !root["models"]["Models"]["Local"]) {
      return meta;
    }
    for (const auto& node : root["models"]["Models"]["Local"]) {
      if (!node || !node["tag"]) continue;
      const std::string tag = node["tag"].as<std::string>();
      LocalModelMeta entry;
      entry.tag = tag;
      if (node["provider"]) entry.provider = node["provider"].as<std::string>();
      if (node["engine"]) entry.engine = node["engine"].as<std::string>();
      if (node["category"]) entry.category = node["category"].as<std::string>();
      if (node["hardware_tier"]) {
        entry.hardware_tier = normalize_caste_name(node["hardware_tier"].as<std::string>());
      } else if (node["caste"]) {
        entry.hardware_tier = normalize_caste_name(node["caste"].as<std::string>());
      }
      if (node["speed"]) entry.speed = node["speed"].as<std::string>();
      if (node["quality"]) entry.quality = node["quality"].as<std::string>();
      if (node["size"]) entry.size_bytes = parse_size_bytes(node["size"].as<std::string>());
      meta[tag] = entry;
    }
  } catch (const std::exception&) {
    return meta;
  }
  return meta;
}

std::vector<nlohmann::json> build_caste_recommendations(
    const std::vector<holder::llm::LocalModel>& installed_models,
    const std::unordered_map<std::string, LocalModelMeta>& model_meta,
    const std::string& machine_caste) {
  std::unordered_set<std::string> installed;
  for (const auto& model : installed_models) {
    installed.insert(model.name);
  }

  std::vector<nlohmann::json> out;
  for (const auto& [tag, meta] : model_meta) {
    if (meta.hardware_tier.empty()) continue;
    if (!caste_meets_or_exceeds(machine_caste, meta.hardware_tier)) continue;

    nlohmann::json item;
    item["tag"] = tag;
    item["provider"] = meta.provider.empty() ? nlohmann::json(nullptr) : nlohmann::json(meta.provider);
    item["engine"] = meta.engine.empty() ? nlohmann::json(nullptr) : nlohmann::json(meta.engine);
    item["category"] = meta.category.empty() ? nlohmann::json(nullptr) : nlohmann::json(meta.category);
    item["required_caste"] = meta.hardware_tier;
    item["installed"] = installed.find(tag) != installed.end();
    out.push_back(std::move(item));
  }

  std::stable_sort(out.begin(), out.end(), [&](const nlohmann::json& a, const nlohmann::json& b) {
    const std::string ta = a.value("tag", "");
    const std::string tb = b.value("tag", "");
    const auto ita = model_meta.find(ta);
    const auto itb = model_meta.find(tb);
    const int qa = (ita != model_meta.end()) ? quality_rank(ita->second.quality) : 0;
    const int qb = (itb != model_meta.end()) ? quality_rank(itb->second.quality) : 0;
    if (qa != qb) return qa > qb;
    const int sa = (ita != model_meta.end()) ? speed_rank(ita->second.speed) : 0;
    const int sb = (itb != model_meta.end()) ? speed_rank(itb->second.speed) : 0;
    if (sa != sb) return sa > sb;
    return ta < tb;
  });

  return out;
}

std::vector<std::string> parse_ranked_models(const std::string& text,
                                             const std::vector<std::string>& candidates) {
  const auto array_text = extract_json_array(text);
  if (!array_text.has_value()) return {};
  try {
    const auto parsed = nlohmann::json::parse(array_text.value());
    if (!parsed.is_array()) return {};
    std::vector<std::string> ranked;
    for (const auto& item : parsed) {
      if (!item.is_string()) continue;
      const auto name = item.get<std::string>();
      if (std::find(candidates.begin(), candidates.end(), name) != candidates.end()) {
        if (std::find(ranked.begin(), ranked.end(), name) == ranked.end()) {
          ranked.push_back(name);
        }
      }
    }
    return ranked;
  } catch (const std::exception&) {
    return {};
  }
}

std::string pick_smallest_model(const std::vector<holder::llm::LocalModel>& models) {
  if (models.empty()) return {};
  const auto* best = &models.front();
  for (const auto& model : models) {
    if (model.size > 0 && (best->size == 0 || model.size < best->size)) {
      best = &model;
    }
  }
  return best->name;
}

std::string pick_router_model(const std::vector<holder::llm::LocalModel>& models,
                              const std::unordered_map<std::string, LocalModelMeta>& meta) {
  long long best_size = 0;
  std::string best;
  for (const auto& model : models) {
    const auto it = meta.find(model.name);
    if (it == meta.end()) continue;
    if (it->second.category != "router") continue;
    const long long size = it->second.size_bytes > 0 ? it->second.size_bytes : model.size;
    if (best.empty() || (size > 0 && (best_size == 0 || size < best_size))) {
      best = model.name;
      best_size = size;
    }
  }
  return best;
}

std::string pick_largest_model(const std::vector<holder::llm::LocalModel>& models) {
  if (models.empty()) return {};
  const auto* best = &models.front();
  for (const auto& model : models) {
    if (model.size > best->size) {
      best = &model;
    }
  }
  return best->name;
}

} // namespace holder::api::support
