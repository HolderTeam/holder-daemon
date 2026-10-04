# Coding-agent instructions

Treat the current code, tests, OpenAPI contract, and active task plan as authoritative. Other planning or design documents may be stale unless the current task explicitly references them.

Holderd owns database access, authentication, HTTP validation, concurrency, background scheduling, and API presentation. Reusable non-platform-specific domain behavior belongs in `holder-core`.

Clients must not duplicate database, Git, Markdown-mutation, or other domain policy implemented in core.

Shared `boost::asio::io_context` instances are owned by dedicated I/O threads. Request workers must not call `run()`, `run_one()`, `poll()`, or similar methods on a shared I/O context while waiting for request socket work.

Preserve request-data lifetimes and worker-owned database access. Treat intermittent memory corruption, iterator invalidation, and concurrency failures as real defects, not CI noise.

When changing an HTTP route, update and test `openapi.yaml` in the same change where applicable.

# Core dependency selection

Make holder-core changes in the standalone workspace repository at `../holder-core`
and test them there first. Do not edit embedded dependency copies inside holder-daemon.

Normal development and GitHub CI consume the published core SDK via
`holder-core/scripts/core-sdk.py` and `HOLDER_CORE_SDK`. Resolve core once, use the
same exact revision across platform jobs, and match the SDK's build configuration
to the daemon's. Use an explicit version tag or full SHA when a task requires a
particular core change, then test daemon against that SDK. Ubuntu source packages
use the packaged `libholder-dev` and `libholder0` through `HOLDER_USE_SYSTEM_CORE`.

For explicit local source development, use `HOLDER_CORE_SOURCE_DIR` pointing at
the standalone `../holder-core` checkout. Source builds are opt-in; do not silently
fall back to compiling core when an SDK is missing. Core owns its tests and
sanitizer coverage; daemon owns its HTTP and integration tests.

Legacy embedded core checkouts and their Git pointers are not the normal
dependency-selection mechanism. Do not advance or reset them as a side effect
of switching branches or updating the SDK.
