#pragma once

#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace holder::test {

inline void replace_all(std::string& value, const std::string& from, const std::string& to) {
  std::string::size_type pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
}

inline std::string normalize_system_command(std::string cmd) {
#ifdef _WIN32
  replace_all(cmd, "/dev/null", "NUL");
  if (!cmd.empty() && cmd.front() == '"') {
    cmd = "\"" + cmd + "\"";
  }
#endif
  return cmd;
}

inline int run_system_command(const std::string& cmd) {
  const auto normalized = normalize_system_command(cmd);
  const int rc = std::system(normalized.c_str());
#ifdef _WIN32
  return rc;
#else
  if (rc == -1) return rc;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return -1;
#endif
}

} // namespace holder::test
