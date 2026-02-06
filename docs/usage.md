# Usage

## Getting Started

### Initialize a Notes Directory

```bash
cd ~/Documents/notes
vimban init
```

This creates a `.vimban/` directory with sequence files and default configuration.

### Create Your First Ticket

```bash
# Interactive (opens in $EDITOR)
vimban create task "Set up development environment"

# Non-interactive (headless/automation)
vimban create task "Set up development environment" --no-edit

# With options
vimban create task "Fix login bug" -p high -a john_doe --due +3d
```

### Quick Creation with Bash Wrappers

Each wrapper provides sensible defaults:

```bash
# Task (medium priority, +7d due)
vimban_task "Implement user search"

# Bug (high priority, +7d due)
vimban_bug "Login fails with special characters"

# Epic
vimban_epic "User authentication system"

# Story
vimban_story "As a user I want to reset my password"

# Sub-task (link to parent with -m)
vimban_subtask "Write unit tests" -m PROJ-42

# Research
vimban_research "Evaluate caching strategies"
```

All wrappers support `--work` / `--personal` scope and `--no-edit` for non-interactive use.

## Listing Tickets

```bash
# List all tickets
vimban list

# Filter by status
vimban list -s in_progress
vimban list -s ready,blocked

# My tickets only
vimban list --mine

# Filter by assignee
vimban list --assignee john_doe

# Filter by priority
vimban list -p critical,high

# Multiple filters
vimban list -s in_progress -p high --mine

# Output formats
vimban list -f yaml
vimban list -f json
vimban list -f md
```

## Moving Tickets

```bash
# Change status
vimban move PROJ-42 in_progress
vimban move PROJ-42 done
vimban move BUG-5 ready

# Move with resolution
vimban move PROJ-42 done --resolve
```

## Viewing Details

```bash
# Show ticket details
vimban show PROJ-42

# Show in different formats
vimban show PROJ-42 -f yaml
vimban show PROJ-42 -f json
```

## Comments

```bash
# Add a comment
vimban comment PROJ-42 "Started working on this"

# Multi-line (opens $EDITOR)
vimban comment PROJ-42
```

## Editing Tickets

```bash
# Edit in $EDITOR
vimban edit PROJ-42

# Update specific fields
vimban edit PROJ-42 --priority critical
vimban edit PROJ-42 --assignee jane_doe
vimban edit PROJ-42 --progress 75
```

## Search

```bash
# Full-text search
vimban search "authentication"

# Search with filters
vimban search "bug" -s in_progress
```

## Dashboard

```bash
# Daily dashboard
vimban dashboard daily

# Weekly overview
vimban dashboard weekly

# Sprint view
vimban dashboard sprint
```

## Kanban Board

```bash
# ASCII kanban board
vimban kanban
```

## TUI (Terminal UI)

```bash
# Launch the TUI
vimban tui
# or
vimban_tui

# With options
vimban_tui --layout kanban
vimban_tui --view people
vimban_tui --done 30  # Show done tickets from last 30 days
```

### TUI Navigation

| Key | Action |
|-----|--------|
| `j`/`k` | Move up/down |
| `h`/`l` | Move left/right (kanban), switch panels (split) |
| `<Tab>` | Cycle views |
| `1-6` | Switch views directly |
| `L` | Cycle layouts (kanban/list/split) |
| `c` | Create ticket |
| `m` | Move ticket status |
| `e` | Edit ticket |
| `C` | Add comment |
| `/` | Search |
| `f` | Filter |
| `r` | Refresh |
| `q` | Quit |
| `?` | Help |

## Git Sync

```bash
# Commit and push changes
vimban commit

# Pull remote changes
vimban commit --pull

# Sync (pull then push)
vimban sync
```

## PARA Method Types

Beyond tickets, vimban supports PARA organizational types:

```bash
# Areas (ongoing responsibilities)
vimban_area "DevOps" --topic technical/devops

# Resources (reference material)
vimban_resource "Kubernetes Cheatsheet" --topic technical/k8s

# Meetings
vimban_meeting "Sprint Planning" --date 2026-02-06

# Journal entries
vimban_journal "Reflections on project progress"

# Recipes
vimban_recipe "Smash Burgers"

# Mentorship tracking
vimban_mentor "Weekly 1:1 with Junior Dev" --date 2026-02-06
```

