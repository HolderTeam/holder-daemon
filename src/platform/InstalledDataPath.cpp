#include "platform/InstalledDataPath.h"

#ifndef HOLDER_INSTALL_DATADIR
#define HOLDER_INSTALL_DATADIR ""
#endif

namespace holder::core {

// LCOV_EXCL_START
std::optional<std::filesystem::path> installed_data_path(const std::filesystem::path& rel_path) {
  const std::filesystem::path root(HOLDER_INSTALL_DATADIR);
  if (root.empty()) return std::nullopt;

  std::filesystem::path candidate = root / rel_path;
  if (std::filesystem::exists(candidate)) return candidate;

  return std::nullopt;
}
// LCOV_EXCL_STOP

} // namespace holder::core
