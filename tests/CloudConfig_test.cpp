#include "api/support/CloudConfig.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

class EnvGuard {
 public:
  EnvGuard(const std::string& key, const std::string& value) : key_(key) {
    const char* previous = std::getenv(key_.c_str());
    if (previous) {
      had_previous_ = true;
      previous_ = previous;
    }
    setenv(key_.c_str(), value.c_str(), 1);
  }

  ~EnvGuard() {
    if (had_previous_) {
      setenv(key_.c_str(), previous_.c_str(), 1);
    } else {
      unsetenv(key_.c_str());
    }
  }

 private:
  std::string key_;
  bool had_previous_ = false;
  std::string previous_;
};

} // namespace

TEST_CASE("CloudConfig parses summary refresh defaults", "[cloud_config]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_config_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto yaml_path = dir / "ai_catalog.yaml";

  std::ofstream out(yaml_path);
  REQUIRE(out.is_open());
  out << "models:\n";
  out << "  runtime:\n";
  out << "    route_policy:\n";
  out << "      default_provider: chocolatefactory\n";
  out << "      provider_order:\n";
  out << "        - chocolatefactory\n";
  out << "        - switchyard\n";
  out << "    cooldown:\n";
  out << "      base_seconds: 45\n";
  out << "      cap_seconds: 600\n";
  out << "    compaction:\n";
  out << "      summary_refresh:\n";
  out << "        trigger_context_tokens: 1800\n";
  out << "        source_context_tokens: 2400\n";
  out << "        response_tokens_budget: 320\n";
  out << "        max_summary_chars: 4200\n";
  out << "        min_interval_seconds: 90\n";
  out << "        min_delta_tokens: 220\n";
  out << "        force_refresh_tokens: 3600\n";
  out << "  provider_defaults:\n";
  out << "    chocolatefactory:\n";
  out << "      provider: ChocolateFactory\n";
  out << "      credential_key: chocolatefactory\n";
  out << "      enabled: true\n";
  out << "      provider_cost_tier: free\n";
  out << "      base_url: https://example.com\n";
  out << "      api_kind: chocolatefactory_generative_language\n";
  out << "      auth_type: api_key_query\n";
  out << "      key_param: key\n";
  out << "      provider_cooldown:\n";
  out << "        base_seconds: 50\n";
  out << "        cap_seconds: 700\n";
  out << "  Models:\n";
  out << "    Cloud:\n";
  out << "      - provider_id: chocolatefactory\n";
  out << "        model_id: gemma-3-12b-it\n";
  out << "        endpoint: /v1beta/models/gemma-3-12b-it:generateContent\n";
  out << "        model_cost_tier: free\n";
  out << "        default_for_low_budget: true\n";
  out << "        model_cooldown:\n";
  out << "          base_seconds: 12\n";
  out << "          cap_seconds: 180\n";
  out.close();

  EnvGuard env("HOLDER_AI_CATALOG_PATH", yaml_path.string());
  const auto cfg = holder::api::support::load_cloudproviders_config();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->summary_refresh.trigger_context_tokens == 1800);
  REQUIRE(cfg->summary_refresh.source_context_tokens == 2400);
  REQUIRE(cfg->summary_refresh.response_tokens_budget == 320);
  REQUIRE(cfg->summary_refresh.max_summary_chars == 4200);
  REQUIRE(cfg->summary_refresh.min_interval_seconds == 90);
  REQUIRE(cfg->summary_refresh.min_delta_tokens == 220);
  REQUIRE(cfg->summary_refresh.force_refresh_tokens == 3600);
  REQUIRE(cfg->cooldown.base_seconds == 45);
  REQUIRE(cfg->cooldown.cap_seconds == 600);
  REQUIRE(cfg->providers.size() == 1);
  REQUIRE(cfg->providers[0].cooldown_base_seconds == 50);
  REQUIRE(cfg->providers[0].cooldown_cap_seconds == 700);
  REQUIRE(cfg->providers[0].cost_tier == "free");
  REQUIRE(cfg->providers[0].models.size() == 1);
  REQUIRE(cfg->providers[0].models[0].cost_tier == "free");
  REQUIRE(cfg->providers[0].models[0].default_for_low_budget == true);
  REQUIRE(cfg->providers[0].models[0].cooldown_base_seconds == 12);
  REQUIRE(cfg->providers[0].models[0].cooldown_cap_seconds == 180);
  REQUIRE(cfg->provider_order.size() == 2);
  REQUIRE(cfg->provider_order[0] == "chocolatefactory");
  REQUIRE(cfg->provider_order[1] == "switchyard");
}

