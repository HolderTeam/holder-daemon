# Daemon event streams

All event endpoints use the daemon's ordinary bearer authentication. Responses
use `text/event-stream`, with UTF-8 `id`, `event` and JSON `data` fields separated
by a blank line. The IDs are opaque: retain them unchanged. Comments are
heartbeats, not events. The REST OpenAPI contract describes the payloads through
`x-sse-events`; generated HTTP clients still need an SSE reader for incremental
stream consumption.

## Changes and initial snapshots

`GET /events` reports invalidations for committed database state and observed Git
HEAD changes. Event names are `project.changed`, `card.changed`,
`resource.changed`, `location.changed`, `thread.changed`, `message.changed` and
`run.changed`. Each carries `entity`, `entity_id`, nullable `project_id`,
`deleted`, and nullable `git_revision`. Reread the affected entity or collection
through the existing API. Links, nudges and Git sync status invalidate their
project; resource metadata, assets and placements invalidate their resource.
`deleted` means the entity disappeared from the database snapshot; soft deletion
is an ordinary invalidation and must be interpreted by rereading live state.

The observer owns a dedicated read-only database connection and checks committed
snapshots every 250 ms. Writes from request workers and background workers use
the same feed. It detects content/index/tag edits even if `updated_at` stays the
same. Intermediate states between observations can coalesce. This is a current
state invalidation feed, not an audit trail or a guarantee of every mutation.
It does not watch arbitrary unindexed filesystem edits, configuration files or
remote changes that have not been imported into the daemon.

To establish a snapshot without missing changes:

1. Fetch `GET /events/cursor`, optionally with `?project_id=...`.
2. Fetch the current entities needed by the client.
3. Subscribe to `/events` with the same project filter and the checkpoint's
   `cursor` in the `Last-Event-ID` header.
4. Process replayed invalidations, reread affected entities, then continue live.

With no cursor, `/events` starts at the current tail. Its first event is `ready`,
with the current checkpoint: `cursor`, `git_revisions`, and `history_urls`.
The `ready` frame's ID preserves the requested replay position; use delivered
change IDs to advance your reconnect cursor. Delivery around a disconnect may
repeat an event. Treat invalidations as idempotent and retain the last processed
ID, rather than simply the last received ID.

## Reconnect and Git recovery

`Last-Event-ID` replays bounded history from this daemon process. The change
journal retains at most 4096 events and 1 MiB of encoded frames. A stale, foreign
or future cursor returns `resync_required`, then closes the connection. Invalid
cursor syntax is an HTTP 400 error. An observer failure also requires resync.

A project-scoped client can additionally supply
`?project_id=...&last_revision=<full-lowercase-Git-SHA>`. When replay is unavailable,
the resync payload includes the supplied revision, current `git_revisions`,
`history_urls`, a new `cursor`, and a `reason`. A revision without an in-memory
cursor also explicitly requires refresh. The daemon does not create a persistent
change journal or synthesize transient events from Git after restart.

Git is the durable history for committed project content. Inspect the existing
`/projects/{project_id}/history` endpoint to compare with the client's known
revision, then refresh current API state using the snapshot procedure above.
History pagination is not a `changed_since` query. Rewritten/pruned history or
an unavailable old revision requires a full refresh.

A revision is the observed project HEAD, not a transaction marker covering each
live database change. Pending Git writes, AI run state and other local state can
change while HEAD stays the same. Always honor `resync_required`, even when the
revision matches. Missing, unborn or inaccessible Git repositories report null;
the observer never initializes or repairs them.

## AI run and model-pull streams

`POST /ai/runs` emits `run_started` with `run_id`, followed by the existing
provider-dependent `router`, `progress`, `fallback`, `chunk`, `done` or `failed`
events. Each event has a replay ID and `run_id`. Reconnect through
`GET /ai/runs/{run_id}/events` with `Last-Event-ID`; do not repeat the POST to
resume. Run histories retain at most 512 events and 1 MiB per run, across at most
128 retained runs. An expired run cursor or truncated initial history emits
`resync_required`; fetch the run's current state. After restart a fresh run
subscription can recover a persisted terminal status, but not transient chunks.
A subscription to an unfinished run without retained history waits for new events.

`GET /ai/runner/pull/{job_id}/events` emits progress snapshots when status changes
and finishes on `completed` or `failed`. Pull history is connection-local. After a
reconnect resync, omit `Last-Event-ID` to receive the latest snapshot. Fetching the
pull job is also available through its ordinary JSON endpoint.

Disconnecting any stream unsubscribes the connection; it does not cancel an AI
run or model pull. A client that needs cancellation must use a supported job
operation. Stream replay never re-executes a write.

## Connection limits and shared transport

Streams use asynchronous socket reads/writes on the daemon's dedicated I/O
threads. Idle subscriptions keep no request worker or database transaction open.
A separate two-thread pool reads subscription sources, including runner adapters.
The listener admits at most 64 simultaneous streams across these endpoints,
rejecting additional requests with HTTP 503. Each socket bounds queued output to
2 MiB, closes on overflow, and has a 10-second write deadline. Heartbeat comments
arrive every 15 seconds. Shutdown cancels all streams and joins source work before
destroying runner adapters.

The change observer fingerprints current indexed entities when the database
changes and reads project Git heads each observation. This has work proportional
to the indexed dataset on changed snapshots; measure larger datasets before
relying on it for high-rate ingestion.

`SseStream` supplies framing, admission, queues, heartbeats and lifecycle without
knowing Holder entity semantics. A future MCP Streamable HTTP adapter can reuse
that machinery and the change source while implementing its own JSON-RPC
messages, authorization, session/replay rules and cancellation. REST change IDs
are not implicitly MCP session IDs or protocol replay cursors. A finite
`changed_since` API remains a separate useful primitive for batch clients.
