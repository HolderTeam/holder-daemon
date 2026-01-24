#include "core/LockFile.h"

#include <boost/interprocess/exceptions.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace holder::core {

LockFile::LockFile(std::filesystem::path path) : path_(std::move(path)) {}

LockFile::~LockFile() {
  try {
    release();
  } catch (...) {
    // Destructors must not throw.
  }
}

LockFile::LockFile(LockFile&& other) noexcept {
  path_ = std::move(other.path_);
  lock_ = std::move(other.lock_);
  locked_ = other.locked_;
  other.locked_ = false;
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    release();
    path_ = std::move(other.path_);
    lock_ = std::move(other.lock_);
    locked_ = other.locked_;
    other.locked_ = false;
  }
  return *this;
}

void LockFile::ensure_lock() {
  if (lock_) return;

  // Ensure file exists so file_lock can open it.
  std::ofstream touch(path_, std::ios::app);
  if (!touch.is_open()) {
    throw std::runtime_error("Failed to open lock file: " + path_.string());
  }
  touch.close();

  lock_ = std::make_unique<boost::interprocess::file_lock>(path_.string().c_str());
}

bool LockFile::try_acquire() {
  ensure_lock();

  if (locked_) return true;

  try {
    if (!lock_->try_lock()) return false;
  } catch (const boost::interprocess::interprocess_exception& ex) {
    throw std::runtime_error("Failed to acquire lock: " + path_.string() +
                             " (" + std::string(ex.what()) + ")");
  }

  locked_ = true;

  return true;
}

void LockFile::release() {
  if (lock_) {
    if (locked_) {
      try {
        lock_->unlock();
      } catch (...) {
      }
      locked_ = false;
    }
    lock_.reset();
  }
}

} // namespace holder::core
