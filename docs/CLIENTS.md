# Client Integration Guide

This document describes how clients should connect to Holder safely and predictably.

## 1) Discover the Running Server

Read `holder.json`.

Expected fields:

- `pid`
- `api_version`
- `server_version`
- `bind`
- `port`
- `auth_token`
- `started_at`

If the file does not exist, server may not be running.

## 2) Health Check

Call:

- `GET /health`
- Header: `Authorization: Bearer <auth_token>`

Use this to validate API compatibility and process health.

## 3) Auth Model

All API requests require bearer auth.

Header:

`Authorization: Bearer <auth_token>`

## 4) Response Conventions

- Success: `{ "ok": true, "data": ... }`
- Error: `{ "ok": false, "error": { "code": "...", "message": "..." } }`

## 5) First-Run Project Flow

1. `GET /projects`
2. If empty: `POST /projects` with `{ "name": "My Project" }`
3. Create first card: `POST /cards`
4. List cards: `GET /cards?project_id=...`
5. Load card: `GET /cards/{card_id}`
6. Autosave edits: `PATCH /cards/{card_id}`

## 6) AI Onboarding Flow (Local Models)

1. `GET /ai/capabilities`
2. Read:
   - `runner_available`
   - `error`
   - `caste` (`name`, `reason`)
   - `models` (installed)
   - `recommended_models`
   - `recommended_install`
3. If required, start install:
   - `POST /ai/runner/pull` with `{ "model": "<tag>" }`
4. Track install:
   - Poll: `GET /ai/runner/pull/{job_id}`
   - Stream: `GET /ai/runner/pull/{job_id}/events`
5. Refresh `GET /ai/capabilities` after pull completes.
6. Use `GET /ai/status` for live operational state (active runs/pulls, runner health).

## 7) AI Execution + History

Primary execution endpoint:

- `POST /ai/runs` (SSE stream)

Persisted run history:

- `GET /ai/runs?project_id=...` or `GET /ai/runs?thread_id=...`
- `GET /ai/runs/{run_id}`
- `GET /ai/runs/{run_id}/events` (SSE reconnect stream)

Thread/message APIs:

- `GET/POST /ai/threads`
- `GET/PATCH /ai/threads/{thread_id}`
- `GET/POST /ai/messages`
- `POST /ai/messages/capture` (one-call prompt+response capture with provenance)
- `GET/PATCH/DELETE /ai/messages/{message_id}`

For manual capture (copy/paste cloud responses), prefer `POST /ai/messages/capture`.

## 8) Search

- `GET /search/cards?project_id=...&q=...`
- `GET /search/ai?project_id=...&q=...`

## 9) Docs Endpoints

- Swagger UI: `GET /docs`
- OpenAPI: `GET /openapi.yaml`
- Model catalog: `GET /models.yaml`

## 10) Local-First Rule

Clients must never write project files or DB directly. Server owns persistence/indexing.
