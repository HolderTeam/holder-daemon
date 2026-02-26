# Privacy Recovery Scenarios and `.hrk` Shape

This document defines the restore flow and token shape for practical recovery
with minimal user decisions.

## Goals

- Make restore work even when the user has no local project selected.
- Keep project metadata private by default.
- Minimize manual "project matching" work.

## Scenario 1: Device Lost/Stolen, `.hrk` + Git Backup

Target flow:

1. User opens Recovery Key tool and chooses `.hrk` file.
2. User enters PIN.
3. Holder decrypts wrapped metadata from `.hrk`.
4. Holder reads `project_id` from decrypted metadata.
5. If local project with that `project_id` exists, Holder reuses it.
6. Otherwise Holder creates a new local project with that `project_id`.
7. If decrypted metadata includes `git_remote_url`, Holder sets remote and tries pull.
8. If pull fails (for example stale URL), user updates remote in Git config UI and retries.

Notes:
- User does not need to preselect a project.
- `project_name` is non-authoritative; name collisions are acceptable.
- Two projects named `Home` are valid if they have different `project_id`s.

## Derived Requirements

Must have:
- Token format/version marker.
- PIN-wrapped payload containing:
  - `project_id`
  - project key material
  - KDF/cipher metadata needed to unwrap

Should have in wrapped payload:
- `project_name` (non-authoritative hint)
- `git_remote_url` (best-effort hint)
- `default_branch` (optional hint)
- `exported_at`

## Proposed `.hrk` Shape

Design decision:
- Do not keep project metadata in cleartext.
- Keep only minimum format/version information clear.
- Put project metadata inside the PIN-wrapped payload.

Conceptual structure:

```json
{
  "version": 3,
  "kdf": {
    "name": "PBKDF2-HMAC-SHA256",
    "iterations": 210000,
    "salt_b64": "..."
  },
  "cipher": {
    "name": "holder-privacy-envelope-v1",
    "wrapped": "{...encrypted payload...}"
  }
}
```

Wrapped payload contents (decrypted after PIN):
- `project_id` (authoritative)
- `project_name` (hint only)
- `project_key` / key material
- `git_remote_url` (hint only)
- `default_branch` (optional)
- `exported_at` (optional)

## Trust Model

- `project_id` is authoritative identity.
- `project_name` and `git_remote_url` are hints only.
- Remote hints can be wrong/outdated; user must be able to edit and retry.

## Suggested UI Flow

1. User chooses `.hrk`.
2. User enters PIN.
3. Holder decrypts metadata.
4. Holder auto-matches or auto-creates local project by `project_id`.
5. Holder applies remote hint and attempts sync.
6. On sync failure, user edits remote and retries.
7. Holder rebuilds and project opens.

## Open Questions

- Do we keep a separate strict mode, or is wrapped-only metadata always the default?
- Do we support multiple remote hints in one token?
