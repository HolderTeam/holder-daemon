#include "storage/google/GoogleDriveProvider.h"

#include "storage/google/DriveApi.h"

namespace holder::storage::google {

GoogleDriveProvider::GoogleDriveProvider(
    GoogleDriveConfig config,
    GoogleDriveCredentials credentials,
    GoogleOAuthClient oauth_client
)
    : config_(std::move(config)),
      credentials_(std::move(credentials)),
      oauth_client_(std::move(oauth_client)) {
  if (config_.folder_id.empty()) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::InvalidConfiguration,
        "Google Drive location is missing folder_id"
    );
  }
  if (credentials_.refresh_token.empty()) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::Authentication,
        "Google Drive is not connected -- no refresh token bound"
    );
  }
}

std::string GoogleDriveProvider::access_token() const {
  // No caching: a fresh provider instance is built per request (see this class's own
  // doc comment), so there's nothing to usefully cache an access token *in* -- the
  // 900ms-or-so round trip to mint one is the accepted cost of that simplicity, same
  // trade-off Android's GoogleDriveStorageProvider makes for the same reason.
  return refresh_access_token(oauth_client_, credentials_.refresh_token).access_token;
}

void GoogleDriveProvider::put(
    const std::string& object_key,
    const std::filesystem::path& staged_file,
    long long /*stored_size*/,
    const std::string& /*stored_sha256*/
) {
  const auto token = access_token();
  const auto existing_file_id = find_file_id(token, config_.folder_id, object_key);
  if (existing_file_id.has_value()) {
    // A retry after a previous attempt's git commit failed partway -- overwrite rather
    // than create a second file with the same name.
    replace_file_content(token, *existing_file_id, staged_file);
  } else {
    upload_file(token, config_.folder_id, object_key, staged_file);
  }
}

void GoogleDriveProvider::get(
    const std::string& object_key,
    const std::filesystem::path& destination_file
) {
  const auto token = access_token();
  const auto file_id = find_file_id(token, config_.folder_id, object_key);
  if (!file_id.has_value()) {
    throw holder::resource::StorageError(
        holder::resource::StorageErrorCode::Integrity, "no Drive file named " + object_key
    );
  }
  download_file(token, *file_id, destination_file);
}

bool GoogleDriveProvider::exists(const std::string& object_key) {
  const auto token = access_token();
  return find_file_id(token, config_.folder_id, object_key).has_value();
}

void GoogleDriveProvider::remove(const std::string& object_key) {
  const auto token = access_token();
  const auto file_id = find_file_id(token, config_.folder_id, object_key);
  if (!file_id.has_value()) return; // already gone: not a failure
  delete_file(token, *file_id);
}

} // namespace holder::storage::google
