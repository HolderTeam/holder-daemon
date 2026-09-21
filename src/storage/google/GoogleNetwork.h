#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <optional>
#include <string>

namespace holder::storage::google {

boost::asio::ip::tcp::resolver::results_type resolve_google_endpoint(
    boost::asio::ip::tcp::resolver& resolver,
    const std::string& host
);

// Redirect only the TCP connection in tests; TLS peer verification, hostname
// checks, SNI, and HTTP Host remain those of the actual Google endpoint.
void set_google_endpoint_for_tests(std::optional<boost::asio::ip::tcp::endpoint> endpoint);

} // namespace holder::storage::google
