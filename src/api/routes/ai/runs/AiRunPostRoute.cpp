#include "api/routes/ai/runs/AiRunPostRoute.h"

#include "api/support/CloudClient.h"
#include "api/support/CloudConfig.h"
#include "api/support/CloudQuota.h"
#include "api/support/HttpResponses.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/ProviderUtils.h"
#include "api/support/RunEventStore.h"
#include "api/support/ThreadCompaction.h"
#include "api/support/Time.h"
#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "ai/AiRunRepo.h"
#include "ai/AiThreadRepo.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace holder::api::routes::ai::runs {
namespace {

namespace http = boost::beast::http;

std::string truncate_bytes(const std::string& text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  return text.substr(0, max_bytes);
}

std::string trim_copy(const std::string& input) {
  std::size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  std::size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
}

std::string collapse_whitespace(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  bool in_space = false;
  for (const char ch : input) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!out.empty() && !in_space) {
        out.push_back(' ');
      }
      in_space = true;
      continue;
    }
    out.push_back(ch);
    in_space = false;
  }
  return trim_copy(out);
}

std::string strip_wrapping_quotes(std::string value) {
  value = trim_copy(value);
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    const bool matching_double = (first == '"' && last == '"');
    const bool matching_single = (first == '\'' && last == '\'');
    if (matching_double || matching_single) {
      value = trim_copy(value.substr(1, value.size() - 2));
    }
  }
  return value;
}

bool should_refresh_thread_title(const std::string& current_title, const std::string& prompt) {
  const auto normalized_title = collapse_whitespace(current_title);
  if (normalized_title.empty()) return true;
  if (normalized_title.rfind("Thread ", 0) == 0 || normalized_title.rfind("AI Thread ", 0) == 0) {
    return true;
  }
  const auto prompt_seed = collapse_whitespace(truncate_bytes(prompt, 80));
  return !prompt_seed.empty() && normalized_title == prompt_seed;
}

bool is_installed_model(const holder::llm::RunnerStatus& status, const std::string& name) {
  return std::find_if(status.models.begin(),
                      status.models.end(),
                      [&](const holder::llm::LocalModel& model) { return model.name == name; }) !=
         status.models.end();
}

