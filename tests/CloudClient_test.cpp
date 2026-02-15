#include "api/support/CloudClient.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("CloudClient parses chocolatefactory response", "[cloud_client]") {
  const std::string body =
      R"({"candidates":[{"content":{"parts":[{"text":"hello "},{"text":"world"}]}}]})";
  const auto parsed = holder::api::support::parse_cloud_response(
      "chocolatefactory_generative_language", 200, body);
  REQUIRE(parsed.text.has_value());
  REQUIRE(parsed.text.value() == "hello world");
  REQUIRE(parsed.error_code.empty());
}

TEST_CASE("CloudClient parses generic chat response", "[cloud_client]") {
  const std::string body = R"({"choices":[{"message":{"content":"chat answer"}}]})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 200, body);
  REQUIRE(parsed.text.has_value());
  REQUIRE(parsed.text.value() == "chat answer");
}

TEST_CASE("CloudClient parses generic responses response", "[cloud_client]") {
  const std::string body = R"({"output_text":"responses answer"})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_responses", 200, body);
  REQUIRE(parsed.text.has_value());
  REQUIRE(parsed.text.value() == "responses answer");
}

TEST_CASE("CloudClient parses mechatropic messages response", "[cloud_client]") {
  const std::string body = R"({"content":[{"type":"text","text":"mecha "},{"type":"text","text":"answer"}]})";
  const auto parsed = holder::api::support::parse_cloud_response("mechatropic_messages", 200, body);
  REQUIRE(parsed.text.has_value());
  REQUIRE(parsed.text.value() == "mecha answer");
}

TEST_CASE("CloudClient maps HTTP 429 to rate_limited", "[cloud_client]") {
  const std::string body = R"({"error":"too many requests"})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 429, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "rate_limited");
  REQUIRE_FALSE(parsed.error_message.empty());
}

TEST_CASE("CloudClient maps malformed success body", "[cloud_client]") {
  const std::string body = R"({"unexpected":"shape"})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 200, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "malformed_response");
}

TEST_CASE("CloudClient run override returns mocked output", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "switchyard";
  provider.kind = "generic_chat";
  provider.base_url = "https://127.0.0.1:1";

  holder::api::support::CloudModelConfig model;
  model.id = "openrouter/auto";
  model.endpoint = "/api/v1/chat/completions";

  holder::api::support::set_run_cloud_model_override_for_tests(
      [](const holder::api::support::CloudProviderConfig& p,
         const holder::api::support::CloudModelConfig& m,
         const std::string&,
         const std::string& prompt,
         std::string*) -> std::optional<std::string> {
        if (p.id != "switchyard" || m.id != "openrouter/auto") return std::nullopt;
        return std::string("mocked: ") + prompt;
      });

  std::string error;
  const auto out =
      holder::api::support::run_cloud_model(provider, model, "key", "hello", &error);
  holder::api::support::clear_run_cloud_model_override_for_tests();

  REQUIRE(out.has_value());
  REQUIRE(out.value() == "mocked: hello");
  REQUIRE(error.empty());
}
