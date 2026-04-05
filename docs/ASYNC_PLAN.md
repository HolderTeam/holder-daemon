# Async + Multi-Runner Plan

This is the canonical implementation plan for the next backend and Linux frontend tranche.

It merges the previous async/concurrency plan and the previous multi-runner plan into one sequence with one TODO list.

## Summary

Holder has two linked problems:

1. The current request path is effectively single-lane, so one slow route can block unrelated work.
2. The current AI runtime model assumes exactly one local runner, so adding more runners is a cross-stack refactor.

These are not separate roadmap tracks anymore. They should be implemented as one coordinated plan because:

- the transport and execution model must not hard-code a single `LocalModelRunner`
- multi-runner AI work must still obey handwritten-work priority and save-path protection
- frontend recovery behavior and request containment need to stay aligned with backend execution semantics

Top-level product rule:

- handwritten user work comes first
- card save capacity must be reserved
- card writes jump the queue over every other class of work
- background AI/runtime work must yield before save/load paths
- if the backend is unavailable, the frontend must still preserve unsaved handwritten work locally

For now, transport stays as HTTP. Unix sockets, WebSockets, and transport replacement are out of scope for the first implementation tranche.

## Current State

Observed issues:

- the Linux frontend can still generate repeated Connections refresh traffic
- the daemon listener accepts one socket and runs one session inline before accepting the next
- the daemon currently threads one `LocalModelRunner*` through the request stack
- `/ai/status`, `/ai/capabilities`, and `/ai/local-models/config` still expose a singular local-runner model
- AI pull/probe work uses ad hoc detached-thread behavior that will not scale cleanly to many runners

Consequence:

- one slow request can freeze unrelated requests
- background AI work can interfere with editor-adjacent save/load behavior
- the current runner model is too singular to grow safely into local plus remote runners

## Goals

- protect the handwritten-work path above all other features
- protect card saves as the single most important execution path
- preserve unsaved handwritten work even if the backend crashes or becomes unavailable
- stop hidden or redundant Connections refresh bursts from the Linux frontend
- remove the daemon's single-request bottleneck without a risky big-bang rewrite
- support more than one Ollama-compatible runner
- keep the local-machine runner simple and automatic
- let users configure remote runners manually
- route runner-qualified model selections through one consistent backend abstraction
- keep the implementation incremental, shippable, and testable phase by phase

## Non-Goals

- replacing HTTP as part of the first tranche
- a single giant concurrency rewrite
- automatic LAN discovery in the first runner tranche
- arbitrary provider-plugin work in the same tranche
- full TLS/auth management for remote runners in the first tranche
- a frontend redesign unrelated to these execution and runner changes

## Product Priority Model

Think of Holder as a three-lane road, not one shared queue.

- save lane: highest priority, reserved capacity, card writes jump ahead of all other queued work
- foreground lane: user-visible editing flow needed to keep working
- background lane: best-effort work that must yield first under pressure

### Save Lane

This is the fast lane for durable handwritten work.

- card writes
- autosave commits
- any write-confirmation path needed to clear dirty state safely

Rules:

- save work jumps the queue ahead of foreground and background work
- save capacity is reserved and cannot be consumed by any other lane
- save work should use short transactions and do blocking non-DB work outside the transaction where possible

### Foreground

This lane protects current-user editing flow and durable handwritten work.

- editor-adjacent card loads needed to keep typing
- project and card selection requests required to continue editing
- minimal status/health requests needed to keep the app usable

### Background

This lane is useful but must yield under pressure.

- Connections graph and related metadata refreshes
- links and backlinks refreshes that are not required for editing
- AI nudges
- AI assistant runs unless explicitly promoted later
- runner probes and model inventory refreshes
- runner pull/install jobs
- background AI status polling
- speculative refreshes and prefetches

### Design Rules

- save work must not sit behind foreground or background work
- foreground work must not sit behind background work
- card writes must have reserved capacity and must not be starved even by other foreground work
- more configured runners must not linearly consume all request-worker capacity
- background work should debounce, coalesce, cancel, queue, degrade, or drop before foreground work is affected
- it is better to miss a nudge or show stale runner/connections status than to risk losing handwritten text
- OS niceness is not the mechanism here; priority must be enforced in the daemon scheduler and queueing model

## Architecture Direction

