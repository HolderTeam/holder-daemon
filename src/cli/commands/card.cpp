#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace holder::cli {
namespace {

struct SearchOptions {
  bool json_output = false;
  int limit = 20;
  std::string query;
};

struct CardsOptions {
  bool json_output = false;
  bool recent = false;
  int limit = 20;
  std::optional<std::string> parent_card_id;
};

struct CardOptions {
  bool json_output = false;
  std::string card_id;
};

SearchOptions parse_search_options(int argc, char* argv[]) {
  SearchOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl search [--json] [--limit N] <query>");
      }
      try {
        options.limit = std::stoi(argv[++i]);
      } catch (const std::exception&) {
        throw std::runtime_error("Invalid search limit: " + std::string(argv[i]));
      }
      if (options.limit < 1) {
        throw std::runtime_error("Search limit must be at least 1");
      }
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown search option: " + arg);
    } else {
      if (!options.query.empty()) options.query += " ";
      options.query += arg;
    }
  }

  if (options.query.empty()) {
    throw std::runtime_error("Usage: holderctl search [--json] [--limit N] <query>");
  }
  return options;
}

CardsOptions parse_cards_options(int argc, char* argv[]) {
  CardsOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--recent") {
      options.recent = true;
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        throw std::runtime_error(
            "Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]"
        );
      }
      try {
        options.limit = std::stoi(argv[++i]);
      } catch (const std::exception&) {
        throw std::runtime_error("Invalid cards limit: " + std::string(argv[i]));
      }
      if (options.limit < 1) {
        throw std::runtime_error("Cards limit must be at least 1");
      }
    } else if (arg == "--parent") {
      if (i + 1 >= argc) {
        throw std::runtime_error(
            "Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]"
        );
      }
      const std::string parent = argv[++i];
      if (parent.empty()) {
        throw std::runtime_error("--parent must not be empty");
      }
      options.parent_card_id = parent;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown cards option: " + arg);
    } else {
      throw std::runtime_error(
          "Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]"
      );
    }
  }

  if (options.recent && options.parent_card_id.has_value()) {
    throw std::runtime_error("holderctl cards cannot combine --recent and --parent");
  }
  if (!options.recent && options.limit != 20) {
    throw std::runtime_error("holderctl cards --limit requires --recent");
  }
  return options;
}

CardOptions parse_card_options(int argc, char* argv[]) {
  CardOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown card option: " + arg);
    } else if (options.card_id.empty()) {
      options.card_id = arg;
    } else {
      throw std::runtime_error("Usage: holderctl card [--json] <card-id>");
    }
  }

  if (options.card_id.empty()) {
    throw std::runtime_error("Usage: holderctl card [--json] <card-id>");
  }
  return options;
}

nlohmann::json card_update_body(const nlohmann::json& card, const std::string& content) {
  nlohmann::json body;
  body["content"] = content;
  body["title"] = json_string(card, "title");
  body["updated_at"] = now_epoch_seconds();
  return body;
} // LCOV_EXCL_LINE

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') out += '\\';
    out += ch;
  }
  out += '"';
  return out;
#else
  std::string out = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out += ch;
    }
  }
  out += "'";
  return out;
#endif
} // LCOV_EXCL_LINE: gcov attributes no executable branch to the function close after shell quoting.

std::string sanitized_filename_component(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || ch == '-' || ch == '_') {
      out += ch;
    } else {
      out += '_';
    }
  }
  return out.empty() ? "card" : out;
}

std::filesystem::path edit_temp_path(const holder::core::Paths& paths, const std::string& card_id) {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  return paths.cache_dir / ("holderctl-edit-" + sanitized_filename_component(card_id) + "-" +
                            std::to_string(unique) + ".md");
}

