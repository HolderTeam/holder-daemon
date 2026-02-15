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