### 1. Frontend Containment And Recovery

The Linux frontend should:

- suppress hidden and redundant Connections refreshes
- keep dirty state independent from background feature success or failure
- preserve failed-save state until save confirmation
- treat save confirmation as a successful `PATCH /cards/{card_id}` response only after the daemon has finished the write path and replied `200 OK`
- write local recovery drafts when backend saves fail or the backend becomes unavailable
- only clear recovery drafts and advance committed editor state after that confirmed durable save boundary
- keep GTK views focused on widget construction, rendering, and event wiring; move testable feature logic into controllers and other non-view files where practical

### 2. AI Runtime Generalization

The daemon should stop treating `LocalModelRunner` as both:

- one runner implementation
- and the entire runner system

Target shape:

- `RunnerClient`: one runner endpoint
- `RunnerRegistry`: many runners plus lookup and status aggregation
- `RunnerRepo`: persisted manual runner configuration
- runner-qualified model refs: `runner_id + model_name`

The current local runner should become a normal registry entry with `source = auto_local`.

Auto-local runner as a normal registry entry:

- the existing built-in `LocalModelRunner` should be exposed through the registry as the canonical runner record with `runner_id = auto-local`
- registry consumers should not need special-case code paths for “the old singleton runner” versus “configured runners”; they should ask the registry for runners and get `auto-local` back like any other entry
- the `auto-local` entry may be synthetic at first, but it must present the same logical shape as persisted manual runners: `runner_id`, `name`, `kind`, `base_url`, `source`, `enabled`, and runtime status/probe data
- recommended initial shape for `auto-local`:
- `runner_id = auto-local`
- `name = Local Ollama`
- `kind = ollama`
- `source = auto_local`
- `enabled = true`
- `base_url = http://127.0.0.1:11434` unless later made configurable
- the registry should own aggregation for `auto-local` status, models, pull jobs, and retry behavior, even if the underlying implementation still delegates to the existing `LocalModelRunner`
- runner-qualified refs for the current local models should therefore normalize to values like `auto-local::phi4-mini:latest`
- future manual `ollama` runners should coexist beside `auto-local`; the existence of one must not suppress or overwrite the other

Runner registry/manager abstraction:

- introduce a `RunnerRegistry` or `RunnerManager` as the daemon-owned entry point for runner discovery, lookup, status aggregation, model inventory, pull ownership, and retry routing
- route code should stop receiving a raw `LocalModelRunner*` and instead depend on the registry/manager abstraction
- the registry should compose:
- persisted config from `RunnerRepo`
- the synthetic or persisted `auto-local` runner record
- per-runner client instances such as an adapter around the current `LocalModelRunner`
- aggregate runtime state needed for `/ai/status` and `/ai/capabilities`

Minimum responsibilities:

- list all known runners
- get one runner by `runner_id`
- resolve a runner-qualified model ref to a concrete runner client plus model name
- aggregate per-runner status and model inventory for status/capability APIs
- route retry and pull operations to the addressed runner
- hide whether a runner is backed by the old singleton implementation, a manual config row, or a future different runtime kind

Required properties:

- one canonical lookup path by `runner_id`
- no special-case API-layer logic for `auto-local` versus manual runners
- bounded-executor-friendly ownership: registry operations should be safe to call from the planned request worker model without introducing detached-thread-only assumptions
- clear separation between persisted config, live runtime state, and per-runner transport/client logic

Recommended implementation split:

- `RunnerRepo`: storage for persisted manual runner rows
- `RunnerClient`: one runtime implementation instance per runner
- `RunnerRegistry`: assembles configured runners plus `auto-local`, exposes lookup/list/aggregate operations, and owns runner-specific routing decisions

Generalize current `LocalModelRunner` into per-runner client logic:

- the current `LocalModelRunner` should stop being treated as “the runner system” and instead become one `RunnerClient` implementation, initially for `kind = ollama`
- its current responsibilities split naturally into per-runner client behavior:
- probe runner health and enumerate models
- stream generation for one addressed runner
- own pull-job lifecycle for one addressed runner
- perform runner-specific retry/spawn behavior where applicable
- the first implementation can wrap the existing `LocalModelRunner` behind an adapter rather than rewriting all behavior at once
- route code and higher-level services should depend on a runner-client interface, not on concrete `LocalModelRunner`
- title generation, nudges, and normal local AI runs should all resolve a runner-qualified model ref first and then call the resolved `RunnerClient`

