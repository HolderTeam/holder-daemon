#include "platform/LockFile.h"

#include <boost/interprocess/exceptions.hpp>

#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace holder::core {
namespace {

std::atomic<bool> g_force_try_lock_throw_for_tests{false};
std::atomic<bool> g_force_unlock_throw_for_tests{false};
std::atomic<bool> g_force_release_throw_for_tests{false};

} // namespace

void lockfile_set_force_try_lock_throw_for_tests(bool enabled) {
  g_force_try_lock_throw_for_tests.store(enabled);
}

void lockfile_set_force_unlock_throw_for_tests(bool enabled) {
  g_force_unlock_throw_for_tests.store(enabled);
}

void lockfile_set_force_release_throw_for_tests(bool enabled) {
  g_force_release_throw_for_tests.store(enabled);
}

LockFile::LockFile(std::filesystem::path path)
    : path_(std::move(path)) {}

LockFile::~LockFile() { release_noexcept(); }

LockFile::LockFile(LockFile&& other) noexcept {
  path_ = std::move(other.path_);
  lock_ = std::move(other.lock_);
  locked_ = other.locked_;
  other.locked_ = false;
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    release_noexcept();
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
    if (g_force_try_lock_throw_for_tests.load()) {
      throw boost::interprocess::interprocess_exception("forced try_lock failure");
    }
    if (!lock_->try_lock()) return false;
  } catch (const boost::interprocess::interprocess_exception& ex) {
    throw std::runtime_error(
        "Failed to acquire lock: " + path_.string() + " (" + std::string(ex.what()) + ")"
    );
  }

  locked_ = true;

  return true;
}

void LockFile::release() {
  if (g_force_release_throw_for_tests.load()) {
    throw std::runtime_error("forced release failure");
  }
  if (lock_) {
    if (locked_) {
      try {
        if (g_force_unlock_throw_for_tests.load()) {
          throw std::runtime_error("forced unlock failure");
        }
        lock_->unlock();
      } catch (const std::exception& ex) {
        (void)ex;
      } catch (...) {
        const bool ignored = true;
        (void)ignored;
      }
      locked_ = false;
    }
    lock_.reset();
  }
}

void LockFile::release_noexcept() noexcept {
  try {
    release();
  } catch (const std::exception& ex) {
    (void)ex;
  } catch (...) {
    const bool ignored = true;
    (void)ignored;
  }
}

} // namespace holder::core
