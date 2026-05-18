#pragma once

#include "model/AiLocalModelConfig.h"
#include "platform/Db.h"

#include <optional>

namespace holder::ai {

class AiLocalModelConfigRepo {
 public:
  explicit AiLocalModelConfigRepo(holder::platform::Db& db);

  std::optional<holder::model::AiLocalModelConfig> get() const;
  void set(
      const std::optional<std::string>& fast_model,
      const std::optional<std::string>& strong_model,
      const std::optional<std::string>& deep_model,
      long long updated_at
  );
  void clear();

 private:
  holder::platform::Db& db_;
};

} // namespace holder::ai