Executor compatibility requirements:

- registry and client APIs must be callable from the planned bounded request-worker model without assuming they can always spawn detached background threads
- long-running operations such as probes and pulls should be schedulable through explicit background execution lanes or injected executors
- synchronous read-only queries like “list cached runner state” or “lookup runner by id” should be cheap and non-blocking at the API layer
- if a client still uses legacy detached-thread internals during transition, that behavior should be contained behind the client boundary and treated as temporary compatibility debt
- new APIs should prefer one of these shapes:
- immediate snapshot calls for already-known state
- explicit “start work” calls whose execution is owned by the registry/background lane
- streaming or polling calls that read runner-owned state rather than starting ad hoc work

Runner-specific probe and pull ownership:

- probe state, model inventory, retry state, and pull jobs must be owned per runner rather than globally
- pull job identifiers may remain globally unique, but each job must also carry `runner_id` and be routed back to the owning runner client
- `/ai/status` and future `/ai/runners` APIs should report probe/pull state inside each runner record, not as one anonymous singleton runner blob
- retry routes should address a specific runner, for example `POST /ai/runners/{runner_id}/retry`, and must not implicitly retry every runner at once
- starting a pull for one runner must not collide with or overwrite pull state for another runner, even if the model names match
- the registry should therefore aggregate status as `runners[]`, with each runner exposing its own availability, last error, model list, and pull jobs

### 3. Safe Concurrency Before Full Async

Before deeper async work:

- stop running accepted sessions inline on the listener loop
- introduce bounded request execution
- make DB usage safe for concurrent request handling using one SQLite connection per worker thread
- keep save-lane protection explicit
- keep AI runtime operations behind bounded execution lanes rather than detached-thread sprawl

### 3.1 Dispatch Contract

The interim worker-pool design should follow an explicit dispatch contract rather than ad hoc prioritization.

Queues:

- `save_queue`: card writes and autosave only
- `foreground_queue`: editor-adjacent card loads, project/card selection, minimal health/status needed to keep the app usable
- `background_queue`: Connections refreshes, links/backlinks refreshes, nudges, AI runs, runner probes, runner pulls, AI status polling, speculative refreshes

Worker classes:

- `save_reserved_worker_count`: at least 1 worker slot reserved for `save_queue`
- `general_worker_count`: remaining worker slots that primarily serve `foreground_queue` and then `background_queue`

Recommended initial counts:

- `save_reserved_worker_count = 1`
- `general_worker_count = 3`

Rationale:

- one reserved save worker is enough to guarantee save-lane admission without overcommitting the process to write-heavy capacity that SQLite cannot use concurrently anyway
- three general workers are enough to break the current single-request bottleneck while keeping the first concurrency step small and debuggable
- this gives an initial total of four request workers, which is a pragmatic default for a local desktop daemon
- tune later from measurements, but do not reduce reserved save capacity to zero

Dispatch rules:

- `save_queue` is always checked first
- reserved save workers only consume `save_queue`
- general workers prefer `foreground_queue`, then `background_queue`
- background work must never run on reserved save workers
- if `save_queue` is non-empty, queued save work jumps ahead of queued foreground/background work
- foreground work must never be dispatched behind queued background work when a general worker becomes available

Admission and degradation rules:

- background work may be delayed, coalesced, canceled, or dropped under pressure
- foreground work may queue briefly but should not be dropped silently
- save work should not be dropped; it should either run, retry under explicit policy, or fail visibly while dirty state remains preserved
- duplicate background refreshes for the same effective target should be coalesced before entering the queue where possible

AI runtime rules:

- runner probes, model refreshes, pull jobs, nudges, and normal AI runs enter `background_queue` by default
- future exceptions that promote an AI route above background must be explicit and justified in docs and code
- adding more runners must not increase `save_reserved_worker_count` pressure or reduce save-lane guarantees

### 3.2 Initial Route To Lane Mapping

The first implementation should classify routes and work items like this.

`save_queue`

- `PATCH /cards/{card_id}`
- any future explicit autosave/write-confirmation path
- any future recovery-draft cleanup step that must run immediately after confirmed durable save

`foreground_queue`

