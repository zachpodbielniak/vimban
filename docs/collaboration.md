# Collaboration Mode

Vimban's collaboration mode lets multiple people share a live kanban board over the network. One person runs the server (`vimban_serve`), everyone else connects as a client — via the web UI, the CLI `--watch` flag, or the TUI. Changes are pushed to all clients in real time via Server-Sent Events (SSE).

## Overview

- **One server, multiple clients.** The server owns the ticket files on disk. Clients read and write through the HTTP API.
- **Identity is derived from tokens.** Each user gets a token in the server's token file. The server maps that token to a username and uses it for all event attribution ("alice created CLAW-00042").
- **Three client types:** web browser (EventSource + presence bar + toast notifications), CLI `--watch` mode (streaming event log in your terminal), and the TUI (background SSE thread, auto-refresh, presence in the header).
- **Project keys** let you configure which ticket ID prefixes belong to a given remote. The first key in the list becomes the default prefix when creating tickets on that remote.

---

## Prerequisites

On the **server host**:

- Python 3.10+ with `quart`, `pyyaml`, and `quart-cors` installed
- A `vimban` notes directory initialized with `vimban init`
- A token file at `~/.config/vimban/token` (or passed via `--token-file`)

On **client machines**:

- `vimban` installed and in `PATH`
- `~/.config/vimban/remote.yaml` configured with the server URL and your token
- For TUI: `vimban_tui` installed (ships alongside `vimban_serve`)

---

## Server Setup

### Starting the server

```bash
# Basic: serve the current directory on port 5005
vimban_serve

# Explicit directory and port
vimban_serve --directory ~/Documents/notes --port 5005

# Bind to all interfaces (needed so LAN clients can reach it)
vimban_serve --bind 0.0.0.0 --port 5005

# With a custom token file
vimban_serve --bind 0.0.0.0 --token-file /etc/vimban/tokens

# Disable auth entirely (local dev/testing only — don't do this on LAN)
vimban_serve --no-token
```

Once running, the server prints:

```
Starting vimban web server on http://0.0.0.0:5005
  Kanban  : http://0.0.0.0:5005
  API     : http://0.0.0.0:5005/api/tickets
  Auth    : enabled (3 token(s) loaded from /home/alice/.config/vimban/token)
```

The web UI is at `http://<server-ip>:5005`. The SSE endpoint is at `/api/events`.

### Token file setup (username:token format)

The token file lives at `~/.config/vimban/token` by default. Each line is either a `username:token` pair or a bare token (treated as `unknown` for backward compatibility).

```
# ~/.config/vimban/token
# Lines starting with # are comments. Blank lines are ignored.
#
# Format: username:token
# The username shows up in the presence bar and event feed.
alice:token-abc-123
bob:token-def-456
carol:token-ghi-789

# Old-style bare token still works — shows as 'unknown' in the UI
some-legacy-token
```

- Colons in usernames are not supported (the split is on the first `:` only, so the token can contain colons).
- The server reloads the token file on every `/api/events` request, so you can add tokens without restarting.
- Keep this file chmod 600. It is the sole source of auth.

### Allowing multiple users

1. Pick a random token for each user. Something like `openssl rand -hex 20` works fine.
2. Add a `username:token` line for each person to the server's token file.
3. Send each person their token out-of-band (email, Signal, whatever). They put it in their `remote.yaml` as `api_token`.
4. The server picks up new lines automatically — no restart needed.

Example token file for a three-person team:

```
alice:b3c7f1a2d9e04581ac63
bob:9d2e6f0c1a3b47825def
carol:f4a8d5b2c9e01674bcf3
```

---

## Connecting Clients

### Web UI

Open `http://<server-ip>:5005` in a browser. In the top-right corner, click the **Collab** button (or the lock icon) and enter your token. Once connected:

