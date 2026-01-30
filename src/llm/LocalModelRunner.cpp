#include "llm/LocalModelRunner.h"

#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/process/v2.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>

namespace holder::llm {
namespace {

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string getenv_or(const char* key, const std::string& fallback) {
  const char* val = std::getenv(key);
  if (val && *val) {
    return std::string(val);
  }
  return fallback;
}

} // namespace

struct LocalModelRunner::RunnerProcess {
  std::mutex mu;
  std::optional<boost::process::v2::process::handle_type> handle;
};

LocalModelRunner::LocalModelRunner()
    : host_(getenv_or("HOLDER_MODEL_RUNNER_HOST", "127.0.0.1")),
      port_(getenv_or("HOLDER_MODEL_RUNNER_PORT", "11434")),
      exec_path_(getenv_or("HOLDER_MODEL_RUNNER_BIN", "")) {}

LocalModelRunner::~LocalModelRunner() = default;

void LocalModelRunner::start_background_probe() {
  bool expected = false;
  if (!background_started_.compare_exchange_strong(expected, true)) {
    return;
  }
  std::thread([this]() { probe(true); }).detach();
}

RunnerStatus LocalModelRunner::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

RunnerStatus LocalModelRunner::retry() {
  probe(true);
  return status();
}

bool LocalModelRunner::http_get_json(const std::string& target,
                                  std::string* out,
                                  std::string* error) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(host_, port_);

    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(2));
    stream.connect(endpoints);

    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host_);
    req.set(http::field::user_agent, "holder/model-runner");

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    if (res.result() != http::status::ok) {
      if (error) {
        *error = "HTTP " + std::to_string(res.result_int());
      }
      return false;
    }

    if (out) {
      *out = res.body();
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  }
}

bool LocalModelRunner::try_spawn(std::string* error) {
  if (spawn_attempted_.exchange(true)) {
    return false;
  }

  boost::process::v2::filesystem::path exe_path;
  if (!exec_path_.empty()) {
    exe_path = boost::process::v2::filesystem::path(exec_path_);
  } else {
    exe_path = boost::process::v2::environment::find_executable("ollama");
  }

  if (exe_path.empty()) {
    if (error) {
      *error = "model runner executable not found";
    }
    return false;
  }

  try {
    boost::asio::io_context ioc;
    boost::process::v2::process proc(ioc.get_executor(), exe_path, {"serve"});
    auto handle = proc.detach();
    if (!process_) {
      process_ = std::make_unique<RunnerProcess>();
    }
    {
      std::lock_guard<std::mutex> lock(process_->mu);
      process_->handle = std::move(handle);
    }
    spdlog::info("Spawned model runner: {}", exe_path.string());
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    spdlog::warn("Failed to spawn model runner: {}", ex.what());
    return false;
  }
}

void LocalModelRunner::probe(bool allow_spawn) {
  RunnerStatus next;
  next.spawn_attempted = spawn_attempted_.load();
  next.last_checked = now_epoch_seconds();

  std::string err;
  std::string version_body;
  bool spawned = false;

  if (!http_get_json("/api/version", &version_body, &err)) {
    if (allow_spawn && try_spawn(&err)) {
      spawned = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      version_body.clear();
      err.clear();
      if (!http_get_json("/api/version", &version_body, &err)) {
        next.available = false;
        next.error = err.empty() ? "model runner not reachable" : err;
      }
    } else {
      next.available = false;
      next.error = err.empty() ? "model runner not reachable" : err;
    }
  }

  if (!version_body.empty()) {
    try {
      auto json = nlohmann::json::parse(version_body);
      if (json.contains("version")) {
        next.version = json["version"].get<std::string>();
      }
    } catch (const std::exception& ex) {
      next.error = ex.what();
    }
  }

  std::string tags_body;
  if (http_get_json("/api/tags", &tags_body, &err)) {
    try {
      auto json = nlohmann::json::parse(tags_body);
      if (json.contains("models") && json["models"].is_array()) {
        for (const auto& model : json["models"]) {
          LocalModel item;
          if (model.contains("name")) item.name = model["name"].get<std::string>();
          if (model.contains("digest")) item.digest = model["digest"].get<std::string>();
          if (model.contains("size")) item.size = model["size"].get<long long>();
          if (model.contains("modified_at")) item.modified_at = model["modified_at"].get<std::string>();
          next.models.push_back(item);
        }
      }
    } catch (const std::exception& ex) {
      next.error = ex.what();
    }
  } else if (next.error.empty() && !err.empty()) {
    next.error = err;
  }

  if (!next.version.empty() || !next.models.empty()) {
    next.available = true;
  }

  next.spawn_attempted = spawn_attempted_.load();

  {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = next;
  }

  if (next.available) {
    const std::string provider = "ollama";
    if (spawned) {
      if (!next.version.empty()) {
        spdlog::info("Started local model runner subprocess ({} {}).", provider, next.version);
      } else {
        spdlog::info("Started local model runner subprocess ({}).", provider);
      }
    } else {
      if (!next.version.empty()) {
        spdlog::info("Connected to already running local model runner instance ({} {}).",
                     provider,
                     next.version);
      } else {
        spdlog::info("Connected to already running local model runner instance ({}).", provider);
      }
    }
  } else {
    spdlog::info("No local model runner available.");
  }
}

void LocalModelRunner::stop() {
  if (!process_) {
    return;
  }

  std::optional<boost::process::v2::process::handle_type> handle;
  {
    std::lock_guard<std::mutex> lock(process_->mu);
    handle = std::move(process_->handle);
  }
  if (!handle.has_value()) {
    return;
  }

  boost::system::error_code ec;
  auto& proc = handle.value();
  boost::process::v2::native_exit_code_type exit_status{};
  proc.terminate(exit_status, ec);
  if (ec) {
    spdlog::warn("Failed to terminate local model runner: {}", ec.message());
    return;
  }
  spdlog::info("Local model runner terminated.");
}

} // namespace holder::llm
