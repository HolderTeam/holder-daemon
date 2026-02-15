#include "api/routes/ai/runs/AiRunPostRoute.h"

#include "api/support/CloudClient.h"
#include "api/support/CloudConfig.h"
#include "api/support/CloudQuota.h"
#include "api/support/HttpResponses.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/ProviderUtils.h"
#include "api/support/RunEventStore.h"
#include "api/support/Time.h"
#include "store/AiMessageRepo.h"
#include "store/AiProviderCredentialRepo.h"
#include "store/AiRouterConfigRepo.h"
#include "store/AiRunRepo.h"
#include "store/AiThreadRepo.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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
                          holder::store::Db& db,
                          const std::function<std::string()>& uuid_v4) {
  if (input.thread_id.has_value() || !input.project_id.has_value()) {
    return;
  }

  holder::store::AiThreadRepo thread_repo(db);
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

RouteDispatchResult execute_cloud_post_path(
    const nlohmann::json& body,
    const std::string& prompt,
    std::string& mode,
    const std::optional<std::string>& project_id,
    const std::optional<std::string>& thread_id,
    const std::string& context_json,
    const std::function<std::string()>& uuid_v4,
    boost::asio::ip::tcp::socket& socket,
    http::response<http::string_body>& res,
    holder::store::Db& db,
    holder::index::FtsIndexer* fts) {
  RouteDispatchResult out{};
  out.handled = true;

  const auto cloud_cfg = support::load_cloudproviders_config();
  if (!cloud_cfg.has_value()) {
    res = support::error_response(http::status::service_unavailable,
                                  "runner_unavailable",
                                  "No local runner and cloudproviders.yaml not found.");
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

  holder::store::AiProviderCredentialRepo credential_repo(db);
  const auto credentials = credential_repo.list();
  std::unordered_map<std::string, holder::model::AiProviderCredential> creds_by_key;
  for (const auto& credential : credentials) {
    creds_by_key[credential.provider] = credential;
  }

  const support::CloudProviderConfig* selected_provider = nullptr;
  const holder::model::AiProviderCredential* selected_cred = nullptr;
  bool selection_failed = false;

  auto try_select = [&](const support::CloudProviderConfig& provider) -> bool {
    if (!provider.enabled) return false;
    const auto it = creds_by_key.find(provider.credential_provider_key);
    if (it == creds_by_key.end()) return false;
    selected_provider = &provider;
    selected_cred = &it->second;
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
    if (!cloud_cfg->default_provider.empty()) {
      const auto* provider = support::find_cloud_provider(cloud_cfg.value(), cloud_cfg->default_provider);
      if (provider) {
        try_select(*provider);
      }
    }
  }
  if (!selected_provider && !selection_failed) {
    for (const auto& provider_cfg : cloud_cfg->providers) {
      if (try_select(provider_cfg)) break;
    }
  }

  if (!selected_provider && !selection_failed) {
    res = support::error_response(http::status::service_unavailable,
                                  "cloud_not_configured",
                                  "No enabled cloud provider with stored API key.");
  }

  if (selected_provider && selected_cred) {
    const auto candidate_models = support::cloud_model_candidates(*selected_provider, requested_model);
    if (candidate_models.empty()) {
      res = support::error_response(http::status::service_unavailable,
                                    "cloud_not_configured",
                                    "No cloud model configured for selected provider.");
    } else {
      holder::store::AiRunRepo run_repo(db);
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
        holder::store::AiMessageRepo msg_repo(db, fts);
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

      for (const auto* candidate : candidate_models) {
        nlohmann::json attempt;
        attempt["model"] = candidate->id;

        const long long now = support::now_epoch_seconds();
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
        std::string compacted_context =
            support::compact_context_tail(context_json, context_budget, &compacted);
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
                                                               selected_cred->api_key,
                                                               prompt_full,
                                                               &cloud_error);
        if (candidate_output.has_value()) {
          chosen_model_id = candidate->id;
          output = candidate_output.value();
          output_prompt_tokens = prompt_full_tokens;
          break;
        }
        final_error = cloud_error.empty() ? "cloud call failed" : cloud_error;
        send_event("fallback",
                   {{"provider", selected_provider->id},
                    {"model", candidate->id},
                    {"error", final_error}});
      }

      const long long updated_at = support::now_epoch_seconds();
      if (!output.has_value() || !chosen_model_id.has_value()) {
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
                               std::optional<std::string>(policy_trace.dump()),
                               updated_at);
      } else {
        const long long response_tokens = support::estimate_tokens_from_text(output.value());
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
          holder::store::AiMessageRepo msg_repo(db, fts);
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

        run_repo.update_status(
            run.run_id,
            "completed",
            std::nullopt,
            message_id,
            std::optional<std::string>(selected_provider->id + ":" + chosen_model_id.value()),
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
    holder::store::Db& db,
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
          support::caste_meets_or_exceeds(machine_caste->name, it->second.hardware_tier)) {
        caste_candidates.push_back(candidate);
      }
    }
    if (!caste_candidates.empty()) {
      candidates = std::move(caste_candidates);
    }
  }

  if (!forced_model_installed) {
    return out;
  }

  std::string router_model;
  if (forced_model.empty() && candidates.size() > 1) {
    auto is_installed = [&](const std::string& name) {
      return std::find_if(runner_status.models.begin(),
                          runner_status.models.end(),
                          [&](const holder::llm::LocalModel& model) { return model.name == name; }) !=
             runner_status.models.end();
    };

    try {
      holder::store::AiRouterConfigRepo router_cfg_repo(db);
      if (project_id.has_value()) {
        const auto cfg = router_cfg_repo.get_for_project(project_id.value());
        if (cfg.has_value() && is_installed(cfg->router_model)) {
          router_model = cfg->router_model;
        }
      }
      if (router_model.empty()) {
        const auto global_cfg = router_cfg_repo.get_global();
        if (global_cfg.has_value() && is_installed(global_cfg->router_model)) {
          router_model = global_cfg->router_model;
        }
      }
    } catch (const std::exception&) {
      // Router config storage unavailable; fallback to auto pick.
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

  holder::store::AiRunRepo run_repo(db);
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
    holder::store::AiMessageRepo msg_repo(db, fts);
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
      holder::store::AiMessageRepo msg_repo(db, fts);
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
    run_repo.update_status(run.run_id,
                           "completed",
                           std::nullopt,
                           message_id,
                           chosen_model,
                           ranked_json.empty() ? std::nullopt
                                               : std::optional<std::string>(ranked_json),
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
                           updated_at);
  }

  return out;
}

} // namespace

RouteDispatchResult handle_ai_runs_post_route(
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::store::Db& db,
    holder::index::FtsIndexer* fts,
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

    holder::llm::RunnerStatus runner_status;
    const bool has_runner = (runner != nullptr);
    if (has_runner) {
      runner_status = runner->status();
    }
    const bool local_runner_ready = has_runner && runner_status.available && !runner_status.models.empty();

    ensure_ai_run_thread(input, db, uuid_v4);

    if (!local_runner_ready) {
      return execute_cloud_post_path(
          body, prompt, mode, project_id, thread_id, context_json, uuid_v4, socket, res, db, fts);
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
