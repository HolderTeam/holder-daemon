#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiRunnerRepo.h"
#include "api/routes/ai/AiRunnerRoutes.h"
#include "http_test_helpers.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {
namespace http = boost::beast::http;

class FakeRunnerClient final : public holder::llm::RunnerClient {
 public:
  void start_background_probe() override {}

  holder::llm::RunnerStatus status() const override {
    if (throw_on_status) {
      throw std::runtime_error("status boom");
    }
    return status_result;
  }

  holder::llm::RunnerStatus retry() override {
    if (throw_on_retry) {
      throw std::runtime_error("retry boom");
    }
    return retry_result;
  }

  holder::llm::RunnerPullJob start_pull(const std::string&) override { return pull_result; }

  std::optional<holder::llm::RunnerPullJob> get_pull(const std::string& job_id) const override {
    if (pull_result.job_id == job_id) {
      return pull_result;
    }
    return std::nullopt;
  }

  std::vector<holder::llm::RunnerPullJob> list_pulls() const override { return pulls_result; }

  bool
  stream_generate(const std::string&, const std::string&, const std::string&, const std::function<void(const std::string&)>&, std::string*)
      override {
    return true;
  }

  holder::llm::RunnerStatus status_result{};
  holder::llm::RunnerStatus retry_result{};
  holder::llm::RunnerPullJob pull_result{};
  std::vector<holder::llm::RunnerPullJob> pulls_result;
  bool throw_on_status = false;
  bool throw_on_retry = false;
};

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiRunnerRoutes returns pull-event dispatch result when handled", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);

  auto req = make_request(http::verb::get, "/ai/runner/pull/job-1/events");
  http::response<http::string_body> res;

  const auto out = holder::api::routes::handle_ai_runner_routes(
      "/ai/runner/pull/job-1/events",
      req,
      res,
      socket,
      db,
      static_cast<holder::llm::RunnerRegistry*>(nullptr),
      []() {
        return std::string("generated-id");
      },
      [](const std::string&) -> std::string {
        return {};
      }
  );

  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::not_found);
}

TEST_CASE("AiRunnerRoutes supports list create patch and delete", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("GET /ai/runners lists auto-local") {
    auto req = make_request(http::verb::get, "/ai/runners");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::ok);
    const auto body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runners"].is_array());
    REQUIRE(body["data"]["runners"].size() == 1);
    REQUIRE(body["data"]["runners"][0]["runner_id"] == "auto-local");
  }

  SECTION("POST then GET then PATCH then DELETE /ai/runners/{id}") {
    auto create = make_request(http::verb::post, "/ai/runners");
    create.set(http::field::content_type, "application/json");
    create.body() = R"({"name":"Office Ollama","kind":"ollama","base_url":"http://office:11434"})";
    create.prepare_payload();

    auto created = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        create,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("runner-123");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(created.handled);
    REQUIRE(res.result() == http::status::created);
    auto body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runner_id"] == "manual-runner-123");
    REQUIRE(body["data"]["source"] == "manual");
    REQUIRE(body["data"]["runtime"]["configured"] == true);

    auto get_req = make_request(http::verb::get, "/ai/runners/manual-runner-123");
    res = {};
    auto got = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        get_req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(got.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["name"] == "Office Ollama");

    auto patch = make_request(http::verb::patch, "/ai/runners/manual-runner-123");
    patch.set(http::field::content_type, "application/json");
    patch.body() = R"({"name":"Desk Ollama","enabled":false})";
    patch.prepare_payload();
    res = {};
    auto patched = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        patch,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(patched.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["name"] == "Desk Ollama");
    REQUIRE(body["data"]["enabled"] == false);
    REQUIRE(body["data"]["runtime"]["configured"] == false);

    auto del = make_request(http::verb::delete_, "/ai/runners/manual-runner-123");
    res = {};
    auto removed = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        del,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(removed.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runner_id"] == "manual-runner-123");
    REQUIRE_FALSE(holder::ai::AiRunnerRepo(db).get("manual-runner-123").has_value());
  }
}

