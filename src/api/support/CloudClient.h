#pragma once

#include "api/support/CloudConfig.h"

#include <optional>
#include <string>

namespace holder::api::support {

long long estimate_tokens_from_text(const std::string& text);
std::string compact_context_tail(const std::string& context_json,
                                 long long allowed_context_tokens,
                                 bool* compacted);
std::optional<std::string> run_cloud_model(const CloudProviderConfig& provider,
                                           const CloudModelConfig& model,
                                           const std::string& api_key,
                                           const std::string& prompt_with_context,
                                           std::string* error);

} // namespace holder::api::support
