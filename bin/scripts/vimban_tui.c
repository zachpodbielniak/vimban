#!/usr/bin/env crispy
#define CRISPY_PARAMS "$(pkg-config --cflags --libs glib-2.0 json-glib-1.0 ncursesw) -Wno-unused-function"

/*
 * vimban_tui.c - ncurses TUI for vimban ticket management
 * Copyright (C) 2025  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Architecture:
 *   - ncurses for rendering, nodelay mode
 *   - GMainLoop drives the event loop
 *   - GIOChannel on STDIN fires key callbacks
 *   - g_timeout_add(100) for periodic redraw tick
 *   - g_timeout_add_seconds(30) for auto data refresh
 *   - vimban CLI invoked via g_spawn_sync with --format json
 *   - json-glib parses ticket/people data
 *   - YAML config loaded line-by-line (stdlib, no yaml-glib dep needed)
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <ncurses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */
#define VERSION_STR     "0.3.0"
#define MIN_WIDTH       (80)
#define MIN_HEIGHT      (24)
#define AUTO_REFRESH_S  (30)
#define TICK_MS         (100)

#define LICENSE_TEXT \
    "vimban_tui - ncurses TUI for vimban ticket management\n" \
    "Copyright (C) 2025  Zach Podbielniak\n\n" \
    "This program is free software: you can redistribute it and/or modify\n" \
    "it under the terms of the GNU Affero General Public License as published\n" \
    "by the Free Software Foundation, either version 3 of the License, or\n" \
    "(at your option) any later version.\n\n" \
    "This program is distributed in the hope that it will be useful,\n" \
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n" \
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n" \
    "GNU Affero General Public License for more details.\n\n" \
    "You should have received a copy of the GNU Affero General Public License\n" \
    "along with this program.  If not, see <https://www.gnu.org/licenses/>."

/* ============================================================
 * CATPPUCCIN MOCHA — 256-colour approximations
 * ============================================================ */
#define C_ROSEWATER  (217)
#define C_FLAMINGO   (216)
#define C_PINK       (211)
#define C_MAUVE      (183)
#define C_RED        (203)
#define C_MAROON     (210)
#define C_PEACH      (215)
#define C_YELLOW     (222)
#define C_GREEN      (149)
#define C_TEAL       (116)
#define C_SKY        (117)
#define C_SAPPHIRE   (110)
#define C_BLUE       (111)
#define C_LAVENDER   (147)
#define C_TEXT       (253)
#define C_SUBTEXT1   (246)
#define C_SUBTEXT0   (245)
#define C_OVERLAY2   (244)
#define C_OVERLAY1   (240)
#define C_OVERLAY0   (239)
#define C_SURFACE2   (238)
#define C_SURFACE1   (237)
#define C_SURFACE0   (236)
#define C_BASE       (235)
#define C_MANTLE     (234)
#define C_CRUST      (233)

/* colour pair indices */
#define PAIR_HEADER           (1)
#define PAIR_SELECTED         (2)
#define PAIR_BORDER           (3)
#define PAIR_STATUS_BAR       (4)
#define PAIR_ERROR            (5)
#define PAIR_SUCCESS          (6)
#define PAIR_HELP             (7)
/* status pairs */
#define PAIR_ST_BACKLOG       (10)
#define PAIR_ST_READY         (11)
#define PAIR_ST_IN_PROGRESS   (12)
#define PAIR_ST_BLOCKED       (13)
#define PAIR_ST_REVIEW        (14)
#define PAIR_ST_DELEGATED     (15)
#define PAIR_ST_DONE          (16)
#define PAIR_ST_CANCELLED     (17)
/* priority pairs */
#define PAIR_PRI_CRITICAL     (20)
#define PAIR_PRI_HIGH         (21)
#define PAIR_PRI_MEDIUM       (22)
#define PAIR_PRI_LOW          (23)
/* assignee pairs */
#define PAIR_ASSIGNEE_BASE    (40)
#define PAIR_ASSIGNEE_COUNT   (8)

/* ============================================================
 * ENUMS
 * ============================================================ */
typedef enum {
    VIEW_TICKETS = 0,
    VIEW_PEOPLE,
    VIEW_DASHBOARD,
    VIEW_MENTORSHIP,
    VIEW_COUNT
} ViewType;

typedef enum {
    LAYOUT_KANBAN = 0,
    LAYOUT_LIST,
    LAYOUT_SPLIT,
    LAYOUT_COUNT
} LayoutType;

/* ============================================================
 * DATA STRUCTURES
 * ============================================================ */
typedef struct {
    gchar *id;
    gchar *title;
    gchar *type;
    gchar *status;
    gchar *priority;
    gchar *assignee;   /* plain stem, e.g. "zach_podbielniak" */
    gchar *due_date;
    gchar *tags;       /* comma-separated */
    gchar *project;
    gchar *filepath;
    gchar *created;
    gchar *updated;
    gchar *issue_link;
    gchar *member_of;
    gint   progress;
} TUITicket;

typedef struct {
    gchar *name;
    gchar *email;
    gchar *role;
    gchar *team;
    gchar *filepath;
} TUIPerson;

/* ---- config ---- */
typedef struct {
    LayoutType  default_layout;
    ViewType    default_view;
    gchar     **kanban_columns;   /* NULL-terminated */
    gint        done_last_days;   /* -1 = hide all done */
    gchar      *scope;            /* "work", "personal", or NULL */
    gboolean    include_archived;
    gboolean    kanban_compact;
    gint        split_left_pct;   /* percent width for list pane in split */
} TUIConfig;

/* ---- application state ---- */
typedef struct {
    ViewType    current_view;
    LayoutType  current_layout;
    gint        selected_row;
    gint        selected_col;
    gint        scroll_offset;
    gint        detail_scroll;
    GHashTable *kanban_scroll;    /* col_idx(int) -> scroll(int) */
    gint        kanban_cards_per_col;

    /* data */
    GPtrArray  *tickets;    /* TUITicket* */
    GPtrArray  *people;     /* TUIPerson* */
    GHashTable *kanban_board; /* status -> GPtrArray<TUITicket*> */

    /* ui */
    gboolean    show_help;
    gboolean    render_markdown;
    gchar      *status_msg;
    gboolean    status_is_error;
    gchar       pending_motion;   /* 'g' for gg compound motion */
} AppState;

/* ---- top-level app ---- */
typedef struct {
    TUIConfig  *config;
    AppState   *state;
    GMainLoop  *main_loop;
    GIOChannel *stdin_channel;
    gchar      *directory;   /* resolved vimban directory */
    gboolean    needs_redraw;
    gboolean    running;
} VimbanTUI;

/* ============================================================
 * STATUSES / TYPES
 * ============================================================ */
static const gchar *STATUSES[] = {
    "backlog", "ready", "in_progress", "blocked",
    "review", "delegated", "done", "cancelled", NULL
};

static const gchar *PRIORITIES[] = {
    "critical", "high", "medium", "low", NULL
};

static const gchar *TICKET_TYPES[] = {
    "task", "bug", "story", "epic", "subtask",
    "research", "meeting", "resource", NULL
};

/* Full creation type list matching Python ALL_TYPES + ['person'] */
static const gchar *CREATE_TYPES[] = {
    "epic", "story", "task", "sub-task", "research", "bug",
    "area", "resource", "recipe", "project", "meeting",
    "person", NULL
};

/* Link relation types */
static const gchar *LINK_RELATIONS[] = {
    "blocks", "blocked-by", "relates-to", "member-of", NULL
};

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */
static void        tui_draw              (VimbanTUI *tui);
static void        tui_load_data         (VimbanTUI *tui);
static void        tui_free_data         (VimbanTUI *tui);
static TUITicket  *tui_get_selected_ticket (VimbanTUI *tui);
static TUIPerson  *tui_get_selected_person (VimbanTUI *tui);
static void        tui_clamp_selection   (VimbanTUI *tui);
static void        tui_set_status        (VimbanTUI *tui, const gchar *msg, gboolean is_error);
static void        tui_suspend_ncurses   (void);
static void        tui_restore_ncurses   (void);

/* ============================================================
 * MEMORY HELPERS
 * ============================================================ */
static void
ticket_free (TUITicket *t)
{
    if (!t) return;
    g_free(t->id);
    g_free(t->title);
    g_free(t->type);
    g_free(t->status);
    g_free(t->priority);
    g_free(t->assignee);
    g_free(t->due_date);
    g_free(t->tags);
    g_free(t->project);
    g_free(t->filepath);
    g_free(t->created);
    g_free(t->updated);
    g_free(t->issue_link);
    g_free(t->member_of);
    g_free(t);
}

static void
person_free (TUIPerson *p)
{
    if (!p) return;
    g_free(p->name);
    g_free(p->email);
    g_free(p->role);
    g_free(p->team);
    g_free(p->filepath);
    g_free(p);
}

/* ============================================================
 * CONFIG — simple line-based YAML reader (no external dep)
 * ============================================================ */
static gchar *
config_read_key (const gchar *path, const gchar *key)
{
    /* Read a scalar top-level key from a YAML file. */
    g_autoptr(GError) err = NULL;
    gchar *contents = NULL;
    gsize  length   = 0;

    if (!g_file_get_contents(path, &contents, &length, &err))
        return NULL;

    gchar **lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    {
    gint    i;
    gchar  *line;
    gchar  *colon;
    gchar  *k;
    gchar *value = NULL;
    for (i = 0; lines[i]; i++) {
        line = g_strstrip(lines[i]);
        if (line[0] == '#') continue;
        colon = strchr(line, ':');
        if (!colon) continue;

        k = g_strndup(line, colon - line);
        g_strstrip(k);

        if (g_strcmp0(k, key) == 0) {
            gchar *v = g_strdup(colon + 1);
            gsize  vlen;
            g_strstrip(v);
            /* strip surrounding quotes */
            vlen = strlen(v);
            if (vlen >= 2 && (v[0] == '"' || v[0] == '\'') && v[0] == v[vlen - 1]) {
                gchar *inner = g_strndup(v + 1, vlen - 2);
                g_free(v);
                v = inner;
            }
            value = v;
            g_free(k);
            break;
        }
        g_free(k);
    }

    g_strfreev(lines);
    return value;
    } /* end block */
}

static TUIConfig *
config_load (void)
{
    TUIConfig *cfg = g_new0(TUIConfig, 1);
    cfg->default_layout    = LAYOUT_KANBAN;
    cfg->default_view      = VIEW_TICKETS;
    cfg->done_last_days    = 7;
    cfg->split_left_pct    = 40;
    cfg->include_archived  = FALSE;
    cfg->kanban_compact    = FALSE;

    /* default kanban columns — 5 base, "done" added dynamically if done_last_days >= 0 */
    cfg->kanban_columns = g_new0(gchar *, 7);
    cfg->kanban_columns[0] = g_strdup("backlog");
    cfg->kanban_columns[1] = g_strdup("ready");
    cfg->kanban_columns[2] = g_strdup("in_progress");
    cfg->kanban_columns[3] = g_strdup("blocked");
    cfg->kanban_columns[4] = g_strdup("review");
    cfg->kanban_columns[5] = NULL;

    /* try config file */
    gchar *config_path = g_build_filename(
        g_get_home_dir(), ".config", "vimban", "config.yaml", NULL
    );

    if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        g_free(config_path);
        return cfg;
    }

    /* layout */
    gchar *layout_str = config_read_key(config_path, "default_layout");
    if (layout_str) {
        if      (g_strcmp0(layout_str, "list")  == 0) cfg->default_layout = LAYOUT_LIST;
        else if (g_strcmp0(layout_str, "split") == 0) cfg->default_layout = LAYOUT_SPLIT;
        g_free(layout_str);
    }

    /* view */
    gchar *view_str = config_read_key(config_path, "default_view");
    if (view_str) {
        if      (g_strcmp0(view_str, "people")     == 0) cfg->default_view = VIEW_PEOPLE;
        else if (g_strcmp0(view_str, "dashboard")  == 0) cfg->default_view = VIEW_DASHBOARD;
        else if (g_strcmp0(view_str, "mentorship") == 0) cfg->default_view = VIEW_MENTORSHIP;
        g_free(view_str);
    }

    /* scope — only accept known values */
    gchar *scope_str = config_read_key(config_path, "scope");
    if (scope_str && scope_str[0]) {
        if (g_strcmp0(scope_str, "work") == 0 || g_strcmp0(scope_str, "personal") == 0) {
            cfg->scope = scope_str;
        } else {
            g_free(scope_str);
        }
    } else {
        g_free(scope_str);
    }

    g_free(config_path);

    /* Append "done" column if done_last_days >= 0 (Python get_kanban_columns logic) */
    if (cfg->done_last_days >= 0) {
        gint ncols = 0;
        while (cfg->kanban_columns[ncols]) ncols++;
        cfg->kanban_columns = g_renew(gchar *, cfg->kanban_columns, ncols + 2);
        cfg->kanban_columns[ncols]     = g_strdup("done");
        cfg->kanban_columns[ncols + 1] = NULL;
    }

    return cfg;
}

static void
config_free (TUIConfig *cfg)
{
    if (!cfg) return;
    g_strfreev(cfg->kanban_columns);
    g_free(cfg->scope);
    g_free(cfg);
}

/* ============================================================
 * COLOUR THEME
 * ============================================================ */