TEST_CASE("AiRunnerRoutes validates runner CRUD inputs and route guards", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("POST rejects missing name or kind") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"base_url":"http://office:11434"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Missing name or kind.");
  }

  SECTION("POST rejects empty name") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"   ","kind":"ollama","base_url":"http://office:11434"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "name cannot be empty.");
  }

  SECTION("POST rejects unsupported kind") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Office","kind":"other","base_url":"http://office:11434"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Unsupported runner kind.");
  }

  SECTION("POST rejects missing base_url") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Office","kind":"ollama"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Missing base_url.");
  }

  SECTION("POST rejects empty base_url") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Office","kind":"ollama","base_url":"   "})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "base_url cannot be empty.");
  }

  SECTION("POST rejects non-http base_url") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Office","kind":"ollama","base_url":"https://office:11434"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(
        nlohmann::json::parse(res.body())["error"]["message"] ==
        "base_url must use http://host:port format."
    );
  }

  SECTION("POST rejects malformed host_port base_url") {
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Office","kind":"ollama","base_url":"http://office"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(
        nlohmann::json::parse(res.body())["error"]["message"] ==
        "base_url must use http://host:port format."
    );
  }

  SECTION("GET runner returns not found when registry missing") {
    auto req = make_request(http::verb::get, "/ai/runners/manual-a");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a",
        req,
        res,
        socket,
        db,
        nullptr,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not found.");
  }

  SECTION("GET runner returns not found when manual runner missing") {
    auto req = make_request(http::verb::get, "/ai/runners/manual-a");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not found.");
  }

  SECTION("PATCH rejects auto-local runner") {
    auto req = make_request(http::verb::patch, "/ai/runners/auto-local");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Nope"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/auto-local",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(
        nlohmann::json::parse(res.body())["error"]["message"] ==
        "auto-local runner is not editable."
    );
  }

  SECTION("PATCH returns not found when runner missing") {
    auto req = make_request(http::verb::patch, "/ai/runners/manual-missing");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Updated"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-missing",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not found.");
  }

  SECTION("PATCH rejects empty name and invalid base_url and allows null base_url") {
    holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
        .runner_id = "manual-patch",
        .name = "Office Ollama",
        .kind = "ollama",
        .base_url = std::optional<std::string>("http://office:11434"),
        .source = "manual",
        .enabled = true,
        .created_at = 1,
        .updated_at = 1,
    });

    auto bad_name = make_request(http::verb::patch, "/ai/runners/manual-patch");
    bad_name.set(http::field::content_type, "application/json");
    bad_name.body() = R"({"name":"   "})";
    bad_name.prepare_payload();

    auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-patch",
        bad_name,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "name cannot be empty.");

    auto bad_url = make_request(http::verb::patch, "/ai/runners/manual-patch");
    bad_url.set(http::field::content_type, "application/json");
    bad_url.body() = R"({"base_url":"https://office:11434"})";
    bad_url.prepare_payload();
    res = {};

    out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-patch",
        bad_url,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(
        nlohmann::json::parse(res.body())["error"]["message"] ==
        "base_url must use http://host:port format."
    );

    auto clear_url = make_request(http::verb::patch, "/ai/runners/manual-patch");
    clear_url.set(http::field::content_type, "application/json");
    clear_url.body() = R"({"base_url":null})";
    clear_url.prepare_payload();
    res = {};

    out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-patch",
        clear_url,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::ok);
    REQUIRE(nlohmann::json::parse(res.body())["data"]["base_url"].is_null());
  }

  SECTION("DELETE rejects auto-local and missing runner") {
    auto auto_req = make_request(http::verb::delete_, "/ai/runners/auto-local");

    auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/auto-local",
        auto_req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(
        nlohmann::json::parse(res.body())["error"]["message"] ==
        "auto-local runner is not deletable."
    );

    auto missing_req = make_request(http::verb::delete_, "/ai/runners/manual-missing");
    res = {};

    out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-missing",
        missing_req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not found.");
  }

  SECTION("route shape fallthroughs return false") {
    auto req = make_request(http::verb::get, "/ai/runners/");

    auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );
    REQUIRE_FALSE(out.handled);

    req = make_request(http::verb::get, "/ai/runners/manual-a/unknown");
    out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a/unknown",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );
    REQUIRE_FALSE(out.handled);
  }
}

TEST_CASE("AiRunnerRoutes supports runner retry by runner id", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "1");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
      .runner_id = "manual-a",
      .name = "Office Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://office:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 1,
      .updated_at = 1,
  });

  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  auto req = make_request(http::verb::post, "/ai/runners/manual-a/retry");
  http::response<http::string_body> res;

  const auto out = holder::api::routes::handle_ai_runner_routes(
      "/ai/runners/manual-a/retry",
      req,
      res,
      socket,
      db,
      &runner_registry,
      []() {
        return std::string("ignored");
      },
      [](const std::string&) -> std::string {
        return {};
      }
  );

  REQUIRE(out.handled);
  REQUIRE(res.result() == http::status::ok);
  const auto body = nlohmann::json::parse(res.body());
  REQUIRE(body["ok"] == true);
  REQUIRE(body["data"]["runner_id"] == "manual-a");
  REQUIRE(body["data"]["runtime"]["configured"] == true);
  REQUIRE(body["data"]["runtime"]["available"] == true);
  REQUIRE(body["data"]["runtime"]["version"] == "fake");
}

