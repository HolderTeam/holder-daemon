#include "api/support/CloudClient.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <cctype>
#include <mutex>
#include <utility>

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

// Uses live TLS sockets; validated via integration/system tests.
// LCOV_EXCL_START
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
// LCOV_EXCL_STOP

std::string status_error_code(int status) {
  if (status == 401 || status == 403) return "auth_failed";
  if (status == 429) return "rate_limited";
  if (status == 408 || status == 504) return "provider_timeout";
  if (status >= 500) return "provider_unavailable";
  return "provider_error";
}

std::string lowercase_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool contains_insensitive(const std::string& haystack, const std::string& needle) {
  return lowercase_ascii(haystack).find(lowercase_ascii(needle)) != std::string::npos;
}

std::optional<std::string> classify_error_from_body(const std::string& provider_kind,
                                                    int status,
                                                    const std::string& response_body) {
  if (status == 401 || status == 403) return std::string("auth_failed");
  if (status == 408 || status == 504) return std::string("provider_timeout");
  if (status >= 500) return std::string("provider_unavailable");

  // Default mappings from status if body does not provide stronger signal.
  std::string code = (status == 429) ? "rate_limited" : "provider_error";

  try {
    const auto parsed = nlohmann::json::parse(response_body);

    if (provider_kind == "generic_responses") {
      if (parsed.contains("error") && parsed["error"].is_object()) {
        const auto& err = parsed["error"];
        if (err.contains("code") && err["code"].is_string() &&
            err["code"].get<std::string>() == "insufficient_quota") {
          return std::string("quota_exceeded");
        }
        if (err.contains("type") && err["type"].is_string() &&
            err["type"].get<std::string>() == "insufficient_quota") {
          return std::string("quota_exceeded");
        }
      }
    }

    if (provider_kind == "mechatropic_messages") {
      if (parsed.contains("error") && parsed["error"].is_object()) {
        const auto& err = parsed["error"];
        if (err.contains("message") && err["message"].is_string()) {
          const std::string msg = err["message"].get<std::string>();
          if (contains_insensitive(msg, "credit balance is too low") ||
              contains_insensitive(msg, "purchase credits") ||
              contains_insensitive(msg, "plans & billing")) {
            return std::string("quota_exceeded");
          }
        }
      }
    }
  } catch (const std::exception&) {
    // Keep status-based fallback mapping.
  }

  return code;
}

std::optional<std::string> parse_chocolatefactory_text(const nlohmann::json& parsed) {
  if (!parsed.contains("candidates") || !parsed["candidates"].is_array() || parsed["candidates"].empty()) {
    return std::nullopt;
  }
  const auto& first = parsed["candidates"][0];
  if (!first.contains("content") || !first["content"].contains("parts") ||
      !first["content"]["parts"].is_array()) {
    return std::nullopt;
  }
  std::string text;
  for (const auto& part : first["content"]["parts"]) {
    if (part.contains("text") && part["text"].is_string()) {
      text += part["text"].get<std::string>();
    }
  }
  if (text.empty()) return std::nullopt;
  return text;
}

std::optional<std::string> parse_generic_chat_text(const nlohmann::json& parsed) {
  if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty()) {
    return std::nullopt;
  }
  const auto& choice = parsed["choices"][0];
  if (!choice.contains("message") || !choice["message"].is_object()) return std::nullopt;
  const auto& message = choice["message"];
  if (!message.contains("content")) return std::nullopt;
  const auto& content = message["content"];
  if (content.is_string()) {
    const auto text = content.get<std::string>();
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
  }
  if (content.is_array()) {
    std::string text;
    for (const auto& item : content) {
      if (item.is_object() && item.value("type", "") == "text" && item.contains("text") &&
          item["text"].is_string()) {
        text += item["text"].get<std::string>();
      }
    }
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
  }
  return std::nullopt;
}

std::optional<std::string> parse_mechatropic_messages_text(const nlohmann::json& parsed) {
  if (!parsed.contains("content") || !parsed["content"].is_array()) {
    return std::nullopt;
  }
  std::string text;
  for (const auto& item : parsed["content"]) {
    if (item.is_object() && item.value("type", "") == "text" && item.contains("text") &&
        item["text"].is_string()) {
      text += item["text"].get<std::string>();
    }
  }
  return text.empty() ? std::nullopt : std::optional<std::string>(text);
}

std::mutex g_run_cloud_model_override_mu;
CloudModelRunnerOverride g_run_cloud_model_override;
std::mutex g_cloud_transport_post_override_mu;
CloudTransportPostOverride g_cloud_transport_post_override;

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

CloudResponseParse parse_cloud_response(const std::string& provider_kind,
                                        int status,
                                        const std::string& response_body) {
  CloudResponseParse out;
  if (status < 200 || status >= 300) {
    out.error_code = classify_error_from_body(provider_kind, status, response_body)
                         .value_or(status_error_code(status));
    out.error_message = "cloud HTTP " + std::to_string(status) + ": " + truncate_bytes(response_body, 300);
    return out;
  }

  try {
    const auto parsed = nlohmann::json::parse(response_body);
    std::optional<std::string> text;
    if (provider_kind == "chocolatefactory_generative_language") {
      text = parse_chocolatefactory_text(parsed);
    } else if (provider_kind == "generic_chat") {
      text = parse_generic_chat_text(parsed);
    } else if (provider_kind == "generic_responses") {
      if (parsed.contains("output_text") && parsed["output_text"].is_string() &&
          !parsed["output_text"].get<std::string>().empty()) {
        text = parsed["output_text"].get<std::string>();
      }
    } else if (provider_kind == "mechatropic_messages") {
      text = parse_mechatropic_messages_text(parsed);
    }
    if (text.has_value()) {
      out.text = text;
      return out;
    }
    out.error_code = "malformed_response";
    out.error_message = "cloud response missing expected text";
    return out;
  } catch (const std::exception& ex) {
    out.error_code = "malformed_response";
    out.error_message = ex.what();
    return out;
  }
} // LCOV_EXCL_LINE

