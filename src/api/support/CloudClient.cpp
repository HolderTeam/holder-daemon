#include "api/support/CloudClient.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

namespace holder::api::support {
namespace {

struct ParsedHttpsBaseUrl {
  std::string host;
  std::string port;
  std::string base_path;
};

std::optional<ParsedHttpsBaseUrl> parse_https_base_url(const std::string& base_url) {
  constexpr const char kPrefix[] = "https://";
  if (base_url.rfind(kPrefix, 0) != 0) return std::nullopt;
  std::string rest = base_url.substr(sizeof(kPrefix) - 1);
  std::string host_port = rest;
  std::string base_path = "";
  const auto slash = rest.find('/');
  if (slash != std::string::npos) {
    host_port = rest.substr(0, slash);
    base_path = rest.substr(slash);
  }
  if (host_port.empty()) return std::nullopt;
  ParsedHttpsBaseUrl out;
  out.port = "443";
  const auto colon = host_port.find(':');
  if (colon == std::string::npos) {
    out.host = host_port;
  } else {
    out.host = host_port.substr(0, colon);
    out.port = host_port.substr(colon + 1);
  }
  out.base_path = base_path;
  if (out.host.empty()) return std::nullopt;
  return out;
}

std::string url_encode_component(const std::string& in) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const char raw_ch : in) {
    const unsigned char ch = static_cast<unsigned char>(raw_ch);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
        ch == '.' || ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('%');
      out.push_back(kHex[(ch >> 4) & 0x0F]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

std::string truncate_bytes(const std::string& text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  return text.substr(0, max_bytes);
}

bool https_post_json(const std::string& base_url,
                     const std::string& target_path_and_query,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     const nlohmann::json& body,
                     int* out_status,
                     std::string* out_body,
                     std::string* error) {
  namespace http = boost::beast::http;
  namespace ssl = boost::asio::ssl;
  using tcp = boost::asio::ip::tcp;

  try {
    const auto parsed_opt = parse_https_base_url(base_url);
    if (!parsed_opt.has_value()) {
      if (error) *error = "invalid https base_url";
      return false;
    }
    const auto& parsed = parsed_opt.value();
    const std::string target = parsed.base_path + target_path_and_query;

    boost::asio::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(parsed.host, parsed.port);

    boost::beast::ssl_stream<boost::beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
      if (error) *error = "failed to set tls host name";
      return false;
    }
    boost::beast::get_lowest_layer(stream).connect(endpoints);
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(parsed.host));
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, parsed.host);
    req.set(http::field::user_agent, "holder/cloud-runner");
    req.set(http::field::content_type, "application/json");
    for (const auto& [name, value] : headers) {
      req.set(name, value);
    }
    req.body() = body.dump();
    req.prepare_payload();

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.shutdown(ec);
    if (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated) {
      ec = {};
    }

    if (out_status) *out_status = static_cast<int>(res.result_int());
    if (out_body) *out_body = res.body();
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

} // namespace

long long estimate_tokens_from_text(const std::string& text) {
  if (text.empty()) return 0;
  return static_cast<long long>((text.size() + 3) / 4);
}

std::string compact_context_tail(const std::string& context_json,
                                 long long allowed_context_tokens,
                                 bool* compacted) {
  if (compacted) *compacted = false;
  if (allowed_context_tokens <= 0) {
    if (compacted) *compacted = !context_json.empty();
    return {};
  }
  const long long max_bytes = allowed_context_tokens * 4;
  if (max_bytes <= 0 || static_cast<long long>(context_json.size()) <= max_bytes) {
    return context_json;
  }
  if (compacted) *compacted = true;
  const std::size_t keep = static_cast<std::size_t>(max_bytes);
  return std::string("[context_compacted]\n") + context_json.substr(context_json.size() - keep);
}

std::optional<std::string> run_cloud_model(const CloudProviderConfig& provider,
                                           const CloudModelConfig& model,
                                           const std::string& api_key,
                                           const std::string& prompt_with_context,
                                           std::string* error) {
  if (provider.kind != "chocolatefactory_generative_language") {
    if (error) *error = "unsupported provider kind: " + provider.kind;
    return std::nullopt;
  }
  if (provider.auth_type != "api_key_query") {
    if (error) *error = "unsupported auth type for chocolatefactory adapter";
    return std::nullopt;
  }
  const std::string key_param = provider.key_param.empty() ? "key" : provider.key_param;
  std::string target = model.endpoint;
  if (target.empty()) {
    if (error) *error = "missing provider model endpoint";
    return std::nullopt;
  }
  target += "?" + key_param + "=" + url_encode_component(api_key);

  nlohmann::json req;
  req["contents"] = nlohmann::json::array(
      {nlohmann::json{{"parts", nlohmann::json::array({nlohmann::json{{"text", prompt_with_context}}})}}});

  int status = 0;
  std::string response_body;
  std::string transport_error;
  if (!https_post_json(provider.base_url, target, {}, req, &status, &response_body, &transport_error)) {
    if (error) *error = transport_error.empty() ? "cloud request failed" : transport_error;
    return std::nullopt;
  }

  if (status < 200 || status >= 300) {
    if (error) *error = "cloud HTTP " + std::to_string(status) + ": " + truncate_bytes(response_body, 300);
    return std::nullopt;
  }

  try {
    const auto parsed = nlohmann::json::parse(response_body);
    if (!parsed.contains("candidates") || !parsed["candidates"].is_array() ||
        parsed["candidates"].empty()) {
      if (error) *error = "cloud response missing candidates";
      return std::nullopt;
    }
    const auto& first = parsed["candidates"][0];
    if (!first.contains("content") || !first["content"].contains("parts") ||
        !first["content"]["parts"].is_array()) {
      if (error) *error = "cloud response missing content parts";
      return std::nullopt;
    }
    std::string text;
    for (const auto& part : first["content"]["parts"]) {
      if (part.contains("text") && part["text"].is_string()) {
        text += part["text"].get<std::string>();
      }
    }
    if (text.empty()) {
      if (error) *error = "cloud response text empty";
      return std::nullopt;
    }
    return text;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return std::nullopt;
  }
}

} // namespace holder::api::support