static void
theme_init (void)
{
    if (!has_colors()) return;
    start_color();
    use_default_colors();

    /* UI chrome */
    init_pair(PAIR_HEADER,     C_LAVENDER,  C_SURFACE0);
    init_pair(PAIR_SELECTED,   C_TEXT,      C_SURFACE1);
    init_pair(PAIR_BORDER,     C_SURFACE2,  -1);
    init_pair(PAIR_STATUS_BAR, C_SUBTEXT1,  C_SURFACE0);
    init_pair(PAIR_ERROR,      C_RED,       -1);
    init_pair(PAIR_SUCCESS,    C_GREEN,     -1);
    init_pair(PAIR_HELP,       C_SUBTEXT0,  -1);

    /* status */
    init_pair(PAIR_ST_BACKLOG,     C_OVERLAY0, -1);
    init_pair(PAIR_ST_READY,       C_BLUE,     -1);
    init_pair(PAIR_ST_IN_PROGRESS, C_SKY,      -1);
    init_pair(PAIR_ST_BLOCKED,     C_RED,      -1);
    init_pair(PAIR_ST_REVIEW,      C_YELLOW,   -1);
    init_pair(PAIR_ST_DELEGATED,   C_MAUVE,    -1);
    init_pair(PAIR_ST_DONE,        C_GREEN,    -1);
    init_pair(PAIR_ST_CANCELLED,   C_OVERLAY0, -1);

    /* priority */
    init_pair(PAIR_PRI_CRITICAL, C_RED,     -1);
    init_pair(PAIR_PRI_HIGH,     C_PEACH,   -1);
    init_pair(PAIR_PRI_MEDIUM,   C_TEXT,    -1);
    init_pair(PAIR_PRI_LOW,      C_OVERLAY1,-1);

    /* assignee (8 distinct) */
    init_pair(PAIR_ASSIGNEE_BASE+0, C_ROSEWATER, -1);
    init_pair(PAIR_ASSIGNEE_BASE+1, C_FLAMINGO,  -1);
    init_pair(PAIR_ASSIGNEE_BASE+2, C_PINK,      -1);
    init_pair(PAIR_ASSIGNEE_BASE+3, C_MAUVE,     -1);
    init_pair(PAIR_ASSIGNEE_BASE+4, C_PEACH,     -1);
    init_pair(PAIR_ASSIGNEE_BASE+5, C_TEAL,      -1);
    init_pair(PAIR_ASSIGNEE_BASE+6, C_SAPPHIRE,  -1);
    init_pair(PAIR_ASSIGNEE_BASE+7, C_LAVENDER,  -1);
}

static gint
theme_status_pair (const gchar *status)
{
    if (!status) return PAIR_ST_BACKLOG;
    if (g_strcmp0(status, "ready")       == 0) return PAIR_ST_READY;
    if (g_strcmp0(status, "in_progress") == 0) return PAIR_ST_IN_PROGRESS;
    if (g_strcmp0(status, "blocked")     == 0) return PAIR_ST_BLOCKED;
    if (g_strcmp0(status, "review")      == 0) return PAIR_ST_REVIEW;
    if (g_strcmp0(status, "delegated")   == 0) return PAIR_ST_DELEGATED;
    if (g_strcmp0(status, "done")        == 0) return PAIR_ST_DONE;
    if (g_strcmp0(status, "cancelled")   == 0) return PAIR_ST_CANCELLED;
    return PAIR_ST_BACKLOG;
}

static gint
theme_priority_pair (const gchar *priority)
{
    if (!priority) return PAIR_PRI_MEDIUM;
    if (g_strcmp0(priority, "critical") == 0) return PAIR_PRI_CRITICAL;
    if (g_strcmp0(priority, "high")     == 0) return PAIR_PRI_HIGH;
    if (g_strcmp0(priority, "low")      == 0) return PAIR_PRI_LOW;
    return PAIR_PRI_MEDIUM;
}

static gint
theme_assignee_pair (const gchar *name)
{
    const gchar *p;
    guint h = 0;
    if (!name || !name[0]) return PAIR_PRI_MEDIUM;
    for (p = name; *p; p++)
        h = h * 31 + (guchar)g_ascii_tolower(*p);
    return PAIR_ASSIGNEE_BASE + (h % PAIR_ASSIGNEE_COUNT);
}

/* ============================================================
 * DATA LOADING — invoke vimban CLI
 * ============================================================ */
static gchar *
vimban_exec (VimbanTUI *tui, gchar **argv_extra)
{
    /*
     * Build: vimban --directory <dir> --format json <subcmd...>
     * Returns stdout as a newly-allocated string, or NULL on failure.
     */
    GPtrArray *args;
    gchar     *stdout_buf;
    gchar     *stderr_buf;
    gint       exit_status;
    gint       i;
    g_autoptr(GError) err2 = NULL;

    args = g_ptr_array_new();
    g_ptr_array_add(args, (gpointer)"vimban");
    g_ptr_array_add(args, (gpointer)"--directory");
    g_ptr_array_add(args, (gpointer)tui->directory);
    g_ptr_array_add(args, (gpointer)"--format");
    g_ptr_array_add(args, (gpointer)"json");
    g_ptr_array_add(args, (gpointer)"--no-color");
    g_ptr_array_add(args, (gpointer)"-q");

    /* inject scope as global flags (must come before subcommand) */
    if (tui->config->scope) {
        if (g_strcmp0(tui->config->scope, "work") == 0)
            g_ptr_array_add(args, (gpointer)"--work");
        else if (g_strcmp0(tui->config->scope, "personal") == 0)
            g_ptr_array_add(args, (gpointer)"--personal");
    }

    for (i = 0; argv_extra && argv_extra[i]; i++)
        g_ptr_array_add(args, argv_extra[i]);

    g_ptr_array_add(args, NULL);

    stdout_buf  = NULL;
    stderr_buf  = NULL;
    exit_status = 0;

    g_spawn_sync(
        NULL,
        (gchar **)args->pdata,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        &stdout_buf,
        &stderr_buf,
        &exit_status,
        &err2
    );

    g_ptr_array_free(args, FALSE);
    g_free(stderr_buf);

    if (err2 || exit_status != 0) {
        g_free(stdout_buf);
        return NULL;
    }

    return stdout_buf;
}

static GPtrArray *
parse_tickets_json (const gchar *json_str)
{
    /*
     * Parse JSON array of ticket objects returned by:
     *   vimban --format json list
     * Returns GPtrArray<TUITicket*> (caller owns, free with g_ptr_array_unref).
     */
    GPtrArray  *arr;
    JsonNode   *root;
    JsonArray  *jarray;
    guint       len;
    guint       i;
    g_autoptr(GError)      err    = NULL;
    g_autoptr(JsonParser)  parser = json_parser_new();

    arr = g_ptr_array_new_with_free_func((GDestroyNotify)ticket_free);

    if (!json_str || !json_str[0])
        return arr;

    if (!json_parser_load_from_data(parser, json_str, -1, &err))
        return arr;

    root = json_parser_get_root(parser);
    if (!root) return arr;

    /* root may be {"tickets":[...]} or directly [...] */
    jarray = NULL;
    if (JSON_NODE_HOLDS_ARRAY(root)) {
        jarray = json_node_get_array(root);
    } else if (JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj          = json_node_get_object(root);
        JsonNode   *tickets_node = json_object_get_member(obj, "tickets");
        if (tickets_node && JSON_NODE_HOLDS_ARRAY(tickets_node))
            jarray = json_node_get_array(tickets_node);
    }

    if (!jarray) return arr;

    len = json_array_get_length(jarray);
    for (i = 0; i < len; i++) {
        JsonNode   *elem = json_array_get_element(jarray, i);
        JsonObject *obj;
        TUITicket  *t;
        if (!JSON_NODE_HOLDS_OBJECT(elem)) continue;
        obj = json_node_get_object(elem);

        t = g_new0(TUITicket, 1);

#define J_STR(field, key) \
        do { \
            JsonNode *n = json_object_get_member(obj, key); \
            t->field = (n && !JSON_NODE_HOLDS_NULL(n)) \
                       ? g_strdup(json_node_get_string(n)) \
                       : g_strdup(""); \
        } while (0)

        J_STR(id,       "id");
        J_STR(title,    "title");
        J_STR(type,     "type");
        J_STR(status,   "status");
        J_STR(priority, "priority");
        J_STR(due_date, "due_date");
        J_STR(project,  "project");
        J_STR(filepath, "filepath");
        J_STR(tags,     "tags");

        /* assignee — may be transclusion string like "![[people/zach.md]]" */
        {
            JsonNode *n = json_object_get_member(obj, "assignee");
            gchar *raw = (n && !JSON_NODE_HOLDS_NULL(n))
                         ? g_strdup(json_node_get_string(n))
                         : NULL;
            if (raw && raw[0]) {
                /* strip ![[...]] if present */
                gchar *inner = raw;
                if (g_str_has_prefix(raw, "![[")) {
                    inner = raw + 3;
                    gchar *end = strstr(inner, "]]");
                    if (end) *end = '\0';
                }
                /* take basename without extension */
                gchar *base = g_path_get_basename(inner);
                gchar *dot  = strrchr(base, '.');
                if (dot) *dot = '\0';
                t->assignee = base;
            } else {
                t->assignee = g_strdup("");
            }
            g_free(raw);
        }

        /* progress */
        {
            JsonNode *n = json_object_get_member(obj, "progress");
            if (n && !JSON_NODE_HOLDS_NULL(n))
                t->progress = (gint)json_node_get_int(n);
        }

        J_STR(created,    "created");
        J_STR(updated,    "updated");
        J_STR(issue_link, "issue_link");
        J_STR(member_of,  "member_of");

        /* defaults */
        if (!t->status   || !t->status[0])   { g_free(t->status);   t->status   = g_strdup("backlog"); }
        if (!t->priority || !t->priority[0]) { g_free(t->priority); t->priority = g_strdup("medium");  }
        if (!t->type     || !t->type[0])     { g_free(t->type);     t->type     = g_strdup("task");    }

#undef J_STR

        g_ptr_array_add(arr, t);
    }

    return arr;
}

static GPtrArray *
parse_people_json (const gchar *json_str)
{
    /*
     * Parse JSON array of person objects.
     * Returns GPtrArray<TUIPerson*>.
     */
    GPtrArray  *arr;
    JsonNode   *root;
    JsonArray  *jarray;
    guint       len;
    guint       i;
    g_autoptr(GError)      err    = NULL;
    g_autoptr(JsonParser)  parser = json_parser_new();

    arr = g_ptr_array_new_with_free_func((GDestroyNotify)person_free);

    if (!json_str || !json_str[0])
        return arr;

    if (!json_parser_load_from_data(parser, json_str, -1, &err))
        return arr;

    root = json_parser_get_root(parser);
    if (!root) return arr;

    jarray = NULL;
    if (JSON_NODE_HOLDS_ARRAY(root)) {
        jarray = json_node_get_array(root);
    } else if (JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        JsonNode   *pn  = json_object_get_member(obj, "people");
        if (pn && JSON_NODE_HOLDS_ARRAY(pn))
            jarray = json_node_get_array(pn);
    }

    if (!jarray) return arr;

    len = json_array_get_length(jarray);
    for (i = 0; i < len; i++) {
        JsonNode   *elem = json_array_get_element(jarray, i);
        JsonObject *obj;
        TUIPerson  *p;
        if (!JSON_NODE_HOLDS_OBJECT(elem)) continue;
        obj = json_node_get_object(elem);

        p = g_new0(TUIPerson, 1);

#define J_STR(field, key) \
        do { \
            JsonNode *n = json_object_get_member(obj, key); \
            p->field = (n && !JSON_NODE_HOLDS_NULL(n)) \
                       ? g_strdup(json_node_get_string(n)) \
                       : g_strdup(""); \
        } while (0)

        J_STR(name,     "name");
        J_STR(email,    "email");
        J_STR(role,     "role");
        J_STR(team,     "team");
        J_STR(filepath, "filepath");

#undef J_STR

        g_ptr_array_add(arr, p);
    }

    return arr;
}

static void
tui_build_kanban_board (VimbanTUI *tui)
{
    /*
     * Organise tickets into per-status GPtrArrays stored in a GHashTable.
     * Keys are static status strings; values are GPtrArray<TUITicket*>
     * (not owned — the tickets live in tui->state->tickets).
     */
    AppState *s = tui->state;
    gint      i;
    guint     j;

    if (s->kanban_board) {
        g_hash_table_destroy(s->kanban_board);
    }
    s->kanban_board = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        NULL,                               /* keys are static strings */
        (GDestroyNotify)g_ptr_array_unref   /* values are ref-counted arrays */
    );

    /* pre-populate all known statuses */
    for (i = 0; STATUSES[i]; i++) {
        g_hash_table_insert(
            s->kanban_board,
            (gpointer)STATUSES[i],
            g_ptr_array_new()
        );
    }

    /* bucket tickets */
    for (j = 0; j < s->tickets->len; j++) {
        TUITicket *t   = g_ptr_array_index(s->tickets, j);
        GPtrArray *col = g_hash_table_lookup(s->kanban_board, t->status);
        if (col)
            g_ptr_array_add(col, t);
    }
}

static void
tui_free_data (VimbanTUI *tui)
{
    AppState *s = tui->state;

    if (s->tickets) {
        g_ptr_array_unref(s->tickets);
        s->tickets = NULL;
    }
    if (s->people) {
        g_ptr_array_unref(s->people);
        s->people = NULL;
    }
    if (s->kanban_board) {
        g_hash_table_destroy(s->kanban_board);
        s->kanban_board = NULL;
    }
}

/* thread data for parallel vimban_exec */
typedef struct {
    VimbanTUI  *tui;
    gchar     **argv;
    gchar      *result;   /* owned by caller after join */
} ExecThreadData;

static gpointer
exec_thread_func (gpointer data)
{
    ExecThreadData *td = (ExecThreadData *)data;
    td->result = vimban_exec(td->tui, td->argv);
    return NULL;
}

