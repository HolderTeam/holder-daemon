#include "git/GitRepo.h"

#include <git2.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace holder::git {

static std::runtime_error git_err(const std::string& what, int rc) {
  const git_error* e = git_error_last();
  std::string msg = what + " (rc=" + std::to_string(rc) + ")";
  if (e && e->message) {
    msg += ": ";
    msg += e->message;
  }
  return std::runtime_error(msg);
}

GitRepo::GitRepo() {
  git_libgit2_init();
}

GitRepo::~GitRepo() {
  if (repo_) {
    git_repository_free(reinterpret_cast<git_repository*>(repo_));
    repo_ = nullptr;
  }
  git_libgit2_shutdown();
}

void GitRepo::ensure_open() const {
  if (!repo_) throw std::runtime_error("GitRepo not opened");
}

void GitRepo::open_or_init(const fs::path& repo_dir) {
  if (repo_) {
    git_repository_free(reinterpret_cast<git_repository*>(repo_));
    repo_ = nullptr;
  }

  repo_dir_ = repo_dir;
  std::error_code ec;
  fs::create_directories(repo_dir_, ec);
  if (ec) throw std::runtime_error("Failed to create repo dir: " + repo_dir_.string() + " (" + ec.message() + ")");

  git_repository* r = nullptr;

  // Try open first
  int rc = git_repository_open(&r, repo_dir_.string().c_str());
  if (rc == 0) {
    repo_ = r;
    spdlog::info("Opened existing git repo: {}", repo_dir_.string());
    return;
  }

  // Otherwise init
  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  opts.flags = GIT_REPOSITORY_INIT_MKPATH; // make dirs
  opts.mode = GIT_REPOSITORY_INIT_SHARED_UMASK;

  rc = git_repository_init_ext(&r, repo_dir_.string().c_str(), &opts);
  if (rc != 0) throw git_err("git_repository_init_ext failed", rc);

  repo_ = r;
  spdlog::info("Initialized new git repo: {}", repo_dir_.string());
}

void GitRepo::write_file(const fs::path& relative_path, const std::string& content) {
  ensure_open();

  // repo workdir:
  const char* wd = git_repository_workdir(reinterpret_cast<git_repository*>(repo_));
  if (!wd) throw std::runtime_error("Repository has no working directory (bare?)");

  fs::path full = fs::path(wd) / relative_path;

  std::error_code ec;
  fs::create_directories(full.parent_path(), ec);
  if (ec) throw std::runtime_error("Failed to create dirs: " + full.parent_path().string() + " (" + ec.message() + ")");

  std::ofstream out(full, std::ios::binary);
  if (!out.is_open()) throw std::runtime_error("Failed to open for write: " + full.string());
  out << content;
  out.close();

  spdlog::info("Wrote file: {}", full.string());
}

void GitRepo::stage_path(const fs::path& relative_path) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  // libgit2 expects POSIX paths inside repo
  std::string p = relative_path.generic_string();

  rc = git_index_add_bypath(index, p.c_str());
  if (rc != 0) {
    git_index_free(index);
    throw git_err("git_index_add_bypath failed for " + p, rc);
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  spdlog::info("Staged path: {}", p);
}

void GitRepo::remove_path(const fs::path& relative_path) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  std::string p = relative_path.generic_string();
  rc = git_index_remove_bypath(index, p.c_str());
  if (rc != 0) {
    git_index_free(index);
    throw git_err("git_index_remove_bypath failed for " + p, rc);
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  spdlog::info("Removed path: {}", p);
}

void GitRepo::make_signature(void** out_sig) const {
  ensure_open();
  git_signature* sig = nullptr;

  // Try user's git config (user.name/user.email)
  int rc = git_signature_default(&sig, reinterpret_cast<git_repository*>(repo_));
  if (rc == 0 && sig) {
    *out_sig = sig;
    return;
  }

  // Fallback: placeholders (v0.1). Cards can later help set real identity.
  rc = git_signature_now(&sig, "Holder", "holder@localhost");
  if (rc != 0) throw git_err("git_signature_now failed", rc);

  *out_sig = sig;
}

