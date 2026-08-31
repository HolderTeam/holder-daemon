#include "api/routes/AiResourceRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "platform/Paths.h"

#include "resource/AssetImportService.h"
#include "resource/AssetEnvelope.h"
#include "resource/LocalDirectoryProvider.h"
#include "resource/LocationBindingStore.h"
#include "resource/LocationRepo.h"
#include "resource/LocationStore.h"
#include "resource/ResourceRepo.h"
#include "resource/ResourceStore.h"
#include "storage/S3CompatibleProvider.h"

#include <boost/beast/http.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

struct ImportJob {
  std::string job_id;
  std::string status = "queued";
  std::string resource_id;
  std::string asset_id;
  bool duplicate_reused = false;
  bool link_created = false;
  std::string error;
};

std::mutex import_jobs_mutex;
std::unordered_map<std::string, ImportJob> import_jobs;
std::mutex import_threads_mutex;
std::vector<std::thread> import_threads;

std::string safe_download_filename(std::string filename) {
  for (auto& ch : filename) {
    if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20U) ch = '_';
  }
  if (filename.empty()) return "asset";
  return filename;
}

void cleanup_asset_cache(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return;
  const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const auto changed = entry.last_write_time(ec);
    if (!ec && changed < cutoff) std::filesystem::remove(entry.path(), ec);
    ec.clear();
  }
}

void stream_file_response(
    boost::asio::ip::tcp::socket& socket,
    const std::filesystem::path& path,
    const holder::model::Asset& asset
) {
  http::response<http::empty_body> header{http::status::ok, 11};
  header.set(http::field::content_type, asset.media_type);
  header.set(
      http::field::content_disposition,
      "attachment; filename=\"" + safe_download_filename(asset.original_filename) + "\""
  );
  header.content_length(static_cast<std::uint64_t>(asset.byte_size));
  header.keep_alive(false);
  http::response_serializer<http::empty_body> serializer{header};
  http::write_header(socket, serializer);

  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open recovered asset");
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      boost::asio::write(socket, boost::asio::buffer(buffer.data(), static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) throw std::runtime_error("failed while reading recovered asset");
}

void update_import_job(
    const std::string& job_id,
    const std::string& status,
    const std::string& error = {}
) {
  std::lock_guard<std::mutex> lock(import_jobs_mutex);
  auto found = import_jobs.find(job_id);
  if (found == import_jobs.end()) return;
  found->second.status = status;
  found->second.error = error;
}

nlohmann::json import_job_json(const ImportJob& job) {
  return {
      {"job_id", job.job_id},
      {"status", job.status},
      {"resource_id", job.resource_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.resource_id)},
      {"asset_id", job.asset_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.asset_id)},
      {"duplicate_reused", job.duplicate_reused},
      {"link_created", job.link_created},
      {"error", job.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.error)},
  };
}

nlohmann::json resource_json(const holder::model::ResourceBundle& bundle) {
  nlohmann::json assets = nlohmann::json::array();
  for (const auto& asset : bundle.assets) {
    nlohmann::json placements = nlohmann::json::array();
    for (const auto& placement : asset.placements) {
      placements.push_back({
          {"placement_id", placement.placement_id},
          {"location_id", placement.location_id},
          {"encoding", placement.encoding},
          {"stored_byte_size", placement.stored_byte_size},
          {"created_at", placement.created_at},
      });
    }
    assets.push_back({
        {"asset_id", asset.asset_id},
        {"resource_id", asset.resource_id},
        {"original_filename", asset.original_filename},
        {"media_type", asset.media_type},
        {"byte_size", asset.byte_size},
        {"plaintext_sha256", asset.plaintext_sha256},
        {"created_at", asset.created_at},
        {"updated_at", asset.updated_at},
        {"placements", std::move(placements)},
    });
  }
  return {
      {"resource_id", bundle.resource.resource_id},
      {"project_id", bundle.resource.project_id},
      {"type", bundle.resource.type},
      {"label", bundle.resource.label},
      {"metadata", bundle.resource.metadata},
      {"created_at", bundle.resource.created_at},
      {"updated_at", bundle.resource.updated_at},
      {"assets", std::move(assets)},
  };
}

