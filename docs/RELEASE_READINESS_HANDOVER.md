# Release-readiness handover — 2026-09-22

## Scope and current checkpoint

The goal is 100% daemon line coverage plus the diagnostics exposed by `make.sh`.
The audit below records the final local evidence for the current release-quality
checkpoint. Live cloud-provider checks remain credential-dependent.

Repository: `holder-daemon`. Current checkpoint commit: `a0c1979` (More testing),
plus the Valgrind suppression documented below.
The prior `8a8ce99` CI build-target fix had a green pipeline. Changes described in
the checkpoint section below are committed; subsequent audit work is separate.
The holder-core submodule remains `b7f0880b0e921f3e39f776528a6a374d3d30629c`.
No sibling repository was changed in this pass. Follow repository `AGENTS.md`;
core fixes belong in the canonical `../holder-core` checkout first.

## Changes committed at the checkpoint

- **Production shutdown fix:** request workers previously let database-open or
  runner-registry initialization exceptions escape `std::thread`, aborting the
  process. A runtime database-loss test exposed this under TSan. `Listener::run`
  now captures the first database-worker exception, stops and joins its workers,
  then rethrows on the caller thread. This preserves daemon cleanup and a nonzero
  exit. A deterministic regression drops `ai_runners` before worker startup; the
  CLI regression also removes/replaces a running daemon's database.
- **Test synchronization fix:** `DiagnosticGitOps` in `HolderCtlSync_test.cpp`
  now protects its call log with a mutex. TSan reported the test thread clearing
  the log while the Git worker accessed it. Snapshot/clear methods replace direct
  access.
- Added tests for storage error-to-HTTP mappings, resource validation and CRUD
  failures, durable location updates, history restore validation, database
  ownership/SQL errors, daemon health monitoring, CLI rendering and invalid
  arguments, timezone edge cases, S3 validation and virtual-host addressing.
- Extracted the existing resource error mapper as `resource_error_response` for
  direct tests of all storage error categories; HTTP behavior is unchanged.
- README documents the existing opt-in holder-core glibc TSan suppression.
  Ignored `build-tsan/` and corrected one pre-existing formatting violation.

## Verified results

### Current final audit

| Check | Result and evidence |
| --- | --- |
| Canonical coverage | **100.0% lines (15,998 / 15,998) and 100.0% functions (992 / 992)**; the coverage suite reported 1,410 registered tests with no failures. `/tmp/holder-daemon-release-coverage-final100.log` |
| Address/Undefined/Leak sanitizers | **1,412 tests: 1,409 passed, 3 skipped; no findings.** Skips are the non-token privacy injection and live Google/S3 tests. `/tmp/holder-daemon-release-asan-final.log` |
| ThreadSanitizer | **3 / 3 CTest entries passed** in 152.84 seconds. The holder-core glibc suppression was supplied explicitly; only GCC/Boost `atomic_thread_fence` compile warnings remain. `/tmp/holder-daemon-release-tsan-final.log` |
| Valgrind MemCheck | The complete 1,408-test run had one glibc TLS report in the forked child of the LocalModelRunner probe; the parent had zero errors. The focused rerun is clean after the exact child-stack suppression in `tools/valgrind/holder.supp`. The subsequent parallel full rerun was stopped by the sandbox process limit after 181 cases, before a summary; no additional defect was reported. `/tmp/holder-daemon-release-memcheck-final.log`, `/tmp/holder-daemon-localrunner-memcheck2.log` |
| Warnings and formatting | `./make.sh warnings`, `./make.sh format-check`, and `git diff --check` pass. `/tmp/holder-daemon-release-warnings.log` |
| Privacy performance profile | Corrected `make.sh perf-privacy` runs the holder-core test and passes: 10/100/1024/5120 KiB rows completed. `/tmp/holder-daemon-release-perf-privacy-escalated.log` |

The Valgrind suppression is limited to the glibc `allocate_dtv` allocation stack
created by `LocalModelRunner::start_background_probe` while its intentionally
missing executable is launched in a child process. It does not suppress definite
leaks or unrelated runner allocations.

The repository is ready for review based on these local checks; do not claim
cloud-provider integration coverage without supplying the required credentials.

### Historical checkpoint results

| Check | Result and scope |
| --- | --- |
| Full coverage-build test suite after the shutdown fix | **1,401 registered; 1,399 passed, 2 live-cloud tests skipped; zero failures.** `/tmp/holder-daemon-wrapup-tests.log` |
| Latest complete canonical coverage report | **97.8% lines: 15,853 / 16,211; 99.8% functions: 989 / 991.** This report predates the checkpoint additions; regenerate before quoting a new percentage. |
| Formatting | `./make.sh format-check` passes after correcting `EnvGuard` formatting. `/tmp/holder-daemon-wrapup-format.log` |
| Whitespace | `git diff --check` passes. |
| Earlier full ASan + UBSan + leak detection | 1,389 registered; 1,386 passed, 3 skipped; no sanitizer findings. This predates the current changes. `/tmp/holder-daemon-audit-asan-ubsan.log` |
| Warnings-as-errors build after the shutdown fix | Passed for `holderd` and `holderctl`. `/tmp/holder-daemon-wrapup-warnings.log` |
| clang-tidy with current Fedora Clang | Completed without compiler errors, but emitted warnings requiring triage. `/tmp/holder-daemon-audit-tidy.log` |
| Valgrind | **Incomplete:** interrupted after 569 / 1,385 test completions. No Valgrind process remained when checked. `/tmp/holder-daemon-release-memcheck.log` |

