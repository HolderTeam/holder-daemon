#pragma once

#include "model/Project.h"

#include <filesystem>
#include <vector>

namespace holder::core {

// Device-local discovery index for project roots. Project identity and content
// remain owned by each repository's manifests; this registry only lets a fresh
// SQLite projection find roots outside the managed projects directory and records
// deliberate local removals so retained files are not automatically re-imported.
class ProjectRegistry {
 public:
  explicit ProjectRegistry(std::filesystem::path path);

  std::vector<std::filesystem::path> roots() const;
  std::vector<std::filesystem::path> discover_roots(const std::filesystem::path& managed_root
  ) const;
  // Ordinary refreshes must not undo a removal using a stale database snapshot.
  void remember(const std::vector<holder::model::Project>& projects) const;
  void forget(const holder::model::Project& project) const;
  // Explicit creation/import can register a previously removed identity again.
  void restore(const holder::model::Project& project) const;

 private:
  std::filesystem::path path_;
};

} // namespace holder::core
