# Card Server — Purpose & Responsibilities

## Overview

The card server is a **local-first backend service** responsible for all *state, logic, and intelligence* in the system.

It acts as the **single source of truth** for:

* projects
* notes/cards
* links between notes
* AI interactions
* search and indexing
* git-backed storage
* local AI model execution

The server is designed to be **headless, reusable, and UI-agnostic**.

It must be usable by:

* the GTK desktop frontend
* future TUIs
* editor plugins
* scripts
* automated agents

---

## Core Responsibilities

### 1. Data Ownership

The server owns all persistent data.

This includes:

* projects
* notes/cards
* links (note ↔ note, note ↔ resource)
* AI prompts and responses
* metadata and provenance

Clients must never manipulate storage directly.

---

### 2. Project Model

A **Project** is the primary unit of work.

A project may contain:

* notes/cards
* hierarchical structure (via links, not folders)
* references to external resources
* AI interaction history

Projects are backed by a **git repository** for:

* history
* syncing
* interoperability with external tools

The server is responsible for keeping git and internal indexes consistent.

---

### 3. Cards

Cards (notes, slips, ideas) are **atomic text units**.

Characteristics:

* uniquely identified (UUID)
* stored as markdown/plain text
* linkable in arbitrary graphs (parent/child, reference, association)
* mutable, autosaved, versioned via git

The server does **not** enforce semantics such as “folder vs leaf” — structure is emergent.

---

### 4. AI Interaction as First-Class Data

AI is treated as **recorded reasoning**, not ephemeral chat.

The server stores:

* prompts
* responses
* source (local model, cloud provider, manual paste)
* timestamps
* associated project/note
* optional model identifiers

This allows:

* revisiting past AI advice
* comparing answers
* searching across AI history
* building on previous reasoning

---

### 5. Local AI Execution

The server may interface with **local AI runtimes** (initially via Ollama).

Responsibilities:

* model discovery
* routing requests to appropriate models
* streaming responses
* managing resource usage (within reason)

The server does not assume:

* high-end GPUs
* cloud connectivity
* specific models being present

Clients request *capabilities*, not models (“code help”, “general reasoning”, etc.).

---

### 6. Cloud AI (Indirect)

The server does **not** directly integrate with hostile or unstable cloud APIs by default.

Instead, it supports:

* prompt export
* response capture
* provenance tracking

This keeps the system:

* ToS-compliant
* resilient to API churn
* usable without paid subscriptions

---

### 7. Resources & Context

Projects may reference **external resources**:

* local directories
* files
* git repositories
* URLs

The server:

* stores these references
* optionally ingests/indexes them (opt-in)
* uses them as context for search and AI

No automatic ingestion or exfiltration is permitted.

---

### 8. Indexing & Search

The server provides:

* full-text search across notes and AI responses
* fast retrieval for UI queries
* incremental reindexing

Implementation:

* SQLite + FTS5 (initially)
* semantic/vector search is optional and future-facing

Search is always local.

---

### 9. API Surface

The server exposes a **stable, documented API** (HTTP or local socket).

Clients can:

* list projects
* read/write notes
* manage links
* submit AI jobs
* query search
* attach metadata

The API must remain:

* simple
* predictable
* backward-compatible

---

## Design Principles

* **Local-first**: nothing assumes the network
* **Single source of truth**: no client-side shadow logic
* **Explicit over implicit**: no “magic”
* **Composable**: works with many frontends
* **Auditable**: data is inspectable, git-backed
* **Boring tech**: reliability over novelty

---

## Non-Goals

* No UI logic
* No theming or presentation concerns
* No hard dependency on specific AI vendors
* No attempt to be a distributed system
* No premature optimisation for massive scale

---

## Long-Term Direction (Informative)

* Multiple concurrent clients
* Background jobs (indexing, ingestion)
* Optional embeddings / vector search
* Agent-like workflows built on stored notes + AI history
* Portable project archives

---

## Mental Model

> The card server is **where thinking lives**.
> Clients are just windows into it.