std::optional<holder::model::AiLocalModelConfig> load_local_model_config(holder::platform::Db& db) {
  try {
    holder::ai::AiLocalModelConfigRepo repo(db);
    return repo.get();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> pick_local_title_model(holder::platform::Db& db,
                                                  holder::llm::LocalModelRunner* runner) {
  if (runner == nullptr) return std::nullopt;
  const auto status = runner->status();
  if (!status.available || status.models.empty()) return std::nullopt;

  const auto cfg = load_local_model_config(db);
  if (cfg.has_value() && cfg->fast_model.has_value() &&
      is_installed_model(status, cfg->fast_model.value())) {
    return cfg->fast_model.value();
  }

  const holder::llm::LocalModel* best = nullptr;
  for (const auto& model : status.models) {
    if (best == nullptr) {
      best = &model;
      continue;
    }
    if (model.size > 0 && (best->size == 0 || model.size < best->size)) {
      best = &model;
    }
  }
  if (best == nullptr || best->name.empty()) return std::nullopt;
  return best->name;
}

std::optional<std::string> generate_thread_title(holder::llm::LocalModelRunner* runner,
                                                 holder::platform::Db& db,
                                                 const std::string& prompt,
                                                 const std::string& assistant_text) {
  const auto model = pick_local_title_model(db, runner);
  if (!model.has_value()) return std::nullopt;

  std::ostringstream prompt_ss;
  prompt_ss << "Write a short human-readable thread title for this conversation.\n";
  prompt_ss << "Constraints:\n";
  prompt_ss << "- Output only the title.\n";
  prompt_ss << "- Keep it under 7 words.\n";
  prompt_ss << "- No quotes, no markdown, no trailing punctuation.\n";
  prompt_ss << "User:\n" << truncate_bytes(prompt, 600) << "\n\n";
  if (!assistant_text.empty()) {
    prompt_ss << "Assistant:\n" << truncate_bytes(assistant_text, 600) << "\n\n";
  }
  prompt_ss << "Title:";

  std::string generated;
  std::string error;
  const bool ok = runner->stream_generate(
      model.value(),
      prompt_ss.str(),
      "{}",
      [&](const std::string& chunk) { generated += chunk; },
      &error);
  if (!ok) return std::nullopt;

  auto title = strip_wrapping_quotes(collapse_whitespace(generated));
  if (!title.empty() && title.back() == '.') {
    title.pop_back();
    title = trim_copy(title);
  }
  if (title.empty()) return std::nullopt;
  if (title.find(':') != std::string::npos) return std::nullopt;
  if (title.find('\n') != std::string::npos) return std::nullopt;
  if (title.rfind("User", 0) == 0 || title.rfind("Assistant", 0) == 0 ||
      title.rfind("Title", 0) == 0) {
    return std::nullopt;
  }
  constexpr std::size_t max_title_bytes = 60;
  if (title.size() > max_title_bytes) {
    title = trim_copy(title.substr(0, max_title_bytes));
  }
  return title.empty() ? std::nullopt : std::optional<std::string>(title);
}

void maybe_update_thread_title(holder::platform::Db& db,
                               holder::llm::LocalModelRunner* runner,
                               const std::optional<std::string>& thread_id,
                               const std::string& prompt,
                               const std::string& assistant_text,
                               long long updated_at) {
  if (!thread_id.has_value() || assistant_text.empty()) return;

  holder::ai::AiThreadRepo thread_repo(db);
  const auto thread = thread_repo.get(thread_id.value());
  if (!thread.has_value()) return;
  if (!should_refresh_thread_title(thread->title, prompt)) return;

  const auto next_title = generate_thread_title(runner, db, prompt, assistant_text);
  if (!next_title.has_value() || next_title.value() == thread->title) return;
  thread_repo.update_title(thread_id.value(), next_title.value(), updated_at);
}

struct AiRunPostInput {
  nlohmann::json body;
  std::string prompt;
  std::string mode = "auto";
  std::optional<std::string> project_id;
  std::optional<std::string> thread_id;
  std::string context_json;
  std::optional<std::string> context_card_id;
};

std::optional<AiRunPostInput> parse_ai_run_post_input(
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res) {
  AiRunPostInput input;
  try {
    input.body = nlohmann::json::parse(req.body());
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    return std::nullopt;
  }

  if (!input.body.contains("prompt")) {
    res = support::error_response(http::status::bad_request, "bad_request", "Missing prompt.");
    return std::nullopt;
  }

  input.prompt = input.body.at("prompt").get<std::string>();
  if (input.body.contains("mode")) {
    input.mode = input.body.at("mode").get<std::string>();
  }
  if (input.body.contains("project_id") && !input.body.at("project_id").is_null()) {
    input.project_id = input.body.at("project_id").get<std::string>();
  }
  if (input.body.contains("thread_id") && !input.body.at("thread_id").is_null()) {
    input.thread_id = input.body.at("thread_id").get<std::string>();
  }
  if (input.body.contains("context") && !input.body.at("context").is_null()) {
    input.context_json = input.body.at("context").dump();
    if (input.body.at("context").is_object() && input.body.at("context").contains("card_id") &&
        !input.body.at("context").at("card_id").is_null()) {
      input.context_card_id = input.body.at("context").at("card_id").get<std::string>();
    }
  }

  return input;
}

void ensure_ai_run_thread(AiRunPostInput& input,
                          holder::platform::Db& db,
                          const std::function<std::string()>& uuid_v4) {
  if (input.thread_id.has_value() || !input.project_id.has_value()) {
    return;
  }

  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread thread;
  thread.thread_id = uuid_v4();
  thread.project_id = input.project_id.value();
  if (input.context_card_id.has_value()) {
    thread.card_id = input.context_card_id.value();
  }
  std::string title = input.prompt;
  if (title.size() > 80) {
    title = title.substr(0, 80);
  }
  thread.title = title;
  thread.created_at = support::now_epoch_seconds();
  thread.updated_at = thread.created_at;
  thread_repo.create(thread);
  input.thread_id = thread.thread_id;
}

const support::CloudModelConfig* choose_compact_summary_model(
    const support::CloudProviderConfig& provider) {
  for (const auto& model : provider.models) {
    if (model.role == "compact") return &model;
  }
  return nullptr;
}

std::pair<long long, long long> effective_cooldown_policy(
    const support::CloudProvidersConfig& cfg,
    const support::CloudProviderConfig& provider,
    const support::CloudModelConfig& model) {
  long long base = cfg.cooldown.base_seconds;
  long long cap = cfg.cooldown.cap_seconds;
  if (provider.cooldown_base_seconds > 0) base = provider.cooldown_base_seconds;
  if (provider.cooldown_cap_seconds > 0) cap = provider.cooldown_cap_seconds;
  if (model.cooldown_base_seconds > 0) base = model.cooldown_base_seconds;
  if (model.cooldown_cap_seconds > 0) cap = model.cooldown_cap_seconds;
  return {base, cap};
}

RouteDispatchResult execute_cloud_post_path(
    const nlohmann::json& body,
    const std::string& prompt,
    std::string& mode,
    const std::optional<std::string>& project_id,
    const std::optional<std::string>& thread_id,
    const std::string& context_json,
    holder::privacy::SecretStore* secret_store,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    boost::asio::ip::tcp::socket& socket,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts) {
  RouteDispatchResult out{};
  out.handled = true;

  const auto cloud_cfg = support::load_cloudproviders_config();
  if (!cloud_cfg.has_value()) {
    res = support::error_response(http::status::service_unavailable,
                                  "runner_unavailable",
                                  "No local runner and ai_catalog.yaml models runtime/catalog not found.");
    return out;
  }

  std::string requested_provider;
  if (body.contains("provider") && !body.at("provider").is_null()) {
    requested_provider = support::normalize_provider_name(body.at("provider").get<std::string>());
  }
  std::string requested_model;
  if (body.contains("model") && !body.at("model").is_null()) {
    requested_model = body.at("model").get<std::string>();
    mode = "model";
  }

  holder::ai::AiProviderCredentialRepo credential_repo(db);
  const auto credentials = credential_repo.list();
  std::unordered_map<std::string, holder::model::AiProviderCredential> creds_by_key;
  for (const auto& credential : credentials) {
    creds_by_key[credential.provider] = credential;
  }
  holder::ai::AiProviderSettingRepo setting_repo(db);
  std::unordered_map<std::string, bool> enabled_by_provider;
  for (const auto& setting : setting_repo.list()) {
    enabled_by_provider[setting.provider] = setting.enabled;
  }

  const support::CloudProviderConfig* selected_provider = nullptr;
  bool selection_failed = false;
  static constexpr const char* kSecretService = "holder.ai_provider_credentials";

  auto try_select = [&](const support::CloudProviderConfig& provider) -> bool {
    const auto enabled_it = enabled_by_provider.find(provider.id);
    const bool effective_enabled =
        (enabled_it != enabled_by_provider.end()) ? enabled_it->second : provider.enabled;
    if (!effective_enabled) return false;
    const auto it = creds_by_key.find(provider.credential_provider_key);
    if (it == creds_by_key.end()) return false;
    selected_provider = &provider;
    return true;
  };

  if (!requested_provider.empty()) {
    const auto* provider = support::find_cloud_provider(cloud_cfg.value(), requested_provider);
    if (provider && try_select(*provider)) {
      // selected by request
    } else {
      res = support::error_response(http::status::service_unavailable,
                                    "cloud_not_configured",
                                    "Requested cloud provider is not enabled/configured.");
      selection_failed = true;
    }
  }

  if (!selected_provider && !selection_failed) {
    for (const auto* provider_cfg : support::ordered_cloud_providers(cloud_cfg.value())) {
      if (provider_cfg && try_select(*provider_cfg)) {
        break;
      }
    }
  }

  if (!selected_provider && !selection_failed) {
    res = support::error_response(http::status::service_unavailable,
                                  "cloud_not_configured",
                                  "No enabled cloud provider with stored API key.");
  }

  if (selected_provider) {
    if (secret_store == nullptr) {
      res = support::error_response(http::status::service_unavailable,
                                    "cloud_not_configured",
                                    "Cloud provider credential secret store is unavailable.");
      return out;
    }
    const auto selected_secret =
        secret_store->get(kSecretService, selected_provider->credential_provider_key);
    if (!selected_secret.has_value()) {
      res = support::error_response(http::status::service_unavailable,
                                    "cloud_not_configured",
                                    "Cloud provider credential secret is missing.");
      return out;
    }
    const std::string provider_api_key = selected_secret->secret;
    const auto candidate_models = support::cloud_model_candidates(*selected_provider, requested_model);
    if (candidate_models.empty()) {
      res = support::error_response(http::status::service_unavailable,
                                    "cloud_not_configured",
                                    "No cloud model configured for selected provider.");
    } else {
      holder::ai::AiRunRepo run_repo(db);
      holder::model::AiRun run;
      run.run_id = uuid_v4();
      run.project_id = project_id;
      run.thread_id = thread_id;
      run.mode = (mode == "model") ? "model" : "auto";
      run.prompt = prompt;
      if (!context_json.empty()) {
        run.context_json = context_json;
      }
      run.status = "started";
      run.created_at = support::now_epoch_seconds();
      run.updated_at = run.created_at;
      run_repo.create(run);
      support::append_run_event(run.run_id,
                                "run_started",
                                {{"status", "started"}, {"created_at", run.created_at}},
                                false);

      out.streamed = true;
      http::response<http::empty_body> sse{http::status::ok, 11};
      sse.set(http::field::content_type, "text/event-stream");
      sse.set(http::field::cache_control, "no-cache");
      sse.set(http::field::connection, "keep-alive");
      sse.keep_alive(true);
      http::serializer<false, http::empty_body> sr{sse};
      boost::system::error_code write_ec;
      http::write_header(socket, sr, write_ec);
      if (write_ec) {
        return out;
      }

      auto send_event = [&](const std::string& name, const nlohmann::json& data) -> bool {
        const bool is_terminal = (name == "done" || name == "failed");
        support::append_run_event(run.run_id, name, data, is_terminal);
        std::string payload = "event: " + name + "\n";
        nlohmann::json wire = data;
        wire["run_id"] = run.run_id;
        payload += "data: " + wire.dump() + "\n\n";
        boost::system::error_code send_ec;
        boost::asio::write(socket, boost::asio::buffer(payload), send_ec);
        return !send_ec;
      };

      nlohmann::json policy_trace;
      policy_trace["path"] = "cloud";
      policy_trace["provider"] = selected_provider->id;
      policy_trace["selection"] = {
          {"requested_provider", requested_provider.empty() ? nlohmann::json(nullptr)
                                                            : nlohmann::json(requested_provider)},
          {"requested_model",
           requested_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(requested_model)},
      };
      policy_trace["attempts"] = nlohmann::json::array();

      send_event("progress", {{"message", "Using cloud provider"}, {"provider", selected_provider->id}});

      if (thread_id.has_value()) {
        holder::ai::AiMessageRepo msg_repo(db, fts);
        holder::model::AiMessage user_msg;
        user_msg.message_id = uuid_v4();
        user_msg.thread_id = thread_id.value();
        user_msg.role = "user";
        user_msg.source = "cloud";
        user_msg.provider = selected_provider->id;
        if (!requested_model.empty()) {
          user_msg.model = requested_model;
        }
        user_msg.content = prompt;
        user_msg.created_at = support::now_epoch_seconds();
        msg_repo.append(user_msg);
      }

      std::optional<std::string> chosen_model_id;
      std::optional<std::string> output;
      long long output_prompt_tokens = 0;
      std::string final_error = "All cloud model attempts failed.";
      std::optional<support::ThreadCompactionState> compaction_state;
      bool summary_refreshed = false;
      const long long refresh_threshold_tokens =
          std::max(1LL, cloud_cfg->summary_refresh.trigger_context_tokens);
      const long long summary_source_tokens =
          std::max(256LL, cloud_cfg->summary_refresh.source_context_tokens);
      const long long summary_response_tokens_budget =
          std::max(64LL, cloud_cfg->summary_refresh.response_tokens_budget);
      const long long max_summary_chars =
          std::max(256LL, cloud_cfg->summary_refresh.max_summary_chars);
      const long long min_refresh_interval_seconds =
          std::max(0LL, cloud_cfg->summary_refresh.min_interval_seconds);
      const long long min_refresh_delta_tokens =
          std::max(0LL, cloud_cfg->summary_refresh.min_delta_tokens);
      const long long force_refresh_tokens =
          std::max(refresh_threshold_tokens, cloud_cfg->summary_refresh.force_refresh_tokens);
      if (thread_id.has_value()) {
        compaction_state = support::load_thread_compaction_state(db, thread_id.value());
      }
      if (thread_id.has_value() && !context_json.empty()) {
        nlohmann::json compaction_trace;
        compaction_trace["strategy"] = "summary_plus_pinned_plus_tail";
        compaction_trace["summary_refresh"] = {{"status", "skipped"}};

        const long long context_tokens = support::estimate_tokens_from_text(context_json);
        const auto* compact_model = choose_compact_summary_model(*selected_provider);
        if (!compact_model) {
          compaction_trace["summary_refresh"]["reason"] = "no_compact_model";
        } else if (context_tokens < refresh_threshold_tokens) {
          compaction_trace["summary_refresh"]["reason"] = "below_threshold";
          compaction_trace["summary_refresh"]["context_tokens"] = context_tokens;
        } else {
          const long long now = support::now_epoch_seconds();
          const long long summary_tokens = (compaction_state.has_value() &&
                                            compaction_state->rolling_summary.has_value())
                                               ? support::estimate_tokens_from_text(
                                                     compaction_state->rolling_summary.value())
                                               : 0;
          const long long token_delta = std::max(0LL, context_tokens - summary_tokens);
          const long long last_refresh_at =
              compaction_state.has_value() ? compaction_state->updated_at : 0;
          const long long since_last_refresh = (last_refresh_at > 0) ? (now - last_refresh_at) : (1LL << 30);
          const bool forced_refresh = context_tokens >= force_refresh_tokens;

          if (!forced_refresh && last_refresh_at > 0 &&
              since_last_refresh < min_refresh_interval_seconds) {
            compaction_trace["summary_refresh"]["reason"] = "min_interval_not_elapsed";
            compaction_trace["summary_refresh"]["seconds_since_last"] = since_last_refresh;
            compaction_trace["summary_refresh"]["min_interval_seconds"] = min_refresh_interval_seconds;
          } else if (!forced_refresh && token_delta < min_refresh_delta_tokens) {
            compaction_trace["summary_refresh"]["reason"] = "min_delta_not_met";
            compaction_trace["summary_refresh"]["token_delta"] = token_delta;
            compaction_trace["summary_refresh"]["min_delta_tokens"] = min_refresh_delta_tokens;
          } else {
            const auto cooldown_state =
                support::load_cloud_model_cooldown(db, selected_provider->id, compact_model->id);
            if (cooldown_state.has_value() && cooldown_state->cooldown_until > now) {
              compaction_trace["summary_refresh"]["reason"] = "cooldown_active";
              compaction_trace["summary_refresh"]["cooldown_until"] = cooldown_state->cooldown_until;
            } else {
              const long long minute_start = now - 60;
              const long long day_start = now - 86400;
              const auto minute_usage = support::load_cloud_window_usage(
                  db, selected_provider->id, compact_model->id, minute_start);
              const auto day_usage = support::load_cloud_window_usage(
                  db, selected_provider->id, compact_model->id, day_start);

              bool src_compacted = false;
              const std::string summary_source =
                  support::compact_context_tail(context_json, summary_source_tokens, &src_compacted);
              const std::optional<std::string> current_summary =
                  (compaction_state.has_value() ? compaction_state->rolling_summary : std::nullopt);
              const std::string summarize_prompt =
                  support::build_structured_summary_refresh_prompt(current_summary, summary_source);
              const long long summary_prompt_tokens = support::estimate_tokens_from_text(summarize_prompt);
              const long long summary_projected_tokens =
                  summary_prompt_tokens + summary_response_tokens_budget;

              bool quota_reject = false;
              if (compact_model->rpm > 0 && minute_usage.requests + 1 > compact_model->rpm) {
                quota_reject = true;
                compaction_trace["summary_refresh"]["reason"] = "rpm_exceeded";
              } else if (compact_model->rpd > 0 && day_usage.requests + 1 > compact_model->rpd) {
                quota_reject = true; // LCOV_EXCL_LINE
                compaction_trace["summary_refresh"]["reason"] = "rpd_exceeded"; // LCOV_EXCL_LINE
              } else if (compact_model->tpm > 0 &&
                         minute_usage.tokens + summary_projected_tokens > compact_model->tpm) { // LCOV_EXCL_LINE
                quota_reject = true; // LCOV_EXCL_LINE
                compaction_trace["summary_refresh"]["reason"] = "tpm_exceeded"; // LCOV_EXCL_LINE
              }

              if (!quota_reject) {
                send_event("progress",
                           {{"message", "Refreshing rolling summary"},
                            {"provider", selected_provider->id},
                            {"model", compact_model->id}});
                std::string summary_error;
                const auto summary_output = support::run_cloud_model(*selected_provider,
                                                                     *compact_model,
                                                                     provider_api_key,
                                                                     summarize_prompt,
                                                                     &summary_error);
                if (summary_output.has_value()) {
                  support::clear_cloud_model_cooldown(db, selected_provider->id, compact_model->id, now);
                  const long long summary_response_tokens =
                      support::estimate_tokens_from_text(summary_output.value());
                  support::record_cloud_usage_event(db,
                                                    selected_provider->id,
                                                    compact_model->id,
                                                    summary_prompt_tokens,
                                                    summary_response_tokens,
                                                    now,
                                                    run.run_id + "-summary");
                  const auto normalized = support::normalize_and_validate_rolling_summary(
                      summary_output.value(), current_summary, max_summary_chars);
                  if (normalized.accepted) {
                    support::ThreadCompactionState next_state;
                    if (compaction_state.has_value()) {
                      next_state = compaction_state.value();
                    }
                    next_state.thread_id = thread_id.value();
                    next_state.rolling_summary = normalized.summary;
                    next_state.updated_at = now;
                    support::upsert_thread_compaction_state(db, next_state);
                    compaction_state = next_state;
                    summary_refreshed = true;
                    compaction_trace["summary_refresh"] = {
                        {"status", "completed"},
                        {"model", compact_model->id},
                        {"forced", forced_refresh},
                        {"source_compacted", src_compacted},
                        {"prompt_tokens", summary_prompt_tokens},
                        {"response_tokens", summary_response_tokens},
                        {"quality_items", normalized.extracted_items},
                        {"quality_fallback_sections", normalized.used_fallback_sections},
                    };
                  } else { // LCOV_EXCL_LINE
                    compaction_trace["summary_refresh"] = {
                        {"status", "skipped"},
                        {"reason", "quality_guard_failed"},
                        {"quality_reason", normalized.reason},
                        {"model", compact_model->id},
                        {"forced", forced_refresh},
                        {"source_compacted", src_compacted},
                        {"prompt_tokens", summary_prompt_tokens},
                        {"response_tokens", summary_response_tokens},
                    };
                  }
                } else {
                  const std::string fail_error =
                      summary_error.empty() ? "summary refresh failed" : summary_error;
                  const auto [cooldown_base, cooldown_cap] =
                      effective_cooldown_policy(*cloud_cfg, *selected_provider, *compact_model);
                  const auto cooldown = support::record_cloud_model_failure(db,
                                                                            selected_provider->id,
                                                                            compact_model->id,
                                                                            fail_error,
                                                                            now,
                                                                            cooldown_base,
                                                                            cooldown_cap);
                  compaction_trace["summary_refresh"] = {
                      {"status", "failed"},
                      {"model", compact_model->id},
                      {"forced", forced_refresh},
                      {"error", fail_error},
                      {"cooldown_until", cooldown.cooldown_until},
                  };
                }
              }
            }
          }
        }
        policy_trace["compaction"] = compaction_trace;
      }

      for (const auto* candidate : candidate_models) {
        nlohmann::json attempt;
        attempt["model"] = candidate->id;
        const long long now = support::now_epoch_seconds();

        const auto cooldown_state =
            support::load_cloud_model_cooldown(db, selected_provider->id, candidate->id);
        if (cooldown_state.has_value() && cooldown_state->cooldown_until > now) {
          attempt["decision"] = "rejected";
          attempt["reason"] = "cooldown_active";
          attempt["cooldown"] = {
              {"failure_count", cooldown_state->failure_count},
              {"cooldown_until", cooldown_state->cooldown_until},
              {"remaining_seconds", cooldown_state->cooldown_until - now}, // LCOV_EXCL_LINE
              {"last_error", cooldown_state->last_error.empty()
                                 ? nlohmann::json(nullptr)
                                 : nlohmann::json(cooldown_state->last_error)},
          };
          policy_trace["attempts"].push_back(attempt);
          continue;
        }

        const long long minute_start = now - 60;
        const long long day_start = now - 86400;
        const auto minute_usage =
            support::load_cloud_window_usage(db, selected_provider->id, candidate->id, minute_start);
        const auto day_usage =
            support::load_cloud_window_usage(db, selected_provider->id, candidate->id, day_start);

        const long long prompt_tokens = support::estimate_tokens_from_text(prompt);
        const long long model_input_budget =
            (candidate->tpm > 0) ? std::max(256LL, (candidate->tpm * 7) / 10) : 6000LL;
        const long long context_budget = std::max(0LL, model_input_budget - prompt_tokens - 64LL);
        bool compacted = false;
        bool used_summary = false;
        int pinned_fact_count = 0;
        std::string compacted_context = support::build_compacted_context(
            context_json, context_budget, compaction_state, &compacted, &used_summary, &pinned_fact_count);
        std::string prompt_full = prompt;
        if (!compacted_context.empty()) {
          prompt_full += "\n\nContext:\n";
          prompt_full += compacted_context;
        }
        const long long prompt_full_tokens = support::estimate_tokens_from_text(prompt_full);
        const long long reserved_response_tokens = 512;
        const long long projected_tokens = prompt_full_tokens + reserved_response_tokens;

        attempt["quota"] = {
            {"rpm_limit", candidate->rpm},
            {"tpm_limit", candidate->tpm},
            {"rpd_limit", candidate->rpd},
            {"minute_requests", minute_usage.requests},
            {"minute_tokens", minute_usage.tokens},
            {"day_requests", day_usage.requests},
            {"projected_tokens", projected_tokens},
        };
        attempt["context_compacted"] = compacted;
        attempt["compaction"] = {
            {"used_summary", used_summary},
            {"pinned_fact_count", pinned_fact_count},
        };
        attempt["input_tokens"] = prompt_full_tokens;

        if (candidate->rpm > 0 && minute_usage.requests + 1 > candidate->rpm) {
          attempt["decision"] = "rejected";
          attempt["reason"] = "rpm_exceeded";
          policy_trace["attempts"].push_back(attempt);
          continue;
        }
        if (candidate->rpd > 0 && day_usage.requests + 1 > candidate->rpd) {
          attempt["decision"] = "rejected";
          attempt["reason"] = "rpd_exceeded";
          policy_trace["attempts"].push_back(attempt);
          continue;
        }
        if (candidate->tpm > 0 && minute_usage.tokens + projected_tokens > candidate->tpm) {
          attempt["decision"] = "rejected";
          attempt["reason"] = "tpm_exceeded";
          policy_trace["attempts"].push_back(attempt);
          continue;
        }

        attempt["decision"] = "selected";
        policy_trace["attempts"].push_back(attempt);
        send_event("progress",
                   {{"message", "Trying cloud model"},
                    {"provider", selected_provider->id},
                    {"model", candidate->id},
                    {"context_compacted", compacted}});

        std::string cloud_error;
        const auto candidate_output = support::run_cloud_model(*selected_provider,
                                                               *candidate,
                                                               provider_api_key,
                                                               prompt_full,
                                                               &cloud_error);
        if (candidate_output.has_value()) {
          chosen_model_id = candidate->id;
          output = candidate_output.value();
          output_prompt_tokens = prompt_full_tokens;
          support::clear_cloud_model_cooldown(
              db, selected_provider->id, candidate->id, support::now_epoch_seconds());
          break;
        }
        final_error = cloud_error.empty() ? "cloud call failed" : cloud_error;
        const auto [cooldown_base, cooldown_cap] =
            effective_cooldown_policy(*cloud_cfg, *selected_provider, *candidate);
        const auto cooldown = support::record_cloud_model_failure(
            db,
            selected_provider->id,
            candidate->id,
            final_error,
            support::now_epoch_seconds(),
            cooldown_base,
            cooldown_cap);
        attempt["cooldown"] = {
            {"failure_count", cooldown.failure_count},
            {"cooldown_until", cooldown.cooldown_until},
            {"remaining_seconds", std::max(0LL, cooldown.cooldown_until - support::now_epoch_seconds())}, // LCOV_EXCL_LINE
        };
        if (!policy_trace["attempts"].empty() && policy_trace["attempts"].back().is_object() &&
            policy_trace["attempts"].back().value("model", "") == candidate->id &&
            policy_trace["attempts"].back().value("decision", "") == "selected") {
          policy_trace["attempts"].back()["cooldown"] = attempt["cooldown"];
        }
        send_event("fallback",
                   {{"provider", selected_provider->id},
                    {"model", candidate->id},
                    {"error", final_error},
                    {"cooldown_until", cooldown.cooldown_until}});
      }

      const long long updated_at = support::now_epoch_seconds();
      if (!output.has_value() || !chosen_model_id.has_value()) {
        if (thread_id.has_value() && !context_json.empty() && !summary_refreshed) {
          support::roll_thread_compaction_state(db, thread_id.value(), context_json, updated_at);
        }
        policy_trace["result"] = {
            {"status", "failed"},
            {"error", final_error},
        };
        send_event("failed", {{"error", final_error}, {"provider", selected_provider->id}});
        run_repo.update_status(run.run_id,
                               "failed",
                               std::optional<std::string>(final_error),
                               std::nullopt,
                               std::nullopt,
                               std::nullopt,
                               std::optional<std::string>(policy_trace.dump()),
                               updated_at);
      } else {
        const long long response_tokens = support::estimate_tokens_from_text(output.value());
        if (thread_id.has_value() && !context_json.empty() && !summary_refreshed) {
          support::roll_thread_compaction_state(db, thread_id.value(), context_json, updated_at);
        }
        support::record_cloud_usage_event(db,
                                          selected_provider->id,
                                          chosen_model_id.value(),
                                          output_prompt_tokens,
                                          response_tokens,
                                          updated_at,
                                          run.run_id);

        policy_trace["result"] = {
            {"status", "completed"},
            {"provider", selected_provider->id},
            {"model", chosen_model_id.value()},
            {"prompt_tokens", output_prompt_tokens},
            {"response_tokens", response_tokens},
        };

        send_event("chunk",
                   {{"provider", selected_provider->id},
                    {"model", chosen_model_id.value()},
                    {"delta", output.value()}});
        send_event("done", {{"provider", selected_provider->id}, {"model", chosen_model_id.value()}});

        std::optional<std::string> message_id;
        if (thread_id.has_value()) {
          holder::ai::AiMessageRepo msg_repo(db, fts);
          holder::model::AiMessage assistant_msg;
          assistant_msg.message_id = uuid_v4();
          assistant_msg.thread_id = thread_id.value();
          assistant_msg.role = "assistant";
          assistant_msg.source = "cloud";
          assistant_msg.provider = selected_provider->id;
          assistant_msg.model = chosen_model_id.value();
          assistant_msg.content = output.value();
          assistant_msg.created_at = updated_at;
          msg_repo.append(assistant_msg);
          message_id = assistant_msg.message_id;
        }

        maybe_update_thread_title(
            db, runner, thread_id, prompt, output.value(), updated_at);

        run_repo.update_status(
            run.run_id,
            "completed",
            std::nullopt,
            message_id,
            std::optional<std::string>(selected_provider->id + ":" + chosen_model_id.value()),
            std::nullopt,
            std::optional<std::string>(policy_trace.dump()),
            updated_at);
      }
    }
  }
  return out;
}

RouteDispatchResult execute_local_post_path(
    const nlohmann::json& body,
    const std::string& prompt,
    std::string& mode,
    const std::optional<std::string>& project_id,
    const std::optional<std::string>& thread_id,
    const std::string& context_json,
    const holder::llm::RunnerStatus& runner_status,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    boost::asio::ip::tcp::socket& socket,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts) {
  RouteDispatchResult out{};
  out.handled = true;

  std::string forced_model;
  if (body.contains("model") && !body.at("model").is_null()) {
    forced_model = body.at("model").get<std::string>();
    mode = "model";
  }

  const auto machine_caste = support::detect_machine_caste();
  std::vector<std::string> candidates;
  candidates.reserve(runner_status.models.size());
  for (const auto& model : runner_status.models) {
    candidates.push_back(model.name);
  }

  const auto model_meta = support::load_local_model_meta();
  const auto local_model_cfg = load_local_model_config(db);
  const bool forced_model_installed =
      forced_model.empty() || std::find(candidates.begin(), candidates.end(), forced_model) != candidates.end();
  if (!forced_model_installed) {
    res = support::error_response(http::status::bad_request,
                                  "bad_request",
                                  "Requested model is not installed.");
  } else if (forced_model.empty() && machine_caste.has_value()) {
    std::vector<std::string> caste_candidates;
    caste_candidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      const auto it = model_meta.find(candidate);
      if (it == model_meta.end() || it->second.hardware_tier.empty() ||
          support::caste_meets_or_exceeds(machine_caste->name, it->second.hardware_tier)) { // LCOV_EXCL_LINE
        caste_candidates.push_back(candidate);
      }
    }
    if (!caste_candidates.empty()) {
      candidates = std::move(caste_candidates);
    }

    auto keep_configured = [&](const std::optional<std::string>& configured_model) {
      if (!configured_model.has_value() || configured_model->empty()) {
        return;
      }
      if (!is_installed_model(runner_status, configured_model.value())) {
        return;
      }
      if (std::find(candidates.begin(), candidates.end(), configured_model.value()) == candidates.end()) {
        candidates.push_back(configured_model.value());
      }
    };
    if (local_model_cfg.has_value()) {
      keep_configured(local_model_cfg->fast_model);
      keep_configured(local_model_cfg->strong_model);
      keep_configured(local_model_cfg->deep_model);
    }
  }

  if (!forced_model_installed) {
    return out;
  }

  std::string router_model;
  std::string configured_strong_model;
  if (forced_model.empty() && local_model_cfg.has_value() && local_model_cfg->strong_model.has_value() &&
      std::find(candidates.begin(), candidates.end(), local_model_cfg->strong_model.value()) !=
          candidates.end()) {
    configured_strong_model = local_model_cfg->strong_model.value();
  }

  if (!configured_strong_model.empty()) {
    candidates = {configured_strong_model};
  }

  if (forced_model.empty() && candidates.size() > 1) {
    if (local_model_cfg.has_value() && local_model_cfg->fast_model.has_value() &&
        std::find(candidates.begin(), candidates.end(), local_model_cfg->fast_model.value()) !=
            candidates.end()) {
      router_model = local_model_cfg->fast_model.value();
    }

    std::vector<holder::llm::LocalModel> candidate_models;
    candidate_models.reserve(runner_status.models.size());
    for (const auto& model : runner_status.models) {
      if (std::find(candidates.begin(), candidates.end(), model.name) != candidates.end()) {
        candidate_models.push_back(model);
      }
    }

    if (router_model.empty()) {
      router_model = support::pick_router_model(candidate_models, model_meta);
    }
    if (router_model.empty()) {
      router_model = support::pick_smallest_model(candidate_models);
    }
  }

  holder::ai::AiRunRepo run_repo(db);
  holder::model::AiRun run;
  run.run_id = uuid_v4();
  run.project_id = project_id;
  run.thread_id = thread_id;
  run.mode = (mode == "model") ? "model" : "auto";
  run.prompt = prompt;
  if (!context_json.empty()) {
    run.context_json = context_json;
  }
  if (!router_model.empty()) {
    run.router_model = router_model;
  }
  run.status = "started";
  run.created_at = support::now_epoch_seconds();
  run.updated_at = run.created_at;
  run_repo.create(run);
  support::append_run_event(run.run_id,
                            "run_started",
                            {{"status", "started"}, {"created_at", run.created_at}},
                            false);

  out.streamed = true;
  http::response<http::empty_body> sse{http::status::ok, 11};
  sse.set(http::field::content_type, "text/event-stream");
  sse.set(http::field::cache_control, "no-cache");
  sse.set(http::field::connection, "keep-alive");
  sse.keep_alive(true);
  http::serializer<false, http::empty_body> sr{sse};
  boost::system::error_code write_ec;
  http::write_header(socket, sr, write_ec);
  if (write_ec) {
    return out;
  }

  auto send_event = [&](const std::string& name, const nlohmann::json& data) -> bool {
    const bool is_terminal = (name == "done" || name == "failed");
    support::append_run_event(run.run_id, name, data, is_terminal);
    std::string payload = "event: " + name + "\n";
    nlohmann::json wire = data;
    wire["run_id"] = run.run_id;
    payload += "data: " + wire.dump() + "\n\n";
    boost::system::error_code send_ec;
    boost::asio::write(socket, boost::asio::buffer(payload), send_ec);
    return !send_ec;
  };

  std::string ranked_json;
  std::string chosen_model;
  std::string assistant_text;

  if (thread_id.has_value()) {
    holder::ai::AiMessageRepo msg_repo(db, fts);
    holder::model::AiMessage user_msg;
    user_msg.message_id = uuid_v4();
    user_msg.thread_id = thread_id.value();
    user_msg.role = "user";
    user_msg.source = "local";
    user_msg.content = prompt;
    user_msg.created_at = support::now_epoch_seconds();
    msg_repo.append(user_msg);
  }

  if (!forced_model.empty()) {
    send_event("router", {{"message", "routing skipped (forced model)"}});
    candidates = {forced_model};
  } else if (candidates.size() == 1) {
    send_event("router", {{"message", "routing skipped (single model)"}});
  } else {
    const size_t router_limit = 50000;
    std::string router_input = prompt;
    if (!context_json.empty()) {
      router_input += "\n\nContext:\n";
      router_input += truncate_bytes(context_json, router_limit);
    }

    std::ostringstream prompt_ss;
    prompt_ss << "You are a routing assistant. Output ONLY a JSON array of model names "
              << "ranked by best fit for the user prompt.\n";
    prompt_ss << "User prompt:\n" << router_input << "\n\n";
    prompt_ss << "Candidate models:\n";
    for (const auto& name : candidates) {
      const auto model_it = std::find_if(runner_status.models.begin(),
                                         runner_status.models.end(),
                                         [&](const holder::llm::LocalModel& model) {
                                           return model.name == name;
                                         });
      if (model_it == runner_status.models.end()) continue;
      const auto it = model_meta.find(model_it->name);
      const std::string category = (it != model_meta.end()) ? it->second.category : "";
      prompt_ss << "- " << model_it->name << " (size=" << model_it->size;
      if (!category.empty()) {
        prompt_ss << ", category=" << category;
      }
      prompt_ss << ")\n";
    }

    std::string router_text;
    std::string router_error;
    nlohmann::json router_options;
    router_options["temperature"] = 0.1;
    router_options["num_predict"] = 256;

    runner->stream_generate(
        router_model,
        prompt_ss.str(),
        router_options.dump(),
        [&](const std::string& chunk) {
          router_text += chunk;
          send_event("router", {{"delta", chunk}});
        },
        &router_error);

    auto ranked = support::parse_ranked_models(router_text, candidates);
    if (ranked.empty()) {
      ranked = candidates;
      std::vector<holder::llm::LocalModel> candidate_models;
      candidate_models.reserve(runner_status.models.size());
      for (const auto& model : runner_status.models) {
        if (std::find(candidates.begin(), candidates.end(), model.name) != candidates.end()) {
          candidate_models.push_back(model);
        }
      }
      const std::string fallback = support::pick_largest_model(candidate_models);
      if (!fallback.empty()) {
        ranked = {fallback};
      }
    }
    nlohmann::json ranked_payload = ranked;
    ranked_json = ranked_payload.dump();
    send_event("router_result",
               {{"router_model", router_model},
                {"ranked", ranked_payload},
                {"error", router_error.empty() ? nlohmann::json(nullptr)
                                               : nlohmann::json(router_error)}});
    candidates = ranked;
  }

  std::string model_error;
  bool any_output = false;
  for (const auto& model : candidates) {
    send_event("progress", {{"message", "Trying model"}, {"model", model}});
    std::string prompt_full = prompt;
    if (!context_json.empty()) {
      prompt_full += "\n\nContext:\n";
      prompt_full += context_json;
    }
    bool got_output = false;
    model_error.clear();
    const bool ok = runner->stream_generate(
        model,
        prompt_full,
        std::string(),
        [&](const std::string& chunk) {
          got_output = true;
          any_output = true;
          assistant_text += chunk;
          send_event("chunk", {{"model", model}, {"delta", chunk}});
        },
        &model_error);

    if (ok && got_output) {
      chosen_model = model;
      break;
    }

    nlohmann::json fallback_event = {{"model", model}};
    if (!model_error.empty()) fallback_event["error"] = model_error;
    if (got_output && !ok) fallback_event["partial"] = true;
    send_event("fallback", fallback_event);
  }

  const long long updated_at = support::now_epoch_seconds();
  if (!chosen_model.empty()) {
    send_event("done", {{"model", chosen_model}});
    std::optional<std::string> message_id;
    if (thread_id.has_value()) {
      holder::ai::AiMessageRepo msg_repo(db, fts);
      holder::model::AiMessage assistant_msg;
      assistant_msg.message_id = uuid_v4();
      assistant_msg.thread_id = thread_id.value();
      assistant_msg.role = "assistant";
      assistant_msg.source = "local";
      assistant_msg.provider = "LocalRunner";
      assistant_msg.model = chosen_model;
      assistant_msg.content = assistant_text;
      assistant_msg.created_at = updated_at;
      msg_repo.append(assistant_msg);
      message_id = assistant_msg.message_id;
    }
    maybe_update_thread_title(
        db, runner, thread_id, prompt, assistant_text, updated_at);
    run_repo.update_status(run.run_id,
                           "completed",
                           std::nullopt,
                           message_id,
                           chosen_model,
                           ranked_json.empty() ? std::nullopt
                                               : std::optional<std::string>(ranked_json),
                           std::nullopt,
                           updated_at);
  } else {
    send_event("failed", {{"error", "All models failed."}});
    run_repo.update_status(run.run_id,
                           "failed",
                           std::optional<std::string>(any_output ? "partial failure" : "no output"),
                           std::nullopt,
                           std::nullopt,
                           ranked_json.empty() ? std::nullopt
                                               : std::optional<std::string>(ranked_json),
                           std::nullopt,
                           updated_at);
  }

  return out;
}

} // namespace