nlohmann::json location_json(
    const holder::model::Location& location,
    holder::resource::LocationBindingStore* bindings
) {
  const auto preview = bindings ? bindings->preview(location.project_id, location.location_id)
                                : std::nullopt;
  return {
      {"location_id", location.location_id},
      {"project_id", location.project_id},
      {"name", location.name},
      {"provider", location.provider},
      {"configuration", location.configuration},
      {"bound", preview.has_value()},
      {"binding_preview", preview.has_value() ? nlohmann::json(*preview) : nlohmann::json(nullptr)},
      {"created_at", location.created_at},
      {"updated_at", location.updated_at},
  };
}

std::unique_ptr<holder::resource::StorageProvider> storage_provider(
    const holder::model::Location& location,
    const holder::resource::LocationBinding& binding
) {
  if (location.provider != binding.provider) {
    throw std::runtime_error("storage declaration and private binding provider mismatch");
  }
  if (location.provider == "local_directory") {
    const auto root = binding.values.find("root_path");
    if (root == binding.values.end() || root->second.empty()) {
      throw std::runtime_error("local directory binding requires root_path");
    }
    return std::make_unique<holder::resource::LocalDirectoryProvider>(root->second);
  }
  if (location.provider == "s3_compatible") {
    auto required_config = [&](const std::string& name) -> std::string {
      const auto value = location.configuration.find(name);
      if (value == location.configuration.end() || value->second.empty()) {
        throw std::runtime_error("S3 location is missing " + name);
      }
      return value->second;
    };
    auto required_secret = [&](const std::string& name) -> std::string {
      const auto value = binding.values.find(name);
      if (value == binding.values.end() || value->second.empty()) {
        throw std::runtime_error("S3 binding is missing " + name);
      }
      return value->second;
    };
    holder::storage::S3CompatibleConfig config;
    config.endpoint = binding.values.contains("endpoint")
                          ? binding.values.at("endpoint")
                          : required_config("endpoint");
    config.region = required_config("region");
    config.bucket = required_config("bucket");
    if (location.configuration.contains("addressing_style")) {
      config.addressing_style = location.configuration.at("addressing_style");
    }
    config.allow_insecure_localhost =
        location.configuration.contains("allow_insecure_localhost") &&
        location.configuration.at("allow_insecure_localhost") == "true";
    holder::storage::S3Credentials credentials;
    credentials.access_key_id = required_secret("access_key_id");
    credentials.secret_access_key = required_secret("secret_access_key");
    if (binding.values.contains("session_token") && !binding.values.at("session_token").empty()) {
      credentials.session_token = binding.values.at("session_token");
    }
    return std::make_unique<holder::storage::S3CompatibleProvider>(config, credentials);
  }
  throw std::runtime_error("unsupported storage provider");
}

std::string location_object_key(
    const holder::model::Location& location,
    const std::string& relative_key
) {
  auto prefix = location.configuration.contains("prefix")
                    ? location.configuration.at("prefix")
                    : std::string();
  while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  return prefix.empty() ? relative_key : prefix + "/" + relative_key;
}