- `GET /cards/{card_id}`
- `GET /cards?project_id=...`
- `GET /cards/context`
- `GET /projects`
- `GET /projects/{project_id}`
- minimal `GET /health` and minimal app-usable status checks
- any project/card selection path whose result is required to continue editing

`background_queue`

- `GET /cards/{card_id}/links`
- `GET /cards/{card_id}/backlinks`
- Connections graph refresh work
- `/ai/status` polling beyond minimal app-usable status
- `POST /ai/runners/{runner_id}/retry`
- `POST /ai/runner/pull`
- `GET /ai/runner/pull/{job_id}`
- `GET /ai/runner/pull/{job_id}/events`
- future `/ai/runners` probe/refresh work
- `POST /ai/runs`
- `GET /ai/runs`
- `GET /ai/runs/{run_id}`
- `GET /ai/runs/{run_id}/events`
- nudge generation and related AI background work

Classification notes:

- some routes may perform mixed work; classify them by the user-criticality of the request, not by the most expensive internal substep
- `GET /ai/status` may expose both minimal foreground-safe status and richer background runner/pull detail, but the richer polling behavior should still be treated as background
- if a route starts foreground and then spawns follow-up refreshes, those follow-up refreshes should enter `background_queue`
- git, indexing, and AI side work triggered from foreground flows should avoid holding up the foreground response unless they are strictly required for correctness

### 4. Deeper Async End State

The eventual shape is:

- async network I/O
- bounded executors for blocking subsystems
- explicit save/foreground/background isolation
- dedicated save-path capacity
- AI runtime operations scheduled through explicit executors, not ambient threads

## Data And API Direction

Runner configuration should become first-class persisted data.

Canonical `runner_id` contract:

- `runner_id` is the stable primary identifier for a runner record across storage, APIs, logs, and model selection
- it must be assigned once and then treated as immutable
- it must not be derived from mutable fields such as display name, base URL, model list, availability, or process state
- it must not encode secrets, machine-local absolute paths, or user-facing labels
- allowed format should be lowercase ASCII with digits and single hyphen separators, matching `^[a-z0-9]+(?:-[a-z0-9]+)*$`
- manual runners should normally receive an opaque generated id rather than a name-derived slug, so renaming a runner never changes references
- reserve `auto-local` as the canonical id of the built-in auto-managed local runner that currently wraps `LocalModelRunner`
- model refs, local model config, pull-job ownership, and future runner CRUD routes should all key off `runner_id`, not runner name
- if a runner is deleted and later recreated, it should get a new `runner_id`; old persisted model refs should then be treated as stale rather than silently rebound
- logs and debug output may include human-readable runner names, but `runner_id` remains the canonical join key

Suggested runner fields:

- `runner_id`
- `name`
- `kind`
- `base_url`
- `source`
- `enabled`
- `status`
- `created_at`
- `updated_at`

Persisted runner configuration schema:

- primary table: `ai_runners`
- one row per configured runner, keyed by `runner_id`
- intended for durable configuration, not transient probe output or pull-job state

Suggested columns for `ai_runners`:

- `runner_id TEXT PRIMARY KEY`
- `name TEXT NOT NULL`
- `kind TEXT NOT NULL`
- `base_url TEXT NULL`
- `source TEXT NOT NULL`
- `enabled INTEGER NOT NULL DEFAULT 1`
- `created_at INTEGER NOT NULL`
- `updated_at INTEGER NOT NULL`

Column semantics:

- `runner_id`: immutable canonical id described above
- `name`: user-facing mutable display name
- `kind`: runner implementation family such as `ollama`, later extensible to other local or remote runtimes
- `base_url`: nullable because the built-in `auto-local` runner may use its conventional local endpoint without a persisted override
- `source`: distinguishes `auto_local` from `manual`
- `enabled`: persisted operator intent, separate from runtime health
- `created_at` / `updated_at`: audit timestamps for config changes

Schema rules:

- do not persist transient runtime state such as availability, last error, model inventory, or pull progress in `ai_runners`
- do not persist secrets in `ai_runners`; any future auth material should live in the existing secret-store path or a dedicated credential table
- `source = auto_local` is represented by the reserved `runner_id = auto-local` row or an equivalent synthetic registry entry, but the rest of the system should treat it like any other runner record
- uniqueness should be enforced on `runner_id`; do not enforce uniqueness on `name` or `base_url`
- runtime aggregation layers may join persisted runner config with probe results to build `/ai/status` and `/ai/capabilities`, but storage and runtime views should remain conceptually separate

