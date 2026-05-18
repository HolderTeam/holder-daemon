#include "http_test_helpers.h"

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"

#include <filesystem>
#include <fstream>

namespace {

void write_text(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  REQUIRE(out.is_open());
  out << content;
}

} // namespace

TEST_CASE("HTTP rebuild repopulates DB from files", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  holder::model::Card card;
  card.card_id = "11111111-1111-1111-1111-111111111111";
  card.project_id = project_id;
  card.title = "Rebuild Card";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 10;
  card.updated_at = 12;
  const std::string card_content = "Card body";
  const auto card_raw = holder::core::render_card_front_matter(card, {}) + card_content;
  write_text(root / card.rel_path, card_raw);

  holder::model::AiMessage msg;
  msg.message_id = "22222222-2222-2222-2222-222222222222";
  msg.thread_id = "thread-1";
  msg.role = "assistant";
  msg.source = "manual_paste";
  msg.content = "AI message body";
  msg.created_at = 20;
  const std::string msg_rel = holder::core::ai_message_rel_path(msg.message_id);
  const auto msg_raw = holder::core::render_ai_message_front_matter(msg, project_id, {}) +
                       msg.content;
  write_text(root / msg_rel, msg_raw);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto rebuild = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::ok
  );
  REQUIRE(rebuild["ok"] == true);
  REQUIRE(rebuild["data"]["cards"] == 1);
  REQUIRE(rebuild["data"]["ai_messages"] == 1);
  REQUIRE(rebuild["data"]["ai_threads"] == 1);

  auto cards = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards?project_id=" + project_id,
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(cards["data"].size() == 1);

  auto threads = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/threads?project_id=" + project_id,
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(threads["data"].size() == 1);

  auto messages = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/messages?thread_id=thread-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(messages["data"].size() == 1);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on missing project or root", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::test::create_project(db, "proj-1", (dir / "missing_root").string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto missing_field = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_field["ok"] == false);

  auto unknown_project = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", "missing"}},
      boost::beast::http::status::not_found
  );
  REQUIRE(unknown_project["ok"] == false);

  auto missing_root = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", "proj-1"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_root["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on card path mismatch", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  holder::model::Card card;
  card.card_id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  card.project_id = project_id;
  card.title = "Mismatch";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 10;
  card.updated_at = 10;
  const std::string card_content = "Card body";
  const auto card_raw = holder::core::render_card_front_matter(card, {}) + card_content;

  const std::string wrong_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  const auto wrong_path = holder::core::card_rel_path(wrong_id);
  write_text(root / wrong_path, card_raw);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto mismatch = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(mismatch["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on short ai message id", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  const auto short_path = root / "ai_messages" / "ab" / "cd" / "abc.md";
  write_text(
      short_path,
      "---\nmessage_id: abc\nthread_id: t\nrole: user\nsource: manual\ncreated_at: 1\n---\nbody"
  );

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto invalid = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on malformed YAML", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  const auto rel_path = holder::core::card_rel_path("cccccccc-cccc-cccc-cccc-cccccccccccc");
  write_text(root / rel_path, "---\n: bad\n---\nbody");

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto invalid = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on duplicate IDs", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  const std::string card_id = "dddddddd-dddd-dddd-dddd-dddddddddddd";
  const auto rel_path = holder::core::card_rel_path(card_id);
  const auto alt_path = holder::core::card_rel_path("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee");

  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = "Dup";
  card.rel_path = rel_path;
  card.created_at = 1;
  card.updated_at = 1;
  const auto raw = holder::core::render_card_front_matter(card, {}) + "body";

  write_text(root / rel_path, raw);
  write_text(root / alt_path, raw);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto dup = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(dup["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP rebuild errors on empty IDs", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  holder::test::create_project(db, project_id, root.string());

  const auto rel_path = holder::core::card_rel_path("ffffffff-ffff-ffff-ffff-ffffffffffff");
  write_text(root / rel_path, "---\ncard_id: \"\"\nproject_id: proj-1\n---\nbody");

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto invalid = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/rebuild",
      nlohmann::json{{"project_id", project_id}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
