#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/support/LocalModelRouting.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_local_model_routing_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

class EnvGuard {
 public:
  EnvGuard(const char* key, const std::string& value)
      : key_(key) {
    const char* current = std::getenv(key_);
    if (current != nullptr) {
      had_old_ = true;
      old_ = current;
    }
    setenv(key_, value.c_str(), 1);
  }

  ~EnvGuard() {
    if (had_old_) {
      setenv(key_, old_.c_str(), 1);
    } else {
      unsetenv(key_);
    }
  }

 private:
  const char* key_;
  bool had_old_ = false;
  std::string old_;
};

} // namespace

TEST_CASE(
    "LocalModelRouting normalize helpers cover aliases and unknowns",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  REQUIRE(trim_ascii("  hi\t") == "hi");
  REQUIRE(lowercase_ascii("AbC") == "abc");

  REQUIRE(normalize_caste_name(" MINI ") == "mini");
  REQUIRE(normalize_caste_name("tiny") == "mini");
  REQUIRE(normalize_caste_name("budget") == "user");
  REQUIRE(normalize_caste_name("laptop") == "developer");
  REQUIRE(normalize_caste_name("gaming") == "workstation");
  REQUIRE(normalize_caste_name("unknown").empty());
}

TEST_CASE("LocalModelRouting caste comparisons", "[local_model_routing]") {
  using namespace holder::api::support;

  REQUIRE(caste_meets_or_exceeds("workstation", "developer"));
  REQUIRE_FALSE(caste_meets_or_exceeds("user", "rig"));
  REQUIRE_FALSE(caste_meets_or_exceeds("mystery", "mini"));
  REQUIRE_FALSE(caste_meets_or_exceeds("mini", "mystery"));
}

TEST_CASE(
    "LocalModelRouting load_local_model_meta handles missing and invalid catalog",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  const auto dir = make_temp_dir();
  const auto bad_yaml = dir / "bad.yaml";
  {
    std::ofstream out(bad_yaml);
    REQUIRE(out.is_open());
    out << "models: [\n"; // malformed
  }
  {
    EnvGuard env("HOLDER_AI_CATALOG_PATH", bad_yaml.string());
    const auto meta = load_local_model_meta();
    REQUIRE(meta.empty());
  }

  const auto wrong_shape = dir / "wrong-shape.yaml";
  {
    std::ofstream out(wrong_shape);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  NotModels:\n";
    out << "    Local: []\n";
  }
  {
    EnvGuard env("HOLDER_AI_CATALOG_PATH", wrong_shape.string());
    const auto meta = load_local_model_meta();
    REQUIRE(meta.empty());
  }
}

TEST_CASE(
    "LocalModelRouting load_local_model_meta parses local metadata and sizes",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  const auto dir = make_temp_dir();
  const auto catalog = dir / "ai_catalog.yaml";
  {
    std::ofstream out(catalog);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Local:\n";
    out << "      - tag: small-router\n";
    out << "        provider: ollama\n";
    out << "        engine: llama\n";
    out << "        category: router\n";
    out << "        hardware_tier: tiny\n";
    out << "        speed: fast\n";
    out << "        quality: medium\n";
    out << "        size: 1.5 GB\n";
    out << "      - tag: dev-model\n";
    out << "        caste: laptop\n";
    out << "        speed: slow\n";
    out << "        quality: high\n";
    out << "        size: 512mb\n";
    out << "      - tag: no-size\n";
    out << "        hardware_tier: rig\n";
    out << "        size: xyz\n";
    out << "      - tag: huge-tb\n";
    out << "        hardware_tier: rig\n";
    out << "        size: 1tb\n";
  }

  EnvGuard env("HOLDER_AI_CATALOG_PATH", catalog.string());
  const auto meta = load_local_model_meta();

  REQUIRE(meta.size() == 4);
  REQUIRE(meta.at("small-router").hardware_tier == "mini");
  REQUIRE(
      meta.at("small-router").size_bytes == static_cast<long long>(1.5 * 1024.0 * 1024.0 * 1024.0)
  );
  REQUIRE(meta.at("dev-model").hardware_tier == "developer");
  REQUIRE(meta.at("dev-model").size_bytes == 512LL * 1024LL * 1024LL);
  REQUIRE(meta.at("no-size").size_bytes == 0);
  REQUIRE(meta.at("huge-tb").size_bytes == 1024LL * 1024LL * 1024LL * 1024LL);
}

TEST_CASE(
    "LocalModelRouting build recommendations sorts by quality then speed",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  std::unordered_map<std::string, LocalModelMeta> meta;
  {
    LocalModelMeta m;
    m.tag = "a";
    m.hardware_tier = "developer";
    m.quality = "high";
    m.speed = "slow";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "b";
    m.hardware_tier = "developer";
    m.quality = "medium";
    m.speed = "fast";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "c";
    m.hardware_tier = "rig";
    m.quality = "high";
    m.speed = "fast";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "d";
    // empty hardware_tier -> skipped
    meta[m.tag] = m;
  }

  std::vector<holder::llm::LocalModel> installed = {
      holder::llm::LocalModel{.name = "b", .digest = "", .size = 10, .modified_at = ""},
  };

  const auto out = build_caste_recommendations(installed, meta, "workstation");
  REQUIRE(out.size() == 2);
  REQUIRE(out[0]["tag"] == "a");
  REQUIRE(out[1]["tag"] == "b");
  REQUIRE(out[1]["installed"] == true);
}