/* Sort comparator: tickets by due_date ascending, NULLs/empty last */
static gint
ticket_cmp_due_date (gconstpointer a, gconstpointer b)
{
    const TUITicket *ta = *(const TUITicket **)a;
    const TUITicket *tb = *(const TUITicket **)b;
    gboolean a_empty = (!ta->due_date || !ta->due_date[0]);
    gboolean b_empty = (!tb->due_date || !tb->due_date[0]);

    if (a_empty && b_empty) return 0;
    if (a_empty) return 1;   /* NULLs last */
    if (b_empty) return -1;
    return g_strcmp0(ta->due_date, tb->due_date);
}

static void
tui_load_data (VimbanTUI *tui)
{
    GThread        *ticket_thread;
    GThread        *people_thread;
    ExecThreadData  ticket_td;
    ExecThreadData  people_td;
    GPtrArray      *list_args;
    gchar         **list_argv;
    gchar          *people_argv[] = { "people", "list", NULL };

    tui_free_data(tui);

    /* Build ticket list argv (scope flags injected by vimban_exec) */
    list_args = g_ptr_array_new();
    g_ptr_array_add(list_args, (gpointer)"list");
    g_ptr_array_add(list_args, NULL);
    list_argv = (gchar **)list_args->pdata;

    /* spawn both vimban commands in parallel */
    ticket_td.tui    = tui;
    ticket_td.argv   = list_argv;
    ticket_td.result = NULL;

    people_td.tui    = tui;
    people_td.argv   = people_argv;
    people_td.result = NULL;

    ticket_thread = g_thread_new("load-tickets", exec_thread_func, &ticket_td);
    people_thread = g_thread_new("load-people",  exec_thread_func, &people_td);

    g_thread_join(ticket_thread);
    g_thread_join(people_thread);

    g_ptr_array_free(list_args, FALSE);

    /* parse results */
    tui->state->tickets = parse_tickets_json(ticket_td.result);
    tui->state->people  = parse_people_json(people_td.result);
    g_free(ticket_td.result);
    g_free(people_td.result);

    /* Filter done tickets by done_last_days cutoff */
    if (tui->config->done_last_days > 0 && tui->state->tickets) {
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autoptr(GDateTime) cutoff = g_date_time_add_days(now, -tui->config->done_last_days);
        g_autofree gchar *cutoff_str = g_date_time_format(cutoff, "%Y-%m-%d");
        guint i;

        for (i = tui->state->tickets->len; i > 0; i--) {
            TUITicket *t = g_ptr_array_index(tui->state->tickets, i - 1);
            if (g_strcmp0(t->status, "done") == 0) {
                /* Use updated date as proxy for end_date */
                const gchar *date_str = (t->updated && t->updated[0]) ? t->updated : NULL;
                if (!date_str || g_strcmp0(date_str, cutoff_str) < 0) {
                    g_ptr_array_remove_index(tui->state->tickets, i - 1);
                }
            }
        }
    } else if (tui->config->done_last_days < 0 && tui->state->tickets) {
        /* done_last_days == -1: exclude all done and cancelled */
        guint i;
        for (i = tui->state->tickets->len; i > 0; i--) {
            TUITicket *t = g_ptr_array_index(tui->state->tickets, i - 1);
            if (g_strcmp0(t->status, "done") == 0 || g_strcmp0(t->status, "cancelled") == 0) {
                g_ptr_array_remove_index(tui->state->tickets, i - 1);
            }
        }
    }

    /* Sort by due_date ascending, NULLs last */
    if (tui->state->tickets && tui->state->tickets->len > 1)
        g_ptr_array_sort(tui->state->tickets, ticket_cmp_due_date);

    tui_build_kanban_board(tui);
    tui_clamp_selection(tui);
}

/* ============================================================
 * STATE HELPERS
 * ============================================================ */
static void
tui_set_status (VimbanTUI *tui, const gchar *msg, gboolean is_error)
{
    g_free(tui->state->status_msg);
    tui->state->status_msg      = g_strdup(msg ? msg : "");
    tui->state->status_is_error = is_error;
    tui->needs_redraw           = TRUE;
}

static gint
kanban_col_scroll (VimbanTUI *tui, gint col_idx)
{
    gpointer val = g_hash_table_lookup(
        tui->state->kanban_scroll,
        GINT_TO_POINTER(col_idx)
    );
    return GPOINTER_TO_INT(val);
}

static void
kanban_col_scroll_set (VimbanTUI *tui, gint col_idx, gint scroll)
{
    g_hash_table_insert(
        tui->state->kanban_scroll,
        GINT_TO_POINTER(col_idx),
        GINT_TO_POINTER(MAX(0, scroll))
    );
}

static gint
kanban_col_count (VimbanTUI *tui)
{
    gint n = 0;
    gint i;
    for (i = 0; tui->config->kanban_columns[i]; i++)
        n++;
    return n;
}

static GPtrArray *
kanban_col_tickets (VimbanTUI *tui, gint col_idx)
{
    const gchar *status;
    gint num_cols;

    if (!tui->state->kanban_board) return NULL;

    num_cols = kanban_col_count(tui);
    if (col_idx < 0 || col_idx >= num_cols) return NULL;

    status = tui->config->kanban_columns[col_idx];
    if (!status) return NULL;
    return g_hash_table_lookup(tui->state->kanban_board, status);
}

static void
tui_clamp_selection (VimbanTUI *tui)
{
    AppState *s = tui->state;

    if (s->current_view == VIEW_PEOPLE) {
        gint max_row = s->people ? (gint)s->people->len - 1 : 0;
        s->selected_row = CLAMP(s->selected_row, 0, MAX(0, max_row));
        s->selected_col = 0;
        return;
    }

    if (s->current_layout == LAYOUT_KANBAN) {
        gint num_cols = kanban_col_count(tui);
        s->selected_col = CLAMP(s->selected_col, 0, MAX(0, num_cols - 1));

        GPtrArray *col = kanban_col_tickets(tui, s->selected_col);
        gint max_row   = col ? (gint)col->len - 1 : 0;
        s->selected_row = CLAMP(s->selected_row, 0, MAX(0, max_row));

        /* adjust scroll to keep selected visible */
        gint cards = s->kanban_cards_per_col;
        if (cards > 0) {
            gint scroll = kanban_col_scroll(tui, s->selected_col);
            if (s->selected_row < scroll)
                kanban_col_scroll_set(tui, s->selected_col, s->selected_row);
            else if (s->selected_row >= scroll + cards)
                kanban_col_scroll_set(tui, s->selected_col, s->selected_row - cards + 1);
        }
        return;
    }

    /* list / split */
    gint max_row = s->tickets ? (gint)s->tickets->len - 1 : 0;
    s->selected_row = CLAMP(s->selected_row, 0, MAX(0, max_row));
    s->selected_col = 0;

    /* scroll offset for list */
    if (s->scroll_offset > s->selected_row)
        s->scroll_offset = s->selected_row;
}

static TUITicket *
tui_get_selected_ticket (VimbanTUI *tui)
{
    AppState *s = tui->state;

    if (s->current_layout == LAYOUT_KANBAN) {
        GPtrArray *col = kanban_col_tickets(tui, s->selected_col);
        if (!col || s->selected_row >= (gint)col->len)
            return NULL;
        return g_ptr_array_index(col, s->selected_row);
    }

    if (!s->tickets || s->selected_row >= (gint)s->tickets->len)
        return NULL;
    return g_ptr_array_index(s->tickets, s->selected_row);
}

static TUIPerson *
tui_get_selected_person (VimbanTUI *tui)
{
    AppState *s = tui->state;
    if (!s->people || s->selected_row >= (gint)s->people->len)
        return NULL;
    return g_ptr_array_index(s->people, s->selected_row);
}

/* ============================================================
 * NCURSES SUSPEND / RESTORE
 * ============================================================ */
static void
tui_suspend_ncurses (void)
{
    /* give up ncurses so we can run interactive programs */
    def_prog_mode();
    endwin();
}

static void
tui_restore_ncurses (void)
{
    reset_prog_mode();
    refresh();
    doupdate();
}

/* ============================================================
 * DRAWING HELPERS
 * ============================================================ */

/* safe addstr that truncates to available width */
static void
safe_addstr (gint y, gint x, const gchar *str, gint max_width)
{
    if (!str || max_width <= 0) return;

    gint len = (gint)strlen(str);
    if (len > max_width) len = max_width;

    /* create a null-terminated copy of exactly len bytes */
    gchar *buf = g_strndup(str, len);
    mvaddstr(y, x, buf);
    g_free(buf);
}

static void
draw_header (VimbanTUI *tui)
{
    AppState   *s = tui->state;
    gint        width = getmaxx(stdscr);
    gint        x;

    static const gchar *view_names[]   = { "Tickets","People","Dashboard","Mentorship" };
    static const gchar *layout_names[] = { "Kanban","List","Split" };

    const gchar *vname = (s->current_view   < VIEW_COUNT)   ? view_names[s->current_view]   : "?";
    const gchar *lname = (s->current_layout < LAYOUT_COUNT) ? layout_names[s->current_layout]: "?";

    gchar *right = g_strdup_printf("[%s] Layout: %s  q:quit  ?:help", vname, lname);
    gchar *left  = g_strdup("VIMBAN TUI");

    gint padding = width - (gint)strlen(left) - (gint)strlen(right) - 2;
    if (padding < 1) padding = 1;
    gchar *header = g_strdup_printf(" %s%*s%s ", left, padding, "", right);

    attron(COLOR_PAIR(PAIR_HEADER));
    safe_addstr(0, 0, header, width);
    attroff(COLOR_PAIR(PAIR_HEADER));

    /* separator */
    attron(COLOR_PAIR(PAIR_BORDER));
    for (x = 0; x < width; x++)
        mvaddch(1, x, '-');
    attroff(COLOR_PAIR(PAIR_BORDER));

    g_free(left);
    g_free(right);
    g_free(header);
}

static void
draw_footer (VimbanTUI *tui)
{
    AppState *s    = tui->state;
    gint      height = getmaxy(stdscr);
    gint      width  = getmaxx(stdscr);
    gint      y      = height - 1;

    if (s->status_msg && s->status_msg[0]) {
        gint pair = s->status_is_error ? PAIR_ERROR : PAIR_SUCCESS;
        attron(COLOR_PAIR(pair));
        safe_addstr(y, 0, s->status_msg, width - 1);
        attroff(COLOR_PAIR(pair));
        /* clear after one frame */
        g_free(s->status_msg);
        s->status_msg = g_strdup("");
    } else {
        const gchar *hints;
        if (s->current_view == VIEW_PEOPLE)
            hints = "j/k:navigate  e:edit  i:info  n:new  d:delete  c/C:comment  t:view  TAB:layout  ?:help";
        else
            hints = "j/k:nav  h/l:col  H/L:status  e:edit  i:info  n:new  m:move  M:relocate  c/C:comment  ?:help";

        attron(COLOR_PAIR(PAIR_HELP));
        safe_addstr(y, 0, hints, width - 1);
        attroff(COLOR_PAIR(PAIR_HELP));
    }
}

/* ============================================================
 * KANBAN VIEW
 * ============================================================ */
static void
draw_kanban (VimbanTUI *tui, gint start_y, gint height, gint width)
{
    AppState    *s;
    gchar      **cols;
    gint         num_cols;
    gint         col_width;
    gint         card_h;
    gint         cards_per;
    gint         ci;

    s        = tui->state;
    cols     = tui->config->kanban_columns;
    num_cols = kanban_col_count(tui);

    if (num_cols == 0) return;

    col_width = (width - 1) / num_cols;
    if (col_width < 4) return;  /* too many columns for terminal width */
    card_h    = tui->config->kanban_compact ? 3 : 4;
    cards_per = (height - 2) / card_h;

    /* store for scroll adjustment */
    s->kanban_cards_per_col = cards_per;

    /* column headers */
    for (ci = 0; ci < num_cols; ci++) {
        const gchar *status  = cols[ci];
        gint         cx      = ci * col_width;
        GPtrArray   *col_arr = g_hash_table_lookup(s->kanban_board, status);
        guint        count   = col_arr ? col_arr->len : 0;
        GPtrArray   *tickets;
        gint         scroll;
        gint         end;
        gint         ti;
        gint         pair;
        gchar       *header;

        {
            g_autofree gchar *upper = g_ascii_strup(status, -1);
            header = g_strdup_printf("%s (%u)", upper, count);
        }

        pair = theme_status_pair(status);
        attron(COLOR_PAIR(pair) | A_BOLD);
        safe_addstr(start_y, cx, header, col_width - 1);
        attroff(COLOR_PAIR(pair) | A_BOLD);

        g_free(header);

        /* vertical separator */
        if (ci < num_cols - 1) {
            gint y;
            attron(COLOR_PAIR(PAIR_BORDER));
            for (y = start_y + 1; y < start_y + height; y++)
                mvaddch(y, cx + col_width - 1, ACS_VLINE);
            attroff(COLOR_PAIR(PAIR_BORDER));
        }

        /* cards */
        tickets = g_hash_table_lookup(s->kanban_board, status);
        if (!tickets) continue;

        scroll = kanban_col_scroll(tui, ci);
        end    = MIN(scroll + cards_per, (gint)tickets->len);

        for (ti = scroll; ti < end; ti++) {
            TUITicket *t     = g_ptr_array_index(tickets, ti);
            gint       disp  = ti - scroll;
            gint       cy    = start_y + 1 + disp * card_h;
            gboolean   sel   = (ci == s->selected_col && ti == s->selected_row);

            if (sel) attron(COLOR_PAIR(PAIR_SELECTED));

            /* line 1: marker + id */
            gchar *line1 = g_strdup_printf("%c%s", sel ? '>' : ' ', t->id ? t->id : "");
            safe_addstr(cy, cx, line1, col_width - 2);
            g_free(line1);

            /* line 2: title */
            gchar *title_pad = g_strdup_printf(" %s", t->title ? t->title : "");
            safe_addstr(cy + 1, cx, title_pad, col_width - 2);
            g_free(title_pad);

            /* line 3: priority + assignee (if not compact) */
            if (card_h >= 4) {
                if (!sel) attroff(COLOR_PAIR(PAIR_SELECTED));

                gchar *pri_str = g_strdup_printf(" [%-.4s]", t->priority ? t->priority : "med");
                if (!sel) attron(COLOR_PAIR(theme_priority_pair(t->priority)));
                safe_addstr(cy + 2, cx, pri_str, (gint)strlen(pri_str));
                if (!sel) attroff(COLOR_PAIR(theme_priority_pair(t->priority)));

                if (t->assignee && t->assignee[0]) {
                    gchar *asgn = g_strdup_printf(" @%s", t->assignee);
                    gint   ax   = cx + (gint)strlen(pri_str);
                    gint   aw   = col_width - 2 - (gint)strlen(pri_str);
                    if (!sel) attron(COLOR_PAIR(theme_assignee_pair(t->assignee)));
                    safe_addstr(cy + 2, ax, asgn, aw);
                    if (!sel) attroff(COLOR_PAIR(theme_assignee_pair(t->assignee)));
                    g_free(asgn);
                }
                g_free(pri_str);

                if (sel) attroff(COLOR_PAIR(PAIR_SELECTED));
            } else {
                if (sel) attroff(COLOR_PAIR(PAIR_SELECTED));
            }
        }

        /* scroll indicator */
        if ((gint)tickets->len > cards_per) {
            gint cx_ind = ci * col_width;
            gchar *ind = g_strdup_printf("[%d-%d/%u]",
                scroll + 1,
                MIN(scroll + cards_per, (gint)tickets->len),
                tickets->len);
            attron(COLOR_PAIR(PAIR_HELP));
            safe_addstr(start_y + height - 1, cx_ind, ind, col_width - 2);
            attroff(COLOR_PAIR(PAIR_HELP));
            g_free(ind);
        }
    }
}

