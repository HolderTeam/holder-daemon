#include "app/Bootstrap.h"

#include "card/CardStore.h"
#include "model/Card.h"
#include "platform/Db.h"
#include "platform/InstalledDataPath.h"
#include "project/ProjectPaths.h"
#include "project/ProjectRepo.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace holder::app {
namespace {

std::filesystem::path find_welcome_markdown() {
  namespace fs = std::filesystem;

  fs::path p1 = fs::current_path() / "config" / "WELCOME.md";
  if (fs::exists(p1)) return p1;

  fs::path p2 = fs::current_path().parent_path() / "config" / "WELCOME.md";
  if (fs::exists(p2)) return p2;

  if (auto installed = holder::core::installed_data_path("config/WELCOME.md")) // LCOV_EXCL_LINE
    return installed.value(); // LCOV_EXCL_LINE

  throw std::runtime_error("Cannot find config/WELCOME.md from current directory."
  ); // LCOV_EXCL_LINE
}

std::string load_welcome_markdown_body() {
  std::ifstream in(find_welcome_markdown());
  if (!in) {
    throw std::runtime_error("Failed to open config/WELCOME.md"); // LCOV_EXCL_LINE
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string derive_title_from_markdown_first_line(
    const std::string& body,
    const std::string& fallback
) {
  std::string line;
  for (char ch : body) {
    if (ch == '\n') break;
    line.push_back(ch);
  }
  const auto non_space = line.find_first_not_of(" \t\r");
  if (non_space == std::string::npos) {
    return fallback;
  }
  auto first = line.substr(non_space);
  if (!first.empty() && first[0] == '#') {
    const auto title_start = first.find_first_not_of("# \t");
    if (title_start != std::string::npos) {
      return first.substr(title_start);
    }
  }
  return fallback;
}

} // namespace

std::string generate_uuid_v4() {
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
}

std::optional<holder::model::Project> ensure_default_home_project(holder::platform::Db& db) {
  holder::project::ProjectRepo repo(db);
  auto projects = repo.list();
  if (!projects.empty()) {
    return std::nullopt;
  }

  holder::model::Project home;
  home.project_id = generate_uuid_v4();
  home.name = "Home";
  home.privacy_mode = "encrypted_git";
  home.project_key_id.reset();
  home.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
  )
                        .count();
  home.updated_at = home.created_at;

  const auto base_root = holder::core::default_projects_root();
  const auto slug = holder::core::slugify(home.name);
  home.root_path = holder::core::unique_project_root(base_root, slug, projects);

  repo.create(home);
  spdlog::info("Bootstrapped default Home project ({})", home.project_id);
  return home;
}

void ensure_default_welcome_card(
    holder::card::CardStore& card_store,
    const holder::model::Project& home
) {
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()
  )
                       .count();
  const std::string content = load_welcome_markdown_body();
  holder::model::Card welcome;
  welcome.card_id = generate_uuid_v4();
  welcome.project_id = home.project_id;
  welcome.title = derive_title_from_markdown_first_line(content, "Welcome");
  welcome.created_at = now;
  welcome.updated_at = now;
  card_store.create(welcome, content);
  spdlog::info("Bootstrapped welcome card ({}) in Home project", welcome.card_id);
}

} // namespace holder::app
