#pragma once

#include <map>
#include <string>

namespace holder::storage {

struct S3SigningInput {
  std::string method;
  std::string canonical_uri;
  std::string canonical_query;
  std::map<std::string, std::string> headers;
  std::string payload_sha256;
  std::string region;
  std::string access_key_id;
  std::string secret_access_key;
  std::string amz_date;
  std::string date;
};

struct S3SigningResult {
  std::string authorization;
  std::string canonical_request;
  std::string string_to_sign;
  std::string signed_headers;
};

S3SigningResult sign_s3_request_v4(const S3SigningInput& input);
std::string sha256_hex(const std::string& value);

} // namespace holder::storage
