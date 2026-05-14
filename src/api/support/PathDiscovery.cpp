#include "api/support/PathDiscovery.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace holder::api::support {

#ifndef HOLDER_INSTALL_DATADIR
#define HOLDER_INSTALL_DATADIR ""
#endif

std::optional<std::filesystem::path> installed_data_path(const std::filesystem::path& rel_path) { // LCOV_EXCL_LINE
  // LCOV_EXCL_START: install-layout fallback is exercised by packaged builds, not repo-local tests.
  namespace fs = std::filesystem;
  const fs::path root(HOLDER_INSTALL_DATADIR);
  if (root.empty()) return std::nullopt;
  fs::path candidate = root / rel_path;
  if (fs::exists(candidate)) return candidate;
  return std::nullopt;
  // LCOV_EXCL_STOP
} // LCOV_EXCL_LINE

std::optional<std::filesystem::path> find_openapi_path() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_OPENAPI_PATH")) {
    fs::path p(env);
    if (fs::exists(p)) return p;
  }
  fs::path p1 = fs::current_path() / "openapi.yaml";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "openapi.yaml";
  if (fs::exists(p2)) return p2;
  if (auto installed = installed_data_path("openapi.yaml")) return installed;
  return std::nullopt;
}

std::optional<std::filesystem::path> find_ai_catalog_path() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_AI_CATALOG_PATH")) {
    fs::path p(env);
    if (fs::exists(p)) return p;
  }
  fs::path p1 = fs::current_path() / "config" / "ai_catalog.yaml";
  if (fs::exists(p1)) return p1;
  if (auto installed = installed_data_path("config/ai_catalog.yaml")) return installed;
  return std::nullopt;
}

std::optional<std::filesystem::path> find_git_providers_path() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_GIT_PROVIDERS_PATH")) {
    fs::path p(env);
    if (fs::exists(p)) return p;
  }
  fs::path p1 = fs::current_path() / "config" / "git_providers.yaml";
  if (fs::exists(p1)) return p1;
  if (auto installed = installed_data_path("config/git_providers.yaml")) return installed;
  return std::nullopt;
}

std::optional<std::filesystem::path> find_docs_root() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_DOCS_ROOT")) {
    fs::path p(env);
    if (fs::exists(p) && fs::is_directory(p)) return p;
  }
  fs::path p1 = fs::current_path() / "assets" / "swagger-ui";
  if (fs::exists(p1) && fs::is_directory(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "assets" / "swagger-ui";
  if (fs::exists(p2) && fs::is_directory(p2)) return p2;
  if (auto installed = installed_data_path("assets/swagger-ui")) {
    if (fs::is_directory(installed.value())) return installed; // LCOV_EXCL_LINE
  }
  return std::nullopt;
}

bool is_safe_relpath(const std::filesystem::path& path) {
  if (path.is_absolute()) return false;
  for (const auto& part : path) {
    if (part == "..") return false;
  }
  return true;
}

std::string content_type_for_extension(const std::string& ext) {
  std::string lower;
  lower.reserve(ext.size());
  for (const char ch : ext) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (lower == ".html") return "text/html; charset=utf-8";
  if (lower == ".css") return "text/css; charset=utf-8";
  if (lower == ".js") return "application/javascript";
  if (lower == ".json") return "application/json";
  if (lower == ".yaml" || lower == ".yml") return "application/yaml";
  if (lower == ".svg") return "image/svg+xml";
  if (lower == ".png") return "image/png";
  if (lower == ".ico") return "image/x-icon";
  if (lower == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

} // namespace holder::api::support
