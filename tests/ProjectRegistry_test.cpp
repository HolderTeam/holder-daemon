#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "http_test_helpers.h"
#include "platform/ProjectRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {

std::filesystem::path canonical_test_path(const std::filesystem::path& path) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) canonical = std::filesystem::absolute(path, ec);
  if (ec) canonical = path.lexically_normal();
  return canonical;
}

} // namespace

TEST_CASE("ProjectRegistry preserves and updates project roots", "[project][registry]") {
  const auto dir = holder::test::make_temp_dir();
  holder::core::ProjectRegistry registry(dir / "config" / "projects.json");

  holder::model::Project first;
  first.project_id = "project-one";
  first.root_path = (dir / "outside" / "one").string();
  registry.remember({first});

  holder::model::Project second;
  second.project_id = "project-two";
  second.root_path = (dir / "outside" / "two").string();
  first.root_path = (dir / "moved" / "one").string();
  registry.remember({first, second});

  const auto roots = registry.roots();
  REQUIRE(roots.size() == 2);
  REQUIRE(std::find(roots.begin(), roots.end(), canonical_test_path(first.root_path)) != roots.end());
  REQUIRE(std::find(roots.begin(), roots.end(), canonical_test_path(second.root_path)) != roots.end());
}

TEST_CASE("ProjectRegistry rejects unsupported content", "[project][registry]") {
  const auto dir = holder::test::make_temp_dir();
  const auto path = dir / "projects.json";
  {
    std::ofstream out(path);
    out << R"({"version":2,"projects":[]})";
  }
  holder::core::ProjectRegistry registry(path);
  REQUIRE_THROWS(registry.roots());
}
