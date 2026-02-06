# Configuration

Vimban uses a layered configuration system: environment variables take precedence, then config file, then built-in defaults.

## Config File

The config file lives at `~/.config/vimban/config.yaml`. An example is provided at `examples/config.yaml.example` in the repo.

```bash
# Create your config
mkdir -p ~/.config/vimban
cp examples/config.yaml.example ~/.config/vimban/config.yaml
```

## Environment Variables

All settings can be overridden via environment variables. Add these to your `.bashrc` or shell profile:

| Variable | Default | Description |
|----------|---------|-------------|
| `VIMBAN_DIR` | `~/Documents/notes` | Root directory for tickets |
| `VIMBAN_PEOPLE_DIR` | `02_areas/work/people` | People directory (relative to root) |
| `VIMBAN_TEMPLATE_DIR` | auto-detected | Template directory path |
| `VIMBAN_REPORTER` | *(empty)* | Default reporter for new tickets |
| `VIMBAN_ASSIGNEE` | *(empty)* | Default assignee for new tickets |
| `VIMBAN_DISTROBOX` | `dev` | Distrobox container for auto re-exec |
| `NO_DBOX_CHECK` | `false` | Set to `1` or `true` to skip distrobox re-exec |

### Example .bashrc

```bash
export VIMBAN_DIR="${HOME}/Documents/notes"
export VIMBAN_REPORTER="your_name"
export VIMBAN_ASSIGNEE="your_name"
export VIMBAN_DISTROBOX="dev"
```

## Per-Project Configuration

Each vimban-managed directory has a `.vimban/` folder containing:

```
.vimban/
├── config.yaml       # Per-project overrides (prefix, people_dir)
├── .sequence          # PROJ-nnn sequence counter
├── .sequence_bug      # BUG-nnn sequence counter
├── .sequence_research # RESEARCH-nnn sequence counter
└── ...                # Additional sequence files per type
```

Initialize a project:

```bash
cd ~/Documents/notes
vimban init
```

The `.vimban/config.yaml` in the project directory supports:

```yaml
prefix: PROJ
people_dir: 02_areas/work/people
```

## Template Resolution

Templates are searched in this order:

1. `VIMBAN_TEMPLATE_DIR` environment variable
2. `~/.dotfiles/vimban/share/vimban/templates/` (submodule install)
3. `~/.dotfiles/share/vimban/templates/` (legacy dotfiles install)
4. Relative to the vimban script (`../../share/vimban/templates/`)
5. `~/.local/share/vimban/templates/` (XDG data directory)

## Distrobox Integration

Vimban and the TUI auto-detect if they're running outside the target distrobox container and re-exec inside it. This ensures Python dependencies are available.

The distrobox target is resolved in this order:
1. `VIMBAN_DISTROBOX` environment variable
2. `distrobox` key in `~/.config/vimban/config.yaml`
3. Default: `dev`

### Change the target container

```yaml
# In ~/.config/vimban/config.yaml
distrobox: "my-container"
```

Or via environment variable:

```bash
export VIMBAN_DISTROBOX="my-container"
```

### Disable distrobox re-exec entirely

Set `distrobox` to `false`, `none`, `disabled`, or `off` in config:

```yaml
# In ~/.config/vimban/config.yaml
distrobox: "false"
```

Or via environment variable:

```bash
# Skip for a single command
NO_DBOX_CHECK=1 vimban list

# Disable permanently
export NO_DBOX_CHECK=1
```

## Scope System

Vimban supports `work` and `personal` scopes. Scope is detected from file paths:

- Files containing `/work/` or starting with `01_projects/work` are work scope
- Files containing `/personal/` or starting with `01_projects/personal` are personal scope
- Default scope when undetected: `personal`

Override with `--work` or `--personal` flags on any command.

## Remote API

Vimban supports connecting to a remote `vimban_serve` instance instead of the local filesystem. This enables multi-machine workflows where one machine hosts the data and others interact with it over HTTP.

### Token File

The server reads tokens from `~/.config/vimban/token`:

```
# API tokens for vimban_serve authentication
# One token per line. Lines starting with # are comments.
# Blank lines are ignored.

abc123-my-first-token
def456-my-second-token
```

Start the server with token auth:

```bash
vimban_serve                   # auth enabled if token file exists
vimban_serve --no-token        # auth disabled (for testing)
vimban_serve --token-file /path/to/tokens
```

### Remote Configuration

Named remotes are stored in `~/.config/vimban/remote.yaml`:

```yaml
work:
  url: "http://192.168.1.10:5005"
  api_token: "abc123-my-first-token"

home:
  url: "https://vimban.example.com:5005"
  api_token: "def456-my-second-token"

local:
  url: "http://127.0.0.1:5005"
  # no api_token — use with --no-token
```

### Remote Flags

These flags work with `vimban`, `vimban_tui`, and `vimban_serve`:

| Flag | Description |
|------|-------------|
| `--remote URL` | Connect to remote server (URL or `config:<name>`) |
| `--api-token TOKEN` | API token (overrides remote.yaml token) |
| `--no-token` | Skip sending auth header |

### Examples

```bash
# Direct URL
vimban --remote http://192.168.1.10:5005 --api-token abc123 list

# Named remote from remote.yaml
vimban --remote config:work list

# No auth (local testing)
vimban --remote http://localhost:5005 --no-token list

# TUI connected to remote
vimban_tui --remote config:work

# Proxy one vimban_serve through another
vimban_serve --remote config:work --port 8080
```
