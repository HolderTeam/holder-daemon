#include "api/support/EventJournal.h"
#include "http_test_helpers.h"
#include <sstream>

namespace {
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

class EventClient {
 public:
  EventClient(unsigned short port, std::string target, std::string cursor = {})
      : socket_(ioc_) {
    socket_.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::authorization, "Bearer testtoken");
    if (!cursor.empty()) req.set("Last-Event-ID", cursor);
    http::write(socket_, req);
    socket_.non_blocking(true);
    const auto header = read_until("\r\n\r\n");
    REQUIRE(header.find("200 OK") != std::string::npos);
    REQUIRE(header.find("text/event-stream") != std::string::npos);
  }
  ~EventClient() { holder::test::close_http_socket(socket_); }
  struct Event {
    std::string id, name;
    nlohmann::json data;
  };
  Event next() {
    const auto frame = read_until("\n\n");
    Event event;
    std::istringstream lines(frame);
    for (std::string line; std::getline(lines, line);) {
      if (line.starts_with("id: ")) event.id = line.substr(4);
      if (line.starts_with("event: ")) event.name = line.substr(7);
      if (line.starts_with("data: ")) event.data = nlohmann::json::parse(line.substr(6));
    }
    return event;
  }

 private:
  std::string read_until(std::string_view delimiter) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
      if (auto end = buffer_.find(delimiter); end != std::string::npos) {
        auto result = buffer_.substr(0, end + delimiter.size());
        buffer_.erase(0, result.size());
        return result;
      }
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("Timed out reading SSE");
      char chunk[4096];
      boost::system::error_code ec;
      auto size = socket_.read_some(boost::asio::buffer(chunk), ec);
      buffer_.append(chunk, size);
      if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::try_again)
        throw boost::system::system_error(ec);
      if (!size) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  boost::asio::io_context ioc_;
  tcp::socket socket_;
  std::string buffer_;
};
} // namespace

TEST_CASE(
    "HTTP change streams replay committed changes without occupying request workers",
    "[http][events]"
) {
  auto db = holder::test::open_db_with_schema(holder::test::make_temp_dir() / "holder.db");
  db.exec(
      "INSERT INTO projects(project_id,name,root_path,created_at,updated_at) VALUES('p','P','/missing-holder-event-http',1,1)"
  );
  db.exec(
      "INSERT INTO projects(project_id,name,root_path,created_at,updated_at) VALUES('q','Q','/missing-holder-event-other',1,1)"
  );
  holder::api::HttpServer server("127.0.0.1", 0, db, "testtoken", nullptr, nullptr);
  const auto bound = server.start();
  holder::core::SignalHandler signals;
  holder::test::HttpServerThreadGuard guard(server, signals);
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
  auto checkpoint = holder::test::http_json_request(
      bound.bind,
      bound.port,
      "testtoken",
      http::verb::get,
      "/events/cursor?project_id=p",
      {},
      http::status::ok
  )["data"];
  REQUIRE(holder::api::support::EventJournal::valid_cursor(checkpoint["cursor"]));
  REQUIRE(checkpoint["git_revisions"]["p"].is_null());
  REQUIRE(checkpoint["git_revisions"].size() == 1);
  REQUIRE(checkpoint["history_urls"]["p"] == "/projects/p/history");
  std::vector<std::unique_ptr<EventClient>> clients;
  for (int i = 0; i < 16; ++i) {
    auto client =
        std::make_unique<EventClient>(bound.port, "/events?project_id=p", checkpoint["cursor"]);
    REQUIRE(client->next().name == "ready");
    clients.push_back(std::move(client));
  }
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
  holder::platform::Db background;
  background.open(db.path());
  background.exec(
      "INSERT INTO cards(card_id,project_id,title,rel_path,created_at,updated_at) VALUES('a-other','q','Other','other.md',1,1)"
  );
  background.exec(
      "INSERT INTO cards(card_id,project_id,title,rel_path,created_at,updated_at) VALUES('c','p','C','c.md',1,1)"
  );
  auto change = clients[0]->next();
  REQUIRE(change.name == "card.changed");
  REQUIRE(change.data["entity_id"] == "c");
  EventClient replay(bound.port, "/events?project_id=p", checkpoint["cursor"]);
  REQUIRE(replay.next().name == "ready");
  REQUIRE(replay.next().id == change.id);
  // A process-local foreign epoch cannot be replayed: recovery points at Git/history.
  EventClient stale(
      bound.port,
      "/events?project_id=p&last_revision=" + std::string(40, 'a'),
      holder::api::support::EventJournal{}.cursor()
  );
  auto resync = stale.next();
  REQUIRE(resync.name == "resync_required");
  REQUIRE(resync.data["last_revision"] == std::string(40, 'a'));
  REQUIRE(resync.data["history_urls"]["p"] == "/projects/p/history");
  REQUIRE(resync.data.contains("git_revisions"));
  // Shutdown cancels idle sockets, with no need to wait for a new change/heartbeat.
  guard.stop();
}

