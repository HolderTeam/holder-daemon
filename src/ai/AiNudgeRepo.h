#pragma once

#include "ai/NudgeService.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

class AiNudgeRepo {
 public:
  explicit AiNudgeRepo(holder::platform::Db& db);

  void create(const Nudge& nudge);
  std::optional<Nudge> find_active_exact_match(const std::string& kind,
                                               const std::string& project_id,
                                               const std::optional<std::string>& card_id,
                                               const std::optional<std::string>& basis_fingerprint,
                                               const std::optional<std::string>& basis_commit) const;
  std::vector<Nudge> list_active(const std::string& project_id,
                                 const std::optional<std::string>& card_id = std::nullopt) const;
  void dismiss_stale_variants(const std::string& kind,
                              const std::string& project_id,
                              const std::optional<std::string>& card_id,
                              const std::optional<std::string>& basis_fingerprint,
                              const std::optional<std::string>& basis_commit);
  bool dismiss(const std::string& nudge_id);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::ai
