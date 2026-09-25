#include "platform/ProjectRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#endif

namespace holder::core {
namespace {

// Serialize read/modify/write operations from concurrent request workers.
std::mutex registry_mutex;

struct RegistryState {
  int version = 1;
  std::map<std::string, std::string> projects;
  std::map<std::string, std::set<std::string>> removed;
};

std::string canonical_path_string(const std::filesystem::path& path);

// A fixed ".tmp" name would let two concurrent remember() calls (e.g. separate
// processes writing the same registry) race on the same temp file before either
// atomic rename happens, corrupting it. Make each writer's temp file unique.
std::string unique_temp_suffix() {
  static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(::_getpid());
#else
  const auto pid = static_cast<unsigned long>(::getpid());
#endif
  return "." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1));
}

RegistryState load_registry(const std::filesystem::path& path) {
  RegistryState state;
  if (!std::filesystem::exists(path)) return state;
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open project registry: " + path.string());
  const auto body = nlohmann::json::parse(in);
  state.version = body.value("version", 0);
  if ((state.version != 1 && state.version != 2) || !body.contains("projects") ||
      !body.at("projects").is_array() ||
      (state.version == 2 &&
       (!body.contains("removed_projects") || !body.at("removed_projects").is_array()))) {
    throw std::runtime_error("unsupported project registry format: " + path.string());
  }
  auto read_entries = [&](const nlohmann::json& entries, bool removed) {
    for (const auto& item : entries) {
      if (!item.is_object() || !item.contains("project_id") || !item.contains("root_path") ||
          !item.at("project_id").is_string() || !item.at("root_path").is_string() ||
          item.at("project_id").get<std::string>().empty() ||
          item.at("root_path").get<std::string>().empty()) {
        throw std::runtime_error("invalid project registry entry: " + path.string());
      }
      const auto id = item.at("project_id").get<std::string>();
      const auto root = canonical_path_string(item.at("root_path").get<std::string>());
      if (removed)
        state.removed[id].insert(root);
      else
        state.projects[id] = root;
    }
  };
  read_entries(body.at("projects"), false);
  if (state.version == 2) read_entries(body.at("removed_projects"), true);
  return state;
}

std::string canonical_path_string(const std::filesystem::path& path) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) canonical = std::filesystem::absolute(path, ec);
  if (ec) canonical = path.lexically_normal();
  return canonical.string();
}

void restrict_file(const std::filesystem::path& path) {
#ifndef _WIN32
  // LCOV_EXCL_START: requires chmod fault injection.
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to restrict project registry permissions: " + path.string());
  }
  // LCOV_EXCL_STOP
#else
  (void)path;
#endif
}

void save_registry(const std::filesystem::path& path, const RegistryState& state) {
  nlohmann::json body = {{"version", state.version}, {"projects", nlohmann::json::array()}};
  for (const auto& [id, root] : state.projects) {
    body["projects"].push_back({{"project_id", id}, {"root_path", root}});
  }
  if (state.version == 2) {
    body["removed_projects"] = nlohmann::json::array();
    for (const auto& [id, roots] : state.removed) {
      for (const auto& root : roots) {
        body["removed_projects"].push_back({{"project_id", id}, {"root_path", root}});
      }
    }
  }
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp" + unique_temp_suffix();
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    // LCOV_EXCL_START: requires disk-write fault injection after the directory is created.
    if (!out) {
      throw std::runtime_error("failed to write project registry: " + temporary.string());
    }
    // LCOV_EXCL_STOP
    out << body.dump(2) << '\n';
    out.flush();
    // LCOV_EXCL_START: requires disk-flush fault injection.
    if (!out) {
      throw std::runtime_error("failed to flush project registry: " + temporary.string());
    }
    // LCOV_EXCL_STOP
  }
  restrict_file(temporary);

  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
#ifdef _WIN32
  if (ec && std::filesystem::exists(path)) {
    std::filesystem::remove(path, ec);
    if (!ec) std::filesystem::rename(temporary, path, ec);
  }
#endif
  // LCOV_EXCL_START: requires atomic rename syscall fault injection.
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace project registry: " + ec.message());
  }
  // LCOV_EXCL_STOP
  restrict_file(path);
}

