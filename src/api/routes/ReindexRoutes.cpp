#include "api/routes/ReindexRoutes.h"

#include "api/support/HttpResponses.h"
#include "index/Reindexer.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_reindex_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  if (path != "/reindex" || req.method() != http::verb::post) {
    return false;
  }

  try {
    holder::index::Reindexer reindexer(db);
    reindexer.run();

    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = {{"message", "Reindex complete."}};
    res = support::json_response(http::status::ok, payload);
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }

  return true;
}

} // namespace holder::api::routes
