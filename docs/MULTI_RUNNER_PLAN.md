# Multi-Runner Plan

## Summary

This plan adds support for multiple model runners in Holder, starting with manually configured Ollama-compatible runners and keeping the current local-machine runner as the default first runner.

The main user story is:

- Holder can talk to the local machine's Ollama if present.
- Holder can also talk to one or more remote runners on the local network.
- The user can name those runners, see their status, and pick models from them.
- Model selection remains simple enough to use in the current AI panel.

This is a cross-cutting feature. It touches:

- daemon runtime management
- AI status and capabilities APIs
- local model configuration persistence
- AI run routing
- Linux frontend AI panel UI and parsing

## Product Goals

- Support more than one Ollama-compatible runner.
- Keep local-machine usage simple and automatic.
- Allow a user to add a remote runner manually with name, host, and port.
- Show runner health and model inventory in the AI panel.
- Let local model dropdowns choose models from any configured runner.
- Preserve backward compatibility where practical during migration.

## Non-Goals For Phase 1

- Automatic LAN scanning or discovery.
- Arbitrary provider plugins beyond Ollama-compatible HTTP runners.
- Full runner authentication or TLS management.
- Cross-device synchronization of runner settings.
- Reworking cloud-provider routing in the same tranche.

## Current State

The current code assumes exactly one local runner:

- daemon owns one `LocalModelRunner`
- `/ai/status` reports one runner
- `/ai/capabilities` reports one runner and one flat list of models
- `/ai/local-models/config` stores only `fast_model`, `strong_model`, `deep_model` as plain strings
- AI run routing asks the one runner for available models and generation
- Linux frontend AI models also assume one runner and one flat model list

That means multi-runner support is not just a UI change. The data model is singular throughout the stack.

## Core Design

### Terminology

- `runner`: a configured local or remote model endpoint, such as an Ollama instance
- `runner_id`: stable internal identifier
- `runner_name`: user-facing short label, such as `office`
- `runner_endpoint`: base URL or host/port description for the runner
- `runner_model_ref`: a model reference qualified by runner identity

### Runner Shape

The system should evolve toward a structure like:

```text
Runner {
  runner_id
  name
  kind
  base_url
  source
  status
  models[]
}
```

Suggested fields:

- `runner_id`: stable ID, not derived from display name
- `name`: editable user-facing label such as `local` or `office`
- `kind`: initially `ollama`
- `base_url`: for example `http://192.168.1.23:11434`
- `source`: `auto_local` or `manual`
- `status`: reachable, unavailable, error, last checked, version
- `models[]`: runner-local model inventory

### Model Identity

The key decision is that model choice must become runner-qualified.

Current shape:

- `phi4-mini:latest`

Needed shape:

- internal: a structured reference such as `runner_id + model_name`
- display: a readable label such as `office:phi4-mini:latest`

Recommendation:

- store a structured model reference in daemon code and API payloads
- allow a compact serialized form for persistence
- use the runner's display name only for UI labels, not as the stored primary key

Example persisted value:

- `runner:<runner_id>:model:phi4-mini:latest`

Example UI label:

- `office:phi4-mini:latest`

This avoids breakage when a user renames `office` to something else later.

## UX Direction

### AI Panel Layout

The `Model Runners` section should evolve to show:

- explanatory text
- one row per configured runner
- status summary for each runner
- an add button
- later: edit, retry, remove actions for manual runners

Phase 1 expected interaction:

- local runner appears automatically if detected or configured as today
- user clicks `+`
- user enters:
  - runner name
  - host or base URL
  - port, defaulting to `11434` if host/port entry mode is used
- Holder saves the runner
- Holder probes the runner
- status and models appear once reachable

### Local Model Dropdowns

The local model dropdowns should present runner-qualified options.

Recommended display:

- `local:phi4-mini:latest`
- `office:qwen3:8b`

Potential refinement later:

