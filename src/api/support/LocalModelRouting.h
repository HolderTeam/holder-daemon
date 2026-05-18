#pragma once

#include "llm/LocalModelRunner.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::api::support {

struct LocalModelMeta {
  std::string tag;
  std::string provider;
  std::string engine;
  std::string category;
  std::string hardware_tier;
  std::string speed;
  std::string quality;
  long long size_bytes = 0;
};

struct CasteInfo {
  std::string name;
  std::string reason;
};

std::string trim_ascii(std::string s);
std::string lowercase_ascii(std::string s);
std::string normalize_caste_name(const std::string& raw);
bool caste_meets_or_exceeds(const std::string& machine_caste, const std::string& required_caste);
std::optional<CasteInfo> detect_machine_caste();

std::unordered_map<std::string, LocalModelMeta> load_local_model_meta();
std::vector<nlohmann::json> build_caste_recommendations(
    const std::vector<holder::llm::LocalModel>& installed_models,
    const std::unordered_map<std::string, LocalModelMeta>& model_meta,
    const std::string& machine_caste
);

std::vector<std::string> parse_ranked_models(
    const std::string& text,
    const std::vector<std::string>& candidates
);
std::string pick_smallest_model(const std::vector<holder::llm::LocalModel>& models);
std::string pick_router_model(
    const std::vector<holder::llm::LocalModel>& models,
    const std::unordered_map<std::string, LocalModelMeta>& meta
);
std::string pick_largest_model(const std::vector<holder::llm::LocalModel>& models);

} // namespace holder::api::support
