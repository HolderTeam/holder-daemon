#include "git/ExecutorGitOps.h"

namespace holder::git {

ExecutorGitOps::ExecutorGitOps(GitOps& inner, const holder::core::SerialExecutor& executor)
    : inner_(inner), executor_(executor) {}

void ExecutorGitOps::open_or_init(const std::filesystem::path& repo_dir) {
  executor_.call([&]() { inner_.open_or_init(repo_dir); });
}

void ExecutorGitOps::write_file(const std::filesystem::path& relative_path,
                                const std::string& content) {
  executor_.call([&]() { inner_.write_file(relative_path, content); });
}

void ExecutorGitOps::stage_path(const std::filesystem::path& relative_path) {
  executor_.call([&]() { inner_.stage_path(relative_path); });
}

void ExecutorGitOps::remove_path(const std::filesystem::path& relative_path) {
  executor_.call([&]() { inner_.remove_path(relative_path); });
}

void ExecutorGitOps::commit(const std::string& message) {
  executor_.call([&]() { inner_.commit(message); });
}

void ExecutorGitOps::set_remote(const std::string& name, const std::string& url) {
  executor_.call([&]() { inner_.set_remote(name, url); });
}

void ExecutorGitOps::remove_remote(const std::string& name) {
  executor_.call([&]() { inner_.remove_remote(name); });
}

void ExecutorGitOps::pull_remote_ff_only(const std::string& name) {
  executor_.call([&]() { inner_.pull_remote_ff_only(name); });
}

RemoteProbeResult ExecutorGitOps::probe_remote(const std::string& name) {
  return executor_.call([&]() { return inner_.probe_remote(name); });
}

PushResult ExecutorGitOps::push_branch(const std::string& name,
                                       const std::string& branch,
                                       bool set_upstream) {
  return executor_.call([&]() { return inner_.push_branch(name, branch, set_upstream); });
}

std::filesystem::path ExecutorGitOps::repo_dir() const {
  return executor_.call([&]() { return inner_.repo_dir(); });
}

} // namespace holder::git
