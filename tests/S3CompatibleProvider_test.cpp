#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "resource/AssetEnvelope.h"
#include "resource/StorageProvider.h"
#include "storage/S3CompatibleProvider.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string required_environment(const char* name) {
  const auto* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    SKIP(std::string("Set ") + name + " to run the S3-compatible integration test");
  }
  return value;
}

} // namespace

TEST_CASE("S3-compatible provider round-trips an object", "[s3][integration]") {
  const auto endpoint = required_environment("HOLDER_TEST_S3_ENDPOINT");
  const auto bucket = required_environment("HOLDER_TEST_S3_BUCKET");
  const auto access_key = required_environment("HOLDER_TEST_S3_ACCESS_KEY_ID");
  const auto secret_key = required_environment("HOLDER_TEST_S3_SECRET_ACCESS_KEY");
  const auto* region_value = std::getenv("HOLDER_TEST_S3_REGION");
  const std::string region = region_value && *region_value ? region_value : "us-east-1";

  holder::storage::S3CompatibleConfig config{
      .endpoint = endpoint,
      .region = region,
      .bucket = bucket,
      .addressing_style = "path",
      .allow_insecure_localhost = endpoint.rfind("http://localhost", 0) == 0 ||
                                  endpoint.rfind("http://127.0.0.1", 0) == 0,
  };
  holder::storage::S3Credentials credentials{
      .access_key_id = access_key,
      .secret_access_key = secret_key,
      .session_token = std::nullopt,
  };
  holder::storage::S3CompatibleProvider provider(config, credentials);

  const auto nonce = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()
  );
  const auto root = std::filesystem::temp_directory_path() / ("holder_s3_test_" + nonce);
  std::filesystem::create_directories(root);
  const auto source = root / "source.bin";
  const auto recovered = root / "recovered.bin";
  std::ofstream(source, std::ios::binary) << "Holder S3-compatible integration test\n" << nonce;
  const auto digest = holder::resource::digest_file(source);
  const auto object_key = "holder-integration-tests/" + nonce + ".bin";

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
