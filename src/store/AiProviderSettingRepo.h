#pragma once

#include "model/AiProviderSetting.h"
#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class AiProviderSettingRepo {
 public:
  explicit AiProviderSettingRepo(Db& db);

  std::vector<holder::model::AiProviderSetting> list() const;
  std::optional<holder::model::AiProviderSetting> get(const std::string& provider) const;
  void upsert(const std::string& provider, bool enabled, long long updated_at);
  void remove(const std::string& provider);

 private:
  Db& db_;
};

} // namespace holder::store
