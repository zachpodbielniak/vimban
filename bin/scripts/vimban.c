#!/usr/bin/env crispy
#define CRISPY_PARAMS "$(pkg-config --cflags --libs yaml-glib-1.0 json-glib-1.0 mcp-glib-1.0 libsoup-3.0) -Wno-unused-function"

/*
 * vimban.c — Markdown-native ticket/kanban management system (:wqira)
 * Copyright (C) 2025  Zach Podbielniak — AGPLv3
 *
 * C port of vimban (Python). Uses crispy for script-style execution.
 * yaml-glib for YAML frontmatter, json-glib for JSON output,
 * mcp-glib for MCP server, libsoup for remote API client.
 *
 * Usage:
 *     vimban [global-options] <command> [command-options]
 *
 * Examples:
 *     vimban init
 *     vimban create task "Fix authentication bug"
 *     vimban list --status in_progress --mine
 *     vimban move PROJ-42 done --resolve
 *     vimban dashboard daily
 */

#include <yaml-glib.h>
#include <json-glib/json-glib.h>
#include <mcp.h>
#include <libsoup/soup.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>


/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

static const gchar *VERSION = "0.3.0";

static const gchar *DEFAULT_DIR_SUFFIX = "Documents/notes";
static const gchar *DEFAULT_PEOPLE_DIR = "02_areas/work/people";
static const gchar *DEFAULT_PREFIX = "PROJ";
static const gchar *CONFIG_DIR_NAME = ".vimban";
static const gchar *CONFIG_FILE_NAME = "config.yaml";
static const gchar *SEQUENCE_FILE_NAME = ".sequence";

/* Exit codes */
#define VIMBAN_EXIT_SUCCESS          (0)
#define VIMBAN_EXIT_GENERAL_ERROR    (1)
#define VIMBAN_EXIT_INVALID_ARGS     (2)
#define VIMBAN_EXIT_FILE_NOT_FOUND   (3)
#define VIMBAN_EXIT_VALIDATION_ERROR (4)
#define VIMBAN_EXIT_KRAFNA_ERROR     (5)
#define VIMBAN_EXIT_GIT_ERROR        (6)

/* Status workflow */
static const gchar *STATUSES[] = {
    "backlog", "ready", "in_progress", "blocked",
    "review", "delegated", "done", "cancelled", NULL
};

/* Ticket types */
static const gchar *TICKET_TYPES[] = {
    "epic", "story", "task", "sub-task", "research", "bug", NULL
};

/* PARA base types */
static const gchar *PARA_TYPES[] = {
    "area", "resource", NULL
};

/* Specialized PARA types (added to ALL_TYPES dynamically) */
static const gchar *SPECIALIZED_PARA_TYPES[] = {
    "meeting", "journal", "recipe", "mentorship", NULL
};

/* Priorities */
static const gchar *PRIORITIES[] = {
    "critical", "high", "medium", "low", NULL
};

/* Dashboard types */
static const gchar *DASHBOARD_TYPES[] = {
    "daily", "weekly", "sprint", "project", "team", "person", NULL
};

/* Report types */
static const gchar *REPORT_TYPES[] = {
    "burndown", "velocity", "workload", "aging", "blockers", NULL
};

/* Output formats */
static const gchar *OUTPUT_FORMATS[] = {
    "plain", "md", "yaml", "json", NULL
};

/* Default output columns */
static const gchar *DEFAULT_COLUMNS[] = {
    "id", "status", "priority", "assignee", "title", "due_date", NULL
};

/* ID prefixes for ticket types */
typedef struct {
    const gchar *type;
    const gchar *prefix;
} TypePrefixEntry;

static const TypePrefixEntry TICKET_PREFIXES[] = {
    { "epic",       "PROJ" },
    { "story",      "PROJ" },
    { "task",       "PROJ" },
    { "sub-task",   "PROJ" },
    { "research",   "RESEARCH" },
    { "bug",        "BUG" },
    { "person",     "PERSON" },
    { NULL, NULL }
};

static const TypePrefixEntry PARA_TYPE_PREFIXES[] = {
    { "area",       "AREA" },
    { "resource",   "RESOURCE" },
    { NULL, NULL }
};

/* Sequence file names by type */
typedef struct {
    const gchar *type;
    const gchar *sequence_file;
} TypeSequenceEntry;

static const TypeSequenceEntry TYPE_SEQUENCES[] = {
    { "epic",        ".sequence" },
    { "story",       ".sequence" },
    { "task",        ".sequence" },
    { "sub-task",    ".sequence" },
    { "research",    ".sequence_research" },
    { "bug",         ".sequence_bug" },
    { "area",        ".sequence_area" },
    { "resource",    ".sequence_resource" },
    { "meeting",     ".sequence_meeting" },
    { "journal",     ".sequence_journal" },
    { "recipe",      ".sequence_recipe" },
    { "mentorship",  ".sequence_mentorship" },
    { "person",      ".sequence_person" },
    { NULL, NULL }
};

/* Valid status transitions */
typedef struct {
    const gchar  *from_status;
    const gchar **to_statuses;
} TransitionEntry;

