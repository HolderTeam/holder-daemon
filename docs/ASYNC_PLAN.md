# TODO Plan: Request Burst Containment and Backend Concurrency Refactor

  ## Summary

  Create a checklist-driven plan for holder-daemon/docs/TODO_backend_concurrency.md that fixes the current failure mode in three phases:

  1. Stop unnecessary frontend request bursts, especially Connections refresh churn.
  2. Remove the daemon’s single-request bottleneck with a safe concurrent interim design.
  3. Move the daemon toward a genuinely async architecture with isolated executors for blocking work.

  The immediate incident to address is the repeated links/backlinks request storm from the Linux frontend combined with a single-lane daemon that runs one HTTP session at a time. The plan
  assumes we keep HTTP for now; transport changes like Unix sockets or WebSockets are out of scope for the first implementation.

  ## Key Changes

  ### Phase 1: Frontend burst containment

  - Gate Connections graph refreshes so they only run when the Connections tool is visible.
  - Add debounce to Connections graph refresh scheduling; target one refresh after edits settle rather than one per buffer event.
  - Enforce at most one in-flight graph refresh per selected card/project view.
  - Drop stale refresh results aggressively when selection or graph-refresh serial changes.
  - Avoid duplicate refresh triggers from both selection updates and list rebuilds when the effective target has not changed.
  - Keep autosave behavior unchanged except for preventing unrelated Connections refresh churn from piggybacking on editor typing.
  - Add debug logging counters for suppressed/stale/skipped Connections refreshes so regressions are visible.

  ### Phase 2: Safe backend concurrency

  - Replace the current listener behavior where accepted sockets are handled inline on the accept loop.
  - Introduce a small bounded worker pool for session execution so one slow request no longer blocks all others.
  - Do not share the current single SQLite handle across worker threads.
  - Add a DB access redesign before enabling multi-worker request handling:
      - either one DB connection per worker thread, or
      - one DB connection per request/session, or
      - a dedicated serialized DB executor if a broader refactor is preferred.
  - Make the concurrency phase explicitly “blocking work on pooled workers”, not “fully async”.
  - Keep the listener thread lightweight: accept, hand off, continue accepting.
  - Add limits/queues for expensive route classes if needed during this phase, especially nudges, AI, git, and card write paths.

  ### Phase 3: Real async/server architecture

  - Move network accept/read/write to a truly asynchronous model rather than synchronous Session::run().
  - Separate request I/O from blocking work execution.
  - Introduce dedicated executors by subsystem:
      - DB executor
      - git/filesystem executor
      - AI/nudge executor
  - Define route classes by latency/cost and ensure cheap CRUD/status routes are not starved by expensive background-style work.
  - Add backpressure and cancellation semantics for superseded UI-driven requests like graph refreshes and nudge polling.
  - Preserve existing HTTP route shapes unless a later product decision explicitly changes client transport.

  ## Public Interfaces and Architecture Decisions

  - No wire-format changes are required in phase 1 or phase 2.
  - Frontend behavior change:
      - Connections refresh becomes visibility-aware, debounced, and single-flight.
  - Backend architecture change:
      - session handling becomes concurrent after DB-safety work is in place.
  - DB decision for implementation:
      - the implementer must not reuse the current single shared holder::platform::Db instance across multiple request workers.
      - recommended default: one SQLite connection per worker thread, opened with the same pragma setup as today.
  - Non-goal for this TODO:
      - switching to Unix sockets, WebSockets, or other persistent transports in the first implementation tranche.

  ## Test Plan

  - Frontend tests:
      - Connections refresh does not fire while the Connections tool is hidden.
      - Editor typing produces one debounced graph refresh, not one per buffer change.
      - Selection churn or card-store churn does not duplicate refreshes for the same effective target.
      - Only one graph refresh remains in flight at a time; stale results are ignored.
  - Backend tests:
      - Listener continues accepting new requests while one request is slow.
      - Concurrent requests to cheap routes still complete while a slow route is running.
      - DB access remains correct under concurrent request load with the chosen DB-connection strategy.
      - No regressions in existing card, nudge, and AI route behavior.
  - Integration/load scenarios:
      - Simulate repeated Connections refresh triggers while editing one card and verify request volume stays bounded.
      - Verify autosave still succeeds under moderate concurrent read load.
      - Reproduce the prior failure pattern and confirm nudges/CRUD no longer time out behind a single stuck request.

  ## TODO Checklist Structure

  - Section 1: Incident summary and observed request pattern.
  - Section 2: Phase 1 frontend containment tasks with checkbox items.
  - Section 3: Phase 2 backend concurrency tasks with checkbox items.
  - Section 4: Phase 3 async architecture tasks with checkbox items.
  - Section 5: Validation checklist and success criteria.

  ## Success Criteria

  - Editing a card no longer generates repeated links/backlinks bursts for the same card.
  - The daemon can continue serving unrelated requests while one request is slow.
  - A slow nudge, AI, or git path no longer freezes card CRUD and health/status routes.
  - The implementation path remains incremental: each phase can be shipped and verified independently.

  ## Assumptions and Defaults

  - TODO file path: holder-daemon/docs/TODO_backend_concurrency.md.
  - Scope: both Linux frontend and daemon.
  - Plan style: phased roadmap, not a single large rewrite.
  - Transport remains HTTP for now.
  - The first backend concurrency step is a bounded worker-pool design, followed by a later async/executor refactor.