TEST_CASE(
    "LocalModelRouting recommendation tie-break uses speed_rank and tag order",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  std::unordered_map<std::string, LocalModelMeta> meta;
  {
    LocalModelMeta m;
    m.tag = "speed-fast";
    m.hardware_tier = "developer";
    m.quality = "low";
    m.speed = "fast";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "speed-medium";
    m.hardware_tier = "developer";
    m.quality = "low";
    m.speed = "medium";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "speed-slow";
    m.hardware_tier = "developer";
    m.quality = "low";
    m.speed = "slow";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "speed-unknown-a";
    m.hardware_tier = "developer";
    m.quality = "unknown";
    m.speed = "unknown";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "speed-unknown-b";
    m.hardware_tier = "developer";
    m.quality = "unknown";
    m.speed = "unknown";
    meta[m.tag] = m;
  }

  const std::vector<holder::llm::LocalModel> installed;
  const auto out = build_caste_recommendations(installed, meta, "developer");

  REQUIRE(out.size() == 5);
  REQUIRE(out[0]["tag"] == "speed-fast");
  REQUIRE(out[1]["tag"] == "speed-medium");
  REQUIRE(out[2]["tag"] == "speed-slow");
  REQUIRE(out[3]["tag"] == "speed-unknown-a");
  REQUIRE(out[4]["tag"] == "speed-unknown-b");
}

TEST_CASE(
    "LocalModelRouting treats bare tags and :latest as the same installed model",
    "[local_model_routing]"
) {
  using namespace holder::api::support;

  std::unordered_map<std::string, LocalModelMeta> meta;
  {
    LocalModelMeta m;
    m.tag = "llama3.2";
    m.hardware_tier = "developer";
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "phi4-mini";
    m.hardware_tier = "developer";
    meta[m.tag] = m;
  }

  SECTION("installed model has :latest but recommendation is bare tag") {
    const std::vector<holder::llm::LocalModel> installed = {
        holder::llm::LocalModel{
            .name = "llama3.2:latest",
            .digest = "",
            .size = 10,
            .modified_at = ""
        },
    };

    const auto out = build_caste_recommendations(installed, meta, "developer");
    REQUIRE(out.size() == 2);
    REQUIRE(out[0]["tag"] == "llama3.2");
    REQUIRE(out[0]["installed"] == true);
  }

  SECTION("installed model is bare tag but recommendation uses :latest") {
    std::unordered_map<std::string, LocalModelMeta> latest_meta;
    {
      LocalModelMeta m;
      m.tag = "llama3.2:latest";
      m.hardware_tier = "developer";
      latest_meta[m.tag] = m;
    }

    const std::vector<holder::llm::LocalModel> installed = {
        holder::llm::LocalModel{.name = "llama3.2", .digest = "", .size = 10, .modified_at = ""},
    };

    const auto out = build_caste_recommendations(installed, latest_meta, "developer");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0]["tag"] == "llama3.2:latest");
    REQUIRE(out[0]["installed"] == true);
  }
}

TEST_CASE(
    "LocalModelRouting parse_ranked_models filters and de-duplicates",
    "[local_model_routing]"
) {
  using namespace holder::api::support;
  const std::vector<std::string> candidates = {"m1", "m2", "m3"};

  REQUIRE(parse_ranked_models("no array here", candidates).empty());
  REQUIRE(parse_ranked_models("[broken", candidates).empty());
  REQUIRE(parse_ranked_models(R"(prefix ["m1",] suffix)", candidates).empty());
  REQUIRE(parse_ranked_models(R"({"x":1})", candidates).empty());

  const auto ranked =
      parse_ranked_models(R"(prefix ["m2","m1","m2","unknown",123] suffix)", candidates);
  REQUIRE(ranked.size() == 2);
  REQUIRE(ranked[0] == "m2");
  REQUIRE(ranked[1] == "m1");
}

TEST_CASE("LocalModelRouting model pickers choose expected tags", "[local_model_routing]") {
  using namespace holder::api::support;

  const std::vector<holder::llm::LocalModel> models = {
      holder::llm::LocalModel{.name = "m-large", .digest = "", .size = 3000, .modified_at = ""},
      holder::llm::LocalModel{.name = "m-zero", .digest = "", .size = 0, .modified_at = ""},
      holder::llm::LocalModel{.name = "m-small", .digest = "", .size = 1000, .modified_at = ""},
  };
  REQUIRE(pick_smallest_model(models) == "m-small");
  REQUIRE(pick_largest_model(models) == "m-large");
  REQUIRE(pick_smallest_model({}).empty());
  REQUIRE(pick_largest_model({}).empty());

  std::unordered_map<std::string, LocalModelMeta> meta;
  {
    LocalModelMeta m;
    m.tag = "m-small";
    m.category = "router";
    m.size_bytes = 900;
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "m-large";
    m.category = "router";
    m.size_bytes = 2500;
    meta[m.tag] = m;
  }
  {
    LocalModelMeta m;
    m.tag = "m-zero";
    m.category = "general";
    m.size_bytes = 10;
    meta[m.tag] = m;
  }

  REQUIRE(pick_router_model(models, meta) == "m-small");
  meta.clear();
  REQUIRE(pick_router_model(models, meta).empty());

  const std::vector<holder::llm::LocalModel> increasing = {
      holder::llm::LocalModel{.name = "x-small", .digest = "", .size = 10, .modified_at = ""},
      holder::llm::LocalModel{.name = "x-large", .digest = "", .size = 100, .modified_at = ""},
  };
  REQUIRE(pick_largest_model(increasing) == "x-large");
}
