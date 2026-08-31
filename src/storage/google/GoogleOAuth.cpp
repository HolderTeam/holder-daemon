#include "storage/google/GoogleOAuth.h"

#include "resource/StorageProvider.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace holder::storage::google {
namespace {

using holder::resource::StorageError;
using holder::resource::StorageErrorCode;

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

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    if (!out.empty()) out += "&";
    out += url_encode(key) + "=" + url_encode(value);
  }
  return out;
}

// OpenSSL's EVP_EncodeBlock produces standard base64 with padding; RFC 7636 wants the
// URL-safe alphabet with no padding, so translate + -> -, / -> _, and stop at the first
// padding character.
std::string base64url_no_pad(const std::vector<unsigned char>& bytes) {
  std::string standard;
  standard.resize(4 * ((bytes.size() + 2) / 3));
  const auto written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(standard.data()), bytes.data(),
      static_cast<int>(bytes.size())
  );
  standard.resize(static_cast<std::size_t>(written));
  std::string out;
  out.reserve(standard.size());
  for (const char ch : standard) {
    if (ch == '+') {
      out.push_back('-');
    } else if (ch == '/') {
      out.push_back('_');
    } else if (ch == '=') {
      break;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::vector<unsigned char> random_bytes(std::size_t count) {
  std::vector<unsigned char> bytes(count);
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("failed to generate random bytes for OAuth");
  }
  return bytes;
}

std::vector<unsigned char> sha256_bytes(const std::string& value) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), EVP_MD_CTX_free
  );
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) {
    throw std::runtime_error("SHA-256 failed while building a PKCE challenge");
  }
  std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1) {
    throw std::runtime_error("SHA-256 failed while building a PKCE challenge");
  }
  digest.resize(size);
  return digest;
}

// POSTs application/x-www-form-urlencoded to a fixed host. Same request/response shape
// as CloudClient.cpp's https_post_json, deliberately duplicated rather than shared --
// storage-provider code in this codebase is independently written per provider by
// design (see holder-planning's S3_ANDROID.md), and this needs a different content type
// and body serialization than that helper provides anyway.
bool https_post_form(
    const std::string& host,
    const std::string& target,
    const std::string& form_body,
    int* out_status,
    std::string* out_body,
    std::string* error
) {
  namespace http = boost::beast::http;
  namespace ssl = boost::asio::ssl;
  using tcp = boost::asio::ip::tcp;

  try {
    boost::asio::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    const auto endpoints = resolver.resolve(host, "443");

    boost::beast::ssl_stream<boost::beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
      if (error) *error = "failed to set tls host name";
      return false;
    }
    boost::beast::get_lowest_layer(stream).connect(endpoints);
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(host));
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "holder/google-drive");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    req.body() = form_body;
    req.prepare_payload();

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.shutdown(ec);
    if (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated) {
      ec = {};
    }

    if (out_status) *out_status = static_cast<int>(res.result_int());
    if (out_body) *out_body = res.body();
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

GoogleTokenResponse parse_token_response(const std::string& body) {
  const auto json = nlohmann::json::parse(body);
  GoogleTokenResponse out;
  out.access_token = json.value("access_token", std::string());
  out.refresh_token = json.value("refresh_token", std::string());
  out.expires_in = json.value("expires_in", static_cast<long long>(0));
  out.scope = json.value("scope", std::string());
  out.token_type = json.value("token_type", std::string());
  return out;
}

GoogleTokenResponse token_request(const std::vector<std::pair<std::string, std::string>>& fields) {
  int status = 0;
  std::string body;
  std::string error;
  if (!https_post_form(
          "oauth2.googleapis.com", "/token", form_encode(fields), &status, &body, &error
      )) {
    throw StorageError(
        StorageErrorCode::Unavailable, "Google token endpoint unreachable: " + error
    );
  }
  if (status == 400 || status == 401) {
    // Google returns 400 for both a malformed request and an invalid/expired/revoked
    // grant (e.g. a revoked refresh token) -- either way, re-authorizing is the fix, not
    // a retry, so both map to Authentication rather than a generic failure.
    throw StorageError(
        StorageErrorCode::Authentication, "Google token request rejected: " + body
    );
  }
  if (status != 200) {
    throw StorageError(
        StorageErrorCode::Transient,
        "Google token request failed: HTTP " + std::to_string(status)
    );
  }
  try {
    return parse_token_response(body);
  } catch (const std::exception& ex) {
    throw StorageError(
        StorageErrorCode::Unavailable,
        std::string("Google token response could not be parsed: ") + ex.what()
    );
  }
}

} // namespace

PkceChallenge generate_pkce_challenge() {
  PkceChallenge out;
  // 32 random bytes -> 43 base64url characters, comfortably within RFC 7636's
  // required 43-128 character range for a code_verifier.
  out.code_verifier = base64url_no_pad(random_bytes(32));
  out.code_challenge = base64url_no_pad(sha256_bytes(out.code_verifier));
  return out;
}

std::string generate_state() {
  return base64url_no_pad(random_bytes(24));
}

std::string build_authorization_url(
    const GoogleOAuthClient& client,
    const std::string& redirect_uri,
    const std::string& scope,
    const std::string& code_challenge,
    const std::string& state
) {
  return "https://accounts.google.com/o/oauth2/v2/auth?" +
         form_encode({
             {"client_id", client.client_id},
             {"redirect_uri", redirect_uri},
             {"response_type", "code"},
             {"scope", scope},
             {"code_challenge", code_challenge},
             {"code_challenge_method", "S256"},
             {"state", state},
             // Without access_type=offline Google never issues a refresh_token at all;
             // without prompt=consent it only issues one on a user's very first consent
             // for this client, which would make reconnecting after a revoke silently
             // stop working. Always forcing the consent screen keeps reconnecting
             // reliable at the cost of an extra click every time.
             {"access_type", "offline"},
             {"prompt", "consent"},
         });
}

GoogleTokenResponse exchange_authorization_code(
    const GoogleOAuthClient& client,
    const std::string& redirect_uri,
    const std::string& code,
    const std::string& code_verifier
) {
  return token_request({
      {"client_id", client.client_id},
      {"client_secret", client.client_secret},
      {"code", code},
      {"code_verifier", code_verifier},
      {"grant_type", "authorization_code"},
      {"redirect_uri", redirect_uri},
  });
}

GoogleTokenResponse refresh_access_token(
    const GoogleOAuthClient& client,
    const std::string& refresh_token
) {
  return token_request({
      {"client_id", client.client_id},
      {"client_secret", client.client_secret},
      {"refresh_token", refresh_token},
      {"grant_type", "refresh_token"},
  });
}

} // namespace holder::storage::google
