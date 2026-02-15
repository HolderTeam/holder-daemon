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
  const auto yaml_path = dir / "cloudproviders.yaml";

  std::ofstream out(yaml_path);
  REQUIRE(out.is_open());
  out << "defaults:\n";
  out << "  route_policy:\n";
  out << "    default_provider: chocolatefactory\n";
  out << "  cooldown:\n";
  out << "    base_seconds: 45\n";
  out << "    cap_seconds: 600\n";
  out << "  compaction:\n";
  out << "    summary_refresh:\n";
  out << "      trigger_context_tokens: 1800\n";
  out << "      source_context_tokens: 2400\n";
  out << "      response_tokens_budget: 320\n";
  out << "      max_summary_chars: 4200\n";
  out << "      min_interval_seconds: 90\n";
  out << "      min_delta_tokens: 220\n";
  out << "      force_refresh_tokens: 3600\n";
  out << "providers:\n";
  out << "  - id: chocolatefactory\n";
  out << "    enabled: true\n";
  out << "    cooldown:\n";
  out << "      base_seconds: 50\n";
  out << "      cap_seconds: 700\n";
  out << "    api:\n";
  out << "      base_url: https://example.com\n";
  out << "      kind: chocolatefactory_generative_language\n";
  out << "    auth:\n";
  out << "      type: api_key_query\n";
  out << "      key_param: key\n";
  out << "    models:\n";
  out << "      - id: gemma-3-12b-it\n";
  out << "        endpoint: /v1beta/models/gemma-3-12b-it:generateContent\n";
  out << "        cooldown:\n";
  out << "          base_seconds: 12\n";
  out << "          cap_seconds: 180\n";
  out.close();

  EnvGuard env("HOLDER_CLOUDPROVIDERS_PATH", yaml_path.string());
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
  REQUIRE(cfg->providers[0].models.size() == 1);
  REQUIRE(cfg->providers[0].models[0].cooldown_base_seconds == 12);
  REQUIRE(cfg->providers[0].models[0].cooldown_cap_seconds == 180);
}

TEST_CASE("CloudConfig rejects unknown provider api.kind", "[cloud_config]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_config_unknown_kind_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto yaml_path = dir / "cloudproviders.yaml";

  std::ofstream out(yaml_path);
  REQUIRE(out.is_open());
  out << "providers:\n";
  out << "  - id: unknown-provider\n";
  out << "    enabled: true\n";
  out << "    api:\n";
  out << "      base_url: https://example.com\n";
  out << "      kind: made_up_kind\n";
  out.close();

  EnvGuard env("HOLDER_CLOUDPROVIDERS_PATH", yaml_path.string());
  REQUIRE_THROWS_WITH(holder::api::support::load_cloudproviders_config(),
                      Catch::Matchers::ContainsSubstring("unsupported api.kind 'made_up_kind'"));
}
