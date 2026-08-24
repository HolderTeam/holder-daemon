#include "storage/S3SigV4.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace holder::storage {
namespace {

std::string hex(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < size; ++index) out << std::setw(2) << +bytes[index];
  return out.str();
}

std::vector<unsigned char> hmac(
    const unsigned char* key,
    std::size_t key_size,
    const std::string& value
) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int size = 0;
  if (HMAC(
          EVP_sha256(),
          key,
          static_cast<int>(key_size),
          reinterpret_cast<const unsigned char*>(value.data()),
          value.size(),
          output.data(),
          &size
      ) == nullptr) {
    throw std::runtime_error("S3 HMAC-SHA256 failed");
  }
  return {output.begin(), output.begin() + size};
}

std::string trim_and_collapse(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  std::string out;
  bool whitespace = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch)) {
      whitespace = true;
    } else {
      if (whitespace && !out.empty()) out.push_back(' ');
      out.push_back(static_cast<char>(ch));
      whitespace = false;
    }
  }
  return out;
}

} // namespace

std::string sha256_hex(const std::string& value) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), EVP_MD_CTX_free
  );
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) {
    throw std::runtime_error("S3 SHA-256 failed");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1) {
    throw std::runtime_error("S3 SHA-256 failed");
  }
  return hex(digest.data(), size);
}

S3SigningResult sign_s3_request_v4(const S3SigningInput& input) {
  if (input.method.empty() || input.canonical_uri.empty() || input.region.empty() ||
      input.access_key_id.empty() || input.secret_access_key.empty() || input.amz_date.empty() ||
      input.date.size() != 8 || input.payload_sha256.size() != 64) {
    throw std::invalid_argument("incomplete S3 signing input");
  }
  std::map<std::string, std::string> headers;
  for (const auto& [name, value] : input.headers) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    headers[lower] = trim_and_collapse(value);
  }
  if (!headers.contains("host") || !headers.contains("x-amz-date") ||
      !headers.contains("x-amz-content-sha256")) {
    throw std::invalid_argument("S3 signing headers are incomplete");
  }

  std::string canonical_headers;
  std::string signed_headers;
  for (const auto& [name, value] : headers) {
    canonical_headers += name + ":" + value + "\n";
    if (!signed_headers.empty()) signed_headers += ";";
    signed_headers += name;
  }
  const auto canonical_request = input.method + "\n" + input.canonical_uri + "\n" +
                                 input.canonical_query + "\n" + canonical_headers + "\n" +
                                 signed_headers + "\n" + input.payload_sha256;
  const auto scope = input.date + "/" + input.region + "/s3/aws4_request";
  const auto string_to_sign =
      "AWS4-HMAC-SHA256\n" + input.amz_date + "\n" + scope + "\n" +
      sha256_hex(canonical_request);

  const auto initial_key = "AWS4" + input.secret_access_key;
  const auto date_key = hmac(
      reinterpret_cast<const unsigned char*>(initial_key.data()), initial_key.size(), input.date
  );
  const auto region_key = hmac(date_key.data(), date_key.size(), input.region);
  const auto service_key = hmac(region_key.data(), region_key.size(), "s3");
  const auto signing_key = hmac(service_key.data(), service_key.size(), "aws4_request");
  const auto signature = hmac(signing_key.data(), signing_key.size(), string_to_sign);
  return {
      "AWS4-HMAC-SHA256 Credential=" + input.access_key_id + "/" + scope +
          ", SignedHeaders=" + signed_headers + ", Signature=" +
          hex(signature.data(), signature.size()),
      canonical_request,
      string_to_sign,
      signed_headers,
  };
}

} // namespace holder::storage
