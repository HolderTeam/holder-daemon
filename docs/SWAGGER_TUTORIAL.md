# Swagger Tutorial (Holder API)

This walkthrough covers:

- project/card setup
- local model onboarding
- running AI completion
- viewing AI run history

Open Swagger UI:

`http://127.0.0.1:11499/docs`

## 0) Authorize

1. Click **Authorize**.
2. Paste token from server log: `Bearer <token>`.
3. Authorize and close.

## 1) Create a Project

`POST /projects`

```json
{
  "name": "My First Project"
}
```

Copy `project_id`.

## 2) Create a Card

`POST /cards`

```json
{
  "project_id": "<project_id>",
  "title": "First note",
  "content": "This card mentions espresso beans."
}
```

Copy `card_id`.

## 3) Check AI Capabilities + Recommendations

`GET /ai/capabilities`

Inspect:

- `data.caste`
- `data.models` (installed)
- `data.recommended_install` (good next pulls)

## 4) Pull a Recommended Model

`POST /ai/runner/pull`

```json
{
  "model": "qwen2.5:0.5b"
}
```

Copy returned `job_id`.

Track progress with:

- `GET /ai/runner/pull/{job_id}`
- `GET /ai/runner/pull/{job_id}/events`

## 5) Run AI Completion

`POST /ai/complete`

```json
{
  "prompt": "Summarize the card in one paragraph.",
  "project_id": "<project_id>",
  "context": {
    "card_id": "<card_id>",
    "card_title": "First note",
    "card_body": "This card mentions espresso beans."
  }
}
```

Response is `text/event-stream` (SSE).
Swagger UI does not render SSE well; use browser network panel or `curl` for live events.

## 6) View Run History

List runs for project:

`GET /ai/runs?project_id=<project_id>`

Fetch one run:

`GET /ai/runs/{run_id}`

## 7) View AI Threads and Messages

- `GET /ai/threads?project_id=<project_id>`
- `GET /ai/messages?thread_id=<thread_id>`

## 8) Search

Cards:

`GET /search/cards?project_id=<project_id>&q=espresso`

AI messages:

`GET /search/ai?project_id=<project_id>&q=summarize`
