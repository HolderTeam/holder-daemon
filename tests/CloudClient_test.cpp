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

TEST_CASE("CloudClient maps generic responses insufficient_quota to quota_exceeded", "[cloud_client]") {
  const std::string body =
      R"({"error":{"message":"quota","type":"insufficient_quota","code":"insufficient_quota"}})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_responses", 429, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "quota_exceeded");
}

TEST_CASE("CloudClient maps mechatropic low credits to quota_exceeded", "[cloud_client]") {
  const std::string body =
      R"({"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."}})";
  const auto parsed = holder::api::support::parse_cloud_response("mechatropic_messages", 400, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "quota_exceeded");
}

TEST_CASE("CloudClient maps malformed success body", "[cloud_client]") {
  const std::string body = R"({"unexpected":"shape"})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 200, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "malformed_response");
}

TEST_CASE("CloudClient malformed success variants hit parser nullopt branches", "[cloud_client]") {
  SECTION("chocolatefactory missing candidates") {
    const auto parsed = holder::api::support::parse_cloud_response(
        "chocolatefactory_generative_language", 200, R"({"x":1})");
    REQUIRE_FALSE(parsed.text.has_value());
    REQUIRE(parsed.error_code == "malformed_response");
  }

  SECTION("chocolatefactory missing parts shape") {
    const auto parsed = holder::api::support::parse_cloud_response(
        "chocolatefactory_generative_language", 200, R"({"candidates":[{"content":{"no_parts":[]}}]})");
    REQUIRE_FALSE(parsed.text.has_value());
    REQUIRE(parsed.error_code == "malformed_response");
  }

  SECTION("generic chat unsupported content type") {
    const auto parsed = holder::api::support::parse_cloud_response(
        "generic_chat", 200, R"({"choices":[{"message":{"content":{"type":"x"}}}]})");
    REQUIRE_FALSE(parsed.text.has_value());
    REQUIRE(parsed.error_code == "malformed_response");
  }

  SECTION("mechatropic missing content array") {
    const auto parsed = holder::api::support::parse_cloud_response(
        "mechatropic_messages", 200, R"({"x":1})");
    REQUIRE_FALSE(parsed.text.has_value());
    REQUIRE(parsed.error_code == "malformed_response");
  }

  SECTION("generic responses empty output_text") {
    const auto parsed = holder::api::support::parse_cloud_response(
        "generic_responses", 200, R"({"output_text":""})");
    REQUIRE_FALSE(parsed.text.has_value());
    REQUIRE(parsed.error_code == "malformed_response");
  }
}

TEST_CASE("CloudClient truncates long non-2xx response message", "[cloud_client]") {
  const std::string body(800, 'x');
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 400, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "provider_error");
  // "cloud HTTP 400: " is prefix then at most 300 bytes of body
  REQUIRE(parsed.error_message.size() <= std::string("cloud HTTP 400: ").size() + 300);
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

TEST_CASE("CloudClient maps generic_responses insufficient_quota type to quota_exceeded", "[cloud_client]") {
  const std::string body = R"({"error":{"message":"quota","type":"insufficient_quota"}})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_responses", 400, body);
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "quota_exceeded");
}

TEST_CASE("CloudClient parses generic chat array content response", "[cloud_client]") {
  const std::string body =
      R"({"choices":[{"message":{"content":[{"type":"text","text":"hello "},{"type":"text","text":"array"}]}}]})";
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 200, body);
  REQUIRE(parsed.text.has_value());
  REQUIRE(parsed.text.value() == "hello array");
}

TEST_CASE("CloudClient parse_cloud_response catches malformed json body", "[cloud_client]") {
  const auto parsed = holder::api::support::parse_cloud_response("generic_chat", 200, "{");
  REQUIRE_FALSE(parsed.text.has_value());
  REQUIRE(parsed.error_code == "malformed_response");
}

TEST_CASE("CloudClient compact_context_tail handles zero token budget", "[cloud_client]") {
  bool compacted = false;
  const auto out = holder::api::support::compact_context_tail("{}", 0, &compacted);
  REQUIRE(out.empty());
  REQUIRE(compacted);
}

