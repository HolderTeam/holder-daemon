#pragma once

#include "platform/Db.h"

#include <cstdint>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

struct NudgeCandidateInput {
  std::string kind;
  std::string project_id;
  std::optional<std::string> card_id;
  std::int64_t created_at = 0;
  std::optional<std::string> basis_fingerprint;
  std::optional<std::string> basis_commit;
  nlohmann::json facts;
};

struct Nudge {
  std::string nudge_id;
  std::string kind;
  std::string project_id;
  std::optional<std::string> card_id;
  std::string title;
  std::string body;
  std::optional<std::string> basis_fingerprint;
  std::optional<std::string> basis_commit;
  std::int64_t created_at = 0;
  bool dismissed = false;
};

struct NudgeDecision {
  bool accepted{false};
  bool should_nudge{false};
  std::string reason;
  std::optional<Nudge> nudge;
};

class NudgeService {
public:
  explicit NudgeService(holder::platform::Db& db);

  NudgeDecision evaluate_and_record(const NudgeCandidateInput& input);

  std::vector<Nudge> list(const std::string& project_id,
                          const std::optional<std::string>& card_id = std::nullopt);

  bool dismiss(const std::string& nudge_id);

private:
  holder::platform::Db& db_;

  static bool is_placeholder_title(const std::string& title);
  static bool is_successful_push_status(const std::string& status);

  static NudgeDecision evaluate_candidate(const NudgeCandidateInput& input);
  static std::string build_nudge_title(const NudgeCandidateInput& input);
  static std::string build_nudge_body(const NudgeCandidateInput& input);
  static std::string build_nudge_id(const NudgeCandidateInput& input);
  static std::string short_content_fingerprint(const std::string& content);
  static std::optional<std::string> current_card_fingerprint(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::string& card_id);
  static std::optional<std::string> current_project_head_commit(
      holder::platform::Db& db,
      const std::string& project_id);
  static bool is_stale(holder::platform::Db& db, const Nudge& nudge);
};

} // namespace holder::ai