TEST_CASE("AiRunnerRoutes covers retry guards and runtime pull serialization", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("retry returns not found when registry missing") {
    auto req = make_request(http::verb::post, "/ai/runners/manual-a/retry");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a/retry",
        req,
        res,
        socket,
        db,
        nullptr,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not found.");
  }

  SECTION("retry returns not configured when runner or client missing") {
    holder::llm::RunnerRegistry runner_registry(&db, nullptr);
    auto req = make_request(http::verb::post, "/ai/runners/manual-a/retry");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a/retry",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "Runner not configured.");
  }

  SECTION("GET /ai/runners serializes runtime pulls") {
    FakeRunnerClient auto_local;
    auto_local.status_result.available = true;
    auto_local.status_result.spawn_attempted = true;
    auto_local.status_result.last_checked = 123;
    auto_local.status_result.version = "fake";
    auto_local.status_result.models.push_back(holder::llm::LocalModel{
        .name = "qwen3:4b",
        .digest = "sha256:abc",
        .size = 42,
        .modified_at = "now"
    });
    auto_local.pulls_result.push_back(holder::llm::RunnerPullJob{
        .job_id = "job-1",
        .model = "qwen3:8b",
        .status = "pulling",
        .progress = {.completed = 5, .total = 10, .percent = 50.0, .stage = "downloading"},
        .updated_at = 456,
        .error = "",
    });

    holder::llm::RunnerRegistry runner_registry(&db, &auto_local);
    auto req = make_request(http::verb::get, "/ai/runners");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::ok);
    const auto body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runners"][0]["runtime"]["pulls"].size() == 1);
    REQUIRE(body["data"]["runners"][0]["runtime"]["pulls"][0]["job_id"] == "job-1");
    REQUIRE(body["data"]["runners"][0]["runtime"]["pulls"][0]["runner_id"] == "auto-local");
    REQUIRE(
        body["data"]["runners"][0]["runtime"]["pulls"][0]["progress"]["stage"] == "downloading"
    );
  }
}

TEST_CASE("AiRunnerRoutes catches route exceptions and preserves fallthroughs", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("GET /ai/runners catches runner status exceptions") {
    FakeRunnerClient auto_local;
    auto_local.throw_on_status = true;
    holder::llm::RunnerRegistry runner_registry(&db, &auto_local);
    auto req = make_request(http::verb::get, "/ai/runners");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "status boom");
  }

  SECTION("POST /ai/runners catches invalid json parse errors") {
    holder::llm::RunnerRegistry runner_registry(&db, nullptr);
    auto req = make_request(http::verb::post, "/ai/runners");
    req.set(http::field::content_type, "application/json");
    req.body() = "{";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["code"] == "bad_request");
  }

  SECTION("retry catches runner client exceptions") {
    FakeRunnerClient auto_local;
    auto_local.throw_on_retry = true;
    holder::llm::RunnerRegistry runner_registry(&db, &auto_local);
    auto req = make_request(http::verb::post, "/ai/runners/auto-local/retry");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/auto-local/retry",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "retry boom");
  }

  SECTION("GET runner catches runtime serialization exceptions") {
    FakeRunnerClient auto_local;
    auto_local.throw_on_status = true;
    holder::llm::RunnerRegistry runner_registry(&db, &auto_local);
    auto req = make_request(http::verb::get, "/ai/runners/auto-local");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/auto-local",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["message"] == "status boom");
  }

  SECTION("PATCH catches repository exceptions") {
    holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
        .runner_id = "manual-patch",
        .name = "Office Ollama",
        .kind = "ollama",
        .base_url = std::optional<std::string>("http://office:11434"),
        .source = "manual",
        .enabled = true,
        .created_at = 1,
        .updated_at = 1,
    });
    db.close();
    holder::llm::RunnerRegistry runner_registry(nullptr, nullptr);
    auto req = make_request(http::verb::patch, "/ai/runners/manual-patch");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"name":"Updated"})";
    req.prepare_payload();

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-patch",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["code"] == "bad_request");
  }

  SECTION("DELETE catches repository exceptions") {
    holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
        .runner_id = "manual-delete",
        .name = "Office Ollama",
        .kind = "ollama",
        .base_url = std::optional<std::string>("http://office:11434"),
        .source = "manual",
        .enabled = true,
        .created_at = 1,
        .updated_at = 1,
    });
    db.close();
    holder::llm::RunnerRegistry runner_registry(nullptr, nullptr);
    auto req = make_request(http::verb::delete_, "/ai/runners/manual-delete");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-delete",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(nlohmann::json::parse(res.body())["error"]["code"] == "bad_request");
  }

  SECTION("empty runner id path falls through") {
    auto req = make_request(http::verb::post, "/ai/runners//retry");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners//retry",
        req,
        res,
        socket,
        db,
        nullptr,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE_FALSE(out.handled);
  }

  SECTION("unsupported method on runner path falls through") {
    auto req = make_request(http::verb::post, "/ai/runners/manual-a");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-a",
        req,
        res,
        socket,
        db,
        nullptr,
        []() {
          return std::string("ignored");
        },
        [](const std::string&) -> std::string {
          return {};
        }
    );

    REQUIRE_FALSE(out.handled);
  }
}
