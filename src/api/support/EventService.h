#pragma once

#include "api/support/SseStream.h"
#include "platform/Db.h"

#include <condition_variable>
#include <filesystem>
#include <thread>

namespace holder::api::support {

class EventService {
 public:
  explicit EventService(const std::filesystem::path& database);
  ~EventService();
  void stop();
  std::shared_ptr<SseRegistry> streams() const { return streams_; }
  bool dispatch(
      const std::string& path,
      const std::string& query,
      const boost::beast::http::request<boost::beast::http::string_body>& req,
      boost::beast::http::response<boost::beast::http::string_body>& res,
      boost::asio::ip::tcp::socket& socket,
      holder::platform::Db& db,
      bool& streamed
  );

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    EventJournal changes{4096, 1024 * 1024};
    nlohmann::json revisions = nlohmann::json::object();
    bool stopped = false;
    bool available = true;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  std::shared_ptr<SseRegistry> streams_ = std::make_shared<SseRegistry>();
  std::thread observer_;
  std::mutex stop_mutex_;
};

} // namespace holder::api::support