/* ============================================================
 * LIST VIEW
 * ============================================================ */
static void
draw_list (VimbanTUI *tui, gint start_y, gint height, gint width)
{
    AppState  *s;
    GPtrArray *tickets;
    gint       w_id;
    gint       w_status;
    gint       w_priority;
    gint       w_assignee;
    gint       w_due;
    gint       w_title;
    gint       visible;
    gint       i;
    gint       x_sep;

    s       = tui->state;
    tickets = s->tickets;

    /* column widths */
    w_id       = 12;
    w_status   = 12;
    w_priority = 10;
    w_assignee = 16;
    w_due      = 12;
    w_title    = MAX(20, width - w_id - w_status - w_priority - w_assignee - w_due - 6);

    /* header */
    attron(A_BOLD);
    gchar *hdr = g_strdup_printf("%-*s %-*s %-*s %-*s %-*s %-*s",
        w_id,       "ID",
        w_status,   "STATUS",
        w_priority, "PRIORITY",
        w_assignee, "ASSIGNEE",
        w_title,    "TITLE",
        w_due,      "DUE"
    );
    safe_addstr(start_y, 0, hdr, width - 1);
    attroff(A_BOLD);
    g_free(hdr);

    /* separator */
    attron(COLOR_PAIR(PAIR_BORDER));
    for (x_sep = 0; x_sep < width - 1; x_sep++)
        mvaddch(start_y + 1, x_sep, '-');
    attroff(COLOR_PAIR(PAIR_BORDER));

    visible = height - 3;

    /*
     * NOTE: scroll_offset is mutated here inside the draw function because
     * the visible area height depends on the actual rendered layout, which
     * is only known at draw time. Ideally this would live in
     * tui_clamp_selection, but refactoring risks regressions since the
     * visible height varies between list, split, and people views.
     */
    if (s->selected_row < s->scroll_offset)
        s->scroll_offset = s->selected_row;
    if (s->selected_row >= s->scroll_offset + visible)
        s->scroll_offset = s->selected_row - visible + 1;

    if (!tickets) return;

    for (i = 0; i < visible; i++) {
        gint       idx = s->scroll_offset + i;
        TUITicket *t;
        gboolean   sel;
        gint       ry;
        gint       x;

        if (idx >= (gint)tickets->len) break;

        t   = g_ptr_array_index(tickets, idx);
        sel = (idx == s->selected_row);
        ry  = start_y + 2 + i;

        if (sel) attron(COLOR_PAIR(PAIR_SELECTED));

        /* ID */
        {
            gchar *id_str = g_strdup_printf("%-*.*s", w_id, w_id, t->id ? t->id : "");
            safe_addstr(ry, 0, id_str, w_id);
            g_free(id_str);
        }

        x = w_id + 1;

        /* STATUS with colour */
        if (!sel) attron(COLOR_PAIR(theme_status_pair(t->status)));
        gchar *st_str = g_strdup_printf("%-*.*s", w_status, w_status, t->status ? t->status : "");
        safe_addstr(ry, x, st_str, w_status);
        g_free(st_str);
        if (!sel) attroff(COLOR_PAIR(theme_status_pair(t->status)));
        x += w_status + 1;

        /* PRIORITY */
        if (!sel) attron(COLOR_PAIR(theme_priority_pair(t->priority)));
        gchar *pri_str = g_strdup_printf("%-*.*s", w_priority, w_priority, t->priority ? t->priority : "");
        safe_addstr(ry, x, pri_str, w_priority);
        g_free(pri_str);
        if (!sel) attroff(COLOR_PAIR(theme_priority_pair(t->priority)));
        x += w_priority + 1;

        /* ASSIGNEE */
        if (!sel && t->assignee && t->assignee[0])
            attron(COLOR_PAIR(theme_assignee_pair(t->assignee)));
        gchar *asgn_str = g_strdup_printf("%-*.*s", w_assignee, w_assignee, t->assignee ? t->assignee : "");
        safe_addstr(ry, x, asgn_str, w_assignee);
        g_free(asgn_str);
        if (!sel && t->assignee && t->assignee[0])
            attroff(COLOR_PAIR(theme_assignee_pair(t->assignee)));
        x += w_assignee + 1;

        /* TITLE */
        gchar *title_str = g_strdup_printf("%-*.*s", w_title, w_title, t->title ? t->title : "");
        safe_addstr(ry, x, title_str, w_title);
        g_free(title_str);
        x += w_title + 1;

        /* DUE */
        gchar *due_str = g_strdup_printf("%-*.*s", w_due, w_due, t->due_date ? t->due_date : "");
        safe_addstr(ry, x, due_str, w_due);
        g_free(due_str);

        if (sel) attroff(COLOR_PAIR(PAIR_SELECTED));
    }
}

/* ============================================================
 * SPLIT VIEW
 * ============================================================ */
static void
draw_split (VimbanTUI *tui, gint start_y, gint height, gint width)
{
    AppState  *s;
    gint       list_w;
    gint       sep_x;
    gint       detail_x;
    gint       detail_w;
    TUITicket *t;
    gint       dy;
    gint       yx;

    s        = tui->state;
    list_w   = (width * tui->config->split_left_pct) / 100;
    sep_x    = list_w;
    detail_x = sep_x + 1;
    detail_w = width - detail_x;

    /* draw list on left */
    draw_list(tui, start_y, height, list_w);

    /* separator */
    attron(COLOR_PAIR(PAIR_BORDER));
    for (yx = start_y; yx < start_y + height; yx++)
        mvaddch(yx, sep_x, ACS_VLINE);
    attroff(COLOR_PAIR(PAIR_BORDER));

    /* detail pane */
    t = tui_get_selected_ticket(tui);
    if (!t) return;

    dy = start_y;

    /* heading */
    attron(A_BOLD | COLOR_PAIR(PAIR_HEADER));
    safe_addstr(dy, detail_x, t->id ? t->id : "", detail_w);
    attroff(A_BOLD | COLOR_PAIR(PAIR_HEADER));
    dy++;

    attron(A_BOLD);
    safe_addstr(dy, detail_x, t->title ? t->title : "", detail_w);
    attroff(A_BOLD);
    dy++;

    /* separator */
    {
        gint xi;
        attron(COLOR_PAIR(PAIR_BORDER));
        for (xi = detail_x; xi < detail_x + detail_w - 1; xi++)
            mvaddch(dy, xi, '-');
        attroff(COLOR_PAIR(PAIR_BORDER));
    }
    dy++;

    /* metadata lines */
    gchar *meta = g_strdup_printf("Status: %-12s Priority: %s",
        t->status   ? t->status   : "",
        t->priority ? t->priority : "");
    safe_addstr(dy, detail_x, meta, detail_w);
    g_free(meta);
    dy++;

    if (t->assignee && t->assignee[0]) {
        gchar *asgn = g_strdup_printf("Assignee: %s", t->assignee);
        safe_addstr(dy, detail_x, asgn, detail_w);
        g_free(asgn);
        dy++;
    }

    if (t->due_date && t->due_date[0]) {
        gchar *due = g_strdup_printf("Due: %s", t->due_date);
        safe_addstr(dy, detail_x, due, detail_w);
        g_free(due);
        dy++;
    }

    if (t->tags && t->tags[0]) {
        gchar *tags = g_strdup_printf("Tags: %s", t->tags);
        safe_addstr(dy, detail_x, tags, detail_w);
        g_free(tags);
        dy++;
    }

    if (t->progress > 0) {
        gchar *prog = g_strdup_printf("Progress: %d%%", t->progress);
        safe_addstr(dy, detail_x, prog, detail_w);
        g_free(prog);
        dy++;
    }

    /* separator before body */
    {
        gint xi;
        attron(COLOR_PAIR(PAIR_BORDER));
        for (xi = detail_x; xi < detail_x + detail_w - 1; xi++)
            mvaddch(dy, xi, '-');
        attroff(COLOR_PAIR(PAIR_BORDER));
    }
    dy++;

    /* body: read file, skip frontmatter, render lines */
    if (t->filepath && t->filepath[0] && dy < start_y + height) {
        gchar *contents = NULL;
        gsize  len      = 0;

        if (g_file_get_contents(t->filepath, &contents, &len, NULL)) {
            /* skip YAML frontmatter between --- markers */
            gchar *body = contents;
            if (g_str_has_prefix(contents, "---")) {
                gchar *end = strstr(contents + 3, "\n---");
                if (end) body = end + 4; /* skip past the closing --- */
            }

            {
            gchar **lines  = g_strsplit(body, "\n", -1);
            gint    scroll = s->detail_scroll;
            gint    shown  = 0;
            gint    li;
            g_free(contents);

            for (li = scroll; lines[li] && dy + shown < start_y + height; li++) {
                /* basic line wrapping */
                gchar *line   = lines[li];
                gint   llen   = (gint)strlen(line);
                gint   offset = 0;

                if (llen == 0) {
                    shown++;
                    dy++;
                    continue;
                }

                while (offset < llen && dy + shown < start_y + height) {
                    safe_addstr(dy + shown, detail_x, line + offset, detail_w);
                    offset += detail_w;
                    shown++;
                }
            }

            g_strfreev(lines);
            }
        }
    }

    (void)s; /* suppress unused warning */
}

/* ============================================================
 * PEOPLE VIEW
 * ============================================================ */
static void
draw_people (VimbanTUI *tui, gint start_y, gint height, gint width)
{
    gint       i;
    gint       x_sep;
    gint       visible;
    AppState  *s      = tui->state;
    GPtrArray *people = s->people;

    gint w_name  = 22;
    gint w_role  = 22;
    gint w_team  = 16;
    gint w_email = MAX(20, width - w_name - w_role - w_team - 5);

    /* header */
    attron(A_BOLD);
    gchar *hdr = g_strdup_printf("%-*s %-*s %-*s %-*s",
        w_name, "NAME", w_role, "ROLE", w_team, "TEAM", w_email, "EMAIL");
    safe_addstr(start_y, 0, hdr, width - 1);
    attroff(A_BOLD);
    g_free(hdr);

    /* separator */
    attron(COLOR_PAIR(PAIR_BORDER));
    for (x_sep = 0; x_sep < width - 1; x_sep++)
        mvaddch(start_y + 1, x_sep, '-');
    attroff(COLOR_PAIR(PAIR_BORDER));

    if (!people) return;

    visible = height - 3;

    /* see NOTE in draw_list about scroll_offset mutation in draw */
    if (s->selected_row < s->scroll_offset)
        s->scroll_offset = s->selected_row;
    if (s->selected_row >= s->scroll_offset + visible)
        s->scroll_offset = s->selected_row - visible + 1;

    for (i = 0; i < visible; i++) {
        gint       idx = s->scroll_offset + i;
        TUIPerson *p;
        gboolean   sel;
        gint       ry;
        if (idx >= (gint)people->len) break;

        p   = g_ptr_array_index(people, idx);
        sel = (idx == s->selected_row);
        ry  = start_y + 2 + i;

        if (sel) attron(COLOR_PAIR(PAIR_SELECTED));

        gchar *row = g_strdup_printf("%-*.*s %-*.*s %-*.*s %-*.*s",
            w_name,  w_name,  p->name  ? p->name  : "",
            w_role,  w_role,  p->role  ? p->role  : "",
            w_team,  w_team,  p->team  ? p->team  : "",
            w_email, w_email, p->email ? p->email : "");
        safe_addstr(ry, 0, row, width - 1);
        g_free(row);

        if (sel) attroff(COLOR_PAIR(PAIR_SELECTED));
    }
}

