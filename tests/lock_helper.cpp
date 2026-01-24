#include "core/LockFile.h"

#include <filesystem>

int main(int argc, char* argv[]) {
  if (argc < 2) return 2;

  std::filesystem::path lock_path(argv[1]);
  holder::core::LockFile lock(lock_path);
  const bool ok = lock.try_acquire();
  return ok ? 0 : 1;
}
