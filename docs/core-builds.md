# Core builds

## Default build

`./make.sh` downloads the latest core SDK that passed CI and keeps using that
version. `./make.sh core-update` updates it. The version is saved in
`out/core-selection.json`; downloads are cached in `.core-sdk`.

On Fedora, it builds `../holder-core` instead. The Linux SDK is built for
Ubuntu 24.04 and uses different library versions from Fedora.

```sh
./make.sh build Release                 # Use the Release SDK
HOLDER_CORE_REF=v0.2.0 ./make.sh build   # Select a tag or full commit SHA
./make.sh core-update                   # Update to latest-green
```

## Choosing core yourself

Set one of these before running `./make.sh`:

| Variable | Core dependency |
| --- | --- |
| `HOLDER_CORE_SDK` | An SDK directory |
| `HOLDER_USE_SYSTEM_CORE=ON` | An installed core package |
| `HOLDER_CORE_SOURCE_DIR` | A core source checkout |

These settings override the default. For example:

```sh
HOLDER_CORE_SOURCE_DIR="$PWD/../holder-core" ./make.sh test Debug
```

Direct CMake builds use `-DHOLDER_CORE_SOURCE_DIR=<source-path>`.
The Windows Debug presets use `../holder-core`.

Published SDKs support `RelWithDebInfo` and `Release`. Diagnostic commands use
`RelWithDebInfo` with an SDK and `Debug` with source builds. Other build types
need a matching SDK, system package or source checkout.

On Linux distributions other than Ubuntu 24.04 and Fedora, choose a core SDK
built for your system, an installed core package or a source checkout.

## Windows

`./make.sh` uses the `windows-sdk-tests` preset and needs `VCPKG_ROOT` set.
When running that preset directly, also set `HOLDER_CORE_SDK`.

The SDK includes core's dependencies. `packaging/windows/sdk-deps` installs
additional Boost dependencies for daemon. Runtime DLLs are copied beside the
executables for tests and distribution.

## CI and releases

CI selects core once and uses the same revision across all platform jobs.
The default is `latest-green`; set `core_ref` to a tag or full commit SHA to pin it.
Core's SDK configuration must match daemon's build type.

For a release, run `ci.yml` manually with `core_ref` pinned and
`core_build_type=Release`. All three platforms build, test and check the release
artifacts. Release artifact names end in `-release`; ordinary CI builds use
`RelWithDebInfo`.

Artifacts include core's schema and welcome resource. `core-build.json` records
which core SDK was used. Each artifact's `release/` directory records daemon's
commit, version and API version. Record the CI run and component commits in the
framework release manifest.

Ubuntu source packages use `libholder-dev` and its shared `libholder0` runtime
through `HOLDER_USE_SYSTEM_CORE=ON`. CI checks these packages separately.
