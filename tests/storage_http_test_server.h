#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace holder::test {

// Owns its I/O thread, including accept/read/write operations. Stopping the context
// cancels the fixture without waiting for an expected request that never arrived.
class StorageHttpTestServer {
 public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Handler = std::function<std::string(const Request&)>;

  explicit StorageHttpTestServer(Handler handler, bool tls = false)
      : acceptor_(context_, {boost::asio::ip::make_address("127.0.0.1"), 0}),
        port_(acceptor_.local_endpoint().port()),
        handler_(std::move(handler)),
        tls_(tls) {
    if (tls_) {
      const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
      tls_context_.use_certificate_chain_file((fixtures / "storage-test-cert.pem").string());
      tls_context_.use_private_key_file(
          (fixtures / "storage-test-key.pem").string(),
          boost::asio::ssl::context::pem
      );
    }
    accept();
    thread_ = std::thread([this] {
      context_.run();
    });
  }
  ~StorageHttpTestServer() { stop(); }
  StorageHttpTestServer(const StorageHttpTestServer&) = delete;
  StorageHttpTestServer& operator=(const StorageHttpTestServer&) = delete;

  std::string endpoint() const {
    return std::string(tls_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_);
  }
  boost::asio::ip::tcp::endpoint address() const {
    return {boost::asio::ip::make_address("127.0.0.1"), port_};
  }
  static std::string certificate_file() {
    return (std::filesystem::path(__FILE__).parent_path() / "fixtures" / "storage-test-cert.pem")
        .string();
  }
  void stop() {
    context_.stop();
    if (thread_.joinable()) thread_.join();
  }
  // Call only after stop(): requests and errors belong to the server's I/O thread.
  const std::vector<Request>& requests() const { return requests_; }
  void rethrow_error() const {
    if (error_) std::rethrow_exception(error_);
  }

  static std::string response(unsigned int status, const std::string& body = {}) {
    return "HTTP/1.1 " + std::to_string(status) +
           " Test\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
  }

 private:
  struct Connection {
    Connection(boost::asio::ip::tcp::socket socket, boost::asio::ssl::context& context)
        : stream(std::move(socket), context) {}
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream;
    boost::beast::flat_buffer buffer;
    Request request;
    std::string response;
  };
  template <class Stream> void read(const std::shared_ptr<Connection>& connection, Stream& stream) {
    boost::beast::http::async_read(
        stream,
        connection->buffer,
        connection->request,
        [this, connection, &stream](boost::system::error_code error, std::size_t) {
          if (error) return;
          requests_.push_back(connection->request);
          try {
            connection->response = handler_(connection->request);
          } catch (...) {
            error_ = std::current_exception();
            return;
          }
          boost::asio::async_write(
              stream,
              boost::asio::buffer(connection->response),
              [this, connection](boost::system::error_code, std::size_t) {
                if (tls_) {
                  connection->stream.async_shutdown([connection](boost::system::error_code) {
                  });
                } else {
                  boost::system::error_code ignored;
                  connection->stream.next_layer().shutdown(
                      boost::asio::ip::tcp::socket::shutdown_both,
                      ignored
                  );
                }
              }
          );
        }
    );
  }
  void accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
          if (ec) return;
          auto connection = std::make_shared<Connection>(std::move(socket), tls_context_);
          if (tls_) {
            connection->stream.async_handshake(
                boost::asio::ssl::stream_base::server,
                [this, connection](boost::system::error_code error) {
                  if (!error) read(connection, connection->stream);
                }
            );
          } else {
            read(connection, connection->stream.next_layer());
          }
          accept();
        }
    );
  }
  boost::asio::io_context context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  unsigned short port_;
  Handler handler_;
  bool tls_;
  boost::asio::ssl::context tls_context_{boost::asio::ssl::context::tls_server};
  std::vector<Request> requests_;
  std::exception_ptr error_;
  std::thread thread_;
};

} // namespace holder::test
