#pragma once

#include <string>

namespace holder::storage::google {

// The Google Cloud "Desktop app" OAuth client Holder registers under the same project
// (and consent screen / Testing publishing status) as its existing Android client --
// see holder-planning/current/GOOGLE_DRIVE.md for that decision and its reasoning.
// Client id/secret are read from environment variables at daemon startup
// (HOLDER_GOOGLE_OAUTH_CLIENT_ID/HOLDER_GOOGLE_OAUTH_CLIENT_SECRET), never committed.
struct GoogleOAuthClient {
  std::string client_id;
  std::string client_secret;
};

// PKCE (RFC 7636): code_verifier is the secret the daemon keeps for the lifetime of one
// connect attempt; code_challenge is the value sent in the authorization URL. Using PKCE
// means the (not-really-confidential, for a Desktop app client) client_secret isn't the
// only thing standing between a leaked authorization code and a real token.
struct PkceChallenge {
  std::string code_verifier;
  std::string code_challenge;
};

struct GoogleTokenResponse {
  std::string access_token;
  // Only ever set on the initial authorization_code exchange -- a refresh-grant response
  // never carries a new refresh_token; callers keep using the one they already stored.
  std::string refresh_token;
  long long expires_in = 0;
  std::string scope;
  std::string token_type;
};

PkceChallenge generate_pkce_challenge();

// An opaque random string for OAuth's `state` parameter (CSRF protection) -- the
// callback route must reject any request whose `state` doesn't match what the
// corresponding authorize call generated.
std::string generate_state();

std::string build_authorization_url(
    const GoogleOAuthClient& client,
    const std::string& redirect_uri,
    const std::string& scope,
    const std::string& code_challenge,
    const std::string& state
);

// POSTs to https://oauth2.googleapis.com/token with grant_type=authorization_code.
// Throws holder::resource::StorageError on failure (Authentication for a rejected/
// expired code, Unavailable/Transient for a network or server problem).
GoogleTokenResponse exchange_authorization_code(
    const GoogleOAuthClient& client,
    const std::string& redirect_uri,
    const std::string& code,
    const std::string& code_verifier
);

// POSTs to https://oauth2.googleapis.com/token with grant_type=refresh_token. Throws
// holder::resource::StorageError the same way exchange_authorization_code does --
// Authentication specifically means the stored refresh token is no longer good (revoked,
// expired) and the Location needs reconnecting, not retrying.
GoogleTokenResponse refresh_access_token(
    const GoogleOAuthClient& client,
    const std::string& refresh_token
);

} // namespace holder::storage::google