void GitRepo::commit(const std::string& message) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  git_oid tree_oid{};
  rc = git_index_write_tree(&tree_oid, index);
  if (rc != 0) {
    git_index_free(index);
    throw git_err("git_index_write_tree failed", rc);
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  git_tree* tree = nullptr;
  rc = git_tree_lookup(&tree, reinterpret_cast<git_repository*>(repo_), &tree_oid);
  if (rc != 0) throw git_err("git_tree_lookup failed", rc);

  git_signature* sig = nullptr;
  make_signature(reinterpret_cast<void**>(&sig));

  // Determine if HEAD exists (initial commit vs subsequent)
  git_reference* head_ref = nullptr;
  rc = git_repository_head(&head_ref, reinterpret_cast<git_repository*>(repo_));

  git_oid commit_oid{};

  // No commits yet (either HEAD missing or branch is unborn)
  if (rc == GIT_ENOTFOUND || rc == GIT_EUNBORNBRANCH) {
    rc = git_commit_create_v(
      &commit_oid,
      reinterpret_cast<git_repository*>(repo_),
      "HEAD",
      sig,
      sig,
      nullptr,
      message.c_str(),
      tree,
      0);

    git_tree_free(tree);
    git_signature_free(sig);

    if (rc != 0) throw git_err("git_commit_create_v (initial) failed", rc);

    spdlog::info("Created initial commit.");
    return;
  }

  if (rc != 0) {
    git_tree_free(tree);
    git_signature_free(sig);
    throw git_err("git_repository_head failed", rc);
  }

  // Lookup parent commit
  const git_oid* parent_oid = git_reference_target(head_ref);
  if (!parent_oid) {
    git_reference_free(head_ref);
    git_tree_free(tree);
    git_signature_free(sig);
    throw std::runtime_error("HEAD has no target oid");
  }

  git_commit* parent = nullptr;
  rc = git_commit_lookup(&parent, reinterpret_cast<git_repository*>(repo_), parent_oid);
  git_reference_free(head_ref);
  if (rc != 0) {
    git_tree_free(tree);
    git_signature_free(sig);
    throw git_err("git_commit_lookup failed", rc);
  }

  rc = git_commit_create_v(
      &commit_oid,
      reinterpret_cast<git_repository*>(repo_),
      "HEAD",
      sig,
      sig,
      nullptr,
      message.c_str(),
      tree,
      1,
      parent);

  git_commit_free(parent);
  git_tree_free(tree);
  git_signature_free(sig);

  if (rc != 0) throw git_err("git_commit_create_v failed", rc);

  spdlog::info("Created commit: {}", message);
}

void GitRepo::set_remote(const std::string& name, const std::string& url) {
  ensure_open();

  git_remote* remote = nullptr;
  const int lookup = git_remote_lookup(&remote, reinterpret_cast<git_repository*>(repo_), name.c_str());
  if (lookup == 0) {
    git_remote_free(remote);
    const int rc = git_remote_set_url(reinterpret_cast<git_repository*>(repo_), name.c_str(), url.c_str());
    if (rc != 0) throw git_err("git_remote_set_url failed", rc);
    spdlog::info("Updated git remote {} -> {}", name, url);
    return;
  }
  if (lookup != GIT_ENOTFOUND) {
    throw git_err("git_remote_lookup failed", lookup);
  }

  const int rc = git_remote_create(&remote,
                                   reinterpret_cast<git_repository*>(repo_),
                                   name.c_str(),
                                   url.c_str());
  if (rc != 0) throw git_err("git_remote_create failed", rc);
  git_remote_free(remote);
  spdlog::info("Created git remote {} -> {}", name, url);
}

void GitRepo::remove_remote(const std::string& name) {
  ensure_open();

  const int rc = git_remote_delete(reinterpret_cast<git_repository*>(repo_), name.c_str());
  if (rc == GIT_ENOTFOUND) {
    return;
  }
  if (rc != 0) throw git_err("git_remote_delete failed", rc);
  spdlog::info("Removed git remote {}", name);
}

} // namespace holder::git
