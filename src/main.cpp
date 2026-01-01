#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

int main() {
  spdlog::info("card-server starting…");
  nlohmann::json j = {{"ok", true}};
  spdlog::info("json smoke test: {}", j.dump());
  return 0;
}
