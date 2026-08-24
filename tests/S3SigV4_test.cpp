#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "storage/S3SigV4.h"

TEST_CASE("S3 Signature V4 matches the AWS GET object example", "[s3]") {
  const auto result = holder::storage::sign_s3_request_v4({
      "GET",
      "/test.txt",
      "",
      {{"host", "examplebucket.s3.amazonaws.com"},
       {"range", "bytes=0-9"},
       {"x-amz-content-sha256",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
       {"x-amz-date", "20130524T000000Z"}},
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "us-east-1",
      "AKIAIOSFODNN7EXAMPLE",
      "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
      "20130524T000000Z",
      "20130524",
  });

  REQUIRE(result.signed_headers == "host;range;x-amz-content-sha256;x-amz-date");
  REQUIRE(result.canonical_request.find("GET\n/test.txt\n\n") == 0);
  REQUIRE(
      result.authorization ==
      "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
      "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
      "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"
  );
}

TEST_CASE("S3 signer normalizes header names and whitespace", "[s3]") {
  const auto result = holder::storage::sign_s3_request_v4({
      "HEAD",
      "/bucket/object",
      "",
      {{"Host", " example.test  "},
       {"X-Amz-Content-Sha256",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
       {"X-Amz-Date", "20260824T120000Z"}},
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "eu-west-2",
      "KEY",
      "SECRET",
      "20260824T120000Z",
      "20260824",
  });
  REQUIRE(result.canonical_request.find("host:example.test\n") != std::string::npos);
  REQUIRE(result.authorization.find("Signature=") != std::string::npos);
}
