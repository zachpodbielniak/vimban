# Configuration

Vimban uses a layered configuration system: environment variables take precedence, then config file, then built-in defaults.

## Config File

The config file lives at `~/.config/vimban/config.yaml`. An example is provided at `.config/vimban/config.yaml.example` in the repo.

```bash
# Create your config
mkdir -p ~/.config/vimban
cp .config/vimban/config.yaml.example ~/.config/vimban/config.yaml
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

To disable this behavior:

```bash
# Skip for a single command
NO_DBOX_CHECK=1 vimban list

# Disable permanently
export NO_DBOX_CHECK=1

# Or change the target container
export VIMBAN_DISTROBOX="my-container"
```

## Scope System

Vimban supports `work` and `personal` scopes. Scope is detected from file paths:

- Files containing `/work/` or starting with `01_projects/work` are work scope
- Files containing `/personal/` or starting with `01_projects/personal` are personal scope
- Default scope when undetected: `personal`

Override with `--work` or `--personal` flags on any command.
