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

inline std::string translate_single_quoted_args_for_windows(const std::string& cmd) {
  std::string out;
  out.reserve(cmd.size());
  bool in_single_quote = false;
  for (const char ch : cmd) {
    if (ch == '\'') {
      in_single_quote = !in_single_quote;
      out.push_back('"');
      continue;
    }
    if (in_single_quote && ch == '"') {
      out += "\\\"";
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

inline std::string normalize_system_command(std::string cmd) {
#ifdef _WIN32
  replace_all(cmd, "/dev/null", "NUL");
  cmd = translate_single_quoted_args_for_windows(cmd);
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
