#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "http_test_helpers.h"
#include "resource/AssetEnvelope.h"
#include "resource/StorageProvider.h"
#include "storage/S3CompatibleProvider.h"
#include "storage_http_test_server.h"

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
      : acceptor_(context_, {boost::asio::ip::make_address("127.0.0.1"), 0}),
        thread_([this]() {
          serve();
        }) {}

  ~BodylessHeadServer() {
    if (thread_.joinable()) thread_.join();
  }

  BodylessHeadServer(const BodylessHeadServer&) = delete;
  BodylessHeadServer& operator=(const BodylessHeadServer&) = delete;

  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

  void finish() {
    if (thread_.joinable()) thread_.join();
    if (error_) std::rethrow_exception(error_);
  }

  const std::string& request_method() const { return request_method_; }

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
      const std::string response = "HTTP/1.1 200 OK\r\n"
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

TEST_CASE("S3-compatible provider accepts bodyless HEAD with object Content-Length", "[s3]") {
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

  const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
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

TEST_CASE("S3 provider streams signed operations through a local endpoint", "[s3]") {
  using Server = holder::test::StorageHttpTestServer;
  bool tls = false;
  SECTION("HTTP on explicitly allowed loopback") {}
  SECTION("TLS with a trusted local certificate") { tls = true; }
  holder::test::EnvGuard trust("SSL_CERT_FILE", Server::certificate_file());
  std::string stored;
  bool present = false;
  Server server(
      [&](const Server::Request& request) {
        if (request.method() == boost::beast::http::verb::put) {
          stored = request.body();
          present = true;
          return Server::response(200);
        }
        if (request.method() == boost::beast::http::verb::get) return Server::response(200, stored);
        if (request.method() == boost::beast::http::verb::delete_) {
          present = false;
          return Server::response(204);
        }
        return Server::response(present ? 200 : 404);
      },
      tls
  );
  holder::storage::S3CompatibleProvider provider(
      {server.endpoint() + "/storage///", "us-east-1", "test bucket", "path", true},
      {"access", "secret", "session-token"}
  );
  const auto root = holder::test::make_temp_dir();
  const auto source = root / "source.bin";
  const auto destination = root / "downloads" / "result.bin";
  const std::string bytes("asset\0bytes", 11);
  std::ofstream(source, std::ios::binary)
      .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  const auto digest = holder::resource::digest_file(source);
  REQUIRE_FALSE(provider.exists("folder/a +#.bin"));
  provider.put("folder/a +#.bin", source, digest.byte_size, digest.sha256);
  provider.get("folder/a +#.bin", destination);
  CHECK(holder::resource::digest_file(destination).sha256 == digest.sha256);
  provider.remove("folder/a +#.bin");
  CHECK_FALSE(provider.exists("folder/a +#.bin"));
  server.stop();
  server.rethrow_error();
  REQUIRE(server.requests().size() == 6);
  CHECK(stored == bytes);
  for (const auto& request : server.requests()) {
    CHECK(request.target() == "/storage/test%20bucket/folder/a%20%2B%23.bin");
    CHECK(request["x-amz-security-token"] == "session-token");
    CHECK(std::string(request["Authorization"]).find("Credential=access/") != std::string::npos);
    CHECK(request["x-amz-date"].size() == 16);
  }
  CHECK(server.requests()[1]["x-amz-content-sha256"] == digest.sha256);
}

TEST_CASE("S3 provider maps failures and bounds retries", "[s3]") {
  using Server = holder::test::StorageHttpTestServer;
  using Code = holder::resource::StorageErrorCode;
  for (const auto& [status, code] : std::vector<std::pair<unsigned int, Code>>{
           {400, Code::Unavailable},
           {401, Code::Authentication},
           {403, Code::Permission},
           {404, Code::Unavailable},
           {429, Code::Transient},
           {503, Code::Transient}
       }) {
    CAPTURE(status);
    Server server([&](const Server::Request&) {
      return Server::response(status, "rejected");
    });
    holder::storage::S3CompatibleProvider provider(
        {server.endpoint(), "us-east-1", "bucket", "path", true},
        {"access", "secret", std::nullopt}
    );
    const auto destination = holder::test::make_temp_dir() / "download.bin";
    bool threw = false;
    try {
      provider.get("object", destination);
    } catch (const holder::resource::StorageError& error) {
      threw = true;
      CHECK(error.code() == code);
    }
    REQUIRE(threw);
    CHECK_FALSE(std::filesystem::exists(destination));
    server.stop();
    server.rethrow_error();
    CHECK(server.requests().size() == ((status == 429 || status >= 500) ? 3 : 1));
  }
}

TEST_CASE("S3 provider retries transient responses and rejects invisible uploads", "[s3]") {
  using Server = holder::test::StorageHttpTestServer;
  int attempt = 0;
  Server server([&](const Server::Request& request) {
    if (request.method() == boost::beast::http::verb::put) return Server::response(200);
    return Server::response(++attempt <= 2 ? 503 : 404);
  });
  holder::storage::S3CompatibleProvider provider(
      {server.endpoint(), "us-east-1", "bucket", "path", true},
      {"access", "secret", std::nullopt}
  );
  const auto source = holder::test::make_temp_dir() / "source.bin";
  std::ofstream(source) << "content";
  const auto digest = holder::resource::digest_file(source);
  bool threw = false;
  try {
    provider.put("object", source, digest.byte_size, digest.sha256);
  } catch (const holder::resource::StorageError& error) {
    threw = true;
    CHECK(error.code() == holder::resource::StorageErrorCode::Integrity);
  }
  REQUIRE(threw);
  provider.remove("object"); // Already absent is idempotent.
  server.stop();
  server.rethrow_error();
  CHECK(server.requests().size() == 5);
}

TEST_CASE("S3 provider removes partial downloads after a broken response", "[s3]") {
  using Server = holder::test::StorageHttpTestServer;
  Server server([](const Server::Request&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial";
  });
  holder::storage::S3CompatibleProvider provider(
      {server.endpoint(), "us-east-1", "bucket", "path", true},
      {"access", "secret", std::nullopt}
  );
  const auto destination = holder::test::make_temp_dir() / "partial.bin";
  REQUIRE_THROWS_AS(provider.get("object", destination), holder::resource::StorageError);
  CHECK_FALSE(std::filesystem::exists(destination));
  server.stop();
  server.rethrow_error();
  CHECK(server.requests().size() == 3);
}

TEST_CASE("S3 provider rejects invalid configuration and object keys", "[s3]") {
  using Provider = holder::storage::S3CompatibleProvider;
  using Error = holder::resource::StorageError;
  holder::storage::S3CompatibleConfig
      config{"https://storage.invalid", "region", "bucket", "path", false};
  holder::storage::S3Credentials credentials{"access", "secret", std::nullopt};
  SECTION("required configuration") {
    for (auto* value :
         {&config.endpoint,
          &config.region,
          &config.bucket,
          &credentials.access_key_id,
          &credentials.secret_access_key}) {
      const auto saved = *value;
      value->clear();
      CHECK_THROWS_AS(Provider(config, credentials), Error);
      *value = saved;
    }
  }
  SECTION("addressing style") {
    config.addressing_style = "unknown";
    CHECK_THROWS_AS(Provider(config, credentials), Error);
  }
  SECTION("endpoint validation") {
    for (const std::string endpoint :
         {"ftp://storage.invalid", "https:///bucket", "http://storage.invalid", "http://localhost"
         }) {
      config.endpoint = endpoint;
      CHECK_THROWS_AS(Provider(config, credentials), Error);
    }
    config.allow_insecure_localhost = true;
    config.endpoint = "http://storage.invalid";
    CHECK_THROWS_AS(Provider(config, credentials), Error);
  }
  SECTION("object keys") {
    Provider provider(config, credentials);
    CHECK_THROWS_AS(provider.exists(""), Error);
    CHECK_THROWS_AS(provider.exists("/absolute"), Error);
  }
}

TEST_CASE("S3 virtual host addressing signs the bucket hostname and object path", "[s3]") {
  using Server = holder::test::StorageHttpTestServer;
  holder::test::EnvGuard trust("SSL_CERT_FILE", Server::certificate_file());
  Server server(
      [](const Server::Request&) {
        return Server::response(200);
      },
      true
  );
  // Prefixing the bucket creates 127.0.0.1 without depending on wildcard DNS.
  holder::storage::S3CompatibleProvider provider(
      {"https://0.0.1:" + std::to_string(server.address().port()) + "/base",
       "region",
       "127",
       "virtual_host",
       false},
      {"access", "secret", std::nullopt}
  );
  CHECK(provider.exists("folder/a b"));
  server.stop();
  server.rethrow_error();
  REQUIRE(server.requests().size() == 1);
  CHECK(server.requests()[0].target() == "/base/folder/a%20b");
  CHECK(server.requests()[0]["Host"] == "127.0.0.1:" + std::to_string(server.address().port()));
}
