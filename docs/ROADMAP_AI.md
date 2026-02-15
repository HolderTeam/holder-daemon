# AI Roadmap (Near-Term)

This roadmap is for the next focused implementation cycle.

Guiding rule: do not duplicate existing APIs that already solve the problem.

## Current Baseline

Already in place:

- Local runner capability/status: `GET /ai/capabilities`
- Caste detection + recommended models/install list in capabilities response
- Model pull workflow:
  - `POST /ai/runner/pull`
  - `GET /ai/runner/pull/{job_id}`
  - `GET /ai/runner/pull/{job_id}/events`
- AI execution with SSE: `POST /ai/complete`
- Persisted run history:
  - `GET /ai/runs`
  - `GET /ai/runs/{run_id}`
- Thread/message storage APIs (`/ai/threads`, `/ai/messages`)

## Commit 1: Persisted Router Config

Goal:

- Add explicit router model configuration (global + optional per-project override).

Why:

- Current router selection is heuristic and implicit.
- Frontends need deterministic control.

Changes:

- Schema: add `ai_router_config` table
  - `scope` (`global` or `project`)
  - `project_id` nullable
  - `router_model`
  - `mode` (optional future flag)
  - `updated_at`
- Add repo: `AiRouterConfigRepo`
- Add endpoints:
  - `GET /ai/router/config?project_id=...`
  - `PUT /ai/router/config`
- Update `/ai/complete` router model resolution order:
  1. forced model in request (already present)
  2. project router config
  3. global router config
  4. existing auto-pick fallback

Acceptance:

- Router config persists and is returned correctly.
- `/ai/complete` uses configured router when valid.
- Existing clients continue to work unchanged.

## Commit 2: Run-Centric Execution Surface

Goal:

- Formalize "runs" as the canonical execution concept.

Why:

- Avoid introducing duplicate `jobs` concept.

Changes:

- Keep `/ai/complete` for backward compatibility.
- Add run-centric stream endpoint:
  - `GET /ai/runs/{run_id}/events` (SSE)
- Optionally add `POST /ai/runs` as an alias entrypoint (internally same pipeline), if needed by frontend architecture.
- Ensure docs consistently use run terminology, not jobs.

Acceptance:

- Frontend can reconnect to a specific run stream by `run_id`.
- No semantic duplication between run/job resources.

## Commit 3: Manual Capture Convenience + Status Consolidation

Goal:

- Improve cloud copy/paste UX and clarify status entrypoint.

Why:

- Provenance capture should be easy for clients.
- Status model should be obvious.

Changes:

- Add convenience endpoint:
  - `POST /ai/messages/capture`
  - Inputs: `project_id`, optional `thread_id`, `prompt`, `response`, provenance (`source/provider/model/url`)
  - Behavior: create/find thread, append user+assistant messages, return IDs.
- Add status alias endpoint:
  - `GET /ai/status` as a thin compatibility alias to `/ai/capabilities`
  - Or document `/ai/capabilities` as canonical and skip alias.

Acceptance:

- Client can record manual cloud responses in one call.
- Status endpoint strategy is explicit and documented.

## Non-Goals (This Cycle)

- Full cloud API execution integration.
- Replacing `/ai/complete` in one step.
- Embeddings/vector search.

## Next Phase: REST Cloud Models (Provider-Agnostic)

Goal:

- Add cloud model execution via REST with strict automatic reliability controls.
- Keep one internal execution contract across providers.

Default product policy:

1. Prefer local models.
2. If local is not viable, prefer free-tier cloud routes (Google/Gemma first).
3. Keep paid providers supported but opt-in/off by default.

Design constraints:

- Strictly automatic quota control (no user micromanagement required).
- Never rely on hardcoded provider limits in code.
- Keep provider config in YAML; keep runtime counters/state in DB.

Planned architecture:

- Provider adapter interface (OpenAI/Anthropic/Google implementations).
- Quota governor (RPM/TPM/RPD budget checks + degradation decisions).
- Context compactor (sliding window + rolling summary + pinned facts).
- Model policy resolver (automatic route/model selection under pressure).

Planned config/state split:

- YAML (`providers.yaml`): endpoints, auth style, model ids, limits, default caps, cost tier, feature flags.
- DB: rolling quota counters, cooldown/error state, per-thread compaction state, per-run policy trace.

Planned rollout:

1. Add provider-agnostic request/response interface and YAML config loader.
2. Implement Google adapter first (Gemma + Gemini endpoints).
3. Add quota governor + compaction policy into `/ai/complete` path.
4. Add optional adapters for other providers without changing core policy engine.

Acceptance criteria:

- Low-end users can run cloud inference without routinely hitting provider caps.
- Automatic degradation is observable in run metadata/policy trace.
- Adding a new provider does not require schema redesign.

## Notes for Frontend Work

Recommended onboarding flow remains:

1. `GET /ai/capabilities`
2. show `caste` + `recommended_install`
3. pull selected model(s)
4. watch pull events
5. execute via `/ai/complete`
6. inspect runs via `/ai/runs`
