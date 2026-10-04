#include "api/support/SseStream.h"

#include <algorithm>
#include <chrono>

namespace holder::api::support {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t kMaxPendingBytes = 2 * 1024 * 1024;
} // namespace

SseRegistry::SseRegistry(std::size_t limit)
    : limit_(limit) {}
SseRegistry::~SseRegistry() {
  stop();
  poll_workers_.join();
}

bool SseRegistry::add(const std::shared_ptr<SseStream>& stream) {
  std::lock_guard lock(mutex_);
  std::erase_if(streams_, [](const auto& weak) {
    const auto locked = weak.lock();
    return !locked || !locked->open();
  });
  if (stopped_ || streams_.size() >= limit_) return false;
  streams_.push_back(stream);
  return true;
}

void SseRegistry::stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  for (const auto& weak : streams_)
    if (auto stream = weak.lock()) stream->cancel();
}

SseStream::SseStream(
    boost::asio::ip::tcp::socket socket,
    std::shared_ptr<SseRegistry> registry,
    Source source
)
    : socket_(std::move(socket)),
      strand_(boost::asio::make_strand(socket_.get_executor())),
      poll_timer_(strand_),
      heartbeat_timer_(strand_),
      write_timer_(strand_),
      registry_(std::move(registry)),
      source_(std::move(source)),
      header_(boost::beast::http::status::ok, 11),
      serializer_(header_) {
  header_.set(boost::beast::http::field::content_type, "text/event-stream");
  header_.set(boost::beast::http::field::cache_control, "no-cache");
  header_.keep_alive(false); // EOF delimits the body; this socket is never reused.
}

std::shared_ptr<SseStream> SseStream::start(
    boost::asio::ip::tcp::socket& socket,
    std::shared_ptr<SseRegistry> registry,
    Source source
) {
  auto stream = reserve(socket, std::move(registry), std::move(source));
  if (stream) stream->activate();
  return stream;
}

std::shared_ptr<SseStream> SseStream::reserve(
    boost::asio::ip::tcp::socket& socket,
    std::shared_ptr<SseRegistry> registry,
    Source source
) {
  if (!registry) {
    // Standalone route callers share an owner that outlives source tasks. A
    // per-stream pool could otherwise be destroyed on its own worker thread.
    static const auto standalone_registry = std::make_shared<SseRegistry>();
    registry = standalone_registry;
  }
  auto stream = std::shared_ptr<SseStream>(
      new SseStream(std::move(socket), registry, std::move(source))
  );
  if (!registry->add(stream)) {
    socket = std::move(stream->socket_);
    return {};
  }
  return stream;
}

void SseStream::activate() {
  const auto self = shared_from_this();
  boost::asio::post(strand_, [self] {
    self->begin();
  });
}

void SseStream::release(boost::asio::ip::tcp::socket& socket) {
  // Only valid before activate: no asynchronous operation owns the socket yet.
  open_ = false;
  socket = std::move(socket_);
}

void SseStream::begin() {
  if (!open()) {
    close();
    return;
  }
  const auto self = shared_from_this();
  write_timer_.expires_after(10s);
  write_timer_.async_wait([self](auto ec) {
    if (!ec) self->close();
  });
  boost::beast::http::async_write_header(
      socket_,
      serializer_,
      boost::asio::bind_executor(
          strand_,
          [self](auto ec, std::size_t) {
            self->write_timer_.cancel();
            if (ec || !self->open()) {
              self->close();
              return;
            }
            self->header_done_ = true;
            self->write_next();
            self->schedule_poll();
            self->heartbeat();
          }
      )
  );
  // Detect disconnects even when the journal is idle. No further request data
  // is valid on a dedicated SSE connection.
  socket_.async_read_some(
      boost::asio::buffer(read_buffer_),
      boost::asio::bind_executor(
          strand_,
          [self](auto, std::size_t) {
            self->close();
          }
      )
  );
}

bool SseStream::send(const StreamEvent& event) { return enqueue(encode_sse(event)); }

bool SseStream::enqueue(std::string frame) {
  if (!open()) return false;
  const auto size = frame.size();
  if (pending_bytes_.fetch_add(size) + size > kMaxPendingBytes) {
    pending_bytes_.fetch_sub(size);
    cancel();
    return false;
  }
  const auto self = shared_from_this();
  boost::asio::post(strand_, [self, frame = std::make_shared<std::string>(std::move(frame))] {
    if (!self->open()) {
      self->pending_bytes_.fetch_sub(frame->size());
      return;
    }
    self->queue_.push_back(frame);
    self->write_next();
  });
  return true;
}

void SseStream::finish() {
  const auto self = shared_from_this();
  boost::asio::post(strand_, [self] {
    self->finished_ = true;
    self->write_next();
  });
}

void SseStream::cancel() {
  open_.store(false);
  const auto self = shared_from_this();
  boost::asio::post(strand_, [self] {
    self->close();
  });
}

void SseStream::close() {
  open_.store(false);
  poll_timer_.cancel();
  heartbeat_timer_.cancel();
  write_timer_.cancel();
  boost::system::error_code ec;
  socket_.cancel(ec);
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
  socket_.close(ec);
  // Pending async writes still own their buffers until their callbacks complete.
}

void SseStream::write_next() {
  if (!open() || !header_done_ || writing_) return;
  if (queue_.empty()) {
    if (finished_) close();
    return;
  }
  writing_ = true;
  const auto frame = queue_.front();
  const auto self = shared_from_this();
  write_timer_.expires_after(10s);
  write_timer_.async_wait([self](auto ec) {
    if (!ec) self->close();
  });
  boost::asio::async_write(
      socket_,
      boost::asio::buffer(*frame),
      boost::asio::bind_executor(
          strand_,
          [self, frame](auto ec, std::size_t) {
            self->write_timer_.cancel();
            self->writing_ = false;
            self->pending_bytes_.fetch_sub(frame->size());
            self->queue_.pop_front();
            if (ec) {
              self->close();
              return;
            }
            self->write_next();
          }
      )
  );
}

void SseStream::schedule_poll() {
  if (!source_ || !open() || finished_) return;
  const auto self = shared_from_this();
  // Keep run() alive until an off-thread source has posted its result, even
  // when socket cancellation drains the last pending I/O operation first.
  auto work = boost::asio::make_work_guard(socket_.get_executor());
  boost::asio::post(registry_->poll_workers(), [self, work = std::move(work)] {
    if (!self->open()) return;
    EventBatch batch;
    try {
      batch = self->source_();
    } catch (...) {
      self->cancel();
      return;
    }
    boost::asio::post(self->strand_, [self, batch = std::move(batch)] {
      if (!self->open()) return;
      if (batch.resync_required) {
        self->send({batch.cursor, "resync_required", {{"reason", "history_unavailable"}}});
        self->finish();
        return;
      }
      for (const auto& event : batch.events)
        if (!self->send(event)) return;
      if (batch.finished) {
        self->finish();
        return;
      }
      self->poll_timer_.expires_after(200ms);
      self->poll_timer_.async_wait([self](auto ec) {
        if (!ec) self->schedule_poll();
      });
    });
  });
}

void SseStream::heartbeat() {
  if (!open() || finished_) return;
  const auto self = shared_from_this();
  heartbeat_timer_.expires_after(15s);
  heartbeat_timer_.async_wait([self](auto ec) {
    if (ec || !self->open()) return;
    self->enqueue(": keepalive\n\n");
    self->heartbeat();
  });
}

} // namespace holder::api::support
