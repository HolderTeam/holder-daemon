#include "cli/commands/Download.h"
#include "http_test_helpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <sstream>

namespace {

namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

class DownloadServer {
 public:
  explicit DownloadServer(std::string response, std::function<void()> before_response = {})
      : acceptor_(context_) {
    boost::system::error_code error;
    acceptor_.open(tcp::v4(), error);
    if (!error) acceptor_.bind({boost::asio::ip::make_address("127.0.0.1"), 0}, error);
    if (!error) acceptor_.listen(1, error);
    if (error) SKIP("Local HTTP socket unavailable: " + error.message());
    connection.bind = "127.0.0.1";
    connection.port = acceptor_.local_endpoint().port();
    connection.token = "download-token";
    acceptor_.non_blocking(true);
    thread_ = std::thread(
        [this, response = std::move(response), before_response = std::move(before_response)] {
          tcp::socket socket(context_);
          boost::system::error_code error;
          while (!stopping_) {
            acceptor_.accept(socket, error);
            if (!error) break;
            if (error != boost::asio::error::would_block && error != boost::asio::error::try_again)
              return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          if (stopping_) return;
          boost::beast::flat_buffer buffer;
          http::read(socket, buffer, request, error);
          if (!error) {
            if (before_response) before_response();
            boost::asio::write(socket, boost::asio::buffer(response), error);
          }
          holder::test::close_http_socket(socket);
        }
    );
  }
  ~DownloadServer() { finish(); }
  void finish() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
  }
  holder::cli::DaemonConnection connection;
  http::request<http::string_body> request;