http::response<http::string_body> route_error(const std::exception& ex) {
  const std::string message = ex.what();
  if (const auto* storage = dynamic_cast<const holder::resource::StorageError*>(&ex)) {
    switch (storage->code()) {
      case holder::resource::StorageErrorCode::Unavailable:
        return support::error_response(
            http::status::service_unavailable, "storage_unavailable", message
        );
      case holder::resource::StorageErrorCode::Authentication:
        return support::error_response(
            http::status::bad_gateway, "storage_authentication_failed", message
        );
      case holder::resource::StorageErrorCode::Permission:
        return support::error_response(
            http::status::bad_gateway, "storage_permission_denied", message
        );
      case holder::resource::StorageErrorCode::Capacity:
        return support::error_response(
            static_cast<http::status>(507), "storage_capacity_exceeded", message
        );
      case holder::resource::StorageErrorCode::Integrity:
        return support::error_response(
            http::status::unprocessable_entity, "storage_integrity_failed", message
        );
      case holder::resource::StorageErrorCode::Conflict:
        return support::error_response(http::status::conflict, "storage_conflict", message);
      case holder::resource::StorageErrorCode::InvalidConfiguration:
        return support::error_response(
            http::status::bad_request, "storage_configuration_invalid", message
        );
      case holder::resource::StorageErrorCode::Transient:
        return support::error_response(
            http::status::service_unavailable, "storage_transient_failure", message
        );
    }
  }
  if (message.find("storage location configuration required") != std::string::npos) {
    return support::error_response(
        http::status::conflict, "storage_binding_required", message
    );
  }
  if (message.rfind("conflict:", 0) == 0) {
    return support::error_response(http::status::conflict, "conflict", message);
  }
  if (message.find("not found") != std::string::npos) {
    return support::error_response(http::status::not_found, "not_found", message);
  }
  return support::error_response(http::status::bad_request, "bad_request", message);
}

} // namespace

void wait_for_asset_import_jobs() {
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(import_threads_mutex);
    threads.swap(import_threads);
  }
  for (auto& thread : threads) {
    if (thread.joinable()) thread.join();
  }
}

