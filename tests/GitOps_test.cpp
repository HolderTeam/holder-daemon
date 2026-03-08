#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitOps.h"

TEST_CASE("RealGitOps probe_remote throws when repo is not opened", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_THROWS(ops.probe_remote("origin"));
}

TEST_CASE("RealGitOps push_branch throws when repo is not opened", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_THROWS(ops.push_branch("origin", "cards", true));
}