The full ThreadSanitizer run after both fixes **passed all three CTest entries**
(allocation test, core suite, daemon suite) in 142 seconds with the explicit glibc
suppression. Log: `/tmp/holder-daemon-wrapup-tsan.log`. The test-helper formatting
change was whitespace-only and did not require repeating this run.

### Historical runs from the earlier checkpoint

- Full ASan/UBSan/LSan tests on `06507d7`: `/tmp/holder-daemon-wrapup-asan.log`.
- Fresh full Valgrind build/run: `/tmp/holder-daemon-release-memcheck.log`.
- Fresh canonical coverage: `/tmp/holder-daemon-release-coverage.log`.

Live Google Drive and S3 tests need credentials; the extensive local TLS/HTTP
protocol tests run without them. ASan also skips core's non-token privacy-error
injection test.

## Memory/concurrency notes

1. The complete Valgrind run was inspected through its defect summary. The one
   report came from `MemoryChecker.1130.log`, test **LocalModelRunner non-fake
   background probe runs once and sets status**. Its forked child reports **416
   bytes possibly lost** from inherited glibc TLS (`allocate_dtv` /
   `pthread_create`); the parent reports zero errors. The focused rerun is clean
   with the exact child-stack suppression described above.
2. That directory also contains **stale September 19 reports** with nonzero errors
   (IDs 109, 114, 133, 214). Do not confuse those stale reports with the current
   run; match timestamps and the test command in each log.
3. Unsuppressed TSan reported glibc `tzset_internal` during concurrent libgit2
   signature creation. The existing core suppression documents glibc's internal
   lock, invisible to TSan in the system library. Use it explicitly only for this
   known report; no Holder race suppression was added.
4. The listener fix covers database-owning save/general workers. It is not a
   general redesign of every thread's exception or thread-creation-failure handling.

## Coverage audit history

The earlier checkpoint required regenerating `build-coverage/coverage/coverage.json` and the HTML report.
The last complete report still had **358 uncovered lines**. The JSON has duplicate
line entries for some functions, so sum counts by file/line before listing gaps.
`/tmp/holder-daemon-current-gaps.json` is a local, pre-additions working inventory;
line numbers are now stale.

Largest files in that inventory (counts differ slightly from LCOV's line model):

| File | Uncovered lines before the new tests |
| --- | ---: |
| `src/api/routes/AiResourceRoutes.cpp` | 52 |
| `src/storage/S3CompatibleProvider.cpp` | 28 |
| `src/app/DaemonApp.cpp` | 28 |
| `src/cli/commands/milestones.cpp` | 22 |
| `src/api/routes/ProjectRoutes.cpp` | 21 |
| `src/api/routes/HistoryRoutes.cpp` | 20 |
| `src/cli/commands/history.cpp` | 15 |

High-value remaining scenarios include asset-cache expiry and interrupted streaming,
background import failures, startup metadata backfills, project sync failure paths,
OAuth state expiry, CLI download/output failures, and durable-file write failures.
Some residual entries are compiler exception-cleanup lines, invariant checks, or
OpenSSL allocation/failure guards. Review each and use narrow, explained exclusions
only where justified. Do not exclude entire error handlers to reach 100%.

`LocationRouteFixture`, `StorageHttpTestServer`, and `GoogleStorageTestServer` provide
local route/network fixtures. Stop/join storage test servers before inspecting their
requests or mutable handler state. The public TLS test certificate is test-only.

## Diagnostic history and limitations

- The earlier checkpoint requested clang-tidy triage. Many warnings
  concern established style, Catch2 expansions, or checked optionals; do not assume
  all are harmless. Clang 18 cannot parse this machine's GCC 16 headers; use current
  Fedora `clang-tools-extra` for analysis, Clang 18 for formatting.
- `make.sh perf-privacy` now invokes the holder-core test target and was verified
  above. A sandbox-only Catch2 discovery failure was reproduced and then cleared
  by running it with access to `/run/user/1000`.
- CI has not been pushed or rechecked from this workspace.

## Resume commands

Run from the daemon repository. These can take substantial time. Keep sanitizer
build directories separate and do not edit `make.sh` while a shell is executing it.

```sh
CTEST_PARALLEL_LEVEL=8 ./make.sh coverage
HOLDER_SAN_DETECT_LEAKS=1 HOLDER_CTEST_TIMEOUT=300 ./make.sh san address,undefined
HOLDER_SAN_BUILD_DIR=build-tsan \
  HOLDER_TSAN_SUPPRESSIONS="$PWD/submodules/holder-core/tools/tsan/glibc.supp" \
  HOLDER_CTEST_TIMEOUT=900 ./make.sh san thread
CTEST_PARALLEL_LEVEL=8 HOLDER_CTEST_TIMEOUT=900 ./make.sh memcheck
./make.sh warnings
HOLDER_CLANG_TIDY=clang-tidy HOLDER_RUN_CLANG_TIDY=run-clang-tidy ./make.sh tidy
./make.sh format-check
git diff --check
```

Loopback tests and Catch2 discovery may need execution outside the workspace sandbox
(Catch2 writes its listing under `/run/user/1000`). A permission failure is not a
code regression. Temporary logs under `/tmp` and ignored build outputs are local
artifacts, not committed evidence; preserve anything needed before cleaning them.
