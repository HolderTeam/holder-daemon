#pragma once

#include "platform/Db.h"

#include <cstdint>
#include <nlohmann/json.hpp>

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
                          const std::optional<std::string>& card_id = std::nullopt) const;

  bool dismiss(const std::string& nudge_id);

private:
  holder::platform::Db& db_;

  static bool is_placeholder_title(const std::string& title);
  static bool is_successful_push_status(const std::string& status);

  static NudgeDecision evaluate_candidate(const NudgeCandidateInput& input);
  static std::string build_nudge_title(const NudgeCandidateInput& input);
  static std::string build_nudge_body(const NudgeCandidateInput& input);
  static std::string build_nudge_id(const NudgeCandidateInput& input);
};

} // namespace holder::ai
