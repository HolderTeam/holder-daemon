#pragma once

#if defined(_WIN32)

#include <cerrno>
#include <cstdlib>

inline int setenv(const char* name, const char* value, int overwrite) {
  if (name == nullptr || name[0] == '\0' || value == nullptr) {
    errno = EINVAL;
    return -1;
  }
  if (!overwrite && std::getenv(name) != nullptr) {
    return 0;
  }
  return _putenv_s(name, value);
}

inline int unsetenv(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  return _putenv_s(name, "");
}

#endif
