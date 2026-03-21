#include "ai/NudgeService.h"

#include "ai/AiNudgeRepo.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
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

std::string truncate_for_prompt(const std::string& text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text;
  }
  return text.substr(0, max_bytes);
}

std::string join_titles(const std::vector<std::string>& titles) {
  std::ostringstream out;
  bool first = true;
  for (const auto& title : titles) {
    if (!first) {
      out << "; ";
    }
    out << title;
    first = false;
  }
  return out.str();
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::optional<std::string> load_card_body(holder::platform::Db& db,
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

  return holder::core::parse_card_file(plain).body;
}

std::vector<std::string> sibling_card_titles(holder::platform::Db& db,
                                             const std::string& project_id,
                                             const std::string& card_id) {
  holder::card::CardRepo card_repo(db);
  const auto card = card_repo.get(card_id);
  if (!card.has_value()) {
    return {};
  }

  std::vector<holder::model::Card> siblings;
  if (card->parent_card_id.has_value()) {
    siblings = card_repo.list_children(project_id, card->parent_card_id.value());
  } else {
    siblings = card_repo.list_roots(project_id);
  }

  std::vector<std::string> titles;
  for (const auto& sibling : siblings) {
    if (sibling.card_id == card_id) {
      continue;
    }
    if (sibling.title.empty()) {
      continue;
    }
    titles.push_back(sibling.title);
    if (titles.size() >= 8) {
      break;
    }
  }
  return titles;
}

std::optional<holder::model::Card> parent_card(holder::platform::Db& db,
                                               const std::string& card_id) {
  holder::card::CardRepo card_repo(db);
  const auto card = card_repo.get(card_id);
  if (!card.has_value() || !card->parent_card_id.has_value()) {
    return std::nullopt;
  }
  return card_repo.get(card->parent_card_id.value());
}

std::vector<holder::model::Card> sibling_cards(holder::platform::Db& db,
                                               const std::string& project_id,
                                               const std::string& card_id) {
  holder::card::CardRepo card_repo(db);
  const auto card = card_repo.get(card_id);
  if (!card.has_value()) {
    return {};
  }

  std::vector<holder::model::Card> siblings;
  if (card->parent_card_id.has_value()) {
    siblings = card_repo.list_children(project_id, card->parent_card_id.value());
  } else {
    siblings = card_repo.list_roots(project_id);
  }

  siblings.erase(
      std::remove_if(
          siblings.begin(),
          siblings.end(),
          [&](const holder::model::Card& sibling) { return sibling.card_id == card_id; }),
      siblings.end());
  return siblings;
}

std::string card_excerpt_line(holder::platform::Db& db,
                              const std::string& project_id,
                              const holder::model::Card& card) {
  const auto body = load_card_body(db, project_id, card.card_id);
  if (!body.has_value()) {
    return "";
  }
  const auto text = trim_copy(body.value());
  if (text.empty()) {
    return "";
  }
  std::ostringstream out;
  out << card.title << ": " << truncate_for_prompt(text, 180);
  return out.str();
}

std::vector<std::string> sibling_card_excerpts(holder::platform::Db& db,
                                               const std::string& project_id,
                                               const std::string& card_id,
                                               std::size_t limit) {
  const auto siblings = sibling_cards(db, project_id, card_id);
  std::vector<std::string> excerpts;
  for (const auto& sibling : siblings) {
    const auto line = card_excerpt_line(db, project_id, sibling);
    if (line.empty()) {
      continue;
    }
    excerpts.push_back(line);
    if (excerpts.size() >= limit) {
      break;
    }
  }
  return excerpts;
}

std::vector<std::string> recent_project_card_excerpts(holder::platform::Db& db,
                                                      const std::string& project_id,
                                                      const std::optional<std::string>& exclude_card_id,
                                                      std::size_t limit) {
  holder::card::CardRepo card_repo(db);
  auto cards = card_repo.list_all(project_id);
  std::sort(cards.begin(), cards.end(), [](const holder::model::Card& a, const holder::model::Card& b) {
    return a.updated_at > b.updated_at;
  });

  std::vector<std::string> excerpts;
  for (const auto& card : cards) {
    if (exclude_card_id.has_value() && card.card_id == exclude_card_id.value()) {
      continue;
    }
    const auto line = card_excerpt_line(db, project_id, card);
    if (line.empty()) {
      continue;
    }
    excerpts.push_back(line);
    if (excerpts.size() >= limit) {
      break;
    }
  }
  return excerpts;
}

std::optional<std::string> latest_ai_thread_excerpt(holder::platform::Db& db,
                                                    const std::string& project_id,
                                                    const std::optional<std::string>& card_id) {
  holder::ai::AiThreadRepo thread_repo(db);
  const auto threads = thread_repo.list(project_id);
  std::optional<holder::model::AiThread> selected_thread;
  if (card_id.has_value()) {
    for (const auto& thread : threads) {
      if (thread.card_id.has_value() && thread.card_id.value() == card_id.value()) {
        selected_thread = thread;
        break;
      }
    }
  }
  if (!selected_thread.has_value() && !threads.empty()) {
    selected_thread = threads.front();
  }
  if (!selected_thread.has_value()) {
    return std::nullopt;
  }

  holder::ai::AiMessageRepo message_repo(db, nullptr);
  const auto messages = message_repo.list_by_thread(selected_thread->thread_id);
  if (messages.empty()) {
    return std::nullopt;
  }

  std::ostringstream out;
  const std::size_t start = (messages.size() > 4) ? (messages.size() - 4) : 0;
  for (std::size_t i = start; i < messages.size(); ++i) {
    const auto& message = messages[i];
    out << (message.role == "assistant" ? "Assistant" : "User") << ": "
        << truncate_for_prompt(trim_copy(message.content), 220) << "\n";
  }
  return trim_copy(out.str());
}

std::string build_nudge_context_summary(holder::platform::Db& db,
                                        const holder::ai::NudgeCandidateInput& input) {
  std::ostringstream out;
  const bool title_only_candidate = input.kind == "card.title_only";
  if (input.card_id.has_value()) {
    const auto title = input.facts.value("title", "");
    if (!title.empty()) {
      out << "Current card title: " << title << "\n";
    }

    const auto body = load_card_body(db, input.project_id, input.card_id.value());
    const auto trimmed_body = body.has_value() ? trim_copy(body.value()) : std::string();
    if (!trimmed_body.empty()) {
      out << "Current card body:\n" << truncate_for_prompt(trim_copy(body.value()), 700) << "\n";
    }

    const auto siblings = sibling_card_titles(db, input.project_id, input.card_id.value());
    if (!siblings.empty()) {
      out << "Sibling cards: " << join_titles(siblings) << "\n";
    }

    if (title_only_candidate && trimmed_body.empty()) {
      const auto parent = parent_card(db, input.card_id.value());
      if (parent.has_value()) {
        out << "Parent card title: " << parent->title << "\n";
        const auto parent_body = load_card_body(db, input.project_id, parent->card_id);
        if (parent_body.has_value()) {
          const auto trimmed_parent_body = trim_copy(parent_body.value());
          if (!trimmed_parent_body.empty()) {
            out << "Parent card excerpt: "
                << truncate_for_prompt(trimmed_parent_body, 220) << "\n";
          }
        }
      }

      const auto sibling_excerpts =
          sibling_card_excerpts(db, input.project_id, input.card_id.value(), 4);
      if (!sibling_excerpts.empty()) {
        out << "Sibling card excerpts:\n";
        for (const auto& excerpt : sibling_excerpts) {
          out << "- " << excerpt << "\n";
        }
      }

      const auto recent_excerpts =
          recent_project_card_excerpts(db, input.project_id, input.card_id, 4);
      if (!recent_excerpts.empty()) {
        out << "Recent project card excerpts:\n";
        for (const auto& excerpt : recent_excerpts) {
          out << "- " << excerpt << "\n";
        }
      }
    }
  }

  const auto ai_excerpt = latest_ai_thread_excerpt(db, input.project_id, input.card_id);
  if (ai_excerpt.has_value()) {
    out << "Recent AI thread:\n" << ai_excerpt.value() << "\n";
  }
  return trim_copy(out.str());
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
                                             const std::string& deterministic_body,
                                             const std::string& context_summary) {
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
  if (!context_summary.empty()) {
    prompt << "Context:\n" << context_summary << "\n";
  }
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
  const auto context_summary = build_nudge_context_summary(db_, input);
  const auto prompt = build_nudge_prompt(input, deterministic, context_summary);
  const bool ok = runner_->stream_generate(
      model.value(),
      prompt,
      "{}",
      [&](const std::string& chunk) { generated += chunk; },
      &error);
  if (!ok) return deterministic;

  auto trimmed = trim_copy(generated);
  if (trimmed.empty()) return deterministic;
  if (trimmed.size() > 240) return deterministic;

  const auto lowered = lower_copy(trimmed);
  if (lowered.find("current card") != std::string::npos ||
      lowered.find("sibling cards") != std::string::npos ||
      lowered.find("recent ai thread") != std::string::npos ||
      lowered.find("recent project card excerpts") != std::string::npos ||
      lowered.find("parent card") != std::string::npos ||
      lowered.find("fallback wording") != std::string::npos ||
      lowered.find("candidate kind") != std::string::npos ||
      lowered.find("facts:") != std::string::npos ||
      lowered.find('\n') != std::string::npos ||
      lowered.find('#') != std::string::npos) {
    return deterministic;
  }

  if (!trimmed.empty() && trimmed.back() == '"') {
    return deterministic;
  }
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
