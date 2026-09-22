#include "google_storage_test_helpers.h"
#include "resource/StorageProvider.h"
#include "storage/google/DriveApi.h"
#include "storage/google/GoogleDriveProvider.h"

#include <fstream>
#include <mutex>
#include <optional>

namespace google = holder::storage::google;
using Server = holder::test::StorageHttpTestServer;
using GoogleServer = holder::test::GoogleStorageTestServer;
using Code = holder::resource::StorageErrorCode;

TEST_CASE("Drive API searches escape query literals and parse empty results", "[google_drive]") {
  std::string body = R"({"files":[{"id":"file-123"}]})";
  std::mutex response_mutex;
  GoogleServer fixture([&](const Server::Request&) {
    std::lock_guard lock(response_mutex);
    return Server::response(200, body);
  });
  REQUIRE(google::find_file_id("access-token", "folder'\\", "name'\\ +#") == "file-123");
  {
    std::lock_guard lock(response_mutex);
    body = R"({"files":[]})";
  }
  REQUIRE_FALSE(google::find_file_id("access-token", "folder", "missing").has_value());
  fixture.finish();
  REQUIRE(fixture.server.requests().size() == 2);
  const auto& request = fixture.server.requests().front();
  CHECK(request["Authorization"] == "Bearer access-token");
  CHECK(request["Host"] == "www.googleapis.com");
  CHECK(std::string(request.target()).find("%5C%27%5C%5C%20%2B%23") != std::string::npos);
  CHECK(std::string(request.target()).find("fields=files%28id%29&pageSize=1") != std::string::npos);
}

TEST_CASE("Drive API maps HTTP statuses to storage error categories", "[google_drive]") {
  for (const auto& [status, code] : std::vector<std::pair<unsigned int, Code>>{
           {400, Code::Unavailable},
           {401, Code::Authentication},
           {403, Code::Permission},
           {404, Code::Integrity},
           {429, Code::Transient},
           {503, Code::Transient}
       }) {
    CAPTURE(status);
    GoogleServer fixture([&](const Server::Request&) {
      return Server::response(status, "denied");
    });
    bool threw = false;
    try {
      (void)google::find_file_id("token", "folder", "name");
    } catch (const holder::resource::StorageError& error) {
      threw = true;
      CHECK(error.code() == code);
    }
    REQUIRE(threw);
  }
}

TEST_CASE(
    "Drive API uploads replaces downloads and deletes bytes over verified TLS",
    "[google_drive]"
) {
  const auto root = holder::test::make_temp_dir();
  const auto source = root / "source.bin";
  const auto download = root / "downloads" / "asset.bin";
  const std::string bytes("binary\0bytes", 12);
  std::ofstream(source, std::ios::binary)
      .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  GoogleServer fixture([&](const Server::Request& request) {
    if (request.method() == boost::beast::http::verb::post)
      return Server::response(200, R"({"id":"uploaded-id"})");
    if (request.method() == boost::beast::http::verb::get) return Server::response(200, bytes);
    return Server::response(204);
  });
  REQUIRE(google::upload_file("token", "folder", "label", source) == "uploaded-id");
  google::replace_file_content("token", "uploaded-id", source);
  google::download_file("token", "uploaded-id", download);
  google::delete_file("token", "uploaded-id");
  fixture.finish();
  std::ifstream read(download, std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(read), {}) == bytes);
  REQUIRE(fixture.server.requests().size() == 4);
  const auto& upload = fixture.server.requests()[0];
  CHECK(upload.target() == "/upload/drive/v3/files?uploadType=multipart");
  CHECK(upload.body().find(R"({"name":"label","parents":["folder"]})") != std::string::npos);
  CHECK(upload.body().find(bytes) != std::string::npos);
  CHECK(fixture.server.requests()[1].method() == boost::beast::http::verb::patch);
  CHECK(fixture.server.requests()[1].body() == bytes);
  CHECK(fixture.server.requests()[2].target() == "/drive/v3/files/uploaded-id?alt=media");
  CHECK(fixture.server.requests()[3].method() == boost::beast::http::verb::delete_);
}

TEST_CASE("Drive API discovers or creates both storage folders", "[google_drive]") {
  bool exists = false;
  SECTION("existing folders") { exists = true; }
  SECTION("missing folders") {}
  int queries = 0;
  int creations = 0;
  GoogleServer fixture([&](const Server::Request& request) {
    if (request.method() == boost::beast::http::verb::get) {
      ++queries;
      return Server::response(
          200,
          exists ? (queries == 1 ? R"({"files":[{"id":"holder"}]})"
                                 : R"({"files":[{"id":"resources"}]})")
                 : R"({"files":[]})"
      );
    }
    ++creations;
    return Server::response(200, creations == 1 ? R"({"id":"holder"})" : R"({"id":"resources"})");
  });
  REQUIRE(google::find_or_create_holder_resources_folder("token") == "resources");
  fixture.finish();
  CHECK(queries == 2);
  CHECK(creations == (exists ? 0 : 2));
  if (!exists) {
    const auto metadata = nlohmann::json::parse(fixture.server.requests()[3].body());
    CHECK(metadata["parents"] == nlohmann::json::array({"holder"}));
    CHECK(metadata["mimeType"] == "application/vnd.google-apps.folder");
  }
}

