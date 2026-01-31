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
#include <limits>
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

std::string trim(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
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

std::string LocalModelRunner::generate_job_id() {
  static std::atomic<unsigned long long> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto seq = ++counter;
  return "pull-" + std::to_string(static_cast<unsigned long long>(now)) + "-" +
         std::to_string(seq);
}

LocalModelRunner::PullJob LocalModelRunner::start_pull(const std::string& model) {
  PullJob job;
  if (model.empty()) {
    job.status = "failed";
    job.error = "missing model";
    job.updated_at = now_epoch_seconds();
    return job;
  }

  {
    std::lock_guard<std::mutex> lock(pulls_mu_);
    for (auto& [id, existing] : pulls_) {
      if (existing.model == model &&
          (existing.status == "queued" || existing.status == "downloading" ||
           existing.status == "verifying")) {
        return existing;
      }
    }
  }

  job.job_id = generate_job_id();
  job.model = model;
  job.status = "queued";
  job.updated_at = now_epoch_seconds();

  {
    std::lock_guard<std::mutex> lock(pulls_mu_);
    pulls_[job.job_id] = job;
  }

  std::thread([this, job_id = job.job_id, model]() { run_pull(job_id, model); }).detach();
  return job;
}

std::optional<LocalModelRunner::PullJob> LocalModelRunner::get_pull(const std::string& job_id) const {
  std::lock_guard<std::mutex> lock(pulls_mu_);
  const auto it = pulls_.find(job_id);
  if (it == pulls_.end()) {
    return std::nullopt;
  }
  return it->second;
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

void LocalModelRunner::run_pull(const std::string& job_id, const std::string& model) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  bool completed = false;

  {
    std::lock_guard<std::mutex> lock(pulls_mu_);
    auto it = pulls_.find(job_id);
    if (it == pulls_.end()) return;
    it->second.status = "downloading";
    it->second.updated_at = now_epoch_seconds();
  }

  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(host_, port_);

    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(10));
    stream.connect(endpoints);

    nlohmann::json body;
    body["name"] = model;
    body["stream"] = true;

    http::request<http::string_body> req{http::verb::post, "/api/pull", 11};
    req.set(http::field::host, host_);
    req.set(http::field::user_agent, "holder/model-runner");
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    http::read_header(stream, buffer, parser);
    const auto header = parser.get();
    if (header.result() != http::status::ok) {
      std::string msg = "HTTP " + std::to_string(header.result_int());
      std::lock_guard<std::mutex> lock(pulls_mu_);
      auto it = pulls_.find(job_id);
      if (it != pulls_.end()) {
        it->second.status = "failed";
        it->second.error = msg;
        it->second.updated_at = now_epoch_seconds();
      }
      return;
    }

    parser.get().body().clear();
    boost::system::error_code ec;
    bool finished = false;
    while (!parser.is_done()) {
      http::read_some(stream, buffer, parser, ec);
      if (ec == http::error::need_buffer || ec == boost::asio::error::would_block) {
        continue;
      }
      if (ec == http::error::end_of_stream) {
        break;
      }
      if (ec) {
        throw boost::system::system_error(ec);
      }

      std::string chunk = parser.get().body();
      parser.get().body().clear();
      size_t start = 0;
      while (start < chunk.size()) {
        const auto end = chunk.find('\n', start);
        const auto line = (end == std::string::npos) ? chunk.substr(start)
                                                     : chunk.substr(start, end - start);
        start = (end == std::string::npos) ? chunk.size() : end + 1;
        const auto trimmed = trim(line);
        if (trimmed.empty()) continue;
        try {
          const auto payload = nlohmann::json::parse(trimmed);
          PullJob update;
          bool done = false;
          if (payload.contains("error")) {
            update.status = "failed";
            update.error = payload["error"].get<std::string>();
            done = true;
          }
          if (payload.contains("status")) {
            update.progress.stage = payload["status"].get<std::string>();
            if (update.progress.stage == "success") {
              update.status = "completed";
              done = true;
            } else if (update.progress.stage == "verifying") {
              update.status = "verifying";
            } else if (update.status.empty()) {
              update.status = "downloading";
            }
          }
          if (payload.contains("completed")) {
            update.progress.completed = payload["completed"].get<long long>();
          }
          if (payload.contains("total")) {
            update.progress.total = payload["total"].get<long long>();
          }
          if (update.progress.total > 0) {
            update.progress.percent =
                (static_cast<double>(update.progress.completed) /
                 static_cast<double>(update.progress.total)) *
                100.0;
          }

          {
            std::lock_guard<std::mutex> lock(pulls_mu_);
            auto it = pulls_.find(job_id);
            if (it != pulls_.end()) {
              if (!update.status.empty()) it->second.status = update.status;
              if (!update.error.empty()) it->second.error = update.error;
              if (!update.progress.stage.empty()) it->second.progress.stage = update.progress.stage;
              if (update.progress.total > 0) it->second.progress.total = update.progress.total;
              if (update.progress.completed > 0) it->second.progress.completed = update.progress.completed;
              if (update.progress.total > 0) it->second.progress.percent = update.progress.percent;
              it->second.updated_at = now_epoch_seconds();
            }
          }
          if (done) {
            if (update.status == "completed") {
              completed = true;
            }
            finished = true;
            break;
          }
        } catch (const std::exception&) {
          // Ignore malformed progress lines.
        }
      }
      if (finished) break;
    }

    boost::system::error_code shutdown_ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);
    if (completed) {
      probe(false);
    }
  } catch (const std::exception& ex) {
    std::lock_guard<std::mutex> lock(pulls_mu_);
    auto it = pulls_.find(job_id);
    if (it != pulls_.end()) {
      it->second.status = "failed";
      it->second.error = ex.what();
      it->second.updated_at = now_epoch_seconds();
    }
  }
}

} // namespace holder::llm
