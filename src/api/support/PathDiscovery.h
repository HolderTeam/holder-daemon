#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace holder::api::support {

std::optional<std::filesystem::path> find_openapi_path();
std::optional<std::filesystem::path> find_ai_catalog_path();
std::optional<std::filesystem::path> find_git_providers_path();
std::optional<std::filesystem::path> find_docs_root();
bool is_safe_relpath(const std::filesystem::path& path);
std::string content_type_for_extension(const std::string& ext);
std::optional<std::string> read_file(const std::filesystem::path& path);

} // namespace holder::api::support
