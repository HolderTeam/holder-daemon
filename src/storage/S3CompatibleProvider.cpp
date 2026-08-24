#include "storage/S3CompatibleProvider.h"

#include "storage/S3SigV4.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace holder::storage {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

constexpr const char* kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct Endpoint {
  bool tls = true;
  std::string host;
  std::string port;
  std::string base_path;
};

Endpoint parse_endpoint(const std::string& value, bool allow_insecure_localhost) {
  Endpoint endpoint;
  std::string remainder;
  if (value.rfind("https://", 0) == 0) {
    endpoint.tls = true;
    endpoint.port = "443";
    remainder = value.substr(8);
  } else if (value.rfind("http://", 0) == 0) {
    endpoint.tls = false;
    endpoint.port = "80";
    remainder = value.substr(7);
  } else {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration,
        "S3 endpoint must use https://"
    );
  }
  const auto slash = remainder.find('/');
  const auto authority = remainder.substr(0, slash);
  endpoint.base_path = slash == std::string::npos ? "" : remainder.substr(slash);
  while (!endpoint.base_path.empty() && endpoint.base_path.back() == '/') {
    endpoint.base_path.pop_back();
  }
  const auto colon = authority.rfind(':');
  endpoint.host = colon == std::string::npos ? authority : authority.substr(0, colon);
  if (colon != std::string::npos) endpoint.port = authority.substr(colon + 1);
  if (endpoint.host.empty()) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration, "S3 endpoint host is empty"
    );
  }
  const bool localhost =
      endpoint.host == "localhost" || endpoint.host == "127.0.0.1" || endpoint.host == "::1";
  if (!endpoint.tls && !(localhost && allow_insecure_localhost)) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration,
        "unverified HTTP is allowed only for explicit localhost development"
    );
  }
  return endpoint;
}

std::string uri_encode_path(const std::string& value) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (char raw_ch : value) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/') {
      out << static_cast<char>(ch);
    } else {
      out << '%' << std::setw(2) << static_cast<int>(ch);
    }
  }
  return out.str();
}

std::pair<std::string, std::string> date_now() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  std::ostringstream date;
  std::ostringstream stamp;
  date << std::put_time(&utc, "%Y%m%d");
  stamp << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
  return {date.str(), stamp.str()};
}

http::verb verb_for(const std::string& method) {
  if (method == "PUT") return http::verb::put;
  if (method == "GET") return http::verb::get;
  if (method == "HEAD") return http::verb::head;
  if (method == "DELETE") return http::verb::delete_;
  throw std::invalid_argument("unsupported S3 method");
}

std::string utf8_path(const std::filesystem::path& path) {
  const auto encoded = path.u8string();
  std::string result;
  result.reserve(encoded.size());
  for (const auto byte : encoded) {
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

void map_status(unsigned int status, const std::string& operation) {
  if (status >= 200 && status < 300) return;
  if (status == 401 || status == 403) {
    throw holder::resource::StorageError(
        status == 401 ? holder::resource::StorageErrorCode::Authentication
                      : holder::resource::StorageErrorCode::Permission,
        "S3 " + operation + " was denied"
    );
  }
  if (status == 404) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::Unavailable, "S3 object not found"
    );
  }
  if (status == 429 || status >= 500) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::Transient,
        "S3 " + operation + " failed with HTTP " + std::to_string(status)
    );
  }
  throw holder::resource::StorageError(
      holder::resource::StorageErrorCode::Unavailable,
      "S3 " + operation + " failed with HTTP " + std::to_string(status)
  );
}

} // namespace

S3CompatibleProvider::S3CompatibleProvider(
    S3CompatibleConfig config,
    S3Credentials credentials
)
    : config_(std::move(config)), credentials_(std::move(credentials)) {
  if (config_.endpoint.empty() || config_.region.empty() || config_.bucket.empty() ||
      credentials_.access_key_id.empty() || credentials_.secret_access_key.empty()) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration,
        "S3 endpoint, region, bucket and credentials are required"
    );
  }
  if (config_.addressing_style != "path" && config_.addressing_style != "virtual_host") {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration,
        "S3 addressing_style must be path or virtual_host"
    );
  }
  (void)parse_endpoint(config_.endpoint, config_.allow_insecure_localhost);
}