/* All statuses except the source are valid targets (Python allows everything) */
static const gchar *TRANSITIONS_FROM_BACKLOG[] = {
    "ready", "in_progress", "blocked", "review", "delegated", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_READY[] = {
    "backlog", "in_progress", "blocked", "review", "delegated", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_IN_PROGRESS[] = {
    "backlog", "ready", "blocked", "review", "delegated", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_BLOCKED[] = {
    "backlog", "ready", "in_progress", "review", "delegated", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_REVIEW[] = {
    "backlog", "ready", "in_progress", "blocked", "delegated", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_DELEGATED[] = {
    "backlog", "ready", "in_progress", "blocked", "review", "done", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_DONE[] = {
    "backlog", "ready", "in_progress", "blocked", "review", "delegated", "cancelled", NULL
};
static const gchar *TRANSITIONS_FROM_CANCELLED[] = {
    "backlog", "ready", "in_progress", "blocked", "review", "delegated", "done", NULL
};

static const TransitionEntry VALID_TRANSITIONS[] = {
    { "backlog",     TRANSITIONS_FROM_BACKLOG },
    { "ready",       TRANSITIONS_FROM_READY },
    { "in_progress", TRANSITIONS_FROM_IN_PROGRESS },
    { "blocked",     TRANSITIONS_FROM_BLOCKED },
    { "review",      TRANSITIONS_FROM_REVIEW },
    { "delegated",   TRANSITIONS_FROM_DELEGATED },
    { "done",        TRANSITIONS_FROM_DONE },
    { "cancelled",   TRANSITIONS_FROM_CANCELLED },
    { NULL, NULL }
};

/* Link relation types */
static const gchar *RELATION_TYPES[] = {
    "member_of", "relates_to", "blocked_by", "blocks", NULL
};


/* ═══════════════════════════════════════════════════════════════════════════
 * Enums
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    VIMBAN_FORMAT_PLAIN = 0,
    VIMBAN_FORMAT_MD,
    VIMBAN_FORMAT_YAML,
    VIMBAN_FORMAT_JSON,
} VimbanFormat;

static VimbanFormat
vimban_format_from_string (const gchar *str)
{
    if (!str) return VIMBAN_FORMAT_PLAIN;
    if (g_strcmp0 (str, "md") == 0)   return VIMBAN_FORMAT_MD;
    if (g_strcmp0 (str, "yaml") == 0) return VIMBAN_FORMAT_YAML;
    if (g_strcmp0 (str, "json") == 0) return VIMBAN_FORMAT_JSON;
    return VIMBAN_FORMAT_PLAIN;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _VimbanConfig            VimbanConfig;
typedef struct _VimbanTransclusionLink  VimbanTransclusionLink;
typedef struct _VimbanTicket            VimbanTicket;
typedef struct _VimbanPerson            VimbanPerson;
typedef struct _VimbanComment           VimbanComment;
typedef struct _VimbanCommentReply      VimbanCommentReply;
typedef struct _VimbanGlobalOpts        VimbanGlobalOpts;
typedef struct _VimbanSpecTypeConfig    VimbanSpecTypeConfig;


/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * VimbanConfig:
 *
 * Per-project configuration loaded from .vimban/config.yaml.
 * Stores ID prefix, people directory location, and default values.
 */
struct _VimbanConfig {
    gchar   *directory;
    gchar   *prefix;
    gchar   *people_dir;
    gchar   *default_status;
    gchar   *default_priority;
};

static void
vimban_config_free (VimbanConfig *config)
{
    if (!config) return;
    g_free (config->directory);
    g_free (config->prefix);
    g_free (config->people_dir);
    g_free (config->default_status);
    g_free (config->default_priority);
    g_free (config);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanConfig, vimban_config_free)

/*
 * VimbanTransclusionLink:
 *
 * Represents a ![[path]] or ![[path|alias]] transclusion reference.
 * Used for people references and ticket relationships.
 */
struct _VimbanTransclusionLink {
    gchar   *path;
    gchar   *alias;
};

static void
vimban_transclusion_link_free (VimbanTransclusionLink *link)
{
    if (!link) return;
    g_free (link->path);
    g_free (link->alias);
    g_free (link);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanTransclusionLink, vimban_transclusion_link_free)

/*
 * VimbanTicket:
 *
 * Represents a ticket from markdown frontmatter. Contains all fields
 * defined in the vimban ticket schema including required fields
 * (id, title, type, status, created, filepath) and optional fields
 * for dates, people, classification, relationships, and progress.
 */
struct _VimbanTicket {
    /* required fields */
    gchar       *id;
    gchar       *title;
    gchar       *type;
    gchar       *status;
    GDateTime   *created;
    gchar       *filepath;

    /* dates */
    GDate       *start_date;
    GDate       *due_date;
    GDate       *end_date;

    /* people (transclusion links) */
    VimbanTransclusionLink *assignee;
    VimbanTransclusionLink *reporter;
    GPtrArray   *watchers;          /* element-type: VimbanTransclusionLink* */

    /* classification */
    gchar       *priority;
    gint         effort;            /* -1 if unset */
    GStrv        tags;              /* NULL-terminated */
    gchar       *project;
    gchar       *sprint;

    /* relationships */
    GPtrArray   *member_of;         /* element-type: VimbanTransclusionLink* */
    GPtrArray   *relates_to;        /* element-type: VimbanTransclusionLink* */
    GPtrArray   *blocked_by;        /* element-type: VimbanTransclusionLink* */
    GPtrArray   *blocks;            /* element-type: VimbanTransclusionLink* */

    /* progress */
    gint         progress;
    gint         checklist_total;
    gint         checklist_done;

    /* metadata */
    GDateTime   *updated;
    gint         version;
    gchar       *issue_link;
};

static void
vimban_ticket_free (VimbanTicket *ticket)
{
    if (!ticket) return;
    g_free (ticket->id);
    g_free (ticket->title);
    g_free (ticket->type);
    g_free (ticket->status);
    g_clear_pointer (&ticket->created, g_date_time_unref);
    g_free (ticket->filepath);
    g_clear_pointer (&ticket->start_date, g_date_free);
    g_clear_pointer (&ticket->due_date, g_date_free);
    g_clear_pointer (&ticket->end_date, g_date_free);
    vimban_transclusion_link_free (ticket->assignee);
    vimban_transclusion_link_free (ticket->reporter);
    g_clear_pointer (&ticket->watchers, g_ptr_array_unref);
    g_free (ticket->priority);
    g_strfreev (ticket->tags);
    g_free (ticket->project);
    g_free (ticket->sprint);
    g_clear_pointer (&ticket->member_of, g_ptr_array_unref);
    g_clear_pointer (&ticket->relates_to, g_ptr_array_unref);
    g_clear_pointer (&ticket->blocked_by, g_ptr_array_unref);
    g_clear_pointer (&ticket->blocks, g_ptr_array_unref);
    g_clear_pointer (&ticket->updated, g_date_time_unref);
    g_free (ticket->issue_link);
    g_free (ticket);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanTicket, vimban_ticket_free)

/*
 * VimbanPerson:
 *
 * Represents a person from their markdown file. Used for tracking
 * team members and their relationships.
 */
struct _VimbanPerson {
    gchar       *name;
    gchar       *filepath;
    gchar       *id;
    gchar       *email;
    gchar       *slack;
    gchar       *role;
    gchar       *team;
    VimbanTransclusionLink *manager;
    GPtrArray   *direct_reports;    /* element-type: VimbanTransclusionLink* */
    GDateTime   *created;
    GDateTime   *updated;
};

static void
vimban_person_free (VimbanPerson *person)
{
    if (!person) return;
    g_free (person->name);
    g_free (person->filepath);
    g_free (person->id);
    g_free (person->email);
    g_free (person->slack);
    g_free (person->role);
    g_free (person->team);
    vimban_transclusion_link_free (person->manager);
    g_clear_pointer (&person->direct_reports, g_ptr_array_unref);
    g_clear_pointer (&person->created, g_date_time_unref);
    g_clear_pointer (&person->updated, g_date_time_unref);
    g_free (person);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanPerson, vimban_person_free)

/*
 * VimbanCommentReply:
 *
 * A reply nested under a parent comment.
 */
struct _VimbanCommentReply {
    GDateTime   *timestamp;
    gchar       *content;
    gchar       *author;
};

static void
vimban_comment_reply_free (VimbanCommentReply *reply)
{
    if (!reply) return;
    g_clear_pointer (&reply->timestamp, g_date_time_unref);
    g_free (reply->content);
    g_free (reply->author);
    g_free (reply);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanCommentReply, vimban_comment_reply_free)

/*
 * VimbanComment:
 *
 * Represents a comment on a ticket or person file. Supports
 * single-level threading with replies.
 */
struct _VimbanComment {
    gint         id;
    GDateTime   *timestamp;
    gchar       *content;
    gchar       *author;
    GPtrArray   *replies;           /* element-type: VimbanCommentReply* */
};

static void
vimban_comment_free (VimbanComment *comment)
{
    if (!comment) return;
    g_clear_pointer (&comment->timestamp, g_date_time_unref);
    g_free (comment->content);
    g_free (comment->author);
    g_clear_pointer (&comment->replies, g_ptr_array_unref);
    g_free (comment);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanComment, vimban_comment_free)

/*
 * VimbanGlobalOpts:
 *
 * Global CLI options parsed before subcommand dispatch.
 */
struct _VimbanGlobalOpts {
    gchar       *directory;
    gchar       *format_str;
    VimbanFormat format;
    gboolean     quiet;
    gboolean     verbose;
    gboolean     no_color;
    gboolean     show_license;
    gboolean     mcp_stdio;
    gboolean     mcp_http;
    gboolean     serve;
    gboolean     work;
    gboolean     personal;
    gboolean     archived;
    gchar       *remote;
    gchar       *api_token;
    gboolean     no_token;
    gboolean     watch;
};

static void
vimban_global_opts_free (VimbanGlobalOpts *opts)
{
    if (!opts) return;
    g_free (opts->directory);
    g_free (opts->format_str);
    g_free (opts->remote);
    g_free (opts->api_token);
    g_free (opts);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VimbanGlobalOpts, vimban_global_opts_free)


/* ═══════════════════════════════════════════════════════════════════════════
 * Specialized Type Registry
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * VimbanSpecTypeConfig:
 *
 * Configuration for a specialized PARA type. Specialized types inherit
 * from base PARA types (area/resource) but have predefined behaviors
 * and directory structures.
 */
struct _VimbanSpecTypeConfig {
    const gchar *name;
    const gchar *parent;         /* "area" or "resource" */
    const gchar *prefix;
    const gchar *base_topic;
    const gchar *sequence_file;
    gboolean     has_status;
    const gchar *template_name;
};

static const VimbanSpecTypeConfig SPEC_TYPE_REGISTRY[] = {
    {
        "meeting", "area", "MTG",
        "work/meetings", ".sequence_meeting",
        TRUE, "meeting.md"
    },
    {
        "journal", "area", "JNL",
        "personal/journal", ".sequence_journal",
        TRUE, "journal.md"
    },
    {
        "recipe", "resource", "RCP",
        "food_and_health/recipes", ".sequence_recipe",
        FALSE, "recipe.md"
    },
    {
        "mentorship", "area", "MNTR",
        "work/mentorship", ".sequence_mentorship",
        TRUE, "mentorship.md"
    },
    { NULL, NULL, NULL, NULL, NULL, FALSE, NULL }
};

/*
 * vimban_get_spec_type_config:
 * @type_name: the type name to look up
 *
 * Look up specialized type configuration by name.
 *
 * Returns: (nullable): pointer to static config entry, or NULL
 */
static const VimbanSpecTypeConfig *
vimban_get_spec_type_config (const gchar *type_name)
{
    gint i;

    if (!type_name) return NULL;

    for (i = 0; SPEC_TYPE_REGISTRY[i].name != NULL; i++)
    {
        if (g_strcmp0 (SPEC_TYPE_REGISTRY[i].name, type_name) == 0)
            return &SPEC_TYPE_REGISTRY[i];
    }

    return NULL;
}

/*
 * vimban_is_specialized_type:
 * @type_name: the type name to check
 *
 * Check if a type name is a specialized PARA type.
 *
 * Returns: TRUE if specialized type
 */
static gboolean
vimban_is_specialized_type (const gchar *type_name)
{
    return vimban_get_spec_type_config (type_name) != NULL;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Utility helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean g_no_color = FALSE;
static gboolean g_quiet = FALSE;
static gboolean g_verbose = FALSE;

static gchar *
_c (const gchar *code, const gchar *text)
{
    if (g_no_color)
        return g_strdup (text);
    return g_strdup_printf ("\033[%sm%s\033[0m", code, text);
}

#define _green(t)   _c ("32", t)
#define _red(t)     _c ("31", t)
#define _yellow(t)  _c ("33", t)
#define _cyan(t)    _c ("36", t)
#define _bold(t)    _c ("1", t)
#define _dim(t)     _c ("2", t)
#define _magenta(t) _c ("35", t)

/*
 * vimban_error:
 * @msg: error message
 * @exit_code: exit code to return
 *
 * Print error message to stderr. Does NOT exit — caller handles return.
 */
static void
vimban_error (const gchar *msg, gint exit_code)
{
    g_autofree gchar *prefix = _red ("Error");
    g_printerr ("%s: %s\n", prefix, msg);
    (void) exit_code;
}

/*
 * vimban_info:
 * @msg: info message
 *
 * Print info message to stdout unless --quiet.
 */
static void
vimban_info (const gchar *msg)
{
    if (!g_quiet)
        g_print ("%s\n", msg);
}

/*
 * vimban_str_in_list:
 * @str: string to find
 * @list: NULL-terminated string array
 *
 * Check if a string appears in a NULL-terminated array.
 *
 * Returns: TRUE if found
 */
static gboolean
vimban_str_in_list (const gchar *str, const gchar **list)
{
    gint i;

    if (!str || !list) return FALSE;

    for (i = 0; list[i] != NULL; i++)
    {
        if (g_strcmp0 (str, list[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * vimban_is_valid_type:
 * @type_name: type name to validate
 *
 * Check if a type name is valid (ticket, PARA, or specialized).
 *
 * Returns: TRUE if valid
 */
static gboolean
vimban_is_valid_type (const gchar *type_name)
{
    if (vimban_str_in_list (type_name, (const gchar **)TICKET_TYPES))
        return TRUE;
    if (vimban_str_in_list (type_name, (const gchar **)PARA_TYPES))
        return TRUE;
    if (vimban_str_in_list (type_name, (const gchar **)SPECIALIZED_PARA_TYPES))
        return TRUE;
    return FALSE;
}

/*
 * vimban_get_default_dir:
 *
 * Get the default vimban directory (~/Documents/notes).
 *
 * Returns: (transfer full): newly allocated path string
 */
static gchar *
vimban_get_default_dir (void)
{
    return g_build_filename (g_get_home_dir (), DEFAULT_DIR_SUFFIX, NULL);
}

/*
 * vimban_get_prefix_for_type:
 * @type_name: ticket type
 * @config: project config (for custom prefix)
 *
 * Get the ID prefix for a given ticket type.
 *
 * Returns: (transfer none): static string with prefix
 */
static const gchar *
vimban_get_prefix_for_type (const gchar              *type_name,
                             const VimbanConfig       *config)
{
    const VimbanSpecTypeConfig *spec;
    gint i;

    /* check specialized types first */
    spec = vimban_get_spec_type_config (type_name);
    if (spec)
        return spec->prefix;

    /* check PARA type prefixes */
    for (i = 0; PARA_TYPE_PREFIXES[i].type != NULL; i++)
    {
        if (g_strcmp0 (type_name, PARA_TYPE_PREFIXES[i].type) == 0)
            return PARA_TYPE_PREFIXES[i].prefix;
    }

    /* check ticket type prefixes */
    for (i = 0; TICKET_PREFIXES[i].type != NULL; i++)
    {
        if (g_strcmp0 (type_name, TICKET_PREFIXES[i].type) == 0)
        {
            /* use project config prefix if it overrides PROJ */
            if (g_strcmp0 (TICKET_PREFIXES[i].prefix, "PROJ") == 0 &&
                config && config->prefix)
            {
                return config->prefix;
            }
            return TICKET_PREFIXES[i].prefix;
        }
    }

    /* fallback */
    return config && config->prefix ? config->prefix : DEFAULT_PREFIX;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * License text
 * ═══════════════════════════════════════════════════════════════════════════ */

static const gchar *LICENSE_TEXT =
    "vimban - Markdown-native ticket/kanban management system\n"
    "Copyright (C) 2025  Zach Podbielniak\n"
    "\n"
    "This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU Affero General Public License as published\n"
    "by the Free Software Foundation, either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU Affero General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Affero General Public License\n"
    "along with this program.  If not, see <https://www.gnu.org/licenses/>.";


/* ═══════════════════════════════════════════════════════════════════════════
 * Config loading
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_config_load:
 * @directory: the base directory path
 *
 * Load project config from .vimban/config.yaml or use defaults.
 *
 * Returns: (transfer full): newly allocated VimbanConfig
 */
static VimbanConfig *
vimban_config_load (const gchar *directory)
{
    VimbanConfig *config;
    g_autofree gchar *config_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(YamlParser) parser = NULL;
    YamlNode *root;
    YamlMapping *mapping;

    config = g_new0 (VimbanConfig, 1);
    config->directory       = g_strdup (directory);
    config->prefix          = g_strdup (DEFAULT_PREFIX);
    config->people_dir      = g_strdup (DEFAULT_PEOPLE_DIR);
    config->default_status  = g_strdup ("backlog");
    config->default_priority = g_strdup ("medium");

    config_path = g_build_filename (directory, CONFIG_DIR_NAME,
                                     CONFIG_FILE_NAME, NULL);

    if (!g_file_test (config_path, G_FILE_TEST_EXISTS))
        return config;

    parser = yaml_parser_new_immutable ();
    if (!yaml_parser_load_from_file (parser, config_path, &error))
    {
        if (g_verbose)
            g_printerr ("Warning: failed to parse %s: %s\n",
                         config_path, error->message);
        return config;
    }

    root = yaml_parser_get_root (parser);
    if (!root || yaml_node_get_node_type (root) != YAML_NODE_MAPPING)
        return config;

    mapping = yaml_node_get_mapping (root);
    if (!mapping)
        return config;

    if (yaml_mapping_has_member (mapping, "prefix"))
    {
        g_free (config->prefix);
        config->prefix = g_strdup (
            yaml_mapping_get_string_member (mapping, "prefix"));
    }

    if (yaml_mapping_has_member (mapping, "people_dir"))
    {
        g_free (config->people_dir);
        config->people_dir = g_strdup (
            yaml_mapping_get_string_member (mapping, "people_dir"));
    }

    if (yaml_mapping_has_member (mapping, "default_status"))
    {
        g_free (config->default_status);
        config->default_status = g_strdup (
            yaml_mapping_get_string_member (mapping, "default_status"));
    }

    if (yaml_mapping_has_member (mapping, "default_priority"))
    {
        g_free (config->default_priority);
        config->default_priority = g_strdup (
            yaml_mapping_get_string_member (mapping, "default_priority"));
    }

    return config;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Regex patterns (compiled lazily)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ![[path]] or ![[path|alias]] */
#define TRANSCLUSION_RE "!\\[\\[([^\\]|]+)(?:\\|([^\\]]+))?\\]\\]"

/* [text](path) */
#define MARKDOWN_LINK_RE "\\[([^\\]]+)\\]\\(([^)]+)\\)"

/* ### Comment #N by author (timestamp) */
#define COMMENT_HEADER_RE "^### Comment #(\\d+)(?: by ([^\\(]+))? \\(([^)]+)\\)\\s*$"

/* #### Reply by author (timestamp) */
#define REPLY_HEADER_RE "^#### Reply(?: by ([^\\(]+))? \\(([^)]+)\\)\\s*$"

/* +Nd, +Nw, +Nm relative date */
#define RELATIVE_DATE_RE "^\\+(\\d+)([dwm])$"


/* ═══════════════════════════════════════════════════════════════════════════
 * TransclusionLink parsing / formatting
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_transclusion_link_parse:
 * @text: string to parse
 *
 * Parse a transclusion or markdown link.
 * Supports ![[path]], ![[path|alias]], and [text](path).
 *
 * Returns: (transfer full) (nullable): new link, or NULL
 */
static VimbanTransclusionLink *
vimban_transclusion_link_parse (const gchar *text)
{
    VimbanTransclusionLink *link;
    g_autoptr(GRegex) re_trans = NULL;
    g_autoptr(GRegex) re_md = NULL;
    g_autoptr(GMatchInfo) info = NULL;

    if (!text || !text[0])
        return NULL;

    /* strip leading/trailing whitespace */
    g_autofree gchar *trimmed = g_strstrip (g_strdup (text));
    if (!trimmed[0])
        return NULL;

    /* try transclusion: ![[path]] or ![[path|alias]] */
    re_trans = g_regex_new (TRANSCLUSION_RE, 0, 0, NULL);
    if (!re_trans) return NULL;
    if (g_regex_match (re_trans, trimmed, 0, &info))
    {
        link = g_new0 (VimbanTransclusionLink, 1);
        link->path  = g_match_info_fetch (info, 1);
        link->alias = g_match_info_fetch (info, 2);
        /* g_match_info_fetch returns "" for unmatched groups */
        if (link->alias && link->alias[0] == '\0')
        {
            g_free (link->alias);
            link->alias = NULL;
        }
        return link;
    }
    g_clear_pointer (&info, g_match_info_unref);

    /* try markdown link: [text](path) */
    re_md = g_regex_new (MARKDOWN_LINK_RE, 0, 0, NULL);
    if (!re_md) return NULL;
    if (g_regex_match (re_md, trimmed, 0, &info))
    {
        link = g_new0 (VimbanTransclusionLink, 1);
        link->alias = g_match_info_fetch (info, 1);
        link->path  = g_match_info_fetch (info, 2);
        return link;
    }

    return NULL;
}

/*
 * vimban_transclusion_link_to_string:
 * @link: transclusion link
 *
 * Format as ![[path]] or ![[path|alias]].
 *
 * Returns: (transfer full): string representation
 */
static gchar *
vimban_transclusion_link_to_string (const VimbanTransclusionLink *link)
{
    if (!link || !link->path)
        return g_strdup ("");

    if (link->alias)
        return g_strdup_printf ("![[%s|%s]]", link->path, link->alias);
    return g_strdup_printf ("![[%s]]", link->path);
}

/*
 * vimban_transclusion_link_get_stem:
 * @link: transclusion link
 *
 * Get the file stem (basename without extension) from a link path.
 *
 * Returns: (transfer full): stem string
 */
static gchar *
vimban_transclusion_link_get_stem (const VimbanTransclusionLink *link)
{
    g_autofree gchar *base = NULL;
    gchar *dot;

    if (!link || !link->path)
        return g_strdup ("");

    base = g_path_get_basename (link->path);
    dot = g_strrstr (base, ".");
    if (dot)
        *dot = '\0';
    return g_strdup (base);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Template resolution
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_resolve_template_dir:
 *
 * Find the template directory via fallback chain:
 *   1. VIMBAN_TEMPLATE_DIR env var
 *   2. ~/.dotfiles/vimban/share/vimban/templates
 *   3. ~/.dotfiles/share/vimban/templates
 *   4. Relative to script: ../../share/vimban/templates
 *   5. XDG: ~/.local/share/vimban/templates
 *
 * Returns: (transfer full) (nullable): path to templates dir, or NULL
 */
static gchar *
vimban_resolve_template_dir (void)
{
    const gchar *env;
    g_autofree gchar *submod = NULL;
    g_autofree gchar *legacy = NULL;
    g_autofree gchar *xdg_path = NULL;
    const gchar *xdg_data;

    /* 1. env var */
    env = g_getenv ("VIMBAN_TEMPLATE_DIR");
    if (env && g_file_test (env, G_FILE_TEST_IS_DIR))
        return g_strdup (env);

    /* 2. submodule path */
    submod = g_build_filename (g_get_home_dir (), ".dotfiles", "vimban",
                                "share", "vimban", "templates", NULL);
    if (g_file_test (submod, G_FILE_TEST_IS_DIR))
        return g_steal_pointer (&submod);

    /* 3. legacy dotfiles */
    legacy = g_build_filename (g_get_home_dir (), ".dotfiles",
                                "share", "vimban", "templates", NULL);
    if (g_file_test (legacy, G_FILE_TEST_IS_DIR))
        return g_steal_pointer (&legacy);

    /* 4. skip script-relative for crispy (no stable __FILE__ dir) */

    /* 5. XDG data */
    xdg_data = g_getenv ("XDG_DATA_HOME");
    if (xdg_data)
        xdg_path = g_build_filename (xdg_data, "vimban", "templates", NULL);
    else
        xdg_path = g_build_filename (g_get_home_dir (), ".local", "share",
                                      "vimban", "templates", NULL);

    if (g_file_test (xdg_path, G_FILE_TEST_IS_DIR))
        return g_steal_pointer (&xdg_path);

    /* fallback: return submodule path even if missing */
    return g_build_filename (g_get_home_dir (), ".dotfiles", "vimban",
                              "share", "vimban", "templates", NULL);
}

/*
 * vimban_load_template:
 * @template_name: template file name (e.g., "task.md")
 *
 * Load a template file from the template directory.
 *
 * Returns: (transfer full) (nullable): template content, or NULL
 */
static gchar *
vimban_load_template (const gchar *template_name)
{
    g_autofree gchar *tmpl_dir = NULL;
    g_autofree gchar *path = NULL;
    gchar *content = NULL;

    tmpl_dir = vimban_resolve_template_dir ();
    if (!tmpl_dir)
        return NULL;

    path = g_build_filename (tmpl_dir, template_name, NULL);
    if (!g_file_get_contents (path, &content, NULL, NULL))
        return NULL;

    return content;
}

/*
 * vimban_fill_template:
 * @tmpl: template string with {{placeholder}} markers
 * @replacements: hash table of placeholder -> value
 *
 * Replace all {{placeholder}} markers in template.
 *
 * Returns: (transfer full): filled template string
 */
static gchar *
vimban_fill_template (const gchar  *tmpl,
                       GHashTable   *replacements)
{
    GHashTableIter iter;
    gpointer key, value;
    gchar *result;

    if (!tmpl)
        return g_strdup ("");
    if (!replacements)
        return g_strdup (tmpl);

    result = g_strdup (tmpl);
    g_hash_table_iter_init (&iter, replacements);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        g_autofree gchar *old = result;
        g_autofree gchar *escaped = g_regex_escape_string ((const gchar *)key, -1);
        g_autoptr(GRegex) regex = g_regex_new (escaped, 0, 0, NULL);
        if (!regex)
        {
            result = g_strdup (old);
            continue;
        }
        result = g_regex_replace_literal (
            regex, old, -1, 0, (const gchar *)value, 0, NULL);
        if (!result)
            result = g_strdup (old);
    }

    return result;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * YAML frontmatter parsing / serialization
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_parse_frontmatter:
 * @content: full markdown file content
 * @out_mapping: (out) (transfer full) (nullable): parsed YAML mapping
 * @out_body: (out) (transfer full) (nullable): body text after frontmatter
 *
 * Split content into YAML frontmatter mapping and body.
 * If no frontmatter found, mapping is NULL and body is the full content.
 *
 * Returns: TRUE if content was parsed (even if no frontmatter)
 */
static gboolean
vimban_parse_frontmatter (const gchar   *content,
                           YamlMapping  **out_mapping,
                           gchar        **out_body)
{
    const gchar *yaml_start;
    const gchar *yaml_end;
    g_autofree gchar *yaml_str = NULL;
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    YamlNode *root;

    *out_mapping = NULL;
    *out_body = NULL;

    if (!content)
    {
        *out_body = g_strdup ("");
        return TRUE;
    }

    /* must start with --- */
    if (!g_str_has_prefix (content, "---"))
    {
        *out_body = g_strdup (content);
        return TRUE;
    }

    /* find closing --- */
    yaml_start = content + 3;
    if (*yaml_start == '\n') yaml_start++;

    yaml_end = strstr (yaml_start, "\n---");
    if (!yaml_end)
    {
        /* no closing delimiter — treat as no frontmatter */
        *out_body = g_strdup (content);
        return TRUE;
    }

    /* extract YAML between delimiters */
    yaml_str = g_strndup (yaml_start, yaml_end - yaml_start);

    /* body is everything after closing --- and its newline */
    {
        const gchar *body_start = yaml_end + 4; /* skip \n--- */
        if (*body_start == '\n') body_start++;
        /* strip leading blank lines */
        while (*body_start == '\n') body_start++;
        *out_body = g_strdup (body_start);
    }

    /* parse YAML */
    parser = yaml_parser_new ();
    if (!yaml_parser_load_from_data (parser, yaml_str, -1, &error))
    {
        if (g_verbose)
            g_printerr ("Warning: YAML parse error: %s\n", error->message);
        return TRUE;  /* body set, mapping NULL */
    }

    root = yaml_parser_get_root (parser);
    if (!root || yaml_node_get_node_type (root) != YAML_NODE_MAPPING)
        return TRUE;

    *out_mapping = yaml_mapping_ref (yaml_node_get_mapping (root));
    return TRUE;
}

/*
 * vimban_dump_frontmatter:
 * @mapping: YAML mapping to serialize
 * @body: body text (may be NULL)
 *
 * Create markdown content with YAML frontmatter:
 *   ---
 *   key: value
 *   ---
 *
 *   body
 *
 * Returns: (transfer full): complete markdown content
 */
static gchar *
vimban_dump_frontmatter (YamlMapping *mapping,
                          const gchar *body)
{
    g_autoptr(YamlGenerator) gen = NULL;
    YamlNode *root;
    g_autofree gchar *yaml_str = NULL;
    gsize len;

    gen = yaml_generator_new ();
    root = yaml_node_new_mapping (mapping);
    yaml_generator_set_root (gen, root);
    yaml_str = yaml_generator_to_data (gen, &len, NULL);
    if (!yaml_str)
        yaml_str = g_strdup ("");

    if (body && body[0])
        return g_strdup_printf ("---\n%s---\n\n%s", yaml_str, body);
    else
        return g_strdup_printf ("---\n%s---\n", yaml_str);
}

/*
 * vimban_update_frontmatter_field:
 * @filepath: path to markdown file
 * @field: field name to update
 * @value: new string value
 * @increment_version: whether to bump version
 *
 * Update a single field in a file's frontmatter, update timestamp,
 * and optionally increment version.
 *
 * Returns: TRUE on success
 */
static gboolean
vimban_update_frontmatter_field (const gchar *filepath,
                                  const gchar *field,
                                  const gchar *value,
                                  gboolean     increment_version)
{
    g_autofree gchar *content = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *result = NULL;
    g_autofree gchar *now_str = NULL;
    g_autoptr(GDateTime) now = NULL;
    YamlMapping *mapping = NULL;
    gint64 ver;

    if (!g_file_get_contents (filepath, &content, NULL, NULL))
        return FALSE;

    vimban_parse_frontmatter (content, &mapping, &body);
    if (!mapping)
        mapping = yaml_mapping_new ();

    yaml_mapping_set_string_member (mapping, field, value);

    now = g_date_time_new_now_local ();
    now_str = g_date_time_format_iso8601 (now);
    yaml_mapping_set_string_member (mapping, "updated", now_str);

    if (increment_version)
    {
        ver = 0;
        if (yaml_mapping_has_member (mapping, "version"))
            ver = yaml_mapping_get_int_member (mapping, "version");
        yaml_mapping_set_int_member (mapping, "version", ver + 1);
    }

    result = vimban_dump_frontmatter (mapping, body);
    yaml_mapping_unref (mapping);

    {
        g_autoptr(GError) write_err = NULL;
        if (!g_file_set_contents (filepath, result, -1, &write_err))
        {
            g_printerr ("vimban: failed to write %s: %s\n",
                         filepath, write_err->message);
            return FALSE;
        }
        return TRUE;
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Date parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_parse_date_field:
 * @value: string from YAML frontmatter (ISO date or NULL)
 *
 * Parse a date field from frontmatter into a GDate.
 *
 * Returns: (transfer full) (nullable): new GDate, or NULL
 */
static GDate *
vimban_parse_date_field (const gchar *value)
{
    GDate *d;
    gint year, month, day;

    if (!value || !value[0])
        return NULL;

    /* try ISO: YYYY-MM-DD */
    if (sscanf (value, "%d-%d-%d", &year, &month, &day) == 3)
    {
        d = g_date_new_dmy ((GDateDay) day, (GDateMonth) month,
                             (GDateYear) year);
        if (g_date_valid (d))
            return d;
        g_date_free (d);
    }

    return NULL;
}

/*
 * vimban_parse_date:
 * @date_str: user-provided date string
 *
 * Parse absolute and relative dates.
 * Supports: ISO (2025-12-25), relative (+7d, +2w, +1m),
 * named (today, tomorrow), weekday names (monday..sunday).
 *
 * Returns: (transfer full) (nullable): new GDate, or NULL
 */
static GDate *
vimban_parse_date (const gchar *date_str)
{
    g_autoptr(GDateTime) now = NULL;
    g_autoptr(GDateTime) target = NULL;
    g_autoptr(GRegex) rel_re = NULL;
    g_autoptr(GMatchInfo) rel_info = NULL;
    GDate *d;
    g_autofree gchar *lower = NULL;
    static const gchar *day_names[] = {
        "monday", "tuesday", "wednesday", "thursday",
        "friday", "saturday", "sunday", NULL
    };
    gint i;

    if (!date_str || !date_str[0])
        return NULL;

    lower = g_ascii_strdown (date_str, -1);
    g_strstrip (lower);
    now = g_date_time_new_now_local ();

    /* named: today */
    if (g_strcmp0 (lower, "today") == 0)
    {
        d = g_date_new_dmy (
            (GDateDay) g_date_time_get_day_of_month (now),
            (GDateMonth) g_date_time_get_month (now),
            (GDateYear) g_date_time_get_year (now));
        return d;
    }

    /* named: tomorrow */
    if (g_strcmp0 (lower, "tomorrow") == 0)
    {
        target = g_date_time_add_days (now, 1);
        d = g_date_new_dmy (
            (GDateDay) g_date_time_get_day_of_month (target),
            (GDateMonth) g_date_time_get_month (target),
            (GDateYear) g_date_time_get_year (target));
        return d;
    }

    /* relative: +Nd, +Nw, +Nm */
    rel_re = g_regex_new (RELATIVE_DATE_RE, 0, 0, NULL);
    if (g_regex_match (rel_re, lower, 0, &rel_info))
    {
        g_autofree gchar *num_s = g_match_info_fetch (rel_info, 1);
        g_autofree gchar *unit_s = g_match_info_fetch (rel_info, 2);
        gint64 num64 = g_ascii_strtoll (num_s, NULL, 10);
        gint num;
        gint days = 0;

        /* reject out-of-range values */
        if (num64 < 0 || num64 > 3650)
            num64 = 0;
        num = (gint) num64;

        if (unit_s[0] == 'd') days = num;
        else if (unit_s[0] == 'w') days = num * 7;
        else if (unit_s[0] == 'm') days = num * 30;

        target = g_date_time_add_days (now, days);
        d = g_date_new_dmy (
            (GDateDay) g_date_time_get_day_of_month (target),
            (GDateMonth) g_date_time_get_month (target),
            (GDateYear) g_date_time_get_year (target));
        return d;
    }

    /* weekday names: monday..sunday → next occurrence */
    for (i = 0; day_names[i]; i++)
    {
        if (g_strcmp0 (lower, day_names[i]) == 0)
        {
            gint target_day = i + 1; /* GDateTime weekday: 1=Mon */
            gint current_day = g_date_time_get_day_of_week (now);
            gint ahead = target_day - current_day;
            if (ahead <= 0) ahead += 7;
            target = g_date_time_add_days (now, ahead);
            d = g_date_new_dmy (
                (GDateDay) g_date_time_get_day_of_month (target),
                (GDateMonth) g_date_time_get_month (target),
                (GDateYear) g_date_time_get_year (target));
            return d;
        }
    }

    /* ISO: YYYY-MM-DD */
    return vimban_parse_date_field (lower);
}

/*
 * vimban_date_to_string:
 * @d: a GDate
 *
 * Format a GDate as ISO string YYYY-MM-DD.
 *
 * Returns: (transfer full): date string
 */
static gchar *
vimban_date_to_string (const GDate *d)
{
    gchar buf[12];

    if (!d || !g_date_valid (d))
        return g_strdup ("");

    g_date_strftime (buf, sizeof (buf), "%Y-%m-%d", d);
    return g_strdup (buf);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * ID generation with file locking
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_get_sequence_file:
 * @type_name: ticket type (may be NULL)
 *
 * Get the sequence file name for a given type.
 *
 * Returns: (transfer none): static string
 */
static const gchar *
vimban_get_sequence_file (const gchar *type_name)
{
    gint i;

    if (!type_name)
        return SEQUENCE_FILE_NAME;

    for (i = 0; TYPE_SEQUENCES[i].type != NULL; i++)
    {
        if (g_strcmp0 (type_name, TYPE_SEQUENCES[i].type) == 0)
            return TYPE_SEQUENCES[i].sequence_file;
    }

    return SEQUENCE_FILE_NAME;
}

/*
 * vimban_next_id:
 * @directory: base directory (contains .vimban/)
 * @custom_id: (nullable): custom ID to use directly
 * @prefix: (nullable): custom prefix override
 * @ticket_type: (nullable): ticket type for sequence selection
 * @config: project config for default prefix
 *
 * Generate next ticket ID with file-based locking.
 * Format: PREFIX-NNNNN (5-digit zero-padded).
 *
 * Returns: (transfer full): new ticket ID string
 */
static gchar *
vimban_next_id (const gchar        *directory,
                 const gchar        *custom_id,
                 const gchar        *prefix,
                 const gchar        *ticket_type,
                 const VimbanConfig *config)
{
    g_autofree gchar *seq_path = NULL;
    g_autofree gchar *contents = NULL;
    const gchar *seq_file;
    const gchar *use_prefix;
    gint current, next_num;
    gint fd;
    struct flock fl;
    gchar buf[32];
    gssize n;

    if (custom_id)
        return g_strdup (custom_id);

    /* determine prefix */
    use_prefix = prefix ? prefix
                        : vimban_get_prefix_for_type (ticket_type, config);

    /* determine sequence file */
    seq_file = vimban_get_sequence_file (ticket_type);
    seq_path = g_build_filename (directory, CONFIG_DIR_NAME, seq_file, NULL);

    /* ensure config dir exists, then open-or-create atomically */
    {
        g_autofree gchar *dir_path = NULL;
        dir_path = g_build_filename (directory, CONFIG_DIR_NAME, NULL);
        g_mkdir_with_parents (dir_path, 0755);
    }

    fd = open (seq_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
    {
        g_printerr ("Error: cannot open sequence file: %s\n", seq_path);
        return g_strdup_printf ("%s-00000", use_prefix);
    }

    memset (&fl, 0, sizeof (fl));
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl (fd, F_SETLKW, &fl) == -1)
    {
        g_printerr ("Error: cannot lock sequence file: %s\n", seq_path);
        close (fd);
        return NULL;
    }

    /* read current value */
    n = read (fd, buf, sizeof (buf) - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    current = (gint) g_ascii_strtoll (g_strstrip (buf), NULL, 10);
    next_num = current + 1;

    /* write new value */
    lseek (fd, 0, SEEK_SET);
    if (ftruncate (fd, 0) != 0)
    {
        /* ignore truncation errors */
    }
    n = snprintf (buf, sizeof (buf), "%d", next_num);
    if (write (fd, buf, n) < 0)
    {
        /* ignore write errors */
    }
    fsync (fd);
    close (fd);  /* releases lock */

    return g_strdup_printf ("%s-%05d", use_prefix, next_num);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Type normalization
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_normalize_type_name:
 * @type_name: type name (possibly plural)
 *
 * Normalize type name to singular form.
 * e.g., "stories" -> "story", "sub-tasks" -> "sub-task"
 *
 * Returns: (transfer full): normalized type name
 */
static gchar *
vimban_normalize_type_name (const gchar *type_name)
{
    gsize len;

    if (!type_name)
        return g_strdup ("");

    /* special plural cases */
    if (g_strcmp0 (type_name, "stories") == 0)
        return g_strdup ("story");
    if (g_strcmp0 (type_name, "sub-tasks") == 0)
        return g_strdup ("sub-task");

    /* standard -s removal if singular form is a valid type */
    len = strlen (type_name);
    if (len > 1 && type_name[len - 1] == 's')
    {
        g_autofree gchar *singular = g_strndup (type_name, len - 1);
        if (vimban_is_valid_type (singular))
            return g_steal_pointer (&singular);
    }

    return g_strdup (type_name);
}

/*
 * vimban_sanitize_filename:
 * @title: raw title string
 * @max_len: maximum length (0 = no limit)
 *
 * Sanitize a title for use as a filename.
 * Strips non-alphanumeric chars, lowercases, replaces spaces with _.
 *
 * Returns: (transfer full): sanitized filename (without .md)
 */
static gchar *
vimban_sanitize_filename (const gchar *title,
                           gsize        max_len)
{
    g_autoptr(GRegex) re_nonword = NULL;
    g_autoptr(GRegex) re_spaces = NULL;
    g_autofree gchar *step1 = NULL;
    g_autofree gchar *step2 = NULL;
    gchar *result;

    if (!title || !title[0])
        return g_strdup ("untitled");

    /* remove non-word chars except spaces and hyphens */
    re_nonword = g_regex_new ("[^\\w\\s-]", 0, 0, NULL);
    step1 = g_regex_replace_literal (re_nonword, title, -1, 0, "", 0, NULL);
    g_strstrip (step1);

    /* lowercase */
    result = g_ascii_strdown (step1, -1);

    /* replace whitespace/hyphens with underscore */
    re_spaces = g_regex_new ("[-\\s]+", 0, 0, NULL);
    step2 = g_regex_replace_literal (re_spaces, result, -1, 0, "_", 0, NULL);
    g_free (result);

    /* truncate */
    if (max_len > 0 && strlen (step2) > max_len)
        step2[max_len] = '\0';

    /* reject path traversal attempts */
    if (g_strstr_len (step2, -1, "/") != NULL ||
        g_strstr_len (step2, -1, "..") != NULL)
    {
        return g_strdup ("untitled");
    }

    return g_steal_pointer (&step2);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Ticket / Person from-file loaders
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * _parse_link_list:
 * @mapping: YAML mapping
 * @key: field name containing a sequence of transclusion strings
 *
 * Parse a YAML sequence field into a GPtrArray of VimbanTransclusionLink.
 *
 * Returns: (transfer full): array (may be empty)
 */
static GPtrArray *
_parse_link_list (YamlMapping *mapping, const gchar *key)
{
    GPtrArray *arr;
    YamlSequence *seq;
    guint i, len;

    arr = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_transclusion_link_free);

    if (!mapping || !yaml_mapping_has_member (mapping, key))
        return arr;

    seq = yaml_mapping_get_sequence_member (mapping, key);
    if (!seq)
        return arr;

    len = yaml_sequence_get_length (seq);
    for (i = 0; i < len; i++)
    {
        const gchar *str = yaml_sequence_get_string_element (seq, i);
        if (str)
        {
            VimbanTransclusionLink *link = vimban_transclusion_link_parse (str);
            if (link)
                g_ptr_array_add (arr, link);
        }
    }

    return arr;
}

/*
 * _parse_string_list:
 * @mapping: YAML mapping
 * @key: field name containing a sequence of strings
 *
 * Parse a YAML sequence field into a NULL-terminated string array.
 *
 * Returns: (transfer full): GStrv (NULL-terminated)
 */
static GStrv
_parse_string_list (YamlMapping *mapping, const gchar *key)
{
    YamlSequence *seq;
    GPtrArray *tmp;
    guint i, len;

    if (!mapping || !yaml_mapping_has_member (mapping, key))
        return NULL;

    seq = yaml_mapping_get_sequence_member (mapping, key);
    if (!seq)
        return NULL;

    tmp = g_ptr_array_new ();
    len = yaml_sequence_get_length (seq);
    for (i = 0; i < len; i++)
    {
        const gchar *s = yaml_sequence_get_string_element (seq, i);
        if (s)
            g_ptr_array_add (tmp, g_strdup (s));
    }
    g_ptr_array_add (tmp, NULL);

    return (GStrv) g_ptr_array_free (tmp, FALSE);
}

/*
 * _yaml_get_string:
 * @mapping: YAML mapping
 * @key: field name
 *
 * Safe wrapper to get a string member, stripping quotes from id fields.
 *
 * Returns: (transfer full) (nullable): string or NULL
 */
static gchar *
_yaml_get_string (YamlMapping *mapping, const gchar *key)
{
    const gchar *val;

    if (!mapping || !yaml_mapping_has_member (mapping, key))
        return NULL;

    val = yaml_mapping_get_string_member (mapping, key);
    if (!val)
        return NULL;

    /* strip surrounding double quotes (e.g., id: "PROJ-00042") */
    if (val[0] == '"')
    {
        gsize len = strlen (val);
        if (len > 1 && val[len - 1] == '"')
            return g_strndup (val + 1, len - 2);
    }

    return g_strdup (val);
}

/*
 * _yaml_get_datetime:
 * @mapping: YAML mapping
 * @key: field name
 *
 * Parse an ISO datetime string from YAML.
 *
 * Returns: (transfer full) (nullable): GDateTime or NULL
 */
static GDateTime *
_yaml_get_datetime (YamlMapping *mapping, const gchar *key)
{
    const gchar *val;
    GDateTime *dt;
    g_autoptr(GTimeZone) tz = NULL;

    if (!mapping || !yaml_mapping_has_member (mapping, key))
        return NULL;

    val = yaml_mapping_get_string_member (mapping, key);
    if (!val || !val[0])
        return NULL;

    /* g_date_time_new_from_iso8601 requires a timezone indicator.
     * provide local timezone as fallback for strings without one. */
    tz = g_time_zone_new_local ();

    dt = g_date_time_new_from_iso8601 (val, tz);
    if (dt)
        return dt;

    /* try replacing space with T for "YYYY-MM-DD HH:MM:SS[.usec]" format */
    {
        g_autofree gchar *normalized = g_strdup (val);
        gchar *space = strchr (normalized, ' ');
        if (space && space - normalized == 10)
        {
            *space = 'T';
            dt = g_date_time_new_from_iso8601 (normalized, tz);
            if (dt)
                return dt;
        }
    }

    /* try parsing as just date YYYY-MM-DD by appending T00:00:00 */
    if (strlen (val) == 10)
    {
        g_autofree gchar *with_time = g_strdup_printf ("%sT00:00:00", val);
        dt = g_date_time_new_from_iso8601 (with_time, tz);
    }

    return dt;
}

/*
 * vimban_ticket_from_file:
 * @filepath: path to the markdown file
 * @error: (out) (nullable): error location
 *
 * Load a ticket from a markdown file by parsing its frontmatter.
 *
 * Returns: (transfer full) (nullable): new VimbanTicket, or NULL on error
 */
static VimbanTicket *
vimban_ticket_from_file (const gchar  *filepath,
                          GError      **error)
{
    VimbanTicket *ticket;
    g_autofree gchar *content = NULL;
    g_autofree gchar *body = NULL;
    YamlMapping *mapping = NULL;
    gboolean status_required;
    const gchar *assignee_str;
    const gchar *reporter_str;

    if (!g_file_get_contents (filepath, &content, NULL, error))
        return NULL;

    vimban_parse_frontmatter (content, &mapping, &body);
    if (!mapping)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "No frontmatter in %s", filepath);
        return NULL;
    }

    ticket = g_new0 (VimbanTicket, 1);
    ticket->filepath = g_strdup (filepath);
    ticket->effort = -1;

    /* required fields */
    ticket->id    = _yaml_get_string (mapping, "id");
    ticket->title = _yaml_get_string (mapping, "title");
    ticket->type  = _yaml_get_string (mapping, "type");
    ticket->status = _yaml_get_string (mapping, "status");
    ticket->created = _yaml_get_datetime (mapping, "created");

    /* check status requirement based on type */
    status_required = TRUE;
    if (ticket->type)
    {
        if (g_strcmp0 (ticket->type, "resource") == 0)
            status_required = FALSE;
        else
        {
            const VimbanSpecTypeConfig *spec =
                vimban_get_spec_type_config (ticket->type);
            if (spec && !spec->has_status)
                status_required = FALSE;
        }
    }

    /* validate required fields */
    if (!ticket->id || !ticket->id[0] ||
        !ticket->title || !ticket->type || !ticket->created)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "Missing required fields in %s", filepath);
        yaml_mapping_unref (mapping);
        vimban_ticket_free (ticket);
        return NULL;
    }

    if (status_required && (!ticket->status || !ticket->status[0]))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "Missing status in %s", filepath);
        yaml_mapping_unref (mapping);
        vimban_ticket_free (ticket);
        return NULL;
    }

    /* dates */
    if (yaml_mapping_has_member (mapping, "start_date"))
        ticket->start_date = vimban_parse_date_field (
            yaml_mapping_get_string_member (mapping, "start_date"));
    if (yaml_mapping_has_member (mapping, "due_date"))
        ticket->due_date = vimban_parse_date_field (
            yaml_mapping_get_string_member (mapping, "due_date"));
    if (yaml_mapping_has_member (mapping, "end_date"))
        ticket->end_date = vimban_parse_date_field (
            yaml_mapping_get_string_member (mapping, "end_date"));

    /* people */
    assignee_str = yaml_mapping_has_member (mapping, "assignee")
        ? yaml_mapping_get_string_member (mapping, "assignee") : NULL;
    reporter_str = yaml_mapping_has_member (mapping, "reporter")
        ? yaml_mapping_get_string_member (mapping, "reporter") : NULL;

    ticket->assignee = vimban_transclusion_link_parse (assignee_str);
    ticket->reporter = vimban_transclusion_link_parse (reporter_str);
    ticket->watchers = _parse_link_list (mapping, "watchers");

    /* classification */
    if (yaml_mapping_has_member (mapping, "priority"))
    {
        g_free (ticket->priority);
        ticket->priority = _yaml_get_string (mapping, "priority");
    }
    else
    {
        ticket->priority = g_strdup ("medium");
    }

    if (yaml_mapping_has_member (mapping, "effort"))
        ticket->effort = (gint) yaml_mapping_get_int_member (mapping, "effort");

    ticket->tags = _parse_string_list (mapping, "tags");

    if (yaml_mapping_has_member (mapping, "project"))
        ticket->project = _yaml_get_string (mapping, "project");
    if (yaml_mapping_has_member (mapping, "sprint"))
        ticket->sprint = _yaml_get_string (mapping, "sprint");

    /* relationships */
    ticket->member_of  = _parse_link_list (mapping, "member_of");
    ticket->relates_to = _parse_link_list (mapping, "relates_to");
    ticket->blocked_by = _parse_link_list (mapping, "blocked_by");
    ticket->blocks     = _parse_link_list (mapping, "blocks");

    /* progress */
    if (yaml_mapping_has_member (mapping, "progress"))
        ticket->progress = (gint) yaml_mapping_get_int_member (mapping, "progress");
    if (yaml_mapping_has_member (mapping, "checklist_total"))
        ticket->checklist_total = (gint) yaml_mapping_get_int_member (mapping, "checklist_total");
    if (yaml_mapping_has_member (mapping, "checklist_done"))
        ticket->checklist_done = (gint) yaml_mapping_get_int_member (mapping, "checklist_done");

    /* metadata */
    ticket->updated = _yaml_get_datetime (mapping, "updated");
    ticket->version = yaml_mapping_has_member (mapping, "version")
        ? (gint) yaml_mapping_get_int_member (mapping, "version") : 1;

    /* external */
    if (yaml_mapping_has_member (mapping, "issue_link"))
        ticket->issue_link = _yaml_get_string (mapping, "issue_link");

    yaml_mapping_unref (mapping);
    return ticket;
}

/*
 * vimban_person_from_file:
 * @filepath: path to the person markdown file
 * @error: (out) (nullable): error location
 *
 * Load a person from a markdown file.
 *
 * Returns: (transfer full) (nullable): new VimbanPerson, or NULL on error
 */
static VimbanPerson *
vimban_person_from_file (const gchar  *filepath,
                          GError      **error)
{
    VimbanPerson *person;
    g_autofree gchar *content = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *base = NULL;
    g_autofree gchar *stem = NULL;
    YamlMapping *mapping = NULL;
    const gchar *manager_str;

    if (!g_file_get_contents (filepath, &content, NULL, error))
        return NULL;

    vimban_parse_frontmatter (content, &mapping, &body);
    if (!mapping)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "No frontmatter in %s", filepath);
        return NULL;
    }

    person = g_new0 (VimbanPerson, 1);
    person->filepath = g_strdup (filepath);

    /* name: from frontmatter or derive from filename */
    if (yaml_mapping_has_member (mapping, "name"))
    {
        person->name = _yaml_get_string (mapping, "name");
    }
    else
    {
        /* derive from filename: john_doe.md -> John Doe */
        gchar *p;
        base = g_path_get_basename (filepath);
        stem = g_strdup (base);
        p = g_strrstr (stem, ".md");
        if (p) *p = '\0';

        /* replace underscores with spaces, then title case */
        {
            g_autofree gchar *with_spaces = NULL;
            gchar **words;
            GString *title_case;
            gint i;

            with_spaces = g_strdelimit (g_strdup (stem), "_", ' ');
            words = g_strsplit (with_spaces, " ", -1);
            title_case = g_string_new ("");
            for (i = 0; words[i]; i++)
            {
                if (i > 0) g_string_append_c (title_case, ' ');
                if (words[i][0])
                {
                    g_string_append_c (title_case,
                        g_ascii_toupper (words[i][0]));
                    g_string_append (title_case, words[i] + 1);
                }
            }
            g_strfreev (words);
            person->name = g_string_free (title_case, FALSE);
        }
    }

    /* optional fields */
    if (yaml_mapping_has_member (mapping, "id"))
        person->id = _yaml_get_string (mapping, "id");
    if (yaml_mapping_has_member (mapping, "email"))
        person->email = _yaml_get_string (mapping, "email");
    if (yaml_mapping_has_member (mapping, "slack"))
        person->slack = _yaml_get_string (mapping, "slack");
    if (yaml_mapping_has_member (mapping, "role"))
        person->role = _yaml_get_string (mapping, "role");
    if (yaml_mapping_has_member (mapping, "team"))
        person->team = _yaml_get_string (mapping, "team");

    /* manager */
    manager_str = yaml_mapping_has_member (mapping, "manager")
        ? yaml_mapping_get_string_member (mapping, "manager") : NULL;
    person->manager = vimban_transclusion_link_parse (manager_str);

    /* direct reports */
    person->direct_reports = _parse_link_list (mapping, "direct_reports");

    /* timestamps */
    person->created = _yaml_get_datetime (mapping, "created");
    person->updated = _yaml_get_datetime (mapping, "updated");

    yaml_mapping_unref (mapping);
    return person;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * File discovery — recursive .md scanning
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_scan_tickets:
 * @directory: base directory to scan
 * @include_archived: whether to include 04_archives/
 * @exclude_types: NULL-terminated array of types to skip (e.g., {"person", NULL})
 *
 * Recursively scan for .md files with valid ticket frontmatter.
 *
 * Returns: (transfer full): GPtrArray of VimbanTicket*
 */
static GPtrArray *
vimban_scan_tickets (const gchar  *directory,
                      gboolean      include_archived,
                      const gchar **exclude_types)
{
    GPtrArray *tickets;
    g_autoptr(GFile) dir = NULL;
    g_autoptr(GFileEnumerator) enumerator = NULL;
    g_autoptr(GError) error = NULL;
    GFileInfo *info;

    tickets = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_ticket_free);

    dir = g_file_new_for_path (directory);
    enumerator = g_file_enumerate_children (
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        NULL, &error);

    if (!enumerator)
        return tickets;

    while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
    {
        const gchar *name = g_file_info_get_name (info);
        GFileType ftype = g_file_info_get_file_type (info);
        g_autofree gchar *child_path = g_build_filename (directory, name, NULL);

        if (ftype == G_FILE_TYPE_DIRECTORY)
        {
            /* skip hidden dirs and .vimban */
            if (name[0] == '.' || g_strcmp0 (name, ".vimban") == 0)
            {
                g_object_unref (info);
                continue;
            }
            /* skip archives unless requested */
            if (!include_archived && g_strcmp0 (name, "04_archives") == 0)
            {
                g_object_unref (info);
                continue;
            }

            /* recurse */
            {
                g_autoptr(GPtrArray) sub = vimban_scan_tickets (
                    child_path, include_archived, exclude_types);
                /* steal all elements from sub into tickets */
                while (sub->len > 0)
                {
                    g_ptr_array_add (tickets,
                        g_ptr_array_steal_index (sub, 0));
                }
            }
        }
        else if (ftype == G_FILE_TYPE_REGULAR &&
                 g_str_has_suffix (name, ".md") &&
                 name[0] != '.')
        {
            g_autoptr(GError) terr = NULL;
            VimbanTicket *ticket = vimban_ticket_from_file (child_path, &terr);

            if (ticket)
            {
                /* skip excluded types */
                gboolean skip = FALSE;
                if (exclude_types && ticket->type)
                {
                    gint j;
                    for (j = 0; exclude_types[j]; j++)
                    {
                        if (g_strcmp0 (ticket->type, exclude_types[j]) == 0)
                        {
                            skip = TRUE;
                            break;
                        }
                    }
                }

                if (skip)
                    vimban_ticket_free (ticket);
                else
                    g_ptr_array_add (tickets, ticket);
            }
        }

        g_object_unref (info);
    }

    return tickets;
}

/*
 * vimban_scan_people:
 * @people_dir: path to people directory
 *
 * Scan a directory for person .md files.
 *
 * Returns: (transfer full): GPtrArray of VimbanPerson*
 */
static GPtrArray *
vimban_scan_people (const gchar *people_dir)
{
    GPtrArray *people;
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    people = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_person_free);

    dir = g_dir_open (people_dir, 0, NULL);
    if (!dir)
        return people;

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *path = NULL;
        g_autoptr(GError) error = NULL;
        VimbanPerson *person;

        if (!g_str_has_suffix (name, ".md") || name[0] == '.')
            continue;

        path = g_build_filename (people_dir, name, NULL);
        person = vimban_person_from_file (path, &error);
        if (person)
            g_ptr_array_add (people, person);
    }

    return people;
}

/*
 * vimban_find_ticket:
 * @directory: base directory
 * @ticket_id: ID to find (case-insensitive, partial-ID, or file path)
 * @prefix: (nullable): project prefix for partial-ID resolution (e.g. "PROJ")
 *
 * Find a single ticket by ID, partial numeric ID, or file path.
 *
 * Accepts:
 * - Full ID: "PROJ-00042" (case-insensitive)
 * - Partial numeric ID: "42" → expanded to PREFIX-00042 when prefix is given
 * - File path: any string containing "/" or ending with ".md"
 *
 * Returns: (transfer full) (nullable): ticket or NULL
 */
static VimbanTicket *
vimban_find_ticket (const gchar *directory,
                     const gchar *ticket_id,
                     const gchar *prefix)
{
    g_autoptr(GPtrArray) tickets = NULL;
    g_autofree gchar *id_upper = NULL;
    g_autofree gchar *normalized_id = NULL;
    guint i;

    if (!ticket_id || !ticket_id[0])
        return NULL;

    /* handle file path references: contains "/" or ends with ".md" */
    if (strchr (ticket_id, '/') || g_str_has_suffix (ticket_id, ".md"))
    {
        g_autofree gchar *abs_path = NULL;
        g_autoptr(GError) err = NULL;

        if (g_path_is_absolute (ticket_id))
            abs_path = g_strdup (ticket_id);
        else
            abs_path = g_build_filename (directory, ticket_id, NULL);

        if (g_file_test (abs_path, G_FILE_TEST_EXISTS))
            return vimban_ticket_from_file (abs_path, &err);

        return NULL;
    }

    /* handle partial numeric ID: "42" → "PROJ-00042" */
    {
        gboolean all_digits = TRUE;
        const gchar *p;

        for (p = ticket_id; *p; p++)
        {
            if (!g_ascii_isdigit (*p))
            {
                all_digits = FALSE;
                break;
            }
        }

        if (all_digits && prefix)
        {
            gint num = atoi (ticket_id);
            normalized_id = g_strdup_printf ("%s-%05d", prefix, num);
        }
        else
        {
            normalized_id = g_strdup (ticket_id);
        }
    }

    id_upper = g_ascii_strup (normalized_id, -1);
    tickets = vimban_scan_tickets (directory, TRUE, NULL);

    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        g_autofree gchar *t_upper = g_ascii_strup (t->id, -1);

        if (g_strcmp0 (t_upper, id_upper) == 0)
        {
            /* steal from array so it's not freed */
            return (VimbanTicket *) g_ptr_array_steal_index (tickets, i);
        }
    }

    return NULL;
}

/*
 * vimban_find_person:
 * @directory: base directory
 * @config: project config
 * @name_or_id: name or ID to search for (case-insensitive)
 *
 * Find a person by name or ID with fuzzy matching.
 *
 * Returns: (transfer full) (nullable): person or NULL
 */
static VimbanPerson *
vimban_find_person (const gchar      *directory,
                     const VimbanConfig *config,
                     const gchar      *name_or_id)
{
    g_autofree gchar *people_path = NULL;
    g_autoptr(GPtrArray) people = NULL;
    g_autofree gchar *search_lower = NULL;
    guint i;
    VimbanPerson *best = NULL;

    people_path = g_build_filename (directory, config->people_dir, NULL);
    people = vimban_scan_people (people_path);
    search_lower = g_ascii_strdown (name_or_id, -1);

    /* exact match first */
    for (i = 0; i < people->len; i++)
    {
        VimbanPerson *p = g_ptr_array_index (people, i);
        g_autofree gchar *name_lower = g_ascii_strdown (p->name, -1);

        if (g_strcmp0 (name_lower, search_lower) == 0)
            return (VimbanPerson *) g_ptr_array_steal_index (people, i);

        if (p->id)
        {
            g_autofree gchar *id_lower = g_ascii_strdown (p->id, -1);
            if (g_strcmp0 (id_lower, search_lower) == 0)
                return (VimbanPerson *) g_ptr_array_steal_index (people, i);
        }
    }

    /* partial match */
    for (i = 0; i < people->len; i++)
    {
        VimbanPerson *p = g_ptr_array_index (people, i);
        g_autofree gchar *name_lower = g_ascii_strdown (p->name, -1);

        if (strstr (name_lower, search_lower))
        {
            if (!best)
                best = p;
            else
                return NULL;  /* ambiguous match */
        }
    }

    if (best)
    {
        /* find and steal from array */
        for (i = 0; i < people->len; i++)
        {
            if (g_ptr_array_index (people, i) == best)
                return (VimbanPerson *) g_ptr_array_steal_index (people, i);
        }
    }

    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Comment parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_extract_section:
 * @content: full file content
 * @section: section name (e.g., "COMMENTS", "DASHBOARD")
 *
 * Extract content between <!-- VIMBAN:section:START --> and END markers.
 *
 * Returns: (transfer full) (nullable): section content or NULL
 */
static gchar *
vimban_extract_section (const gchar *content,
                         const gchar *section)
{
    g_autofree gchar *pattern_str = NULL;
    g_autoptr(GRegex) re = NULL;
    g_autoptr(GMatchInfo) info = NULL;

    pattern_str = g_strdup_printf (
        "<!-- VIMBAN:%s:START -->(.*?)<!-- VIMBAN:%s:END -->",
        section, section);
    re = g_regex_new (pattern_str, G_REGEX_DOTALL, 0, NULL);

    if (!re || !g_regex_match (re, content, 0, &info))
        return NULL;

    return g_strstrip (g_match_info_fetch (info, 1));
}

/*
 * vimban_replace_section:
 * @content: full file content
 * @section: section name
 * @new_content: replacement content
 *
 * Replace content between VIMBAN section markers.
 *
 * Returns: (transfer full): updated content
 */
static gchar *
vimban_replace_section (const gchar *content,
                         const gchar *section,
                         const gchar *new_content)
{
    g_autofree gchar *pattern_str = NULL;
    g_autofree gchar *replacement = NULL;
    g_autoptr(GRegex) re = NULL;

    pattern_str = g_strdup_printf (
        "(<!-- VIMBAN:%s:START -->)(.*?)(<!-- VIMBAN:%s:END -->)",
        section, section);
    re = g_regex_new (pattern_str, G_REGEX_DOTALL, 0, NULL);

    replacement = g_strdup_printf ("\\1\n\n%s\n\n\\3", new_content);
    return g_regex_replace (re, content, -1, 0, replacement, 0, NULL);
}

/*
 * vimban_parse_comments:
 * @content: full file content
 *
 * Parse all comments from the COMMENTS section of a file.
 *
 * Returns: (transfer full): GPtrArray of VimbanComment*
 */
static GPtrArray *
vimban_parse_comments (const gchar *content)
{
    GPtrArray *comments;
    g_autofree gchar *section = NULL;
    g_autoptr(GRegex) re_header = NULL;
    g_autoptr(GRegex) re_reply = NULL;
    gchar **lines;
    VimbanComment *current = NULL;
    GString *cur_content;
    gboolean in_reply = FALSE;
    GDateTime *reply_ts = NULL;
    gchar *reply_author = NULL;
    GString *reply_buf;
    guint i;

    comments = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_comment_free);

    section = vimban_extract_section (content, "COMMENTS");
    if (!section)
        return comments;

    re_header = g_regex_new (COMMENT_HEADER_RE, G_REGEX_MULTILINE, 0, NULL);
    re_reply  = g_regex_new (REPLY_HEADER_RE, G_REGEX_MULTILINE, 0, NULL);
    cur_content = g_string_new ("");
    reply_buf   = g_string_new ("");

    lines = g_strsplit (section, "\n", -1);

    for (i = 0; lines[i]; i++)
    {
        const gchar *line = lines[i];
        g_autoptr(GMatchInfo) h_info = NULL;
        g_autoptr(GMatchInfo) r_info = NULL;

        /* check for comment header */
        if (g_regex_match (re_header, line, 0, &h_info))
        {
            /* save previous comment */
            if (current)
            {
                if (in_reply && reply_ts)
                {
                    VimbanCommentReply *r = g_new0 (VimbanCommentReply, 1);
                    r->timestamp = g_steal_pointer (&reply_ts);
                    r->author    = g_steal_pointer (&reply_author);
                    r->content   = g_strstrip (g_strdup (reply_buf->str));
                    g_ptr_array_add (current->replies, r);
                }
                current->content = g_strstrip (g_strdup (cur_content->str));
                g_ptr_array_add (comments, current);
            }

            /* start new comment */
            {
                g_autofree gchar *id_str = g_match_info_fetch (h_info, 1);
                g_autofree gchar *author  = g_match_info_fetch (h_info, 2);
                g_autofree gchar *ts_str  = g_match_info_fetch (h_info, 3);
                GDateTime *ts;

                current = g_new0 (VimbanComment, 1);
                current->id = (gint) g_ascii_strtoll (id_str, NULL, 10);
                current->replies = g_ptr_array_new_with_free_func (
                    (GDestroyNotify) vimban_comment_reply_free);

                if (author && author[0])
                    current->author = g_strstrip (g_strdup (author));

                ts = g_date_time_new_from_iso8601 (ts_str, NULL);
                current->timestamp = ts ? ts : g_date_time_new_now_local ();
            }

            g_string_truncate (cur_content, 0);
            g_string_truncate (reply_buf, 0);
            in_reply = FALSE;
            g_clear_pointer (&reply_ts, g_date_time_unref);
            g_free (reply_author);
            reply_author = NULL;
            continue;
        }

        /* check for reply header */
        if (g_regex_match (re_reply, line, 0, &r_info) && current)
        {
            /* save previous reply if any */
            if (in_reply && reply_ts)
            {
                VimbanCommentReply *r = g_new0 (VimbanCommentReply, 1);
                r->timestamp = g_steal_pointer (&reply_ts);
                r->author    = g_steal_pointer (&reply_author);
                r->content   = g_strstrip (g_strdup (reply_buf->str));
                g_ptr_array_add (current->replies, r);
            }

            {
                g_autofree gchar *author = g_match_info_fetch (r_info, 1);
                g_autofree gchar *ts_str = g_match_info_fetch (r_info, 2);

                reply_author = (author && author[0])
                    ? g_strstrip (g_strdup (author)) : NULL;
                reply_ts = g_date_time_new_from_iso8601 (ts_str, NULL);
                if (!reply_ts)
                    reply_ts = g_date_time_new_now_local ();
            }

            in_reply = TRUE;
            g_string_truncate (reply_buf, 0);
            continue;
        }

        /* accumulate content */
        if (current)
        {
            if (in_reply)
            {
                if (g_str_has_prefix (line, ">> "))
                    g_string_append_printf (reply_buf, "%s\n", line + 3);
                else if (g_str_has_prefix (line, ">>"))
                    g_string_append_printf (reply_buf, "%s\n", line + 2);
                else if (line[0] == '\0')
                    g_string_append_c (reply_buf, '\n');
            }
            else
            {
                if (g_str_has_prefix (line, "> "))
                    g_string_append_printf (cur_content, "%s\n", line + 2);
                else if (g_str_has_prefix (line, ">"))
                    g_string_append_printf (cur_content, "%s\n", line + 1);
                else if (line[0] == '\0')
                    g_string_append_c (cur_content, '\n');
            }
        }
    }

    /* save last comment */
    if (current)
    {
        if (in_reply && reply_ts)
        {
            VimbanCommentReply *r = g_new0 (VimbanCommentReply, 1);
            r->timestamp = g_steal_pointer (&reply_ts);
            r->author    = g_steal_pointer (&reply_author);
            r->content   = g_strstrip (g_strdup (reply_buf->str));
            g_ptr_array_add (current->replies, r);
        }
        current->content = g_strstrip (g_strdup (cur_content->str));
        g_ptr_array_add (comments, current);
    }

    g_strfreev (lines);
    g_string_free (cur_content, TRUE);
    g_string_free (reply_buf, TRUE);
    g_clear_pointer (&reply_ts, g_date_time_unref);
    g_free (reply_author);

    return comments;
}

/*
 * vimban_get_next_comment_id:
 * @content: full file content
 *
 * Get the next available comment ID.
 *
 * Returns: next ID (1 if no comments exist)
 */
static gint
vimban_get_next_comment_id (const gchar *content)
{
    g_autoptr(GPtrArray) comments = NULL;
    gint max_id = 0;
    guint i;

    comments = vimban_parse_comments (content);
    for (i = 0; i < comments->len; i++)
    {
        VimbanComment *c = g_ptr_array_index (comments, i);
        if (c->id > max_id)
            max_id = c->id;
    }

    return max_id + 1;
}

/*
 * vimban_ensure_comment_section:
 * @content: full file content
 *
 * Add VIMBAN:COMMENTS markers if missing.
 *
 * Returns: (transfer full): content with markers
 */
static gchar *
vimban_ensure_comment_section (const gchar *content)
{
    if (strstr (content, "<!-- VIMBAN:COMMENTS:START -->"))
        return g_strdup (content);

    return g_strdup_printf ("%s%s\n## Comments\n\n"
                             "<!-- VIMBAN:COMMENTS:START -->\n\n"
                             "<!-- VIMBAN:COMMENTS:END -->\n",
                             content,
                             g_str_has_suffix (content, "\n") ? "" : "\n");
}

/*
 * vimban_insert_comment:
 * @filepath: path to markdown file
 * @text: comment text
 * @reply_to: comment ID to reply to, or -1 for new comment
 * @author: (nullable): author name
 * @error: (out) (nullable): error location
 *
 * Insert a comment or reply into a file.
 *
 * Returns: comment ID on success, -1 on error
 */
static gint
vimban_insert_comment (const gchar  *filepath,
                        const gchar  *text,
                        gint          reply_to,
                        const gchar  *author,
                        GError      **error)
{
    g_autofree gchar *raw_content = NULL;
    g_autofree gchar *content = NULL;
    g_autofree gchar *author_part = NULL;
    g_autofree gchar *timestamp = NULL;
    g_autoptr(GDateTime) now = NULL;
    GString *result;
    gchar **text_lines;
    GString *formatted;
    guint i;

    if (!g_file_get_contents (filepath, &raw_content, NULL, error))
        return -1;

    content = vimban_ensure_comment_section (raw_content);

    now = g_date_time_new_now_local ();
    timestamp = g_date_time_format_iso8601 (now);
    author_part = author ? g_strdup_printf (" by %s", author)
                         : g_strdup ("");

    /* format text lines with > or >> prefix */
    text_lines = g_strsplit (text, "\n", -1);
    formatted = g_string_new ("");

    if (reply_to >= 0)
    {
        /* reply: use >> prefix */
        for (i = 0; text_lines[i]; i++)
        {
            if (text_lines[i][0])
                g_string_append_printf (formatted, ">> %s\n", text_lines[i]);
            else
                g_string_append (formatted, ">>\n");
        }

        /* find parent comment and insert reply after it */
        {
            g_autofree gchar *reply_md = g_strdup_printf (
                "\n#### Reply%s (%s)\n\n%s",
                author_part, timestamp, formatted->str);

            /* find end of parent comment (next ### or end marker) */
            g_autofree gchar *pattern_str = g_strdup_printf (
                "(### Comment #%d(?: by [^\\(]+)? \\([^)]+\\).*?)"
                "(?=### Comment #|<!-- VIMBAN:COMMENTS:END -->)",
                reply_to);
            g_autoptr(GRegex) re = g_regex_new (pattern_str, G_REGEX_DOTALL, 0, NULL);
            g_autoptr(GMatchInfo) info = NULL;

            if (g_regex_match (re, content, 0, &info))
            {
                gint end_pos;
                g_match_info_fetch_pos (info, 0, NULL, &end_pos);

                result = g_string_new ("");
                g_string_append_len (result, content, end_pos);
                g_string_append (result, reply_md);
                g_string_append (result, content + end_pos);

                {
                    g_autoptr(GError) write_err = NULL;
                    if (!g_file_set_contents (filepath, result->str, -1, &write_err))
                        g_printerr ("vimban: failed to write %s: %s\n",
                                     filepath, write_err->message);
                }
                g_string_free (result, TRUE);
            }
            else
            {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "Comment #%d not found", reply_to);
                g_strfreev (text_lines);
                g_string_free (formatted, TRUE);
                return -1;
            }
        }

        g_strfreev (text_lines);
        g_string_free (formatted, TRUE);
        return reply_to;
    }
    else
    {
        /* new comment: use > prefix */
        gint new_id;

        for (i = 0; text_lines[i]; i++)
        {
            if (text_lines[i][0])
                g_string_append_printf (formatted, "> %s\n", text_lines[i]);
            else
                g_string_append (formatted, ">\n");
        }

        new_id = vimban_get_next_comment_id (content);

        {
            g_autofree gchar *comment_md = g_strdup_printf (
                "\n\n### Comment #%d%s (%s)\n\n%s\n",
                new_id, author_part, timestamp, formatted->str);

            g_autofree gchar *new_content = NULL;
            GString *buf = g_string_new (content);
            gchar *marker = strstr (buf->str, "<!-- VIMBAN:COMMENTS:END -->");
            if (marker)
            {
                gsize pos = marker - buf->str;
                g_string_insert (buf, (gssize) pos, comment_md);
            }
            new_content = g_string_free (buf, FALSE);

            {
                g_autoptr(GError) write_err = NULL;
                if (!g_file_set_contents (filepath, new_content, -1, &write_err))
                    g_printerr ("vimban: failed to write %s: %s\n",
                                 filepath, write_err->message);
            }
            g_free (new_content);
        }

        g_strfreev (text_lines);
        g_string_free (formatted, TRUE);
        return new_id;
    }
}

/*
 * vimban_parse_comment_range:
 * @range_str: range specification (e.g., "1,3-5,9" or "all")
 * @max_id: maximum comment ID
 *
 * Parse a comment range string into an array of IDs.
 *
 * Returns: (transfer full): GArray of gint
 */
static GArray *
vimban_parse_comment_range (const gchar *range_str,
                             gint         max_id)
{
    GArray *ids;
    gchar **parts;
    guint i;

    ids = g_array_new (FALSE, FALSE, sizeof (gint));

    if (g_ascii_strcasecmp (range_str, "all") == 0)
    {
        gint j;
        for (j = 1; j <= max_id; j++)
            g_array_append_val (ids, j);
        return ids;
    }

    parts = g_strsplit (range_str, ",", -1);
    for (i = 0; parts[i]; i++)
    {
        g_autofree gchar *part = g_strstrip (g_strdup (parts[i]));
        if (!part[0]) continue;

        if (strchr (part, '-'))
        {
            gchar **range = g_strsplit (part, "-", 2);
            if (range[0] && range[1])
            {
                gint start = (gint) g_ascii_strtoll (range[0], NULL, 10);
                gint end   = (gint) g_ascii_strtoll (range[1], NULL, 10);
                gint j;
                for (j = start; j <= end && j <= max_id; j++)
                {
                    if (j >= 1)
                        g_array_append_val (ids, j);
                }
            }
            g_strfreev (range);
        }
        else
        {
            gint id = (gint) g_ascii_strtoll (part, NULL, 10);
            if (id >= 1 && id <= max_id)
                g_array_append_val (ids, id);
        }
    }

    g_strfreev (parts);

    /* sort and deduplicate */
    /* insertion sort (small arrays, simple) */
    {
        guint j, k;
        for (j = 1; j < ids->len; j++)
        {
            gint key = g_array_index (ids, gint, j);
            k = j;
            while (k > 0 && g_array_index (ids, gint, k - 1) > key)
            {
                g_array_index (ids, gint, k) = g_array_index (ids, gint, k - 1);
                k--;
            }
            g_array_index (ids, gint, k) = key;
        }
    }
    /* simple dedup since sorted */
    for (i = 1; i < ids->len; )
    {
        if (g_array_index (ids, gint, i) == g_array_index (ids, gint, i - 1))
            g_array_remove_index (ids, i);
        else
            i++;
    }

    return ids;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Output formatting
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ANSI color codes for statuses */
static const gchar *
_status_color (const gchar *status)
{
    if (!status)          return "";
    if (g_no_color)       return "";
    if (g_strcmp0 (status, "backlog") == 0)     return "\033[2m";
    if (g_strcmp0 (status, "ready") == 0)       return "\033[94m";
    if (g_strcmp0 (status, "in_progress") == 0) return "\033[96m";
    if (g_strcmp0 (status, "blocked") == 0)     return "\033[91m";
    if (g_strcmp0 (status, "review") == 0)      return "\033[93m";
    if (g_strcmp0 (status, "delegated") == 0)   return "\033[95m";
    if (g_strcmp0 (status, "done") == 0)        return "\033[92m";
    if (g_strcmp0 (status, "cancelled") == 0)   return "\033[2m";
    return "";
}

static const gchar *
_priority_color (const gchar *priority)
{
    if (!priority)      return "";
    if (g_no_color)     return "";
    if (g_strcmp0 (priority, "critical") == 0)  return "\033[91m";
    if (g_strcmp0 (priority, "high") == 0)      return "\033[93m";
    if (g_strcmp0 (priority, "low") == 0)       return "\033[2m";
    return "";
}

static const gchar *
_end_color (void)
{
    return g_no_color ? "" : "\033[0m";
}

/*
 * vimban_ticket_get_field:
 * @ticket: the ticket
 * @field: field name
 *
 * Get a ticket field value as string for display.
 *
 * Returns: (transfer full): field value string
 */
static gchar *
vimban_ticket_get_field (const VimbanTicket *ticket,
                          const gchar        *field)
{
    if (g_strcmp0 (field, "id") == 0)
        return g_strdup (ticket->id ? ticket->id : "");
    if (g_strcmp0 (field, "title") == 0)
        return g_strdup (ticket->title ? ticket->title : "");
    if (g_strcmp0 (field, "type") == 0)
        return g_strdup (ticket->type ? ticket->type : "");
    if (g_strcmp0 (field, "status") == 0)
        return g_strdup (ticket->status ? ticket->status : "");
    if (g_strcmp0 (field, "priority") == 0)
        return g_strdup (ticket->priority ? ticket->priority : "medium");
    if (g_strcmp0 (field, "assignee") == 0)
        return vimban_transclusion_link_get_stem (ticket->assignee);
    if (g_strcmp0 (field, "due_date") == 0)
        return ticket->due_date ? vimban_date_to_string (ticket->due_date)
                                : g_strdup ("");
    if (g_strcmp0 (field, "project") == 0)
        return g_strdup (ticket->project ? ticket->project : "");
    if (g_strcmp0 (field, "tags") == 0)
        return ticket->tags ? g_strjoinv (",", ticket->tags) : g_strdup ("");
    if (g_strcmp0 (field, "progress") == 0)
        return g_strdup_printf ("%d", ticket->progress);
    if (g_strcmp0 (field, "filepath") == 0)
        return g_strdup (ticket->filepath ? ticket->filepath : "");
    if (g_strcmp0 (field, "issue_link") == 0)
        return g_strdup (ticket->issue_link ? ticket->issue_link : "");

    return g_strdup ("");
}

/*
 * vimban_ticket_to_json_node:
 * @ticket: the ticket
 *
 * Convert a ticket to a JsonNode for json-glib output.
 *
 * Returns: (transfer full): new JsonNode
 */
static JsonNode *
vimban_ticket_to_json_node (const VimbanTicket *ticket)
{
    JsonBuilder *b = json_builder_new ();
    g_autofree gchar *assignee = NULL;
    g_autofree gchar *due = NULL;
    g_autofree gchar *created_str = NULL;
    g_autofree gchar *updated_str = NULL;

    json_builder_begin_object (b);

    json_builder_set_member_name (b, "id");
    json_builder_add_string_value (b, ticket->id ? ticket->id : "");

    json_builder_set_member_name (b, "title");
    json_builder_add_string_value (b, ticket->title ? ticket->title : "");

    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, ticket->type ? ticket->type : "");

    json_builder_set_member_name (b, "status");
    json_builder_add_string_value (b, ticket->status ? ticket->status : "");

    json_builder_set_member_name (b, "priority");
    json_builder_add_string_value (b, ticket->priority ? ticket->priority : "medium");

    assignee = vimban_transclusion_link_get_stem (ticket->assignee);
    json_builder_set_member_name (b, "assignee");
    json_builder_add_string_value (b, assignee);

    due = ticket->due_date ? vimban_date_to_string (ticket->due_date) : g_strdup ("");
    json_builder_set_member_name (b, "due_date");
    json_builder_add_string_value (b, due);

    json_builder_set_member_name (b, "project");
    json_builder_add_string_value (b, ticket->project ? ticket->project : "");

    /* tags array */
    json_builder_set_member_name (b, "tags");
    json_builder_begin_array (b);
    if (ticket->tags)
    {
        gint i;
        for (i = 0; ticket->tags[i]; i++)
            json_builder_add_string_value (b, ticket->tags[i]);
    }
    json_builder_end_array (b);

    json_builder_set_member_name (b, "progress");
    json_builder_add_int_value (b, ticket->progress);

    json_builder_set_member_name (b, "filepath");
    json_builder_add_string_value (b, ticket->filepath ? ticket->filepath : "");

    json_builder_set_member_name (b, "issue_link");
    json_builder_add_string_value (b, ticket->issue_link ? ticket->issue_link : "");

    if (ticket->created)
    {
        created_str = g_date_time_format_iso8601 (ticket->created);
        json_builder_set_member_name (b, "created");
        json_builder_add_string_value (b, created_str);
    }

    if (ticket->updated)
    {
        updated_str = g_date_time_format_iso8601 (ticket->updated);
        json_builder_set_member_name (b, "updated");
        json_builder_add_string_value (b, updated_str);
    }

    json_builder_end_object (b);

    {
        JsonNode *node = json_builder_get_root (b);
        g_object_unref (b);
        return node;
    }
}

/*
 * vimban_person_to_json_node:
 * @person: the person
 *
 * Convert a person to a JsonNode.
 *
 * Returns: (transfer full): new JsonNode
 */
static JsonNode *
vimban_person_to_json_node (const VimbanPerson *person)
{
    JsonBuilder *b = json_builder_new ();

    json_builder_begin_object (b);

    json_builder_set_member_name (b, "id");
    json_builder_add_string_value (b, person->id ? person->id : "");

    json_builder_set_member_name (b, "name");
    json_builder_add_string_value (b, person->name ? person->name : "");

    json_builder_set_member_name (b, "email");
    json_builder_add_string_value (b, person->email ? person->email : "");

    json_builder_set_member_name (b, "role");
    json_builder_add_string_value (b, person->role ? person->role : "");

    json_builder_set_member_name (b, "team");
    json_builder_add_string_value (b, person->team ? person->team : "");

    json_builder_set_member_name (b, "filepath");
    json_builder_add_string_value (b, person->filepath ? person->filepath : "");

    json_builder_end_object (b);

    {
        JsonNode *node = json_builder_get_root (b);
        g_object_unref (b);
        return node;
    }
}

/*
 * vimban_format_output_plain:
 * @tickets: GPtrArray of VimbanTicket*
 * @columns: NULL-terminated column names
 * @no_header: skip header row
 *
 * Format tickets as aligned plain-text table with colors.
 *
 * Returns: (transfer full): formatted string
 */
static gchar *
vimban_format_output_plain (const GPtrArray  *tickets,
                             const gchar     **columns,
                             gboolean          no_header)
{
    GString *out;
    gint *widths;
    gint num_cols, c;
    guint i;

    if (!columns) columns = DEFAULT_COLUMNS;

    /* count columns */
    for (num_cols = 0; columns[num_cols]; num_cols++);

    /* calculate column widths */
    widths = g_new0 (gint, num_cols);
    for (c = 0; c < num_cols; c++)
        widths[c] = (gint) strlen (columns[c]);

    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        for (c = 0; c < num_cols; c++)
        {
            g_autofree gchar *val = vimban_ticket_get_field (t, columns[c]);
            gint len = (gint) strlen (val);
            if (len > widths[c])
                widths[c] = len;
        }
    }

    out = g_string_new ("");

    /* header */
    if (!no_header)
    {
        for (c = 0; c < num_cols; c++)
        {
            g_autofree gchar *upper = g_ascii_strup (columns[c], -1);
            if (c > 0) g_string_append (out, "  ");
            if (!g_no_color)
                g_string_append_printf (out, "\033[1m%-*s\033[0m",
                                         widths[c], upper);
            else
                g_string_append_printf (out, "%-*s", widths[c], upper);
        }
        g_string_append_c (out, '\n');
    }

    /* data rows */
    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        for (c = 0; c < num_cols; c++)
        {
            g_autofree gchar *val = vimban_ticket_get_field (t, columns[c]);
            const gchar *color = "";
            const gchar *end = "";

            if (g_strcmp0 (columns[c], "status") == 0)
                color = _status_color (val);
            else if (g_strcmp0 (columns[c], "priority") == 0)
                color = _priority_color (val);

            if (color[0]) end = _end_color ();

            if (c > 0) g_string_append (out, "  ");
            g_string_append_printf (out, "%s%-*s%s",
                                     color, widths[c], val, end);
        }
        g_string_append_c (out, '\n');
    }

    g_free (widths);
    return g_string_free (out, FALSE);
}

/*
 * vimban_format_output_md:
 * @tickets: GPtrArray of VimbanTicket*
 * @columns: NULL-terminated column names
 * @no_header: skip header row
 *
 * Format tickets as markdown table.
 *
 * Returns: (transfer full): formatted string
 */
static gchar *
vimban_format_output_md (const GPtrArray  *tickets,
                          const gchar     **columns,
                          gboolean          no_header)
{
    GString *out;
    gint c, num_cols;
    guint i;

    if (!columns) columns = DEFAULT_COLUMNS;
    for (num_cols = 0; columns[num_cols]; num_cols++);

    out = g_string_new ("");

    /* compile pipe-escape regex once, not per-cell */
    g_autoptr(GRegex) pipe_re = g_regex_new ("\\|", 0, 0, NULL);

    if (!no_header)
    {
        /* header row */
        g_string_append (out, "| ");
        for (c = 0; c < num_cols; c++)
        {
            if (c > 0) g_string_append (out, " | ");
            g_string_append (out, columns[c]);
        }
        g_string_append (out, " |\n");

        /* separator */
        g_string_append_c (out, '|');
        for (c = 0; c < num_cols; c++)
            g_string_append (out, "---|");
        g_string_append_c (out, '\n');
    }

    /* data rows */
    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        g_string_append (out, "| ");
        for (c = 0; c < num_cols; c++)
        {
            g_autofree gchar *val = vimban_ticket_get_field (t, columns[c]);
            /* escape pipe chars */
            g_autofree gchar *escaped = pipe_re
                ? g_regex_replace_literal (pipe_re, val, -1, 0, "\\|", 0, NULL)
                : NULL;
            if (c > 0) g_string_append (out, " | ");
            g_string_append (out, escaped ? escaped : val);
        }
        g_string_append (out, " |\n");
    }

    return g_string_free (out, FALSE);
}

/*
 * vimban_format_output_json:
 * @tickets: GPtrArray of VimbanTicket*
 *
 * Format tickets as JSON array.
 *
 * Returns: (transfer full): JSON string
 */
static gchar *
vimban_format_output_json (const GPtrArray *tickets)
{
    g_autoptr(JsonGenerator) gen = NULL;
    JsonNode *root;
    JsonBuilder *b;
    guint i;

    b = json_builder_new ();
    json_builder_begin_array (b);

    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        JsonNode *node = vimban_ticket_to_json_node (t);
        json_builder_add_value (b, node);
    }

    json_builder_end_array (b);
    root = json_builder_get_root (b);
    g_object_unref (b);

    gen = json_generator_new ();
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_indent (gen, 2);
    json_generator_set_root (gen, root);

    {
        gchar *str = json_generator_to_data (gen, NULL);
        json_node_unref (root);
        return str;
    }
}

/*
 * vimban_format_output_yaml:
 * @tickets: GPtrArray of VimbanTicket*
 * @columns: NULL-terminated column names
 *
 * Format tickets as YAML.
 *
 * Returns: (transfer full): YAML string
 */
static gchar *
vimban_format_output_yaml (const GPtrArray  *tickets,
                            const gchar     **columns)
{
    g_autoptr(YamlGenerator) gen = NULL;
    YamlNode *root;
    YamlSequence *seq;
    guint i;
    gint c, num_cols;

    if (!columns) columns = DEFAULT_COLUMNS;
    for (num_cols = 0; columns[num_cols]; num_cols++);

    seq = yaml_sequence_new ();

    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);
        YamlMapping *m = yaml_mapping_new ();

        for (c = 0; c < num_cols; c++)
        {
            g_autofree gchar *val = vimban_ticket_get_field (t, columns[c]);
            yaml_mapping_set_string_member (m, columns[c], val);
        }

        yaml_sequence_add_mapping_element (seq, m);
        yaml_mapping_unref (m);
    }

    root = yaml_node_new_sequence (seq);
    gen = yaml_generator_new ();
    yaml_generator_set_root (gen, root);

    {
        gchar *str = yaml_generator_to_data (gen, NULL, NULL);
        return str;
    }
}

/*
 * vimban_format_output:
 * @tickets: GPtrArray of VimbanTicket*
 * @format: output format enum
 * @columns: (nullable): column list (defaults to DEFAULT_COLUMNS)
 * @no_header: skip header for plain/md
 *
 * Format tickets for display in the requested format.
 *
 * Returns: (transfer full): formatted string
 */
static gchar *
vimban_format_output (const GPtrArray   *tickets,
                       VimbanFormat       format,
                       const gchar      **columns,
                       gboolean           no_header)
{
    if (!tickets || tickets->len == 0)
        return g_strdup ("");

    switch (format)
    {
    case VIMBAN_FORMAT_JSON:
        return vimban_format_output_json (tickets);
    case VIMBAN_FORMAT_YAML:
        return vimban_format_output_yaml (tickets, columns);
    case VIMBAN_FORMAT_MD:
        return vimban_format_output_md (tickets, columns, no_header);
    default:
        return vimban_format_output_plain (tickets, columns, no_header);
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Ticket filtering and sorting
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_filter_tickets:
 * @tickets: input array (not modified)
 * @status: (nullable): comma-separated status filter
 * @type: (nullable): comma-separated type filter
 * @priority: (nullable): priority filter
 * @assignee: (nullable): assignee substring filter
 * @project: (nullable): project filter
 * @tag: (nullable): tag filter
 * @scope: (nullable): "work" or "personal"
 * @mine: if TRUE, match assignee to $USER
 * @overdue: if TRUE, filter to overdue tickets
 * @unassigned: if TRUE, filter to unassigned tickets
 *
 * Filter tickets based on various criteria (AND logic).
 *
 * Returns: (transfer full): filtered GPtrArray (tickets not owned, do not free items)
 */
static GPtrArray *
vimban_filter_tickets (const GPtrArray *tickets,
                        const gchar     *status,
                        const gchar     *type,
                        const gchar     *priority,
                        const gchar     *assignee,
                        const gchar     *project,
                        const gchar     *tag,
                        const gchar     *scope,
                        gboolean         mine,
                        gboolean         overdue,
                        gboolean         unassigned)
{
    GPtrArray *result;
    guint i;
    g_autoptr(GDateTime) now_dt = NULL;
    GDate today_date;

    result = g_ptr_array_new ();
    now_dt = g_date_time_new_now_local ();
    g_date_set_dmy (&today_date,
        (GDateDay) g_date_time_get_day_of_month (now_dt),
        (GDateMonth) g_date_time_get_month (now_dt),
        (GDateYear) g_date_time_get_year (now_dt));

    for (i = 0; i < tickets->len; i++)
    {
        VimbanTicket *t = g_ptr_array_index (tickets, i);

        /* status filter */
        if (status && status[0])
        {
            gchar **statuses = g_strsplit (status, ",", -1);
            gboolean match = FALSE;
            gint j;
            for (j = 0; statuses[j]; j++)
            {
                if (g_strcmp0 (g_strstrip (statuses[j]), t->status) == 0)
                { match = TRUE; break; }
            }
            g_strfreev (statuses);
            if (!match) continue;
        }

        /* type filter */
        if (type && type[0])
        {
            gchar **types = g_strsplit (type, ",", -1);
            gboolean match = FALSE;
            gint j;
            for (j = 0; types[j]; j++)
            {
                g_autofree gchar *norm =
                    vimban_normalize_type_name (g_strstrip (types[j]));
                if (g_strcmp0 (norm, t->type) == 0)
                { match = TRUE; break; }
            }
            g_strfreev (types);
            if (!match) continue;
        }

        /* priority */
        if (priority && priority[0])
        {
            if (g_strcmp0 (priority, t->priority) != 0) continue;
        }

        /* assignee */
        if (assignee && assignee[0])
        {
            if (!t->assignee) continue;
            {
                g_autofree gchar *link_str = vimban_transclusion_link_to_string (t->assignee);
                g_autofree gchar *a_lower = g_ascii_strdown (link_str, -1);
                g_autofree gchar *f_lower = g_ascii_strdown (assignee, -1);
                if (!strstr (a_lower, f_lower)) continue;
            }
        }

        /* project */
        if (project && project[0])
        {
            if (g_strcmp0 (project, t->project) != 0) continue;
        }

        /* tag */
        if (tag && tag[0] && t->tags)
        {
            gboolean found = FALSE;
            gint j;
            for (j = 0; t->tags[j]; j++)
            {
                if (g_strcmp0 (tag, t->tags[j]) == 0)
                { found = TRUE; break; }
            }
            if (!found) continue;
        }

        /* scope */
        if (scope && scope[0] && t->filepath)
        {
            if (g_strcmp0 (scope, "work") == 0)
            {
                if (!strstr (t->filepath, "01_projects/work/") &&
                    !strstr (t->filepath, "02_areas/work/"))
                    continue;
            }
            else if (g_strcmp0 (scope, "personal") == 0)
            {
                if (!strstr (t->filepath, "01_projects/personal/") &&
                    !strstr (t->filepath, "02_areas/personal/"))
                    continue;
            }
        }

        /* mine */
        if (mine)
        {
            const gchar *user = g_getenv ("USER");
            if (!user) user = g_getenv ("USERNAME");
            if (!t->assignee || !user) continue;
            {
                g_autofree gchar *stem =
                    vimban_transclusion_link_get_stem (t->assignee);
                g_autofree gchar *stem_lower = g_ascii_strdown (stem, -1);
                g_autofree gchar *user_lower = g_ascii_strdown (user, -1);
                if (!strstr (stem_lower, user_lower)) continue;
            }
        }

        /* overdue */
        if (overdue)
        {
            if (!t->due_date) continue;
            if (g_date_compare (t->due_date, &today_date) >= 0) continue;
        }

        /* unassigned */
        if (unassigned)
        {
            if (t->assignee) continue;
        }

        g_ptr_array_add (result, t);
    }

    return result;
}

/*
 * _cmp_priority:
 *
 * Return numeric value for priority (higher = more urgent).
 */
static gint
_priority_rank (const gchar *priority)
{
    if (!priority) return 1;
    if (g_strcmp0 (priority, "critical") == 0) return 4;
    if (g_strcmp0 (priority, "high") == 0)     return 3;
    if (g_strcmp0 (priority, "medium") == 0)   return 2;
    if (g_strcmp0 (priority, "low") == 0)      return 1;
    return 0;
}

/*
 * _cmp_status:
 *
 * Return numeric value for status position in workflow.
 */
static gint
_status_rank (const gchar *status)
{
    gint i;
    for (i = 0; STATUSES[i]; i++)
    {
        if (g_strcmp0 (status, STATUSES[i]) == 0)
            return i;
    }
    return 99;
}

/* sort comparison functions */
static gint
_sort_by_due_date (gconstpointer a, gconstpointer b)
{
    const VimbanTicket *ta = *(const VimbanTicket **) a;
    const VimbanTicket *tb = *(const VimbanTicket **) b;

    if (!ta->due_date && !tb->due_date) return 0;
    if (!ta->due_date) return 1;   /* NULL last */
    if (!tb->due_date) return -1;
    return g_date_compare (ta->due_date, tb->due_date);
}

static gint
_sort_by_priority (gconstpointer a, gconstpointer b)
{
    const VimbanTicket *ta = *(const VimbanTicket **) a;
    const VimbanTicket *tb = *(const VimbanTicket **) b;

    return _priority_rank (tb->priority) - _priority_rank (ta->priority);
}

static gint
_sort_by_status (gconstpointer a, gconstpointer b)
{
    const VimbanTicket *ta = *(const VimbanTicket **) a;
    const VimbanTicket *tb = *(const VimbanTicket **) b;

    return _status_rank (ta->status) - _status_rank (tb->status);
}

static gint
_sort_by_created (gconstpointer a, gconstpointer b)
{
    const VimbanTicket *ta = *(const VimbanTicket **) a;
    const VimbanTicket *tb = *(const VimbanTicket **) b;

    if (!ta->created && !tb->created) return 0;
    if (!ta->created) return 1;
    if (!tb->created) return -1;
    return g_date_time_compare (ta->created, tb->created);
}

static gint
_sort_by_title (gconstpointer a, gconstpointer b)
{
    const VimbanTicket *ta = *(const VimbanTicket **) a;
    const VimbanTicket *tb = *(const VimbanTicket **) b;

    return g_strcmp0 (ta->title, tb->title);
}

/*
 * vimban_sort_tickets:
 * @tickets: GPtrArray to sort in-place
 * @sort_by: field name to sort by
 * @reverse: whether to reverse the sort
 */
static void
vimban_sort_tickets (GPtrArray    *tickets,
                      const gchar  *sort_by,
                      gboolean      reverse)
{
    GCompareFunc cmp;

    if (!sort_by || g_strcmp0 (sort_by, "due_date") == 0)
        cmp = _sort_by_due_date;
    else if (g_strcmp0 (sort_by, "priority") == 0)
        cmp = _sort_by_priority;
    else if (g_strcmp0 (sort_by, "status") == 0)
        cmp = _sort_by_status;
    else if (g_strcmp0 (sort_by, "created") == 0)
        cmp = _sort_by_created;
    else if (g_strcmp0 (sort_by, "title") == 0)
        cmp = _sort_by_title;
    else
        cmp = _sort_by_due_date;

    g_ptr_array_sort (tickets, cmp);

    if (reverse)
    {
        /* reverse in-place */
        guint i, j;
        for (i = 0, j = tickets->len - 1; i < j; i++, j--)
        {
            gpointer tmp = tickets->pdata[i];
            tickets->pdata[i] = tickets->pdata[j];
            tickets->pdata[j] = tmp;
        }
    }
}

/*
 * vimban_is_valid_transition:
 * @from: current status
 * @to: target status
 *
 * Check if a status transition is valid.
 *
 * Returns: TRUE if valid
 */
static gboolean
vimban_is_valid_transition (const gchar *from,
                             const gchar *to)
{
    gint i, j;

    for (i = 0; VALID_TRANSITIONS[i].from_status != NULL; i++)
    {
        if (g_strcmp0 (VALID_TRANSITIONS[i].from_status, from) == 0)
        {
            for (j = 0; VALID_TRANSITIONS[i].to_statuses[j] != NULL; j++)
            {
                if (g_strcmp0 (VALID_TRANSITIONS[i].to_statuses[j], to) == 0)
                    return TRUE;
            }
            return FALSE;
        }
    }

    return FALSE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Person reference resolution
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_resolve_person_ref:
 * @ref: person reference (name, filename, or transclusion link)
 * @directory: base directory
 * @config: project config
 *
 * Resolve a person reference to a transclusion link.
 *
 * Returns: (transfer full) (nullable): link or NULL
 */
static VimbanTransclusionLink *
vimban_resolve_person_ref (const gchar        *ref,
                            const gchar        *directory,
                            const VimbanConfig *config)
{
    g_autofree gchar *people_path = NULL;
    g_autoptr(GPtrArray) people = NULL;
    g_autofree gchar *ref_lower = NULL;
    guint i;

    if (!ref || !ref[0])
        return NULL;

    /* already a transclusion */
    if (g_str_has_prefix (ref, "![["))
        return vimban_transclusion_link_parse (ref);

    people_path = g_build_filename (directory, config->people_dir, NULL);
    if (!g_file_test (people_path, G_FILE_TEST_IS_DIR))
        return NULL;

    people = vimban_scan_people (people_path);
    ref_lower = g_ascii_strdown (ref, -1);

    /* exact filename match */
    for (i = 0; i < people->len; i++)
    {
        VimbanPerson *p = g_ptr_array_index (people, i);
        g_autofree gchar *base = g_path_get_basename (p->filepath);
        g_autofree gchar *stem = g_strdup (base);
        gchar *dot = g_strrstr (stem, ".md");
        if (dot) *dot = '\0';

        if (g_strcmp0 (stem, ref) == 0)
        {
            g_autofree gchar *rel = NULL;
            /* make relative path from directory */
            if (g_str_has_prefix (p->filepath, directory))
                rel = g_strdup (p->filepath + strlen (directory) + 1);
            else
                rel = g_strdup (p->filepath);

            {
                g_autofree gchar *link_str = g_strdup_printf ("![[%s]]", rel);
                return vimban_transclusion_link_parse (link_str);
            }
        }
    }

    /* name match */
    for (i = 0; i < people->len; i++)
    {
        VimbanPerson *p = g_ptr_array_index (people, i);
        g_autofree gchar *name_lower = g_ascii_strdown (p->name, -1);

        if (g_strcmp0 (name_lower, ref_lower) == 0 ||
            strstr (name_lower, ref_lower))
        {
            g_autofree gchar *rel = NULL;
            if (g_str_has_prefix (p->filepath, directory))
                rel = g_strdup (p->filepath + strlen (directory) + 1);
            else
                rel = g_strdup (p->filepath);

            {
                g_autofree gchar *link_str = g_strdup_printf ("![[%s]]", rel);
                return vimban_transclusion_link_parse (link_str);
            }
        }
    }

    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Subcommand handler stubs
 *
 * Each handler is a function with signature:
 *   gint cmd_xxx (gint argc, gchar **argv,
 *                 VimbanGlobalOpts *global, VimbanConfig *config)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Subcommand handler signature */
typedef gint (*VimbanCmdFunc) (gint          argc,
                                gchar       **argv,
                                VimbanGlobalOpts *global,
                                VimbanConfig     *config);

/* stub macro for unimplemented commands */
#define VIMBAN_CMD_STUB(name)                                        \
    static gint                                                      \
    cmd_##name (gint          argc,                                  \
                gchar       **argv,                                  \
                VimbanGlobalOpts *global,                             \
                VimbanConfig     *config)                             \
    {                                                                \
        (void) argc; (void) argv; (void) global; (void) config;     \
        g_printerr ("Error: command '%s' not yet implemented\n",     \
                     #name);                                         \
        return VIMBAN_EXIT_GENERAL_ERROR;                             \
    }

/*
 * cmd_init:
 * @argc: argument count
 * @argv: argument values
 * @global: global options
 * @config: project config
 *
 * Initialize vimban in a directory. Creates .vimban/config.yaml,
 * .vimban/.sequence, and updates .gitignore.
 */
static gint
cmd_init (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *vimban_dir_path = NULL;
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *seq_path = NULL;
    g_autofree gchar *gitignore_path = NULL;
    g_autofree gchar *gitignore_content = NULL;
    gboolean opt_force = FALSE;
    gchar *opt_prefix = NULL;
    gchar *opt_people_dir = NULL;
    gboolean opt_no_git = FALSE;
    const gchar *target_dir;

    GOptionEntry entries[] = {
        { "force",      'f', 0, G_OPTION_ARG_NONE,   &opt_force,
          "Force reinitialize", NULL },
        { "prefix",     'p', 0, G_OPTION_ARG_STRING, &opt_prefix,
          "ID prefix (default: PROJ)", "PREFIX" },
        { "people-dir", 0,   0, G_OPTION_ARG_STRING, &opt_people_dir,
          "People directory", "DIR" },
        { "no-git",     0,   0, G_OPTION_ARG_NONE,   &opt_no_git,
          "Skip .gitignore update", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("- Initialize vimban");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* use directory from global opts or config */
    target_dir = config->directory;

    vimban_dir_path = g_build_filename (target_dir, CONFIG_DIR_NAME, NULL);

    /* check if already initialized */
    if (g_file_test (vimban_dir_path, G_FILE_TEST_IS_DIR) && !opt_force)
    {
        g_autofree gchar *msg = g_strdup_printf (
            "vimban already initialized in %s (use --force to reinitialize)",
            target_dir);
        vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    /* create .vimban directory */
    if (g_mkdir_with_parents (vimban_dir_path, 0755) != 0)
    {
        g_autofree gchar *msg = g_strdup_printf (
            "Failed to create directory: %s", vimban_dir_path);
        vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    /* write config.yaml */
    config_path = g_build_filename (vimban_dir_path, CONFIG_FILE_NAME, NULL);
    {
        g_autofree gchar *cfg = g_strdup_printf (
            "prefix: %s\n"
            "people_dir: %s\n"
            "default_status: backlog\n"
            "default_priority: medium\n",
            opt_prefix ? opt_prefix : DEFAULT_PREFIX,
            opt_people_dir ? opt_people_dir : DEFAULT_PEOPLE_DIR);
        g_file_set_contents (config_path, cfg, -1, NULL);
    }

    /* create sequence file if missing */
    seq_path = g_build_filename (vimban_dir_path, SEQUENCE_FILE_NAME, NULL);
    if (!g_file_test (seq_path, G_FILE_TEST_EXISTS))
        g_file_set_contents (seq_path, "0", -1, NULL);

    /* update .gitignore */
    if (!opt_no_git)
    {
        gitignore_path = g_build_filename (target_dir, ".gitignore", NULL);
        if (g_file_test (gitignore_path, G_FILE_TEST_EXISTS))
        {
            g_file_get_contents (gitignore_path, &gitignore_content, NULL, NULL);
            if (gitignore_content && !strstr (gitignore_content, ".vimban/.sequence"))
            {
                g_autofree gchar *appended = g_strdup_printf (
                    "%s\n.vimban/.sequence\n", gitignore_content);
                g_file_set_contents (gitignore_path, appended, -1, NULL);
            }
        }
        else
        {
            g_file_set_contents (gitignore_path, ".vimban/.sequence\n", -1, NULL);
        }
    }

    g_print ("Initialized vimban in %s\n", target_dir);
    g_print ("  Prefix: %s\n", opt_prefix ? opt_prefix : DEFAULT_PREFIX);
    g_print ("  People dir: %s\n",
             opt_people_dir ? opt_people_dir : DEFAULT_PEOPLE_DIR);

    g_free (opt_prefix);
    g_free (opt_people_dir);

    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_create:
 * @argc: argument count
 * @argv: argument values
 * @global: global options
 * @config: project config
 *
 * Create a new ticket of the given type. Generates ID, fills template,
 * writes file, and opens editor unless --no-edit.
 */
static gint
cmd_create (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *ticket_id = NULL;
    g_autofree gchar *safe_title = NULL;
    g_autofree gchar *output_path = NULL;
    g_autofree gchar *tmpl = NULL;
    g_autofree gchar *content = NULL;
    g_autofree gchar *now_str = NULL;
    g_autoptr(GDateTime) now = NULL;
    g_autoptr(GHashTable) replacements = NULL;
    VimbanTransclusionLink *assignee_link = NULL;
    VimbanTransclusionLink *reporter_link = NULL;
    g_autofree gchar *assignee_str = NULL;
    g_autofree gchar *reporter_str = NULL;
    g_autofree gchar *type_str = NULL;
    g_autofree gchar *vimban_dir = NULL;
    g_autofree gchar *tmpl_name = NULL;
    const gchar *type_arg;
    const gchar *title_arg;

    gchar *opt_id        = NULL;
    gchar *opt_prefix    = NULL;
    gchar *opt_assignee  = NULL;
    gchar *opt_reporter  = NULL;
    gchar *opt_priority  = NULL;
    gchar *opt_tags      = NULL;
    gchar *opt_project   = NULL;
    gchar *opt_due       = NULL;
    gchar *opt_output    = NULL;
    gboolean opt_no_edit  = FALSE;
    gboolean opt_dry_run  = FALSE;

    GOptionEntry entries[] = {
        { "id",       0,   0, G_OPTION_ARG_STRING, &opt_id,
          "Custom ID", "ID" },
        { "prefix",   0,   0, G_OPTION_ARG_STRING, &opt_prefix,
          "ID prefix override", "PREFIX" },
        { "assignee", 'a', 0, G_OPTION_ARG_STRING, &opt_assignee,
          "Assignee name or reference", "NAME" },
        { "reporter", 'r', 0, G_OPTION_ARG_STRING, &opt_reporter,
          "Reporter name or reference", "NAME" },
        { "priority", 'p', 0, G_OPTION_ARG_STRING, &opt_priority,
          "Priority (critical/high/medium/low)", "PRI" },
        { "tags",     't', 0, G_OPTION_ARG_STRING, &opt_tags,
          "Tags (comma-separated)", "TAGS" },
        { "project",  'P', 0, G_OPTION_ARG_STRING, &opt_project,
          "Project name", "PROJ" },
        { "due",       0,  0, G_OPTION_ARG_STRING, &opt_due,
          "Due date (ISO or relative)", "DATE" },
        { "output",   'o', 0, G_OPTION_ARG_STRING, &opt_output,
          "Output path override", "PATH" },
        { "no-edit",   0,  0, G_OPTION_ARG_NONE,   &opt_no_edit,
          "Skip opening editor", NULL },
        { "dry-run",   0,  0, G_OPTION_ARG_NONE,   &opt_dry_run,
          "Preview without writing", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("<type> [title] - Create a ticket");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* positional: argv[1]=type, argv[2]=title (optional) */
    if (argc < 2)
    {
        g_printerr ("Error: type required\n");
        g_printerr ("Usage: vimban create <type> [title] [options]\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    type_arg  = argv[1];
    title_arg = (argc >= 3) ? argv[2] : NULL;

    /* normalize type */
    type_str = vimban_normalize_type_name (type_arg);

    /* validate type */
    if (!vimban_is_valid_type (type_str))
    {
        g_autofree gchar *msg = g_strdup_printf ("Unknown ticket type: %s", type_str);
        vimban_error (msg, VIMBAN_EXIT_INVALID_ARGS);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* check initialization */
    vimban_dir = g_build_filename (config->directory, CONFIG_DIR_NAME, NULL);
    if (!g_file_test (vimban_dir, G_FILE_TEST_IS_DIR))
    {
        g_autofree gchar *msg = g_strdup_printf (
            "vimban not initialized in %s. Run 'vimban init' first.",
            config->directory);
        vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    /* title required for regular types */
    if (!title_arg &&
        !vimban_str_in_list (type_str, (const gchar **) PARA_TYPES) &&
        !vimban_is_specialized_type (type_str))
    {
        vimban_error ("Title is required for regular ticket types",
                      VIMBAN_EXIT_INVALID_ARGS);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* generate ID */
    ticket_id = vimban_next_id (config->directory, opt_id,
                                 opt_prefix, type_str, config);
    if (!ticket_id)
    {
        vimban_error ("Failed to generate ticket ID (sequence lock failed)",
                      VIMBAN_EXIT_GENERAL_ERROR);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    /* resolve people */
    if (opt_assignee && opt_assignee[0])
        assignee_link = vimban_resolve_person_ref (opt_assignee,
                                                    config->directory, config);
    if (opt_reporter && opt_reporter[0])
        reporter_link = vimban_resolve_person_ref (opt_reporter,
                                                    config->directory, config);

    assignee_str = assignee_link
        ? vimban_transclusion_link_to_string (assignee_link)
        : g_strdup ("");
    reporter_str = reporter_link
        ? vimban_transclusion_link_to_string (reporter_link)
        : g_strdup ("");

    /* load template */
    tmpl_name = g_strdup_printf ("%s.md", type_str);
    tmpl = vimban_load_template (tmpl_name);
    if (!tmpl)
    {
        /* fallback: build a minimal frontmatter */
        now = g_date_time_new_now_local ();
        now_str = g_date_time_format_iso8601 (now);
        tmpl = g_strdup_printf (
            "---\n"
            "id: {{ID}}\n"
            "title: {{TITLE}}\n"
            "type: %s\n"
            "status: backlog\n"
            "priority: {{PRIORITY}}\n"
            "assignee: {{ASSIGNEE}}\n"
            "reporter: {{REPORTER}}\n"
            "created: %s\n"
            "---\n\n",
            type_str, now_str);
    }

    /* fill template */
    now = g_date_time_new_now_local ();
    now_str = g_date_time_format_iso8601 (now);

    replacements = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          NULL, g_free);
    g_hash_table_insert (replacements, (gpointer) "{{ID}}",
                         g_strdup (ticket_id));
    g_hash_table_insert (replacements, (gpointer) "{{TITLE}}",
                         g_strdup (title_arg ? title_arg : ""));
    g_hash_table_insert (replacements, (gpointer) "{{TYPE}}",
                         g_strdup (type_str));
    g_hash_table_insert (replacements, (gpointer) "{{STATUS}}",
                         g_strdup (config->default_status));
    g_hash_table_insert (replacements, (gpointer) "{{PRIORITY}}",
                         g_strdup (opt_priority ? opt_priority
                                                : config->default_priority));
    g_hash_table_insert (replacements, (gpointer) "{{ASSIGNEE}}",
                         g_strdup (assignee_str));
    g_hash_table_insert (replacements, (gpointer) "{{REPORTER}}",
                         g_strdup (reporter_str));
    g_hash_table_insert (replacements, (gpointer) "{{CREATED}}",
                         g_strdup (now_str));
    g_hash_table_insert (replacements, (gpointer) "{{UPDATED}}",
                         g_strdup (now_str));
    g_hash_table_insert (replacements, (gpointer) "{{TAGS}}",
                         g_strdup (opt_tags ? opt_tags : ""));
    g_hash_table_insert (replacements, (gpointer) "{{PROJECT}}",
                         g_strdup (opt_project ? opt_project : ""));

    /* due date */
    if (opt_due && opt_due[0])
    {
        g_autoptr(GDate) due = vimban_parse_date (opt_due);
        g_autofree gchar *due_str = vimban_date_to_string (due);
        g_hash_table_insert (replacements, (gpointer) "{{DUE_DATE}}",
                             g_strdup (due_str));
    }
    else
    {
        g_hash_table_insert (replacements, (gpointer) "{{DUE_DATE}}",
                             g_strdup (""));
    }

    /* date shortcut for today */
    {
        g_autoptr(GDate) today = g_date_new ();
        g_date_set_time_t (today, time (NULL));
        g_autofree gchar *today_str = vimban_date_to_string (today);
        g_hash_table_insert (replacements, (gpointer) "{{DATE}}",
                             g_strdup (today_str));
    }

    content = vimban_fill_template (tmpl, replacements);

    /* determine output path */
    if (opt_output && opt_output[0])
    {
        if (g_path_is_absolute (opt_output))
            output_path = g_strdup (opt_output);
        else
            output_path = g_build_filename (config->directory, opt_output, NULL);
    }
    else
    {
        /* build default path under 01_projects/work or personal */
        const gchar *scope = global->personal ? "personal" : "work";
        safe_title = vimban_sanitize_filename (title_arg ? title_arg : type_str, 50);
        g_autofree gchar *filename = g_strdup_printf ("%s_%s.md",
                                                       ticket_id, safe_title);
        output_path = g_build_filename (config->directory,
                                         "01_projects", scope,
                                         type_str,
                                         filename, NULL);
    }

    /* dry run */
    if (opt_dry_run)
    {
        g_print ("=== DRY RUN ===\n");
        g_print ("Would create: %s\n", output_path);
        g_print ("ID: %s\n", ticket_id);
        g_print ("Content:\n%s\n", content);
        goto cleanup;
    }

    /* write file */
    {
        g_autofree gchar *parent = g_path_get_dirname (output_path);
        g_mkdir_with_parents (parent, 0755);
    }

    if (!g_file_set_contents (output_path, content, -1, &error))
    {
        g_autofree gchar *msg = g_strdup_printf (
            "Failed to write file: %s", error->message);
        vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
        goto cleanup_err;
    }

    /* open editor */
    if (!opt_no_edit)
    {
        const gchar *editor = g_getenv ("EDITOR");
        if (!editor || !editor[0]) editor = "vi";
        {
            const gchar *spawn_argv[] = { editor, output_path, NULL };
            g_spawn_sync (NULL, (gchar **) spawn_argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                          NULL, NULL, NULL, NULL, NULL, NULL);
        }
    }

    /* output result */
    switch (global->format)
    {
    case VIMBAN_FORMAT_JSON:
        g_print ("{\"id\": \"%s\", \"path\": \"%s\"}\n",
                 ticket_id, output_path);
        break;
    case VIMBAN_FORMAT_YAML:
        g_print ("id: %s\npath: %s\n", ticket_id, output_path);
        break;
    default:
        g_print ("Created %s: %s\n", ticket_id, output_path);
        break;
    }

cleanup:
    vimban_transclusion_link_free (assignee_link);
    vimban_transclusion_link_free (reporter_link);
    g_free (opt_id);
    g_free (opt_prefix);
    g_free (opt_assignee);
    g_free (opt_reporter);
    g_free (opt_priority);
    g_free (opt_tags);
    g_free (opt_project);
    g_free (opt_due);
    g_free (opt_output);
    return VIMBAN_EXIT_SUCCESS;

cleanup_err:
    vimban_transclusion_link_free (assignee_link);
    vimban_transclusion_link_free (reporter_link);
    g_free (opt_id);
    g_free (opt_prefix);
    g_free (opt_assignee);
    g_free (opt_reporter);
    g_free (opt_priority);
    g_free (opt_tags);
    g_free (opt_project);
    g_free (opt_due);
    g_free (opt_output);
    return VIMBAN_EXIT_GENERAL_ERROR;
}

/*
 * vimban_get_search_paths:
 * @config: vimban config
 * @work: TRUE for work scope only
 * @personal: TRUE for personal scope only
 *
 * Get search paths for ticket scanning. Searches 01_projects/work and/or
 * 01_projects/personal subdirectories (matching Python behavior).
 *
 * Returns: (transfer full): NULL-terminated array of directory paths
 */
static gchar **
vimban_get_search_paths (VimbanConfig *config,
                          gboolean      work,
                          gboolean      personal)
{
    GPtrArray *paths;

    paths = g_ptr_array_new ();

    if (work)
    {
        g_autofree gchar *p = g_build_filename (
            config->directory, "01_projects", "work", NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (paths, g_strdup (p));
    }
    else if (personal)
    {
        g_autofree gchar *p = g_build_filename (
            config->directory, "01_projects", "personal", NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (paths, g_strdup (p));
    }
    else
    {
        g_autofree gchar *pw = g_build_filename (
            config->directory, "01_projects", "work", NULL);
        g_autofree gchar *pp = g_build_filename (
            config->directory, "01_projects", "personal", NULL);
        if (g_file_test (pw, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (paths, g_strdup (pw));
        if (g_file_test (pp, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (paths, g_strdup (pp));
    }

    g_ptr_array_add (paths, NULL);
    return (gchar **) g_ptr_array_free (paths, FALSE);
}


/*
 * vimban_get_para_search_paths:
 * @config: vimban config
 * @work: TRUE for work scope only
 * @personal: TRUE for personal scope only
 * @include_areas: whether to include 02_areas paths
 * @include_resources: whether to include 03_resources paths
 *
 * Get PARA (areas/resources) search paths.
 *
 * Returns: (transfer full): NULL-terminated array of directory paths
 */
static gchar **
vimban_get_para_search_paths (VimbanConfig *config,
                               gboolean      work,
                               gboolean      personal,
                               gboolean      include_areas,
                               gboolean      include_resources)
{
    GPtrArray *paths;
    const gchar *scopes[3] = { NULL, NULL, NULL };
    gint nscopes = 0;

    paths = g_ptr_array_new ();

    if (work)
    {
        scopes[0] = "work";
        nscopes = 1;
    }
    else if (personal)
    {
        scopes[0] = "personal";
        nscopes = 1;
    }
    else
    {
        scopes[0] = "work";
        scopes[1] = "personal";
        nscopes = 2;
    }

    {
        gint i;
        for (i = 0; i < nscopes; i++)
        {
            if (include_areas)
            {
                g_autofree gchar *p = g_build_filename (
                    config->directory, "02_areas", scopes[i], NULL);
                if (g_file_test (p, G_FILE_TEST_IS_DIR))
                    g_ptr_array_add (paths, g_strdup (p));
            }
            if (include_resources)
            {
                g_autofree gchar *p = g_build_filename (
                    config->directory, "03_resources", scopes[i], NULL);
                if (g_file_test (p, G_FILE_TEST_IS_DIR))
                    g_ptr_array_add (paths, g_strdup (p));
            }
        }
    }

    g_ptr_array_add (paths, NULL);
    return (gchar **) g_ptr_array_free (paths, FALSE);
}


/*
 * cmd_list:
 *
 * List tickets with filters, sorting, and output formatting.
 * Searches 01_projects/{work,personal} by default. Excludes person, area,
 * and resource types unless --areas or --resources flags are used.
 */
static gint
cmd_list (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) all_tickets = NULL;
    GPtrArray *filtered = NULL;
    g_autofree gchar *output = NULL;
    const gchar **columns = NULL;
    g_autofree gchar *scope = NULL;
    g_auto(GStrv) search_paths = NULL;
    g_auto(GStrv) para_paths = NULL;
    GPtrArray *exclude_list;
    gchar **exclude_types;
    gint i;

    /* command-specific options */
    gchar *opt_status    = NULL;
    gchar *opt_type      = NULL;
    gchar *opt_assignee  = NULL;
    gchar *opt_project   = NULL;
    gchar *opt_priority  = NULL;
    gchar *opt_tag       = NULL;
    gchar *opt_sort      = NULL;
    gchar *opt_columns   = NULL;
    gboolean opt_mine       = FALSE;
    gboolean opt_overdue    = FALSE;
    gboolean opt_blocked    = FALSE;
    gboolean opt_unassigned = FALSE;
    gboolean opt_no_header  = FALSE;
    gboolean opt_reverse    = FALSE;
    gboolean opt_areas      = FALSE;
    gboolean opt_resources  = FALSE;

    GOptionEntry entries[] = {
        { "status",    's', 0, G_OPTION_ARG_STRING, &opt_status,
          "Filter status (comma-sep)", "STATUS" },
        { "type",      't', 0, G_OPTION_ARG_STRING, &opt_type,
          "Filter type (comma-sep)", "TYPE" },
        { "assignee",  'a', 0, G_OPTION_ARG_STRING, &opt_assignee,
          "Filter assignee", "NAME" },
        { "project",   'P', 0, G_OPTION_ARG_STRING, &opt_project,
          "Filter project", "PROJECT" },
        { "priority",   0,  0, G_OPTION_ARG_STRING, &opt_priority,
          "Filter priority", "PRIORITY" },
        { "tag",        0,  0, G_OPTION_ARG_STRING, &opt_tag,
          "Filter tag", "TAG" },
        { "sort",       0,  0, G_OPTION_ARG_STRING, &opt_sort,
          "Sort field", "FIELD" },
        { "columns",    0,  0, G_OPTION_ARG_STRING, &opt_columns,
          "Columns (comma-sep)", "COLS" },
        { "mine",       0,  0, G_OPTION_ARG_NONE,   &opt_mine,
          "Assigned to $USER", NULL },
        { "overdue",    0,  0, G_OPTION_ARG_NONE,   &opt_overdue,
          "Only overdue", NULL },
        { "blocked",    0,  0, G_OPTION_ARG_NONE,   &opt_blocked,
          "Only blocked", NULL },
        { "unassigned", 0,  0, G_OPTION_ARG_NONE,   &opt_unassigned,
          "Only unassigned", NULL },
        { "no-header",  0,  0, G_OPTION_ARG_NONE,   &opt_no_header,
          "Omit header", NULL },
        { "reverse",    0,  0, G_OPTION_ARG_NONE,   &opt_reverse,
          "Reverse sort", NULL },
        { "areas",      0,  0, G_OPTION_ARG_NONE,   &opt_areas,
          "Include area types", NULL },
        { "resources",  0,  0, G_OPTION_ARG_NONE,   &opt_resources,
          "Include resource types", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("- List tickets");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* auto-include PARA paths when filtering by specialized types */
    if (opt_type)
    {
        g_auto(GStrv) type_parts = g_strsplit (opt_type, ",", -1);
        if (type_parts && type_parts[0])
        {
            g_autofree gchar *norm = vimban_normalize_type_name (type_parts[0]);
            const VimbanSpecTypeConfig *spec = vimban_get_spec_type_config (norm);
            if (spec)
            {
                if (g_strcmp0 (spec->parent, "area") == 0)
                    opt_areas = TRUE;
                else if (g_strcmp0 (spec->parent, "resource") == 0)
                    opt_resources = TRUE;
            }
        }
    }

    /* build exclude types list */
    exclude_list = g_ptr_array_new ();
    g_ptr_array_add (exclude_list, (gpointer) "person");
    if (!opt_areas)
        g_ptr_array_add (exclude_list, (gpointer) "area");
    if (!opt_resources)
        g_ptr_array_add (exclude_list, (gpointer) "resource");
    g_ptr_array_add (exclude_list, NULL);
    exclude_types = (gchar **) g_ptr_array_free (exclude_list, FALSE);

    /* determine scope from global flags */
    if (global->work)
        scope = g_strdup ("work");
    else if (global->personal)
        scope = g_strdup ("personal");

    /* get search paths: 01_projects/{work,personal} */
    search_paths = vimban_get_search_paths (config,
                                              global->work, global->personal);

    /* get PARA search paths if areas/resources requested */
    para_paths = vimban_get_para_search_paths (config,
                                                global->work, global->personal,
                                                opt_areas, opt_resources);

    /* scan tickets from all search paths */
    all_tickets = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_ticket_free);

    for (i = 0; search_paths[i]; i++)
    {
        g_autoptr(GPtrArray) sub = vimban_scan_tickets (
            search_paths[i], global->archived,
            (const gchar **) exclude_types);
        while (sub->len > 0)
            g_ptr_array_add (all_tickets,
                g_ptr_array_steal_index (sub, 0));
    }

    for (i = 0; para_paths[i]; i++)
    {
        g_autoptr(GPtrArray) sub = vimban_scan_tickets (
            para_paths[i], global->archived,
            (const gchar **) exclude_types);
        while (sub->len > 0)
            g_ptr_array_add (all_tickets,
                g_ptr_array_steal_index (sub, 0));
    }

    /* apply filters */
    if (opt_blocked)
    {
        /* --blocked overrides status filter */
        g_free (opt_status);
        opt_status = g_strdup ("blocked");
    }

    filtered = vimban_filter_tickets (all_tickets,
                                       opt_status, opt_type, opt_priority,
                                       opt_assignee, opt_project, opt_tag,
                                       scope, opt_mine, opt_overdue,
                                       opt_unassigned);

    /* sort */
    vimban_sort_tickets (filtered,
                          opt_sort ? opt_sort : "due_date",
                          opt_reverse);

    /* parse columns */
    if (opt_columns)
    {
        gchar **col_arr = g_strsplit (opt_columns, ",", -1);
        columns = (const gchar **) col_arr;
    }

    /* format and output */
    output = vimban_format_output (filtered, global->format,
                                    columns, opt_no_header);
    if (output && output[0])
        g_print ("%s", output);

    /* cleanup */
    g_ptr_array_unref (filtered);
    g_free (exclude_types);
    g_free (opt_status);
    g_free (opt_type);
    g_free (opt_assignee);
    g_free (opt_project);
    g_free (opt_priority);
    g_free (opt_tag);
    g_free (opt_sort);
    if (columns)
        g_strfreev ((gchar **) columns);

    return VIMBAN_EXIT_SUCCESS;
}
/*
 * cmd_show:
 *
 * Show ticket details. Supports plain, json, yaml, and md formats.
 * --raw prints the raw file, --content prints body only.
 * --links shows relationship fields.
 */
static gint
cmd_show (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(VimbanTicket) ticket = NULL;
    gboolean opt_links   = FALSE;
    gboolean opt_raw     = FALSE;
    gboolean opt_content = FALSE;
    const gchar *ticket_ref;

    GOptionEntry entries[] = {
        { "links",   0, 0, G_OPTION_ARG_NONE, &opt_links,
          "Show relationship links", NULL },
        { "raw",     0, 0, G_OPTION_ARG_NONE, &opt_raw,
          "Print raw file content", NULL },
        { "content", 0, 0, G_OPTION_ARG_NONE, &opt_content,
          "Print body only (no frontmatter)", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> - Show ticket details");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: ticket ID required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    ticket_ref = argv[1];
    ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);

    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    /* --raw: dump full file */
    if (opt_raw)
    {
        g_autofree gchar *raw = NULL;
        g_file_get_contents (ticket->filepath, &raw, NULL, NULL);
        if (raw) g_print ("%s", raw);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* --content: body only */
    if (opt_content)
    {
        g_autofree gchar *raw = NULL;
        g_autofree gchar *body = NULL;
        YamlMapping *mapping = NULL;
        g_file_get_contents (ticket->filepath, &raw, NULL, NULL);
        if (raw)
        {
            vimban_parse_frontmatter (raw, &mapping, &body);
            if (mapping) yaml_mapping_unref (mapping);
            if (body && body[0])
                g_print ("%s\n", body);
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    /* json / yaml / md formats */
    if (global->format == VIMBAN_FORMAT_JSON)
    {
        g_autoptr(JsonGenerator) gen = json_generator_new ();
        JsonNode *node = vimban_ticket_to_json_node (ticket);
        json_generator_set_pretty (gen, TRUE);
        json_generator_set_indent (gen, 2);
        json_generator_set_root (gen, node);
        g_autofree gchar *str = json_generator_to_data (gen, NULL);
        json_node_unref (node);
        g_print ("%s\n", str);
        return VIMBAN_EXIT_SUCCESS;
    }

    if (global->format == VIMBAN_FORMAT_YAML)
    {
        GPtrArray *arr = g_ptr_array_new ();
        g_ptr_array_add (arr, ticket);
        g_autofree gchar *str = vimban_format_output_yaml (arr, NULL);
        g_ptr_array_free (arr, FALSE);
        g_print ("%s", str);
        return VIMBAN_EXIT_SUCCESS;
    }

    if (global->format == VIMBAN_FORMAT_MD)
    {
        g_autofree gchar *raw = NULL;
        g_file_get_contents (ticket->filepath, &raw, NULL, NULL);
        if (raw) g_print ("%s", raw);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* plain format */
    {
        g_autofree gchar *bold_id   = _bold (ticket->id ? ticket->id : "");
        g_autofree gchar *sc        = _status_color (ticket->status) ? g_strdup (_status_color (ticket->status)) : g_strdup ("");
        g_autofree gchar *pc        = _priority_color (ticket->priority) ? g_strdup (_priority_color (ticket->priority)) : g_strdup ("");
        const gchar *end            = _end_color ();
        g_autoptr(GDateTime) now_dt = g_date_time_new_now_local ();
        GDate today;

        g_date_set_dmy (&today,
            (GDateDay) g_date_time_get_day_of_month (now_dt),
            (GDateMonth) g_date_time_get_month (now_dt),
            (GDateYear) g_date_time_get_year (now_dt));

        g_print ("%s: %s\n", bold_id, ticket->title ? ticket->title : "");
        g_print ("  Type: %s\n", ticket->type ? ticket->type : "");
        g_print ("  Status: %s%s%s\n", sc, ticket->status ? ticket->status : "", end);
        g_print ("  Priority: %s%s%s\n", pc, ticket->priority ? ticket->priority : "medium", end);

        if (ticket->assignee)
        {
            g_autofree gchar *stem = vimban_transclusion_link_get_stem (ticket->assignee);
            g_print ("  Assignee: %s\n", stem);
        }
        if (ticket->reporter)
        {
            g_autofree gchar *stem = vimban_transclusion_link_get_stem (ticket->reporter);
            g_print ("  Reporter: %s\n", stem);
        }
        if (ticket->due_date && g_date_valid (ticket->due_date))
        {
            g_autofree gchar *due_str = vimban_date_to_string (ticket->due_date);
            gboolean overdue = g_date_compare (ticket->due_date, &today) < 0;
            if (overdue && !g_no_color)
                g_print ("  Due: \033[31m%s\033[0m\n", due_str);
            else
                g_print ("  Due: %s\n", due_str);
        }
        if (ticket->project && ticket->project[0])
            g_print ("  Project: %s\n", ticket->project);
        if (ticket->tags && ticket->tags[0])
        {
            g_autofree gchar *tag_str = g_strjoinv (", ", ticket->tags);
            g_print ("  Tags: %s\n", tag_str);
        }
        if (ticket->issue_link && ticket->issue_link[0])
            g_print ("  External: %s\n", ticket->issue_link);
        g_print ("  Progress: %d%%\n", ticket->progress);
        g_print ("  File: %s\n", ticket->filepath);

        if (opt_links)
        {
            g_print ("\n");
            if (ticket->member_of && ticket->member_of->len > 0)
            {
                g_print ("  Member of:\n");
                guint i;
                for (i = 0; i < ticket->member_of->len; i++)
                {
                    VimbanTransclusionLink *l = g_ptr_array_index (ticket->member_of, i);
                    g_print ("    - %s\n", l->path ? l->path : "");
                }
            }
            if (ticket->blocked_by && ticket->blocked_by->len > 0)
            {
                g_print ("  Blocked by:\n");
                guint i;
                for (i = 0; i < ticket->blocked_by->len; i++)
                {
                    VimbanTransclusionLink *l = g_ptr_array_index (ticket->blocked_by, i);
                    g_print ("    - %s\n", l->path ? l->path : "");
                }
            }
            if (ticket->blocks && ticket->blocks->len > 0)
            {
                g_print ("  Blocks:\n");
                guint i;
                for (i = 0; i < ticket->blocks->len; i++)
                {
                    VimbanTransclusionLink *l = g_ptr_array_index (ticket->blocks, i);
                    g_print ("    - %s\n", l->path ? l->path : "");
                }
            }
            if (ticket->relates_to && ticket->relates_to->len > 0)
            {
                g_print ("  Related to:\n");
                guint i;
                for (i = 0; i < ticket->relates_to->len; i++)
                {
                    VimbanTransclusionLink *l = g_ptr_array_index (ticket->relates_to, i);
                    g_print ("    - %s\n", l->path ? l->path : "");
                }
            }
        }
    }

    return VIMBAN_EXIT_SUCCESS;
}

/* Lower-priority stubs kept for dispatch table completeness */
VIMBAN_CMD_STUB (generate_link)
VIMBAN_CMD_STUB (get_id)
/*
 * cmd_edit:
 *
 * Edit a ticket's frontmatter fields. If no ticket given, uses fzf.
 * If no field flags given, opens the file in $EDITOR.
 */
static gint
cmd_edit (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar *opt_status    = NULL;
    gchar *opt_priority  = NULL;
    gchar *opt_assignee  = NULL;
    gchar *opt_add_tag   = NULL;
    gchar *opt_remove_tag = NULL;
    gchar *opt_due       = NULL;
    gchar *opt_clear     = NULL;
    gint   opt_progress  = -1;
    gboolean opt_interactive = FALSE;
    gboolean opt_dry_run     = FALSE;
    const gchar *ticket_ref;

    GOptionEntry entries[] = {
        { "status",     's', 0, G_OPTION_ARG_STRING,  &opt_status,
          "Set status", "STATUS" },
        { "priority",   'p', 0, G_OPTION_ARG_STRING,  &opt_priority,
          "Set priority", "PRI" },
        { "assignee",   'a', 0, G_OPTION_ARG_STRING,  &opt_assignee,
          "Set assignee", "NAME" },
        { "add-tag",    0,   0, G_OPTION_ARG_STRING,  &opt_add_tag,
          "Add tag", "TAG" },
        { "remove-tag", 0,   0, G_OPTION_ARG_STRING,  &opt_remove_tag,
          "Remove tag", "TAG" },
        { "progress",   0,   0, G_OPTION_ARG_INT,     &opt_progress,
          "Set progress %", "N" },
        { "due",        0,   0, G_OPTION_ARG_STRING,  &opt_due,
          "Set due date", "DATE" },
        { "clear",      0,   0, G_OPTION_ARG_STRING,  &opt_clear,
          "Clear a field", "FIELD" },
        { "interactive",'i', 0, G_OPTION_ARG_NONE,    &opt_interactive,
          "Open in editor", NULL },
        { "dry-run",    0,   0, G_OPTION_ARG_NONE,    &opt_dry_run,
          "Preview changes", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("[ticket] [field=value...] - Edit ticket");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* determine ticket ref */
    if (argc >= 2)
    {
        ticket_ref = argv[1];
    }
    else
    {
        /* fzf picker */
        g_autoptr(GPtrArray) all = NULL;
        GString *lines_buf;
        gint j;

        all = vimban_scan_tickets (config->directory, FALSE, NULL);
        lines_buf = g_string_new ("");
        for (j = 0; (guint) j < all->len; j++)
        {
            VimbanTicket *t = g_ptr_array_index (all, j);
            g_autofree gchar *asn = vimban_transclusion_link_get_stem (t->assignee);
            g_string_append_printf (lines_buf, "%s\t%s\t%s\t%s\t%s\n",
                t->id ? t->id : "",
                t->status ? t->status : "",
                t->priority ? t->priority : "",
                asn,
                t->title ? t->title : "");
        }

        {
            /* write lines to temp file, pipe through fzf via shell */
            g_autofree gchar *tmpfile = NULL;
            gint tmpfd = g_file_open_tmp ("vimban-fzf-XXXXXX", &tmpfile, NULL);
            if (tmpfd < 0)
            {
                g_string_free (lines_buf, TRUE);
                vimban_error ("Failed to create temp file for fzf", VIMBAN_EXIT_GENERAL_ERROR);
                return VIMBAN_EXIT_GENERAL_ERROR;
            }
            if (write (tmpfd, lines_buf->str, lines_buf->len) < 0)
            {
                /* ignore write error, fzf will see empty input */
            }
            close (tmpfd);
            g_string_free (lines_buf, TRUE);

            g_autofree gchar *out_tmp = NULL;
            {
                gint out_fd = g_file_open_tmp ("vimban-fzf-out-XXXXXX", &out_tmp, NULL);
                if (out_fd >= 0) close (out_fd);
            }

            gint fzf_ret;
            {
                g_autofree gchar *q_tmp = g_shell_quote (tmpfile);
                g_autofree gchar *q_out = g_shell_quote (out_tmp ? out_tmp : "/dev/null");
                g_autofree gchar *fzf_cmd = g_strdup_printf (
                    "fzf --header 'ID\\tStatus\\tPriority\\tAssignee\\tTitle'"
                    " --delimiter '\\t' < %s > %s",
                    q_tmp, q_out);
                const gchar *sh_argv[] = { "/bin/sh", "-c", fzf_cmd, NULL };
                gint exit_status = 0;
                g_spawn_sync (NULL, (gchar **) sh_argv, NULL,
                              G_SPAWN_CHILD_INHERITS_STDIN,
                              NULL, NULL, NULL, NULL, &exit_status, NULL);
                fzf_ret = exit_status;
            }
            g_unlink (tmpfile);

            if (fzf_ret != 0 || !out_tmp)
            {
                if (out_tmp) g_unlink (out_tmp);
                return VIMBAN_EXIT_SUCCESS; /* user cancelled */
            }

            {
                g_autofree gchar *fzf_out = NULL;
                g_file_get_contents (out_tmp, &fzf_out, NULL, NULL);
                g_unlink (out_tmp);
                if (!fzf_out || !fzf_out[0])
                    return VIMBAN_EXIT_SUCCESS;
                gchar **parts = g_strsplit (g_strstrip (fzf_out), "\t", 2);
                ticket_ref = g_strdup (parts[0]);
                g_strfreev (parts);
            }
        }
    }

    /* find ticket */
    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    /* check if any updates requested */
    gboolean has_updates = opt_status || opt_priority || opt_assignee ||
                           opt_add_tag || opt_remove_tag || opt_clear ||
                           opt_due || (opt_progress >= 0) ||
                           (argc > 2);

    /* open editor if no updates or --interactive */
    if (!has_updates || opt_interactive)
    {
        const gchar *editor = g_getenv ("EDITOR");
        if (!editor || !editor[0]) editor = "vi";
        {
            const gchar *spawn_argv[] = { editor, ticket->filepath, NULL };
            g_spawn_sync (NULL, (gchar **) spawn_argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                          NULL, NULL, NULL, NULL, NULL, NULL);
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    /* load frontmatter */
    g_autofree gchar *raw = NULL;
    g_autofree gchar *body = NULL;
    YamlMapping *mapping = NULL;

    if (!g_file_get_contents (ticket->filepath, &raw, NULL, NULL))
    {
        g_printerr ("Error: cannot read ticket file: %s\n", ticket->filepath);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }
    vimban_parse_frontmatter (raw, &mapping, &body);
    if (!mapping) mapping = yaml_mapping_new ();

    /* apply field updates */
    if (opt_status)
        yaml_mapping_set_string_member (mapping, "status", opt_status);

    if (opt_priority)
        yaml_mapping_set_string_member (mapping, "priority", opt_priority);

    if (opt_assignee)
    {
        VimbanTransclusionLink *alink =
            vimban_resolve_person_ref (opt_assignee, config->directory, config);
        if (alink)
        {
            g_autofree gchar *astr = vimban_transclusion_link_to_string (alink);
            yaml_mapping_set_string_member (mapping, "assignee", astr);
            vimban_transclusion_link_free (alink);
        }
    }

    if (opt_progress >= 0)
        yaml_mapping_set_int_member (mapping, "progress", opt_progress);

    if (opt_due)
    {
        g_autoptr(GDate) d = vimban_parse_date (opt_due);
        g_autofree gchar *dstr = vimban_date_to_string (d);
        yaml_mapping_set_string_member (mapping, "due_date", dstr);
    }

    if (opt_clear && opt_clear[0])
        yaml_mapping_set_string_member (mapping, opt_clear, "");

    /* add/remove tag */
    if (opt_add_tag || opt_remove_tag)
    {
        GStrv cur_tags = _parse_string_list (mapping, "tags");
        GPtrArray *new_tags = g_ptr_array_new ();
        gint j;

        if (cur_tags)
        {
            for (j = 0; cur_tags[j]; j++)
            {
                if (opt_remove_tag && g_strcmp0 (cur_tags[j], opt_remove_tag) == 0)
                    continue;
                g_ptr_array_add (new_tags, cur_tags[j]);
            }
        }
        if (opt_add_tag)
        {
            gboolean found = FALSE;
            for (j = 0; (guint) j < new_tags->len; j++)
                if (g_strcmp0 (g_ptr_array_index (new_tags, j), opt_add_tag) == 0)
                { found = TRUE; break; }
            if (!found)
                g_ptr_array_add (new_tags, opt_add_tag);
        }

        YamlSequence *seq = yaml_sequence_new ();
        for (j = 0; (guint) j < new_tags->len; j++)
            yaml_sequence_add_string_element (seq, g_ptr_array_index (new_tags, j));
        yaml_mapping_set_sequence_member (mapping, "tags", seq);
        yaml_sequence_unref (seq);

        g_ptr_array_free (new_tags, FALSE);
        g_strfreev (cur_tags);
    }

    /* positional field=value pairs (argv[2..]) */
    {
        gint j;
        for (j = 2; j < argc; j++)
        {
            gchar *eq = strchr (argv[j], '=');
            if (eq)
            {
                *eq = '\0';
                yaml_mapping_set_string_member (mapping, argv[j], eq + 1);
                *eq = '=';
            }
        }
    }

    /* dry run */
    if (opt_dry_run)
    {
        g_print ("=== DRY RUN ===\nWould update: %s\n", ticket->filepath);
        yaml_mapping_unref (mapping);
        goto edit_cleanup;
    }

    /* bump version + updated */
    {
        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *now_str = g_date_time_format_iso8601 (now);
        gint64 ver = 0;
        if (yaml_mapping_has_member (mapping, "version"))
            ver = yaml_mapping_get_int_member (mapping, "version");
        yaml_mapping_set_int_member (mapping, "version", ver + 1);
        yaml_mapping_set_string_member (mapping, "updated", now_str);
    }

    {
        g_autofree gchar *result = vimban_dump_frontmatter (mapping, body);
        g_autoptr(GError) write_err = NULL;
        if (!g_file_set_contents (ticket->filepath, result, -1, &write_err))
            g_printerr ("vimban: failed to write %s: %s\n",
                         ticket->filepath, write_err->message);
    }
    yaml_mapping_unref (mapping);

    if (global->format == VIMBAN_FORMAT_JSON)
        g_print ("{\"updated\": \"%s\"}\n", ticket_ref ? ticket_ref : ticket->id);
    else
        g_print ("Updated %s\n", ticket->id);

edit_cleanup:
    g_free (opt_status);
    g_free (opt_priority);
    g_free (opt_assignee);
    g_free (opt_add_tag);
    g_free (opt_remove_tag);
    g_free (opt_due);
    g_free (opt_clear);
    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_move:
 *
 * Move a ticket to a new status. Validates the transition unless --force.
 * Sets end_date when done, start_date on in_progress.
 */
static gint
cmd_move (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gboolean opt_force  = FALSE;
    gboolean opt_reopen = FALSE;
    gchar *opt_comment  = NULL;

    GOptionEntry entries[] = {
        { "force",   'f', 0, G_OPTION_ARG_NONE,   &opt_force,
          "Skip transition validation", NULL },
        { "reopen",   0,  0, G_OPTION_ARG_NONE,   &opt_reopen,
          "Clear end_date when reopening", NULL },
        { "comment", 'c', 0, G_OPTION_ARG_STRING, &opt_comment,
          "Add comment on move", "TEXT" },
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> <status> - Move ticket status");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 3)
    {
        g_printerr ("Error: ticket and status required\n");
        g_printerr ("Usage: vimban move <ticket> <status> [options]\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *ticket_ref = argv[1];
    const gchar *new_status = argv[2];

    /* validate new status */
    if (!vimban_str_in_list (new_status, (const gchar **) STATUSES))
    {
        g_autofree gchar *msg = g_strdup_printf ("Unknown status: %s", new_status);
        vimban_error (msg, VIMBAN_EXIT_VALIDATION_ERROR);
        g_free (opt_comment);
        return VIMBAN_EXIT_VALIDATION_ERROR;
    }

    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        g_free (opt_comment);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    const gchar *current_status = ticket->status ? ticket->status : "backlog";

    /* validate transition */
    if (!opt_force &&
        !vimban_is_valid_transition (current_status, new_status))
    {
        g_autofree gchar *msg = g_strdup_printf (
            "Invalid transition: %s -> %s (use --force to override)",
            current_status, new_status);
        vimban_error (msg, VIMBAN_EXIT_VALIDATION_ERROR);
        g_free (opt_comment);
        return VIMBAN_EXIT_VALIDATION_ERROR;
    }

    /* load frontmatter */
    g_autofree gchar *raw = NULL;
    g_autofree gchar *body = NULL;
    YamlMapping *mapping = NULL;

    if (!g_file_get_contents (ticket->filepath, &raw, NULL, NULL))
    {
        g_printerr ("Error: cannot read ticket file: %s\n", ticket->filepath);
        g_free (opt_comment);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }
    vimban_parse_frontmatter (raw, &mapping, &body);
    if (!mapping) mapping = yaml_mapping_new ();

    /* update status */
    yaml_mapping_set_string_member (mapping, "status", new_status);

    /* timestamps */
    {
        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *now_str = g_date_time_format_iso8601 (now);
        gint64 ver = 0;
        if (yaml_mapping_has_member (mapping, "version"))
            ver = yaml_mapping_get_int_member (mapping, "version");
        yaml_mapping_set_int_member (mapping, "version", ver + 1);
        yaml_mapping_set_string_member (mapping, "updated", now_str);
    }

    /* end_date on done */
    if (g_strcmp0 (new_status, "done") == 0)
    {
        g_autoptr(GDate) today = g_date_new ();
        g_date_set_time_t (today, time (NULL));
        g_autofree gchar *ds = vimban_date_to_string (today);
        yaml_mapping_set_string_member (mapping, "end_date", ds);
    }

    /* start_date on in_progress */
    if (g_strcmp0 (new_status, "in_progress") == 0 &&
        !yaml_mapping_has_member (mapping, "start_date"))
    {
        g_autoptr(GDate) today = g_date_new ();
        g_date_set_time_t (today, time (NULL));
        g_autofree gchar *ds = vimban_date_to_string (today);
        yaml_mapping_set_string_member (mapping, "start_date", ds);
    }

    /* clear end_date if reopen */
    if (opt_reopen &&
        (g_strcmp0 (current_status, "done") == 0 ||
         g_strcmp0 (current_status, "cancelled") == 0))
    {
        yaml_mapping_set_string_member (mapping, "end_date", "");
    }

    {
        g_autofree gchar *result = vimban_dump_frontmatter (mapping, body);
        g_autoptr(GError) write_err = NULL;
        if (!g_file_set_contents (ticket->filepath, result, -1, &write_err))
            g_printerr ("vimban: failed to write %s: %s\n",
                         ticket->filepath, write_err->message);
    }
    yaml_mapping_unref (mapping);

    /* optional comment */
    if (opt_comment && opt_comment[0])
    {
        const gchar *user = g_getenv ("USER");
        vimban_insert_comment (ticket->filepath, opt_comment, -1, user, NULL);
    }

    if (global->format == VIMBAN_FORMAT_JSON)
        g_print ("{\"ticket\": \"%s\", \"from\": \"%s\", \"to\": \"%s\"}\n",
                 ticket->id, current_status, new_status);
    else
        g_print ("Moved %s: %s -> %s\n", ticket->id, current_status, new_status);

    g_free (opt_comment);
    return VIMBAN_EXIT_SUCCESS;
}
/* move_location: low priority, keep as stub */
VIMBAN_CMD_STUB (move_location)

/*
 * cmd_archive:
 *
 * Move a done/cancelled ticket to 04_archives/01_projects/<scope>/<status>/.
 */
static gint
cmd_archive (gint              argc,
             gchar           **argv,
             VimbanGlobalOpts *global,
             VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gboolean opt_dry_run = FALSE;
    const gchar *ticket_ref;

    GOptionEntry entries[] = {
        { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run,
          "Preview without moving", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> - Archive a ticket");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: ticket ID required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    ticket_ref = argv[1];

    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    /* validate archivable status */
    if (g_strcmp0 (ticket->status, "done") != 0 &&
        g_strcmp0 (ticket->status, "cancelled") != 0)
    {
        g_autofree gchar *msg = g_strdup_printf (
            "Cannot archive ticket with status '%s'. Must be done or cancelled.",
            ticket->status ? ticket->status : "");
        vimban_error (msg, VIMBAN_EXIT_VALIDATION_ERROR);
        return VIMBAN_EXIT_VALIDATION_ERROR;
    }

    /* detect scope from path */
    const gchar *scope = "personal";
    if (strstr (ticket->filepath, "/work/"))
        scope = "work";

    /* build destination */
    g_autofree gchar *filename = g_path_get_basename (ticket->filepath);
    g_autofree gchar *archive_dir = g_build_filename (
        config->directory, "04_archives", "01_projects",
        scope, ticket->status, NULL);
    g_autofree gchar *dest = g_build_filename (archive_dir, filename, NULL);

    /* relative paths for display */
    const gchar *dir = config->directory;
    gsize dir_len = strlen (dir);
    const gchar *rel_src = ticket->filepath;
    if (g_str_has_prefix (rel_src, dir))
        rel_src += dir_len + 1;
    const gchar *rel_dst = dest;
    if (g_str_has_prefix (rel_dst, dir))
        rel_dst += dir_len + 1;

    if (opt_dry_run)
    {
        g_print ("Would archive: %s -> %s\n", rel_src, rel_dst);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* check already archived */
    if (strstr (ticket->filepath, "04_archives"))
    {
        g_print ("Ticket is already archived: %s\n", rel_src);
        return VIMBAN_EXIT_SUCCESS;
    }

    if (g_file_test (dest, G_FILE_TEST_EXISTS))
    {
        g_autofree gchar *msg = g_strdup_printf (
            "File already exists at destination: %s", rel_dst);
        vimban_error (msg, VIMBAN_EXIT_VALIDATION_ERROR);
        return VIMBAN_EXIT_VALIDATION_ERROR;
    }

    g_mkdir_with_parents (archive_dir, 0755);

    if (g_rename (ticket->filepath, dest) != 0)
    {
        /* fallback: copy + delete */
        g_autofree gchar *src_content = NULL;
        if (!g_file_get_contents (ticket->filepath, &src_content, NULL, NULL) ||
            !g_file_set_contents (dest, src_content, -1, NULL))
        {
            vimban_error ("Failed to move file", VIMBAN_EXIT_GENERAL_ERROR);
            return VIMBAN_EXIT_GENERAL_ERROR;
        }
        g_unlink (ticket->filepath);
    }

    g_print ("Archived: %s -> %s\n", rel_src, rel_dst);
    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_link:
 *
 * Link two tickets together via a relationship field.
 * Usage: vimban link <ticket> --relates-to <target>
 */
static gint
cmd_link (gint              argc,
          gchar           **argv,
          VimbanGlobalOpts *global,
          VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar *opt_relates_to  = NULL;
    gchar *opt_blocked_by  = NULL;
    gchar *opt_blocks      = NULL;
    gchar *opt_member_of   = NULL;
    gboolean opt_remove    = FALSE;
    gboolean opt_dry_run   = FALSE;

    GOptionEntry entries[] = {
        { "relates-to",  0, 0, G_OPTION_ARG_STRING, &opt_relates_to,
          "Add relates_to link", "TICKET" },
        { "blocked-by",  0, 0, G_OPTION_ARG_STRING, &opt_blocked_by,
          "Add blocked_by link", "TICKET" },
        { "blocks",      0, 0, G_OPTION_ARG_STRING, &opt_blocks,
          "Add blocks link", "TICKET" },
        { "member-of",   0, 0, G_OPTION_ARG_STRING, &opt_member_of,
          "Add member_of link", "TICKET" },
        { "remove",      0, 0, G_OPTION_ARG_NONE,   &opt_remove,
          "Remove instead of add", NULL },
        { "dry-run",     0, 0, G_OPTION_ARG_NONE,   &opt_dry_run,
          "Preview", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> [--relation target] - Link tickets");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: ticket ID required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *ticket_ref = argv[1];
    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    /* build field->target_ref pairs */
    struct { const gchar *field; const gchar *target_ref; } pairs[4];
    gint np = 0;
    if (opt_relates_to)  { pairs[np].field = "relates_to";  pairs[np].target_ref = opt_relates_to;  np++; }
    if (opt_blocked_by)  { pairs[np].field = "blocked_by";  pairs[np].target_ref = opt_blocked_by;  np++; }
    if (opt_blocks)      { pairs[np].field = "blocks";      pairs[np].target_ref = opt_blocks;      np++; }
    if (opt_member_of)   { pairs[np].field = "member_of";   pairs[np].target_ref = opt_member_of;   np++; }

    if (np == 0)
    {
        g_printerr ("Error: specify at least one relation flag\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* load frontmatter once */
    g_autofree gchar *raw = NULL;
    g_autofree gchar *body = NULL;
    YamlMapping *mapping = NULL;
    if (!g_file_get_contents (ticket->filepath, &raw, NULL, NULL))
    {
        g_printerr ("Error: cannot read ticket file: %s\n", ticket->filepath);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }
    vimban_parse_frontmatter (raw, &mapping, &body);
    if (!mapping) mapping = yaml_mapping_new ();

    gint i;
    for (i = 0; i < np; i++)
    {
        /* find target */
        g_autoptr(VimbanTicket) target =
            vimban_find_ticket (config->directory, pairs[i].target_ref, config->prefix);
        if (!target)
        {
            g_autofree gchar *msg = g_strdup_printf (
                "Target not found: %s", pairs[i].target_ref);
            vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
            yaml_mapping_unref (mapping);
            return VIMBAN_EXIT_FILE_NOT_FOUND;
        }

        /* relative path for link */
        g_autofree gchar *rel = NULL;
        if (g_str_has_prefix (target->filepath, config->directory))
            rel = g_strdup (target->filepath + strlen (config->directory) + 1);
        else
            rel = g_strdup (target->filepath);

        g_autofree gchar *link_str = g_strdup_printf ("![[%s]]", rel);

        /* get existing sequence */
        YamlSequence *seq = NULL;
        if (yaml_mapping_has_member (mapping, pairs[i].field))
            seq = yaml_mapping_get_sequence_member (mapping, pairs[i].field);

        YamlSequence *new_seq = yaml_sequence_new ();

        /* copy existing entries (excluding target if removing) */
        if (seq)
        {
            guint j, slen = yaml_sequence_get_length (seq);
            for (j = 0; j < slen; j++)
            {
                const gchar *s = yaml_sequence_get_string_element (seq, j);
                if (!s) continue;
                if (opt_remove && strstr (s, rel)) continue;
                yaml_sequence_add_string_element (new_seq, s);
            }
        }

        /* add new link if not removing */
        if (!opt_remove)
        {
            gboolean already = FALSE;
            guint j, slen2 = yaml_sequence_get_length (new_seq);
            for (j = 0; j < slen2; j++)
            {
                const gchar *s = yaml_sequence_get_string_element (new_seq, j);
                if (s && strstr (s, rel)) { already = TRUE; break; }
            }
            if (!already)
                yaml_sequence_add_string_element (new_seq, link_str);
        }

        yaml_mapping_set_sequence_member (mapping, pairs[i].field, new_seq);
        yaml_sequence_unref (new_seq);
    }

    if (opt_dry_run)
    {
        g_print ("=== DRY RUN ===\nWould update: %s\n", ticket->filepath);
        yaml_mapping_unref (mapping);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* bump version */
    {
        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *now_str = g_date_time_format_iso8601 (now);
        gint64 ver = 0;
        if (yaml_mapping_has_member (mapping, "version"))
            ver = yaml_mapping_get_int_member (mapping, "version");
        yaml_mapping_set_int_member (mapping, "version", ver + 1);
        yaml_mapping_set_string_member (mapping, "updated", now_str);
    }

    {
        g_autofree gchar *result = vimban_dump_frontmatter (mapping, body);
        g_autoptr(GError) write_err = NULL;
        if (!g_file_set_contents (ticket->filepath, result, -1, &write_err))
            g_printerr ("vimban: failed to write %s: %s\n",
                         ticket->filepath, write_err->message);
    }
    yaml_mapping_unref (mapping);

    g_print ("%s %s links updated\n",
             opt_remove ? "Removed" : "Added",
             ticket->id);

    g_free (opt_relates_to);
    g_free (opt_blocked_by);
    g_free (opt_blocks);
    g_free (opt_member_of);
    return VIMBAN_EXIT_SUCCESS;
}
/*
 * cmd_comment:
 *
 * Add a comment to a ticket. Text from --text or stdin.
 */
static gint
cmd_comment (gint              argc,
             gchar           **argv,
             VimbanGlobalOpts *global,
             VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar *opt_text     = NULL;
    gchar *opt_user     = NULL;
    gint   opt_reply_to = -1;

    GOptionEntry entries[] = {
        { "text",     't', 0, G_OPTION_ARG_STRING, &opt_text,
          "Comment text", "TEXT" },
        { "user",     'u', 0, G_OPTION_ARG_STRING, &opt_user,
          "Author name", "NAME" },
        { "reply-to", 0,   0, G_OPTION_ARG_INT,    &opt_reply_to,
          "Reply to comment ID", "N" },
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> - Add comment");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: ticket ID required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *ticket_ref = argv[1];
    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    /* get text */
    g_autofree gchar *text = NULL;
    if (opt_text && opt_text[0])
    {
        text = g_strdup (opt_text);
    }
    else
    {
        /* read from stdin */
        GString *buf = g_string_new ("");
        gchar line[1024];
        while (fgets (line, sizeof (line), stdin))
            g_string_append (buf, line);
        text = g_string_free (buf, FALSE);
        g_strstrip (text);
    }

    if (!text || !text[0])
    {
        vimban_error ("Comment text is required", VIMBAN_EXIT_INVALID_ARGS);
        g_free (opt_text);
        g_free (opt_user);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *author = opt_user ? opt_user : g_getenv ("USER");
    gint new_id = vimban_insert_comment (ticket->filepath, text, opt_reply_to,
                                          author, &error);
    if (new_id < 0)
    {
        g_autofree gchar *msg = g_strdup_printf (
            "Failed to add comment: %s", error ? error->message : "unknown");
        vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
        g_free (opt_text);
        g_free (opt_user);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    if (global->format == VIMBAN_FORMAT_JSON)
        g_print ("{\"comment_id\": %d, \"ticket\": \"%s\"}\n", new_id, ticket->id);
    else
        g_print ("Added comment #%d to %s\n", new_id, ticket->id);

    g_free (opt_text);
    g_free (opt_user);
    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_comments:
 *
 * Show comments on a specific ticket.
 */
static gint
cmd_comments (gint              argc,
              gchar           **argv,
              VimbanGlobalOpts *global,
              VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;

    GOptionEntry entries[] = {
        { NULL }
    };

    ctx = g_option_context_new ("<ticket> - Show comments");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: ticket ID required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *ticket_ref = argv[1];
    g_autoptr(VimbanTicket) ticket = vimban_find_ticket (config->directory, ticket_ref, config->prefix);
    if (!ticket)
    {
        g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
        vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
        return VIMBAN_EXIT_FILE_NOT_FOUND;
    }

    g_autofree gchar *raw = NULL;
    g_file_get_contents (ticket->filepath, &raw, NULL, NULL);
    if (!raw)
    {
        vimban_error ("Failed to read file", VIMBAN_EXIT_GENERAL_ERROR);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    g_autoptr(GPtrArray) comments = vimban_parse_comments (raw);

    if (comments->len == 0)
    {
        g_print ("No comments\n");
        return VIMBAN_EXIT_SUCCESS;
    }

    if (global->format == VIMBAN_FORMAT_JSON)
    {
        g_autoptr(JsonBuilder) b = json_builder_new ();
        json_builder_begin_array (b);
        guint i;
        for (i = 0; i < comments->len; i++)
        {
            VimbanComment *c = g_ptr_array_index (comments, i);
            json_builder_begin_object (b);
            json_builder_set_member_name (b, "id");
            json_builder_add_int_value (b, c->id);
            json_builder_set_member_name (b, "author");
            json_builder_add_string_value (b, c->author ? c->author : "");
            json_builder_set_member_name (b, "content");
            json_builder_add_string_value (b, c->content ? c->content : "");
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        JsonNode *root = json_builder_get_root (b);
        g_autoptr(JsonGenerator) gen = json_generator_new ();
        json_generator_set_pretty (gen, TRUE);
        json_generator_set_root (gen, root);
        g_autofree gchar *str = json_generator_to_data (gen, NULL);
        json_node_unref (root);
        g_print ("%s\n", str);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* plain format */
    guint i;
    for (i = 0; i < comments->len; i++)
    {
        VimbanComment *c = g_ptr_array_index (comments, i);
        g_autofree gchar *ts_str = c->timestamp
            ? g_date_time_format (c->timestamp, "%Y-%m-%d %H:%M")
            : g_strdup ("");
        g_autofree gchar *bold_num = _bold (g_strdup_printf ("#%d", c->id));

        g_print ("%s", bold_num);
        if (c->author && c->author[0])
            g_print (" by %s", c->author);
        g_print (" (%s)\n", ts_str);
        if (c->content && c->content[0])
            g_print ("  %s\n", c->content);

        /* replies */
        if (c->replies && c->replies->len > 0)
        {
            guint j;
            for (j = 0; j < c->replies->len; j++)
            {
                VimbanCommentReply *r = g_ptr_array_index (c->replies, j);
                g_autofree gchar *rts = r->timestamp
                    ? g_date_time_format (r->timestamp, "%Y-%m-%d %H:%M")
                    : g_strdup ("");
                g_print ("  -> Reply");
                if (r->author && r->author[0])
                    g_print (" by %s", r->author);
                g_print (" (%s)\n", rts);
                if (r->content && r->content[0])
                    g_print ("     %s\n", r->content);
            }
        }
        g_print ("\n");
    }

    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_dashboard:
 *
 * Show dashboard views (daily, person). Other types output a basic ticket list.
 */
static gint
cmd_dashboard (gint              argc,
               gchar           **argv,
               VimbanGlobalOpts *global,
               VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar *opt_person  = NULL;
    gchar *opt_section = NULL;

    GOptionEntry entries[] = {
        { "person",  'p', 0, G_OPTION_ARG_STRING, &opt_person,
          "Person reference (for person dashboard)", "NAME" },
        { "section",  0,  0, G_OPTION_ARG_STRING, &opt_section,
          "Section filter", "SECTION" },
        { NULL }
    };

    ctx = g_option_context_new ("[type] - Generate dashboard");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *dash_type = (argc >= 2) ? argv[1] : "daily";

    /* scan all tickets */
    g_autoptr(GPtrArray) all = vimban_scan_tickets (config->directory, FALSE, NULL);

    g_autoptr(GDateTime) now_dt = g_date_time_new_now_local ();
    GDate today;
    g_date_set_dmy (&today,
        (GDateDay) g_date_time_get_day_of_month (now_dt),
        (GDateMonth) g_date_time_get_month (now_dt),
        (GDateYear) g_date_time_get_year (now_dt));
    g_autofree gchar *today_str = vimban_date_to_string (&today);

    if (g_strcmp0 (dash_type, "daily") == 0)
    {
        const gchar *user = g_getenv ("USER");
        if (!user) user = "";

        /* active: in_progress, blocked, review assigned to $USER */
        g_autoptr(GPtrArray) active = g_ptr_array_new ();
        g_autoptr(GPtrArray) overdue = g_ptr_array_new ();
        g_autoptr(GPtrArray) due_today = g_ptr_array_new ();
        guint i;

        for (i = 0; i < all->len; i++)
        {
            VimbanTicket *t = g_ptr_array_index (all, i);
            gboolean is_mine = FALSE;

            if (t->assignee && user[0])
            {
                g_autofree gchar *stem = vimban_transclusion_link_get_stem (t->assignee);
                g_autofree gchar *sl = g_ascii_strdown (stem, -1);
                g_autofree gchar *ul = g_ascii_strdown (user, -1);
                is_mine = (strstr (sl, ul) != NULL);
            }

            if (!is_mine) continue;

            if (g_strcmp0 (t->status, "in_progress") == 0 ||
                g_strcmp0 (t->status, "blocked") == 0 ||
                g_strcmp0 (t->status, "review") == 0)
                g_ptr_array_add (active, t);

            if (t->due_date && g_date_valid (t->due_date))
            {
                gint cmp = g_date_compare (t->due_date, &today);
                if (cmp < 0 && g_strcmp0 (t->status, "done") != 0)
                    g_ptr_array_add (overdue, t);
                else if (cmp == 0)
                    g_ptr_array_add (due_today, t);
            }
        }

        VimbanFormat fmt = global->format;
        if (fmt == VIMBAN_FORMAT_PLAIN) fmt = VIMBAN_FORMAT_MD;

        const gchar *col[] = { "id", "status", "priority", "title", "due_date", NULL };

        g_print ("# Daily Dashboard\n\n**Date:** %s\n\n", today_str);

        g_print ("## Active (%u)\n", active->len);
        if (active->len > 0)
        {
            g_autofree gchar *s = vimban_format_output (active, fmt, col, FALSE);
            g_print ("%s\n", s);
        }
        else
            g_print ("_No active tickets_\n\n");

        g_print ("## Overdue (%u)\n", overdue->len);
        if (overdue->len > 0)
        {
            g_autofree gchar *s = vimban_format_output (overdue, fmt, col, FALSE);
            g_print ("%s\n", s);
        }
        else
            g_print ("_No overdue tickets_\n\n");

        g_print ("## Due Today (%u)\n", due_today->len);
        if (due_today->len > 0)
        {
            const gchar *col2[] = { "id", "status", "priority", "title", NULL };
            g_autofree gchar *s = vimban_format_output (due_today, fmt, col2, FALSE);
            g_print ("%s\n", s);
        }
        else
            g_print ("_Nothing due today_\n");
    }
    else if (g_strcmp0 (dash_type, "person") == 0 && opt_person)
    {
        VimbanTransclusionLink *plink =
            vimban_resolve_person_ref (opt_person, config->directory, config);

        if (!plink)
        {
            g_autofree gchar *msg = g_strdup_printf ("Person not found: %s", opt_person);
            vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
            g_free (opt_person);
            g_free (opt_section);
            return VIMBAN_EXIT_FILE_NOT_FOUND;
        }

        g_autoptr(GPtrArray) mine = g_ptr_array_new ();
        guint i;
        for (i = 0; i < all->len; i++)
        {
            VimbanTicket *t = g_ptr_array_index (all, i);
            if (t->assignee && strstr (t->assignee->path, plink->path))
                g_ptr_array_add (mine, t);
        }
        vimban_transclusion_link_free (plink);

        const gchar *col[] = { "id", "status", "priority", "title", "due_date", NULL };
        g_autofree gchar *s = vimban_format_output (mine, global->format, col, FALSE);
        if (s && s[0]) g_print ("%s", s);
    }
    else
    {
        /* generic: show all active */
        g_autoptr(GPtrArray) filtered = vimban_filter_tickets (
            all, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            FALSE, FALSE, FALSE);
        const gchar *col[] = { "id", "status", "priority", "title", "due_date", NULL };
        g_autofree gchar *s = vimban_format_output (filtered, global->format, col, FALSE);
        if (s && s[0]) g_print ("%s", s);
        g_ptr_array_unref (filtered);
    }

    g_free (opt_person);
    g_free (opt_section);
    return VIMBAN_EXIT_SUCCESS;
}

/*
 * cmd_kanban:
 *
 * Display kanban board grouped by status columns.
 */
static gint
cmd_kanban (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar   *opt_project   = NULL;
    gchar   *opt_assignee  = NULL;
    gchar   *opt_status    = NULL;
    gboolean opt_mine      = FALSE;
    gboolean opt_hide_empty = FALSE;
    gboolean opt_compact   = FALSE;
    gint     opt_width     = 0;
    gint     opt_done_last = -1;

    GOptionEntry entries[] = {
        { "project",    'P', 0, G_OPTION_ARG_STRING, &opt_project,
          "Filter by project", "PROJ" },
        { "assignee",   'a', 0, G_OPTION_ARG_STRING, &opt_assignee,
          "Filter by assignee", "NAME" },
        { "status",     's', 0, G_OPTION_ARG_STRING, &opt_status,
          "Comma-sep statuses to show", "STATUS" },
        { "mine",        0,  0, G_OPTION_ARG_NONE,   &opt_mine,
          "My tickets only", NULL },
        { "hide-empty",  0,  0, G_OPTION_ARG_NONE,   &opt_hide_empty,
          "Hide empty columns", NULL },
        { "compact",     0,  0, G_OPTION_ARG_NONE,   &opt_compact,
          "Compact card display", NULL },
        { "width",       0,  0, G_OPTION_ARG_INT,    &opt_width,
          "Column width", "N" },
        { "done-last",   0,  0, G_OPTION_ARG_INT,    &opt_done_last,
          "Show done tickets from last N days", "N" },
        { NULL }
    };

    ctx = g_option_context_new ("- Display kanban board");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* collect tickets from search paths */
    g_auto(GStrv) search_paths = vimban_get_search_paths (config,
                                    global->work, global->personal);
    g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vimban_ticket_free);

    gint i;
    for (i = 0; search_paths[i]; i++)
    {
        g_autoptr(GPtrArray) sub = vimban_scan_tickets (
            search_paths[i], FALSE, NULL);
        while (sub->len > 0)
            g_ptr_array_add (all, g_ptr_array_steal_index (sub, 0));
    }

    /* filter */
    g_autoptr(GPtrArray) filtered = vimban_filter_tickets (
        all, NULL, NULL, NULL,
        opt_assignee, opt_project, NULL, NULL,
        opt_mine, FALSE, FALSE);

    /* handle --done-last */
    if (opt_done_last >= 0)
    {
        g_autoptr(GDateTime) now_dt = g_date_time_new_now_local ();
        GDate cutoff;
        g_date_set_dmy (&cutoff,
            (GDateDay) g_date_time_get_day_of_month (now_dt),
            (GDateMonth) g_date_time_get_month (now_dt),
            (GDateYear) g_date_time_get_year (now_dt));
        g_date_subtract_days (&cutoff, (guint) opt_done_last);

        GPtrArray *kept = g_ptr_array_new ();
        guint j;
        for (j = 0; j < filtered->len; j++)
        {
            VimbanTicket *t = g_ptr_array_index (filtered, j);
            if (g_strcmp0 (t->status, "done") != 0)
            {
                g_ptr_array_add (kept, t);
            }
            else if (t->end_date && g_date_compare (t->end_date, &cutoff) >= 0)
            {
                g_ptr_array_add (kept, t);
            }
        }
        g_ptr_array_unref (filtered);
        filtered = kept;
    }

    /* determine statuses to display */
    gchar **display_statuses = NULL;
    if (opt_status && opt_status[0])
    {
        display_statuses = g_strsplit (opt_status, ",", -1);
    }
    else if (opt_done_last >= 0)
    {
        display_statuses = g_strsplit (
            "backlog,ready,in_progress,blocked,review,delegated,done", ",", -1);
    }
    else
    {
        display_statuses = g_strsplit (
            "backlog,ready,in_progress,blocked,review,delegated", ",", -1);
    }

    /* group by status */
    GHashTable *board = g_hash_table_new_full (
        g_str_hash, g_str_equal, NULL,
        (GDestroyNotify) g_ptr_array_unref);

    gint si;
    for (si = 0; display_statuses[si]; si++)
    {
        gchar *s = g_strstrip (display_statuses[si]);
        g_hash_table_insert (board, s, g_ptr_array_new ());
    }

    guint j;
    for (j = 0; j < filtered->len; j++)
    {
        VimbanTicket *t = g_ptr_array_index (filtered, j);
        if (!t->status) continue;
        GPtrArray *col_arr = g_hash_table_lookup (board, t->status);
        if (col_arr)
            g_ptr_array_add (col_arr, t);
    }

    /* output */
    if (global->format == VIMBAN_FORMAT_JSON)
    {
        g_autoptr(JsonBuilder) b = json_builder_new ();
        json_builder_begin_object (b);
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            GPtrArray *col_arr = g_hash_table_lookup (board, s);
            json_builder_set_member_name (b, s);
            json_builder_begin_array (b);
            if (col_arr)
            {
                guint k;
                for (k = 0; k < col_arr->len; k++)
                {
                    VimbanTicket *t = g_ptr_array_index (col_arr, k);
                    JsonNode *node = vimban_ticket_to_json_node (t);
                    json_builder_add_value (b, node);
                }
            }
            json_builder_end_array (b);
        }
        json_builder_end_object (b);
        JsonNode *root = json_builder_get_root (b);
        g_autoptr(JsonGenerator) gen = json_generator_new ();
        json_generator_set_pretty (gen, TRUE);
        json_generator_set_root (gen, root);
        g_autofree gchar *str = json_generator_to_data (gen, NULL);
        json_node_unref (root);
        g_print ("%s\n", str);
        g_hash_table_destroy (board);
        g_strfreev (display_statuses);
        g_free (opt_project);
        g_free (opt_assignee);
        g_free (opt_status);
        return VIMBAN_EXIT_SUCCESS;
    }

    if (global->format == VIMBAN_FORMAT_MD)
    {
        g_print ("# Kanban Board\n\n");
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            GPtrArray *col_arr = g_hash_table_lookup (board, s);
            guint cnt = col_arr ? col_arr->len : 0;

            if (opt_hide_empty && cnt == 0) continue;

            g_autofree gchar *heading = g_ascii_strup (s, -1);
            /* replace _ with space */
            gchar *p;
            for (p = heading; *p; p++) if (*p == '_') *p = ' ';
            g_print ("## %s (%u)\n\n", heading, cnt);

            if (cnt == 0)
            {
                g_print ("_Empty_\n\n");
                continue;
            }

            if (opt_compact)
            {
                guint k;
                for (k = 0; k < col_arr->len; k++)
                {
                    VimbanTicket *t = g_ptr_array_index (col_arr, k);
                    g_print ("- **%s**: %s\n", t->id, t->title ? t->title : "");
                }
            }
            else
            {
                g_print ("| ID | Title | Priority | Assignee | Due |\n");
                g_print ("|---|---|---|---|---|\n");
                guint k;
                for (k = 0; k < col_arr->len; k++)
                {
                    VimbanTicket *t = g_ptr_array_index (col_arr, k);
                    g_autofree gchar *asn = vimban_transclusion_link_get_stem (t->assignee);
                    g_autofree gchar *due = t->due_date ? vimban_date_to_string (t->due_date) : g_strdup ("");
                    const gchar *title = t->title ? t->title : "";
                    g_print ("| %s | %.40s%s | %s | %s | %s |\n",
                             t->id ? t->id : "",
                             title,
                             strlen (title) > 40 ? "..." : "",
                             t->priority ? t->priority : "",
                             asn, due);
                }
            }
            g_print ("\n");
        }
        g_hash_table_destroy (board);
        g_strfreev (display_statuses);
        g_free (opt_project);
        g_free (opt_assignee);
        g_free (opt_status);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* plain terminal output */
    {
        /* calculate column width */
        gint term_width = 120;
        const gchar *term_cols_env = g_getenv ("COLUMNS");
        if (term_cols_env) term_width = (gint) g_ascii_strtoll (term_cols_env, NULL, 10);
        if (term_width < 40) term_width = 120;

        gint num_cols = 0;
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            if (opt_hide_empty)
            {
                GPtrArray *ca = g_hash_table_lookup (board, s);
                if (!ca || ca->len == 0) continue;
            }
            num_cols++;
        }
        if (num_cols == 0) num_cols = 1;

        gint col_w = opt_width > 0 ? opt_width
                                   : MAX (18, (term_width - (num_cols - 1) * 2) / num_cols);

        /* header */
        GString *line = g_string_new ("");
        gboolean first = TRUE;
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            GPtrArray *ca = g_hash_table_lookup (board, s);
            guint cnt = ca ? ca->len : 0;
            if (opt_hide_empty && cnt == 0) continue;

            if (!first) g_string_append (line, "  ");
            first = FALSE;

            g_autofree gchar *label_base = g_ascii_strup (s, -1);
            gchar *lp;
            for (lp = label_base; *lp; lp++) if (*lp == '_') *lp = ' ';
            g_autofree gchar *label = g_strdup_printf ("%s (%u)", label_base, cnt);

            const gchar *sc = _status_color (s);
            const gchar *ec = _end_color ();
            if (sc[0])
                g_string_append_printf (line, "%s\033[1m%-*s\033[0m%s",
                                         sc, col_w, label, ec);
            else
                g_string_append_printf (line, "\033[1m%-*s\033[0m", col_w, label);
        }
        g_print ("%s\n", line->str);
        g_string_free (line, TRUE);

        /* separator */
        GString *sep = g_string_new ("");
        first = TRUE;
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            GPtrArray *ca = g_hash_table_lookup (board, s);
            if (opt_hide_empty && (!ca || ca->len == 0)) continue;
            if (!first) g_string_append (sep, "  ");
            first = FALSE;
            gint k;
            for (k = 0; k < col_w; k++) g_string_append (sep, "\xe2\x94\x80");
        }
        g_print ("%s\n", sep->str);
        g_string_free (sep, TRUE);

        /* find max rows */
        gint max_rows = 0;
        for (si = 0; display_statuses[si]; si++)
        {
            gchar *s = g_strstrip (display_statuses[si]);
            GPtrArray *ca = g_hash_table_lookup (board, s);
            if (ca && (gint) ca->len > max_rows)
                max_rows = (gint) ca->len;
        }

        /* rows */
        gint row;
        for (row = 0; row < max_rows; row++)
        {
            if (!opt_compact)
            {
                /* line 1: ID */
                GString *id_line = g_string_new ("");
                first = TRUE;
                for (si = 0; display_statuses[si]; si++)
                {
                    gchar *s = g_strstrip (display_statuses[si]);
                    GPtrArray *ca = g_hash_table_lookup (board, s);
                    if (opt_hide_empty && (!ca || ca->len == 0)) continue;
                    if (!first) g_string_append (id_line, "  ");
                    first = FALSE;
                    if (ca && row < (gint) ca->len)
                    {
                        VimbanTicket *t = g_ptr_array_index (ca, row);
                        g_string_append_printf (id_line, "\033[1m%-*s\033[0m",
                                                 col_w, t->id ? t->id : "");
                    }
                    else
                        g_string_append_printf (id_line, "%-*s", col_w, "");
                }
                g_print ("%s\n", id_line->str);
                g_string_free (id_line, TRUE);

                /* line 2: title */
                GString *title_line = g_string_new ("");
                first = TRUE;
                for (si = 0; display_statuses[si]; si++)
                {
                    gchar *s = g_strstrip (display_statuses[si]);
                    GPtrArray *ca = g_hash_table_lookup (board, s);
                    if (opt_hide_empty && (!ca || ca->len == 0)) continue;
                    if (!first) g_string_append (title_line, "  ");
                    first = FALSE;
                    if (ca && row < (gint) ca->len)
                    {
                        VimbanTicket *t = g_ptr_array_index (ca, row);
                        g_string_append_printf (title_line, "%-*.*s",
                                                 col_w, col_w - 1,
                                                 t->title ? t->title : "");
                    }
                    else
                        g_string_append_printf (title_line, "%-*s", col_w, "");
                }
                g_print ("%s\n", title_line->str);
                g_string_free (title_line, TRUE);

                /* line 3: priority + assignee */
                GString *meta_line = g_string_new ("");
                first = TRUE;
                for (si = 0; display_statuses[si]; si++)
                {
                    gchar *s = g_strstrip (display_statuses[si]);
                    GPtrArray *ca = g_hash_table_lookup (board, s);
                    if (opt_hide_empty && (!ca || ca->len == 0)) continue;
                    if (!first) g_string_append (meta_line, "  ");
                    first = FALSE;
                    if (ca && row < (gint) ca->len)
                    {
                        VimbanTicket *t = g_ptr_array_index (ca, row);
                        g_autofree gchar *asn_stem = vimban_transclusion_link_get_stem (t->assignee);
                        g_autofree gchar *asn_short = asn_stem[0]
                            ? g_strdup_printf ("@%.8s", asn_stem) : g_strdup ("");
                        g_autofree gchar *meta = g_strdup_printf ("[%.4s] %s",
                                                                    t->priority ? t->priority : "med",
                                                                    asn_short);
                        const gchar *pc = _priority_color (t->priority);
                        const gchar *ec = _end_color ();
                        if (pc[0])
                            g_string_append_printf (meta_line, "%s%-*.*s%s",
                                                     pc, col_w, col_w - 1, meta, ec);
                        else
                            g_string_append_printf (meta_line, "%-*.*s",
                                                     col_w, col_w - 1, meta);
                    }
                    else
                        g_string_append_printf (meta_line, "%-*s", col_w, "");
                }
                g_print ("%s\n", meta_line->str);
                g_string_free (meta_line, TRUE);

                /* separator between cards (not after last) */
                if (row < max_rows - 1)
                {
                    GString *csep = g_string_new ("");
                    first = TRUE;
                    for (si = 0; display_statuses[si]; si++)
                    {
                        gchar *s2 = g_strstrip (display_statuses[si]);
                        GPtrArray *ca2 = g_hash_table_lookup (board, s2);
                        if (opt_hide_empty && (!ca2 || ca2->len == 0)) continue;
                        if (!first) g_string_append (csep, "  ");
                        first = FALSE;
                        gint k;
                        for (k = 0; k < col_w; k++) g_string_append (csep, "\xe2\x94\x80");
                    }
                    g_print ("%s\n", csep->str);
                    g_string_free (csep, TRUE);
                }
            }
            else
            {
                /* compact: just ID per row */
                GString *crow = g_string_new ("");
                first = TRUE;
                for (si = 0; display_statuses[si]; si++)
                {
                    gchar *s = g_strstrip (display_statuses[si]);
                    GPtrArray *ca = g_hash_table_lookup (board, s);
                    if (opt_hide_empty && (!ca || ca->len == 0)) continue;
                    if (!first) g_string_append (crow, "  ");
                    first = FALSE;
                    if (ca && row < (gint) ca->len)
                    {
                        VimbanTicket *t = g_ptr_array_index (ca, row);
                        g_string_append_printf (crow, "%-*.*s",
                                                 col_w, col_w, t->id ? t->id : "");
                    }
                    else
                        g_string_append_printf (crow, "%-*s", col_w, "");
                }
                g_print ("%s\n", crow->str);
                g_string_free (crow, TRUE);
            }
        }
    }

    g_hash_table_destroy (board);
    g_strfreev (display_statuses);
    g_free (opt_project);
    g_free (opt_assignee);
    g_free (opt_status);
    return VIMBAN_EXIT_SUCCESS;
}
/*
 * cmd_search:
 *
 * Full-text search via rg (ripgrep). Fixed string by default, --regex for
 * regex mode. --files-only lists matching files. --context N adds context.
 */
static gint
cmd_search (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gboolean opt_regex          = FALSE;
    gboolean opt_case_sensitive = FALSE;
    gboolean opt_files_only     = FALSE;
    gint     opt_context        = 0;

    GOptionEntry entries[] = {
        { "regex",          'e', 0, G_OPTION_ARG_NONE, &opt_regex,
          "Treat query as regex", NULL },
        { "case-sensitive", 'S', 0, G_OPTION_ARG_NONE, &opt_case_sensitive,
          "Case-sensitive search", NULL },
        { "files-only",     'l', 0, G_OPTION_ARG_NONE, &opt_files_only,
          "Print matching filenames only", NULL },
        { "context",        'C', 0, G_OPTION_ARG_INT,  &opt_context,
          "Lines of context", "N" },
        { NULL }
    };

    ctx = g_option_context_new ("<query> - Search tickets");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    if (argc < 2)
    {
        g_printerr ("Error: query required\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *query = argv[1];

    /* build rg command */
    GPtrArray *cmd_arr = g_ptr_array_new_with_free_func (NULL);
    gchar *ctx_str_copy = NULL;
    g_ptr_array_add (cmd_arr, (gpointer) "rg");

    if (!opt_case_sensitive)
        g_ptr_array_add (cmd_arr, (gpointer) "-i");

    if (opt_files_only)
    {
        g_ptr_array_add (cmd_arr, (gpointer) "-l");
    }
    else if (opt_context > 0)
    {
        g_ptr_array_add (cmd_arr, (gpointer) "-C");
        ctx_str_copy = g_strdup_printf ("%d", opt_context);
        g_ptr_array_add (cmd_arr, ctx_str_copy);
    }

    if (opt_regex)
        g_ptr_array_add (cmd_arr, (gpointer) "-e");
    else
        g_ptr_array_add (cmd_arr, (gpointer) "-F");

    g_ptr_array_add (cmd_arr, (gpointer) query);
    g_ptr_array_add (cmd_arr, (gpointer) config->directory);
    g_ptr_array_add (cmd_arr, (gpointer) "--glob");
    g_ptr_array_add (cmd_arr, (gpointer) "*.md");
    g_ptr_array_add (cmd_arr, NULL);

    gchar **rg_argv = (gchar **) cmd_arr->pdata;
    gchar *rg_stdout = NULL;
    gchar *rg_stderr = NULL;
    gint exit_status = 0;
    g_autoptr(GError) spawn_err = NULL;

    gboolean ok = g_spawn_sync (NULL, rg_argv, NULL,
                                G_SPAWN_SEARCH_PATH,
                                NULL, NULL,
                                &rg_stdout, &rg_stderr,
                                &exit_status, &spawn_err);

    g_ptr_array_free (cmd_arr, TRUE);
    g_free (ctx_str_copy);

    if (!ok)
    {
        vimban_error ("ripgrep (rg) not found or failed to execute",
                      VIMBAN_EXIT_GENERAL_ERROR);
        g_free (rg_stdout);
        g_free (rg_stderr);
        return VIMBAN_EXIT_GENERAL_ERROR;
    }

    if (rg_stdout && rg_stdout[0])
        g_print ("%s", rg_stdout);

    g_free (rg_stdout);
    g_free (rg_stderr);

    return (WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0)
        ? VIMBAN_EXIT_SUCCESS
        : VIMBAN_EXIT_GENERAL_ERROR;
}

/*
 * cmd_validate:
 *
 * Validate ticket frontmatter. Checks required fields, valid statuses,
 * types, and priorities. --strict fails on warnings too.
 */
static gint
cmd_validate (gint              argc,
              gchar           **argv,
              VimbanGlobalOpts *global,
              VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gboolean opt_fix    = FALSE;
    gboolean opt_strict = FALSE;

    GOptionEntry entries[] = {
        { "fix",    0, 0, G_OPTION_ARG_NONE, &opt_fix,
          "Attempt to fix issues", NULL },
        { "strict", 0, 0, G_OPTION_ARG_NONE, &opt_strict,
          "Fail on warnings too", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("[files...] - Validate tickets");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* collect files to validate */
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

    if (argc >= 2)
    {
        gint i;
        for (i = 1; i < argc; i++)
            g_ptr_array_add (paths, g_strdup (argv[i]));
    }
    else
    {
        /* scan entire directory recursively */
        g_autoptr(GPtrArray) tickets = vimban_scan_tickets (
            config->directory, TRUE, NULL);
        guint j;
        for (j = 0; j < tickets->len; j++)
        {
            VimbanTicket *t = g_ptr_array_index (tickets, j);
            g_ptr_array_add (paths, g_strdup (t->filepath));
        }
    }

    GPtrArray *errors   = g_ptr_array_new_with_free_func (g_free);
    GPtrArray *warnings = g_ptr_array_new_with_free_func (g_free);

    guint pi;
    for (pi = 0; pi < paths->len; pi++)
    {
        const gchar *fpath = g_ptr_array_index (paths, pi);
        g_autofree gchar *raw = NULL;
        g_autofree gchar *body = NULL;
        YamlMapping *mapping = NULL;

        if (!g_file_get_contents (fpath, &raw, NULL, NULL))
        {
            g_ptr_array_add (errors, g_strdup_printf ("%s: cannot read file", fpath));
            continue;
        }

        vimban_parse_frontmatter (raw, &mapping, &body);
        if (!mapping) continue; /* not a vimban file */

        /* skip if no id or type */
        if (!yaml_mapping_has_member (mapping, "id") &&
            !yaml_mapping_has_member (mapping, "type"))
        {
            yaml_mapping_unref (mapping);
            continue;
        }

        g_autofree gchar *ftype = _yaml_get_string (mapping, "type");

        /* check required fields (skip person type) */
        if (g_strcmp0 (ftype, "person") != 0)
        {
            /* determine if status is required for this type */
            gboolean needs_status = TRUE;
            if (ftype)
            {
                if (g_strcmp0 (ftype, "resource") == 0)
                    needs_status = FALSE;
                else
                {
                    const VimbanSpecTypeConfig *spec =
                        vimban_get_spec_type_config (ftype);
                    if (spec && !spec->has_status)
                        needs_status = FALSE;
                }
            }

            const gchar *required[] = { "id", "title", "type", "created", NULL };
            gint ri;
            for (ri = 0; required[ri]; ri++)
            {
                if (!yaml_mapping_has_member (mapping, required[ri]) ||
                    !yaml_mapping_get_string_member (mapping, required[ri]) ||
                    !yaml_mapping_get_string_member (mapping, required[ri])[0])
                {
                    g_ptr_array_add (errors, g_strdup_printf (
                        "%s: missing required field '%s'", fpath, required[ri]));
                }
            }

            /* check status separately based on type requirements */
            if (needs_status)
            {
                if (!yaml_mapping_has_member (mapping, "status") ||
                    !yaml_mapping_get_string_member (mapping, "status") ||
                    !yaml_mapping_get_string_member (mapping, "status")[0])
                {
                    g_ptr_array_add (errors, g_strdup_printf (
                        "%s: missing required field 'status'", fpath));
                }
            }
        }

        /* validate status */
        g_autofree gchar *status_val = _yaml_get_string (mapping, "status");
        if (status_val && status_val[0] &&
            !vimban_str_in_list (status_val, (const gchar **) STATUSES))
        {
            g_ptr_array_add (errors, g_strdup_printf (
                "%s: invalid status '%s'", fpath, status_val));
        }

        /* validate type */
        if (ftype && ftype[0] && !vimban_is_valid_type (ftype) &&
            g_strcmp0 (ftype, "person") != 0)
        {
            g_ptr_array_add (errors, g_strdup_printf (
                "%s: invalid type '%s'", fpath, ftype));
        }

        /* validate priority */
        g_autofree gchar *priority_val = _yaml_get_string (mapping, "priority");
        if (priority_val && priority_val[0] &&
            !vimban_str_in_list (priority_val, (const gchar **) PRIORITIES))
        {
            g_ptr_array_add (warnings, g_strdup_printf (
                "%s: invalid priority '%s'", fpath, priority_val));
        }

        yaml_mapping_unref (mapping);
    }

    /* output */
    if (errors->len > 0)
    {
        g_print ("Errors:\n");
        guint i;
        for (i = 0; i < errors->len; i++)
            g_print ("  %s\n", (gchar *) g_ptr_array_index (errors, i));
    }

    if (warnings->len > 0)
    {
        g_print ("\nWarnings:\n");
        guint i;
        for (i = 0; i < warnings->len; i++)
            g_print ("  %s\n", (gchar *) g_ptr_array_index (warnings, i));
    }

    if (errors->len == 0 && warnings->len == 0)
        g_print ("All files valid\n");

    {
        guint n_errors = errors->len;
        guint n_warnings = warnings->len;

        g_ptr_array_unref (paths);
        g_ptr_array_unref (errors);
        g_ptr_array_unref (warnings);

        if (n_errors > 0) return VIMBAN_EXIT_VALIDATION_ERROR;
        if (opt_strict && n_warnings > 0) return VIMBAN_EXIT_VALIDATION_ERROR;
        return VIMBAN_EXIT_SUCCESS;
    }
}

/* report/sync: low priority stubs */
VIMBAN_CMD_STUB (report)
VIMBAN_CMD_STUB (sync)

/*
 * cmd_commit:
 *
 * Git add vimban files and commit. Optionally pull and push.
 */
static gint
cmd_commit (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    g_autoptr(GOptionContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    gchar   *opt_message  = NULL;
    gboolean opt_no_pull  = FALSE;
    gboolean opt_no_push  = FALSE;
    gboolean opt_all      = FALSE;
    gboolean opt_dry_run  = FALSE;

    GOptionEntry entries[] = {
        { "message",  'm', 0, G_OPTION_ARG_STRING, &opt_message,
          "Commit message", "MSG" },
        { "no-pull",   0,  0, G_OPTION_ARG_NONE,   &opt_no_pull,
          "Skip git pull", NULL },
        { "no-push",   0,  0, G_OPTION_ARG_NONE,   &opt_no_push,
          "Skip git push", NULL },
        { "all",       0,  0, G_OPTION_ARG_NONE,   &opt_all,
          "Stage all files", NULL },
        { "dry-run",   0,  0, G_OPTION_ARG_NONE,   &opt_dry_run,
          "Preview", NULL },
        { NULL }
    };

    ctx = g_option_context_new ("- Git commit vimban changes");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* verify git repo */
    g_autofree gchar *git_dir = g_build_filename (config->directory, ".git", NULL);
    if (!g_file_test (git_dir, G_FILE_TEST_EXISTS))
    {
        g_autofree gchar *msg = g_strdup_printf (
            "'%s' is not a git repository", config->directory);
        vimban_error (msg, VIMBAN_EXIT_GIT_ERROR);
        g_free (opt_message);
        return VIMBAN_EXIT_GIT_ERROR;
    }

    /* default message */
    if (!opt_message || !opt_message[0])
    {
        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *ts = g_date_time_format (now, "%Y-%m-%d %H:%M");
        g_free (opt_message);
        opt_message = g_strdup_printf ("vimban: sync %s", ts);
    }

    if (opt_dry_run)
    {
        g_print ("=== DRY RUN ===\n");
        g_print ("Directory: %s\n", config->directory);
        g_print ("Message: %s\n", opt_message);
        g_print ("Pull: %s  Push: %s\n",
                 opt_no_pull ? "no" : "yes",
                 opt_no_push ? "no" : "yes");
        g_free (opt_message);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* helper: run git subcommand in config->directory */
    #define _GIT_RUN(args, out_str, fail_msg, fail_ret) \
    { \
        gchar *git_argv[] = args; \
        gchar *_out = NULL; \
        gchar *_err = NULL; \
        gint _exit = 0; \
        g_autoptr(GError) _ge = NULL; \
        gboolean _ok = g_spawn_sync (config->directory, git_argv, NULL, \
            G_SPAWN_SEARCH_PATH, NULL, NULL, \
            &_out, &_err, &_exit, &_ge); \
        if (!_ok || !WIFEXITED (_exit) || WEXITSTATUS (_exit) != 0) { \
            g_autofree gchar *_msg = g_strdup_printf ("%s: %s", fail_msg, \
                _err ? g_strstrip (_err) : (_ge ? _ge->message : "failed")); \
            vimban_error (_msg, VIMBAN_EXIT_GIT_ERROR); \
            g_free (_out); g_free (_err); g_free (opt_message); \
            return fail_ret; \
        } \
        if (out_str) *out_str = _out; else g_free (_out); \
        g_free (_err); \
    }

    /* check for remote */
    gchar *remote_out = NULL;
    {
        gchar *git_argv[] = { (gchar *)"git", (gchar *)"remote", NULL };
        gchar *_out = NULL, *_err = NULL;
        gint _exit = 0;
        g_spawn_sync (config->directory, git_argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &_out, &_err, &_exit, NULL);
        remote_out = _out;
        g_free (_err);
    }
    gboolean has_remote = remote_out && g_strstrip (remote_out)[0];

    /* pull */
    if (!opt_no_pull && has_remote)
    {
        gchar *pull_argv[] = {
            (gchar *)"git", (gchar *)"pull",
            (gchar *)"--rebase", (gchar *)"--autostash", NULL
        };
        gchar *pull_out = NULL, *pull_err = NULL;
        gint pull_exit = 0;
        g_spawn_sync (config->directory, pull_argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &pull_out, &pull_err, &pull_exit, NULL);
        if (!WIFEXITED (pull_exit) || WEXITSTATUS (pull_exit) != 0)
        {
            g_autofree gchar *msg = g_strdup_printf (
                "git pull failed: %s",
                pull_err ? g_strstrip (pull_err) : "unknown");
            vimban_error (msg, VIMBAN_EXIT_GIT_ERROR);
            g_free (pull_out); g_free (pull_err);
            g_free (remote_out); g_free (opt_message);
            return VIMBAN_EXIT_GIT_ERROR;
        }
        g_free (pull_out); g_free (pull_err);
    }
    g_free (remote_out);

    /* stage files */
    if (opt_all)
    {
        gchar *add_argv[] = { (gchar *)"git", (gchar *)"add", (gchar *)"-A", NULL };
        gchar *_out = NULL, *_err = NULL; gint _exit = 0;
        g_spawn_sync (config->directory, add_argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &_out, &_err, &_exit, NULL);
        g_free (_out); g_free (_err);
    }
    else
    {
        /* stage each vimban file */
        g_autoptr(GPtrArray) tickets = vimban_scan_tickets (
            config->directory, TRUE, NULL);
        guint i;
        for (i = 0; i < tickets->len; i++)
        {
            VimbanTicket *t = g_ptr_array_index (tickets, i);
            gchar *add_argv[] = {
                (gchar *)"git", (gchar *)"add", t->filepath, NULL
            };
            gchar *_out = NULL, *_err = NULL; gint _exit = 0;
            g_spawn_sync (config->directory, add_argv, NULL,
                          G_SPAWN_SEARCH_PATH, NULL, NULL,
                          &_out, &_err, &_exit, NULL);
            g_free (_out); g_free (_err);
        }

        /* stage .vimban/ */
        g_autofree gchar *vimban_cfg_dir = g_build_filename (
            config->directory, CONFIG_DIR_NAME, NULL);
        if (g_file_test (vimban_cfg_dir, G_FILE_TEST_IS_DIR))
        {
            gchar *add_argv[] = {
                (gchar *)"git", (gchar *)"add", vimban_cfg_dir, NULL
            };
            gchar *_out = NULL, *_err = NULL; gint _exit = 0;
            g_spawn_sync (config->directory, add_argv, NULL,
                          G_SPAWN_SEARCH_PATH, NULL, NULL,
                          &_out, &_err, &_exit, NULL);
            g_free (_out); g_free (_err);
        }
    }

    /* check if anything staged */
    {
        gchar *st_argv[] = {
            (gchar *)"git", (gchar *)"status", (gchar *)"--porcelain", NULL
        };
        gchar *st_out = NULL, *st_err = NULL; gint st_exit = 0;
        g_spawn_sync (config->directory, st_argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &st_out, &st_err, &st_exit, NULL);
        gboolean nothing = !st_out || !g_strstrip (st_out)[0];
        g_free (st_out); g_free (st_err);
        if (nothing)
        {
            g_print ("No changes to commit\n");
            g_free (opt_message);
            return VIMBAN_EXIT_SUCCESS;
        }
    }

    /* commit */
    {
        gchar *commit_argv[] = {
            (gchar *)"git", (gchar *)"commit",
            (gchar *)"-m", opt_message, NULL
        };
        gchar *c_out = NULL, *c_err = NULL; gint c_exit = 0;
        gboolean c_ok = g_spawn_sync (config->directory, commit_argv, NULL,
                                       G_SPAWN_SEARCH_PATH, NULL, NULL,
                                       &c_out, &c_err, &c_exit, NULL);
        if (!c_ok || !WIFEXITED (c_exit) || WEXITSTATUS (c_exit) != 0)
        {
            g_autofree gchar *msg = g_strdup_printf (
                "git commit failed: %s",
                c_err ? g_strstrip (c_err) : "unknown");
            vimban_error (msg, VIMBAN_EXIT_GIT_ERROR);
            g_free (c_out); g_free (c_err); g_free (opt_message);
            return VIMBAN_EXIT_GIT_ERROR;
        }
        g_free (c_out); g_free (c_err);
    }

    g_print ("Committed: %s\n", opt_message);

    /* push */
    if (!opt_no_push)
    {
        /* re-check remote */
        gchar *rchk_argv[] = { (gchar *)"git", (gchar *)"remote", NULL };
        gchar *rchk_out = NULL, *rchk_err = NULL; gint rchk_exit = 0;
        g_spawn_sync (config->directory, rchk_argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &rchk_out, &rchk_err, &rchk_exit, NULL);
        gboolean push_has_remote = rchk_out && g_strstrip (rchk_out)[0];
        g_free (rchk_out); g_free (rchk_err);

        if (push_has_remote)
        {
            gchar *push_argv[] = { (gchar *)"git", (gchar *)"push", NULL };
            gchar *p_out = NULL, *p_err = NULL; gint p_exit = 0;
            g_spawn_sync (config->directory, push_argv, NULL,
                          G_SPAWN_SEARCH_PATH, NULL, NULL,
                          &p_out, &p_err, &p_exit, NULL);
            if (!WIFEXITED (p_exit) || WEXITSTATUS (p_exit) != 0)
            {
                g_autofree gchar *msg = g_strdup_printf (
                    "git push failed: %s",
                    p_err ? g_strstrip (p_err) : "unknown");
                vimban_error (msg, VIMBAN_EXIT_GIT_ERROR);
                g_free (p_out); g_free (p_err); g_free (opt_message);
                return VIMBAN_EXIT_GIT_ERROR;
            }
            g_free (p_out); g_free (p_err);
            g_print ("Pushed to remote\n");
        }
        else
        {
            if (!g_quiet)
                g_printerr ("Warning: no remote configured, skipping push\n");
        }
    }

    g_free (opt_message);
    return VIMBAN_EXIT_SUCCESS;
}
/*
 * cmd_people:
 *
 * People management dispatcher. Subcommands: list, show, create, search.
 * Usage: vimban people <list|show|create|search> [args...]
 */
static gint
cmd_people (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    if (argc < 2)
    {
        g_printerr ("Usage: vimban people <list|show|create|search> [args]\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *subcmd = argv[1];

    g_autofree gchar *people_path = g_build_filename (
        config->directory, config->people_dir, NULL);

    if (g_strcmp0 (subcmd, "list") == 0)
    {
        g_autoptr(GPtrArray) people = vimban_scan_people (people_path);

        if (people->len == 0)
        {
            g_print ("No people found in %s\n", people_path);
            return VIMBAN_EXIT_SUCCESS;
        }

        if (global->format == VIMBAN_FORMAT_JSON)
        {
            g_autoptr(JsonBuilder) b = json_builder_new ();
            json_builder_begin_array (b);
            guint i;
            for (i = 0; i < people->len; i++)
            {
                VimbanPerson *p = g_ptr_array_index (people, i);
                JsonNode *node = vimban_person_to_json_node (p);
                json_builder_add_value (b, node);
            }
            json_builder_end_array (b);
            JsonNode *root = json_builder_get_root (b);
            g_autoptr(JsonGenerator) gen = json_generator_new ();
            json_generator_set_pretty (gen, TRUE);
            json_generator_set_root (gen, root);
            g_autofree gchar *str = json_generator_to_data (gen, NULL);
            json_node_unref (root);
            g_print ("%s\n", str);
        }
        else
        {
            /* plain table */
            guint i;
            g_print ("%-30s %-20s %-20s %s\n", "NAME", "ROLE", "TEAM", "EMAIL");
            for (i = 0; i < people->len; i++)
            {
                VimbanPerson *p = g_ptr_array_index (people, i);
                g_print ("%-30s %-20s %-20s %s\n",
                         p->name  ? p->name  : "",
                         p->role  ? p->role  : "",
                         p->team  ? p->team  : "",
                         p->email ? p->email : "");
            }
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    if (g_strcmp0 (subcmd, "show") == 0)
    {
        if (argc < 3)
        {
            g_printerr ("Usage: vimban people show <name>\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }

        g_autoptr(VimbanPerson) person =
            vimban_find_person (config->directory, config, argv[2]);
        if (!person)
        {
            g_autofree gchar *msg = g_strdup_printf ("Person not found: %s", argv[2]);
            vimban_error (msg, VIMBAN_EXIT_FILE_NOT_FOUND);
            return VIMBAN_EXIT_FILE_NOT_FOUND;
        }

        if (global->format == VIMBAN_FORMAT_JSON)
        {
            JsonNode *node = vimban_person_to_json_node (person);
            g_autoptr(JsonGenerator) gen = json_generator_new ();
            json_generator_set_pretty (gen, TRUE);
            json_generator_set_root (gen, node);
            g_autofree gchar *str = json_generator_to_data (gen, NULL);
            json_node_unref (node);
            g_print ("%s\n", str);
        }
        else
        {
            g_autofree gchar *bold_name = _bold (person->name ? person->name : "");
            g_print ("%s\n", bold_name);
            if (person->role  && person->role[0])  g_print ("  Role:  %s\n", person->role);
            if (person->team  && person->team[0])  g_print ("  Team:  %s\n", person->team);
            if (person->email && person->email[0]) g_print ("  Email: %s\n", person->email);
            if (person->slack && person->slack[0]) g_print ("  Slack: %s\n", person->slack);
            g_print ("  File:  %s\n", person->filepath);
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    if (g_strcmp0 (subcmd, "create") == 0)
    {
        if (argc < 3)
        {
            g_printerr ("Usage: vimban people create <name>\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }

        const gchar *name = argv[2];
        g_autofree gchar *filename_base = g_ascii_strdown (name, -1);
        /* replace spaces with _ */
        gchar *p;
        for (p = filename_base; *p; p++) if (*p == ' ') *p = '_';
        g_autofree gchar *filename = g_strdup_printf ("%s.md", filename_base);
        g_autofree gchar *filepath = g_build_filename (people_path, filename, NULL);

        if (g_file_test (filepath, G_FILE_TEST_EXISTS))
        {
            g_autofree gchar *msg = g_strdup_printf ("Person file already exists: %s", filepath);
            vimban_error (msg, VIMBAN_EXIT_GENERAL_ERROR);
            return VIMBAN_EXIT_GENERAL_ERROR;
        }

        g_autofree gchar *person_id = vimban_next_id (
            config->directory, NULL, NULL, "person", config);
        if (!person_id)
        {
            vimban_error ("Failed to generate person ID", VIMBAN_EXIT_GENERAL_ERROR);
            return VIMBAN_EXIT_GENERAL_ERROR;
        }

        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *now_str = g_date_time_format_iso8601 (now);

        g_autofree gchar *content = g_strdup_printf (
            "---\nid: \"%s\"\nname: %s\ntype: person\ncreated: %s\n---\n\n",
            person_id, name, now_str);

        g_mkdir_with_parents (people_path, 0755);
        {
            g_autoptr(GError) write_err = NULL;
            if (!g_file_set_contents (filepath, content, -1, &write_err))
            {
                g_printerr ("vimban: failed to write %s: %s\n",
                             filepath, write_err->message);
                return VIMBAN_EXIT_GENERAL_ERROR;
            }
        }

        {
            const gchar *editor = g_getenv ("EDITOR");
            if (!editor || !editor[0]) editor = "vi";
            {
                const gchar *spawn_argv[] = { editor, filepath, NULL };
                g_spawn_sync (NULL, (gchar **) spawn_argv, NULL,
                              G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                              NULL, NULL, NULL, NULL, NULL, NULL);
            }
        }

        g_print ("Created %s: %s\n", person_id, filepath);
        return VIMBAN_EXIT_SUCCESS;
    }

    if (g_strcmp0 (subcmd, "search") == 0)
    {
        if (argc < 3)
        {
            g_printerr ("Usage: vimban people search <query>\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }

        const gchar *query = argv[2];
        g_autoptr(GPtrArray) people = vimban_scan_people (people_path);
        g_autofree gchar *q_lower = g_ascii_strdown (query, -1);

        guint i;
        for (i = 0; i < people->len; i++)
        {
            VimbanPerson *person = g_ptr_array_index (people, i);
            g_autofree gchar *name_lower = g_ascii_strdown (
                person->name ? person->name : "", -1);
            if (strstr (name_lower, q_lower))
                g_print ("%s: %s\n",
                         person->name ? person->name : "",
                         person->filepath);
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    g_printerr ("Error: unknown people subcommand: %s\n", subcmd);
    g_printerr ("Available: list, show, create, search\n");
    return VIMBAN_EXIT_INVALID_ARGS;
}

/*
 * cmd_mentor:
 *
 * Mentor management dispatcher. Subcommands: new, list, show.
 */
static gint
cmd_mentor (gint              argc,
            gchar           **argv,
            VimbanGlobalOpts *global,
            VimbanConfig     *config)
{
    if (argc < 2)
    {
        g_printerr ("Usage: vimban mentor <new|list|show> [args]\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    const gchar *subcmd = argv[1];

    if (g_strcmp0 (subcmd, "list") == 0)
    {
        g_autofree gchar *mntr_dir = g_build_filename (
            config->directory, "02_areas", "work", "mentorship", NULL);

        if (!g_file_test (mntr_dir, G_FILE_TEST_IS_DIR))
        {
            g_print ("No mentorship meetings found.\n");
            return VIMBAN_EXIT_SUCCESS;
        }

        /* scan for mentorship-type tickets */
        const gchar *exclude[] = { NULL }; /* include all */
        g_autoptr(GPtrArray) tickets = vimban_scan_tickets (mntr_dir, FALSE, exclude);

        guint i;
        for (i = 0; i < tickets->len; i++)
        {
            VimbanTicket *t = g_ptr_array_index (tickets, i);
            if (g_strcmp0 (t->type, "mentorship") != 0) continue;
            g_autofree gchar *asn = vimban_transclusion_link_get_stem (t->assignee);
            g_print ("%s  [%s]  %s  %s\n",
                     t->id ? t->id : "",
                     t->status ? t->status : "",
                     asn,
                     t->title ? t->title : "");
        }
        return VIMBAN_EXIT_SUCCESS;
    }

    if (g_strcmp0 (subcmd, "show") == 0)
    {
        if (argc < 3)
        {
            g_printerr ("Usage: vimban mentor show <meeting-id>\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }
        /* delegate to cmd_show */
        gchar *show_argv[] = { (gchar *)"show", (gchar *) argv[2], NULL };
        return cmd_show (2, show_argv, global, config);
    }

    if (g_strcmp0 (subcmd, "new") == 0)
    {
        /* minimal implementation: create mentorship ticket */
        if (argc < 3)
        {
            g_printerr ("Usage: vimban mentor new <person>\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }

        const gchar *person_ref = argv[2];
        g_autoptr(VimbanPerson) person =
            vimban_find_person (config->directory, config, person_ref);

        const gchar *person_name = person
            ? (person->name ? person->name : person_ref)
            : person_ref;

        g_autofree gchar *ticket_id = vimban_next_id (
            config->directory, NULL, NULL, "mentorship", config);
        if (!ticket_id)
        {
            vimban_error ("Failed to generate mentorship ID", VIMBAN_EXIT_GENERAL_ERROR);
            return VIMBAN_EXIT_GENERAL_ERROR;
        }

        g_autoptr(GDateTime) now = g_date_time_new_now_local ();
        g_autofree gchar *now_str = g_date_time_format_iso8601 (now);
        g_autofree gchar *title = g_strdup_printf ("Mentor Meeting with %s", person_name);
        g_autofree gchar *safe = vimban_sanitize_filename (person_name, 30);
        g_autofree gchar *date_str = g_date_time_format (now, "%Y-%m-%d");
        g_autofree gchar *filename = g_strdup_printf ("%s_%s_%s.md",
                                                       ticket_id, safe, date_str);
        g_autofree gchar *out_dir = g_build_filename (
            config->directory, "02_areas", "work", "mentorship",
            safe, NULL);
        g_autofree gchar *out_path = g_build_filename (out_dir, filename, NULL);

        g_autofree gchar *content = g_strdup_printf (
            "---\n"
            "id: \"%s\"\n"
            "title: %s\n"
            "type: mentorship\n"
            "status: in_progress\n"
            "priority: medium\n"
            "created: %s\n"
            "---\n\n"
            "## Meeting Notes\n\n"
            "## Action Items\n\n",
            ticket_id, title, now_str);

        g_mkdir_with_parents (out_dir, 0755);
        {
            g_autoptr(GError) write_err = NULL;
            if (!g_file_set_contents (out_path, content, -1, &write_err))
            {
                g_printerr ("vimban: failed to write %s: %s\n",
                             out_path, write_err->message);
                return VIMBAN_EXIT_GENERAL_ERROR;
            }
        }

        {
            const gchar *editor = g_getenv ("EDITOR");
            if (!editor || !editor[0]) editor = "vi";
            {
                const gchar *spawn_argv[] = { editor, out_path, NULL };
                g_spawn_sync (NULL, (gchar **) spawn_argv, NULL,
                              G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                              NULL, NULL, NULL, NULL, NULL, NULL);
            }
        }

        g_print ("Created %s: %s\n", ticket_id, out_path);
        return VIMBAN_EXIT_SUCCESS;
    }

    g_printerr ("Error: unknown mentor subcommand: %s\n", subcmd);
    g_printerr ("Available: new, list, show\n");
    return VIMBAN_EXIT_INVALID_ARGS;
}
VIMBAN_CMD_STUB (convert)
VIMBAN_CMD_STUB (completion)
VIMBAN_CMD_STUB (tui)
VIMBAN_CMD_STUB (watch_cmd)


/* ═══════════════════════════════════════════════════════════════════════════
 * MCP Server
 *
 * Exposes vimban functionality as an MCP (Model Context Protocol) server.
 * Supports both stdio transport (--mcp) and HTTP/SSE transport (--mcp-http).
 *
 * Each tool handler builds results directly using the same infrastructure
 * functions used by the CLI subcommands, avoiding the need to fork or
 * capture stdout.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * VimbanMcpContext:
 *
 * Shared context passed as user_data to every MCP tool handler.
 * Holds the loaded config and synthesised global opts so handlers
 * can call the same infrastructure used by CLI commands.
 */
typedef struct {
	VimbanConfig     *config;
	VimbanGlobalOpts *global;
} VimbanMcpContext;

static void
vimban_mcp_context_free (VimbanMcpContext *ctx)
{
	if (!ctx) return;
	/* config and global are owned by the caller (main), not freed here */
	g_free (ctx);
}

/* ── schema helpers ────────────────────────────────────────────────────── */

/*
 * mcp_schema_string_prop:
 * @b: JsonBuilder in use
 * @name: property name
 * @desc: description string
 *
 * Appends a single string-typed property entry into the current JSON object.
 */
static void
mcp_schema_string_prop (JsonBuilder *b,
                         const gchar *name,
                         const gchar *desc)
{
	json_builder_set_member_name (b, name);
	json_builder_begin_object (b);
	json_builder_set_member_name (b, "type");
	json_builder_add_string_value (b, "string");
	json_builder_set_member_name (b, "description");
	json_builder_add_string_value (b, desc);
	json_builder_end_object (b);
}

/*
 * mcp_schema_bool_prop:
 * @b: JsonBuilder in use
 * @name: property name
 * @desc: description string
 *
 * Appends a single boolean-typed property entry.
 */
static void
mcp_schema_bool_prop (JsonBuilder *b,
                       const gchar *name,
                       const gchar *desc)
{
	json_builder_set_member_name (b, name);
	json_builder_begin_object (b);
	json_builder_set_member_name (b, "type");
	json_builder_add_string_value (b, "boolean");
	json_builder_set_member_name (b, "description");
	json_builder_add_string_value (b, desc);
	json_builder_end_object (b);
}

/*
 * mcp_schema_int_prop:
 * @b: JsonBuilder in use
 * @name: property name
 * @desc: description string
 *
 * Appends a single integer-typed property entry.
 */
static void
mcp_schema_int_prop (JsonBuilder *b,
                      const gchar *name,
                      const gchar *desc)
{
	json_builder_set_member_name (b, name);
	json_builder_begin_object (b);
	json_builder_set_member_name (b, "type");
	json_builder_add_string_value (b, "integer");
	json_builder_set_member_name (b, "description");
	json_builder_add_string_value (b, desc);
	json_builder_end_object (b);
}

/*
 * mcp_schema_begin:
 * @b: JsonBuilder — caller must g_object_unref after mcp_schema_end
 *
 * Starts a standard JSON Schema object with type=object and opens
 * the "properties" sub-object. Caller adds properties then calls
 * mcp_schema_end().
 */
static void
mcp_schema_begin (JsonBuilder *b)
{
	json_builder_begin_object (b);
	json_builder_set_member_name (b, "type");
	json_builder_add_string_value (b, "object");
	json_builder_set_member_name (b, "properties");
	json_builder_begin_object (b);
}

/*
 * mcp_schema_end:
 * @b: JsonBuilder
 *
 * Closes the properties object and the root schema object.
 *
 * Returns: (transfer full): the root JsonNode for mcp_tool_set_input_schema()
 */
static JsonNode *
mcp_schema_end (JsonBuilder *b)
{
	json_builder_end_object (b); /* properties */
	json_builder_end_object (b); /* root       */
	return json_builder_get_root (b);
}

/* ── arg extraction helpers ─────────────────────────────────────────────── */

/*
 * mcp_arg_str:
 * @args: (nullable): JsonObject of arguments
 * @key: member name to fetch
 *
 * Returns the string value of @key from @args, or NULL if absent or wrong type.
 *
 * Returns: (transfer none) (nullable): string value
 */
static const gchar *
mcp_arg_str (JsonObject  *args,
              const gchar *key)
{
	JsonNode *node;
	if (!args) return NULL;
	node = json_object_get_member (args, key);
	if (!node || JSON_NODE_TYPE (node) != JSON_NODE_VALUE) return NULL;
	if (json_node_get_value_type (node) != G_TYPE_STRING) return NULL;
	return json_node_get_string (node);
}

/*
 * mcp_arg_bool:
 * @args: (nullable): JsonObject of arguments
 * @key: member name to fetch
 * @def: default value when key is absent
 *
 * Returns the boolean value of @key from @args, or @def if absent.
 *
 * Returns: boolean value
 */
static gboolean
mcp_arg_bool (JsonObject  *args,
               const gchar *key,
               gboolean     def)
{
	JsonNode *node;
	if (!args) return def;
	node = json_object_get_member (args, key);
	if (!node || JSON_NODE_TYPE (node) != JSON_NODE_VALUE) return def;
	if (json_node_get_value_type (node) != G_TYPE_BOOLEAN) return def;
	return json_node_get_boolean (node);
}

/*
 * mcp_arg_int:
 * @args: (nullable): JsonObject of arguments
 * @key: member name to fetch
 * @def: default value when key is absent
 *
 * Returns the integer value of @key from @args, or @def if absent.
 *
 * Returns: integer value
 */
static gint
mcp_arg_int (JsonObject  *args,
              const gchar *key,
              gint         def)
{
	JsonNode *node;
	if (!args) return def;
	node = json_object_get_member (args, key);
	if (!node || JSON_NODE_TYPE (node) != JSON_NODE_VALUE) return def;
	return (gint) json_node_get_int (node);
}

/* ── result helpers ─────────────────────────────────────────────────────── */

/*
 * mcp_result_text:
 * @text: text to wrap
 *
 * Create a successful McpToolResult containing @text.
 *
 * Returns: (transfer full): new McpToolResult
 */
static McpToolResult *
mcp_result_text (const gchar *text)
{
	McpToolResult *r = mcp_tool_result_new (FALSE);
	mcp_tool_result_add_text (r, text ? text : "");
	return r;
}

/*
 * mcp_result_error:
 * @msg: error message
 *
 * Create an error McpToolResult containing @msg.
 *
 * Returns: (transfer full): new McpToolResult
 */
static McpToolResult *
mcp_result_error (const gchar *msg)
{
	McpToolResult *r = mcp_tool_result_new (TRUE);
	mcp_tool_result_add_text (r, msg ? msg : "Unknown error");
	return r;
}

/*
 * mcp_tickets_to_json_str:
 * @tickets: (element-type VimbanTicket): array of tickets
 *
 * Serialise @tickets to a pretty-printed JSON array string.
 *
 * Returns: (transfer full): JSON string
 */
static gchar *
mcp_tickets_to_json_str (const GPtrArray *tickets)
{
	g_autoptr(JsonBuilder) b = json_builder_new ();
	g_autoptr(JsonGenerator) gen = json_generator_new ();
	guint i;

	json_builder_begin_array (b);
	for (i = 0; i < tickets->len; i++)
	{
		VimbanTicket *t = g_ptr_array_index (tickets, i);
		JsonNode *node = vimban_ticket_to_json_node (t);
		json_builder_add_value (b, node);
	}
	json_builder_end_array (b);

	{
		JsonNode *root = json_builder_get_root (b);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, root);
		json_node_unref (root);
	}

	return json_generator_to_data (gen, NULL);
}

/* ── tool handlers ──────────────────────────────────────────────────────── */

/*
 * mcp_handle_list_tickets:
 *
 * MCP handler for "list_tickets". Scans all project tickets, applies
 * filters from args, and returns a JSON array of matching tickets.
 */
static McpToolResult *
mcp_handle_list_tickets (McpServer   *server,
                          const gchar *name,
                          JsonObject  *args,
                          gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *status    = mcp_arg_str (args, "status");
	const gchar *type      = mcp_arg_str (args, "type");
	const gchar *assignee  = mcp_arg_str (args, "assignee");
	const gchar *project   = mcp_arg_str (args, "project");
	const gchar *priority  = mcp_arg_str (args, "priority");
	const gchar *tag       = mcp_arg_str (args, "tag");
	gboolean     mine      = mcp_arg_bool (args, "mine", FALSE);
	gboolean     overdue   = mcp_arg_bool (args, "overdue", FALSE);
	gboolean     unassigned = mcp_arg_bool (args, "unassigned", FALSE);

	g_auto(GStrv) search_paths = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) filtered = NULL;
	g_autofree gchar *json_str = NULL;

	(void) server; (void) name;

	all = g_ptr_array_new_with_free_func ((GDestroyNotify) vimban_ticket_free);

	search_paths = vimban_get_search_paths (ctx->config, FALSE, FALSE);
	{
		gint i;
		for (i = 0; search_paths[i]; i++)
		{
			const gchar *exclude_types[] = { "person", "area", "resource", NULL };
			g_autoptr(GPtrArray) sub = vimban_scan_tickets (
				search_paths[i], FALSE, exclude_types);
			while (sub->len > 0)
				g_ptr_array_add (all, g_ptr_array_steal_index (sub, 0));
		}
	}

	filtered = vimban_filter_tickets (all, status, type, priority,
	                                   assignee, project, tag, NULL,
	                                   mine, overdue, unassigned);

	json_str = mcp_tickets_to_json_str (filtered);
	return mcp_result_text (json_str);
}

/*
 * mcp_handle_show_ticket:
 *
 * MCP handler for "show_ticket". Finds a ticket by ID and returns its
 * JSON representation.
 */
static McpToolResult *
mcp_handle_show_ticket (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	/* accept both "ticket" (Python compat) and "ticket_id" */
	const gchar *ticket_id = mcp_arg_str (args, "ticket");
	g_autoptr(VimbanTicket) ticket = NULL;
	g_autofree gchar *out = NULL;

	(void) server; (void) name;

	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket_id");

	if (!ticket_id || !ticket_id[0])
		return mcp_result_error ("ticket is required");

	ticket = vimban_find_ticket (ctx->config->directory, ticket_id, ctx->config->prefix);
	if (!ticket)
	{
		g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_id);
		return mcp_result_error (msg);
	}

	{
		g_autoptr(JsonBuilder) b = json_builder_new ();
		g_autoptr(JsonGenerator) gen = json_generator_new ();
		JsonNode *node = vimban_ticket_to_json_node (ticket);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, node);
		json_node_unref (node);
		out = json_generator_to_data (gen, NULL);
		(void) b;
	}

	return mcp_result_text (out);
}

/*
 * mcp_handle_create_ticket:
 *
 * MCP handler for "create_ticket". Creates a new ticket file from args.
 * Does NOT open an editor (no-edit mode for MCP context).
 */
static McpToolResult *
mcp_handle_create_ticket (McpServer   *server,
                           const gchar *name,
                           JsonObject  *args,
                           gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *type_arg  = mcp_arg_str (args, "type");
	const gchar *title_arg = mcp_arg_str (args, "title");
	const gchar *priority  = mcp_arg_str (args, "priority");
	const gchar *assignee  = mcp_arg_str (args, "assignee");
	const gchar *project   = mcp_arg_str (args, "project");
	const gchar *tags      = mcp_arg_str (args, "tags");
	const gchar *due       = mcp_arg_str (args, "due_date");
	const gchar *scope_str = mcp_arg_str (args, "scope");

	g_autofree gchar *type_str    = NULL;
	g_autofree gchar *ticket_id   = NULL;
	g_autofree gchar *safe_title  = NULL;
	g_autofree gchar *output_path = NULL;
	g_autofree gchar *now_str     = NULL;
	g_autofree gchar *content     = NULL;
	g_autofree gchar *tmpl_name   = NULL;
	g_autofree gchar *tmpl        = NULL;
	g_autofree gchar *assignee_str = NULL;
	g_autofree gchar *vimban_dir  = NULL;
	g_autoptr(GDateTime) now      = NULL;
	g_autoptr(GHashTable) replacements = NULL;
	VimbanTransclusionLink *assignee_link = NULL;
	const gchar *scope = "work";
	g_autoptr(GError) err = NULL;

	(void) server; (void) name;

	if (!type_arg || !type_arg[0])
		return mcp_result_error ("type is required");
	if (!title_arg || !title_arg[0])
		return mcp_result_error ("title is required");

	type_str = vimban_normalize_type_name (type_arg);
	if (!vimban_is_valid_type (type_str))
	{
		g_autofree gchar *msg = g_strdup_printf ("Unknown ticket type: %s", type_str);
		return mcp_result_error (msg);
	}

	vimban_dir = g_build_filename (ctx->config->directory, CONFIG_DIR_NAME, NULL);
	if (!g_file_test (vimban_dir, G_FILE_TEST_IS_DIR))
		return mcp_result_error ("vimban not initialized in this directory");

	ticket_id = vimban_next_id (ctx->config->directory, NULL, NULL,
	                             type_str, ctx->config);
	if (!ticket_id)
		return mcp_result_error ("Failed to generate ticket ID (sequence lock failed)");

	if (assignee && assignee[0])
		assignee_link = vimban_resolve_person_ref (
			assignee, ctx->config->directory, ctx->config);

	assignee_str = assignee_link
		? vimban_transclusion_link_to_string (assignee_link)
		: g_strdup ("");

	/* load or build template */
	tmpl_name = g_strdup_printf ("%s.md", type_str);
	tmpl = vimban_load_template (tmpl_name);
	if (!tmpl)
	{
		now = g_date_time_new_now_local ();
		now_str = g_date_time_format_iso8601 (now);
		tmpl = g_strdup_printf (
			"---\n"
			"id: {{ID}}\n"
			"title: {{TITLE}}\n"
			"type: %s\n"
			"status: backlog\n"
			"priority: {{PRIORITY}}\n"
			"assignee: {{ASSIGNEE}}\n"
			"created: {{CREATED}}\n"
			"---\n\n",
			type_str);
	}

	now = g_date_time_new_now_local ();
	now_str = g_date_time_format_iso8601 (now);

	replacements = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert (replacements, (gpointer)"{{ID}}",
	                     g_strdup (ticket_id));
	g_hash_table_insert (replacements, (gpointer)"{{TITLE}}",
	                     g_strdup (title_arg));
	g_hash_table_insert (replacements, (gpointer)"{{TYPE}}",
	                     g_strdup (type_str));
	g_hash_table_insert (replacements, (gpointer)"{{STATUS}}",
	                     g_strdup (ctx->config->default_status));
	g_hash_table_insert (replacements, (gpointer)"{{PRIORITY}}",
	                     g_strdup (priority ? priority : ctx->config->default_priority));
	g_hash_table_insert (replacements, (gpointer)"{{ASSIGNEE}}",
	                     g_strdup (assignee_str));
	g_hash_table_insert (replacements, (gpointer)"{{REPORTER}}",
	                     g_strdup (""));
	g_hash_table_insert (replacements, (gpointer)"{{CREATED}}",
	                     g_strdup (now_str));
	g_hash_table_insert (replacements, (gpointer)"{{UPDATED}}",
	                     g_strdup (now_str));
	g_hash_table_insert (replacements, (gpointer)"{{TAGS}}",
	                     g_strdup (tags ? tags : ""));
	g_hash_table_insert (replacements, (gpointer)"{{PROJECT}}",
	                     g_strdup (project ? project : ""));

	if (due && due[0])
	{
		g_autoptr(GDate) due_date = vimban_parse_date (due);
		g_autofree gchar *due_str = vimban_date_to_string (due_date);
		g_hash_table_insert (replacements, (gpointer)"{{DUE_DATE}}",
		                     g_strdup (due_str));
	}
	else
	{
		g_hash_table_insert (replacements, (gpointer)"{{DUE_DATE}}",
		                     g_strdup (""));
	}

	{
		g_autoptr(GDate) today = g_date_new ();
		g_date_set_time_t (today, time (NULL));
		g_autofree gchar *today_str = vimban_date_to_string (today);
		g_hash_table_insert (replacements, (gpointer)"{{DATE}}",
		                     g_strdup (today_str));
	}

	content = vimban_fill_template (tmpl, replacements);

	if (scope_str && g_strcmp0 (scope_str, "personal") == 0)
		scope = "personal";

	safe_title = vimban_sanitize_filename (title_arg, 50);
	{
		g_autofree gchar *filename = g_strdup_printf ("%s_%s.md", ticket_id, safe_title);
		output_path = g_build_filename (ctx->config->directory,
		                                 "01_projects", scope,
		                                 type_str, filename, NULL);
	}

	{
		g_autofree gchar *parent = g_path_get_dirname (output_path);
		g_mkdir_with_parents (parent, 0755);
	}

	if (!g_file_set_contents (output_path, content, -1, &err))
	{
		g_autofree gchar *msg = g_strdup_printf ("Failed to write file: %s",
		                                          err ? err->message : "unknown");
		vimban_transclusion_link_free (assignee_link);
		return mcp_result_error (msg);
	}

	vimban_transclusion_link_free (assignee_link);

	{
		g_autofree gchar *out = g_strdup_printf (
			"{\"id\": \"%s\", \"path\": \"%s\"}", ticket_id, output_path);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_edit_ticket:
 *
 * MCP handler for "edit_ticket". Updates fields on an existing ticket.
 */
static McpToolResult *
mcp_handle_edit_ticket (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	/* accept both "ticket" (Python compat) and "ticket_id" */
	const gchar *ticket_ref = mcp_arg_str (args, "ticket");
	const gchar *status     = mcp_arg_str (args, "status");
	const gchar *priority   = mcp_arg_str (args, "priority");
	const gchar *assignee   = mcp_arg_str (args, "assignee");
	const gchar *add_tag    = mcp_arg_str (args, "add_tag");
	const gchar *remove_tag = mcp_arg_str (args, "remove_tag");
	const gchar *due        = mcp_arg_str (args, "due_date");
	const gchar *clear_field = mcp_arg_str (args, "clear_field");
	gint         progress   = mcp_arg_int (args, "progress", -1);

	g_autoptr(VimbanTicket) ticket = NULL;
	g_autofree gchar *raw  = NULL;
	g_autofree gchar *body = NULL;
	YamlMapping *mapping   = NULL;

	(void) server; (void) name;

	/* fallback to "ticket_id" for backward compat */
	if (!ticket_ref || !ticket_ref[0])
		ticket_ref = mcp_arg_str (args, "ticket_id");

	if (!ticket_ref || !ticket_ref[0])
		return mcp_result_error ("ticket is required");

	ticket = vimban_find_ticket (ctx->config->directory, ticket_ref, ctx->config->prefix);
	if (!ticket)
	{
		g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_ref);
		return mcp_result_error (msg);
	}

	if (!g_file_get_contents (ticket->filepath, &raw, NULL, NULL))
		return mcp_result_error ("Failed to read ticket file");
	vimban_parse_frontmatter (raw, &mapping, &body);
	if (!mapping) mapping = yaml_mapping_new ();

	/* apply each field update */
	if (status && status[0])
		yaml_mapping_set_string_member (mapping, "status", status);

	if (priority && priority[0])
		yaml_mapping_set_string_member (mapping, "priority", priority);

	if (assignee && assignee[0])
	{
		VimbanTransclusionLink *link = vimban_resolve_person_ref (
			assignee, ctx->config->directory, ctx->config);
		if (link)
		{
			g_autofree gchar *link_str = vimban_transclusion_link_to_string (link);
			yaml_mapping_set_string_member (mapping, "assignee", link_str);
			vimban_transclusion_link_free (link);
		}
		else
		{
			yaml_mapping_set_string_member (mapping, "assignee", assignee);
		}
	}

	if (due && due[0])
	{
		g_autoptr(GDate) due_date = vimban_parse_date (due);
		g_autofree gchar *due_str = vimban_date_to_string (due_date);
		yaml_mapping_set_string_member (mapping, "due_date", due_str);
	}

	if (progress >= 0)
		yaml_mapping_set_int_member (mapping, "progress", progress);

	/* add/remove tags: use _parse_string_list pattern from cmd_edit */
	if ((add_tag && add_tag[0]) || (remove_tag && remove_tag[0]))
	{
		GStrv cur_tags = _parse_string_list (mapping, "tags");
		GPtrArray *new_tags = g_ptr_array_new ();
		gint j;

		/* copy existing tags, skipping the one to remove */
		if (cur_tags)
		{
			for (j = 0; cur_tags[j]; j++)
			{
				if (remove_tag && g_strcmp0 (cur_tags[j], remove_tag) == 0)
					continue;
				g_ptr_array_add (new_tags, cur_tags[j]);
			}
		}

		/* add the new tag if not already present */
		if (add_tag && add_tag[0])
		{
			gboolean found = FALSE;
			for (j = 0; (guint) j < new_tags->len; j++)
				if (g_strcmp0 (g_ptr_array_index (new_tags, j), add_tag) == 0)
				{ found = TRUE; break; }
			if (!found)
				g_ptr_array_add (new_tags, (gpointer) add_tag);
		}

		{
			YamlSequence *seq = yaml_sequence_new ();
			for (j = 0; (guint) j < new_tags->len; j++)
				yaml_sequence_add_string_element (seq, g_ptr_array_index (new_tags, j));
			yaml_mapping_set_sequence_member (mapping, "tags", seq);
			yaml_sequence_unref (seq);
		}

		g_ptr_array_free (new_tags, FALSE);
		g_strfreev (cur_tags);
	}

	/* clear a field if requested */
	if (clear_field && clear_field[0])
	{
		if (yaml_mapping_has_member (mapping, clear_field))
			yaml_mapping_remove_member (mapping, clear_field);
	}

	/* bump version and updated timestamp */
	{
		g_autoptr(GDateTime) now_dt = g_date_time_new_now_local ();
		g_autofree gchar *now_s = g_date_time_format_iso8601 (now_dt);
		gint64 ver = 0;
		if (yaml_mapping_has_member (mapping, "version"))
			ver = yaml_mapping_get_int_member (mapping, "version");
		yaml_mapping_set_int_member (mapping, "version", ver + 1);
		yaml_mapping_set_string_member (mapping, "updated", now_s);
	}

	/* write back */
	{
		g_autofree gchar *new_content = vimban_dump_frontmatter (mapping, body);
		g_autoptr(GError) werr = NULL;
		if (!g_file_set_contents (ticket->filepath, new_content, -1, &werr))
		{
			yaml_mapping_unref (mapping);
			g_autofree gchar *msg = g_strdup_printf ("Failed to write: %s",
			                                          werr ? werr->message : "unknown");
			return mcp_result_error (msg);
		}
	}

	yaml_mapping_unref (mapping);

	{
		g_autofree gchar *out = g_strdup_printf (
			"Updated ticket %s", ticket->id ? ticket->id : ticket_ref);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_move_ticket:
 *
 * MCP handler for "move_ticket". Changes a ticket's status.
 */
static McpToolResult *
mcp_handle_move_ticket (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	/* accept both "ticket" (Python compat) and "ticket_id" */
	const gchar *ticket_ref = mcp_arg_str (args, "ticket");
	const gchar *new_status = mcp_arg_str (args, "status");
	gboolean     force      = mcp_arg_bool (args, "force", FALSE);
	gboolean     reopen     = mcp_arg_bool (args, "reopen", FALSE);
	const gchar *comment    = mcp_arg_str (args, "comment");

	/* delegate to cmd_move via argv construction */
	GPtrArray *av = g_ptr_array_new ();

	(void) server; (void) name;

	/* fallback to "ticket_id" for backward compat */
	if (!ticket_ref || !ticket_ref[0])
		ticket_ref = mcp_arg_str (args, "ticket_id");

	if (!ticket_ref || !ticket_ref[0])
		return mcp_result_error ("ticket is required");
	if (!new_status || !new_status[0])
		return mcp_result_error ("status is required");

	g_ptr_array_add (av, (gpointer) "move");
	g_ptr_array_add (av, (gpointer) ticket_ref);
	g_ptr_array_add (av, (gpointer) new_status);
	if (force)
		g_ptr_array_add (av, (gpointer) "--force");
	if (reopen)
		g_ptr_array_add (av, (gpointer) "--reopen");
	if (comment && comment[0])
	{
		g_ptr_array_add (av, (gpointer) "--comment");
		g_ptr_array_add (av, (gpointer) comment);
	}
	g_ptr_array_add (av, NULL);

	{
		gint ret = cmd_move ((gint) av->len - 1, (gchar **) av->pdata,
		                      ctx->global, ctx->config);
		g_ptr_array_free (av, FALSE);
		if (ret != VIMBAN_EXIT_SUCCESS)
		{
			g_autofree gchar *msg = g_strdup_printf (
				"Failed to move ticket %s to %s", ticket_ref, new_status);
			return mcp_result_error (msg);
		}
	}

	{
		g_autofree gchar *out = g_strdup_printf (
			"Moved %s to %s", ticket_ref, new_status);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_search_tickets:
 *
 * MCP handler for "search_tickets". Runs rg over the notes directory
 * and returns matching lines.
 */
static McpToolResult *
mcp_handle_search_tickets (McpServer   *server,
                             const gchar *name,
                             JsonObject  *args,
                             gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *query     = mcp_arg_str (args, "query");
	gboolean     regex     = mcp_arg_bool (args, "regex", FALSE);
	gboolean     case_sens = mcp_arg_bool (args, "case_sensitive", FALSE);
	gboolean     files_only = mcp_arg_bool (args, "files_only", FALSE);

	GPtrArray *cmd_arr;
	gchar *rg_stdout = NULL;
	gchar *rg_stderr = NULL;
	gint exit_status = 0;
	g_autoptr(GError) spawn_err = NULL;
	McpToolResult *result;

	(void) server; (void) name;

	if (!query || !query[0])
		return mcp_result_error ("query is required");

	cmd_arr = g_ptr_array_new ();
	g_ptr_array_add (cmd_arr, (gpointer) "rg");
	if (!case_sens)
		g_ptr_array_add (cmd_arr, (gpointer) "-i");
	if (files_only)
		g_ptr_array_add (cmd_arr, (gpointer) "-l");
	g_ptr_array_add (cmd_arr, regex ? (gpointer)"-e" : (gpointer)"-F");
	g_ptr_array_add (cmd_arr, (gpointer) query);
	g_ptr_array_add (cmd_arr, (gpointer) ctx->config->directory);
	g_ptr_array_add (cmd_arr, (gpointer) "--glob");
	g_ptr_array_add (cmd_arr, (gpointer) "*.md");
	g_ptr_array_add (cmd_arr, NULL);

	g_spawn_sync (NULL, (gchar **) cmd_arr->pdata, NULL,
	              G_SPAWN_SEARCH_PATH, NULL, NULL,
	              &rg_stdout, &rg_stderr,
	              &exit_status, &spawn_err);

	g_ptr_array_free (cmd_arr, FALSE);

	if (rg_stdout && rg_stdout[0])
		result = mcp_result_text (rg_stdout);
	else
		result = mcp_result_text ("No matches found");

	g_free (rg_stdout);
	g_free (rg_stderr);
	return result;
}

/*
 * mcp_handle_get_dashboard:
 *
 * MCP handler for "get_dashboard". Builds and returns a dashboard view.
 */
static McpToolResult *
mcp_handle_get_dashboard (McpServer   *server,
                            const gchar *name,
                            JsonObject  *args,
                            gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *dash_type = mcp_arg_str (args, "type");

	GPtrArray *av = g_ptr_array_new ();
	g_autofree gchar *type_copy = NULL;

	(void) server; (void) name;

	if (!dash_type || !dash_type[0])
		dash_type = "daily";

	type_copy = g_strdup (dash_type);

	g_ptr_array_add (av, (gpointer) "dashboard");
	g_ptr_array_add (av, type_copy);
	g_ptr_array_add (av, NULL);

	/* cmd_dashboard prints to stdout; capture via pipe */
	{
		static GMutex stdout_mutex;
		gint pipefd[2];
		gint saved_stdout = -1;
		gchar buf[65536];
		gssize nread;
		GString *out_str;

		if (pipe (pipefd) != 0)
		{
			g_ptr_array_free (av, FALSE);
			return mcp_result_error ("pipe() failed");
		}

		g_mutex_lock (&stdout_mutex);
		saved_stdout = dup (STDOUT_FILENO);
		if (saved_stdout < 0)
		{
			g_mutex_unlock (&stdout_mutex);
			close (pipefd[0]);
			close (pipefd[1]);
			g_ptr_array_free (av, FALSE);
			return mcp_result_error ("dup() failed");
		}
		dup2 (pipefd[1], STDOUT_FILENO);
		close (pipefd[1]);

		cmd_dashboard ((gint) av->len - 1, (gchar **) av->pdata,
		               ctx->global, ctx->config);

		fflush (stdout);
		dup2 (saved_stdout, STDOUT_FILENO);
		close (saved_stdout);
		g_mutex_unlock (&stdout_mutex);

		out_str = g_string_new ("");
		while ((nread = read (pipefd[0], buf, sizeof (buf) - 1)) > 0)
		{
			buf[nread] = '\0';
			g_string_append (out_str, buf);
		}
		close (pipefd[0]);

		g_ptr_array_free (av, FALSE);

		{
			gchar *text = g_string_free (out_str, FALSE);
			McpToolResult *r = mcp_result_text (text[0] ? text : "(empty dashboard)");
			g_free (text);
			return r;
		}
	}
}

/*
 * mcp_handle_get_kanban:
 *
 * MCP handler for "get_kanban". Returns kanban board as text.
 */
static McpToolResult *
mcp_handle_get_kanban (McpServer   *server,
                        const gchar *name,
                        JsonObject  *args,
                        gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *project  = mcp_arg_str (args, "project");
	const gchar *assignee = mcp_arg_str (args, "assignee");
	gboolean     mine     = mcp_arg_bool (args, "mine", FALSE);

	GPtrArray *av = g_ptr_array_new ();
	g_autofree gchar *project_flag  = NULL;
	g_autofree gchar *assignee_flag = NULL;

	(void) server; (void) name;

	g_ptr_array_add (av, (gpointer) "kanban");
	if (project && project[0])
	{
		project_flag = g_strdup_printf ("--project=%s", project);
		g_ptr_array_add (av, project_flag);
	}
	if (assignee && assignee[0])
	{
		assignee_flag = g_strdup_printf ("--assignee=%s", assignee);
		g_ptr_array_add (av, assignee_flag);
	}
	if (mine)
		g_ptr_array_add (av, (gpointer) "--mine");
	g_ptr_array_add (av, (gpointer) "--no-color");
	g_ptr_array_add (av, NULL);

	{
		static GMutex stdout_mutex;
		gint pipefd[2];
		gint saved_stdout = -1;
		gchar buf[65536];
		gssize nread;
		GString *out_str;

		if (pipe (pipefd) != 0)
		{
			g_ptr_array_free (av, FALSE);
			return mcp_result_error ("pipe() failed");
		}

		g_mutex_lock (&stdout_mutex);
		saved_stdout = dup (STDOUT_FILENO);
		if (saved_stdout < 0)
		{
			g_mutex_unlock (&stdout_mutex);
			close (pipefd[0]);
			close (pipefd[1]);
			g_ptr_array_free (av, FALSE);
			return mcp_result_error ("dup() failed");
		}
		dup2 (pipefd[1], STDOUT_FILENO);
		close (pipefd[1]);

		ctx->global->no_color = TRUE;
		cmd_kanban ((gint) av->len - 1, (gchar **) av->pdata,
		            ctx->global, ctx->config);
		ctx->global->no_color = FALSE;

		fflush (stdout);
		dup2 (saved_stdout, STDOUT_FILENO);
		close (saved_stdout);
		g_mutex_unlock (&stdout_mutex);

		out_str = g_string_new ("");
		while ((nread = read (pipefd[0], buf, sizeof (buf) - 1)) > 0)
		{
			buf[nread] = '\0';
			g_string_append (out_str, buf);
		}
		close (pipefd[0]);
		g_ptr_array_free (av, FALSE);

		{
			gchar *text = g_string_free (out_str, FALSE);
			McpToolResult *r = mcp_result_text (text[0] ? text : "(empty kanban)");
			g_free (text);
			return r;
		}
	}
}

/*
 * mcp_handle_list_people:
 *
 * MCP handler for "list_people". Returns JSON array of all people.
 */
static McpToolResult *
mcp_handle_list_people (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	g_autofree gchar *people_path = NULL;
	g_autoptr(GPtrArray) people  = NULL;
	g_autoptr(JsonBuilder) b     = json_builder_new ();
	g_autoptr(JsonGenerator) gen = json_generator_new ();
	g_autofree gchar *out        = NULL;

	(void) server; (void) name; (void) args;

	people_path = g_build_filename (ctx->config->directory,
	                                 ctx->config->people_dir, NULL);
	people = vimban_scan_people (people_path);

	json_builder_begin_array (b);
	{
		guint i;
		for (i = 0; i < people->len; i++)
		{
			VimbanPerson *p = g_ptr_array_index (people, i);
			JsonNode *node = vimban_person_to_json_node (p);
			json_builder_add_value (b, node);
		}
	}
	json_builder_end_array (b);

	{
		JsonNode *root = json_builder_get_root (b);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, root);
		json_node_unref (root);
	}

	out = json_generator_to_data (gen, NULL);
	return mcp_result_text (out);
}

/*
 * mcp_handle_show_person:
 *
 * MCP handler for "show_person". Returns JSON for a single person.
 */
static McpToolResult *
mcp_handle_show_person (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *name_ref = mcp_arg_str (args, "name");
	g_autoptr(VimbanPerson) person = NULL;

	(void) server; (void) name;

	if (!name_ref || !name_ref[0])
		return mcp_result_error ("name is required");

	person = vimban_find_person (ctx->config->directory, ctx->config, name_ref);
	if (!person)
	{
		g_autofree gchar *msg = g_strdup_printf ("Person not found: %s", name_ref);
		return mcp_result_error (msg);
	}

	{
		g_autoptr(JsonGenerator) gen = json_generator_new ();
		g_autofree gchar *out = NULL;
		JsonNode *node = vimban_person_to_json_node (person);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, node);
		json_node_unref (node);
		out = json_generator_to_data (gen, NULL);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_search_people:
 *
 * MCP handler for "search_people". Fuzzy-matches people by name.
 */
static McpToolResult *
mcp_handle_search_people (McpServer   *server,
                            const gchar *name,
                            JsonObject  *args,
                            gpointer     user_data)
{
	VimbanMcpContext *ctx   = (VimbanMcpContext *) user_data;
	const gchar *query       = mcp_arg_str (args, "query");
	g_autofree gchar *people_path = NULL;
	g_autoptr(GPtrArray) people   = NULL;
	g_autofree gchar *q_lower     = NULL;
	g_autoptr(JsonBuilder) b      = json_builder_new ();
	g_autoptr(JsonGenerator) gen  = json_generator_new ();
	g_autofree gchar *out         = NULL;
	guint i;

	(void) server; (void) name;

	if (!query || !query[0])
		return mcp_result_error ("query is required");

	people_path = g_build_filename (ctx->config->directory,
	                                 ctx->config->people_dir, NULL);
	people  = vimban_scan_people (people_path);
	q_lower = g_ascii_strdown (query, -1);

	json_builder_begin_array (b);
	for (i = 0; i < people->len; i++)
	{
		VimbanPerson *p = g_ptr_array_index (people, i);
		g_autofree gchar *n_lower = g_ascii_strdown (
			p->name ? p->name : "", -1);
		if (strstr (n_lower, q_lower))
		{
			JsonNode *node = vimban_person_to_json_node (p);
			json_builder_add_value (b, node);
		}
	}
	json_builder_end_array (b);

	{
		JsonNode *root = json_builder_get_root (b);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, root);
		json_node_unref (root);
	}

	out = json_generator_to_data (gen, NULL);
	return mcp_result_text (out);
}

/*
 * mcp_handle_create_person:
 *
 * MCP handler for "create_person". Creates a new person markdown file.
 */
static McpToolResult *
mcp_handle_create_person (McpServer   *server,
                            const gchar *name,
                            JsonObject  *args,
                            gpointer     user_data)
{
	VimbanMcpContext *ctx      = (VimbanMcpContext *) user_data;
	const gchar *person_name   = mcp_arg_str (args, "name");
	const gchar *email         = mcp_arg_str (args, "email");
	const gchar *role          = mcp_arg_str (args, "role");
	const gchar *team          = mcp_arg_str (args, "team");

	g_autofree gchar *people_path   = NULL;
	g_autofree gchar *filename_base = NULL;
	g_autofree gchar *filename      = NULL;
	g_autofree gchar *filepath      = NULL;
	g_autofree gchar *person_id     = NULL;
	g_autofree gchar *now_str       = NULL;
	g_autofree gchar *content       = NULL;
	g_autoptr(GDateTime) now        = NULL;
	gchar *p;

	(void) server; (void) name;

	if (!person_name || !person_name[0])
		return mcp_result_error ("name is required");

	people_path   = g_build_filename (ctx->config->directory,
	                                   ctx->config->people_dir, NULL);
	filename_base = g_ascii_strdown (person_name, -1);
	for (p = filename_base; *p; p++) if (*p == ' ') *p = '_';
	filename = g_strdup_printf ("%s.md", filename_base);
	filepath = g_build_filename (people_path, filename, NULL);

	if (g_file_test (filepath, G_FILE_TEST_EXISTS))
	{
		g_autofree gchar *msg = g_strdup_printf (
			"Person file already exists: %s", filepath);
		return mcp_result_error (msg);
	}

	person_id = vimban_next_id (ctx->config->directory, NULL, NULL,
	                             "person", ctx->config);
	if (!person_id)
		return mcp_result_error ("Failed to generate person ID (sequence lock failed)");
	now     = g_date_time_new_now_local ();
	now_str = g_date_time_format_iso8601 (now);

	content = g_strdup_printf (
		"---\nid: \"%s\"\nname: %s\ntype: person\ncreated: %s%s%s%s%s\n---\n\n",
		person_id, person_name, now_str,
		email ? "\nemail: " : "", email ? email : "",
		role  ? "\nrole: "  : "", role  ? role  : "");

	/* rebuild content more cleanly */
	{
		GString *fm = g_string_new ("---\n");
		g_string_append_printf (fm, "id: \"%s\"\n", person_id);
		g_string_append_printf (fm, "name: %s\n", person_name);
		g_string_append (fm, "type: person\n");
		g_string_append_printf (fm, "created: %s\n", now_str);
		if (email && email[0]) g_string_append_printf (fm, "email: %s\n", email);
		if (role  && role[0])  g_string_append_printf (fm, "role: %s\n",  role);
		if (team  && team[0])  g_string_append_printf (fm, "team: %s\n",  team);
		g_string_append (fm, "---\n\n");
		g_free (content);
		content = g_string_free (fm, FALSE);
	}

	g_mkdir_with_parents (people_path, 0755);

	{
		g_autoptr(GError) err = NULL;
		if (!g_file_set_contents (filepath, content, -1, &err))
		{
			g_autofree gchar *msg = g_strdup_printf (
				"Failed to write: %s", err ? err->message : "unknown");
			return mcp_result_error (msg);
		}
	}

	{
		g_autofree gchar *out = g_strdup_printf (
			"{\"id\": \"%s\", \"path\": \"%s\"}", person_id, filepath);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_get_person_dashboard:
 *
 * MCP handler for "get_person_dashboard". Returns tickets assigned to a person.
 */
static McpToolResult *
mcp_handle_get_person_dashboard (McpServer   *server,
                                   const gchar *name,
                                   JsonObject  *args,
                                   gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *person_name = mcp_arg_str (args, "name");

	g_autoptr(GPtrArray) all      = NULL;
	g_autoptr(GPtrArray) assigned = NULL;
	g_autofree gchar *json_str    = NULL;
	g_autofree gchar *name_lower  = NULL;
	guint i;

	(void) server; (void) name;

	if (!person_name || !person_name[0])
		return mcp_result_error ("name is required");

	all = vimban_scan_tickets (ctx->config->directory, FALSE, NULL);
	assigned = g_ptr_array_new ();
	name_lower = g_ascii_strdown (person_name, -1);

	for (i = 0; i < all->len; i++)
	{
		VimbanTicket *t = g_ptr_array_index (all, i);
		if (!t->assignee) continue;
		g_autofree gchar *stem = vimban_transclusion_link_get_stem (t->assignee);
		g_autofree gchar *stem_lower = g_ascii_strdown (stem, -1);
		if (strstr (stem_lower, name_lower))
			g_ptr_array_add (assigned, t);
	}

	json_str = mcp_tickets_to_json_str (assigned);
	return mcp_result_text (json_str);
}

/*
 * mcp_handle_validate_tickets:
 *
 * MCP handler for "validate_tickets". Scans all tickets and returns
 * validation errors and warnings as JSON.
 */
static McpToolResult *
mcp_handle_validate_tickets (McpServer   *server,
                               const gchar *name,
                               JsonObject  *args,
                               gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;

	GPtrArray *errors   = g_ptr_array_new_with_free_func (g_free);
	GPtrArray *warnings = g_ptr_array_new_with_free_func (g_free);
	g_autoptr(GPtrArray) tickets = vimban_scan_tickets (
		ctx->config->directory, TRUE, NULL);
	guint pi;

	(void) server; (void) name; (void) args;

	for (pi = 0; pi < tickets->len; pi++)
	{
		VimbanTicket *t = g_ptr_array_index (tickets, pi);
		if (!t->id || !t->id[0])
			g_ptr_array_add (errors,
			                 g_strdup_printf ("%s: missing id", t->filepath));
		if (!t->title || !t->title[0])
			g_ptr_array_add (errors,
			                 g_strdup_printf ("%s: missing title", t->filepath));
		if (!t->type || !t->type[0])
			g_ptr_array_add (errors,
			                 g_strdup_printf ("%s: missing type", t->filepath));
		if (t->status && !vimban_str_in_list (t->status, (const gchar **) STATUSES))
			g_ptr_array_add (warnings,
			                 g_strdup_printf ("%s: unknown status '%s'",
			                                   t->filepath, t->status));
		if (t->priority && !vimban_str_in_list (t->priority, (const gchar **) PRIORITIES))
			g_ptr_array_add (warnings,
			                 g_strdup_printf ("%s: unknown priority '%s'",
			                                   t->filepath, t->priority));
	}

	{
		g_autoptr(JsonBuilder) b = json_builder_new ();
		g_autoptr(JsonGenerator) gen = json_generator_new ();
		g_autofree gchar *out = NULL;
		guint i;

		json_builder_begin_object (b);

		json_builder_set_member_name (b, "errors");
		json_builder_begin_array (b);
		for (i = 0; i < errors->len; i++)
			json_builder_add_string_value (b, g_ptr_array_index (errors, i));
		json_builder_end_array (b);

		json_builder_set_member_name (b, "warnings");
		json_builder_begin_array (b);
		for (i = 0; i < warnings->len; i++)
			json_builder_add_string_value (b, g_ptr_array_index (warnings, i));
		json_builder_end_array (b);

		json_builder_set_member_name (b, "total_tickets");
		json_builder_add_int_value (b, (gint64) tickets->len);

		json_builder_end_object (b);

		{
			JsonNode *root = json_builder_get_root (b);
			json_generator_set_pretty (gen, TRUE);
			json_generator_set_indent (gen, 2);
			json_generator_set_root (gen, root);
			json_node_unref (root);
		}

		out = json_generator_to_data (gen, NULL);
		g_ptr_array_unref (errors);
		g_ptr_array_unref (warnings);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_add_comment:
 *
 * MCP handler for "add_comment". Appends a comment to a ticket file.
 */
static McpToolResult *
mcp_handle_add_comment (McpServer   *server,
                         const gchar *name,
                         JsonObject  *args,
                         gpointer     user_data)
{
	VimbanMcpContext *ctx  = (VimbanMcpContext *) user_data;
	/* accept "target" (Python compat), "ticket", and "ticket_id" */
	const gchar *ticket_id = mcp_arg_str (args, "target");
	const gchar *text      = mcp_arg_str (args, "text");
	const gchar *author    = mcp_arg_str (args, "author");

	g_autoptr(VimbanTicket) ticket = NULL;
	g_autoptr(GError) err = NULL;
	gint comment_id;

	(void) server; (void) name;

	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket");
	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket_id");

	if (!ticket_id || !ticket_id[0])
		return mcp_result_error ("target is required");
	if (!text || !text[0])
		return mcp_result_error ("text is required");

	ticket = vimban_find_ticket (ctx->config->directory, ticket_id, ctx->config->prefix);
	if (!ticket)
	{
		g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_id);
		return mcp_result_error (msg);
	}

	if (!author || !author[0])
		author = g_getenv ("USER");

	comment_id = vimban_insert_comment (ticket->filepath, text, -1,
	                                     author, &err);
	if (comment_id < 0)
	{
		g_autofree gchar *msg = g_strdup_printf ("Failed to add comment: %s",
		                                          err ? err->message : "unknown");
		return mcp_result_error (msg);
	}

	{
		g_autofree gchar *out = g_strdup_printf (
			"{\"comment_id\": %d, \"ticket\": \"%s\"}",
			comment_id, ticket->id ? ticket->id : ticket_id);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_get_comments:
 *
 * MCP handler for "get_comments". Returns comments on a ticket as JSON.
 */
static McpToolResult *
mcp_handle_get_comments (McpServer   *server,
                          const gchar *name,
                          JsonObject  *args,
                          gpointer     user_data)
{
	VimbanMcpContext *ctx  = (VimbanMcpContext *) user_data;
	/* accept "target" (Python compat), "ticket", and "ticket_id" */
	const gchar *ticket_id = mcp_arg_str (args, "target");
	g_autoptr(VimbanTicket) ticket = NULL;
	g_autofree gchar *raw  = NULL;

	(void) server; (void) name;

	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket");
	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket_id");

	if (!ticket_id || !ticket_id[0])
		return mcp_result_error ("target is required");

	ticket = vimban_find_ticket (ctx->config->directory, ticket_id, ctx->config->prefix);
	if (!ticket)
	{
		g_autofree gchar *msg = g_strdup_printf ("Ticket not found: %s", ticket_id);
		return mcp_result_error (msg);
	}

	g_file_get_contents (ticket->filepath, &raw, NULL, NULL);
	if (!raw)
		return mcp_result_error ("Failed to read ticket file");

	{
		g_autoptr(GPtrArray) comments = vimban_parse_comments (raw);
		g_autoptr(JsonBuilder) b = json_builder_new ();
		g_autoptr(JsonGenerator) gen = json_generator_new ();
		g_autofree gchar *out = NULL;
		guint i;

		json_builder_begin_array (b);
		for (i = 0; i < comments->len; i++)
		{
			VimbanComment *c = g_ptr_array_index (comments, i);
			json_builder_begin_object (b);
			json_builder_set_member_name (b, "id");
			json_builder_add_int_value (b, c->id);
			json_builder_set_member_name (b, "author");
			json_builder_add_string_value (b, c->author ? c->author : "");
			json_builder_set_member_name (b, "content");
			json_builder_add_string_value (b, c->content ? c->content : "");
			if (c->timestamp)
			{
				g_autofree gchar *ts = g_date_time_format_iso8601 (c->timestamp);
				json_builder_set_member_name (b, "timestamp");
				json_builder_add_string_value (b, ts);
			}
			json_builder_end_object (b);
		}
		json_builder_end_array (b);

		{
			JsonNode *root = json_builder_get_root (b);
			json_generator_set_pretty (gen, TRUE);
			json_generator_set_indent (gen, 2);
			json_generator_set_root (gen, root);
			json_node_unref (root);
		}
		out = json_generator_to_data (gen, NULL);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_link_tickets:
 *
 * MCP handler for "link_tickets". Creates a relationship link between two tickets.
 */
static McpToolResult *
mcp_handle_link_tickets (McpServer   *server,
                          const gchar *name,
                          JsonObject  *args,
                          gpointer     user_data)
{
	VimbanMcpContext *ctx   = (VimbanMcpContext *) user_data;
	/* accept "ticket" (Python compat) and "ticket_id" */
	const gchar *ticket_id  = mcp_arg_str (args, "ticket");
	/* accept "target" (Python compat) and "target_id" */
	const gchar *target_id  = mcp_arg_str (args, "target");
	const gchar *relation   = mcp_arg_str (args, "relation");

	GPtrArray *av = g_ptr_array_new ();
	g_autofree gchar *rel_flag = NULL;

	(void) server; (void) name;

	if (!ticket_id || !ticket_id[0])
		ticket_id = mcp_arg_str (args, "ticket_id");
	if (!ticket_id || !ticket_id[0])
		return mcp_result_error ("ticket is required");

	if (!target_id || !target_id[0])
		target_id = mcp_arg_str (args, "target_id");
	if (!target_id || !target_id[0])
		return mcp_result_error ("target is required");
	if (!relation || !relation[0])
		relation = "relates-to";

	/* validate relation against allowlist to prevent argument injection */
	if (g_strcmp0 (relation, "blocks") != 0 &&
	    g_strcmp0 (relation, "blocked-by") != 0 &&
	    g_strcmp0 (relation, "relates-to") != 0 &&
	    g_strcmp0 (relation, "member-of") != 0 &&
	    g_strcmp0 (relation, "duplicates") != 0)
	{
		g_ptr_array_free (av, FALSE);
		return mcp_result_error ("invalid relation type; must be one of: "
		                         "blocks, blocked-by, relates-to, member-of, duplicates");
	}

	g_ptr_array_add (av, (gpointer) "link");
	g_ptr_array_add (av, (gpointer) ticket_id);
	rel_flag = g_strdup_printf ("--%s=%s", relation, target_id);
	g_ptr_array_add (av, rel_flag);
	g_ptr_array_add (av, NULL);

	{
		gint ret = cmd_link ((gint) av->len - 1, (gchar **) av->pdata,
		                      ctx->global, ctx->config);
		g_ptr_array_free (av, FALSE);
		if (ret != VIMBAN_EXIT_SUCCESS)
		{
			g_autofree gchar *msg = g_strdup_printf (
				"Failed to link %s -[%s]-> %s", ticket_id, relation, target_id);
			return mcp_result_error (msg);
		}
	}

	{
		g_autofree gchar *out = g_strdup_printf (
			"Linked %s -[%s]-> %s", ticket_id, relation, target_id);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_generate_link:
 *
 * MCP handler for "generate_link". Generates a wikilink or markdown link
 * for a ticket or person by ID.
 */
static McpToolResult *
mcp_handle_generate_link (McpServer   *server,
                            const gchar *name,
                            JsonObject  *args,
                            gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	const gchar *ref      = mcp_arg_str (args, "ref");
	const gchar *fmt      = mcp_arg_str (args, "format");

	(void) server; (void) name;

	if (!ref || !ref[0])
		return mcp_result_error ("ref is required");

	/* try ticket first */
	{
		g_autoptr(VimbanTicket) ticket = vimban_find_ticket (
			ctx->config->directory, ref, ctx->config->prefix);
		if (ticket)
		{
			g_autofree gchar *link = NULL;
			g_autofree gchar *rel_path = NULL;

			/* strip base directory to get relative path */
			if (g_str_has_prefix (ticket->filepath, ctx->config->directory))
			{
				rel_path = g_strdup (
					ticket->filepath + strlen (ctx->config->directory) + 1);
			}
			else
			{
				rel_path = g_strdup (ticket->filepath);
			}

			/* strip .md suffix */
			{
				gchar *dot = g_strrstr (rel_path, ".md");
				if (dot) *dot = '\0';
			}

			if (g_strcmp0 (fmt, "markdown") == 0)
				link = g_strdup_printf ("[%s](%s)", ticket->title, rel_path);
			else
				link = g_strdup_printf ("![[%s|%s]]", rel_path,
				                         ticket->id ? ticket->id : ref);

			return mcp_result_text (link);
		}
	}

	/* try person */
	{
		g_autoptr(VimbanPerson) person = vimban_find_person (
			ctx->config->directory, ctx->config, ref);
		if (person)
		{
			g_autofree gchar *link = NULL;
			g_autofree gchar *rel_path = NULL;

			if (g_str_has_prefix (person->filepath, ctx->config->directory))
			{
				rel_path = g_strdup (
					person->filepath + strlen (ctx->config->directory) + 1);
			}
			else
			{
				rel_path = g_strdup (person->filepath);
			}

			{
				gchar *dot = g_strrstr (rel_path, ".md");
				if (dot) *dot = '\0';
			}

			if (g_strcmp0 (fmt, "markdown") == 0)
				link = g_strdup_printf ("[%s](%s)",
				                         person->name ? person->name : ref,
				                         rel_path);
			else
				link = g_strdup_printf ("![[%s|%s]]", rel_path,
				                         person->name ? person->name : ref);

			return mcp_result_text (link);
		}
	}

	{
		g_autofree gchar *msg = g_strdup_printf ("Reference not found: %s", ref);
		return mcp_result_error (msg);
	}
}

/*
 * mcp_handle_convert_find_missing:
 *
 * MCP handler for "convert_find_missing". Finds tickets referenced in notes
 * that do not exist as files.
 */
static McpToolResult *
mcp_handle_convert_find_missing (McpServer   *server,
                                   const gchar *name,
                                   JsonObject  *args,
                                   gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	g_autoptr(GPtrArray) tickets = NULL;
	GString *out;
	guint i;
	gchar *rg_stdout = NULL;
	gchar *rg_stderr = NULL;
	gint exit_status = 0;

	(void) server; (void) name; (void) args;

	/* find all ![[...]] transclusions in notes */
	{
		gchar *rg_argv[] = {
			(gchar *) "rg", (gchar *) "-o",
			(gchar *) "!\\[\\[([^\\]]+)\\]\\]",
			(gchar *) "--only-matching",
			(gchar *) "-r", (gchar *) "$1",
			(gchar *) ctx->config->directory,
			(gchar *) "--glob", (gchar *) "*.md",
			NULL
		};
		g_spawn_sync (NULL, rg_argv, NULL, G_SPAWN_SEARCH_PATH,
		              NULL, NULL, &rg_stdout, &rg_stderr,
		              &exit_status, NULL);
	}

	tickets = vimban_scan_tickets (ctx->config->directory, TRUE, NULL);
	out = g_string_new ("");

	if (rg_stdout && rg_stdout[0])
	{
		gchar **lines = g_strsplit (rg_stdout, "\n", -1);
		gint li;
		for (li = 0; lines[li]; li++)
		{
			const gchar *ref = g_strstrip (lines[li]);
			gboolean found = FALSE;
			if (!ref || !ref[0]) continue;

			for (i = 0; i < tickets->len; i++)
			{
				VimbanTicket *t = g_ptr_array_index (tickets, i);
				g_autofree gchar *base = g_path_get_basename (t->filepath);
				gchar *dot = g_strrstr (base, ".md");
				if (dot) *dot = '\0';
				if (g_strcmp0 (base, ref) == 0 ||
				    g_strcmp0 (t->id, ref) == 0)
				{
					found = TRUE;
					break;
				}
			}

			if (!found)
				g_string_append_printf (out, "Missing: %s\n", ref);
		}
		g_strfreev (lines);
	}

	g_free (rg_stdout);
	g_free (rg_stderr);

	{
		gchar *result = g_string_free (out, FALSE);
		McpToolResult *r = mcp_result_text (result[0] ? result : "No missing references found");
		g_free (result);
		return r;
	}
}

/*
 * mcp_handle_generate_report:
 *
 * MCP handler for "generate_report". Generates a basic statistics report.
 */
static McpToolResult *
mcp_handle_generate_report (McpServer   *server,
                              const gchar *name,
                              JsonObject  *args,
                              gpointer     user_data)
{
	VimbanMcpContext *ctx  = (VimbanMcpContext *) user_data;
	const gchar *type      = mcp_arg_str (args, "type");
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new ();
	g_autoptr(JsonGenerator) gen = json_generator_new ();
	g_autofree gchar *out = NULL;
	guint i;

	/* status counts */
	gint cnt_backlog = 0, cnt_ready = 0, cnt_in_progress = 0;
	gint cnt_blocked = 0, cnt_review = 0, cnt_done = 0;
	gint cnt_cancelled = 0, cnt_delegated = 0;

	(void) server; (void) name; (void) type;

	all = vimban_scan_tickets (ctx->config->directory, FALSE, NULL);

	for (i = 0; i < all->len; i++)
	{
		VimbanTicket *t = g_ptr_array_index (all, i);
		const gchar *s = t->status ? t->status : "";
		if      (g_strcmp0 (s, "backlog")     == 0) cnt_backlog++;
		else if (g_strcmp0 (s, "ready")       == 0) cnt_ready++;
		else if (g_strcmp0 (s, "in_progress") == 0) cnt_in_progress++;
		else if (g_strcmp0 (s, "blocked")     == 0) cnt_blocked++;
		else if (g_strcmp0 (s, "review")      == 0) cnt_review++;
		else if (g_strcmp0 (s, "done")        == 0) cnt_done++;
		else if (g_strcmp0 (s, "cancelled")   == 0) cnt_cancelled++;
		else if (g_strcmp0 (s, "delegated")   == 0) cnt_delegated++;
	}

	json_builder_begin_object (b);
	json_builder_set_member_name (b, "total");
	json_builder_add_int_value (b, (gint64) all->len);
	json_builder_set_member_name (b, "by_status");
	json_builder_begin_object (b);
#define _ADD_STATUS(k, v) \
	json_builder_set_member_name (b, k); json_builder_add_int_value (b, (gint64)(v));
	_ADD_STATUS ("backlog",     cnt_backlog)
	_ADD_STATUS ("ready",       cnt_ready)
	_ADD_STATUS ("in_progress", cnt_in_progress)
	_ADD_STATUS ("blocked",     cnt_blocked)
	_ADD_STATUS ("review",      cnt_review)
	_ADD_STATUS ("done",        cnt_done)
	_ADD_STATUS ("cancelled",   cnt_cancelled)
	_ADD_STATUS ("delegated",   cnt_delegated)
#undef _ADD_STATUS
	json_builder_end_object (b);
	json_builder_end_object (b);

	{
		JsonNode *root = json_builder_get_root (b);
		json_generator_set_pretty (gen, TRUE);
		json_generator_set_indent (gen, 2);
		json_generator_set_root (gen, root);
		json_node_unref (root);
	}
	out = json_generator_to_data (gen, NULL);
	return mcp_result_text (out);
}

/*
 * mcp_handle_create_mentor_meeting:
 *
 * MCP handler for "create_mentor_meeting". Creates a mentorship meeting ticket.
 */
static McpToolResult *
mcp_handle_create_mentor_meeting (McpServer   *server,
                                    const gchar *name,
                                    JsonObject  *args,
                                    gpointer     user_data)
{
	VimbanMcpContext *ctx    = (VimbanMcpContext *) user_data;
	const gchar *person_name = mcp_arg_str (args, "person");

	GPtrArray *av = g_ptr_array_new ();

	(void) server; (void) name;

	if (!person_name || !person_name[0])
		return mcp_result_error ("person is required");

	g_ptr_array_add (av, (gpointer) "mentor");
	g_ptr_array_add (av, (gpointer) "new");
	g_ptr_array_add (av, (gpointer) person_name);
	g_ptr_array_add (av, NULL);

	/* cmd_mentor will call the editor; we override EDITOR to suppress it */
	{
		const gchar *orig_editor = g_getenv ("EDITOR");
		g_setenv ("EDITOR", "true", TRUE); /* "true" is a no-op command */
		gint ret = cmd_mentor ((gint) av->len - 1, (gchar **) av->pdata,
		                        ctx->global, ctx->config);
		if (orig_editor)
			g_setenv ("EDITOR", orig_editor, TRUE);
		else
			g_unsetenv ("EDITOR");

		g_ptr_array_free (av, FALSE);

		if (ret != VIMBAN_EXIT_SUCCESS)
		{
			g_autofree gchar *msg = g_strdup_printf (
				"Failed to create mentor meeting for %s", person_name);
			return mcp_result_error (msg);
		}
	}

	{
		g_autofree gchar *out = g_strdup_printf (
			"Created mentor meeting for %s", person_name);
		return mcp_result_text (out);
	}
}

/*
 * mcp_handle_list_mentor_meetings:
 *
 * MCP handler for "list_mentor_meetings". Lists mentorship meeting tickets.
 */
static McpToolResult *
mcp_handle_list_mentor_meetings (McpServer   *server,
                                   const gchar *name,
                                   JsonObject  *args,
                                   gpointer     user_data)
{
	VimbanMcpContext *ctx = (VimbanMcpContext *) user_data;
	g_autofree gchar *mntr_dir = g_build_filename (
		ctx->config->directory, "02_areas", "work", "mentorship", NULL);
	g_autofree gchar *json_str = NULL;

	(void) server; (void) name; (void) args;

	if (!g_file_test (mntr_dir, G_FILE_TEST_IS_DIR))
		return mcp_result_text ("[]");

	{
		const gchar *exclude[] = { NULL };
		g_autoptr(GPtrArray) all = vimban_scan_tickets (mntr_dir, FALSE, exclude);
		g_autoptr(GPtrArray) meetings = g_ptr_array_new ();
		guint i;

		for (i = 0; i < all->len; i++)
		{
			VimbanTicket *t = g_ptr_array_index (all, i);
			if (g_strcmp0 (t->type, "mentorship") == 0)
				g_ptr_array_add (meetings, t);
		}

		json_str = mcp_tickets_to_json_str (meetings);
	}

	return mcp_result_text (json_str);
}

/* ── tool registration helper ───────────────────────────────────────────── */

/*
 * vimban_mcp_add_tool:
 * @server: McpServer to register with
 * @name: tool name
 * @desc: tool description
 * @schema: (transfer full): input schema JsonNode
 * @handler: handler function
 * @ctx: shared context (user_data)
 * @read_only: hint that tool does not modify state
 *
 * Registers a single tool on @server with the given schema and handler.
 */
static void
vimban_mcp_add_tool (McpServer      *server,
                      const gchar    *name,
                      const gchar    *desc,
                      JsonNode       *schema,
                      McpToolHandler  handler,
                      VimbanMcpContext *ctx,
                      gboolean        read_only)
{
	McpTool *tool = mcp_tool_new (name, desc);
	mcp_tool_set_input_schema (tool, schema);
	json_node_unref (schema);
	mcp_tool_set_read_only_hint (tool, read_only);
	mcp_server_add_tool (server, tool, handler, ctx, NULL);
	g_object_unref (tool);
}

/* ── server setup ───────────────────────────────────────────────────────── */

/*
 * vimban_mcp_register_tools:
 * @server: the McpServer
 * @ctx: shared context passed to every handler
 *
 * Registers all vimban MCP tools on @server.
 */
static void
vimban_mcp_register_tools (McpServer        *server,
                             VimbanMcpContext *ctx)
{
	JsonBuilder *b;
	JsonNode *schema;

	/* ── list_tickets ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "status",
		"Filter by status (comma-separated): backlog, ready, in_progress, "
		"blocked, review, delegated, done, cancelled");
	mcp_schema_string_prop (b, "type",
		"Filter by ticket type (comma-separated): epic, story, task, "
		"sub-task, research, bug");
	mcp_schema_string_prop (b, "assignee",
		"Filter by assignee name (substring match)");
	mcp_schema_string_prop (b, "project",
		"Filter by project name");
	mcp_schema_string_prop (b, "priority",
		"Filter by priority: critical, high, medium, low");
	mcp_schema_string_prop (b, "tag",
		"Filter by tag");
	mcp_schema_bool_prop (b, "mine",
		"Show only tickets assigned to the current $USER");
	mcp_schema_bool_prop (b, "overdue",
		"Show only overdue tickets (past due_date, not done)");
	mcp_schema_bool_prop (b, "unassigned",
		"Show only unassigned tickets");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "list_tickets",
		"List and filter tickets from the vimban notes directory",
		schema, mcp_handle_list_tickets, ctx, TRUE);

	/* ── show_ticket ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "ticket",
		"Ticket ID or path (e.g. PROJ-42) — required");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "show_ticket",
		"Show details of a single ticket by ID",
		schema, mcp_handle_show_ticket, ctx, TRUE);

	/* ── create_ticket ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "type",
		"Ticket type (required): epic, story, task, sub-task, research, bug");
	mcp_schema_string_prop (b, "title",
		"Ticket title (required)");
	mcp_schema_string_prop (b, "priority",
		"Priority: critical, high, medium, low (default: medium)");
	mcp_schema_string_prop (b, "assignee",
		"Assignee name or person reference");
	mcp_schema_string_prop (b, "project",
		"Project name");
	mcp_schema_string_prop (b, "tags",
		"Comma-separated tags");
	mcp_schema_string_prop (b, "due_date",
		"Due date in ISO format (YYYY-MM-DD) or relative (e.g. +7d)");
	mcp_schema_string_prop (b, "scope",
		"Scope: work (default) or personal");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "create_ticket",
		"Create a new ticket in the vimban notes directory",
		schema, mcp_handle_create_ticket, ctx, FALSE);

	/* ── edit_ticket ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "ticket",
		"Ticket ID or path (required)");
	mcp_schema_string_prop (b, "status",
		"New status value");
	mcp_schema_string_prop (b, "priority",
		"New priority value");
	mcp_schema_string_prop (b, "assignee",
		"New assignee name or reference");
	mcp_schema_string_prop (b, "due_date",
		"New due date (ISO or relative)");
	mcp_schema_int_prop (b, "progress",
		"Progress percentage (0-100)");
	mcp_schema_string_prop (b, "add_tag",
		"Tag to add");
	mcp_schema_string_prop (b, "remove_tag",
		"Tag to remove");
	mcp_schema_string_prop (b, "clear_field",
		"Clear a field (e.g., 'assignee', 'due_date')");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "edit_ticket",
		"Edit fields on an existing ticket",
		schema, mcp_handle_edit_ticket, ctx, FALSE);

	/* ── move_ticket ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "ticket",
		"Ticket ID or path (required)");
	mcp_schema_string_prop (b, "status",
		"Target status (required): backlog, ready, in_progress, blocked, "
		"review, delegated, done, cancelled");
	mcp_schema_bool_prop (b, "force",
		"Skip transition validation");
	mcp_schema_bool_prop (b, "reopen",
		"Clear end_date when reopening from done/cancelled");
	mcp_schema_string_prop (b, "comment",
		"Optional comment to add on move");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "move_ticket",
		"Move a ticket to a new status",
		schema, mcp_handle_move_ticket, ctx, FALSE);

	/* ── search_tickets ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "query",
		"Search query string (required)");
	mcp_schema_bool_prop (b, "regex",
		"Treat query as a regex pattern");
	mcp_schema_bool_prop (b, "case_sensitive",
		"Case-sensitive search (default: false)");
	mcp_schema_bool_prop (b, "files_only",
		"Return only matching file paths, not line content");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "search_tickets",
		"Full-text search across all ticket markdown files using ripgrep",
		schema, mcp_handle_search_tickets, ctx, TRUE);

	/* ── get_dashboard ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "type",
		"Dashboard type: daily (default), weekly, sprint, project, team, person");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "get_dashboard",
		"Get a dashboard view of current work",
		schema, mcp_handle_get_dashboard, ctx, TRUE);

	/* ── get_kanban ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "project",
		"Filter by project name");
	mcp_schema_string_prop (b, "assignee",
		"Filter by assignee name");
	mcp_schema_bool_prop (b, "mine",
		"Show only tickets assigned to current $USER");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "get_kanban",
		"Get the kanban board view showing tickets by status column",
		schema, mcp_handle_get_kanban, ctx, TRUE);

	/* ── list_people ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "list_people",
		"List all people in the people directory",
		schema, mcp_handle_list_people, ctx, TRUE);

	/* ── show_person ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "name",
		"Person name or partial name to look up (required)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "show_person",
		"Show details for a person by name",
		schema, mcp_handle_show_person, ctx, TRUE);

	/* ── search_people ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "query",
		"Name substring to search for (required)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "search_people",
		"Search people by name substring",
		schema, mcp_handle_search_people, ctx, TRUE);

	/* ── create_person ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "name",
		"Full name of the person (required)");
	mcp_schema_string_prop (b, "email",
		"Email address");
	mcp_schema_string_prop (b, "role",
		"Job role or title");
	mcp_schema_string_prop (b, "team",
		"Team name");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "create_person",
		"Create a new person profile in the people directory",
		schema, mcp_handle_create_person, ctx, FALSE);

	/* ── get_person_dashboard ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "name",
		"Person name to get dashboard for (required)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "get_person_dashboard",
		"Get all tickets assigned to a specific person",
		schema, mcp_handle_get_person_dashboard, ctx, TRUE);

	/* ── validate_tickets ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "validate_tickets",
		"Validate all tickets for required fields and valid values, "
		"returning errors and warnings",
		schema, mcp_handle_validate_tickets, ctx, TRUE);

	/* ── add_comment ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "target",
		"Ticket ID or path to comment on (required)");
	mcp_schema_string_prop (b, "text",
		"Comment text (required)");
	mcp_schema_string_prop (b, "author",
		"Author name (defaults to $USER)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "add_comment",
		"Add a comment to a ticket",
		schema, mcp_handle_add_comment, ctx, FALSE);

	/* ── get_comments ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "target",
		"Ticket ID or path to retrieve comments for (required)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "get_comments",
		"Get all comments on a ticket",
		schema, mcp_handle_get_comments, ctx, TRUE);

	/* ── link_tickets ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "ticket",
		"Source ticket ID or path (required)");
	mcp_schema_string_prop (b, "target",
		"Target ticket ID or path (required)");
	mcp_schema_string_prop (b, "relation",
		"Relation type: relates-to (default), blocked-by, blocks, member-of");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "link_tickets",
		"Create a relationship link between two tickets",
		schema, mcp_handle_link_tickets, ctx, FALSE);

	/* ── generate_link ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "ref",
		"Ticket ID, person name, or any reference to generate a link for (required)");
	mcp_schema_string_prop (b, "format",
		"Link format: wikilink (default) or markdown");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "generate_link",
		"Generate a wikilink or markdown link for a ticket or person",
		schema, mcp_handle_generate_link, ctx, TRUE);

	/* ── convert_find_missing ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "convert_find_missing",
		"Find wikilink references in notes that do not resolve to existing files",
		schema, mcp_handle_convert_find_missing, ctx, TRUE);

	/* ── generate_report ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "type",
		"Report type: workload (default), burndown, velocity, aging, blockers");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "generate_report",
		"Generate a statistical report on tickets",
		schema, mcp_handle_generate_report, ctx, TRUE);

	/* ── create_mentor_meeting ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	mcp_schema_string_prop (b, "person",
		"Person name or reference for the mentee (required)");
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "create_mentor_meeting",
		"Create a new mentorship meeting note for a person",
		schema, mcp_handle_create_mentor_meeting, ctx, FALSE);

	/* ── list_mentor_meetings ── */
	b = json_builder_new ();
	mcp_schema_begin (b);
	schema = mcp_schema_end (b);
	g_object_unref (b);
	vimban_mcp_add_tool (server, "list_mentor_meetings",
		"List all mentorship meeting notes",
		schema, mcp_handle_list_mentor_meetings, ctx, TRUE);
}

/*
 * vimban_run_mcp_stdio:
 * @config: project configuration
 * @global: global options
 *
 * Start the MCP server on stdio (newline-delimited JSON-RPC).
 *
 * Returns: exit code
 */
static gint
vimban_run_mcp_stdio (VimbanConfig     *config,
                       VimbanGlobalOpts *global)
{
	g_autoptr(McpServer) server = NULL;
	g_autoptr(McpStdioTransport) transport = NULL;
	g_autoptr(GError) error = NULL;
	VimbanMcpContext *ctx;

	ctx          = g_new0 (VimbanMcpContext, 1);
	ctx->config  = config;
	ctx->global  = global;

	server = mcp_server_new ("vimban", VERSION);
	mcp_server_set_instructions (server,
		"vimban is a Markdown-native ticket and kanban management system. "
		"Tickets live as .md files with YAML frontmatter in a notes directory. "
		"Use list_tickets to browse, show_ticket for details, create_ticket to "
		"file new work, move_ticket to advance status, and get_kanban for a "
		"board view. People profiles are managed via list_people / show_person. "
		"Mentorship notes use create_mentor_meeting and list_mentor_meetings.");

	vimban_mcp_register_tools (server, ctx);

	transport = mcp_stdio_transport_new ();
	mcp_server_set_transport (server, MCP_TRANSPORT (transport));

	if (!mcp_server_run (server, &error))
	{
		g_printerr ("MCP stdio server error: %s\n",
		            error ? error->message : "unknown");
		vimban_mcp_context_free (ctx);
		return VIMBAN_EXIT_GENERAL_ERROR;
	}

	vimban_mcp_context_free (ctx);
	return VIMBAN_EXIT_SUCCESS;
}

/*
 * vimban_run_mcp_http:
 * @config: project configuration
 * @global: global options
 *
 * Start the MCP server over HTTP/SSE on port 3001 (or $VIMBAN_MCP_PORT).
 *
 * Returns: exit code
 */
static gint
vimban_run_mcp_http (VimbanConfig     *config,
                      VimbanGlobalOpts *global)
{
	g_autoptr(McpServer) server = NULL;
	McpHttpServerTransport *transport = NULL;
	g_autoptr(GError) error = NULL;
	VimbanMcpContext *ctx;
	guint port = 3001;
	const gchar *port_env;

	port_env = g_getenv ("VIMBAN_MCP_PORT");
	if (port_env && port_env[0])
		port = (guint) g_ascii_strtoull (port_env, NULL, 10);

	ctx         = g_new0 (VimbanMcpContext, 1);
	ctx->config = config;
	ctx->global = global;

	server = mcp_server_new ("vimban", VERSION);
	mcp_server_set_instructions (server,
		"vimban is a Markdown-native ticket and kanban management system. "
		"Tickets live as .md files with YAML frontmatter in a notes directory. "
		"Use list_tickets to browse, show_ticket for details, create_ticket to "
		"file new work, move_ticket to advance status, and get_kanban for a "
		"board view. People profiles are managed via list_people / show_person. "
		"Mentorship notes use create_mentor_meeting and list_mentor_meetings.");

	vimban_mcp_register_tools (server, ctx);

	transport = mcp_http_server_transport_new_full (NULL, port);
	mcp_server_set_transport (server, MCP_TRANSPORT (transport));

	g_printerr ("vimban MCP HTTP server starting on port %u\n", port);

	if (!mcp_server_run (server, &error))
	{
		g_printerr ("MCP HTTP server error: %s\n",
		            error ? error->message : "unknown");
		g_object_unref (transport);
		vimban_mcp_context_free (ctx);
		return VIMBAN_EXIT_GENERAL_ERROR;
	}

	g_object_unref (transport);
	vimban_mcp_context_free (ctx);
	return VIMBAN_EXIT_SUCCESS;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Subcommand dispatch table
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const gchar    *name;
    const gchar    *help;
    VimbanCmdFunc   handler;
} VimbanSubcommand;

static const VimbanSubcommand SUBCOMMANDS[] = {
    { "init",           "Initialize vimban",                cmd_init },
    { "create",         "Create a ticket",                  cmd_create },
    { "list",           "List tickets",                     cmd_list },
    { "show",           "Show ticket details",              cmd_show },
    { "generate-link",  "Generate link for ticket/person",  cmd_generate_link },
    { "get-link",       "Alias for generate-link",          cmd_generate_link },
    { "get-id",         "Get ID from reference",            cmd_get_id },
    { "edit",           "Edit ticket",                      cmd_edit },
    { "move",           "Move ticket status",               cmd_move },
    { "move-location",  "Move ticket file",                 cmd_move_location },
    { "archive",        "Archive ticket",                   cmd_archive },
    { "link",           "Link tickets",                     cmd_link },
    { "comment",        "Add/view comments",                cmd_comment },
    { "comments",       "List recent comments",             cmd_comments },
    { "dashboard",      "Generate dashboard",               cmd_dashboard },
    { "kanban",         "Display kanban board",              cmd_kanban },
    { "search",         "Search tickets",                   cmd_search },
    { "validate",       "Validate tickets",                 cmd_validate },
    { "report",         "Generate reports",                 cmd_report },
    { "sync",           "Sync with external systems",       cmd_sync },
    { "commit",         "Git commit changes",               cmd_commit },
    { "watch",          "Watch live events",                cmd_watch_cmd },
    { "people",         "People management",                cmd_people },
    { "mentor",         "Mentorship management",            cmd_mentor },
    { "convert",        "Convert/ingest files",             cmd_convert },
    { "completion",     "Generate shell completion",        cmd_completion },
    { "tui",            "Launch interactive TUI",           cmd_tui },
    { NULL, NULL, NULL }
};


/* ═══════════════════════════════════════════════════════════════════════════
 * CLI argument parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_print_usage:
 *
 * Print usage information listing all commands.
 */
static void
vimban_print_usage (void)
{
    gint i;

    g_print ("vimban %s - Markdown-native ticket/kanban management system\n\n",
             VERSION);
    g_print ("Usage: vimban [global-options] <command> [command-options]\n\n");
    g_print ("Global options:\n");
    g_print ("  -d, --directory DIR    Working directory (default: ~/Documents/notes)\n");
    g_print ("  -f, --format FORMAT    Output format: plain, md, yaml, json\n");
    g_print ("  -q, --quiet            Suppress non-essential output\n");
    g_print ("  -v, --verbose          Verbose output\n");
    g_print ("  --no-color             Disable colors\n");
    g_print ("  --work                 Operate on work tickets\n");
    g_print ("  --personal             Operate on personal tickets\n");
    g_print ("  --archived             Include archived items\n");
    g_print ("  --remote URL           Connect to remote vimban_serve\n");
    g_print ("  --api-token TOKEN      API token for remote auth\n");
    g_print ("  --no-token             Skip auth token\n");
    g_print ("  --mcp                  Run as MCP server (stdio)\n");
    g_print ("  --mcp-http             Run as MCP server (HTTP)\n");
    g_print ("  --serve                Start web UI server\n");
    g_print ("  --watch                Watch live events (requires --remote)\n");
    g_print ("  --version              Print version\n");
    g_print ("  --license              Print license (AGPLv3)\n");
    g_print ("\nCommands:\n");

    for (i = 0; SUBCOMMANDS[i].name != NULL; i++)
    {
        g_print ("  %-20s %s\n", SUBCOMMANDS[i].name, SUBCOMMANDS[i].help);
    }

    g_print ("\nExamples:\n");
    g_print ("  vimban init\n");
    g_print ("  vimban create task \"Fix authentication bug\" -a john -p high\n");
    g_print ("  vimban list --status in_progress,review --mine\n");
    g_print ("  vimban move PROJ-42 done --resolve\n");
    g_print ("  vimban dashboard daily -f md\n");

    g_print ("\nEnvironment Variables:\n");
    g_print ("  VIMBAN_DIR             Default directory\n");
    g_print ("  VIMBAN_FORMAT          Default output format\n");
    g_print ("  VIMBAN_ID_PREFIX       ID prefix (default: PROJ)\n");
    g_print ("  VIMBAN_PEOPLE_DIR      People subdir\n");
}

/*
 * vimban_find_command_index:
 * @argc: argument count
 * @argv: argument values
 *
 * Find the index of the first non-option argument (the command name).
 * Skips global options and their values.
 *
 * Returns: index of command in argv, or -1 if not found
 */
static gint
vimban_find_command_index (gint argc, gchar **argv)
{
    gint i;

    for (i = 1; i < argc; i++)
    {
        /* skip option flags and their values */
        if (argv[i][0] == '-')
        {
            /* options that take a value: skip next arg too
             * (but not if value is embedded via --option=value) */
            if (g_strcmp0 (argv[i], "-d") == 0 ||
                g_strcmp0 (argv[i], "--directory") == 0 ||
                g_strcmp0 (argv[i], "-f") == 0 ||
                g_strcmp0 (argv[i], "--format") == 0 ||
                g_strcmp0 (argv[i], "--remote") == 0 ||
                g_strcmp0 (argv[i], "--api-token") == 0)
            {
                i++; /* skip the value */
            }
            else if (g_str_has_prefix (argv[i], "--directory=") ||
                     g_str_has_prefix (argv[i], "--format=") ||
                     g_str_has_prefix (argv[i], "--remote=") ||
                     g_str_has_prefix (argv[i], "--api-token="))
            {
                /* value embedded in same arg — do not skip next */
            }
            continue;
        }
        return i;
    }

    return -1;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

gint
main (gint   argc,
      gchar *argv[])
{
    g_autoptr(VimbanGlobalOpts) global = NULL;
    g_autoptr(VimbanConfig)     config = NULL;
    g_autoptr(GOptionContext)   context = NULL;
    g_autoptr(GError)           error = NULL;
    const gchar                *cmd_name = NULL;
    gchar                      *dir = NULL;
    gint                        cmd_idx;
    gint                        cmd_argc;
    gchar                     **cmd_argv;
    gint                        i;
    gboolean                    show_version = FALSE;

    global = g_new0 (VimbanGlobalOpts, 1);

    GOptionEntry entries[] = {
        { "directory",  'd', 0, G_OPTION_ARG_STRING,  &global->directory,
          "Working directory", "DIR" },
        { "format",     'f', 0, G_OPTION_ARG_STRING,  &global->format_str,
          "Output format", "FORMAT" },
        { "quiet",      'q', 0, G_OPTION_ARG_NONE,    &global->quiet,
          "Suppress output", NULL },
        { "verbose",    'v', 0, G_OPTION_ARG_NONE,    &global->verbose,
          "Verbose output", NULL },
        { "no-color",   0,   0, G_OPTION_ARG_NONE,    &global->no_color,
          "Disable colors", NULL },
        { "work",       0,   0, G_OPTION_ARG_NONE,    &global->work,
          "Work scope", NULL },
        { "personal",   0,   0, G_OPTION_ARG_NONE,    &global->personal,
          "Personal scope", NULL },
        { "archived",   0,   0, G_OPTION_ARG_NONE,    &global->archived,
          "Include archives", NULL },
        { "remote",     0,   0, G_OPTION_ARG_STRING,  &global->remote,
          "Remote URL", "URL" },
        { "api-token",  0,   0, G_OPTION_ARG_STRING,  &global->api_token,
          "API token", "TOKEN" },
        { "no-token",   0,   0, G_OPTION_ARG_NONE,    &global->no_token,
          "Skip auth", NULL },
        { "mcp",        0,   0, G_OPTION_ARG_NONE,    &global->mcp_stdio,
          "MCP server (stdio)", NULL },
        { "mcp-http",   0,   0, G_OPTION_ARG_NONE,    &global->mcp_http,
          "MCP server (HTTP)", NULL },
        { "serve",      0,   0, G_OPTION_ARG_NONE,    &global->serve,
          "Start web server", NULL },
        { "watch",      0,   0, G_OPTION_ARG_NONE,    &global->watch,
          "Watch live events", NULL },
        { "version",    0,   0, G_OPTION_ARG_NONE,    &show_version,
          "Print version", NULL },
        { "license",    0,   0, G_OPTION_ARG_NONE,    &global->show_license,
          "Print license", NULL },
        { NULL }
    };

    /*
     * Two-pass parsing: first find the command index so we only
     * parse global options on the args before the command.
     */
    cmd_idx = vimban_find_command_index (argc, argv);

    /*
     * Build a temporary argc/argv with only the global args
     * (everything before the command) for GOptionContext to parse.
     */
    gint global_argc;
    gchar **global_argv;

    if (cmd_idx > 0)
    {
        global_argc = cmd_idx;
        global_argv = g_new0 (gchar *, global_argc + 1);
        for (i = 0; i < global_argc; i++)
            global_argv[i] = g_strdup (argv[i]);
        global_argv[global_argc] = NULL;
    }
    else
    {
        global_argc = argc;
        global_argv = g_strdupv (argv);
    }

    context = g_option_context_new ("<command> [options]");
    g_option_context_set_summary (context,
        "vimban - Markdown-native ticket/kanban management system");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_set_help_enabled (context, FALSE);
    g_option_context_set_ignore_unknown_options (context, TRUE);

    if (!g_option_context_parse (context, &global_argc, &global_argv, &error))
    {
        g_printerr ("Error: %s\n", error->message);
        g_strfreev (global_argv);
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    g_strfreev (global_argv);

    /* apply global state */
    g_no_color = global->no_color;
    g_quiet    = global->quiet;
    g_verbose  = global->verbose;

    global->format = vimban_format_from_string (global->format_str);

    /* handle env var overrides */
    if (!global->format_str)
    {
        const gchar *env_fmt = g_getenv ("VIMBAN_FORMAT");
        if (env_fmt)
            global->format = vimban_format_from_string (env_fmt);
    }

    /* handle --version */
    if (show_version)
    {
        g_print ("vimban %s\n", VERSION);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* handle --license */
    if (global->show_license)
    {
        g_print ("%s\n", LICENSE_TEXT);
        return VIMBAN_EXIT_SUCCESS;
    }

    /* handle mutual exclusivity: --work and --personal */
    if (global->work && global->personal)
    {
        g_printerr ("Error: --work and --personal are mutually exclusive\n");
        return VIMBAN_EXIT_INVALID_ARGS;
    }

    /* handle mutual exclusivity: --mcp, --mcp-http, --serve */
    {
        gint mode_count = 0;
        if (global->mcp_stdio) mode_count++;
        if (global->mcp_http)  mode_count++;
        if (global->serve)     mode_count++;
        if (mode_count > 1)
        {
            g_printerr ("Error: --mcp, --mcp-http, and --serve are mutually exclusive\n");
            return VIMBAN_EXIT_INVALID_ARGS;
        }
    }

    /* resolve directory */
    dir = global->directory
        ? g_strdup (global->directory)
        : g_strdup (g_getenv ("VIMBAN_DIR"));

    if (!dir || dir[0] == '\0')
    {
        g_free (dir);
        dir = vimban_get_default_dir ();
    }

    /* expand ~ */
    if (dir[0] == '~')
    {
        g_autofree gchar *old = dir;
        dir = g_build_filename (g_get_home_dir (), dir + 1, NULL);
        (void) old;
    }

    /* handle --mcp and --mcp-http modes before loading config so that
     * the config load happens inside the mode handler with the resolved dir */
    if (global->mcp_stdio || global->mcp_http)
    {
        config = vimban_config_load (dir);
        g_free (dir);
        dir = NULL;

        if (global->mcp_stdio)
            return vimban_run_mcp_stdio (config, global);
        else
            return vimban_run_mcp_http (config, global);
    }

    /* load config */
    config = vimban_config_load (dir);
    g_free (dir);

    /* find and dispatch command */
    if (cmd_idx < 0)
    {
        vimban_print_usage ();
        return VIMBAN_EXIT_SUCCESS;
    }

    cmd_name = argv[cmd_idx];
    cmd_argc = argc - cmd_idx;
    cmd_argv = argv + cmd_idx;

    /* look up command in dispatch table */
    for (i = 0; SUBCOMMANDS[i].name != NULL; i++)
    {
        if (g_strcmp0 (SUBCOMMANDS[i].name, cmd_name) == 0)
        {
            return SUBCOMMANDS[i].handler (cmd_argc, cmd_argv,
                                            global, config);
        }
    }

    g_printerr ("Error: unknown command: %s\n", cmd_name);
    g_printerr ("Run 'vimban --help' for usage.\n");
    return VIMBAN_EXIT_INVALID_ARGS;
}
