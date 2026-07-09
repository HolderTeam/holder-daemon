#pragma once

#include "ai/NudgeService.h"
#include "api/Router.h"
#include "card/CardStore.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace holder::api {

class Session {
 public:
  using tcp = boost::asio::ip::tcp;
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response = boost::beast::http::response<boost::beast::http::string_body>;
  struct IoHandle {
    std::function<void()> cancel;
  };
  using IoHandlePtr = std::shared_ptr<IoHandle>;
  using SocketHook = std::function<void(const IoHandlePtr&)>;

  enum class RequestLane : std::uint8_t {
    Save,
    Foreground,
    Background,
  };

  struct PreparedRequest {
    tcp::socket socket;
    Request req;
    std::chrono::steady_clock::time_point request_started;
    std::string path;
    std::string query_string;
    RequestLane lane = RequestLane::Foreground;
  };

  struct PreparedResponse {
    tcp::socket socket;
    Request req;
    Response res;
    std::chrono::steady_clock::time_point request_started;
    RequestLane lane = RequestLane::Foreground;
  };

  Session(
      tcp::socket socket,
      holder::platform::Db& db,
      const std::string& auth_token,
      const Router& router,
      std::chrono::steady_clock::time_point started_at,
      holder::card::CardStore* card_store,
      holder::index::FtsIndexer* fts,
      holder::ai::NudgeService* nudge_service,
      holder::privacy::SecretStore* secret_store = nullptr,
      holder::git::GitOps* git_ops = nullptr,
      holder::llm::RunnerRegistry* runner_registry = nullptr
  );
  Session(
      PreparedRequest prepared,
      holder::platform::Db& db,
      const std::string& auth_token,
      const Router& router,
      std::chrono::steady_clock::time_point started_at,
      holder::card::CardStore* card_store,
      holder::index::FtsIndexer* fts,
      holder::ai::NudgeService* nudge_service,
      holder::privacy::SecretStore* secret_store = nullptr,
      holder::git::GitOps* git_ops = nullptr,
      holder::llm::RunnerRegistry* runner_registry = nullptr
  );

  void run();
  static std::optional<PreparedRequest> prepare_request(
      tcp::socket socket,
      const SocketHook& on_io_start = {},
      const SocketHook& on_io_done = {}
  );
  std::optional<PreparedResponse> execute();
  static void write_prepared_response(
      PreparedResponse prepared,
      const SocketHook& on_io_start = {},
      const SocketHook& on_io_done = {}
  );
  static const char* lane_name(RequestLane lane);

 private:
  bool ensure_request_loaded();
  std::optional<PreparedResponse> process_loaded_request();
  static RequestLane classify_request_lane(const Request& req, const std::string& path);

  tcp::socket socket_;
  holder::platform::Db& db_;
  const std::string& auth_token_;
  const Router& router_;
  std::chrono::steady_clock::time_point started_at_;
  holder::card::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::ai::NudgeService* nudge_service_ = nullptr;
  holder::privacy::SecretStore* secret_store_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
  holder::llm::RunnerRegistry* runner_registry_ = nullptr;
  Request req_;
  std::chrono::steady_clock::time_point request_started_;
  std::string path_;
  std::string query_string_;
  bool has_loaded_request_ = false;
};

} // namespace holder::api