unsigned int S3CompatibleProvider::request(
    const std::string& method,
    const std::string& object_key,
    const std::filesystem::path* upload,
    const std::filesystem::path* download,
    long long content_length,
    const std::string& payload_sha256
) {
  auto endpoint = parse_endpoint(config_.endpoint, config_.allow_insecure_localhost);
  if (object_key.empty() || object_key.front() == '/') {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration, "invalid S3 object key"
    );
  }
  std::string host = endpoint.host;
  std::string target = endpoint.base_path;
  if (config_.addressing_style == "virtual_host") {
    host = config_.bucket + "." + host;
    target += "/" + uri_encode_path(object_key);
  } else {
    target += "/" + uri_encode_path(config_.bucket) + "/" + uri_encode_path(object_key);
  }
  if (target.empty() || target.front() != '/') target.insert(target.begin(), '/');
  const bool default_port = (endpoint.tls && endpoint.port == "443") ||
                            (!endpoint.tls && endpoint.port == "80");
  const auto host_header = host + (default_port ? "" : ":" + endpoint.port);
  const auto [date, amz_date] = date_now();

  std::map<std::string, std::string> headers = {
      {"host", host_header},
      {"x-amz-content-sha256", payload_sha256},
      {"x-amz-date", amz_date},
  };
  if (credentials_.session_token.has_value() && !credentials_.session_token->empty()) {
    headers["x-amz-security-token"] = *credentials_.session_token;
  }
  const auto signing = sign_s3_request_v4({
      method,
      target,
      "",
      headers,
      payload_sha256,
      config_.region,
      credentials_.access_key_id,
      credentials_.secret_access_key,
      amz_date,
      date,
  });

  auto exchange = [&](auto& stream) -> unsigned int {
    beast::flat_buffer buffer;
    if (upload != nullptr) {
      beast::error_code error;
      const auto upload_path = utf8_path(*upload);
      http::request<http::file_body> request{verb_for(method), target, 11};
      request.set(http::field::host, host_header);
      request.set("x-amz-content-sha256", payload_sha256);
      request.set("x-amz-date", amz_date);
      request.set(http::field::authorization, signing.authorization);
      if (credentials_.session_token.has_value() && !credentials_.session_token->empty()) {
        request.set("x-amz-security-token", *credentials_.session_token);
      }
      request.body().open(upload_path.c_str(), beast::file_mode::scan, error);
      if (error) throw std::runtime_error("failed to open S3 upload: " + error.message());
      request.content_length(static_cast<std::uint64_t>(content_length));
      http::write(stream, request);
      http::response<http::string_body> response;
      http::read(stream, buffer, response);
      return response.result_int();
    }

    http::request<http::empty_body> request{verb_for(method), target, 11};
    request.set(http::field::host, host_header);
    request.set("x-amz-content-sha256", payload_sha256);
    request.set("x-amz-date", amz_date);
    request.set(http::field::authorization, signing.authorization);
    if (credentials_.session_token.has_value() && !credentials_.session_token->empty()) {
      request.set("x-amz-security-token", *credentials_.session_token);
    }
    http::write(stream, request);
    if (download != nullptr) {
      std::filesystem::create_directories(download->parent_path());
      beast::error_code error;
      const auto download_path = utf8_path(*download);
      http::response_parser<http::file_body> parser;
      parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
      parser.get().body().open(download_path.c_str(), beast::file_mode::write, error);
      if (error) throw std::runtime_error("failed to open S3 download: " + error.message());
      http::read(stream, buffer, parser);
      return parser.get().result_int();
    }
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response.result_int();
  };

  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      asio::io_context context;
      tcp::resolver resolver(context);
      const auto resolved = resolver.resolve(host, endpoint.port);
      unsigned int status = 0;
      if (endpoint.tls) {
        ssl::context tls(ssl::context::tls_client);
        tls.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(context, tls);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
          throw std::runtime_error("failed to set S3 TLS hostname");
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(host));
        beast::get_lowest_layer(stream).connect(resolved);
        stream.handshake(ssl::stream_base::client);
        status = exchange(stream);
        beast::error_code ignored;
        stream.shutdown(ignored);
      } else {
        beast::tcp_stream stream(context);
        stream.connect(resolved);
        status = exchange(stream);
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
      }
      if ((status == 429 || status >= 500) && attempt < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
        continue;
      }
      return status;
    } catch (const holder::resource::StorageError&) {
      throw;
    } catch (const std::exception& ex) {
      if (attempt == 2) {
        throw holder::resource::StorageError(
            holder::resource::StorageErrorCode::Unavailable,
            std::string("S3 endpoint unavailable: ") + ex.what()
        );
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
    }
  }
  throw holder::resource::StorageError(
      holder::resource::StorageErrorCode::Unavailable, "S3 request failed"
  );
}

void S3CompatibleProvider::put(
    const std::string& object_key,
    const std::filesystem::path& staged_file,
    long long stored_size,
    const std::string& stored_sha256
) {
  const auto status =
      request("PUT", object_key, &staged_file, nullptr, stored_size, stored_sha256);
  map_status(status, "PUT");
  if (!exists(object_key)) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::Integrity, "S3 object was not visible after PUT"
    );
  }
}

void S3CompatibleProvider::get(
    const std::string& object_key,
    const std::filesystem::path& destination_file
) {
  const auto status =
      request("GET", object_key, nullptr, &destination_file, 0, kEmptySha256);
  if (status < 200 || status >= 300) {
    std::error_code ignored;
    std::filesystem::remove(destination_file, ignored);
  }
  map_status(status, "GET");
}

bool S3CompatibleProvider::exists(const std::string& object_key) {
  const auto status = request("HEAD", object_key, nullptr, nullptr, 0, kEmptySha256);
  if (status == 404) return false;
  map_status(status, "HEAD");
  return true;
}

void S3CompatibleProvider::remove(const std::string& object_key) {
  const auto status = request("DELETE", object_key, nullptr, nullptr, 0, kEmptySha256);
  if (status == 404) return;
  map_status(status, "DELETE");
}

} // namespace holder::storage