Relationship to local model config:

- `ai_local_model_config` can remain a separate table initially, but its `fast_model`, `strong_model`, and `deep_model` values should migrate from bare model names to runner-qualified refs
- if later needed, that table can be renamed or reshaped, but the immediate schema step is to make runner records first-class without coupling them to transient runtime state

Persisted manual runner records:

- a manual runner record is any non-`auto_local` runner entry created, edited, enabled, disabled, or deleted through persisted config
- manual runner records live in `ai_runners` with `source = manual`
- the first supported manual kind should be `ollama`, using a user-supplied `base_url`
- creating a manual runner generates a new opaque `runner_id`, stores the normalized config row, and leaves runtime probing to registry/background status work
- editing a manual runner may change mutable fields such as `name`, `base_url`, and `enabled`, but must not change `runner_id`
- deleting a manual runner removes its persisted config row only; it must not implicitly rewrite existing runner-qualified model refs to some other runner
- if local model config or other persisted references still point at a deleted manual runner, those refs should remain stale/invalid until the user reselects a valid runner-qualified model
- disabled manual runners remain persisted and visible in config/status responses, but they should not be chosen for new AI work unless explicitly re-enabled
- manual runner records should be validated at write time for shape and required fields, but reachability/health should be runtime concerns rather than persistence preconditions
- a failed probe or temporary outage must not delete or mutate the persisted manual runner row beyond runtime status reporting

Model selection must become runner-qualified.

Runner-qualified model reference format:

- canonical structured form: `{ runner_id, model_name }`
- canonical compact string form: `runner_id::model_name`
- parsing rule: split on the first `::`; the left side is `runner_id`, the right side is `model_name`
- because `runner_id` is restricted to lowercase ASCII letters, digits, and hyphens, `::` is reserved as a safe separator
- `model_name` is case-sensitive and must be preserved exactly as reported by the runner
- neither side may be empty; refs with empty `runner_id` or empty `model_name` are invalid
- plain model names without a runner qualifier are legacy-only input and should be normalized at migration boundaries, not treated as the long-term format
- persisted local model config should store runner-qualified refs, not bare model names
- APIs may expose both the structured form and the compact string during transition, but all internal comparisons should normalize to `{ runner_id, model_name }`
- UI display labels remain separate from refs; for example a dropdown label may show `Office Ollama: phi4-mini:latest` while the stored value is `office-ollama::phi4-mini:latest`

Current shape:

- `phi4-mini:latest`

Needed shape:

- internal: `runner_id + model_name`
- persisted: compact serialized reference or structured equivalent
- UI label: runner-qualified display such as `office:phi4-mini:latest`

API direction:

- expand `/ai/status` to report aggregate state plus `runners[]`
- expand `/ai/capabilities` to report `runners[]` plus runner-qualified model refs
- migrate `/ai/local-models/config` to runner-qualified model selections
- add explicit runner CRUD and retry routes
- preserve singular compatibility fields only as a transition aid, not as the long-term model

## DB Strategy

Chosen strategy for this plan:

- one SQLite connection per worker thread

Why this is the default choice:

- it removes the current unsafe shared-handle assumption
- it keeps connection ownership simple and predictable
- it works naturally with SQLite WAL mode for concurrent reads plus serialized writes
- it avoids the extra churn of opening a fresh connection for every request
- it avoids turning the DB into a new artificial single-lane bottleneck

Rules for this strategy:

- never share a SQLite connection across worker threads
- each request worker owns one long-lived connection to the same DB file
- keep `WAL`, `foreign_keys`, and `synchronous = NORMAL`
- add a reasonable `busy_timeout`
- keep transactions short
- keep non-DB blocking work outside write transactions
- writes still serialize at SQLite level, so app-level queue priority must ensure saves reach the write lock ahead of low-value work

Recommended implementation details:

- open and initialize each worker-owned connection at worker startup, not lazily per request
- use the same per-connection PRAGMA setup on every worker-owned handle
- keep prepared-statement lifetime confined to the owning worker thread
- if a request needs to hop executors later, it must not carry SQLite statements or connection-owned objects across threads
- save-lane requests should acquire the write transaction as late as possible and release it as early as possible

