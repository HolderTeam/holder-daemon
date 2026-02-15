#pragma once

#include "api/support/LocalModelRouting.h"

#include <cctype>
#include <string>

namespace holder::api::support {

inline std::string normalize_provider_name(const std::string& raw) {
  const std::string key = lowercase_ascii(trim_ascii(raw));
  if (key.empty()) return {};
  for (const char ch : key) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || ch == '-' || ch == '_' || ch == '.') continue;
    return {};
  }
  return key;
}

inline std::string mask_api_key(const std::string& api_key) {
  if (api_key.empty()) return {};
  if (api_key.size() <= 8) return "****";
  return api_key.substr(0, 4) + "..." + api_key.substr(api_key.size() - 2);
}

} // namespace holder::api::support
