#pragma once

#include "git/GitOps.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {

// POST /locations/{id}/oauth/google-drive/authorize -- authenticated, dispatched from
// handle_ai_resource_routes's normal /locations/{id}/<action> routing alongside
// "binding"/"test", since the desktop app calling this already carries the daemon's own
// bearer token. Generates a fresh PKCE challenge and state, stashes them keyed by
// location_id, and returns the Google authorization URL for the caller to open in the
// system browser.
bool handle_google_drive_oauth_authorize_route(
    const std::string& location_id,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db
);

// GET /locations/{id}/oauth/google-drive/callback -- Google's redirect hits this
// directly from the user's browser, which never carries the daemon's own bearer token.
// Must be dispatched from Session::process_loaded_request *before* the bearer-auth
// check, the same way handle_static_routes already is (see that call site) -- this is
// the one path+method combination under /locations/ that bypasses auth; everything else
// there still requires it. Returns false immediately (does nothing) for any path/method
// that doesn't match, so it's safe to probe on every request.
//
// Validates state against what the authorize call generated, exchanges the code for
// tokens, finds-or-creates the "Holder/Resources" Drive folder, writes its id into the
// Location's configuration, and binds the refresh token -- then serves a plain HTML
// page telling the user they can close the tab, since a browser (not the GTK app) is
// what's showing this response.
bool handle_google_drive_oauth_callback_route(
    const std::string& path,
    const std::string& query_string,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    holder::privacy::SecretStore* secret_store,
    holder::git::GitOps* git_ops
);

} // namespace holder::api::routes
