#include "api/support/EventJournal.h"
#include "api/support/RunEventStore.h"
#include "api/support/SseStream.h"
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <thread>

using namespace holder::api::support;

TEST_CASE("Event journal uses absolute replay cursors across trimming", "[events]") {
  EventJournal journal(3);
  const auto baseline = journal.cursor();
  std::string third;
  for (int n = 1; n <= 600; ++n) {
    const auto event = journal.append("chunk", {{"n", n}}, n == 600);
    if (n == 597) third = event.id;
  }
  auto batch = journal.read(third);
  REQUIRE_FALSE(batch.resync_required);
  REQUIRE(batch.finished);
  REQUIRE(batch.truncated);
  REQUIRE(batch.events.size() == 3);
  REQUIRE(batch.events[0].data["n"] == 598);
  REQUIRE(batch.events[2].data["n"] == 600);
  REQUIRE(journal.read(batch.cursor).events.empty());
  REQUIRE(journal.read(baseline).resync_required);
  REQUIRE(journal.read(EventJournal{}.cursor()).resync_required);
  REQUIRE(journal.read(batch.cursor.substr(0, 37) + "601").resync_required);
}

TEST_CASE("Event journal bounds bytes and encodes one JSON data line", "[events]") {
  EventJournal journal(10, 180);
  auto first = journal.append("chunk", {{"delta", "line one\nline two"}});
  REQUIRE(encode_sse(first).find("line one\\nline two") != std::string::npos);
  journal.append("chunk", {{"delta", std::string(300, 'x')}});
  REQUIRE(journal.read().events.empty());
  REQUIRE(journal.read(first.id).resync_required);
  for (const auto& bad :
       {"",
        "hello",
        "00000000-0000-0000-0000-000000000000:-1",
        "00000000-0000-0000-0000-000000000000:18446744073709551616"})
    REQUIRE_FALSE(EventJournal::valid_cursor(bad));
}

TEST_CASE("A reused run ID starts a new event epoch", "[events]") {
  const auto first = append_run_event("journal-reuse", "run_started", {}, false);
  append_run_event("journal-reuse", "done", {}, true);
  const auto next = append_run_event("journal-reuse", "run_started", {}, false);
  REQUIRE(first.id != next.id);
  const auto batch = read_run_events("journal-reuse", "");
  REQUIRE(batch);
  REQUIRE_FALSE(batch->finished);
  REQUIRE(batch->events.size() == 1);
  REQUIRE(read_run_events("journal-reuse", first.id)->resync_required);
}

TEST_CASE("SSE admission restores rejected sockets and bounds producer output", "[events]") {
  boost::asio::io_context ioc;
  auto registry = std::make_shared<SseRegistry>(1);
  boost::asio::ip::tcp::socket one(ioc), two(ioc);
  auto stream = SseStream::reserve(one, registry);
  REQUIRE(stream);
  REQUIRE_FALSE(SseStream::reserve(two, registry));
  REQUIRE_FALSE(stream->send({"id", "chunk", {{"delta", std::string(2 * 1024 * 1024, 'x')}}}));
  REQUIRE_FALSE(stream->open());
  ioc.run();
}

TEST_CASE(
    "SSE cancellation keeps its I/O context alive until source work returns",
    "[events][http]"
) {
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  tcp::socket client(ioc);
  client.connect(acceptor.local_endpoint());
  tcp::socket socket(ioc);
  acceptor.accept(socket);
  auto registry = std::make_shared<SseRegistry>();
  std::promise<void> entered, released, exited;
  auto entered_future = entered.get_future();
  auto release_future = released.get_future().share();
  auto exited_future = exited.get_future();
  auto stream = SseStream::start(socket, registry, [&] {
    entered.set_value();
    release_future.wait();
    EventBatch batch;
    batch.finished = true;
    return batch;
  });
  std::thread io_worker([&] {
    ioc.run();
    exited.set_value();
  });
  struct Cleanup {
    std::function<void()> action;
    ~Cleanup() { action(); }
  } cleanup{[&] {
    stream->cancel();
    try {
      released.set_value();
    } catch (const std::future_error&) {
    }
    io_worker.join();
    registry->poll_workers().join();
  }};
  REQUIRE(entered_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  client.close();
  REQUIRE(exited_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
  released.set_value();
  REQUIRE(exited_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}
