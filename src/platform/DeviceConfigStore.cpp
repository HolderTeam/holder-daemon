#include "platform/DeviceConfigStore.h"

#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "ai/AiRunnerRepo.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#endif

namespace holder::core {
namespace {

// A fixed ".tmp" name would let two concurrent writers race on the same temp file
// before either atomic rename happens, corrupting it. Make each writer's temp file
// unique.
std::string unique_temp_suffix() {
  static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(::_getpid());
#else
  const auto pid = static_cast<unsigned long>(::getpid());
#endif
  return "." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1));
}

std::mutex config_mutex;
std::optional<std::filesystem::path> configured_path;

nlohmann::json optional_string_json(const std::optional<std::string>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

void restrict_file(const std::filesystem::path& path) {
#ifndef _WIN32
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to restrict device config permissions: " + path.string());
  }
#else
  (void)path;
#endif
}

nlohmann::json snapshot(holder::platform::Db& db) {
  nlohmann::json body = {
      {"version", 1},
      {"local_model", nullptr},
      {"manual_runners", nlohmann::json::array()},
      {"provider_settings", nlohmann::json::array()},
  };
  if (const auto config = holder::ai::AiLocalModelConfigRepo(db).get(); config.has_value()) {
    body["local_model"] = {
        {"fast_model", optional_string_json(config->fast_model)},
        {"strong_model", optional_string_json(config->strong_model)},
        {"deep_model", optional_string_json(config->deep_model)},
        {"updated_at", config->updated_at},
    };
  }
  for (const auto& runner : holder::ai::AiRunnerRepo(db).list()) {
    if (runner.source == "auto_local") continue;
    body["manual_runners"].push_back({
        {"runner_id", runner.runner_id},
        {"name", runner.name},
        {"kind", runner.kind},
        {"base_url", optional_string_json(runner.base_url)},
        {"source", runner.source},
        {"enabled", runner.enabled},
        {"created_at", runner.created_at},
        {"updated_at", runner.updated_at},
    });
  }
  for (const auto& setting : holder::ai::AiProviderSettingRepo(db).list()) {
    body["provider_settings"].push_back({
        {"provider", setting.provider},
        {"enabled", setting.enabled},
        {"updated_at", setting.updated_at},
    });
  }
  return body;
}

void write_snapshot(holder::platform::Db& db, const std::filesystem::path& path) {
  const auto body = snapshot(db);
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp" + unique_temp_suffix();
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write device config: " + temporary.string());
    out << body.dump(2) << '\n';
    out.flush();
    if (!out) throw std::runtime_error("failed to flush device config: " + temporary.string());
  }
  restrict_file(temporary);
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
#ifdef _WIN32
  if (ec && std::filesystem::exists(path)) {
    std::filesystem::remove(path, ec);
    if (!ec) std::filesystem::rename(temporary, path, ec);
  }
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace device config: " + ec.message());
  }
  restrict_file(path);
}

std::optional<std::string> optional_string(const nlohmann::json& body, const char* key) {
  if (!body.contains(key) || body.at(key).is_null()) return std::nullopt;
  return body.at(key).get<std::string>();
}

void restore_unlocked(holder::platform::Db& db, const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open device config: " + path.string());
  const auto body = nlohmann::json::parse(in);
  if (body.value("version", 0) != 1) {
    throw std::runtime_error("unsupported device config version: " + path.string());
  }

  holder::ai::AiLocalModelConfigRepo local(db);
  if (body.contains("local_model") && !body.at("local_model").is_null()) {
    const auto& value = body.at("local_model");
    local.set(
        optional_string(value, "fast_model"),
        optional_string(value, "strong_model"),
        optional_string(value, "deep_model"),
        value.at("updated_at").get<long long>()
    );
  } else {
    local.clear();
  }

  holder::ai::AiRunnerRepo runners(db);
  for (const auto& existing : runners.list()) {
    if (existing.source != "auto_local") runners.remove(existing.runner_id);
  }
  for (const auto& value : body.value("manual_runners", nlohmann::json::array())) {
    holder::model::AiRunner runner;
    runner.runner_id = value.at("runner_id").get<std::string>();
    runner.name = value.at("name").get<std::string>();
    runner.kind = value.at("kind").get<std::string>();
    runner.base_url = optional_string(value, "base_url");
    runner.source = value.at("source").get<std::string>();
    runner.enabled = value.at("enabled").get<bool>();
    runner.created_at = value.at("created_at").get<long long>();
    runner.updated_at = value.at("updated_at").get<long long>();
    runners.upsert(runner);
  }

  holder::ai::AiProviderSettingRepo settings(db);
  for (const auto& existing : settings.list()) settings.remove(existing.provider);
  for (const auto& value : body.value("provider_settings", nlohmann::json::array())) {
    settings.upsert(
        value.at("provider").get<std::string>(),
        value.at("enabled").get<bool>(),
        value.at("updated_at").get<long long>()
    );
  }
}

} // namespace

void initialize_device_config(holder::platform::Db& db, const std::filesystem::path& path) {
  std::lock_guard lock(config_mutex);
  configured_path = path;
  if (std::filesystem::exists(path)) {
    restore_unlocked(db, path);
  } else {
    write_snapshot(db, path);
  }
}

void persist_device_config(holder::platform::Db& db) {
  std::lock_guard lock(config_mutex);
  if (!configured_path.has_value()) return;
  write_snapshot(db, *configured_path);
}

void restore_device_config(holder::platform::Db& db, const std::filesystem::path& path) {
  std::lock_guard lock(config_mutex);
  restore_unlocked(db, path);
}

} // namespace holder::core