/* ============================================================
 * DASHBOARD VIEW
 * ============================================================ */
static void
draw_dashboard (VimbanTUI *tui, gint start_y, gint height, gint width)
{
    AppState *s = tui->state;
    gint      i;
    gint      y   = start_y;

    attron(A_BOLD);
    safe_addstr(y++, 2, "DASHBOARD", width - 4);
    attroff(A_BOLD);
    y++;

    if (!s->tickets || !s->people) {
        safe_addstr(y, 2, "No data loaded.", width - 4);
        return;
    }

    gchar *summary = g_strdup_printf("Total tickets: %u   People: %u",
        s->tickets->len, s->people->len);
    safe_addstr(y++, 2, summary, width - 4);
    g_free(summary);
    y++;

    safe_addstr(y++, 2, "BY STATUS:", width - 4);

    for (i = 0; STATUSES[i] && y < start_y + height; i++) {
        GPtrArray *col   = g_hash_table_lookup(s->kanban_board, STATUSES[i]);
        guint      count = col ? col->len : 0;
        gchar     *line;
        if (count == 0) continue;

        attron(COLOR_PAIR(theme_status_pair(STATUSES[i])));
        line = g_strdup_printf("  %-14s %u", STATUSES[i], count);
        safe_addstr(y++, 2, line, width - 4);
        g_free(line);
        attroff(COLOR_PAIR(theme_status_pair(STATUSES[i])));
    }
}

/* ============================================================
 * HELP OVERLAY
 * ============================================================ */
static const gchar *HELP_LINES[] = {
    "VIMBAN TUI — KEY BINDINGS",
    "",
    "NAVIGATION",
    "  j/k         Move down/up",
    "  h/l         Move left/right (kanban columns)",
    "  H/L         Move ticket status backward/forward",
    "  gg          Go to top",
    "  G           Go to bottom",
    "  Ctrl-d/u    Page down/up",
    "  Tab         Cycle layout (kanban/list/split)",
    "  t           Next view   T: prev view",
    "",
    "ACTIONS",
    "  e           Edit selected item in $EDITOR",
    "  E           Edit ticket metadata (field picker)",
    "  i           Show item info",
    "  n           Create new item",
    "  d           Delete selected item",
    "  m           Move ticket to different status",
    "  M           Move file location (fzf)",
    "  A           Archive ticket",
    "  p           Change priority",
    "  c           Add comment",
    "  r           Refresh data",
    "  S           Commit & sync (git pull/push)",
    "",
    "OTHER",
    "  ?           Toggle this help",
    "  q           Quit",
    "",
    "Press any key to close...",
    NULL
};

static void
draw_help (gint start_y, gint height, gint width)
{
    gint i;
    for (i = 0; HELP_LINES[i] && i < height; i++) {
        if (i == 0) attron(A_BOLD);
        safe_addstr(start_y + i, 2, HELP_LINES[i], width - 4);
        if (i == 0) attroff(A_BOLD);
    }
}

/* ============================================================
 * MAIN DRAW DISPATCH
 * ============================================================ */
static void
tui_draw (VimbanTUI *tui)
{
    AppState *s      = tui->state;
    gint      height = getmaxy(stdscr);
    gint      width  = getmaxx(stdscr);

    clear();

    /* terminal too small? */
    if (width < MIN_WIDTH || height < MIN_HEIGHT) {
        gchar *msg = g_strdup_printf(
            "Terminal too small. Need %dx%d, got %dx%d",
            MIN_WIDTH, MIN_HEIGHT, width, height);
        safe_addstr(0, 0, msg, width);
        g_free(msg);
        refresh();
        return;
    }

    draw_header(tui);

    gint content_y = 2;
    gint content_h = height - 4;

    if (s->show_help) {
        draw_help(content_y, content_h, width);
    } else if (s->current_view == VIEW_PEOPLE) {
        draw_people(tui, content_y, content_h, width);
    } else if (s->current_view == VIEW_DASHBOARD) {
        draw_dashboard(tui, content_y, content_h, width);
    } else if (s->current_view == VIEW_MENTORSHIP) {
        safe_addstr(content_y, 2, "Mentorship: use 'vimban mentor' directly.", width - 4);
    } else {
        /* TICKETS view — dispatch by layout */
        switch (s->current_layout) {
            case LAYOUT_KANBAN: draw_kanban(tui, content_y, content_h, width); break;
            case LAYOUT_LIST:   draw_list  (tui, content_y, content_h, width); break;
            case LAYOUT_SPLIT:  draw_split (tui, content_y, content_h, width); break;
            default:            break;
        }
    }

    draw_footer(tui);
    refresh();
    tui->needs_redraw = FALSE;
}

/* ============================================================
 * DIALOG HELPERS
 * ============================================================ */

/*
 * Generic list picker dialog.
 * items is NULL-terminated array of strings.
 * current is the pre-selected item (or NULL).
 * Returns newly-allocated selected string or NULL on cancel.
 */
static gchar *
dialog_select_list (const gchar *title, const gchar **items, const gchar *current)
{
    gint     n = 0;
    gint     screen_h;
    gint     screen_w;
    gint     dh, dw, sy, sx;
    WINDOW  *win;
    gint     selected;
    gint     visible;
    gint     scroll;
    gint     i;

    while (items[n]) n++;
    if (n == 0) return NULL;

    screen_h = getmaxy(stdscr);
    screen_w = getmaxx(stdscr);

    dh  = MIN(n + 5, screen_h - 4);
    dw  = MIN(40, screen_w - 4);
    sy  = (screen_h - dh) / 2;
    sx  = (screen_w - dw) / 2;

    win = newwin(dh, dw, sy, sx);
    keypad(win, TRUE);

    selected = 0;
    if (current) {
        for (i = 0; i < n; i++) {
            if (g_strcmp0(items[i], current) == 0) { selected = i; break; }
        }
    }

    visible = dh - 4;
    scroll  = 0;

    while (TRUE) {
        gint key;
        werase(win);
        wattron(win, COLOR_PAIR(PAIR_BORDER));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(PAIR_BORDER));

        wattron(win, A_BOLD);
        mvwaddnstr(win, 1, 2, title, dw - 4);
        wattroff(win, A_BOLD);

        for (i = 0; i < visible; i++) {
            gint   idx = scroll + i;
            gint   ry  = 3 + i;
            gchar *line;
            if (idx >= n) break;

            if (idx == selected) {
                wattron(win, COLOR_PAIR(PAIR_SELECTED));
                line = g_strdup_printf("> %-*.*s", dw - 6, dw - 6, items[idx]);
                mvwaddnstr(win, ry, 2, line, dw - 4);
                g_free(line);
                wattroff(win, COLOR_PAIR(PAIR_SELECTED));
            } else {
                gchar mark = (current && g_strcmp0(items[idx], current) == 0) ? '*' : ' ';
                line = g_strdup_printf("%c %s", mark, items[idx]);
                mvwaddnstr(win, ry, 2, line, dw - 4);
                g_free(line);
            }
        }

        wrefresh(win);

        key = wgetch(win);
        if (key == 'q' || key == 27) {
            delwin(win);
            return NULL;
        } else if (key == 'j' || key == KEY_DOWN) {
            if (selected < n - 1) {
                selected++;
                if (selected >= scroll + visible) scroll++;
            }
        } else if (key == 'k' || key == KEY_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scroll) scroll--;
            }
        } else if (key == '\n' || key == KEY_ENTER) {
            gchar *result = g_strdup(items[selected]);
            delwin(win);
            return result;
        }
    }
}

/*
 * Text input dialog.
 * Returns newly-allocated string or NULL on cancel.
 */
static gchar *
dialog_text_input (const gchar *prompt, const gchar *initial)
{
    gint screen_h = getmaxy(stdscr);
    gint screen_w = getmaxx(stdscr);

    gint dh = 5;
    gint dw = MIN(60, screen_w - 4);
    gint sy = (screen_h - dh) / 2;
    gint sx = (screen_w - dw) / 2;

    WINDOW *win = newwin(dh, dw, sy, sx);
    keypad(win, TRUE);
    curs_set(1);
    wtimeout(win, -1); /* blocking for dialog */

    gchar  buf[512] = {0};
    if (initial)
        g_strlcpy(buf, initial, sizeof(buf));
    gint cursor_pos = (gint)strlen(buf);

    while (TRUE) {
        werase(win);
        wattron(win, COLOR_PAIR(PAIR_BORDER));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(PAIR_BORDER));

        mvwaddnstr(win, 1, 2, prompt, dw - 4);

        gint iw = dw - 6;
        gint start_char = cursor_pos > iw ? cursor_pos - iw : 0;
        mvwaddnstr(win, 2, 2, buf + start_char, iw);
        wmove(win, 2, 2 + MIN(cursor_pos, iw));
        wrefresh(win);

        gint key = wgetch(win);
        if (key == 27) {
            curs_set(0);
            delwin(win);
            return NULL;
        } else if (key == '\n' || key == KEY_ENTER) {
            curs_set(0);
            delwin(win);
            if (!buf[0]) return NULL;
            return g_strdup(buf);
        } else if (key == KEY_BACKSPACE || key == 127) {
            if (cursor_pos > 0) {
                memmove(buf + cursor_pos - 1, buf + cursor_pos,
                        strlen(buf) - cursor_pos + 1);
                cursor_pos--;
            }
        } else if (key == KEY_DC) {
            gint len = (gint)strlen(buf);
            if (cursor_pos < len)
                memmove(buf + cursor_pos, buf + cursor_pos + 1, len - cursor_pos);
        } else if (key == KEY_LEFT) {
            if (cursor_pos > 0) cursor_pos--;
        } else if (key == KEY_RIGHT) {
            gint len = (gint)strlen(buf);
            if (cursor_pos < len) cursor_pos++;
        } else if (key == KEY_HOME) {
            cursor_pos = 0;
        } else if (key == KEY_END) {
            cursor_pos = (gint)strlen(buf);
        } else if (key >= 32 && key <= 126) {
            gint len = (gint)strlen(buf);
            if (len < (gint)sizeof(buf) - 2) {
                memmove(buf + cursor_pos + 1, buf + cursor_pos, len - cursor_pos + 1);
                buf[cursor_pos] = (char)key;
                cursor_pos++;
            }
        }
    }
}

/*
 * Confirm dialog. Returns TRUE if user presses y/Y.
 */
static gboolean
dialog_confirm (const gchar *title, const gchar *message, const gchar *item_title)
{
    gint screen_h = getmaxy(stdscr);
    gint screen_w = getmaxx(stdscr);

    gint dh = item_title ? 9 : 7;
    gint dw = MIN(52, screen_w - 4);
    gint sy = (screen_h - dh) / 2;
    gint sx = (screen_w - dw) / 2;

    WINDOW *win = newwin(dh, dw, sy, sx);
    keypad(win, TRUE);
    wtimeout(win, -1);

    werase(win);
    wattron(win, COLOR_PAIR(PAIR_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(PAIR_BORDER));

    wattron(win, A_BOLD);
    mvwaddnstr(win, 1, 2, title, dw - 4);
    wattroff(win, A_BOLD);

    gint row = 3;
    if (item_title) {
        gchar *it = g_strdup_printf("\"%.*s\"", dw - 6, item_title);
        mvwaddnstr(win, row, 2, it, dw - 4);
        g_free(it);
        row += 2;
    }

    mvwaddnstr(win, row,     2, message,        dw - 4);
    mvwaddnstr(win, row + 2, 2, "[y] Yes  [n] No", dw - 4);
    wrefresh(win);

    gboolean result = FALSE;
    while (TRUE) {
        gint key = wgetch(win);
        if (key == 'y' || key == 'Y') { result = TRUE;  break; }
        if (key == 'n' || key == 'N' || key == 27) { result = FALSE; break; }
    }

    delwin(win);
    return result;
}

/* info dialog */
static void
dialog_info (const gchar *title, const gchar **lines, gint nlines)
{
    gint     screen_h;
    gint     screen_w;
    gint     max_llen;
    gint     dw, content_h, dh, sy, sx;
    WINDOW  *win;
    gint     scroll;
    gint     i;

    screen_h  = getmaxy(stdscr);
    screen_w  = getmaxx(stdscr);
    max_llen  = (gint)strlen(title);

    for (i = 0; i < nlines; i++) {
        gint ll = lines[i] ? (gint)strlen(lines[i]) : 0;
        if (ll > max_llen) max_llen = ll;
    }

    dw        = MIN(max_llen + 6, screen_w - 4);
    content_h = MIN(nlines, screen_h - 10);
    dh        = content_h + 6;
    sy        = (screen_h - dh) / 2;
    sx        = (screen_w - dw) / 2;

    win    = newwin(dh, dw, sy, sx);
    keypad(win, TRUE);
    scroll = 0;
    wtimeout(win, -1);

    while (TRUE) {
        const gchar *help;
        gint         key;
        werase(win);
        wattron(win, COLOR_PAIR(PAIR_BORDER));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(PAIR_BORDER));

        wattron(win, A_BOLD);
        mvwaddnstr(win, 1, 2, title, dw - 4);
        wattroff(win, A_BOLD);

        for (i = 0; i < content_h; i++) {
            gint         idx   = scroll + i;
            const gchar *line;
            const gchar *colon;
            if (idx >= nlines) break;
            line  = lines[idx] ? lines[idx] : "";
            colon = strchr(line, ':');

            /* highlight field names */
            if (colon) {
                gint   flen  = (gint)(colon - line) + 1;
                gchar *field = g_strndup(line, flen);
                wattron(win, COLOR_PAIR(PAIR_HEADER));
                mvwaddnstr(win, 3 + i, 2, field, dw - 4);
                wattroff(win, COLOR_PAIR(PAIR_HEADER));
                mvwaddnstr(win, 3 + i, 2 + flen, colon + 1, dw - 4 - flen);
                g_free(field);
            } else {
                mvwaddnstr(win, 3 + i, 2, line, dw - 4);
            }
        }

        help = nlines > content_h
            ? "j/k:scroll  q/Enter:close"
            : "q/Enter:close";
        mvwaddnstr(win, dh - 2, 2, help, dw - 4);

        wrefresh(win);

        key = wgetch(win);
        if (key == 'q' || key == 27 || key == '\n' || key == KEY_ENTER)
            break;
        if ((key == 'j' || key == KEY_DOWN) && scroll < nlines - content_h)
            scroll++;
        if ((key == 'k' || key == KEY_UP) && scroll > 0)
            scroll--;
        if (key == 4) scroll = MIN(scroll + content_h/2, MAX(0, nlines - content_h));
        if (key == 21) scroll = MAX(scroll - content_h/2, 0);
        if (key == 'g') scroll = 0;
        if (key == 'G') scroll = MAX(0, nlines - content_h);
    }

    delwin(win);
}