void validate_project(const holder::model::Project& project) {
  if (project.project_id.empty() || project.root_path.empty()) {
    throw std::invalid_argument("project registry requires project_id and root_path");
  }
}

bool looks_like_project(const std::filesystem::path& root) {
  return std::filesystem::is_directory(root) &&
         (std::filesystem::exists(root / "cards") ||
          std::filesystem::exists(root / "trash" / "cards") ||
          std::filesystem::exists(root / "ai_messages") ||
          std::filesystem::exists(root / "resources") ||
          std::filesystem::exists(root / "locations") ||
          std::filesystem::exists(root / ".holder" / "privacy.json") ||
          std::filesystem::exists(root / ".holder" / "project.json"));
}

std::string project_identity(const std::filesystem::path& root) {
  // Identity also prevents a moved copy of a removed project being rediscovered.
  // Leave malformed metadata to the recovery code's normal validation policy.
  for (const auto* filename : {"project.json", "privacy.json"}) {
    std::ifstream in(root / ".holder" / filename, std::ios::binary);
    if (!in) continue;
    const auto body = nlohmann::json::parse(in, nullptr, false);
    if (body.is_object() && body.contains("project_id") && body["project_id"].is_string()) {
      const auto id = body["project_id"].get<std::string>();
      if (!id.empty()) return id;
    }
  }
  return {};
}

bool removed_root(const RegistryState& state, const std::filesystem::path& root) {
  const auto path = canonical_path_string(root);
  const auto id = project_identity(root);
  // Explicit import wins only at its registered root, not at any retained copies.
  if (!id.empty()) {
    const auto active = state.projects.find(id);
    if (active != state.projects.end() && active->second == path) return false;
    if (state.removed.contains(id)) return true;
  }
  for (const auto& [removed_id, paths] : state.removed) {
    if (paths.contains(path)) return true;
  }
  return false;
}

} // namespace

ProjectRegistry::ProjectRegistry(std::filesystem::path path)
    : path_(std::move(path)) {}

std::vector<std::filesystem::path> ProjectRegistry::roots() const {
  std::lock_guard lock(registry_mutex);
  const auto state = load_registry(path_);
  std::set<std::filesystem::path> result;
  for (const auto& [id, root] : state.projects)
    result.insert(root);
  return {result.begin(), result.end()};
}

std::vector<std::filesystem::path> ProjectRegistry::discover_roots(
    const std::filesystem::path& managed_root
) const {
  std::lock_guard lock(registry_mutex);
  const auto state = load_registry(path_);
  std::set<std::filesystem::path> candidates;
  for (const auto& [id, root] : state.projects)
    candidates.insert(root);
  if (std::filesystem::is_directory(managed_root)) {
    for (const auto& entry : std::filesystem::directory_iterator(managed_root)) {
      if (looks_like_project(entry.path())) candidates.insert(canonical_path_string(entry.path()));
    }
  }
  std::vector<std::filesystem::path> result;
  for (const auto& root : candidates) {
    if (!removed_root(state, root)) result.push_back(root);
  }
  return result;
}

void ProjectRegistry::remember(const std::vector<holder::model::Project>& projects) const {
  std::lock_guard lock(registry_mutex);
  auto state = load_registry(path_);
  for (const auto& project : projects) {
    validate_project(project);
    if (state.removed.contains(project.project_id) &&
        !state.projects.contains(project.project_id)) {
      continue;
    }
    state.projects[project.project_id] = canonical_path_string(project.root_path);
  }
  save_registry(path_, state);
}

void ProjectRegistry::forget(const holder::model::Project& project) const {
  validate_project(project);
  std::lock_guard lock(registry_mutex);
  auto state = load_registry(path_);
  // Older binaries must reject this format rather than discard removal records.
  state.version = 2;
  state.removed[project.project_id].insert(canonical_path_string(project.root_path));
  if (const auto previous = state.projects.find(project.project_id);
      previous != state.projects.end()) {
    state.removed[project.project_id].insert(previous->second);
    state.projects.erase(previous);
  }
  save_registry(path_, state);
}

void ProjectRegistry::restore(const holder::model::Project& project) const {
  validate_project(project);
  std::lock_guard lock(registry_mutex);
  auto state = load_registry(path_);
  state.projects[project.project_id] = canonical_path_string(project.root_path);
  save_registry(path_, state);
}

} // namespace holder::core
