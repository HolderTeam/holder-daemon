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

`./make.sh` also handles the `caste` dependency:
- Git clone: initializes submodule automatically.
- ZIP download: fetches pinned `caste` archive (requires `curl` or `wget`).

## Quick Start (Ubuntu)

This is the primary tested dev path.

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl \
  libboost-system-dev libboost-filesystem-dev \
  libsqlite3-dev nlohmann-json3-dev libspdlog-dev libyaml-cpp-dev \
  libgit2-dev libmd4c-dev xdg-utils-cxx-dev catch2

./make.sh
```

Server will start at `127.0.0.1:11499` by default and print docs URL + auth token.

## macOS / Windows

Holder is intended to be cross-platform, but Ubuntu has the smoothest setup today.

- macOS: use Homebrew equivalents for the dependencies above, then run `./make.sh`.
- Windows: recommended path is WSL (Ubuntu) and follow the Ubuntu instructions.

## Useful Commands

```bash
./make.sh                 # configure + build + tests + run holder
./make.sh Debug           # debug build
./build/holder --help
./build/holder --reindex
```
