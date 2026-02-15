#include "api/support/CloudConfig.h"

#include "api/support/PathDiscovery.h"

#include <yaml-cpp/yaml.h>

#include <unordered_set>

namespace holder::api::support {
namespace {

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

std::string normalize_provider_name(const std::string& raw) {
  const std::string key = lowercase_ascii(trim_ascii(raw));
  if (key.empty()) return {};
  for (const char ch : key) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || ch == '-' || ch == '_' || ch == '.') continue;
    return {};
  }
  return key;
}

} // namespace

std::optional<CloudProvidersConfig> load_cloudproviders_config() {
  const auto path = find_cloudproviders_path();
  if (!path.has_value()) return std::nullopt;

  CloudProvidersConfig cfg;
  const YAML::Node root = YAML::LoadFile(path->string());
  if (root["defaults"] && root["defaults"]["route_policy"] &&
      root["defaults"]["route_policy"]["default_provider"]) {
    cfg.default_provider = normalize_provider_name(
        root["defaults"]["route_policy"]["default_provider"].as<std::string>());
  }
  if (root["defaults"] && root["defaults"]["compaction"] &&
      root["defaults"]["compaction"]["summary_refresh"]) {
    const auto summary_refresh = root["defaults"]["compaction"]["summary_refresh"];
    if (summary_refresh["trigger_context_tokens"]) {
      cfg.summary_refresh.trigger_context_tokens =
          summary_refresh["trigger_context_tokens"].as<long long>();
    }
    if (summary_refresh["source_context_tokens"]) {
      cfg.summary_refresh.source_context_tokens =
          summary_refresh["source_context_tokens"].as<long long>();
    }
    if (summary_refresh["response_tokens_budget"]) {
      cfg.summary_refresh.response_tokens_budget =
          summary_refresh["response_tokens_budget"].as<long long>();
    }
    if (summary_refresh["max_summary_chars"]) {
      cfg.summary_refresh.max_summary_chars = summary_refresh["max_summary_chars"].as<long long>();
    }
  }
  if (root["defaults"] && root["defaults"]["cooldown"]) {
    const auto cooldown = root["defaults"]["cooldown"];
    if (cooldown["base_seconds"]) {
      cfg.cooldown.base_seconds = cooldown["base_seconds"].as<long long>();
    }
    if (cooldown["cap_seconds"]) {
      cfg.cooldown.cap_seconds = cooldown["cap_seconds"].as<long long>();
    }
  }

  if (root["providers"] && root["providers"].IsSequence()) {
    for (const auto& provider_node : root["providers"]) {
      if (!provider_node["id"]) continue;
      CloudProviderConfig provider;
      provider.id = normalize_provider_name(provider_node["id"].as<std::string>());
      if (provider.id.empty()) continue;
      provider.display_name = provider_node["display_name"]
                                  ? provider_node["display_name"].as<std::string>()
                                  : provider.id;
      provider.enabled = provider_node["enabled"] ? provider_node["enabled"].as<bool>() : false;
      if (provider_node["api"]) {
        if (provider_node["api"]["base_url"]) {
          provider.base_url = provider_node["api"]["base_url"].as<std::string>();
        }
        if (provider_node["api"]["kind"]) {
          provider.kind = provider_node["api"]["kind"].as<std::string>();
        }
      }
      if (provider_node["auth"]) {
        if (provider_node["auth"]["type"]) {
          provider.auth_type = provider_node["auth"]["type"].as<std::string>();
        }
        if (provider_node["auth"]["key_param"]) {
          provider.key_param = provider_node["auth"]["key_param"].as<std::string>();
        }
        if (provider_node["auth"]["header_name"]) {
          provider.header_name = provider_node["auth"]["header_name"].as<std::string>();
        }
        if (provider_node["auth"]["bearer_prefix"]) {
          provider.bearer_prefix = provider_node["auth"]["bearer_prefix"].as<std::string>();
        }
        if (provider_node["auth"]["credential_provider_key"]) {
          provider.credential_provider_key = normalize_provider_name(
              provider_node["auth"]["credential_provider_key"].as<std::string>());
        }
      }
      if (provider.credential_provider_key.empty()) {
        provider.credential_provider_key = provider.id;
      }
      if (provider_node["models"] && provider_node["models"].IsSequence()) {
        for (const auto& model_node : provider_node["models"]) {
          if (!model_node["id"] || !model_node["endpoint"]) continue;
          CloudModelConfig m;
          m.id = model_node["id"].as<std::string>();
          m.endpoint = model_node["endpoint"].as<std::string>();
          m.role = model_node["role"] ? model_node["role"].as<std::string>() : "";
          if (model_node["limits"]) {
            if (model_node["limits"]["rpm"]) {
              m.rpm = model_node["limits"]["rpm"].as<long long>();
            }
            if (model_node["limits"]["tpm"]) {
              m.tpm = model_node["limits"]["tpm"].as<long long>();
            }
            if (model_node["limits"]["rpd"]) {
              m.rpd = model_node["limits"]["rpd"].as<long long>();
            }
          }
          provider.models.push_back(std::move(m));
        }
      }
      cfg.providers.push_back(std::move(provider));
    }
  }
  return cfg;
}

const CloudProviderConfig* find_cloud_provider(const CloudProvidersConfig& cfg,
                                               const std::string& provider_id) {
  for (const auto& provider : cfg.providers) {
    if (provider.id == provider_id) return &provider;
  }
  return nullptr;
}

const CloudModelConfig* choose_cloud_model(const CloudProviderConfig& provider,
                                           const std::string& requested_model) {
  if (!requested_model.empty()) {
    for (const auto& model : provider.models) {
      if (model.id == requested_model) return &model;
    }
  }
  for (const auto& model : provider.models) {
    if (model.role == "default") return &model;
  }
  if (!provider.models.empty()) return &provider.models.front();
  return nullptr;
}

const CloudModelConfig* find_cloud_model(const CloudProviderConfig& provider,
                                         const std::string& model_id) {
  for (const auto& model : provider.models) {
    if (model.id == model_id) return &model;
  }
  return nullptr;
}

std::vector<const CloudModelConfig*> cloud_model_candidates(const CloudProviderConfig& provider,
                                                            const std::string& requested_model) {
  std::vector<const CloudModelConfig*> out;
  std::unordered_set<std::string> seen;
  auto push = [&](const CloudModelConfig* model) {
    if (!model) return;
    if (seen.insert(model->id).second) {
      out.push_back(model);
    }
  };

  if (!requested_model.empty()) {
    push(find_cloud_model(provider, requested_model));
  }
  push(choose_cloud_model(provider, ""));
  for (const auto& model : provider.models) {
    if (model.role == "compact") push(&model);
  }
  for (const auto& model : provider.models) {
    push(&model);
  }
  return out;
}

} // namespace holder::api::support
