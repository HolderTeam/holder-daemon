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
- libgit2
- md4c
- libsodium
- platform keyring support: libsecret on Linux, Keychain on macOS, Credential Manager on Windows

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
  libgit2-dev libmd4c-dev catch2 libsodium-dev \
  libsecret-1-dev \
  lcov

./make.sh
```

Server will start at `127.0.0.1:11499` by default and print docs URL + auth token in the terminal log.

## Quick Start (FreeBSD)

```sh
sudo pkg install \
  cmake ninja pkgconf git curl \
  boost-libs openssl sqlite3 nlohmann-json spdlog yaml-cpp \
  libgit2 md4c catch2 libsodium

./make.sh
```

## Quick Start (Mac OS)

```sh
brew install boost openssl@3 sqlite nlohmann-json spdlog yaml-cpp libgit2 md4c libsodium lcov

./make.sh
```

## Quick Start (Windows)

I compiled it using Visual Studio (below), but there is a command line way
if you are an established dev on Windows and know what you are doing.

### Command line version

You may need this:

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
```

#### Build

```powershell

cmake --preset windows-vcpkg-debug
cmake --build --preset windows-vcpkg-debug
```

#### Test run:

```powershell
cmake --preset windows-vcpkg-tests-debug
cmake --build --preset windows-vcpkg-tests-debug
ctest --preset windows-vcpkg-tests-debug
```

Some tests are skipped on Windows because they specifically check POSIX permission bits, symlink
failure behavior, or Unix-style build-directory discovery.

### Visual Studio version

Install classic Visual Studio and choose "Desktop Development with C++" and
accept all the options it preselects (the desktop/CMake tools).

Check out the repo with Git. Visual Studio will automatically configure it with cmake.

The first configure will take a while because vcpkg builds dependencies.

It doesn't really say a lot while it does this, but open the Windows Task Manager and you will see it is busy.

You can also look at the installed dependency tree at `../.vcpkg-holder-daemon` and see it filling up with the
best of the last forty years of open source. It will take about 2GB. It is like npm.

### Building the application

Under the configurations drop-down choose `windows-vcpkg-debug`.

Then under the "Build" menu choose "Build All".

This builds the local server `holderd.exe` and the command line interface `holderctl.exe`.

Run them using F5 or just run them in the command line.

### Running the test suite.

Under the configurations drop-down, choose. `windows-vcpkg-tests-debug`

Then under the "Build" menu choose "Build All".

Then under the "Test" menu, go to the submenu "Run Test Preset for windows-vcpkg-tests-debug"
then click on "windows-vcpkg-tests-debug"

## Useful Commands (Linux/BSD/Mac)

```bash
./make.sh --help          # list supported build/test commands
./make.sh                 # configure + build + tests + run holder
./make.sh Debug           # debug build
./make.sh perf-privacy    # run encrypted-card perf profile table
./make.sh perf-privacy Debug
./make.sh coverage        # build + run tests + generate HTML coverage report
./make.sh warnings        # build holderd + holderctl with warnings as errors
./make.sh memcheck        # Valgrind memcheck; slow, excludes timing-sensitive tests
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
