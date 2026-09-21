# Release-readiness handover — 2026-09-21

## Scope and current checkpoint

The goal is 100% daemon line coverage plus the diagnostics exposed by `make.sh`.
**This goal is not complete; do not describe the release as signed off.** The user committed the checkpoint and asked to continue, updating this document
regularly. The audit is active.

Repository: `holder-daemon`. Current checkpoint commit: `06507d7` (More fixes).
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

### Active runs after resuming

- Full ASan/UBSan/LSan tests on `06507d7`: `/tmp/holder-daemon-wrapup-asan.log`.
- Fresh full Valgrind build/run: `/tmp/holder-daemon-release-memcheck.log`.
- Fresh canonical coverage: `/tmp/holder-daemon-release-coverage.log`.

Results will replace these pending entries when each run finishes.

Live Google Drive and S3 tests need credentials; the extensive local TLS/HTTP
protocol tests run without them. ASan also skips core's non-token privacy-error
injection test.

## Memory/concurrency follow-up

1. Finish a fresh full Valgrind run against the final sources. CTest's individual
   `Passed` lines alone are not a clean memory audit: inspect the defect summary
   and `build-memcheck/Testing/Temporary/MemoryChecker.*.log` too.
2. One report from the interrupted run needs triage:
   `MemoryChecker.1120.log`, test **LocalModelRunner non-fake background probe runs
   once and sets status**. A forked child reports **416 bytes possibly lost** from
   glibc TLS allocation (`allocate_dtv` / `pthread_create`); the parent reports
   zero errors. Determine whether this is inherited thread TLS at child `_exit`
   or an application problem before considering a narrowly justified suppression.
3. That directory also contains **stale September 19 reports** with nonzero errors
   (IDs 109, 114, 133, 214). Do not confuse those with the September 21 run; match
   timestamps and the test command in each log. Recheck the corresponding C API
   cases during the fresh full run.
4. Unsuppressed TSan reported glibc `tzset_internal` during concurrent libgit2
   signature creation. The existing core suppression documents glibc's internal
   lock, invisible to TSan in the system library. Use it explicitly only for this
   known report; no Holder race suppression was added.
5. The listener fix covers database-owning save/general workers. It is not a
   general redesign of every thread's exception or thread-creation-failure handling.

## Coverage work still to do

Regenerate `build-coverage/coverage/coverage.json` and the HTML report first.
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

## Other unfinished diagnostics

- Triage clang-tidy warnings, then rerun it on final sources. Many current warnings
  concern established style, Catch2 expansions, or checked optionals; do not assume
  all are harmless. Clang 18 cannot parse this machine's GCC 16 headers; use current
  Fedora `clang-tools-extra` for analysis, Clang 18 for formatting.
- `./make.sh perf-privacy` currently invokes the **daemon** test binary, but its
  named test (`CardStore encrypted project perf profile (manual)`) lives in
  **holder-core**. Correct the command to the core test target and verify it really
  executes the profile; a zero-match invocation is not a performance result.
- Regenerate final coverage, rerun full diagnostics after any further changes, and
  push/recheck CI. The earlier green pipeline does not include this working tree.

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
