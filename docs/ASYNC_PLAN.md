# Async Plan

## Summary

This plan addresses the failure mode seen in Holder when the Linux frontend generates repeated Connections refresh requests and the daemon handles HTTP sessions one at a time.

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

- Editing one card should not generate repeated duplicate Connections requests.
- One slow request must not freeze unrelated requests.
- Cheap routes such as health, status, and normal CRUD should remain responsive under load.
- The implementation should be incremental, with each phase shippable and testable on its own.

## Non-Goals

- Switching away from HTTP in phase 1.
- Rewriting the entire daemon around a new transport.
- Doing a single large concurrency rewrite in one step.

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

## Phase 2: Safe Backend Concurrency

### Deliverables

- [ ] Stop handling accepted sockets inline on the listener loop.
- [ ] Introduce a small bounded worker pool for request/session execution.
- [ ] Keep the listener thread lightweight: accept, dispatch, continue accepting.
- [ ] Make DB usage safe before enabling multiple concurrent request workers.
- [ ] Add concurrency limits or queues for expensive work if needed.
- [ ] Preserve existing route behavior while removing the single-request bottleneck.

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

## Phase 3: Real Async Architecture

### Deliverables

- [ ] Move connection accept, read, and write handling to an actually asynchronous model.
- [ ] Separate request I/O from blocking subsystem work.
- [ ] Introduce dedicated executors for major blocking subsystems.
- [ ] Add backpressure so expensive work cannot starve cheap routes.
- [ ] Add cancellation or supersession behavior for stale UI-driven work where appropriate.
- [ ] Keep route semantics stable unless a deliberate API change is approved.

### Executor Targets

- [ ] DB executor
- [ ] Git/filesystem executor
- [ ] AI/nudge executor

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

### Backend

- [ ] Verify the listener can continue accepting requests while one request is slow.
- [ ] Verify cheap routes still complete while a slow route is running.
- [ ] Verify DB behavior is correct under concurrent request load with the chosen DB strategy.
- [ ] Verify card, nudge, and AI routes do not regress.

### Integration

- [ ] Reproduce repeated Connections refresh triggers while editing one card and confirm request volume stays bounded.
- [ ] Verify autosave still succeeds under moderate concurrent read load.
- [ ] Reproduce the prior timeout pattern and confirm cheap routes no longer get trapped behind one slow request.

## Success Criteria

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
