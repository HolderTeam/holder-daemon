# Async Plan

## Summary

This plan addresses the failure mode seen in Holder when the Linux frontend generates repeated Connections refresh requests and the daemon handles HTTP sessions one at a time.

The top-level product rule is: handwritten user work comes first. If the system is under pressure, card editing and autosave must be protected ahead of Connections, nudges, AI status, and other secondary features.

In particular, card save capacity must be reserved. Best-effort work must never be able to consume all execution capacity while user-written text is waiting to be persisted.

If the backend dies or the system becomes unstable, the frontend should still preserve unsaved handwritten work through a local crash-recovery draft path.

The work is split into three phases:

1. Stop unnecessary frontend request bursts.
2. Remove the daemon's single-request bottleneck with a safe concurrent interim design.
3. Move the daemon toward a genuinely async architecture with isolated executors for blocking work.

For now, transport stays as HTTP. Unix sockets, WebSockets, or other transport changes are not part of the first implementation tranche.

## Incident Summary

Observed behavior:

- The Linux frontend repeatedly requested the same card's `/links` and `/backlinks` endpoints in tight loops.
- The daemon accepted one socket and ran one session inline before accepting the next connection.
- Slow routes such as nudge evaluation degraded first.
- Once one request path stalled badly enough, unrelated routes such as autosave also timed out behind it.

Current contributing causes:

- Connections graph refreshes are triggered too often.
- Graph refresh requests are not visibility-aware.
- Duplicate refreshes are allowed to overlap.
- The daemon request path is effectively single-lane.
- Blocking work is not isolated from cheap request handling.

## Goals

- Protect the handwritten-work path above all other features.
- Protect card saves as the single most important execution path in the system.
- Preserve unsaved handwritten work even if the backend is unavailable or crashes.
- Editing one card should not generate repeated duplicate Connections requests.
- One slow request must not freeze unrelated requests.
- Cheap routes such as health, status, and normal CRUD should remain responsive under load.
- The implementation should be incremental, with each phase shippable and testable on its own.

## Non-Goals

- Switching away from HTTP in phase 1.
- Rewriting the entire daemon around a new transport.
- Doing a single large concurrency rewrite in one step.

## Priority Model

Holder should treat request classes differently based on user value.

### Foreground

This lane protects current-user editing flow and durable handwritten work.

- Editor-adjacent card loads needed to keep typing
- Card writes and autosave
- Project and card selection requests required to continue editing
- Minimal health or status requests needed to keep the app usable

### Background

This lane is useful but must yield under pressure.

- Connections graph and related metadata refreshes
- Links and backlinks refreshes that are not required for editing
- AI nudges
- Background AI status polling
- Toolbox refreshes and speculative prefetches

### Design Rules

- Foreground work must not sit behind background work.
- Card writes must have reserved capacity and must not be starved even by other foreground tasks.
- Background work should debounce, coalesce, and cancel aggressively.
- If capacity is constrained, background work should delay, degrade, or drop first.
- It is better to miss a nudge or show stale connections than to risk losing user-written text.
- If backend persistence is unavailable, the frontend must still preserve unsaved handwritten work locally.

## Crash Recovery Model

The frontend should own a local recovery-draft mechanism that does not depend on the daemon being healthy.

### Recovery Rules

- Dirty editor state is tracked locally as soon as the user types.
- Failed backend saves must not clear dirty state.
- When save attempts fail or the backend becomes unavailable, the frontend should write or update a local recovery draft.
- Recovery drafts should be removed only after a confirmed successful save makes them unnecessary.
- On next launch, the app should detect recovery drafts and offer restore when they contain newer or unsaved handwritten work.

### Recovery Draft Requirements

- [ ] One recovery draft per dirty card
- [ ] Atomic write behavior
- [ ] Enough metadata to match and restore safely
- [ ] Separate storage path from normal project persistence
- [ ] Safe cleanup only after confirmed durable save

Suggested metadata:

