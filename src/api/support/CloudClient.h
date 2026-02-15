#pragma once

#include "api/support/CloudConfig.h"

#include <optional>
#include <string>

namespace holder::api::support {

struct CloudResponseParse {
  std::optional<std::string> text;
  std::string error_code;
  std::string error_message;
};

long long estimate_tokens_from_text(const std::string& text);
std::string compact_context_tail(const std::string& context_json,
                                 long long allowed_context_tokens,
                                 bool* compacted);
CloudResponseParse parse_cloud_response(const std::string& provider_kind,
                                        int status,
                                        const std::string& response_body);
std::optional<std::string> run_cloud_model(const CloudProviderConfig& provider,
                                           const CloudModelConfig& model,
                                           const std::string& api_key,
                                           const std::string& prompt_with_context,
                                           std::string* error);

} // namespace holder::api::support
