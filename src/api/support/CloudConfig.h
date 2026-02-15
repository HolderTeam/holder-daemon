#pragma once

#include <optional>
#include <string>
#include <vector>

namespace holder::api::support {

struct CloudModelConfig {
  std::string id;
  std::string endpoint;
  std::string role;
  long long rpm = 0;
  long long tpm = 0;
  long long rpd = 0;
  long long cooldown_base_seconds = 0; // 0 => use provider/global default
  long long cooldown_cap_seconds = 0;  // 0 => use provider/global default
};

struct CloudProviderConfig {
  std::string id;
  std::string display_name;
  bool enabled = false;
  std::string base_url;
  std::string kind;
  std::string auth_type;
  std::string key_param;
  std::string header_name;
  std::string bearer_prefix;
  std::string credential_provider_key;
  long long cooldown_base_seconds = 0; // 0 => use global default
  long long cooldown_cap_seconds = 0;  // 0 => use global default
  std::vector<CloudModelConfig> models;
};

struct CloudProvidersConfig {
  struct SummaryRefreshConfig {
    long long trigger_context_tokens = 1200;
    long long source_context_tokens = 2000;
    long long response_tokens_budget = 256;
    long long max_summary_chars = 5000;
  };
  struct CooldownConfig {
    long long base_seconds = 30;
    long long cap_seconds = 900;
  };

  std::string default_provider;
  SummaryRefreshConfig summary_refresh;
  CooldownConfig cooldown;
  std::vector<CloudProviderConfig> providers;
};

std::optional<CloudProvidersConfig> load_cloudproviders_config();
const CloudProviderConfig* find_cloud_provider(const CloudProvidersConfig& cfg,
                                               const std::string& provider_id);
const CloudModelConfig* choose_cloud_model(const CloudProviderConfig& provider,
                                           const std::string& requested_model);
const CloudModelConfig* find_cloud_model(const CloudProviderConfig& provider,
                                         const std::string& model_id);
std::vector<const CloudModelConfig*> cloud_model_candidates(const CloudProviderConfig& provider,
                                                            const std::string& requested_model);

} // namespace holder::api::support
