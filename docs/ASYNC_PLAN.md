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

### Foreground

This lane protects current-user editing flow and durable handwritten work.

- editor-adjacent card loads needed to keep typing
- card writes and autosave
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

- foreground work must not sit behind background work
- card writes must have reserved capacity and must not be starved even by other foreground work
- more configured runners must not linearly consume all request-worker capacity
- background work should debounce, coalesce, cancel, queue, degrade, or drop before foreground work is affected
- it is better to miss a nudge or show stale runner/connections status than to risk losing handwritten text

## Architecture Direction

### 1. Frontend Containment And Recovery

The Linux frontend should:

- suppress hidden and redundant Connections refreshes
- keep dirty state independent from background feature success or failure
- preserve failed-save state until save confirmation
- write local recovery drafts when backend saves fail or the backend becomes unavailable

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

### 3. Safe Concurrency Before Full Async

Before deeper async work:

- stop running accepted sessions inline on the listener loop
- introduce bounded request execution
- make DB usage safe for concurrent request handling
- keep save-path protection explicit
- keep AI runtime operations behind bounded execution lanes rather than detached-thread sprawl

### 4. Deeper Async End State

The eventual shape is:

- async network I/O
- bounded executors for blocking subsystems
- explicit foreground/background isolation
- dedicated save-path capacity
- AI runtime operations scheduled through explicit executors, not ambient threads

## Data And API Direction

Runner configuration should become first-class persisted data.

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

Model selection must become runner-qualified.

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

## Single TODO List

### Phase 1: Frontend Burst Containment

- [ ] Refresh Connections data only when the Connections tool is visible
- [ ] Debounce Connections graph refresh scheduling
- [ ] Enforce single-flight graph refresh for the active selection
- [ ] Drop stale graph refresh results when project/card/generation changes
- [ ] Suppress duplicate refresh triggers for the same effective target
- [ ] Add debug visibility for skipped, suppressed, stale, and coalesced refreshes
- [ ] Keep dirty editor state independent from background request success/failure

### Phase 2: Recovery And Save Semantics

- [ ] Preserve unsaved state until save confirmation
- [ ] Add retry/failure handling that does not incorrectly clear dirty state
- [ ] Add frontend-owned local recovery drafts for failed/unavailable backend saves
- [ ] Remove recovery drafts only after confirmed durable save
- [ ] Define save confirmation behavior shared between backend and frontend cleanup

### Phase 3: Runner Abstraction And Persistence

- [ ] Define canonical `runner_id`
- [ ] Define runner-qualified model reference format
- [ ] Define persisted runner configuration schema
- [ ] Introduce persisted manual runner records
- [ ] Represent the auto-local runner as a normal registry entry
- [ ] Introduce a runner registry/manager abstraction
- [ ] Generalize current `LocalModelRunner` behavior into per-runner client logic
- [ ] Keep registry/client APIs compatible with bounded executors rather than detached-thread-only behavior
- [ ] Make status probing and pull-job ownership runner-specific

### Phase 4: Safe Backend Concurrency

- [ ] Stop handling accepted sockets inline on the listener loop
- [ ] Introduce a small bounded worker pool for request/session execution
- [ ] Keep the listener thread lightweight: accept, dispatch, continue accepting
- [ ] Make DB usage safe before enabling concurrent request workers
- [ ] Choose and document the DB strategy:
- [ ] one DB connection per worker thread, or
- [ ] one DB connection per request/session, or
- [ ] dedicated serialized DB executor
- [ ] Introduce foreground/background execution lanes
- [ ] Ensure foreground save/load paths are not queued behind nudge, Connections, probe, pull, or other background work
- [ ] Reserve execution capacity specifically for card save operations

### Phase 5: API And Frontend Migration

