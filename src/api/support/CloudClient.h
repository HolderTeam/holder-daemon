#pragma once

#include "api/support/CloudConfig.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace holder::api::support {

struct CloudResponseParse {
  std::optional<std::string> text;
  std::string error_code;
  std::string error_message;
};

using CloudModelRunnerOverride = std::function<std::optional<std::string>(
    const CloudProviderConfig& provider,
    const CloudModelConfig& model,
    const std::string& api_key,
    const std::string& prompt_with_context,
    std::string* error)>;
using CloudTransportPostOverride = std::function<bool(
    const std::string& base_url,
    const std::string& target_path_and_query,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& request_body,
    int* out_status,
    std::string* out_body,
    std::string* error)>;

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
void set_run_cloud_model_override_for_tests(CloudModelRunnerOverride fn);
void clear_run_cloud_model_override_for_tests();
void set_cloud_transport_post_override_for_tests(CloudTransportPostOverride fn);
void clear_cloud_transport_post_override_for_tests();

} // namespace holder::api::support