## Frontend design

BTW, on the frontend, please don't put code in views unless it is widget related,
we have a range of useful folders where new files can be made and existing files can be added to:

- controllers: feature and workflow logic that reacts to events, coordinates models/services/state, and decides what should happen next.
- models: plain data structures and lightweight value objects shared across the app.
- services: integrations and reusable operations such as API access, persistence, transport, parsing, and system-facing helpers.
- state: long-lived application state containers and state-tracking structures.
- utils: small generic helper functions that do not belong to a specific feature or integration layer.
- views: GTK/libadwaita widget construction, rendering, and UI event wiring.

## Single TODO List

### Phase 1: Frontend Burst Containment

- [✅] Refresh Connections data only when the Connections tool is visible
- [✅] Debounce Connections graph refresh scheduling
- [✅] Enforce single-flight graph refresh for the active selection
- [✅] Drop stale graph refresh results when project/card/generation changes
- [✅] Suppress duplicate refresh triggers for the same effective target
- [✅] Add debug visibility for skipped, suppressed, stale, and coalesced refreshes
- [✅] Keep dirty editor state independent from background request success/failure

### Phase 2: Recovery And Save Semantics

- [✅] Preserve unsaved state until save confirmation
- [✅] Add retry/failure handling that does not incorrectly clear dirty state
- [✅] Add frontend-owned local recovery drafts for failed/unavailable backend saves
- [✅] Remove recovery drafts only after confirmed durable save
- [✅] Define save confirmation behaviour shared between backend and frontend cleanup
- Recovery-draft state must layer on top of explicit frontend draft state; background request outcomes and transport errors must not clear, reset, or redefine editor dirtiness.
- Save confirmation contract:
- the daemon confirms a card save only by returning success from `PATCH /cards/{card_id}` after its synchronous write path has completed
- frontend draft cleanup, committed-baseline advancement, and `Saved` UI state must happen only after that success response
- transport errors, backend errors, retries, queued saves, and `Saving...` UI state do not count as save confirmation

### Phase 3: Runner Abstraction And Persistence

- [✅] Define canonical `runner_id`
- [✅] Define runner-qualified model reference format
- [✅] Define persisted runner configuration schema
- [✅] Introduce persisted manual runner records
- [✅] Represent the auto-local runner as a normal registry entry
- [✅] Introduce a runner registry/manager abstraction
- [✅] Generalize current `LocalModelRunner` behavior into per-runner client logic
- [✅] Keep registry/client APIs compatible with bounded executors rather than detached-thread-only behavior
- [✅] Make status probing and pull-job ownership runner-specific

### Phase 4: API And Frontend AI Panel Config

- [✅] Add `GET /ai/runners`
- [✅] Add `POST /ai/runners`
- [✅] Add `PATCH /ai/runners/{runner_id}`
- [✅] Add `DELETE /ai/runners/{runner_id}`
- [✅] Add `POST /ai/runners/{runner_id}/retry`
- [✅] Expand `/ai/status` to include `runners[]`
- [✅] Expand `/ai/capabilities` to include `runners[]` and runner-qualified model refs
- [✅] Update `/ai/local-models/config` to read and write runner-qualified selections
- [ ] Remove remaining no-longer-needed compatibility fields instead of extending them
- [✅] Update the Linux frontend AI panel to show multiple runners
- [✅] Add runner list/add flow in the Linux frontend
- [✅] Update dropdowns and parsers to use runner-qualified model labels/refs

### Phase 5: Priority-Aware AI Routing

- [✅] Resolve selected local model config through `runner_id + model_name`
- [✅] Update normal local AI run paths to use the chosen runner
- [✅] Update title generation paths
- [✅] Update nudge paths that rely on local models
- [✅] Make pull jobs runner-aware in status APIs and UI
- [✅] Keep probes, pulls, nudges, and AI assistant work on background capacity by default
- [✅] Ensure multiple configured runners do not consume reserved save capacity


### Phase 6: Safe Backend Concurrency

