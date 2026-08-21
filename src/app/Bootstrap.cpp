#include "app/Bootstrap.h"

#include "platform/InstalledDataPath.h"
#include "platform/Paths.h"
#include "project/DefaultProject.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/spdlog.h>

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

std::optional<holder::model::Project> bootstrap_default_home_project(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts
) {
  const std::string content = load_welcome_markdown_body();
  const std::string title = derive_title_from_markdown_first_line(content, "Welcome");

  const auto home = holder::project::ensure_default_project(
      db,
      "Home",
      "encrypted_git",
      title,
      content,
      generate_uuid_v4,
      holder::core::default_projects_root(),
      fts
  );
  if (home.has_value()) {
    spdlog::info("Bootstrapped default Home project ({})", home->project_id);
  }
  return home;
}

} // namespace holder::app
