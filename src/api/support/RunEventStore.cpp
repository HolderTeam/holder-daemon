#include "api/support/RunEventStore.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace holder::api::support {
namespace {

std::mutex g_run_events_mu;
std::unordered_map<std::string, RunEventStream> g_run_events;

} // namespace

void append_run_event(
    const std::string& run_id,
    std::string name,
    nlohmann::json data,
    bool finished
) {
  std::lock_guard<std::mutex> lock(g_run_events_mu);
  auto& stream = g_run_events[run_id];
  data["run_id"] = run_id;
  stream.events.push_back({std::move(name), std::move(data)});
  if (stream.events.size() > 512) {
    stream.events.erase(
        stream.events.begin(),
        stream.events.begin() + static_cast<std::ptrdiff_t>(stream.events.size() - 512)
    );
  }
  stream.finished = stream.finished || finished;
  stream.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
  )
                          .count();
}

std::optional<RunEventStream> get_run_event_stream(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(g_run_events_mu);
  const auto it = g_run_events.find(run_id);
  if (it == g_run_events.end()) return std::nullopt;
  return it->second;
}

} // namespace holder::api::support