TEST_CASE(
    "Drive API rejects malformed replies and cleans unsuccessful downloads",
    "[google_drive]"
) {
  const auto root = holder::test::make_temp_dir();
  const auto source = root / "source.bin";
  std::ofstream(source) << "bytes";
  const auto destination = root / "downloads" / "asset.bin";
  std::string response = Server::response(200, "not-json");
  std::mutex response_mutex;
  const auto set_response = [&](std::string value) {
    std::lock_guard lock(response_mutex);
    response = std::move(value);
  };
  GoogleServer fixture([&](const Server::Request&) {
    std::lock_guard lock(response_mutex);
    return response;
  });
  REQUIRE_THROWS_AS(
      google::find_file_id("token", "folder", "name"),
      holder::resource::StorageError
  );
  REQUIRE_THROWS_AS(
      google::upload_file("token", "folder", "name", source),
      holder::resource::StorageError
  );
  REQUIRE_THROWS_AS(
      google::find_or_create_holder_resources_folder("token"),
      holder::resource::StorageError
  );
  set_response(Server::response(403, "forbidden"));
  REQUIRE_THROWS_AS(
      google::upload_file("token", "folder", "name", source),
      holder::resource::StorageError
  );
  REQUIRE_THROWS_AS(
      google::replace_file_content("token", "id", source),
      holder::resource::StorageError
  );
  REQUIRE_THROWS_AS(
      google::download_file("token", "id", destination),
      holder::resource::StorageError
  );
  CHECK_FALSE(std::filesystem::exists(destination));
  set_response("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial");
  REQUIRE_THROWS_AS(
      google::replace_file_content("token", "id", source),
      holder::resource::StorageError
  );
  REQUIRE_THROWS_AS(
      google::download_file("token", "id", destination),
      holder::resource::StorageError
  );
  CHECK_FALSE(std::filesystem::exists(destination));
  set_response(Server::response(404));
  REQUIRE_NOTHROW(google::delete_file("token", "missing"));
  REQUIRE_THROWS_AS(
      google::upload_file("token", "folder", "name", root / "missing"),
      holder::resource::StorageError
  );
  std::filesystem::create_directories(destination);
  REQUIRE_THROWS_AS(
      google::download_file("token", "id", destination),
      holder::resource::StorageError
  );
}

TEST_CASE("Google network test redirection rejects non-loopback addresses", "[google_drive]") {
  REQUIRE_THROWS_AS(
      google::set_google_endpoint_for_tests(
          boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("192.0.2.1"), 443)
      ),
      std::invalid_argument
  );
}

TEST_CASE("Drive API rejects malformed search result shapes as storage errors", "[google_drive]") {
  for (const std::string body :
       {"{}",
        R"({"files":{}})",
        R"({"files":[{}]})",
        R"({"files":[{"id":12}]})",
        R"({"files":[{"id":""}]})"}) {
    CAPTURE(body);
    GoogleServer fixture([&](const Server::Request&) {
      return Server::response(200, body);
    });
    REQUIRE_THROWS_AS(
        google::find_file_id("token", "folder", "name"),
        holder::resource::StorageError
    );
  }
}

TEST_CASE("Drive API validates ids returned by upload and folder creation", "[google_drive]") {
  const auto source = holder::test::make_temp_dir() / "source";
  std::ofstream(source) << "bytes";
  for (const std::string body : {"not-json", "{}", R"({"id":12})", R"({"id":""})"}) {
    CAPTURE(body);
    GoogleServer fixture([&](const Server::Request& request) {
      return Server::response(
          200,
          request.method() == boost::beast::http::verb::get ? R"({"files":[]})" : body
      );
    });
    REQUIRE_THROWS_AS(
        google::upload_file("token", "folder", "name", source),
        holder::resource::StorageError
    );
    REQUIRE_THROWS_AS(
        google::find_or_create_holder_resources_folder("token"),
        holder::resource::StorageError
    );
  }
}

TEST_CASE("Drive API reports interrupted metadata responses as unavailable", "[google_drive]") {
  GoogleServer fixture([](const Server::Request&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial";
  });
  REQUIRE_THROWS_AS(
      google::find_file_id("token", "folder", "name"),
      holder::resource::StorageError
  );
}
