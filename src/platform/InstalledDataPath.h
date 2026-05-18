#pragma once

#include <filesystem>
#include <optional>

namespace holder::core {

std::optional<std::filesystem::path> installed_data_path(
    const std::filesystem::path& rel_path
);

} // namespace holder::core