- group options by runner in the UI if the dropdown gets too long

### Validation

Phase 1 should validate:

- runner name is non-empty
- runner name is unique among configured runners
- endpoint parses cleanly
- duplicate endpoint entries are rejected or warned on

## API Direction

The current singular routes should evolve without making the transition painful.

### New Or Expanded Routes

- `GET /ai/runners`
  - list configured runners, their status, and maybe lightweight metadata
- `POST /ai/runners`
  - add a manual runner
- `PUT /ai/runners/{runner_id}`
  - edit a manual runner
- `DELETE /ai/runners/{runner_id}`
  - remove a manual runner
- `POST /ai/runners/{runner_id}/retry`
  - re-probe a runner

### Existing Routes

`/ai/status`

Current:

- returns one runner summary and pull jobs

Proposed:

- return aggregate AI status plus a `runners[]` collection
- optional top-level summary fields may remain for compatibility for one transition period

`/ai/capabilities`

Current:

- returns one flat local model list and runner state

Proposed:

- return `runners[]`
- return an aggregate flattened model list only if needed for compatibility
- include structured model references suitable for dropdowns

`/ai/local-models/config`

Current:

- stores plain strings for `fast_model`, `strong_model`, `deep_model`

Proposed:

- store runner-qualified model references
- ideally return both:
  - structured fields for new clients
  - serialized string form for transition if needed

## Data Model And Persistence

### New Persistence Need

Runner configuration should become first-class persisted data.

Suggested stored fields:

- `runner_id`
- `name`
- `kind`
- `base_url`
- `source`
- `enabled`
- `created_at`
- `updated_at`

### Local Model Config Migration

Current local model config stores only model names.

That needs a migration path.

Recommended migration behavior:

- existing plain model names are interpreted as belonging to the default local runner if one exists
- once the user re-saves preferences, config is stored in the new runner-qualified form
- migration should not silently discard old choices

### Auto Local Runner

The current machine's runner should still exist as a special case:

- `source = auto_local`
- created implicitly by the daemon rather than by user action
- can be shown in the same runners list as manual entries

Important rule:

- do not special-case the local runner throughout the whole system forever
- special handling should mostly be limited to creation and default labeling

## Backend Architecture Direction

### Generalization

`LocalModelRunner` currently behaves like both:

- a runner implementation
- the entire runner system

That should be split conceptually into:

- `IRunnerClient` or equivalent interface for one runner endpoint
- runner registry or manager for many runners
- persistence repo for configured runners
- routing logic that resolves a runner-qualified model reference to one runner client

### Suggested Shape

- `RunnerClient`
  - probe status
  - list models
  - stream generate
  - start pull
  - list pull jobs

- `RunnerRegistry`
  - list configured runners
  - list active runner clients
  - resolve `runner_id`
  - refresh or retry a runner

- `RunnerRepo`
  - persist manual runners

### Pull Jobs

Current pull-job state is attached to the single `LocalModelRunner`.

With multiple runners, pull jobs should become runner-scoped.

That means:

- pull job IDs should include runner ownership logically
- status APIs should expose which runner owns each pull
- UI should display that association

## AI Run Routing

This is the most important behavioral change after config persistence.

### Current State

AI run routing assumes one local runner and chooses among its models.

### Proposed State

When a local model is selected, the selected config should resolve to:

- which runner to call
- which model name to ask that runner to generate with

That implies:

- title generation paths
- normal local inference paths
- nudge inference paths
- any runner pull/install paths

all need to resolve a runner-qualified model reference, not just a flat model string.

## Discovery

Automatic LAN discovery is useful but should not be part of the first implementation.

Reason:

- it increases complexity quickly
- it adds probing policy questions
- it adds UX questions about trust and duplicates
- manual add is enough for the first useful version

Later optional directions:

- manual `Scan local network` action
- probe common Ollama ports on RFC1918 ranges
- import discovered runners as suggestions rather than auto-adding them