/* ============================================================
 * INPUT SANITIZER
 * ============================================================ */

/*
 * strip_control_chars:
 * @str: (nullable): string to sanitise in-place
 *
 * Removes control characters (bytes < 32) except TAB from @str.
 * Prevents injection when dialog input is passed to CLI arguments.
 */
static void
strip_control_chars (gchar *str)
{
    gchar *src, *dst;

    if (!str) return;
    for (src = dst = str; *src; src++) {
        if ((guchar)*src >= 32 || *src == '\t')
            *dst++ = *src;
    }
    *dst = '\0';
}

/* ============================================================
 * ACTIONS
 * ============================================================ */
static void
action_edit (VimbanTUI *tui)
{
    /*
     * Open selected item's file in $EDITOR.
     * Suspend ncurses, run editor, restore.
     */
    const gchar *filepath = NULL;

    if (tui->state->current_view == VIEW_PEOPLE) {
        TUIPerson *p = tui_get_selected_person(tui);
        if (!p) { tui_set_status(tui, "No person selected", TRUE); return; }
        filepath = p->filepath;
    } else {
        TUITicket *t = tui_get_selected_ticket(tui);
        if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }
        filepath = t->filepath;
    }

    if (!filepath || !filepath[0]) {
        tui_set_status(tui, "No file path available", TRUE);
        return;
    }

    const gchar *editor = g_getenv("EDITOR");
    if (!editor || !editor[0]) editor = "nvim";

    tui_suspend_ncurses();

    gchar *argv[] = { (gchar *)editor, (gchar *)filepath, NULL };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    tui_restore_ncurses();
    tui_load_data(tui);
    tui_set_status(tui, "Edited. Data refreshed.", FALSE);
}

static void
action_new (VimbanTUI *tui)
{
    /* Select type */
    g_autoptr(GError) err = NULL;
    gchar *type_sel = dialog_select_list("Create New Item",
        (const gchar **)CREATE_TYPES, NULL);
    if (!type_sel) return;

    gchar *prompt  = g_strdup_printf("New %s title:", type_sel);
    gchar *title   = dialog_text_input(prompt, "");
    g_free(prompt);

    if (!title) { g_free(type_sel); return; }
    strip_control_chars(title);

    /* call vimban create */
    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "create", type_sel, title, NULL
    };
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    if (exit_status == 0) {
        gchar *msg = g_strdup_printf("Created %s: %s", type_sel, title);
        tui_load_data(tui);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
    } else {
        tui_set_status(tui, "Error creating item", TRUE);
    }

    g_free(type_sel);
    g_free(title);
}

static void
action_move (VimbanTUI *tui)
{
    TUITicket *t = tui_get_selected_ticket(tui);
    if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }

    gchar *new_status = dialog_select_list("Move to Status",
        (const gchar **)STATUSES, t->status);
    if (!new_status || g_strcmp0(new_status, t->status) == 0) {
        g_free(new_status);
        return;
    }

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "move", t->id, new_status, NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    if (exit_status == 0) {
        gchar *id_copy = g_strdup(t->id);
        gchar *msg = g_strdup_printf("Moved %s to %s", id_copy, new_status);
        tui_load_data(tui);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
        g_free(id_copy);
    } else {
        tui_set_status(tui, "Error moving ticket", TRUE);
    }
    g_free(new_status);
}

static void
action_move_status_delta (VimbanTUI *tui, gint delta)
{
    /* Move selected ticket forward/backward in status order (H/L keys). */
    TUITicket *t = tui_get_selected_ticket(tui);
    gint       idx = -1;
    gint       n   = 0;
    gint       new_idx;
    gint       i;

    if (!t) return;

    for (i = 0; STATUSES[i]; i++) {
        if (g_strcmp0(STATUSES[i], t->status) == 0) { idx = i; break; }
    }
    if (idx < 0) return;

    while (STATUSES[n]) n++;
    new_idx = CLAMP(idx + delta, 0, n - 1);
    if (new_idx == idx) return;

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "move", t->id, (gchar *)STATUSES[new_idx], NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    if (exit_status == 0) {
        gchar *id_copy = g_strdup(t->id);
        gchar *msg = g_strdup_printf("Moved %s to %s", id_copy, STATUSES[new_idx]);
        tui_load_data(tui);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
        g_free(id_copy);
    } else {
        tui_set_status(tui, "Error moving ticket", TRUE);
    }
}

static void
action_delete (VimbanTUI *tui)
{
    const gchar *filepath = NULL;
    const gchar *item_id  = NULL;
    const gchar *label    = NULL;

    if (tui->state->current_view == VIEW_PEOPLE) {
        TUIPerson *p = tui_get_selected_person(tui);
        if (!p) { tui_set_status(tui, "No person selected", TRUE); return; }
        filepath = p->filepath;
        label    = p->name;
        item_id  = p->name;
    } else {
        TUITicket *t = tui_get_selected_ticket(tui);
        if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }
        filepath = t->filepath;
        label    = t->title;
        item_id  = t->id;
    }

    gchar *confirm_title = g_strdup_printf("Delete %s?", item_id);
    gboolean ok = dialog_confirm(confirm_title, "This cannot be undone.", label);
    g_free(confirm_title);

    if (!ok) { tui_set_status(tui, "Delete cancelled", FALSE); return; }

    {
        /* copy strings before tui_load_data frees the structs */
        gchar *id_copy   = g_strdup(item_id);
        gchar *path_copy = g_strdup(filepath);

        if (g_remove(path_copy) == 0) {
            gchar *msg = g_strdup_printf("Deleted %s", id_copy);
            tui_load_data(tui);
            tui_set_status(tui, msg, FALSE);
            g_free(msg);
        } else {
            tui_set_status(tui, "Failed to delete", TRUE);
        }
        g_free(id_copy);
        g_free(path_copy);
    }
}

static void
action_priority (VimbanTUI *tui)
{
    TUITicket *t = tui_get_selected_ticket(tui);
    if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }

    gchar *new_pri = dialog_select_list("Set Priority",
        (const gchar **)PRIORITIES, t->priority);
    if (!new_pri || g_strcmp0(new_pri, t->priority) == 0) {
        g_free(new_pri);
        return;
    }

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "edit", t->id, "--priority", new_pri, NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    if (exit_status == 0) {
        gchar *id_copy = g_strdup(t->id);
        gchar *msg = g_strdup_printf("Set %s priority to %s", id_copy, new_pri);
        tui_load_data(tui);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
        g_free(id_copy);
    } else {
        tui_set_status(tui, "Error changing priority", TRUE);
    }
    g_free(new_pri);
}

static void
action_comment (VimbanTUI *tui)
{
    const gchar *item_id = NULL;
    if (tui->state->current_view == VIEW_PEOPLE) {
        TUIPerson *p = tui_get_selected_person(tui);
        if (!p) { tui_set_status(tui, "No person selected", TRUE); return; }
        item_id = p->name;
    } else {
        TUITicket *t = tui_get_selected_ticket(tui);
        if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }
        item_id = t->id;
    }

    gchar *comment = dialog_text_input("Comment:", "");
    if (!comment) return;
    strip_control_chars(comment);

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "comment", (gchar *)item_id, comment,
        "-u", "zach_podbielniak", NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    if (exit_status == 0) {
        gchar *msg = g_strdup_printf("Added comment to %s", item_id);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
    } else {
        tui_set_status(tui, "Error adding comment", TRUE);
    }
    g_free(comment);
}

static void
action_comment_reply (VimbanTUI *tui)
{
    /*
     * Add a threaded reply to an existing comment.
     * Fetches comments via "vimban comment <id> --print all",
     * shows a selection dialog, then prompts for reply text.
     */
    const gchar *item_id = NULL;
    g_autofree gchar *cmd_out = NULL;
    GPtrArray  *display_items;
    GArray     *comment_nums;
    gchar     **lines;
    gint        i;
    gint        n_lines;

    if (tui->state->current_view == VIEW_PEOPLE) {
        TUIPerson *p = tui_get_selected_person(tui);
        if (!p) { tui_set_status(tui, "No person selected", TRUE); return; }
        item_id = p->name;
    } else {
        TUITicket *t = tui_get_selected_ticket(tui);
        if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }
        item_id = t->id;
    }

    /* fetch comments via vimban CLI */
    {
        gchar *argv[] = {
            "vimban", "--no-color", "--directory", tui->directory,
            "comment", (gchar *)item_id, "--print", "all", NULL
        };
        g_autoptr(GError) err = NULL;
        gchar *stdout_buf = NULL;
        gint   exit_status = 0;

        g_spawn_sync(NULL, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &stdout_buf, NULL, &exit_status, &err);

        if (exit_status != 0 || !stdout_buf || !stdout_buf[0]) {
            g_free(stdout_buf);
            tui_set_status(tui, "No comments to reply to", TRUE);
            return;
        }
        cmd_out = stdout_buf;
    }

    /* parse comment output: "#N (timestamp)" lines followed by text */
    lines = g_strsplit(cmd_out, "\n", -1);
    n_lines = (gint)g_strv_length(lines);

    display_items = g_ptr_array_new_with_free_func(g_free);
    comment_nums  = g_array_new(FALSE, FALSE, sizeof(gint));

    for (i = 0; i < n_lines; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (line[0] == '#' && g_ascii_isdigit(line[1])) {
            gint num = (gint)g_ascii_strtoll(line + 1, NULL, 10);
            gchar *text = "";
            gchar *preview;
            gchar *label;

            if (i + 1 < n_lines)
                text = g_strstrip(lines[i + 1]);

            /* truncate preview to 40 chars */
            if ((gint)strlen(text) > 40) {
                preview = g_strndup(text, 40);
                label = g_strdup_printf("#%d: %s...", num, preview);
                g_free(preview);
            } else {
                label = g_strdup_printf("#%d: %s", num, text);
            }

            g_ptr_array_add(display_items, label);
            g_array_append_val(comment_nums, num);
            i++; /* skip text line */
        }
    }

    g_strfreev(lines);

    if (display_items->len == 0) {
        g_ptr_array_unref(display_items);
        g_array_unref(comment_nums);
        tui_set_status(tui, "No comments to reply to", TRUE);
        return;
    }

    /* show selection dialog */
    {
        const gchar **items_arr;
        gchar *selected;
        gint   sel_idx;
        guint  j;

        g_ptr_array_add(display_items, NULL);
        items_arr = (const gchar **)display_items->pdata;

        selected = dialog_select_list("Reply to Comment", items_arr, NULL);
        if (!selected) {
            g_ptr_array_unref(display_items);
            g_array_unref(comment_nums);
            return;
        }

        /* find which index was selected */
        sel_idx = -1;
        for (j = 0; j < display_items->len - 1; j++) {
            if (g_strcmp0(selected, (gchar *)display_items->pdata[j]) == 0) {
                sel_idx = (gint)j;
                break;
            }
        }
        g_free(selected);

        if (sel_idx < 0 || sel_idx >= (gint)comment_nums->len) {
            g_ptr_array_unref(display_items);
            g_array_unref(comment_nums);
            return;
        }

        {
            gint   reply_to = g_array_index(comment_nums, gint, sel_idx);
            gchar *prompt;
            gchar *reply_text;

            g_ptr_array_unref(display_items);
            g_array_unref(comment_nums);

            prompt = g_strdup_printf("Reply to #%d:", reply_to);
            reply_text = dialog_text_input(prompt, "");
            g_free(prompt);

            if (!reply_text) return;
            strip_control_chars(reply_text);

            {
                g_autofree gchar *reply_to_str = g_strdup_printf("%d", reply_to);
                gchar *argv[] = {
                    "vimban", "--directory", tui->directory,
                    "comment", (gchar *)item_id, reply_text,
                    "--reply-to", reply_to_str,
                    "-u", "zach_podbielniak", NULL
                };
                g_autoptr(GError) err = NULL;
                gint exit_status = 0;

                g_spawn_sync(NULL, argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                                                 | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &exit_status, &err);

                if (exit_status == 0) {
                    gchar *msg = g_strdup_printf("Replied to comment #%d", reply_to);
                    tui_set_status(tui, msg, FALSE);
                    g_free(msg);
                } else {
                    tui_set_status(tui, "Error adding reply", TRUE);
                }
            }
            g_free(reply_text);
        }
    }
}

