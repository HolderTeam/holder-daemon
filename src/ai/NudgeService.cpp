#include "ai/NudgeService.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace holder::ai {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

} // namespace

bool NudgeService::is_placeholder_title(const std::string& title) {
  return lower_copy(title).rfind("untitled", 0) == 0;
}

bool NudgeService::is_successful_push_status(const std::string& status) {
  return status == "pushed" || status == "up_to_date";
}

NudgeDecision NudgeService::evaluate_candidate(const NudgeCandidateInput& input) {
  if (input.kind == "card.title_only") {
    const auto title = input.facts.value("title", "");
    const auto body_empty = input.facts.value("body_empty", false);
    const auto doc_chars = input.facts.value("doc_chars", 0);
    const auto should_nudge =
        body_empty && !title.empty() && !is_placeholder_title(title) && doc_chars <= 160;
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "title_only_candidate_ready" : "title_only_not_actionable"};
  }
  if (input.kind == "card.stuck_drafting") {
    const auto autosave_count = input.facts.value("autosave_count", 0);
    const auto body_chars = input.facts.value("body_chars", 0);
    const auto should_nudge = autosave_count >= 3 && body_chars > 0 && body_chars <= 160;
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "stuck_drafting_candidate_ready"
                                   : "stuck_drafting_not_actionable"};
  }
  if (input.kind == "git.push_failed_repeated") {
    const auto failure_count = input.facts.value("failure_count", 0);
    const auto latest_status = input.facts.value("latest_status", "");
    const auto should_nudge = failure_count >= 2 && !is_successful_push_status(latest_status);
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "git_push_failure_candidate_ready"
                                   : "git_push_failure_not_actionable"};
  }
  return {.accepted = false, .should_nudge = false, .reason = "unknown_candidate_kind"};
}

std::string NudgeService::build_nudge_title(const NudgeCandidateInput& input) {
  if (input.kind == "card.title_only") {
    return "Start this card";
  }
  if (input.kind == "card.stuck_drafting") {
    return "Unstick this draft";
  }
  if (input.kind == "git.push_failed_repeated") {
    return "Fix git push";
  }
  return "Suggestion";
}

std::string NudgeService::build_nudge_body(const NudgeCandidateInput& input) {
  if (input.kind == "card.title_only") {
    const auto title = input.facts.value("title", "");
    std::ostringstream body;
    body << "You named this card \"" << title
         << "\" but it still only has a title. Draft an opening paragraph or a short outline next.";
    return body.str();
  }
  if (input.kind == "card.stuck_drafting") {
    const auto title = input.facts.value("title", "");
    std::ostringstream body;
    body << "You have been editing \"" << title
         << "\" for a while without getting far. Try writing three bullets first, then expand one.";
    return body.str();
  }
  if (input.kind == "git.push_failed_repeated") {
    const auto latest_status = input.facts.value("latest_status", "");
    std::ostringstream body;
    body << "Git push is still failing (" << latest_status
         << "). Check auth or remote setup before trying again.";
    return body.str();
  }
  return "No suggestion available.";
}

std::optional<Nudge> NudgeService::find_active_exact_match(const NudgeCandidateInput& input) const {
  for (const auto& nudge : nudges_) {
    if (nudge.dismissed) {
      continue;
    }
    if (nudge.kind != input.kind || nudge.project_id != input.project_id ||
        nudge.card_id != input.card_id || nudge.basis_fingerprint != input.basis_fingerprint ||
        nudge.basis_commit != input.basis_commit) {
      continue;
    }
    return nudge;
  }
  return std::nullopt;
}

void NudgeService::dismiss_stale_variants(const NudgeCandidateInput& input) {
  for (auto& nudge : nudges_) {
    if (nudge.dismissed) {
      continue;
    }
    if (nudge.kind != input.kind || nudge.project_id != input.project_id || nudge.card_id != input.card_id) {
      continue;
    }
    if (nudge.basis_fingerprint == input.basis_fingerprint && nudge.basis_commit == input.basis_commit) {
      continue;
    }
    nudge.dismissed = true;
  }
}

NudgeDecision NudgeService::evaluate_and_record(const NudgeCandidateInput& input) {
  auto decision = evaluate_candidate(input);
  if (!decision.accepted || !decision.should_nudge) {
    return decision;
  }

  if (const auto existing = find_active_exact_match(input); existing.has_value()) {
    decision.nudge = existing;
    return decision;
  }

  dismiss_stale_variants(input);

  Nudge nudge{
      .nudge_id = "nudge-" + std::to_string(next_id_++),
      .kind = input.kind,
      .project_id = input.project_id,
      .card_id = input.card_id,
      .title = build_nudge_title(input),
      .body = build_nudge_body(input),
      .basis_fingerprint = input.basis_fingerprint,
      .basis_commit = input.basis_commit,
      .created_at = input.created_at,
      .dismissed = false,
  };
  nudges_.push_back(nudge);
  decision.nudge = nudge;
  return decision;
}

std::vector<Nudge> NudgeService::list(const std::string& project_id,
                                      const std::optional<std::string>& card_id) const {
  std::vector<Nudge> out;
  for (const auto& nudge : nudges_) {
    if (nudge.dismissed || nudge.project_id != project_id) {
      continue;
    }
    if (!card_id.has_value()) {
      if (nudge.card_id.has_value()) {
        continue;
      }
      out.push_back(nudge);
      continue;
    }
    if (!nudge.card_id.has_value() || nudge.card_id == card_id) {
      out.push_back(nudge);
    }
  }
  return out;
}

bool NudgeService::dismiss(const std::string& nudge_id) {
  for (auto& nudge : nudges_) {
    if (nudge.nudge_id == nudge_id && !nudge.dismissed) {
      nudge.dismissed = true;
      return true;
    }
  }
  return false;
}

} // namespace holder::ai
