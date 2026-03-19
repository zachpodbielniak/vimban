#!/usr/bin/python3

# vimban - Markdown-native ticket/kanban management system
# Copyright (C) 2025  Zach Podbielniak
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
vimban - Markdown-native ticket/kanban management system (:wqira)

A CLI-first ticket management tool operating on markdown files with
YAML frontmatter. Designed for Neovim integration via vim-filter.

Usage:
    vimban [global-options] <command> [command-options]

Examples:
    vimban init
    vimban create task "Fix authentication bug"
    vimban list --status in_progress --mine
    vimban move PROJ-42 done --resolve
    vimban dashboard daily
"""

# ============================================================================
# DISTROBOX CHECK (Before any other imports)
# ============================================================================
from os import environ
from pathlib import Path as _Path
from subprocess import run
from sys import argv, exit


def _read_config_value(key: str, default: str = "") -> str:
    """
    Read a single top-level key from config.yaml using stdlib only.

    This runs before yaml is imported so we do a simple line-based
    parse. Only handles top-level scalar values (key: value).

    Args:
        key: The YAML key to look up
        default: Fallback if key is not found

    Returns:
        The value as a string, or default
    """
    config_path: _Path = _Path.home() / ".config" / "vimban" / "config.yaml"
    if not config_path.exists():
        return default
    try:
        with open(config_path, "r") as f:
            for line in f:
                stripped: str = line.strip()
                if stripped.startswith("#") or ":" not in stripped:
                    continue
                k, _, v = stripped.partition(":")
                if k.strip() == key:
                    val: str = v.strip().strip('"').strip("'")
                    return val if val else default
    except (OSError, ValueError):
        pass
    return default


ctr_id: str | None = ""

if ("CONTAINER_ID" in environ):
    ctr_id = environ.get("CONTAINER_ID")

# Check if distrobox check should be skipped via env or config
no_dbox_check = environ.get("NO_DBOX_CHECK", "").lower() in ("1", "true")
if not no_dbox_check:
    _cfg_dbox: str = _read_config_value("distrobox", "")
    if _cfg_dbox.lower() in ("", "false", "none", "disabled", "off"):
        no_dbox_check = True

# Determine target container: env > config > default "dev"
_dbox_target: str = environ.get("VIMBAN_DISTROBOX", "") or _read_config_value("distrobox", "dev")

# if we are not in the target distrobox re-exec the script
# inside of the target distrobox
if not no_dbox_check and (_dbox_target != ctr_id):
    cmd: list[str] = [
        "distrobox",
        "enter",
        _dbox_target,
        "--",
        *argv
    ]

    run(cmd)
    exit(0)


# ============================================================================
# IMPORTS
# ============================================================================
import argparse
import fcntl
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import datetime, date, timedelta
from pathlib import Path
from typing import Optional, Any

import yaml


# ============================================================================
# MCP SERVER SUPPORT (Optional)
# ============================================================================
try:
    from mcp.server.fastmcp import FastMCP
    MCP_AVAILABLE: bool = True
except ImportError:
    MCP_AVAILABLE = False


# ============================================================================
# TEMPLATE RESOLUTION
# ============================================================================
def _resolve_template_dir() -> Path:
    """
    Resolve the template directory with a fallback chain.

    Lookup order:
    1. VIMBAN_TEMPLATE_DIR environment variable
    2. Submodule path: ~/.dotfiles/vimban/share/vimban/templates
    3. Legacy dotfiles path: ~/.dotfiles/share/vimban/templates
    4. Relative to script: ../../share/vimban/templates
    5. XDG data: ~/.local/share/vimban/templates

    Returns:
        Path to the templates directory
    """
    # check environment variable override
    env_path: str | None = os.environ.get("VIMBAN_TEMPLATE_DIR")
    if env_path:
        p: Path = Path(env_path)
        if p.exists():
            return p

    # submodule path (vimban as submodule in dotfiles)
    submod: Path = Path.home() / ".dotfiles" / "vimban" / "share" / "vimban" / "templates"
    if submod.exists():
        return submod

    # legacy dotfiles path (vimban embedded in dotfiles)
    legacy: Path = Path.home() / ".dotfiles" / "share" / "vimban" / "templates"
    if legacy.exists():
        return legacy

    # relative to this script (standalone install)
    script_dir: Path = Path(__file__).resolve().parent
    relative: Path = script_dir.parent.parent / "share" / "vimban" / "templates"
    if relative.exists():
        return relative

    # XDG data directory
    xdg_data: str = os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))
    xdg: Path = Path(xdg_data) / "vimban" / "templates"
    if xdg.exists():
        return xdg

    # fallback to submodule path (will fail gracefully later if missing)
    return submod


# ============================================================================
# REMOTE API CLIENT
# ============================================================================
REMOTE_CONFIG_FILE: Path = Path.home() / ".config" / "vimban" / "remote.yaml"


def _resolve_remote(remote_str: str) -> tuple[str, str | None, list[str]]:
    """
    Resolve a --remote value to (url, token, project_keys).

    Supports:
        - Direct URL: "http://host:5005" or "https://host:5005/path"
        - Config lookup: "config:<name>" reads ~/.config/vimban/remote.yaml

    Args:
        remote_str: The --remote argument value

    Returns:
        Tuple of (base_url, api_token_or_None, project_keys)
        project_keys is [] for direct URLs or when not configured.
        The first entry in project_keys is the default prefix for new tickets.
    """
    if remote_str.startswith("config:"):
        name: str = remote_str[7:]
        if not REMOTE_CONFIG_FILE.exists():
            print(f"Error: remote config file not found: {REMOTE_CONFIG_FILE}", file=sys.stderr)
            sys.exit(1)
        with open(REMOTE_CONFIG_FILE, "r") as f:
            data: dict = yaml.safe_load(f) or {}
        if name not in data:
            print(f"Error: remote '{name}' not found in {REMOTE_CONFIG_FILE}", file=sys.stderr)
            print(f"Available remotes: {', '.join(data.keys())}", file=sys.stderr)
            sys.exit(1)
        entry: dict = data[name]
        url: str = entry.get("url", "")
        token: str | None = entry.get("api_token", None)
        project_keys: list[str] = entry.get("project_keys", [])
        if not url:
            print(f"Error: remote '{name}' has no url defined", file=sys.stderr)
            sys.exit(1)
        return (url.rstrip("/"), token, project_keys)
    else:
        return (remote_str.rstrip("/"), None, [])


def _watch_events(remote_url: str, token: str) -> None:
    """Connect to SSE event stream and display events. Reconnects on drop."""
    import time

    sse_url = remote_url.rstrip('/') + '/api/events'
    headers = {'Accept': 'text/event-stream', 'Authorization': 'Bearer ' + token}

    print(f'[vimban watch] connecting to {remote_url} ...', flush=True)

    while True:
        try:
            import urllib.request
            req = urllib.request.Request(sse_url, headers=headers)
            with urllib.request.urlopen(req, timeout=90) as resp:
                print('[vimban watch] connected — watching for events (Ctrl+C to exit)', flush=True)
                buffer = ''
                for raw in resp:
                    line = raw.decode('utf-8').rstrip('\n').rstrip('\r')
                    if line.startswith('data:'):
                        buffer = line[5:].strip()
                    elif line == '' and buffer:
                        try:
                            evt = json.loads(buffer)
                            _display_watch_event(evt)
                        except Exception:
                            pass
                        buffer = ''
        except KeyboardInterrupt:
            print('\n[vimban watch] disconnected')
            break
        except Exception as e:
            print(f'[vimban watch] connection lost ({e}), reconnecting in 5s...', flush=True)
            time.sleep(5)


def _display_watch_event(evt: dict) -> None:
    """Print a human-readable line for an SSE event."""
    t = evt.get('type', '')
    d = evt.get('data', {})
    user = d.get('user', '?')
    tid = d.get('ticket_id', '')

    if t == 'CONNECTED':
        presence = d.get('presence', [])
        users = ', '.join(p['user'] for p in presence) if presence else 'none'
        print(f'  presence: {users}', flush=True)
    elif t == 'PRESENCE_JOIN':
        print(f'  >> {d.get("user", "?")} joined', flush=True)
    elif t == 'PRESENCE_LEAVE':
        print(f'  << {d.get("user", "?")} left', flush=True)
    elif t == 'TICKET_CREATED':
        print(f'  + {user} created {tid}: {d.get("title", "")}', flush=True)
    elif t == 'TICKET_UPDATED':
        field = d.get('field', '')
        value = d.get('value', '')
        fields = d.get('fields', [])
        if field:
            print(f'  ~ {user} moved {tid} -> {value}', flush=True)
        else:
            print(f'  ~ {user} edited {tid} ({", ".join(fields)})', flush=True)
    elif t == 'TICKET_COMMENT':
        print(f'  > {user} commented on {tid}', flush=True)
    elif t == 'TICKET_ARCHIVED':
        print(f'  x {user} archived {tid}', flush=True)
    # silently ignore keepalive pings (: ping ...) and unknown event types


def _remote_request(
    base_url: str,
    method: str,
    path: str,
    token: str | None = None,
    data: dict | None = None,
    params: dict | None = None,
) -> dict:
    """
    Make an HTTP request to a remote vimban_serve instance.

    Uses urllib.request (stdlib only) to avoid extra dependencies.

    Args:
        base_url: Base URL of the remote server (e.g. http://host:5005)
        method: HTTP method (GET, POST)
        path: API path (e.g. /api/tickets)
        token: Bearer token for auth (None to skip)
        data: JSON body for POST requests
        params: Query parameters for GET requests

    Returns:
        Parsed JSON response dict

    Raises:
        SystemExit on connection errors or auth failures
    """
    url: str = f"{base_url}{path}"
    if params:
        query: str = urllib.parse.urlencode(
            {k: v for k, v in params.items() if v}
        )
        if query:
            url = f"{url}?{query}"

    body: bytes | None = None
    if data is not None:
        body = json.dumps(data).encode("utf-8")

    req: urllib.request.Request = urllib.request.Request(url, data=body, method=method)
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")

    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            resp_data: bytes = resp.read()
            return json.loads(resp_data)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("Error: authentication failed (401 Unauthorized)", file=sys.stderr)
            print("Check your API token or use --no-token if auth is disabled on the server", file=sys.stderr)
            sys.exit(1)
        elif e.code == 404:
            print(f"Error: not found (404): {path}", file=sys.stderr)
            sys.exit(1)
        else:
            body_text: str = ""
            try:
                body_text = e.read().decode("utf-8", errors="replace")
            except Exception:
                pass
            print(f"Error: HTTP {e.code}: {body_text}", file=sys.stderr)
            sys.exit(1)
    except urllib.error.URLError as e:
        print(f"Error: cannot connect to {base_url}: {e.reason}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: request failed: {e}", file=sys.stderr)
        sys.exit(1)


def _remote_dispatch(args: argparse.Namespace, remote_url: str, token: str | None, project_keys: list[str] | None = None) -> int:
    """
    Dispatch a CLI command through the remote API.

    Maps each CLI command to the corresponding vimban_serve API endpoint,
    sends the request, and formats the output according to args.format.

    Args:
        args: Parsed argparse namespace with command and subcommand args
        remote_url: Base URL of the remote vimban_serve
        token: Bearer token (or None for --no-token)
        project_keys: Optional list of project key prefixes from remote config.
            First entry is the default prefix for new tickets.

    Returns:
        Exit code (0 for success)
    """
    command: str = args.command
    fmt: str = getattr(args, "format", "plain")

    if command == "list":
        params: dict[str, str] = {}
        if getattr(args, "status", None):
            params["status"] = args.status
        if getattr(args, "type", None):
            params["type"] = args.type
        if getattr(args, "assignee", None):
            params["assignee"] = args.assignee
        if getattr(args, "priority", None):
            params["priority"] = args.priority
        if getattr(args, "project", None):
            params["project"] = args.project
        if getattr(args, "work", False):
            params["scope"] = "work"
        elif getattr(args, "personal", False):
            params["scope"] = "personal"
        if getattr(args, "archived", False):
            params["include_done"] = "true"

        resp: dict = _remote_request(remote_url, "GET", "/api/tickets", token, params=params)
        tickets: list = resp.get("tickets", [])
        _output_remote_data(tickets, fmt, "list")
        return 0

    elif command == "show":
        ticket_id: str = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        resp = _remote_request(remote_url, "GET", f"/api/ticket/{ticket_id}", token)
        ticket: dict = resp.get("ticket", {})
        _output_remote_data(ticket, fmt, "show")
        return 0

    elif command == "create":
        create_data: dict = {
            "type": getattr(args, "type", "task"),
            "title": getattr(args, "title", ""),
            "priority": getattr(args, "priority", "medium") or "medium",
        }
        if getattr(args, "assignee", None):
            create_data["assignee"] = args.assignee
        if getattr(args, "reporter", None):
            create_data["reporter"] = args.reporter
        if getattr(args, "project", None):
            create_data["project"] = args.project
        if getattr(args, "tags", None):
            create_data["tags"] = args.tags
        if getattr(args, "due", None):
            create_data["due"] = args.due
        if getattr(args, "member_of", None):
            create_data["member_of"] = ",".join(args.member_of)
        if getattr(args, "work", False):
            create_data["scope"] = "work"
        elif getattr(args, "personal", False):
            create_data["scope"] = "personal"
        # --prefix flag takes precedence; fall back to first project_key from config
        explicit_prefix: str = getattr(args, "prefix", None) or ""
        if explicit_prefix:
            create_data["prefix"] = explicit_prefix
        elif project_keys:
            create_data["prefix"] = project_keys[0]

        resp = _remote_request(remote_url, "POST", "/api/tickets", token, data=create_data)
        if resp.get("success"):
            print(resp.get("id", "Created"))
        else:
            print(f"Error: {resp.get('message', 'Failed')}", file=sys.stderr)
            return 1
        return 0

    elif command == "move":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        new_status: str = getattr(args, "status", "") or getattr(args, "new_status", "")
        resp = _remote_request(remote_url, "POST", f"/api/ticket/{ticket_id}/move", token, data={"status": new_status})
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    elif command == "archive":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        resp = _remote_request(remote_url, "POST", f"/api/ticket/{ticket_id}/archive", token)
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    elif command == "comment":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        text: str = getattr(args, "text", "") or getattr(args, "message", "")
        resp = _remote_request(remote_url, "POST", f"/api/ticket/{ticket_id}/comment", token, data={"text": text})
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    elif command == "comments":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        resp = _remote_request(remote_url, "GET", f"/api/ticket/{ticket_id}/comments", token)
        comments: list = resp.get("comments", [])
        _output_remote_data(comments, fmt, "comments")
        return 0

    elif command == "edit":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "id", "")
        edit_data: dict = {}
        if getattr(args, "priority", None):
            edit_data["priority"] = args.priority
        if getattr(args, "assignee", None):
            edit_data["assignee"] = args.assignee
        if getattr(args, "progress", None):
            edit_data["progress"] = args.progress
        if getattr(args, "tags", None):
            edit_data["tags"] = args.tags
        if getattr(args, "due", None):
            edit_data["due_date"] = args.due
        resp = _remote_request(remote_url, "POST", f"/api/ticket/{ticket_id}/edit", token, data=edit_data)
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    elif command == "search":
        query: str = getattr(args, "query", "") or getattr(args, "pattern", "")
        resp = _remote_request(remote_url, "GET", "/api/search", token, params={"q": query})
        tickets = resp.get("tickets", [])
        _output_remote_data(tickets, fmt, "list")
        return 0

    elif command == "dashboard":
        dtype: str = getattr(args, "type", "daily") or "daily"
        resp = _remote_request(remote_url, "GET", f"/api/dashboard/{dtype}", token)
        print(resp.get("content", ""))
        return 0

    elif command == "kanban":
        resp = _remote_request(remote_url, "GET", "/api/kanban", token)
        board: dict = resp.get("board", {})
        if fmt == "json":
            print(json.dumps(board, indent=2))
        else:
            for status_name, items in board.items():
                if items:
                    print(f"\n=== {status_name.upper()} ({len(items)}) ===")
                    for t in items:
                        tid: str = t.get("id", "?")
                        title: str = t.get("title", "")
                        print(f"  {tid}: {title}")
        return 0

    elif command == "people":
        resp = _remote_request(remote_url, "GET", "/api/people", token)
        people: list = resp.get("people", [])
        _output_remote_data(people, fmt, "people")
        return 0

    elif command == "report":
        rtype: str = getattr(args, "type", "summary") or "summary"
        resp = _remote_request(remote_url, "GET", "/api/report", token, params={"type": rtype})
        if resp.get("format") == "json":
            _output_remote_data(resp.get("report", {}), fmt, "report")
        else:
            print(resp.get("content", ""))
        return 0

    elif command in ("sync", "commit"):
        pull: bool = getattr(args, "pull", False)
        resp = _remote_request(remote_url, "POST", "/api/commit", token, data={"pull": pull})
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    elif command == "link":
        ticket_id = getattr(args, "ticket_id", "") or getattr(args, "source", "")
        target: str = getattr(args, "target", "")
        link_type: str = getattr(args, "type", "related") or "related"
        resp = _remote_request(
            remote_url, "POST", f"/api/ticket/{ticket_id}/link", token,
            data={"target": target, "type": link_type}
        )
        print(resp.get("message", ""))
        return 0 if resp.get("success") else 1

    else:
        print(f"Error: command '{command}' is not supported in remote mode", file=sys.stderr)
        print("Supported remote commands: list, show, create, move, archive, comment,", file=sys.stderr)
        print("  comments, edit, search, dashboard, kanban, people, report, sync, commit, link", file=sys.stderr)
        return 1


def _output_remote_data(data: Any, fmt: str, context: str) -> None:
    """
    Format and print data received from a remote API response.

    Args:
        data: The data to output (list, dict, etc.)
        fmt: Output format (plain, json, yaml, md)
        context: Context hint (list, show, comments, people, report)
    """
    if fmt == "json":
        print(json.dumps(data, indent=2))
    elif fmt == "yaml":
        print(yaml.dump(data, default_flow_style=False))
    elif fmt == "md":
        # markdown table for lists
        if isinstance(data, list) and data:
            if context in ("list", "comments", "people"):
                keys: list[str] = list(data[0].keys()) if data else []
                if keys:
                    print("| " + " | ".join(keys) + " |")
                    print("| " + " | ".join(["---"] * len(keys)) + " |")
                    for item in data:
                        vals: list[str] = [str(item.get(k, "")) for k in keys]
                        print("| " + " | ".join(vals) + " |")
            else:
                print(json.dumps(data, indent=2))
        elif isinstance(data, dict):
            for k, v in data.items():
                print(f"**{k}**: {v}")
        else:
            print(str(data))
    else:
        # plain text
        if isinstance(data, list):
            for item in data:
                if isinstance(item, dict):
                    tid: str = item.get("id", "")
                    title: str = item.get("title", "")
                    status: str = item.get("status", "")
                    priority: str = item.get("priority", "")
                    if context == "list":
                        print(f"{tid}  {status:<12} {priority:<8} {title}")
                    elif context == "comments":
                        author: str = item.get("author", "")
                        ts: str = item.get("timestamp", "")
                        body: str = item.get("text", item.get("body", ""))
                        print(f"[{ts}] {author}: {body}")
                    elif context == "people":
                        name: str = item.get("name", item.get("id", ""))
                        role: str = item.get("role", "")
                        print(f"{name}  {role}")
                    else:
                        print(str(item))
                else:
                    print(str(item))
        elif isinstance(data, dict):
            for k, v in data.items():
                print(f"{k}: {v}")
        else:
            print(str(data))


# ============================================================================
# CONSTANTS
# ============================================================================
VERSION: str = "0.2.1"
DEFAULT_DIR: Path = Path.home() / "Documents" / "notes"
DEFAULT_PEOPLE_DIR: str = "02_areas/work/people"
DEFAULT_PREFIX: str = "PROJ"
CONFIG_DIR_NAME: str = ".vimban"
SEQUENCE_FILE: str = ".sequence"
CONFIG_FILE: str = "config.yaml"
TEMPLATE_DIR: Path = _resolve_template_dir()

# ============================================================================
# ID PREFIX REFERENCE
# ============================================================================
# Each ticket type has its own ID prefix and sequence file:
#
#   Ticket Type              Prefix        Sequence File
#   -----------------------  ------------  ---------------------
#   epic, story, task,       PROJ-         .sequence
#     sub-task
#   bug                      BUG-          .sequence_bug
#   research                 RESEARCH-     .sequence_research
#   area                     AREA-         .sequence_area
#   resource                 RESOURCE-     .sequence_resource
#   meeting                  MTG-          .sequence_meeting
#   mentorship               MNTR-         .sequence_mentorship
#   journal                  JNL-          .sequence_journal
#   recipe                   RCP-          .sequence_recipe
#   person                   PERSON-       .sequence_person
#
# Prefixes are defined in TICKET_PREFIXES, PARA_PREFIXES, and specialized
# type configs. The DEFAULT_PREFIX (PROJ) is used as fallback.
# ============================================================================

# Exit codes
EXIT_SUCCESS: int = 0
EXIT_GENERAL_ERROR: int = 1
EXIT_INVALID_ARGS: int = 2
EXIT_FILE_NOT_FOUND: int = 3
EXIT_VALIDATION_ERROR: int = 4
EXIT_KRAFNA_ERROR: int = 5
EXIT_GIT_ERROR: int = 6

# License text for --license flag
LICENSE_TEXT: str = """vimban - Markdown-native ticket/kanban management system
Copyright (C) 2025  Zach Podbielniak

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>."""

# Bash completion script (embedded for `vimban completion bash`)
BASH_COMPLETION_SCRIPT: str = r'''# vimban bash completion
# Usage: eval "$(vimban completion bash)"

_vimban_completions () {
    local cur prev words cword
    _init_completion || return

    local commands="init create list show generate-link get-link get-id edit move link comment comments dashboard kanban search validate report people sync commit convert completion tui"
    local types="epic story task sub-task research bug area resource meeting journal recipe"
    local statuses="backlog ready in_progress blocked review delegated done cancelled"
    local priorities="critical high medium low"
    local formats="plain md yaml json"
    local people_cmds="list show edit dashboard create search"
    local sections="assigned reported watching blocked overdue due_soon full"
    local relations="member_of relates_to blocked_by blocks"
    local report_types="burndown velocity workload aging blockers"
    local dashboard_types="daily weekly sprint project team person"

    # Find command
    local cmd=""
    for ((i=1; i < cword; i++)); do
        [[ "${words[i]}" != -* ]] && { cmd="${words[i]}"; break; }
    done

    case "${cmd}" in
        "")
            COMPREPLY=($(compgen -W "${commands}" -- "${cur}"))
            ;;
        create)
            case "${prev}" in
                create)
                    COMPREPLY=($(compgen -W "${types}" -- "${cur}"))
                    ;;
                -a|--assignee|-r|--reporter|-w|--watcher)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                -p|--priority)
                    COMPREPLY=($(compgen -W "${priorities}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-a --assignee -r --reporter -w --watcher -p --priority -t --tags -P --project -m --member-of --due -e --effort -o --output --id --prefix --topic --date --no-edit --dry-run" -- "${cur}"))
                    ;;
            esac
            ;;
        convert)
            case "${prev}" in
                convert)
                    COMPREPLY=($(compgen -W "find-missing" -- "${cur}"))
                    ;;
                find-missing)
                    COMPREPLY=($(compgen -W "--areas --resources --meetings --journals --recipes --people --dry-run --no-confirm -v --verbose" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--areas --resources --meetings --journals --recipes --people --dry-run --no-confirm -v --verbose" -- "${cur}"))
                    ;;
            esac
            ;;
        list)
            case "${prev}" in
                -s|--status)
                    COMPREPLY=($(compgen -W "${statuses}" -- "${cur}"))
                    ;;
                -t|--type)
                    COMPREPLY=($(compgen -W "${types}" -- "${cur}"))
                    ;;
                --priority)
                    COMPREPLY=($(compgen -W "${priorities}" -- "${cur}"))
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                -a|--assignee)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-s --status -t --type -a --assignee -P --project --tag --priority --overdue --due-soon --blocked --unassigned --mine --areas --resources --sort --reverse --limit --columns --no-header -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        show)
            case "${prev}" in
                show)
                    # Complete with ticket IDs
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--links --tree --history --raw -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        generate-link|get-link)
            case "${prev}" in
                generate-link|get-link)
                    # Complete with ticket IDs
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--full --markdown --transclusion" -- "${cur}"))
                    ;;
            esac
            ;;
        get-id)
            # Complete with ticket IDs, person names, and file paths
            local ids people
            ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
            people=$(NO_DBOX_CHECK=1 vimban people list -f plain --no-header 2>/dev/null | awk '{print $1}')
            COMPREPLY=($(compgen -W "${ids} ${people}" -- "${cur}"))
            [[ ${#COMPREPLY[@]} -eq 0 ]] && COMPREPLY=($(compgen -f -- "${cur}"))
            ;;
        edit)
            case "${prev}" in
                edit)
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                -s|--status)
                    COMPREPLY=($(compgen -W "${statuses}" -- "${cur}"))
                    ;;
                -p|--priority)
                    COMPREPLY=($(compgen -W "${priorities}" -- "${cur}"))
                    ;;
                -a|--assignee)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-i --interactive -a --assignee -s --status -p --priority --add-tag --remove-tag --progress --due --clear --dry-run" -- "${cur}"))
                    ;;
            esac
            ;;
        move)
            case "${prev}" in
                move)
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                *)
                    # Check if we already have ticket ID
                    local has_ticket=false
                    for ((i=2; i < cword; i++)); do
                        if [[ "${words[i]}" =~ ^[A-Z]+-[0-9]+$ ]] || [[ "${words[i]}" =~ ^[0-9]+$ ]]
                        then
                            has_ticket=true
                            break
                        fi
                    done

                    if [[ "${has_ticket}" == "true" ]] && [[ "${prev}" != "--"* ]]
                    then
                        COMPREPLY=($(compgen -W "${statuses}" -- "${cur}"))
                    else
                        COMPREPLY=($(compgen -W "--force --comment --resolve --reopen" -- "${cur}"))
                    fi
                    ;;
            esac
            ;;
        link)
            case "${prev}" in
                link)
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                member_of|relates_to|blocked_by|blocks)
                    # Target ticket
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                *)
                    # Check if we need relation type
                    local has_relation=false
                    for rel in ${relations}
                    do
                        if [[ " ${words[*]} " =~ " ${rel} " ]]
                        then
                            has_relation=true
                            break
                        fi
                    done

                    if [[ "${has_relation}" == "false" ]]
                    then
                        COMPREPLY=($(compgen -W "${relations}" -- "${cur}"))
                    else
                        COMPREPLY=($(compgen -W "--remove --bidirectional --dry-run" -- "${cur}"))
                    fi
                    ;;
            esac
            ;;
        comment)
            case "${prev}" in
                comment)
                    # Complete with ticket IDs
                    local ids
                    ids=$(NO_DBOX_CHECK=1 vimban list -f plain --no-header 2>/dev/null | awk '{print $1}')
                    COMPREPLY=($(compgen -W "${ids}" -- "${cur}"))
                    ;;
                --reply-to)
                    # Complete with comment numbers (would need file context)
                    COMPREPLY=($(compgen -W "1 2 3 4 5" -- "${cur}"))
                    ;;
                --print|--print-full)
                    COMPREPLY=($(compgen -W "all 1 2 3" -- "${cur}"))
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--reply-to --print --print-full --new-id-output -e --edit --dry-run -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        comments)
            case "${prev}" in
                -u|--user)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-u --user --days --limit --reverse -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        dashboard)
            case "${prev}" in
                dashboard)
                    COMPREPLY=($(compgen -W "${dashboard_types}" -- "${cur}"))
                    ;;
                --section)
                    COMPREPLY=($(compgen -W "${sections}" -- "${cur}"))
                    ;;
                --person)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-o --output -P --project --person --sprint --section --markers -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        kanban)
            case "${prev}" in
                -s|--status)
                    COMPREPLY=($(compgen -W "${statuses}" -- "${cur}"))
                    ;;
                -a|--assignee)
                    local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                    if [[ -d "${pdir}" ]]
                    then
                        COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                    fi
                    ;;
                -f|--format)
                    COMPREPLY=($(compgen -W "${formats}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-P --project -a --assignee --mine -s --status --hide-empty --compact -w --width -f --format" -- "${cur}"))
                    ;;
            esac
            ;;
        search)
            case "${prev}" in
                --context)
                    COMPREPLY=($(compgen -W "1 2 3 5 10" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-E --regex -i --ignore-case -I --case-sensitive --body-only --frontmatter-only --context -l --files-only" -- "${cur}"))
                    ;;
            esac
            ;;
        validate)
            case "${prev}" in
                *)
                    COMPREPLY=($(compgen -W "--fix --strict --schema" -- "${cur}"))
                    ;;
            esac
            ;;
        report)
            case "${prev}" in
                report)
                    COMPREPLY=($(compgen -W "${report_types}" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "-P --project --sprint --from --to -o --output" -- "${cur}"))
                    ;;
            esac
            ;;
        sync)
            case "${prev}" in
                --provider)
                    COMPREPLY=($(compgen -W "jira monday" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--provider --dry-run --push --pull" -- "${cur}"))
                    ;;
            esac
            ;;
        commit)
            COMPREPLY=($(compgen -W "-m --message --no-pull --no-push --dry-run -A --all --pull" -- "${cur}"))
            ;;
        people)
            local pcmd=""
            for ((i=2; i < cword; i++)); do
                [[ "${words[i]}" != -* ]] && { pcmd="${words[i]}"; break; }
            done
            case "${pcmd}" in
                "")
                    COMPREPLY=($(compgen -W "${people_cmds}" -- "${cur}"))
                    ;;
                show|edit|dashboard)
                    case "${prev}" in
                        show|edit|dashboard)
                            local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                            if [[ -d "${pdir}" ]]
                            then
                                COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                            fi
                            ;;
                        --section)
                            COMPREPLY=($(compgen -W "${sections}" -- "${cur}"))
                            ;;
                        *)
                            if [[ "${pcmd}" == "show" ]]
                            then
                                COMPREPLY=($(compgen -W "--tickets --raw -f --format" -- "${cur}"))
                            elif [[ "${pcmd}" == "dashboard" ]]
                            then
                                COMPREPLY=($(compgen -W "--section --update --all -f --format" -- "${cur}"))
                            fi
                            # edit has no additional options
                            ;;
                    esac
                    ;;
                list)
                    case "${prev}" in
                        --team)
                            # Could populate with known teams
                            ;;
                        *)
                            COMPREPLY=($(compgen -W "--team --has-blocked --has-overdue -f --format" -- "${cur}"))
                            ;;
                    esac
                    ;;
                create)
                    case "${prev}" in
                        --manager)
                            local pdir="${VIMBAN_DIR:-${HOME}/Documents/notes}/${VIMBAN_PEOPLE_DIR:-02_areas/work/people}"
                            if [[ -d "${pdir}" ]]
                            then
                                COMPREPLY=($(compgen -W "$(find "${pdir}" -name '*.md' -exec basename {} .md \;)" -- "${cur}"))
                            fi
                            ;;
                        *)
                            COMPREPLY=($(compgen -W "--email --role --team --manager --no-edit" -- "${cur}"))
                            ;;
                    esac
                    ;;
            esac
            ;;
        init)
            case "${prev}" in
                *)
                    COMPREPLY=($(compgen -W "-p --prefix --people-dir --no-git --force" -- "${cur}"))
                    ;;
            esac
            ;;
        completion)
            case "${prev}" in
                completion)
                    COMPREPLY=($(compgen -W "bash" -- "${cur}"))
                    ;;
            esac
            ;;
        tui)
            case "${prev}" in
                --layout)
                    COMPREPLY=($(compgen -W "kanban list split" -- "${cur}"))
                    ;;
                --view)
                    COMPREPLY=($(compgen -W "tickets people kanban dashboard reports" -- "${cur}"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "--layout --view" -- "${cur}"))
                    ;;
            esac
            ;;
    esac

    # Add global options to any command
    if [[ "${cur}" == -* ]]
    then
        COMPREPLY+=($(compgen -W "-d --directory -f --format -q --quiet -v --verbose --no-color --work --personal -h --help --version --license" -- "${cur}"))
    fi
}

complete -F _vimban_completions vimban
'''

# Status workflow
STATUSES: list[str] = [
    "backlog", "ready", "in_progress", "blocked",
    "review", "delegated", "done", "cancelled"
]

VALID_TRANSITIONS: dict[str, list[str]] = {
    "backlog": ["ready", "in_progress", "blocked", "review", "delegated", "done", "cancelled"],
    "ready": ["backlog", "in_progress", "blocked", "review", "delegated", "done", "cancelled"],
    "in_progress": ["backlog", "ready", "blocked", "review", "delegated", "done", "cancelled"],
    "blocked": ["backlog", "ready", "in_progress", "review", "delegated", "done", "cancelled"],
    "review": ["backlog", "ready", "in_progress", "blocked", "delegated", "done", "cancelled"],
    "delegated": ["backlog", "ready", "in_progress", "blocked", "review", "done", "cancelled"],
    "done": ["backlog", "ready", "in_progress", "blocked", "review", "delegated", "cancelled"],
    "cancelled": ["backlog", "ready", "in_progress", "blocked", "review", "delegated", "done"],
}

TICKET_TYPES: list[str] = ["epic", "story", "task", "sub-task", "research", "bug"]
PRIORITIES: list[str] = ["critical", "high", "medium", "low"]

# PARA types (non-project types for areas and resources)
PARA_TYPES: list[str] = ["area", "resource"]
# Note: ALL_TYPES is defined after SPECIALIZED_PARA_TYPES below

# Separate ID prefixes for PARA types
PARA_PREFIXES: dict[str, str] = {
    "area": "AREA",
    "resource": "RESOURCE"
}

# Prefixes for ticket types
TICKET_PREFIXES: dict[str, str] = {
    "epic": "PROJ",
    "story": "PROJ",
    "task": "PROJ",
    "sub-task": "PROJ",
    "research": "RESEARCH",
    "bug": "BUG",
    "person": "PERSON",
}

# Sequence files for each type category
SEQUENCE_FILE_AREA: str = ".sequence_area"
SEQUENCE_FILE_RESOURCE: str = ".sequence_resource"
SEQUENCE_FILE_PERSON: str = ".sequence_person"
SEQUENCE_FILE_RESEARCH: str = ".sequence_research"
SEQUENCE_FILE_BUG: str = ".sequence_bug"


# ============================================================================
# SPECIALIZED PARA TYPES
# ============================================================================
@dataclass
class SpecializedTypeConfig:
    """
    Configuration for a specialized PARA type.

    Specialized types inherit from base PARA types (area/resource) but have
    predefined behaviors and directory structures.
    """
    name: str
    parent: str  # "area" or "resource"
    prefix: str  # ID prefix (e.g., "MTG")
    base_topic: str  # Base directory under PARA folder
    sequence_file: str  # Sequence file name
    has_status: bool  # Whether type uses status field
    template_name: str  # Template filename


class SpecializedType(ABC):
    """
    Abstract base class for specialized PARA types.

    Subclass this to create new specialized types. Each type defines:
    - Configuration via get_config()
    - Path computation via get_base_path()
    - Filename generation via get_filename()
    - Title handling via get_title()
    - Template placeholders via get_template_replacements()
    """

    @classmethod
    @abstractmethod
    def get_config(cls) -> SpecializedTypeConfig:
        """Return configuration for this type."""
        pass

    @classmethod
    def get_base_path(cls, config_dir: Path, topic: Optional[str] = None) -> Path:
        """
        Compute base directory path for this type.

        Override in subclasses for special path logic (e.g., date-based paths).

        Args:
            config_dir: Base notes directory
            topic: Optional topic subdirectory from --topic flag

        Returns:
            Full path to the base directory
        """
        type_config = cls.get_config()
        parent_dir = "02_areas" if type_config.parent == "area" else "03_resources"
        base = config_dir / parent_dir / type_config.base_topic
        if topic:
            base = base / topic
        return base

    @classmethod
    def get_filename(cls, title: str, **kwargs) -> str:
        """
        Generate filename for this type.

        Override in subclasses for special naming patterns.

        Args:
            title: The title to use for filename
            **kwargs: Additional arguments (e.g., date for meetings)

        Returns:
            Filename with .md extension
        """
        safe_title = re.sub(r'[^\w\s-]', '', title).strip().lower()
        safe_title = re.sub(r'[-\s]+', '_', safe_title)[:50]
        return f"{safe_title}.md"

    @classmethod
    def get_title(cls, title: Optional[str], **kwargs) -> str:
        """
        Return title to use. Override for types with default titles.

        Args:
            title: User-provided title (may be None)
            **kwargs: Additional arguments

        Returns:
            Title to use for the item
        """
        return title or "Untitled"

    @classmethod
    def get_template_replacements(cls, **kwargs) -> dict[str, str]:
        """
        Return type-specific template replacements.

        Override to add custom placeholders.

        Args:
            **kwargs: Arguments from create command

        Returns:
            Dict mapping placeholders to values
        """
        return {}


class MeetingType(SpecializedType):
    """Meeting specialized type with date-based filenames."""

    @classmethod
    def get_config(cls) -> SpecializedTypeConfig:
        return SpecializedTypeConfig(
            name="meeting",
            parent="area",
            prefix="MTG",
            base_topic="work/meetings",
            sequence_file=".sequence_meeting",
            has_status=True,
            template_name="meeting.md",
        )

    @classmethod
    def get_filename(cls, title: str, **kwargs) -> str:
        """Filename format: {title}_{YYYYMMDD}.md"""
        meeting_date = kwargs.get("date")
        if meeting_date is None:
            meeting_date = datetime.now()
        elif isinstance(meeting_date, str):
            meeting_date = datetime.strptime(meeting_date, "%Y-%m-%d")

        date_str = meeting_date.strftime("%Y%m%d")
        safe_title = re.sub(r'[^\w\s-]', '', title).strip().lower()
        safe_title = re.sub(r'[-\s]+', '_', safe_title)[:50]
        return f"{safe_title}_{date_str}.md"

    @classmethod
    def get_template_replacements(cls, **kwargs) -> dict[str, str]:
        meeting_date = kwargs.get("date")
        if meeting_date is None:
            meeting_date = datetime.now()
        elif isinstance(meeting_date, str):
            meeting_date = datetime.strptime(meeting_date, "%Y-%m-%d")

        return {
            "{{DATE}}": meeting_date.strftime("%Y-%m-%d"),
        }


class JournalType(SpecializedType):
    """Journal specialized type with date-based paths and filenames."""

    @classmethod
    def get_config(cls) -> SpecializedTypeConfig:
        return SpecializedTypeConfig(
            name="journal",
            parent="area",
            prefix="JNL",
            base_topic="personal/journal",
            sequence_file=".sequence_journal",
            has_status=True,
            template_name="journal.md",
        )

    @classmethod
    def get_base_path(cls, config_dir: Path, topic: Optional[str] = None) -> Path:
        """Add YYYY/MM to path for journals."""
        type_config = cls.get_config()
        now = datetime.now()
        year_month = now.strftime("%Y/%m")

        if topic:
            # Topic goes before date: personal/journal/{topic}/YYYY/MM
            return config_dir / "02_areas" / type_config.base_topic / topic / year_month
        return config_dir / "02_areas" / type_config.base_topic / year_month

    @classmethod
    def get_filename(cls, title: str, **kwargs) -> str:
        """Filename format: YYYY-MM-DD.md"""
        return datetime.now().strftime("%Y-%m-%d.md")

    @classmethod
    def get_title(cls, title: Optional[str], **kwargs) -> str:
        """Default title to today's date if not provided."""
        if title:
            return title
        return datetime.now().strftime("%Y-%m-%d")

    @classmethod
    def get_template_replacements(cls, **kwargs) -> dict[str, str]:
        return {
            "{{DATE}}": datetime.now().strftime("%Y-%m-%d"),
        }


class RecipeType(SpecializedType):
    """Recipe specialized type for cooking recipes."""

    @classmethod
    def get_config(cls) -> SpecializedTypeConfig:
        return SpecializedTypeConfig(
            name="recipe",
            parent="resource",
            prefix="RCP",
            base_topic="food_and_health/recipes",
            sequence_file=".sequence_recipe",
            has_status=False,
            template_name="recipe.md",
        )


class MentorshipType(SpecializedType):
    """Mentorship meeting type for tracking mentor/mentee sessions."""

    @classmethod
    def get_config(cls) -> SpecializedTypeConfig:
        return SpecializedTypeConfig(
            name="mentorship",
            parent="area",
            prefix="MNTR",
            base_topic="work/mentorship",
            sequence_file=".sequence_mentorship",
            has_status=True,
            template_name="mentorship.md",
        )

    @classmethod
    def get_base_path(cls, config_dir: Path, topic: Optional[str] = None, **kwargs) -> Path:
        """
        Compute base directory path for mentorship meetings.

        Creates person-specific subdirectories:
        02_areas/work/mentorship/{person_name}/

        Args:
            config_dir: Base notes directory
            topic: Optional topic subdirectory (unused for mentorship)
            **kwargs: Must include 'person_name'

        Returns:
            Full path including person subdirectory
        """
        type_config = cls.get_config()
        base = config_dir / "02_areas" / type_config.base_topic

        person_name = kwargs.get("person_name")
        if person_name:
            safe_name = re.sub(r'[^\w\s-]', '', person_name).strip().lower()
            safe_name = re.sub(r'[-\s]+', '_', safe_name)[:30]
            base = base / safe_name

        return base

    @classmethod
    def get_filename(cls, title: str, **kwargs) -> str:
        """
        Filename format: {YYYYMMDD}.md (person name is in directory path)

        Args:
            title: Meeting title (unused, kept for interface compatibility)
            **kwargs: Optionally 'date'

        Returns:
            Filename string like '20250113.md'
        """
        meeting_date = kwargs.get("date")
        if meeting_date is None:
            meeting_date = datetime.now()
        elif isinstance(meeting_date, str):
            meeting_date = datetime.strptime(meeting_date, "%Y-%m-%d")

        date_str = meeting_date.strftime("%Y%m%d")
        return f"{date_str}.md"

    @classmethod
    def get_template_replacements(cls, **kwargs) -> dict[str, str]:
        """
        Get template placeholder replacements for mentorship meetings.

        Args:
            **kwargs: Must include 'date' (optional, defaults to today)

        Returns:
            Dictionary of placeholder -> value mappings
        """
        meeting_date = kwargs.get("date")
        if meeting_date is None:
            meeting_date = datetime.now()
        elif isinstance(meeting_date, str):
            meeting_date = datetime.strptime(meeting_date, "%Y-%m-%d")

        return {
            "{{DATE}}": meeting_date.strftime("%Y-%m-%d"),
        }


# Registry of all specialized types
SPECIALIZED_TYPE_REGISTRY: dict[str, type[SpecializedType]] = {
    "meeting": MeetingType,
    "journal": JournalType,
    "recipe": RecipeType,
    "mentorship": MentorshipType,
}

# Convenience lists derived from registry
SPECIALIZED_PARA_TYPES: list[str] = list(SPECIALIZED_TYPE_REGISTRY.keys())
ALL_TYPES: list[str] = TICKET_TYPES + PARA_TYPES + SPECIALIZED_PARA_TYPES


def normalize_type_name(type_name: str) -> str:
    """
    Normalize type name to singular form.

    Supports plural forms like 'recipes' -> 'recipe', 'stories' -> 'story'.

    Args:
        type_name: Type name (possibly plural)

    Returns:
        Singular form of the type name
    """
    if not type_name:
        return type_name

    # Special cases for irregular plurals
    plurals: dict[str, str] = {
        'stories': 'story',
        'sub-tasks': 'sub-task',
    }
    if type_name in plurals:
        return plurals[type_name]

    # Standard -s suffix removal if singular form exists
    if type_name.endswith('s') and type_name[:-1] in ALL_TYPES:
        return type_name[:-1]

    return type_name


def get_specialized_type(type_name: str) -> Optional[type[SpecializedType]]:
    """Get the specialized type class by name."""
    return SPECIALIZED_TYPE_REGISTRY.get(normalize_type_name(type_name))


def is_specialized_type(type_name: str) -> bool:
    """Check if a type name is a specialized type."""
    return normalize_type_name(type_name) in SPECIALIZED_TYPE_REGISTRY


# Default columns for list output
DEFAULT_COLUMNS: list[str] = ["id", "status", "priority", "assignee", "title", "due_date"]

# Transclusion regex pattern: ![[path]] or ![[path|alias]]
TRANSCLUSION_PATTERN = re.compile(r'!\[\[([^\]|]+)(?:\|([^\]]+))?\]\]')

# Markdown link pattern: [text](path)
MARKDOWN_LINK_PATTERN = re.compile(r'\[([^\]]+)\]\(([^)]+)\)')

# Comment patterns (headers with quote block bodies)
# Optional 'by <author>' before timestamp: ### Comment #1 by clawdbot (2026-01-24...)
COMMENT_HEADER_PATTERN = re.compile(
    r'^### Comment #(\d+)(?: by ([^\(]+))? \(([^)]+)\)\s*$'
)
# Optional 'by <author>' before timestamp: #### Reply by clawdbot (2026-01-24...)
REPLY_HEADER_PATTERN = re.compile(
    r'^#### Reply(?: by ([^\(]+))? \(([^)]+)\)\s*$'
)


# ============================================================================
# DATA CLASSES
# ============================================================================
@dataclass
class Config:
    """
    Vimban project configuration.

    Stores configuration for a vimban-enabled directory including
    ID prefix, people directory location, and default values.
    """
    directory: Path
    prefix: str = DEFAULT_PREFIX
    people_dir: str = DEFAULT_PEOPLE_DIR
    default_status: str = "backlog"
    default_priority: str = "medium"

    @classmethod
    def load(cls, directory: Path) -> "Config":
        """
        Load config from .vimban/config.yaml or use defaults.

        Args:
            directory: The directory to load config from

        Returns:
            Config object with loaded or default values
        """
        config_file = directory / CONFIG_DIR_NAME / CONFIG_FILE
        if config_file.exists():
            with open(config_file, 'r') as f:
                data = yaml.safe_load(f) or {}
            return cls(
                directory=directory,
                prefix=data.get('prefix', DEFAULT_PREFIX),
                people_dir=data.get('people_dir', DEFAULT_PEOPLE_DIR),
                default_status=data.get('default_status', 'backlog'),
                default_priority=data.get('default_priority', 'medium'),
            )
        return cls(directory=directory)

    def save(self) -> None:
        """Save config to .vimban/config.yaml."""
        config_dir = self.directory / CONFIG_DIR_NAME
        config_dir.mkdir(parents=True, exist_ok=True)
        config_file = config_dir / CONFIG_FILE

        data = {
            'prefix': self.prefix,
            'people_dir': self.people_dir,
            'default_status': self.default_status,
            'default_priority': self.default_priority,
        }

        with open(config_file, 'w') as f:
            yaml.dump(data, f, default_flow_style=False)


@dataclass
class TransclusionLink:
    """
    Represents a ![[path]] or ![[path|alias]] transclusion.

    Transclusion links are used to reference other files in the
    knowledge base, particularly for people references and ticket
    relationships.
    """
    path: str
    alias: Optional[str] = None

    def __str__(self) -> str:
        if self.alias:
            return f"![[{self.path}|{self.alias}]]"
        return f"![[{self.path}]]"

    @classmethod
    def parse(cls, text: str) -> Optional["TransclusionLink"]:
        """
        Parse a transclusion or markdown link string.

        Supports both formats:
        - Transclusion: ![[path]] or ![[path|alias]]
        - Markdown: [text](path)

        Args:
            text: String potentially containing a link

        Returns:
            TransclusionLink if valid, None otherwise
        """
        if not text:
            return None

        text = text.strip()

        # Try transclusion format first: ![[path]] or ![[path|alias]]
        match = TRANSCLUSION_PATTERN.match(text)
        if match:
            return cls(path=match.group(1), alias=match.group(2))

        # Try markdown link format: [text](path)
        match = MARKDOWN_LINK_PATTERN.match(text)
        if match:
            # For markdown links, text becomes alias, path is the URL/path
            return cls(path=match.group(2), alias=match.group(1))

        return None


@dataclass
class Ticket:
    """
    Represents a ticket from markdown frontmatter.

    Contains all fields defined in the vimban ticket schema including
    required fields (id, title, type, status, created, filepath) and
    optional fields for dates, people, classification, relationships,
    and progress tracking.
    """
    # Required fields
    id: str
    title: str
    type: str
    status: str
    created: datetime
    filepath: Path

    # Dates
    start_date: Optional[date] = None
    due_date: Optional[date] = None
    end_date: Optional[date] = None

    # People (transclusion format)
    assignee: Optional[TransclusionLink] = None
    reporter: Optional[TransclusionLink] = None
    watchers: list[TransclusionLink] = field(default_factory=list)

    # Classification
    priority: str = "medium"
    effort: Optional[int] = None
    tags: list[str] = field(default_factory=list)
    project: Optional[str] = None
    sprint: Optional[str] = None

    # Relationships (transclusion format)
    member_of: list[TransclusionLink] = field(default_factory=list)
    relates_to: list[TransclusionLink] = field(default_factory=list)
    blocked_by: list[TransclusionLink] = field(default_factory=list)
    blocks: list[TransclusionLink] = field(default_factory=list)

    # Progress
    progress: int = 0
    checklist_total: int = 0
    checklist_done: int = 0

    # Metadata
    updated: Optional[datetime] = None
    version: int = 1

    # External system integration
    issue_link: Optional[str] = None

    @classmethod
    def from_file(cls, filepath: Path) -> "Ticket":
        """
        Load ticket from markdown file.

        Args:
            filepath: Path to the markdown file

        Returns:
            Ticket object populated from frontmatter

        Raises:
            ValueError: If required fields are missing
        """
        content = filepath.read_text()
        metadata, _ = parse_frontmatter(content)

        if not metadata:
            raise ValueError(f"No frontmatter found in {filepath}")

        # Parse required fields
        ticket_id = metadata.get('id', '').strip('"')
        title = metadata.get('title', '')
        ticket_type = metadata.get('type', '')
        status = metadata.get('status', '')
        created_str = metadata.get('created', '')

        # Check if status is required for this type
        # Resources and specialized types without status don't require it
        status_required = True
        if ticket_type == 'resource':
            status_required = False
        elif is_specialized_type(ticket_type):
            type_cls = get_specialized_type(ticket_type)
            if type_cls and not type_cls.get_config().has_status:
                status_required = False

        required_fields = [ticket_id, title, ticket_type, created_str]
        if status_required:
            required_fields.append(status)

        if not all(required_fields):
            raise ValueError(f"Missing required fields in {filepath}")

        # Parse created datetime
        if isinstance(created_str, datetime):
            created = created_str
        else:
            created = datetime.fromisoformat(str(created_str))

        # Parse optional date fields
        start_date = parse_date_field(metadata.get('start_date'))
        due_date = parse_date_field(metadata.get('due_date'))
        end_date = parse_date_field(metadata.get('end_date'))

        # Parse people fields
        assignee = TransclusionLink.parse(metadata.get('assignee', ''))
        reporter = TransclusionLink.parse(metadata.get('reporter', ''))
        watchers = [TransclusionLink.parse(w) for w in metadata.get('watchers', [])]
        watchers = [w for w in watchers if w]

        # Parse relationship fields
        member_of = [TransclusionLink.parse(m) for m in metadata.get('member_of', [])]
        member_of = [m for m in member_of if m]
        relates_to = [TransclusionLink.parse(r) for r in metadata.get('relates_to', [])]
        relates_to = [r for r in relates_to if r]
        blocked_by = [TransclusionLink.parse(b) for b in metadata.get('blocked_by', [])]
        blocked_by = [b for b in blocked_by if b]
        blocks = [TransclusionLink.parse(b) for b in metadata.get('blocks', [])]
        blocks = [b for b in blocks if b]

        # Parse updated datetime
        updated_str = metadata.get('updated')
        updated = None
        if updated_str:
            if isinstance(updated_str, datetime):
                updated = updated_str
            else:
                updated = datetime.fromisoformat(str(updated_str))

        return cls(
            id=ticket_id,
            title=title,
            type=ticket_type,
            status=status,
            created=created,
            filepath=filepath,
            start_date=start_date,
            due_date=due_date,
            end_date=end_date,
            assignee=assignee,
            reporter=reporter,
            watchers=watchers,
            priority=metadata.get('priority', 'medium'),
            effort=metadata.get('effort'),
            tags=metadata.get('tags', []),
            project=metadata.get('project'),
            sprint=metadata.get('sprint'),
            member_of=member_of,
            relates_to=relates_to,
            blocked_by=blocked_by,
            blocks=blocks,
            progress=metadata.get('progress', 0),
            checklist_total=metadata.get('checklist_total', 0),
            checklist_done=metadata.get('checklist_done', 0),
            updated=updated,
            version=metadata.get('version', 1),
            issue_link=metadata.get('issue_link'),
        )

    def to_frontmatter(self) -> dict:
        """
        Convert to frontmatter dict.

        Returns:
            Dictionary suitable for YAML frontmatter
        """
        data: dict[str, Any] = {
            'id': f'"{self.id}"',
            'title': self.title,
            'type': self.type,
            'status': self.status,
            'created': self.created.isoformat(),
        }

        # Dates
        if self.start_date:
            data['start_date'] = self.start_date.isoformat()
        if self.due_date:
            data['due_date'] = self.due_date.isoformat()
        if self.end_date:
            data['end_date'] = self.end_date.isoformat()

        # People
        if self.assignee:
            data['assignee'] = str(self.assignee)
        if self.reporter:
            data['reporter'] = str(self.reporter)
        if self.watchers:
            data['watchers'] = [str(w) for w in self.watchers]

        # Classification
        data['priority'] = self.priority
        if self.effort is not None:
            data['effort'] = self.effort
        if self.tags:
            data['tags'] = self.tags
        if self.project:
            data['project'] = self.project
        if self.sprint:
            data['sprint'] = self.sprint

        # Relationships
        if self.member_of:
            data['member_of'] = [str(m) for m in self.member_of]
        if self.relates_to:
            data['relates_to'] = [str(r) for r in self.relates_to]
        if self.blocked_by:
            data['blocked_by'] = [str(b) for b in self.blocked_by]
        if self.blocks:
            data['blocks'] = [str(b) for b in self.blocks]

        # Progress
        data['progress'] = self.progress
        if self.checklist_total:
            data['checklist_total'] = self.checklist_total
            data['checklist_done'] = self.checklist_done

        # Metadata
        data['updated'] = datetime.now().isoformat()
        data['version'] = self.version

        # External link
        if self.issue_link:
            data['issue_link'] = self.issue_link

        return data

    def to_dict(self) -> dict:
        """
        Convert to plain dictionary for output formatting.

        Returns:
            Dictionary with string values suitable for display
        """
        assignee_str = ""
        if self.assignee:
            # Extract just the filename without extension
            assignee_str = Path(self.assignee.path).stem

        return {
            'id': self.id,
            'title': self.title,
            'type': self.type,
            'status': self.status,
            'priority': self.priority,
            'assignee': assignee_str,
            'due_date': str(self.due_date) if self.due_date else '',
            'project': self.project or '',
            'tags': ','.join(self.tags) if self.tags else '',
            'progress': self.progress,
            'filepath': str(self.filepath),
            'issue_link': self.issue_link or '',
        }


@dataclass
class Person:
    """
    Represents a person from their markdown file.

    Used for tracking team members and their relationships,
    including manager/direct-report hierarchy.
    """
    name: str
    filepath: Path
    id: Optional[str] = None
    email: Optional[str] = None
    slack: Optional[str] = None
    role: Optional[str] = None
    team: Optional[str] = None
    manager: Optional[TransclusionLink] = None
    direct_reports: list[TransclusionLink] = field(default_factory=list)
    created: Optional[datetime] = None
    updated: Optional[datetime] = None

    @classmethod
    def from_file(cls, filepath: Path) -> "Person":
        """
        Load person from markdown file.

        Args:
            filepath: Path to the person's markdown file

        Returns:
            Person object populated from frontmatter
        """
        content = filepath.read_text()
        metadata, _ = parse_frontmatter(content)

        if not metadata:
            raise ValueError(f"No frontmatter found in {filepath}")

        person_id = metadata.get('id')
        name = metadata.get('name', filepath.stem.replace('_', ' ').title())

        manager = TransclusionLink.parse(metadata.get('manager', ''))
        direct_reports = [
            TransclusionLink.parse(d) for d in metadata.get('direct_reports', [])
        ]
        direct_reports = [d for d in direct_reports if d]

        created_str = metadata.get('created')
        created = None
        if created_str:
            if isinstance(created_str, datetime):
                created = created_str
            else:
                created = datetime.fromisoformat(str(created_str))

        updated_str = metadata.get('updated')
        updated = None
        if updated_str:
            if isinstance(updated_str, datetime):
                updated = updated_str
            else:
                updated = datetime.fromisoformat(str(updated_str))

        return cls(
            name=name,
            filepath=filepath,
            id=person_id,
            email=metadata.get('email'),
            slack=metadata.get('slack'),
            role=metadata.get('role'),
            team=metadata.get('team'),
            manager=manager,
            direct_reports=direct_reports,
            created=created,
            updated=updated,
        )

    def to_dict(self) -> dict:
        """
        Convert to plain dictionary for output formatting.

        Returns:
            Dictionary with string values suitable for display
        """
        return {
            'id': self.id or '',
            'name': self.name,
            'email': self.email or '',
            'role': self.role or '',
            'team': self.team or '',
            'filepath': str(self.filepath),
        }


@dataclass
class Comment:
    """
    Represents a comment on a ticket or person file.

    Comments support single-level threading where the parent comment
    can have multiple replies. Each comment has an auto-incremented ID
    and timestamp, with optional author attribution.
    """
    id: int
    timestamp: datetime
    content: str
    author: Optional[str] = None
    replies: list[tuple[datetime, str, Optional[str]]] = field(default_factory=list)

    def to_dict(self) -> dict:
        """
        Convert to plain dictionary for output formatting.

        Returns:
            Dictionary with string values suitable for display
        """
        result: dict = {
            'id': self.id,
            'timestamp': self.timestamp.isoformat(),
            'content': self.content,
            'replies': [
                {'timestamp': ts.isoformat(), 'content': c, 'author': a}
                for ts, c, a in self.replies
            ],
        }
        if self.author:
            result['author'] = self.author
        return result


# ============================================================================
# SYNC PROVIDER ABSTRACT CLASS (Extensibility)
# ============================================================================
class SyncProvider(ABC):
    """
    Abstract base class for external system integration.

    Implement this class to add sync capability with external
    ticket management systems like Jira, Monday.com, Linear, etc.

    The provider is responsible for:
    1. Authentication with the external system
    2. Mapping between vimban tickets and external tickets
    3. Bidirectional sync of ticket data
    4. Handling conflicts and merge strategies
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """Provider name (e.g., 'jira', 'monday')."""
        ...

    @property
    @abstractmethod
    def requires_auth(self) -> bool:
        """Whether this provider requires authentication."""
        ...

    @abstractmethod
    def authenticate(self, **kwargs) -> bool:
        """
        Authenticate with the external system.

        Returns True if authentication successful.
        Credentials should be sourced from environment variables or config.
        """
        ...

    @abstractmethod
    def fetch_tickets(
        self,
        project: Optional[str] = None,
        since: Optional[datetime] = None
    ) -> list[dict]:
        """
        Fetch tickets from external system.

        Returns list of ticket data dicts that can be converted to Ticket objects.
        """
        ...

    @abstractmethod
    def push_ticket(self, ticket: Ticket) -> Optional[str]:
        """
        Push a ticket to the external system.

        Returns the external system's ID for the ticket (to store in issue_link).
        Returns None if push failed.
        """
        ...

    @abstractmethod
    def update_ticket(self, ticket: Ticket) -> bool:
        """
        Update an existing ticket in the external system.

        Uses ticket.issue_link to identify the external ticket.
        Returns True if update successful.
        """
        ...

    @abstractmethod
    def map_status(self, vimban_status: str) -> str:
        """Map vimban status to external system status."""
        ...

    @abstractmethod
    def reverse_map_status(self, external_status: str) -> str:
        """Map external system status to vimban status."""
        ...

    def sync(
        self,
        directory: Path,
        project: Optional[str] = None,
        dry_run: bool = False
    ) -> dict:
        """
        Perform bidirectional sync.

        Default implementation:
        1. Fetch external tickets
        2. Compare with local tickets (using issue_link)
        3. Update local tickets with external changes
        4. Push local changes to external system

        Returns dict with sync statistics.
        """
        stats = {
            'fetched': 0,
            'created': 0,
            'updated': 0,
            'errors': 0,
        }
        # Subclasses should implement actual sync logic
        return stats


class JiraSyncProvider(SyncProvider):
    """Jira integration - not yet implemented."""

    @property
    def name(self) -> str:
        return "jira"

    @property
    def requires_auth(self) -> bool:
        return True

    def authenticate(self, **kwargs) -> bool:
        raise NotImplementedError("Jira sync not yet implemented")

    def fetch_tickets(
        self,
        project: Optional[str] = None,
        since: Optional[datetime] = None
    ) -> list[dict]:
        raise NotImplementedError("Jira sync not yet implemented")

    def push_ticket(self, ticket: Ticket) -> Optional[str]:
        raise NotImplementedError("Jira sync not yet implemented")

    def update_ticket(self, ticket: Ticket) -> bool:
        raise NotImplementedError("Jira sync not yet implemented")

    def map_status(self, vimban_status: str) -> str:
        raise NotImplementedError("Jira sync not yet implemented")

    def reverse_map_status(self, external_status: str) -> str:
        raise NotImplementedError("Jira sync not yet implemented")


class MondaySyncProvider(SyncProvider):
    """Monday.com integration - not yet implemented."""

    @property
    def name(self) -> str:
        return "monday"

    @property
    def requires_auth(self) -> bool:
        return True

    def authenticate(self, **kwargs) -> bool:
        raise NotImplementedError("Monday.com sync not yet implemented")

    def fetch_tickets(
        self,
        project: Optional[str] = None,
        since: Optional[datetime] = None
    ) -> list[dict]:
        raise NotImplementedError("Monday.com sync not yet implemented")

    def push_ticket(self, ticket: Ticket) -> Optional[str]:
        raise NotImplementedError("Monday.com sync not yet implemented")

    def update_ticket(self, ticket: Ticket) -> bool:
        raise NotImplementedError("Monday.com sync not yet implemented")

    def map_status(self, vimban_status: str) -> str:
        raise NotImplementedError("Monday.com sync not yet implemented")

    def reverse_map_status(self, external_status: str) -> str:
        raise NotImplementedError("Monday.com sync not yet implemented")


# Provider registry
SYNC_PROVIDERS: dict[str, type[SyncProvider]] = {
    "jira": JiraSyncProvider,
    "monday": MondaySyncProvider,
}


# ============================================================================
# OUTPUT FORMATTING & COLORS
# ============================================================================
class Colors:
    """ANSI color codes with --no-color support."""

    def __init__(self, enabled: bool = True):
        self.enabled = enabled

    @property
    def HEADER(self) -> str:
        return '\033[95m' if self.enabled else ''

    @property
    def BLUE(self) -> str:
        return '\033[94m' if self.enabled else ''

    @property
    def CYAN(self) -> str:
        return '\033[96m' if self.enabled else ''

    @property
    def GREEN(self) -> str:
        return '\033[92m' if self.enabled else ''

    @property
    def YELLOW(self) -> str:
        return '\033[93m' if self.enabled else ''

    @property
    def RED(self) -> str:
        return '\033[91m' if self.enabled else ''

    @property
    def BOLD(self) -> str:
        return '\033[1m' if self.enabled else ''

    @property
    def DIM(self) -> str:
        return '\033[2m' if self.enabled else ''

    @property
    def END(self) -> str:
        return '\033[0m' if self.enabled else ''

    def status_color(self, status: str) -> str:
        """Get color for a status."""
        status_colors = {
            'backlog': self.DIM,
            'ready': self.BLUE,
            'in_progress': self.CYAN,
            'blocked': self.RED,
            'review': self.YELLOW,
            'delegated': self.HEADER,
            'done': self.GREEN,
            'cancelled': self.DIM,
        }
        return status_colors.get(status, '')

    def priority_color(self, priority: str) -> str:
        """Get color for a priority."""
        priority_colors = {
            'critical': self.RED,
            'high': self.YELLOW,
            'medium': '',
            'low': self.DIM,
        }
        return priority_colors.get(priority, '')


def format_output(
    data: Any,
    fmt: str,
    columns: Optional[list[str]] = None,
    colors: Optional[Colors] = None,
    no_header: bool = False
) -> str:
    """
    Format data for output.

    Args:
        data: Data to format (list of dicts, single dict, or Ticket/Person objects)
        fmt: Output format ('plain', 'md', 'yaml', 'json')
        columns: Columns to include (for plain/md table formats)
        colors: Colors instance for plain output
        no_header: Whether to omit header row

    Returns:
        Formatted string
    """
    if colors is None:
        colors = Colors(enabled=True)

    # Convert objects to dicts
    if isinstance(data, (Ticket, Person)):
        data = [data.to_dict()]
    elif isinstance(data, list) and len(data) > 0:
        if isinstance(data[0], Ticket):
            data = [t.to_dict() for t in data]
        elif isinstance(data[0], Person):
            data = [p.to_dict() for p in data]

    # Ensure we have a list
    if isinstance(data, dict):
        data = [data]

    if not data:
        return ""

    # Use default columns if not specified
    if columns is None:
        columns = DEFAULT_COLUMNS

    if fmt == 'json':
        return json.dumps(data, indent=2, default=str)

    elif fmt == 'yaml':
        return yaml.dump(data, default_flow_style=False, allow_unicode=True)

    elif fmt == 'md':
        # Markdown table format
        lines = []
        if not no_header:
            lines.append('| ' + ' | '.join(columns) + ' |')
            lines.append('|' + '|'.join(['---'] * len(columns)) + '|')

        for item in data:
            row = []
            for col in columns:
                val = str(item.get(col, ''))
                # Escape pipe characters in markdown
                val = val.replace('|', '\\|')
                row.append(val)
            lines.append('| ' + ' | '.join(row) + ' |')

        return '\n'.join(lines)

    else:  # plain format
        lines = []

        # Calculate column widths
        widths = {col: len(col) for col in columns}
        for item in data:
            for col in columns:
                val = str(item.get(col, ''))
                widths[col] = max(widths[col], len(val))

        # Header
        if not no_header:
            header_parts = []
            for col in columns:
                header_parts.append(f"{colors.BOLD}{col.upper():<{widths[col]}}{colors.END}")
            lines.append('  '.join(header_parts))

        # Data rows
        for item in data:
            row_parts = []
            for col in columns:
                val = str(item.get(col, ''))

                # Apply color based on column
                color = ''
                if col == 'status':
                    color = colors.status_color(val)
                elif col == 'priority':
                    color = colors.priority_color(val)

                if color:
                    row_parts.append(f"{color}{val:<{widths[col]}}{colors.END}")
                else:
                    row_parts.append(f"{val:<{widths[col]}}")

            lines.append('  '.join(row_parts))

        return '\n'.join(lines)


def format_kanban(
    tickets: list[Ticket],
    fmt: str = 'plain',
    colors: Optional[Colors] = None,
    hide_empty: bool = False,
    compact: bool = False,
    column_width: Optional[int] = None,
    statuses: Optional[list[str]] = None
) -> str:
    """
    Format tickets as a kanban board grouped by status.

    Args:
        tickets: List of tickets to display
        fmt: Output format ('plain', 'md', 'yaml', 'json')
        colors: Colors instance for plain output
        hide_empty: Whether to hide empty columns
        compact: Whether to use compact card display
        column_width: Column width for plain output (auto if None)
        statuses: List of statuses to display (default: all active statuses)

    Returns:
        Formatted kanban board string
    """
    if colors is None:
        colors = Colors(enabled=True)

    # Default to active statuses (exclude done/cancelled unless specified)
    if statuses is None:
        statuses = ['backlog', 'ready', 'in_progress', 'blocked', 'review', 'delegated']

    # Group tickets by status
    board: dict[str, list[Ticket]] = {status: [] for status in statuses}
    for ticket in tickets:
        if ticket.status in board:
            board[ticket.status].append(ticket)

    # Remove empty columns if requested
    if hide_empty:
        board = {s: t for s, t in board.items() if t}

    if not board:
        return "No tickets found"

    # JSON output
    if fmt == 'json':
        data = {}
        for status, status_tickets in board.items():
            data[status] = [t.to_dict() for t in status_tickets]
        return json.dumps(data, indent=2, default=str)

    # YAML output
    if fmt == 'yaml':
        data = {}
        for status, status_tickets in board.items():
            data[status] = [t.to_dict() for t in status_tickets]
        return yaml.dump(data, default_flow_style=False, allow_unicode=True)

    # Markdown output
    if fmt == 'md':
        lines = []
        lines.append("# Kanban Board\n")
        for status, status_tickets in board.items():
            lines.append(f"## {status.upper().replace('_', ' ')} ({len(status_tickets)})\n")
            if status_tickets:
                if compact:
                    for t in status_tickets:
                        lines.append(f"- **{t.id}**: {t.title}")
                else:
                    lines.append("| ID | Title | Priority | Assignee | Due |")
                    lines.append("|---|---|---|---|---|")
                    for t in status_tickets:
                        assignee = Path(t.assignee.path).stem if t.assignee else ''
                        due = str(t.due_date) if t.due_date else ''
                        title = t.title[:40] + '...' if len(t.title) > 40 else t.title
                        lines.append(f"| {t.id} | {title} | {t.priority} | {assignee} | {due} |")
            else:
                lines.append("_Empty_")
            lines.append("")
        return '\n'.join(lines)

    # Plain text output (terminal kanban board)
    # Calculate column width
    if column_width is None:
        try:
            import shutil
            term_width = shutil.get_terminal_size().columns
        except Exception:
            term_width = 120
        num_cols = len(board)
        column_width = max(18, (term_width - (num_cols - 1) * 2) // num_cols) if num_cols > 0 else 20

    # Find max tickets in any column
    max_tickets = max(len(t) for t in board.values()) if board else 0

    lines = []

    # Header row
    header_parts = []
    for status in board.keys():
        count = len(board[status])
        label = f"{status.upper().replace('_', ' ')} ({count})"
        color = colors.status_color(status)
        header_parts.append(f"{color}{colors.BOLD}{label:<{column_width}}{colors.END}")
    lines.append("  ".join(header_parts))

    # Separator
    sep_parts = ["\u2500" * column_width for _ in board]
    lines.append("  ".join(sep_parts))

    # Ticket rows
    for row_idx in range(max_tickets):
        # Each ticket takes 3 lines in normal mode, 1 in compact
        if compact:
            row_parts = []
            for status in board.keys():
                status_tickets = board[status]
                if row_idx < len(status_tickets):
                    t = status_tickets[row_idx]
                    card = f"{t.id}"[:column_width]
                    row_parts.append(f"{card:<{column_width}}")
                else:
                    row_parts.append(" " * column_width)
            lines.append("  ".join(row_parts))
        else:
            # Line 1: ID
            row_parts = []
            for status in board.keys():
                status_tickets = board[status]
                if row_idx < len(status_tickets):
                    t = status_tickets[row_idx]
                    row_parts.append(f"{colors.BOLD}{t.id:<{column_width}}{colors.END}")
                else:
                    row_parts.append(" " * column_width)
            lines.append("  ".join(row_parts))

            # Line 2: Title (truncated)
            row_parts = []
            for status in board.keys():
                status_tickets = board[status]
                if row_idx < len(status_tickets):
                    t = status_tickets[row_idx]
                    title = t.title[:column_width - 1] if len(t.title) >= column_width else t.title
                    row_parts.append(f"{title:<{column_width}}")
                else:
                    row_parts.append(" " * column_width)
            lines.append("  ".join(row_parts))

            # Line 3: Priority and assignee
            row_parts = []
            for status in board.keys():
                status_tickets = board[status]
                if row_idx < len(status_tickets):
                    t = status_tickets[row_idx]
                    pri_color = colors.priority_color(t.priority)
                    assignee = '@' + Path(t.assignee.path).stem[:8] if t.assignee else ''
                    meta = f"[{t.priority[:4]}] {assignee}"[:column_width]
                    row_parts.append(f"{pri_color}{meta:<{column_width}}{colors.END}")
                else:
                    row_parts.append(" " * column_width)
            lines.append("  ".join(row_parts))

            # Separator between cards
            if row_idx < max_tickets - 1:
                sep_parts = ["\u2500" * column_width for _ in board]
                lines.append("  ".join(sep_parts))

    return '\n'.join(lines)


# ============================================================================
# ERROR HANDLING
# ============================================================================
def error(msg: str, code: int = EXIT_GENERAL_ERROR) -> None:
    """Print error message and exit."""
    print(f"vimban: error: {msg}", file=sys.stderr)
    sys.exit(code)


def warn(msg: str) -> None:
    """Print warning message to stderr."""
    print(f"vimban: warning: {msg}", file=sys.stderr)


def info(msg: str, verbose: bool = False) -> None:
    """Print info message (only if verbose)."""
    if verbose:
        print(f"vimban: {msg}", file=sys.stderr)


# ============================================================================
# FRONTMATTER HANDLING
# ============================================================================
def parse_frontmatter(content: str) -> tuple[dict, str]:
    """
    Parse YAML frontmatter from markdown content.

    Args:
        content: Full file content

    Returns:
        Tuple of (metadata dict, body content)
    """
    if not content.startswith('---'):
        return {}, content

    try:
        # Find the closing ---
        end_idx = content.index('---', 3)
        yaml_content = content[3:end_idx].strip()
        body = content[end_idx + 3:].strip()

        metadata = yaml.safe_load(yaml_content) or {}
        return metadata, body
    except (ValueError, yaml.YAMLError):
        return {}, content


def dump_frontmatter(metadata: dict, body: str) -> str:
    """
    Create markdown content with YAML frontmatter.

    Args:
        metadata: Frontmatter dict
        body: Body content

    Returns:
        Full markdown content
    """
    yaml_str = yaml.dump(
        metadata,
        default_flow_style=False,
        allow_unicode=True,
        sort_keys=False
    )
    return f"---\n{yaml_str}---\n\n{body}"


def update_frontmatter_field(
    filepath: Path,
    field: str,
    value: Any,
    increment_version: bool = True
) -> None:
    """
    Update a single field in a file's frontmatter.

    Also updates the 'updated' timestamp and optionally increments version.

    Args:
        filepath: Path to the markdown file
        field: Field name to update
        value: New value for the field
        increment_version: Whether to increment the version number
    """
    content = filepath.read_text()
    metadata, body = parse_frontmatter(content)

    metadata[field] = value
    metadata['updated'] = datetime.now().isoformat()

    if increment_version:
        metadata['version'] = metadata.get('version', 0) + 1

    filepath.write_text(dump_frontmatter(metadata, body))


def find_files_referencing_path(
    old_path: str,
    config: Config,
    exclude_file: Optional[Path] = None
) -> list[tuple[Path, str]]:
    """
    Find all markdown files that reference a given path.

    Searches both frontmatter fields and body content for transclusion
    links (![[path]]) and markdown links ([text](path)).

    Args:
        old_path: Relative path to search for (e.g., "01_projects/task.md")
        config: Config instance with directory info
        exclude_file: File to exclude from search (typically the file being moved)

    Returns:
        List of tuples (filepath, location) where location is either a
        frontmatter field name or "body"
    """
    # Fields that can contain file references
    reference_fields: list[str] = [
        'member_of', 'relates_to', 'blocked_by', 'blocks',
        'assignee', 'reporter', 'watchers',
        'manager', 'direct_reports', 'children'
    ]

    # Patterns to match in content
    transclusion_pattern: str = rf'!\[\[{re.escape(old_path)}(?:\|[^\]]+)?\]\]'
    markdown_link_pattern: str = rf'\[[^\]]*\]\({re.escape(old_path)}\)'

    results: list[tuple[Path, str]] = []

    # Scan all markdown files
    for md_file in config.directory.rglob("*.md"):
        if exclude_file and md_file.resolve() == exclude_file.resolve():
            continue

        try:
            content: str = md_file.read_text()
            metadata, body = parse_frontmatter(content)

            # Check frontmatter fields
            for field in reference_fields:
                field_value = metadata.get(field)
                if field_value is None:
                    continue

                # Handle both single values and lists
                values: list = field_value if isinstance(field_value, list) else [field_value]

                for val in values:
                    if isinstance(val, str) and old_path in val:
                        results.append((md_file, field))
                        break

            # Check body content
            if re.search(transclusion_pattern, body) or re.search(markdown_link_pattern, body):
                results.append((md_file, "body"))

        except (OSError, ValueError, yaml.YAMLError):
            # Skip files that can't be read or parsed
            continue

    return results


def update_path_references(
    filepath: Path,
    old_path: str,
    new_path: str,
    location: str
) -> bool:
    """
    Update references from old_path to new_path in a file.

    Handles both frontmatter fields and body content.

    Args:
        filepath: Path to the file to update
        old_path: Old relative path to replace
        new_path: New relative path to use
        location: Either a frontmatter field name or "body"

    Returns:
        True if file was modified, False otherwise
    """
    try:
        content: str = filepath.read_text()
        metadata, body = parse_frontmatter(content)
        modified: bool = False

        if location == "body":
            # Replace in body content
            # Handle transclusion links: ![[old_path]] and ![[old_path|alias]]
            new_body: str = re.sub(
                rf'!\[\[{re.escape(old_path)}(\|[^\]]+)?\]\]',
                rf'![[{new_path}\1]]',
                body
            )
            # Handle markdown links: [text](old_path)
            new_body = re.sub(
                rf'\[([^\]]*)\]\({re.escape(old_path)}\)',
                rf'[\1]({new_path})',
                new_body
            )

            if new_body != body:
                body = new_body
                modified = True
        else:
            # Update frontmatter field
            field_value = metadata.get(location)
            if field_value is None:
                return False

            if isinstance(field_value, list):
                new_values: list[str] = []
                for val in field_value:
                    if isinstance(val, str) and old_path in val:
                        new_val: str = val.replace(old_path, new_path)
                        new_values.append(new_val)
                        modified = True
                    else:
                        new_values.append(val)
                metadata[location] = new_values
            elif isinstance(field_value, str) and old_path in field_value:
                metadata[location] = field_value.replace(old_path, new_path)
                modified = True

        if modified:
            metadata['updated'] = datetime.now().isoformat()
            metadata['version'] = metadata.get('version', 0) + 1
            filepath.write_text(dump_frontmatter(metadata, body))

        return modified

    except (OSError, ValueError, yaml.YAMLError) as e:
        print(f"Warning: Could not update {filepath}: {e}", file=sys.stderr)
        return False


def parse_date_field(value: Any) -> Optional[date]:
    """
    Parse a date field from frontmatter.

    Args:
        value: Date value (string, date, datetime, or None)

    Returns:
        date object or None
    """
    if value is None:
        return None
    if isinstance(value, date):
        return value
    if isinstance(value, datetime):
        return value.date()
    if isinstance(value, str) and value:
        try:
            return date.fromisoformat(value)
        except ValueError:
            return None
    return None


# ============================================================================
# DATE PARSING
# ============================================================================
def parse_date(date_str: str) -> Optional[date]:
    """
    Parse a date string supporting absolute and relative formats.

    Supports:
    - ISO format: 2025-12-25
    - Relative: +7d, +2w
    - Named days: today, tomorrow, monday, tuesday, etc.

    Args:
        date_str: Date string to parse

    Returns:
        date object or None if invalid
    """
    if not date_str:
        return None

    date_str = date_str.lower().strip()
    today = date.today()

    # Handle special names
    if date_str == 'today':
        return today
    elif date_str == 'tomorrow':
        return today + timedelta(days=1)

    # Handle relative dates (+7d, +2w)
    relative_match = re.match(r'\+(\d+)([dwm])', date_str)
    if relative_match:
        num = int(relative_match.group(1))
        unit = relative_match.group(2)
        if unit == 'd':
            return today + timedelta(days=num)
        elif unit == 'w':
            return today + timedelta(weeks=num)
        elif unit == 'm':
            return today + timedelta(days=num * 30)  # Approximate

    # Handle day names (monday, tuesday, etc.)
    day_names = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday']
    if date_str in day_names:
        target_day = day_names.index(date_str)
        current_day = today.weekday()
        days_ahead = target_day - current_day
        if days_ahead <= 0:
            days_ahead += 7
        return today + timedelta(days=days_ahead)

    # Try ISO format
    try:
        return date.fromisoformat(date_str)
    except ValueError:
        return None


# ============================================================================
# ID GENERATION
# ============================================================================
def next_id(
    config_dir: Path,
    custom_id: Optional[str] = None,
    prefix: Optional[str] = None,
    ticket_type: Optional[str] = None
) -> str:
    """
    Generate next ticket ID with file-based locking.

    Args:
        config_dir: Path to .vimban directory
        custom_id: Custom ID to use (bypasses sequence)
        prefix: Custom prefix (default: from config)
        ticket_type: Type of ticket (area/resource use separate sequences)

    Returns:
        New ticket ID (e.g., "PROJ-00042", "AREA-00001", "RESOURCE-00001")
    """
    if custom_id:
        return custom_id

    # Determine sequence file and prefix based on type
    # Check specialized types first (via registry)
    if ticket_type and is_specialized_type(ticket_type):
        type_cls = get_specialized_type(ticket_type)
        type_config = type_cls.get_config()
        sequence_file = config_dir / type_config.sequence_file
        prefix = prefix or type_config.prefix
    elif ticket_type == "area":
        sequence_file = config_dir / SEQUENCE_FILE_AREA
        prefix = prefix or PARA_PREFIXES["area"]
    elif ticket_type == "resource":
        sequence_file = config_dir / SEQUENCE_FILE_RESOURCE
        prefix = prefix or PARA_PREFIXES["resource"]
    elif ticket_type == "person":
        sequence_file = config_dir / SEQUENCE_FILE_PERSON
        prefix = prefix or TICKET_PREFIXES["person"]
    elif ticket_type == "research":
        sequence_file = config_dir / SEQUENCE_FILE_RESEARCH
        prefix = prefix or TICKET_PREFIXES["research"]
    elif ticket_type == "bug":
        sequence_file = config_dir / SEQUENCE_FILE_BUG
        prefix = prefix or TICKET_PREFIXES["bug"]
    else:
        # Default: epic, story, task, sub-task share .sequence with PROJ- prefix
        sequence_file = config_dir / SEQUENCE_FILE
        prefix = prefix or TICKET_PREFIXES.get(ticket_type, DEFAULT_PREFIX)

    # Ensure sequence file exists
    if not sequence_file.exists():
        sequence_file.write_text('0')

    # Use file locking for concurrent access safety
    # Lock is automatically released when file closes (no explicit unlock needed)
    with open(sequence_file, 'r+') as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        f.seek(0)
        current = int(f.read().strip() or '0')
        next_num = current + 1
        f.seek(0)
        f.truncate()
        f.write(str(next_num))
        f.flush()
        os.fsync(f.fileno())

    return f"{prefix}-{next_num:05d}"


# ============================================================================
# KRAFNA INTEGRATION
# ============================================================================
def krafna_query(query: str, directory: Path) -> list[dict]:
    """
    Execute a Krafna query.

    Args:
        query: Krafna query string
        directory: Directory to search in

    Returns:
        List of result dicts
    """
    try:
        result = subprocess.run(
            ["krafna", "--json", "--from", str(directory), query],
            capture_output=True,
            text=True,
            check=True
        )
        return json.loads(result.stdout) if result.stdout else []
    except subprocess.CalledProcessError as e:
        error(f"Krafna query failed: {e.stderr}", EXIT_KRAFNA_ERROR)
    except json.JSONDecodeError as e:
        error(f"Failed to parse Krafna output: {e}", EXIT_KRAFNA_ERROR)
    except FileNotFoundError:
        # Krafna not installed, fall back to manual scan
        warn("krafna not found, using fallback search")
        return []
    return []


def fallback_list_tickets(
    directory: Path,
    filters: dict,
    exclude_types: Optional[list[str]] = None,
    include_archived: bool = False
) -> list[Ticket]:
    """
    List tickets without Krafna (fallback method).

    Scans markdown files and filters based on frontmatter.

    Args:
        directory: Directory to search
        filters: Dictionary of filter criteria
        exclude_types: List of types to exclude (default: ['person'])
        include_archived: If False, exclude files under 04_archives/ (default: False)

    Returns:
        List of matching Ticket objects
    """
    if exclude_types is None:
        exclude_types = ['person']

    tickets = []

    # Find all markdown files
    for md_file in directory.rglob('*.md'):
        # Skip archived files unless explicitly included
        if not include_archived and '/04_archives/' in str(md_file):
            continue
        try:
            content = md_file.read_text()
            metadata, _ = parse_frontmatter(content)

            # Skip files without vimban frontmatter
            if not metadata.get('id') or not metadata.get('type'):
                continue

            # Skip excluded types
            if metadata.get('type') in exclude_types:
                continue

            ticket = Ticket.from_file(md_file)

            # Apply filters
            if filters.get('status'):
                statuses = filters['status'].split(',')
                if ticket.status not in statuses:
                    continue

            if filters.get('type'):
                # Normalize type names for comparison (handles plurals)
                types = [normalize_type_name(t.strip()) for t in filters['type'].split(',')]
                if ticket.type not in types:
                    continue

            if filters.get('priority'):
                if ticket.priority != filters['priority']:
                    continue

            if filters.get('project'):
                if ticket.project != filters['project']:
                    continue

            if filters.get('assignee'):
                if not ticket.assignee:
                    continue
                if filters['assignee'].lower() not in str(ticket.assignee).lower():
                    continue

            tickets.append(ticket)

        except (ValueError, yaml.YAMLError):
            # Skip files that can't be parsed
            continue

    return tickets


# ============================================================================
# SECTION MARKERS (for vim !! integration)
# ============================================================================
SECTION_PATTERN = re.compile(
    r'(<!-- VIMBAN:(\w+):START -->)(.*?)(<!-- VIMBAN:\2:END -->)',
    re.DOTALL
)


def extract_section(content: str, section: str) -> Optional[str]:
    """
    Extract content between VIMBAN section markers.

    Args:
        content: Full file content
        section: Section name (e.g., 'DASHBOARD', '1ON1')

    Returns:
        Content between markers or None if not found
    """
    pattern = re.compile(
        rf'<!-- VIMBAN:{section}:START -->(.*?)<!-- VIMBAN:{section}:END -->',
        re.DOTALL
    )
    match = pattern.search(content)
    return match.group(1).strip() if match else None


def replace_section(content: str, section: str, new_content: str) -> str:
    """
    Replace content between VIMBAN section markers.

    Preserves the markers themselves.

    Args:
        content: Full file content
        section: Section name
        new_content: New content to insert

    Returns:
        Updated content
    """
    pattern = re.compile(
        rf'(<!-- VIMBAN:{section}:START -->)(.*?)(<!-- VIMBAN:{section}:END -->)',
        re.DOTALL
    )

    replacement = f'\\1\n\n{new_content}\n\n\\3'
    return pattern.sub(replacement, content)


# ============================================================================
# COMMENT HANDLING
# ============================================================================
def parse_comments(content: str) -> list[Comment]:
    """
    Extract comments from markdown content.

    Parses the Comments section between VIMBAN:COMMENTS markers and
    extracts all comments with their replies.

    Args:
        content: Full file content

    Returns:
        List of Comment objects
    """
    comments: list[Comment] = []
    section = extract_comment_section(content)

    if not section:
        return comments

    lines = section.split('\n')
    current_comment: Optional[Comment] = None
    current_content: list[str] = []
    in_reply: bool = False
    reply_timestamp: Optional[datetime] = None
    reply_author: Optional[str] = None
    reply_content: list[str] = []

    for line in lines:
        # Check for new comment header
        comment_match = COMMENT_HEADER_PATTERN.match(line)
        if comment_match:
            # Save previous comment if exists
            if current_comment is not None:
                if in_reply and reply_timestamp:
                    current_comment.replies.append(
                        (reply_timestamp, '\n'.join(reply_content).strip(), reply_author)
                    )
                current_comment.content = '\n'.join(current_content).strip()
                comments.append(current_comment)

            # Start new comment
            # Regex groups: (1)=id, (2)=author (optional), (3)=timestamp
            comment_id = int(comment_match.group(1))
            comment_author = comment_match.group(2)
            if comment_author:
                comment_author = comment_author.strip()
            timestamp_str = comment_match.group(3)
            try:
                timestamp = datetime.fromisoformat(timestamp_str)
            except ValueError:
                timestamp = datetime.now()

            current_comment = Comment(
                id=comment_id,
                timestamp=timestamp,
                content='',
                author=comment_author,
                replies=[]
            )
            current_content = []
            in_reply = False
            reply_content = []
            reply_author = None
            continue

        # Check for reply header
        reply_match = REPLY_HEADER_PATTERN.match(line)
        if reply_match and current_comment is not None:
            # Save previous reply if exists
            if in_reply and reply_timestamp:
                current_comment.replies.append(
                    (reply_timestamp, '\n'.join(reply_content).strip(), reply_author)
                )

            # Regex groups: (1)=author (optional), (2)=timestamp
            reply_author = reply_match.group(1)
            if reply_author:
                reply_author = reply_author.strip()
            timestamp_str = reply_match.group(2)
            try:
                reply_timestamp = datetime.fromisoformat(timestamp_str)
            except ValueError:
                reply_timestamp = datetime.now()

            in_reply = True
            reply_content = []
            continue

        # Accumulate content (strip quote prefixes)
        if current_comment is not None:
            if in_reply:
                # Strip >> prefix from reply lines
                if line.startswith('>> '):
                    reply_content.append(line[3:])
                elif line.startswith('>>'):
                    reply_content.append(line[2:])
                elif line.strip() == '':
                    reply_content.append('')
            else:
                # Strip > prefix from comment lines
                if line.startswith('> '):
                    current_content.append(line[2:])
                elif line.startswith('>'):
                    current_content.append(line[1:])
                elif line.strip() == '':
                    current_content.append('')

    # Save last comment
    if current_comment is not None:
        if in_reply and reply_timestamp:
            current_comment.replies.append(
                (reply_timestamp, '\n'.join(reply_content).strip(), reply_author)
            )
        current_comment.content = '\n'.join(current_content).strip()
        comments.append(current_comment)

    return comments


def extract_comment_section(content: str) -> Optional[str]:
    """
    Get raw comment section between VIMBAN:COMMENTS markers.

    Args:
        content: Full file content

    Returns:
        Content between markers or None if not found
    """
    return extract_section(content, 'COMMENTS')


def parse_comment_range(range_str: str, max_id: int) -> list[int]:
    """
    Parse a comment range string into list of IDs.

    Supports:
    - 'all' for all comments
    - Single IDs: '1,3,5'
    - Ranges: '2-5'
    - Combined: '1,2-5,9'

    Args:
        range_str: Range specification string
        max_id: Maximum comment ID in file

    Returns:
        List of comment IDs to display
    """
    if range_str.lower() == 'all':
        return list(range(1, max_id + 1))

    ids: set[int] = set()
    parts = range_str.split(',')

    for part in parts:
        part = part.strip()
        if not part:
            continue

        if '-' in part:
            try:
                start, end = part.split('-', 1)
                start_id = int(start.strip())
                end_id = int(end.strip())
                for i in range(start_id, end_id + 1):
                    if 1 <= i <= max_id:
                        ids.add(i)
            except ValueError:
                continue
        else:
            try:
                comment_id = int(part)
                if 1 <= comment_id <= max_id:
                    ids.add(comment_id)
            except ValueError:
                continue

    return sorted(ids)


def format_comment_output(
    comments: list[Comment],
    ids: list[int],
    include_threads: bool = False,
    fmt: str = 'plain',
    colors: Optional[Colors] = None
) -> str:
    """
    Format comments for terminal output.

    Args:
        comments: List of all comments
        ids: List of comment IDs to display
        include_threads: Whether to include thread replies
        fmt: Output format ('plain', 'md', 'yaml', 'json')
        colors: Colors instance for plain output

    Returns:
        Formatted string for display
    """
    if colors is None:
        colors = Colors(enabled=True)

    # Filter to requested IDs
    filtered = [c for c in comments if c.id in ids]

    if not filtered:
        return "No comments found"

    if fmt == 'json':
        data = [c.to_dict() for c in filtered]
        if not include_threads:
            for item in data:
                item.pop('replies', None)
        return json.dumps(data, indent=2, default=str)

    if fmt == 'yaml':
        data = [c.to_dict() for c in filtered]
        if not include_threads:
            for item in data:
                item.pop('replies', None)
        return yaml.dump(data, default_flow_style=False, allow_unicode=True)

    if fmt == 'md':
        lines = []
        for c in filtered:
            # Format: ### Comment #N by author (timestamp) or ### Comment #N (timestamp)
            author_part = f" by {c.author}" if c.author else ""
            lines.append(f"### Comment #{c.id}{author_part} ({c.timestamp.isoformat()})")
            lines.append("")
            # Prefix each line of content with > (quote block)
            for content_line in c.content.split('\n'):
                lines.append(f"> {content_line}" if content_line else ">")
            lines.append("")
            if include_threads and c.replies:
                for ts, reply, reply_author in c.replies:
                    # Format: #### Reply by author (timestamp) or #### Reply (timestamp)
                    reply_author_part = f" by {reply_author}" if reply_author else ""
                    lines.append(f"#### Reply{reply_author_part} ({ts.isoformat()})")
                    lines.append("")
                    # Prefix each line of reply with >> (double quote block)
                    for reply_line in reply.split('\n'):
                        lines.append(f">> {reply_line}" if reply_line else ">>")
                    lines.append("")
        return '\n'.join(lines)

    # Plain format
    lines = []
    for c in filtered:
        # Format: #N by author (timestamp) or #N (timestamp)
        author_part = f" by {c.author}" if c.author else ""
        lines.append(
            f"{colors.BOLD}#{c.id}{author_part}{colors.END} "
            f"{colors.DIM}({c.timestamp.strftime('%Y-%m-%d %H:%M')}){colors.END}"
        )
        lines.append(c.content)
        if include_threads and c.replies:
            for ts, reply, reply_author in c.replies:
                # Format: ↳ Reply by author (timestamp) or ↳ Reply (timestamp)
                reply_author_part = f" by {reply_author}" if reply_author else ""
                lines.append(
                    f"  {colors.CYAN}↳ Reply{reply_author_part}{colors.END} "
                    f"{colors.DIM}({ts.strftime('%Y-%m-%d %H:%M')}){colors.END}"
                )
                # Indent reply content
                for reply_line in reply.split('\n'):
                    lines.append(f"    {reply_line}")
        lines.append("")

    return '\n'.join(lines)


def get_next_comment_id(content: str) -> int:
    """
    Get next available comment ID by parsing existing comments.

    Args:
        content: Full file content

    Returns:
        Next available comment ID (1 if no comments exist)
    """
    comments = parse_comments(content)
    if not comments:
        return 1
    return max(c.id for c in comments) + 1


def ensure_comment_section(content: str) -> str:
    """
    Add comment section markers if missing.

    Adds the Comments section at the end of the file if it doesn't exist.

    Args:
        content: Full file content

    Returns:
        Content with comment section markers
    """
    if '<!-- VIMBAN:COMMENTS:START -->' in content:
        return content

    # Add comment section at end
    if not content.endswith('\n'):
        content += '\n'

    content += '\n## Comments\n\n'
    content += '<!-- VIMBAN:COMMENTS:START -->\n\n'
    content += '<!-- VIMBAN:COMMENTS:END -->\n'

    return content


def insert_comment(
    filepath: Path,
    text: str,
    reply_to: Optional[int] = None,
    author: Optional[str] = None
) -> int:
    """
    Insert a comment into a file.

    If reply_to is specified, adds a thread reply to that comment.
    Otherwise, creates a new top-level comment.

    Args:
        filepath: Path to the markdown file
        text: Comment text
        reply_to: Comment ID to reply to (None for new comment)
        author: Optional author attribution for the comment

    Returns:
        Comment ID (new ID for top-level, parent ID for reply)

    Raises:
        ValueError: If reply_to comment doesn't exist
    """
    content = filepath.read_text()

    # Ensure comment section exists
    content = ensure_comment_section(content)

    timestamp = datetime.now().isoformat()

    # Format author part for header (e.g., " by clawdbot" or "")
    author_part = f" by {author}" if author else ""

    if reply_to is not None:
        # Adding a reply to existing comment
        comments = parse_comments(content)
        parent = next((c for c in comments if c.id == reply_to), None)
        if not parent:
            raise ValueError(f"Comment #{reply_to} not found")

        # Format reply text with >> prefix for each line (double quote block)
        reply_lines = [f">> {line}" if line else ">>" for line in text.split('\n')]
        reply_text = '\n'.join(reply_lines)

        # Find the end of the parent comment or its last reply
        # and insert the reply there
        reply_markdown = f"\n#### Reply{author_part} ({timestamp})\n\n{reply_text}\n"

        # Find the comment header and insert before the next comment or end marker
        # Pattern handles optional 'by <author>' in existing comments
        pattern = re.compile(
            rf'(### Comment #{reply_to}(?: by [^\(]+)? \([^)]+\).*?)'
            rf'(?=### Comment #|<!-- VIMBAN:COMMENTS:END -->)',
            re.DOTALL
        )
        match = pattern.search(content)
        if match:
            insert_pos = match.end()
            content = content[:insert_pos] + reply_markdown + content[insert_pos:]

        filepath.write_text(content)
        return reply_to
    else:
        # Adding a new top-level comment
        new_id = get_next_comment_id(content)

        # Format comment text with > prefix for each line (quote block)
        comment_lines = [f"> {line}" if line else ">" for line in text.split('\n')]
        comment_text = '\n'.join(comment_lines)

        comment_markdown = f"\n\n### Comment #{new_id}{author_part} ({timestamp})\n\n{comment_text}\n\n"

        # Insert before the end marker
        content = content.replace(
            '<!-- VIMBAN:COMMENTS:END -->',
            comment_markdown + '<!-- VIMBAN:COMMENTS:END -->'
        )

        filepath.write_text(content)
        return new_id


# ============================================================================
# TRANSCLUSION HANDLING
# ============================================================================
def create_transclusion(
    path: str,
    alias: Optional[str] = None
) -> str:
    """
    Create a transclusion string.

    Args:
        path: Path to the file
        alias: Optional display alias

    Returns:
        Transclusion string like ![[path]] or ![[path|alias]]
    """
    if alias:
        return f"![[{path}|{alias}]]"
    return f"![[{path}]]"


def resolve_person_reference(
    ref: str,
    people_dir: Path,
    fuzzy: bool = True
) -> Optional[TransclusionLink]:
    """
    Resolve a person reference to a transclusion link.

    Accepts:
    - Full transclusion: ![[02_areas/work/people/john.md]]
    - Filename: john_smith
    - Name: "John Smith"
    - Fuzzy: john (if unique match)

    Args:
        ref: Reference string
        people_dir: Path to people directory
        fuzzy: Whether to allow fuzzy matching

    Returns:
        TransclusionLink or None if not found
    """
    if not ref:
        return None

    # Already a transclusion
    if ref.startswith('![['):
        return TransclusionLink.parse(ref)

    # Ensure people_dir exists
    if not people_dir.exists():
        return None

    # Try exact filename match
    for person_file in people_dir.glob('*.md'):
        if person_file.stem == ref:
            rel_path = str(person_file.relative_to(people_dir.parent.parent.parent))
            return TransclusionLink(path=rel_path)

    # Try name match in frontmatter
    for person_file in people_dir.glob('*.md'):
        try:
            content = person_file.read_text()
            metadata, _ = parse_frontmatter(content)
            if metadata.get('name', '').lower() == ref.lower():
                rel_path = str(person_file.relative_to(people_dir.parent.parent.parent))
                return TransclusionLink(path=rel_path)
        except (ValueError, yaml.YAMLError):
            continue

    # Fuzzy match if enabled
    if fuzzy:
        matches = []
        ref_lower = ref.lower()
        for person_file in people_dir.glob('*.md'):
            if ref_lower in person_file.stem.lower():
                matches.append(person_file)
            else:
                try:
                    content = person_file.read_text()
                    metadata, _ = parse_frontmatter(content)
                    name = metadata.get('name', '').lower()
                    if ref_lower in name:
                        matches.append(person_file)
                except (ValueError, yaml.YAMLError):
                    continue

        if len(matches) == 1:
            rel_path = str(matches[0].relative_to(people_dir.parent.parent.parent))
            return TransclusionLink(path=rel_path)

    return None


# ============================================================================
# TEMPLATE HANDLING
# ============================================================================
DEFAULT_TEMPLATES: dict[str, str] = {
    'task': '''---
id: {{ID}}
title: "{{TITLE}}"
type: task
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
watchers: []
priority: {{PRIORITY}}
effort:
tags: {{TAGS}}
project: {{PROJECT}}

due_date: {{DUE_DATE}}
start_date:
end_date:

member_of: {{MEMBER_OF}}
relates_to: []
blocked_by: []
blocks: []

progress: 0
issue_link:
---

# {{TITLE}}

## Description



## Acceptance Criteria

- [ ]

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'epic': '''---
id: {{ID}}
title: "{{TITLE}}"
type: epic
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
watchers: []
priority: {{PRIORITY}}
effort:
tags: {{TAGS}}
project: {{PROJECT}}

due_date: {{DUE_DATE}}
start_date:
end_date:

relates_to: []

progress: 0
issue_link:
---

# {{TITLE}}

## Overview



## Goals

- [ ]

## Stories

<!-- VIMBAN:STORIES:START -->

<!-- VIMBAN:STORIES:END -->

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'story': '''---
id: {{ID}}
title: "{{TITLE}}"
type: story
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
watchers: []
priority: {{PRIORITY}}
effort:
tags: {{TAGS}}
project: {{PROJECT}}

due_date: {{DUE_DATE}}
start_date:
end_date:

member_of: {{MEMBER_OF}}
relates_to: []
blocked_by: []
blocks: []

progress: 0
issue_link:
---

# {{TITLE}}

## User Story

As a [user type], I want to [action] so that [benefit].

## Acceptance Criteria

- [ ]

## Tasks

<!-- VIMBAN:TASKS:START -->

<!-- VIMBAN:TASKS:END -->

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'sub-task': '''---
id: {{ID}}
title: "{{TITLE}}"
type: sub-task
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
priority: {{PRIORITY}}
tags: {{TAGS}}

due_date: {{DUE_DATE}}
start_date:
end_date:

member_of: {{MEMBER_OF}}
blocked_by: []
blocks: []

progress: 0
issue_link:
---

# {{TITLE}}

## Description



## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'research': '''---
id: {{ID}}
title: "{{TITLE}}"
type: research
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
priority: {{PRIORITY}}
tags: {{TAGS}}
project: {{PROJECT}}

due_date: {{DUE_DATE}}

relates_to: []

progress: 0
issue_link:
---

# {{TITLE}}

## Research Question



## Findings



## Recommendations



## Resources

- [ ]

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'bug': '''---
id: {{ID}}
title: "{{TITLE}}"
type: bug
status: backlog
created: {{CREATED}}
updated: {{CREATED}}
version: 1

assignee: {{ASSIGNEE}}
reporter: {{REPORTER}}
watchers: []
priority: {{PRIORITY}}
tags: {{TAGS}}
project: {{PROJECT}}

due_date: {{DUE_DATE}}
start_date:
end_date:

relates_to: []
blocked_by: []
blocks: []

progress: 0
issue_link:
---

# {{TITLE}}

## Description



## Steps to Reproduce

1.

## Expected Behavior



## Actual Behavior



## Environment

- OS:
- Version:

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
    'person': '''---
name: "{{NAME}}"
email: "{{EMAIL}}"
role: "{{ROLE}}"
team: "{{TEAM}}"
type: person
created: {{CREATED}}
updated: {{CREATED}}

manager: {{MANAGER}}
direct_reports: []
---

# {{NAME}}

## About



## 1:1 Notes

<!-- VIMBAN:1ON1:START -->

<!-- VIMBAN:1ON1:END -->

## Current Focus

<!-- VIMBAN:DASHBOARD:START -->

<!-- VIMBAN:DASHBOARD:END -->

## Notes



## Comments

<!-- VIMBAN:COMMENTS:START -->

<!-- VIMBAN:COMMENTS:END -->
''',
}


def load_template(ticket_type: str) -> str:
    """
    Load template for a ticket type.

    First checks the user's template directory, then falls back to defaults.

    Args:
        ticket_type: Type of ticket (task, epic, story, etc.)

    Returns:
        Template string
    """
    # Check user template directory
    user_template = TEMPLATE_DIR / f"{ticket_type}.md"
    if user_template.exists():
        return user_template.read_text()

    # Fall back to default
    return DEFAULT_TEMPLATES.get(ticket_type, DEFAULT_TEMPLATES['task'])


def fill_template(
    template: str,
    ticket_id: str,
    title: str,
    assignee: Optional[TransclusionLink] = None,
    reporter: Optional[TransclusionLink] = None,
    priority: str = "medium",
    due_date: Optional[date] = None,
    tags: Optional[list[str]] = None,
    project: Optional[str] = None,
    member_of: Optional[list[str]] = None,
    **kwargs
) -> str:
    """
    Fill template placeholders with values.

    Args:
        template: Template string with {{PLACEHOLDER}} markers
        ticket_id: Ticket ID
        title: Ticket title
        assignee: Assignee transclusion link
        reporter: Reporter transclusion link
        priority: Priority level
        due_date: Due date
        tags: List of tags
        project: Project identifier
        member_of: Parent ticket paths
        **kwargs: Additional template variables

    Returns:
        Filled template string
    """
    now = datetime.now().isoformat()

    replacements = {
        '{{ID}}': f'"{ticket_id}"',
        '{{TITLE}}': title,
        '{{CREATED}}': now,
        '{{ASSIGNEE}}': f'"{assignee}"' if assignee else '',
        '{{REPORTER}}': f'"{reporter}"' if reporter else '',
        '{{PRIORITY}}': priority,
        '{{STATUS}}': kwargs.get('status', 'backlog'),
        '{{DUE_DATE}}': str(due_date) if due_date else '',
        '{{TAGS}}': json.dumps(tags) if tags else '[]',
        '{{PROJECT}}': project or '',
        '{{MEMBER_OF}}': json.dumps(member_of) if member_of else '[]',
        '{{NAME}}': kwargs.get('name', ''),
        '{{EMAIL}}': kwargs.get('email', ''),
        '{{ROLE}}': kwargs.get('role', ''),
        '{{TEAM}}': kwargs.get('team', ''),
        '{{MANAGER}}': kwargs.get('manager', ''),
    }

    result = template
    for placeholder, value in replacements.items():
        result = result.replace(placeholder, str(value))

    return result


# ============================================================================
# TICKET RESOLUTION
# ============================================================================
def find_ticket(
    ticket_ref: str,
    config: Config,
    args: argparse.Namespace
) -> Optional[Path]:
    """
    Find a ticket file by ID or path.

    Searches work and personal directories based on --work/--personal flags.
    If no flag specified, searches both directories.

    Accepts:
    - Full ID: PROJ-00042
    - Partial ID: 42
    - File path: ./tasks/my_task.md

    Args:
        ticket_ref: Ticket reference (ID or path)
        config: Vimban configuration
        args: Parsed command line arguments (for scope flags)

    Returns:
        Path to ticket file or None
    """
    # Check if it's a path
    if '/' in ticket_ref or ticket_ref.endswith('.md'):
        path = Path(ticket_ref)
        if not path.is_absolute():
            path = config.directory / path
        if path.exists():
            return path
        return None

    # Handle partial ID (just number)
    if ticket_ref.isdigit():
        ticket_ref = f"{config.prefix}-{int(ticket_ref):05d}"

    # Get search paths (work, personal, or both)
    search_paths = get_search_paths(args, config)

    # Detect ID prefix to determine additional search paths
    id_prefix = ticket_ref.split('-')[0] if '-' in ticket_ref else ''

    # Add specialized type paths based on ID prefix
    for type_name, type_cls in SPECIALIZED_TYPE_REGISTRY.items():
        type_config = type_cls.get_config()
        if id_prefix == type_config.prefix:
            parent_dir = "02_areas" if type_config.parent == "area" else "03_resources"
            specialized_path = config.directory / parent_dir / type_config.base_topic
            if specialized_path not in search_paths:
                search_paths.append(specialized_path)

    # Add PARA paths for area/resource IDs
    if id_prefix == 'AR':
        areas_path = config.directory / "02_areas"
        if areas_path not in search_paths:
            search_paths.append(areas_path)
    elif id_prefix == 'RS':
        resources_path = config.directory / "03_resources"
        if resources_path not in search_paths:
            search_paths.append(resources_path)

    # Search for matching ID in frontmatter
    for search_path in search_paths:
        if not search_path.exists():
            continue
        for md_file in search_path.rglob('*.md'):
            try:
                content = md_file.read_text()
                metadata, _ = parse_frontmatter(content)
                file_id = metadata.get('id', '').strip('"')
                if file_id == ticket_ref:
                    return md_file
            except (ValueError, yaml.YAMLError):
                continue

    return None


def find_person(
    person_ref: str,
    config: Config,
    args: argparse.Namespace
) -> Optional[Path]:
    """
    Find a person file by name, filename, ID, or path.

    Searches people directories (work and personal) based on --work/--personal flags.
    If no flag specified, searches both directories.

    Accepts:
    - Full path: ./people/john_smith.md
    - Filename: john_smith
    - Name: "John Smith"
    - ID: PERSON-00001

    Args:
        person_ref: Person reference (name, filename, ID, or path)
        config: Vimban configuration
        args: Parsed command line arguments (for scope flags)

    Returns:
        Path to person file or None
    """
    # Check if it's a path
    if '/' in person_ref or person_ref.endswith('.md'):
        path = Path(person_ref)
        if not path.is_absolute():
            path = config.directory / path
        if path.exists():
            return path
        return None

    # Get people directories
    people_dirs = get_people_dirs(args, config)

    # Check if it's a PERSON-XXXXX ID
    if person_ref.upper().startswith('PERSON-'):
        for people_dir in people_dirs:
            if not people_dir.exists():
                continue
            for md_file in people_dir.glob('*.md'):
                try:
                    content = md_file.read_text()
                    metadata, _ = parse_frontmatter(content)
                    if metadata.get('id', '').upper() == person_ref.upper():
                        return md_file
                except (ValueError, yaml.YAMLError):
                    continue
        return None

    for people_dir in people_dirs:
        if not people_dir.exists():
            continue

        # Try exact filename match
        exact_path = people_dir / f"{person_ref}.md"
        if exact_path.exists():
            return exact_path

        # Try sanitized filename
        sanitized = sanitize_filename(person_ref)
        sanitized_path = people_dir / f"{sanitized}.md"
        if sanitized_path.exists():
            return sanitized_path

        # Search by name in frontmatter
        for md_file in people_dir.glob('*.md'):
            try:
                content = md_file.read_text()
                metadata, _ = parse_frontmatter(content)
                if metadata.get('type') != 'person':
                    continue
                name = metadata.get('name', '')
                if name.lower() == person_ref.lower():
                    return md_file
            except (ValueError, yaml.YAMLError):
                continue

    return None


# ============================================================================
# COMMANDS
# ============================================================================
def cmd_init(args: argparse.Namespace, config: Config) -> int:
    """
    Initialize vimban in a directory.

    Creates:
    - .vimban/config.yaml
    - .vimban/.sequence
    - Optionally updates .gitignore
    """
    target_dir = Path(args.directory) if args.directory else config.directory
    vimban_dir = target_dir / CONFIG_DIR_NAME

    if vimban_dir.exists():
        if not getattr(args, 'force', False):
            error(f"vimban already initialized in {target_dir}", EXIT_GENERAL_ERROR)
        warn(f"Reinitializing vimban in {target_dir}")

    vimban_dir.mkdir(parents=True, exist_ok=True)

    # Create config
    new_config = Config(
        directory=target_dir,
        prefix=getattr(args, 'prefix', None) or DEFAULT_PREFIX,
        people_dir=getattr(args, 'people_dir', None) or DEFAULT_PEOPLE_DIR,
    )
    new_config.save()

    # Create sequence file
    sequence_file = vimban_dir / SEQUENCE_FILE
    if not sequence_file.exists():
        sequence_file.write_text('0')

    # Update .gitignore
    if not getattr(args, 'no_git', False):
        gitignore = target_dir / '.gitignore'
        ignore_entry = '.vimban/.sequence'
        if gitignore.exists():
            content = gitignore.read_text()
            if ignore_entry not in content:
                with open(gitignore, 'a') as f:
                    f.write(f'\n{ignore_entry}\n')
        else:
            gitignore.write_text(f'{ignore_entry}\n')

    print(f"Initialized vimban in {target_dir}")
    print(f"  Prefix: {new_config.prefix}")
    print(f"  People dir: {new_config.people_dir}")

    return EXIT_SUCCESS


def get_projects_base(
    args: argparse.Namespace,
    config: Config,
    interactive: bool = False
) -> Path:
    """
    Get the base projects path based on --work or --personal flags.

    If interactive=True and neither flag set, prompt user to choose.
    Returns path to 01_projects/work/, 01_projects/personal/, or 01_projects/.

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info
        interactive: If True, prompt user when no scope specified

    Returns:
        Path to the projects base directory
    """
    base = config.directory / "01_projects"

    if getattr(args, 'work', False):
        return base / "work"
    elif getattr(args, 'personal', False):
        return base / "personal"
    elif interactive:
        # Prompt user to choose
        print("Select scope:")
        print("  1) work")
        print("  2) personal")
        try:
            choice = input("Choice [1/2]: ").strip()
        except EOFError:
            error("No scope specified and stdin not available", EXIT_INVALID_ARGS)
        if choice == '1' or choice.lower() == 'work' or choice.lower() == 'w':
            return base / "work"
        elif choice == '2' or choice.lower() == 'personal' or choice.lower() == 'p':
            return base / "personal"
        else:
            error(f"Invalid choice: {choice}. Use 1/work or 2/personal", EXIT_INVALID_ARGS)
    else:
        return base  # Return base for searching both


def get_search_paths(args: argparse.Namespace, config: Config) -> list[Path]:
    """
    Get list of paths to search for tickets.

    If --work or --personal specified, returns only that path.
    Otherwise returns both work and personal paths.

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info

    Returns:
        List of paths to search
    """
    base = config.directory / "01_projects"

    if getattr(args, 'work', False):
        return [base / "work"]
    elif getattr(args, 'personal', False):
        return [base / "personal"]
    else:
        return [base / "work", base / "personal"]


def get_people_dirs(args: argparse.Namespace, config: Config) -> list[Path]:
    """
    Get list of people directories to search based on --work/--personal flags.

    If --work specified, returns only work people directory.
    If --personal specified, returns only personal people directory.
    Otherwise returns both.

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info

    Returns:
        List of people directory paths to search
    """
    base = config.directory / "02_areas"

    if getattr(args, 'work', False):
        return [base / "work" / "people"]
    elif getattr(args, 'personal', False):
        return [base / "personal" / "people"]
    else:
        return [base / "work" / "people", base / "personal" / "people"]


def get_para_directories(config: Config) -> list[str]:
    """
    Get all directories under PARA folders for move-location fzf selection.

    Returns directories from:
    - 00_inbox/
    - 01_projects/
    - 02_areas/
    - 03_resources/
    - 04_archives/

    Args:
        config: Config instance with directory info

    Returns:
        List of relative directory paths suitable for fzf display
    """
    para_roots: list[str] = [
        "00_inbox",
        "01_projects",
        "02_areas",
        "03_resources",
        "04_archives",
    ]

    directories: list[str] = []

    for root_name in para_roots:
        root_path: Path = config.directory / root_name
        if root_path.exists() and root_path.is_dir():
            # Add the root itself
            directories.append(root_name)
            # Add all subdirectories recursively
            for dirpath in root_path.rglob("*"):
                if dirpath.is_dir():
                    rel_path: str = str(dirpath.relative_to(config.directory))
                    directories.append(rel_path)

    # Sort for consistent display
    directories.sort()
    return directories


def get_para_base(
    args: argparse.Namespace,
    config: Config,
    para_type: str
) -> Path:
    """
    Get base directory for PARA types (area/resource) using --topic.

    Args:
        args: Parsed command line arguments (expects --topic)
        config: Config instance with directory info
        para_type: Either "area" or "resource"

    Returns:
        Path to the PARA type base directory with topic subdirectory
    """
    if para_type == "area":
        base = config.directory / "02_areas"
    elif para_type == "resource":
        base = config.directory / "03_resources"
    else:
        error(f"Invalid PARA type: {para_type}", EXIT_INVALID_ARGS)

    # Add topic subdirectory if specified
    topic = getattr(args, 'topic', None)
    if topic:
        return base / topic
    else:
        return base


def get_para_search_paths(
    args: argparse.Namespace,
    config: Config
) -> list[Path]:
    """
    Get list of paths to search for PARA types (areas/resources).

    Based on --areas and --resources flags.

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info

    Returns:
        List of paths to search for areas/resources
    """
    paths: list[Path] = []

    if getattr(args, 'areas', False):
        paths.append(config.directory / "02_areas")
    if getattr(args, 'resources', False):
        paths.append(config.directory / "03_resources")

    return paths


def get_people_base(
    args: argparse.Namespace,
    config: Config,
    interactive: bool = False
) -> Path:
    """
    Get single people directory for creation.

    If interactive=True and no scope specified, prompt user.

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info
        interactive: If True, prompt user when no scope specified

    Returns:
        Path to the people directory
    """
    base = config.directory / "02_areas"

    if getattr(args, 'work', False):
        return base / "work" / "people"
    elif getattr(args, 'personal', False):
        return base / "personal" / "people"
    elif interactive:
        print("Select scope:")
        print("  1) work")
        print("  2) personal")
        try:
            choice = input("Choice [1/2]: ").strip()
        except EOFError:
            error("No scope specified and stdin not available", EXIT_INVALID_ARGS)
        if choice == '1' or choice.lower() in ('work', 'w'):
            return base / "work" / "people"
        elif choice == '2' or choice.lower() in ('personal', 'p'):
            return base / "personal" / "people"
        else:
            error(f"Invalid choice: {choice}", EXIT_INVALID_ARGS)
    else:
        # Default to work for backwards compatibility
        return base / "work" / "people"


def find_person_in_scopes(
    person_ref: str,
    args: argparse.Namespace,
    config: Config
) -> Optional[TransclusionLink]:
    """
    Find a person by reference, searching work and personal if not specified.

    Args:
        person_ref: Person reference (name, filename, or transclusion)
        args: Parsed command line arguments (for scope flags)
        config: Config instance with directory info

    Returns:
        TransclusionLink if found, None otherwise
    """
    people_dirs = get_people_dirs(args, config)

    for people_dir in people_dirs:
        if not people_dir.exists():
            continue
        result = resolve_person_reference(person_ref, people_dir)
        if result:
            return result

    return None


def find_ticket_path(
    ticket_id: str,
    config: Config,
    args: argparse.Namespace
) -> Optional[Path]:
    """
    Find a ticket file by ID, searching work and personal if not specified.

    Handles both full IDs (PROJ-00042) and short form (42).

    Args:
        ticket_id: Ticket ID to find (full or short form)
        config: Config instance with directory info
        args: Parsed command line arguments (for scope flags)

    Returns:
        Path to the ticket file if found, None otherwise
    """
    # Normalize ID (handle short form like "42" -> "PROJ-00042")
    if ticket_id.isdigit():
        ticket_id = f"{config.prefix}-{int(ticket_id):05d}"

    # Get search paths
    search_paths = get_search_paths(args, config)

    # Search for ticket
    for search_path in search_paths:
        if not search_path.exists():
            continue
        for md_file in search_path.rglob("*.md"):
            # Quick check: ID in filename
            if ticket_id.lower() in md_file.name.lower():
                return md_file
            # Check frontmatter for ID
            try:
                content = md_file.read_text()
                # Check first 500 chars for efficiency (frontmatter is at top)
                header = content[:500]
                if f'id: "{ticket_id}"' in header or f"id: '{ticket_id}'" in header:
                    return md_file
                if f"id: {ticket_id}" in header:
                    return md_file
            except Exception:
                continue

    return None


def resolve_parent_path(member_of_ref: str, config: Config) -> Optional[Path]:
    """
    Resolve a member_of reference to a file path.

    Accepts transclusion format (![[path/to/file.md]]) or relative path.

    Args:
        member_of_ref: A transclusion link or relative path
        config: Config instance with directory info

    Returns:
        Path to the parent file if found, None otherwise
    """
    if not member_of_ref:
        return None

    # Handle transclusion format: ![[path/to/file.md]]
    if member_of_ref.startswith('![['):
        match = TRANSCLUSION_PATTERN.match(member_of_ref.strip())
        if match:
            return config.directory / match.group(1)

    # Handle direct path
    path = Path(member_of_ref)
    if not path.is_absolute():
        path = config.directory / member_of_ref

    if path.exists():
        return path

    return None


def update_parent_children(
    parent_path: Path,
    child_path: Path,
    config: Config
) -> bool:
    """
    Update parent ticket to add child to its 'children' field.

    Creates the 'children' field if it doesn't exist.

    Args:
        parent_path: Path to the parent ticket file
        child_path: Path to the newly created child ticket file
        config: Config instance with directory info

    Returns:
        True if parent was updated, False otherwise
    """
    if not parent_path.exists():
        return False

    try:
        # Read parent content
        parent_content = parent_path.read_text()
        metadata, body = parse_frontmatter(parent_content)

        # Create transclusion link to child
        rel_child = str(child_path.relative_to(config.directory))
        child_link = create_transclusion(rel_child)

        # Get or create children field
        children = metadata.get('children', [])
        if not isinstance(children, list):
            children = [children] if children else []

        # Add child if not already present
        if child_link not in children:
            children.append(child_link)

        # Update metadata
        metadata['children'] = children
        metadata['updated'] = datetime.now().isoformat()
        metadata['version'] = metadata.get('version', 0) + 1

        # Write updated parent
        parent_path.write_text(dump_frontmatter(metadata, body))
        return True

    except Exception as e:
        # Don't fail child creation if parent update fails
        print(f"Warning: Could not update parent {parent_path}: {e}", file=sys.stderr)
        return False


def determine_output_path(
    ticket_type: str,
    safe_title: str,
    member_of: Optional[list[str]],
    config: Config,
    projects_base: Path
) -> Path:
    """
    Determine output path based on ticket type and parent relationship.

    Directory structure rules:
    - epic: creates directory, file inside (base/epic_{title}/epic_{title}.md)
    - story with epic parent: directory under epic (epic_dir/story_{title}/story_{title}.md)
    - story without parent: directory in base (base/story_{title}/story_{title}.md)
    - task/sub-task with parent: flat file in parent's directory
    - task/sub-task without parent: flat file in base
    - bug/research: flat files in base

    Args:
        ticket_type: Type of ticket (epic, story, task, etc.)
        safe_title: Sanitized title for filename
        member_of: List of parent references (transclusion links or paths)
        config: Config instance with directory info
        projects_base: Base path for projects (e.g., 01_projects/work/)

    Returns:
        Path where the ticket file should be created
    """
    base = projects_base

    if ticket_type == 'epic':
        # Epic always creates a directory
        dir_name = f"epic_{safe_title}"
        return base / dir_name / f"epic_{safe_title}.md"

    elif ticket_type == 'story':
        # Story creates a directory
        dir_name = f"story_{safe_title}"

        if member_of:
            # Find parent epic directory
            parent_path = resolve_parent_path(member_of[0], config)
            if parent_path and parent_path.parent.name.startswith('epic_'):
                return parent_path.parent / dir_name / f"story_{safe_title}.md"

        # No epic parent - create directory in 01_projects
        return base / dir_name / f"story_{safe_title}.md"

    else:
        # task, sub-task, bug, research - flat files
        filename = f"{ticket_type}_{safe_title}.md"

        if member_of and ticket_type in ('task', 'sub-task'):
            parent_path = resolve_parent_path(member_of[0], config)
            if parent_path:
                # Place in parent's directory
                return parent_path.parent / filename

        return base / filename


def cmd_create_para(
    args: argparse.Namespace,
    config: Config,
    title: Optional[str]
) -> int:
    """
    Create a new PARA type (area, resource, or specialized type).

    PARA types have different handling:
    - Use --topic instead of --work/--personal for path
    - Use separate ID sequences (AR-XXX, RS-XXX, MTG-XXX, etc.)
    - No type prefix in filename
    - Areas have status, resources don't
    - Specialized types may have custom path/filename generation

    Args:
        args: Parsed command line arguments
        config: Config instance with directory info
        title: Title for the new item (may be None for journal)

    Returns:
        Exit code
    """
    config_dir = config.directory / CONFIG_DIR_NAME
    ticket_type = args.type

    # Check if specialized type
    if is_specialized_type(ticket_type):
        type_cls = get_specialized_type(ticket_type)
        type_config = type_cls.get_config()

        # Get the meeting date from args if provided
        meeting_date = getattr(args, 'date', None)

        # Get title (may have default for some types like journal)
        actual_title = type_cls.get_title(title, date=meeting_date)

        # Generate ID with type-specific prefix and sequence
        ticket_id = next_id(
            config_dir,
            custom_id=getattr(args, 'id', None),
            prefix=getattr(args, 'prefix', None),
            ticket_type=ticket_type
        )

        # Parse tags
        tags_str = getattr(args, 'tags', None) or ''
        tags = [t.strip() for t in tags_str.split(',')] if tags_str else []

        # Resolve people references (search both work/personal scopes)
        assignee = find_person_in_scopes(
            getattr(args, 'assignee', None) or '',
            args, config
        )
        reporter = find_person_in_scopes(
            getattr(args, 'reporter', None) or '',
            args, config
        )

        # Parse due date
        due_date = parse_date(getattr(args, 'due', None) or '')

        # Get member_of, project, priority
        member_of = getattr(args, 'member_of', None)
        project = getattr(args, 'project', None)
        priority = getattr(args, 'priority', None) or config.default_priority

        # Load template
        template = load_template(ticket_type)

        # Determine if type has status
        if type_config.has_status:
            status = getattr(args, 'status', None) or config.default_status
            content = fill_template(
                template,
                ticket_id=ticket_id,
                title=actual_title,
                assignee=assignee,
                reporter=reporter,
                priority=priority,
                due_date=due_date,
                tags=tags,
                project=project,
                member_of=member_of,
                status=status,
            )
        else:
            content = fill_template(
                template,
                ticket_id=ticket_id,
                title=actual_title,
                assignee=assignee,
                reporter=reporter,
                priority=priority,
                due_date=due_date,
                tags=tags,
                project=project,
                member_of=member_of,
            )

        # Apply type-specific template replacements
        extra_replacements = type_cls.get_template_replacements(date=meeting_date)
        for placeholder, value in extra_replacements.items():
            content = content.replace(placeholder, str(value))

        # Get base path using type class
        topic = getattr(args, 'topic', None)
        para_base = type_cls.get_base_path(config.directory, topic=topic)

        # Get filename using type class
        filename = type_cls.get_filename(actual_title, date=meeting_date)

        # Determine output path
        output_path: Path
        if getattr(args, 'output', None):
            output_path = Path(args.output)
            if not output_path.is_absolute():
                output_path = config.directory / output_path
        elif member_of:
            # Place in parent's directory if member_of is specified
            parent_path = resolve_parent_path(member_of[0], config)
            if parent_path:
                output_path = parent_path.parent / filename
            else:
                output_path = para_base / filename
        else:
            output_path = para_base / filename

    else:
        # Regular PARA type (area/resource)
        # Title is required for these
        if not title:
            error("Title is required", EXIT_INVALID_ARGS)

        # Generate ID with type-specific prefix and sequence
        ticket_id = next_id(
            config_dir,
            custom_id=getattr(args, 'id', None),
            prefix=getattr(args, 'prefix', None),
            ticket_type=ticket_type
        )

        # Parse tags
        tags_str = getattr(args, 'tags', None) or ''
        tags = [t.strip() for t in tags_str.split(',')] if tags_str else []

        # Load template
        template = load_template(ticket_type)

        # Fill template - areas have status, resources don't
        if ticket_type == "area":
            status = getattr(args, 'status', None) or config.default_status
            content = fill_template(
                template,
                ticket_id=ticket_id,
                title=title,
                status=status,
                tags=tags,
            )
        else:
            content = fill_template(
                template,
                ticket_id=ticket_id,
                title=title,
                tags=tags,
            )

        # Sanitize title for filename (NO type prefix for PARA types)
        safe_title = re.sub(r'[^\w\s-]', '', title).strip().lower()
        safe_title = re.sub(r'[-\s]+', '_', safe_title)[:50]

        # Determine output path
        output_path: Path
        if getattr(args, 'output', None):
            output_path = Path(args.output)
            if not output_path.is_absolute():
                output_path = config.directory / output_path
        else:
            # Get PARA base with topic
            para_base = get_para_base(args, config, ticket_type)
            output_path = para_base / f"{safe_title}.md"

        actual_title = title

    if getattr(args, 'dry_run', False):
        print("=== DRY RUN ===")
        print(f"Would create: {output_path}")
        print(f"ID: {ticket_id}")
        print(f"Content:\n{content}")
        return EXIT_SUCCESS

    # Write file
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content)

    # Update parent ticket's children field if member_of was specified
    if member_of:
        for parent_ref in member_of:
            parent_path = resolve_parent_path(parent_ref, config)
            if parent_path:
                update_parent_children(parent_path, output_path, config)

    # Open in editor unless --no-edit
    if not getattr(args, 'no_edit', False):
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(output_path)])

    # Output based on format
    fmt = getattr(args, 'format', 'plain')
    if fmt == 'json':
        print(json.dumps({'id': ticket_id, 'path': str(output_path)}))
    elif fmt == 'yaml':
        print(yaml.dump({'id': ticket_id, 'path': str(output_path)}))
    else:
        print(f"Created {ticket_id}: {output_path}")

    return EXIT_SUCCESS


def cmd_create(args: argparse.Namespace, config: Config) -> int:
    """
    Create a new ticket.

    Supports:
    - Template-based creation
    - People reference resolution
    - Date parsing (absolute and relative)
    - stdin for title
    - --dry-run preview
    - PARA types (area/resource) with --topic
    """
    # Get title from args or stdin
    title = getattr(args, 'title', None)
    if not title and not sys.stdin.isatty():
        title = sys.stdin.read().strip()

    # Check if initialized
    config_dir = config.directory / CONFIG_DIR_NAME
    if not config_dir.exists():
        error(f"vimban not initialized in {config.directory}. Run 'vimban init' first.", EXIT_GENERAL_ERROR)

    # Handle PARA types (area/resource) and specialized types differently
    # Some specialized types (like journal) don't require title
    if args.type in PARA_TYPES or is_specialized_type(args.type):
        return cmd_create_para(args, config, title)

    # Title is required for regular ticket types
    if not title:
        error("Title is required", EXIT_INVALID_ARGS)

    # Generate ID for regular ticket types
    # Only pass explicit --prefix if provided; let next_id() use type-specific prefixes
    ticket_id = next_id(
        config_dir,
        custom_id=getattr(args, 'id', None),
        prefix=getattr(args, 'prefix', None),
        ticket_type=args.type
    )

    # Resolve people references (search both work/personal scopes)
    assignee = find_person_in_scopes(
        getattr(args, 'assignee', None) or '',
        args, config
    )
    reporter = find_person_in_scopes(
        getattr(args, 'reporter', None) or '',
        args, config
    )

    # Parse due date
    due_date = parse_date(getattr(args, 'due', None) or '')

    # Parse tags
    tags_str = getattr(args, 'tags', None) or ''
    tags = [t.strip() for t in tags_str.split(',')] if tags_str else []

    # Load and fill template
    template = load_template(args.type)
    content = fill_template(
        template,
        ticket_id=ticket_id,
        title=title,
        assignee=assignee,
        reporter=reporter,
        priority=getattr(args, 'priority', None) or config.default_priority,
        due_date=due_date,
        tags=tags,
        project=getattr(args, 'project', None),
        member_of=getattr(args, 'member_of', None),
    )

    # Determine output path
    # Sanitize title for filename
    safe_title = re.sub(r'[^\w\s-]', '', title).strip().lower()
    safe_title = re.sub(r'[-\s]+', '_', safe_title)[:50]

    output_path: Path
    if getattr(args, 'output', None):
        output_path = Path(args.output)
        if not output_path.is_absolute():
            output_path = config.directory / output_path
    else:
        # Get projects base (interactive prompt if no --work/--personal)
        projects_base = get_projects_base(args, config, interactive=True)

        # Use hierarchical directory structure based on ticket type
        output_path = determine_output_path(
            args.type,
            safe_title,
            getattr(args, 'member_of', None),
            config,
            projects_base
        )

    if getattr(args, 'dry_run', False):
        print("=== DRY RUN ===")
        print(f"Would create: {output_path}")
        print(f"ID: {ticket_id}")
        print(f"Content:\n{content}")
        return EXIT_SUCCESS

    # Write file
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content)

    # Update parent ticket's children field if member_of was specified
    member_of = getattr(args, 'member_of', None)
    if member_of:
        for parent_ref in member_of:
            parent_path = resolve_parent_path(parent_ref, config)
            if parent_path:
                update_parent_children(parent_path, output_path, config)

    # Open in editor unless --no-edit
    if not getattr(args, 'no_edit', False):
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(output_path)])

    # Output based on format
    fmt = getattr(args, 'format', 'plain')
    if fmt == 'json':
        print(json.dumps({'id': ticket_id, 'path': str(output_path)}))
    elif fmt == 'yaml':
        print(yaml.dump({'id': ticket_id, 'path': str(output_path)}))
    else:
        print(f"Created {ticket_id}: {output_path}")

    return EXIT_SUCCESS


def cmd_list(args: argparse.Namespace, config: Config) -> int:
    """
    List tickets with filters.

    Uses Krafna for querying when available,
    falls back to filesystem scan otherwise.
    Searches both work and personal directories if no scope specified.
    Excludes areas/resources by default unless --areas or --resources flags used.
    """
    # Build filters dict
    # Normalize type names to handle plural forms (e.g., 'recipes' -> 'recipe')
    raw_type = getattr(args, 'type', None)
    normalized_type = None
    if raw_type:
        normalized_type = ','.join(
            normalize_type_name(t.strip()) for t in raw_type.split(',')
        )

    filters = {
        'status': getattr(args, 'status', None),
        'type': normalized_type,
        'priority': getattr(args, 'priority', None),
        'project': getattr(args, 'project', None),
        'assignee': getattr(args, 'assignee', None),
    }

    # Determine which types to exclude
    # Always exclude 'person', exclude PARA types unless explicitly requested
    exclude_types = ['person']
    include_areas = getattr(args, 'areas', False)
    include_resources = getattr(args, 'resources', False)

    # Auto-include PARA paths when filtering by specialized types
    # Use the first normalized type for specialized type path detection
    type_filter = normalized_type.split(',')[0] if normalized_type else None
    if type_filter and is_specialized_type(type_filter):
        type_cls = get_specialized_type(type_filter)
        if type_cls:
            type_config = type_cls.get_config()
            if type_config.parent == "area":
                include_areas = True
            elif type_config.parent == "resource":
                include_resources = True

    if not include_areas:
        exclude_types.append('area')
    if not include_resources:
        exclude_types.append('resource')

    # Get search paths (work, personal, or both)
    search_paths = get_search_paths(args, config)

    # Add PARA search paths if requested
    para_paths = get_para_search_paths(args, config)
    search_paths.extend(para_paths)

    # Add specialized type paths if filtering by specialized type
    if type_filter and is_specialized_type(type_filter):
        type_cls = get_specialized_type(type_filter)
        if type_cls:
            type_config = type_cls.get_config()
            parent_dir = "02_areas" if type_config.parent == "area" else "03_resources"
            specialized_path = config.directory / parent_dir / type_config.base_topic
            if specialized_path not in search_paths:
                search_paths.append(specialized_path)

    # Add archive search paths when --archived is specified
    include_archived = getattr(args, 'archived', False)
    if include_archived:
        if getattr(args, 'work', False):
            archive_path = config.directory / '04_archives' / '01_projects' / 'work'
            if archive_path.exists():
                search_paths.append(archive_path)
        elif getattr(args, 'personal', False):
            archive_path = config.directory / '04_archives' / '01_projects' / 'personal'
            if archive_path.exists():
                search_paths.append(archive_path)
        else:
            # Both scopes
            for scope in ['work', 'personal']:
                archive_path = config.directory / '04_archives' / '01_projects' / scope
                if archive_path.exists():
                    search_paths.append(archive_path)

    # Collect tickets from all search paths
    tickets: list[Ticket] = []
    for search_path in search_paths:
        if search_path.exists():
            path_tickets = fallback_list_tickets(search_path, filters, exclude_types, include_archived)
            tickets.extend(path_tickets)

    # Apply additional filters
    today = date.today()

    if getattr(args, 'overdue', False):
        tickets = [t for t in tickets if t.due_date and t.due_date < today]

    due_soon = getattr(args, 'due_soon', None)
    if due_soon:
        days = int(due_soon) if due_soon != True else 7
        cutoff = today + timedelta(days=days)
        tickets = [t for t in tickets if t.due_date and t.due_date <= cutoff]

    if getattr(args, 'blocked', False):
        tickets = [t for t in tickets if t.status == 'blocked']

    if getattr(args, 'unassigned', False):
        tickets = [t for t in tickets if not t.assignee]

    if getattr(args, 'mine', False):
        user = os.environ.get('USER', '')
        tickets = [t for t in tickets if t.assignee and user.lower() in str(t.assignee).lower()]

    # Sort
    sort_field = getattr(args, 'sort', None) or 'due_date'
    reverse = getattr(args, 'reverse', False)

    def sort_key(t: Ticket) -> Any:
        val = getattr(t, sort_field, None)
        if val is None:
            # Return max date for None so tickets without dates sort last
            if sort_field in ('due_date', 'start_date', 'end_date', 'created', 'updated'):
                return date.max
            return ''
        return val

    tickets.sort(key=sort_key, reverse=reverse)

    # Limit
    limit = getattr(args, 'limit', None)
    if limit:
        tickets = tickets[:limit]

    # Format output
    columns_str = getattr(args, 'columns', None)
    columns = columns_str.split(',') if columns_str else None
    no_header = getattr(args, 'no_header', False)
    no_color = getattr(args, 'no_color', False)
    fmt = getattr(args, 'format', 'plain')

    output = format_output(
        tickets,
        fmt,
        columns=columns,
        colors=Colors(enabled=not no_color),
        no_header=no_header
    )

    if output:
        print(output)

    return EXIT_SUCCESS


def cmd_show(args: argparse.Namespace, config: Config) -> int:
    """
    Show ticket details.

    Displays full ticket information including linked tickets
    if --links is specified.
    """
    ticket_ref = args.ticket
    ticket_path = find_ticket(ticket_ref, config, args)

    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    # Show raw content if requested
    if getattr(args, 'raw', False):
        print(ticket_path.read_text())
        return EXIT_SUCCESS

    # Show content only (no frontmatter) if requested
    if getattr(args, 'content', False):
        content = ticket_path.read_text()
        _, body = parse_frontmatter(content)
        print(body.strip())
        return EXIT_SUCCESS

    try:
        ticket = Ticket.from_file(ticket_path)
    except ValueError as e:
        error(str(e), EXIT_VALIDATION_ERROR)

    fmt = getattr(args, 'format', 'plain')

    if fmt == 'json':
        print(json.dumps(ticket.to_dict(), indent=2))
    elif fmt == 'yaml':
        print(yaml.dump(ticket.to_dict(), default_flow_style=False))
    elif fmt == 'md':
        # Show full markdown content
        print(ticket_path.read_text())
    else:
        # Plain format - detailed view
        colors = Colors(enabled=not getattr(args, 'no_color', False))

        print(f"{colors.BOLD}{ticket.id}{colors.END}: {ticket.title}")
        print(f"  Type: {ticket.type}")
        print(f"  Status: {colors.status_color(ticket.status)}{ticket.status}{colors.END}")
        print(f"  Priority: {colors.priority_color(ticket.priority)}{ticket.priority}{colors.END}")

        if ticket.assignee:
            print(f"  Assignee: {Path(ticket.assignee.path).stem}")
        if ticket.reporter:
            print(f"  Reporter: {Path(ticket.reporter.path).stem}")
        if ticket.due_date:
            overdue = ticket.due_date < date.today()
            color = colors.RED if overdue else ''
            print(f"  Due: {color}{ticket.due_date}{colors.END}")
        if ticket.project:
            print(f"  Project: {ticket.project}")
        if ticket.tags:
            print(f"  Tags: {', '.join(ticket.tags)}")
        if ticket.issue_link:
            print(f"  External: {ticket.issue_link}")
        print(f"  Progress: {ticket.progress}%")
        print(f"  File: {ticket_path}")

        # Show linked tickets if requested
        if getattr(args, 'links', False):
            print()
            if ticket.member_of:
                print("  Member of:")
                for link in ticket.member_of:
                    print(f"    - {link.path}")
            if ticket.blocked_by:
                print("  Blocked by:")
                for link in ticket.blocked_by:
                    print(f"    - {link.path}")
            if ticket.blocks:
                print("  Blocks:")
                for link in ticket.blocks:
                    print(f"    - {link.path}")
            if ticket.relates_to:
                print("  Related to:")
                for link in ticket.relates_to:
                    print(f"    - {link.path}")

    return EXIT_SUCCESS


def cmd_generate_link(args: argparse.Namespace, config: Config) -> int:
    """
    Generate a link to a ticket or person file in various formats.

    Outputs a link to stdout:
    - Default: relative path from config directory
    - With --full: absolute path
    - With --markdown: markdown format [text](./path)
    - With --transclusion: transclusion format ![[path]]

    For tickets, markdown text is the ticket ID.
    For people, markdown text is the person's name.
    """
    ref = args.ref  # Can be ticket ID or person reference
    use_full = getattr(args, 'full', False)
    use_markdown = getattr(args, 'markdown', False)
    use_transclusion = getattr(args, 'transclusion', False)

    # Try to find as ticket first
    ticket_path = find_ticket(ref, config, args)

    if ticket_path:
        # Get link path based on format
        if use_full:
            link_path = str(ticket_path)
        else:
            try:
                link_path = str(ticket_path.relative_to(config.directory))
            except ValueError:
                link_path = str(ticket_path)

        if use_transclusion:
            print(f"![[{link_path}]]")
        elif use_markdown:
            # Markdown format - use ticket ID for link text
            try:
                ticket = Ticket.from_file(ticket_path)
                link_text = ticket.id
            except ValueError:
                link_text = ticket_path.stem
            print(f"[{link_text}](./{link_path})")
        else:
            # Default: just the path
            print(link_path)
        return EXIT_SUCCESS

    # Try to find as person
    person_path = find_person(ref, config, args)

    if person_path:
        if use_full:
            link_path = str(person_path)
        else:
            try:
                link_path = str(person_path.relative_to(config.directory))
            except ValueError:
                link_path = str(person_path)

        if use_transclusion:
            print(f"![[{link_path}]]")
        elif use_markdown:
            # Markdown format - use person's display name for link text
            try:
                person = Person.from_file(person_path)
                name = person.name
                # If name looks like a filename (has underscores, no spaces), format it
                if '_' in name and ' ' not in name:
                    link_text = name.replace('_', ' ').title()
                else:
                    link_text = name
            except ValueError:
                link_text = person_path.stem.replace('_', ' ').title()
            print(f"[{link_text}](./{link_path})")
        else:
            # Default: just the path
            print(link_path)
        return EXIT_SUCCESS

    error(f"Not found: {ref}", EXIT_FILE_NOT_FOUND)


def cmd_get_id(args: argparse.Namespace, config: Config) -> int:
    """
    Get the ID from a ticket or person file path, or person name.

    Reverse operation of generate-link. Given a path or reference,
    returns the ticket ID or person ID.

    Args:
        args: Parsed command line arguments with 'ref'
        config: Config instance

    Returns:
        Exit code
    """
    ref = args.ref
    filepath = Path(ref)

    # Handle relative paths
    if not filepath.is_absolute():
        filepath = config.directory / filepath

    # If the path exists, try to extract ID from it
    if filepath.exists():
        # Try as ticket
        try:
            ticket = Ticket.from_file(filepath)
            print(ticket.id)
            return EXIT_SUCCESS
        except ValueError:
            pass

        # Try as person
        try:
            person = Person.from_file(filepath)
            if person.id:
                print(person.id)
            else:
                print(person.name)
            return EXIT_SUCCESS
        except ValueError:
            pass

        error(f"Could not extract ID from: {filepath}", EXIT_GENERAL_ERROR)

    # Path doesn't exist - try to resolve as a reference
    # Try as person reference first (more likely use case)
    person_path = find_person(ref, config, args)
    if person_path:
        try:
            person = Person.from_file(person_path)
            if person.id:
                print(person.id)
            else:
                print(person.name)
            return EXIT_SUCCESS
        except ValueError:
            pass

    # Try as ticket ID
    ticket_path = find_ticket(ref, config, args)
    if ticket_path:
        try:
            ticket = Ticket.from_file(ticket_path)
            print(ticket.id)
            return EXIT_SUCCESS
        except ValueError:
            pass

    error(f"Could not find ticket or person: {ref}", EXIT_FILE_NOT_FOUND)


def cmd_edit(args: argparse.Namespace, config: Config) -> int:
    """
    Edit ticket fields.

    If no ticket specified, opens fzf picker.
    If ticket specified but no updates, opens in $EDITOR.
    Otherwise updates frontmatter fields directly.
    """
    ticket_ref = getattr(args, 'ticket', None)

    # No ticket specified → fzf picker
    if not ticket_ref:
        # Check if fzf is available
        if not shutil.which('fzf'):
            error("fzf is required for interactive ticket selection", EXIT_INVALID_ARGS)

        # Get search paths and collect tickets
        search_paths = get_search_paths(args, config)
        tickets: list[Ticket] = []
        for search_path in search_paths:
            if search_path.exists():
                path_tickets = fallback_list_tickets(search_path, {})
                tickets.extend(path_tickets)

        if not tickets:
            error("No tickets found", EXIT_FILE_NOT_FOUND)

        # Format for fzf: ID | status | priority | assignee | title
        lines: list[str] = []
        for t in tickets:
            assignee = Path(t.assignee.path).stem if t.assignee else '-'
            line = f"{t.id}\t{t.status}\t{t.priority}\t{assignee}\t{t.title}"
            lines.append(line)

        # Run fzf with preview
        fzf_cmd = [
            'fzf',
            '--header', 'ID\tStatus\tPriority\tAssignee\tTitle',
            '--preview', 'vimban show {1} --content',
            '--preview-window', 'right:60%:wrap',
            '--delimiter', '\t',
        ]

        result = subprocess.run(
            fzf_cmd,
            input='\n'.join(lines),
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            # User cancelled
            return EXIT_SUCCESS

        ticket_ref = result.stdout.strip().split('\t')[0]

    ticket_path = find_ticket(ticket_ref, config, args)

    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    # Check if any updates are specified
    has_updates = (
        getattr(args, 'interactive', False) or
        getattr(args, 'status', None) or
        getattr(args, 'priority', None) or
        getattr(args, 'progress', None) is not None or
        getattr(args, 'due', None) or
        getattr(args, 'assignee', None) or
        getattr(args, 'add_tag', None) or
        getattr(args, 'remove_tag', None) or
        getattr(args, 'clear', None) or
        (getattr(args, 'fields', None) and len(args.fields) > 0)
    )

    # No updates specified → open in editor (implicit -i)
    if not has_updates:
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(ticket_path)])
        return EXIT_SUCCESS

    # Interactive edit flag
    if getattr(args, 'interactive', False):
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(ticket_path)])
        return EXIT_SUCCESS

    # Collect updates
    updates: dict[str, Any] = {}

    if getattr(args, 'status', None):
        updates['status'] = args.status
        # Auto-set end_date when moving to done
        if args.status == 'done':
            updates['end_date'] = date.today().isoformat()
    if getattr(args, 'priority', None):
        updates['priority'] = args.priority
    if getattr(args, 'progress', None) is not None:
        updates['progress'] = args.progress
    if getattr(args, 'due', None):
        due = parse_date(args.due)
        if due:
            updates['due_date'] = due.isoformat()

    # Handle assignee (search both work/personal scopes)
    if getattr(args, 'assignee', None):
        assignee = find_person_in_scopes(args.assignee, args, config)
        if assignee:
            updates['assignee'] = str(assignee)

    # Handle tags
    if getattr(args, 'add_tag', None):
        content = ticket_path.read_text()
        metadata, body = parse_frontmatter(content)
        tags = metadata.get('tags', [])
        if args.add_tag not in tags:
            tags.append(args.add_tag)
        updates['tags'] = tags

    if getattr(args, 'remove_tag', None):
        content = ticket_path.read_text()
        metadata, body = parse_frontmatter(content)
        tags = metadata.get('tags', [])
        if args.remove_tag in tags:
            tags.remove(args.remove_tag)
        updates['tags'] = tags

    # Handle clear
    if getattr(args, 'clear', None):
        updates[args.clear] = None

    # Handle positional field=value arguments
    for filter_arg in getattr(args, 'fields', []) or []:
        if '=' in filter_arg:
            field, value = filter_arg.split('=', 1)
            updates[field] = value

    if not updates:
        # This shouldn't happen due to has_updates check, but safety fallback
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(ticket_path)])
        return EXIT_SUCCESS

    # Preview changes
    if getattr(args, 'dry_run', False):
        print("=== DRY RUN ===")
        print(f"Would update {ticket_path}:")
        for field, value in updates.items():
            print(f"  {field}: {value}")
        return EXIT_SUCCESS

    # Apply updates
    content = ticket_path.read_text()
    metadata, body = parse_frontmatter(content)

    for field, value in updates.items():
        metadata[field] = value

    metadata['updated'] = datetime.now().isoformat()
    metadata['version'] = metadata.get('version', 0) + 1

    ticket_path.write_text(dump_frontmatter(metadata, body))

    fmt = getattr(args, 'format', 'plain')
    if fmt == 'json':
        print(json.dumps({'updated': ticket_ref, 'fields': list(updates.keys())}))
    else:
        print(f"Updated {ticket_ref}: {', '.join(updates.keys())}")

    return EXIT_SUCCESS


def cmd_move(args: argparse.Namespace, config: Config) -> int:
    """
    Move ticket to new status.

    Validates workflow transitions unless --force is specified.
    Sets end_date when moving to done.
    """
    ticket_ref = args.ticket
    new_status = args.status
    ticket_path = find_ticket(ticket_ref, config, args)

    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    content = ticket_path.read_text()
    metadata, body = parse_frontmatter(content)
    current_status = metadata.get('status', 'backlog')

    # Validate transition
    if not getattr(args, 'force', False):
        valid_next = VALID_TRANSITIONS.get(current_status, [])
        if new_status not in valid_next:
            error(
                f"Invalid transition: {current_status} -> {new_status}. "
                f"Valid transitions: {', '.join(valid_next)}",
                EXIT_VALIDATION_ERROR
            )

    # Update status
    metadata['status'] = new_status
    metadata['updated'] = datetime.now().isoformat()
    metadata['version'] = metadata.get('version', 0) + 1

    # Set end_date if completing (always set when moving to done)
    if new_status == 'done':
        metadata['end_date'] = date.today().isoformat()

    # Set start_date if starting
    if new_status == 'in_progress' and not metadata.get('start_date'):
        metadata['start_date'] = date.today().isoformat()

    # Clear end_date if reopening
    if getattr(args, 'reopen', False) and current_status in ('done', 'cancelled'):
        metadata['end_date'] = None

    ticket_path.write_text(dump_frontmatter(metadata, body))

    fmt = getattr(args, 'format', 'plain')
    if fmt == 'json':
        print(json.dumps({'ticket': ticket_ref, 'from': current_status, 'to': new_status}))
    else:
        print(f"Moved {ticket_ref}: {current_status} -> {new_status}")

    return EXIT_SUCCESS


def cmd_move_location(args: argparse.Namespace, config: Config) -> int:
    """
    Move a ticket file to a different directory.

    Updates all references to the old path in other files.
    Uses fzf for interactive selection if ticket or destination not specified.
    """
    ticket_ref: Optional[str] = getattr(args, 'ticket', None)
    destination: Optional[str] = getattr(args, 'destination', None)
    dry_run: bool = getattr(args, 'dry_run', False)
    no_update_refs: bool = getattr(args, 'no_update_refs', False)

    # Select ticket if not provided
    if not ticket_ref:
        if not shutil.which('fzf'):
            error("fzf is required for interactive ticket selection", EXIT_INVALID_ARGS)

        # Get search paths and collect tickets
        search_paths: list[Path] = get_search_paths(args, config)
        tickets: list[Ticket] = []
        for search_path in search_paths:
            if search_path.exists():
                path_tickets: list[Ticket] = fallback_list_tickets(search_path, {})
                tickets.extend(path_tickets)

        # Also search PARA directories
        para_search_paths: list[Path] = [
            config.directory / "02_areas",
            config.directory / "03_resources",
        ]
        for para_path in para_search_paths:
            if para_path.exists():
                path_tickets = fallback_list_tickets(para_path, {})
                tickets.extend(path_tickets)

        if not tickets:
            error("No tickets found", EXIT_FILE_NOT_FOUND)

        # Format for fzf: ID | status | type | title | path
        lines: list[str] = []
        for t in tickets:
            rel_path: str = str(t.filepath.relative_to(config.directory))
            line: str = f"{t.id}\t{t.status}\t{t.type}\t{t.title}\t{rel_path}"
            lines.append(line)

        # Run fzf
        fzf_cmd: list[str] = [
            'fzf',
            '--header', 'ID\tStatus\tType\tTitle\tPath',
            '--preview', 'vimban show {1} --content',
            '--preview-window', 'right:50%:wrap',
            '--delimiter', '\t',
        ]

        result = subprocess.run(
            fzf_cmd,
            input='\n'.join(lines),
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            return EXIT_SUCCESS  # User cancelled

        ticket_ref = result.stdout.strip().split('\t')[0]

    # Find ticket file
    ticket_path: Optional[Path] = find_ticket(ticket_ref, config, args)
    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    # Get old path (relative)
    old_rel_path: str = str(ticket_path.relative_to(config.directory))
    old_dir: str = str(ticket_path.parent.relative_to(config.directory))

    # Select destination if not provided
    if not destination:
        if not shutil.which('fzf'):
            error("fzf is required for interactive directory selection", EXIT_INVALID_ARGS)

        # Get PARA directories
        para_dirs: list[str] = get_para_directories(config)

        if not para_dirs:
            error("No directories found", EXIT_FILE_NOT_FOUND)

        # Run fzf for directory selection
        fzf_cmd = [
            'fzf',
            '--header', 'Select destination directory',
            '--preview', f'ls -la "{config.directory}/{{}}/" 2>/dev/null || echo "Empty or new directory"',
            '--preview-window', 'right:40%:wrap',
        ]

        result = subprocess.run(
            fzf_cmd,
            input='\n'.join(para_dirs),
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            return EXIT_SUCCESS  # User cancelled

        destination = result.stdout.strip()

    # Validate destination
    dest_path: Path = config.directory / destination
    if not dest_path.exists():
        error(f"Destination directory does not exist: {destination}", EXIT_FILE_NOT_FOUND)
    if not dest_path.is_dir():
        error(f"Destination is not a directory: {destination}", EXIT_INVALID_ARGS)

    # Calculate new path
    new_file_path: Path = dest_path / ticket_path.name
    new_rel_path: str = str(new_file_path.relative_to(config.directory))

    # Check if destination is different
    if old_rel_path == new_rel_path:
        print(f"File is already at {old_rel_path}")
        return EXIT_SUCCESS

    # Check if file already exists at destination
    if new_file_path.exists():
        error(f"File already exists at destination: {new_rel_path}", EXIT_VALIDATION_ERROR)

    # Find files that reference this path
    references: list[tuple[Path, str]] = []
    if not no_update_refs:
        references = find_files_referencing_path(old_rel_path, config, exclude_file=ticket_path)

    # Dry-run output
    if dry_run:
        print(f"Would move: {old_rel_path} -> {new_rel_path}")
        if references:
            print(f"\nWould update {len(references)} reference(s):")
            for ref_file, location in references:
                ref_rel: str = str(ref_file.relative_to(config.directory))
                print(f"  - {ref_rel} ({location})")
        else:
            print("\nNo references to update.")
        return EXIT_SUCCESS

    # Move the file
    try:
        shutil.move(str(ticket_path), str(new_file_path))
    except OSError as e:
        error(f"Failed to move file: {e}", EXIT_GENERAL_ERROR)

    print(f"Moved: {old_rel_path} -> {new_rel_path}")

    # Update references
    if references and not no_update_refs:
        updated_count: int = 0
        for ref_file, location in references:
            if update_path_references(ref_file, old_rel_path, new_rel_path, location):
                updated_count += 1
                ref_rel = str(ref_file.relative_to(config.directory))
                print(f"  Updated reference in: {ref_rel} ({location})")

        print(f"\nUpdated {updated_count} reference(s).")

    return EXIT_SUCCESS


def cmd_archive(args: argparse.Namespace, config: Config) -> int:
    """
    Archive a completed or cancelled ticket.

    Moves the ticket file to 04_archives/01_projects/<personal_or_work>/<status>/
    based on the ticket's current location and status.
    """
    ticket_ref: Optional[str] = getattr(args, 'ticket', None)
    dry_run: bool = getattr(args, 'dry_run', False)

    # Select ticket if not provided
    if not ticket_ref:
        if not shutil.which('fzf'):
            error("fzf is required for interactive ticket selection", EXIT_INVALID_ARGS)

        # Get search paths and collect only done/cancelled tickets
        search_paths: list[Path] = get_search_paths(args, config)
        tickets: list[Ticket] = []
        for search_path in search_paths:
            if search_path.exists():
                path_tickets: list[Ticket] = fallback_list_tickets(
                    search_path,
                    {'status': 'done,cancelled'},
                    include_archived=False
                )
                tickets.extend(path_tickets)

        if not tickets:
            error("No done/cancelled tickets found to archive", EXIT_FILE_NOT_FOUND)

        # Format for fzf: ID | status | type | title | path
        lines: list[str] = []
        for t in tickets:
            rel_path: str = str(t.filepath.relative_to(config.directory))
            line: str = f"{t.id}\t{t.status}\t{t.type}\t{t.title}\t{rel_path}"
            lines.append(line)

        # Run fzf
        fzf_cmd: list[str] = [
            'fzf',
            '--header', 'ID\tStatus\tType\tTitle\tPath',
            '--preview', 'vimban show {1} --content',
            '--preview-window', 'right:50%:wrap',
            '--delimiter', '\t',
        ]

        result = subprocess.run(
            fzf_cmd,
            input='\n'.join(lines),
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            return EXIT_SUCCESS  # User cancelled

        ticket_ref = result.stdout.strip().split('\t')[0]

    # Find ticket file
    ticket_path: Optional[Path] = find_ticket(ticket_ref, config, args)
    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    # Load ticket to get status
    content: str = ticket_path.read_text()
    metadata, _ = parse_frontmatter(content)
    status: str = metadata.get('status', 'unknown')

    # Validate status is archivable
    archivable_statuses: list[str] = ['done', 'cancelled']
    if status not in archivable_statuses:
        error(f"Cannot archive ticket with status '{status}'. Must be one of: {', '.join(archivable_statuses)}", EXIT_VALIDATION_ERROR)

    # Determine scope (personal or work) from current path
    rel_path: str = str(ticket_path.relative_to(config.directory))
    scope: str = 'personal'  # default
    if '/work/' in rel_path or rel_path.startswith('01_projects/work'):
        scope = 'work'
    elif '/personal/' in rel_path or rel_path.startswith('01_projects/personal'):
        scope = 'personal'

    # Calculate archive destination
    archive_dir: Path = config.directory / '04_archives' / '01_projects' / scope / status
    new_file_path: Path = archive_dir / ticket_path.name
    new_rel_path: str = str(new_file_path.relative_to(config.directory))

    # Check if already archived
    if '/04_archives/' in rel_path:
        print(f"Ticket is already archived: {rel_path}")
        return EXIT_SUCCESS

    # Check if file already exists at destination
    if new_file_path.exists():
        error(f"File already exists at destination: {new_rel_path}", EXIT_VALIDATION_ERROR)

    # Find files that reference this path
    references: list[tuple[Path, str]] = find_files_referencing_path(rel_path, config, exclude_file=ticket_path)

    # Dry-run output
    if dry_run:
        print(f"Would archive: {rel_path} -> {new_rel_path}")
        if references:
            print(f"\nWould update {len(references)} reference(s):")
            for ref_file, location in references:
                ref_rel: str = str(ref_file.relative_to(config.directory))
                print(f"  - {ref_rel} ({location})")
        else:
            print("\nNo references to update.")
        return EXIT_SUCCESS

    # Create archive directory if needed (with 00_index.md)
    if not archive_dir.exists():
        archive_dir.mkdir(parents=True, exist_ok=True)
        index_file: Path = archive_dir / '00_index.md'
        if not index_file.exists():
            index_file.touch()
        print(f"Created archive directory: {archive_dir.relative_to(config.directory)}")

    # Move the file
    try:
        shutil.move(str(ticket_path), str(new_file_path))
    except OSError as e:
        error(f"Failed to move file: {e}", EXIT_GENERAL_ERROR)

    print(f"Archived: {rel_path} -> {new_rel_path}")

    # Update references
    if references:
        updated_count: int = 0
        for ref_file, location in references:
            if update_path_references(ref_file, rel_path, new_rel_path, location):
                updated_count += 1
                ref_rel = str(ref_file.relative_to(config.directory))
                print(f"  Updated reference in: {ref_rel} ({location})")

        print(f"\nUpdated {updated_count} reference(s).")

    return EXIT_SUCCESS


def cmd_link(args: argparse.Namespace, config: Config) -> int:
    """
    Link tickets together.

    Supports bidirectional linking when --bidirectional is specified.
    """
    ticket_ref = args.ticket
    relation = args.relation
    target_ref = args.target
    ticket_path = find_ticket(ticket_ref, config, args)

    if not ticket_path:
        error(f"Ticket not found: {ticket_ref}", EXIT_FILE_NOT_FOUND)

    target_path = find_ticket(target_ref, config, args)
    if not target_path:
        error(f"Target ticket not found: {target_ref}", EXIT_FILE_NOT_FOUND)

    # Create transclusion link
    rel_target = str(target_path.relative_to(config.directory))
    target_link = create_transclusion(rel_target)

    # Load ticket
    content = ticket_path.read_text()
    metadata, body = parse_frontmatter(content)

    # Get or create the relationship list
    relations = metadata.get(relation, [])
    if not isinstance(relations, list):
        relations = [relations] if relations else []

    if getattr(args, 'remove', False):
        # Remove the link
        relations = [r for r in relations if target_link not in r]
    else:
        # Add the link if not already present
        if target_link not in relations:
            relations.append(target_link)

    metadata[relation] = relations
    metadata['updated'] = datetime.now().isoformat()
    metadata['version'] = metadata.get('version', 0) + 1

    if getattr(args, 'dry_run', False):
        print("=== DRY RUN ===")
        print(f"Would update {ticket_path}:")
        print(f"  {relation}: {relations}")
        return EXIT_SUCCESS

    ticket_path.write_text(dump_frontmatter(metadata, body))

    # Handle bidirectional
    if getattr(args, 'bidirectional', False) and not getattr(args, 'remove', False):
        reverse_relations = {
            'blocked_by': 'blocks',
            'blocks': 'blocked_by',
            'member_of': 'children',  # Parent tracks children in children field
            'relates_to': 'relates_to',
        }
        reverse = reverse_relations.get(relation)
        if reverse:
            rel_source = str(ticket_path.relative_to(config.directory))
            source_link = create_transclusion(rel_source)

            target_content = target_path.read_text()
            target_metadata, target_body = parse_frontmatter(target_content)

            target_relations = target_metadata.get(reverse, [])
            if not isinstance(target_relations, list):
                target_relations = [target_relations] if target_relations else []

            if source_link not in target_relations:
                target_relations.append(source_link)

            target_metadata[reverse] = target_relations
            target_metadata['updated'] = datetime.now().isoformat()
            target_metadata['version'] = target_metadata.get('version', 0) + 1

            target_path.write_text(dump_frontmatter(target_metadata, target_body))

    action = "Unlinked" if getattr(args, 'remove', False) else "Linked"
    print(f"{action} {ticket_ref} {relation} {target_ref}")

    return EXIT_SUCCESS


def cmd_dashboard(args: argparse.Namespace, config: Config) -> int:
    """
    Generate dashboard views.

    Supports daily, weekly, sprint, project, team, and person dashboards.
    """
    dashboard_type = getattr(args, 'type', 'daily') or 'daily'
    fmt = getattr(args, 'format', 'md')
    section = getattr(args, 'section', None)
    person_ref = getattr(args, 'person', None)

    # Get all tickets
    tickets = fallback_list_tickets(config.directory, {})
    today = date.today()

    # Filter based on dashboard type
    if dashboard_type == 'daily':
        # My tasks: in_progress, blocked, due today/overdue
        user = os.environ.get('USER', '')
        my_tickets = [t for t in tickets if t.assignee and user.lower() in str(t.assignee).lower()]
        active = [t for t in my_tickets if t.status in ('in_progress', 'blocked', 'review')]
        due_today = [t for t in my_tickets if t.due_date == today]
        overdue = [t for t in my_tickets if t.due_date and t.due_date < today and t.status != 'done']

        if section == 'assigned':
            tickets = active
        elif section == 'overdue':
            tickets = overdue
        elif section == 'due_soon':
            cutoff = today + timedelta(days=7)
            tickets = [t for t in my_tickets if t.due_date and t.due_date <= cutoff]
        else:
            # Full daily view
            output_lines = []
            output_lines.append("# Daily Dashboard")
            output_lines.append(f"\n**Date:** {today}")
            output_lines.append(f"\n## Active ({len(active)})")
            if active:
                output_lines.append(format_output(active, 'md', ['id', 'status', 'priority', 'title', 'due_date']))
            else:
                output_lines.append("_No active tickets_")

            output_lines.append(f"\n## Overdue ({len(overdue)})")
            if overdue:
                output_lines.append(format_output(overdue, 'md', ['id', 'status', 'priority', 'title', 'due_date']))
            else:
                output_lines.append("_No overdue tickets_")

            output_lines.append(f"\n## Due Today ({len(due_today)})")
            if due_today:
                output_lines.append(format_output(due_today, 'md', ['id', 'status', 'priority', 'title']))
            else:
                output_lines.append("_Nothing due today_")

            print('\n'.join(output_lines))
            return EXIT_SUCCESS

    elif dashboard_type == 'person' and person_ref:
        # Person-specific dashboard (search both work/personal scopes)
        person_link = find_person_in_scopes(person_ref, args, config)

        if not person_link:
            error(f"Person not found: {person_ref}", EXIT_FILE_NOT_FOUND)

        person_tickets = [
            t for t in tickets
            if t.assignee and person_link.path in str(t.assignee)
        ]

        if section:
            section = section.upper()
            if section == 'ASSIGNED':
                active = [t for t in person_tickets if t.status not in ('done', 'cancelled')]
                tickets = active
            elif section == 'BLOCKED':
                tickets = [t for t in person_tickets if t.status == 'blocked']
            elif section == 'OVERDUE':
                tickets = [t for t in person_tickets if t.due_date and t.due_date < today and t.status != 'done']
            elif section == 'DASHBOARD':
                active = [t for t in person_tickets if t.status not in ('done', 'cancelled')]
                tickets = active
        else:
            tickets = person_tickets

    # Output the results
    if not tickets:
        if fmt == 'md':
            print("_No tickets found_")
        else:
            print("No tickets found")
        return EXIT_SUCCESS

    output = format_output(
        tickets,
        fmt,
        columns=['id', 'status', 'priority', 'title', 'due_date'],
        colors=Colors(enabled=not getattr(args, 'no_color', False))
    )
    print(output)

    return EXIT_SUCCESS


def _fix_missing_end_date(ticket: Ticket) -> None:
    """
    Fix a done ticket that's missing end_date by using updated date.

    Updates both the file frontmatter and the in-memory ticket object.
    """
    if ticket.filepath and ticket.updated:
        content: str = ticket.filepath.read_text()
        metadata, body = parse_frontmatter(content)
        metadata['end_date'] = ticket.updated.date().isoformat()
        ticket.filepath.write_text(dump_frontmatter(metadata, body))
        ticket.end_date = ticket.updated.date()


def cmd_kanban(args: argparse.Namespace, config: Config) -> int:
    """
    Display kanban board view of tickets.

    Shows tickets organized by status columns in a kanban-style layout.
    Supports multiple output formats: plain (terminal), markdown, yaml, json.
    """
    # Build filters
    filters: dict[str, Any] = {}
    if getattr(args, 'project', None):
        filters['project'] = args.project
    if getattr(args, 'assignee', None):
        filters['assignee'] = args.assignee

    # Get search paths (work, personal, or both)
    search_paths = get_search_paths(args, config)

    # Collect tickets from all search paths
    tickets: list[Ticket] = []
    for search_path in search_paths:
        if search_path.exists():
            path_tickets = fallback_list_tickets(search_path, filters)
            tickets.extend(path_tickets)

    # Filter by --mine
    if getattr(args, 'mine', False):
        user = os.environ.get('USER', '')
        tickets = [t for t in tickets if t.assignee and user.lower() in str(t.assignee).lower()]

    # Handle --done-last filtering
    done_last = getattr(args, 'done_last', None)
    if done_last is not None:
        if done_last > 0:
            # Filter done tickets by end_date, with fallback to updated date
            cutoff_date = date.today() - timedelta(days=done_last)
            filtered_tickets: list[Ticket] = []
            for t in tickets:
                if t.status != 'done':
                    filtered_tickets.append(t)
                elif t.end_date and t.end_date >= cutoff_date:
                    filtered_tickets.append(t)
                elif t.end_date is None and t.updated and t.updated.date() >= cutoff_date:
                    # Fix frontmatter: set end_date from updated date
                    _fix_missing_end_date(t)
                    filtered_tickets.append(t)
            tickets = filtered_tickets
        # If done_last == 0, show all done (no filtering needed)

    # Parse statuses to display
    statuses: Optional[list[str]] = None
    status_arg = getattr(args, 'status', None)
    if status_arg:
        statuses = [s.strip() for s in status_arg.split(',')]
    elif done_last is not None:
        # Auto-include done column when --done-last is specified
        statuses = ['backlog', 'ready', 'in_progress', 'blocked', 'review', 'delegated', 'done']

    # Get output options
    fmt = getattr(args, 'format', 'plain')
    hide_empty = getattr(args, 'hide_empty', False)
    compact = getattr(args, 'compact', False)
    column_width = getattr(args, 'width', None)
    no_color = getattr(args, 'no_color', False)

    # Format and output
    output = format_kanban(
        tickets,
        fmt=fmt,
        colors=Colors(enabled=not no_color),
        hide_empty=hide_empty,
        compact=compact,
        column_width=column_width,
        statuses=statuses
    )

    print(output)
    return EXIT_SUCCESS


def cmd_search(args: argparse.Namespace, config: Config) -> int:
    """
    Search tickets by content.

    Uses ripgrep for full-text search with optional regex support.
    """
    query = args.query
    regex_mode = getattr(args, 'regex', False)
    case_sensitive = getattr(args, 'case_sensitive', False)
    files_only = getattr(args, 'files_only', False)
    context_lines = getattr(args, 'context', 0)

    # Build ripgrep command
    rg_cmd = ['rg']

    if not case_sensitive:
        rg_cmd.append('-i')

    if files_only:
        rg_cmd.append('-l')
    elif context_lines:
        rg_cmd.extend(['-C', str(context_lines)])

    if regex_mode:
        rg_cmd.append('-e')
    else:
        rg_cmd.append('-F')  # Fixed string

    rg_cmd.extend([query, str(config.directory)])
    rg_cmd.extend(['--glob', '*.md'])

    try:
        result = subprocess.run(rg_cmd, capture_output=True, text=True)
        if result.stdout:
            print(result.stdout)
        return EXIT_SUCCESS if result.returncode == 0 else EXIT_GENERAL_ERROR
    except FileNotFoundError:
        error("ripgrep (rg) not found", EXIT_GENERAL_ERROR)

    return EXIT_GENERAL_ERROR


def cmd_validate(args: argparse.Namespace, config: Config) -> int:
    """
    Validate ticket frontmatter.

    Checks for required fields and valid values.
    Optionally fixes issues with --fix.
    """
    files = getattr(args, 'files', None) or []
    fix_mode = getattr(args, 'fix', False)
    strict = getattr(args, 'strict', False)

    # Get files to validate
    if files:
        paths = [Path(f) for f in files]
    else:
        paths = list(config.directory.rglob('*.md'))

    errors = []
    warnings = []
    fixed = []

    for path in paths:
        try:
            content = path.read_text()
            metadata, body = parse_frontmatter(content)

            # Skip non-vimban files
            if not metadata.get('id') and not metadata.get('type'):
                continue

            # Check required fields
            if metadata.get('type') != 'person':
                required = ['id', 'title', 'type', 'status', 'created']
                for field in required:
                    if not metadata.get(field):
                        errors.append(f"{path}: Missing required field '{field}'")

            # Check valid status
            status = metadata.get('status', '')
            if status and status not in STATUSES:
                errors.append(f"{path}: Invalid status '{status}'")

            # Check valid type
            ticket_type = metadata.get('type', '')
            if ticket_type and ticket_type not in TICKET_TYPES + ['person']:
                errors.append(f"{path}: Invalid type '{ticket_type}'")

            # Check valid priority
            priority = metadata.get('priority', '')
            if priority and priority not in PRIORITIES:
                warnings.append(f"{path}: Invalid priority '{priority}'")

            # Check dates
            due_date = metadata.get('due_date')
            if due_date and not parse_date_field(due_date):
                warnings.append(f"{path}: Invalid due_date format")

        except (ValueError, yaml.YAMLError) as e:
            errors.append(f"{path}: Parse error: {e}")

    # Output results
    if errors:
        print("Errors:")
        for err in errors:
            print(f"  {err}")

    if warnings:
        print("\nWarnings:")
        for warn_msg in warnings:
            print(f"  {warn_msg}")

    if not errors and not warnings:
        print("All files valid")

    if strict and (errors or warnings):
        return EXIT_VALIDATION_ERROR

    return EXIT_SUCCESS if not errors else EXIT_VALIDATION_ERROR


def cmd_report(args: argparse.Namespace, config: Config) -> int:
    """
    Generate reports and analytics.

    Supports burndown, velocity, workload, aging, and blocker reports.
    """
    report_type = args.type
    project = getattr(args, 'project', None)

    # Get tickets
    filters = {'project': project} if project else {}
    tickets = fallback_list_tickets(config.directory, filters)

    if report_type == 'workload':
        # Count tickets by assignee
        workload: dict[str, dict[str, int]] = {}
        for t in tickets:
            if t.status in ('done', 'cancelled'):
                continue
            assignee = Path(t.assignee.path).stem if t.assignee else 'Unassigned'
            if assignee not in workload:
                workload[assignee] = {'total': 0, 'in_progress': 0, 'blocked': 0}
            workload[assignee]['total'] += 1
            if t.status == 'in_progress':
                workload[assignee]['in_progress'] += 1
            elif t.status == 'blocked':
                workload[assignee]['blocked'] += 1

        print("# Workload Report\n")
        print("| Assignee | Total | In Progress | Blocked |")
        print("|----------|-------|-------------|---------|")
        for assignee, counts in sorted(workload.items()):
            print(f"| {assignee} | {counts['total']} | {counts['in_progress']} | {counts['blocked']} |")

    elif report_type == 'blockers':
        # List blocked tickets
        blocked = [t for t in tickets if t.status == 'blocked']
        print(f"# Blockers Report ({len(blocked)} blocked)\n")
        if blocked:
            output = format_output(
                blocked, 'md',
                columns=['id', 'priority', 'assignee', 'title', 'due_date']
            )
            print(output)
        else:
            print("_No blocked tickets_")

    elif report_type == 'aging':
        # Tickets by age
        today = date.today()
        aging: dict[str, list[Ticket]] = {
            '< 1 week': [],
            '1-2 weeks': [],
            '2-4 weeks': [],
            '> 4 weeks': [],
        }

        for t in tickets:
            if t.status in ('done', 'cancelled'):
                continue
            age = (today - t.created.date()).days
            if age < 7:
                aging['< 1 week'].append(t)
            elif age < 14:
                aging['1-2 weeks'].append(t)
            elif age < 28:
                aging['2-4 weeks'].append(t)
            else:
                aging['> 4 weeks'].append(t)

        print("# Aging Report\n")
        for period, period_tickets in aging.items():
            print(f"## {period} ({len(period_tickets)})")
            if period_tickets:
                output = format_output(period_tickets, 'md', columns=['id', 'status', 'title'])
                print(output)
            print()

    else:
        warn(f"Report type '{report_type}' not fully implemented")
        print(f"Total tickets: {len(tickets)}")
        print(f"Active: {len([t for t in tickets if t.status not in ('done', 'cancelled')])}")
        print(f"Done: {len([t for t in tickets if t.status == 'done'])}")

    return EXIT_SUCCESS


def cmd_sync(args: argparse.Namespace, config: Config) -> int:
    """
    Sync with external systems.

    Stub implementation - providers not yet implemented.
    """
    provider_name = getattr(args, 'provider', None) or 'jira'
    dry_run = getattr(args, 'dry_run', False)

    if provider_name not in SYNC_PROVIDERS:
        error(f"Unknown sync provider: {provider_name}. Available: {', '.join(SYNC_PROVIDERS.keys())}")

    provider_class = SYNC_PROVIDERS[provider_name]
    provider = provider_class()

    try:
        if dry_run:
            print(f"=== DRY RUN: {provider.name} sync ===")
            print("Would authenticate and sync tickets")
            return EXIT_SUCCESS

        provider.authenticate()
    except NotImplementedError as e:
        error(str(e), EXIT_GENERAL_ERROR)

    return EXIT_SUCCESS


def _is_vimban_file(filepath: Path) -> bool:
    """
    Check if a file is a vimban-managed ticket file.

    Looks for YAML frontmatter with an 'id' field matching vimban
    ID patterns (PROJ-*, BUG-*, MTG-*, etc.).

    Args:
        filepath: Path to the markdown file to check

    Returns:
        True if file has vimban frontmatter with valid ID
    """
    if not filepath.exists() or filepath.suffix != '.md':
        return False

    try:
        content: str = filepath.read_text()
    except (OSError, IOError):
        return False

    # Check for YAML frontmatter
    if not content.startswith('---'):
        return False

    # Find end of frontmatter
    end_idx: int = content.find('---', 3)
    if end_idx == -1:
        return False

    frontmatter: str = content[3:end_idx]

    # Look for id field with vimban pattern
    # Patterns: PROJ-*, BUG-*, RESEARCH-*, AREA-*, RESOURCE-*,
    #           MTG-*, MNTR-*, JNL-*, RCP-*, PERSON-*
    id_pattern: re.Pattern = re.compile(
        r'^id:\s*["\']?'
        r'(PROJ|BUG|RESEARCH|AREA|RESOURCE|MTG|MNTR|JNL|RCP|PERSON)-\d+',
        re.MULTILINE
    )
    return bool(id_pattern.search(frontmatter))


def _find_vimban_files(directory: Path) -> list[Path]:
    """
    Find all vimban-managed files in a directory.

    Recursively searches for markdown files with vimban frontmatter.

    Args:
        directory: Root directory to search

    Returns:
        List of paths to vimban-managed files
    """
    vimban_files: list[Path] = []

    for md_file in directory.rglob('*.md'):
        # Skip hidden directories and .vimban config
        if any(part.startswith('.') for part in md_file.parts):
            continue
        if _is_vimban_file(md_file):
            vimban_files.append(md_file)

    return vimban_files


def cmd_commit(args: argparse.Namespace, config: Config) -> int:
    """
    Pull, commit, and push vimban changes to git remote.

    By default: pulls from remote, stages vimban-managed ticket files AND
    the .vimban/ configuration directory, creates a commit with either
    an auto-generated or user-provided message, and pushes to the
    configured remote.

    Use --all to stage all files in the directory instead.

    Args:
        args: Parsed command line arguments
        config: Vimban configuration object

    Returns:
        Exit code indicating success or failure
    """
    # Check git availability
    if not shutil.which('git'):
        error("git is not installed or not in PATH", EXIT_GIT_ERROR)

    directory: Path = config.directory
    verbose: bool = getattr(args, 'verbose', False)
    message: str = getattr(args, 'message', None)
    no_pull: bool = getattr(args, 'no_pull', False)
    no_push: bool = getattr(args, 'no_push', False)
    dry_run: bool = getattr(args, 'dry_run', False)
    stage_all: bool = getattr(args, 'all', False)
    pull_only: bool = getattr(args, 'pull', False)

    # Verify git repository
    git_dir: Path = directory / '.git'
    if not git_dir.exists():
        error(f"'{directory}' is not a git repository", EXIT_GIT_ERROR)

    # Handle pull-only mode
    if pull_only:
        if dry_run:
            print("=== DRY RUN ===")
            print(f"Directory: {directory}")
            print("Pull only (no commit/push)")
            return EXIT_SUCCESS

        # Check if remote is configured
        remote_result: subprocess.CompletedProcess = subprocess.run(
            ['git', 'remote'],
            cwd=directory,
            capture_output=True,
            text=True
        )
        if not remote_result.stdout.strip():
            error("No remote configured", EXIT_GIT_ERROR)

        info("Pulling from remote", verbose)
        pull_result: subprocess.CompletedProcess = subprocess.run(
            ['git', 'pull', '--rebase', '--autostash'],
            cwd=directory,
            capture_output=True,
            text=True
        )
        if pull_result.returncode != 0:
            error(f"git pull failed: {pull_result.stderr.strip()}", EXIT_GIT_ERROR)
        if pull_result.stdout.strip():
            print(pull_result.stdout.strip())
        else:
            print("Already up to date")
        return EXIT_SUCCESS

    # Generate commit message if not provided
    if not message:
        timestamp: str = datetime.now().strftime("%Y-%m-%d %H:%M")
        message = f"vimban: sync {timestamp}"

    # Find vimban files if not staging all
    vimban_files: list[Path] = []
    if not stage_all:
        vimban_files = _find_vimban_files(directory)
        if not vimban_files:
            print("No vimban files found")
            return EXIT_SUCCESS

    if dry_run:
        print("=== DRY RUN ===")
        print(f"Directory: {directory}")
        print(f"Pull: {'no' if no_pull else 'yes'}")
        config_dir: Path = directory / CONFIG_DIR_NAME
        if stage_all:
            print("Stage: all files")
        else:
            stage_desc: str = f"{len(vimban_files)} vimban files"
            if config_dir.exists():
                stage_desc += f" + {CONFIG_DIR_NAME}/"
            print(f"Stage: {stage_desc}")
        print(f"Message: {message}")
        print(f"Push: {'no' if no_push else 'yes'}")
        if not stage_all:
            print("\nFiles to stage:")
            for f in vimban_files:
                print(f"  {f.relative_to(directory)}")
            if config_dir.exists():
                print(f"  {CONFIG_DIR_NAME}/")
        return EXIT_SUCCESS

    # Check if remote is configured (needed for pull/push)
    remote_result: subprocess.CompletedProcess = subprocess.run(
        ['git', 'remote'],
        cwd=directory,
        capture_output=True,
        text=True
    )
    has_remote: bool = bool(remote_result.stdout.strip())

    # Pull first if remote exists and not disabled
    if not no_pull and has_remote:
        info("Pulling from remote", verbose)
        pull_result: subprocess.CompletedProcess = subprocess.run(
            ['git', 'pull', '--rebase', '--autostash'],
            cwd=directory,
            capture_output=True,
            text=True
        )
        if pull_result.returncode != 0:
            error(f"git pull failed: {pull_result.stderr.strip()}", EXIT_GIT_ERROR)
        if verbose and pull_result.stdout.strip():
            print(pull_result.stdout.strip())

    # Stage changes
    if stage_all:
        info(f"Staging all changes in {directory}", verbose)
        result: subprocess.CompletedProcess = subprocess.run(
            ['git', 'add', '-A'],
            cwd=directory,
            capture_output=True,
            text=True
        )
        if result.returncode != 0:
            error(f"git add failed: {result.stderr.strip()}", EXIT_GIT_ERROR)
    else:
        info(f"Staging {len(vimban_files)} vimban files", verbose)
        # Stage each vimban file individually
        for vimban_file in vimban_files:
            result = subprocess.run(
                ['git', 'add', str(vimban_file)],
                cwd=directory,
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                warn(f"Failed to stage {vimban_file}: {result.stderr.strip()}")

        # Stage .vimban/ directory if it exists
        config_dir: Path = directory / CONFIG_DIR_NAME
        if config_dir.exists():
            info(f"Staging {CONFIG_DIR_NAME}/ directory", verbose)
            result = subprocess.run(
                ['git', 'add', str(config_dir)],
                cwd=directory,
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                warn(f"Failed to stage {CONFIG_DIR_NAME}/: {result.stderr.strip()}")

    # Check if there are changes to commit
    status_result: subprocess.CompletedProcess = subprocess.run(
        ['git', 'status', '--porcelain'],
        cwd=directory,
        capture_output=True,
        text=True
    )
    if not status_result.stdout.strip():
        print("No changes to commit")
        return EXIT_SUCCESS

    # Create commit
    info(f"Creating commit: {message}", verbose)
    commit_result: subprocess.CompletedProcess = subprocess.run(
        ['git', 'commit', '-m', message],
        cwd=directory,
        capture_output=True,
        text=True
    )
    if commit_result.returncode != 0:
        error(f"git commit failed: {commit_result.stderr.strip()}", EXIT_GIT_ERROR)

    print(f"Committed: {message}")

    # Push if remote exists and not disabled
    if not no_push:
        if not has_remote:
            warn("No remote configured, skipping push")
            return EXIT_SUCCESS

        info("Pushing to remote", verbose)
        push_result: subprocess.CompletedProcess = subprocess.run(
            ['git', 'push'],
            cwd=directory,
            capture_output=True,
            text=True
        )
        if push_result.returncode != 0:
            error(f"git push failed: {push_result.stderr.strip()}", EXIT_GIT_ERROR)

        print("Pushed to remote")

    return EXIT_SUCCESS


def cmd_people_list(args: argparse.Namespace, config: Config) -> int:
    """List all people from work and/or personal directories."""
    people_dirs = get_people_dirs(args, config)

    people = []
    found_any = False
    for people_dir in people_dirs:
        if not people_dir.exists():
            continue
        found_any = True
        for person_file in people_dir.glob('*.md'):
            try:
                person = Person.from_file(person_file)
                people.append(person)
            except (ValueError, yaml.YAMLError):
                continue

    if not found_any:
        print("No people directory found")
        return EXIT_SUCCESS

    # Apply filters
    team = getattr(args, 'team', None)
    if team:
        people = [p for p in people if p.team and team.lower() in p.team.lower()]

    fmt = getattr(args, 'format', 'plain')
    output = format_output(
        people, fmt,
        columns=['name', 'role', 'team', 'email'],
        colors=Colors(enabled=not getattr(args, 'no_color', False))
    )

    if output:
        print(output)

    return EXIT_SUCCESS


def cmd_people_show(args: argparse.Namespace, config: Config) -> int:
    """Show person details, searching work and personal if not specified."""
    person_ref = args.person
    person_link = find_person_in_scopes(person_ref, args, config)

    if not person_link:
        error(f"Person not found: {person_ref}", EXIT_FILE_NOT_FOUND)

    person_path = config.directory / person_link.path

    if getattr(args, 'raw', False):
        print(person_path.read_text())
        return EXIT_SUCCESS

    try:
        person = Person.from_file(person_path)
    except ValueError as e:
        error(str(e), EXIT_VALIDATION_ERROR)

    fmt = getattr(args, 'format', 'plain')

    if fmt == 'json':
        print(json.dumps(person.to_dict(), indent=2))
    elif fmt == 'yaml':
        print(yaml.dump(person.to_dict(), default_flow_style=False))
    else:
        colors = Colors(enabled=not getattr(args, 'no_color', False))
        print(f"{colors.BOLD}{person.name}{colors.END}")
        if person.role:
            print(f"  Role: {person.role}")
        if person.team:
            print(f"  Team: {person.team}")
        if person.email:
            print(f"  Email: {person.email}")
        if person.slack:
            print(f"  Slack: {person.slack}")
        print(f"  File: {person_path}")

        # Show tickets if requested
        if getattr(args, 'tickets', False):
            tickets = fallback_list_tickets(config.directory, {})
            person_tickets = [
                t for t in tickets
                if t.assignee and person_link.path in str(t.assignee)
                and t.status not in ('done', 'cancelled')
            ]
            if person_tickets:
                print(f"\n  Active Tickets ({len(person_tickets)}):")
                for t in person_tickets[:10]:  # Limit to 10
                    print(f"    {t.id}: {t.title[:40]}")

    return EXIT_SUCCESS


def cmd_people_edit(args: argparse.Namespace, config: Config) -> int:
    """Open person file in $EDITOR."""
    person_ref = args.person
    person_link = find_person_in_scopes(person_ref, args, config)

    if not person_link:
        error(f"Person not found: {person_ref}", EXIT_FILE_NOT_FOUND)

    person_path = config.directory / person_link.path

    editor = os.environ.get('EDITOR', 'vim')
    subprocess.run([editor, str(person_path)])
    return EXIT_SUCCESS


def cmd_people_create(args: argparse.Namespace, config: Config) -> int:
    """Create a new person file, prompting for scope if not specified."""
    name = args.name

    # Get people directory (interactive prompt if no scope)
    people_dir = get_people_base(args, config, interactive=True)
    people_dir.mkdir(parents=True, exist_ok=True)

    # Generate filename from name
    filename = name.lower().replace(' ', '_') + '.md'
    filepath = people_dir / filename

    if filepath.exists():
        error(f"Person file already exists: {filepath}", EXIT_GENERAL_ERROR)

    # Generate ID for the person
    new_id = next_id(config.directory / CONFIG_DIR_NAME, ticket_type='person')

    # Load and fill template
    template = load_template('person')

    # Resolve manager if specified (search both scopes)
    manager = ''
    if getattr(args, 'manager', None):
        manager_link = find_person_in_scopes(args.manager, args, config)
        if manager_link:
            manager = str(manager_link)

    content = fill_template(
        template,
        ticket_id=new_id,
        title='',
        name=name,
        email=getattr(args, 'email', '') or '',
        role=getattr(args, 'role', '') or '',
        team=getattr(args, 'team', '') or '',
        manager=manager,
    )

    filepath.write_text(content)

    # Open in editor
    if not getattr(args, 'no_edit', False):
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(filepath)])

    print(f"Created person: {new_id} -> {filepath}")
    return EXIT_SUCCESS


def cmd_people(args: argparse.Namespace, config: Config) -> int:
    """People management dispatcher."""
    people_cmd = getattr(args, 'people_command', None)

    if people_cmd == 'list':
        return cmd_people_list(args, config)
    elif people_cmd == 'show':
        return cmd_people_show(args, config)
    elif people_cmd == 'edit':
        return cmd_people_edit(args, config)
    elif people_cmd == 'create':
        return cmd_people_create(args, config)
    elif people_cmd == 'dashboard':
        # Delegate to dashboard command with person context
        args.type = 'person'
        return cmd_dashboard(args, config)
    elif people_cmd == 'search':
        # Simple search through people files (both scopes if not specified)
        query = getattr(args, 'query', '')
        people_dirs = get_people_dirs(args, config)

        for people_dir in people_dirs:
            if not people_dir.exists():
                continue
            for person_file in people_dir.glob('*.md'):
                content = person_file.read_text()
                if query.lower() in content.lower():
                    metadata, _ = parse_frontmatter(content)
                    name = metadata.get('name', person_file.stem)
                    print(f"{name}: {person_file}")

        return EXIT_SUCCESS
    else:
        error("No people subcommand specified. Use: list, show, create, dashboard, search")

    return EXIT_GENERAL_ERROR


# ============================================================================
# MENTOR COMMAND
# ============================================================================
def cmd_mentor_new(args: argparse.Namespace, config: Config) -> int:
    """
    Create a new mentor meeting record.

    Creates a mentorship ticket linked to a person, opens in $EDITOR.
    By default, assumes you are the mentor. Use --mentored-by to flip.

    Args:
        args: Parsed arguments with person, mentored_by, date, no_edit
        config: Vimban configuration

    Returns:
        Exit code
    """
    person_ref = args.person
    mentored_by = getattr(args, 'mentored_by', False)
    meeting_date_str = getattr(args, 'date', None)
    no_edit = getattr(args, 'no_edit', False)
    dry_run = getattr(args, 'dry_run', False)
    fmt = getattr(args, 'format', 'plain')

    # Resolve person reference
    people_dirs = get_people_dirs(args, config)
    person_link: Optional[TransclusionLink] = None
    person_name: str = person_ref

    for people_dir in people_dirs:
        if people_dir.exists():
            person_link = resolve_person_reference(person_ref, people_dir)
            if person_link:
                # Get the person's display name from the file
                person_path = config.directory / person_link.path
                if person_path.exists():
                    content = person_path.read_text()
                    metadata, _ = parse_frontmatter(content)
                    person_name = metadata.get('name', person_ref)
                break

    if not person_link:
        error(f"Person not found: {person_ref}", EXIT_FILE_NOT_FOUND)

    # Parse meeting date
    meeting_date: Optional[datetime] = None
    if meeting_date_str:
        try:
            meeting_date = datetime.strptime(meeting_date_str, "%Y-%m-%d")
        except ValueError:
            error(f"Invalid date format: {meeting_date_str}. Use YYYY-MM-DD", EXIT_INVALID_ARGS)
    else:
        meeting_date = datetime.now()

    # Generate title based on direction
    if mentored_by:
        title = f"Mentoring Session with {person_name}"
    else:
        title = f"Mentor Meeting with {person_name}"

    # Get mentorship type configuration
    mentorship_type = MentorshipType
    type_config = mentorship_type.get_config()

    # Generate unique ID
    ticket_id = next_id(config.directory, prefix=type_config.prefix)

    # Get user's person reference (self)
    my_person_link: Optional[TransclusionLink] = None
    my_name = os.environ.get('USER', 'unknown')
    for people_dir in people_dirs:
        if people_dir.exists():
            my_person_link = resolve_person_reference(my_name, people_dir)
            if my_person_link:
                break

    # Determine assignee (mentee) and reporter (mentor)
    if mentored_by:
        # Person is the mentor, I am the mentee
        assignee_link = my_person_link
        reporter_link = person_link
    else:
        # I am the mentor, person is the mentee
        assignee_link = person_link
        reporter_link = my_person_link

    # Load and fill template
    template = load_template(type_config.template_name.replace('.md', ''))

    # Fill standard template fields
    content = fill_template(
        template,
        ticket_id=ticket_id,
        title=title,
        assignee=assignee_link,
        reporter=reporter_link,
        priority=config.default_priority,
        status=config.default_status,
    )

    # Apply type-specific replacements (like {{DATE}})
    type_replacements = mentorship_type.get_template_replacements(date=meeting_date)
    for placeholder, value in type_replacements.items():
        content = content.replace(placeholder, value)

    # Add the person to relates_to
    # Update the relates_to field in frontmatter to include person link
    # Quote the transclusion link to avoid YAML tag interpretation
    relates_to_replacement = f'relates_to:\n  - "{person_link}"'
    content = content.replace("relates_to: []", relates_to_replacement)

    # Determine output path
    filename = mentorship_type.get_filename(
        title,
        person_name=person_name,
        date=meeting_date
    )
    output_dir = mentorship_type.get_base_path(config.directory, person_name=person_name)
    output_path = output_dir / filename

    # Dry run - show what would be created
    if dry_run:
        print("=== DRY RUN ===")
        print(f"Would create: {output_path}")
        print(f"ID: {ticket_id}")
        print(f"Content:\n{content}")
        return EXIT_SUCCESS

    # Create directory and write the file
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content)

    # Open in editor unless --no-edit
    if not no_edit:
        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, str(output_path)])

    # Output result
    if fmt == 'json':
        result = {
            'id': ticket_id,
            'title': title,
            'path': str(output_path),
            'person': person_name,
            'direction': 'mentored_by' if mentored_by else 'mentoring',
        }
        print(json.dumps(result, indent=2))
    elif fmt == 'yaml':
        result = {
            'id': ticket_id,
            'title': title,
            'path': str(output_path),
            'person': person_name,
            'direction': 'mentored_by' if mentored_by else 'mentoring',
        }
        print(yaml.dump(result, default_flow_style=False))
    else:
        print(f"Created {ticket_id}: {title}")
        print(f"  Path: {output_path}")

    return EXIT_SUCCESS


def cmd_mentor_list(args: argparse.Namespace, config: Config) -> int:
    """
    List mentor meetings, optionally filtered by person.

    Args:
        args: Parsed arguments with optional person filter
        config: Vimban configuration

    Returns:
        Exit code
    """
    person_filter = getattr(args, 'person', None)
    mentored_by = getattr(args, 'mentored_by', False)
    fmt = getattr(args, 'format', 'plain')
    no_color = getattr(args, 'no_color', False)
    colors = Colors(enabled=not no_color)

    # Find all mentorship tickets
    mentorship_dir = config.directory / "02_areas" / "work" / "mentorship"

    if not mentorship_dir.exists():
        if fmt == 'json':
            print(json.dumps({'meetings': [], 'count': 0}))
        elif fmt == 'yaml':
            print(yaml.dump({'meetings': [], 'count': 0}))
        else:
            print("No mentorship meetings found.")
        return EXIT_SUCCESS

    meetings: list[dict] = []

    for meeting_file in sorted(mentorship_dir.glob('*.md'), reverse=True):
        try:
            ticket = Ticket.from_file(meeting_file)
            if ticket.type != 'mentorship':
                continue

            # Filter by person if specified
            if person_filter:
                person_match = False
                # Check both assignee (mentee) and reporter (mentor)
                if mentored_by:
                    # Looking for meetings where person is mentor (reporter)
                    if ticket.reporter and person_filter.lower() in str(ticket.reporter).lower():
                        person_match = True
                else:
                    # Looking for meetings where person is mentee (assignee)
                    if ticket.assignee and person_filter.lower() in str(ticket.assignee).lower():
                        person_match = True

                if not person_match:
                    continue

            meetings.append({
                'id': ticket.id,
                'title': ticket.title,
                'date': ticket.created.strftime('%Y-%m-%d') if ticket.created else '',
                'assignee': str(ticket.assignee) if ticket.assignee else '',
                'reporter': str(ticket.reporter) if ticket.reporter else '',
                'status': ticket.status,
                'path': str(meeting_file),
            })

        except Exception:
            continue

    # Output
    if fmt == 'json':
        print(json.dumps({'meetings': meetings, 'count': len(meetings)}, indent=2))
    elif fmt == 'yaml':
        print(yaml.dump({'meetings': meetings, 'count': len(meetings)}, default_flow_style=False))
    else:
        if not meetings:
            print("No mentorship meetings found.")
            return EXIT_SUCCESS

        # Group by person (assignee for mentoring, reporter for mentored_by)
        grouped: dict[str, list[dict]] = {}
        for m in meetings:
            key = m['assignee'] if not mentored_by else m['reporter']
            # Extract just the name from the transclusion link
            # Strip transclusion markers ![[...]] and quotes
            key = key.strip('"').replace('![[', '').replace(']]', '')
            if '/' in key:
                key = key.split('/')[-1].replace('.md', '').replace('_', ' ').title()
            if not key:
                key = 'Unknown'
            if key not in grouped:
                grouped[key] = []
            grouped[key].append(m)

        for person, person_meetings in grouped.items():
            print(f"\n{colors.BOLD}{person}{colors.END}")
            print("-" * len(person))
            for m in person_meetings:
                status_color = colors.status_color(m['status'])
                print(f"  {m['id']} [{status_color}{m['status']}{colors.END}] {m['date']} - {m['title']}")

        print(f"\nTotal: {len(meetings)} meeting(s)")

    return EXIT_SUCCESS


def cmd_mentor_show(args: argparse.Namespace, config: Config) -> int:
    """
    Show details of a specific mentor meeting.

    Args:
        args: Parsed arguments with meeting ID
        config: Vimban configuration

    Returns:
        Exit code
    """
    meeting_id = args.meeting_id
    fmt = getattr(args, 'format', 'plain')

    # Find the meeting
    ticket_path = find_ticket(meeting_id, config, args)

    if not ticket_path:
        error(f"Meeting not found: {meeting_id}", EXIT_FILE_NOT_FOUND)

    try:
        ticket = Ticket.from_file(ticket_path)
    except Exception as e:
        error(f"Error loading meeting: {e}", EXIT_GENERAL_ERROR)

    if ticket.type != 'mentorship':
        error(f"{meeting_id} is not a mentorship meeting (type: {ticket.type})", EXIT_INVALID_ARGS)

    # Set ticket attribute for cmd_show compatibility
    args.ticket = meeting_id

    # Delegate to show command
    return cmd_show(args, config)


def cmd_mentor(args: argparse.Namespace, config: Config) -> int:
    """
    Mentor command dispatcher.

    Args:
        args: Parsed arguments with mentor_command
        config: Vimban configuration

    Returns:
        Exit code
    """
    mentor_cmd = getattr(args, 'mentor_command', None)

    if mentor_cmd == 'new':
        return cmd_mentor_new(args, config)
    elif mentor_cmd == 'list':
        return cmd_mentor_list(args, config)
    elif mentor_cmd == 'show':
        return cmd_mentor_show(args, config)
    else:
        error("No mentor subcommand specified. Use: new, list, show")

    return EXIT_GENERAL_ERROR


# ============================================================================
# CONVERT COMMAND
# ============================================================================
def has_valid_vimban_frontmatter(filepath: Path) -> bool:
    """
    Check if file has valid vimban frontmatter with id and type fields.

    Args:
        filepath: Path to the markdown file

    Returns:
        True if file has valid vimban frontmatter, False otherwise
    """
    try:
        content = filepath.read_text()
        if not content.startswith('---'):
            return False

        # Find end of frontmatter
        end_idx = content.find('---', 3)
        if end_idx == -1:
            return False

        frontmatter = content[3:end_idx].strip()
        try:
            fm = yaml.safe_load(frontmatter)
            return fm is not None and 'id' in fm and 'type' in fm
        except yaml.YAMLError:
            return False
    except Exception:
        return False


def detect_type_from_path(filepath: Path, notes_dir: Path) -> str:
    """
    Determine vimban type based on file path.

    Checks specialized types first (most specific), then falls back
    to generic PARA types.

    Args:
        filepath: Path to the file
        notes_dir: Base notes directory

    Returns:
        Detected type name
    """
    try:
        rel_path = filepath.relative_to(notes_dir)
        rel_str = str(rel_path)
    except ValueError:
        return "resource"  # Fallback

    # Check specialized types first (most specific)
    for type_name, type_cls in SPECIALIZED_TYPE_REGISTRY.items():
        type_config = type_cls.get_config()
        parent_dir = "02_areas" if type_config.parent == "area" else "03_resources"
        type_path = f"{parent_dir}/{type_config.base_topic}"
        if rel_str.startswith(type_path + "/"):
            return type_name

    # Check for people (before generic area fallback)
    if "/people/" in rel_str and rel_str.endswith(".md"):
        return "person"

    # Fall back to generic PARA types
    if rel_str.startswith("02_areas/"):
        return "area"
    if rel_str.startswith("03_resources/"):
        return "resource"

    return "resource"  # Default fallback


def detect_scope_from_path(filepath: Path, notes_dir: Path) -> str:
    """
    Detect work/personal scope from file path.

    Checks if path is under a 'work' subdirectory.
    Defaults to 'personal' for all other paths.

    Args:
        filepath: Path to the file
        notes_dir: Base notes directory

    Returns:
        'work' or 'personal'
    """
    try:
        rel_path = filepath.relative_to(notes_dir)
        parts = rel_path.parts
        # Check if second part is 'work' (e.g., 02_areas/work/...)
        if len(parts) >= 2 and parts[1] == 'work':
            return 'work'
        return 'personal'
    except ValueError:
        return 'personal'


def extract_title_from_file(filepath: Path) -> str:
    """
    Extract title from first # heading or filename.

    Args:
        filepath: Path to the file

    Returns:
        Extracted title
    """
    try:
        content = filepath.read_text()

        # Skip frontmatter if present
        if content.startswith('---'):
            end = content.find('---', 3)
            if end != -1:
                content = content[end + 3:]

        # Look for first # heading
        for line in content.split('\n'):
            line = line.strip()
            if line.startswith('# '):
                return line[2:].strip()
    except Exception:
        pass

    # Fall back to filename
    basename = filepath.stem
    return basename.replace('_', ' ').replace('-', ' ').title()


def convert_file_to_vimban(
    filepath: Path,
    detected_type: str,
    notes_dir: Path
) -> str:
    """
    Add vimban frontmatter to file.

    Args:
        filepath: Path to the file
        detected_type: The type to assign
        notes_dir: Base notes directory

    Returns:
        Generated ID
    """
    # Extract title
    title = extract_title_from_file(filepath)

    # Generate ID
    config_dir = notes_dir / CONFIG_DIR_NAME
    new_id = next_id(config_dir, ticket_type=detected_type)

    # Determine if type has status
    has_status = True
    if detected_type == "resource":
        has_status = False
    elif is_specialized_type(detected_type):
        type_cls = get_specialized_type(detected_type)
        type_config = type_cls.get_config()
        has_status = type_config.has_status

    # Read existing content
    content = filepath.read_text()

    # Check for existing incomplete frontmatter
    existing_fm = {}
    body = content
    if content.startswith('---'):
        end = content.find('---', 3)
        if end != -1:
            try:
                fm_text = content[3:end].strip()
                existing_fm = yaml.safe_load(fm_text) or {}
                body = content[end + 3:].lstrip('\n')
            except yaml.YAMLError:
                pass

    # Build new frontmatter
    now = datetime.now().isoformat()
    new_fm = {
        'id': new_id,
        'title': title,
        'type': detected_type,
        'created': now,
        'updated': now,
        'version': 1,
    }

    # Add status for types that have it
    if has_status:
        new_fm['status'] = existing_fm.get('status', 'backlog')

    # Add type-specific fields
    if detected_type == "meeting":
        new_fm['date'] = datetime.now().strftime("%Y-%m-%d")
        new_fm['attendees'] = existing_fm.get('attendees', [])
    elif detected_type == "journal":
        new_fm['date'] = datetime.now().strftime("%Y-%m-%d")
        new_fm['mood'] = existing_fm.get('mood', '')
        new_fm['energy'] = existing_fm.get('energy', '')
        new_fm['status'] = 'active'
    elif detected_type == "recipe":
        new_fm['prep_time'] = existing_fm.get('prep_time', '')
        new_fm['cook_time'] = existing_fm.get('cook_time', '')
        new_fm['total_time'] = existing_fm.get('total_time', '')
        new_fm['servings'] = existing_fm.get('servings', '')
        new_fm['difficulty'] = existing_fm.get('difficulty', '')
        new_fm['cuisine'] = existing_fm.get('cuisine', '')
        new_fm['diet'] = existing_fm.get('diet', ['carnivore'])

    # Add tags (preserve existing)
    new_fm['tags'] = existing_fm.get('tags', [])

    # Merge existing fields (don't overwrite what we've set)
    for key, value in existing_fm.items():
        if key not in new_fm:
            new_fm[key] = value

    # Build new content
    fm_yaml = yaml.dump(new_fm, default_flow_style=False, allow_unicode=True, sort_keys=False)
    new_content = f"---\n{fm_yaml}---\n\n{body}"

    # Write file
    filepath.write_text(new_content)

    return new_id


def cmd_convert_find_missing(args: argparse.Namespace, config: Config) -> int:
    """
    Find and convert untracked markdown files.

    Scans specified directories for files missing vimban frontmatter
    and optionally converts them.
    """
    notes_dir = config.directory
    dry_run = getattr(args, 'dry_run', False)
    no_confirm = getattr(args, 'no_confirm', False)
    verbose = getattr(args, 'verbose', False)

    # Determine scan paths based on flags
    scan_paths: list[Path] = []
    if getattr(args, 'meetings', False):
        scan_paths.append(notes_dir / "02_areas" / "work" / "meetings")
    if getattr(args, 'journals', False):
        scan_paths.append(notes_dir / "02_areas" / "personal" / "journal")
    if getattr(args, 'recipes', False):
        scan_paths.append(notes_dir / "03_resources" / "food_and_health" / "recipes")
    if getattr(args, 'people', False):
        scan_paths.append(notes_dir / "02_areas" / "work" / "people")
        scan_paths.append(notes_dir / "02_areas" / "personal" / "people")
    if getattr(args, 'areas', False):
        scan_paths.append(notes_dir / "02_areas")
    if getattr(args, 'resources', False):
        scan_paths.append(notes_dir / "03_resources")

    if not scan_paths:
        print("Error: At least one scope flag required (--areas, --resources, --meetings, --journals, --recipes, --people)")
        return EXIT_INVALID_ARGS

    # Find untracked files
    untracked: list[tuple[Path, str]] = []
    seen: set[Path] = set()

    for scan_path in scan_paths:
        if not scan_path.exists():
            if verbose:
                print(f"Skipping non-existent path: {scan_path}")
            continue

        for filepath in scan_path.rglob("*.md"):
            # Skip if already processed
            if filepath in seen:
                continue
            seen.add(filepath)

            # Skip hidden files/directories
            if any(part.startswith('.') for part in filepath.parts):
                continue

            # Skip special files
            if filepath.name.startswith('00_') or filepath.name.lower() in ('readme.md',):
                if verbose:
                    print(f"Skipping special file: {filepath}")
                continue

            # Check if already has valid frontmatter
            if has_valid_vimban_frontmatter(filepath):
                if verbose:
                    print(f"Skipping (already tracked): {filepath}")
                continue

            # Detect type and scope, add to list
            detected_type = detect_type_from_path(filepath, notes_dir)
            detected_scope = detect_scope_from_path(filepath, notes_dir)
            untracked.append((filepath, detected_type, detected_scope))

    if not untracked:
        print("No untracked files found.")
        return EXIT_SUCCESS

    # Process based on mode
    if dry_run:
        print(f"Would convert {len(untracked)} files:\n")
        for filepath, detected_type, detected_scope in untracked:
            try:
                rel_path = filepath.relative_to(notes_dir)
            except ValueError:
                rel_path = filepath
            print(f"  {rel_path} -> {detected_type} ({detected_scope})")
        print("\nNo changes made (dry run).")
        return EXIT_SUCCESS

    # Interactive or batch mode
    converted = 0
    skipped = 0
    errors = 0

    for i, (filepath, detected_type, detected_scope) in enumerate(untracked):
        try:
            rel_path = filepath.relative_to(notes_dir)
        except ValueError:
            rel_path = filepath

        if not no_confirm:
            print(f"\n[{i+1}/{len(untracked)}] {rel_path}")
            print(f"      Detected type: {detected_type} ({detected_scope})")
            try:
                response = input(f"      Convert to {detected_type}? [y/n/q]: ").lower().strip()
            except (EOFError, KeyboardInterrupt):
                print("\n\nAborted.")
                break

            if response == 'q':
                print("Quitting...")
                break
            if response != 'y':
                skipped += 1
                print("      Skipped")
                continue

        try:
            new_id = convert_file_to_vimban(filepath, detected_type, notes_dir)
            if no_confirm:
                print(f"  Converted: {rel_path} -> {new_id} ({detected_scope})")
            else:
                print(f"      Converted: {new_id}")
            converted += 1
        except Exception as e:
            if no_confirm:
                print(f"  Error: {rel_path}: {e}")
            else:
                print(f"      Error: {e}")
            errors += 1

    print(f"\nSummary:")
    print(f"  Converted: {converted}")
    if skipped:
        print(f"  Skipped: {skipped}")
    if errors:
        print(f"  Errors: {errors}")

    return EXIT_SUCCESS if errors == 0 else EXIT_GENERAL_ERROR


def cmd_convert(args: argparse.Namespace, config: Config) -> int:
    """Handle convert subcommands."""
    convert_cmd = getattr(args, 'convert_cmd', None)

    if convert_cmd == 'find-missing':
        return cmd_convert_find_missing(args, config)
    else:
        print("Usage: vimban convert find-missing [--areas] [--resources] [--meetings] [--journals] [--recipes] [--people]")
        print("       [--dry-run] [--no-confirm] [-v]")
        return EXIT_INVALID_ARGS


def cmd_completion(args: argparse.Namespace, config: Config) -> int:
    """Output shell completion script."""
    shell = getattr(args, 'shell', 'bash')
    if shell == 'bash':
        print(BASH_COMPLETION_SCRIPT)
    else:
        error(f"Unsupported shell: {shell}", EXIT_INVALID_ARGS)
    return EXIT_SUCCESS


def cmd_tui(args: argparse.Namespace, config: Config) -> int:
    """
    Launch the interactive TUI interface.

    Calls vimban_tui script with appropriate arguments.
    """
    cmd: list[str] = ['vimban_tui']

    if getattr(args, 'layout', None):
        cmd.extend(['--layout', args.layout])
    if getattr(args, 'view', None):
        cmd.extend(['--view', args.view])

    cmd.extend(['--directory', str(config.directory)])

    result = subprocess.run(cmd)
    return result.returncode


def cmd_comments(args: argparse.Namespace, config: Config) -> int:
    """
    List recent comments across all tickets.

    Supports filtering by user (-u/--user), days (--days), and sorting.
    Default sort is newest first.
    """
    no_color = getattr(args, 'no_color', False)
    colors = Colors(enabled=not no_color)
    fmt = getattr(args, 'format', 'plain')

    # Get filter options
    user_filter = getattr(args, 'user', None)
    if user_filter:
        user_filter = user_filter.lower()
    days = getattr(args, 'days', None)
    reverse_sort = getattr(args, 'reverse', False)
    limit = getattr(args, 'limit', None)

    # Calculate cutoff date if --days specified
    cutoff_date = None
    if days is not None:
        cutoff_date = datetime.now() - timedelta(days=days)

    # Collect all comments across tickets
    all_comments: list[tuple[str, str, Path, Comment]] = []  # (ticket_id, ticket_title, path, comment)

    # Find all markdown files
    for md_file in config.directory.rglob('*.md'):
        try:
            content = md_file.read_text()
            metadata, _ = parse_frontmatter(content)

            # Skip files without vimban frontmatter
            if not metadata.get('id') or not metadata.get('type'):
                continue

            # Skip person files (they have comments but we focus on tickets)
            if metadata.get('type') == 'person':
                continue

            ticket_id = metadata.get('id', '').strip('"')
            ticket_title = metadata.get('title', md_file.stem)

            comments = parse_comments(content)
            for comment in comments:
                # Filter by user
                if user_filter:
                    author_match = comment.author and user_filter in comment.author.lower()
                    # Also check replies for user matches
                    reply_match = any(
                        a and user_filter in a.lower()
                        for _, _, a in comment.replies
                    )
                    if not author_match and not reply_match:
                        continue

                # Filter by date
                if cutoff_date:
                    # Check comment timestamp
                    if comment.timestamp < cutoff_date:
                        # Still check replies - they might be newer
                        recent_replies = [
                            (ts, c, a) for ts, c, a in comment.replies
                            if ts >= cutoff_date
                        ]
                        if not recent_replies:
                            continue

                all_comments.append((ticket_id, ticket_title, md_file, comment))

        except (ValueError, yaml.YAMLError):
            continue

    # Sort by timestamp (newest first by default)
    def get_latest_timestamp(item: tuple) -> datetime:
        """Get the most recent timestamp from comment or its replies."""
        _, _, _, comment = item
        timestamps = [comment.timestamp]
        timestamps.extend(ts for ts, _, _ in comment.replies)
        return max(timestamps)

    all_comments.sort(key=get_latest_timestamp, reverse=not reverse_sort)

    # Apply limit
    if limit:
        all_comments = all_comments[:limit]

    if not all_comments:
        print("No comments found")
        return EXIT_SUCCESS

    # Format output
    if fmt == 'json':
        data = []
        for ticket_id, ticket_title, path, comment in all_comments:
            entry = comment.to_dict()
            entry['ticket_id'] = ticket_id
            entry['ticket_title'] = ticket_title
            entry['filepath'] = str(path)
            data.append(entry)
        print(json.dumps(data, indent=2, default=str))
        return EXIT_SUCCESS

    if fmt == 'yaml':
        data = []
        for ticket_id, ticket_title, path, comment in all_comments:
            entry = comment.to_dict()
            entry['ticket_id'] = ticket_id
            entry['ticket_title'] = ticket_title
            entry['filepath'] = str(path)
            data.append(entry)
        print(yaml.dump(data, default_flow_style=False, allow_unicode=True))
        return EXIT_SUCCESS

    # Plain or md format
    lines = []
    for ticket_id, ticket_title, path, comment in all_comments:
        # Header with ticket info
        author_part = f" by {comment.author}" if comment.author else ""
        if fmt == 'md':
            lines.append(f"### [{ticket_id}] {ticket_title}")
            lines.append(f"**Comment #{comment.id}**{author_part} ({comment.timestamp.isoformat()})")
            lines.append("")
            for content_line in comment.content.split('\n'):
                lines.append(f"> {content_line}" if content_line else ">")
            if comment.replies:
                for ts, reply, reply_author in comment.replies:
                    reply_author_part = f" by {reply_author}" if reply_author else ""
                    lines.append(f"#### Reply{reply_author_part} ({ts.isoformat()})")
                    for reply_line in reply.split('\n'):
                        lines.append(f">> {reply_line}" if reply_line else ">>")
            lines.append("")
        else:
            # Plain format
            lines.append(
                f"{colors.BOLD}[{ticket_id}]{colors.END} {ticket_title}"
            )
            lines.append(
                f"  {colors.CYAN}#{comment.id}{author_part}{colors.END} "
                f"{colors.DIM}({comment.timestamp.strftime('%Y-%m-%d %H:%M')}){colors.END}"
            )
            # Indent comment content
            for content_line in comment.content.split('\n'):
                lines.append(f"    {content_line}")
            if comment.replies:
                for ts, reply, reply_author in comment.replies:
                    reply_author_part = f" by {reply_author}" if reply_author else ""
                    lines.append(
                        f"    {colors.CYAN}↳ Reply{reply_author_part}{colors.END} "
                        f"{colors.DIM}({ts.strftime('%Y-%m-%d %H:%M')}){colors.END}"
                    )
                    for reply_line in reply.split('\n'):
                        lines.append(f"      {reply_line}")
            lines.append("")

    print('\n'.join(lines))
    return EXIT_SUCCESS


def cmd_comment(args: argparse.Namespace, config: Config) -> int:
    """
    Add or view comments on tickets and people.

    Supports:
    - Adding new comments (from args or stdin)
    - Threading with --reply-to
    - Viewing with --print and --print-full
    - Script-friendly --new-id-output
    """
    target_ref = args.target
    no_color = getattr(args, 'no_color', False)
    colors = Colors(enabled=not no_color)
    fmt = getattr(args, 'format', 'plain')

    # Resolve target (try ticket first, then person)
    target_path = find_ticket(target_ref, config, args)

    if not target_path:
        # Try as person
        people_dirs = get_people_dirs(args, config)
        for pdir in people_dirs:
            if pdir.exists():
                person_link = resolve_person_reference(target_ref, pdir)
                if person_link:
                    target_path = config.directory / person_link.path
                    break

    if not target_path or not target_path.exists():
        error(f"Target not found: {target_ref}", EXIT_FILE_NOT_FOUND)

    content = target_path.read_text()

    # Handle --print or --print-full (view mode)
    print_range = getattr(args, 'print', None)
    print_full_range = getattr(args, 'print_full', None)

    if print_range is not None or print_full_range is not None:
        comments = parse_comments(content)
        if not comments:
            print("No comments found")
            return EXIT_SUCCESS

        max_id = max(c.id for c in comments)
        range_str = print_full_range if print_full_range is not None else print_range
        include_threads = print_full_range is not None

        ids = parse_comment_range(range_str, max_id)
        output = format_comment_output(
            comments, ids, include_threads=include_threads, fmt=fmt, colors=colors
        )
        print(output)
        return EXIT_SUCCESS

    # Adding a comment - get text from args or stdin
    text = getattr(args, 'text', None)
    if not text and not sys.stdin.isatty():
        text = sys.stdin.read().strip()

    if not text:
        error("Comment text is required (provide as argument or via stdin)", EXIT_INVALID_ARGS)

    # Handle --edit if specified
    if getattr(args, 'edit', False):
        import tempfile
        with tempfile.NamedTemporaryFile(
            mode='w',
            suffix='.md',
            delete=False
        ) as f:
            f.write(text)
            temp_file = f.name

        editor = os.environ.get('EDITOR', 'vim')
        subprocess.run([editor, temp_file])

        with open(temp_file, 'r') as f:
            text = f.read().strip()
        os.unlink(temp_file)

        if not text:
            print("Comment cancelled (empty text)")
            return EXIT_SUCCESS

    reply_to = getattr(args, 'reply_to', None)
    user = getattr(args, 'user', None)

    # Handle --dry-run
    if getattr(args, 'dry_run', False):
        print("=== DRY RUN ===")
        print(f"Target: {target_path}")
        if user:
            print(f"Author: {user}")
        if reply_to:
            print(f"Reply to: #{reply_to}")
        else:
            next_id = get_next_comment_id(content)
            print(f"New comment ID: #{next_id}")
        print(f"Text:\n{text}")
        return EXIT_SUCCESS

    # Insert comment
    try:
        comment_id = insert_comment(target_path, text, reply_to=reply_to, author=user)
    except ValueError as e:
        error(str(e), EXIT_VALIDATION_ERROR)

    # Handle output
    new_id_output = getattr(args, 'new_id_output', False)

    if new_id_output:
        # Script-friendly: just output the ID
        print(comment_id)
    else:
        if reply_to:
            print(f"Reply added to comment #{reply_to}")
        else:
            print(f"Comment #{comment_id} added to {target_path.name}")

    return EXIT_SUCCESS


# ============================================================================
# MCP SERVER
# ============================================================================
def ticket_to_mcp_dict(ticket: Ticket) -> dict:
    """
    Convert a Ticket object to a JSON-serializable dictionary for MCP.

    Args:
        ticket: The Ticket object to convert

    Returns:
        Dictionary with all ticket fields, dates as ISO strings
    """
    return {
        'id': ticket.id,
        'title': ticket.title,
        'type': ticket.type,
        'status': ticket.status,
        'priority': ticket.priority,
        'assignee': str(ticket.assignee) if ticket.assignee else None,
        'reporter': str(ticket.reporter) if ticket.reporter else None,
        'watchers': [str(w) for w in ticket.watchers],
        'created': ticket.created.isoformat() if ticket.created else None,
        'updated': ticket.updated.isoformat() if ticket.updated else None,
        'start_date': str(ticket.start_date) if ticket.start_date else None,
        'due_date': str(ticket.due_date) if ticket.due_date else None,
        'end_date': str(ticket.end_date) if ticket.end_date else None,
        'effort': ticket.effort,
        'tags': ticket.tags,
        'project': ticket.project,
        'sprint': ticket.sprint,
        'member_of': [str(m) for m in ticket.member_of],
        'relates_to': [str(r) for r in ticket.relates_to],
        'blocked_by': [str(b) for b in ticket.blocked_by],
        'blocks': [str(b) for b in ticket.blocks],
        'progress': ticket.progress,
        'checklist_total': ticket.checklist_total,
        'checklist_done': ticket.checklist_done,
        'issue_link': ticket.issue_link,
        'filepath': str(ticket.filepath),
    }


def person_to_mcp_dict(person: Person) -> dict:
    """
    Convert a Person object to a JSON-serializable dictionary for MCP.

    Args:
        person: The Person object to convert

    Returns:
        Dictionary with all person fields
    """
    return {
        'name': person.name,
        'email': person.email,
        'slack': person.slack,
        'role': person.role,
        'team': person.team,
        'manager': str(person.manager) if person.manager else None,
        'direct_reports': [str(d) for d in person.direct_reports],
        'created': person.created.isoformat() if person.created else None,
        'updated': person.updated.isoformat() if person.updated else None,
        'filepath': str(person.filepath),
    }


def create_mcp_server(host: str = "127.0.0.1", port: int = 5004):
    """
    Create and configure the MCP server for vimban.

    Args:
        host: Host to bind to (for HTTP transport)
        port: Port to bind to (for HTTP transport)

    Provides tools for:
    - Creating tickets
    - Listing and filtering tickets
    - Getting ticket details
    - Editing tickets
    - Moving tickets between statuses
    - Managing ticket relationships
    - Comments
    - Dashboards and kanban views
    - Search
    - Reports
    - People management
    """
    if not MCP_AVAILABLE:
        print("Error: mcp package required for MCP server.", file=sys.stderr)
        print("Install with: pip install 'mcp[cli]'", file=sys.stderr)
        sys.exit(1)

    mcp = FastMCP("vimban", json_response=True, host=host, port=port)

    # Load default config
    directory = Path(environ.get('VIMBAN_DIR', '') or str(DEFAULT_DIR)).expanduser()
    config = Config.load(directory)

    @mcp.tool()
    def create_ticket(
        ticket_type: str,
        title: str,
        assignee: str = "",
        reporter: str = "",
        priority: str = "",
        tags: str = "",
        project: str = "",
        member_of: str = "",
        due_date: str = "",
        effort: int = 0,
        topic: str = "",
        ticket_date: str = "",
        scope: str = "",
    ) -> dict:
        """
        Create a new ticket.

        Args:
            ticket_type: Type of ticket (epic, story, task, sub-task, research,
                        bug, area, resource, meeting, journal, recipe)
            title: The ticket title
            assignee: Person reference to assign the ticket to
            reporter: Person reference who reported the ticket
            priority: Priority level (critical, high, medium, low)
            tags: Comma-separated list of tags
            project: Project identifier
            member_of: Parent ticket ID this belongs to
            due_date: Due date (YYYY-MM-DD or +7d for relative)
            effort: Story points / effort estimate
            topic: Topic path for area/resource types (e.g., 'technical/linux')
            ticket_date: Date for meeting type (YYYY-MM-DD)
            scope: Scope filter (work, personal, or empty for default)

        Returns:
            Dictionary with created ticket details or error
        """
        # Build args namespace
        args = argparse.Namespace(
            type=ticket_type,
            title=title,
            assignee=assignee if assignee else None,
            reporter=reporter if reporter else None,
            watcher=None,
            priority=priority if priority else None,
            tags=tags if tags else None,
            project=project if project else None,
            member_of=[member_of] if member_of else None,
            due=due_date if due_date else None,
            effort=effort if effort else None,
            topic=topic if topic else None,
            date=ticket_date if ticket_date else None,
            id=None,
            prefix=None,
            output=None,
            no_edit=True,
            dry_run=False,
            work=scope == "work",
            personal=scope == "personal",
            format='json',
            quiet=True,
            verbose=False,
        )

        try:
            # Handle specialized types (meeting, journal, recipe)
            if is_specialized_type(ticket_type):
                result = cmd_create_para(args, config)
                if result == EXIT_SUCCESS:
                    # Find the created ticket
                    filters = {'type': ticket_type}
                    search_paths = get_search_paths(args, config)
                    para_paths = get_para_search_paths(args, config)
                    search_paths.extend(para_paths)
                    for search_path in search_paths:
                        if search_path.exists():
                            tickets = fallback_list_tickets(search_path, filters, [])
                            if tickets:
                                tickets.sort(key=lambda t: t.created, reverse=True)
                                return {"success": True, "ticket": ticket_to_mcp_dict(tickets[0])}
                    return {"success": True, "message": f"Created {ticket_type}"}
                return {"error": f"Failed to create {ticket_type}"}
            else:
                result = cmd_create(args, config)
                if result == EXIT_SUCCESS:
                    filters = {'type': ticket_type}
                    search_paths = get_search_paths(args, config)
                    for search_path in search_paths:
                        if search_path.exists():
                            tickets = fallback_list_tickets(search_path, filters, [])
                            if tickets:
                                tickets.sort(key=lambda t: t.created, reverse=True)
                                return {"success": True, "ticket": ticket_to_mcp_dict(tickets[0])}
                    return {"success": True, "message": f"Created {ticket_type}"}
                return {"error": f"Failed to create {ticket_type}"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def list_tickets(
        status: str = "",
        ticket_type: str = "",
        assignee: str = "",
        project: str = "",
        tags: str = "",
        priority: str = "",
        overdue: bool = False,
        due_soon: int = 0,
        blocked: bool = False,
        unassigned: bool = False,
        mine: bool = False,
        include_areas: bool = False,
        include_resources: bool = False,
        sort: str = "due_date",
        limit: int = 50,
        scope: str = "",
    ) -> dict:
        """
        List tickets with optional filters.

        Args:
            status: Filter by status (comma-separated: backlog,ready,in_progress,
                   blocked,review,delegated,done,cancelled)
            ticket_type: Filter by type (comma-separated: epic,story,task,etc.)
            assignee: Filter by assignee (partial match)
            project: Filter by project identifier
            tags: Filter by tags (comma-separated)
            priority: Filter by priority (critical,high,medium,low)
            overdue: Only show overdue tickets
            due_soon: Only show tickets due within N days (0 = disabled)
            blocked: Only show blocked tickets
            unassigned: Only show unassigned tickets
            mine: Only show tickets assigned to $USER
            include_areas: Include area type items
            include_resources: Include resource type items
            sort: Sort field (due_date, priority, created, updated)
            limit: Maximum number of tickets to return (default: 50)
            scope: Scope filter (work, personal, or empty for both)

        Returns:
            Dictionary with 'tickets' list and 'count'
        """
        try:
            # Normalize type names
            normalized_type = None
            if ticket_type:
                normalized_type = ','.join(
                    normalize_type_name(t.strip()) for t in ticket_type.split(',')
                )

            filters = {
                'status': status if status else None,
                'type': normalized_type,
                'priority': priority if priority else None,
                'project': project if project else None,
                'assignee': assignee if assignee else None,
            }

            exclude_types = ['person']
            if not include_areas:
                exclude_types.append('area')
            if not include_resources:
                exclude_types.append('resource')

            # Create args for path resolution
            args = argparse.Namespace(
                work=scope == "work",
                personal=scope == "personal",
                areas=include_areas,
                resources=include_resources,
            )

            search_paths = get_search_paths(args, config)
            para_paths = get_para_search_paths(args, config)
            search_paths.extend(para_paths)

            tickets: list[Ticket] = []
            for search_path in search_paths:
                if search_path.exists():
                    path_tickets = fallback_list_tickets(search_path, filters, exclude_types)
                    tickets.extend(path_tickets)

            # Apply additional filters
            today = date.today()

            if overdue:
                tickets = [t for t in tickets if t.due_date and t.due_date < today]

            if due_soon > 0:
                cutoff = today + timedelta(days=due_soon)
                tickets = [t for t in tickets if t.due_date and t.due_date <= cutoff]

            if blocked:
                tickets = [t for t in tickets if t.status == 'blocked']

            if unassigned:
                tickets = [t for t in tickets if not t.assignee]

            if mine:
                user = os.environ.get('USER', '')
                tickets = [t for t in tickets if t.assignee and user.lower() in str(t.assignee).lower()]

            # Sort
            def sort_key(t: Ticket) -> Any:
                if sort == 'priority':
                    priority_order = {'critical': 0, 'high': 1, 'medium': 2, 'low': 3}
                    return priority_order.get(t.priority, 2)
                elif sort == 'created':
                    return t.created
                elif sort == 'updated':
                    return t.updated or t.created
                else:  # due_date
                    return t.due_date or date.max

            tickets.sort(key=sort_key)
            tickets = tickets[:limit]

            return {
                "tickets": [ticket_to_mcp_dict(t) for t in tickets],
                "count": len(tickets),
            }
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def show_ticket(
        ticket: str,
        include_links: bool = False,
        include_tree: bool = False,
        raw: bool = False,
        content_only: bool = False,
    ) -> dict:
        """
        Get details of a specific ticket.

        Args:
            ticket: Ticket ID or path
            include_links: Include linked ticket details
            include_tree: Show hierarchy tree
            raw: Return raw file content
            content_only: Return body without frontmatter

        Returns:
            Ticket details or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )
            ticket_path = find_ticket(ticket, config, args)

            if not ticket_path or not ticket_path.exists():
                return {"error": f"Ticket not found: {ticket}"}

            if raw:
                content = ticket_path.read_text()
                return {"content": content, "filepath": str(ticket_path)}

            if content_only:
                content = ticket_path.read_text()
                _, body = parse_frontmatter(content)
                return {"body": body, "filepath": str(ticket_path)}

            ticket_obj = Ticket.from_file(ticket_path)
            result = ticket_to_mcp_dict(ticket_obj)

            if include_links:
                links = {
                    'member_of': [],
                    'relates_to': [],
                    'blocked_by': [],
                    'blocks': [],
                }
                for link in ticket_obj.member_of:
                    link_path = find_ticket(link.path, config, args)
                    if link_path and link_path.exists():
                        try:
                            linked = Ticket.from_file(link_path)
                            links['member_of'].append(ticket_to_mcp_dict(linked))
                        except Exception:
                            pass
                result['linked_tickets'] = links

            return result
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def edit_ticket(
        ticket: str,
        assignee: str = "",
        status: str = "",
        priority: str = "",
        add_tag: str = "",
        remove_tag: str = "",
        progress: int = -1,
        due_date: str = "",
        clear_field: str = "",
    ) -> dict:
        """
        Edit a ticket's fields.

        Args:
            ticket: Ticket ID or path
            assignee: Set assignee (person reference)
            status: Set status
            priority: Set priority (critical, high, medium, low)
            add_tag: Add a tag
            remove_tag: Remove a tag
            progress: Set progress (0-100, -1 = no change)
            due_date: Set due date (YYYY-MM-DD or +7d)
            clear_field: Clear a field (e.g., 'assignee', 'due_date')

        Returns:
            Updated ticket details or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )
            ticket_path = find_ticket(ticket, config, args)

            if not ticket_path or not ticket_path.exists():
                return {"error": f"Ticket not found: {ticket}"}

            content = ticket_path.read_text()
            metadata, body = parse_frontmatter(content)
            changes_made = False

            if assignee:
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(assignee, pdir)
                        if person_link:
                            metadata['assignee'] = str(person_link)
                            changes_made = True
                            break

            if status:
                metadata['status'] = status
                changes_made = True

            if priority:
                metadata['priority'] = priority
                changes_made = True

            if add_tag:
                tags = metadata.get('tags', [])
                if add_tag not in tags:
                    tags.append(add_tag)
                    metadata['tags'] = tags
                    changes_made = True

            if remove_tag:
                tags = metadata.get('tags', [])
                if remove_tag in tags:
                    tags.remove(remove_tag)
                    metadata['tags'] = tags
                    changes_made = True

            if progress >= 0:
                metadata['progress'] = min(100, max(0, progress))
                changes_made = True

            if due_date:
                parsed_date = parse_date(due_date)
                if parsed_date:
                    metadata['due_date'] = str(parsed_date)
                    changes_made = True

            if clear_field:
                if clear_field in metadata:
                    del metadata[clear_field]
                    changes_made = True

            if changes_made:
                metadata['updated'] = datetime.now().isoformat()
                new_content = dump_frontmatter(metadata, body)
                ticket_path.write_text(new_content)

            ticket_obj = Ticket.from_file(ticket_path)
            return {"success": True, "ticket": ticket_to_mcp_dict(ticket_obj)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def move_ticket(
        ticket: str,
        status: str,
        comment: str = "",
        resolve: bool = False,
        reopen: bool = False,
        force: bool = False,
    ) -> dict:
        """
        Change a ticket's status.

        Args:
            ticket: Ticket ID or path
            status: New status (backlog, ready, in_progress, blocked,
                   review, delegated, done, cancelled)
            comment: Add a transition comment
            resolve: Set end_date when moving to done
            reopen: Clear end_date when reopening from done/cancelled
            force: Skip validation

        Returns:
            Updated ticket details or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )
            ticket_path = find_ticket(ticket, config, args)

            if not ticket_path or not ticket_path.exists():
                return {"error": f"Ticket not found: {ticket}"}

            content = ticket_path.read_text()
            metadata, body = parse_frontmatter(content)

            old_status = metadata.get('status', '')
            metadata['status'] = status
            metadata['updated'] = datetime.now().isoformat()

            if resolve and status == 'done':
                metadata['end_date'] = str(date.today())

            if reopen and old_status in ('done', 'cancelled'):
                if 'end_date' in metadata:
                    del metadata['end_date']

            if comment:
                comment_id = get_next_comment_id(content)
                user = os.environ.get('USER', 'system')
                timestamp = datetime.now().strftime('%Y-%m-%d %H:%M')
                transition_comment = f"**Status changed** from `{old_status}` to `{status}`\n\n{comment}"
                comment_block = f"\n\n---\n**#{comment_id}** | {user} | {timestamp}\n\n{transition_comment}\n"
                body = body.rstrip() + comment_block

            new_content = dump_frontmatter(metadata, body)
            ticket_path.write_text(new_content)

            ticket_obj = Ticket.from_file(ticket_path)
            return {"success": True, "ticket": ticket_to_mcp_dict(ticket_obj)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def link_tickets(
        ticket: str,
        relation: str,
        target: str,
        remove: bool = False,
        bidirectional: bool = False,
    ) -> dict:
        """
        Create or remove a relationship between tickets.

        Args:
            ticket: Source ticket ID or path
            relation: Relationship type (member_of, relates_to, blocked_by, blocks)
            target: Target ticket ID or path
            remove: Remove the link instead of adding
            bidirectional: Create reverse link on target ticket

        Returns:
            Success status or error
        """
        try:
            if relation not in ('member_of', 'relates_to', 'blocked_by', 'blocks'):
                return {"error": f"Invalid relation type: {relation}"}

            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            source_path = find_ticket(ticket, config, args)
            target_path = find_ticket(target, config, args)

            if not source_path or not source_path.exists():
                return {"error": f"Source ticket not found: {ticket}"}
            if not target_path or not target_path.exists():
                return {"error": f"Target ticket not found: {target}"}

            # Get target link path relative to notes dir
            target_rel = target_path.relative_to(config.directory)
            target_link = f"![[{target_rel}]]"

            content = source_path.read_text()
            metadata, body = parse_frontmatter(content)

            links = metadata.get(relation, [])
            if isinstance(links, str):
                links = [links] if links else []

            if remove:
                links = [l for l in links if target not in l and str(target_rel) not in l]
            else:
                if target_link not in links:
                    links.append(target_link)

            metadata[relation] = links
            metadata['updated'] = datetime.now().isoformat()

            new_content = dump_frontmatter(metadata, body)
            source_path.write_text(new_content)

            if bidirectional:
                reverse_relation = {
                    'blocked_by': 'blocks',
                    'blocks': 'blocked_by',
                    'relates_to': 'relates_to',
                    'member_of': 'member_of',
                }.get(relation, relation)

                source_rel = source_path.relative_to(config.directory)
                source_link = f"![[{source_rel}]]"

                target_content = target_path.read_text()
                target_metadata, target_body = parse_frontmatter(target_content)

                target_links = target_metadata.get(reverse_relation, [])
                if isinstance(target_links, str):
                    target_links = [target_links] if target_links else []

                if remove:
                    target_links = [l for l in target_links if ticket not in l and str(source_rel) not in l]
                else:
                    if source_link not in target_links:
                        target_links.append(source_link)

                target_metadata[reverse_relation] = target_links
                target_metadata['updated'] = datetime.now().isoformat()

                target_new = dump_frontmatter(target_metadata, target_body)
                target_path.write_text(target_new)

            return {"success": True, "message": f"Link {'removed' if remove else 'created'}"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def add_comment(
        target: str,
        text: str,
        reply_to: int = 0,
        author: str = "",
    ) -> dict:
        """
        Add a comment to a ticket or person.

        Args:
            target: Ticket ID or person reference
            text: Comment text (supports markdown)
            reply_to: Reply to comment #N (0 = top-level comment)
            author: Optional author attribution for the comment

        Returns:
            Comment ID or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )
            target_path = find_ticket(target, config, args)

            if not target_path:
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(target, pdir)
                        if person_link:
                            target_path = config.directory / person_link.path
                            break

            if not target_path or not target_path.exists():
                return {"error": f"Target not found: {target}"}

            reply_arg = reply_to if reply_to > 0 else None
            author_arg = author if author else None
            comment_id = insert_comment(target_path, text, reply_to=reply_arg, author=author_arg)

            return {"success": True, "comment_id": comment_id}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def get_comments(
        target: str,
        comment_range: str = "all",
        include_threads: bool = False,
        author: str = "",
    ) -> dict:
        """
        Get comments from a ticket or person.

        Args:
            target: Ticket ID or person reference
            comment_range: Range to fetch (e.g., 'all', '1,2-5,9')
            include_threads: Include reply threads
            author: Filter by author (partial match, case-insensitive)

        Returns:
            List of comments or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )
            target_path = find_ticket(target, config, args)

            if not target_path:
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(target, pdir)
                        if person_link:
                            target_path = config.directory / person_link.path
                            break

            if not target_path or not target_path.exists():
                return {"error": f"Target not found: {target}"}

            content = target_path.read_text()
            comments = parse_comments(content)

            if not comments:
                return {"comments": [], "count": 0}

            max_id = max(c.id for c in comments)
            ids = parse_comment_range(comment_range, max_id)

            result_comments = []
            author_filter = author.lower() if author else ""
            for c in comments:
                if c.id in ids:
                    # Filter by author if specified
                    if author_filter:
                        comment_author = (c.author or "").lower()
                        if author_filter not in comment_author:
                            continue

                    comment_dict = c.to_dict()
                    if not include_threads:
                        comment_dict.pop('replies', None)
                    elif author_filter:
                        # Filter replies by author too
                        comment_dict['replies'] = [
                            r for r in comment_dict.get('replies', [])
                            if author_filter in (r.get('author') or "").lower()
                        ]
                    result_comments.append(comment_dict)

            return {"comments": result_comments, "count": len(result_comments)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def get_dashboard(
        dashboard_type: str = "daily",
        project: str = "",
        person: str = "",
        sprint: str = "",
        section: str = "",
    ) -> dict:
        """
        Generate a dashboard view.

        Args:
            dashboard_type: Type of dashboard (daily, weekly, sprint, project, team, person)
            project: Filter by project
            person: Person reference for person dashboard
            sprint: Filter by sprint
            section: Return specific section only

        Returns:
            Dashboard content or error
        """
        try:
            args = argparse.Namespace(
                dashboard_type=dashboard_type,
                project=project if project else None,
                person=person if person else None,
                sprint=sprint if sprint else None,
                section=section if section else None,
                output=None,
                markers=False,
                format='md',
                work=False,
                personal=False,
                quiet=True,
            )

            # Capture stdout
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                result = cmd_dashboard(args, config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result == EXIT_SUCCESS:
                return {"dashboard": output, "type": dashboard_type}
            return {"error": "Failed to generate dashboard"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def get_kanban(
        project: str = "",
        assignee: str = "",
        mine: bool = False,
        statuses: str = "",
        hide_empty: bool = False,
        compact: bool = False,
    ) -> dict:
        """
        Get a kanban board view.

        Args:
            project: Filter by project
            assignee: Filter by assignee
            mine: Show only tickets assigned to $USER
            statuses: Comma-separated statuses to display
            hide_empty: Hide empty columns
            compact: Compact card display

        Returns:
            Kanban board data or error
        """
        try:
            args = argparse.Namespace(
                project=project if project else None,
                assignee=assignee if assignee else None,
                mine=mine,
                status=statuses if statuses else None,
                hide_empty=hide_empty,
                compact=compact,
                width=None,
                format='json',
                work=False,
                personal=False,
                quiet=True,
            )

            # Get tickets
            filters = {
                'project': project if project else None,
                'assignee': assignee if assignee else None,
            }

            search_paths = get_search_paths(args, config)
            tickets: list[Ticket] = []
            for search_path in search_paths:
                if search_path.exists():
                    path_tickets = fallback_list_tickets(search_path, filters, ['person', 'area', 'resource'])
                    tickets.extend(path_tickets)

            if mine:
                user = os.environ.get('USER', '')
                tickets = [t for t in tickets if t.assignee and user.lower() in str(t.assignee).lower()]

            # Group by status
            columns = {}
            status_order = ['backlog', 'ready', 'in_progress', 'blocked', 'review', 'delegated', 'done', 'cancelled']

            for status in status_order:
                status_tickets = [t for t in tickets if t.status == status]
                if not hide_empty or status_tickets:
                    columns[status] = [ticket_to_mcp_dict(t) for t in status_tickets]

            return {"columns": columns, "total": len(tickets)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def search_tickets(
        query: str,
        regex: bool = False,
        case_sensitive: bool = False,
        body_only: bool = False,
        frontmatter_only: bool = False,
        context_lines: int = 0,
        files_only: bool = False,
    ) -> dict:
        """
        Search through tickets.

        Args:
            query: Search query string
            regex: Use regex matching
            case_sensitive: Case sensitive search
            body_only: Search body only
            frontmatter_only: Search frontmatter only
            context_lines: Number of context lines to show
            files_only: Return file paths only

        Returns:
            Search results or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            search_paths = get_search_paths(args, config)
            results = []

            flags = 0 if case_sensitive else re.IGNORECASE
            pattern = re.compile(query, flags) if regex else None

            for search_path in search_paths:
                if not search_path.exists():
                    continue

                for md_file in search_path.rglob('*.md'):
                    try:
                        content = md_file.read_text()
                        metadata, body = parse_frontmatter(content)

                        if not metadata.get('id'):
                            continue

                        search_text = ""
                        if body_only:
                            search_text = body
                        elif frontmatter_only:
                            search_text = yaml.dump(metadata)
                        else:
                            search_text = content

                        matched = False
                        if pattern:
                            matched = bool(pattern.search(search_text))
                        else:
                            if case_sensitive:
                                matched = query in search_text
                            else:
                                matched = query.lower() in search_text.lower()

                        if matched:
                            if files_only:
                                results.append(str(md_file))
                            else:
                                ticket = Ticket.from_file(md_file)
                                result = ticket_to_mcp_dict(ticket)
                                if context_lines > 0:
                                    lines = search_text.split('\n')
                                    context = []
                                    for i, line in enumerate(lines):
                                        line_matches = False
                                        if pattern:
                                            line_matches = bool(pattern.search(line))
                                        else:
                                            if case_sensitive:
                                                line_matches = query in line
                                            else:
                                                line_matches = query.lower() in line.lower()
                                        if line_matches:
                                            start = max(0, i - context_lines)
                                            end = min(len(lines), i + context_lines + 1)
                                            context.extend(lines[start:end])
                                    result['context'] = '\n'.join(context)
                                results.append(result)
                    except Exception:
                        continue

            return {"results": results, "count": len(results)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def validate_tickets(
        files: str = "",
        strict: bool = False,
    ) -> dict:
        """
        Validate ticket structure and frontmatter.

        Args:
            files: Comma-separated file paths (empty = validate all)
            strict: Fail on warnings

        Returns:
            Validation results
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            search_paths = get_search_paths(args, config)
            errors = []
            warnings = []
            valid_count = 0

            if files:
                file_list = [Path(f.strip()) for f in files.split(',')]
            else:
                file_list = []
                for search_path in search_paths:
                    if search_path.exists():
                        file_list.extend(search_path.rglob('*.md'))

            for md_file in file_list:
                try:
                    content = md_file.read_text()
                    metadata, _ = parse_frontmatter(content)

                    if not metadata.get('id'):
                        continue

                    # Check required fields
                    required = ['id', 'title', 'type', 'created']
                    for field in required:
                        if not metadata.get(field):
                            errors.append(f"{md_file}: Missing required field '{field}'")

                    # Check status for types that need it
                    ticket_type = metadata.get('type', '')
                    if ticket_type not in ('resource', 'recipe') and not metadata.get('status'):
                        errors.append(f"{md_file}: Missing status field")

                    # Warnings
                    if not metadata.get('assignee'):
                        warnings.append(f"{md_file}: No assignee")

                    valid_count += 1
                except Exception as e:
                    errors.append(f"{md_file}: Parse error - {e}")

            return {
                "valid_count": valid_count,
                "errors": errors,
                "warnings": warnings,
                "passed": len(errors) == 0 and (not strict or len(warnings) == 0),
            }
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def generate_report(
        report_type: str,
        project: str = "",
        sprint: str = "",
        from_date: str = "",
        to_date: str = "",
    ) -> dict:
        """
        Generate an analytical report.

        Args:
            report_type: Report type (burndown, velocity, workload, aging, blockers)
            project: Filter by project
            sprint: Filter by sprint
            from_date: Start date (YYYY-MM-DD)
            to_date: End date (YYYY-MM-DD)

        Returns:
            Report data or error
        """
        try:
            args = argparse.Namespace(
                report_type=report_type,
                project=project if project else None,
                sprint=sprint if sprint else None,
                from_date=from_date if from_date else None,
                to_date=to_date if to_date else None,
                output=None,
                format='json',
                work=False,
                personal=False,
                quiet=True,
            )

            # Capture stdout
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                result = cmd_report(args, config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result == EXIT_SUCCESS:
                try:
                    return json.loads(output)
                except json.JSONDecodeError:
                    return {"report": output, "type": report_type}
            return {"error": "Failed to generate report"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def list_people(
        team: str = "",
        has_blocked: bool = False,
        has_overdue: bool = False,
    ) -> dict:
        """
        List people in the knowledge base.

        Args:
            team: Filter by team
            has_blocked: Only people with blocked tickets
            has_overdue: Only people with overdue tickets

        Returns:
            List of people or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            people_dirs = get_people_dirs(args, config)
            people = []

            for people_dir in people_dirs:
                if not people_dir.exists():
                    continue

                for md_file in people_dir.rglob('*.md'):
                    try:
                        person = Person.from_file(md_file)

                        if team and person.team != team:
                            continue

                        person_dict = person_to_mcp_dict(person)

                        if has_blocked or has_overdue:
                            # Check tickets for this person
                            person_ref = md_file.stem
                            filters = {'assignee': person_ref}
                            search_paths = get_search_paths(args, config)

                            person_tickets = []
                            for search_path in search_paths:
                                if search_path.exists():
                                    tickets = fallback_list_tickets(search_path, filters, ['person'])
                                    person_tickets.extend(tickets)

                            if has_blocked:
                                blocked = [t for t in person_tickets if t.status == 'blocked']
                                if not blocked:
                                    continue
                                person_dict['blocked_count'] = len(blocked)

                            if has_overdue:
                                today = date.today()
                                overdue = [t for t in person_tickets if t.due_date and t.due_date < today]
                                if not overdue:
                                    continue
                                person_dict['overdue_count'] = len(overdue)

                        people.append(person_dict)
                    except Exception:
                        continue

            return {"people": people, "count": len(people)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def show_person(
        person: str,
        include_tickets: bool = False,
        raw: bool = False,
    ) -> dict:
        """
        Get details of a specific person.

        Args:
            person: Person reference (name or filename)
            include_tickets: Include assigned ticket details
            raw: Return raw file content

        Returns:
            Person details or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            people_dirs = get_people_dirs(args, config)
            person_path = None

            for pdir in people_dirs:
                if pdir.exists():
                    person_link = resolve_person_reference(person, pdir)
                    if person_link:
                        person_path = config.directory / person_link.path
                        break

            if not person_path or not person_path.exists():
                return {"error": f"Person not found: {person}"}

            if raw:
                content = person_path.read_text()
                return {"content": content, "filepath": str(person_path)}

            person_obj = Person.from_file(person_path)
            result = person_to_mcp_dict(person_obj)

            if include_tickets:
                person_ref = person_path.stem
                filters = {'assignee': person_ref}
                search_paths = get_search_paths(args, config)

                person_tickets = []
                for search_path in search_paths:
                    if search_path.exists():
                        tickets = fallback_list_tickets(search_path, filters, ['person'])
                        person_tickets.extend(tickets)

                result['tickets'] = [ticket_to_mcp_dict(t) for t in person_tickets]

            return result
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def create_person(
        name: str,
        email: str = "",
        role: str = "",
        team: str = "",
        manager: str = "",
    ) -> dict:
        """
        Create a new person file.

        Args:
            name: Person's name
            email: Email address
            role: Role/title
            team: Team name
            manager: Manager reference

        Returns:
            Created person details or error
        """
        try:
            args = argparse.Namespace(
                name=name,
                email=email if email else None,
                role=role if role else None,
                team=team if team else None,
                manager=manager if manager else None,
                no_edit=True,
                format='json',
                work=False,
                personal=False,
                quiet=True,
            )

            result = cmd_people_create(args, config)

            if result == EXIT_SUCCESS:
                # Find the created person
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(name, pdir)
                        if person_link:
                            person_path = config.directory / person_link.path
                            if person_path.exists():
                                person_obj = Person.from_file(person_path)
                                return {"success": True, "person": person_to_mcp_dict(person_obj)}
                return {"success": True, "message": f"Created person: {name}"}
            return {"error": "Failed to create person"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def search_people(
        query: str,
    ) -> dict:
        """
        Search for people by name, email, role, or team.

        Args:
            query: Search query

        Returns:
            Matching people or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            people_dirs = get_people_dirs(args, config)
            results = []
            query_lower = query.lower()

            for people_dir in people_dirs:
                if not people_dir.exists():
                    continue

                for md_file in people_dir.rglob('*.md'):
                    try:
                        person = Person.from_file(md_file)

                        # Search in name, email, role, team
                        searchable = ' '.join(filter(None, [
                            person.name,
                            person.email,
                            person.role,
                            person.team,
                        ])).lower()

                        if query_lower in searchable:
                            results.append(person_to_mcp_dict(person))
                    except Exception:
                        continue

            return {"results": results, "count": len(results)}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def get_person_dashboard(
        person: str,
        section: str = "",
    ) -> dict:
        """
        Generate a dashboard for a specific person.

        Args:
            person: Person reference
            section: Return specific section only

        Returns:
            Person dashboard or error
        """
        try:
            args = argparse.Namespace(
                person=person,
                section=section if section else None,
                update=False,
                all=False,
                format='md',
                work=False,
                personal=False,
                quiet=True,
            )

            # Capture stdout
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                # Use the person dashboard command
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(person, pdir)
                        if person_link:
                            args.person = str(person_link.path)
                            break

                result = cmd_people(argparse.Namespace(
                    subcommand='dashboard',
                    **vars(args)
                ), config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result == EXIT_SUCCESS or output:
                return {"dashboard": output, "person": person}
            return {"error": "Failed to generate person dashboard"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def generate_link(
        target: str,
        absolute: bool = False,
        link_format: str = "markdown",
    ) -> dict:
        """
        Generate a transclusion or markdown link.

        Args:
            target: Ticket ID or person reference
            absolute: Use absolute path
            link_format: Link format (markdown or transclusion)

        Returns:
            Generated link or error
        """
        try:
            args = argparse.Namespace(
                work=False,
                personal=False,
            )

            target_path = find_ticket(target, config, args)

            if not target_path:
                people_dirs = get_people_dirs(args, config)
                for pdir in people_dirs:
                    if pdir.exists():
                        person_link = resolve_person_reference(target, pdir)
                        if person_link:
                            target_path = config.directory / person_link.path
                            break

            if not target_path or not target_path.exists():
                return {"error": f"Target not found: {target}"}

            if absolute:
                path_str = str(target_path)
            else:
                path_str = str(target_path.relative_to(config.directory))

            if link_format == "transclusion":
                link = f"![[{path_str}]]"
            else:
                # Get title for markdown link
                try:
                    content = target_path.read_text()
                    metadata, _ = parse_frontmatter(content)
                    title = metadata.get('title', '') or metadata.get('name', '') or target_path.stem
                except Exception:
                    title = target_path.stem
                link = f"[{title}]({path_str})"

            return {"link": link, "path": path_str, "format": link_format}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def convert_find_missing(
        areas: bool = False,
        resources: bool = False,
        meetings: bool = False,
        journals: bool = False,
        recipes: bool = False,
        dry_run: bool = True,
    ) -> dict:
        """
        Find and convert untracked markdown files to vimban tickets.

        Args:
            areas: Scan 02_areas/ directory
            resources: Scan 03_resources/ directory
            meetings: Scan meetings directory
            journals: Scan journals directory
            recipes: Scan recipes directory
            dry_run: Preview without making changes (default: true for safety)

        Returns:
            List of files found/converted or error
        """
        try:
            args = argparse.Namespace(
                areas=areas,
                resources=resources,
                meetings=meetings,
                journals=journals,
                recipes=recipes,
                dry_run=dry_run,
                no_confirm=True,
                verbose=False,
                format='json',
                work=False,
                personal=False,
                quiet=True,
            )

            # Capture stdout
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                result = cmd_convert_find_missing(args, config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result == EXIT_SUCCESS:
                try:
                    return json.loads(output)
                except json.JSONDecodeError:
                    lines = output.strip().split('\n')
                    return {"files": lines, "count": len(lines), "dry_run": dry_run}
            return {"files": [], "count": 0, "dry_run": dry_run}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def create_mentor_meeting(
        person: str,
        notes: str = "",
        mentored_by: bool = False,
        meeting_date: str = "",
    ) -> dict:
        """
        Create a mentor meeting record.

        Args:
            person: Person reference (name, filename, or transclusion link)
            notes: Optional initial notes for the meeting
            mentored_by: If True, you are the mentee (person is your mentor)
            meeting_date: Meeting date (YYYY-MM-DD, default: today)

        Returns:
            Created meeting details or error
        """
        try:
            args = argparse.Namespace(
                person=person,
                mentored_by=mentored_by,
                date=meeting_date if meeting_date else None,
                no_edit=True,
                format='json',
                work=False,
                personal=False,
            )

            # Capture stdout for the result
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                result_code = cmd_mentor_new(args, config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result_code == EXIT_SUCCESS:
                try:
                    result = json.loads(output)
                    # If notes provided, add them to the file
                    if notes and 'path' in result:
                        meeting_path = Path(result['path'])
                        if meeting_path.exists():
                            content = meeting_path.read_text()
                            # Add notes to the "Topics Discussed" section
                            content = content.replace(
                                "## Topics Discussed\n<!-- Key topics covered in this session -->",
                                f"## Topics Discussed\n<!-- Key topics covered in this session -->\n\n{notes}"
                            )
                            meeting_path.write_text(content)
                    return result
                except json.JSONDecodeError:
                    return {"error": "Failed to parse result", "raw": output}
            return {"error": "Failed to create mentor meeting"}
        except Exception as e:
            return {"error": str(e)}

    @mcp.tool()
    def list_mentor_meetings(
        person: str = "",
        mentored_by: bool = False,
        limit: int = 20,
    ) -> dict:
        """
        List mentor meetings, optionally filtered by person.

        Args:
            person: Filter by person (name or reference)
            mentored_by: If True, filter by mentor instead of mentee
            limit: Maximum number of meetings to return

        Returns:
            List of mentor meetings or error
        """
        try:
            args = argparse.Namespace(
                person=person if person else None,
                mentored_by=mentored_by,
                format='json',
                no_color=True,
                work=False,
                personal=False,
            )

            # Capture stdout for the result
            import io
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()

            try:
                result_code = cmd_mentor_list(args, config)
                output = sys.stdout.getvalue()
            finally:
                sys.stdout = old_stdout

            if result_code == EXIT_SUCCESS:
                try:
                    result = json.loads(output)
                    # Apply limit
                    if 'meetings' in result and len(result['meetings']) > limit:
                        result['meetings'] = result['meetings'][:limit]
                        result['count'] = len(result['meetings'])
                    return result
                except json.JSONDecodeError:
                    return {"error": "Failed to parse result", "raw": output}
            return {"meetings": [], "count": 0}
        except Exception as e:
            return {"error": str(e)}

    return mcp


def run_mcp_server(transport: str = "stdio", host: str = "127.0.0.1", port: int = 5004) -> int:
    """
    Run the MCP server with specified transport.

    Args:
        transport: Transport type ('stdio' or 'http')
        host: Host to bind for HTTP transport
        port: Port to bind for HTTP transport

    Returns:
        Exit code
    """
    if not MCP_AVAILABLE:
        print("Error: mcp package required for MCP server.", file=sys.stderr)
        print("Install with: pip install 'mcp[cli]'", file=sys.stderr)
        return EXIT_GENERAL_ERROR

    mcp = create_mcp_server(host=host, port=port)

    if transport == "stdio":
        mcp.run(transport="stdio")
    else:
        mcp.run(transport="streamable-http")

    return EXIT_SUCCESS


# ============================================================================
# CLI PARSER
# ============================================================================
def create_parser() -> argparse.ArgumentParser:
    """Create the argument parser with all subcommands."""

    epilog = """
Examples:
    vimban init
    vimban create task "Fix authentication bug" -a john -p high
    vimban list --status in_progress,review --mine
    vimban show PROJ-42 --links
    vimban move PROJ-42 done --resolve
    vimban edit PROJ-42 status=review -a jane
    vimban link PROJ-42 blocked_by PROJ-41
    vimban dashboard daily -f md
    vimban search "authentication"
    vimban people list --has-overdue
    vimban report workload

Environment Variables:
    VIMBAN_DIR             Default directory (default: ~/Documents/notes)
    VIMBAN_FORMAT          Default output format
    VIMBAN_ID_PREFIX       ID prefix (default: PROJ)
    VIMBAN_PEOPLE_DIR      People subdir (default: 02_areas/work/people)
"""

    parser = argparse.ArgumentParser(
        prog='vimban',
        description='Markdown-native ticket/kanban management system (:wqira)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog
    )

    # Global options
    parser.add_argument('-d', '--directory',
                        help=f'Working directory (default: {DEFAULT_DIR})')
    parser.add_argument('-f', '--format',
                        choices=['plain', 'md', 'yaml', 'json'],
                        default='plain',
                        help='Output format (default: plain)')
    parser.add_argument('-q', '--quiet', action='store_true',
                        help='Suppress non-essential output')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')
    parser.add_argument('--no-color', action='store_true',
                        help='Disable colors')
    parser.add_argument('--version', action='version',
                        version=f'%(prog)s {VERSION}')
    parser.add_argument('--license', action='store_true',
                        help='Show license (AGPLv3)')

    # MCP server modes (mutually exclusive)
    mcp_group = parser.add_mutually_exclusive_group()
    mcp_group.add_argument('--mcp', action='store_true',
                           help='Run as MCP server (stdio transport)')
    mcp_group.add_argument('--mcp-http', action='store_true',
                           help='Run as MCP server (HTTP transport, port 5004)')
    mcp_group.add_argument('--serve', action='store_true',
                           help='Start web UI server (default port 5005)')

    # Work/Personal scope (mutually exclusive)
    scope_group = parser.add_mutually_exclusive_group()
    scope_group.add_argument('--work', action='store_true',
                             help='Operate on work tickets (01_projects/work/)')
    scope_group.add_argument('--personal', action='store_true',
                             help='Operate on personal tickets (01_projects/personal/)')

    # Archive inclusion flag (global)
    parser.add_argument('--archived', action='store_true',
                        help='Include items from 04_archives/ (excluded by default)')

    # Remote API flags (global)
    parser.add_argument('--remote',
                        help='Connect to remote vimban_serve (URL or config:<name>)')
    parser.add_argument('--api-token',
                        help='API token for remote server authentication')
    parser.add_argument('--no-token', action='store_true',
                        help='Skip sending auth token to remote server')
    parser.add_argument('--watch', action='store_true',
                        help='Watch for live events from remote server (requires --remote)')

    # Subparsers
    subparsers = parser.add_subparsers(dest='command', help='Commands')

    # init
    init_parser = subparsers.add_parser('init', help='Initialize vimban')
    init_parser.add_argument('directory', nargs='?', help='Directory to initialize')
    init_parser.add_argument('-p', '--prefix', help=f'ID prefix (default: {DEFAULT_PREFIX})')
    init_parser.add_argument('--people-dir', help=f'People subdir (default: {DEFAULT_PEOPLE_DIR})')
    init_parser.add_argument('--no-git', action='store_true', help='Skip .gitignore')
    init_parser.add_argument('--force', action='store_true', help='Reinitialize if exists')

    # create
    create_parser = subparsers.add_parser('create', help='Create a ticket')
    create_parser.add_argument('type', choices=ALL_TYPES, help='Ticket type')
    create_parser.add_argument('title', nargs='?', help='Ticket title')
    create_parser.add_argument('--id', help='Custom full ID')
    create_parser.add_argument('--prefix', help='Custom prefix (auto sequence)')
    create_parser.add_argument('-a', '--assignee', help='Assignee reference')
    create_parser.add_argument('-r', '--reporter', help='Reporter reference')
    create_parser.add_argument('-w', '--watcher', action='append', help='Add watcher')
    create_parser.add_argument('-p', '--priority', choices=PRIORITIES, help='Priority')
    create_parser.add_argument('-t', '--tags', help='Comma-separated tags')
    create_parser.add_argument('-P', '--project', help='Project identifier')
    create_parser.add_argument('-m', '--member-of', action='append', help='Parent ticket')
    create_parser.add_argument('--due', help='Due date (YYYY-MM-DD or +7d)')
    create_parser.add_argument('-e', '--effort', type=int, help='Story points')
    create_parser.add_argument('-o', '--output', help='Output file path')
    create_parser.add_argument('--topic', help='Topic path for area/resource (e.g., technical/linux)')
    create_parser.add_argument('--date', help='Date for meeting (YYYY-MM-DD, default: today)')
    create_parser.add_argument('--no-edit', action='store_true', help="Don't open editor")
    create_parser.add_argument('--dry-run', action='store_true', help='Preview creation')

    # list
    list_parser = subparsers.add_parser('list', help='List tickets')
    list_parser.add_argument('-s', '--status', help='Filter status (comma-sep)')
    list_parser.add_argument('-t', '--type', help='Filter type (comma-sep)')
    list_parser.add_argument('-a', '--assignee', help='Filter assignee')
    list_parser.add_argument('-P', '--project', help='Filter project')
    list_parser.add_argument('--tag', action='append', help='Filter tag')
    list_parser.add_argument('--priority', help='Filter priority')
    list_parser.add_argument('--due-before', help='Due before date')
    list_parser.add_argument('--due-after', help='Due after date')
    list_parser.add_argument('--overdue', action='store_true', help='Only overdue')
    list_parser.add_argument('--due-soon', nargs='?', const='7', help='Due within N days')
    list_parser.add_argument('--stale', nargs='?', const='14', help='Not updated in N days')
    list_parser.add_argument('--blocked', action='store_true', help='Only blocked')
    list_parser.add_argument('--unassigned', action='store_true', help='Only unassigned')
    list_parser.add_argument('--mine', action='store_true', help='Assigned to $USER')
    list_parser.add_argument('--areas', action='store_true', help='Include areas')
    list_parser.add_argument('--resources', action='store_true', help='Include resources')
    list_parser.add_argument('--sort', help='Sort field (default: due_date)')
    list_parser.add_argument('--reverse', action='store_true', help='Reverse sort')
    list_parser.add_argument('--limit', type=int, help='Limit results')
    list_parser.add_argument('--columns', help='Columns (comma-sep)')
    list_parser.add_argument('--no-header', action='store_true', help='Omit header')
    list_parser.add_argument('--krafna', help='Raw Krafna query')
    list_parser.add_argument('filters', nargs='*', help='field=value filters')

    # show
    show_parser = subparsers.add_parser('show', help='Show ticket details')
    show_parser.add_argument('ticket', help='Ticket ID or path')
    show_parser.add_argument('--links', action='store_true', help='Show linked tickets')
    show_parser.add_argument('--tree', action='store_true', help='Show hierarchy')
    show_parser.add_argument('--history', action='store_true', help='Git history')
    show_parser.add_argument('--raw', action='store_true', help='Raw file content')
    show_parser.add_argument('--content', action='store_true', help='Show content only (no frontmatter)')

    # generate-link
    genlink_parser = subparsers.add_parser('generate-link', help='Generate link for ticket or person')
    genlink_parser.add_argument('ref', help='Ticket ID or person reference')
    genlink_parser.add_argument('--full', action='store_true', help='Output absolute path instead of relative')
    genlink_parser.add_argument('--markdown', action='store_true', help='Output markdown link [ID](./path)')
    genlink_parser.add_argument('--transclusion', action='store_true', help='Output transclusion link ![[path]]')

    # get-link (alias for generate-link)
    getlink_parser = subparsers.add_parser('get-link', help='Alias for generate-link')
    getlink_parser.add_argument('ref', help='Ticket ID or person reference')
    getlink_parser.add_argument('--full', action='store_true', help='Output absolute path instead of relative')
    getlink_parser.add_argument('--markdown', action='store_true', help='Output markdown link [ID](./path)')
    getlink_parser.add_argument('--transclusion', action='store_true', help='Output transclusion link ![[path]]')

    # get-id (reverse of generate-link)
    getid_parser = subparsers.add_parser('get-id', help='Get ID from a ticket/person reference')
    getid_parser.add_argument('ref', help='Path, ticket ID, or person name')

    # edit
    edit_parser = subparsers.add_parser('edit', help='Edit ticket')
    edit_parser.add_argument('ticket', nargs='?', help='Ticket ID or path (fzf picker if omitted)')
    edit_parser.add_argument('fields', nargs='*', help='field=value updates')
    edit_parser.add_argument('-i', '--interactive', action='store_true', help='Open in $EDITOR')
    edit_parser.add_argument('-a', '--assignee', help='Set assignee')
    edit_parser.add_argument('-s', '--status', help='Set status')
    edit_parser.add_argument('-p', '--priority', choices=PRIORITIES, help='Set priority')
    edit_parser.add_argument('--add-tag', help='Add tag')
    edit_parser.add_argument('--remove-tag', help='Remove tag')
    edit_parser.add_argument('--progress', type=int, help='Set progress (0-100)')
    edit_parser.add_argument('--due', help='Set due date')
    edit_parser.add_argument('--clear', help='Clear field')
    edit_parser.add_argument('--dry-run', action='store_true', help='Preview changes')

    # move
    move_parser = subparsers.add_parser('move', help='Move ticket status')
    move_parser.add_argument('ticket', help='Ticket ID or path')
    move_parser.add_argument('status', choices=STATUSES, help='New status')
    move_parser.add_argument('--force', action='store_true', help='Skip validation')
    move_parser.add_argument('--comment', help='Transition comment')
    move_parser.add_argument('--resolve', action='store_true', help='Set end_date (for done)')
    move_parser.add_argument('--reopen', action='store_true', help='Reopen from done/cancelled')

    # move-location
    moveloc_parser = subparsers.add_parser('move-location', help='Move ticket file to new directory')
    moveloc_parser.add_argument('ticket', nargs='?', help='Ticket ID or path (fzf picker if omitted)')
    moveloc_parser.add_argument('destination', nargs='?', help='Destination directory (fzf picker if omitted)')
    moveloc_parser.add_argument('--dry-run', action='store_true', help='Preview changes without executing')
    moveloc_parser.add_argument('--no-update-refs', action='store_true', help='Skip updating references in other files')

    # archive
    archive_parser = subparsers.add_parser('archive', help='Archive a completed/cancelled ticket')
    archive_parser.add_argument('ticket', nargs='?', help='Ticket ID or path (fzf picker if omitted)')
    archive_parser.add_argument('--dry-run', action='store_true', help='Preview changes without executing')

    # link
    link_parser = subparsers.add_parser('link', help='Link tickets')
    link_parser.add_argument('ticket', help='Ticket ID or path')
    link_parser.add_argument('relation', choices=['member_of', 'relates_to', 'blocked_by', 'blocks'],
                             help='Relation type')
    link_parser.add_argument('target', help='Target ticket')
    link_parser.add_argument('--remove', action='store_true', help='Remove instead of add')
    link_parser.add_argument('--bidirectional', action='store_true', help='Create reverse link')
    link_parser.add_argument('--dry-run', action='store_true', help='Preview changes')

    # comment
    comment_parser = subparsers.add_parser('comment', help='Add/view comments')
    comment_parser.add_argument('target', help='Ticket ID, path, or person')
    comment_parser.add_argument('text', nargs='?', help='Comment text (reads stdin if absent)')
    comment_parser.add_argument('--reply-to', type=int, metavar='N',
                                help='Reply to comment #N')
    comment_parser.add_argument('--print', nargs='?', const='all', metavar='RANGE',
                                help='Print parent comments (e.g., 1,2-5,9 or all)')
    comment_parser.add_argument('--print-full', nargs='?', const='all', metavar='RANGE',
                                help='Print comments with threads')
    comment_parser.add_argument('--new-id-output', action='store_true',
                                help='Output only new comment ID (for scripting)')
    comment_parser.add_argument('-e', '--edit', action='store_true',
                                help='Edit comment in $EDITOR before saving')
    comment_parser.add_argument('-u', '--user', metavar='NAME',
                                help='Author attribution for the comment')
    comment_parser.add_argument('--dry-run', action='store_true',
                                help='Preview without writing')

    # comments (list recent comments across tickets)
    comments_parser = subparsers.add_parser('comments', help='List recent comments across tickets')
    comments_parser.add_argument('-u', '--user', metavar='NAME',
                                 help='Filter by comment author')
    comments_parser.add_argument('--days', type=int, metavar='N',
                                 help='Show comments from the last N days')
    comments_parser.add_argument('--limit', type=int, metavar='N',
                                 help='Limit number of comments shown')
    comments_parser.add_argument('--reverse', action='store_true',
                                 help='Sort oldest first (default: newest first)')
    comments_parser.add_argument('-f', '--format', choices=['plain', 'md', 'yaml', 'json'],
                                 default='plain', help='Output format')

    # dashboard
    dashboard_parser = subparsers.add_parser('dashboard', help='Generate dashboard')
    dashboard_parser.add_argument('type', nargs='?', default='daily',
                                  choices=['daily', 'weekly', 'sprint', 'project', 'team', 'person'],
                                  help='Dashboard type')
    dashboard_parser.add_argument('-o', '--output', help='Output file')
    dashboard_parser.add_argument('-P', '--project', help='Filter project')
    dashboard_parser.add_argument('--person', help='Person reference')
    dashboard_parser.add_argument('--sprint', help='Filter sprint')
    dashboard_parser.add_argument('--section', help='Output section only (for vim !!)')
    dashboard_parser.add_argument('--markers', action='store_true', help='Include section markers')

    # kanban
    kanban_parser = subparsers.add_parser('kanban', help='Display kanban board view')
    kanban_parser.add_argument('-P', '--project', help='Filter by project')
    kanban_parser.add_argument('-a', '--assignee', help='Filter by assignee')
    kanban_parser.add_argument('--mine', action='store_true', help='Show only my tickets')
    kanban_parser.add_argument('-s', '--status', help='Comma-separated statuses to display')
    kanban_parser.add_argument('--hide-empty', action='store_true', help='Hide empty columns')
    kanban_parser.add_argument('--compact', action='store_true', help='Compact card display')
    kanban_parser.add_argument('-w', '--width', type=int, help='Column width (default: auto)')
    kanban_parser.add_argument('--done-last', type=int, default=None, metavar='DAYS',
                               help='Show done tasks from last DAYS (0=all done)')
    # Note: uses global -f/--format from parent parser

    # search
    search_parser = subparsers.add_parser('search', help='Search tickets')
    search_parser.add_argument('query', help='Search query')
    search_parser.add_argument('-E', '--regex', action='store_true', help='Regex mode')
    search_parser.add_argument('-i', '--ignore-case', action='store_true', default=True,
                               help='Case insensitive (default)')
    search_parser.add_argument('-I', '--case-sensitive', action='store_true', help='Case sensitive')
    search_parser.add_argument('--body-only', action='store_true', help='Body only')
    search_parser.add_argument('--frontmatter-only', action='store_true', help='Frontmatter only')
    search_parser.add_argument('--context', type=int, default=0, help='Context lines')
    search_parser.add_argument('-l', '--files-only', action='store_true', help='List files only')

    # validate
    validate_parser = subparsers.add_parser('validate', help='Validate tickets')
    validate_parser.add_argument('files', nargs='*', help='Files to validate')
    validate_parser.add_argument('--fix', action='store_true', help='Auto-fix issues')
    validate_parser.add_argument('--strict', action='store_true', help='Fail on warnings')
    validate_parser.add_argument('--schema', help='Custom schema file')

    # report
    report_parser = subparsers.add_parser('report', help='Generate reports')
    report_parser.add_argument('type', choices=['burndown', 'velocity', 'workload', 'aging', 'blockers'],
                               help='Report type')
    report_parser.add_argument('-P', '--project', help='Filter project')
    report_parser.add_argument('--sprint', help='Filter sprint')
    report_parser.add_argument('--from', dest='from_date', help='Start date')
    report_parser.add_argument('--to', dest='to_date', help='End date')
    report_parser.add_argument('-o', '--output', help='Output file')

    # sync
    sync_parser = subparsers.add_parser('sync', help='Sync with external systems')
    sync_parser.add_argument('--provider', choices=['jira', 'monday'], default='jira',
                             help='Sync provider')
    sync_parser.add_argument('--dry-run', action='store_true', help='Preview sync')
    sync_parser.add_argument('--push', action='store_true', help='Push local changes')
    sync_parser.add_argument('--pull', action='store_true', help='Pull external changes')

    # commit
    commit_parser = subparsers.add_parser('commit', help='Pull, commit, and push vimban changes')
    commit_parser.add_argument('-m', '--message', help='Commit message (default: auto-generated timestamp)')
    commit_parser.add_argument('--no-pull', action='store_true', help='Skip pulling from remote')
    commit_parser.add_argument('--no-push', action='store_true', help='Skip pushing to remote')
    commit_parser.add_argument('--dry-run', action='store_true', help='Preview without making changes')
    commit_parser.add_argument('-A', '--all', action='store_true', help='Stage all files (default: only vimban files)')
    commit_parser.add_argument('--pull', action='store_true', help='Only pull from remote (no commit/push)')

    # watch (requires --remote)
    watch_parser = subparsers.add_parser('watch', help='Watch real-time collab events (requires --remote)')
    watch_parser.add_argument('--format', choices=['plain', 'json'], default='plain', help='Output format')

    # people
    people_parser = subparsers.add_parser('people', help='People management')
    people_subparsers = people_parser.add_subparsers(dest='people_command')

    # people list
    people_list = people_subparsers.add_parser('list', help='List all people')
    people_list.add_argument('--team', help='Filter by team')
    people_list.add_argument('--has-blocked', action='store_true', help='Has blocked tickets')
    people_list.add_argument('--has-overdue', action='store_true', help='Has overdue tickets')

    # people show
    people_show = people_subparsers.add_parser('show', help='Show person details')
    people_show.add_argument('person', help='Person reference')
    people_show.add_argument('--tickets', action='store_true', help='Include ticket details')
    people_show.add_argument('--raw', action='store_true', help='Raw file content')

    # people edit
    people_edit = people_subparsers.add_parser('edit', help='Edit person file')
    people_edit.add_argument('person', help='Person reference')

    # people dashboard
    people_dash = people_subparsers.add_parser('dashboard', help='Generate person dashboard')
    people_dash.add_argument('person', help='Person reference')
    people_dash.add_argument('--section', help='Section only (for vim !!)')
    people_dash.add_argument('--update', action='store_true', help='Update file in-place')
    people_dash.add_argument('--all', action='store_true', help='All people')

    # people create
    people_create = people_subparsers.add_parser('create', help='Create person file')
    people_create.add_argument('name', help='Person name')
    people_create.add_argument('--email', help='Email address')
    people_create.add_argument('--role', help='Role/title')
    people_create.add_argument('--team', help='Team name')
    people_create.add_argument('--manager', help='Manager reference')
    people_create.add_argument('--no-edit', action='store_true', help="Don't open editor")

    # people search
    people_search = people_subparsers.add_parser('search', help='Search people')
    people_search.add_argument('query', help='Search query')

    # mentor
    mentor_parser = subparsers.add_parser('mentor', help='Mentor meeting management')
    mentor_subparsers = mentor_parser.add_subparsers(dest='mentor_command')

    # mentor new
    mentor_new = mentor_subparsers.add_parser('new', help='Create mentor meeting')
    mentor_new.add_argument('person', help='Person reference (mentee by default)')
    mentor_new.add_argument('--mentored-by', action='store_true',
                            help='You are the mentee (person is your mentor)')
    mentor_new.add_argument('--date', help='Meeting date (YYYY-MM-DD, default: today)')
    mentor_new.add_argument('--no-edit', action='store_true', help="Don't open editor")
    mentor_new.add_argument('--dry-run', action='store_true', help="Preview without creating")

    # mentor list
    mentor_list = mentor_subparsers.add_parser('list', help='List mentor meetings')
    mentor_list.add_argument('person', nargs='?', help='Filter by person')
    mentor_list.add_argument('--mentored-by', action='store_true',
                             help='Filter by mentor (instead of mentee)')

    # mentor show
    mentor_show = mentor_subparsers.add_parser('show', help='Show mentor meeting details')
    mentor_show.add_argument('meeting_id', help='Meeting ID (e.g., MNTR-1)')

    # convert
    convert_parser = subparsers.add_parser('convert', help='Convert/ingest files')
    convert_subparsers = convert_parser.add_subparsers(dest='convert_cmd')

    # convert find-missing
    find_missing = convert_subparsers.add_parser('find-missing',
        help='Find and convert untracked markdown files')
    find_missing.add_argument('--areas', action='store_true',
        help='Scan 02_areas/ directory')
    find_missing.add_argument('--resources', action='store_true',
        help='Scan 03_resources/ directory')
    find_missing.add_argument('--meetings', action='store_true',
        help='Scan 02_areas/work/meetings/')
    find_missing.add_argument('--journals', action='store_true',
        help='Scan 02_areas/personal/journal/')
    find_missing.add_argument('--recipes', action='store_true',
        help='Scan 03_resources/food_and_health/recipes/')
    find_missing.add_argument('--people', action='store_true',
        help='Scan people directories (02_areas/work/people/ and 02_areas/personal/people/)')
    find_missing.add_argument('--dry-run', action='store_true',
        help='Preview without making changes')
    find_missing.add_argument('--no-confirm', action='store_true',
        help='Process all files without prompts')
    find_missing.add_argument('-v', '--verbose', action='store_true',
        help='Show detailed output')

    # completion
    completion_parser = subparsers.add_parser('completion', help='Generate shell completion')
    completion_parser.add_argument('shell', nargs='?', default='bash', choices=['bash'],
                                   help='Shell type (default: bash)')

    # tui
    tui_parser = subparsers.add_parser('tui', help='Launch interactive TUI')
    tui_parser.add_argument('--layout', choices=['kanban', 'list', 'split'],
                            help='Initial layout (overrides config)')
    tui_parser.add_argument('--view',
                            choices=['tickets', 'people', 'kanban', 'dashboard', 'reports', 'mentorship'],
                            help='Initial view (overrides config)')

    return parser


# ============================================================================
# MAIN
# ============================================================================
def main() -> int:
    """Main entry point."""
    parser = create_parser()
    args = parser.parse_args()

    # Handle --license
    if getattr(args, 'license', False):
        print(LICENSE_TEXT)
        return EXIT_SUCCESS

    # Handle MCP server modes
    if getattr(args, 'mcp', False):
        return run_mcp_server(transport="stdio")
    elif getattr(args, 'mcp_http', False):
        return run_mcp_server(transport="http", host="127.0.0.1", port=5004)
    elif getattr(args, 'serve', False):
        # launch the web UI server (vimban_serve)
        script_dir: Path = Path(__file__).resolve().parent
        serve_script: Path = script_dir / "vimban_serve"
        if not serve_script.exists():
            error("vimban_serve not found. Ensure it is installed alongside vimban.")
        serve_cmd: list[str] = [str(serve_script)] + [
            a for a in argv[1:] if a != "--serve"
        ]
        env_copy: dict[str, str] = environ.copy()
        env_copy["NO_DBOX_CHECK"] = "1"
        result = subprocess.run(serve_cmd, env=env_copy)
        return result.returncode

    # Handle --watch: connect to SSE stream, bypass all subcommand dispatch
    if getattr(args, 'watch', False):
        if not getattr(args, 'remote', None):
            print('Error: --watch requires --remote', file=sys.stderr)
            sys.exit(1)
        remote_url, remote_token, _watch_keys = _resolve_remote(args.remote)
        if getattr(args, 'api_token', None):
            remote_token = args.api_token
        if getattr(args, 'no_token', False):
            remote_token = None
        _watch_events(remote_url, remote_token or '')
        sys.exit(0)

    # Get directory from args, env, or default
    directory = Path(
        getattr(args, 'directory', None) or
        environ.get('VIMBAN_DIR', '') or
        str(DEFAULT_DIR)
    ).expanduser()

    # Load config
    config = Config.load(directory)

    # Override format from env if not specified
    if not getattr(args, 'format', None):
        args.format = environ.get('VIMBAN_FORMAT', 'plain')

    # Dispatch command
    command = getattr(args, 'command', None)

    if not command:
        parser.print_help()
        return EXIT_SUCCESS

    # remote mode: route through vimban_serve API instead of local filesystem
    if getattr(args, 'remote', None):
        remote_url, remote_token, remote_project_keys = _resolve_remote(args.remote)
        # --api-token overrides token from remote.yaml
        if getattr(args, 'api_token', None):
            remote_token = args.api_token
        # --no-token means don't send any auth
        if getattr(args, 'no_token', False):
            remote_token = None
        return _remote_dispatch(args, remote_url, remote_token, remote_project_keys)

    commands = {
        'init': cmd_init,
        'create': cmd_create,
        'list': cmd_list,
        'show': cmd_show,
        'generate-link': cmd_generate_link,
        'get-link': cmd_generate_link,  # Alias
        'get-id': cmd_get_id,
        'edit': cmd_edit,
        'move': cmd_move,
        'move-location': cmd_move_location,
        'archive': cmd_archive,
        'link': cmd_link,
        'dashboard': cmd_dashboard,
        'kanban': cmd_kanban,
        'search': cmd_search,
        'validate': cmd_validate,
        'report': cmd_report,
        'sync': cmd_sync,
        'commit': cmd_commit,
        'people': cmd_people,
        'mentor': cmd_mentor,
        'convert': cmd_convert,
        'completion': cmd_completion,
        'comment': cmd_comment,
        'comments': cmd_comments,
        'tui': cmd_tui,
    }

    cmd_func = commands.get(command)
    if cmd_func:
        try:
            return cmd_func(args, config)
        except KeyboardInterrupt:
            print("\nInterrupted", file=sys.stderr)
            return EXIT_GENERAL_ERROR
        except Exception as e:
            if getattr(args, 'verbose', False):
                import traceback
                traceback.print_exc()
            error(str(e), EXIT_GENERAL_ERROR)
    else:
        error(f"Unknown command: {command}", EXIT_INVALID_ARGS)

    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
