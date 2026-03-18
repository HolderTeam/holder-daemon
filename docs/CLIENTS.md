# Holder Daemon Client Integration (Codebase-Accurate)

This document reflects the current `holder-daemon` backend behavior and route surface.

## 1) Discover Running Daemon + Token

Read server info file:

- `~/.local/share/holder/server/holder.json` (XDG-based; see `platform/Paths`)

Current fields written by backend:

- `pid`
- `bind`
- `port`
- `started_at`
- `api_version`
- `server_version`
- `auth_token`

Important:

- `auth_token` is generated at daemon startup.
- If daemon restarts, old token becomes invalid and clients must reread `holder.json`.

## 2) Authentication Model

Most endpoints require:

- `Authorization: Bearer <auth_token>`

Unauthenticated endpoints are static/docs only:

- `GET /openapi.yaml`
- `GET /ai_catalog.yaml`
- `GET /ai_catalog.json`
- `GET /git_providers.yaml`
- `GET /git_providers.json`
- `GET /docs` and `/docs/*`

`GET /health` is authenticated in current code path.

## 3) Response Shape

- Success: `{ "ok": true, "data": ... }`
- Error: `{ "ok": false, "error": { "code": "...", "message": "..." } }`

Common error codes include `bad_request`, `not_found`, `unauthorized`, `conflict`, `method_not_allowed`.

## 4) Core Non-AI Endpoints

Project/card clients usually use:

- `GET/POST /projects`
- `GET/PATCH/DELETE /projects/{project_id}`
- `POST /projects/{project_id}/git/test-remote`
- `POST /projects/{project_id}/git/push`
- `GET /projects/{project_id}/git/sync-status`
- `GET /projects/{project_id}/encryption-check`
- `POST /projects/{project_id}/recovery-token/export`
- `POST /projects/{project_id}/recovery-token/import`
- `POST /recovery-token/import`

- `GET/POST /cards`
- `GET/PATCH/DELETE /cards/{card_id}`
- `GET /cards/context`

- `GET/POST /resources`
- `PATCH/DELETE /resources/{resource_id}`

- `GET/DELETE /trash`
- `DELETE /trash/{item_id}`

- `GET /search/cards`
- `GET /search/ai`

## 5) AI: Status + Onboarding Surface

### Runtime/capabilities

- `GET /ai/capabilities`
  - Local runner availability and installed local models
  - Machine caste detection + recommended local models
  - Router config snapshot (`global`, `project`, `effective`)
- `GET /ai/status`
  - Active runs count
  - Runner state
  - Pull jobs summary
  - Configured cloud provider credentials (masked preview)
- `POST /ai/runner/retry`
  - Retry local runner probe/start

### Local model pull/install

- `POST /ai/runner/pull` body: `{ "model": "<tag>" }`
- `GET /ai/runner/pull/{job_id}`
- `GET /ai/runner/pull/{job_id}/events` (SSE)

## 6) AI: Provider Catalog + Credentials + Router Config

### Provider catalog (from `config/ai_catalog.yaml`)

- `GET /ai/providers/catalog`
  - Includes providers, models, limits, auth/api metadata, setup/docs URLs
  - Merges YAML defaults with DB state for `enabled` + `configured`

### Provider settings (enabled flags)

- `GET /ai/providers/settings`
- `PUT /ai/providers/settings` with `{ "provider": "...", "enabled": true|false }`
- `DELETE /ai/providers/settings/{provider}`

### Provider credentials

- `GET /ai/providers/credentials`
- `PUT /ai/providers/credentials` with `{ "provider": "...", "api_key": "..." }`
- `DELETE /ai/providers/credentials/{provider}`

Notes:

- On credential PUT, backend also enables that provider in settings.
- Credential values are stored in DB (`ai_provider_credentials`), returned masked in read APIs.

### Router config

- `GET /ai/router/config?project_id=...`
- `PUT /ai/router/config` with:
  - `scope`: `global` or `project`
  - `project_id` (required for `scope=project`)
  - `router_model` (optional; omit/null to clear)

Effective precedence is project override > global override > auto.

## 7) AI: Runs (Execution + History)

### Start run

- `POST /ai/runs` (SSE response)

Backend behavior:

- Uses local path when local runner is ready and no explicit cloud provider is requested.
- Uses cloud path when local runner unavailable or provider is explicitly requested.
- Auto-creates thread when `project_id` is provided and `thread_id` is missing.

### Query runs

- `GET /ai/runs?project_id=...` or `GET /ai/runs?thread_id=...`
- `GET /ai/runs/{run_id}`
- `GET /ai/runs/{run_id}/events` (SSE stream/reconnect)

Stored run record includes:

- mode, prompt, context
- router/chosen model
- ranked candidates (local)
- policy trace (cloud)
- status/error

## 8) AI: Threads + Messages

### Threads

- `GET /ai/threads?project_id=...`
- `POST /ai/threads`
- `GET /ai/threads/{thread_id}`
- `PATCH /ai/threads/{thread_id}`
- `DELETE /ai/threads/{thread_id}`

### Messages

- `GET /ai/messages?thread_id=...`
- `POST /ai/messages`
- `GET /ai/messages/{message_id}`
- `PATCH /ai/messages/{message_id}`
- `DELETE /ai/messages/{message_id}` (soft delete/trash)
- `POST /ai/messages/{message_id}/restore`
- `POST /ai/messages/capture` (single call prompt+response capture; creates thread if needed)

## 9) AI Catalog / Config Source

Cloud/local model metadata is loaded from:

- `config/ai_catalog.yaml`
- or `HOLDER_AI_CATALOG_PATH` override

If missing, cloud catalog endpoints and cloud run selection paths return config-related errors.

## 10) Client Rules

- Do not write project files, git metadata, or SQLite directly.
- Treat daemon as source of truth for state transitions.
- On 401, reread `holder.json` and retry with fresh bearer token.