TEST_CASE("CloudClient run_cloud_model rejects unsupported provider kind", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "unsupported_kind";
  provider.auth_type = "bearer_header";
  provider.base_url = "https://invalid.invalid";

  holder::api::support::CloudModelConfig model;
  model.id = "m";
  model.endpoint = "/v1";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("unsupported provider kind") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model rejects missing endpoint", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "bearer_header";
  provider.base_url = "https://invalid.invalid";

  holder::api::support::CloudModelConfig model;
  model.id = "m";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("missing provider model endpoint") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model rejects unsupported auth type", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "magic_auth";
  provider.base_url = "https://invalid.invalid";

  holder::api::support::CloudModelConfig model;
  model.id = "m";
  model.endpoint = "/v1";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("unsupported auth type") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model builds query-key auth target and fails invalid https base", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_responses";
  provider.auth_type = "api_key_query";
  provider.key_param = "api_key";
  provider.base_url = "http://not-https";

  holder::api::support::CloudModelConfig model;
  model.id = "m";
  model.endpoint = "/v1/responses";

  std::string error;
  const auto out =
      holder::api::support::run_cloud_model(provider, model, "k a+b/c", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error: invalid https base_url") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model rejects https base with empty host", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "https:///api";

  holder::api::support::CloudModelConfig model;
  model.id = "chat-test";
  model.endpoint = "/v1/chat";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error: invalid https base_url") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model builds bearer auth and mechatropic payload", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "mechatropic_messages";
  provider.auth_type = "bearer_header";
  provider.header_name = "Authorization";
  provider.bearer_prefix = "Bearer";
  provider.base_url = "http://not-https";

  holder::api::support::CloudModelConfig model;
  model.id = "claude-test";
  model.endpoint = "/v1/messages";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error: invalid https base_url") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model reaches https parse with host/port/base_path", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "chocolatefactory_generative_language";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "https://127.0.0.1:1/base";

  holder::api::support::CloudModelConfig model;
  model.id = "cf-model";
  model.endpoint = "/v1/run";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error:") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model reaches https parse with host only", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "https://localhost";

  holder::api::support::CloudModelConfig model;
  model.id = "chat-test";
  model.endpoint = "/v1/chat";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error:") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model builds header-key auth and generic chat payload", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "http://not-https";

  holder::api::support::CloudModelConfig model;
  model.id = "chat-test";
  model.endpoint = "/v1/chat";

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("transport_error: invalid https base_url") != std::string::npos);
}

TEST_CASE("CloudClient run_cloud_model returns parsed text on transport success", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "https://example.com";

  holder::api::support::CloudModelConfig model;
  model.id = "chat-test";
  model.endpoint = "/v1/chat";

  holder::api::support::set_cloud_transport_post_override_for_tests(
      [](const std::string&,
         const std::string&,
         const std::vector<std::pair<std::string, std::string>>&,
         const std::string&,
         int* out_status,
         std::string* out_body,
         std::string*) -> bool {
        if (out_status) *out_status = 200;
        if (out_body) *out_body = R"({"choices":[{"message":{"content":"ok text"}}]})";
        return true;
      });

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  holder::api::support::clear_cloud_transport_post_override_for_tests();

  REQUIRE(out.has_value());
  REQUIRE(out.value() == "ok text");
  REQUIRE(error.empty());
}

TEST_CASE("CloudClient run_cloud_model propagates parsed cloud error on transport success", "[cloud_client]") {
  holder::api::support::CloudProviderConfig provider;
  provider.id = "p";
  provider.kind = "generic_chat";
  provider.auth_type = "header_key";
  provider.header_name = "x-api-key";
  provider.base_url = "https://example.com";

  holder::api::support::CloudModelConfig model;
  model.id = "chat-test";
  model.endpoint = "/v1/chat";

  holder::api::support::set_cloud_transport_post_override_for_tests(
      [](const std::string&,
         const std::string&,
         const std::vector<std::pair<std::string, std::string>>&,
         const std::string&,
         int* out_status,
         std::string* out_body,
         std::string*) -> bool {
        if (out_status) *out_status = 429;
        if (out_body) *out_body = R"({"error":"too many requests"})";
        return true;
      });

  std::string error;
  const auto out = holder::api::support::run_cloud_model(provider, model, "k", "prompt", &error);
  holder::api::support::clear_cloud_transport_post_override_for_tests();

  REQUIRE_FALSE(out.has_value());
  REQUIRE(error.find("rate_limited:") != std::string::npos);
}
