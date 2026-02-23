# Architecture Split: holder (Backend Authority)

## Purpose

`holder` is the backend/API authority for Holder.

This repo owns:
- Backend runtime and storage behavior
- `openapi.yaml` as the canonical API contract
- Contract/conformance tests for API endpoints and semantics
- Versioning and release policy for backend compatibility

This repo does **not** own desktop UI behavior or cross-client UX parity tests.

## Repository Boundaries

Backend-owned concerns:
- API routes, request/response formats, auth behavior
- Data model migrations and persistence semantics
- AI backend orchestration and server-side policies
- Docs for API consumers (`openapi.yaml`, endpoint docs)

Out of scope (owned by `holder-desktop`):
- GTK/WinUI/SwiftUI implementation details
- Product UX parity workflows
- Desktop automation harnesses (dogtail/XCTest/UIA)

## Contract Rules

1. `openapi.yaml` is canonical.
2. Any API behavior change must update `openapi.yaml` in same PR.
3. Contract tests must pass before merge.
4. Breaking changes require a version bump and a migration note.

## Versioning Policy

- Server API version is exposed by `/health` (`api_version`).
- Use semantic intent:
  - Patch: bugfix, no contract break
  - Minor: additive API changes
  - Major: breaking contract changes

When introducing a breaking change:
- Update `api_version` accordingly.
- Add explicit migration notes for desktop clients.
- Keep old behavior behind a temporary compatibility path only if needed for release overlap.

## Compatibility Policy With `holder-desktop`

`holder` publishes release artifacts (or stable build references) consumed by `holder-desktop` CI.

`holder-desktop` pins backend versions and is expected to support a declared range.

Recommended rule:
- Desktop release notes declare a supported backend range, e.g. `>=0.1.4 <0.2.0`.

## CI Expectations (Backend)

Required checks in this repo:
- Unit/integration test suite passes
- Contract tests pass against `openapi.yaml`
- OpenAPI lint/validation passes
- Migration safety checks pass (if schema changes)

Optional but recommended:
- Publish machine-readable artifact metadata (version, commit, build date)

## Release Handoff to `holder-desktop`

For each backend release:
1. Tag release and publish artifacts.
2. Ensure `openapi.yaml` is included and versioned.
3. Provide changelog section:
   - Added endpoints/fields
   - Deprecated behaviors
   - Breaking changes and migration notes

## Change Management

For changes that may impact desktop UX semantics:
- Open a companion issue in `holder-desktop`
- Link planned API changes early (before merge)
- Mark as `desktop-impacting` in PR labels
