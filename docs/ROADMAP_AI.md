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
- AI execution with SSE: `POST /ai/runs`
- Persisted run history:
  - `GET /ai/runs`
  - `GET /ai/runs/{run_id}`
  - `GET /ai/runs/{run_id}/events`
- Thread/message storage APIs (`/ai/threads`, `/ai/messages`)
- Persisted router config:
  - schema: `ai_router_config`
  - endpoints:
    - `GET /ai/router/config`
    - `PUT /ai/router/config`
  - `/ai/runs` router precedence:
    1. forced request model
    2. project router config
    3. global router config
    4. auto fallback
- `/ai/capabilities` includes router config and effective scope/model
- Operational status endpoint:
  - `GET /ai/status`
  - exposes live runtime state (runner health, active runs, active pulls)
- Manual capture convenience endpoint:
  - `POST /ai/messages/capture`
  - creates/fetches thread and appends user+assistant messages with provenance
- Cloud provider credential setup endpoints:
  - `GET /ai/providers/credentials`
  - `PUT /ai/providers/credentials`
  - `DELETE /ai/providers/credentials/{provider}`
  - `/ai/status` includes configured cloud providers
- Cloud provider catalog endpoint:
  - `GET /ai/providers/catalog`
  - reads `config/cloudproviders.yaml` and marks configured providers from DB
- `/ai/runs` cloud fallback path (provider-agnostic selection, first adapter):
  - if local runner unavailable, route via configured cloud provider credentials
  - uses `config/cloudproviders.yaml` + `ai_provider_credentials`
  - first execution adapter: `chocolatefactory` (`generateContent`)

## Next Phase: REST Cloud Models (Provider-Agnostic)

Goal:

- Add cloud model execution via REST with strict automatic reliability controls.
- Keep one internal execution contract across providers.

Default product policy:

1. Prefer local models.
2. If local is not viable, prefer free-tier cloud routes (ChocolateFactory/Gemma first).
3. Keep paid providers supported but opt-in/off by default.

Design constraints:

- Strictly automatic quota control (no user micromanagement required).
- Never rely on hardcoded provider limits in code.
- Keep provider config in YAML; keep runtime counters/state in DB.

Planned architecture:

- Provider adapter interface (OpenAI/Anthropic/ChocolateFactory implementations).
- Quota governor (RPM/TPM/RPD budget checks + degradation decisions).
- Context compactor (sliding window + rolling summary + pinned facts).
- Model policy resolver (automatic route/model selection under pressure).

Planned config/state split:

- YAML (`cloudproviders.yaml`): endpoints, auth style, model ids, limits, default caps, cost tier, feature flags.
- DB: rolling quota counters, cooldown/error state, per-thread compaction state, per-run policy trace.

Planned rollout:

1. Add provider-agnostic request/response interface and YAML config loader.
2. Implement ChocolateFactory adapter first (Gemma + Gemini endpoints).
3. Add quota governor + compaction policy into `/ai/runs` path.
4. Add optional adapters for other providers without changing core policy engine.

Acceptance criteria:

- Low-end users can run cloud inference without routinely hitting provider caps.
- Automatic degradation is observable in run metadata/policy trace.
- Adding a new provider does not require schema redesign.

## Next Phase: Embeddings + Vector Retrieval

Goal:

- Add semantic retrieval for cards/AI history/resources to improve run context quality.

Scope:

- local-first embedding generation and storage
- vector similarity retrieval at run time
- context assembly that mixes:
  - recency (recent turns)
  - lexical search (FTS)
  - semantic retrieval (vectors)

Initial constraints:

- keep retrieval transparent and inspectable
- cap retrieved context by token budget before model call
- preserve provenance (which chunks were retrieved and why)

## Notes for Frontend Work

Recommended onboarding flow remains:

1. `GET /ai/capabilities`
2. show `caste` + `recommended_install`
3. pull selected model(s)
4. watch pull events
5. execute via `/ai/runs`
6. inspect runs via `/ai/runs`
