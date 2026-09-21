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
  REQUIRE(
      url.find("scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file") != std::string::npos
  );
}

#include "google_storage_test_helpers.h"
#include "resource/StorageProvider.h"

TEST_CASE("Google OAuth exchanges and refreshes encoded credentials over verified TLS", "[oauth]") {
  using Server = holder::test::StorageHttpTestServer;
  holder::test::GoogleStorageTestServer fixture([](const Server::Request&) {
    return Server::response(
        200,
        R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"scope":"drive.file","token_type":"Bearer"})"
    );
  });
  const GoogleOAuthClient client{"client +&", "secret=&#"};
  const auto token = holder::storage::google::exchange_authorization_code(
      client,
      "http://localhost/callback",
      "code+",
      "verifier~"
  );
  CHECK(token.access_token == "access");
  CHECK(token.refresh_token == "refresh");
  CHECK(token.expires_in == 3600);
  CHECK(token.scope == "drive.file");
  CHECK(token.token_type == "Bearer");
  CHECK(
      holder::storage::google::refresh_access_token(client, "refresh+&").access_token == "access"
  );
  fixture.finish();
  REQUIRE(fixture.server.requests().size() == 2);
  const auto& exchange = fixture.server.requests()[0];
  CHECK(exchange.target() == "/token");
  CHECK(exchange["Host"] == "oauth2.googleapis.com");
  CHECK(exchange["Content-Type"] == "application/x-www-form-urlencoded");
  CHECK(
      exchange.body() ==
      "client_id=client%20%2B%26&client_secret=secret%3D%26%23&code=code%2B&code_verifier=verifier~&grant_type=authorization_code&redirect_uri=http%3A%2F%2Flocalhost%2Fcallback"
  );
  CHECK(
      fixture.server.requests()[1].body() ==
      "client_id=client%20%2B%26&client_secret=secret%3D%26%23&refresh_token=refresh%2B%26&grant_type=refresh_token"
  );
}

TEST_CASE("Google OAuth classifies rejected and malformed token responses", "[oauth]") {
  using Server = holder::test::StorageHttpTestServer;
  using Code = holder::resource::StorageErrorCode;
  struct Reply {
    unsigned status;
    std::string body;
    Code code;
  };
  for (const auto& reply : std::vector<Reply>{
           {400, "invalid_grant", Code::Authentication},
           {401, "invalid_client", Code::Authentication},
           {503, "unavailable", Code::Transient},
           {200, "not-json", Code::Unavailable},
           {200, "{}", Code::Unavailable},
           {200, R"({"access_token":""})", Code::Unavailable},
           {200, R"({"access_token":12})", Code::Unavailable}
       }) {
    CAPTURE(reply.status, reply.body);
    holder::test::GoogleStorageTestServer fixture([&](const Server::Request&) {
      return Server::response(reply.status, reply.body);
    });
    bool threw = false;
    try {
      (void)holder::storage::google::refresh_access_token({"id", "secret"}, "refresh");
    } catch (const holder::resource::StorageError& error) {
      threw = true;
      CHECK(error.code() == reply.code);
    }
    REQUIRE(threw);
  }
}

TEST_CASE("Google OAuth reports interrupted responses as unavailable", "[oauth]") {
  using Server = holder::test::StorageHttpTestServer;
  holder::test::GoogleStorageTestServer fixture([](const Server::Request&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial";
  });
  REQUIRE_THROWS_AS(
      holder::storage::google::refresh_access_token({"id", "secret"}, "refresh"),
      holder::resource::StorageError
  );
}

TEST_CASE("Google OAuth requires both configured client credentials", "[oauth]") {
  holder::test::EnvGuard client_id("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "client");
  holder::test::EnvGuard client_secret("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "secret");
  CHECK(holder::storage::google::google_oauth_client_from_env().client_id == "client");
  CHECK(holder::storage::google::google_oauth_client_from_env().client_secret == "secret");
  SECTION("missing id") {
    holder::test::EnvGuard missing("HOLDER_GOOGLE_OAUTH_CLIENT_ID", std::nullopt);
    REQUIRE_THROWS(holder::storage::google::google_oauth_client_from_env());
  }
  SECTION("missing secret") {
    holder::test::EnvGuard missing("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", std::nullopt);
    REQUIRE_THROWS(holder::storage::google::google_oauth_client_from_env());
  }
}
