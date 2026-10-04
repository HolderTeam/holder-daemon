#include "api/support/EventJournal.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <charconv>
#include <stdexcept>
#include <string_view>

namespace holder::api::support {
namespace {

bool parse_cursor(const std::string& cursor, std::string_view& epoch, std::uint64_t& sequence) {
  const auto separator = cursor.find(':');
  if (separator != 36 || cursor.size() <= separator + 1) return false;
  epoch = std::string_view(cursor).substr(0, separator);
  for (std::size_t i = 0; i < epoch.size(); ++i) {
    const char ch = epoch[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (ch != '-') return false;
    } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  const auto* begin = cursor.data() + separator + 1;
  const auto* end = cursor.data() + cursor.size();
  const auto parsed = std::from_chars(begin, end, sequence);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

} // namespace

EventJournal::EventJournal(std::size_t capacity, std::size_t byte_limit)
    : epoch_(boost::uuids::to_string(boost::uuids::random_generator()())),
      capacity_(capacity),
      byte_limit_(byte_limit) {
  if (!capacity_ || !byte_limit_) throw std::invalid_argument("Empty event journal capacity");
}

StreamEvent EventJournal::append(std::string name, nlohmann::json data, bool finished) {
  std::lock_guard lock(mutex_);
  StreamEvent event{epoch_ + ":" + std::to_string(++sequence_), std::move(name), std::move(data)};
  const auto size = encode_sse(event).size();
  events_.push_back(event);
  sizes_.push_back(size);
  bytes_ += size;
  finished_ = finished_ || finished;
  while (events_.size() > capacity_ || bytes_ > byte_limit_) {
    bytes_ -= sizes_.front();
    sizes_.pop_front();
    events_.pop_front();
    ++oldest_sequence_;
  }
  return event;
}

EventBatch EventJournal::read(const std::string& after) const {
  std::lock_guard lock(mutex_);
  EventBatch batch;
  batch.cursor = epoch_ + ":" + std::to_string(sequence_);
  batch.finished = finished_;
  batch.truncated = oldest_sequence_ > 1;
  std::uint64_t sequence = oldest_sequence_ - 1;
  if (!after.empty()) {
    std::string_view epoch;
    if (!parse_cursor(after, epoch, sequence) || epoch != epoch_ || sequence > sequence_ ||
        sequence < oldest_sequence_ - 1) {
      batch.resync_required = true;
      return batch;
    }
  }
  const auto offset = static_cast<std::size_t>(sequence - (oldest_sequence_ - 1));
  batch.events.assign(events_.begin() + static_cast<std::ptrdiff_t>(offset), events_.end());
  return batch;
}

std::string EventJournal::cursor() const {
  std::lock_guard lock(mutex_);
  return epoch_ + ":" + std::to_string(sequence_);
}

bool EventJournal::valid_cursor(const std::string& cursor) {
  std::string_view epoch;
  std::uint64_t sequence = 0;
  return parse_cursor(cursor, epoch, sequence);
}

std::string encode_sse(const StreamEvent& event) {
  // JSON dump escapes embedded newlines; names and cursors are daemon-generated.
  return "id: " + event.id + "\nevent: " + event.name + "\ndata: " + event.data.dump() + "\n\n";
}

} // namespace holder::api::support