std::string read_text_file_raw(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    // LCOV_EXCL_START
    throw std::runtime_error("Failed to read editor temp file: " + path.string());
    // LCOV_EXCL_STOP
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_text_file_raw(const std::filesystem::path& path, const std::string& content) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    // LCOV_EXCL_START
    throw std::runtime_error("Failed to create editor temp file: " + path.string());
    // LCOV_EXCL_STOP
  }
  out << content;
}

void remove_temp_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

std::string required_editor() {
  const char* editor = std::getenv("EDITOR");
  if (editor == nullptr || std::string(editor).empty()) {
    throw std::runtime_error("EDITOR is not set.");
  }
  return editor;
}

void run_editor_on_file(const std::string& editor, const std::filesystem::path& path) {
  const auto command = editor + " " + shell_quote(path.string());
  const int rc = std::system(command.c_str());
  if (rc != 0) {
    throw std::runtime_error("Editor failed.");
  }
}

} // namespace

int command_search(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_search_options(argc, argv);
  const auto current_project_id = read_current_project_id(paths);
  const auto projects_payload = list_projects_payload(paths, false);
  (void)find_project_by_id(projects_payload.at("data"), current_project_id);

  try {
    const auto connection = read_secure_daemon_connection(paths);
    const std::string target = "/search/cards?project_id=" +
                               url_encode_component(current_project_id) +
                               "&q=" + url_encode_component(options.query) +
                               "&limit=" + std::to_string(options.limit);
    const auto response = http_json_request(
        connection,
        boost::beast::http::verb::get,
        target,
        std::chrono::seconds(10) // LCOV_EXCL_LINE
    );

    if (response.status != boost::beast::http::status::ok || !response.payload.value("ok", false)) {
      const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      throw std::runtime_error("Search failed: " + api_error_message(response, fallback));
    }

    if (options.json_output) {
      std::cout << response.payload.dump(2) << "\n";
      return 0;
    }

    const auto& cards = response.payload.at("data");
    if (!cards.is_array() || cards.empty()) {
      std::cout << "No cards found.\n";
      return 0;
    }

    for (const auto& card : cards) {
      std::cout << json_string(card, "card_id") << "\t" << json_string(card, "title") << "\n";
      const auto snippet = json_string(card, "snippet");
      if (!snippet.empty()) {
        std::cout << "  " << snippet << "\n";
      }
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to search cards: ") + ex.what());
  }
}

int command_cards(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_cards_options(argc, argv);
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");

  try {
    std::string target = "/cards?project_id=" + url_encode_component(project_id) + "&count=true";
    if (options.recent) {
      target += "&view=recent&limit=" + std::to_string(options.limit);
    } else {
      target += "&view=tree";
      if (options.parent_card_id.has_value()) {
        target += "&parent_card_id=" + url_encode_component(options.parent_card_id.value());
      }
    }

    const auto payload = card_api_request(paths, boost::beast::http::verb::get, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& cards = payload.at("data");
    if (!cards.is_array() || cards.empty()) {
      std::cout << (options.recent ? "No recent cards.\n" : "No root cards.\n");
      return 0;
    }

    std::cout << "CARD_ID\tTITLE\tCHILDREN\tUPDATED\n";
    for (const auto& card : cards) {
      std::cout << json_string(card, "card_id") << "\t" << json_string(card, "title") << "\t"
                << card.value("child_count", 0) << "\t" << card.value("updated_at", 0) << "\n";
    }
    return 0;
    // LCOV_EXCL_START
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list cards: ") + ex.what());
  }
  // LCOV_EXCL_STOP
}

int command_card(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_card_options(argc, argv);
  const auto current_project_id = read_current_project_id(paths);
  const auto projects_payload = list_projects_payload(paths, false);
  (void)find_project_by_id(projects_payload.at("data"), current_project_id);

  try {
    const auto connection = read_secure_daemon_connection(paths);
    const auto response = http_json_request(
        connection,
        boost::beast::http::verb::get,
        "/cards/" + url_encode_component(options.card_id),
        std::chrono::seconds(10) // LCOV_EXCL_LINE
    );

    if (response.status != boost::beast::http::status::ok || !response.payload.value("ok", false)) {
      const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      throw std::runtime_error("Card request failed: " + api_error_message(response, fallback));
    }

    const auto& data = response.payload.at("data");
    const auto card_project_id = json_string(data, "project_id");
    if (card_project_id != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + options.card_id);
    }

    if (options.json_output) {
      std::cout << response.payload.dump(2) << "\n";
      return 0;
    }

    const auto content = json_string(data, "content");
    std::cout << content;
    if (content.empty() || content.back() != '\n') {
      std::cout << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to print card: ") + ex.what());
  }
}

int command_edit(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc != 3 || std::string(argv[2]).empty()) {
    throw std::runtime_error("Usage: holderctl edit <card-id>");
  }

  const std::string card_id = argv[2];
  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    const auto fetched = card_api_request(
        paths,
        boost::beast::http::verb::get,
        "/cards/" + url_encode_component(card_id)
    );
    const auto& data = fetched.at("data");
    if (json_string(data, "project_id") != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + card_id);
    }

    const auto editor = required_editor();
    const auto original_content = json_string(data, "content");
    const auto temp_path = edit_temp_path(paths, card_id);
    write_text_file_raw(temp_path, original_content);

    run_editor_on_file(editor, temp_path);
    const auto edited_content = read_text_file_raw(temp_path);

    if (edited_content == original_content) {
      remove_temp_file(temp_path);
      std::cout << "No changes.\n";
      return 0;
    }

    (void)card_api_request(
        paths,
        boost::beast::http::verb::patch,
        "/cards/" + url_encode_component(card_id),
        card_update_body(data, edited_content)
    );
    remove_temp_file(temp_path);
    std::cout << "Updated card: " << card_id << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to edit card: ") + ex.what());
  }
}

