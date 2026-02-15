# TODO: `src/api/Session.cpp` Refactor

Goal: split a large mixed-responsibility file into a clear domain + layer hierarchy that is easier for humans to navigate and change safely.

## Target Architecture

Use both dimensions:

1. Domain split
- `projects`
- `cards`
- `resources`
- `search`
- `ai/providers`
- `ai/runs`
- `ai/messages_threads`
- `trash`
- `static_docs`

2. Layer split
- `transport`: socket/session read/write, auth gate, request lifecycle
- `routes`: path/method matching, query/body validation, response mapping
- `services`: business logic and orchestration
- `support`: shared helpers (cloud config/client/quota, model routing, run events)
- `store`: existing repos remain DB access layer

## Proposed Directory Layout

```text
src/api/
  Session.cpp                  # thin transport + dispatch orchestration
  Session.h
  Router.cpp
  Router.h
  routes/
    StaticRoutes.cpp
    ProjectRoutes.cpp
    CardRoutes.cpp
    ResourceRoutes.cpp
    SearchRoutes.cpp
    AiStatusRoutes.cpp
    AiProviderRoutes.cpp
    AiRunRoutes.cpp
    AiThreadMessageRoutes.cpp
    TrashRoutes.cpp
  services/
    AiRunService.cpp
    AiProviderService.cpp
    AiMessageThreadService.cpp
  support/
    PathDiscovery.cpp
    RunEventStore.cpp
    LocalModelRouting.cpp
    CloudConfig.cpp
    CloudClient.cpp
    CloudQuota.cpp
```

## TODO Checklist

1. Create `SessionContext` type for shared dependencies and request utilities.
2. Extract static/docs path resolution and file serving helpers into `support/PathDiscovery.cpp`.
3. Extract run event global state and helpers into `support/RunEventStore.cpp`.
4. Extract local model routing/caste helpers into `support/LocalModelRouting.cpp`.
5. Extract cloud provider YAML parsing and typed config into `support/CloudConfig.cpp`.
6. Extract Beast HTTPS calls into `support/CloudClient.cpp`.
7. Extract quota usage queries/checks into `support/CloudQuota.cpp`.
8. Remove duplicate helper implementations while extracting (for example `truncate_bytes`).
9. Add thin route module for static endpoints (`/openapi.yaml`, `/models.yaml`, `/cloudproviders.yaml`, `/docs`).
10. Add thin route module for project endpoints.
11. Add thin route module for card endpoints.
12. Add thin route module for resource endpoints.
13. Add thin route module for search endpoints.
14. Add thin route module for AI status/router/runner pull endpoints.
15. Add thin route module for AI provider endpoints.
16. Add thin route module for AI run endpoints (local + cloud paths).
17. Add thin route module for AI thread/message endpoints.
18. Add thin route module for trash endpoints.
19. Make `Session::run()` only do read, auth, dispatch chain, write, and log timing.
20. Keep route dispatch order explicit and documented in one place.
21. Add/refactor tests so each route module has at least one happy-path and one not-handled case.
22. Add regression tests for highest-risk flows:
- `POST /ai/runs` local execution path
- `POST /ai/runs` cloud fallback path
- `/ai/providers/catalog`
- `/ai/providers/credentials` CRUD
23. Document coding convention for new route/service files (single responsibility, no hidden globals).
24. Enforce max file size target for route/service files (recommended: <= 500 lines each).

## Migration Strategy

Use incremental PRs:

1. PR A: helper extraction only (`support/*`) with zero behavior change.
2. PR B: static + projects + resources routes.
3. PR C: cards + search + trash routes.
4. PR D: AI provider + AI status routes.
5. PR E: AI runs + AI messages/threads routes.
6. PR F: final Session slimming + cleanup + docs.

Each PR should:
- keep behavior stable
- keep tests green
- avoid schema/API contract changes unless explicitly planned

