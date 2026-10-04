#include "api/support/EventService.h"

#include "api/support/ChangeFeed.h"
#include "api/support/HttpResponses.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <charconv>
#include <future>
#include <map>
#include <stdexcept>

namespace holder::api::support {
namespace {
namespace http = boost::beast::http;

std::string decode(const std::string& encoded) {
  std::string result;
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%') {
      if (i + 2 >= encoded.size()) throw std::invalid_argument("Invalid query encoding.");
      unsigned int byte = 0;
      const auto parsed = std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + i + 3 || byte == 0)
        throw std::invalid_argument("Invalid query encoding.");
      result.push_back(static_cast<char>(byte));
      i += 2;
    } else
      result.push_back(encoded[i] == '+' ? ' ' : encoded[i]);
  }
  return result;
}

std::map<std::string, std::string> parameters(const std::string& query) {
  std::map<std::string, std::string> result;
  for (std::size_t offset = 0; offset < query.size();) {
    const auto end = query.find('&', offset);
    const auto field = query.substr(offset, end == std::string::npos ? end : end - offset);
    const auto equal = field.find('=');
    const auto key = decode(field.substr(0, equal));
    const auto value = equal == std::string::npos ? "" : decode(field.substr(equal + 1));
    if ((key != "project_id" && key != "last_revision") || !result.emplace(key, value).second)
      throw std::invalid_argument("Unsupported or repeated event query parameter.");
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return result;
}

std::string history_url(const std::string& project) {
  std::string encoded;
  constexpr char hex[] = "0123456789ABCDEF";
  for (const char ch : project) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.')
      encoded.push_back(static_cast<char>(byte));
    else {
      encoded += '%';
      encoded += hex[byte >> 4];
      encoded += hex[byte & 15];
    }
  }
  return "/projects/" + encoded + "/history";
}

nlohmann::json checkpoint(
    const std::string& cursor,
    const nlohmann::json& revisions,
    const std::string& project
) {
  auto selected = revisions;
  if (!project.empty()) selected = {{project, revisions.value(project, nlohmann::json(nullptr))}};
  auto history = nlohmann::json::object();
  for (const auto& [id, revision] : selected.items())
    history[id] = history_url(id);
  return {{"cursor", cursor}, {"git_revisions", selected}, {"history_urls", history}};
}

} // namespace

EventService::EventService(const std::filesystem::path& database) {
  auto ready = std::make_shared<std::promise<void>>();
  auto future = ready->get_future();
  observer_ = std::thread([database, state = state_, ready] {
    holder::platform::Db db;
    std::unique_ptr<ChangeFeed> feed;
    std::unique_lock lock(state->mutex);
    auto observe = [&] {
      try {
        if (!feed) {
          if (database.empty()) throw std::runtime_error("No database available for change feed");
          db.open(database);
          db.exec("PRAGMA query_only=ON");
          feed = std::make_unique<ChangeFeed>(db, state->changes);
        } else {
          feed->poll();
        }
        state->revisions = feed->revisions();
        state->available = true;
      } catch (const std::exception& ex) {
        if (state->available) {
          state->changes.append("resync_required", {{"reason", "observer_unavailable"}});
          spdlog::warn("change feed observer unavailable: {}", ex.what());
        }
        state->available = false;
      }
    };
    // An unavailable projection must not take down health/recovery routes.
    observe();
    ready->set_value();
    while (!state->stopped) {
      if (state->wake.wait_for(lock, std::chrono::milliseconds(250), [&] {
            return state->stopped;
          }))
        break;
      observe();
    }
  });
  try {
    future.get();
  } catch (...) {
    observer_.join();
    throw;
  }
}

EventService::~EventService() { stop(); }

void EventService::stop() {
  std::lock_guard stop_lock(stop_mutex_);
  {
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
  }
  state_->wake.notify_all();
  streams_->stop();
  if (observer_.joinable()) observer_.join();
}

