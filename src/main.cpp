#include <spdlog/spdlog.h>
#include "core/Paths.h"

int main() {
  spdlog::info("holder starting…");

  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();

  spdlog::info("data_dir:   {}", paths.data_dir.string());
  spdlog::info("config_dir: {}", paths.config_dir.string());
  spdlog::info("cache_dir:  {}", paths.cache_dir.string());
  spdlog::info("db_path:    {}", paths.db_path().string());

  return 0;
}