static void
action_info (VimbanTUI *tui)
{
    /*
     * Build an array of info lines from the selected ticket or person
     * and display in the scrollable info dialog.
     */
    GPtrArray *lines_arr = g_ptr_array_new_with_free_func(g_free);
    gchar      title_buf[128] = {0};

    if (tui->state->current_view == VIEW_PEOPLE) {
        TUIPerson *p = tui_get_selected_person(tui);
        if (!p) { tui_set_status(tui, "No person selected", TRUE); return; }

        g_snprintf(title_buf, sizeof(title_buf), "Person: %s", p->name ? p->name : "");

#define ADD(s) g_ptr_array_add(lines_arr, g_strdup(s))
#define ADDF(fmt, ...) g_ptr_array_add(lines_arr, g_strdup_printf(fmt, ##__VA_ARGS__))

        if (p->name  && p->name[0])  ADDF("Name: %s",  p->name);
        if (p->email && p->email[0]) ADDF("Email: %s", p->email);
        if (p->role  && p->role[0])  ADDF("Role: %s",  p->role);
        if (p->team  && p->team[0])  ADDF("Team: %s",  p->team);
        ADD("");
        if (p->filepath && p->filepath[0]) ADDF("Path: %s", p->filepath);

    } else {
        TUITicket *t = tui_get_selected_ticket(tui);
        if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }

        g_snprintf(title_buf, sizeof(title_buf), "Ticket: %s", t->id ? t->id : "");

        if (t->id       && t->id[0])       ADDF("ID: %s",       t->id);
        if (t->title    && t->title[0])    ADDF("Title: %s",    t->title);
        if (t->type     && t->type[0])     ADDF("Type: %s",     t->type);
        if (t->status   && t->status[0])   ADDF("Status: %s",   t->status);
        if (t->priority && t->priority[0]) ADDF("Priority: %s", t->priority);
        if (t->assignee && t->assignee[0]) ADDF("Assignee: %s", t->assignee);
        if (t->due_date && t->due_date[0]) ADDF("Due: %s",      t->due_date);
        if (t->tags     && t->tags[0])     ADDF("Tags: %s",     t->tags);
        if (t->project  && t->project[0])  ADDF("Project: %s",  t->project);
        if (t->progress > 0)               ADDF("Progress: %d%%", t->progress);
        if (t->created    && t->created[0])    ADDF("Created: %s",    t->created);
        if (t->updated    && t->updated[0])    ADDF("Updated: %s",    t->updated);
        if (t->issue_link && t->issue_link[0]) ADDF("Issue Link: %s", t->issue_link);
        if (t->member_of  && t->member_of[0])  ADDF("Member Of: %s",  t->member_of);
        ADD("");
        /* Show relative path */
        if (t->filepath && t->filepath[0]) {
            if (tui->directory && g_str_has_prefix(t->filepath, tui->directory)) {
                const gchar *rel = t->filepath + strlen(tui->directory);
                if (*rel == '/') rel++;
                ADDF("Path: %s", rel);
            } else {
                ADDF("Path: %s", t->filepath);
            }
        }

#undef ADD
#undef ADDF
    }

    /* NULL-terminate for dialog */
    g_ptr_array_add(lines_arr, NULL);

    dialog_info(title_buf,
        (const gchar **)lines_arr->pdata,
        (gint)lines_arr->len - 1);

    g_ptr_array_unref(lines_arr);
}

static void
action_edit_metadata (VimbanTUI *tui)
{
    TUITicket *t = tui_get_selected_ticket(tui);
    if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }

    static const gchar *fields[] = {
        "assignee", "status", "priority",
        "add-tag", "remove-tag", "progress", "due", "clear",
        "link", "unlink", NULL
    };

    gchar *field = dialog_select_list("Edit Field", fields, NULL);
    if (!field) return;

    gchar *value   = NULL;
    gchar *arg_key = NULL;

    if (g_strcmp0(field, "assignee") == 0) {
        value   = dialog_text_input("Assignee (person ref):", t->assignee ? t->assignee : "");
        arg_key = "--assignee";
    } else if (g_strcmp0(field, "status") == 0) {
        value   = dialog_select_list("Status", (const gchar **)STATUSES, t->status);
        arg_key = "--status";
    } else if (g_strcmp0(field, "priority") == 0) {
        value   = dialog_select_list("Priority", (const gchar **)PRIORITIES, t->priority);
        arg_key = "--priority";
    } else if (g_strcmp0(field, "add-tag") == 0) {
        value   = dialog_text_input("Tag to add:", "");
        arg_key = "--add-tag";
    } else if (g_strcmp0(field, "remove-tag") == 0) {
        if (t->tags && t->tags[0]) {
            gchar **tag_parts = g_strsplit(t->tags, ",", -1);
            gint    ntags = 0;
            gint    ti;
            while (tag_parts[ntags]) { g_strstrip(tag_parts[ntags]); ntags++; }
            {
                const gchar **tag_list = g_new0(const gchar *, ntags + 1);
                for (ti = 0; ti < ntags; ti++) tag_list[ti] = tag_parts[ti];
                tag_list[ntags] = NULL;
                value = dialog_select_list("Remove Tag", tag_list, NULL);
                g_free(tag_list);
            }
            g_strfreev(tag_parts);
        } else {
            tui_set_status(tui, "No tags to remove", TRUE);
            g_free(field);
            return;
        }
        arg_key = "--remove-tag";
    } else if (g_strcmp0(field, "progress") == 0) {
        gchar *cur = g_strdup_printf("%d", t->progress);
        value   = dialog_text_input("Progress (0-100):", cur);
        g_free(cur);
        arg_key = "--progress";
    } else if (g_strcmp0(field, "due") == 0) {
        value   = dialog_text_input("Due date (YYYY-MM-DD or +7d):",
                    t->due_date ? t->due_date : "");
        arg_key = "--due";
    } else if (g_strcmp0(field, "clear") == 0) {
        static const gchar *clearable[] = { "assignee", "due_date", "progress", "effort", NULL };
        value   = dialog_select_list("Clear Field", clearable, NULL);
        arg_key = "--clear";
    } else if (g_strcmp0(field, "link") == 0) {
        /* Link: select relation, then fzf for target, then vimban link */
        gchar *relation = dialog_select_list("Link Relation",
            (const gchar **)LINK_RELATIONS, NULL);
        if (!relation) { g_free(field); return; }

        tui_suspend_ncurses();
        {
            g_autofree gchar *id_copy = g_strdup(t->id);
            g_autofree gchar *fzf_prompt = g_strdup_printf("Link %s %s: ", id_copy, relation);
            /* pipe ticket list into fzf for target selection */
            g_autofree gchar *fzf_cmd = g_strdup_printf(
                "vimban --directory '%s' list -f plain --no-color -q 2>/dev/null | fzf --prompt='%s'",
                tui->directory, fzf_prompt);
            gchar *fzf_out = NULL;
            gint   fzf_rc  = 0;
            gchar *fzf_argv[] = { "bash", "-c", fzf_cmd, NULL };

            g_spawn_sync(NULL, fzf_argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                         NULL, NULL, &fzf_out, NULL, &fzf_rc, NULL);
            tui_restore_ncurses();

            if (fzf_rc == 0 && fzf_out && fzf_out[0]) {
                /* extract ticket ID (first whitespace-delimited token) */
                g_strstrip(fzf_out);
                gchar *space = strchr(fzf_out, ' ');
                if (space) *space = '\0';

                gchar *link_argv[] = {
                    "vimban", "--directory", tui->directory,
                    "link", id_copy, relation, fzf_out,
                    "--bidirectional", NULL
                };
                g_autoptr(GError) lerr = NULL;
                gint link_rc = 0;
                g_spawn_sync(NULL, link_argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &link_rc, &lerr);
                if (link_rc == 0) {
                    gchar *msg = g_strdup_printf("Linked %s %s %s", id_copy, relation, fzf_out);
                    tui_load_data(tui);
                    tui_set_status(tui, msg, FALSE);
                    g_free(msg);
                } else {
                    tui_set_status(tui, "Error linking tickets", TRUE);
                }
            } else {
                tui_set_status(tui, "Link cancelled", FALSE);
            }
            g_free(fzf_out);
        }
        g_free(relation);
        g_free(field);
        return;
    } else if (g_strcmp0(field, "unlink") == 0) {
        /* Unlink: select relation, fzf for target, vimban link --remove */
        gchar *relation = dialog_select_list("Unlink Relation",
            (const gchar **)LINK_RELATIONS, NULL);
        if (!relation) { g_free(field); return; }

        tui_suspend_ncurses();
        {
            g_autofree gchar *id_copy = g_strdup(t->id);
            g_autofree gchar *fzf_prompt = g_strdup_printf("Unlink %s %s: ", id_copy, relation);
            g_autofree gchar *fzf_cmd = g_strdup_printf(
                "vimban --directory '%s' list -f plain --no-color -q 2>/dev/null | fzf --prompt='%s'",
                tui->directory, fzf_prompt);
            gchar *fzf_out = NULL;
            gint   fzf_rc  = 0;
            gchar *fzf_argv[] = { "bash", "-c", fzf_cmd, NULL };

            g_spawn_sync(NULL, fzf_argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                         NULL, NULL, &fzf_out, NULL, &fzf_rc, NULL);
            tui_restore_ncurses();

            if (fzf_rc == 0 && fzf_out && fzf_out[0]) {
                g_strstrip(fzf_out);
                gchar *space = strchr(fzf_out, ' ');
                if (space) *space = '\0';

                gchar *unlink_argv[] = {
                    "vimban", "--directory", tui->directory,
                    "link", id_copy, relation, fzf_out,
                    "--remove", NULL
                };
                g_autoptr(GError) lerr = NULL;
                gint unlink_rc = 0;
                g_spawn_sync(NULL, unlink_argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &unlink_rc, &lerr);
                if (unlink_rc == 0) {
                    gchar *msg = g_strdup_printf("Unlinked %s %s %s", id_copy, relation, fzf_out);
                    tui_load_data(tui);
                    tui_set_status(tui, msg, FALSE);
                    g_free(msg);
                } else {
                    tui_set_status(tui, "Error unlinking tickets", TRUE);
                }
            } else {
                tui_set_status(tui, "Unlink cancelled", FALSE);
            }
            g_free(fzf_out);
        }
        g_free(relation);
        g_free(field);
        return;
    }

    if (!value || !arg_key) { g_free(field); g_free(value); return; }
    strip_control_chars(value);

    {
        gchar *argv[] = {
            "vimban", "--directory", tui->directory,
            "edit", t->id, (gchar *)arg_key, value, NULL
        };
        g_autoptr(GError) err = NULL;
        gint exit_status = 0;
        g_spawn_sync(NULL, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, &exit_status, &err);

        if (exit_status == 0) {
            gchar *id_copy = g_strdup(t->id);
            gchar *msg = g_strdup_printf("Updated %s: %s", id_copy, field);
            tui_load_data(tui);
            tui_set_status(tui, msg, FALSE);
            g_free(msg);
            g_free(id_copy);
        } else {
            tui_set_status(tui, "Error updating metadata", TRUE);
        }
    }

    g_free(field);
    g_free(value);
}

static void
action_archive (VimbanTUI *tui)
{
    TUITicket *t = tui_get_selected_ticket(tui);
    if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }

    if (g_strcmp0(t->status, "done") != 0 && g_strcmp0(t->status, "cancelled") != 0) {
        tui_set_status(tui, "Archive requires done or cancelled status", TRUE);
        return;
    }

    tui_suspend_ncurses();

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "archive", t->id, NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    tui_restore_ncurses();

    if (exit_status == 0) {
        gchar *id_copy = g_strdup(t->id);
        gchar *msg;
        tui_load_data(tui);
        msg = g_strdup_printf("Archived %s", id_copy);
        tui_set_status(tui, msg, FALSE);
        g_free(msg);
        g_free(id_copy);
    } else {
        tui_load_data(tui);
        tui_set_status(tui, "Archive failed", TRUE);
    }
}

static void
action_move_location (VimbanTUI *tui)
{
    /* Use vimban move-location which internally calls fzf. */
    g_autofree gchar *item_id = NULL;
    TUITicket *t = tui_get_selected_ticket(tui);
    if (!t) { tui_set_status(tui, "No ticket selected", TRUE); return; }
    item_id = g_strdup(t->id);

    tui_suspend_ncurses();

    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "move-location", (gchar *)item_id, NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;
    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    tui_restore_ncurses();
    tui_load_data(tui);

    if (exit_status == 0)
        tui_set_status(tui, "Moved", FALSE);
    else
        tui_set_status(tui, "Move cancelled or failed", TRUE);
}

static void
action_commit (VimbanTUI *tui)
{
    /*
     * Run vimban commit with terminal I/O, show output,
     * then wait for Enter before restoring ncurses.
     */
    gchar *argv[] = {
        "vimban", "--directory", tui->directory,
        "commit", NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;

    tui_suspend_ncurses();

    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                 NULL, NULL, NULL, NULL, &exit_status, &err);

    /* show result and wait for user to press Enter */
    if (exit_status == 0)
        g_print("\nCommit successful.\n");
    else
        g_print("\nCommit failed.\n");

    g_print("Press Enter to continue...");
    fflush(stdout);
    {
        gint ch;
        while ((ch = fgetc(stdin)) != '\n' && ch != EOF)
            ;
    }

    tui_restore_ncurses();
    tui_load_data(tui);

    if (exit_status == 0)
        tui_set_status(tui, "Committed successfully", FALSE);
    else
        tui_set_status(tui, "Commit failed", TRUE);
}