- [✅] Stop handling accepted sockets inline on the listener loop
- [✅] Introduce a small bounded worker pool for request/session execution
- [✅] Keep the listener thread lightweight: accept, dispatch, continue accepting
- [✅] Make DB usage safe before enabling concurrent request workers
- [✅] Implement one SQLite connection per worker thread
- [✅] Add `busy_timeout` and any per-connection setup needed on worker-owned handles
- [✅] Introduce explicit save, foreground, and background execution lanes
- [✅] Define concrete queue names and admission rules in code comments/docs to match this plan
- [✅] Start with `save_reserved_worker_count = 1` and `general_worker_count = 3`
- [✅] Add route-to-lane mapping in code comments/docs to match this plan
- [✅] Route card writes through a dedicated highest-priority save queue
- [✅] Reserve execution capacity specifically for card save operations
- [✅] Ensure save work jumps queued foreground/background work at dispatch time
- [✅] Ensure foreground save/load paths are not queued behind nudge, Connections, probe, pull, or other background work


### Phase 7: Deeper Async Architecture

- [ ] Move connection accept/read/write handling to an actually asynchronous model
- [ ] Separate request I/O from blocking subsystem work
- [ ] Introduce dedicated executors for major blocking subsystems
- [ ] Add backpressure so expensive work cannot starve cheap routes
- [ ] Add cancellation or supersession for stale UI-driven background work where appropriate
- [ ] Keep route semantics stable unless a deliberate API change is approved
- [ ] Preserve the three-lane scheduler model in the deeper async architecture
- [ ] Keep explicit foreground/save protection after the deeper refactor

### Phase 8: Cleanup

- [✅] Remove obsolete singular-runner assumptions from daemon code
- [✅] Remove obsolete singular-runner assumptions from Linux frontend parsers and UI
- [✅] Remove any remaining temporary compatibility response fields after frontend migration is complete
- [ ] Tighten logs and debug output to include runner identity and priority lane where useful

## Test Plan

### Frontend

- [✅] Verify Connections refresh does not fire while the Connections tool is hidden
- [✅] Verify typing produces one debounced refresh rather than one per buffer event
- [✅] Verify only one graph refresh is in flight at a time
- [✅] Verify stale graph results are ignored
- [✅] Verify failed saves keep the card marked unsaved
- [✅] Verify recovery drafts are written, restored, and cleaned up safely
- [✅] Verify multi-runner UI shows runner-qualified model options clearly

### Backend

- [✅] Verify the listener continues accepting requests while one request is slow
- [✅] Verify cheap routes still complete while a slow route is running
- [✅] Verify one SQLite connection per worker thread behaves correctly under concurrent request load
- [✅] Verify card, nudge, and AI routes do not regress
- [✅] Verify autosave is not blocked behind background work under load
- [✅] Verify reserved save capacity still allows card writes when background capacity is saturated
- [✅] Verify queued card writes jump ahead of non-save queued work at dispatch time
- [✅] Verify multi-runner status aggregation and runner CRUD
- [✅] Verify migration from plain model names to runner-qualified refs
- [✅] Verify AI run routing goes to the selected runner
- [✅] Verify canonical runner retry goes through `/ai/runners/{runner_id}/retry`
- [✅] Verify multiple configured runners do not block card save paths under load

### Integration

- [ ] Reproduce repeated Connections refresh triggers while editing one card and confirm request volume stays bounded
- [ ] Reproduce the prior timeout pattern and confirm unrelated requests no longer get trapped behind one slow request
- [ ] Reproduce background AI/runtime load while typing and confirm handwritten work still saves reliably
- [ ] Simulate backend death during editing and confirm the frontend preserves recoverable handwritten work

## Implementation Order

Recommended order:

1. Phase 1
2. Phase 2
3. Phase 3
4. Phase 4
5. Phase 5
6. Phase 6
7. Phase 7
8. Phase 8

This order preserves the most important product invariant first:

- stop unnecessary load
- protect handwritten work
- generalize AI runtime shape
- only then deepen concurrency and API migration

## Success Criteria

- handwritten work remains responsive and durable under background load
- card saves retain reserved execution capacity under system pressure
- card writes jump the queue ahead of non-save work
- unsaved handwritten work remains recoverable if the backend crashes or is unavailable
- editing a card no longer causes repeated duplicate `/links` and `/backlinks` bursts
- the daemon remains responsive to unrelated requests while one request is slow
- the system supports more than one configured runner without regressing current local-runner behavior
- AI routing uses runner-qualified selections consistently across daemon and Linux frontend
- each phase can be implemented, tested, and shipped independently
