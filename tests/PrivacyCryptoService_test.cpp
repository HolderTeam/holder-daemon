#include "privacy/CryptoService.h"
#include "privacy/PrivacyError.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

TEST_CASE("PrivacyCryptoService envelope round-trip", "[privacy]") {
  const auto key = holder::privacy::generate_random_key();
  const std::string key_id = "key-123";
  const std::string plaintext = "# Card\nBody\n";

  const std::string envelope = holder::privacy::encrypt_envelope_v1(plaintext, key, key_id);
  REQUIRE(envelope.rfind("HolderPriv1\n", 0) == 0);

  const std::string decrypted = holder::privacy::decrypt_envelope_v1(envelope, key, key_id);
  REQUIRE(decrypted == plaintext);
}

TEST_CASE("PrivacyCryptoService rejects tampered ciphertext", "[privacy]") {
  const auto key = holder::privacy::generate_random_key();
  const std::string key_id = "key-123";
  const std::string plaintext = "hello";
  std::string envelope = holder::privacy::encrypt_envelope_v1(plaintext, key, key_id);

  // Flip a byte in the base64 ciphertext line.
  const auto first_nl = envelope.find('\n');
  const auto second_nl = envelope.find('\n', first_nl + 1);
  REQUIRE(second_nl != std::string::npos);
  auto third_nl = envelope.find('\n', second_nl + 1);
  if (third_nl == std::string::npos) {
    third_nl = envelope.size();
  }
  REQUIRE(third_nl > second_nl + 2);
  envelope[second_nl + 2] = (envelope[second_nl + 2] == 'A') ? 'B' : 'A';

  REQUIRE_THROWS_AS(holder::privacy::decrypt_envelope_v1(envelope, key, key_id),
                    holder::privacy::PrivacyError);
}

TEST_CASE("PrivacyCryptoService rejects key_id mismatch", "[privacy]") {
  const auto key = holder::privacy::generate_random_key();
  const std::string envelope = holder::privacy::encrypt_envelope_v1("hello", key, "key-a");

  try {
    (void)holder::privacy::decrypt_envelope_v1(envelope, key, "key-b");
    FAIL("Expected privacy error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::EnvelopeMetadataMismatch);
  }
}

TEST_CASE("PrivacyCryptoService key base64 round-trip", "[privacy]") {
  const auto key = holder::privacy::generate_random_key();
  const auto encoded = holder::privacy::key_to_base64(key);
  const auto decoded = holder::privacy::key_from_base64(encoded);
  REQUIRE(decoded == key);
}
