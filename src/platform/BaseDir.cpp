#include "platform/BaseDir.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace XdgUtils {
namespace BaseDir {

namespace {

std::string env_value(const char* key) {
  const char* value = std::getenv(key);
  return value == nullptr ? std::string() : std::string(value);
}

std::string home_from_system() {
#ifdef _WIN32
  return env_value("USERPROFILE");
#else
  if (passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
    return pw->pw_dir;
  }
  return {};
#endif
}

std::string home_dir() {
  std::string home = env_value("HOME");
  if (!home.empty()) return home;

  home = home_from_system();
  if (!home.empty()) return home;

  return ".";
}

std::string absolute_env_or_default(const char* key, const char* suffix) {
  const std::string value = env_value(key);
  if (!value.empty() && std::filesystem::path(value).is_absolute()) {
    return value;
  }
  return (std::filesystem::path(home_dir()) / suffix).string();
}

} // namespace

const std::string Home() { return home_dir(); }

const std::string XdgDataHome() { return absolute_env_or_default("XDG_DATA_HOME", ".local/share"); }

const std::string XdgConfigHome() { return absolute_env_or_default("XDG_CONFIG_HOME", ".config"); }

const std::string XdgCacheHome() { return absolute_env_or_default("XDG_CACHE_HOME", ".cache"); }

} // namespace BaseDir
} // namespace XdgUtils
