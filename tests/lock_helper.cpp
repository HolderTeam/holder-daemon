#include "core/LockFile.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

int main(int argc, char* argv[]) {
  if (argc < 2) return 2;

  std::filesystem::path lock_path(argv[1]);
  unsigned long hold_ms = 0;
  if (argc >= 3) {
    hold_ms = std::strtoul(argv[2], nullptr, 10);
  }

  holder::core::LockFile lock(lock_path);
  const bool ok = lock.try_acquire();
  if (!ok) return 1;

  if (hold_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  }

  return 0;
}
