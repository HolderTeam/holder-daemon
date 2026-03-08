#pragma once

#include <filesystem>
#include <string>

namespace holder::core {

struct ServerInfo {
  int pid = 0;
  std::string bind;
  int port = 0;
  long long started_at = 0;
  std::string api_version;
  std::string server_version;
  std::string auth_token;
};

int current_pid();

// Generate a hex token with 2*bytes characters.
std::string generate_auth_token(std::size_t bytes = 16);

// Write server info to holder.json atomically when possible.
void write_server_info(const std::filesystem::path& path, const ServerInfo& info);

} // namespace holder::core
