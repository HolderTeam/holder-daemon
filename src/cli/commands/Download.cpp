#include "cli/commands/Download.h"

#include "identity/Uuid.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace holder::cli {
namespace {

std::string download_filename(const std::string& disposition) {
  // Holderd supplies a sanitized quoted filename. Keep the raw header as well.
  const auto start = disposition.find("filename=\"");
  if (start == std::string::npos) return {};
  const auto value_start = start + 10;
  const auto end = disposition.find('"', value_start);
  return end == std::string::npos ? std::string()
                                  : disposition.substr(value_start, end - value_start);
}

class StagedFile {
 public:
  explicit StagedFile(const std::filesystem::path& output)
      : output_(std::filesystem::absolute(output).lexically_normal()) {
    if (std::filesystem::is_directory(output_)) {
      throw CliError("invalid_output", "Output must be a file, not a directory.");
    }
    directory_ = output_.parent_path() / (".holderctl-export-" + holder::identity::uuid_v4());
    // LCOV_EXCL_START: requires an external filesystem race after the validated parent lookup.
    if (!std::filesystem::create_directory(directory_)) {
      throw CliError("output_failed", "Could not create export staging directory.");
    }
    // LCOV_EXCL_STOP
    file_ = directory_ / "payload";
#ifndef _WIN32
    std::error_code error;
    std::filesystem::permissions(
        directory_,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        error
    );
    // LCOV_EXCL_START: requires chmod to fail on the newly created local staging directory.
    if (error) {
      std::error_code ignored;
      std::filesystem::remove(directory_, ignored);
      throw CliError("output_failed", "Could not secure export staging directory.");
    }
    // LCOV_EXCL_STOP
#endif
  }
  ~StagedFile() {
    std::error_code ignored;
    std::filesystem::remove(file_, ignored);
    std::filesystem::remove(directory_, ignored);
  }
  StagedFile(const StagedFile&) = delete;
  StagedFile& operator=(const StagedFile&) = delete;
  const std::filesystem::path& path() const { return file_; }
  void commit() {
#ifdef _WIN32
    if (!MoveFileExW(
            file_.c_str(),
            output_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        )) {
      throw CliError(
          "output_failed",
          "Could not replace export destination.",
          {{"system_error", GetLastError()}}
      );
    }
#else
    std::error_code error;
    std::filesystem::rename(file_, output_, error);
    if (error)
      throw CliError("output_failed", "Could not replace export destination: " + error.message());
#endif
  }

 private:
  std::filesystem::path output_;
  std::filesystem::path directory_;
  std::filesystem::path file_;
};

} // namespace

DownloadMetadata http_download(
    const DaemonConnection& connection,
    const std::string& target,
    std::chrono::seconds timeout,
    const std::function<void(const char*, std::size_t)>& sink
) try {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context context;
  tcp::resolver resolver(context);
  boost::beast::tcp_stream stream(context);
  // Beast deadlines apply to asynchronous operations. This context belongs only to this
  // download; it is never a daemon/shared I/O context.
  auto perform = [&](auto initiate) {
    context.restart();
    stream.expires_after(timeout);
    boost::system::error_code result = boost::asio::error::operation_aborted;
    initiate([&](boost::system::error_code error, auto...) {
      result = error;
    });
    context.run();
    return result;
  };
  auto checked = [&](auto initiate) {
    const auto error = perform(std::move(initiate));
    if (error) throw boost::system::system_error(error);
  };
  const auto endpoints = resolver.resolve(connection.bind, std::to_string(connection.port));
  checked([&](auto handler) {
    stream.async_connect(endpoints, std::move(handler));
  });
  http::request<http::empty_body> request{http::verb::get, target, 11};
  request.set(http::field::host, connection.bind);
  request.set(http::field::user_agent, "holderctl");
  request.set(http::field::authorization, "Bearer " + connection.token);
  request.keep_alive(false);
  checked([&](auto handler) {
    http::async_write(stream, request, std::move(handler));
  });

  boost::beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
  checked([&](auto handler) {
    http::async_read_header(stream, buffer, parser, std::move(handler));
  });
  const bool success = parser.get().result() == http::status::ok;
  DownloadMetadata metadata;
  metadata.content_type = std::string(parser.get()[http::field::content_type]);
  metadata.content_disposition = std::string(parser.get()[http::field::content_disposition]);
  metadata.filename = download_filename(metadata.content_disposition);
  std::string error_body;
  std::array<char, 64 * 1024> bytes{};
  while (!parser.is_done()) {
    parser.get().body().data = bytes.data();
    parser.get().body().size = bytes.size();
    const auto error = perform([&](auto handler) {
      http::async_read_some(stream, buffer, parser, std::move(handler));
    });
    const auto count = bytes.size() - parser.get().body().size;
    if (success && count > 0) {
      sink(bytes.data(), count);
      metadata.byte_size += count;
    } else if (!success && count > 0) {
      if (error_body.size() + count > 1024 * 1024) {
        throw CliError("download_failed", "Server error response exceeds 1 MiB.");
      }
      error_body.append(bytes.data(), count);
    }
    if (error && error != http::error::need_buffer) {
      throw CliError("download_failed", "Incomplete asset download: " + error.message());
    }
  }
  boost::system::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  if (!success) {
    const auto payload = nlohmann::json::parse(error_body, nullptr, false);
    std::string code = "download_failed";
    std::string message = "HTTP " + std::to_string(parser.get().result_int());
    nlohmann::json details = {{"http_status", parser.get().result_int()}};
    if (payload.is_object() && payload.contains("error") && payload.at("error").is_object()) {
      const auto& error = payload.at("error");
      code = json_string(error, "code", code);
      message = json_string(error, "message", message);
      if (error.contains("details")) details = error.at("details");
    }
    throw CliError(code, message, std::move(details), message);
  }
  return metadata;
} catch (const boost::system::system_error& error) {
  throw CliError("download_failed", "Asset download failed: " + std::string(error.what()));
}

DownloadMetadata download_to_file(
    const DaemonConnection& connection,
    const std::string& target,
    const std::filesystem::path& output,
    std::chrono::seconds timeout
) try {
  StagedFile staging(output);
  std::ofstream file(staging.path(), std::ios::binary | std::ios::trunc);
  if (!file) throw CliError("output_failed", "Could not open export staging file.");
#ifndef _WIN32
  std::filesystem::permissions(
      staging.path(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace
  );
#endif
  const auto metadata =
      http_download(connection, target, timeout, [&](const char* bytes, std::size_t count) {
        file.write(bytes, static_cast<std::streamsize>(count));
        if (!file) throw CliError("output_failed", "Could not write export staging file.");
      });
  file.close();
  if (!file) throw CliError("output_failed", "Could not close export staging file.");
  staging.commit();
  return metadata;
} catch (const std::filesystem::filesystem_error& error) {
  throw CliError("output_failed", "Export output failed: " + std::string(error.what()));
}

} // namespace holder::cli