bool EventService::dispatch(
    const std::string& path,
    const std::string& query,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    bool& streamed
) {
  if (path != "/events" && path != "/events/cursor") return false;
  if (req.method() != http::verb::get) {
    res = error_response(
        http::status::method_not_allowed,
        "method_not_allowed",
        "Use GET for events."
    );
    return true;
  }
  std::string project, revision;
  try {
    const auto params = parameters(query);
    if (const auto it = params.find("project_id"); it != params.end()) {
      project = it->second;
      if (project.empty()) throw std::invalid_argument("project_id must not be empty.");
    }
    if (const auto it = params.find("last_revision"); it != params.end()) {
      if (path == "/events/cursor")
        throw std::invalid_argument("last_revision is only supported on /events.");
      revision = it->second;
      if (project.empty() || revision.size() != 40 ||
          revision.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument(
            "last_revision requires project_id and a full lowercase Git SHA."
        );
    }
    const std::string last_id(req["Last-Event-ID"]);
    if (!last_id.empty() && !EventJournal::valid_cursor(last_id))
      throw std::invalid_argument("Invalid Last-Event-ID.");
  } catch (const std::invalid_argument& ex) {
    res = error_response(http::status::bad_request, "bad_request", ex.what());
    return true;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->available || state_->stopped) {
      res = error_response(
          http::status::service_unavailable,
          "unavailable",
          "Change feed unavailable."
      );
      return true;
    }
  }
  if (!project.empty()) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(
            db.handle(),
            "SELECT 1 FROM projects WHERE project_id=?",
            -1,
            &raw,
            nullptr
        ) != SQLITE_OK) {
      if (raw) sqlite3_finalize(raw);
      res =
          error_response(http::status::service_unavailable, "unavailable", "Project unavailable.");
      return true;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, project.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
      res = error_response(
          rc == SQLITE_DONE ? http::status::not_found : http::status::service_unavailable,
          rc == SQLITE_DONE ? "not_found" : "unavailable",
          "Project unavailable."
      );
      return true;
    }
  }
  std::string cursor(req["Last-Event-ID"]);
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->available || state_->stopped) {
      res = error_response(
          http::status::service_unavailable,
          "unavailable",
          "Change feed unavailable."
      );
      return true;
    }
    if (path == "/events/cursor") {
      res = json_response(
          http::status::ok,
          {{"ok", true}, {"data", checkpoint(state_->changes.cursor(), state_->revisions, project)}}
      );
      return true;
    }
    if (cursor.empty() && revision.empty()) cursor = state_->changes.cursor();
  }
  auto source = [state = state_, project, revision, cursor, first = true]() mutable {
    std::lock_guard lock(state->mutex);
    auto batch = state->changes.read(cursor);

    if (batch.resync_required || !state->available || (!revision.empty() && cursor.empty())) {
      auto data = checkpoint(batch.cursor, state->revisions, project);
      data["reason"] = state->available ? "history_unavailable" : "observer_unavailable";
      data["last_revision"] = revision.empty() ? nlohmann::json(nullptr) : nlohmann::json(revision);
      batch.events = {{batch.cursor, "resync_required", data}};
      batch.resync_required = false;
      batch.finished = true;
      return batch;
    }
    std::erase_if(batch.events, [&](const StreamEvent& event) {
      return !project.empty() &&
             event.data.value("project_id", nlohmann::json(nullptr)) != project &&
             event.name != "resync_required";
    });
    if (first) {
      // Preserve the old cursor on ready; advancing it before replay was delivered
      // could silently skip events if the connection dropped during its handshake.
      batch.events.insert(
          batch.events.begin(),
          {cursor, "ready", checkpoint(batch.cursor, state->revisions, project)}
      );
      first = false;
    }
    for (auto& event : batch.events) {
      if (event.name == "resync_required") {
        const auto reason = event.data.value("reason", "observer_unavailable");
        event.data = checkpoint(batch.cursor, state->revisions, project);
        event.data["reason"] = reason;
        event.data["last_revision"] = revision.empty() ? nlohmann::json(nullptr)
                                                       : nlohmann::json(revision);
        batch.finished = true;
      }
    }
    cursor = batch.cursor;
    return batch;
  };
  auto stream = SseStream::start(socket, streams_, std::move(source));
  if (!stream)
    res =
        error_response(http::status::service_unavailable, "server_busy", "Too many event streams.");
  else
    streamed = true;
  return true;
}

} // namespace holder::api::support
