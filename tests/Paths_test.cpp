#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/BaseDir.h"
#include "platform/Paths.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

std::filesystem::path make_temp_dir(const std::string& prefix) {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / (prefix + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

class EnvGuard {
 public:
  EnvGuard(const char* key, const std::string& value)
      : key_(key) {
    if (const char* current = std::getenv(key_)) {
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

#ifndef _WIN32
mode_t mode_bits(const std::filesystem::path& path) {
  struct stat st {};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_mode & 0777;
}
#endif

} // namespace

TEST_CASE("Paths resolve uses XDG homes and app id", "[paths]") {
  auto xdg_root = make_temp_dir("holder_paths_resolve_test_");
  EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto paths = holder::core::Paths::resolve("holder-test");
  REQUIRE(paths.data_dir == (xdg_root / "data" / "holder-test"));
  REQUIRE(paths.config_dir == (xdg_root / "config" / "holder-test"));
  REQUIRE(paths.cache_dir == (xdg_root / "cache" / "holder-test"));
}

TEST_CASE("BaseDir resolves home and ignores relative XDG homes", "[paths]") {
  auto home = make_temp_dir("holder_basedir_home_");
  EnvGuard home_env("HOME", home.string());
  EnvGuard data_env("XDG_DATA_HOME", "relative-data");
  EnvGuard config_env("XDG_CONFIG_HOME", "relative-config");
  EnvGuard cache_env("XDG_CACHE_HOME", "relative-cache");

  REQUIRE(XdgUtils::BaseDir::Home() == home.string());
  REQUIRE(XdgUtils::BaseDir::XdgDataHome() == (home / ".local/share").string());
  REQUIRE(XdgUtils::BaseDir::XdgConfigHome() == (home / ".config").string());
  REQUIRE(XdgUtils::BaseDir::XdgCacheHome() == (home / ".cache").string());

#ifndef _WIN32
  EnvGuard empty_home("HOME", "");
  REQUIRE_FALSE(XdgUtils::BaseDir::Home().empty());
#endif
}

TEST_CASE("Paths info_path is derived from server_dir", "[paths]") {
  holder::core::Paths p;
  p.data_dir = "/tmp/holder_paths_info";
  p.config_dir = "/tmp/holder_paths_info_config";
  p.cache_dir = "/tmp/holder_paths_info_cache";

  REQUIRE(p.info_path() == (p.server_dir() / "holder.json"));
}

TEST_CASE("Paths ensure_dirs creates all required directories", "[paths]") {
  namespace fs = std::filesystem;
  auto root = make_temp_dir("holder_paths_ensure_ok_");

  holder::core::Paths p;
  p.data_dir = root / "data";
  p.config_dir = root / "config";
  p.cache_dir = root / "cache";

  REQUIRE_NOTHROW(p.ensure_dirs());
  REQUIRE(fs::is_directory(p.server_dir()));
  REQUIRE(fs::is_directory(p.log_dir()));
  REQUIRE(fs::is_directory(p.config_dir));
  REQUIRE(fs::is_directory(p.cache_dir));
#ifndef _WIN32
  REQUIRE(mode_bits(p.server_dir()) == 0700);
  REQUIRE(mode_bits(p.log_dir()) == 0700);
#endif
}

TEST_CASE("Paths ensure_dirs throws when server_dir cannot be created", "[paths]") {
  namespace fs = std::filesystem;
  auto root = make_temp_dir("holder_paths_server_fail_");

  holder::core::Paths p;
  p.data_dir = root / "as-file";
  p.config_dir = root / "config";
  p.cache_dir = root / "cache";
  {
    std::ofstream out(p.data_dir);
    out << "not a directory";
  }

  REQUIRE_THROWS(p.ensure_dirs());
}

TEST_CASE("Paths ensure_dirs throws when log_dir cannot be created", "[paths]") {
  namespace fs = std::filesystem;
  auto root = make_temp_dir("holder_paths_log_fail_");

  holder::core::Paths p;
  p.data_dir = root / "data";
  p.config_dir = root / "config";
  p.cache_dir = root / "cache";
  fs::create_directories(p.server_dir());
  {
    std::ofstream out(p.log_dir());
    out << "not a directory";
  }

  REQUIRE_THROWS(p.ensure_dirs());
}

TEST_CASE("Paths ensure_dirs throws when config_dir cannot be created", "[paths]") {
  namespace fs = std::filesystem;
  auto root = make_temp_dir("holder_paths_config_fail_");

  holder::core::Paths p;
  p.data_dir = root / "data";
  p.config_dir = root / "config-file";
  p.cache_dir = root / "cache";
  fs::create_directories(p.server_dir());
  fs::create_directories(p.log_dir());
  {
    std::ofstream out(p.config_dir);
    out << "not a directory";
  }

  REQUIRE_THROWS(p.ensure_dirs());
}

TEST_CASE("Paths ensure_dirs throws when cache_dir cannot be created", "[paths]") {
  namespace fs = std::filesystem;
  auto root = make_temp_dir("holder_paths_cache_fail_");

  holder::core::Paths p;
  p.data_dir = root / "data";
  p.config_dir = root / "config";
  p.cache_dir = root / "cache-file";
  fs::create_directories(p.server_dir());
  fs::create_directories(p.log_dir());
  fs::create_directories(p.config_dir);
  {
    std::ofstream out(p.cache_dir);
    out << "not a directory";
  }

  REQUIRE_THROWS(p.ensure_dirs());
}
