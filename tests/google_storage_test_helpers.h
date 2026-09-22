#pragma once

#include "http_test_helpers.h"
#include "storage/google/GoogleNetwork.h"
#include "storage_http_test_server.h"

namespace holder::test {

struct GoogleStorageTestServer {
  EnvGuard trust{"SSL_CERT_FILE", StorageHttpTestServer::certificate_file()};
  StorageHttpTestServer server;

  explicit GoogleStorageTestServer(StorageHttpTestServer::Handler handler)
      : server(std::move(handler), true) {
    holder::storage::google::set_google_endpoint_for_tests(server.address());
  }
  ~GoogleStorageTestServer() {
    holder::storage::google::set_google_endpoint_for_tests(std::nullopt);
  }
  void finish() {
    server.stop();
    server.rethrow_error();
  }
};

} // namespace holder::test