RouteDispatchResult handle_ai_runs_post_route(
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::privacy::SecretStore* secret_store,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4) {
  RouteDispatchResult out{};
  out.handled = true;
  try {
    const auto parsed = parse_ai_run_post_input(req, res);
    if (!parsed.has_value()) {
      return out;
    }
    AiRunPostInput input = parsed.value();
    const auto& body = input.body;
    const std::string& prompt = input.prompt;
    std::string& mode = input.mode;
    auto& project_id = input.project_id;
    auto& thread_id = input.thread_id;
    const std::string& context_json = input.context_json;
    const bool cloud_provider_requested =
        body.contains("provider") && !body.at("provider").is_null() &&
        !support::normalize_provider_name(body.at("provider").get<std::string>()).empty();

    holder::llm::RunnerStatus runner_status;
    const bool has_runner = (runner != nullptr);
    if (has_runner) {
      runner_status = runner->status();
    }
    const bool local_runner_ready = has_runner && runner_status.available && !runner_status.models.empty();

    ensure_ai_run_thread(input, db, uuid_v4);

    if (!local_runner_ready || cloud_provider_requested) {
      return execute_cloud_post_path(
          body,
          prompt,
          mode,
          project_id,
          thread_id,
          context_json,
          secret_store,
          runner,
          uuid_v4,
          socket,
          res,
          db,
          fts);
    }
    return execute_local_post_path(body,
                                   prompt,
                                   mode,
                                   project_id,
                                   thread_id,
                                   context_json,
                                   runner_status,
                                   runner,
                                   uuid_v4,
                                   socket,
                                   res,
                                   db,
                                   fts);
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }
  return out;
}

} // namespace holder::api::routes::ai::runs
