#include "api/support/CloudConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
  out << "  compaction:\n";
  out << "    summary_refresh:\n";
  out << "      trigger_context_tokens: 1800\n";
  out << "      source_context_tokens: 2400\n";
  out << "      response_tokens_budget: 320\n";
  out << "      max_summary_chars: 4200\n";
  out << "providers:\n";
  out << "  - id: chocolatefactory\n";
  out << "    enabled: true\n";
  out << "    api:\n";
  out << "      base_url: https://example.com\n";
  out << "      kind: chocolatefactory_generative_language\n";
  out << "    auth:\n";
  out << "      type: api_key_query\n";
  out << "      key_param: key\n";
  out << "    models:\n";
  out << "      - id: gemma-3-12b-it\n";
  out << "        endpoint: /v1beta/models/gemma-3-12b-it:generateContent\n";
  out.close();

  EnvGuard env("HOLDER_CLOUDPROVIDERS_PATH", yaml_path.string());
  const auto cfg = holder::api::support::load_cloudproviders_config();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->summary_refresh.trigger_context_tokens == 1800);
  REQUIRE(cfg->summary_refresh.source_context_tokens == 2400);
  REQUIRE(cfg->summary_refresh.response_tokens_budget == 320);
  REQUIRE(cfg->summary_refresh.max_summary_chars == 4200);
}

