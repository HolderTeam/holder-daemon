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

Execution phases:

1. Foundation (Done)
   - provider-agnostic config loader (`config/cloudproviders.yaml`)
   - credential setup/storage (`/ai/providers/credentials`, DB-backed)
   - provider catalog for clients (`/ai/providers/catalog`)
   - `/ai/runs` cloud fallback path using configured credentials
   - first cloud adapter: `chocolatefactory` (`generateContent`)

2. Reliability Controls (Next)
   - quota governor in `/ai/runs` (RPM/TPM/RPD checks from config + DB counters)
   - context compactor (sliding window + rolling summary + pinned facts)
   - deterministic degradation policy under pressure (model downshift / summarise-first / temporary cooldown)
   - run-level policy trace metadata (why provider/model/context strategy was chosen)
   - status: in progress
     - baseline implemented:
       - DB-backed cloud usage events
       - RPM/TPM/RPD gate checks in cloud `/ai/runs` path
       - context compaction (tail window) before cloud call
       - policy trace persisted on run metadata
     - now implemented:
       - cooldown/error-state persistence for cloud provider+model pairs
       - `/ai/runs` cloud candidate selection rejects models with active cooldown
       - failed cloud attempts set backoff cooldown; successful attempts clear cooldown
       - dedicated `policy_trace_json` persistence on runs + structured `policy_trace` in run API payloads
       - per-thread compaction state persistence (`rolling_summary`, `pinned_facts_json`)
       - cloud context builder now uses rolling summary + pinned facts + tail-window
       - summariser-model-driven summary refresh (uses provider `role: compact` when context is large)
       - summary refresh thresholds/budgets now loaded from `cloudproviders.yaml` (`defaults.compaction.summary_refresh`)
       - cooldown backoff policy now loaded from `cloudproviders.yaml` (`defaults.cooldown`)
       - provider/model cooldown overrides now supported (`providers[].cooldown`, `providers[].models[].cooldown`)
      - still to do:
        - compact-model refresh policy tuning (cadence/heuristics beyond current thresholds)

3. Additional Providers (After Reliability)
   - implement adapters for `switchyard`, `chadjeopardy`, `mechatropic`
   - keep the same internal execution contract and policy engine
   - avoid schema redesign when adding providers

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
