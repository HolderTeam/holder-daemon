#pragma once

#include "resource/StorageProvider.h"
#include "storage/google/GoogleOAuth.h"

#include <filesystem>
#include <string>

namespace holder::storage::google {

struct GoogleDriveConfig {
  // The Drive fileId of the well-known "Holder/Resources" folder -- found (and created
  // if necessary) once, at connect time, by find_or_create_holder_resources_folder, and
  // stored in Location.configuration["folder_id"] from then on. GoogleDriveProvider
  // itself never creates this folder; it's handed an already-known id.
  std::string folder_id;
};

struct GoogleDriveCredentials {
  // A long-lived refresh token, obtained once through the OAuth authorization_code
  // flow and stored in LocationBindingStore's values["refresh_token"] -- never an
  // access token, which is minted fresh (and short-lived) on demand for each call.
  std::string refresh_token;
};

// StorageProvider backed by a single, well-known Google Drive folder -- the C++
// counterpart to Android's GoogleDriveStorageProvider (independently written, no
// shared code; see holder-planning's GOOGLE_DRIVE.md/S3_ANDROID.md for why). Every
// method mints a fresh access token from the stored refresh token before calling
// DriveApi; there is no token caching within a provider instance, since a fresh
// instance is constructed per request by AiResourceRoutes.cpp's storage_provider()
// factory (unlike Android's process-wide provider registration, so this has none of
// Android's "one connected account per device" limitation -- multiple Drive Locations
// on desktop each get their own provider instance from their own configuration).
class GoogleDriveProvider final : public holder::resource::StorageProvider {
 public:
  GoogleDriveProvider(
      GoogleDriveConfig config,
      GoogleDriveCredentials credentials,
      GoogleOAuthClient oauth_client
  );

  void put(
      const std::string& object_key,
      const std::filesystem::path& staged_file,
      long long stored_size,
      const std::string& stored_sha256
  ) override;
  void get(
      const std::string& object_key,
      const std::filesystem::path& destination_file
  ) override;
  bool exists(const std::string& object_key) override;
  void remove(const std::string& object_key) override;

 private:
  std::string access_token() const;

  GoogleDriveConfig config_;
  GoogleDriveCredentials credentials_;
  GoogleOAuthClient oauth_client_;
};

} // namespace holder::storage::google
