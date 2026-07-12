# Holder Local Daemon

Holderd is a local-first card server, primarily used as a backend for card applications.

## What You Need

- CMake 3.22+
- Ninja
- C++20 compiler (GCC/Clang/MSVC)
- Git

Runtime/build dependencies used by this repo:

- Boost (`system`, `filesystem`)
- OpenSSL
- SQLite3
- nlohmann-json
- spdlog
- yaml-cpp
- XdgUtils BaseDir
- libgit2
- md4c
- libsodium
- libsecret (required on Ubuntu for encryption/recovery flows and full test pass)

`./make.sh` also handles the `caste` dependency:
- Git clone: initializes submodule automatically.
- ZIP download: fetches pinned `caste` archive (requires `curl` or `wget`).

Coverage tooling (optional):
- lcov (`lcov`, `genhtml`)

Model catalog config lives at `config/models.yaml` and is served by the API at `/models.yaml`.

## Optional Runtime Dependencies

- Ollama (for local model execution via AI routes)
  - Not required to build or run the core card/project APIs.
  - Backend looks for `ollama` on `PATH` and connects to `127.0.0.1:11434` by default.
  - Override host/port with `HOLDER_MODEL_RUNNER_HOST` and `HOLDER_MODEL_RUNNER_PORT`.

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
  libssl-dev \
  libsqlite3-dev nlohmann-json3-dev libspdlog-dev libyaml-cpp-dev \
  libgit2-dev libmd4c-dev xdg-utils-cxx-dev catch2 libsodium-dev \
  libsecret-1-dev \
  lcov

./make.sh
```

Server will start at `127.0.0.1:11499` by default and print docs URL + auth token in the terminal log.

## macOS / Windows

Holder is intended to be cross-platform, but Ubuntu has the smoothest setup today.

- macOS: use Homebrew equivalents for the dependencies above, then run `./make.sh`.
- Windows: use Visual Studio to build, or use WSL (Ubuntu) and follow the Ubuntu instructions.

Will need portability plan and testing, mainly around secrets since we need libsecret at the moment.

## Useful Commands

```bash
./make.sh                 # configure + build + tests + run holder
./make.sh Debug           # debug build
./make.sh perf-privacy    # run encrypted-card perf profile table
./make.sh perf-privacy Debug
./make.sh coverage        # build + run tests + generate HTML coverage report
./make.sh san             # ASan build + tests
HOLDER_SAN_DETECT_LEAKS=1 ./make.sh san  # ASan + LSan build + tests
./make.sh san address,undefined  # ASan + UBSan build + tests
./make.sh san thread      # TSan build + tests
./build/holderd --help
./build/holderd --reindex
./scripts/cloud-smoke.sh --provider switchyard --token "$HOLDER_TOKEN" --api-key "$SWITCHYARD_API_KEY"
./scripts/factory-reset.sh --force  # This wipes all user data, useful for development and testing the onboarding path. Warning: don't use on the actual holder instance you use as a user.
```

## Daemons.

In computing, a daemon is a program that runs as a background process,
rather than being under the direct control of an interactive user.
Sometimes this is called a service, a server or a backend,
but the original term from 1963 is a daemon,
based on the Ancient Greek word δαίμων.
This is a different word than demon, used in later Christian tradition for fallen angel.
Socrates described a daemon as an "attendant, ministering, or indwelling spirit; genius",
more like the Holy Spirit.
Demons are baddies, daemons are goodies.
