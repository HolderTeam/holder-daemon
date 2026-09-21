#include "cli/commands/MilestoneDateTime.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace holder::cli {
namespace {

using namespace std::chrono;

struct CivilDate {
  int year = 0;
  unsigned int month = 0;
  unsigned int day = 0;
};

int digits(const std::string& value, std::size_t position, std::size_t count) {
  if (position + count > value.size()) throw std::invalid_argument("date/time is incomplete");
  int result = 0;
  for (std::size_t i = position; i < position + count; ++i) {
    if (value[i] < '0' || value[i] > '9') throw std::invalid_argument("expected a digit");
    result = result * 10 + (value[i] - '0');
  }
  return result;
}

year_month_day checked_ymd(const CivilDate& date) {
  const year_month_day value{year{date.year}, month{date.month}, day{date.day}};
  if (!value.ok()) throw std::invalid_argument("calendar date is invalid");
  return value;
}

bool date_shape(const std::string& value) {
  return value.size() == 10 && value[4] == '-' && value[7] == '-';
}

CivilDate parse_date(const std::string& value) {
  if (!date_shape(value)) throw std::invalid_argument("expected YYYY-MM-DD");
  CivilDate date{
      .year = digits(value, 0, 4),
      .month = static_cast<unsigned int>(digits(value, 5, 2)),
      .day = static_cast<unsigned int>(digits(value, 8, 2)),
  };
  (void)checked_ymd(date);
  return date;
}

std::tm local_time(long long epoch_seconds) {
  const auto value = static_cast<std::time_t>(epoch_seconds);
  if (static_cast<long long>(value) !=
      epoch_seconds) { // LCOV_EXCL_LINE: only reachable with a narrower platform time_t.
    throw std::invalid_argument("date/time is outside the platform time range"); // LCOV_EXCL_LINE
  }
  std::tm result{};
#ifdef _WIN32
  if (localtime_s(&result, &value) != 0) {
#else
  if (localtime_r(&value, &result) == nullptr) {
#endif
    throw std::invalid_argument("date/time is outside the platform time range");
  }
  return result;
}

long long local_midnight(const CivilDate& date) {
  std::tm local{};
  local.tm_year = date.year - 1900;
  local.tm_mon = static_cast<int>(date.month) - 1;
  local.tm_mday = static_cast<int>(date.day);
  local.tm_isdst = -1;
  const auto epoch = std::mktime(&local);
  // LCOV_EXCL_START: requires a platform mktime range failure for an already validated date.
  if (epoch == static_cast<std::time_t>(-1)) {
    throw std::invalid_argument("local calendar date is outside the platform time range");
  }
  // LCOV_EXCL_STOP
  const auto round_trip = local_time(static_cast<long long>(epoch));
  if (round_trip.tm_year != date.year - 1900 ||
      round_trip.tm_mon != static_cast<int>(date.month) - 1 ||
      round_trip.tm_mday != static_cast<int>(date.day) || round_trip.tm_hour != 0 ||
      round_trip.tm_min != 0 || round_trip.tm_sec != 0) {
    throw std::invalid_argument("local calendar date has no representable midnight");
  }
  return static_cast<long long>(epoch);
}

CivilDate next_date(const CivilDate& date) {
  const year_month_day next{sys_days{checked_ymd(date)} + days{1}};
  return {
      .year = static_cast<int>(next.year()),
      .month = static_cast<unsigned int>(next.month()),
      .day = static_cast<unsigned int>(next.day()),
  };
}

CivilDate local_date(long long epoch_seconds) {
  const auto local = local_time(epoch_seconds);
  return {
      .year = local.tm_year + 1900,
      .month = static_cast<unsigned int>(local.tm_mon + 1),
      .day = static_cast<unsigned int>(local.tm_mday),
  };
}

long long local_day_end(const CivilDate& date) {
  const auto next_midnight = local_midnight(next_date(date));
  // LCOV_EXCL_START: subtraction cannot overflow after a successful platform time_t conversion.
  if (next_midnight == std::numeric_limits<long long>::min()) {
    throw std::invalid_argument("local calendar date is outside the platform time range");
  }
  // LCOV_EXCL_STOP
  return next_midnight - 1;
}

ParsedMilestoneWhen parse_rfc3339(const std::string& value) {
  try {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':') {
      throw std::invalid_argument("invalid shape");
    }
    const CivilDate date{
        .year = digits(value, 0, 4),
        .month = static_cast<unsigned int>(digits(value, 5, 2)),
        .day = static_cast<unsigned int>(digits(value, 8, 2)),
    };
    const auto ymd = checked_ymd(date);
    const int hour = digits(value, 11, 2);
    const int minute = digits(value, 14, 2);
    const int second = digits(value, 17, 2);
    if (hour > 23 || minute > 59 || second > 59) {
      throw std::invalid_argument("clock time is invalid");
    }

    std::size_t position = 19;
    if (position < value.size() && value[position] == '.') {
      ++position;
      const auto fraction_start = position;
      while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
        ++position;
      }
      if (position == fraction_start) throw std::invalid_argument("fraction is empty");
    }

    long long offset = 0;
    if (position < value.size() && (value[position] == 'Z' || value[position] == 'z')) {
      ++position;
    } else {
      if (position + 6 != value.size() || (value[position] != '+' && value[position] != '-') ||
          value[position + 3] != ':') {
        throw std::invalid_argument("explicit UTC offset is required");
      }
      const int offset_hour = digits(value, position + 1, 2);
      const int offset_minute = digits(value, position + 4, 2);
      if (offset_hour > 23 || offset_minute > 59) {
        throw std::invalid_argument("UTC offset is invalid");
      }
      offset = static_cast<long long>(offset_hour * 60 + offset_minute) * 60;
      if (value[position] == '-') offset = -offset;
      position += 6;
    }
    if (position != value.size()) throw std::invalid_argument("trailing characters");

    const auto day_seconds = duration_cast<seconds>(sys_days{ymd}.time_since_epoch()).count();
    return {
        .epoch_seconds = day_seconds + hour * 3600LL + minute * 60LL + second - offset,
        .kind = MilestoneWhenKind::Timed,
    };
  } catch (const std::invalid_argument&) {
    throw std::invalid_argument(
        "WHEN must be YYYY-MM-DD or an RFC 3339 timestamp with an explicit offset."
    );
  }
}