std::optional<std::string> run_cloud_model(const CloudProviderConfig& provider,
                                           const CloudModelConfig& model,
                                           const std::string& api_key,
                                           const std::string& prompt_with_context,
                                           std::string* error) {
  CloudModelRunnerOverride override_fn;
  {
    std::lock_guard<std::mutex> lock(g_run_cloud_model_override_mu);
    override_fn = g_run_cloud_model_override;
  }
  if (override_fn) {
    return override_fn(provider, model, api_key, prompt_with_context, error);
  }

  if (provider.kind != "chocolatefactory_generative_language" &&
      provider.kind != "generic_chat" && provider.kind != "generic_responses" &&
      provider.kind != "mechatropic_messages") {
    if (error) *error = "unsupported provider kind: " + provider.kind;
    return std::nullopt;
  }
  std::string target = model.endpoint;
  if (target.empty()) {
    if (error) *error = "missing provider model endpoint";
    return std::nullopt;
  }

  std::vector<std::pair<std::string, std::string>> headers;
  if (provider.auth_type == "api_key_query") {
    const std::string key_param = provider.key_param.empty() ? "key" : provider.key_param;
    target += "?" + key_param + "=" + url_encode_component(api_key);
  } else if (provider.auth_type == "bearer_header") {
    const std::string header_name = provider.header_name.empty() ? "Authorization" : provider.header_name;
    const std::string prefix = provider.bearer_prefix.empty() ? "Bearer" : provider.bearer_prefix;
    headers.emplace_back(header_name, prefix + " " + api_key);
  } else if (provider.auth_type == "header_key") {
    const std::string header_name = provider.header_name.empty() ? "x-api-key" : provider.header_name;
    headers.emplace_back(header_name, api_key);
  } else {
    if (error) *error = "unsupported auth type: " + provider.auth_type;
    return std::nullopt;
  }

  nlohmann::json req;
  if (provider.kind == "chocolatefactory_generative_language") {
    req["contents"] = nlohmann::json::array(
        {nlohmann::json{{"parts", nlohmann::json::array({nlohmann::json{{"text", prompt_with_context}}})}}});
  } else if (provider.kind == "generic_chat") {
    req["model"] = model.id;
    req["messages"] = nlohmann::json::array({nlohmann::json{
        {"role", "user"},
        {"content", prompt_with_context},
    }});
  } else if (provider.kind == "generic_responses") {
    req["model"] = model.id;
    req["input"] = prompt_with_context;
  } else if (provider.kind == "mechatropic_messages") {
    headers.emplace_back("anthropic-version", "2023-06-01");
    req["model"] = model.id;
    req["max_tokens"] = 1000;
    req["messages"] = nlohmann::json::array({nlohmann::json{
        {"role", "user"},
        {"content", prompt_with_context},
    }});
  }

  int status = 0;
  std::string response_body;
  std::string transport_error;
  CloudTransportPostOverride transport_override;
  {
    std::lock_guard<std::mutex> lock(g_cloud_transport_post_override_mu);
    transport_override = g_cloud_transport_post_override;
  }
  const bool transport_ok = transport_override
                                ? transport_override(provider.base_url,
                                                     target,
                                                     headers,
                                                     req.dump(),
                                                     &status,
                                                     &response_body,
                                                     &transport_error)
                                : https_post_json(provider.base_url,
                                                  target,
                                                  headers,
                                                  req,
                                                  &status,
                                                  &response_body,
                                                  &transport_error);
  if (!transport_ok) {
    if (error) {
      *error = "transport_error: " + (transport_error.empty() ? std::string("cloud request failed")
                                                               : transport_error);
    }
    return std::nullopt;
  }

  const auto parsed = parse_cloud_response(provider.kind, status, response_body);
  if (!parsed.text.has_value()) {
    if (error) {
      *error = parsed.error_code.empty() ? parsed.error_message
                                         : (parsed.error_code + ": " + parsed.error_message);
    }
    return std::nullopt;
  }
  return parsed.text.value();
}

void set_run_cloud_model_override_for_tests(CloudModelRunnerOverride fn) {
  std::lock_guard<std::mutex> lock(g_run_cloud_model_override_mu);
  g_run_cloud_model_override = std::move(fn);
}

void clear_run_cloud_model_override_for_tests() {
  std::lock_guard<std::mutex> lock(g_run_cloud_model_override_mu);
  g_run_cloud_model_override = nullptr;
}

void set_cloud_transport_post_override_for_tests(CloudTransportPostOverride fn) {
  std::lock_guard<std::mutex> lock(g_cloud_transport_post_override_mu);
  g_cloud_transport_post_override = std::move(fn);
}

void clear_cloud_transport_post_override_for_tests() {
  std::lock_guard<std::mutex> lock(g_cloud_transport_post_override_mu);
  g_cloud_transport_post_override = nullptr;
}

} // namespace holder::api::support
