# vimban

Markdown-native ticket and knowledge management system. All data lives as plain `.md` files with YAML frontmatter in a git repository. No database, no SaaS -- just your editor and your filesystem.

## Features

- **CLI-first** -- Full CRUD operations, search, dashboards, and kanban boards from the terminal
- **Neovim integration** -- Floating windows, Telescope pickers, buffer-local mappings, transclusion support
- **TUI** -- Terminal UI with kanban, list, and split layouts
- **PARA method** -- Built-in support for Projects, Areas, Resources, and Archives
- **Git sync** -- Commit, push, and pull built into the workflow
- **MCP server** -- Expose vimban as an AI assistant tool via Model Context Protocol
- **13 item types** -- Tickets (epic, story, task, sub-task, bug, research), PARA (area, resource), and specialized (meeting, journal, recipe, mentorship, person)
- **Configurable** -- Environment variables, config file, or per-project settings

## Quick Start

```bash
# Clone (or add as submodule to your dotfiles)
git clone https://gitlab.com/zachpodbielniak/vimban.git

# Install
make install

# Initialize a notes directory
cd ~/Documents/notes
vimban init

# Create a ticket
vimban create task "Set up development environment" -p medium --due +7d

# List tickets
vimban list -s in_progress --mine

# Kanban board
vimban kanban

# TUI
vimban tui
```

## Requirements

- Python 3.12+
- PyYAML
- python-frontmatter
- git
- Optional: Neovim 0.9+ with Telescope for editor integration
- Optional: distrobox (auto re-exec into dev container)

## Installation

### GNU Stow (dotfiles submodule)

```bash
# Add as submodule
cd ~/.dotfiles
git submodule add https://gitlab.com/zachpodbielniak/vimban.git vimban

# Stow it
stow vimban
```

This symlinks `bin/scripts/*` and `share/vimban/*` into your home directory via stow's standard layout.

### Manual Install

```bash
make install
```

Installs scripts to `~/bin/scripts/`, templates to `~/share/vimban/`, and example config to `~/.config/vimban/`.

### Python Dependencies

```bash
make venv
source venv/bin/activate
```

Or install globally:

```bash
pip install -r requirements.txt
```

## Configuration

Copy the example config and customize:

```bash
mkdir -p ~/.config/vimban
cp examples/config.yaml.example ~/.config/vimban/config.yaml
```

Key settings:

```yaml
reporter: "your_name"
assignee: "your_name"
directory: "~/Documents/notes"
default_prefix: "PROJ"
default_scope: "personal"
distrobox: "dev"
```

Environment variables take precedence over the config file. See [docs/configuration.md](docs/configuration.md) for the full reference.

## Item Types

| Type | Prefix | Description |
|------|--------|-------------|
| epic | PROJ | Large initiative containing stories |
| story | PROJ | User-facing feature or deliverable |
| task | PROJ | Concrete unit of work |
| sub-task | PROJ | Child of a task or story |
| bug | BUG | Defect report |
| research | RESEARCH | Investigation or analysis |
| area | AREA | Ongoing responsibility (PARA) |
| resource | RESOURCE | Reference material (PARA) |
| meeting | MTG | Meeting notes with action items |
| journal | JNL | Personal journal entry |
| recipe | RCP | Recipe with ingredients and steps |
| mentorship | MNTR | 1:1 mentorship session |
| person | PERSON | People profile with dashboard |

## Status Workflow

```
backlog -> ready -> in_progress -> review -> done
                        |                     |
                        v                     v
                     blocked              cancelled
                        |
                        v
                    delegated
```

## Bash Wrappers

Quick-create wrappers with sensible defaults:

| Command | Creates | Default Priority |
|---------|---------|-----------------|
| `vimban_task` | task | medium |
| `vimban_bug` | bug | high |
| `vimban_epic` | epic | medium |
| `vimban_story` | story | medium |
| `vimban_subtask` | sub-task | medium |
| `vimban_research` | research | medium |
| `vimban_area` | area | -- |
| `vimban_resource` | resource | -- |
| `vimban_meeting` | meeting | medium |
| `vimban_journal` | journal | -- |
| `vimban_recipe` | recipe | -- |
| `vimban_mentor` | mentorship | medium |

All wrappers support `--work`/`--personal` scope and `--no-edit` for headless use.

## Neovim Keybindings

All keybindings live under `<leader>v`:

| Key | Action |
|-----|--------|
| `<leader>vl` | List my tickets (Telescope) |
| `<leader>vL` | Filtered list |
| `<leader>vc` | Create ticket |
| `<leader>vm` | Move status |
| `<leader>ve` | Edit ticket fields |
| `<leader>vs` | Search tickets |
| `<leader>vd` | Daily dashboard |
| `<leader>vk` | Kanban board |
| `<leader>vi` | Insert transclusion link |
| `<leader>vC` | Add comment |
| `<leader>v?` | Help |

Transclusion keys under `<leader>t`:

| Key | Action |
|-----|--------|
| `<leader>ti` | Insert PARA link |
| `<leader>te` | Expand transclusion inline |
| `<leader>tf` | Preview in float |
| `<leader>tg` | Go to file |
| `<leader>tr` | Run command transclusion |

See [docs/neovim-integration.md](docs/neovim-integration.md) for the full reference including setup, floating window navigation, and buffer-local mappings.

## Documentation

- [Usage](docs/usage.md) -- CLI commands, TUI navigation, PARA types
- [Configuration](docs/configuration.md) -- Config file, environment variables, scopes
- [Architecture](docs/architecture.md) -- File format, ID system, type system, directory structure
- [Templates](docs/templates.md) -- Template variables, customization, creating new types
- [Neovim Integration](docs/neovim-integration.md) -- Keybindings, Telescope pickers, transclusions

## License

AGPLv3 -- See [LICENSE](LICENSE) for details.
