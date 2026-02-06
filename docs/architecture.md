# Architecture

Vimban is a markdown-native ticket and knowledge management system built around YAML frontmatter and the PARA method. All data is stored as plain markdown files in a git repository.

## Design Principles

1. **Markdown-native** - Every ticket, area, resource, and journal entry is a standard `.md` file with YAML frontmatter
2. **Git-powered** - All persistence and sync is through git, enabling collaboration, history, and offline work
3. **CLI-first** - Full functionality from the command line; Neovim and TUI are optional interfaces
4. **PARA integration** - Ticket storage follows the PARA method (Projects, Areas, Resources, Archives)
5. **No database** - The filesystem _is_ the database; frontmatter _is_ the schema

## File Format

Every vimban-managed file follows this structure:

```markdown
---
id: PROJ-00042
title: "Fix authentication bug"
type: task
status: in_progress
created: 2026-02-01
updated: 2026-02-03
version: 1

assignee: john_doe
reporter: jane_doe
watchers: []
priority: high
effort:
tags: [auth, security]
project: myapp

due_date: 2026-02-10
start_date: 2026-02-01
end_date:

member_of: PROJ-00038
relates_to: []
blocked_by: []
blocks: []

progress: 50
issue_link:
---

# Fix authentication bug

## Description

...

## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
```

