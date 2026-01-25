#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/AiMessagePaths.h"

TEST_CASE("ai_message_rel_path shards by message_id", "[ai_message_paths]") {
  REQUIRE(holder::core::ai_message_rel_path("abcd1234") == "ai_messages/ab/cd/abcd1234.md");
}