TEST_CASE("CloudConfig rejects unknown provider api.kind", "[cloud_config]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_config_unknown_kind_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto yaml_path = dir / "ai_catalog.yaml";

  std::ofstream out(yaml_path);
  REQUIRE(out.is_open());
  out << "models:\n";
  out << "  runtime:\n";
  out << "    route_policy:\n";
  out << "      default_provider: unknown-provider\n";
  out << "  Models:\n";
  out << "    Cloud:\n";
  out << "      - provider: Unknown\n";
  out << "        provider_id: unknown-provider\n";
  out << "        credential_key: unknown-provider\n";
  out << "        enabled: true\n";
  out << "        base_url: https://example.com\n";
  out << "        api_kind: made_up_kind\n";
  out << "        auth_type: bearer_header\n";
  out << "        header_name: Authorization\n";
  out << "        model_id: unknown-model\n";
  out << "        endpoint: /v1/unknown\n";
  out.close();

  EnvGuard env("HOLDER_AI_CATALOG_PATH", yaml_path.string());
  REQUIRE_THROWS_WITH(holder::api::support::load_cloudproviders_config(),
                      Catch::Matchers::ContainsSubstring("unsupported api.kind 'made_up_kind'"));
}

TEST_CASE("CloudConfig orders providers by configured order then cost tier", "[cloud_config]") {
  holder::api::support::CloudProvidersConfig cfg;
  cfg.provider_order = {"switchyard"};
  holder::api::support::CloudProviderConfig p1;
  p1.id = "chocolatefactory";
  p1.enabled = true;
  p1.cost_tier = "free";
  cfg.providers.push_back(p1);

  holder::api::support::CloudProviderConfig p2;
  p2.id = "switchyard";
  p2.enabled = true;
  p2.cost_tier = "low";
  cfg.providers.push_back(p2);

  holder::api::support::CloudProviderConfig p3;
  p3.id = "mechatropic";
  p3.enabled = true;
  p3.cost_tier = "paid";
  cfg.providers.push_back(p3);

  holder::api::support::CloudProviderConfig p4;
  p4.id = "unknown";
  p4.enabled = true;
  cfg.providers.push_back(p4);

  const auto ordered = holder::api::support::ordered_cloud_providers(cfg);
  REQUIRE(ordered.size() == 4);
  REQUIRE(ordered[0]->id == "switchyard");
  REQUIRE(ordered[1]->id == "chocolatefactory");
  REQUIRE(ordered[2]->id == "mechatropic");
  REQUIRE(ordered[3]->id == "unknown");
}

