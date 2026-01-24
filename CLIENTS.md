# Client Integration Guide

This document describes how a client should connect to the holder server and
interact with the API in a safe, predictable way. It assumes a local-first
deployment and a server that owns all persistent data.

## 1) Discover the running server

Clients should read the server info file (`holder.json`) to find the bind
address, port, auth token, and version fields.

Expected fields:
- `pid`
- `api_version`
- `server_version`
- `bind`
- `port`
- `auth_token`
- `started_at`

`holder.json` is written atomically by the server on startup. If the file does
not exist, the server may not be running.

## 2) First API call: health check

The client should validate connectivity and API compatibility:

- `GET /health`
- Header: `Authorization: Bearer <auth_token>`

Expected success response:
```json
{
  "ok": true,
  "data": {
    "db_ok": true,
    "uptime_ms": 1234,
    "api_version": "0.1",
    "server_version": "0.1.0",
    "pid": 12345
  }
}
```

If `api_version` is unknown or unsupported, the client should refuse to
continue and prompt the user to update either the client or the server.

## 3) Auth model

All API requests require the bearer token from `holder.json`.

Header:
```
Authorization: Bearer <auth_token>
```

If the token is missing or invalid, the server responds:
```json
{
  "ok": false,
  "error": { "code": "unauthorized", "message": "Missing or invalid token." }
}
```

## 4) Response shape conventions

All JSON responses are standardized:

- Success:
```json
{ "ok": true, "data": ... }
```
- Error:
```json
{ "ok": false, "error": { "code": "...", "message": "..." } }
```

## 5) Typical request flow (future-facing)

Once project and card endpoints are implemented, a client flow should look
like:

1. `GET /projects` to list projects.
2. `POST /projects` to create a new project if needed.
3. `GET /cards?project_id=...` or `GET /cards/{id}` to load content.
4. `PATCH /cards/{id}` for updates (auto-save style).
5. `GET /search/cards?project_id=...&q=...` for search.

## 6) Search endpoints

Search endpoints require `project_id` and `q`:

- `GET /search/cards?project_id=...&q=...`
- `GET /search/ai?project_id=...&q=...`

Results include metadata so clients can render a list immediately:

`/search/cards` response items:
- `card_id`
- `title`
- `updated_at`
- `created_at`
- `snippet`
- `rank`

`/search/ai` response items:
- `message_id`
- `created_at`
- `snippet`
- `rank`

## 7) Notes on local-first behavior

Clients should never touch the project repo or database directly. The server
owns all persistence and indexing.

If the server is not running, the client can prompt the user to start it or
attempt to launch it as a separate process.

## 8) Swagger docs

The server exposes Swagger UI at:

- `http://<bind>:<port>/docs`

The OpenAPI spec is served at:

- `http://<bind>:<port>/openapi.yaml`