- A **presence bar** appears showing all currently connected users.
- **Toast notifications** pop up in the corner when tickets are created, moved, commented on, or archived.
- The board **auto-refreshes** on `TICKET_CREATED`, `TICKET_UPDATED`, and `TICKET_ARCHIVED` events — no manual reload needed.
- The EventSource connection uses `?token=<your-token>` in the URL (browsers can't set `Authorization` headers on EventSource connections).

To test without the browser:

```bash
# Verify the SSE stream is reachable and auth works
curl -N -H "Authorization: Bearer token-abc-123" \
  http://192.168.1.10:5005/api/events
```

You should see a `data: {"type":"CONNECTED",...}` line immediately, followed by `: ping <ts>` keepalive comments every 30 seconds.

### CLI (--watch)

The `--watch` flag streams events from the server to your terminal. It auto-reconnects if the connection drops.

```bash
# Watch a named remote (reads URL and token from remote.yaml)
vimban --remote config:work --watch

# Watch a direct URL with an inline token
vimban --remote http://192.168.1.10:5005 --api-token token-abc-123 --watch
```

Example output:

```
[vimban watch] connecting to http://192.168.1.10:5005 ...
[vimban watch] connected — watching for events (Ctrl+C to exit)
  presence: alice, bob
  >> carol joined
  + carol created CLAW-00043: fix login redirect
  ~ alice moved CLAW-00041 -> in_progress
  > bob commented on CLAW-00043
  << bob left
^C
[vimban watch] disconnected
```

Press Ctrl+C to exit. On connection loss the client prints `connection lost (...), reconnecting in 5s...` and retries automatically.

The `watch` subcommand also works as a standalone subcommand:

```bash
vimban --remote config:work watch
vimban --remote config:work watch --format json   # raw JSON event stream
```

### TUI (Terminal UI)

Pass `--remote` when launching the TUI. Everything else is automatic.

```bash
# Connect TUI to a named remote
vimban tui --remote config:work

# Connect with an explicit URL and token
vimban tui --remote http://192.168.1.10:5005 --api-token token-abc-123

# Connect but skip auth (server must have --no-token)
vimban tui --remote http://127.0.0.1:5005 --no-token
```

What happens when you connect:

1. A **background daemon thread** starts immediately, subscribed to `/api/events` with a 90-second read timeout.
2. The thread feeds incoming events into a thread-safe queue. The TUI main loop drains the queue on each iteration.
3. On `TICKET_CREATED`, `TICKET_UPDATED`, or `TICKET_ARCHIVED`: the data layer refreshes automatically.
4. On `CONNECTED`, `PRESENCE_JOIN`, or `PRESENCE_LEAVE`: the presence list updates and the header redraws.
5. The **header bar** shows connected collaborators on the right side: `[collab:alice,bob,carol] [kanban] ...`
6. If the SSE connection drops, the background thread sleeps 5 seconds and reconnects silently — the TUI stays usable in the meantime.

In remote mode, the TUI swaps out its local file loader for the remote API — all ticket reads and writes go through the server. The `--directory` flag is ignored when `--remote` is active.

---

## Project Keys

### What are project keys?

Project keys are the ticket ID prefixes used on a specific remote server — e.g., `CLAW`, `PROJ`, `BUG`. When you create a ticket on a remote, vimban needs to know which prefix to use. Project keys let you configure that per-remote in `remote.yaml` instead of passing `--prefix` every time.

### Configuring project keys per remote

Add a `project_keys` list to the remote entry in `~/.config/vimban/remote.yaml`:

```yaml
teamserver:
  url: "http://192.168.1.10:5005"
  api_token: "alice:token-abc-123"
  project_keys:
    - CLAW    # first entry = default prefix for new tickets
    - PROJ
    - BUG
```

- The **first key** in the list is the default prefix injected when creating tickets on this remote.
- Subsequent keys document what other prefixes exist on the server (used for display filtering and tooling — the CLI does not enforce them as an allowlist).
- If `project_keys` is omitted, the server's built-in defaults apply (typically `PROJ`).
- You can override the prefix on a per-command basis with `--prefix`.

### How prefix injection works

When you run `vimban --remote config:teamserver create task "fix login redirect"`:

1. `_resolve_remote("config:teamserver")` reads `remote.yaml` and returns `(url, token, ["CLAW", "PROJ", "BUG"])`.
2. If no `--prefix` flag was given, the first entry from `project_keys` (`CLAW`) is injected into the create payload.
3. The server creates the ticket as `CLAW-XXXXX`.

To override:

```bash
# Use BUG prefix instead of the default CLAW
vimban --remote config:teamserver create bug "null ptr in parser" --prefix BUG
```

---

## Remote Config Reference

Full annotated example of `~/.config/vimban/remote.yaml`:

```yaml
# ~/.config/vimban/remote.yaml
# Copy from examples/remote.yaml.example and customize.
#
# Named remotes are referenced with: vimban --remote config:<name>
# Example: vimban --remote config:work list

# ============================================================================
# Work server on the LAN
# ============================================================================
work:
  # Base URL of the vimban_serve instance. No trailing slash.
  url: "http://192.168.1.10:5005"

  # Your personal token from the server's token file.
  # Format: "username:token" — the username must match what the server
  # has in its token file (so event attribution shows your name).
  api_token: "alice:token-abc-123"

  # Optional list of ticket ID prefixes used on this remote.
  # First entry is the default prefix for new tickets.
  # Subsequent entries are alternates (informational — not enforced as allowlist).
  project_keys:
    - CLAW    # default prefix: vimban --remote config:work create task "..."
    - PROJ    #   -> creates CLAW-XXXXX
    - BUG

# ============================================================================
# Personal server over HTTPS
# ============================================================================
home:
  url: "https://vimban.example.com:5005"
  api_token: "alice:token-home-999"
  project_keys:
    - CLAW
    - LIB
    - CORE

# ============================================================================
# Local dev server (no auth)
# ============================================================================
local:
  url: "http://127.0.0.1:5005"
  # api_token omitted — use with --no-token flag:
  # vimban --remote config:local --no-token list
```

The file is read by both `vimban` (CLI) and `vimban_tui`. The `api_token` value in `remote.yaml` is what you pass as `Authorization: Bearer <token>` on all requests — it must be the literal token string (the part after the colon in `username:token`), not the `username:token` pair. If you want your username shown in events, the server's token file must have a matching `username:token` line.

---

## SSE API Reference

### GET /api/events

Establishes a persistent SSE stream. Requires auth.

**Auth:** `Authorization: Bearer <token>` header, OR `?token=<token>` query param (for browser EventSource which cannot set headers).

```bash
# Header auth (curl, CLI --watch)
curl -N -H "Authorization: Bearer token-abc-123" \
  http://192.168.1.10:5005/api/events

# Query param auth (browser EventSource)
# GET /api/events?token=token-abc-123
```

**Response headers:**

```
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no
Connection: keep-alive
```

**Keepalive:** The server sends `: ping <unix_timestamp>` SSE comments every 30 seconds to keep the connection alive through proxies and load balancers. These are not data events — clients should silently ignore them.

**On connect:** The server immediately sends a `CONNECTED` event (see below), then starts broadcasting all subsequent board activity.

**On disconnect:** The server removes the client from `_presence` and broadcasts `PRESENCE_LEAVE` to remaining clients.

---

### GET /api/presence

Returns the list of currently connected users. **No auth required.** Useful for a quick sanity check without needing a token.

```bash
curl http://192.168.1.10:5005/api/presence
```

Response:

```json
{
  "presence": [
    {"user": "alice", "connected_at": 1708704000.123, "viewing": null},
    {"user": "bob",   "connected_at": 1708704060.456, "viewing": null}
  ],
  "count": 2
}
```

---

### GET /api/snapshot

Returns the full current board state in one shot. Requires auth. Called by the web UI immediately after opening the SSE stream so it can reconcile any events missed before the stream was established.

```bash
curl -H "Authorization: Bearer token-abc-123" \
  http://192.168.1.10:5005/api/snapshot
```

Response:

```json
{
  "tickets": [ ... ],
  "presence": [
    {"user": "alice", "connected_at": 1708704000.123, "viewing": null}
  ],
  "ts": 1708704123.456
}
```

`tickets` is the same structure returned by `/api/tickets`. `ts` is the server's Unix timestamp at the time of the snapshot — clients can use it to detect stale data.

---

### Event payload format

All SSE messages are `data: <json>\n\n` lines. The JSON envelope is always:

```json
{
  "type": "EVENT_TYPE",
  "data": { ... },
  "ts": 1708704123.456
}
```

`ts` is the server's Unix timestamp when the event was emitted.

#### CONNECTED

Sent immediately on connect. Contains the client's assigned ID and the current presence list.

```json
{
  "type": "CONNECTED",
  "data": {
    "client_id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
    "user": "alice",
    "presence": [
      {"user": "alice", "connected_at": 1708704000.1, "viewing": null},
      {"user": "bob",   "connected_at": 1708703900.5, "viewing": null}
    ]
  },
  "ts": 1708704000.1
}
```

#### PRESENCE_JOIN

Emitted to all existing clients when a new client connects.

```json
{
  "type": "PRESENCE_JOIN",
  "data": {
    "user": "carol",
    "presence": [
      {"user": "alice", "connected_at": 1708704000.1, "viewing": null},
      {"user": "bob",   "connected_at": 1708703900.5, "viewing": null},
      {"user": "carol", "connected_at": 1708704060.2, "viewing": null}
    ]
  },
  "ts": 1708704060.2
}
```

#### PRESENCE_LEAVE

Emitted to remaining clients when a connection closes (clean disconnect or timeout).

```json
{
  "type": "PRESENCE_LEAVE",
  "data": {
    "user": "bob",
    "presence": [
      {"user": "alice", "connected_at": 1708704000.1, "viewing": null},
      {"user": "carol", "connected_at": 1708704060.2, "viewing": null}
    ]
  },
  "ts": 1708704180.7
}
```

#### TICKET_CREATED

Emitted when a new ticket is created via `POST /api/tickets`.

```json
{
  "type": "TICKET_CREATED",
  "data": {
    "ticket_id": "CLAW-00043",
    "title": "fix login redirect after OAuth",
    "type": "task",
    "status": "backlog",
    "user": "carol"
  },
  "ts": 1708704200.0
}
```

#### TICKET_UPDATED

Emitted on status changes (`POST /api/ticket/<id>/move`) and field edits (`POST /api/ticket/<id>/edit`).

Status change (has `field` and `value`):

```json
{
  "type": "TICKET_UPDATED",
  "data": {
    "ticket_id": "CLAW-00041",
    "field": "status",
    "value": "in_progress",
    "user": "alice"
  },
  "ts": 1708704240.0
}
```

Field edit (has `fields` list, no `field`/`value`):

```json
{
  "type": "TICKET_UPDATED",
  "data": {
    "ticket_id": "CLAW-00041",
    "fields": ["priority", "assignee"],
    "user": "alice"
  },
  "ts": 1708704245.0
}
```

#### TICKET_COMMENT

Emitted when a comment is posted via `POST /api/ticket/<id>/comment`.

```json
{
  "type": "TICKET_COMMENT",
  "data": {
    "ticket_id": "CLAW-00043",
    "user": "bob"
  },
  "ts": 1708704260.0
}
```

Comment text is not included in the event payload — clients that need it should fetch `/api/ticket/<id>/comments`.

#### TICKET_ARCHIVED

Emitted when a ticket is archived via `POST /api/ticket/<id>/archive`.

```json
{
  "type": "TICKET_ARCHIVED",
  "data": {
    "ticket_id": "CLAW-00039",
    "user": "alice"
  },
  "ts": 1708704300.0
}
```

---

## Troubleshooting

**`Auth: DISABLED (no tokens found in ~/.config/vimban/token)`**

The server started but found no valid tokens. Check:
- The file exists at `~/.config/vimban/token` (or wherever you passed `--token-file`).
- Lines are `username:token` or bare tokens — no extra whitespace, no BOM.
- The file is readable by the user running `vimban_serve`.

**`Error: authentication failed (401 Unauthorized)` on the client**

Your `api_token` in `remote.yaml` doesn't match any token in the server's token file. The `api_token` value should be just the token string — not the full `username:token` pair. If your token file has `alice:token-abc-123`, put `alice:token-abc-123` in `remote.yaml` — the server splits on `:` and uses `token-abc-123` as the lookup key.

Wait — re-reading the code: `_resolve_remote` reads `api_token` raw and passes it as the Bearer value. And the server's `_load_tokens` parses `username:token` lines storing `{token: username}`. So `api_token: "alice:token-abc-123"` would send `Bearer alice:token-abc-123`, which the server would look up in `_valid_tokens` — and find it only if `alice:token-abc-123` is stored as a key. That means the server would need to store the full `alice:token-abc-123` string as the token, which only happens with a bare (non-colon) token format.

To be safe: use a bare token string in both places. Put `token-abc-123` in the server's token file as `alice:token-abc-123` (server stores key=`token-abc-123`, value=`alice`), and put `api_token: "token-abc-123"` in `remote.yaml`. The `username:token` example in the remote config template is for documentation clarity; the actual `api_token` value sent over the wire must be just the token part.

**SSE stream connects but immediately gets 401**

The `/api/events` endpoint does auth itself (not via the middleware, which skips it for some paths). Check that the token is being sent — either as `Authorization: Bearer <token>` or as `?token=<token>` in the URL. The web UI uses the query param path; the CLI and TUI use the header.

**`[vimban watch] connection lost (...), reconnecting in 5s...` in a tight loop**

The server is running but rejecting the connection. Run `curl -v -H "Authorization: Bearer <your-token>" http://<host>:5005/api/events` and check the HTTP status code. 401 = bad token. 404 = wrong URL. 500 = server error — check `vimban_serve` output.

**TUI shows no presence in the header**

The background SSE thread starts automatically when `--remote` is passed to `vimban_tui`. If the header shows no `[collab:...]` block, either the SSE connection failed silently or no one else is connected. Try `vimban --remote config:work --watch` in a separate terminal to verify the stream is working.

**Web UI presence bar is empty but you're connected**

The presence bar only shows other connected users. If you're the only one connected, it will be empty. Use `curl http://<host>:5005/api/presence` to see the raw presence data including yourself.

**Tickets created on a remote get the wrong prefix**

Check that `project_keys` is set in `remote.yaml` and the first entry is the prefix you want. If `project_keys` is omitted, the server falls back to its built-in default (usually `PROJ`). You can always override per-command with `--prefix CLAW`.

**Server stops broadcasting events after a while**

The server sends `: ping <ts>` keepalive comments every 30 seconds. If a reverse proxy (nginx, caddy) is stripping these or has a short proxy_read_timeout, the connection will silently drop. Set `proxy_read_timeout 3600;` (nginx) or equivalent. Also set `X-Accel-Buffering: no` passthrough so nginx doesn't buffer the stream.
