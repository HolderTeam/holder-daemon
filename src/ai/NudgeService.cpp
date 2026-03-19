#include "ai/NudgeService.h"

#include "ai/AiNudgeRepo.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace holder::ai {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string fnv1a_hex(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : value) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

} // namespace

NudgeService::NudgeService(holder::platform::Db& db) : db_(db) {}

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
            .reason = should_nudge ? "title_only_candidate_ready" : "title_only_not_actionable",
            .nudge = std::nullopt};
  }
  if (input.kind == "card.stuck_drafting") {
    const auto autosave_count = input.facts.value("autosave_count", 0);
    const auto body_chars = input.facts.value("body_chars", 0);
    const auto should_nudge = autosave_count >= 3 && body_chars > 0 && body_chars <= 160;
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "stuck_drafting_candidate_ready"
                                   : "stuck_drafting_not_actionable",
            .nudge = std::nullopt};
  }
  if (input.kind == "git.push_failed_repeated") {
    const auto failure_count = input.facts.value("failure_count", 0);
    const auto latest_status = input.facts.value("latest_status", "");
    const auto should_nudge = failure_count >= 2 && !is_successful_push_status(latest_status);
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "git_push_failure_candidate_ready"
                                   : "git_push_failure_not_actionable",
            .nudge = std::nullopt};
  }
  return {.accepted = false,
          .should_nudge = false,
          .reason = "unknown_candidate_kind",
          .nudge = std::nullopt};
}

std::string NudgeService::build_nudge_title(const NudgeCandidateInput& input) {
  if (input.kind == "card.title_only") return "Start this card";
  if (input.kind == "card.stuck_drafting") return "Unstick this draft";
  if (input.kind == "git.push_failed_repeated") return "Fix git push";
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

std::string NudgeService::build_nudge_id(const NudgeCandidateInput& input) {
  std::ostringstream key;
  key << input.kind << '\n' << input.project_id << '\n';
  if (input.card_id.has_value()) key << input.card_id.value();
  key << '\n';
  if (input.basis_fingerprint.has_value()) key << input.basis_fingerprint.value();
  key << '\n';
  if (input.basis_commit.has_value()) key << input.basis_commit.value();
  return "nudge-" + fnv1a_hex(key.str());
}

NudgeDecision NudgeService::evaluate_and_record(const NudgeCandidateInput& input) {
  auto decision = evaluate_candidate(input);
  if (!decision.accepted || !decision.should_nudge) {
    return decision;
  }

  AiNudgeRepo repo(db_);
  if (const auto existing =
          repo.find_active_exact_match(input.kind,
                                       input.project_id,
                                       input.card_id,
                                       input.basis_fingerprint,
                                       input.basis_commit);
      existing.has_value()) {
    decision.nudge = existing;
    return decision;
  }

  repo.dismiss_stale_variants(
      input.kind, input.project_id, input.card_id, input.basis_fingerprint, input.basis_commit);

  Nudge nudge{
      .nudge_id = build_nudge_id(input),
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
  repo.create(nudge);
  decision.nudge = nudge;
  return decision;
}

std::vector<Nudge> NudgeService::list(const std::string& project_id,
                                      const std::optional<std::string>& card_id) const {
  return AiNudgeRepo(db_).list_active(project_id, card_id);
}

bool NudgeService::dismiss(const std::string& nudge_id) {
  return AiNudgeRepo(db_).dismiss(nudge_id);
}

} // namespace holder::ai
