#include "api/support/CloudConfig.h"

#include "api/support/PathDiscovery.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace holder::api::support {
namespace {

std::string trim_ascii(std::string s) {
  auto not_space = [](unsigned char c) {
    return !std::isspace(c);
  };
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.back())))
    s.pop_back();
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

int cost_tier_rank(const std::string& cost_tier) {
  const auto tier = lowercase_ascii(trim_ascii(cost_tier));
  if (tier == "free") return 0;
  if (tier == "low") return 1;
  if (tier == "paid") return 2;
  return 3;
}

bool is_supported_provider_kind(const std::string& kind) {
  return kind == "chocolatefactory_generative_language" || kind == "generic_chat" ||
         kind == "generic_responses" || kind == "mechatropic_messages";
}

} // namespace

std::optional<CloudProvidersConfig> load_cloudproviders_config() {
  const auto path = find_ai_catalog_path();
  if (!path.has_value()) return std::nullopt;

  CloudProvidersConfig cfg;
  const YAML::Node root = YAML::LoadFile(path->string());
  const YAML::Node models_root = root["models"];
  if (!models_root || !models_root.IsMap()) {
    return std::nullopt;
  }
  const YAML::Node runtime_root = models_root["runtime"];

  if (runtime_root && runtime_root["route_policy"] &&
      runtime_root["route_policy"]["default_provider"]) {
    cfg.default_provider = normalize_provider_name(
        runtime_root["route_policy"]["default_provider"].as<std::string>()
    );
  }
  if (runtime_root && runtime_root["route_policy"] &&
      runtime_root["route_policy"]["provider_order"] &&
      runtime_root["route_policy"]["provider_order"].IsSequence()) {
    for (const auto& provider_id : runtime_root["route_policy"]["provider_order"]) {
      const std::string normalized = normalize_provider_name(provider_id.as<std::string>());
      if (!normalized.empty()) {
        cfg.provider_order.push_back(normalized);
      }
    }
  }
  if (runtime_root && runtime_root["compaction"] && runtime_root["compaction"]["summary_refresh"]) {
    const auto summary_refresh = runtime_root["compaction"]["summary_refresh"];
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
    if (summary_refresh["min_interval_seconds"]) {
      cfg.summary_refresh.min_interval_seconds =
          summary_refresh["min_interval_seconds"].as<long long>();
    }
    if (summary_refresh["min_delta_tokens"]) {
      cfg.summary_refresh.min_delta_tokens = summary_refresh["min_delta_tokens"].as<long long>();
    }
    if (summary_refresh["force_refresh_tokens"]) {
      cfg.summary_refresh.force_refresh_tokens =
          summary_refresh["force_refresh_tokens"].as<long long>();
    }
  }
  if (runtime_root && runtime_root["cooldown"]) {
    const auto cooldown = runtime_root["cooldown"];
    if (cooldown["base_seconds"]) {
      cfg.cooldown.base_seconds = cooldown["base_seconds"].as<long long>();
    }
    if (cooldown["cap_seconds"]) {
      cfg.cooldown.cap_seconds = cooldown["cap_seconds"].as<long long>();
    }
  }

  const YAML::Node cloud_models = (models_root["Models"] && models_root["Models"]["Cloud"] &&
                                   models_root["Models"]["Cloud"].IsSequence())
                                      ? models_root["Models"]["Cloud"]
                                      : YAML::Node();
  const YAML::Node provider_defaults = (models_root["provider_defaults"] &&
                                        models_root["provider_defaults"].IsMap())
                                           ? models_root["provider_defaults"]
                                           : YAML::Node();
  if (cloud_models) {
    std::unordered_map<std::string, size_t> provider_index;
    for (const auto& model_node : cloud_models) {
      if (!model_node) continue;

      std::string provider_id_raw;
      if (model_node["provider_id"]) provider_id_raw = model_node["provider_id"].as<std::string>();
      const std::string provider_id = normalize_provider_name(provider_id_raw);
      if (provider_id.empty()) continue;
      const YAML::Node provider_default = (provider_defaults && provider_defaults[provider_id])
                                              ? provider_defaults[provider_id]
                                              : YAML::Node();

      size_t index = 0;
      auto it = provider_index.find(provider_id);
      if (it == provider_index.end()) {
        CloudProviderConfig provider;
        provider.id = provider_id;
        provider.display_name = model_node["provider"]
                                    ? model_node["provider"].as<std::string>()
                                    : (provider_default && provider_default["provider"]
                                           ? provider_default["provider"].as<std::string>()
                                           : provider.id);
        provider.enabled = model_node["enabled"] ? model_node["enabled"].as<bool>()
                                                 : (provider_default && provider_default["enabled"]
                                                        ? provider_default["enabled"].as<bool>()
                                                        : false);
        if (model_node["provider_cost_tier"]) {
          provider.cost_tier = model_node["provider_cost_tier"].as<std::string>();
        } else if (provider_default && provider_default["provider_cost_tier"]) {
          provider.cost_tier = provider_default["provider_cost_tier"].as<std::string>();
        }
        if (model_node["setup_url"])
          provider.setup_url = model_node["setup_url"].as<std::string>();
        else if (provider_default && provider_default["setup_url"]) {
          provider.setup_url = provider_default["setup_url"].as<std::string>();
        }
        if (model_node["docs_url"])
          provider.docs_url = model_node["docs_url"].as<std::string>();
        else if (provider_default && provider_default["docs_url"]) {
          provider.docs_url = provider_default["docs_url"].as<std::string>();
        }
        if (model_node["api_key_label"])
          provider.api_key_label = model_node["api_key_label"].as<std::string>();
        else if (provider_default && provider_default["api_key_label"]) {
          provider.api_key_label = provider_default["api_key_label"].as<std::string>();
        }
        if (model_node["api_key_hint"])
          provider.api_key_hint = model_node["api_key_hint"].as<std::string>();
        else if (provider_default && provider_default["api_key_hint"]) {
          provider.api_key_hint = provider_default["api_key_hint"].as<std::string>();
        }
        if (model_node["base_url"]) {
          provider.base_url = model_node["base_url"].as<std::string>();
        } else if (provider_default && provider_default["base_url"]) {
          provider.base_url = provider_default["base_url"].as<std::string>();
        } else if (model_node["endpoint"]) {
          provider.base_url = model_node["endpoint"].as<std::string>();
        }
        if (model_node["api_kind"]) {
          provider.kind = model_node["api_kind"].as<std::string>();
        } else if (provider_default && provider_default["api_kind"]) {
          provider.kind = provider_default["api_kind"].as<std::string>();
        }
        if (!provider.kind.empty() && !is_supported_provider_kind(provider.kind)) {
          throw std::runtime_error(
              "ai_catalog.yaml: cloud provider '" + provider.id + "' has unsupported api.kind '" +
              provider.kind + "'"
          );
        }
        if (model_node["auth_type"])
          provider.auth_type = model_node["auth_type"].as<std::string>();
        else if (provider_default && provider_default["auth_type"]) {
          provider.auth_type = provider_default["auth_type"].as<std::string>();
        }
        if (model_node["key_param"])
          provider.key_param = model_node["key_param"].as<std::string>();
        else if (provider_default && provider_default["key_param"]) {
          provider.key_param = provider_default["key_param"].as<std::string>();
        }
        if (model_node["header_name"])
          provider.header_name = model_node["header_name"].as<std::string>();
        else if (provider_default && provider_default["header_name"]) {
          provider.header_name = provider_default["header_name"].as<std::string>();
        }
        if (model_node["bearer_prefix"]) {
          provider.bearer_prefix = model_node["bearer_prefix"].as<std::string>();
        } else if (provider_default && provider_default["bearer_prefix"]) {
          provider.bearer_prefix = provider_default["bearer_prefix"].as<std::string>();
        }
        if (model_node["credential_key"]) {
          provider.credential_provider_key = normalize_provider_name(
              model_node["credential_key"].as<std::string>()
          );
        } else if (provider_default && provider_default["credential_key"]) {
          provider.credential_provider_key = normalize_provider_name(
              provider_default["credential_key"].as<std::string>()
          );
        }
        const YAML::Node provider_cooldown =
            (model_node["provider_cooldown"] && model_node["provider_cooldown"].IsMap())
                ? model_node["provider_cooldown"]
                : (provider_default && provider_default["provider_cooldown"] &&
                           provider_default["provider_cooldown"].IsMap()
                       ? provider_default["provider_cooldown"]
                       : YAML::Node());
        if (provider_cooldown) {
          if (provider_cooldown["base_seconds"]) {
            provider.cooldown_base_seconds = provider_cooldown["base_seconds"].as<long long>();
          }
          if (provider_cooldown["cap_seconds"]) {
            provider.cooldown_cap_seconds = provider_cooldown["cap_seconds"].as<long long>();
          }
        }
        if (provider.credential_provider_key.empty()) {
          provider.credential_provider_key = provider.id;
        }
        cfg.providers.push_back(std::move(provider));
        index = cfg.providers.size() - 1;
        provider_index[provider_id] = index;
      } else {
        index = it->second;
      }

      auto& provider = cfg.providers[index];
      if (provider.display_name.empty() && model_node["provider"]) {
        provider.display_name = model_node["provider"].as<std::string>();
      } else if (provider.display_name.empty() && provider_default &&
                 provider_default["provider"]) {
        provider.display_name = provider_default["provider"].as<std::string>();
      }
      if (model_node["enabled"])
        provider.enabled = model_node["enabled"].as<bool>();
      else if (!provider.enabled && provider_default && provider_default["enabled"]) {
        provider.enabled = provider_default["enabled"].as<bool>();
      }
      if (provider.cost_tier.empty() && model_node["provider_cost_tier"]) {
        provider.cost_tier = model_node["provider_cost_tier"].as<std::string>();
      } else if (provider.cost_tier.empty() && provider_default &&
                 provider_default["provider_cost_tier"]) {
        provider.cost_tier = provider_default["provider_cost_tier"].as<std::string>();
      }
      if (provider.setup_url.empty() && model_node["setup_url"]) {
        provider.setup_url = model_node["setup_url"].as<std::string>();
      } else if (provider.setup_url.empty() && provider_default && provider_default["setup_url"]) {
        provider.setup_url = provider_default["setup_url"].as<std::string>();
      }
      if (provider.docs_url.empty() && model_node["docs_url"]) {
        provider.docs_url = model_node["docs_url"].as<std::string>();
      } else if (provider.docs_url.empty() && provider_default && provider_default["docs_url"]) {
        provider.docs_url = provider_default["docs_url"].as<std::string>();
      }
      if (provider.api_key_label.empty() && model_node["api_key_label"]) {
        provider.api_key_label = model_node["api_key_label"].as<std::string>();
      } else if (provider.api_key_label.empty() && provider_default &&
                 provider_default["api_key_label"]) {
        provider.api_key_label = provider_default["api_key_label"].as<std::string>();
      }
      if (provider.api_key_hint.empty() && model_node["api_key_hint"]) {
        provider.api_key_hint = model_node["api_key_hint"].as<std::string>();
      } else if (provider.api_key_hint.empty() && provider_default &&
                 provider_default["api_key_hint"]) {
        provider.api_key_hint = provider_default["api_key_hint"].as<std::string>();
      }
      if (provider.base_url.empty()) {
        if (model_node["base_url"])
          provider.base_url = model_node["base_url"].as<std::string>();
        else if (provider_default && provider_default["base_url"]) {
          provider.base_url = provider_default["base_url"].as<std::string>();
        } else if (model_node["endpoint"])
          provider.base_url = model_node["endpoint"].as<std::string>();
      }
      if (provider.kind.empty() && model_node["api_kind"]) {
        provider.kind = model_node["api_kind"].as<std::string>();
        if (!is_supported_provider_kind(provider.kind)) {
          throw std::runtime_error(
              "ai_catalog.yaml: cloud provider '" + provider.id + "' has unsupported api.kind '" +
              provider.kind + "'"
          );
        }
        // Provider kind is initialized on first encounter (new-provider block above),
        // so this duplicate-pass provider_default fallback is effectively unreachable.
        // LCOV_EXCL_START
      } else if (provider.kind.empty() && provider_default && provider_default["api_kind"]) {
        provider.kind = provider_default["api_kind"].as<std::string>();
        if (!is_supported_provider_kind(provider.kind)) {
          throw std::runtime_error(
              "ai_catalog.yaml: cloud provider '" + provider.id + "' has unsupported api.kind '" +
              provider.kind + "'"
          );
        }
      }
      // LCOV_EXCL_STOP
      if (provider.auth_type.empty() && model_node["auth_type"]) {
        provider.auth_type = model_node["auth_type"].as<std::string>();
      } else if (provider.auth_type.empty() && provider_default && provider_default["auth_type"]) {
        provider.auth_type = provider_default["auth_type"].as<std::string>();
      }
      if (provider.key_param.empty() && model_node["key_param"]) {
        provider.key_param = model_node["key_param"].as<std::string>();
      } else if (provider.key_param.empty() && provider_default && provider_default["key_param"]) {
        provider.key_param = provider_default["key_param"].as<std::string>();
      }
      if (provider.header_name.empty() && model_node["header_name"]) {
        provider.header_name = model_node["header_name"].as<std::string>();
      } else if (provider.header_name.empty() && provider_default &&
                 provider_default["header_name"]) {
        provider.header_name = provider_default["header_name"].as<std::string>();
      }
      if (provider.bearer_prefix.empty() && model_node["bearer_prefix"]) {
        provider.bearer_prefix = model_node["bearer_prefix"].as<std::string>();
      } else if (provider.bearer_prefix.empty() && provider_default &&
                 provider_default["bearer_prefix"]) {
        provider.bearer_prefix = provider_default["bearer_prefix"].as<std::string>();
      }
      // credential_provider_key is initialized to provider.id in the first-pass
      // new-provider block, so duplicate-pass key assignment is effectively unreachable.
      // LCOV_EXCL_START
      if (provider.credential_provider_key.empty() && model_node["credential_key"]) {
        provider.credential_provider_key = normalize_provider_name(
            model_node["credential_key"].as<std::string>()
        );
      } else if (provider.credential_provider_key.empty() && provider_default &&
                 provider_default["credential_key"]) {
        provider.credential_provider_key = normalize_provider_name(
            provider_default["credential_key"].as<std::string>()
        );
      }
      if (provider.credential_provider_key.empty()) {
        provider.credential_provider_key = provider.id;
      }
      // LCOV_EXCL_STOP
      if (provider.cooldown_base_seconds == 0 && model_node["provider_cooldown"] &&
          model_node["provider_cooldown"]["base_seconds"]) {
        provider.cooldown_base_seconds =
            model_node["provider_cooldown"]["base_seconds"].as<long long>();
      } else if (provider.cooldown_base_seconds == 0 && provider_default &&
                 provider_default["provider_cooldown"] &&
                 provider_default["provider_cooldown"]["base_seconds"]) {
        provider.cooldown_base_seconds =
            provider_default["provider_cooldown"]["base_seconds"].as<long long>();
      }
      if (provider.cooldown_cap_seconds == 0 && model_node["provider_cooldown"] &&
          model_node["provider_cooldown"]["cap_seconds"]) {
        provider.cooldown_cap_seconds =
            model_node["provider_cooldown"]["cap_seconds"].as<long long>();
      } else if (provider.cooldown_cap_seconds == 0 && provider_default &&
                 provider_default["provider_cooldown"] &&
                 provider_default["provider_cooldown"]["cap_seconds"]) {
        provider.cooldown_cap_seconds =
            provider_default["provider_cooldown"]["cap_seconds"].as<long long>();
      }

      if (!model_node["model_id"] || !model_node["endpoint"]) continue;
      CloudModelConfig model;
      model.id = model_node["model_id"].as<std::string>();
      model.endpoint = model_node["endpoint"].as<std::string>();
      model.role = model_node["role"] ? model_node["role"].as<std::string>() : "";
      if (model_node["model_cost_tier"]) {
        model.cost_tier = model_node["model_cost_tier"].as<std::string>();
      } else if (model_node["cost_tier"]) {
        model.cost_tier = model_node["cost_tier"].as<std::string>();
      }
      if (model_node["default_for_low_budget"]) {
        model.default_for_low_budget = model_node["default_for_low_budget"].as<bool>();
      }
      if (model_node["limits"]) {
        if (model_node["limits"]["rpm"]) model.rpm = model_node["limits"]["rpm"].as<long long>();
        if (model_node["limits"]["tpm"]) model.tpm = model_node["limits"]["tpm"].as<long long>();
        if (model_node["limits"]["rpd"]) model.rpd = model_node["limits"]["rpd"].as<long long>();
      }
      const YAML::Node model_cooldown = (model_node["model_cooldown"] &&
                                         model_node["model_cooldown"].IsMap())
                                            ? model_node["model_cooldown"]
                                            : model_node["cooldown"];
      if (model_cooldown) {
        if (model_cooldown["base_seconds"]) {
          model.cooldown_base_seconds = model_cooldown["base_seconds"].as<long long>();
        }
        if (model_cooldown["cap_seconds"]) {
          model.cooldown_cap_seconds = model_cooldown["cap_seconds"].as<long long>();
        }
      }

      bool replaced = false;
      for (auto& existing : provider.models) {
        if (existing.id == model.id) {
          existing = std::move(model);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        provider.models.push_back(std::move(model));
      }
    }
  }
  return cfg;
}

const CloudProviderConfig* find_cloud_provider(
    const CloudProvidersConfig& cfg,
    const std::string& provider_id
) {
  for (const auto& provider : cfg.providers) {
    if (provider.id == provider_id) return &provider;
  }
  return nullptr;
}

const CloudModelConfig* choose_cloud_model(
    const CloudProviderConfig& provider,
    const std::string& requested_model
) {
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

const CloudModelConfig* find_cloud_model(
    const CloudProviderConfig& provider,
    const std::string& model_id
) {
  for (const auto& model : provider.models) {
    if (model.id == model_id) return &model;
  }
  return nullptr;
}

std::vector<const CloudModelConfig*> cloud_model_candidates(
    const CloudProviderConfig& provider,
    const std::string& requested_model
) {
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

std::vector<const CloudProviderConfig*> ordered_cloud_providers(const CloudProvidersConfig& cfg) {
  std::vector<const CloudProviderConfig*> out;
  std::unordered_set<std::string> seen;
  auto push = [&](const CloudProviderConfig* provider) {
    if (!provider) return;
    if (seen.insert(provider->id).second) {
      out.push_back(provider);
    }
  };

  for (const auto& id : cfg.provider_order) {
    push(find_cloud_provider(cfg, id));
  }

  std::vector<const CloudProviderConfig*> remaining;
  for (const auto& provider : cfg.providers) {
    if (seen.find(provider.id) == seen.end()) {
      remaining.push_back(&provider);
    }
  }
  std::stable_sort(remaining.begin(), remaining.end(), [](const auto* a, const auto* b) {
    const int rank_a = cost_tier_rank(a->cost_tier);
    const int rank_b = cost_tier_rank(b->cost_tier);
    if (rank_a != rank_b) return rank_a < rank_b;
    return a->id < b->id;
  });
  for (const auto* provider : remaining) {
    push(provider);
  }
  return out;
}

} // namespace holder::api::support