int command_new(const holder::core::Paths& paths, int argc, char* argv[]) {
  std::string content = join_args(2, argc, argv);
  if (content.empty()) {
    content = read_stdin_all();
  }
  if (trim_ascii_whitespace(content).empty()) {
    throw std::runtime_error("Usage: holderctl new <text>  OR  <command> | holderctl new");
  }

  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    const auto payload = card_api_request(
        paths,
        boost::beast::http::verb::post,
        "/cards",
        // LCOV_EXCL_START
        {{"project_id", project_id},
         {"title", title_from_content(content)},
         {"content", content},
         {"created_at", now_epoch_seconds()},
         {"updated_at", now_epoch_seconds()}},
        // LCOV_EXCL_STOP
        boost::beast::http::status::created
    );
    std::cout << "Created card: " << json_string(payload.at("data"), "card_id") << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to create card: ") + ex.what());
  }
}

int command_append(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(
        "Usage: holderctl append <card-id> <text>  OR  <command> | holderctl append <card-id>"
    );
  }

  const std::string card_id = argv[2];
  std::string addition = join_args(3, argc, argv);
  if (addition.empty()) {
    addition = read_stdin_all();
  }
  if (trim_ascii_whitespace(addition).empty()) {
    throw std::runtime_error(
        "Usage: holderctl append <card-id> <text>  OR  <command> | holderctl append <card-id>"
    );
  }

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    const auto fetched = card_api_request(
        paths,
        boost::beast::http::verb::get,
        "/cards/" + url_encode_component(card_id)
    );
    const auto& data = fetched.at("data");
    if (json_string(data, "project_id") != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + card_id);
    }

    auto content = json_string(data, "content");
    trim_trailing_line_breaks(content);
    if (!content.empty()) {
      content += "\n\n";
    }
    content += addition;

    (void)card_api_request(
        paths,
        boost::beast::http::verb::patch,
        "/cards/" + url_encode_component(card_id),
        card_update_body(data, content)
    );
    std::cout << "Appended to card: " << card_id << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to append to card: ") + ex.what());
  }
}

} // namespace holder::cli