- [ ] Add `GET /ai/runners`
- [ ] Add `POST /ai/runners`
- [ ] Add `PUT /ai/runners/{runner_id}`
- [ ] Add `DELETE /ai/runners/{runner_id}`
- [ ] Add `POST /ai/runners/{runner_id}/retry`
- [ ] Expand `/ai/status` to include `runners[]`
- [ ] Expand `/ai/capabilities` to include `runners[]` and runner-qualified model refs
- [ ] Update `/ai/local-models/config` to read and write runner-qualified selections
- [ ] Preserve temporary compatibility fields for current clients where needed
- [ ] Update the Linux frontend AI panel to show multiple runners
- [ ] Add runner list/add flow in the Linux frontend
- [ ] Update dropdowns and parsers to use runner-qualified model labels/refs

### Phase 6: Priority-Aware AI Routing

- [ ] Resolve selected local model config through `runner_id + model_name`
- [ ] Update normal local AI run paths to use the chosen runner
- [ ] Update title generation paths
- [ ] Update nudge paths that rely on local models
- [ ] Make pull jobs runner-aware in status APIs and UI
- [ ] Keep probes, pulls, nudges, and AI assistant work on background capacity by default
- [ ] Ensure multiple configured runners do not consume reserved save capacity

### Phase 7: Deeper Async Architecture

- [ ] Move connection accept/read/write handling to an actually asynchronous model
- [ ] Separate request I/O from blocking subsystem work
- [ ] Introduce dedicated executors for major blocking subsystems
- [ ] Add backpressure so expensive work cannot starve cheap routes
- [ ] Add cancellation or supersession for stale UI-driven background work where appropriate
- [ ] Keep route semantics stable unless a deliberate API change is approved
- [ ] Keep explicit foreground/save protection after the deeper refactor

### Phase 8: Cleanup

- [ ] Remove obsolete singular-runner assumptions from daemon code
- [ ] Remove obsolete singular-runner assumptions from Linux frontend parsers and UI
- [ ] Remove temporary compatibility response fields after frontend migration is complete
- [ ] Tighten logs and debug output to include runner identity and priority lane where useful

## Test Plan

### Frontend

- [ ] Verify Connections refresh does not fire while the Connections tool is hidden
- [ ] Verify typing produces one debounced refresh rather than one per buffer event
- [ ] Verify only one graph refresh is in flight at a time
- [ ] Verify stale graph results are ignored
- [ ] Verify failed saves keep the card marked unsaved
- [ ] Verify recovery drafts are written, restored, and cleaned up safely
- [ ] Verify multi-runner UI shows runner-qualified model options clearly

### Backend

- [ ] Verify the listener continues accepting requests while one request is slow
- [ ] Verify cheap routes still complete while a slow route is running
- [ ] Verify DB behavior is correct under concurrent request load with the chosen DB strategy
- [ ] Verify card, nudge, and AI routes do not regress
- [ ] Verify autosave is not blocked behind background work under load
- [ ] Verify reserved save capacity still allows card writes when background capacity is saturated
- [ ] Verify multi-runner status aggregation and runner CRUD
- [ ] Verify migration from plain model names to runner-qualified refs
- [ ] Verify AI run routing goes to the selected runner
- [ ] Verify multiple configured runners do not block card save paths under load

### Integration

- [ ] Reproduce repeated Connections refresh triggers while editing one card and confirm request volume stays bounded
- [ ] Reproduce the prior timeout pattern and confirm unrelated requests no longer get trapped behind one slow request
- [ ] Reproduce background AI/runtime load while typing and confirm handwritten work still saves reliably
- [ ] Simulate backend death during editing and confirm the frontend preserves recoverable handwritten work
- [ ] Verify current single-runner clients still work during the compatibility window

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
- unsaved handwritten work remains recoverable if the backend crashes or is unavailable
- editing a card no longer causes repeated duplicate `/links` and `/backlinks` bursts
- the daemon remains responsive to unrelated requests while one request is slow
- the system supports more than one configured runner without regressing current local-runner behavior
- AI routing uses runner-qualified selections consistently across daemon and Linux frontend
- each phase can be implemented, tested, and shipped independently