TEST_CASE("HTTP change stream validates auth, project and revision inputs", "[http][events]") {
  auto db = holder::test::open_db_with_schema(holder::test::make_temp_dir() / "holder.db");
  holder::api::HttpServer server("127.0.0.1", 0, db, "testtoken", nullptr, nullptr);
  const auto bound = server.start();
  holder::core::SignalHandler signals;
  holder::test::HttpServerThreadGuard guard(server, signals);
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
  auto request = [&](const std::string& target,
                     http::status expected,
                     const std::string& token = "testtoken") {
    REQUIRE(
        holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, target)
            .status == expected
    );
  };
  request("/events", http::status::unauthorized, "bad-token");
  request("/events?project_id=missing", http::status::not_found);
  request("/events?last_revision=" + std::string(40, 'a'), http::status::bad_request);
  request("/events?project_id=%00", http::status::bad_request);
  request("/events?project_id=p&project_id=q", http::status::bad_request);
  request("/events?unsupported=true", http::status::bad_request);
  REQUIRE(
      holder::test::http_request_raw(
          bound.bind,
          bound.port,
          "testtoken",
          http::verb::post,
          "/events"
      )
          .status == http::status::method_not_allowed
  );
}

TEST_CASE("HTTP restart recovery reports the current Git revision", "[http][events]") {
  const auto dir = holder::test::make_temp_dir();
  holder::git::GitRepo git;
  git.open_or_init(dir / "project");
  git.write_file("card.md", "first");
  git.stage_path("card.md");
  git.commit("first");
  const auto old_revision = git.head_oid().value();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "p", (dir / "project").string());
  std::string old_cursor;
  {
    holder::api::HttpServer server("127.0.0.1", 0, db, "testtoken", nullptr, nullptr);
    const auto bound = server.start();
    holder::core::SignalHandler signals;
    holder::test::HttpServerThreadGuard guard(server, signals);
    REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
    old_cursor = holder::test::http_json_request(
        bound.bind,
        bound.port,
        "testtoken",
        http::verb::get,
        "/events/cursor?project_id=p",
        {},
        http::status::ok
    )["data"]["cursor"];
  }
  git.write_file("card.md", "second");
  git.stage_path("card.md");
  git.commit("second");
  holder::api::HttpServer server("127.0.0.1", 0, db, "testtoken", nullptr, nullptr);
  const auto bound = server.start();
  holder::core::SignalHandler signals;
  holder::test::HttpServerThreadGuard guard(server, signals);
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
  EventClient resumed(bound.port, "/events?project_id=p&last_revision=" + old_revision, old_cursor);
  const auto event = resumed.next();
  REQUIRE(event.name == "resync_required");
  REQUIRE(event.data["git_revisions"]["p"] == git.head_oid().value());
  REQUIRE(event.data["last_revision"] == old_revision);
  REQUIRE(event.id != old_cursor);
  EventClient revision_only(bound.port, "/events?project_id=p&last_revision=" + old_revision);
  REQUIRE(revision_only.next().name == "resync_required");
}

TEST_CASE(
    "HTTP change feed remains unavailable until projection initialization succeeds",
    "[http][events]"
) {
  const auto dir = holder::test::make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  holder::api::HttpServer server("127.0.0.1", 0, db, "testtoken", nullptr, nullptr);
  const auto bound = server.start();
  holder::core::SignalHandler signals;
  holder::test::HttpServerThreadGuard guard(server, signals);
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
  REQUIRE(
      holder::test::http_request_raw(
          bound.bind,
          bound.port,
          "testtoken",
          http::verb::get,
          "/events?project_id=missing"
      )
          .status == http::status::service_unavailable
  );
  REQUIRE(
      holder::test::http_request_raw(
          bound.bind,
          bound.port,
          "testtoken",
          http::verb::get,
          "/events/cursor"
      )
          .status == http::status::service_unavailable
  );
  std::ifstream schema(SCHEMA_SQL_PATH);
  REQUIRE(schema.is_open());
  db.exec(std::string(std::istreambuf_iterator<char>(schema), std::istreambuf_iterator<char>()));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  auto status = http::status::service_unavailable;
  while (status == http::status::service_unavailable && std::chrono::steady_clock::now() < deadline
  ) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    status = holder::test::http_request_raw(
                 bound.bind,
                 bound.port,
                 "testtoken",
                 http::verb::get,
                 "/events/cursor"
    )
                 .status;
  }
  REQUIRE(status == http::status::ok);
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, "testtoken"));
}
