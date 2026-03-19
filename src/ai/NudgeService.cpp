#include "ai/NudgeService.h"

#include "ai/AiNudgeRepo.h"
#include "card/CardFrontMatter.h"
#include "card/CardRepo.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"

#include <openssl/sha.h>

#include <git2.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
  for (const char ch : value) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::string trim_copy(const std::string& input) {
  std::size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  std::size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

NudgeService::NudgeService(holder::platform::Db& db,
                           holder::llm::LocalModelRunner* runner)
    : db_(db), runner_(runner) {}

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

std::string NudgeService::build_nudge_prompt(const NudgeCandidateInput& input,
                                             const std::string& deterministic_body) {
  std::ostringstream prompt;
  prompt << "Rewrite this app nudge for a local personal knowledge tool.\n";
  prompt << "Constraints:\n";
  prompt << "- Output exactly one short paragraph.\n";
  prompt << "- Keep it under 35 words.\n";
  prompt << "- Be concrete and helpful, not chatty.\n";
  prompt << "- Do not mention AI, models, or that this was rewritten.\n";
  prompt << "- Do not use markdown bullets.\n";
  prompt << "Candidate kind: " << input.kind << "\n";
  prompt << "Facts: " << input.facts.dump() << "\n";
  prompt << "Fallback wording: " << deterministic_body << "\n";
  prompt << "Return only the rewritten body text.";
  return prompt.str();
}

std::optional<std::string> NudgeService::pick_local_model_for_nudges() const {
  if (runner_ == nullptr) return std::nullopt;
  const auto status = runner_->status();
  if (!status.available || status.models.empty()) return std::nullopt;

  const holder::llm::LocalModel* best = nullptr;
  for (const auto& model : status.models) {
    if (best == nullptr) {
      best = &model;
      continue;
    }
    if (model.size > 0 && (best->size == 0 || model.size < best->size)) {
      best = &model;
    }
  }
  if (best == nullptr || best->name.empty()) return std::nullopt;
  return best->name;
}

std::string NudgeService::build_nudge_body_with_runner(const NudgeCandidateInput& input) const {
  const auto deterministic = build_nudge_body(input);
  const auto model = pick_local_model_for_nudges();
  if (!model.has_value()) return deterministic;

  std::string generated;
  std::string error;
  const auto prompt = build_nudge_prompt(input, deterministic);
  const bool ok = runner_->stream_generate(
      model.value(),
      prompt,
      "{}",
      [&](const std::string& chunk) { generated += chunk; },
      &error);
  if (!ok) return deterministic;

  const auto trimmed = trim_copy(generated);
  if (trimmed.empty()) return deterministic;
  return trimmed;
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

std::string NudgeService::short_content_fingerprint(const std::string& content) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(content.data()),
         content.size(),
         digest);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 6; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return out.str();
}

std::optional<std::string> NudgeService::current_card_fingerprint(holder::platform::Db& db,
                                                                  const std::string& project_id,
                                                                  const std::string& card_id) {
  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);
  const auto project = project_repo.get(project_id);
  const auto card = card_repo.get(card_id);
  if (!project.has_value() || !card.has_value()) {
    return std::nullopt;
  }

  const auto raw = read_file(std::filesystem::path(project->root_path) / card->rel_path);
  if (!raw.has_value()) {
    return std::nullopt;
  }

  std::string plain = raw.value();
  if (project->privacy_mode == "encrypted_git") {
    if (!project->project_key_id.has_value() || project->project_key_id->empty()) {
      return std::nullopt;
    }
    try {
      plain = holder::privacy::decrypt_project_blob(
          project->project_id, project->project_key_id.value(), plain);
    } catch (...) {
      return std::nullopt;
    }
  }

  return short_content_fingerprint(holder::core::parse_card_file(plain).body);
}

std::optional<std::string> NudgeService::current_project_head_commit(
    holder::platform::Db& db,
    const std::string& project_id) {
  holder::project::ProjectRepo project_repo(db);
  const auto project = project_repo.get(project_id);
  if (!project.has_value()) {
    return std::nullopt;
  }

  git_repository* repo = nullptr;
  if (git_repository_open(&repo, project->root_path.c_str()) != 0 || repo == nullptr) {
    return std::nullopt;
  }

  git_reference* head = nullptr;
  const int rc = git_repository_head(&head, repo);
  if (rc != 0 || head == nullptr) {
    git_repository_free(repo);
    return std::nullopt;
  }

  const git_oid* oid = git_reference_target(head);
  std::optional<std::string> out;
  if (oid != nullptr) {
    const char* text = git_oid_tostr_s(oid);
    if (text != nullptr && text[0] != '\0') {
      out = std::string(text);
    }
  }
  git_reference_free(head);
  git_repository_free(repo);
  return out;
}

bool NudgeService::is_stale(holder::platform::Db& db, const Nudge& nudge) {
  if (nudge.basis_fingerprint.has_value()) {
    if (!nudge.card_id.has_value()) return false;
    const auto current =
        current_card_fingerprint(db, nudge.project_id, nudge.card_id.value());
    if (current.has_value() && current.value() != nudge.basis_fingerprint.value()) {
      return true;
    }
  }

  if (nudge.basis_commit.has_value()) {
    const auto current = current_project_head_commit(db, nudge.project_id);
    if (current.has_value() && current.value() != nudge.basis_commit.value()) {
      return true;
    }
  }

  return false;
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
      .body = build_nudge_body_with_runner(input),
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
                                      const std::optional<std::string>& card_id) {
  AiNudgeRepo repo(db_);
  const auto active = repo.list_active(project_id, card_id);
  std::vector<Nudge> out;
  out.reserve(active.size());
  for (const auto& nudge : active) {
    if (is_stale(db_, nudge)) {
      repo.dismiss(nudge.nudge_id);
      continue;
    }
    out.push_back(nudge);
  }
  return out;
}

bool NudgeService::dismiss(const std::string& nudge_id) {
  return AiNudgeRepo(db_).dismiss(nudge_id);
}

} // namespace holder::ai
