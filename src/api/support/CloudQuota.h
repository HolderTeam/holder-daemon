#pragma once

#include "store/Db.h"

#include <string>

namespace holder::api::support {

struct CloudQuotaWindowUsage {
  long long requests = 0;
  long long tokens = 0;
};

CloudQuotaWindowUsage load_cloud_window_usage(holder::store::Db& db,
                                              const std::string& provider,
                                              const std::string& model_id,
                                              long long since_epoch_seconds);

void record_cloud_usage_event(holder::store::Db& db,
                              const std::string& provider,
                              const std::string& model_id,
                              long long prompt_tokens,
                              long long response_tokens,
                              long long created_at,
                              const std::string& event_id_seed);

} // namespace holder::api::support
