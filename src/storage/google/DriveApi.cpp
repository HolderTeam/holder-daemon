#include "storage/google/DriveApi.h"

#include "resource/StorageProvider.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace holder::storage::google {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using holder::resource::StorageError;
using holder::resource::StorageErrorCode;

constexpr const char* kHost = "www.googleapis.com";
constexpr const char* kFilesPath = "/drive/v3/files";
constexpr const char* kUploadPath = "/upload/drive/v3/files";
constexpr const char* kFolderMimeType = "application/vnd.google-apps.folder";
// One boundary string reused across every multipart upload -- fine, since it's just a
// delimiter Drive parses out of a single request body, not shared state between
// requests. Chosen unlikely to collide with real file content.
constexpr const char* kMultipartBoundary = "holder-google-drive-upload-boundary";

StorageErrorCode status_to_error_code(unsigned int status) {
  if (status == 401) return StorageErrorCode::Authentication;
  if (status == 403) return StorageErrorCode::Permission;
  if (status == 404) return StorageErrorCode::Integrity;
  if (status == 429 || status >= 500) return StorageErrorCode::Transient;
  return StorageErrorCode::Unavailable;
}

std::string url_encode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const char raw_ch : value) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('%');
      out.push_back(kHex[(ch >> 4) & 0x0F]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

// Drive's query syntax needs a single-quoted string literal's own backslashes and
// single quotes backslash-escaped (see the Drive API "Search for files" reference).
// object_key/folder names are opaque strings this code doesn't otherwise control the
// content of, so this matters for correctness, not just tidiness.
std::string escape_drive_query_literal(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '\'') out.push_back('\\');
    out.push_back(ch);
  }
  return out;
}

struct DriveResponse {
  unsigned int status = 0;
  std::string body;
};

// Establishes a fresh TLS connection to www.googleapis.com and exchanges one request.
// No connection pooling or retry -- every Drive operation here is either a small
// metadata call or one upload/download per Asset, not a hot path, so
// S3CompatibleProvider's more elaborate per-request-with-retry shape isn't needed.
template <typename Request, typename ResponseParser>
void exchange(Request& req, ResponseParser& parser) {
  asio::io_context ioc;
  ssl::context ctx(ssl::context::tls_client);
  ctx.set_default_verify_paths();
  tcp::resolver resolver(ioc);
  const auto endpoints = resolver.resolve(kHost, "443");
  beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
  if (!SSL_set_tlsext_host_name(stream.native_handle(), kHost)) {
    throw std::runtime_error("failed to set TLS hostname for Drive request");
  }
  beast::get_lowest_layer(stream).connect(endpoints);
  stream.set_verify_mode(ssl::verify_peer);
  stream.set_verify_callback(ssl::host_name_verification(kHost));
  stream.handshake(ssl::stream_base::client);

  http::write(stream, req);
  beast::flat_buffer buffer;
  http::read(stream, buffer, parser);

  boost::system::error_code ec;
  stream.shutdown(ec);
}

DriveResponse json_request(
    http::verb method,
    const std::string& target,
    const std::string& access_token,
    const std::optional<nlohmann::json>& json_body
) {
  try {
    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, kHost);
    req.set(http::field::authorization, "Bearer " + access_token);
    req.set(http::field::user_agent, "holder/google-drive");
    if (json_body.has_value()) {
      req.set(http::field::content_type, "application/json; charset=UTF-8");
      req.body() = json_body->dump();
    }
    req.prepare_payload();

    http::response_parser<http::string_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    exchange(req, parser);
    return {parser.get().result_int(), parser.get().body()};
  } catch (const StorageError&) {
    throw;
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable, std::string("Drive request failed: ") + ex.what()
    );
  }
}

void check_ok(const DriveResponse& response, const std::string& operation) {
  if (response.status >= 200 && response.status < 300) return;
  throw StorageError(
      status_to_error_code(response.status),
      "Drive " + operation + " failed: HTTP " + std::to_string(response.status) + " " +
          response.body
  );
}

std::string read_whole_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StorageError(
        StorageErrorCode::Unavailable, "could not open staged file for Drive upload"
    );
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::optional<std::string> find_file_id_in_scope(
    const std::string& access_token,
    const std::string& query
) {
  const std::string target = std::string(kFilesPath) + "?q=" + url_encode(query) +
                              "&fields=" + url_encode("files(id)") + "&pageSize=1";
  const auto response = json_request(http::verb::get, target, access_token, std::nullopt);
  check_ok(response, "search");
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(response.body);
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable,
        std::string("Drive search response could not be parsed: ") + ex.what()
    );
  }
  const auto& files = json.at("files");
  if (files.empty()) return std::nullopt;
  return files.at(0).at("id").get<std::string>();
}

std::string find_or_create_folder(
    const std::string& access_token,
    const std::string& name,
    const std::optional<std::string>& parent_id
) {
  std::string query = "name = '" + escape_drive_query_literal(name) + "' and mimeType = '" +
                       std::string(kFolderMimeType) + "' and trashed = false";
  query += parent_id.has_value()
               ? " and '" + escape_drive_query_literal(*parent_id) + "' in parents"
               : " and 'root' in parents";
  const auto existing = find_file_id_in_scope(access_token, query);
  if (existing.has_value()) return *existing;

  nlohmann::json metadata = {{"name", name}, {"mimeType", kFolderMimeType}};
  if (parent_id.has_value()) metadata["parents"] = nlohmann::json::array({*parent_id});
  const auto response = json_request(http::verb::post, kFilesPath, access_token, metadata);
  check_ok(response, "folder create");
  try {
    return nlohmann::json::parse(response.body).at("id").get<std::string>();
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable,
        std::string("Drive folder-create response could not be parsed: ") + ex.what()
    );
  }
}

} // namespace