/* ============================================================
 * KEY HANDLER
 * ============================================================ */
static gboolean
tui_handle_key (VimbanTUI *tui, gint key)
{
    /*
     * Process a single keypress.
     * Returns TRUE if the application should quit.
     */
    AppState *s = tui->state;

    /* close help with any key */
    if (s->show_help) {
        s->show_help = FALSE;
        tui->needs_redraw = TRUE;
        return FALSE;
    }

    /* compound motion: 'gg' → go to top */
    if (s->pending_motion == 'g') {
        s->pending_motion = 0;
        if (key == 'g') {
            s->selected_row  = 0;
            s->scroll_offset = 0;
            if (s->current_layout == LAYOUT_KANBAN)
                kanban_col_scroll_set(tui, s->selected_col, 0);
            tui_clamp_selection(tui);
        }
        tui->needs_redraw = TRUE;
        return FALSE;
    }

    switch (key) {
    /* ---- quit ---- */
    case 'q':
        return TRUE;

    /* ---- help ---- */
    case '?':
        s->show_help = !s->show_help;
        break;

    /* ---- view cycling ---- */
    case 't':
        s->current_view = (ViewType)((s->current_view + 1) % VIEW_COUNT);
        s->selected_row = 0;
        s->selected_col = 0;
        s->scroll_offset = 0;
        break;

    case 'T':
        s->current_view = (ViewType)((s->current_view + VIEW_COUNT - 1) % VIEW_COUNT);
        s->selected_row = 0;
        s->selected_col = 0;
        s->scroll_offset = 0;
        break;

    /* ---- layout cycling (TAB) ---- */
    case '\t':
        s->current_layout = (LayoutType)((s->current_layout + 1) % LAYOUT_COUNT);
        tui_clamp_selection(tui);
        break;

    /* ---- navigation ---- */
    case 'j':
    case KEY_DOWN:
        s->selected_row++;
        s->detail_scroll = 0;
        tui_clamp_selection(tui);
        break;

    case 'k':
    case KEY_UP:
        s->selected_row--;
        if (s->selected_row < 0) s->selected_row = 0;
        s->detail_scroll = 0;
        tui_clamp_selection(tui);
        break;

    case 'h':
    case KEY_LEFT:
        if (s->current_layout == LAYOUT_KANBAN) {
            s->selected_col--;
            if (s->selected_col < 0) s->selected_col = 0;
            s->selected_row = 0;
            s->detail_scroll = 0;
            tui_clamp_selection(tui);
        }
        break;

    case 'l':
    case KEY_RIGHT:
        if (s->current_layout == LAYOUT_KANBAN) {
            s->selected_col++;
            s->selected_row = 0;
            s->detail_scroll = 0;
            tui_clamp_selection(tui);
        }
        break;

    case 'H':
        /* move ticket status backward (kanban only) */
        if (s->current_layout == LAYOUT_KANBAN)
            action_move_status_delta(tui, -1);
        break;

    case 'L':
        /* move ticket status forward (kanban only) */
        if (s->current_layout == LAYOUT_KANBAN)
            action_move_status_delta(tui, +1);
        break;

    case 'g':
        s->pending_motion = 'g';
        break;

    case 'G': {
        /* go to bottom */
        gint max_row = 0;
        if (s->current_view == VIEW_PEOPLE) {
            max_row = s->people ? (gint)s->people->len - 1 : 0;
        } else if (s->current_layout == LAYOUT_KANBAN) {
            GPtrArray *col = kanban_col_tickets(tui, s->selected_col);
            max_row = col ? (gint)col->len - 1 : 0;
        } else {
            max_row = s->tickets ? (gint)s->tickets->len - 1 : 0;
        }
        s->selected_row = MAX(0, max_row);
        tui_clamp_selection(tui);
        break;
    }

    case 4: { /* Ctrl-D: page down */
        gint ph = (getmaxy(stdscr) - 6) / 2;
        s->selected_row += ph;
        tui_clamp_selection(tui);
        break;
    }

    case 21: { /* Ctrl-U: page up */
        gint ph = (getmaxy(stdscr) - 6) / 2;
        s->selected_row -= ph;
        if (s->selected_row < 0) s->selected_row = 0;
        tui_clamp_selection(tui);
        break;
    }

    /* ---- detail scroll (split view) ---- */
    case KEY_PPAGE:
        if (s->detail_scroll > 0) s->detail_scroll -= 10;
        break;

    case KEY_NPAGE:
        s->detail_scroll += 10;
        break;

    /* ---- actions ---- */
    case 'e':
        action_edit(tui);
        break;

    case 'E':
        if (s->current_view != VIEW_PEOPLE)
            action_edit_metadata(tui);
        break;

    case 'n':
        action_new(tui);
        break;

    case 'm':
        if (s->current_view != VIEW_PEOPLE)
            action_move(tui);
        break;

    case 'M':
        action_move_location(tui);
        break;

    case 'd':
        action_delete(tui);
        break;

    case 'A':
        if (s->current_view != VIEW_PEOPLE)
            action_archive(tui);
        break;

    case 'p':
        if (s->current_view != VIEW_PEOPLE)
            action_priority(tui);
        break;

    case 'c':
        action_comment(tui);
        break;

    case 'C':
        action_comment_reply(tui);
        break;

    case 'i':
        action_info(tui);
        break;

    case 'r':
        /* toggle markdown rendering */
        s->render_markdown = !s->render_markdown;
        tui_set_status(tui, s->render_markdown
            ? "Markdown rendering on" : "Markdown rendering off", FALSE);
        break;

    case 'R':
        tui_load_data(tui);
        tui_set_status(tui, "Data refreshed", FALSE);
        break;

    case 'S':
        action_commit(tui);
        break;

    case KEY_RESIZE:
        tui_clamp_selection(tui);
        break;

    default:
        break;
    }

    tui->needs_redraw = TRUE;
    return FALSE;
}

/* ============================================================
 * GMAINLOOP CALLBACKS
 * ============================================================ */
static gboolean
on_stdin_ready (GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
    /*
     * GIOChannel callback: STDIN has data.
     * Drain all pending key events with getch() in a tight loop.
     */
    VimbanTUI *tui = (VimbanTUI *)user_data;
    (void)channel;
    (void)condition;

    gint key;
    while ((key = getch()) != ERR) {
        gboolean quit = tui_handle_key(tui, key);
        if (quit) {
            g_main_loop_quit(tui->main_loop);
            tui->running = FALSE;
            return FALSE;
        }
    }

    if (tui->needs_redraw)
        tui_draw(tui);

    return TRUE; /* keep source alive */
}

static gboolean
on_tick (gpointer user_data)
{
    /*
     * 100ms periodic tick.
     * Redraws if flagged (catches resize events etc.).
     */
    VimbanTUI *tui = (VimbanTUI *)user_data;

    if (!tui->running)
        return FALSE;

    if (tui->needs_redraw)
        tui_draw(tui);

    return TRUE;
}

static gboolean
on_auto_refresh (gpointer user_data)
{
    VimbanTUI *tui = (VimbanTUI *)user_data;

    if (!tui->running)
        return FALSE;

    tui_load_data(tui);
    tui->needs_redraw = TRUE;

    return TRUE;
}

/* ============================================================
 * STARTUP PULL
 * ============================================================ */
static void
startup_pull (const gchar *directory)
{
    gchar *argv[] = {
        "vimban", "--directory", (gchar *)directory,
        "commit", "--pull", NULL
    };
    g_autoptr(GError) err = NULL;
    gint exit_status = 0;

    g_spawn_sync(NULL, argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, &exit_status, &err);
}

/* ============================================================
 * OPTION PARSING & main()
 * ============================================================ */
static gchar      *opt_directory = NULL;
static gchar      *opt_layout    = NULL;
static gchar      *opt_view      = NULL;
static gboolean    opt_work      = FALSE;
static gboolean    opt_personal  = FALSE;
static gboolean    opt_archived  = FALSE;
static gboolean    opt_no_pull   = FALSE;
static gboolean    opt_version   = FALSE;
static gboolean    opt_license   = FALSE;
static gint        opt_done_last = -999; /* sentinel: not set */

static GOptionEntry opt_entries[] = {
    { "directory",  'd', 0, G_OPTION_ARG_STRING,  &opt_directory, "Vimban directory",              "DIR"    },
    { "layout",      0,  0, G_OPTION_ARG_STRING,  &opt_layout,    "Initial layout (kanban/list/split)", "LAYOUT" },
    { "view",        0,  0, G_OPTION_ARG_STRING,  &opt_view,      "Initial view",                  "VIEW"   },
    { "work",        0,  0, G_OPTION_ARG_NONE,    &opt_work,      "Show work scope only",          NULL     },
    { "personal",    0,  0, G_OPTION_ARG_NONE,    &opt_personal,  "Show personal scope only",      NULL     },
    { "archived",    0,  0, G_OPTION_ARG_NONE,    &opt_archived,  "Include archived items",        NULL     },
    { "no-pull",     0,  0, G_OPTION_ARG_NONE,    &opt_no_pull,   "Skip startup git pull",         NULL     },
    { "version",    'V', 0, G_OPTION_ARG_NONE,    &opt_version,   "Show version and exit",         NULL     },
    { "license",     0,  0, G_OPTION_ARG_NONE,    &opt_license,   "Show license and exit",         NULL     },
    { NULL }
};

int
main (int argc, char **argv)
{
    /* Enable UTF-8 locale for wide-character ncurses */
    setlocale(LC_ALL, "");

    /* option parsing */
    g_autoptr(GOptionContext) ctx = g_option_context_new("- vimban TUI");
    g_option_context_add_main_entries(ctx, opt_entries, NULL);
    g_option_context_set_summary(ctx,
        "ncurses TUI for vimban ticket management\n\n"
        "Examples:\n"
        "  vimban_tui\n"
        "  vimban_tui --layout list\n"
        "  vimban_tui --view people\n"
        "  vimban_tui --work\n"
        "  vimban_tui --directory ~/Documents/notes");

    g_autoptr(GError) err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        g_printerr("Error: %s\n", err ? err->message : "unknown error");
        return 1;
    }

    if (opt_version) {
        g_print("vimban_tui %s\n", VERSION_STR);
        return 0;
    }

    if (opt_license) {
        g_print("%s\n", LICENSE_TEXT);
        return 0;
    }

    /* resolve vimban directory */
    gchar *directory;
    if (opt_directory && opt_directory[0]) {
        directory = g_strdup(opt_directory);
    } else {
        const gchar *env_dir = g_getenv("VIMBAN_DIR");
        if (env_dir && env_dir[0]) {
            directory = g_strdup(env_dir);
        } else {
            directory = g_build_filename(g_get_home_dir(), "Documents", "notes", NULL);
        }
    }

    /* startup pull */
    if (!opt_no_pull) {
        startup_pull(directory);
    }

    /* load config */
    TUIConfig *config = config_load();

    /* apply CLI overrides */
    if (opt_layout) {
        if (g_strcmp0(opt_layout, "list")  == 0) config->default_layout = LAYOUT_LIST;
        else if (g_strcmp0(opt_layout, "split") == 0) config->default_layout = LAYOUT_SPLIT;
        else config->default_layout = LAYOUT_KANBAN;
    }
    if (opt_view) {
        if      (g_strcmp0(opt_view, "people")     == 0) config->default_view = VIEW_PEOPLE;
        else if (g_strcmp0(opt_view, "dashboard")  == 0) config->default_view = VIEW_DASHBOARD;
        else if (g_strcmp0(opt_view, "mentorship") == 0) config->default_view = VIEW_MENTORSHIP;
        else config->default_view = VIEW_TICKETS;
    }
    if (opt_work)     { g_free(config->scope); config->scope = g_strdup("work");     }
    if (opt_personal) { g_free(config->scope); config->scope = g_strdup("personal"); }
    if (opt_archived) config->include_archived = TRUE;
    if (opt_done_last != -999) config->done_last_days = opt_done_last;

    /* build initial app state */
    AppState *state = g_new0(AppState, 1);
    state->current_view   = config->default_view;
    state->current_layout = config->default_layout;
    state->kanban_scroll  = g_hash_table_new(g_direct_hash, g_direct_equal);

    /* build TUI struct */
    VimbanTUI tui_obj = {0};
    VimbanTUI *tui    = &tui_obj;
    tui->config       = config;
    tui->state        = state;
    tui->directory    = directory;
    tui->needs_redraw = TRUE;
    tui->running      = TRUE;

    /* initialise ncurses */
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);   /* non-blocking getch */

    theme_init();

    /* load initial data */
    tui_load_data(tui);
    tui_draw(tui);

    /* GMainLoop + GIOChannel for stdin */
    tui->main_loop     = g_main_loop_new(NULL, FALSE);
    tui->stdin_channel = g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_encoding(tui->stdin_channel, NULL, NULL);
    g_io_channel_set_flags(tui->stdin_channel, G_IO_FLAG_NONBLOCK, NULL);

    g_io_add_watch(tui->stdin_channel, G_IO_IN, on_stdin_ready, tui);
    g_timeout_add(TICK_MS, on_tick, tui);
    g_timeout_add_seconds(AUTO_REFRESH_S, on_auto_refresh, tui);

    g_main_loop_run(tui->main_loop);

    /* cleanup */
    endwin();

    g_main_loop_unref(tui->main_loop);
    g_io_channel_unref(tui->stdin_channel);

    tui_free_data(tui);
    g_hash_table_destroy(state->kanban_scroll);
    g_free(state->status_msg);
    g_free(state);
    config_free(config);
    g_free(directory);

    return 0;
}
