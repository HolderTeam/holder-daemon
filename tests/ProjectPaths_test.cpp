#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "platform/Paths.h"
#include "project/ProjectPaths.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

class EnvGuard {
 public:
  EnvGuard(const char* key, std::string value)
      : key_(key) {
    const char* current = std::getenv(key_);
    if (current) {
      old_ = current;
    }
    set_env(value);
  }

  ~EnvGuard() {
    if (old_.has_value()) {
      set_env(old_.value());
    } else {
      unset_env();
    }
  }

 private:
  void set_env(const std::string& value) {
#ifdef _WIN32
    _putenv_s(key_, value.c_str());
#else
    setenv(key_, value.c_str(), 1);
#endif
  }

  void unset_env() {
#ifdef _WIN32
    _putenv_s(key_, "");
#else
    unsetenv(key_);
#endif
  }

  const char* key_;
  std::optional<std::string> old_;
};

} // namespace

TEST_CASE("slugify converts names to URL-safe slugs", "[project_paths]") {
  using holder::core::slugify;

  REQUIRE(slugify("Hello World") == "hello-world");
  REQUIRE(slugify("  Hello   World  ") == "hello-world");
  REQUIRE(slugify("C++ Is Great!") == "c-is-great");
  REQUIRE(slugify("__My_Project__") == "__my_project__");
  REQUIRE(slugify("--Already--Slug--") == "already-slug");
  REQUIRE(slugify("???") == "project");
}

TEST_CASE("slugify skips non-ascii bytes", "[project_paths]") {
  using holder::core::slugify;

  REQUIRE(slugify("Cafe \xC3\xA9lan") == "cafe-lan");
  REQUIRE(slugify("\xF0\x9F\xA6\x8A") == "project");
}

TEST_CASE("default_projects_root uses env override", "[project_paths]") {
  const auto base = std::filesystem::temp_directory_path() / "holder_projects_root_test";
  EnvGuard guard("HOLDER_PROJECTS_ROOT", base.string());

  REQUIRE(holder::core::default_projects_root() == base);
}

TEST_CASE("default_projects_root falls back to data dir", "[project_paths]") {
  if (const char* current = std::getenv("HOLDER_PROJECTS_ROOT")) {
#ifdef _WIN32
    _putenv_s("HOLDER_PROJECTS_ROOT", "");
#else
    unsetenv("HOLDER_PROJECTS_ROOT");
#endif
  }
  const auto paths = holder::core::Paths::resolve("holder");
  const auto expected = paths.data_dir / "projects";

  REQUIRE(holder::core::default_projects_root() == expected);
}

TEST_CASE("unique_project_root avoids collisions", "[project_paths]") {
  namespace fs = std::filesystem;
  const auto base = fs::temp_directory_path() / "holder_unique_root_test";
  fs::create_directories(base);

  std::vector<holder::model::Project> existing;
  holder::model::Project p1;
  p1.root_path = (base / "demo").string();
  existing.push_back(p1);
  holder::model::Project p2;
  p2.root_path = (base / "demo-2").string();
  existing.push_back(p2);

  const auto unique = holder::core::unique_project_root(base, "demo", existing);
  REQUIRE(unique == (base / "demo-3").string());
}

TEST_CASE("unique_project_root skips existing filesystem paths", "[project_paths]") {
  namespace fs = std::filesystem;
  const auto base = fs::temp_directory_path() / "holder_unique_root_fs_test";
  fs::create_directories(base);

  const auto occupied = base / "demo";
  fs::create_directories(occupied);

  std::vector<holder::model::Project> existing;
  const auto unique = holder::core::unique_project_root(base, "demo", existing);
  REQUIRE(unique == (base / "demo-2").string());
}

TEST_CASE(
    "unique_project_root handles collisions with existing dirs and DB entries",
    "[project_paths]"
) {
  namespace fs = std::filesystem;
  const auto base = fs::temp_directory_path() / "holder_unique_root_collision_test";
  fs::create_directories(base);

  fs::create_directories(base / "demo");
  fs::create_directories(base / "demo-2");

  std::vector<holder::model::Project> existing;
  holder::model::Project p1;
  p1.root_path = (base / "demo-3").string();
  existing.push_back(p1);

  const auto unique = holder::core::unique_project_root(base, "demo", existing);
  REQUIRE(unique == (base / "demo-4").string());
}

TEST_CASE("unique_project_root throws after exhausting candidate attempts", "[project_paths]") {
  namespace fs = std::filesystem;
  const auto base = fs::temp_directory_path() / "holder_unique_root_exhaust_test";
  fs::create_directories(base);

  std::vector<holder::model::Project> existing;
  existing.reserve(1000);
  for (int attempt = 1; attempt <= 1000; ++attempt) {
    holder::model::Project p;
    std::string suffix;
    if (attempt > 1) {
      suffix = "-" + std::to_string(attempt);
    }
    p.root_path = (base / ("demo" + suffix)).string();
    existing.push_back(p);
  }

  bool threw = false;
  try {
    (void)holder::core::unique_project_root(base, "demo", existing);
  } catch (const std::runtime_error& e) {
    threw = true;
    REQUIRE(std::string(e.what()) == "Unable to generate unique project root path.");
  }
  REQUIRE(threw);
}