TEST_CASE("CloudConfig merges provider defaults and replaces duplicate model ids", "[cloud_config]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_config_merge_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto yaml_path = dir / "ai_catalog.yaml";

  std::ofstream out(yaml_path);
  REQUIRE(out.is_open());
  out << "models:\n";
  out << "  runtime:\n";
  out << "    route_policy:\n";
  out << "      default_provider: \" Bad Name! \"\n";
  out << "      provider_order:\n";
  out << "        - \"alpha\"\n";
  out << "        - \" bad id! \"\n";
  out << "        - \"beta\"\n";
  out << "  provider_defaults:\n";
  out << "    beta:\n";
  out << "      provider: Beta Default\n";
  out << "      enabled: true\n";
  out << "      provider_cost_tier: low\n";
  out << "      setup_url: https://beta/setup\n";
  out << "      docs_url: https://beta/docs\n";
  out << "      api_key_label: Beta Key\n";
  out << "      api_key_hint: Get key\n";
  out << "      base_url: https://beta.base\n";
  out << "      api_kind: generic_chat\n";
  out << "      auth_type: bearer_header\n";
  out << "      key_param: key\n";
  out << "      header_name: Authorization\n";
  out << "      bearer_prefix: Bearer\n";
  out << "      provider_cooldown:\n";
  out << "        base_seconds: 7\n";
  out << "        cap_seconds: 70\n";
  out << "    gamma:\n";
  out << "      provider: Gamma Default\n";
  out << "  Models:\n";
  out << "    Cloud:\n";
  out << "      - provider_id: alpha\n";
  out << "        provider: \"\"\n";
  out << "        enabled: false\n";
  out << "        provider_cost_tier: free\n";
  out << "        setup_url: https://alpha/setup\n";
  out << "        docs_url: https://alpha/docs\n";
  out << "        api_key_label: Alpha Key\n";
  out << "        api_key_hint: Hint\n";
  out << "        endpoint: https://alpha.endpoint\n";
  out << "        api_kind: generic_responses\n";
  out << "        auth_type: header_key\n";
  out << "        key_param: k\n";
  out << "        header_name: x-key\n";
  out << "        bearer_prefix: BearerX\n";
  out << "        credential_key: \"bad key!\"\n";
  out << "        provider_cooldown:\n";
  out << "          base_seconds: 3\n";
  out << "          cap_seconds: 30\n";
  out << "        model_id: alpha-model\n";
  out << "        endpoint: /v1/alpha\n";
  out << "        role: default\n";
  out << "        model_cost_tier: paid\n";
  out << "      - provider_id: alpha\n";
  out << "        provider: Alpha Display\n";
  out << "        model_id: alpha-model\n";
  out << "        endpoint: /v2/alpha\n";
  out << "      - provider_id: beta\n";
  out << "        model_id: beta-model\n";
  out << "        endpoint: /v1/beta\n";
  out << "        cost_tier: free\n";
  out << "      - provider_id: gamma\n";
  out << "        provider: \"\"\n";
  out << "        model_id: gamma-1\n";
  out << "        endpoint: /v1/gamma\n";
  out << "      - provider_id: gamma\n";
  out << "        model_id: gamma-2\n";
  out << "        endpoint: /v2/gamma\n";
  out.close();

  EnvGuard env("HOLDER_AI_CATALOG_PATH", yaml_path.string());
  const auto cfg = holder::api::support::load_cloudproviders_config();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->default_provider.empty());
  REQUIRE(cfg->provider_order.size() == 2);
  REQUIRE(cfg->provider_order[0] == "alpha");
  REQUIRE(cfg->provider_order[1] == "beta");

  const auto* alpha = holder::api::support::find_cloud_provider(cfg.value(), "alpha");
  REQUIRE(alpha != nullptr);
  REQUIRE(alpha->display_name == "Alpha Display");
  REQUIRE(alpha->credential_provider_key == "alpha");
  REQUIRE(alpha->cooldown_base_seconds == 3);
  REQUIRE(alpha->cooldown_cap_seconds == 30);
  REQUIRE(alpha->models.size() == 1);
  REQUIRE(alpha->models[0].endpoint == "/v2/alpha");

  const auto* beta = holder::api::support::find_cloud_provider(cfg.value(), "beta");
  REQUIRE(beta != nullptr);
  REQUIRE(beta->display_name == "Beta Default");
  REQUIRE(beta->cost_tier == "low");
  REQUIRE(beta->setup_url == "https://beta/setup");
  REQUIRE(beta->docs_url == "https://beta/docs");
  REQUIRE(beta->api_key_label == "Beta Key");
  REQUIRE(beta->api_key_hint == "Get key");
  REQUIRE(beta->base_url == "https://beta.base");
  REQUIRE(beta->kind == "generic_chat");
  REQUIRE(beta->auth_type == "bearer_header");
  REQUIRE(beta->key_param == "key");
  REQUIRE(beta->header_name == "Authorization");
  REQUIRE(beta->bearer_prefix == "Bearer");
  REQUIRE(beta->credential_provider_key == "beta");
  REQUIRE(beta->models.size() == 1);
  REQUIRE(beta->models[0].cost_tier == "free");

  const auto* gamma = holder::api::support::find_cloud_provider(cfg.value(), "gamma");
  REQUIRE(gamma != nullptr);
  REQUIRE(gamma->display_name == "Gamma Default");
}

TEST_CASE("CloudConfig helper selectors cover null and requested branches", "[cloud_config]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.models.push_back(holder::api::support::CloudModelConfig{
      .id = "m-default", .endpoint = "/d", .role = "default"});
  provider.models.push_back(holder::api::support::CloudModelConfig{
      .id = "m-compact", .endpoint = "/c", .role = "compact"});
  provider.models.push_back(holder::api::support::CloudModelConfig{
      .id = "m-other", .endpoint = "/o", .role = ""});

  REQUIRE(holder::api::support::find_cloud_model(provider, "missing") == nullptr);
  REQUIRE(holder::api::support::find_cloud_model(provider, "m-other") != nullptr);
  REQUIRE(holder::api::support::choose_cloud_model(provider, "m-other")->id == "m-other");
  REQUIRE(holder::api::support::choose_cloud_model(provider, "missing")->id == "m-default");

  const auto candidates = holder::api::support::cloud_model_candidates(provider, "m-compact");
  REQUIRE(candidates.size() == 3);
  REQUIRE(candidates[0]->id == "m-compact");
  REQUIRE(candidates[1]->id == "m-default");
  REQUIRE(candidates[2]->id == "m-other");

  holder::api::support::CloudProviderConfig empty_provider;
  REQUIRE(holder::api::support::choose_cloud_model(empty_provider, "") == nullptr);

  holder::api::support::CloudProvidersConfig cfg;
  cfg.providers.push_back(provider);
  REQUIRE(holder::api::support::find_cloud_provider(cfg, "missing") == nullptr);
}