## Compatibility Strategy

The plan should avoid breaking the current single-runner UI all at once.

Recommended compatibility approach:

1. Introduce runner collections in the daemon.
2. Keep old top-level singular response fields temporarily where needed.
3. Update Linux frontend to consume `runners[]`.
4. Migrate local model config to runner-qualified references.
5. Remove compatibility fields only after frontend migration is complete.

## Phases

## Phase 1: Planning And Data Model

- [ ] Define canonical `runner_id`
- [ ] Define runner-qualified model reference format
- [ ] Define persisted runner config schema
- [ ] Define API payload shape for `runners[]`
- [ ] Decide whether model config persistence stores structured JSON or compact strings

## Phase 2: Backend Runner Registry

- [ ] Introduce persisted manual runner records
- [ ] Represent the auto-local runner as a normal registry entry
- [ ] Build a runner registry or manager that exposes multiple runners
- [ ] Generalize current `LocalModelRunner` behavior into per-runner client logic
- [ ] Make status probing runner-specific

## Phase 3: API Evolution

- [ ] Add `GET /ai/runners`
- [ ] Add `POST /ai/runners`
- [ ] Add update and delete routes for manual runners
- [ ] Add runner-specific retry route
- [ ] Expand `/ai/status` to report multiple runners
- [ ] Expand `/ai/capabilities` to report multiple runners and runner-qualified models
- [ ] Update `/ai/local-models/config` to read and write runner-qualified model selections

## Phase 4: Frontend UI

- [ ] Add runner list UI under `Model Runners`
- [ ] Add `+` flow to create a runner
- [ ] Show runner status, endpoint, and version
- [ ] Add runner-qualified model labels in dropdowns
- [ ] Keep the current local-machine flow easy to understand

## Phase 5: AI Routing

- [ ] Resolve selected model config through `runner_id + model_name`
- [ ] Update local AI run paths to use the chosen runner
- [ ] Update title generation paths
- [ ] Update nudge paths that rely on local models
- [ ] Make pull jobs runner-aware

## Phase 6: Migration And Cleanup

- [ ] Migrate old local model config safely
- [ ] Preserve old single-runner compatibility long enough for frontend migration
- [ ] Remove obsolete singular assumptions from models and parsers
- [ ] Tighten logs and debug output to include runner identity

## Testing

### Backend

- [ ] Add tests for runner create, update, delete, and list
- [ ] Add tests for invalid endpoint handling
- [ ] Add tests for multi-runner status aggregation
- [ ] Add tests for runner-qualified model config persistence
- [ ] Add tests for AI run routing to the selected runner
- [ ] Add tests for migration from plain model names to runner-qualified refs

### Frontend

- [ ] Add tests for runner list rendering
- [ ] Add tests for add-runner validation
- [ ] Add tests for runner-qualified dropdown options
- [ ] Add tests for preserving selected model config across refreshes

### Integration

- [ ] Verify local auto-runner plus one manual remote runner works end-to-end
- [ ] Verify unavailable remote runner does not break local runner use
- [ ] Verify runner rename does not break saved model selections if references are ID-based
- [ ] Verify debug logs and activity messages identify the runner used

## Open Questions

- Should runner base URLs be stored as full URLs or host/port fields normalized into a URL?
- Should the first implementation allow only `http://` and local-network targets?
- Should duplicate models on different runners be shown as separate entries with labels only, or grouped visually?
- Should pull/install be supported only for runners that expose Ollama-compatible pull behavior?
- Should the auto-local runner be removable, hideable, or only disableable?

## Recommendations

- Start with manual runner addition only.
- Keep the auto-local runner as the default first runner.
- Use stable `runner_id` values for persistence.
- Use runner-qualified model references internally from the start.
- Keep display labels human-friendly, such as `office:model-name`.
- Delay network scanning until the manual flow is solid.
