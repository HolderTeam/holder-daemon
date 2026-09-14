# Coding-agent instructions

Treat the current code, tests, OpenAPI contract, and active task plan as authoritative. Other planning or design documents may be stale unless the current task explicitly references them.

Holderd owns database access, authentication, HTTP validation, concurrency, background scheduling, and API presentation. Reusable non-platform-specific domain behavior belongs in `holder-core`.

Clients must not duplicate database, Git, Markdown-mutation, or other domain policy implemented in core.

Shared `boost::asio::io_context` instances are owned by dedicated I/O threads. Request workers must not call `run()`, `run_one()`, `poll()`, or similar methods on a shared I/O context while waiting for request socket work.

Preserve request-data lifetimes and worker-owned database access. Treat intermittent memory corruption, iterator invalidation, and concurrency failures as real defects, not CI noise.

When changing an HTTP route, update and test `openapi.yaml` in the same change where applicable.

# Submodules

Do not modify holder-core through the submodule checkout inside holder-daemon.
Make holder-core changes only in the canonical workspace repository at ../holder-core,
commit and test them there, then update the consuming repository’s submodule pointer to that commit.

If a task requires changes to both holder-core and a consumer:

In the standalone holder-core repository, make a new branch off main and change, test, and commit holder-core 
Return to the consumer repository.
Advance its holder-core submodule to the committed revision.
Test the consumer against that revision.

Never treat a consumer’s submodule checkout as the working copy of holder-core.
