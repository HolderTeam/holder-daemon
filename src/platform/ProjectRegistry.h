#pragma once

#include "model/Project.h"

#include <filesystem>
#include <vector>

namespace holder::core {

// Device-local discovery index for project roots. Project identity and content
// remain owned by each repository's manifests; this registry only lets a fresh
// SQLite projection find roots outside the managed projects directory.
class ProjectRegistry {
 public:
  explicit ProjectRegistry(std::filesystem::path path);

  std::vector<std::filesystem::path> roots() const;
  void remember(const std::vector<holder::model::Project>& projects) const;

 private:
  std::filesystem::path path_;
};

} // namespace holder::core