bool handle_ai_resource_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get,
    holder::privacy::SecretStore* secret_store,
    holder::git::GitOps* git_ops,
    boost::asio::ip::tcp::socket* socket,
    bool* streamed
) {
  if (streamed != nullptr) *streamed = false;
  std::unique_ptr<holder::resource::LocationBindingStore> bindings;
  if (secret_store != nullptr) {
    bindings = std::make_unique<holder::resource::LocationBindingStore>(*secret_store);
  }

  if (path == "/resources" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }
    try {
      holder::resource::ResourceRepo repo(db);
      nlohmann::json data = nlohmann::json::array();
      for (const auto& resource : repo.list(project_id)) {
        data.push_back(resource_json(*repo.get_bundle(resource.resource_id)));
      }
      res = support::json_response(http::status::ok, {{"ok", true}, {"data", std::move(data)}});
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path == "/resources" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("project_id") || !body.contains("type") || !body.contains("label")) {
        res = support::error_response(
            http::status::bad_request, "bad_request", "Missing required fields."
        );
        return true;
      }
      holder::model::ResourceBundle bundle;
      bundle.resource.resource_id = body.value("resource_id", std::string());
      if (bundle.resource.resource_id.empty()) bundle.resource.resource_id = uuid_v4();
      bundle.resource.project_id = body.at("project_id").get<std::string>();
      bundle.resource.type = body.at("type").get<std::string>();
      bundle.resource.label = body.at("label").get<std::string>();
      if (body.contains("metadata")) {
        bundle.resource.metadata =
            body.at("metadata").get<holder::model::ResourceMetadata>();
      }
      bundle.resource.created_at = body.value("created_at", support::now_epoch_seconds());
      bundle.resource.updated_at = body.value("updated_at", bundle.resource.created_at);
      if (git_ops != nullptr) {
        holder::resource::ResourceStore(db, nullptr, git_ops).put(bundle);
      } else {
        holder::resource::ResourceRepo(db).add(bundle.resource);
      }
      res = support::json_response(
          http::status::created, {{"ok", true}, {"data", resource_json(bundle)}}
      );
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path == "/locations" && req.method() == http::verb::get) {
    const auto project_id = param_get("project_id");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }
    try {
      nlohmann::json data = nlohmann::json::array();
      for (const auto& location : holder::resource::LocationRepo(db).list(project_id)) {
        data.push_back(location_json(location, bindings.get()));
      }
      nlohmann::json preferred = nullptr;
      if (bindings) {
        const auto value = bindings->preferred(project_id);
        if (value.has_value()) preferred = *value;
      }
      res = support::json_response(
          http::status::ok,
          {{"ok", true}, {"data", std::move(data)}, {"preferred_location_id", preferred}}
      );
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path == "/locations" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("project_id") || !body.contains("name") || !body.contains("provider")) {
        throw std::invalid_argument("Missing required fields.");
      }
      holder::model::Location location;
      location.location_id = body.value("location_id", std::string());
      if (location.location_id.empty()) location.location_id = uuid_v4();
      location.project_id = body.at("project_id").get<std::string>();
      location.name = body.at("name").get<std::string>();
      location.provider = body.at("provider").get<std::string>();
      if (location.provider != "local_directory" && location.provider != "s3_compatible") {
        throw std::invalid_argument("unsupported storage provider");
      }
      if (body.contains("configuration")) {
        location.configuration =
            body.at("configuration").get<std::map<std::string, std::string>>();
      }
      location.created_at = body.value("created_at", support::now_epoch_seconds());
      location.updated_at = body.value("updated_at", location.created_at);
      if (git_ops != nullptr) {
        holder::resource::LocationStore(db, nullptr, git_ops).put(location);
      } else {
        holder::resource::LocationRepo(db).put(location);
      }
      res = support::json_response(
          http::status::created,
          {{"ok", true}, {"data", location_json(location, bindings.get())}}
      );
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path == "/locations/preferred" && req.method() == http::verb::put) {
    try {
      if (!bindings) throw std::runtime_error("secret store unavailable");
      const auto body = nlohmann::json::parse(req.body());
      const auto project_id = body.at("project_id").get<std::string>();
      const auto location_id = body.at("location_id").get<std::string>();
      const auto location = holder::resource::LocationRepo(db).get(location_id);
      if (!location.has_value() || location->project_id != project_id) {
        throw std::runtime_error("location not found in project");
      }
      bindings->set_preferred(project_id, location_id, support::now_epoch_seconds());
      res = support::json_response(
          http::status::ok, {{"ok", true}, {"data", {{"location_id", location_id}}}}
      );
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path == "/imports" && req.method() == http::verb::post) {
    try {
      if (!bindings || git_ops == nullptr) {
        throw std::runtime_error("asset import services unavailable");
      }
      const auto body = nlohmann::json::parse(req.body());
      holder::resource::AssetImportRequest request;
      request.project_id = body.at("project_id").get<std::string>();
      request.card_id = body.at("card_id").get<std::string>();
      request.location_id = body.at("location_id").get<std::string>();
      request.source_file = body.at("source_path").get<std::string>();
      request.now = support::now_epoch_seconds();
      if (!std::filesystem::is_regular_file(request.source_file)) {
        throw std::invalid_argument("asset source must be a readable regular file");
      }
      const auto location = holder::resource::LocationRepo(db).get(request.location_id);
      if (!location.has_value() || location->project_id != request.project_id) {
        throw std::runtime_error("storage location not found in project");
      }
      const auto binding = bindings->get(request.project_id, request.location_id);
      if (!binding.has_value()) throw std::runtime_error("storage location configuration required");
      // Resolve declarations and credentials before handing work to the background job. The job
      // retains neither the HTTP body nor the local source path in durable project state.
      const auto location_copy = *location;
      const auto binding_copy = *binding;
      const auto db_path = db.path();
      const auto cache = holder::core::Paths::resolve("holder").cache_dir / "asset-staging";
      const auto job_id = uuid_v4();
      {
        std::lock_guard<std::mutex> lock(import_jobs_mutex);
        import_jobs[job_id] = ImportJob{
            .job_id = job_id,
            .status = "queued",
            .resource_id = {},
            .asset_id = {},
            .duplicate_reused = false,
            .link_created = false,
            .error = {},
        };
      }
      std::thread import_thread(
          [job_id,
           request,
           location_copy,
           binding_copy,
           db_path,
           cache,
           uuid_v4,
           git_ops]() mutable {
            try {
              holder::platform::Db job_db;
              job_db.open(db_path);
              auto provider = storage_provider(location_copy, binding_copy);
              holder::resource::AssetImportService importer(
                  job_db,
                  cache,
                  uuid_v4,
                  nullptr,
                  git_ops,
                  [job_id](holder::resource::AssetImportStage stage) {
                    switch (stage) {
                      case holder::resource::AssetImportStage::Staging:
                        update_import_job(job_id, "staging");
                        break;
                      case holder::resource::AssetImportStage::Storing:
                        update_import_job(job_id, "storing");
                        break;
                      case holder::resource::AssetImportStage::Committing:
                        update_import_job(job_id, "committing");
                        break;
                    }
                  }
              );
              const auto result = importer.import_file(request, *provider);
              std::lock_guard<std::mutex> lock(import_jobs_mutex);
              auto found = import_jobs.find(job_id);
              if (found != import_jobs.end()) {
                found->second.status = "completed";
                found->second.resource_id = result.resource_id;
                found->second.asset_id = result.asset_id;
                found->second.duplicate_reused = result.duplicate_reused;
                found->second.link_created = result.link_created;
              }
            } catch (const std::exception& ex) {
              update_import_job(job_id, "failed", ex.what());
            } catch (...) {
              update_import_job(job_id, "failed", "unknown asset import error");
            }
          }
      );
      {
        std::lock_guard<std::mutex> lock(import_threads_mutex);
        import_threads.push_back(std::move(import_thread));
      }
      res = support::json_response(
          http::status::accepted,
          {{"ok", true},
           {"data",
            import_job_json(ImportJob{
                .job_id = job_id,
                .status = "queued",
                .resource_id = {},
                .asset_id = {},
                .duplicate_reused = false,
                .link_created = false,
                .error = {},
            })}}
      );
    } catch (const std::exception& ex) {
      res = route_error(ex);
    }
    return true;
  }

  if (path.rfind("/imports/", 0) == 0 && req.method() == http::verb::get) {
    const auto job_id = path.substr(std::string("/imports/").size());
    std::lock_guard<std::mutex> lock(import_jobs_mutex);
    const auto found = import_jobs.find(job_id);
    if (job_id.empty() || found == import_jobs.end()) {
      res = support::error_response(http::status::not_found, "not_found", "Import job not found.");
    } else {
      res = support::json_response(
          http::status::ok, {{"ok", true}, {"data", import_job_json(found->second)}}
      );
    }
    return true;
  }

  if (path.rfind("/resources/", 0) == 0 &&
      path.find("/assets/") != std::string::npos &&
      path.ends_with("/content") &&
      req.method() == http::verb::get) {
    try {
      if (!bindings || git_ops == nullptr || socket == nullptr || streamed == nullptr) {
        throw std::runtime_error("asset retrieval services unavailable");
      }
      const auto resource_start = std::string("/resources/").size();
      const auto assets_pos = path.find("/assets/", resource_start);
      const auto content_pos = path.size() - std::string("/content").size();
      const auto resource_id = path.substr(resource_start, assets_pos - resource_start);
      const auto asset_id = path.substr(
          assets_pos + std::string("/assets/").size(),
          content_pos - (assets_pos + std::string("/assets/").size())
      );
      const auto bundle = holder::resource::ResourceRepo(db).get_bundle(resource_id);
      if (!bundle.has_value()) throw std::runtime_error("resource not found");
      const auto asset = std::find_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& item) {
        return item.asset_id == asset_id;
      });
      if (asset == bundle->assets.end()) throw std::runtime_error("asset not found in resource");
      const auto requested_placement = param_get("placement_id");
      const auto placement = std::find_if(
          asset->placements.begin(), asset->placements.end(), [&](const auto& item) {
            return requested_placement.empty() || item.placement_id == requested_placement;
          }
      );
      if (placement == asset->placements.end()) throw std::runtime_error("asset placement not found");
      const auto location = holder::resource::LocationRepo(db).get(placement->location_id);
      if (!location.has_value() || location->project_id != bundle->resource.project_id) {
        throw std::runtime_error("storage location not found in project");
      }
      const auto binding = bindings->get(location->project_id, location->location_id);
      if (!binding.has_value()) throw std::runtime_error("storage location configuration required");
      auto provider = storage_provider(*location, *binding);
      const auto cache = holder::core::Paths::resolve("holder").cache_dir / "asset-cache";
      std::filesystem::create_directories(cache);
      cleanup_asset_cache(cache);
      const auto recovered = cache / (uuid_v4() + ".recovered");
      try {
        holder::resource::AssetImportService(db, cache, uuid_v4, nullptr, git_ops).retrieve(
            resource_id, asset_id, placement->placement_id, *provider, recovered
        );
        *streamed = true;
        stream_file_response(*socket, recovered, *asset);
        std::filesystem::remove(recovered);
      } catch (...) {
        std::filesystem::remove(recovered);
        throw;
      }
    } catch (const std::exception& ex) {
      if (streamed != nullptr && *streamed) {
        boost::system::error_code ignored;
        socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
      } else {
        res = route_error(ex);
      }
    }
    return true;
  }

  if (path.rfind("/locations/", 0) == 0) {
    const auto suffix = path.substr(std::string("/locations/").size());
    const auto separator = suffix.find('/');
    const auto location_id = suffix.substr(0, separator);
    const auto action =
        separator == std::string::npos ? std::string() : suffix.substr(separator + 1);
    if (location_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      return true;
    }

    if (action == "binding" && req.method() == http::verb::put) {
      try {
        if (!bindings) throw std::runtime_error("secret store unavailable");
        const auto location = holder::resource::LocationRepo(db).get(location_id);
        if (!location.has_value()) throw std::runtime_error("location not found");
        const auto body = nlohmann::json::parse(req.body());
        holder::resource::LocationBinding binding;
        binding.provider = location->provider;
        binding.values = body.at("values").get<std::map<std::string, std::string>>();
        const auto preview = body.value("preview", std::string("Configured"));
        bindings->bind(
            location->project_id,
            location_id,
            binding,
            preview,
            support::now_epoch_seconds()
        );
        res = support::json_response(
            http::status::ok,
            {{"ok", true},
             {"data",
              {{"location_id", location_id}, {"bound", true}, {"binding_preview", preview}}}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }

    if (action == "binding" && req.method() == http::verb::delete_) {
      try {
        if (!bindings) throw std::runtime_error("secret store unavailable");
        const auto location = holder::resource::LocationRepo(db).get(location_id);
        if (!location.has_value()) throw std::runtime_error("location not found");
        bindings->unbind(location->project_id, location_id);
        if (bindings->preferred(location->project_id) == location_id) {
          bindings->clear_preferred(location->project_id);
        }
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", {{"location_id", location_id}}}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }

    if (action == "test" && req.method() == http::verb::post) {
      try {
        if (!bindings) throw std::runtime_error("secret store unavailable");
        const auto location = holder::resource::LocationRepo(db).get(location_id);
        if (!location.has_value()) throw std::runtime_error("location not found");
        const auto binding = bindings->get(location->project_id, location_id);
        if (!binding.has_value()) throw std::runtime_error("storage location configuration required");
        auto provider = storage_provider(*location, *binding);
        const auto probe_dir = holder::core::Paths::resolve("holder").cache_dir / "asset-probes";
        std::filesystem::create_directories(probe_dir);
        const auto probe_file = probe_dir / (uuid_v4() + ".probe");
        std::ofstream(probe_file, std::ios::binary) << "Holder storage probe\n";
        const auto digest = holder::resource::digest_file(probe_file);
        const auto object_key = location_object_key(*location, ".holder-probes/" + uuid_v4());
        bool remote_cleanup_needed = false;
        try {
          // A PUT can reach the provider even if the client subsequently sees an error. Mark the
          // object as possibly present before starting the request so every failure path attempts
          // best-effort cleanup.
          remote_cleanup_needed = true;
          provider->put(object_key, probe_file, digest.byte_size, digest.sha256);
          if (!provider->exists(object_key)) throw std::runtime_error("storage probe not found");
          provider->remove(object_key);
          remote_cleanup_needed = false;
        } catch (...) {
          if (remote_cleanup_needed) {
            try {
              provider->remove(object_key);
            } catch (...) {
            }
          }
          std::error_code ignored;
          std::filesystem::remove(probe_file, ignored);
          throw;
        }
        std::error_code ignored;
        std::filesystem::remove(probe_file, ignored);
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", {{"available", true}}}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }

    if (action.empty() && req.method() == http::verb::get) {
      const auto location = holder::resource::LocationRepo(db).get(location_id);
      if (!location.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "location not found");
      } else {
        res = support::json_response(
            http::status::ok,
            {{"ok", true}, {"data", location_json(*location, bindings.get())}}
        );
      }
      return true;
    }

    if (action.empty() && req.method() == http::verb::patch) {
      try {
        auto location = holder::resource::LocationRepo(db).get(location_id);
        if (!location.has_value()) throw std::runtime_error("location not found");
        const auto body = nlohmann::json::parse(req.body());
        if (body.contains("name")) location->name = body.at("name").get<std::string>();
        if (body.contains("configuration")) {
          location->configuration =
              body.at("configuration").get<std::map<std::string, std::string>>();
        }
        location->updated_at = body.value("updated_at", support::now_epoch_seconds());
        if (git_ops != nullptr) {
          holder::resource::LocationStore(db, nullptr, git_ops).put(*location);
        } else {
          holder::resource::LocationRepo(db).put(*location);
        }
        res = support::json_response(
            http::status::ok,
            {{"ok", true}, {"data", location_json(*location, bindings.get())}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }

    if (action.empty() && req.method() == http::verb::delete_) {
      try {
        const auto location = holder::resource::LocationRepo(db).get(location_id);
        if (!location.has_value()) throw std::runtime_error("location not found");
        if (git_ops != nullptr) {
          holder::resource::LocationStore(db, nullptr, git_ops).remove(location_id);
        } else {
          holder::resource::LocationRepo(db).remove(location_id);
        }
        if (bindings && bindings->preferred(location->project_id) == location_id) {
          bindings->clear_preferred(location->project_id);
        }
        if (bindings) bindings->unbind(location->project_id, location_id);
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", {{"location_id", location_id}}}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }

    res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    return true;
  }

  if (path.rfind("/resources/", 0) == 0) {
    const auto resource_id = path.substr(std::string("/resources/").size());
    if (resource_id.empty() || resource_id.find('/') != std::string::npos) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      return true;
    }
    if (req.method() == http::verb::get) {
      const auto bundle = holder::resource::ResourceRepo(db).get_bundle(resource_id);
      if (!bundle.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Resource not found.");
      } else {
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", resource_json(*bundle)}}
        );
      }
      return true;
    }
    if (req.method() == http::verb::patch) {
      try {
        holder::resource::ResourceRepo repo(db);
        auto bundle = repo.get_bundle(resource_id);
        if (!bundle.has_value()) throw std::runtime_error("Resource not found.");
        const auto body = nlohmann::json::parse(req.body());
        if (body.contains("type")) bundle->resource.type = body.at("type").get<std::string>();
        if (body.contains("label")) bundle->resource.label = body.at("label").get<std::string>();
        if (body.contains("metadata")) {
          const auto patch = body.at("metadata").get<holder::model::ResourceMetadata>();
          for (const auto& [property, values] : patch) {
            if (values.empty()) {
              bundle->resource.metadata.erase(property);
            } else {
              bundle->resource.metadata[property] = values;
            }
          }
        }
        bundle->resource.updated_at = body.value("updated_at", support::now_epoch_seconds());
        if (git_ops != nullptr) {
          holder::resource::ResourceStore(db, nullptr, git_ops).put(*bundle);
        } else {
          repo.put_bundle(*bundle);
        }
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", resource_json(*bundle)}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }
    if (req.method() == http::verb::delete_) {
      try {
        if (git_ops != nullptr) {
          holder::resource::ResourceStore(db, nullptr, git_ops).remove(resource_id);
        } else {
          holder::resource::ResourceRepo(db).remove(resource_id);
        }
        res = support::json_response(
            http::status::ok, {{"ok", true}, {"data", {{"resource_id", resource_id}}}}
        );
      } catch (const std::exception& ex) {
        res = route_error(ex);
      }
      return true;
    }
    res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    return true;
  }

  return false;
}

} // namespace holder::api::routes