- `project_id`
- `card_id`
- title
- body
- timestamp
- last known saved fingerprint or revision marker

### Notes

- This is not a replacement for normal saves.
- This is the last-resort protection path when the daemon or system is unhealthy.
- Recovery drafts should be frontend-owned because the backend may be the failing component.

## Phase 1: Frontend Burst Containment

### Deliverables

- [ ] Only refresh Connections graph data when the Connections tool is visible.
- [ ] Debounce Connections graph refresh scheduling so editor typing does not trigger one request cycle per buffer event.
- [ ] Enforce at most one in-flight graph refresh for the active selection.
- [ ] Drop stale graph refresh results when project, card, or refresh generation changes.
- [ ] Avoid duplicate refresh triggers when the effective target has not changed.
- [ ] Add debug visibility for skipped, suppressed, stale, and coalesced Connections refreshes.

### Likely Files

- `holder-desktop/frontends/linux/src/views/toolbox/connections_tool_view.vala`
- `holder-desktop/frontends/linux/src/views/toolbox.vala`
- `holder-desktop/frontends/linux/src/views/window.vala`
- `holder-desktop/frontends/linux/src/controllers/connections.vala`

### Notes

- The main fix is to stop hidden or redundant graph refreshes.
- Autosave behavior should remain functionally unchanged.
- Single-flight plus debounce is the minimum acceptable behavior for this phase.
- While the user is actively editing, the frontend should avoid generating low-value background traffic wherever practical.
- Frontend state should continue to track card dirtiness independently from background feature success or failure.
- Recovery drafts are not the main deliverable of phase 1, but phase 1 should not make later recovery integration harder.

## Phase 2: Safe Backend Concurrency

### Deliverables

- [ ] Stop handling accepted sockets inline on the listener loop.
- [ ] Introduce a small bounded worker pool for request/session execution.
- [ ] Keep the listener thread lightweight: accept, dispatch, continue accepting.
- [ ] Make DB usage safe before enabling multiple concurrent request workers.
- [ ] Add concurrency limits or queues for expensive work if needed.
- [ ] Preserve existing route behavior while removing the single-request bottleneck.
- [ ] Introduce at least two execution lanes: foreground for card editing/autosave paths, background for best-effort work.
- [ ] Ensure foreground write paths are not queued behind nudge, Connections, or other background work.
- [ ] Reserve execution capacity specifically for card save operations.
- [ ] Introduce retry behavior and failure handling for save operations that preserves dirty state until save confirmation.
- [ ] Define how backend save confirmation interacts with frontend recovery-draft cleanup.

### DB Safety Requirements

- [ ] Do not share the current single SQLite handle across multiple worker threads.
- [ ] Choose and document one strategy before enabling concurrent session execution.

Candidate strategies:

- [ ] One DB connection per worker thread.
- [ ] One DB connection per request or session.
- [ ] A dedicated serialized DB executor, if that proves safer for the current codebase.

### Likely Files

- `holder-daemon/src/api/Listener.cpp`
- `holder-daemon/src/api/Listener.h`
- `holder-daemon/src/api/Session.cpp`
- `holder-daemon/src/api/Session.h`
- `holder-daemon/src/api/HttpServer.cpp`
- `holder-daemon/src/platform/Db.cpp`
- `holder-daemon/src/platform/Db.h`

### Notes

- This phase is about safe concurrency, not about becoming fully async.
- A worker pool is an interim containment step.
- The point of this phase is that one slow request no longer freezes all request handling.
- This phase should already encode product priority, not just raw concurrency.
- Save-path protection is a product invariant, not an optimization.

## Phase 3: Real Async Architecture

### Deliverables

- [ ] Move connection accept, read, and write handling to an actually asynchronous model.
- [ ] Separate request I/O from blocking subsystem work.
- [ ] Introduce dedicated executors for major blocking subsystems.
- [ ] Add backpressure so expensive work cannot starve cheap routes.
- [ ] Add cancellation or supersession behavior for stale UI-driven work where appropriate.
- [ ] Keep route semantics stable unless a deliberate API change is approved.
- [ ] Preserve an explicit foreground lane for handwritten-work protection even after the deeper async refactor.
- [ ] Preserve dedicated save-path capacity even after the deeper async refactor.
- [ ] Add a full frontend crash-recovery flow for unsaved handwritten work.

