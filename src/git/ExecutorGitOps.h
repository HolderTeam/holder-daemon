#pragma once

#include "core/SerialExecutor.h"
#include "git/GitOps.h"

namespace holder::git {

class ExecutorGitOps final : public GitOps {
 public:
  ExecutorGitOps(GitOps& inner, const holder::core::SerialExecutor& executor);

  void open_or_init(const std::filesystem::path& repo_dir) override;
  void write_file(const std::filesystem::path& relative_path,
                  const std::string& content) override;
  void stage_path(const std::filesystem::path& relative_path) override;
  void remove_path(const std::filesystem::path& relative_path) override;
  void commit(const std::string& message) override;
  void set_remote(const std::string& name, const std::string& url) override;
  void remove_remote(const std::string& name) override;
  void pull_remote_ff_only(const std::string& name) override;
  RemoteProbeResult probe_remote(const std::string& name) override;
  PushResult push_branch(const std::string& name,
                         const std::string& branch,
                         bool set_upstream) override;
  std::filesystem::path repo_dir() const override;

 private:
  GitOps& inner_;
  const holder::core::SerialExecutor& executor_;
};

} // namespace holder::git