std::optional<std::string> find_file_id(
    const std::string& access_token,
    const std::string& folder_id,
    const std::string& name
) {
  const std::string query = "name = '" + escape_drive_query_literal(name) + "' and '" +
                             escape_drive_query_literal(folder_id) +
                             "' in parents and trashed = false";
  return find_file_id_in_scope(access_token, query);
}

std::string upload_file(
    const std::string& access_token,
    const std::string& folder_id,
    const std::string& name,
    const std::filesystem::path& staged_file
) {
  const auto file_bytes = read_whole_file(staged_file);
  const nlohmann::json metadata = {
      {"name", name}, {"parents", nlohmann::json::array({folder_id})}
  };

  std::string body;
  body += "--";
  body += kMultipartBoundary;
  body += "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n";
  body += metadata.dump();
  body += "\r\n--";
  body += kMultipartBoundary;
  body += "\r\nContent-Type: application/octet-stream\r\n\r\n";
  body += file_bytes;
  body += "\r\n--";
  body += kMultipartBoundary;
  body += "--\r\n";

  try {
    http::request<http::string_body> req{
        http::verb::post, std::string(kUploadPath) + "?uploadType=multipart", 11
    };
    req.set(http::field::host, kHost);
    req.set(http::field::authorization, "Bearer " + access_token);
    req.set(http::field::user_agent, "holder/google-drive");
    req.set(
        http::field::content_type,
        std::string("multipart/related; boundary=") + kMultipartBoundary
    );
    req.body() = std::move(body);
    req.prepare_payload();

    http::response_parser<http::string_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    exchange(req, parser);
    const DriveResponse response{parser.get().result_int(), parser.get().body()};
    check_ok(response, "upload");
    return nlohmann::json::parse(response.body).at("id").get<std::string>();
  } catch (const StorageError&) {
    throw;
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable, std::string("Drive upload failed: ") + ex.what()
    );
  }
}

void replace_file_content(
    const std::string& access_token,
    const std::string& file_id,
    const std::filesystem::path& staged_file
) {
  const auto file_bytes = read_whole_file(staged_file);
  try {
    http::request<http::string_body> req{
        http::verb::patch,
        std::string(kUploadPath) + "/" + file_id + "?uploadType=media", 11
    };
    req.set(http::field::host, kHost);
    req.set(http::field::authorization, "Bearer " + access_token);
    req.set(http::field::user_agent, "holder/google-drive");
    req.set(http::field::content_type, "application/octet-stream");
    req.body() = file_bytes;
    req.prepare_payload();

    http::response_parser<http::string_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    exchange(req, parser);
    check_ok({parser.get().result_int(), parser.get().body()}, "update");
  } catch (const StorageError&) {
    throw;
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable, std::string("Drive update failed: ") + ex.what()
    );
  }
}

void download_file(
    const std::string& access_token,
    const std::string& file_id,
    const std::filesystem::path& destination_file
) {
  try {
    http::request<http::empty_body> req{
        http::verb::get, std::string(kFilesPath) + "/" + file_id + "?alt=media", 11
    };
    req.set(http::field::host, kHost);
    req.set(http::field::authorization, "Bearer " + access_token);
    req.set(http::field::user_agent, "holder/google-drive");
    req.prepare_payload();

    std::filesystem::create_directories(destination_file.parent_path());
    beast::error_code file_error;
    http::response_parser<http::file_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    parser.get().body().open(
        destination_file.string().c_str(), beast::file_mode::write, file_error
    );
    if (file_error) {
      throw std::runtime_error(
          "failed to open Drive download destination: " + file_error.message()
      );
    }

    exchange(req, parser);
    const auto status = parser.get().result_int();
    if (status < 200 || status >= 300) {
      std::error_code ignored;
      std::filesystem::remove(destination_file, ignored);
      throw StorageError(
          status_to_error_code(status), "Drive download failed: HTTP " + std::to_string(status)
      );
    }
  } catch (const StorageError&) {
    std::error_code ignored;
    std::filesystem::remove(destination_file, ignored);
    throw;
  } catch (const std::exception& ex) {
    std::error_code ignored;
    std::filesystem::remove(destination_file, ignored);
    throw StorageError(
        StorageErrorCode::Unavailable, std::string("Drive download failed: ") + ex.what()
    );
  }
}

void delete_file(const std::string& access_token, const std::string& file_id) {
  const auto response =
      json_request(http::verb::delete_, std::string(kFilesPath) + "/" + file_id, access_token, std::nullopt);
  if (response.status == 404) return; // already gone: not a failure
  check_ok(response, "delete");
}

std::string find_or_create_holder_resources_folder(const std::string& access_token) {
  const auto holder_folder_id = find_or_create_folder(access_token, "Holder", std::nullopt);
  return find_or_create_folder(access_token, "Resources", holder_folder_id);
}

} // namespace holder::storage::google
