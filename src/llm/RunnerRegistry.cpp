#include "llm/ExecutorRunnerClient.h"
#include "llm/RunnerRegistry.h"

#include "ai/AiRunnerRepo.h"
#include "llm/LocalModelRunner.h"
#include "llm/LocalRunnerClient.h"

#include <string_view>

namespace holder::llm {
namespace {

holder::model::AiRunner auto_local_runner_record() {
  return holder::model::AiRunner{
      .runner_id = RunnerRegistry::kAutoLocalRunnerId,
      .name = "Local Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://127.0.0.1:11434"),
      .source = "auto_local",
      .enabled = true,
      .created_at = 0,
      .updated_at = 0,
  };
} // LCOV_EXCL_LINE

struct ParsedBaseUrl {
  std::string host;
  std::string port;
};

std::optional<ParsedBaseUrl> parse_http_base_url(const std::optional<std::string>& base_url) {
  if (!base_url.has_value() || base_url->empty()) {
    return std::nullopt;
  }

  std::string_view value = base_url.value();
  constexpr std::string_view prefix = "http://";
  if (!value.starts_with(prefix)) {
    return std::nullopt;
  }

  value.remove_prefix(prefix.size());
  if (value.empty()) {
    return std::nullopt;
  }

  const auto slash = value.find('/');
  if (slash != std::string_view::npos) {
    if (slash != value.size()) {
      value = value.substr(0, slash);
    }
  }
  if (value.empty()) {
    return std::nullopt;
  }

  const auto colon = value.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon == value.size() - 1) {
    return std::nullopt;
  }

  ParsedBaseUrl out;
  out.host = std::string(value.substr(0, colon));
  out.port = std::string(value.substr(colon + 1));
  if (out.host.empty() || out.port.empty()) {
    return std::nullopt;
  }
  return out;
}

} // namespace

RunnerRegistry::RunnerRegistry(holder::platform::Db* db,
                               RunnerClient* auto_local_client,
                               const holder::core::SerialExecutor* executor)
    : db_(db), auto_local_client_(auto_local_client), executor_(executor) {
  if (auto_local_client_ != nullptr && executor_ != nullptr) {
    auto_local_wrapped_client_ =
        std::make_unique<ExecutorRunnerClient>(*auto_local_client_, *executor_);
  }
  load_manual_clients();
}

void RunnerRegistry::refresh() {
  load_manual_clients();
}

std::vector<holder::model::AiRunner> RunnerRegistry::list_runners() const {
  std::vector<holder::model::AiRunner> out;
  out.push_back(auto_local_runner_record());
  if (db_ != nullptr) {
    holder::ai::AiRunnerRepo repo(*db_);
    auto manual = repo.list();
    out.insert(out.end(), manual.begin(), manual.end());
  }
  return out;
}

std::optional<holder::model::AiRunner> RunnerRegistry::get_runner(const std::string& runner_id) const {
  if (runner_id == kAutoLocalRunnerId) {
    return auto_local_runner_record();
  }
  if (db_ != nullptr) {
    holder::ai::AiRunnerRepo repo(*db_);
    return repo.get(runner_id);
  }
  return std::nullopt;
}

RunnerClient* RunnerRegistry::get_client(const std::string& runner_id) const {
  if (runner_id == kAutoLocalRunnerId) {
    return auto_local_wrapped_client_ != nullptr ? auto_local_wrapped_client_.get()
                                                 : auto_local_client_;
  }
  const auto it = manual_clients_.find(runner_id);
  return it != manual_clients_.end() ? it->second.get() : nullptr;
}

void RunnerRegistry::load_manual_clients() {
  manual_clients_.clear();
  if (db_ == nullptr) {
    return;
  }

  holder::ai::AiRunnerRepo repo(*db_);
  for (const auto& runner : repo.list()) {
    if (!runner.enabled || runner.source != "manual" || runner.kind != "ollama") {
      continue;
    }
    const auto parsed = parse_http_base_url(runner.base_url);
    if (!parsed.has_value()) {
      continue;
    }

    auto manual_runner = std::make_unique<LocalModelRunner>(
        parsed->host, parsed->port, std::string(), false);
    std::unique_ptr<RunnerClient> client =
        std::make_unique<LocalRunnerClient>(std::move(manual_runner));
    if (executor_ != nullptr) {
      client = std::make_unique<ExecutorRunnerClient>(std::move(client), *executor_);
    }
    client->start_background_probe();
    manual_clients_.emplace(runner.runner_id, std::move(client));
  }
}

} // namespace holder::llm
