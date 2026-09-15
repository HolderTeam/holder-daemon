#pragma once

#include "cli/commands/Common.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace holder::cli {

struct DownloadMetadata {
  std::string content_type;
  std::string content_disposition;
  std::string filename;
  std::uint64_t byte_size = 0;
};

// Only successful response bytes reach the sink. JSON API failures retain their typed errors.
// Timeout applies to each socket operation; transfer buffers remain bounded regardless of size.
DownloadMetadata http_download(
    const DaemonConnection& connection, const std::string& target,
    std::chrono::seconds timeout,
    const std::function<void(const char*, std::size_t)>& sink
);

// Stages beside the destination; a failed or interrupted download leaves it untouched.
DownloadMetadata download_to_file(
    const DaemonConnection& connection, const std::string& target,
    const std::filesystem::path& output, std::chrono::seconds timeout
);

} // namespace holder::cli
