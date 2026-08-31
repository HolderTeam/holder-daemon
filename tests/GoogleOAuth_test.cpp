#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "storage/google/GoogleOAuth.h"

#include <algorithm>
#include <cctype>
#include <string>

using holder::storage::google::GoogleOAuthClient;
using holder::storage::google::PkceChallenge;

namespace {

bool is_base64url_charset(const std::string& value) {
  return std::all_of(value.begin(), value.end(), [](char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '-' || ch == '_';
  });
}

} // namespace

TEST_CASE("PKCE code_verifier meets RFC 7636's length and charset requirements", "[oauth]") {
  const auto challenge = holder::storage::google::generate_pkce_challenge();

  REQUIRE(challenge.code_verifier.size() >= 43);
  REQUIRE(challenge.code_verifier.size() <= 128);
  REQUIRE(is_base64url_charset(challenge.code_verifier));

  REQUIRE_FALSE(challenge.code_challenge.empty());
  REQUIRE(is_base64url_charset(challenge.code_challenge));
  // A SHA-256 digest is always 32 bytes; base64url with no padding encodes that as
  // exactly 43 characters (ceil(32 * 8 / 6), rounded down since there's no padding).
  REQUIRE(challenge.code_challenge.size() == 43);
}

TEST_CASE("PKCE challenges are not reused across calls", "[oauth]") {
  const auto first = holder::storage::google::generate_pkce_challenge();
  const auto second = holder::storage::google::generate_pkce_challenge();

  REQUIRE(first.code_verifier != second.code_verifier);
  REQUIRE(first.code_challenge != second.code_challenge);
}

TEST_CASE("generate_state produces a non-empty, base64url-safe, non-repeating value", "[oauth]") {
  const auto first = holder::storage::google::generate_state();
  const auto second = holder::storage::google::generate_state();

  REQUIRE_FALSE(first.empty());
  REQUIRE(is_base64url_charset(first));
  REQUIRE(first != second);
}

TEST_CASE("build_authorization_url carries every required parameter", "[oauth]") {
  const GoogleOAuthClient client{"test-client-id", "test-client-secret"};
  const auto url = holder::storage::google::build_authorization_url(
      client,
      "http://127.0.0.1:4321/locations/loc-1/oauth/google-drive/callback",
      "https://www.googleapis.com/auth/drive.file",
      "test-code-challenge",
      "test-state"
  );

  REQUIRE(url.rfind("https://accounts.google.com/o/oauth2/v2/auth?", 0) == 0);
  REQUIRE(url.find("client_id=test-client-id") != std::string::npos);
  REQUIRE(url.find("response_type=code") != std::string::npos);
  REQUIRE(url.find("code_challenge=test-code-challenge") != std::string::npos);
  REQUIRE(url.find("code_challenge_method=S256") != std::string::npos);
  REQUIRE(url.find("state=test-state") != std::string::npos);
  REQUIRE(url.find("access_type=offline") != std::string::npos);
  REQUIRE(url.find("prompt=consent") != std::string::npos);
  // The client secret must never appear in a URL the user's browser (and their history,
  // proxy logs, etc.) sees -- only the authorization code exchange, a direct
  // server-to-server POST, ever sends it.
  REQUIRE(url.find("test-client-secret") == std::string::npos);
  // redirect_uri and scope are percent-encoded in the query string, so check for their
  // encoded form rather than the raw value.
  REQUIRE(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A4321") != std::string::npos);
  REQUIRE(url.find("scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file") != std::string::npos);
}
