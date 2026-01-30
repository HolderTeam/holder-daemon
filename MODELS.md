Here’s a **minimal v0.1** schema + class list that supports:

* projects
* notes (as files, DB for query)
* corkboard ordering
* AI prompt export + response capture (with provenance)
* full-text search over notes + AI responses (FTS5)
* resources (pointers only, no ingestion yet)

---

## SQLite tables (v0.1)

### 1) `projects`

Core project identity + where it lives on disk.

* `project_id TEXT PRIMARY KEY` (UUID)
* `name TEXT NOT NULL`
* `root_path TEXT NOT NULL` (absolute path)
* `created_at INTEGER NOT NULL`
* `updated_at INTEGER NOT NULL`

(Optionally later: `default_note_id`, `settings_json`, etc.)

---

### 2) `notes`

Metadata for notes (canonical content lives as files).

* `note_id TEXT PRIMARY KEY` (UUID)
* `project_id TEXT NOT NULL REFERENCES projects(project_id)`
* `title TEXT NOT NULL`
* `rel_path TEXT NOT NULL` (e.g. `notes/ab/cd/<uuid>.md`)
* `parent_note_id TEXT NULL REFERENCES notes(note_id)` (for folder-like structure)
* `sort_key REAL NOT NULL` (for manual ordering in lists/boards)
* `created_at INTEGER NOT NULL`
* `updated_at INTEGER NOT NULL`
* `deleted_at INTEGER NULL` (soft delete makes syncing/indexing easier)

Indexes:

* `(project_id, updated_at)`
* `(project_id, parent_note_id, sort_key)`

---

### 3) `note_links`

Explicit edges between notes (wiki links, references, relationships).

* `project_id TEXT NOT NULL`
* `from_note_id TEXT NOT NULL`
* `to_note_id TEXT NOT NULL`
* `kind TEXT NOT NULL` (e.g. `wiki`, `ref`, `parent`, `tag`)
* `label TEXT NULL`
* `created_at INTEGER NOT NULL`

Primary key (simple):

* `PRIMARY KEY(project_id, from_note_id, to_note_id, kind)`

(You can derive parent/child from `notes.parent_note_id` and still keep this for “real” links.)

---

### 4) `resources`

Project “assets/refs”: folders, files, URLs, repos.

* `resource_id TEXT PRIMARY KEY`
* `project_id TEXT NOT NULL`
* `kind TEXT NOT NULL` (`dir`, `file`, `repo`, `url`)
* `uri TEXT NOT NULL` (store as URI, e.g. `file:///…` or `https://…`)
* `label TEXT NOT NULL`
* `note TEXT NULL` (why it matters)
* `created_at INTEGER NOT NULL`
* `updated_at INTEGER NOT NULL`

(Leave ingestion for later.)

---

### 5) `ai_threads`

A thread is a container to group prompts/responses under a project (and optionally a note).

* `thread_id TEXT PRIMARY KEY`
* `project_id TEXT NOT NULL`
* `note_id TEXT NULL` (thread “attached” to a note)
* `title TEXT NOT NULL` (e.g. “Refactor plan”, default auto-title)
* `created_at INTEGER NOT NULL`
* `updated_at INTEGER NOT NULL`

---

### 6) `ai_messages`

Stores both prompts and responses (simple, ordered, searchable).

* `message_id TEXT PRIMARY KEY`
* `thread_id TEXT NOT NULL REFERENCES ai_threads(thread_id)`
* `role TEXT NOT NULL` (`user` / `assistant`)
* `source TEXT NOT NULL` (`local`, `manual_paste`, `other`)
* `provider TEXT NULL` (e.g. `ChatGPT`, `Claude`, `Gemini`, `Ollama`)
* `model TEXT NULL` (free text)
* `content TEXT NOT NULL` (markdown)
* `created_at INTEGER NOT NULL`

Optional:

* `prompt_hash TEXT NULL` (for de-dupe / “waiting for paste” workflow)
* `meta_json TEXT NULL` (future-proof for citations, tokens, etc.)

Index:

* `(thread_id, created_at)`

---

### 7) FTS (full-text search)

You want search across **note bodies** and **AI messages**.

Two straightforward FTS tables:

#### `notes_fts` (FTS5)

* `note_id UNINDEXED`
* `project_id UNINDEXED`
* `title`
* `body`

#### `ai_fts` (FTS5)

* `message_id UNINDEXED`
* `thread_id UNINDEXED`
* `project_id UNINDEXED`
* `content`

You populate these from:

* note file content (server reads file)
* ai_messages.content

This keeps searching dead simple in v0.1.

---

## Minimal classes (v0.1)

### Domain structs (no SQL inside)

* `Project { ProjectId id; string name; string root_path; ts created_at; ts updated_at; }`
* `Note { NoteId id; ProjectId project_id; string title; string rel_path; optional<NoteId> parent; double sort_key; ts created_at; ts updated_at; optional<ts> deleted_at; }`
* `NoteLink { ProjectId project_id; NoteId from; NoteId to; string kind; optional<string> label; ts created_at; }`
* `Resource { ResourceId id; ProjectId project_id; ResourceKind kind; string uri; string label; optional<string> note; ts created_at; ts updated_at; }`
* `AiThread { ThreadId id; ProjectId project_id; optional<NoteId> note_id; string title; ts created_at; ts updated_at; }`
* `AiMessage { MessageId id; ThreadId thread_id; Role role; Source source; optional<string> provider; optional<string> model; string content; ts created_at; optional<string> prompt_hash; }`

### Repository layer (SQL lives here)

* `ProjectRepo`

  * `create(name, root_path)`
  * `get(project_id)`
  * `list()`

* `NoteRepo`

  * `create(project_id, title, parent_id?)`
  * `get(note_id)`
  * `list(project_id, parent_id?)`
  * `update_title(note_id, title)`
  * `touch_updated(note_id)`
  * `soft_delete(note_id)`
  * `move(note_id, new_parent_id, new_sort_key)`

* `LinkRepo`

  * `upsert_links(project_id, from_note_id, vector<LinkSpec>)`
  * `list_outgoing(note_id)`
  * `list_backlinks(note_id)`
  * `delete_links_from(note_id)` (used during reconcile)

* `ResourceRepo`

  * `add(project_id, kind, uri, label)`
  * `list(project_id)`
  * `remove(resource_id)`

* `AiRepo` (or split into ThreadRepo + MessageRepo)

  * `create_thread(project_id, note_id?, title)`
  * `append_message(thread_id, role, source, provider, model, content, prompt_hash?)`
  * `list_threads(project_id)`
  * `list_messages(thread_id)`

* `SearchRepo`

  * `search_notes(project_id, query)`
  * `search_ai(project_id, query)`

### Services / use-cases (orchestrate multiple repos)

* `ProjectService`

  * create project folders (`notes/`, `.zet/`, git init)
  * open project
* `NoteService`

  * create note: write file + insert DB + index
  * update note: write file + update DB + reindex + optionally reconcile links
* `LinkReconciler`

  * parse markdown (front matter + body)
  * extract `[[wiki]]` / `[md](link)` etc.
  * update `note_links`
* `AiCaptureService`

  * export prompt → create message with `prompt_hash`
  * capture pasted response → append assistant message linked to last prompt
* `IndexService`

  * update notes_fts
  * update ai_fts
* `GitService` (libgit2)

  * init repo
  * stage & commit changed note files (optional in v0.1)

