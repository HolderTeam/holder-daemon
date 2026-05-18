#pragma once

#include <boost/interprocess/sync/file_lock.hpp>

#include <filesystem>
#include <memory>

namespace holder::core {

class LockFile {
 public:
  explicit LockFile(std::filesystem::path path);
  ~LockFile();

  LockFile(const LockFile&) = delete;
  LockFile& operator=(const LockFile&) = delete;

  LockFile(LockFile&& other) noexcept;
  LockFile& operator=(LockFile&& other) noexcept;

  // Returns true if the lock was acquired, false if already held elsewhere.
  bool try_acquire();

  void release();
  bool is_locked() const { return locked_; }

 private:
  void ensure_lock();

  std::filesystem::path path_;
  std::unique_ptr<boost::interprocess::file_lock> lock_;
  bool locked_ = false;
};

} // namespace holder::core
