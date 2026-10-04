#pragma once

#include "api/support/EventJournal.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace holder::api::support {

class SseStream;

// Subscriptions occupy asynchronous sockets, not HTTP request workers. Polling
// runner adapters can wait for their serialized executor, so use a small separate
// pool instead of calling them on an I/O thread.
class SseRegistry {
 public:
  // Keep the registry owner alive until source work has joined at shutdown.
  explicit SseRegistry(std::size_t limit = 64);
  ~SseRegistry();
  bool add(const std::shared_ptr<SseStream>& stream);
  void stop();
  boost::asio::thread_pool& poll_workers() { return poll_workers_; }

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<SseStream>> streams_;
  bool stopped_ = false;
  std::size_t limit_;
  boost::asio::thread_pool poll_workers_{2};
};

class SseStream : public std::enable_shared_from_this<SseStream> {
 public:
  using Source = std::function<EventBatch()>;
  static std::shared_ptr<SseStream> start(
      boost::asio::ip::tcp::socket& socket,
      std::shared_ptr<SseRegistry> registry = {},
      Source source = {}
  );
  // Reserve admission before committing work; activate only after setup succeeds.
  static std::shared_ptr<SseStream> reserve(
      boost::asio::ip::tcp::socket& socket,
      std::shared_ptr<SseRegistry> registry = {},
      Source source = {}
  );
  void activate();
  void release(boost::asio::ip::tcp::socket& socket);
  bool send(const StreamEvent& event);
  void finish();
  void cancel();
  bool open() const { return open_.load(); }

 private:
  SseStream(
      boost::asio::ip::tcp::socket socket,
      std::shared_ptr<SseRegistry> registry,
      Source source
  );
  void begin();
  bool enqueue(std::string frame);
  void write_next();
  void schedule_poll();
  void heartbeat();
  void close();

  boost::asio::ip::tcp::socket socket_;
  boost::asio::strand<boost::asio::any_io_executor> strand_;
  boost::asio::steady_timer poll_timer_;
  boost::asio::steady_timer heartbeat_timer_;
  boost::asio::steady_timer write_timer_;
  std::shared_ptr<SseRegistry> registry_;
  Source source_;
  boost::beast::http::response<boost::beast::http::empty_body> header_;
  boost::beast::http::response_serializer<boost::beast::http::empty_body> serializer_;
  std::deque<std::shared_ptr<std::string>> queue_;
  std::atomic<bool> open_{true};
  std::atomic<std::size_t> pending_bytes_{0};
  bool header_done_ = false;
  bool writing_ = false;
  bool finished_ = false;
  std::array<char, 1> read_buffer_{};
};

struct FinishSseStream {
  std::shared_ptr<SseStream> stream;
  ~FinishSseStream() {
    if (stream) stream->finish();
  }
};

} // namespace holder::api::support
