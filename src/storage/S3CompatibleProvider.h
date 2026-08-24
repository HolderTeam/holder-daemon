#pragma once

#include "resource/StorageProvider.h"

#include <filesystem>
#include <optional>
#include <string>

namespace holder::storage {

struct S3CompatibleConfig {
  std::string endpoint;
  std::string region;
  std::string bucket;
  std::string addressing_style = "path";
  bool allow_insecure_localhost = false;
};

struct S3Credentials {
  std::string access_key_id;
  std::string secret_access_key;
  std::optional<std::string> session_token;
};

class S3CompatibleProvider final : public holder::resource::StorageProvider {
 public:
  S3CompatibleProvider(S3CompatibleConfig config, S3Credentials credentials);

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
  unsigned int request(
      const std::string& method,
      const std::string& object_key,
      const std::filesystem::path* upload,
      const std::filesystem::path* download,
      long long content_length,
      const std::string& payload_sha256
  );

  S3CompatibleConfig config_;
  S3Credentials credentials_;
};

} // namespace holder::storage
