#pragma once

#include "platform/Signal.h"

#include <filesystem>

namespace holder::sync {

class ProjectSyncWorker {
 public:
  explicit ProjectSyncWorker(
      std::filesystem::path db_path,
      int push_interval_seconds = 1200,
      int pull_interval_seconds = 300,
      int poll_interval_seconds = 30
  );

  void run(const holder::core::SignalHandler& signals);

  static void set_fail_post_pull_metrics_for_tests(bool enabled);
  static void set_fail_post_push_metrics_for_tests(bool enabled);

 private:
  long long now_epoch_seconds() const;
  void run_startup_pull_pass();
  void run_push_cycle();

  std::filesystem::path db_path_;
  int push_interval_seconds_ = 1200;
  int pull_interval_seconds_ = 300;
  int poll_interval_seconds_ = 30;
};

} // namespace holder::sync