## Web Server

Launch a full web UI in the browser:

```bash
# Via vimban
vimban --serve

# Or directly
vimban_serve

# With options
vimban_serve --port 8080
vimban_serve --bind 0.0.0.0 --port 5005
```

The web UI provides the same features as the TUI: kanban board, list view, dashboards, people view, ticket creation, status transitions, comments, and search.

Default port is 5005. Override via `server_port` in config.yaml or `--port` flag.

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `c` | Create new ticket |
| `r` | Refresh current view |
| `s` | Git sync |
| `1-4` | Switch views (Kanban, List, Dashboard, People) |
| `/` | Focus search |
| `Esc` | Close modals |

Requires `quart` and `quart-cors` Python packages.

### Token Authentication

The web server supports token-based authentication. Create a token file at `~/.config/vimban/token` with one token per line:

```
# Lines starting with # are comments
my-secret-token-123
another-valid-token
```

When the token file exists and contains valid tokens, all `/api/*` endpoints require a `Bearer` token in the `Authorization` header. The `/api/health` endpoint is always unauthenticated.

```bash
# Start with auth (auto-detected from token file)
vimban_serve

# Disable auth for testing
vimban_serve --no-token

# Test the API
curl -H "Authorization: Bearer my-secret-token-123" http://localhost:5005/api/tickets
```

### API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/health` | Health check (unauthenticated) |
| GET | `/api/tickets` | List tickets (supports query params) |
| POST | `/api/tickets` | Create a ticket |
| GET | `/api/ticket/<id>` | Get a single ticket |
| POST | `/api/ticket/<id>/move` | Move ticket to new status |
| POST | `/api/ticket/<id>/comment` | Add a comment |
| GET | `/api/ticket/<id>/comments` | Get ticket comments |
| POST | `/api/ticket/<id>/edit` | Edit ticket fields |
| POST | `/api/ticket/<id>/archive` | Archive a ticket |
| POST | `/api/ticket/<id>/link` | Link tickets |
| GET | `/api/dashboard/<type>` | Get dashboard data |
| GET | `/api/kanban` | Get kanban board as JSON |
| GET | `/api/people` | List people |
| GET | `/api/search?q=<query>` | Search tickets |
| GET | `/api/report` | Get report data |
| POST | `/api/sync` | Git sync |
| POST | `/api/commit` | Git commit (with optional pull) |

## Remote Access

Connect to a remote `vimban_serve` instance from any machine:

```bash
# Connect using a direct URL
vimban --remote http://192.168.1.10:5005 --api-token mytoken list

# Use a named remote from ~/.config/vimban/remote.yaml
vimban --remote config:work list
vimban --remote config:work show PROJ-00042

# Create a ticket on the remote
vimban --remote config:work create task "Fix the thing" -p high

# TUI connected to remote server
vimban_tui --remote config:work

# Skip auth for local testing
vimban --remote http://localhost:5005 --no-token list
```

Supported commands in remote mode: `list`, `show`, `create`, `move`, `archive`, `comment`, `comments`, `edit`, `search`, `dashboard`, `kanban`, `people`, `report`, `sync`, `commit`, `link`.

See `docs/configuration.md` for remote.yaml format and token file setup.

## MCP Server

Vimban can run as an MCP (Model Context Protocol) server for AI assistant integration:

```bash
vimban --mcp
# or
vimban --mcp-http
```

Requires the optional `mcp` Python package.

## Global Options

These flags work with any command:

| Flag | Description |
|------|-------------|
| `-d, --directory DIR` | Set working directory |
| `-f, --format FORMAT` | Output format: plain, md, yaml, json |
| `-q, --quiet` | Suppress non-essential output |
| `-v, --verbose` | Verbose output |
| `--no-color` | Disable colored output |
| `--work` | Force work scope |
| `--personal` | Force personal scope |
| `--archived` | Include archived tickets |
| `--remote URL` | Connect to remote vimban_serve (URL or `config:<name>`) |
| `--api-token TOKEN` | API token for remote server |
| `--no-token` | Skip auth when connecting to remote |
| `--version` | Show version |
| `--license` | Show license |
| `-h, --help` | Show help |
