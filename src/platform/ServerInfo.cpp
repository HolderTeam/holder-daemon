#include "platform/ServerInfo.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace holder::core {

namespace {

void write_owner_only_file(const std::filesystem::path& path, const std::string& body) {
#ifdef _WIN32
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to open server info temp file: " + path.string());
  }
  out << body;
  out.close();
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    throw std::runtime_error("Failed to open server info temp file: " + path.string());
  }

  const char* cursor = body.data();
  std::size_t remaining = body.size();
  while (remaining > 0) {
    const ssize_t written = ::write(fd, cursor, remaining);
    if (written <= 0) {
      ::close(fd); // LCOV_EXCL_LINE: requires syscall fault injection.
      std::filesystem::remove(path); // LCOV_EXCL_LINE
      throw std::runtime_error("Failed to write server info temp file: " + path.string()); // LCOV_EXCL_LINE
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }

  if (::close(fd) != 0) {
    std::filesystem::remove(path); // LCOV_EXCL_LINE: requires syscall fault injection.
    throw std::runtime_error("Failed to close server info temp file: " + path.string()); // LCOV_EXCL_LINE
  }
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

} // namespace

int current_pid() {
#ifdef _WIN32
  return static_cast<int>(::GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
}

std::string generate_auth_token(std::size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);

  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);

  for (std::size_t i = 0; i < bytes; ++i) {
    const int v = dist(rd);
    out.push_back(kHex[(v >> 4) & 0xF]);
    out.push_back(kHex[v & 0xF]);
  }

  return out;
}

void write_server_info(const std::filesystem::path& path, const ServerInfo& info) {
  nlohmann::json j;
  j["pid"] = info.pid;
  j["bind"] = info.bind;
  j["port"] = info.port;
  j["started_at"] = info.started_at;
  j["api_version"] = info.api_version;
  j["server_version"] = info.server_version;
  j["auth_token"] = info.auth_token;

  const auto tmp_path = path.string() + ".tmp";
  write_owner_only_file(tmp_path, j.dump(2) + "\n");

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
      throw std::runtime_error("Failed to write server info: " + path.string() +
                               " (" + ec.message() + ")");
    }
  }
}

} // namespace holder::core
