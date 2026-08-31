#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace holder::storage::google {

// Thin wrapper around the Drive v3 REST API -- not a general Drive client, just the
// operations GoogleDriveProvider needs. All calls take a short-lived Bearer access
// token (see GoogleOAuth::refresh_access_token); none of them handle token refresh
// themselves. Every call throws holder::resource::StorageError on failure, mapping
// HTTP status the same way GoogleDriveProvider's own doc comment describes.

// Finds a file by exact name inside a known parent folder, returning its Drive fileId,
// or nullopt if no such file exists. Never returns more than one match even though
// Drive allows duplicate names within a folder -- dedup-by-content-hash already happens
// upstream of any StorageProvider, so nothing here relies on Drive-side name
// uniqueness (same reasoning as Android's DriveApi.findId).
std::optional<std::string> find_file_id(
    const std::string& access_token,
    const std::string& folder_id,
    const std::string& name
);

// Uploads staged_file's bytes as a new file named `name` inside `folder_id`. Returns
// the new file's Drive fileId.
std::string upload_file(
    const std::string& access_token,
    const std::string& folder_id,
    const std::string& name,
    const std::filesystem::path& staged_file
);

// Overwrites an existing file's content in place -- used for a retried put after a
// previous attempt's git commit failed partway, so the retry doesn't create a second
// file with the same name (same case Android's GoogleDriveStorageProvider.put handles).
void replace_file_content(
    const std::string& access_token,
    const std::string& file_id,
    const std::filesystem::path& staged_file
);

void download_file(
    const std::string& access_token,
    const std::string& file_id,
    const std::filesystem::path& destination_file
);

// A no-op (not an error) if the file is already gone.
void delete_file(const std::string& access_token, const std::string& file_id);

// Finds the well-known "Holder/Resources" folder under My Drive -- a normal,
// user-visible folder, not the hidden appDataFolder, so the user can find and recover
// their own files without Holder (see GOOGLE_DRIVE.md) -- creating it and its "Holder"
// parent if either doesn't exist yet. Returns the Resources folder's id. Only needed
// once, at connect time; GoogleDriveProvider itself is handed an already-known
// folder_id via Location.configuration and never calls this.
std::string find_or_create_holder_resources_folder(const std::string& access_token);

} // namespace holder::storage::google