long long parse_calendar_bound(const std::string& value, bool end_bound) {
  if (date_shape(value)) {
    const auto date = parse_date(value);
    return end_bound ? local_day_end(date) : local_midnight(date);
  }
  return parse_rfc3339(value).epoch_seconds;
}

} // namespace

ParsedMilestoneWhen parse_milestone_when(const std::string& value) {
  if (date_shape(value)) {
    return {
        .epoch_seconds = local_midnight(parse_date(value)),
        .kind = MilestoneWhenKind::AllDay,
    };
  }
  return parse_rfc3339(value);
}

CalendarRange resolve_calendar_range(
    const std::optional<std::string>& from,
    const std::optional<std::string>& to,
    long long now_epoch_seconds
) {
  CalendarRange result;
  if (!from.has_value() && !to.has_value()) {
    const auto today = local_date(now_epoch_seconds);
    result.from = local_midnight(today);
    result.to = local_day_end(today);
  } else if (from.has_value() && !to.has_value()) {
    result.from = parse_calendar_bound(*from, false);
    result.to = local_day_end(local_date(result.from));
  } else if (!from.has_value() && to.has_value()) {
    result.to = parse_calendar_bound(*to, true);
    result.from = local_midnight(local_date(result.to));
  } else {
    result.from = parse_calendar_bound(*from, false);
    result.to = parse_calendar_bound(*to, true);
  }
  if (result.from > result.to) {
    throw std::invalid_argument("calendar --from must not be after --to.");
  }
  return result;
}

std::string format_milestone_when(long long epoch_seconds, bool all_day) {
  const auto local = local_time(epoch_seconds);
  std::ostringstream out;
  if (all_day) {
    out << std::put_time(&local, "%Y-%m-%d");
    return out.str();
  }

  out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
  const CivilDate date{
      .year = local.tm_year + 1900,
      .month = static_cast<unsigned int>(local.tm_mon + 1),
      .day = static_cast<unsigned int>(local.tm_mday),
  };
  const auto civil_seconds =
      duration_cast<seconds>(sys_days{checked_ymd(date)}.time_since_epoch()).count() +
      local.tm_hour * 3600LL + local.tm_min * 60LL + local.tm_sec;
  const auto offset_seconds = civil_seconds - epoch_seconds;
  const auto absolute_offset = std::llabs(offset_seconds);
  out << (offset_seconds < 0 ? '-' : '+') << std::setfill('0') << std::setw(2)
      << absolute_offset / 3600 << ':' << std::setw(2) << (absolute_offset / 60) % 60;
  return out.str();
}

} // namespace holder::cli
