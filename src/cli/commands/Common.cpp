#include "cli/commands/Common.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cerrno>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace holder::cli {

void print_usage(std::ostream& out) {
  out << "Usage: holderctl <command>\n"
      << "\n"
      << "Commands:\n"
      << "  token      Print the local daemon bearer token\n"
      << "  status     Print local daemon status\n"
      << "  health     Check daemon metadata, process state, token, and HTTP health\n"
      << "  paths      Print Holder data/config/cache paths\n"
      << "  project    Manage projects; use 'project new <name>' to create one\n"
      << "  projects   List Holder projects; use --json for raw API output\n"
      << "  use        Set the current project by id/name; no args resets to Home\n"
      << "  current    Print the current project\n"
      << "  cards      List root cards in the current project; use --recent for latest\n"
      << "  search     Search cards in the current project\n"
      << "  card       Print a card from the current project\n"
      << "  edit       Open $EDITOR for a card, then save changes\n"
      << "  new        Create a card in the current project from args or stdin\n"
      << "  append     Append text/stdin to a card in the current project\n"
      << "  resource   Manage resources in the current project\n"
      << "  recovery-token  Export or import encrypted project recovery tokens\n"
      << "  openapi    Open local Swagger/OpenAPI docs; use --url to print the URL\n"
      << "  reindex    Rebuild the daemon search index from the local database\n"
      << "  restart    Restart the local daemon and rotate its bearer token\n"
      << "  logs       Print daemon logs; use --follow to tail or --path for the file path\n"
      << "  version    Print holderctl version\n";
}

bool is_process_running(int pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  return true;
#else
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM; // LCOV_EXCL_LINE: depends on host process ownership/permissions.
#endif
}

#ifndef _WIN32
void require_secure_file(const std::filesystem::path& path) {
  struct stat file_stat {};
  if (::lstat(path.c_str(), &file_stat) != 0) {
    throw std::runtime_error("Cannot inspect token file: " + path.string());
  }
  if (S_ISLNK(file_stat.st_mode)) {
    throw std::runtime_error("Refusing token file symlink: " + path.string());
  }
  if (!S_ISREG(file_stat.st_mode)) {
    throw std::runtime_error("Token file is not a regular file: " + path.string());
  }
  if (file_stat.st_uid != ::geteuid()) { // LCOV_EXCL_LINE: requires a file owned by another user.
    throw std::runtime_error("Token file is not owned by the current user: " + path.string()); // LCOV_EXCL_LINE
  }
  if ((file_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    throw std::runtime_error("Token file must be readable only by its owner: " + path.string());
  }

  const auto parent = path.parent_path();
  struct stat dir_stat {};
  if (::stat(parent.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) { // LCOV_EXCL_LINE: parent exists for a normal info path.
    throw std::runtime_error("Cannot inspect token directory: " + parent.string()); // LCOV_EXCL_LINE
  }
  if (dir_stat.st_uid != ::geteuid()) { // LCOV_EXCL_LINE: requires a directory owned by another user.
    throw std::runtime_error("Token directory is not owned by the current user: " + parent.string()); // LCOV_EXCL_LINE
  }
  if ((dir_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    throw std::runtime_error("Token directory must be accessible only by its owner: " + parent.string());
  }
}
#else
void require_secure_file(const std::filesystem::path&) {}
#endif

ServerInfoFile read_server_info(const holder::core::Paths& paths) {
  const auto info_path = paths.info_path();
  std::ifstream in(info_path);
  if (!in.is_open()) {
    throw std::runtime_error("Holder daemon info file not found: " + info_path.string());
  }
  return {.path = info_path, .json = nlohmann::json::parse(in)};
}

std::string json_string(const nlohmann::json& json,
                        const char* key,
                        const std::string& fallback) {
  if (!json.contains(key) || json.at(key).is_null()) return fallback;
  return json.at(key).get<std::string>();
}

int json_int(const nlohmann::json& json, const char* key, int fallback) {
  if (!json.contains(key) || json.at(key).is_null()) return fallback;
  return json.at(key).get<int>();
}

DaemonConnection read_secure_daemon_connection(const holder::core::Paths& paths) {
  require_secure_file(paths.info_path());
  auto info = read_server_info(paths);
  DaemonConnection connection;
  connection.bind = json_string(info.json, "bind", "127.0.0.1");
  connection.port = json_int(info.json, "port", 11499);
  connection.token = json_string(info.json, "auth_token");
  if (connection.token.empty()) {
    throw std::runtime_error("Holder daemon info file has no auth_token: " + info.path.string());
  }
  connection.info = std::move(info);
  return connection;
}

HttpJsonResponse http_json_request(const DaemonConnection& connection,
                                   boost::beast::http::verb method,
                                   const std::string& target,
                                   std::chrono::seconds timeout,
                                   const std::optional<nlohmann::json>& body) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(connection.bind, std::to_string(connection.port));

  boost::beast::tcp_stream stream(ioc);
  stream.expires_after(timeout);
  stream.connect(endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, connection.bind);
  req.set(http::field::user_agent, "holderctl");
  req.set(http::field::authorization, "Bearer " + connection.token);
  req.keep_alive(false);
  if (body.has_value()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body->dump();
    req.prepare_payload();
  }

  http::write(stream, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  boost::system::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);

  return {.status = res.result(), .payload = nlohmann::json::parse(res.body())};
}

} // namespace holder::cli
