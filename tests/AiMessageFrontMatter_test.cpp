#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/AiMessageFrontMatter.h"

TEST_CASE("parse_ai_message_file reads front matter and body", "[ai_message_front_matter]") {
  const std::string raw =
      "---\n"
      "message_id: msg-1\n"
      "project_id: proj-1\n"
      "thread_id: thread-1\n"
      "role: user\n"
      "source: manual\n"
      "provider: Ollama\n"
      "model: llama\n"
      "created_at: 10\n"
      "prompt_hash: hash\n"
      "meta_json: \"{\\\"tokens\\\": 2}\"\n"
      "links:\n"
      "  - to: card-1\n"
      "    to_type: card\n"
      "    kind: ref\n"
      "    created_at: 12\n"
      "  - to: res-1\n"
      "    to_type: resource\n"
      "    kind: source\n"
      "    created_at: 13\n"
      "---\n"
      "Hello\n";

  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.project_id == "proj-1");
  REQUIRE(parsed.message.message_id == "msg-1");
  REQUIRE(parsed.message.thread_id == "thread-1");
  REQUIRE(parsed.message.role == "user");
  REQUIRE(parsed.message.source == "manual");
  REQUIRE(parsed.message.provider.has_value());
  REQUIRE(parsed.message.model.has_value());
  REQUIRE(parsed.message.prompt_hash.has_value());
  REQUIRE(parsed.message.meta_json.has_value());
  REQUIRE(parsed.links.size() == 2);
  REQUIRE(parsed.links[0].to_card_id == "card-1");
  REQUIRE(parsed.links[0].to_type == "card");
  REQUIRE(parsed.links[1].to_card_id == "res-1");
  REQUIRE(parsed.links[1].to_type == "resource");
  REQUIRE(parsed.body == "Hello\n");
}

TEST_CASE("parse_ai_message_file falls back when no front matter", "[ai_message_front_matter]") {
  const std::string raw = "Just a body\n";
  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}
