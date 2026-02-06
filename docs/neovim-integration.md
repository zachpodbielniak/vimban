# Neovim Integration

Vimban includes a comprehensive Neovim plugin (`vimban.lua`) providing floating windows, Telescope pickers, and full ticket CRUD operations.

## Setup

The plugin lives at `lua/custom/vimban.lua` in your Neovim config. It must be loaded from your keybindings or init file:

```lua
-- In your mappings or init.lua
local vimban = require('custom.vimban')
```

### Configuration

The plugin has a config table you can customize:

```lua
local vimban = require('custom.vimban')
vimban.config = {
    notes_dir = vim.fn.expand('~/Documents/notes/'),
    statuses = { 'backlog', 'ready', 'in_progress', 'blocked', 'review', 'delegated', 'done', 'cancelled' },
    ticket_types = { 'task', 'bug', 'story', 'research', 'epic', 'sub-task' },
    priorities = { 'critical', 'high', 'medium', 'low' },
    dashboard_types = { 'daily', 'weekly', 'sprint', 'project', 'person' },
}
```

### Dependencies

- **Telescope.nvim** - Required for fuzzy pickers (`<leader>vl`, `<leader>vi`, etc.)
- **vimban CLI** - Must be in `$PATH`

## Keybindings Reference

All vimban keybindings live under the `<leader>v` namespace in normal mode.

### Ticket Operations

| Key | Function | Description |
|-----|----------|-------------|
| `<leader>vl` | `fzf_tickets('--mine')` | List my tickets (Telescope picker) |
| `<leader>vL` | `list_filtered()` | List with filter menu (status, priority, etc.) |
| `<leader>vc` | `create_ticket()` | Create new ticket wizard |
| `<leader>vm` | `move_status()` | Change ticket status |
| `<leader>ve` | `edit_ticket()` | Edit ticket fields (priority, assignee, etc.) |
| `<leader>vs` | `search_tickets()` | Search tickets by keyword |

### Viewing

| Key | Function | Description |
|-----|----------|-------------|
| `<leader>vd` | `dashboard('daily')` | Show daily dashboard in float |
| `<leader>vD` | `dashboard_picker()` | Pick dashboard type (daily, weekly, sprint, project, person) |
| `<leader>vk` | `kanban_board()` | Interactive kanban board in floating window |
| `<leader>vf` | `show_ticket_float()` | Preview ticket under cursor in float |
| `<leader>vg` | `goto_ticket()` | Navigate to ticket file from transclusion |
| `<leader>vp` | `people_dashboard()` | People dashboard |
| `<leader>v?` | `show_help()` | Show keybinding help |

### Comments

| Key | Function | Description |
|-----|----------|-------------|
| `<leader>vC` | `add_comment()` | Add comment to current ticket |
| `<leader>vr` | `reply_comment()` | Reply to a comment |

### Links & Transclusions

| Key | Function | Description |
|-----|----------|-------------|
| `<leader>vi` | `insert_link()` | Insert transclusion link (Telescope picker for tickets + PARA notes) |
| `<leader>vR` | `regenerate_section()` | Regenerate VIMBAN section under cursor |

### Visual Mode

| Key | Function | Description |
|-----|----------|-------------|
| `<leader>vc` | `create_from_selection()` | Create ticket from selected text |

## Transclusion Keybindings

These are related keybindings under `<leader>t` for working with transclusion links (`![[path]]` format):

| Key | Description |
|-----|-------------|
| `<leader>ti` | Insert PARA transclusion link |
| `<leader>te` | Expand transclusion inline (insert file/command output below cursor) |
| `<leader>tf` | Open transclusion in floating window (preview without inserting) |
| `<leader>tg` | Go to file under cursor (navigate to linked note/ticket) |
| `<leader>tr` | Run transclusion command silently (for `![[!command]]` links) |

### Transclusion Formats Supported

- `![[path/to/note.md]]` - File transclusion (relative to notes dir)
- `![[!command args]]` - Command transclusion (runs shell command)
- `[Title](path/to/note.md)` - Markdown link to note
- `[Title](!command args)` - Markdown link to command output

## Floating Window Navigation

All floating windows support vim-style navigation:

| Key | Action |
|-----|--------|
| `q` / `<Esc>` | Close window |
| `j` / `k` | Move up/down |
| `<CR>` | Select/open item |
| `/` | Search (in kanban view) |

### Kanban Board Actions

The interactive kanban board (`<leader>vk`) supports additional keys:

| Key | Action |
|-----|--------|
| `h` / `l` | Move between columns |
| `m` | Move ticket to different status |
| `o` | Open ticket file |
| `<CR>` | Show ticket detail |

## Buffer-Local Mappings

When editing a ticket file (detected by YAML frontmatter with vimban fields), these buffer-local mappings are available:

| Key | Description |
|-----|-------------|
| `<localleader>m` | Move this ticket (change status) |
| `<localleader>c` | Add comment to this ticket |
| `<localleader>e` | Edit this ticket's fields |

## Example Neovim Configuration

Here is a minimal setup using lazy.nvim style mappings:

```lua
-- In your mappings file
local M = {}

M.vimban = {
    n = {
        -- Core operations
        ["<leader>vl"] = {
            function() require('custom.vimban').fzf_tickets('--mine') end,
            "vimban: list my tickets",
        },
        ["<leader>vc"] = {
            function() require('custom.vimban').create_ticket() end,
            "vimban: create ticket",
        },
        ["<leader>vk"] = {
            function() require('custom.vimban').kanban_board() end,
            "vimban: kanban board",
        },
        ["<leader>vd"] = {
            function() require('custom.vimban').dashboard('daily') end,
            "vimban: daily dashboard",
        },
        ["<leader>v?"] = {
            function() require('custom.vimban').show_help() end,
            "vimban: help",
        },

        -- Transclusion
        ["<leader>ti"] = {
            function() require('custom.vimban').insert_para_link() end,
            "Insert transclusion link",
        },
    },
    v = {
        ["<leader>vc"] = {
            function() require('custom.vimban').create_from_selection() end,
            "vimban: create from selection",
        },
    },
}

return M
```
