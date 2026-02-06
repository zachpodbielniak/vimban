# Templates

Vimban uses markdown templates with placeholder variables to create new items. Templates live in `share/vimban/templates/` and are resolved via the template resolution chain (see [Configuration](configuration.md#template-resolution)).

## Template Variables

Templates use `{{VARIABLE}}` syntax for placeholders that are replaced at creation time.

### Common Variables

These appear across most templates:

| Variable | Replaced With | Source |
|----------|---------------|--------|
| `{{ID}}` | Generated ID (e.g., `PROJ-00042`) | Auto-generated from sequence file |
| `{{TITLE}}` | Item title | CLI argument or interactive prompt |
| `{{CREATED}}` | Creation date (`YYYY-MM-DD`) | Current date |
| `{{ASSIGNEE}}` | Assignee name | `-a` flag, `VIMBAN_ASSIGNEE`, or config |
| `{{REPORTER}}` | Reporter name | `-r` flag, `VIMBAN_REPORTER`, or config |
| `{{PRIORITY}}` | Priority level | `-p` flag or default |
| `{{TAGS}}` | Tag list | `-t` flag or empty `[]` |
| `{{PROJECT}}` | Project name | `-P` flag or empty |
| `{{DUE_DATE}}` | Due date | `--due` flag or empty |
| `{{MEMBER_OF}}` | Parent ticket ID | `-m` flag or empty |
| `{{STATUS}}` | Initial status | Varies by type |

### Specialized Variables

| Variable | Used In | Replaced With |
|----------|---------|---------------|
| `{{DATE}}` | meeting, journal, mentorship | Date from `--date` or today |
| `{{NAME}}` | person | Person's name |
| `{{EMAIL}}` | person | Person's email |
| `{{ROLE}}` | person | Person's role/title |
| `{{TEAM}}` | person | Person's team |
| `{{MANAGER}}` | person | Person's manager |

## Available Templates

### Ticket Templates

#### `task.md`

Standard work item with acceptance criteria.

```yaml
type: task
status: backlog
```

Sections: Description, Acceptance Criteria, Notes, Comments

#### `bug.md`

Bug report with reproduction steps and environment info.

```yaml
type: bug
status: backlog
```

Sections: Description, Steps to Reproduce, Expected Behavior, Actual Behavior, Environment, Notes, Comments

#### `epic.md`

Large initiative with goals and child stories.

```yaml
type: epic
status: backlog
```

Sections: Overview, Goals, Stories (auto-managed via `VIMBAN:STORIES` markers), Notes, Comments

#### `story.md`

User story with acceptance criteria and child tasks.

```yaml
type: story
status: backlog
```

Sections: User Story, Acceptance Criteria, Tasks (auto-managed via `VIMBAN:TASKS` markers), Notes, Comments

#### `sub-task.md`

Child task linked to a parent via `member_of`.

```yaml
type: sub-task
status: backlog
```

Sections: Description, Notes, Comments

#### `research.md`

Investigation with structured findings.

```yaml
type: research
status: backlog
```

Sections: Research Question, Findings, Recommendations, Resources, Notes, Comments

### PARA Templates

#### `area.md`

Ongoing responsibility or standard to maintain.

```yaml
type: area
status: active  # (set via {{STATUS}})
```

Sections: Purpose, Responsibilities, Current Focus, Related Projects, Notes, Comments

#### `resource.md`

Reference material and collected knowledge.

```yaml
type: resource
# No status field
```

Sections: Summary, Content, Sources, Related, Comments

### Specialized Templates

#### `meeting.md`

Meeting notes with agenda and action items.

```yaml
type: meeting
status: active  # (set via {{STATUS}})
date: 2026-02-06
```

Sections: Agenda, Discussion, Action Items, Decisions, Follow-up, Comments

#### `journal.md`

Daily journal with mood and energy tracking.

```yaml
type: journal
status: active
date: 2026-02-06
mood:
energy:
```

Sections: Morning, Main Entry, Evening, Comments

#### `recipe.md`

Recipe with cooking metadata.

```yaml
type: recipe
prep_time:
cook_time:
total_time:
servings:
difficulty:
cuisine:
diet: [carnivore]
```

Sections: Summary, Ingredients, Equipment, Instructions, Notes, Sources, Comments

#### `mentorship.md`

1:1 mentorship session tracking.

```yaml
type: mentorship
status: active  # (set via {{STATUS}})
date: 2026-02-06
```

Sections: Topics Discussed, Progress & Wins, Challenges, Action Items, Notes, Comments

#### `person.md`

People profile with auto-managed dashboard sections.

```yaml
type: person
name: "John Doe"
email: "john@example.com"
role: "Senior Engineer"
team: "Platform"
manager: jane_doe
direct_reports: []
```

Sections: About, 1:1 Notes (auto-managed), Current Focus (auto-managed), Notes, Comments

## Customizing Templates

### Override Templates

To customize a template:

1. Copy the original from `share/vimban/templates/`
2. Place in your template directory (set via `VIMBAN_TEMPLATE_DIR`)
3. Edit the copy

```bash
# Example: customize the task template
mkdir -p ~/.local/share/vimban/templates
cp share/vimban/templates/task.md ~/.local/share/vimban/templates/task.md
# Edit ~/.local/share/vimban/templates/task.md
```

### Creating New Templates

New template files can be added to the templates directory. The filename (without `.md`) must match the type name passed to `vimban create`.

Requirements for custom templates:

- Must include YAML frontmatter delimited by `---`
- Must include `id`, `title`, `type`, `created`, `updated`, `version` fields minimum
- Should include `<!-- VIMBAN:COMMENTS:START -->` / `<!-- VIMBAN:COMMENTS:END -->` markers for comment support
- Use `{{VARIABLE}}` syntax for any fields you want populated at creation time

### Section Markers

If your template needs auto-managed sections (like stories in an epic or tasks in a story), add the appropriate markers:

```markdown
## My Section

<!-- VIMBAN:MYSECTION:START -->

<!-- VIMBAN:MYSECTION:END -->
```

The Neovim plugin's `regenerate_section()` function (`<leader>vR`) can refresh content between these markers.
