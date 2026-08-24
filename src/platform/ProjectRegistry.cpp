#include "platform/ProjectRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace holder::core {
namespace {

nlohmann::json load_registry(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return {{"version", 1}, {"projects", nlohmann::json::array()}};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open project registry: " + path.string());
  }
  auto body = nlohmann::json::parse(in);
  if (body.value("version", 0) != 1 || !body.contains("projects") ||
      !body.at("projects").is_array()) {
    throw std::runtime_error("unsupported project registry format: " + path.string());
  }
  return body;
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
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to restrict project registry permissions: " + path.string());
  }
#else
  (void)path;
#endif
}

} // namespace

ProjectRegistry::ProjectRegistry(std::filesystem::path path)
    : path_(std::move(path)) {}

std::vector<std::filesystem::path> ProjectRegistry::roots() const {
  const auto body = load_registry(path_);
  std::vector<std::filesystem::path> result;
  for (const auto& item : body.at("projects")) {
    if (!item.is_object() || !item.contains("root_path") ||
        !item.at("root_path").is_string() || item.at("root_path").get<std::string>().empty()) {
      throw std::runtime_error("invalid project registry entry: " + path_.string());
    }
    result.emplace_back(item.at("root_path").get<std::string>());
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void ProjectRegistry::remember(const std::vector<holder::model::Project>& projects) const {
  auto body = load_registry(path_);
  std::map<std::string, std::string> by_id;
  for (const auto& item : body.at("projects")) {
    if (!item.is_object() || !item.contains("project_id") || !item.contains("root_path") ||
        !item.at("project_id").is_string() || !item.at("root_path").is_string()) {
      throw std::runtime_error("invalid project registry entry: " + path_.string());
    }
    by_id[item.at("project_id").get<std::string>()] =
        canonical_path_string(item.at("root_path").get<std::string>());
  }
  for (const auto& project : projects) {
    if (project.project_id.empty() || project.root_path.empty()) {
      throw std::invalid_argument("project registry requires project_id and root_path");
    }
    by_id[project.project_id] = canonical_path_string(project.root_path);
  }

  body = {{"version", 1}, {"projects", nlohmann::json::array()}};
  for (const auto& [project_id, root_path] : by_id) {
    body["projects"].push_back({{"project_id", project_id}, {"root_path", root_path}});
  }

  std::filesystem::create_directories(path_.parent_path());
  auto temporary = path_;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("failed to write project registry: " + temporary.string());
    }
    out << body.dump(2) << '\n';
    out.flush();
    if (!out) {
      throw std::runtime_error("failed to flush project registry: " + temporary.string());
    }
  }
  restrict_file(temporary);

  std::error_code ec;
  std::filesystem::rename(temporary, path_, ec);
#ifdef _WIN32
  if (ec && std::filesystem::exists(path_)) {
    std::filesystem::remove(path_, ec);
    if (!ec) std::filesystem::rename(temporary, path_, ec);
  }
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace project registry: " + ec.message());
  }
  restrict_file(path_);
}

} // namespace holder::core
