#pragma once

#include "llm/RunnerModelRef.h"
#include "model/Card.h"
#include "platform/Db.h"
#include "llm/RunnerRegistry.h"

#include <cstdint>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

struct NudgeServiceTestAccess;

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
  explicit NudgeService(holder::platform::Db& db,
                        holder::llm::RunnerRegistry* runner_registry = nullptr);

  NudgeDecision evaluate_and_record(const NudgeCandidateInput& input);

  std::vector<Nudge> list(const std::string& project_id,
                          const std::optional<std::string>& card_id = std::nullopt);

  bool dismiss(const std::string& nudge_id);

private:
  friend struct NudgeServiceTestAccess;

  holder::platform::Db& db_;
  holder::llm::RunnerRegistry* runner_registry_ = nullptr;

  static bool is_placeholder_title(const std::string& title);
  static bool is_successful_push_status(const std::string& status);

  static NudgeDecision evaluate_candidate(const NudgeCandidateInput& input);
  static std::string build_nudge_title(const NudgeCandidateInput& input);
  static std::string build_nudge_body(const NudgeCandidateInput& input);
  std::string build_nudge_body_with_runner(const NudgeCandidateInput& input) const;
  std::optional<holder::llm::ResolvedRunnerModel> pick_local_model_for_nudges() const;
  static std::string build_nudge_prompt(const NudgeCandidateInput& input,
                                        const std::string& deterministic_body,
                                        const std::string& context_summary);
  static std::string build_nudge_id(const NudgeCandidateInput& input);
  static std::string short_content_fingerprint(const std::string& content);
  static std::optional<std::string> access_load_card_body(holder::platform::Db& db,
                                                          const std::string& project_id,
                                                          const std::string& card_id);
  static std::vector<std::string> access_sibling_card_titles(holder::platform::Db& db,
                                                             const std::string& project_id,
                                                             const std::string& card_id);
  static std::vector<holder::model::Card> access_sibling_cards(holder::platform::Db& db,
                                                               const std::string& project_id,
                                                               const std::string& card_id);
  static std::string access_card_excerpt_line(holder::platform::Db& db,
                                              const std::string& project_id,
                                              const holder::model::Card& card);
  static std::vector<std::string> access_recent_project_card_excerpts(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::optional<std::string>& exclude_card_id,
      std::size_t limit);
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
