#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/messages/AiMessageCaptureRoutes.h"
#include "http_test_helpers.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "model/AiThread.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method,
                                              const std::string& target,
                                              const std::string& body = "") {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  req.body() = body;
  req.prepare_payload();
  return req;
}

} // namespace

TEST_CASE("AiMessageCaptureRoutes uncovered branches", "[ai][capture-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "repo1").string());
  holder::test::create_project(db, "proj-2", (dir / "repo2").string());

  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread thread1{
      .thread_id = "thread-1",
      .project_id = "proj-1",
      .card_id = std::nullopt,
      .title = "Thread One",
      .created_at = 1,
      .updated_at = 1,
  };
  thread_repo.create(thread1);
  holder::model::AiThread thread2{
      .thread_id = "thread-2",
      .project_id = "proj-2",
      .card_id = std::nullopt,
      .title = "Thread Two",
      .created_at = 1,
      .updated_at = 1,
  };
  thread_repo.create(thread2);

  auto call = [&](const std::string& body, const std::function<std::string()>& uuid_v4) {
    auto req = make_request(http::verb::post, "/ai/messages/capture", body);
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_capture_routes(
        "/ai/messages/capture", req, res, db, nullptr, uuid_v4);
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };
  size_t uuid_counter = 0;
  auto next_uuid = [&]() {
    ++uuid_counter;
    return std::string("generated-id-") + std::to_string(uuid_counter);
  };

  SECTION("missing required fields") {
    auto [status, payload] = call(R"({"project_id":"proj-1","prompt":"p"})", next_uuid);
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("project not found") {
    auto [status, payload] = call(
        R"({"project_id":"missing","thread_id":"thread-1","prompt":"p","response":"r"})", next_uuid);
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("explicit thread id not found") {
    auto [status, payload] = call(
        R"({"project_id":"proj-1","thread_id":"missing-thread","prompt":"p","response":"r"})", next_uuid);
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("explicit thread belongs to different project") {
    auto [status, payload] = call(
        R"({"project_id":"proj-1","thread_id":"thread-2","prompt":"p","response":"r"})", next_uuid);
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("explicit thread capture stores assistant context metadata") {
    auto [status, payload] = call(
        R"({"project_id":"proj-1","thread_id":"thread-1","prompt":"Q","response":"A","context":{"kind":"test","n":2}})",
        next_uuid);
    REQUIRE(status == http::status::created);
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["thread_id"] == "thread-1");

    holder::ai::AiMessageRepo msg_repo(db, nullptr);
    const auto msgs = msg_repo.list_by_thread("thread-1");
    REQUIRE(msgs.size() == 2);
    REQUIRE(msgs[1].meta_json.has_value());
    const auto meta = nlohmann::json::parse(msgs[1].meta_json.value());
    REQUIRE(meta.contains("context"));
    REQUIRE(meta["context"]["kind"] == "test");
    REQUIRE(meta["context"]["n"] == 2);
  }

  SECTION("conflict in append maps to http conflict") {
    auto seq = std::vector<std::string>{"dup-msg-id", "dup-msg-id"};
    size_t i = 0;
    auto dup_uuid = [&]() {
      if (i < seq.size()) return seq[i++];
      return std::string("dup-msg-id");
    };

    auto [status, payload] =
        call(R"({"project_id":"proj-1","thread_id":"thread-1","prompt":"Q","response":"A"})", dup_uuid);
    REQUIRE(status == http::status::conflict);
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "conflict");
  }

  SECTION("invalid json maps to bad_request") {
    auto req = make_request(http::verb::post, "/ai/messages/capture", "{");
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_capture_routes(
        "/ai/messages/capture", req, res, db, nullptr, []() { return std::string("id"); });
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }
}
