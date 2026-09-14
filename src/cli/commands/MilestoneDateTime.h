#pragma once

#include <optional>
#include <string>

namespace holder::cli {

enum class MilestoneWhenKind {
  Timed,
  AllDay,
};

struct ParsedMilestoneWhen {
  long long epoch_seconds = 0;
  MilestoneWhenKind kind = MilestoneWhenKind::Timed;
};

struct CalendarRange {
  long long from = 0;
  long long to = 0;
};

// A date-only value is an all-day local calendar date encoded as local
// midnight. A timed value must be RFC 3339 with an explicit Z or +/-HH:MM
// offset. Fractional seconds are accepted and truncated to epoch seconds.
ParsedMilestoneWhen parse_milestone_when(const std::string& value);

// Calendar ranges are inclusive. Date-only from/to values expand to the first
// and final second of their local day; the final second is derived from the
// next local midnight so daylight-saving transitions remain correct. Missing
// bounds use the local day containing the supplied bound, or today's local day
// when both are missing.
CalendarRange resolve_calendar_range(
    const std::optional<std::string>& from,
    const std::optional<std::string>& to,
    long long now_epoch_seconds
);

// Human presentation uses the current local timezone. All-day values render
// as YYYY-MM-DD; timed values render as RFC 3339 with the local UTC offset.
std::string format_milestone_when(long long epoch_seconds, bool all_day);

} // namespace holder::cli
