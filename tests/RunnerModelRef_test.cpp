#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "llm/LocalModelRunner.h"
#include "llm/LocalRunnerClient.h"
#include "llm/RunnerModelRef.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"

TEST_CASE("RunnerModelRef helpers normalize and resolve refs", "[llm]") {
  SECTION("normalize_local_runner_model_ref preserves empty and explicit refs") {
    REQUIRE(holder::llm::normalize_local_runner_model_ref("") == "");
    REQUIRE(holder::llm::normalize_local_runner_model_ref("manual-a::m1") == "manual-a::m1");
    REQUIRE(holder::llm::normalize_local_runner_model_ref("m1") == "auto-local::m1");
  }

  SECTION("local_model_name_from_ref returns raw value for plain model names") {
    REQUIRE(
        holder::llm::local_model_name_from_ref(std::optional<std::string>("m1"), "auto-local") ==
        std::optional<std::string>("m1")
    );
  }

  SECTION(
      "resolve_configured_runner_model returns nullopt for invalid normalized refs and missing clients"
  ) {
    holder::llm::RunnerRegistry empty_registry(nullptr, nullptr);

    REQUIRE_FALSE(holder::llm::resolve_configured_runner_model(
                      std::optional<std::string>("manual-a::"),
                      &empty_registry
    )
                      .has_value());
    REQUIRE_FALSE(holder::llm::resolve_configured_runner_model(
                      std::optional<std::string>("manual-a::m1"),
                      &empty_registry
    )
                      .has_value());
  }

  SECTION("resolve_configured_runner_model resolves auto-local plain refs") {
    holder::llm::LocalModelRunner auto_local_runner;
    holder::llm::LocalRunnerClient auto_local_client(&auto_local_runner);
    holder::llm::RunnerRegistry registry(nullptr, &auto_local_client);

    const auto resolved =
        holder::llm::resolve_configured_runner_model(std::optional<std::string>("m1"), &registry);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->runner_id == "auto-local");
    REQUIRE(resolved->model_name == "m1");
    REQUIRE(resolved->runner == &auto_local_client);
  }
}
