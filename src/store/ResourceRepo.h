#pragma once

#include "model/Resource.h"
#include "store/Db.h"

#include <string>
#include <vector>

namespace holder::store {

class ResourceRepo {
public:
  explicit ResourceRepo(Db& db);

  void add(const holder::model::Resource& resource);
  std::vector<holder::model::Resource> list(const std::string& project_id) const;
  void remove(const std::string& resource_id);

private:
  Db& db_;
};

} // namespace holder::store
