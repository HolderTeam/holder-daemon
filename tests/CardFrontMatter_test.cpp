#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/CardFrontMatter.h"

#include <string>

TEST_CASE("parse_card_file reads front matter and body", "[card_front_matter]") {
  const std::string raw =
      "---\n"
      "card_id: abcd1234\n"
      "project_id: proj-1\n"
      "title: Hello\n"
      "created_at: 10\n"
      "updated_at: 20\n"
      "parent_card_id: null\n"
      "sort_key: 1.5\n"
      "rel_path: cards/ab/cd/abcd1234.md\n"
      "deleted_at: null\n"
      "---\n"
      "Body text\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.body == "Body text\n");
  REQUIRE(parsed.card.card_id == "abcd1234");
  REQUIRE(parsed.card.project_id == "proj-1");
  REQUIRE(parsed.card.title == "Hello");
  REQUIRE(parsed.card.created_at == 10);
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE_FALSE(parsed.card.parent_card_id.has_value());
  REQUIRE(parsed.card.sort_key == 1.5);
  REQUIRE(parsed.card.rel_path == "cards/ab/cd/abcd1234.md");
  REQUIRE_FALSE(parsed.card.deleted_at.has_value());
}

TEST_CASE("parse_card_file falls back when no front matter", "[card_front_matter]") {
  const std::string raw = "Just a body\n";
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}
