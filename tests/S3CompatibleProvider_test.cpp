#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "resource/AssetEnvelope.h"
#include "resource/StorageProvider.h"
#include "storage/S3CompatibleProvider.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::string required_environment(const char* name) {
  const auto* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    SKIP(std::string("Set ") + name + " to run the S3-compatible integration test");
  }
  return value;
}

class BodylessHeadServer {
 public:
  BodylessHeadServer()
      : acceptor_(
            context_,
            {boost::asio::ip::make_address("127.0.0.1"), 0}
        ),
        thread_([this]() {
          serve();
        }) {}

  ~BodylessHeadServer() {
    if (thread_.joinable()) thread_.join();
  }

  BodylessHeadServer(const BodylessHeadServer&) = delete;
  BodylessHeadServer& operator=(const BodylessHeadServer&) = delete;

  std::uint16_t port() const {
    return acceptor_.local_endpoint().port();
  }

  void finish() {
    if (thread_.joinable()) thread_.join();
    if (error_) std::rethrow_exception(error_);
  }

  const std::string& request_method() const {
    return request_method_;
  }

 private:
  void serve() {
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    try {
      tcp::socket socket(context_);
      acceptor_.accept(socket);

      boost::beast::flat_buffer buffer;
      http::request<http::empty_body> request;
      http::read(socket, buffer, request);
      request_method_ = std::string(request.method_string());

      // S3 servers legitimately return the corresponding object's length in
      // HEAD response headers but no body bytes. Keep the connection alive
      // briefly to reproduce the behaviour that made a normal body parser
      // wait against MinIO.
      const std::string response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Length: 4096\r\n"
          "Content-Type: application/octet-stream\r\n"
          "Connection: keep-alive\r\n"
          "\r\n";
      boost::asio::write(socket, boost::asio::buffer(response));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      boost::system::error_code ignored;
      socket.shutdown(tcp::socket::shutdown_both, ignored);
    } catch (...) {
      error_ = std::current_exception();
    }
  }

  boost::asio::io_context context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::thread thread_;
  std::exception_ptr error_;
  std::string request_method_;
};

} // namespace

TEST_CASE(
    "S3-compatible provider accepts bodyless HEAD with object Content-Length",
    "[s3]"
) {
  BodylessHeadServer server;
  holder::storage::S3CompatibleConfig config{
      .endpoint = "http://127.0.0.1:" + std::to_string(server.port()),
      .region = "us-east-1",
      .bucket = "holder-test",
      .addressing_style = "path",
      .allow_insecure_localhost = true,
  };
  holder::storage::S3Credentials credentials{
      .access_key_id = "test-access-key",
      .secret_access_key = "test-secret-key",
      .session_token = std::nullopt,
  };
  holder::storage::S3CompatibleProvider provider(config, credentials);

  REQUIRE(provider.exists("objects/example.bin"));
  server.finish();
  REQUIRE(server.request_method() == "HEAD");
}

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
