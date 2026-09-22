#include "storage/google/GoogleNetwork.h"

#include <mutex>
#include <stdexcept>

namespace holder::storage::google {
namespace {
std::mutex endpoint_mutex;
std::optional<boost::asio::ip::tcp::endpoint> test_endpoint;
} // namespace

boost::asio::ip::tcp::resolver::results_type resolve_google_endpoint(
    boost::asio::ip::tcp::resolver& resolver,
    const std::string& host
) {
  std::optional<boost::asio::ip::tcp::endpoint> endpoint;
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex);
    endpoint = test_endpoint;
  }
  if (endpoint) return boost::asio::ip::tcp::resolver::results_type::create(*endpoint, host, "443");
  // Production DNS depends on external Google service availability. Protocol
  // tests redirect TCP to loopback while exercising the same verified TLS path.
  return resolver.resolve(host, "443"); // LCOV_EXCL_LINE
}

void set_google_endpoint_for_tests(std::optional<boost::asio::ip::tcp::endpoint> endpoint) {
  if (endpoint && !endpoint->address().is_loopback())
    throw std::invalid_argument("Google test endpoint must be loopback");
  std::lock_guard<std::mutex> lock(endpoint_mutex);
  test_endpoint = endpoint;
}

} // namespace holder::storage::google
