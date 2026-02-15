#pragma once

#include "store/Db.h"

#include <optional>
#include <string>

namespace holder::api::support {

struct CloudQuotaWindowUsage {
  long long requests = 0;
  long long tokens = 0;
};

struct CloudModelCooldownState {
  std::string provider;
  std::string model_id;
  long long failure_count = 0;
  long long cooldown_until = 0;
  std::string last_error;
  long long updated_at = 0;
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

std::optional<CloudModelCooldownState> load_cloud_model_cooldown(holder::store::Db& db,
                                                                  const std::string& provider,
                                                                  const std::string& model_id);

CloudModelCooldownState record_cloud_model_failure(holder::store::Db& db,
                                                   const std::string& provider,
                                                   const std::string& model_id,
                                                   const std::string& error,
                                                   long long now_epoch_seconds,
                                                   long long cooldown_base_seconds = 30,
                                                   long long cooldown_cap_seconds = 900);

void clear_cloud_model_cooldown(holder::store::Db& db,
                                const std::string& provider,
                                const std::string& model_id,
                                long long now_epoch_seconds);

} // namespace holder::api::support