### Frontmatter Fields

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique identifier (e.g., `PROJ-00042`) |
| `title` | string | Human-readable title |
| `type` | string | Item type (see [Types](#type-system)) |
| `status` | string | Current workflow state |
| `created` | date | Creation date |
| `updated` | date | Last modification date |
| `version` | int | Schema version for future migrations |
| `assignee` | string | Who is responsible |
| `reporter` | string | Who created/reported it |
| `watchers` | list | People tracking this item |
| `priority` | string | `critical`, `high`, `medium`, `low` |
| `effort` | string | Estimated effort (free-form) |
| `tags` | list | Labels for categorization |
| `project` | string | Project grouping |
| `due_date` | date | When it's due |
| `start_date` | date | When work began |
| `end_date` | date | When work completed |
| `member_of` | string | Parent ticket ID |
| `relates_to` | list | Related ticket IDs |
| `blocked_by` | list | Blocking ticket IDs |
| `blocks` | list | Tickets this blocks |
| `progress` | int | 0-100 completion percentage |
| `issue_link` | string | External issue tracker URL |

Not all fields appear in every type. Simpler types like `area` and `resource` only have `id`, `title`, `type`, `status`, `created`, `updated`, `version`, and `tags`.

## ID System

Each type category has its own ID prefix and sequence counter stored in `.vimban/`:

| Type | Prefix | Sequence File | Example |
|------|--------|---------------|---------|
| epic, story, task, sub-task | `PROJ` | `.sequence` | `PROJ-00042` |
| bug | `BUG` | `.sequence_bug` | `BUG-00007` |
| research | `RESEARCH` | `.sequence_research` | `RESEARCH-00013` |
| area | `AREA` | `.sequence_area` | `AREA-00005` |
| resource | `RESOURCE` | `.sequence_resource` | `RESOURCE-00021` |
| meeting | `MTG` | `.sequence_meeting` | `MTG-00089` |
| mentorship | `MNTR` | `.sequence_mentorship` | `MNTR-00003` |
| journal | `JNL` | `.sequence_journal` | `JNL-00145` |
| recipe | `RCP` | `.sequence_recipe` | `RCP-00012` |
| person | `PERSON` | `.sequence_person` | `PERSON-00008` |

IDs are zero-padded to 5 digits. The `PROJ` prefix is configurable via `default_prefix` in config.

### Sequence Files

Sequence files are plain text files containing the next ID number. They use file locking (`fcntl`) to prevent race conditions during concurrent creation.

## Type System

Vimban has three categories of types:

### 1. Ticket Types

Standard work items with full workflow support:

- **epic** - Large initiative containing stories
- **story** - User-facing feature or deliverable
- **task** - Concrete unit of work
- **sub-task** - Child of a task or story
- **research** - Investigation or analysis
- **bug** - Defect report

### 2. PARA Types

Organizational types following the PARA method:

- **area** - Ongoing responsibility (e.g., "DevOps", "Health")
- **resource** - Reference material (e.g., "Kubernetes Cheatsheet")

### 3. Specialized Types

Types with custom behaviors (date-based filenames, special directories):

- **meeting** - Meeting notes with date, attendees, action items
- **journal** - Personal journal entries with mood/energy tracking
- **recipe** - Recipes with ingredients, prep/cook time
- **mentorship** - 1:1 mentorship session tracking
- **person** - People profiles with 1:1 notes and dashboards

## Status Workflow

```
backlog --> ready --> in_progress --> review --> done
                         |                       |
                         v                       v
                      blocked               cancelled
                         |
                         v
                     delegated
```

All transitions are permitted (any status can move to any other status). This is intentional to support real-world workflows where tickets sometimes need to move backwards.

### Statuses

| Status | Description |
|--------|-------------|
| `backlog` | Not yet planned |
| `ready` | Planned and ready to start |
| `in_progress` | Actively being worked |
| `blocked` | Waiting on external dependency |
| `review` | Work done, awaiting review |
| `delegated` | Handed off to someone else |
| `done` | Completed |
| `cancelled` | Won't be done |

## Directory Structure

Vimban operates on a PARA-organized notes directory:

```
~/Documents/notes/
├── .vimban/
│   ├── config.yaml          # Per-project config
│   ├── .sequence             # PROJ ID counter
│   ├── .sequence_bug         # BUG ID counter
│   ├── .sequence_research    # RESEARCH ID counter
│   ├── .sequence_area        # AREA ID counter
│   ├── .sequence_resource    # RESOURCE ID counter
│   ├── .sequence_meeting     # MTG ID counter
│   ├── .sequence_mentorship  # MNTR ID counter
│   ├── .sequence_journal     # JNL ID counter
│   ├── .sequence_recipe      # RCP ID counter
│   └── .sequence_person      # PERSON ID counter
├── 00_inbox/                 # Inbox items
├── 01_projects/
│   ├── work/                 # Work scope tickets
│   │   └── *.md
│   └── personal/             # Personal scope tickets
│       └── *.md
├── 02_areas/
│   ├── work/
│   │   ├── people/           # Person files
│   │   │   └── *.md
│   │   └── meetings/         # Meeting notes
│   │       └── *.md
│   └── personal/
│       └── journal/          # Journal entries
│           └── *.md
├── 03_resources/             # Reference material
│   └── *.md
└── 04_archives/              # Archived items
    └── *.md
```

## Comment Threading

Comments are stored inline in the markdown body between sentinel markers:

```markdown
<!-- VIMBAN:COMMENTS:START -->

### Comment by john_doe (2026-02-03 14:30)

This is looking good, just needs tests.

### Comment by jane_doe (2026-02-03 15:00)

Added unit tests, ready for review.

<!-- VIMBAN:COMMENTS:END -->
```

This keeps comments co-located with the ticket content and preserves full history in git.

## Section Markers

Templates use HTML comment markers for structured sections that vimban can programmatically update:

| Marker | Used In | Purpose |
|--------|---------|---------|
| `VIMBAN:COMMENTS:START/END` | All types | Comment thread |
| `VIMBAN:STORIES:START/END` | Epic | Child stories list |
| `VIMBAN:TASKS:START/END` | Story | Child tasks list |
| `VIMBAN:1ON1:START/END` | Person | 1:1 meeting notes |
| `VIMBAN:DASHBOARD:START/END` | Person | Personal dashboard |

## Relationship Model

Tickets can be linked via frontmatter fields:

- **`member_of`** - Parent relationship (task belongs to story, story belongs to epic)
- **`relates_to`** - Non-hierarchical reference
- **`blocked_by`** - This ticket cannot proceed until the listed tickets are done
- **`blocks`** - Listed tickets cannot proceed until this one is done

Transclusion links (`![[path/to/note.md]]`) provide cross-file references that the Neovim plugin can expand inline.

## MCP Server

Vimban optionally exposes its API as an MCP (Model Context Protocol) server, allowing AI assistants to create, query, and update tickets programmatically. This requires the `mcp` Python package.

```bash
# stdio mode (for claude desktop, etc.)
vimban --mcp

# HTTP mode
vimban --mcp-http
```

## Web API & Authentication

`vimban_serve` provides a full REST API at `/api/*` endpoints. All API routes return JSON.

### Token-Based Auth

Authentication uses Bearer tokens validated against `~/.config/vimban/token`:

```
Request flow:

  Client                          vimban_serve
    |                                  |
    |  GET /api/tickets                |
    |  Authorization: Bearer <token>   |
    |--------------------------------->|
    |                                  |  load tokens from ~/.config/vimban/token
    |                                  |  validate Bearer token against set
    |       200 OK (JSON)              |
    |<---------------------------------|
```

**Token file format:**
- One token per line
- Lines starting with `#` are comments
- Blank/whitespace-only lines are ignored
- Tokens are reloaded on each request (no restart needed)

**Auth bypass:**
- `GET /` (HTML UI page) — always allowed
- `GET /api/health` — always allowed (health check)
- `--no-token` flag — disables all auth validation
- If no token file exists or it's empty — auth is disabled

### Remote Client Architecture

When `--remote` is passed to `vimban`, `vimban_tui`, or `vimban_serve`, commands are routed through HTTP instead of the local filesystem:

```
  vimban --remote config:work list
    |
    |  1. resolve "config:work" from ~/.config/vimban/remote.yaml
    |     -> url: http://192.168.1.10:5005, token: abc123
    |
    |  2. map "list" command to GET /api/tickets
    |
    |  3. send HTTP request with Bearer token
    |     -> GET http://192.168.1.10:5005/api/tickets
    |
    |  4. format JSON response per --format flag
```

The remote client uses `urllib.request` (stdlib only) — no extra dependencies needed on the client side.

### Remote Config Resolution

The `--remote` flag accepts two formats:

1. **Direct URL**: `http://host:port` — used as-is
2. **Config reference**: `config:<name>` — resolved from `~/.config/vimban/remote.yaml`

Token precedence: `--api-token` flag > `remote.yaml` api_token > no token (`--no-token`)
