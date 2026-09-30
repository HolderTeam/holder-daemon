# Windows Git over SSH

The Windows daemon manifest must request `libgit2` with `features: ["ssh"]`.
Its top-level vcpkg manifest owns dependency installation; the Core submodule's
manifest does not add features to it. Plain `libgit2` installed the Windows
SSL/WinHTTP defaults without libssh2, despite Core requesting SSH in its own
manifest. Advancing the Core submodule alone could not enable SSH transport.

The fix explicitly enables the daemon's libgit2 SSH feature and consumes Core
commit `46d242e9f13dd593dd14541bcb662150dbcfe84c`. That Core change addresses two
related authentication defects:

- Key discovery falls back from empty/unset `HOME` to `USERPROFILE` on Windows.
- Creating an agent credential does not authenticate it. On libgit2's later
  authentication callbacks, the provider now advances from agent to existing
  `id_ed25519` and `id_rsa` files, then stops. Previously it repeatedly returned
  agent credentials, making file fallback unreachable. Each remote operation
  resets this sequence. SSH-memory-only requests are declined because these
  credentials require the SSH-key type.

## Verification on 30 September 2026

The original manifest was configured with empty build and installed-dependency
directories (`out/build/ssh-baseline` and `out/ssh-baseline-deps`). Both `holderd`
and `holderctl` built successfully. The installed libgit2 1.9.4 had no SSH
feature in vcpkg's status database. Calling `git_libgit2_features()` on its DLL
returned **1979**, with `GIT_FEATURE_SSH` (bit 4) clear. The previously installed
daemon DLL gave the same result.

The fixed manifest was configured into separate empty directories:

```powershell
cmake --preset windows-vcpkg-tests-debug -B out/build/ssh-verified `
  -DVCPKG_INSTALLED_DIR="$PWD/out/ssh-verified-deps"
cmake --build out/build/ssh-verified --target holderd holderctl holder_core_tests holder_daemon_tests -j 6
ctest --test-dir out/build/ssh-verified --output-on-failure -j 6
```

Run these commands in an MSVC x64 developer shell with `VCPKG_ROOT` set. The
fresh fixed installation recorded `libgit2[ssh]` and its libssh2 dependency.
Its DLL reported **1983**, with `GIT_FEATURE_SSH` set. Dependency binary caches
were allowed; neither verification directory reused an installed dependency
tree or existing CMake build. The normal Windows presets use the same manifest.

Core's standalone suite passed all 781 test entries (three existing Windows
skips). The new credential regression tests cover agent/file retry order,
exhaustion, operation reset, missing-key skipping, HOME precedence, USERPROFILE
fallback, and allowed credential types.

The clean daemon build produced `holderd`, `holderctl`, and both test binaries
successfully. Its combined Windows suite passed all **1,404 test entries**, with
zero failures and nine skips (Windows-specific cases and external cloud-provider
integration cases), in 81.25 seconds. The new `Windows daemon libgit2 includes
SSH transport` runtime assertion passed. The daemon and daemon-test Debug DLLs
have the same SHA-256 hash as the freshly installed Debug libgit2 DLL.

The opt-in Core test `Git over SSH can push probe and fetch` uses a disposable
remote supplied by `HOLDER_TEST_SSH_REMOTE_URL`. The Windows fixture
`tools/windows_ssh_smoke.py` supplies a loopback Paramiko SSH server, a bare Git
repository, trusted host key, and isolated Windows home. It runs real Core
push, probe, and fetch and checks the fetched file. `HOME` is unset throughout.
The tested authentication paths are RSA file fallback, Ed25519 file fallback,
and a temporary Windows named-pipe agent speaking the OpenSSH agent protocol.
Agent mode removes the private key files and checks that signing occurred.
The user's SSH service, agent keys, and known-hosts file are untouched.
All three smoke modes passed against both the standalone Core build and the
Core test binary linked by the clean daemon build, with four assertions per
mode; agent mode performed three RSA-SHA2-512 signatures.

Local logs are under `out/ssh-baseline-*.log`, `out/ssh-verified-*.log`, and
`out/ssh-smoke-daemon-*.log`. Verified daemon executables are in
`out/build/ssh-verified`. Core's fix is on `fix/windows-ssh-credentials`;
the daemon changes are on `fix/windows-git-ssh`.

To repeat the smoke tests with Python 3.12+ and Paramiko installed:

```powershell
python tools/windows_ssh_smoke.py out/build/ssh-verified/holder-core/tests/holder_core_tests.exe file
python tools/windows_ssh_smoke.py out/build/ssh-verified/holder-core/tests/holder_core_tests.exe ed25519
python tools/windows_ssh_smoke.py out/build/ssh-verified/holder-core/tests/holder_core_tests.exe agent
```

An optional third argument supplies `git.exe` when it is not on PATH. The
fixture's repository is disposable; the opt-in C++ test pushes to its supplied
remote, so never point it at a production repository.

## Existing limitations

The shipped libssh2 supports the Windows OpenSSH agent named pipe
`\\.\pipe\openssh-ssh-agent`, or the pipe named by `SSH_AUTH_SOCK`. The actual
Windows service was stopped on this machine; the smoke test verified the
named-pipe protocol with an isolated agent, not that service or GitHub access.
Load passphrase-protected identities into a running agent: file fallback uses
an empty passphrase and has no interactive prompt. File fallback checks only
the two conventional key names and expects their accompanying `.pub` files;
SSH config `IdentityFile` aliases and arbitrary key paths are not implemented.
Trusted host keys must already exist in libgit2's home `.ssh/known_hosts`.
On Windows libgit2 can prefer `HOMEDRIVE`/`HOMEPATH` for that home; the fixture
sets these consistently with `USERPROFILE`. Host-key validation remains enabled.