### Executor Targets

- [ ] DB executor
- [ ] Git/filesystem executor
- [ ] AI/nudge executor
- [ ] Foreground request executor or reserved capacity for card read/write paths
- [ ] Dedicated save queue or executor, or an equivalent reserved-capacity mechanism for card writes

### Notes

- The end state is not "everything runs anywhere".
- The end state is: async network layer, bounded executors for blocking work, and explicit isolation between cheap and expensive routes.

## Test Plan

### Frontend

- [ ] Verify Connections refresh does not fire while the Connections tool is hidden.
- [ ] Verify typing produces one debounced graph refresh rather than one per buffer change.
- [ ] Verify selection churn does not duplicate refreshes for the same effective target.
- [ ] Verify only one graph refresh is in flight at a time.
- [ ] Verify stale results are ignored cleanly.
- [ ] Verify active editing suppresses or coalesces non-essential background traffic where intended.
- [ ] Verify dirty state remains accurate even when background requests fail.
- [ ] Verify failed saves leave the card marked unsaved until confirmation of a later successful save.
- [ ] Verify recovery drafts are written when backend saves fail.
- [ ] Verify recovery drafts survive app restart and can be restored.
- [ ] Verify confirmed successful saves clean up obsolete recovery drafts safely.

### Backend

- [ ] Verify the listener can continue accepting requests while one request is slow.
- [ ] Verify cheap routes still complete while a slow route is running.
- [ ] Verify DB behavior is correct under concurrent request load with the chosen DB strategy.
- [ ] Verify card, nudge, and AI routes do not regress.
- [ ] Verify autosave is not blocked behind background work under load.
- [ ] Verify background work can degrade or queue without affecting foreground save paths.
- [ ] Verify reserved save capacity still allows card writes when background capacity is saturated.
- [ ] Verify save retries do not lose or incorrectly clear pending handwritten changes.

### Integration

- [ ] Reproduce repeated Connections refresh triggers while editing one card and confirm request volume stays bounded.
- [ ] Verify autosave still succeeds under moderate concurrent read load.
- [ ] Reproduce the prior timeout pattern and confirm cheap routes no longer get trapped behind one slow request.
- [ ] Reproduce background load while typing and confirm handwritten work still saves reliably.
- [ ] Simulate backend death during editing and confirm the frontend preserves recoverable handwritten work.
- [ ] Simulate app restart after failed saves and confirm recovery detection offers restore.

## Success Criteria

- [ ] The handwritten-work path remains responsive and durable under background load.
- [ ] Card saves retain reserved execution capacity under system pressure.
- [ ] Unsaved handwritten work remains recoverable even if the backend crashes or is unavailable.
- [ ] Editing a card no longer causes repeated duplicate `/links` and `/backlinks` bursts for the same card.
- [ ] The daemon remains responsive to unrelated requests while one request is slow.
- [ ] Slow nudge, AI, or git work no longer freezes normal card CRUD and status routes.
- [ ] Each phase can be implemented, tested, and shipped independently.

## Implementation Order

- [ ] Phase 1: Frontend burst containment
- [ ] Phase 2: Safe backend concurrency
- [ ] Phase 3: Real async architecture

## Decision Log

- Transport remains HTTP for the initial fix.
- Frontend request burst containment is required even if the backend becomes concurrent.
- Backend concurrency must not be enabled by simply sharing the current DB handle across threads.
- Handwritten user work is the highest-priority path and must be protected ahead of background features.
- The architecture should model at least two classes of work: foreground and background.
- Card save operations are the most important execution path and should have reserved capacity by design.
- The frontend must provide a local recovery path for unsaved handwritten work when backend persistence is unavailable.
