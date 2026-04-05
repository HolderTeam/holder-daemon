#pragma once

#include "ai/NudgeService.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes::ai {

struct DispatchResult {
  bool handled = false;
  bool streamed = false;
};

DispatchResult dispatch_ai_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::ai::NudgeService* nudge_service,
    holder::privacy::SecretStore* secret_store,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param);

} // namespace holder::api::routes::ai
