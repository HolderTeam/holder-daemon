#pragma once
#include <filesystem>
#include <string>

namespace holder::core {

struct Paths {
  std::filesystem::path data_dir; // ~/.local/share/holder
  std::filesystem::path config_dir; // ~/.config/holder
  std::filesystem::path cache_dir; // ~/.cache/holder

  std::filesystem::path server_dir() const { return data_dir / "server"; }
  std::filesystem::path db_path() const { return server_dir() / "holder.db"; }
  std::filesystem::path lock_path() const { return server_dir() / "holder.lock"; }
  std::filesystem::path info_path() const { return server_dir() / "holder.json"; }
  std::filesystem::path log_dir() const { return server_dir() / "logs"; }

  // For unix sockets, you may later prefer XDG_RUNTIME_DIR, but this is fine for now.
  std::filesystem::path socket_path() const { return server_dir() / "holder.sock"; }

  static Paths resolve(const std::string& app_id = "holder");
  void ensure_dirs() const;
};

} // namespace holder::core