 private:
  boost::asio::io_context context_;
  tcp::acceptor acceptor_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

std::string response_bytes(const std::string& bytes, bool chunked = false) {
  std::string response =
      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"payload.bin\"\r\nConnection: close\r\n";
  if (!chunked)
    return response + "Content-Length: " + std::to_string(bytes.size()) + "\r\n\r\n" + bytes;
  response += "Transfer-Encoding: chunked\r\n\r\n";
  for (std::size_t offset = 0; offset < bytes.size(); offset += 7919) {
    const auto count = std::min<std::size_t>(7919, bytes.size() - offset);
    std::ostringstream length;
    length << std::hex << count;
    response += length.str() + "\r\n" + bytes.substr(offset, count) + "\r\n";
  }
  return response + "0\r\nX-Test-Trailer: complete\r\n\r\n";
}

std::string read_binary(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void require_no_staging_files(const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    REQUIRE(entry.path().filename().string().find(".holderctl-export-") != 0);
  }
}

} // namespace

TEST_CASE(
    "holderctl downloads preserve binary bytes metadata authentication and chunking",
    "[holderctl][download]"
) {
  std::string bytes;
  SECTION("empty") {}
  SECTION("all byte values and JSON-looking content") {
    bytes = "{\"ok\":false}";
    for (int value = 0; value < 256; ++value)
      bytes.push_back(static_cast<char>(value));
  }
  SECTION("larger than Beast's default buffered body limit") {
    bytes.resize(9 * 1024 * 1024 + 3);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<char>(i % 256);
  }
  for (bool chunked : {false, true}) {
    DownloadServer server(response_bytes(bytes, chunked));
    std::string downloaded;
    std::size_t largest_chunk = 0;
    const auto metadata = holder::cli::http_download(
        server.connection,
        "/asset/content",
        std::chrono::seconds(10),
        [&](const char* buffer, std::size_t count) {
          downloaded.append(buffer, count);
          largest_chunk = std::max(largest_chunk, count);
        }
    );
    server.finish();
    REQUIRE(downloaded == bytes);
    REQUIRE(metadata.byte_size == bytes.size());
    REQUIRE(metadata.content_type == "application/octet-stream");
    REQUIRE(metadata.filename == "payload.bin");
    REQUIRE(metadata.content_disposition == "attachment; filename=\"payload.bin\"");
    REQUIRE(largest_chunk <= 64 * 1024);
    REQUIRE(server.request.target() == "/asset/content");
    REQUIRE(server.request[http::field::authorization] == "Bearer download-token");
  }
}

TEST_CASE(
    "holderctl downloads keep structured API errors out of binary output",
    "[holderctl][download]"
) {
  const std::string body =
      R"({"ok":false,"error":{"code":"storage_unavailable","message":"Storage offline","details":{"location_id":"location-1"}}})";
  DownloadServer server(
      "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body
  );
  bool sink_called = false;
  try {
    holder::cli::http_download(
        server.connection,
        "/asset/content",
        std::chrono::seconds(10),
        [&](const char*, std::size_t) {
          sink_called = true;
        }
    );
    FAIL("Expected typed server error");
  } catch (const holder::cli::CliError& error) {
    REQUIRE(error.code() == "storage_unavailable");
    REQUIRE(error.message() == "Storage offline");
    REQUIRE(error.details()["location_id"] == "location-1");
  }
  REQUIRE_FALSE(sink_called);
}

TEST_CASE(
    "holderctl downloads do not follow redirects or emit non-JSON server errors",
    "[holderctl][download]"
) {
  for (const auto& status : {"302 Found", "401 Unauthorized", "500 Internal Server Error"}) {
    DownloadServer server(
        std::string("HTTP/1.1 ") + status +
        "\r\nContent-Length: 5\r\nLocation: http://untrusted.invalid/\r\n\r\nerror"
    );
    bool sink_called = false;
    REQUIRE_THROWS_AS(
        holder::cli::http_download(
            server.connection,
            "/asset/content",
            std::chrono::seconds(10),
            [&](const char*, std::size_t) {
              sink_called = true;
            }
        ),
        holder::cli::CliError
    );
    REQUIRE_FALSE(sink_called);
  }
}

TEST_CASE(
    "holderctl file downloads replace atomically and preserve destinations on failure",
    "[holderctl][download]"
) {
  const auto directory = holder::test::make_temp_dir();
  const auto output = directory / "export.bin";
  std::ofstream(output, std::ios::binary) << "original";
  SECTION("successful replacement, then empty replacement") {
    const std::string bytes("\0\r\n\x1a\xff", 5);
    DownloadServer server(response_bytes(bytes));
    const auto metadata = holder::cli::download_to_file(
        server.connection,
        "/asset/content",
        output,
        std::chrono::seconds(10)
    );
    REQUIRE(read_binary(output) == bytes);
    REQUIRE(metadata.byte_size == bytes.size());
    DownloadServer empty(response_bytes(""));
    holder::cli::download_to_file(
        empty.connection,
        "/asset/content",
        output,
        std::chrono::seconds(10)
    );
    REQUIRE(read_binary(output).empty());
  }
  SECTION("truncated successful HTTP body") {
    DownloadServer server("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\npartial");
    REQUIRE_THROWS_AS(
        holder::cli::download_to_file(
            server.connection,
            "/asset/content",
            output,
            std::chrono::seconds(10)
        ),
        holder::cli::CliError
    );
    REQUIRE(read_binary(output) == "original");
  }
  SECTION("server failure") {
    DownloadServer server("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\n\r\nerror");
    REQUIRE_THROWS_AS(
        holder::cli::download_to_file(
            server.connection,
            "/asset/content",
            output,
            std::chrono::seconds(10)
        ),
        holder::cli::CliError
    );
    REQUIRE(read_binary(output) == "original");
  }
  SECTION("socket deadline expires") {
    DownloadServer server(response_bytes("late bytes"), [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    });
    REQUIRE_THROWS_AS(
        holder::cli::download_to_file(
            server.connection,
            "/asset/content",
            output,
            std::chrono::seconds(1)
        ),
        holder::cli::CliError
    );
    REQUIRE(read_binary(output) == "original");
  }
  SECTION("destination appears as a directory before commit") {
    DownloadServer server(response_bytes("bytes"), [&] {
      std::filesystem::remove(output);
      std::filesystem::create_directory(output);
    });
    // A directory destination must never be removed to make replacement succeed.
    REQUIRE_THROWS_AS(
        holder::cli::download_to_file(
            server.connection,
            "/asset/content",
            output,
            std::chrono::seconds(10)
        ),
        holder::cli::CliError
    );
    REQUIRE(std::filesystem::is_directory(output));
  }
  require_no_staging_files(directory);
}
