#pragma once

#include <string>

namespace holder::api::support {

inline std::string query_param_value(const std::string& query_string, const std::string& key) {
  const std::string needle = key + "=";
  const auto pos = query_string.find(needle);
  if (pos == std::string::npos) return {};
  const auto start = pos + needle.size();
  const auto end = query_string.find('&', start);
  return query_string.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace holder::api::support
