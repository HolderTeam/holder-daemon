#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "resource/AssetEnvelope.h"
#include "resource/StorageProvider.h"
#include "storage/google/GoogleDriveProvider.h"
#include "storage/google/GoogleOAuth.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string required_environment(const char* name) {
  const auto* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    SKIP(std::string("Set ") + name + " to run the Google Drive integration test");
  }
  return value;
}

} // namespace

TEST_CASE("Google Drive provider rejects a missing folder_id", "[google_drive]") {
  holder::storage::google::GoogleDriveConfig config; // folder_id left empty
  holder::storage::google::GoogleDriveCredentials credentials{"some-refresh-token"};
  holder::storage::google::GoogleOAuthClient client{"client-id", "client-secret"};

  REQUIRE_THROWS_AS(
      holder::storage::google::GoogleDriveProvider(config, credentials, client),
      holder::resource::StorageError
  );
}

TEST_CASE("Google Drive provider rejects a missing refresh token", "[google_drive]") {
  holder::storage::google::GoogleDriveConfig config{"some-folder-id"};
  holder::storage::google::GoogleDriveCredentials credentials; // refresh_token left empty
  holder::storage::google::GoogleOAuthClient client{"client-id", "client-secret"};

  REQUIRE_THROWS_AS(
      holder::storage::google::GoogleDriveProvider(config, credentials, client),
      holder::resource::StorageError
  );
}

// Live/opt-in only -- exercises a real refresh-token exchange and a real Drive folder,
// same shape and same reasoning as S3CompatibleProvider_test.cpp's own
// "round-trips an object" integration test: no test double stands in for Google's
// servers, so this only runs when a real refresh token and OAuth client are provided.
// Setting these up: connect Google Drive once through any existing client (Android, or
// a manual OAuth code exchange) to obtain a refresh token, and use the Drive UI or API
// to find the Location's Location.configuration["folder_id"] for the resulting
// "Holder/Resources" folder.
TEST_CASE("Google Drive provider round-trips an object", "[google_drive][integration]") {
  const auto client_id = required_environment("HOLDER_TEST_GOOGLE_OAUTH_CLIENT_ID");
  const auto client_secret = required_environment("HOLDER_TEST_GOOGLE_OAUTH_CLIENT_SECRET");
  const auto refresh_token = required_environment("HOLDER_TEST_GOOGLE_DRIVE_REFRESH_TOKEN");
  const auto folder_id = required_environment("HOLDER_TEST_GOOGLE_DRIVE_FOLDER_ID");

  holder::storage::google::GoogleDriveProvider provider(
      holder::storage::google::GoogleDriveConfig{folder_id},
      holder::storage::google::GoogleDriveCredentials{refresh_token},
      holder::storage::google::GoogleOAuthClient{client_id, client_secret}
  );

  const auto nonce =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root =
      std::filesystem::temp_directory_path() / ("holder_google_drive_test_" + nonce);
  std::filesystem::create_directories(root);
  const auto source = root / "source.bin";
  const auto recovered = root / "recovered.bin";
  std::ofstream(source, std::ios::binary)
      << "Holder Google Drive integration test\n"
      << nonce;
  const auto digest = holder::resource::digest_file(source);
  const auto object_key = "holder-integration-tests-" + nonce + ".bin";

  try {
    provider.put(object_key, source, digest.byte_size, digest.sha256);
    REQUIRE(provider.exists(object_key));
    provider.get(object_key, recovered);
    REQUIRE(holder::resource::digest_file(recovered).sha256 == digest.sha256);
    provider.remove(object_key);
    REQUIRE_FALSE(provider.exists(object_key));
  } catch (...) {
    try {
      provider.remove(object_key);
    } catch (...) {
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    throw;
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}
