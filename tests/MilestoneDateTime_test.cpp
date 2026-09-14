#include "cli/commands/MilestoneDateTime.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <cstdlib>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

class TimeZoneGuard {
 public:
  explicit TimeZoneGuard(const std::string& value) {
    if (const char* current = std::getenv("TZ")) old_ = current;
    set(value);
  }

  ~TimeZoneGuard() {
    if (old_.has_value()) {
      set(*old_);
    } else {
#ifdef _WIN32
      _putenv_s("TZ", "");
      _tzset();
#else
      unsetenv("TZ");
      tzset();
#endif
    }
  }

 private:
  static void set(const std::string& value) {
#ifdef _WIN32
    _putenv_s("TZ", value.c_str());
    _tzset();
#else
    setenv("TZ", value.c_str(), 1);
    tzset();
#endif
  }

  std::optional<std::string> old_;
};

} // namespace

TEST_CASE(
    "milestone times require unambiguous dates or explicitly offset timestamps",
    "[holderctl][milestones][datetime]"
) {
  const auto utc = holder::cli::parse_milestone_when("2026-06-01T10:00:45Z");
  const auto offset = holder::cli::parse_milestone_when("2026-06-01T12:30:45+02:30");
  const auto fractional = holder::cli::parse_milestone_when("2026-06-01T10:00:45.987z");

  CHECK(utc.kind == holder::cli::MilestoneWhenKind::Timed);
  CHECK(offset.epoch_seconds == utc.epoch_seconds);
  CHECK(fractional.epoch_seconds == utc.epoch_seconds);

  CHECK_THROWS_AS(holder::cli::parse_milestone_when("2026-06-01T10:00:45"), std::invalid_argument);
  CHECK_THROWS_AS(holder::cli::parse_milestone_when("06/01/2026"), std::invalid_argument);
  CHECK_THROWS_AS(holder::cli::parse_milestone_when("2026-02-29"), std::invalid_argument);
  CHECK_THROWS_AS(holder::cli::parse_milestone_when("2026-06-01T25:00:00Z"), std::invalid_argument);
}

TEST_CASE(
    "date-only milestone and calendar values use inclusive local calendar days",
    "[holderctl][milestones][datetime]"
) {
  TimeZoneGuard timezone("UTC0");
  const auto date = holder::cli::parse_milestone_when("2026-06-01");
  const auto midnight = holder::cli::parse_milestone_when("2026-06-01T00:00:00Z");
  const auto noon = holder::cli::parse_milestone_when("2026-06-01T12:00:00Z");

  CHECK(date.kind == holder::cli::MilestoneWhenKind::AllDay);
  CHECK(date.epoch_seconds == midnight.epoch_seconds);
  CHECK(holder::cli::format_milestone_when(date.epoch_seconds, true) == "2026-06-01");

  const auto explicit_range = holder::cli::resolve_calendar_range(
      std::string("2026-06-01"),
      std::string("2026-06-01"),
      noon.epoch_seconds
  );
  CHECK(explicit_range.from == midnight.epoch_seconds);
  CHECK(explicit_range.to - explicit_range.from + 1 == 24 * 60 * 60);

  const auto default_range =
      holder::cli::resolve_calendar_range(std::nullopt, std::nullopt, noon.epoch_seconds);
  CHECK(default_range.from == explicit_range.from);
  CHECK(default_range.to == explicit_range.to);

  const auto from_only = holder::cli::resolve_calendar_range(
      std::string("2026-06-01T12:00:00Z"),
      std::nullopt,
      noon.epoch_seconds
  );
  CHECK(from_only.from == noon.epoch_seconds);
  CHECK(from_only.to == explicit_range.to);

  const auto to_only = holder::cli::resolve_calendar_range(
      std::nullopt,
      std::string("2026-06-01T12:00:00Z"),
      noon.epoch_seconds
  );
  CHECK(to_only.from == explicit_range.from);
  CHECK(to_only.to == noon.epoch_seconds);

  CHECK_THROWS_AS(
      holder::cli::resolve_calendar_range(
          std::string("2026-06-02"),
          std::string("2026-06-01"),
          noon.epoch_seconds
      ),
      std::invalid_argument
  );
}

#ifndef _WIN32
TEST_CASE(
    "calendar date bounds follow daylight-saving day length",
    "[holderctl][milestones][datetime]"
) {
  TimeZoneGuard timezone("Europe/London");
  const auto now = holder::cli::parse_milestone_when("2026-01-01T00:00:00Z").epoch_seconds;

  const auto spring = holder::cli::resolve_calendar_range(
      std::string("2026-03-29"),
      std::string("2026-03-29"),
      now
  );
  CHECK(spring.to - spring.from + 1 == 23 * 60 * 60);

  const auto autumn = holder::cli::resolve_calendar_range(
      std::string("2026-10-25"),
      std::string("2026-10-25"),
      now
  );
  CHECK(autumn.to - autumn.from + 1 == 25 * 60 * 60);
}
#endif
