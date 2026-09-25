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
  REQUIRE(
      std::find(roots.begin(), roots.end(), canonical_test_path(first.root_path)) != roots.end()
  );
  REQUIRE(
      std::find(roots.begin(), roots.end(), canonical_test_path(second.root_path)) != roots.end()
  );
}

TEST_CASE("ProjectRegistry rejects unsupported content", "[project][registry]") {
  const auto dir = holder::test::make_temp_dir();
  const auto path = dir / "projects.json";
  {
    std::ofstream out(path);
    out << R"({"version":3,"projects":[]})";
  }
  holder::core::ProjectRegistry registry(path);
  REQUIRE_THROWS(registry.roots());
}

TEST_CASE(
    "ProjectRegistry rejects malformed entries and incomplete projects",
    "[project][registry]"
) {
  const auto path = holder::test::make_temp_dir() / "registry.json";
  holder::core::ProjectRegistry registry(path);
  for (const auto& entry : std::vector<nlohmann::json>{
           nullptr,
           {{"project_id", "p"}},
           {{"project_id", 12}, {"root_path", 12}}
       }) {
    std::ofstream(path
    ) << nlohmann::json{{"version", 1}, {"projects", nlohmann::json::array({entry})}};
    REQUIRE_THROWS(registry.roots());
    REQUIRE_THROWS(registry.remember({}));
  }
  std::ofstream(path) << R"({"version":1,"projects":[]})";
  REQUIRE_THROWS_AS(registry.remember({holder::model::Project{}}), std::invalid_argument);
}

TEST_CASE(
    "ProjectRegistry keeps removals across refresh and explicit recovery",
    "[project][registry]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto registry_path = dir / "config" / "projects.json";
  const auto managed = dir / "projects";
  holder::model::Project removed;
  removed.project_id = "removed-home";
  removed.root_path = (managed / "home").string();
  holder::model::Project other;
  other.project_id = "other-project";
  other.root_path = (dir / "external-project").string();
  auto write_identity = [](const holder::model::Project& project) {
    const auto metadata = std::filesystem::path(project.root_path) / ".holder";
    std::filesystem::create_directories(metadata);
    std::ofstream(metadata / "project.json") << nlohmann::json{{"project_id", project.project_id}};
  };
  write_identity(removed);
  write_identity(other);
  holder::core::ProjectRegistry registry(registry_path);
  registry.remember({removed, other});
  registry.forget(removed);

  // A late refresh with a pre-deletion snapshot must not restore the project.
  registry.remember({removed, other});
  auto roots = holder::core::ProjectRegistry(registry_path).discover_roots(managed);
  REQUIRE(roots == std::vector<std::filesystem::path>{canonical_test_path(other.root_path)});
  REQUIRE(std::filesystem::exists(removed.root_path));

  // Moving or copying retained files must not evade the removed identity.
  auto moved = removed;
  moved.root_path = (managed / "moved-home").string();
  write_identity(moved);
  REQUIRE(registry.discover_roots(managed) == roots);

  // Explicit recovery permits the chosen copy, leaving the obsolete copy excluded.
  registry.restore(moved);
  auto restored_roots = registry.discover_roots(managed);
  REQUIRE(restored_roots.size() == 2);
  REQUIRE(
      std::find(
          restored_roots.begin(),
          restored_roots.end(),
          canonical_test_path(moved.root_path)
      ) != restored_roots.end()
  );
  REQUIRE(
      std::find(
          restored_roots.begin(),
          restored_roots.end(),
          canonical_test_path(removed.root_path)
      ) == restored_roots.end()
  );
  registry.forget(moved);
  REQUIRE(registry.discover_roots(managed) == roots);
  std::ifstream input(registry_path);
  const auto body = nlohmann::json::parse(input);
  REQUIRE(body["version"] == 2);
  REQUIRE(body["removed_projects"].size() == 2);
}

TEST_CASE(
    "ProjectRegistry excludes removed legacy roots without manifests",
    "[project][registry]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto managed = dir / "projects";
  holder::model::Project project;
  project.project_id = "legacy-home";
  project.root_path = (managed / "home").string();
  std::filesystem::create_directories(std::filesystem::path(project.root_path) / "cards");
  holder::core::ProjectRegistry registry(dir / "projects.json");
  REQUIRE(registry.discover_roots(managed).size() == 1);
  registry.forget(project);
  REQUIRE(registry.discover_roots(managed).empty());
}

TEST_CASE(
    "ProjectRegistry rejects malformed removal records without replacing them",
    "[project][registry]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto path = dir / "projects.json";
  holder::core::ProjectRegistry registry(path);
  holder::model::Project project;
  project.project_id = "home";
  project.root_path = (dir / "home").string();
  for (const auto& removed : std::vector<nlohmann::json>{
           nullptr,
           "invalid",
           nlohmann::json::array({{{"project_id", "home"}}})
       }) {
    const auto body = nlohmann::json{
        {"version", 2},
        {"projects", nlohmann::json::array()},
        {"removed_projects", removed}
    };
    { std::ofstream(path) << body; }
    REQUIRE_THROWS(registry.roots());
    REQUIRE_THROWS(registry.discover_roots(dir));
    REQUIRE_THROWS(registry.remember({project}));
    REQUIRE_THROWS(registry.forget(project));
    REQUIRE_THROWS(registry.restore(project));
    std::ifstream input(path);
    REQUIRE(nlohmann::json::parse(input) == body);
  }
}
