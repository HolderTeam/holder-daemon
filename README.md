# Holder

Local-first card server.

## What You Need

- CMake 3.22+
- Ninja
- C++20 compiler (GCC/Clang/MSVC)
- Git

Runtime/build dependencies used by this repo:

- Boost (`system`, `filesystem`)
- SQLite3
- nlohmann-json
- spdlog
- yaml-cpp
- XdgUtils BaseDir
- libgit2
- md4c
- age

`./make.sh` also handles the `caste` dependency:
- Git clone: initializes submodule automatically.
- ZIP download: fetches pinned `caste` archive (requires `curl` or `wget`).

Model catalog config lives at `config/models.yaml` and is served by the API at `/models.yaml`.

## Quick Start (Ubuntu)

The idea is that it should work on any OS.

If you want to contribue instructions for your distro/Operating System,
that would be very welcome.

But here are friendly instructions for development on Ubuntu.

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl \
  libboost-system-dev libboost-filesystem-dev \
  libsqlite3-dev nlohmann-json3-dev libspdlog-dev libyaml-cpp-dev \
  libgit2-dev libmd4c-dev xdg-utils-cxx-dev catch2 libsodium-dev

./make.sh
```

Server will start at `127.0.0.1:11499` by default and print docs URL + auth token.

## macOS / Windows

Holder is intended to be cross-platform, but Ubuntu has the smoothest setup today.

- macOS: use Homebrew equivalents for the dependencies above, then run `./make.sh`.
- Windows: use Visual Studio to build, or use WSL (Ubuntu) and follow the Ubuntu instructions.

## Useful Commands

```bash
./make.sh                 # configure + build + tests + run holder
./make.sh Debug           # debug build
./make.sh perf-privacy    # run encrypted-card perf profile table
./make.sh perf-privacy Debug
./build/holder --help
./build/holder --reindex
./scripts/cloud-smoke.sh --provider switchyard --token "$HOLDER_TOKEN" --api-key "$SWITCHYARD_API_KEY"
./scripts/factory-reset.sh --force  # This wipes all user data, useful for development and testing the onboarding path. Warning: don't use on the actual holder instance you use as a user.
```
