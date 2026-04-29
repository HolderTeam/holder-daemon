#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/ConcurrencyProfilePolicy.h"

TEST_CASE("concurrency_profile_for_caste maps machine tiers to expected thread counts", "[startup]") {
  SECTION("mini uses smallest profile") {
    const auto profile = holder::core::concurrency_profile_for_caste(Caste::Mini);
    REQUIRE(profile.io_threads == 1);
    REQUIRE(profile.general_workers == 2);
    REQUIRE(profile.ingress_workers == 1);
    REQUIRE(profile.save_workers == 1);
    REQUIRE(profile.writer_workers == 1);
  }

  SECTION("user uses normal profile") {
    const auto profile = holder::core::concurrency_profile_for_caste(Caste::User);
    REQUIRE(profile.io_threads == 1);
    REQUIRE(profile.general_workers == 3);
    REQUIRE(profile.ingress_workers == 1);
    REQUIRE(profile.save_workers == 1);
    REQUIRE(profile.writer_workers == 1);
  }

  SECTION("developer and above use stronger profile") {
    for (const auto caste : {Caste::Developer, Caste::Workstation, Caste::Rig}) {
      const auto profile = holder::core::concurrency_profile_for_caste(caste);
      REQUIRE(profile.io_threads == 2);
      REQUIRE(profile.general_workers == 4);
      REQUIRE(profile.ingress_workers == 1);
      REQUIRE(profile.save_workers == 1);
      REQUIRE(profile.writer_workers == 1);
    }
  }
}
