#include "api/support/RunEventStore.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace holder::api::support {
namespace {

struct StoredRun {
  std::shared_ptr<EventJournal> journal;
  long long updated_at = 0;
};
std::mutex g_run_events_mu;
std::unordered_map<std::string, StoredRun> g_run_events;
constexpr std::size_t kMaxRetainedRuns = 128;

} // namespace

StreamEvent append_run_event(
    const std::string& run_id,
    std::string name,
    nlohmann::json data,
    bool finished
) {
  std::lock_guard lock(g_run_events_mu);
  if (!g_run_events.contains(run_id) && g_run_events.size() >= kMaxRetainedRuns) {
    auto oldest = g_run_events.begin();
    for (auto it = g_run_events.begin(); it != g_run_events.end(); ++it) {
      if (it->second.updated_at < oldest->second.updated_at) oldest = it;
    }
    g_run_events.erase(oldest);
  }
  auto& stream = g_run_events[run_id];
  if (!stream.journal || name == "run_started") stream.journal = std::make_shared<EventJournal>();
  stream.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
  )
                          .count();
  data["run_id"] = run_id;
  return stream.journal->append(std::move(name), std::move(data), finished);
}

std::optional<EventBatch> read_run_events(const std::string& run_id, const std::string& after) {
  std::lock_guard lock(g_run_events_mu);
  const auto it = g_run_events.find(run_id);
  if (it == g_run_events.end()) return std::nullopt;
  return it->second.journal->read(after);
}

std::optional<RunEventStream> get_run_event_stream(const std::string& run_id) {
  std::lock_guard lock(g_run_events_mu);
  const auto it = g_run_events.find(run_id);
  if (it == g_run_events.end()) return std::nullopt;
  const auto batch = it->second.journal->read();
  RunEventStream stream;
  stream.finished = batch.finished;
  stream.updated_at = it->second.updated_at;
  for (const auto& event : batch.events)
    stream.events.push_back({event.name, event.data, event.id});
  return stream;
}

} // namespace holder::api::support
