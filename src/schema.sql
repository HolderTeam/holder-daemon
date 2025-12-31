-- schema.sql (v0.1)
-- Local-first card-server schema: projects, notes, links, resources, AI threads/messages, and FTS5.
-- The app/server is responsible for keeping FTS tables in sync (no triggers in v0.1).

PRAGMA foreign_keys = ON;

-- ----------------------------
-- Projects
-- ----------------------------
CREATE TABLE IF NOT EXISTS projects (
  project_id  TEXT PRIMARY KEY,          -- UUID
  name        TEXT NOT NULL,
  root_path   TEXT NOT NULL,             -- absolute path on disk
  created_at  INTEGER NOT NULL,          -- unix epoch seconds (or ms, but be consistent)
  updated_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_projects_updated
  ON projects(updated_at);

-- ----------------------------
-- Notes
-- ----------------------------
CREATE TABLE IF NOT EXISTS notes (
  note_id        TEXT PRIMARY KEY,       -- UUID
  project_id     TEXT NOT NULL,
  title          TEXT NOT NULL,
  rel_path       TEXT NOT NULL,          -- path relative to project root (e.g. notes/ab/cd/<uuid>.md)
  parent_note_id TEXT NULL,              -- folder-like nesting (optional)
  sort_key       REAL NOT NULL DEFAULT 0.0, -- manual ordering within a parent scope
  created_at     INTEGER NOT NULL,
  updated_at     INTEGER NOT NULL,
  deleted_at     INTEGER NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(parent_note_id) REFERENCES notes(note_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_notes_project_updated
  ON notes(project_id, updated_at);

CREATE INDEX IF NOT EXISTS idx_notes_project_parent_sort
  ON notes(project_id, parent_note_id, sort_key);

CREATE INDEX IF NOT EXISTS idx_notes_project_title
  ON notes(project_id, title);

-- Ensure rel_path uniqueness within a project (so file mapping stays sane)
CREATE UNIQUE INDEX IF NOT EXISTS uq_notes_project_relpath
  ON notes(project_id, rel_path);

-- ----------------------------
-- Note links
-- ----------------------------
-- Stores explicit directed edges between notes. Used for:
-- - wiki-style links
-- - backlinks
-- - arbitrary relationships
CREATE TABLE IF NOT EXISTS note_links (
  project_id    TEXT NOT NULL,
  from_note_id  TEXT NOT NULL,
  to_note_id    TEXT NOT NULL,
  kind          TEXT NOT NULL,           -- e.g. 'wiki', 'ref', 'tag', 'related'
  label         TEXT NULL,
  created_at    INTEGER NOT NULL,

  FOREIGN KEY(project_id)   REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(from_note_id) REFERENCES notes(note_id)       ON DELETE CASCADE,
  FOREIGN KEY(to_note_id)   REFERENCES notes(note_id)       ON DELETE CASCADE,

  PRIMARY KEY(project_id, from_note_id, to_note_id, kind)
);

CREATE INDEX IF NOT EXISTS idx_note_links_from
  ON note_links(project_id, from_note_id);

CREATE INDEX IF NOT EXISTS idx_note_links_to
  ON note_links(project_id, to_note_id);

-- ----------------------------
-- Project resources (pointers only in v0.1)
-- ----------------------------
CREATE TABLE IF NOT EXISTS resources (
  resource_id  TEXT PRIMARY KEY,         -- UUID
  project_id   TEXT NOT NULL,
  kind         TEXT NOT NULL,            -- 'dir', 'file', 'repo', 'url'
  uri          TEXT NOT NULL,            -- store as URI: file:///..., https://..., git+ssh://...
  label        TEXT NOT NULL,
  note         TEXT NULL,                -- human description (why it matters)
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_resources_project
  ON resources(project_id);

CREATE INDEX IF NOT EXISTS idx_resources_project_kind
  ON resources(project_id, kind);

-- ----------------------------
-- AI threads + messages
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_threads (
  thread_id   TEXT PRIMARY KEY,          -- UUID
  project_id  TEXT NOT NULL,
  note_id     TEXT NULL,                 -- optional attachment to a note/card
  title       TEXT NOT NULL,
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(note_id)    REFERENCES notes(note_id)       ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_ai_threads_project_updated
  ON ai_threads(project_id, updated_at);

CREATE INDEX IF NOT EXISTS idx_ai_threads_note
  ON ai_threads(note_id);

CREATE TABLE IF NOT EXISTS ai_messages (
  message_id  TEXT PRIMARY KEY,          -- UUID
  thread_id   TEXT NOT NULL,
  role        TEXT NOT NULL,             -- 'user' | 'assistant'
  source      TEXT NOT NULL,             -- 'local' | 'manual_paste' | 'other'
  provider    TEXT NULL,                 -- e.g. 'Ollama', 'ChatGPT', 'Claude', 'Gemini', 'Perplexity'
  model       TEXT NULL,                 -- free-text model id/name
  content     TEXT NOT NULL,             -- markdown
  created_at  INTEGER NOT NULL,
  prompt_hash TEXT NULL,                 -- used to correlate "export prompt" -> "paste response"
  meta_json   TEXT NULL,                 -- future-proof (citations, token counts, etc.)

  FOREIGN KEY(thread_id) REFERENCES ai_threads(thread_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ai_messages_thread_time
  ON ai_messages(thread_id, created_at);

CREATE INDEX IF NOT EXISTS idx_ai_messages_prompt_hash
  ON ai_messages(prompt_hash);

-- ----------------------------
-- Full-text search (FTS5)
-- ----------------------------
-- Contentless FTS: server maintains rows explicitly.
-- Store IDs as UNINDEXED columns for join-back.
--
-- Notes: index title + body
CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5(
  note_id    UNINDEXED,
  project_id UNINDEXED,
  title,
  body,
  tokenize = 'unicode61'
);

CREATE INDEX IF NOT EXISTS idx_notes_fts_project
  ON notes_fts(project_id);

-- AI messages: index content
-- We include project_id for easy scoping without extra joins.
CREATE VIRTUAL TABLE IF NOT EXISTS ai_fts USING fts5(
  message_id UNINDEXED,
  thread_id  UNINDEXED,
  project_id UNINDEXED,
  content,
  tokenize = 'unicode61'
);

CREATE INDEX IF NOT EXISTS idx_ai_fts_project
  ON ai_fts(project_id);

-- ----------------------------
-- A tiny schema version table (handy for migrations)
-- ----------------------------
CREATE TABLE IF NOT EXISTS schema_version (
  version INTEGER NOT NULL
);

-- Initialize schema version to 1 if empty
INSERT INTO schema_version(version)
SELECT 1
WHERE NOT EXISTS (SELECT 1 FROM schema_version);
