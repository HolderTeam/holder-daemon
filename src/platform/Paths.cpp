#include "platform/Paths.h"

#include <stdexcept>
#include <system_error>

#include <XdgUtils/BaseDir/BaseDir.h>

namespace fs = std::filesystem;

namespace holder::core {

Paths Paths::resolve(std::string app_id) {
  Paths p{};
  p.data_dir   = fs::path(XdgUtils::BaseDir::XdgDataHome())   / app_id;
  p.config_dir = fs::path(XdgUtils::BaseDir::XdgConfigHome()) / app_id;
  p.cache_dir  = fs::path(XdgUtils::BaseDir::XdgCacheHome())  / app_id;
  return p;
}

void Paths::ensure_dirs() const {
  std::error_code ec;

  fs::create_directories(server_dir(), ec);
  if (ec) {
    throw std::runtime_error("Failed to create server_dir: " + server_dir().string() + " (" + ec.message() + ")");
  }

  fs::create_directories(log_dir(), ec);
  if (ec) {
    throw std::runtime_error("Failed to create log_dir: " + log_dir().string() + " (" + ec.message() + ")");
  }

  // Optional: also create config_dir/cache_dir if you intend to use them immediately
  fs::create_directories(config_dir, ec);
  if (ec) {
    throw std::runtime_error("Failed to create config_dir: " + config_dir.string() + " (" + ec.message() + ")");
  }

  fs::create_directories(cache_dir, ec);
  if (ec) {
    throw std::runtime_error("Failed to create cache_dir: " + cache_dir.string() + " (" + ec.message() + ")");
  }
}

} // namespace holder::core // LCOV_EXCL_LINE
