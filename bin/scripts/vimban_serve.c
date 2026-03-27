#!/usr/bin/env crispy
#define CRISPY_PARAMS "$(pkg-config --cflags --libs yaml-glib-1.0 json-glib-1.0 libsoup-3.0) -Wno-unused-function"

/*
 * vimban_serve.c — Web UI server for vimban ticket management
 * Copyright (C) 2025  Zach Podbielniak — AGPLv3
 *
 * C port of vimban_serve (Python/Quart). Uses crispy for script-style
 * execution. libsoup-3.0 for the HTTP server, json-glib for JSON building
 * and parsing, glib for data structures and subprocess management.
 *
 * Token auth: reads ~/.config/vimban/token  (username:token  or bare token)
 * All /api/* routes (except /api/health) require a Bearer token when auth
 * is enabled. SSE clients may pass ?token= as a query parameter since
 * EventSource cannot set custom headers.
 *
 * Usage:
 *     vimban_serve.c [options]
 *     crispy bin/scripts/vimban_serve.c --port 5006
 *
 * Examples:
 *     crispy bin/scripts/vimban_serve.c
 *     crispy bin/scripts/vimban_serve.c --port 8080
 *     crispy bin/scripts/vimban_serve.c --bind 0.0.0.0 --no-token
 */

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>


/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define VIMBAN_SERVE_VERSION   "0.3.0"
#define DEFAULT_PORT           (5005)
#define DEFAULT_BIND           "127.0.0.1"
#define TOKEN_FILE_RELPATH     ".config/vimban/token"
#define SSE_KEEPALIVE_INTERVAL (25)   /* seconds between SSE ping comments */
#define CMD_TIMEOUT_SEC        (30)
#define CMD_TIMEOUT_LONG_SEC   (60)   /* for commit/sync */
#define SSE_MAX_CLIENTS        (64)   /* HIGH-4: cap on concurrent SSE connections */
#define MAX_REQUEST_BODY_SIZE  (1 * 1024 * 1024)  /* LOW-3: 1 MB body limit */

/* Canonical ticket statuses */
static const gchar * const STATUSES[] = {
	"backlog", "ready", "in_progress", "blocked",
	"review", "delegated", "done", "cancelled", NULL
};

/* Valid ticket types for creation */
static const gchar * const TICKET_TYPES[] = {
	"epic", "story", "task", "sub-task", "research", "bug", NULL
};

/* Valid priority levels */
static const gchar * const PRIORITIES[] = {
	"critical", "high", "medium", "low", "none", NULL
};

/* Valid dashboard types (MED-3) */
static const gchar * const DASHBOARD_TYPES[] = {
	"daily", "weekly", "monthly", "sprint", "project",
	"team", "person", "summary", NULL
};

/* Valid report types (MED-4) */
static const gchar * const REPORT_TYPES[] = {
	"summary", "velocity", "burndown", "aging", "workload",
	"overdue", "sprint", "daily", "weekly", NULL
};


/* ═══════════════════════════════════════════════════════════════════════════
 * Structs
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * SseClient — represents a single connected SSE subscriber.
 *
 * @id:         UUID string uniquely identifying this connection.
 * @user:       Resolved username (from token lookup).
 * @output:     The SoupServerMessage output stream for writing events.
 * @connected_at: Unix timestamp of connection establishment.
 * @viewing:    Optional ticket ID the client is currently viewing (or NULL).
 */
typedef struct _SseClient {
	gchar              *id;
	gchar              *user;
	SoupServerMessage  *msg;
	gint64              connected_at;
	gchar              *viewing;
} SseClient;

/*
 * VimbanServeApp — top-level application state passed to all handlers.
 *
 * @server:       The SoupServer instance.
 * @loop:         The GMainLoop driving the event loop.
 * @auth_enabled: TRUE when token auth is active.
 * @tokens:       Hash table mapping token -> username (owned).
 * @token_file:   Absolute path to the token file.
 * @sse_clients:  GPtrArray of SseClient* currently connected.
 * @no_token:     TRUE if --no-token flag was passed.
 */
typedef struct _VimbanServeApp {
	SoupServer  *server;
	GMainLoop   *loop;
	gboolean     auth_enabled;
	GHashTable  *tokens;        /* gchar* -> gchar* (both owned by table) */
	gchar       *token_file;
	GPtrArray   *sse_clients;   /* element-type: SseClient* */
	gboolean     no_token;
	gchar       *directory;     /* vimban working directory (-d flag) */
	gboolean     work;          /* --work scope flag */
	gboolean     personal;      /* --personal scope flag */
	gboolean     archived;      /* --archived flag */
} VimbanServeApp;


/* ─── SseClient lifecycle ─────────────────────────────────────────────── */

static SseClient *
sse_client_new(
	const gchar       *user,
	SoupServerMessage *msg
){
	SseClient *c;

	c               = g_new0(SseClient, 1);
	c->id           = g_uuid_string_random();
	c->user         = g_strdup(user ? user : "unknown");
	c->msg          = g_object_ref(msg);
	c->connected_at = g_get_real_time() / G_USEC_PER_SEC;
	c->viewing      = NULL;

	return c;
}

static void
sse_client_free(
	SseClient *c
){
	if (!c) return;
	g_free(c->id);
	g_free(c->user);
	g_free(c->viewing);
	if (c->msg) g_object_unref(c->msg);
	g_free(c);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SseClient, sse_client_free)


/* ─── VimbanServeApp lifecycle ────────────────────────────────────────── */

static VimbanServeApp *
vimban_serve_app_new(void)
{
	VimbanServeApp *app;

	app               = g_new0(VimbanServeApp, 1);
	app->auth_enabled = FALSE;
	app->tokens       = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	app->sse_clients  = g_ptr_array_new_with_free_func((GDestroyNotify)sse_client_free);

	return app;
}

static void
vimban_serve_app_free(
	VimbanServeApp *app
){
	if (!app) return;
	if (app->server)      g_object_unref(app->server);
	if (app->loop)        g_main_loop_unref(app->loop);
	if (app->tokens)      g_hash_table_destroy(app->tokens);
	if (app->sse_clients) g_ptr_array_free(app->sse_clients, TRUE);
	g_free(app->token_file);
	g_free(app->directory);
	g_free(app);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VimbanServeApp, vimban_serve_app_free)


/* ═══════════════════════════════════════════════════════════════════════════
 * Token loading
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * load_tokens() — populate app->tokens from the token file.
 *
 * Token file format:
 *   username:token   -> maps token to username
 *   bare_token       -> maps token to "unknown"
 *   # comment lines and blank lines are ignored
 *
 * @app: The application state (tokens hash table is populated).
 */
static void
load_tokens(
	VimbanServeApp *app
){
	g_autoptr(GFile)          token_file = NULL;
	g_autoptr(GFileInputStream) fis      = NULL;
	g_autoptr(GDataInputStream) dis      = NULL;
	g_autoptr(GError)         err        = NULL;
	gchar                    *line       = NULL;
	gsize                     len        = 0;

	if (!app->token_file) return;

	token_file = g_file_new_for_path(app->token_file);
	fis        = g_file_read(token_file, NULL, &err);
	if (!fis) {
		/* LOW-2: Warn when token file was configured but not found */
		g_printerr("WARNING: Token file not found at %s — auth is DISABLED\n",
		           app->token_file);
		return;
	}

	dis = g_data_input_stream_new(G_INPUT_STREAM(fis));

	while ((line = g_data_input_stream_read_line(dis, &len, NULL, NULL)) != NULL) {
		gchar *trimmed = g_strstrip(line);

		/* Skip blank lines and comments */
		if (!*trimmed || g_str_has_prefix(trimmed, "#")) {
			g_free(line);
			continue;
		}

		if (strchr(trimmed, ':')) {
			/* username:token format */
			gchar **parts = g_strsplit(trimmed, ":", 2);
			if (parts[0] && parts[1]) {
				g_autofree gchar *username = g_strstrip(g_strdup(parts[0]));
				g_autofree gchar *token    = g_strstrip(g_strdup(parts[1]));
				g_hash_table_insert(app->tokens, g_steal_pointer(&token), g_steal_pointer(&username));
			}
			g_strfreev(parts);
		} else {
			/* Bare token — backward compat */
			g_hash_table_insert(app->tokens,
			                    g_strdup(trimmed),
			                    g_strdup("unknown"));
		}

		g_free(line);
	}
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Validation helpers (HIGH-2, HIGH-3)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * is_valid_ticket_id() — validate a ticket ID matches PREFIX-NNNNN pattern.
 *
 * @id: The string to validate.
 *
 * Returns: TRUE if id matches 1-16 uppercase letters, dash, 1-10 digits.
 */
static gboolean
is_valid_ticket_id(
	const gchar *id
){
	const gchar *p;
	gint letters = 0;
	gint digits = 0;
	gboolean saw_dash = FALSE;

	if (!id || !id[0]) return FALSE;
	for (p = id; *p; p++) {
		if (!saw_dash) {
			if (*p == '-') { saw_dash = TRUE; continue; }
			if (*p >= 'A' && *p <= 'Z') { letters++; continue; }
			return FALSE;
		} else {
			if (*p >= '0' && *p <= '9') { digits++; continue; }
			return FALSE;
		}
	}
	return saw_dash && letters >= 1 && letters <= 16 && digits >= 1 && digits <= 10;
}

/*
 * is_in_allowlist() — check if a value appears in a NULL-terminated string array.
 *
 * @value:   The string to look up.
 * @allowed: NULL-terminated array of allowed strings.
 *
 * Returns: TRUE if value matches one of the allowed entries.
 */
static gboolean
is_in_allowlist(
	const gchar        *value,
	const gchar * const *allowed
){
	gint i;

	if (!value) return FALSE;
	for (i = 0; allowed[i]; i++)
		if (g_strcmp0(value, allowed[i]) == 0) return TRUE;
	return FALSE;
}

/*
 * constant_time_str_equal() — compare two strings in constant time.
 *
 * Prevents timing side-channel attacks on token comparison (HIGH-3).
 *
 * @a: First string.
 * @b: Second string.
 *
 * Returns: TRUE if strings are identical.
 */
static gboolean
constant_time_str_equal(
	const gchar *a,
	const gchar *b
){
	gsize len_a, len_b, i;
	volatile guchar result = 0;

	if (!a || !b) return FALSE;
	len_a = strlen(a);
	len_b = strlen(b);
	if (len_a != len_b) return FALSE;
	for (i = 0; i < len_a; i++)
		result |= (guchar)a[i] ^ (guchar)b[i];
	return result == 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * CLI wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * vimban_cmd() — run a vimban subcommand and capture its output.
 *
 * Global flags (-f/--format, --work, --personal, --archived) are extracted
 * and placed before the subcommand so vimban's argparse sees them correctly.
 *
 * LOW-5: Builds a minimal environment for the child process instead of
 * inheriting the full parent environment.
 *
 * @args:    NULL-terminated array of argument strings (no "vimban" prefix).
 * @timeout: Command timeout in seconds.
 * @out_stdout: (transfer full) stdout text, or NULL on error.
 * @out_stderr: (transfer full) stderr text, or NULL.
 *
 * Returns: exit code (0 on success).
 */
static gint
vimban_cmd(
	VimbanServeApp    *app,
	const gchar * const *args,
	gint                 timeout,
	gchar              **out_stdout,
	gchar              **out_stderr
){
	GPtrArray      *global_flags  = NULL;
	GPtrArray      *subcmd_args   = NULL;
	GPtrArray      *cmd_argv      = NULL;
	gchar         **envp          = NULL;
	gint            rc            = 1;
	gint            i             = 0;
	gint            argc          = 0;
	guint           j             = 0;
	g_autoptr(GError) err         = NULL;

	if (out_stdout) *out_stdout = NULL;
	if (out_stderr) *out_stderr = NULL;

	global_flags = g_ptr_array_new();
	subcmd_args  = g_ptr_array_new();

	/* Count args */
	while (args[argc]) argc++;

	/* Split global flags from subcommand args */
	i = 0;
	while (i < argc) {
		if ((g_strcmp0(args[i], "-f") == 0 || g_strcmp0(args[i], "--format") == 0)
		    && i + 1 < argc)
		{
			g_ptr_array_add(global_flags, (gpointer)args[i]);
			g_ptr_array_add(global_flags, (gpointer)args[i + 1]);
			i += 2;
		} else if (g_strcmp0(args[i], "--work") == 0
		           || g_strcmp0(args[i], "--personal") == 0
		           || g_strcmp0(args[i], "--archived") == 0)
		{
			g_ptr_array_add(global_flags, (gpointer)args[i]);
			i++;
		} else {
			g_ptr_array_add(subcmd_args, (gpointer)args[i]);
			i++;
		}
	}

	/* Build final argv: vimban -d DIR --no-color -q [global_flags] [subcmd_args] */
	cmd_argv = g_ptr_array_new();
	g_ptr_array_add(cmd_argv, (gpointer)"vimban");
	if (app->directory)
	{
		g_ptr_array_add(cmd_argv, (gpointer)"-d");
		g_ptr_array_add(cmd_argv, (gpointer)app->directory);
	}
	g_ptr_array_add(cmd_argv, (gpointer)"--no-color");
	g_ptr_array_add(cmd_argv, (gpointer)"-q");
	if (app->work)
		g_ptr_array_add(cmd_argv, (gpointer)"--work");
	if (app->personal)
		g_ptr_array_add(cmd_argv, (gpointer)"--personal");
	if (app->archived)
		g_ptr_array_add(cmd_argv, (gpointer)"--archived");
	for (j = 0; j < global_flags->len; j++)
		g_ptr_array_add(cmd_argv, global_flags->pdata[j]);
	for (j = 0; j < subcmd_args->len; j++)
		g_ptr_array_add(cmd_argv, subcmd_args->pdata[j]);
	g_ptr_array_add(cmd_argv, NULL);

	/* LOW-5: Build a minimal environment for the child process */
	{
		const gchar *home_val = g_getenv("HOME");
		const gchar *path_val = g_getenv("PATH");
		const gchar *user_val = g_getenv("USER");
		const gchar *lang_val = g_getenv("LANG");

		envp = g_new0(gchar *, 6);
		envp[0] = g_strdup_printf("HOME=%s", home_val ? home_val : "/tmp");
		envp[1] = g_strdup_printf("PATH=%s", path_val ? path_val : "/usr/bin");
		envp[2] = g_strdup_printf("USER=%s", user_val ? user_val : "nobody");
		envp[3] = g_strdup_printf("LANG=%s", lang_val ? lang_val : "C");
		envp[4] = g_strdup("NO_DBOX_CHECK=1");
		envp[5] = NULL;
	}

	g_spawn_sync(
		NULL,                        /* working dir: inherit */
		(gchar **)cmd_argv->pdata,
		envp,
		G_SPAWN_SEARCH_PATH,
		NULL, NULL,                  /* child setup */
		out_stdout,
		out_stderr,
		&rc,
		&err
	);

	if (err) {
		if (out_stderr)
			*out_stderr = g_strdup(err->message);
		rc = 1;
	}

	g_strfreev(envp);
	g_ptr_array_free(global_flags, TRUE);
	g_ptr_array_free(subcmd_args, TRUE);
	g_ptr_array_free(cmd_argv, TRUE);

	return rc;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * JSON response helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * add_cors_headers() — attach CORS response headers.
 *
 * Matches Python cors(app, allow_origin="*") — wildcard origin is set
 * for all methods so POST responses are not blocked by the browser
 * when accessed cross-origin or through a proxy.
 *
 * TODO: In production, restrict Access-Control-Allow-Origin to specific
 * origins instead of "*". This requires C code changes to read allowed
 * origins from config and match against the request Origin header.
 *
 * @msg: The SoupServerMessage to annotate.
 */
static void
add_cors_headers(
	SoupServerMessage *msg
){
	SoupMessageHeaders *hdrs;

	hdrs = soup_server_message_get_response_headers(msg);

	soup_message_headers_replace(hdrs, "Access-Control-Allow-Origin", "*");
	soup_message_headers_replace(hdrs, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	soup_message_headers_replace(hdrs, "Access-Control-Allow-Headers",
	                             "Content-Type, Authorization");
}

/*
 * respond_json() — write a JSON body and set Content-Type.
 *
 * @msg:    The SoupServerMessage to send the response on.
 * @status: HTTP status code.
 * @json:   JSON string (transfer none — this function copies it).
 */
static void
respond_json(
	SoupServerMessage *msg,
	guint              status,
	const gchar       *json
){
	SoupMessageBody    *body;
	SoupMessageHeaders *hdrs;

	add_cors_headers(msg);
	soup_server_message_set_status(msg, status, NULL);

	hdrs = soup_server_message_get_response_headers(msg);
	soup_message_headers_set_content_type(hdrs, "application/json", NULL);

	body = soup_server_message_get_response_body(msg);
	soup_message_body_append(body, SOUP_MEMORY_COPY, json, strlen(json));
	soup_message_body_complete(body);
}

/*
 * respond_error() — send a simple {"error": "...", "message": "..."} response.
 *
 * @msg:    The SoupServerMessage.
 * @status: HTTP status code (e.g. 400, 404, 401).
 * @error:  Short error tag.
 * @detail: Human-readable message.
 */
static void
respond_error(
	SoupServerMessage *msg,
	guint              status,
	const gchar       *error,
	const gchar       *detail
){
	g_autoptr(JsonBuilder) b    = json_builder_new();
	g_autoptr(JsonNode)    root = NULL;
	g_autoptr(JsonGenerator) gen = json_generator_new();
	gchar                 *json = NULL;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "error");
	json_builder_add_string_value(b, error ? error : "error");
	json_builder_set_member_name(b, "message");
	json_builder_add_string_value(b, detail ? detail : "");
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	json = json_generator_to_data(gen, NULL);

	respond_json(msg, status, json);
	g_free(json);
}

/*
 * build_error_json() — construct a {"success": bool, "message": "..."} JSON string.
 *
 * CRIT-1: Replaces all hand-built g_strdup_printf JSON error responses
 * with properly escaped JSON via JsonBuilder.
 *
 * @success: Value for the "success" field.
 * @message: Human-readable message string (NULL becomes "Unknown error").
 *
 * Returns: (transfer full) JSON string.  Caller must g_free().
 */
static gchar *
build_error_json(
	gboolean     success,
	const gchar *message
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "success");
	json_builder_add_boolean_value(b, success);
	json_builder_set_member_name(b, "message");
	json_builder_add_string_value(b, message ? message : "Unknown error");
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

/*
 * build_wrapper_json() — wrap a raw JSON string inside {"key": <parsed>}.
 *
 * HIGH-5: Validates raw_json before embedding it into a response.
 * If raw_json is invalid, returns an error JSON instead.
 *
 * @key:      The wrapper object key name.
 * @raw_json: The raw JSON string from stdout.
 *
 * Returns: (transfer full) JSON string.  Caller must g_free().
 */
static gchar *
build_wrapper_json(
	const gchar *key,
	const gchar *raw_json
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonNode *parsed = NULL;

	if (!raw_json || !json_parser_load_from_data(parser, raw_json, -1, NULL)) {
		return build_error_json(FALSE, "Invalid data from backend");
	}

	parsed = json_parser_get_root(parser);

	b   = json_builder_new();
	gen = json_generator_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, key);
	json_builder_add_value(b, json_node_copy(parsed));
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

/*
 * build_sse_event_json() — build a JSON object for SSE broadcast data.
 *
 * CRIT-2: Replaces all hand-built g_strdup_printf SSE event payloads
 * with properly escaped JSON via JsonBuilder.
 *
 * Takes a NULL-terminated variadic list of key-value string pairs.
 * All values are treated as strings.
 *
 * @first_key: The first key string.
 * @...:       Alternating value, key, value, ..., NULL terminator.
 *
 * Returns: (transfer full) JSON string.  Caller must g_free().
 */
static gchar *
build_sse_event_json(
	const gchar *first_key,
	...
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	va_list ap;
	const gchar *key;
	const gchar *val;

	json_builder_begin_object(b);

	key = first_key;
	va_start(ap, first_key);
	while (key != NULL) {
		val = va_arg(ap, const gchar *);
		json_builder_set_member_name(b, key);
		json_builder_add_string_value(b, val ? val : "");
		key = va_arg(ap, const gchar *);
	}
	va_end(ap);

	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

/*
 * require_json_content_type() — enforce Content-Type: application/json on
 * POST requests (LOW-1).
 *
 * @msg: The incoming SoupServerMessage.
 *
 * Returns: TRUE if valid, FALSE if a 415 response was sent.
 */
static gboolean
require_json_content_type(
	SoupServerMessage *msg
){
	const gchar *ct;

	ct = soup_message_headers_get_one(
		soup_server_message_get_request_headers(msg), "Content-Type");
	if (!ct || !g_str_has_prefix(ct, "application/json")) {
		respond_error(msg, 415, "unsupported_media_type",
		              "Content-Type must be application/json");
		return FALSE;
	}
	return TRUE;
}

/*
 * parse_request_body() — parse the JSON body of an incoming POST request.
 *
 * LOW-3: Enforces a maximum body size of MAX_REQUEST_BODY_SIZE bytes.
 *
 * @msg: The incoming SoupServerMessage.
 *
 * Returns: (transfer full) JsonObject* or NULL if the body is absent/invalid.
 *          Caller must json_object_unref() the result.
 */
static JsonObject *
parse_request_body(
	SoupServerMessage *msg
){
	SoupMessageBody       *body;
	g_autoptr(GBytes)      flat   = NULL;
	g_autoptr(JsonParser)  parser = NULL;
	g_autoptr(GError)      err    = NULL;
	JsonNode              *root   = NULL;

	body = soup_server_message_get_request_body(msg);
	if (!body)
		return NULL;

	/* flatten accumulated chunks so body->data is populated */
	flat = soup_message_body_flatten(body);
	if (!body->data || body->length == 0)
		return NULL;

	/* LOW-3: reject oversized request bodies */
	if (body->length > MAX_REQUEST_BODY_SIZE)
		return NULL;

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, body->data, (gssize)body->length, &err))
		return NULL;

	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_object_ref(json_node_get_object(root));
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Auth check
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * check_auth() — validate the Bearer token on /api/* routes.
 *
 * Skips auth for:
 *   - GET /              (HTML UI)
 *   - GET /api/health
 *   - GET /api/presence  (read-only presence list, matches Python)
 *   - All paths when auth is disabled (no_token or no tokens loaded)
 *
 * HIGH-3: Token lookup uses constant-time comparison to prevent timing
 * side-channel attacks.
 *
 * SSE clients may pass ?token=<value> because EventSource cannot set headers.
 *
 * @app:       Application state.
 * @msg:       Incoming request message.
 * @path:      Request path.
 * @out_user:  (transfer full) Resolved username on success, or NULL.
 *
 * Returns: TRUE if the request is authorised, FALSE otherwise (response
 *          already written to @msg).
 */
static gboolean
check_auth(
	VimbanServeApp    *app,
	SoupServerMessage *msg,
	const gchar       *path,
	gchar            **out_user
){
	SoupMessageHeaders *req_hdrs;
	GUri               *uri;
	const gchar        *auth_hdr;
	const gchar        *token = NULL;
	const gchar        *username = NULL;
	GHashTableIter      iter;
	gpointer            key;
	gpointer            value;

	if (out_user) *out_user = NULL;

	/* When auth is disabled pass everything */
	if (!app->auth_enabled) return TRUE;

	/* PUBLIC paths that bypass auth */
	if (g_strcmp0(path, "/") == 0
	    || g_strcmp0(path, "/api/health") == 0
	    || g_strcmp0(path, "/api/presence") == 0)
	{
		return TRUE;
	}

	/* Only enforce on /api/ routes */
	if (!g_str_has_prefix(path, "/api/"))
		return TRUE;

	req_hdrs = soup_server_message_get_request_headers(msg);
	auth_hdr = soup_message_headers_get_one(req_hdrs, "Authorization");

	if (auth_hdr) {
		if (!g_str_has_prefix(auth_hdr, "Bearer ")) {
			respond_error(msg, 401, "unauthorized", "Bearer token required");
			return FALSE;
		}
		token = auth_hdr + 7; /* skip "Bearer " */
	}

	/* Fallback: ?token= query parameter (for SSE EventSource) */
	if (!token) {
		uri = soup_server_message_get_uri(msg);
		if (uri) {
			const gchar *query = g_uri_get_query(uri);
			if (query) {
				g_autoptr(GHashTable) params = soup_form_decode(query);
				/* LOW-4: Copy token before params hash table is freed */
				g_autofree gchar *token_copy = g_strdup(g_hash_table_lookup(params, "token"));
				token = token_copy;
				if (token) {
					/* HIGH-3: constant-time token comparison */
					username = NULL;
					g_hash_table_iter_init(&iter, app->tokens);
					while (g_hash_table_iter_next(&iter, &key, &value)) {
						if (constant_time_str_equal(token, (const gchar *)key)) {
							username = (const gchar *)value;
							break;
						}
					}
					if (!username) {
						respond_error(msg, 401, "unauthorized", "Invalid token");
						return FALSE;
					}
					if (out_user) *out_user = g_strdup(username);
					return TRUE;
				}
			}
		}
		respond_error(msg, 401, "unauthorized", "Authorization header required");
		return FALSE;
	}

	/* HIGH-3: constant-time token comparison for header-based auth */
	username = NULL;
	g_hash_table_iter_init(&iter, app->tokens);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (constant_time_str_equal(token, (const gchar *)key)) {
			username = (const gchar *)value;
			break;
		}
	}
	if (!username) {
		respond_error(msg, 401, "unauthorized", "Invalid token");
		return FALSE;
	}

	if (out_user) *out_user = g_strdup(username);
	return TRUE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * SSE management
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sse_send() — write a single SSE data frame to a client.
 *
 * @client:  The target SSE client.
 * @payload: JSON string to send as the data field.
 */
static void
sse_send(
	SseClient   *client,
	const gchar *payload
){
	g_autofree gchar *frame = NULL;
	SoupMessageBody  *body;

	if (!client || !client->msg) return;

	frame = g_strdup_printf("data: %s\n\n", payload);
	body  = soup_server_message_get_response_body(client->msg);
	soup_message_body_append(body, SOUP_MEMORY_COPY, frame, strlen(frame));
	/* Flush the chunk to the network */
	soup_server_message_unpause(client->msg);
}

/*
 * sse_broadcast() — send an event to every connected SSE client.
 *
 * CRIT-2: The data_json parameter is parsed and re-serialised via
 * JsonBuilder to guarantee safe JSON output.
 *
 * @app:        Application state (owns sse_clients array).
 * @event_type: Event type string (e.g. "TICKET_UPDATED").
 * @data_json:  JSON string of the event data object.
 */
static void
sse_broadcast(
	VimbanServeApp *app,
	const gchar    *event_type,
	const gchar    *data_json
){
	g_autoptr(JsonBuilder)   b   = json_builder_new();
	g_autoptr(JsonNode)      root = NULL;
	g_autoptr(JsonGenerator) gen = json_generator_new();
	gchar                   *payload = NULL;
	gint64                   ts;
	guint                    i;

	ts = g_get_real_time() / G_USEC_PER_SEC;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, event_type);
	json_builder_set_member_name(b, "data");

	/* Embed the pre-serialised data object verbatim via a raw value */
	{
		g_autoptr(JsonParser) p = json_parser_new();
		if (data_json && json_parser_load_from_data(p, data_json, -1, NULL)) {
			JsonNode *dn = json_parser_get_root(p);
			json_builder_add_value(b, json_node_copy(dn));
		} else {
			json_builder_begin_object(b);
			json_builder_end_object(b);
		}
	}

	json_builder_set_member_name(b, "ts");
	json_builder_add_int_value(b, ts);
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	payload = json_generator_to_data(gen, NULL);

	for (i = 0; i < app->sse_clients->len; i++)
		sse_send((SseClient *)app->sse_clients->pdata[i], payload);

	g_free(payload);
}

/*
 * sse_keepalive_cb() — GSource callback that sends SSE ping comments.
 *
 * Fires every SSE_KEEPALIVE_INTERVAL seconds to prevent proxies from
 * closing idle connections.
 *
 * @user_data: VimbanServeApp*
 */
static gboolean
sse_keepalive_cb(
	gpointer user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	gint64          ts;
	g_autofree gchar *frame = NULL;
	SoupMessageBody  *body;
	guint             i;

	ts    = g_get_real_time() / G_USEC_PER_SEC;
	frame = g_strdup_printf(": ping %" G_GINT64_FORMAT "\n\n", ts);

	for (i = 0; i < app->sse_clients->len; i++) {
		SseClient *c = (SseClient *)app->sse_clients->pdata[i];
		if (!c->msg) continue;
		body = soup_server_message_get_response_body(c->msg);
		soup_message_body_append(body, SOUP_MEMORY_COPY, frame, strlen(frame));
		soup_server_message_unpause(c->msg);
	}

	return G_SOURCE_CONTINUE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Path parsing helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * extract_path_segment() — extract the Nth path component (0-indexed).
 *
 * e.g. "/api/ticket/PROJ-42/move" with index 2 -> "PROJ-42"
 *
 * @path:  The request path string.
 * @index: Zero-based segment index.
 *
 * Returns: (transfer full) segment string, or NULL.
 */
static gchar *
extract_path_segment(
	const gchar *path,
	guint        index
){
	gchar  **parts;
	gchar   *result = NULL;
	guint    count  = 0;
	guint    i;

	if (!path) return NULL;

	parts = g_strsplit(path, "/", -1);

	/* Parts[0] is "" (before leading '/'), so real segments start at 1 */
	for (i = 1; parts[i] != NULL; i++) {
		if (*parts[i] == '\0') continue;  /* skip empty segments */
		if (count == index) {
			result = g_strdup(parts[i]);
			break;
		}
		count++;
	}

	g_strfreev(parts);
	return result;
}

/*
 * get_query_param() — extract a query parameter value from a SoupServerMessage.
 *
 * @msg:   The request message.
 * @param: Parameter name.
 *
 * Returns: (transfer full) parameter value, or NULL if absent.
 */
static gchar *
get_query_param(
	SoupServerMessage *msg,
	const gchar       *param
){
	GUri               *uri;
	const gchar        *query;
	g_autoptr(GHashTable) params = NULL;
	const gchar        *val;

	uri = soup_server_message_get_uri(msg);
	if (!uri) return NULL;

	query = g_uri_get_query(uri);
	if (!query) return NULL;

	params = soup_form_decode(query);
	val    = g_hash_table_lookup(params, param);

	return val ? g_strdup(val) : NULL;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Route handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Full embedded HTML UI template (from Python vimban_serve) */
static const gchar *EMBEDDED_HTML =
	"<!DOCTYPE html>\n"
	"<html lang=\"en\">\n"
	"<head>\n"
	"    <meta charset=\"UTF-8\">\n"
	"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
	"    <title>vimban</title>\n"
	"    <link href=\"https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;600;700&display=swap\" rel=\"stylesheet\">\n"
	"    <style>\n"
	"        :root {\n"
	"            /* Catppuccin Mocha */\n"
	"            --bg-base: #1e1e2e;\n"
	"            --bg-mantle: #181825;\n"
	"            --bg-crust: #11111b;\n"
	"            --bg-surface0: #313244;\n"
	"            --bg-surface1: #45475a;\n"
	"            --bg-surface2: #585b70;\n"
	"            --bg-overlay0: #6c7086;\n"
	"            --text: #cdd6f4;\n"
	"            --text-sub: #bac2de;\n"
	"            --text-overlay: #a6adc8;\n"
	"            --text-muted: #8087a2;\n"
	"            --blue: #89b4fa;\n"
	"            --lavender: #b4befe;\n"
	"            --sapphire: #74c7ec;\n"
	"            --teal: #94e2d5;\n"
	"            --green: #a6e3a1;\n"
	"            --yellow: #f9e2af;\n"
	"            --peach: #fab387;\n"
	"            --maroon: #eba0ac;\n"
	"            --red: #f38ba8;\n"
	"            --mauve: #cba6f7;\n"
	"            --pink: #f5c2e7;\n"
	"            --flamingo: #f2cdcd;\n"
	"            --rosewater: #f5e0dc;\n"
	"            --shadow: 0 4px 20px rgba(0, 0, 0, 0.4);\n"
	"            --radius: 8px;\n"
	"            --radius-sm: 4px;\n"
	"        }\n"
	"\n"
	"        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
	"        :focus-visible { outline: 2px solid var(--blue); outline-offset: 2px; }\n"
	"        :focus:not(:focus-visible) { outline: none; }\n"
	"\n"
	"        body {\n"
	"            font-family: 'JetBrains Mono', monospace;\n"
	"            background: var(--bg-crust);\n"
	"            color: var(--text);\n"
	"            line-height: 1.6;\n"
	"            min-height: 100vh;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* HEADER                                                       */\n"
	"        /* ============================================================ */\n"
	"        .header {\n"
	"            background: linear-gradient(135deg, var(--bg-mantle), var(--bg-base));\n"
	"            border-bottom: 1px solid var(--bg-surface0);\n"
	"            padding: 14px 24px;\n"
	"            position: sticky;\n"
	"            top: 0;\n"
	"            z-index: 100;\n"
	"            backdrop-filter: blur(10px);\n"
	"        }\n"
	"\n"
	"        .header-content {\n"
	"            max-width: 1800px;\n"
	"            margin: 0 auto;\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            align-items: center;\n"
	"            gap: 16px;\n"
	"            flex-wrap: wrap;\n"
	"        }\n"
	"\n"
	"        .logo {\n"
	"            display: flex;\n"
	"            align-items: center;\n"
	"            gap: 10px;\n"
	"        }\n"
	"\n"
	"        .logo h1 {\n"
	"            font-size: 22px;\n"
	"            font-weight: 700;\n"
	"            background: linear-gradient(90deg, var(--green), var(--teal));\n"
	"            -webkit-background-clip: text;\n"
	"            -webkit-text-fill-color: transparent;\n"
	"            background-clip: text;\n"
	"        }\n"
	"\n"
	"        .logo .version {\n"
	"            font-size: 11px;\n"
	"            color: var(--text-muted);\n"
	"            padding: 2px 8px;\n"
	"            background: var(--bg-surface0);\n"
	"            border-radius: 10px;\n"
	"        }\n"
	"\n"
	"        .header-actions {\n"
	"            display: flex;\n"
	"            gap: 8px;\n"
	"            align-items: center;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* NAVIGATION TABS                                              */\n"
	"        /* ============================================================ */\n"
	"        .nav-tabs {\n"
	"            display: flex;\n"
	"            gap: 2px;\n"
	"            background: var(--bg-mantle);\n"
	"            padding: 4px;\n"
	"            border-radius: var(--radius);\n"
	"        }\n"
	"\n"
	"        .nav-tab {\n"
	"            padding: 8px 16px;\n"
	"            background: transparent;\n"
	"            border: none;\n"
	"            border-radius: var(--radius-sm);\n"
	"            color: var(--text-muted);\n"
	"            font-family: inherit;\n"
	"            font-size: 13px;\n"
	"            font-weight: 500;\n"
	"            cursor: pointer;\n"
	"            transition: all 0.2s ease;\n"
	"        }\n"
	"\n"
	"        .nav-tab:hover {\n"
	"            color: var(--text);\n"
	"            background: var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        .nav-tab.active {\n"
	"            color: var(--bg-crust);\n"
	"            background: var(--green);\n"
	"            font-weight: 600;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* STATS BAR                                                    */\n"
	"        /* ============================================================ */\n"
	"        .stats-bar {\n"
	"            display: flex;\n"
	"            gap: 12px;\n"
	"            padding: 12px 24px;\n"
	"            background: var(--bg-mantle);\n"
	"            border-bottom: 1px solid var(--bg-surface0);\n"
	"            flex-wrap: wrap;\n"
	"            max-width: 1800px;\n"
	"            margin: 0 auto;\n"
	"            position: relative;\n"
	"            z-index: 50;\n"
	"        }\n"
	"\n"
	"        .stat-item {\n"
	"            display: flex;\n"
	"            align-items: center;\n"
	"            gap: 8px;\n"
	"            padding: 6px 14px;\n"
	"            background: var(--bg-base);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            font-size: 12px;\n"
	"        }\n"
	"\n"
	"        .stat-value {\n"
	"            font-weight: 700;\n"
	"            color: var(--green);\n"
	"            font-size: 16px;\n"
	"        }\n"
	"\n"
	"        .stat-label { color: var(--text-muted); }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* MAIN LAYOUT                                                  */\n"
	"        /* ============================================================ */\n"
	"        .main-container {\n"
	"            max-width: 1800px;\n"
	"            margin: 0 auto;\n"
	"            padding: 20px 24px;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* TOOLBAR                                                      */\n"
	"        /* ============================================================ */\n"
	"        .toolbar {\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            align-items: center;\n"
	"            margin-bottom: 16px;\n"
	"            flex-wrap: wrap;\n"
	"            gap: 12px;\n"
	"        }\n"
	"\n"
	"        .search-box {\n"
	"            display: flex;\n"
	"            align-items: center;\n"
	"            gap: 8px;\n"
	"            flex: 1;\n"
	"            max-width: 400px;\n"
	"        }\n"
	"\n"
	"        .search-input {\n"
	"            flex: 1;\n"
	"            padding: 10px 14px;\n"
	"            background: var(--bg-base);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius);\n"
	"            color: var(--text);\n"
	"            font-family: inherit;\n"
	"            font-size: 13px;\n"
	"            transition: all 0.2s ease;\n"
	"        }\n"
	"\n"
	"        .search-input:focus {\n"
	"            outline: none;\n"
	"            border-color: var(--green);\n"
	"            box-shadow: 0 0 0 3px rgba(166, 227, 161, 0.1);\n"
	"        }\n"
	"\n"
	"        .search-input::placeholder { color: var(--text-muted); }\n"
	"\n"
	"        .filters {\n"
	"            display: flex;\n"
	"            gap: 8px;\n"
	"            flex-wrap: wrap;\n"
	"            align-items: center;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* PROJECT TABS                                                 */\n"
	"        /* ============================================================ */\n"
	"        .project-tabs {\n"
	"            display: flex;\n"
	"            gap: 4px;\n"
	"            padding: 8px 0 0 0;\n"
	"            border-bottom: 2px solid var(--bg-surface0);\n"
	"            margin-bottom: 12px;\n"
	"            overflow-x: auto;\n"
	"            flex-wrap: nowrap;\n"
	"        }\n"
	"\n"
	"        .project-tab {\n"
	"            background: transparent;\n"
	"            border: none;\n"
	"            color: var(--text-muted);\n"
	"            padding: 6px 14px;\n"
	"            border-radius: var(--radius-sm) var(--radius-sm) 0 0;\n"
	"            cursor: pointer;\n"
	"            font-family: inherit;\n"
	"            font-size: 0.85rem;\n"
	"            white-space: nowrap;\n"
	"            position: relative;\n"
	"            transition: color 0.15s, background 0.15s;\n"
	"            border-bottom: 2px solid transparent;\n"
	"            margin-bottom: -2px;\n"
	"        }\n"
	"\n"
	"        .project-tab:hover { color: var(--text); background: var(--bg-surface0); }\n"
	"        .project-tab.active { color: var(--blue); border-bottom-color: var(--blue); }\n"
	"\n"
	"        .project-tab .badge {\n"
	"            background: var(--bg-surface1);\n"
	"            color: var(--text-sub);\n"
	"            font-size: 0.7rem;\n"
	"            padding: 1px 5px;\n"
	"            border-radius: 8px;\n"
	"            margin-left: 5px;\n"
	"        }\n"
	"\n"
	"        .project-tab.active .badge { background: var(--blue); color: var(--bg-base); }\n"
	"\n"
	"        select {\n"
	"            padding: 8px 12px;\n"
	"            background: var(--bg-base);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius);\n"
	"            color: var(--text);\n"
	"            font-family: inherit;\n"
	"            font-size: 13px;\n"
	"            cursor: pointer;\n"
	"            transition: all 0.2s ease;\n"
	"        }\n"
	"\n"
	"        select:hover { border-color: var(--blue); }\n"
	"        select:focus { outline: none; border-color: var(--green); }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* BUTTONS                                                      */\n"
	"        /* ============================================================ */\n"
	"        .btn {\n"
	"            padding: 8px 16px;\n"
	"            border: none;\n"
	"            border-radius: var(--radius);\n"
	"            cursor: pointer;\n"
	"            font-family: inherit;\n"
	"            font-weight: 600;\n"
	"            font-size: 13px;\n"
	"            transition: all 0.2s ease;\n"
	"            display: inline-flex;\n"
	"            align-items: center;\n"
	"            gap: 6px;\n"
	"        }\n"
	"\n"
	"        .btn-primary {\n"
	"            background: linear-gradient(135deg, var(--green), var(--teal));\n"
	"            color: var(--bg-crust);\n"
	"        }\n"
	"\n"
	"        .btn-primary:hover {\n"
	"            transform: translateY(-1px);\n"
	"            box-shadow: 0 4px 15px rgba(166, 227, 161, 0.3);\n"
	"        }\n"
	"\n"
	"        .btn-secondary {\n"
	"            background: var(--bg-surface0);\n"
	"            color: var(--text);\n"
	"            border: 1px solid var(--bg-surface1);\n"
	"        }\n"
	"\n"
	"        .btn-secondary:hover {\n"
	"            background: var(--bg-surface1);\n"
	"            border-color: var(--blue);\n"
	"        }\n"
	"\n"
	"        .btn-danger {\n"
	"            background: var(--red);\n"
	"            color: var(--bg-crust);\n"
	"        }\n"
	"\n"
	"        .btn-danger:hover { background: var(--maroon); }\n"
	"\n"
	"        .btn-small {\n"
	"            padding: 5px 10px;\n"
	"            font-size: 11px;\n"
	"        }\n"
	"\n"
	"        .btn-icon {\n"
	"            padding: 6px;\n"
	"            min-width: 32px;\n"
	"            justify-content: center;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* BADGES                                                       */\n"
	"        /* ============================================================ */\n"
	"        .badge {\n"
	"            display: inline-block;\n"
	"            padding: 3px 10px;\n"
	"            border-radius: 20px;\n"
	"            font-size: 11px;\n"
	"            font-weight: 600;\n"
	"            text-transform: uppercase;\n"
	"            letter-spacing: 0.3px;\n"
	"        }\n"
	"\n"
	"        .badge-backlog { background: rgba(108,112,134,0.2); color: var(--bg-overlay0); }\n"
	"        .badge-ready { background: rgba(137,180,250,0.2); color: var(--blue); }\n"
	"        .badge-in_progress { background: rgba(249,226,175,0.2); color: var(--yellow); }\n"
	"        .badge-blocked { background: rgba(243,139,168,0.2); color: var(--red); }\n"
	"        .badge-review { background: rgba(203,166,247,0.2); color: var(--mauve); }\n"
	"        .badge-delegated { background: rgba(148,226,213,0.2); color: var(--teal); }\n"
	"        .badge-done { background: rgba(166,227,161,0.2); color: var(--green); }\n"
	"        .badge-cancelled { background: rgba(88,91,112,0.2); color: var(--bg-surface2); }\n"
	"\n"
	"        .badge-critical { background: rgba(243,139,168,0.2); color: var(--red); }\n"
	"        .badge-high { background: rgba(250,179,135,0.2); color: var(--peach); }\n"
	"        .badge-medium { background: rgba(249,226,175,0.2); color: var(--yellow); }\n"
	"        .badge-low { background: rgba(166,227,161,0.2); color: var(--green); }\n"
	"\n"
	"        .tags { display: flex; gap: 4px; flex-wrap: wrap; }\n"
	"\n"
	"        .tag {\n"
	"            padding: 2px 8px;\n"
	"            background: var(--bg-surface0);\n"
	"            border-radius: var(--radius-sm);\n"
	"            font-size: 11px;\n"
	"            color: var(--text-overlay);\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* TABLE                                                        */\n"
	"        /* ============================================================ */\n"
	"        .table-container {\n"
	"            background: var(--bg-base);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            overflow: hidden;\n"
	"            box-shadow: var(--shadow);\n"
	"        }\n"
	"\n"
	"        .table-scroll { overflow-x: auto; }\n"
	"\n"
	"        table { width: 100%; border-collapse: collapse; }\n"
	"\n"
	"        th, td {\n"
	"            padding: 12px 14px;\n"
	"            text-align: left;\n"
	"            border-bottom: 1px solid var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        th {\n"
	"            background: var(--bg-mantle);\n"
	"            font-weight: 600;\n"
	"            color: var(--text-muted);\n"
	"            font-size: 11px;\n"
	"            text-transform: uppercase;\n"
	"            letter-spacing: 0.5px;\n"
	"            position: sticky;\n"
	"            top: 0;\n"
	"            z-index: 10;\n"
	"            cursor: pointer;\n"
	"            user-select: none;\n"
	"        }\n"
	"\n"
	"        th:hover { color: var(--green); }\n"
	"        th.sorted-asc::after { content: ' \\25B2'; color: var(--green); }\n"
	"        th.sorted-desc::after { content: ' \\25BC'; color: var(--green); }\n"
	"\n"
	"        tr { transition: background 0.15s ease; }\n"
	"        tr:hover { background: var(--bg-mantle); }\n"
	"        tr.clickable { cursor: pointer; }\n"
	"\n"
	"        td { font-size: 13px; }\n"
	"\n"
	"        .id-cell {\n"
	"            font-weight: 600;\n"
	"            color: var(--blue);\n"
	"            white-space: nowrap;\n"
	"        }\n"
	"\n"
	"        .title-cell {\n"
	"            max-width: 350px;\n"
	"            overflow: hidden;\n"
	"            text-overflow: ellipsis;\n"
	"            white-space: nowrap;\n"
	"        }\n"
	"\n"
	"        .date-cell {\n"
	"            color: var(--text-muted);\n"
	"            white-space: nowrap;\n"
	"            font-size: 12px;\n"
	"        }\n"
	"\n"
	"        .progress-bar-bg {\n"
	"            width: 80px;\n"
	"            height: 6px;\n"
	"            background: var(--bg-surface0);\n"
	"            border-radius: 3px;\n"
	"            overflow: hidden;\n"
	"        }\n"
	"\n"
	"        .progress-bar-fill {\n"
	"            height: 100%;\n"
	"            background: var(--green);\n"
	"            border-radius: 3px;\n"
	"            transition: width 0.3s ease;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* KANBAN BOARD                                                 */\n"
	"        /* ============================================================ */\n"
	"        .kanban-board {\n"
	"            display: flex;\n"
	"            gap: 12px;\n"
	"            overflow-x: auto;\n"
	"            padding-bottom: 12px;\n"
	"            min-height: 500px;\n"
	"        }\n"
	"\n"
	"        .kanban-column {\n"
	"            flex: 0 0 280px;\n"
	"            background: var(--bg-base);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            display: flex;\n"
	"            flex-direction: column;\n"
	"            max-height: calc(100vh - 280px); /* header ~52px + stats ~40px + toolbar ~50px + padding */\n"
	"        }\n"
	"\n"
	"        .kanban-column-header {\n"
	"            padding: 12px 14px;\n"
	"            border-bottom: 1px solid var(--bg-surface0);\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            align-items: center;\n"
	"            background: var(--bg-mantle);\n"
	"            border-radius: var(--radius) var(--radius) 0 0;\n"
	"        }\n"
	"\n"
	"        .kanban-column-title {\n"
	"            font-weight: 600;\n"
	"            font-size: 12px;\n"
	"            text-transform: uppercase;\n"
	"            letter-spacing: 0.5px;\n"
	"        }\n"
	"\n"
	"        .kanban-column-count {\n"
	"            background: var(--bg-surface0);\n"
	"            padding: 2px 8px;\n"
	"            border-radius: 10px;\n"
	"            font-size: 11px;\n"
	"            font-weight: 600;\n"
	"            color: var(--text-muted);\n"
	"        }\n"
	"\n"
	"        .kanban-column-body {\n"
	"            flex: 1;\n"
	"            overflow-y: auto;\n"
	"            padding: 8px;\n"
	"            display: flex;\n"
	"            flex-direction: column;\n"
	"            gap: 6px;\n"
	"        }\n"
	"\n"
	"        .kanban-card {\n"
	"            background: var(--bg-mantle);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius-sm);\n"
	"            padding: 10px 12px;\n"
	"            cursor: pointer;\n"
	"            transition: all 0.2s ease;\n"
	"        }\n"
	"\n"
	"        .kanban-card:hover {\n"
	"            border-color: var(--blue);\n"
	"            transform: translateY(-1px);\n"
	"            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);\n"
	"        }\n"
	"\n"
	"        .kanban-card-id {\n"
	"            font-size: 11px;\n"
	"            color: var(--blue);\n"
	"            font-weight: 600;\n"
	"            margin-bottom: 4px;\n"
	"        }\n"
	"\n"
	"        .kanban-card-title {\n"
	"            font-size: 13px;\n"
	"            margin-bottom: 6px;\n"
	"            line-height: 1.4;\n"
	"        }\n"
	"\n"
	"        .kanban-card-meta {\n"
	"            display: flex;\n"
	"            gap: 6px;\n"
	"            flex-wrap: wrap;\n"
	"            align-items: center;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* MODAL                                                        */\n"
	"        /* ============================================================ */\n"
	"        .modal-overlay {\n"
	"            display: none;\n"
	"            position: fixed;\n"
	"            top: 0; left: 0;\n"
	"            width: 100%; height: 100%;\n"
	"            background: rgba(0, 0, 0, 0.75);\n"
	"            z-index: 1000;\n"
	"            justify-content: center;\n"
	"            align-items: flex-start;\n"
	"            padding: 40px 20px;\n"
	"            overflow-y: auto;\n"
	"            backdrop-filter: blur(4px);\n"
	"        }\n"
	"\n"
	"        .modal-overlay.active { display: flex; }\n"
	"\n"
	"        .modal {\n"
	"            background: var(--bg-base);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius);\n"
	"            width: 100%;\n"
	"            max-width: 700px;\n"
	"            max-height: 90vh;\n"
	"            overflow-y: auto;\n"
	"            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);\n"
	"            animation: modalSlide 0.2s ease;\n"
	"        }\n"
	"\n"
	"        @keyframes modalSlide {\n"
	"            from { opacity: 0; transform: translateY(-20px); }\n"
	"            to { opacity: 1; transform: translateY(0); }\n"
	"        }\n"
	"\n"
	"        .modal-header {\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            align-items: center;\n"
	"            padding: 16px 20px;\n"
	"            border-bottom: 1px solid var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        .modal-header h2 {\n"
	"            font-size: 18px;\n"
	"            font-weight: 600;\n"
	"            color: var(--green);\n"
	"        }\n"
	"\n"
	"        .modal-close {\n"
	"            background: none;\n"
	"            border: none;\n"
	"            color: var(--text-muted);\n"
	"            font-size: 22px;\n"
	"            cursor: pointer;\n"
	"            padding: 4px 8px;\n"
	"            border-radius: var(--radius-sm);\n"
	"            transition: all 0.2s;\n"
	"        }\n"
	"\n"
	"        .modal-close:hover {\n"
	"            color: var(--red);\n"
	"            background: var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        .modal-body { padding: 20px; scrollbar-gutter: stable; }\n"
	"\n"
	"        .modal-footer {\n"
	"            display: flex;\n"
	"            justify-content: flex-end;\n"
	"            gap: 8px;\n"
	"            padding: 16px 20px;\n"
	"            border-top: 1px solid var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* FORM                                                         */\n"
	"        /* ============================================================ */\n"
	"        .form-group {\n"
	"            margin-bottom: 14px;\n"
	"        }\n"
	"\n"
	"        .form-group label {\n"
	"            display: block;\n"
	"            font-size: 12px;\n"
	"            font-weight: 600;\n"
	"            color: var(--text-muted);\n"
	"            margin-bottom: 4px;\n"
	"            text-transform: uppercase;\n"
	"            letter-spacing: 0.3px;\n"
	"        }\n"
	"\n"
	"        .form-group input,\n"
	"        .form-group textarea,\n"
	"        .form-group select {\n"
	"            width: 100%;\n"
	"            padding: 10px 12px;\n"
	"            background: var(--bg-mantle);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius-sm);\n"
	"            color: var(--text);\n"
	"            font-family: inherit;\n"
	"            font-size: 13px;\n"
	"            transition: border-color 0.2s;\n"
	"        }\n"
	"\n"
	"        .form-group input:focus,\n"
	"        .form-group textarea:focus,\n"
	"        .form-group select:focus {\n"
	"            outline: none;\n"
	"            border-color: var(--green);\n"
	"        }\n"
	"\n"
	"        .form-group textarea {\n"
	"            min-height: 80px;\n"
	"            resize: vertical;\n"
	"        }\n"
	"\n"
	"        .form-row {\n"
	"            display: grid;\n"
	"            grid-template-columns: 1fr 1fr;\n"
	"            gap: 12px;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* DETAIL VIEW                                                  */\n"
	"        /* ============================================================ */\n"
	"        .detail-header {\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            align-items: flex-start;\n"
	"            margin-bottom: 16px;\n"
	"        }\n"
	"\n"
	"        .detail-id {\n"
	"            font-size: 14px;\n"
	"            color: var(--blue);\n"
	"            font-weight: 600;\n"
	"        }\n"
	"\n"
	"        .detail-title {\n"
	"            font-size: 20px;\n"
	"            font-weight: 700;\n"
	"            margin: 8px 0;\n"
	"        }\n"
	"\n"
	"        .detail-meta {\n"
	"            display: grid;\n"
	"            grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));\n"
	"            gap: 12px;\n"
	"            margin-bottom: 16px;\n"
	"            padding: 14px;\n"
	"            background: var(--bg-mantle);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"        }\n"
	"\n"
	"        .detail-field label {\n"
	"            font-size: 11px;\n"
	"            color: var(--text-muted);\n"
	"            text-transform: uppercase;\n"
	"            letter-spacing: 0.3px;\n"
	"        }\n"
	"\n"
	"        .detail-field .value {\n"
	"            font-size: 13px;\n"
	"            margin-top: 2px;\n"
	"        }\n"
	"\n"
	"        .detail-body {\n"
	"            padding: 14px;\n"
	"            background: var(--bg-mantle);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            margin-bottom: 16px;\n"
	"            white-space: pre-wrap;\n"
	"            font-size: 13px;\n"
	"            line-height: 1.7;\n"
	"        }\n"
	"\n"
	"        .comment-list {\n"
	"            margin-top: 16px;\n"
	"        }\n"
	"\n"
	"        .comment {\n"
	"            padding: 12px;\n"
	"            background: var(--bg-mantle);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            margin-bottom: 8px;\n"
	"        }\n"
	"\n"
	"        .comment-header {\n"
	"            display: flex;\n"
	"            justify-content: space-between;\n"
	"            font-size: 12px;\n"
	"            color: var(--text-muted);\n"
	"            margin-bottom: 6px;\n"
	"        }\n"
	"\n"
	"        .comment-author { color: var(--blue); font-weight: 600; }\n"
	"\n"
	"        .comment-body { font-size: 13px; }\n"
	"\n"
	"        .comment-form {\n"
	"            display: flex;\n"
	"            gap: 8px;\n"
	"            margin-top: 12px;\n"
	"        }\n"
	"\n"
	"        .comment-form input {\n"
	"            flex: 1;\n"
	"            padding: 8px 12px;\n"
	"            background: var(--bg-mantle);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius-sm);\n"
	"            color: var(--text);\n"
	"            font-family: inherit;\n"
	"            font-size: 13px;\n"
	"        }\n"
	"\n"
	"        .comment-form input:focus {\n"
	"            outline: none;\n"
	"            border-color: var(--green);\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* DASHBOARD                                                    */\n"
	"        /* ============================================================ */\n"
	"        .dashboard-content {\n"
	"            background: var(--bg-base);\n"
	"            border-radius: var(--radius);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            padding: 20px;\n"
	"            white-space: pre-wrap;\n"
	"            font-size: 13px;\n"
	"            line-height: 1.6;\n"
	"            max-height: calc(100vh - 300px);\n"
	"            overflow-y: auto;\n"
	"        }\n"
	"\n"
	"        .dashboard-tabs {\n"
	"            display: flex;\n"
	"            gap: 6px;\n"
	"            margin-bottom: 16px;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* PEOPLE VIEW                                                  */\n"
	"        /* ============================================================ */\n"
	"        .people-grid {\n"
	"            display: grid;\n"
	"            grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));\n"
	"            gap: 12px;\n"
	"        }\n"
	"\n"
	"        .person-card {\n"
	"            background: var(--bg-base);\n"
	"            border: 1px solid var(--bg-surface0);\n"
	"            border-radius: var(--radius);\n"
	"            padding: 16px;\n"
	"            cursor: pointer;\n"
	"            transition: all 0.2s ease;\n"
	"        }\n"
	"\n"
	"        .person-card:hover {\n"
	"            border-color: var(--blue);\n"
	"            transform: translateY(-2px);\n"
	"            box-shadow: 0 4px 15px rgba(0, 0, 0, 0.3);\n"
	"        }\n"
	"\n"
	"        .person-name {\n"
	"            font-size: 15px;\n"
	"            font-weight: 600;\n"
	"            color: var(--text);\n"
	"            margin-bottom: 4px;\n"
	"        }\n"
	"\n"
	"        .person-role {\n"
	"            font-size: 12px;\n"
	"            color: var(--text-muted);\n"
	"            margin-bottom: 8px;\n"
	"        }\n"
	"\n"
	"        .person-team {\n"
	"            font-size: 11px;\n"
	"            padding: 2px 8px;\n"
	"            background: var(--bg-surface0);\n"
	"            border-radius: var(--radius-sm);\n"
	"            color: var(--blue);\n"
	"            display: inline-block;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* EMPTY STATE                                                  */\n"
	"        /* ============================================================ */\n"
	"        .empty-state {\n"
	"            text-align: center;\n"
	"            padding: 60px 20px;\n"
	"            color: var(--text-muted);\n"
	"        }\n"
	"\n"
	"        .empty-state-icon { font-size: 48px; margin-bottom: 12px; }\n"
	"        .empty-state h3 { font-size: 18px; margin-bottom: 6px; color: var(--text-overlay); }\n"
	"        .empty-state p { font-size: 13px; }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* SPINNER                                                      */\n"
	"        /* ============================================================ */\n"
	"        .spinner {\n"
	"            display: inline-block;\n"
	"            width: 24px;\n"
	"            height: 24px;\n"
	"            border: 3px solid var(--bg-surface1);\n"
	"            border-top-color: var(--green);\n"
	"            border-radius: 50%;\n"
	"            animation: spin 0.6s linear infinite;\n"
	"        }\n"
	"        .spinner-container {\n"
	"            display: flex;\n"
	"            align-items: center;\n"
	"            justify-content: center;\n"
	"            padding: 40px;\n"
	"            gap: 12px;\n"
	"            color: var(--text-muted);\n"
	"            font-size: 13px;\n"
	"        }\n"
	"        @keyframes spin { to { transform: rotate(360deg); } }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* TOAST                                                        */\n"
	"        /* ============================================================ */\n"
	"        .toast-container {\n"
	"            position: fixed;\n"
	"            bottom: 20px;\n"
	"            right: 20px;\n"
	"            z-index: 2000;\n"
	"            display: flex;\n"
	"            flex-direction: column;\n"
	"            gap: 8px;\n"
	"        }\n"
	"\n"
	"        .toast {\n"
	"            padding: 12px 20px;\n"
	"            border-radius: var(--radius);\n"
	"            font-size: 13px;\n"
	"            font-weight: 500;\n"
	"            animation: toastIn 0.3s ease;\n"
	"            max-width: 400px;\n"
	"        }\n"
	"\n"
	"        .toast-success {\n"
	"            background: var(--green);\n"
	"            color: var(--bg-crust);\n"
	"        }\n"
	"\n"
	"        .toast-error {\n"
	"            background: var(--red);\n"
	"            color: var(--bg-crust);\n"
	"        }\n"
	"\n"
	"        @keyframes toastIn {\n"
	"            from { opacity: 0; transform: translateY(20px); }\n"
	"            to { opacity: 1; transform: translateY(0); }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* SCROLLBAR                                                    */\n"
	"        /* ============================================================ */\n"
	"        ::-webkit-scrollbar { width: 6px; height: 6px; }\n"
	"        ::-webkit-scrollbar-track { background: var(--bg-crust); }\n"
	"        ::-webkit-scrollbar-thumb { background: var(--bg-surface1); border-radius: 3px; }\n"
	"        ::-webkit-scrollbar-thumb:hover { background: var(--bg-surface2); }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* RESPONSIVE                                                   */\n"
	"        /* ============================================================ */\n"
	"        @media (max-width: 1024px) {\n"
	"            .kanban-column { flex: 0 0 240px; }\n"
	"            .detail-meta { grid-template-columns: 1fr 1fr; }\n"
	"        }\n"
	"        @media (max-width: 768px) {\n"
	"            .header-content { flex-direction: column; align-items: flex-start; }\n"
	"            .toolbar { flex-direction: column; }\n"
	"            .search-box { max-width: 100%; }\n"
	"            .form-row { grid-template-columns: 1fr; }\n"
	"            .kanban-board { flex-direction: column; overflow-x: visible; }\n"
	"            .kanban-column { flex: 1 1 auto; max-height: none; }\n"
	"            .modal { max-width: 95vw; }\n"
	"            .stats-bar { flex-wrap: wrap; gap: 8px; }\n"
	"            .table-scroll { overflow-x: auto; }\n"
	"        }\n"
	"        @media (max-width: 480px) {\n"
	"            .header { padding: 10px 12px; }\n"
	"            .main-container { padding: 12px; }\n"
	"            .nav-tabs { gap: 2px; }\n"
	"            .nav-tab { padding: 6px 10px; font-size: 12px; }\n"
	"            .modal { max-width: 100vw; border-radius: 0; }\n"
	"            .modal-overlay { padding: 0; }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* VIEW PANELS (show/hide)                                      */\n"
	"        /* ============================================================ */\n"
	"        .view-panel { display: none; }\n"
	"        .view-panel.active { display: block; }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* COLLAB PRESENCE                                              */\n"
	"        /* ============================================================ */\n"
	"        .presence-bar {\n"
	"            display: flex;\n"
	"            align-items: center;\n"
	"            gap: 6px;\n"
	"            padding: 0 8px;\n"
	"        }\n"
	"        .presence-label {\n"
	"            font-size: 0.72rem;\n"
	"            color: var(--text-muted);\n"
	"        }\n"
	"        .presence-dot {\n"
	"            display: inline-flex;\n"
	"            align-items: center;\n"
	"            justify-content: center;\n"
	"            width: 22px;\n"
	"            height: 22px;\n"
	"            border-radius: 50%;\n"
	"            background: var(--blue);\n"
	"            color: var(--bg-base);\n"
	"            font-size: 0.7rem;\n"
	"            font-weight: 700;\n"
	"            cursor: default;\n"
	"        }\n"
	"        .presence-offline { color: var(--text-muted); font-size: 0.8rem; }\n"
	"        .presence-settings-btn {\n"
	"            background: none;\n"
	"            border: none;\n"
	"            cursor: pointer;\n"
	"            color: var(--text-muted);\n"
	"            font-size: 0.85rem;\n"
	"            padding: 2px 5px;\n"
	"            border-radius: 4px;\n"
	"        }\n"
	"        .presence-settings-btn:hover { color: var(--text); background: var(--bg-overlay0); }\n"
	"    </style>\n"
	"</head>\n"
	"<body>\n"
	"    <!-- HEADER -->\n"
	"    <div class=\"header\">\n"
	"        <div class=\"header-content\">\n"
	"            <div class=\"logo\">\n"
	"                <h1>vimban</h1>\n"
	"                <span class=\"version\">v" VIMBAN_SERVE_VERSION "</span>\n"
	"            </div>\n"
	"            <nav class=\"nav-tabs\">\n"
	"                <button class=\"nav-tab active\" onclick=\"switchView('kanban', event)\" aria-label=\"Kanban view\">Kanban</button>\n"
	"                <button class=\"nav-tab\" onclick=\"switchView('list', event)\" aria-label=\"List view\">List</button>\n"
	"                <button class=\"nav-tab\" onclick=\"switchView('dashboard', event)\" aria-label=\"Dashboard view\">Dashboard</button>\n"
	"                <button class=\"nav-tab\" onclick=\"switchView('people', event)\" aria-label=\"People view\">People</button>\n"
	"            </nav>\n"
	"            <div class=\"header-actions\">\n"
	"                <div class=\"presence-bar\" id=\"presenceBar\"></div>\n"
	"                <button class=\"presence-settings-btn\" onclick=\"openCollabSettings()\" title=\"Collab settings\">&#9881;</button>\n"
	"                <button class=\"btn btn-secondary btn-small\" onclick=\"gitSync()\">Sync</button>\n"
	"                <button class=\"btn btn-primary btn-small\" onclick=\"openCreateModal()\">+ New</button>\n"
	"            </div>\n"
	"        </div>\n"
	"    </div>\n"
	"\n"
	"    <!-- STATS BAR -->\n"
	"    <div class=\"stats-bar\" id=\"statsBar\"></div>\n"
	"\n"
	"    <!-- MAIN CONTENT -->\n"
	"    <div class=\"main-container\">\n"
	"\n"
	"        <!-- KANBAN VIEW -->\n"
	"        <div class=\"view-panel active\" id=\"view-kanban\">\n"
	"            <div class=\"project-tabs\" id=\"projectTabs\">\n"
	"                <button class=\"project-tab active\" data-project=\"\" id=\"projectTabAll\">All Projects</button>\n"
	"                <!-- dynamically populated by loadProjects() -->\n"
	"            </div>\n"
	"            <div class=\"toolbar\">\n"
	"                <div class=\"search-box\">\n"
	"                    <input type=\"text\" class=\"search-input\" id=\"kanbanSearch\"\n"
	"                           placeholder=\"Search tickets...\" oninput=\"debouncedFilterKanban()\">\n"
	"                </div>\n"
	"                <div class=\"filters\">\n"
	"                    <select id=\"kanbanPriority\" onchange=\"loadKanban()\">\n"
	"                        <option value=\"\">All priorities</option>\n"
	"                        <option value=\"critical\">Critical</option>\n"
	"                        <option value=\"high\">High</option>\n"
	"                        <option value=\"medium\">Medium</option>\n"
	"                        <option value=\"low\">Low</option>\n"
	"                    </select>\n"
	"                    <select id=\"kanbanType\" onchange=\"loadKanban()\">\n"
	"                        <option value=\"\">All types</option>\n"
	"                        <option value=\"task\">Task</option>\n"
	"                        <option value=\"bug\">Bug</option>\n"
	"                        <option value=\"story\">Story</option>\n"
	"                        <option value=\"epic\">Epic</option>\n"
	"                        <option value=\"research\">Research</option>\n"
	"                        <option value=\"sub-task\">Sub-task</option>\n"
	"                    </select>\n"
	"                    <select id=\"kanbanScope\" onchange=\"loadKanban()\">\n"
	"                        <option value=\"\">All scopes</option>\n"
	"                        <option value=\"work\">Work</option>\n"
	"                        <option value=\"personal\">Personal</option>\n"
	"                    </select>\n"
	"                </div>\n"
	"            </div>\n"
	"            <div class=\"kanban-board\" id=\"kanbanBoard\"></div>\n"
	"        </div>\n"
	"\n"
	"        <!-- LIST VIEW -->\n"
	"        <div class=\"view-panel\" id=\"view-list\">\n"
	"            <div class=\"toolbar\">\n"
	"                <div class=\"search-box\">\n"
	"                    <input type=\"text\" class=\"search-input\" id=\"listSearch\"\n"
	"                           placeholder=\"Search tickets...\" oninput=\"debouncedFilterList()\">\n"
	"                </div>\n"
	"                <div class=\"filters\">\n"
	"                    <select id=\"listStatus\" onchange=\"loadList()\">\n"
	"                        <option value=\"\">All statuses</option>\n"
	"                        <option value=\"backlog\">Backlog</option>\n"
	"                        <option value=\"ready\">Ready</option>\n"
	"                        <option value=\"in_progress\">In Progress</option>\n"
	"                        <option value=\"blocked\">Blocked</option>\n"
	"                        <option value=\"review\">Review</option>\n"
	"                        <option value=\"delegated\">Delegated</option>\n"
	"                        <option value=\"done\">Done</option>\n"
	"                        <option value=\"cancelled\">Cancelled</option>\n"
	"                    </select>\n"
	"                    <select id=\"listPriority\" onchange=\"loadList()\">\n"
	"                        <option value=\"\">All priorities</option>\n"
	"                        <option value=\"critical\">Critical</option>\n"
	"                        <option value=\"high\">High</option>\n"
	"                        <option value=\"medium\">Medium</option>\n"
	"                        <option value=\"low\">Low</option>\n"
	"                    </select>\n"
	"                    <select id=\"listType\" onchange=\"loadList()\">\n"
	"                        <option value=\"\">All types</option>\n"
	"                        <option value=\"task\">Task</option>\n"
	"                        <option value=\"bug\">Bug</option>\n"
	"                        <option value=\"story\">Story</option>\n"
	"                        <option value=\"epic\">Epic</option>\n"
	"                        <option value=\"research\">Research</option>\n"
	"                        <option value=\"sub-task\">Sub-task</option>\n"
	"                    </select>\n"
	"                    <select id=\"listScope\" onchange=\"loadList()\">\n"
	"                        <option value=\"\">All scopes</option>\n"
	"                        <option value=\"work\">Work</option>\n"
	"                        <option value=\"personal\">Personal</option>\n"
	"                    </select>\n"
	"                </div>\n"
	"            </div>\n"
	"            <div class=\"table-container\">\n"
	"                <div class=\"table-scroll\">\n"
	"                    <table>\n"
	"                        <thead>\n"
	"                            <tr>\n"
	"                                <th onclick=\"sortList('id')\">ID</th>\n"
	"                                <th onclick=\"sortList('type')\">Type</th>\n"
	"                                <th onclick=\"sortList('status')\">Status</th>\n"
	"                                <th onclick=\"sortList('priority')\">Priority</th>\n"
	"                                <th onclick=\"sortList('assignee')\">Assignee</th>\n"
	"                                <th onclick=\"sortList('title')\">Title</th>\n"
	"                                <th onclick=\"sortList('progress')\">Progress</th>\n"
	"                                <th onclick=\"sortList('due_date')\">Due</th>\n"
	"                            </tr>\n"
	"                        </thead>\n"
	"                        <tbody id=\"listBody\"></tbody>\n"
	"                    </table>\n"
	"                </div>\n"
	"            </div>\n"
	"        </div>\n"
	"\n"
	"        <!-- DASHBOARD VIEW -->\n"
	"        <div class=\"view-panel\" id=\"view-dashboard\">\n"
	"            <div class=\"dashboard-tabs\">\n"
	"                <button class=\"btn btn-secondary btn-small active\" onclick=\"loadDashboard('daily', this)\">Daily</button>\n"
	"                <button class=\"btn btn-secondary btn-small\" onclick=\"loadDashboard('weekly', this)\">Weekly</button>\n"
	"                <button class=\"btn btn-secondary btn-small\" onclick=\"loadDashboard('sprint', this)\">Sprint</button>\n"
	"                <button class=\"btn btn-secondary btn-small\" onclick=\"loadDashboard('project', this)\">Project</button>\n"
	"                <button class=\"btn btn-secondary btn-small\" onclick=\"loadDashboard('team', this)\">Team</button>\n"
	"            </div>\n"
	"            <div class=\"dashboard-content\" id=\"dashboardContent\"><div class=\"spinner-container\"><div class=\"spinner\"></div>Loading...</div></div>\n"
	"        </div>\n"
	"\n"
	"        <!-- PEOPLE VIEW -->\n"
	"        <div class=\"view-panel\" id=\"view-people\">\n"
	"            <div class=\"toolbar\">\n"
	"                <div class=\"search-box\">\n"
	"                    <input type=\"text\" class=\"search-input\" id=\"peopleSearch\"\n"
	"                           placeholder=\"Search people...\" oninput=\"debouncedFilterPeople()\">\n"
	"                </div>\n"
	"            </div>\n"
	"            <div class=\"people-grid\" id=\"peopleGrid\"></div>\n"
	"        </div>\n"
	"    </div>\n"
	"\n"
	"    <!-- CREATE MODAL -->\n"
	"    <div class=\"modal-overlay\" id=\"createModal\">\n"
	"        <div class=\"modal\">\n"
	"            <div class=\"modal-header\">\n"
	"                <h2>Create Ticket</h2>\n"
	"                <button class=\"modal-close\" onclick=\"closeModal('createModal')\" aria-label=\"Close\">&times;</button>\n"
	"            </div>\n"
	"            <div class=\"modal-body\">\n"
	"                <div class=\"form-row\">\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Type</label>\n"
	"                        <select id=\"createType\">\n"
	"                            <option value=\"task\">Task</option>\n"
	"                            <option value=\"bug\">Bug</option>\n"
	"                            <option value=\"story\">Story</option>\n"
	"                            <option value=\"epic\">Epic</option>\n"
	"                            <option value=\"sub-task\">Sub-task</option>\n"
	"                            <option value=\"research\">Research</option>\n"
	"                            <option value=\"meeting\">Meeting</option>\n"
	"                            <option value=\"journal\">Journal</option>\n"
	"                            <option value=\"recipe\">Recipe</option>\n"
	"                            <option value=\"mentorship\">Mentorship</option>\n"
	"                        </select>\n"
	"                    </div>\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Priority</label>\n"
	"                        <select id=\"createPriority\">\n"
	"                            <option value=\"medium\">Medium</option>\n"
	"                            <option value=\"low\">Low</option>\n"
	"                            <option value=\"high\">High</option>\n"
	"                            <option value=\"critical\">Critical</option>\n"
	"                        </select>\n"
	"                    </div>\n"
	"                </div>\n"
	"                <div class=\"form-group\">\n"
	"                    <label>Title</label>\n"
	"                    <input type=\"text\" id=\"createTitle\" placeholder=\"Ticket title...\">\n"
	"                </div>\n"
	"                <div class=\"form-row\">\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Assignee</label>\n"
	"                        <input type=\"text\" id=\"createAssignee\" placeholder=\"e.g. john_doe\">\n"
	"                    </div>\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Reporter</label>\n"
	"                        <input type=\"text\" id=\"createReporter\" placeholder=\"e.g. jane_doe\">\n"
	"                    </div>\n"
	"                </div>\n"
	"                <div class=\"form-row\">\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Project</label>\n"
	"                        <input type=\"text\" id=\"createProject\" placeholder=\"e.g. myapp\">\n"
	"                    </div>\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Due Date</label>\n"
	"                        <input type=\"text\" id=\"createDue\" placeholder=\"e.g. +7d, 2026-03-01\">\n"
	"                    </div>\n"
	"                </div>\n"
	"                <div class=\"form-row\">\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Tags</label>\n"
	"                        <input type=\"text\" id=\"createTags\" placeholder=\"comma-separated\">\n"
	"                    </div>\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Member Of</label>\n"
	"                        <input type=\"text\" id=\"createMemberOf\" placeholder=\"e.g. PROJ-00001\">\n"
	"                    </div>\n"
	"                </div>\n"
	"                <div class=\"form-row\">\n"
	"                    <div class=\"form-group\">\n"
	"                        <label>Scope</label>\n"
	"                        <select id=\"createScope\">\n"
	"                            <option value=\"\">Default</option>\n"
	"                            <option value=\"personal\">Personal</option>\n"
	"                            <option value=\"work\">Work</option>\n"
	"                        </select>\n"
	"                    </div>\n"
	"                </div>\n"
	"            </div>\n"
	"            <div class=\"modal-footer\">\n"
	"                <button class=\"btn btn-secondary\" onclick=\"closeModal('createModal')\">Cancel</button>\n"
	"                <button class=\"btn btn-primary\" onclick=\"submitCreate()\">Create</button>\n"
	"            </div>\n"
	"        </div>\n"
	"    </div>\n"
	"\n"
	"    <!-- DETAIL MODAL -->\n"
	"    <div class=\"modal-overlay\" id=\"detailModal\">\n"
	"        <div class=\"modal\" style=\"max-width:800px;\">\n"
	"            <div class=\"modal-header\">\n"
	"                <h2 id=\"detailTitle\">Ticket Detail</h2>\n"
	"                <button class=\"modal-close\" onclick=\"closeModal('detailModal')\" aria-label=\"Close\">&times;</button>\n"
	"            </div>\n"
	"            <div class=\"modal-body\" id=\"detailBody\"><div class=\"spinner-container\"><div class=\"spinner\"></div>Loading...</div></div>\n"
	"        </div>\n"
	"    </div>\n"
	"\n"
	"    <!-- TOAST CONTAINER -->\n"
	"    <div class=\"toast-container\" id=\"toastContainer\"></div>\n"
	"\n"
	"    <script>\n"
	"        /* ============================================================ */\n"
	"        /* STATE                                                        */\n"
	"        /* ============================================================ */\n"
	"        let currentView = 'kanban';\n"
	"        let allTickets = [];\n"
	"        let currentProject = localStorage.getItem('vimban_project') || '';\n"
	"        let listSortField = 'id';\n"
	"        let listSortAsc = true;\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* UTILITIES                                                    */\n"
	"        /* ============================================================ */\n"
	"        function toast(msg, type = 'success') {\n"
	"            const container = document.getElementById('toastContainer');\n"
	"            const el = document.createElement('div');\n"
	"            el.className = `toast toast-${type}`;\n"
	"            el.textContent = msg;\n"
	"            container.appendChild(el);\n"
	"            setTimeout(() => el.remove(), 4000);\n"
	"        }\n"
	"\n"
	"        function escapeHtml(str) {\n"
	"            if (!str) return '';\n"
	"            var d=document.createElement('div'); d.appendChild(document.createTextNode(str)); return d.innerHTML;\n"
	"        }\n"
	"\n"
	"        function debounce(fn, ms) {\n"
	"            let t; return function() { clearTimeout(t); t = setTimeout(fn, ms); };\n"
	"        }\n"
	"\n"
	"        const _inflightControllers = new Map();\n"
	"        async function api(url, opts = {}) {\n"
	"            try {\n"
	"                /* Cancel any in-flight GET to the same URL */\n"
	"                const method = (opts.method || 'GET').toUpperCase();\n"
	"                if (method === 'GET' && _inflightControllers.has(url)) {\n"
	"                    _inflightControllers.get(url).abort();\n"
	"                }\n"
	"                const authHeader = collab.token\n"
	"                    ? { 'Authorization': 'Bearer ' + collab.token }\n"
	"                    : {};\n"
	"                const controller = new AbortController();\n"
	"                if (method === 'GET') _inflightControllers.set(url, controller);\n"
	"                const timeout = setTimeout(() => controller.abort(), 15000);\n"
	"                const resp = await fetch(url, {\n"
	"                    headers: { 'Content-Type': 'application/json', ...authHeader },\n"
	"                    signal: controller.signal,\n"
	"                    ...opts\n"
	"                });\n"
	"                clearTimeout(timeout);\n"
	"                _inflightControllers.delete(url);\n"
	"                if (!resp.ok) throw new Error(resp.status + ' ' + resp.statusText);\n"
	"                return await resp.json();\n"
	"            } catch (e) {\n"
	"                if (e.name === 'AbortError') return null; /* silently ignore cancelled requests */\n"
	"                toast('Network error: ' + e.message, 'error');\n"
	"                return null;\n"
	"            }\n"
	"        }\n"
	"\n"
	"        function badgeHtml(value, prefix) {\n"
	"            if (!value) return '';\n"
	"            const cls = prefix + '-' + value.replace(/ /g, '_');\n"
	"            return `<span class=\"badge ${cls}\">${value.replace(/_/g, ' ')}</span>`;\n"
	"        }\n"
	"\n"
	"        function tagsHtml(tags) {\n"
	"            if (!tags || !tags.length) return '';\n"
	"            const arr = Array.isArray(tags) ? tags : String(tags).split(',');\n"
	"            return '<div class=\"tags\">' +\n"
	"                arr.map(t => `<span class=\"tag\">${t.trim()}</span>`).join('') +\n"
	"                '</div>';\n"
	"        }\n"
	"\n"
	"        function progressHtml(val) {\n"
	"            const v = parseInt(val) || 0;\n"
	"            return `<div class=\"progress-bar-bg\"><div class=\"progress-bar-fill\" style=\"width:${v}%\"></div></div>`;\n"
	"        }\n"
	"\n"
	"        function truncate(s, n) {\n"
	"            if (!s) return '';\n"
	"            return s.length > n ? s.substring(0, n) + '...' : s;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* VIEW SWITCHING                                               */\n"
	"        /* ============================================================ */\n"
	"        function switchView(view, evt) {\n"
	"            currentView = view;\n"
	"            document.querySelectorAll('.nav-tab').forEach(t => t.classList.remove('active'));\n"
	"            document.querySelectorAll('.view-panel').forEach(p => p.classList.remove('active'));\n"
	"            const src = (evt && evt.target) || document.querySelector('.nav-tab');\n"
	"            src.classList.add('active');\n"
	"            document.getElementById('view-' + view).classList.add('active');\n"
	"\n"
	"            if (view === 'kanban') { loadProjects(); loadKanban(); }\n"
	"            else if (view === 'list') loadList();\n"
	"            else if (view === 'dashboard') loadDashboard('daily');\n"
	"            else if (view === 'people') loadPeople();\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* STATS                                                        */\n"
	"        /* ============================================================ */\n"
	"        async function loadStats() {\n"
	"            const data = await api('/api/tickets?include_done=false');\n"
	"            if (!data) return;\n"
	"            const tickets = data.tickets || [];\n"
	"            allTickets = tickets;\n"
	"\n"
	"            const counts = {};\n"
	"            tickets.forEach(t => {\n"
	"                const s = t.status || 'unknown';\n"
	"                counts[s] = (counts[s] || 0) + 1;\n"
	"            });\n"
	"\n"
	"            const bar = document.getElementById('statsBar');\n"
	"            bar.innerHTML = `\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${tickets.length}</span><span class=\"stat-label\">Total</span></div>\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${counts.in_progress || 0}</span><span class=\"stat-label\">In Progress</span></div>\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${counts.ready || 0}</span><span class=\"stat-label\">Ready</span></div>\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${counts.blocked || 0}</span><span class=\"stat-label\">Blocked</span></div>\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${counts.review || 0}</span><span class=\"stat-label\">Review</span></div>\n"
	"                <div class=\"stat-item\"><span class=\"stat-value\">${counts.backlog || 0}</span><span class=\"stat-label\">Backlog</span></div>\n"
	"                <div class=\"stat-item\" style=\"margin-left:auto\"><span class=\"stat-label\">Updated ${new Date().toLocaleTimeString()}</span></div>\n"
	"            `;\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* PROJECT TABS                                                 */\n"
	"        /* ============================================================ */\n"
	"        async function loadProjects() {\n"
	"            const data = await api('/api/projects');\n"
	"            if (!data) return;\n"
	"            const bar = document.getElementById('projectTabs');\n"
	"            // Wire up \"All Projects\" button programmatically\n"
	"            const allBtn = document.getElementById('projectTabAll');\n"
	"            allBtn.onclick = () => setProject(allBtn, '');\n"
	"            // Remove all tabs except \"All Projects\"\n"
	"            bar.querySelectorAll('.project-tab:not(#projectTabAll)').forEach(el => el.remove());\n"
	"            data.projects.forEach(p => {\n"
	"                const btn = document.createElement('button');\n"
	"                btn.className = 'project-tab' + (currentProject === p.name ? ' active' : '');\n"
	"                btn.dataset.project = p.name;\n"
	"                btn.onclick = () => setProject(btn, p.name);\n"
	"                const nameNode = document.createTextNode(p.name);\n"
	"                btn.appendChild(nameNode);\n"
	"                if (p.count) {\n"
	"                    const badge = document.createElement('span');\n"
	"                    badge.className = 'badge';\n"
	"                    badge.textContent = p.count;\n"
	"                    btn.appendChild(badge);\n"
	"                }\n"
	"                bar.appendChild(btn);\n"
	"            });\n"
	"            // Ensure \"All\" tab has correct active state\n"
	"            bar.querySelector('[data-project=\"\"]').classList.toggle('active', currentProject === '');\n"
	"            // Restore persisted project if it no longer exists (e.g. all tickets archived)\n"
	"            const exists = currentProject === '' || data.projects.some(p => p.name === currentProject);\n"
	"            if (!exists) { currentProject = ''; localStorage.removeItem('vimban_project'); }\n"
	"        }\n"
	"\n"
	"        function setProject(btn, project) {\n"
	"            currentProject = project;\n"
	"            if (project) localStorage.setItem('vimban_project', project);\n"
	"            else localStorage.removeItem('vimban_project');\n"
	"            document.querySelectorAll('.project-tab').forEach(t => t.classList.remove('active'));\n"
	"            btn.classList.add('active');\n"
	"            loadKanban();\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* KANBAN VIEW                                                  */\n"
	"        /* ============================================================ */\n"
	"        async function loadKanban() {\n"
	"            const priority = document.getElementById('kanbanPriority').value;\n"
	"            const type = document.getElementById('kanbanType').value;\n"
	"            const scope = document.getElementById('kanbanScope').value;\n"
	"\n"
	"            let url = '/api/tickets?include_done=false';\n"
	"            if (priority) url += '&priority=' + priority;\n"
	"            if (type) url += '&type=' + type;\n"
	"            if (scope) url += '&scope=' + scope;\n"
	"            if (currentProject) url += '&project=' + encodeURIComponent(currentProject);\n"
	"\n"
	"            const data = await api(url);\n"
	"            if (!data) return;\n"
	"            const tickets = data.tickets || [];\n"
	"\n"
	"            /* NOTE: these statuses must match STATUSES[] in vimban binary (minus done/cancelled for kanban) */\n"
	"            const columns = ['backlog', 'ready', 'in_progress', 'blocked', 'review', 'delegated'];\n"
	"            const board = document.getElementById('kanbanBoard');\n"
	"\n"
	"            board.innerHTML = columns.map(status => {\n"
	"                const colTickets = tickets.filter(t => t.status === status);\n"
	"                const colorVar = {\n"
	"                    backlog: 'var(--bg-overlay0)',\n"
	"                    ready: 'var(--blue)',\n"
	"                    in_progress: 'var(--yellow)',\n"
	"                    blocked: 'var(--red)',\n"
	"                    review: 'var(--mauve)',\n"
	"                    delegated: 'var(--teal)'\n"
	"                }[status] || 'var(--text-muted)';\n"
	"\n"
	"                const cards = colTickets.map(t => `\n"
	"                    <div class=\"kanban-card\" role=\"button\" tabindex=\"0\" aria-label=\"Ticket ${escapeHtml(t.id)}\" data-ticket-id=\"${escapeHtml(t.id)}\" onclick=\"openDetail(this.dataset.ticketId)\" onkeydown=\"if(event.key==='Enter')openDetail(this.dataset.ticketId)\">\n"
	"                        <div class=\"kanban-card-id\">${t.id}</div>\n"
	"                        <div class=\"kanban-card-title\">${truncate(t.title, 60)}</div>\n"
	"                        <div class=\"kanban-card-meta\">\n"
	"                            ${badgeHtml(t.priority, 'badge')}\n"
	"                            ${t.assignee ? '<span class=\"tag\">' + t.assignee + '</span>' : ''}\n"
	"                            ${t.type ? '<span class=\"tag\">' + t.type + '</span>' : ''}\n"
	"                        </div>\n"
	"                    </div>\n"
	"                `).join('');\n"
	"\n"
	"                return `\n"
	"                    <div class=\"kanban-column\">\n"
	"                        <div class=\"kanban-column-header\">\n"
	"                            <span class=\"kanban-column-title\" style=\"color:${colorVar}\">${status.replace(/_/g, ' ')}</span>\n"
	"                            <span class=\"kanban-column-count\">${colTickets.length}</span>\n"
	"                        </div>\n"
	"                        <div class=\"kanban-column-body\">${cards || '<div class=\"empty-state\"><p>No tickets</p></div>'}</div>\n"
	"                    </div>\n"
	"                `;\n"
	"            }).join('');\n"
	"        }\n"
	"\n"
	"        function filterKanban() {\n"
	"            const q = document.getElementById('kanbanSearch').value.toLowerCase();\n"
	"            document.querySelectorAll('.kanban-card').forEach(card => {\n"
	"                const text = card.textContent.toLowerCase();\n"
	"                card.style.display = text.includes(q) ? '' : 'none';\n"
	"            });\n"
	"        }\n"
	"        const debouncedFilterKanban = debounce(filterKanban, 200);\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* LIST VIEW                                                    */\n"
	"        /* ============================================================ */\n"
	"        let listTickets = [];\n"
	"\n"
	"        async function loadList() {\n"
	"            const status = document.getElementById('listStatus').value;\n"
	"            const priority = document.getElementById('listPriority').value;\n"
	"            const type = document.getElementById('listType').value;\n"
	"            const scope = document.getElementById('listScope').value;\n"
	"\n"
	"            let url = '/api/tickets?';\n"
	"            if (status) url += 'status=' + status + '&';\n"
	"            if (priority) url += 'priority=' + priority + '&';\n"
	"            if (type) url += 'type=' + type + '&';\n"
	"            if (scope) url += 'scope=' + scope + '&';\n"
	"            if (status === 'done' || status === 'cancelled') url += 'include_done=true&';\n"
	"\n"
	"            const data = await api(url);\n"
	"            if (!data) return;\n"
	"            listTickets = data.tickets || [];\n"
	"            renderList();\n"
	"        }\n"
	"\n"
	"        function sortList(field) {\n"
	"            if (listSortField === field) {\n"
	"                listSortAsc = !listSortAsc;\n"
	"            } else {\n"
	"                listSortField = field;\n"
	"                listSortAsc = true;\n"
	"            }\n"
	"            /* Update sort indicators on headers */\n"
	"            document.querySelectorAll('#view-list th').forEach(th => {\n"
	"                th.classList.remove('sorted-asc', 'sorted-desc');\n"
	"            });\n"
	"            const idx = ['id','type','status','priority','assignee','title','progress','due_date'].indexOf(field);\n"
	"            if (idx >= 0) {\n"
	"                const th = document.querySelectorAll('#view-list th')[idx];\n"
	"                if (th) th.classList.add(listSortAsc ? 'sorted-asc' : 'sorted-desc');\n"
	"            }\n"
	"            renderList();\n"
	"        }\n"
	"\n"
	"        function renderList() {\n"
	"            const sorted = [...listTickets].sort((a, b) => {\n"
	"                const va = (a[listSortField] || '').toString().toLowerCase();\n"
	"                const vb = (b[listSortField] || '').toString().toLowerCase();\n"
	"                if (va < vb) return listSortAsc ? -1 : 1;\n"
	"                if (va > vb) return listSortAsc ? 1 : -1;\n"
	"                return 0;\n"
	"            });\n"
	"\n"
	"            const tbody = document.getElementById('listBody');\n"
	"            if (!sorted.length) {\n"
	"                tbody.innerHTML = '<tr><td colspan=\"8\" class=\"empty-state\"><p>No tickets match current filters</p></td></tr>';\n"
	"                return;\n"
	"            }\n"
	"            tbody.innerHTML = sorted.map(t => `\n"
	"                <tr class=\"clickable\" role=\"button\" tabindex=\"0\" aria-label=\"Ticket ${escapeHtml(t.id)}\" data-ticket-id=\"${escapeHtml(t.id)}\" onclick=\"openDetail(this.dataset.ticketId)\" onkeydown=\"if(event.key==='Enter')openDetail(this.dataset.ticketId)\">\n"
	"                    <td class=\"id-cell\">${t.id}</td>\n"
	"                    <td><span class=\"tag\">${t.type || ''}</span></td>\n"
	"                    <td>${badgeHtml(t.status, 'badge')}</td>\n"
	"                    <td>${badgeHtml(t.priority, 'badge')}</td>\n"
	"                    <td>${t.assignee || ''}</td>\n"
	"                    <td class=\"title-cell\">${truncate(t.title, 50)}</td>\n"
	"                    <td>${progressHtml(t.progress)}</td>\n"
	"                    <td class=\"date-cell\">${t.due_date || ''}</td>\n"
	"                </tr>\n"
	"            `).join('');\n"
	"        }\n"
	"\n"
	"        function filterList() {\n"
	"            const q = document.getElementById('listSearch').value.toLowerCase();\n"
	"            document.querySelectorAll('#listBody tr').forEach(row => {\n"
	"                row.style.display = row.textContent.toLowerCase().includes(q) ? '' : 'none';\n"
	"            });\n"
	"        }\n"
	"        const debouncedFilterList = debounce(filterList, 200);\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* DASHBOARD VIEW                                               */\n"
	"        /* ============================================================ */\n"
	"        async function loadDashboard(type, btn) {\n"
	"            if (btn) {\n"
	"                document.querySelectorAll('.dashboard-tabs .btn').forEach(b => b.classList.remove('active'));\n"
	"                btn.classList.add('active');\n"
	"            }\n"
	"            const el = document.getElementById('dashboardContent');\n"
	"            el.innerHTML = '<div class=\"spinner-container\"><div class=\"spinner\"></div>Loading...</div>';\n"
	"            const data = await api('/api/dashboard/' + type);\n"
	"            if (data) {\n"
	"                el.textContent = data.content || 'No data';\n"
	"            }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* PEOPLE VIEW                                                  */\n"
	"        /* ============================================================ */\n"
	"        async function loadPeople() {\n"
	"            const data = await api('/api/people');\n"
	"            if (!data) return;\n"
	"            const people = data.people || [];\n"
	"            const grid = document.getElementById('peopleGrid');\n"
	"\n"
	"            if (!people.length) {\n"
	"                grid.innerHTML = '<div class=\"empty-state\"><div class=\"empty-state-icon\">👥</div><h3>No People</h3><p>No people records found</p></div>';\n"
	"                return;\n"
	"            }\n"
	"\n"
	"            grid.innerHTML = people.map(p => `\n"
	"                <div class=\"person-card\" role=\"button\" tabindex=\"0\" aria-label=\"${escapeHtml(p.name || 'Person')}\" data-person-id=\"${escapeHtml(p.id || p.name)}\" onclick=\"openPersonDetail(this.dataset.personId)\" onkeydown=\"if(event.key==='Enter')openPersonDetail(this.dataset.personId)\">\n"
	"                    <div class=\"person-name\">${p.name || p.title || 'Unknown'}</div>\n"
	"                    <div class=\"person-role\">${p.role || ''}</div>\n"
	"                    ${p.team ? '<span class=\"person-team\">' + p.team + '</span>' : ''}\n"
	"                    ${p.email ? '<div style=\"font-size:12px;color:var(--text-muted);margin-top:6px\">' + p.email + '</div>' : ''}\n"
	"                </div>\n"
	"            `).join('');\n"
	"        }\n"
	"\n"
	"        function filterPeople() {\n"
	"            const q = document.getElementById('peopleSearch').value.toLowerCase();\n"
	"            document.querySelectorAll('.person-card').forEach(card => {\n"
	"                card.style.display = card.textContent.toLowerCase().includes(q) ? '' : 'none';\n"
	"            });\n"
	"        }\n"
	"        const debouncedFilterPeople = debounce(filterPeople, 200);\n"
	"\n"
	"        async function openPersonDetail(id) {\n"
	"            /* re-use the detail modal for person info */\n"
	"            const data = await api('/api/person/' + encodeURIComponent(id));\n"
	"            if (!data || !data.person) { toast('Person not found', 'error'); return; }\n"
	"            showDetailModal(data.person);\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* TICKET DETAIL                                                */\n"
	"        /* ============================================================ */\n"
	"        async function openDetail(id) {\n"
	"            document.getElementById('detailModal').classList.add('active');\n"
	"            document.getElementById('detailBody').innerHTML = '<div class=\"spinner-container\"><div class=\"spinner\"></div>Loading...</div>';\n"
	"            document.getElementById('detailTitle').textContent = id;\n"
	"\n"
	"            const data = await api('/api/ticket/' + encodeURIComponent(id));\n"
	"            if (!data || !data.ticket) {\n"
	"                document.getElementById('detailBody').innerHTML = '<p>Ticket not found</p>';\n"
	"                return;\n"
	"            }\n"
	"            showDetailModal(data.ticket);\n"
	"        }\n"
	"\n"
	"        function showDetailModal(t) {\n"
	"            const modal = document.getElementById('detailModal');\n"
	"            modal.classList.add('active');\n"
	"            modal.dataset.ticketId = t.id || '';\n"
	"            document.getElementById('detailTitle').textContent = t.id || t.title || 'Detail';\n"
	"\n"
	"            const fields = [\n"
	"                ['Type', t.type],\n"
	"                ['Status', t.status],\n"
	"                ['Priority', t.priority],\n"
	"                ['Assignee', t.assignee],\n"
	"                ['Reporter', t.reporter],\n"
	"                ['Project', t.project],\n"
	"                ['Progress', t.progress != null ? t.progress + '%' : ''],\n"
	"                ['Due Date', t.due_date],\n"
	"                ['Created', t.created],\n"
	"                ['Updated', t.updated],\n"
	"                ['Member Of', t.member_of],\n"
	"                ['Tags', Array.isArray(t.tags) ? t.tags.join(', ') : (t.tags || '')],\n"
	"            ].filter(f => f[1]);\n"
	"\n"
	"            const metaHtml = fields.map(f => `\n"
	"                <div class=\"detail-field\">\n"
	"                    <label>${f[0]}</label>\n"
	"                    <div class=\"value\">${f[0] === 'Status' ? badgeHtml(f[1], 'badge') : (f[0] === 'Priority' ? badgeHtml(f[1], 'badge') : f[1])}</div>\n"
	"                </div>\n"
	"            `).join('');\n"
	"\n"
	"            const body = t.body || t.description || '';\n"
	"\n"
	"            /* parse comments from body */\n"
	"            let commentsHtml = '';\n"
	"            if (t.comments && t.comments.length) {\n"
	"                commentsHtml = '<div class=\"comment-list\"><h3 style=\"margin-bottom:8px;font-size:14px;color:var(--text-overlay)\">Comments</h3>' +\n"
	"                    t.comments.map(c => `\n"
	"                        <div class=\"comment\">\n"
	"                            <div class=\"comment-header\">\n"
	"                                <span class=\"comment-author\">${escapeHtml(c.author || 'unknown')}</span>\n"
	"                                <span>${escapeHtml(c.date || '')}</span>\n"
	"                            </div>\n"
	"                            <div class=\"comment-body\">${escapeHtml(c.text || c.body || '')}</div>\n"
	"                        </div>\n"
	"                    `).join('') + '</div>';\n"
	"            }\n"
	"\n"
	"            document.getElementById('detailBody').innerHTML = `\n"
	"                <div class=\"detail-meta\">${metaHtml}</div>\n"
	"                ${body ? '<div class=\"detail-body\">' + escapeHtml(body) + '</div>' : ''}\n"
	"                ${commentsHtml}\n"
	"                <div class=\"comment-form\">\n"
	"                    <input type=\"text\" id=\"commentInput\" placeholder=\"Add a comment...\">\n"
	"                    <button class=\"btn btn-primary btn-small\" data-ticket-id=\"${escapeHtml(t.id)}\" onclick=\"submitComment(this.dataset.ticketId)\">Send</button>\n"
	"                </div>\n"
	"                <div style=\"display:flex;gap:8px;margin-top:16px;flex-wrap:wrap;\">\n"
	"                    <select id=\"moveStatus\" style=\"flex:1\">\n"
	"                        <option value=\"\">Move to...</option>\n"
	"                        ${/* NOTE: must match STATUSES[] in vimban binary */['backlog','ready','in_progress','blocked','review','delegated','done','cancelled']\n"
	"                            .filter(s => s !== t.status)\n"
	"                            .map(s => '<option value=\"' + s + '\">' + s.replace(/_/g,' ') + '</option>')\n"
	"                            .join('')}\n"
	"                    </select>\n"
	"                    <button class=\"btn btn-secondary btn-small\" data-ticket-id=\"${escapeHtml(t.id)}\" onclick=\"moveTicket(this.dataset.ticketId)\">Move</button>\n"
	"                    <select id=\"editPriority\" style=\"flex:1\">\n"
	"                        <option value=\"\">Set priority...</option>\n"
	"                        ${/* NOTE: must match PRIORITIES[] in vimban binary */['critical','high','medium','low']\n"
	"                            .filter(p => p !== t.priority)\n"
	"                            .map(p => '<option value=\"' + p + '\">' + p + '</option>')\n"
	"                            .join('')}\n"
	"                    </select>\n"
	"                    <button class=\"btn btn-secondary btn-small\" data-ticket-id=\"${escapeHtml(t.id)}\" onclick=\"editPriority(this.dataset.ticketId)\">Set</button>\n"
	"                </div>\n"
	"            `;\n"
	"        }\n"
	"\n"
	"        async function submitComment(id) {\n"
	"            const text = document.getElementById('commentInput').value.trim();\n"
	"            if (!text) return;\n"
	"            const data = await api('/api/ticket/' + id + '/comment', {\n"
	"                method: 'POST',\n"
	"                body: JSON.stringify({ text })\n"
	"            });\n"
	"            if (data && data.success) {\n"
	"                toast('Comment added');\n"
	"                document.getElementById('commentInput').value = '';\n"
	"                openDetail(id);\n"
	"            } else {\n"
	"                toast(typeof data?.message === 'string' ? data.message : 'Failed', 'error');\n"
	"            }\n"
	"        }\n"
	"\n"
	"        async function moveTicket(id) {\n"
	"            const status = document.getElementById('moveStatus').value;\n"
	"            if (!status) return;\n"
	"            /* Optimistic update: move card in UI immediately */\n"
	"            const prevTickets = [...allTickets];\n"
	"            const ticket = allTickets.find(t => t.id === id);\n"
	"            if (ticket) ticket.status = status;\n"
	"            closeModal('detailModal');\n"
	"            refreshCurrentView();\n"
	"            const data = await api('/api/ticket/' + id + '/move', {\n"
	"                method: 'POST',\n"
	"                body: JSON.stringify({ status })\n"
	"            });\n"
	"            if (data && data.success) {\n"
	"                toast(`Moved ${id} to ${status}`);\n"
	"            } else {\n"
	"                /* Revert on failure */\n"
	"                allTickets.splice(0, allTickets.length, ...prevTickets);\n"
	"                refreshCurrentView();\n"
	"                toast(typeof data?.message === 'string' ? data.message : 'Failed to move', 'error');\n"
	"            }\n"
	"        }\n"
	"\n"
	"        async function editPriority(id) {\n"
	"            const priority = document.getElementById('editPriority').value;\n"
	"            if (!priority) return;\n"
	"            const data = await api('/api/ticket/' + id + '/edit', {\n"
	"                method: 'POST',\n"
	"                body: JSON.stringify({ priority })\n"
	"            });\n"
	"            if (data && data.success) {\n"
	"                toast('Priority updated');\n"
	"                openDetail(id);\n"
	"            } else {\n"
	"                toast(typeof data?.message === 'string' ? data.message : 'Failed', 'error');\n"
	"            }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* CREATE                                                       */\n"
	"        /* ============================================================ */\n"
	"        function openCreateModal() {\n"
	"            document.getElementById('createModal').classList.add('active');\n"
	"            document.getElementById('createTitle').focus();\n"
	"        }\n"
	"\n"
	"        async function submitCreate() {\n"
	"            const title = document.getElementById('createTitle').value.trim();\n"
	"            if (!title) { toast('Title is required', 'error'); document.getElementById('createTitle').focus(); return; }\n"
	"            if (title.length > 200) { toast('Title too long (max 200 chars)', 'error'); return; }\n"
	"            if (!document.getElementById('createType').value) { toast('Type is required', 'error'); return; }\n"
	"\n"
	"            const payload = {\n"
	"                type: document.getElementById('createType').value,\n"
	"                title,\n"
	"                priority: document.getElementById('createPriority').value,\n"
	"                assignee: document.getElementById('createAssignee').value.trim(),\n"
	"                reporter: document.getElementById('createReporter').value.trim(),\n"
	"                project: document.getElementById('createProject').value.trim(),\n"
	"                due: document.getElementById('createDue').value.trim(),\n"
	"                tags: document.getElementById('createTags').value.trim(),\n"
	"                member_of: document.getElementById('createMemberOf').value.trim(),\n"
	"                scope: document.getElementById('createScope').value,\n"
	"            };\n"
	"\n"
	"            const data = await api('/api/tickets', {\n"
	"                method: 'POST',\n"
	"                body: JSON.stringify(payload)\n"
	"            });\n"
	"\n"
	"            if (data && data.success) {\n"
	"                toast('Created ' + (data.id || 'ticket'));\n"
	"                closeModal('createModal');\n"
	"                /* clear form */\n"
	"                document.getElementById('createTitle').value = '';\n"
	"                document.getElementById('createAssignee').value = '';\n"
	"                document.getElementById('createReporter').value = '';\n"
	"                document.getElementById('createProject').value = '';\n"
	"                document.getElementById('createDue').value = '';\n"
	"                document.getElementById('createTags').value = '';\n"
	"                document.getElementById('createMemberOf').value = '';\n"
	"                refreshCurrentView();\n"
	"            } else {\n"
	"                toast(typeof data?.message === 'string' ? data.message : 'Failed to create', 'error');\n"
	"            }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* GIT SYNC                                                     */\n"
	"        /* ============================================================ */\n"
	"        async function gitSync() {\n"
	"            toast('Syncing...');\n"
	"            const data = await api('/api/sync', { method: 'POST' });\n"
	"            if (data && data.success) {\n"
	"                toast('Sync complete');\n"
	"            } else {\n"
	"                toast(typeof data?.message === 'string' ? data.message : 'Sync failed', 'error');\n"
	"            }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* MODALS                                                       */\n"
	"        /* ============================================================ */\n"
	"        function closeModal(id) {\n"
	"            document.getElementById(id).classList.remove('active');\n"
	"        }\n"
	"\n"
	"        /* close on overlay click (event delegation) */\n"
	"        document.addEventListener('click', e => {\n"
	"            if (e.target.classList.contains('modal-overlay')) e.target.classList.remove('active');\n"
	"        });\n"
	"\n"
	"        /* Escape handled in unified keydown handler below */\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* COLLAB — SSE real-time collaboration                        */\n"
	"        /* ============================================================ */\n"
	"        const collab = {\n"
	"            token: null,\n"
	"            user: null,\n"
	"            es: null,\n"
	"            connected: false,\n"
	"            presence: [],\n"
	"        };\n"
	"\n"
	"        function collabInit() {\n"
	"            collab.token = localStorage.getItem('vimban_collab_token') || '';\n"
	"            if (collab.token) collabConnect();\n"
	"        }\n"
	"\n"
	"        async function collabConnect() {\n"
	"            if (collab.es) { collab.es.close(); collab.es = null; }\n"
	"\n"
	"            /* 1. Hydrate from snapshot before subscribing to event stream */\n"
	"            try {\n"
	"                const snap = await fetch('/api/snapshot', { headers: { 'Authorization': 'Bearer ' + collab.token } });\n"
	"                if (snap.ok) {\n"
	"                    const snapData = await snap.json();\n"
	"                    collab.presence = snapData.presence || [];\n"
	"                    collabRenderPresence();\n"
	"                }\n"
	"            } catch (_) { /* non-fatal — SSE CONNECTED event will carry presence */ }\n"
	"\n"
	"            /* 2. Subscribe to event stream (token-only, no ?user= — server resolves identity) */\n"
	"            const url = '/api/events?token=' + encodeURIComponent(collab.token);\n"
	"            collab.es = new EventSource(url);\n"
	"            collab.es.onmessage = (e) => {\n"
	"                try { collabHandle(JSON.parse(e.data)); } catch (err) { console.warn('SSE parse error:', err, e.data); }\n"
	"            };\n"
	"            collab.es.onopen = () => { collab._retryDelay = 1000; };\n"
	"            collab.es.onerror = () => {\n"
	"                collab.connected = false;\n"
	"                collabRenderPresence();\n"
	"                /* Close broken connection and reconnect with backoff */\n"
	"                if (collab.es && collab.es.readyState === EventSource.CLOSED) {\n"
	"                    collab.es = null;\n"
	"                    const delay = collab._retryDelay || 1000;\n"
	"                    collab._retryDelay = Math.min(delay * 2, 30000);\n"
	"                    setTimeout(() => { if (collab.token) collabConnect(); }, delay);\n"
	"                }\n"
	"            };\n"
	"        }\n"
	"\n"
	"        function collabHandle(msg) {\n"
	"            const { type, data } = msg;\n"
	"            switch (type) {\n"
	"                case 'CONNECTED':\n"
	"                    collab.connected = true;\n"
	"                    collab.user = data.user || collab.user;\n"
	"                    collab.presence = data.presence || [];\n"
	"                    collabRenderPresence();\n"
	"                    break;\n"
	"                case 'PRESENCE_JOIN':\n"
	"                case 'PRESENCE_LEAVE':\n"
	"                    collab.presence = data.presence || [];\n"
	"                    collabRenderPresence();\n"
	"                    break;\n"
	"                case 'TICKET_CREATED':\n"
	"                    if (data.user !== collab.user) {\n"
	"                        toast('+ ' + data.user + ' created ' + data.ticket_id, 'success');\n"
	"                        debouncedRefresh();\n"
	"                    }\n"
	"                    break;\n"
	"                case 'TICKET_UPDATED':\n"
	"                    if (data.user !== collab.user) {\n"
	"                        toast('~ ' + data.user + ' updated ' + data.ticket_id, 'success');\n"
	"                        debouncedRefresh();\n"
	"                    }\n"
	"                    break;\n"
	"                case 'TICKET_COMMENT':\n"
	"                    if (data.user !== collab.user) {\n"
	"                        toast('> ' + data.user + ' commented on ' + data.ticket_id, 'success');\n"
	"                    }\n"
	"                    break;\n"
	"                case 'TICKET_ARCHIVED':\n"
	"                    if (data.user !== collab.user) {\n"
	"                        toast('x ' + data.user + ' archived ' + data.ticket_id, 'success');\n"
	"                        debouncedRefresh();\n"
	"                    }\n"
	"                    break;\n"
	"            }\n"
	"        }\n"
	"\n"
	"        function collabRenderPresence() {\n"
	"            const bar = document.getElementById('presenceBar');\n"
	"            if (!bar) return;\n"
	"            if (!collab.token) {\n"
	"                bar.innerHTML = '';\n"
	"                return;\n"
	"            }\n"
	"            if (!collab.connected || !collab.presence.length) {\n"
	"                bar.innerHTML = '<span class=\"presence-offline\" title=\"Disconnected\">\\u25CF</span>';\n"
	"                return;\n"
	"            }\n"
	"            const dots = collab.presence.map(p =>\n"
	"                '<span class=\"presence-dot\" title=\"' + escapeHtml(p.user) + '\">' +\n"
	"                escapeHtml(p.user.charAt(0).toUpperCase()) + '</span>'\n"
	"            ).join('');\n"
	"            bar.innerHTML = '<span class=\"presence-label\">online:</span>' + dots;\n"
	"        }\n"
	"\n"
	"        function openCollabSettings() {\n"
	"            const token = prompt('API token (leave blank to disconnect):', collab.token || '');\n"
	"            if (token === null) return;\n"
	"            localStorage.setItem('vimban_collab_token', token);\n"
	"            collab.token = token;\n"
	"            collab.user  = null;  /* will be resolved from token by server on CONNECTED */\n"
	"            if (token) {\n"
	"                collabConnect();\n"
	"            } else if (collab.es) {\n"
	"                collab.es.close();\n"
	"                collab.es = null;\n"
	"                collab.connected = false;\n"
	"                collab.presence = [];\n"
	"                collabRenderPresence();\n"
	"            }\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* REFRESH                                                      */\n"
	"        /* ============================================================ */\n"
	"        function refreshCurrentView() {\n"
	"            loadStats();\n"
	"            if (currentView === 'kanban') loadKanban();\n"
	"            else if (currentView === 'list') loadList();\n"
	"            else if (currentView === 'dashboard') loadDashboard('daily');\n"
	"            else if (currentView === 'people') loadPeople();\n"
	"        }\n"
	"\n"
	"        /* Debounced refresh for SSE events to prevent rapid loops */\n"
	"        let _refreshTimer = null;\n"
	"        function debouncedRefresh() {\n"
	"            if (_refreshTimer) clearTimeout(_refreshTimer);\n"
	"            _refreshTimer = setTimeout(() => { _refreshTimer = null; refreshCurrentView(); }, 500);\n"
	"        }\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* KEYBOARD SHORTCUTS                                           */\n"
	"        /* ============================================================ */\n"
	"        document.addEventListener('keydown', e => {\n"
	"            /* Escape always works, even in inputs */\n"
	"            if (e.key === 'Escape') {\n"
	"                document.querySelectorAll('.modal-overlay.active').forEach(m => m.classList.remove('active'));\n"
	"                return;\n"
	"            }\n"
	"            /* skip shortcuts when focused on form elements */\n"
	"            const tag = document.activeElement.tagName;\n"
	"            if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;\n"
	"            if (document.activeElement.isContentEditable) return;\n"
	"\n"
	"            if (e.key === 'c') openCreateModal();\n"
	"            else if (e.key === 'r') refreshCurrentView();\n"
	"            else if (e.key === '1') { document.querySelector('.nav-tab:nth-child(1)').click(); }\n"
	"            else if (e.key === '2') { document.querySelector('.nav-tab:nth-child(2)').click(); }\n"
	"            else if (e.key === '3') { document.querySelector('.nav-tab:nth-child(3)').click(); }\n"
	"            else if (e.key === '4') { document.querySelector('.nav-tab:nth-child(4)').click(); }\n"
	"            else if (e.key === '/') { e.preventDefault(); document.querySelector('.view-panel.active .search-input')?.focus(); }\n"
	"            else if (e.key === 's' && !e.ctrlKey) gitSync();\n"
	"        });\n"
	"\n"
	"        /* ============================================================ */\n"
	"        /* INIT                                                         */\n"
	"        /* ============================================================ */\n"
	"        loadStats();\n"
	"        loadProjects();\n"
	"        loadKanban();\n"
	"        collabInit();\n"
	"    </script>\n"
	"</body>\n"
	"</html>\n"
;


/* ─── GET / — serve embedded HTML UI ──────────────────────────────────── */

static void
handle_index(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	SoupMessageHeaders *hdrs;
	SoupMessageBody    *body;

	(void)server; (void)path; (void)query; (void)user_data;

	/* Handle OPTIONS preflight */
	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	add_cors_headers(msg);
	soup_server_message_set_status(msg, 200, NULL);

	hdrs = soup_server_message_get_response_headers(msg);
	soup_message_headers_set_content_type(hdrs, "text/html; charset=utf-8", NULL);

	body = soup_server_message_get_response_body(msg);
	soup_message_body_append(body, SOUP_MEMORY_STATIC,
	                         EMBEDDED_HTML, strlen(EMBEDDED_HTML));
	soup_message_body_complete(body);
}


/* ─── GET /api/health ────────────────────────────────────────────────── */

static void
handle_health(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	(void)server; (void)path; (void)query; (void)user_data;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	respond_json(msg, 200,
	             "{\"status\":\"ok\",\"version\":\"" VIMBAN_SERVE_VERSION "\"}");
}


/* ─── GET /api/tickets, POST /api/tickets ────────────────────────────── */

static void
handle_tickets(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	const gchar    *method;
	g_autofree gchar *auth_user = NULL;

	(void)server; (void)path; (void)query;

	method = soup_server_message_get_method(msg);

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	if (!check_auth(app, msg, path, &auth_user)) return;

	if (g_strcmp0(method, "GET") == 0) {
		/* Build vimban list -f json [filters...] */
		GPtrArray *args = g_ptr_array_new();

		g_autofree gchar *status_val   = get_query_param(msg, "status");
		g_autofree gchar *type_val     = get_query_param(msg, "type");
		g_autofree gchar *assignee_val = get_query_param(msg, "assignee");
		g_autofree gchar *priority_val = get_query_param(msg, "priority");
		g_autofree gchar *project_val  = get_query_param(msg, "project");
		g_autofree gchar *scope_val    = get_query_param(msg, "scope");
		g_autofree gchar *done_val     = get_query_param(msg, "include_done");

		g_ptr_array_add(args, (gpointer)"list");
		g_ptr_array_add(args, (gpointer)"-f");
		g_ptr_array_add(args, (gpointer)"json");
		if (status_val && *status_val)   { g_ptr_array_add(args, "-s"); g_ptr_array_add(args, status_val); }
		if (type_val && *type_val)       { g_ptr_array_add(args, "-t"); g_ptr_array_add(args, type_val); }
		if (assignee_val && *assignee_val){ g_ptr_array_add(args, "--assignee"); g_ptr_array_add(args, assignee_val); }
		if (priority_val && *priority_val){ g_ptr_array_add(args, "--priority"); g_ptr_array_add(args, priority_val); }
		if (project_val && *project_val) { g_ptr_array_add(args, "-P"); g_ptr_array_add(args, project_val); }
		if (g_strcmp0(scope_val, "work") == 0)     g_ptr_array_add(args, (gpointer)"--work");
		if (g_strcmp0(scope_val, "personal") == 0) g_ptr_array_add(args, (gpointer)"--personal");
		if (g_strcmp0(done_val, "true") == 0)      g_ptr_array_add(args, (gpointer)"--archived");
		g_ptr_array_add(args, NULL);

		{
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, (const gchar * const *)args->pdata,
			                        CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stdout_str = out;
			g_autofree gchar *stderr_str = err_out;
			g_autofree gchar *resp       = NULL;

			if (rc != 0 || !stdout_str || !*stdout_str) {
				respond_json(msg, 200, "{\"tickets\":[]}");
			} else {
				/* HIGH-5: validate stdout JSON before wrapping */
				resp = build_wrapper_json("tickets", stdout_str);
				respond_json(msg, 200, resp);
			}
		}

		g_ptr_array_free(args, TRUE);
		return;
	}

	if (g_strcmp0(method, "POST") == 0) {
		/* LOW-1: require Content-Type: application/json */
		g_autoptr(JsonObject) body = NULL;
		GPtrArray            *args = NULL;
		const gchar          *type_str  = "task";
		const gchar          *title_str = "";
		const gchar          *prio_str  = "medium";

		if (!require_json_content_type(msg))
			return;

		/* Create a new ticket */
		body = parse_request_body(msg);

		if (!body) {
			respond_error(msg, 400, "bad_request", "No data provided");
			return;
		}

		type_str  = json_object_has_member(body, "type")     ? json_object_get_string_member(body, "type")     : "task";
		title_str = json_object_has_member(body, "title")    ? json_object_get_string_member(body, "title")    : "";
		prio_str  = json_object_has_member(body, "priority") ? json_object_get_string_member(body, "priority") : "medium";

		if (!title_str || !*title_str) {
			respond_error(msg, 400, "bad_request", "title is required");
			return;
		}

		/* HIGH-2: validate type and priority against allowlists */
		if (!is_in_allowlist(type_str, TICKET_TYPES)) {
			respond_error(msg, 400, "bad_request", "Invalid ticket type");
			return;
		}
		if (!is_in_allowlist(prio_str, PRIORITIES)) {
			respond_error(msg, 400, "bad_request", "Invalid priority");
			return;
		}

		args = g_ptr_array_new();
		g_ptr_array_add(args, (gpointer)"create");
		g_ptr_array_add(args, (gpointer)type_str);
		g_ptr_array_add(args, (gpointer)title_str);
		g_ptr_array_add(args, (gpointer)"--no-edit");
		g_ptr_array_add(args, (gpointer)"-p");
		g_ptr_array_add(args, (gpointer)prio_str);

		if (json_object_has_member(body, "assignee") && *json_object_get_string_member(body, "assignee")) {
			g_ptr_array_add(args, (gpointer)"--assignee");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "assignee"));
		}
		if (json_object_has_member(body, "reporter") && *json_object_get_string_member(body, "reporter")) {
			g_ptr_array_add(args, (gpointer)"-r");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "reporter"));
		}
		if (json_object_has_member(body, "project") && *json_object_get_string_member(body, "project")) {
			g_ptr_array_add(args, (gpointer)"-P");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "project"));
		}
		if (json_object_has_member(body, "tags") && *json_object_get_string_member(body, "tags")) {
			g_ptr_array_add(args, (gpointer)"-t");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "tags"));
		}
		if (json_object_has_member(body, "due") && *json_object_get_string_member(body, "due")) {
			g_ptr_array_add(args, (gpointer)"--due");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "due"));
		}
		if (json_object_has_member(body, "member_of") && *json_object_get_string_member(body, "member_of")) {
			g_ptr_array_add(args, (gpointer)"-m");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "member_of"));
		}
		if (json_object_has_member(body, "scope")) {
			const gchar *scope = json_object_get_string_member(body, "scope");
			if (g_strcmp0(scope, "work") == 0)     g_ptr_array_add(args, (gpointer)"--work");
			if (g_strcmp0(scope, "personal") == 0) g_ptr_array_add(args, (gpointer)"--personal");
		}
		if (json_object_has_member(body, "prefix") && *json_object_get_string_member(body, "prefix")) {
			g_ptr_array_add(args, (gpointer)"--prefix");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "prefix"));
		}
		g_ptr_array_add(args, NULL);

		{
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, (const gchar * const *)args->pdata,
			                        CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stdout_str = out;
			g_autofree gchar *stderr_str = err_out;

			if (rc != 0) {
				/* CRIT-1: use build_error_json instead of g_strdup_printf */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to create ticket");
				respond_json(msg, 400, emsg);
			} else {
				/* Extract ticket ID from output (e.g. PROJ-00042) */
				GRegex  *re = g_regex_new("([A-Z]+-[0-9]+)", 0, 0, NULL);
				GMatchInfo *mi = NULL;
				gchar   *ticket_id = NULL;

				if (re && g_regex_match(re, stdout_str ? stdout_str : "", 0, &mi))
					ticket_id = g_match_info_fetch(mi, 1);

				if (ticket_id) {
					/* CRIT-1: build response with JsonBuilder */
					{
						g_autoptr(JsonBuilder) rb = json_builder_new();
						g_autoptr(JsonGenerator) rg = json_generator_new();
						g_autoptr(JsonNode) rn = NULL;
						g_autofree gchar *resp = NULL;

						json_builder_begin_object(rb);
						json_builder_set_member_name(rb, "success");
						json_builder_add_boolean_value(rb, TRUE);
						json_builder_set_member_name(rb, "id");
						json_builder_add_string_value(rb, ticket_id);
						json_builder_end_object(rb);

						rn = json_builder_get_root(rb);
						json_generator_set_root(rg, rn);
						resp = json_generator_to_data(rg, NULL);
						respond_json(msg, 200, resp);
					}

					/* CRIT-2: Broadcast creation event with JsonBuilder */
					{
						g_autofree gchar *ev = build_sse_event_json(
							"ticket_id", ticket_id,
							"title", title_str,
							"type", type_str,
							"status", "backlog",
							"user", auth_user ? auth_user : "unknown",
							NULL);
						sse_broadcast(app, "TICKET_CREATED", ev);
					}
				} else {
					respond_json(msg, 200, "{\"success\":true}");
				}

				g_free(ticket_id);
				if (mi) g_match_info_free(mi);
				if (re) g_regex_unref(re);
			}
		}

		g_ptr_array_free(args, TRUE);
		return;
	}

	respond_error(msg, 405, "method_not_allowed", "Method not allowed");
}


/* ─── /api/ticket/<id>[/action] — show, move, edit, comment, link, archive */

static void
handle_ticket_dispatch(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app    = (VimbanServeApp *)user_data;
	const gchar    *method;
	g_autofree gchar *ticket_id = NULL;
	g_autofree gchar *action    = NULL;
	g_autofree gchar *auth_user = NULL;

	(void)server; (void)query;

	method    = soup_server_message_get_method(msg);
	ticket_id = extract_path_segment(path, 2);  /* /api/ticket/<id> */
	action    = extract_path_segment(path, 3);  /* optional: move|edit|comment|link|archive|comments */

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	if (!check_auth(app, msg, path, &auth_user)) return;

	if (!ticket_id) {
		respond_error(msg, 400, "bad_request", "Missing ticket ID");
		return;
	}

	/* HIGH-2: validate ticket ID format */
	if (!is_valid_ticket_id(ticket_id)) {
		respond_error(msg, 400, "bad_request", "Invalid ticket ID format");
		return;
	}

	/* GET /api/ticket/<id> — show single ticket */
	if (g_strcmp0(method, "GET") == 0 && !action) {
		const gchar *argv[] = { "show", ticket_id, "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_autofree gchar *stderr_str = err_out;

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_error(msg, 404, "not_found", "Ticket not found");
			return;
		}
		{
			/* HIGH-5: validate stdout JSON before wrapping */
			g_autofree gchar *resp = build_wrapper_json("ticket", stdout_str);
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* GET /api/ticket/<id>/comments */
	if (g_strcmp0(method, "GET") == 0 && g_strcmp0(action, "comments") == 0) {
		const gchar *argv[] = { "comments", ticket_id, "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_autofree gchar *stderr_str = err_out;

		if (rc != 0) {
			respond_json(msg, 400, "{\"comments\":[]}");
			return;
		}
		{
			/* HIGH-5: validate stdout JSON before wrapping */
			g_autofree gchar *resp = build_wrapper_json("comments",
				stdout_str && *stdout_str ? stdout_str : "[]");
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* POST /api/ticket/<id>/move */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "move") == 0) {
		g_autoptr(JsonObject) body = NULL;
		const gchar *new_status;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body || !json_object_has_member(body, "status")) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "status required");
				respond_json(msg, 400, resp);
			}
			return;
		}
		new_status = json_object_get_string_member(body, "status");

		/* HIGH-2: validate status against allowlist */
		if (!is_in_allowlist(new_status, STATUSES)) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "Invalid status value");
				respond_json(msg, 400, resp);
			}
			return;
		}

		{
			const gchar *argv[] = { "move", ticket_id, new_status, NULL };
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stderr_str = err_out;
			g_autofree gchar *resp = NULL;
			g_free(out);

			if (rc != 0) {
				/* CRIT-1: use build_error_json */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to move ticket");
				respond_json(msg, 400, emsg);
				return;
			}
			{
				/* CRIT-2: build SSE event with JsonBuilder */
				g_autofree gchar *ev = build_sse_event_json(
					"ticket_id", ticket_id,
					"field", "status",
					"value", new_status,
					"user", auth_user ? auth_user : "unknown",
					NULL);
				sse_broadcast(app, "TICKET_UPDATED", ev);
			}
			/* CRIT-1: build success response with JsonBuilder */
			{
				g_autofree gchar *move_msg = g_strdup_printf("Moved %s to %s", ticket_id, new_status);
				resp = build_error_json(TRUE, move_msg);
			}
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* POST /api/ticket/<id>/comment */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "comment") == 0) {
		g_autoptr(JsonObject) body = NULL;
		const gchar *text;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body || !json_object_has_member(body, "text")) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "text required");
				respond_json(msg, 400, resp);
			}
			return;
		}
		text = json_object_get_string_member(body, "text");
		{
			const gchar *argv[] = { "comment", ticket_id, text, NULL };
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stderr_str = err_out;
			g_free(out);

			if (rc != 0) {
				/* CRIT-1: use build_error_json */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to add comment");
				respond_json(msg, 400, emsg);
				return;
			}
			{
				/* CRIT-2: build SSE event with JsonBuilder */
				g_autofree gchar *ev = build_sse_event_json(
					"ticket_id", ticket_id,
					"user", auth_user ? auth_user : "unknown",
					NULL);
				sse_broadcast(app, "TICKET_COMMENT", ev);
			}
			respond_json(msg, 200, "{\"success\":true,\"message\":\"Comment added\"}");
		}
		return;
	}

	/* POST /api/ticket/<id>/edit */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "edit") == 0) {
		g_autoptr(JsonObject) body = NULL;
		GPtrArray            *args = NULL;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "No data provided");
				respond_json(msg, 400, resp);
			}
			return;
		}

		args = g_ptr_array_new();
		g_ptr_array_add(args, (gpointer)"edit");
		g_ptr_array_add(args, (gpointer)ticket_id);

		if (json_object_has_member(body, "priority")) {
			const gchar *p = json_object_get_string_member(body, "priority");
			/* HIGH-2: validate priority */
			if (!is_in_allowlist(p, PRIORITIES)) {
				{
					g_autofree gchar *resp = build_error_json(FALSE, "Invalid priority");
					respond_json(msg, 400, resp);
				}
				g_ptr_array_free(args, TRUE);
				return;
			}
			g_ptr_array_add(args, (gpointer)"--priority");
			g_ptr_array_add(args, (gpointer)p);
		}
		if (json_object_has_member(body, "assignee")) {
			g_ptr_array_add(args, (gpointer)"--assignee");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "assignee"));
		}
		if (json_object_has_member(body, "progress")) {
			g_ptr_array_add(args, (gpointer)"--progress");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "progress"));
		}
		if (json_object_has_member(body, "tags")) {
			g_ptr_array_add(args, (gpointer)"--tags");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "tags"));
		}
		if (json_object_has_member(body, "due_date")) {
			g_ptr_array_add(args, (gpointer)"--due");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "due_date"));
		}
		g_ptr_array_add(args, NULL);

		{
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, (const gchar * const *)args->pdata,
			                        CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stderr_str = err_out;
			g_free(out);

			if (rc != 0) {
				/* CRIT-1: use build_error_json */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to edit ticket");
				respond_json(msg, 400, emsg);
			} else {
				/* CRIT-2: build SSE event with JsonBuilder */
				g_autofree gchar *ev = build_sse_event_json(
					"ticket_id", ticket_id,
					"user", auth_user ? auth_user : "unknown",
					NULL);
				sse_broadcast(app, "TICKET_UPDATED", ev);
				respond_json(msg, 200, "{\"success\":true,\"message\":\"Ticket updated\"}");
			}
		}

		g_ptr_array_free(args, TRUE);
		return;
	}

	/* POST /api/ticket/<id>/archive */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "archive") == 0) {
		const gchar *argv[] = { "archive", ticket_id, NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stderr_str = NULL;

		rc = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		stderr_str = err_out;
		g_free(out);

		if (rc != 0) {
			/* CRIT-1: use build_error_json */
			g_autofree gchar *emsg = build_error_json(FALSE,
				stderr_str ? stderr_str : "Failed to archive");
			respond_json(msg, 400, emsg);
			return;
		}
		{
			/* CRIT-2: build SSE event with JsonBuilder */
			g_autofree gchar *ev = build_sse_event_json(
				"ticket_id", ticket_id,
				"user", auth_user ? auth_user : "unknown",
				NULL);
			sse_broadcast(app, "TICKET_ARCHIVED", ev);
		}
		{
			/* CRIT-1: build success response with JsonBuilder */
			g_autofree gchar *archive_msg = g_strdup_printf("Archived %s", ticket_id);
			g_autofree gchar *resp = build_error_json(TRUE, archive_msg);
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* POST /api/ticket/<id>/link */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "link") == 0) {
		g_autoptr(JsonObject) body = NULL;
		const gchar *target;
		const gchar *link_type;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body || !json_object_has_member(body, "target")) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "target ticket ID required");
				respond_json(msg, 400, resp);
			}
			return;
		}
		target    = json_object_get_string_member(body, "target");
		link_type = json_object_has_member(body, "type")
		                ? json_object_get_string_member(body, "type")
		                : "related";

		/* HIGH-2: validate target ticket ID */
		if (!is_valid_ticket_id(target)) {
			{
				g_autofree gchar *resp = build_error_json(FALSE, "Invalid target ticket ID format");
				respond_json(msg, 400, resp);
			}
			return;
		}

		{
			const gchar *argv[] = { "link", ticket_id, target, "--type", link_type, NULL };
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stderr_str = err_out;
			g_free(out);

			if (rc != 0) {
				/* CRIT-1: use build_error_json */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to link tickets");
				respond_json(msg, 400, emsg);
				return;
			}
			/* CRIT-1: build success response with JsonBuilder */
			{
				g_autofree gchar *link_msg = g_strdup_printf("Linked %s to %s", ticket_id, target);
				g_autofree gchar *resp = build_error_json(TRUE, link_msg);
				respond_json(msg, 200, resp);
			}
		}
		return;
	}

	respond_error(msg, 404, "not_found", "Unknown action");
}


/* ─── GET /api/search ────────────────────────────────────────────────── */

static void
handle_search(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autofree gchar *q_val = NULL;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	q_val = get_query_param(msg, "q");

	/* MED-2: validate search query — length limit and no leading dash */
	if (!q_val || !*q_val) {
		respond_json(msg, 200, "{\"tickets\":[]}");
		return;
	}
	if (strlen(q_val) > 512 || q_val[0] == '-') {
		respond_error(msg, 400, "bad_request", "Invalid search query");
		return;
	}

	{
		const gchar *argv[] = { "search", q_val, "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_free(err_out);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_json(msg, 200, "{\"tickets\":[]}");
			return;
		}
		/* Search may return non-JSON (ripgrep text output) when -f json
		 * is not supported by the search subcommand. Validate first. */
		{
			g_autoptr(JsonParser) sp = json_parser_new();
			if (json_parser_load_from_data(sp, stdout_str, -1, NULL)) {
				g_autofree gchar *resp = build_wrapper_json("tickets", stdout_str);
				respond_json(msg, 200, resp);
			} else {
				/* Non-JSON output — return empty list like Python */
				respond_json(msg, 200, "{\"tickets\":[]}");
			}
		}
	}
}


/* ─── GET /api/dashboard/<type> ──────────────────────────────────────── */

static void
handle_dashboard(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autofree gchar *dash_type = NULL;

	(void)server; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* Extract dashboard type from path: /api/dashboard/<type> */
	dash_type = extract_path_segment(path, 2);
	if (!dash_type || !*dash_type) {
		g_free(dash_type);
		dash_type = g_strdup("daily");
	}

	/* MED-3: validate dashboard type against allowlist */
	if (!is_in_allowlist(dash_type, DASHBOARD_TYPES)) {
		respond_error(msg, 400, "bad_request", "Invalid dashboard type");
		return;
	}

	{
		const gchar *argv[] = { "dashboard", dash_type, NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_free(err_out);

		/* Dashboard outputs plain text; wrap it in a JSON string */
		{
			g_autoptr(JsonBuilder)   b   = json_builder_new();
			g_autoptr(JsonNode)      root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar                   *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "content");
			json_builder_add_string_value(b, (rc == 0 && stdout_str) ? stdout_str : "");
			json_builder_end_object(b);

			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 200, resp);
			g_free(resp);
		}
	}
}


/* ─── GET /api/kanban ────────────────────────────────────────────────── */

static void
handle_kanban(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autofree gchar *scope   = NULL;
	g_autofree gchar *project = NULL;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	scope   = get_query_param(msg, "scope");
	project = get_query_param(msg, "project");

	{
		GPtrArray *args = g_ptr_array_new();
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stdout_str = NULL;

		g_ptr_array_add(args, (gpointer)"list");
		g_ptr_array_add(args, (gpointer)"-f");
		g_ptr_array_add(args, (gpointer)"json");
		if (g_strcmp0(scope, "work") == 0)     g_ptr_array_add(args, (gpointer)"--work");
		if (g_strcmp0(scope, "personal") == 0) g_ptr_array_add(args, (gpointer)"--personal");
		if (project && *project) { g_ptr_array_add(args, (gpointer)"-P"); g_ptr_array_add(args, project); }
		g_ptr_array_add(args, NULL);

		rc = vimban_cmd(app, (const gchar * const *)args->pdata,
		                CMD_TIMEOUT_SEC, &out, &err_out);
		stdout_str = out;
		g_free(err_out);
		g_ptr_array_free(args, TRUE);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_json(msg, 200, "{\"board\":{}}");
			return;
		}

		/*
		 * Parse the JSON ticket array and group by status to produce the
		 * kanban board structure: {"board": {"backlog": [...], ...}}
		 */
		{
			g_autoptr(JsonParser) parser = json_parser_new();
			JsonNode  *root_node = NULL;
			/* INFO-2: use g_free as key-destroy to avoid borrowed pointer issues */
			GHashTable *board = NULL;
			JsonArray *tickets = NULL;
			guint      n;
			gint       s;
			guint      i;
			guint      bi;
			guint      bn;

			if (!json_parser_load_from_data(parser, stdout_str, -1, NULL)) {
				respond_json(msg, 200, "{\"board\":{}}");
				return;
			}

			root_node = json_parser_get_root(parser);
			if (!root_node || !JSON_NODE_HOLDS_ARRAY(root_node)) {
				respond_json(msg, 200, "{\"board\":{}}");
				return;
			}

			/* Build a hash table of status -> JsonArray */
			board = g_hash_table_new_full(g_str_hash, g_str_equal,
			                              g_free, (GDestroyNotify)json_array_unref);

			/* INFO-2: use g_strdup for canonical status keys */
			for (s = 0; STATUSES[s]; s++)
				g_hash_table_insert(board, g_strdup(STATUSES[s]), json_array_new());

			tickets = json_node_get_array(root_node);
			n       = json_array_get_length(tickets);
			for (i = 0; i < n; i++) {
				JsonObject *t          = json_array_get_object_element(tickets, i);
				const gchar *status    = "backlog";
				JsonArray   *bucket    = NULL;

				if (json_object_has_member(t, "status"))
					status = json_object_get_string_member(t, "status");

				bucket = (JsonArray *)g_hash_table_lookup(board, status);
				if (!bucket) {
					bucket = json_array_new();
					/* INFO-2: use g_strdup for unknown status keys */
					g_hash_table_insert(board, g_strdup(status), bucket);
				}
				json_array_add_object_element(bucket, json_object_ref(t));
			}

			/* Serialise to {"board": {...}} */
			{
				g_autoptr(JsonBuilder)   b   = json_builder_new();
				g_autoptr(JsonNode)      out_root = NULL;
				g_autoptr(JsonGenerator) gen = json_generator_new();
				gchar                   *resp = NULL;

				json_builder_begin_object(b);
				json_builder_set_member_name(b, "board");
				json_builder_begin_object(b);
				for (s = 0; STATUSES[s]; s++) {
					JsonArray *bucket = (JsonArray *)g_hash_table_lookup(board, STATUSES[s]);
					json_builder_set_member_name(b, STATUSES[s]);
					if (bucket) {
						json_builder_begin_array(b);
						bn = json_array_get_length(bucket);
						for (bi = 0; bi < bn; bi++) {
							JsonObject *to   = json_array_get_object_element(bucket, bi);
							JsonNode   *node = json_node_alloc();
							json_node_init_object(node, to);
							json_builder_add_value(b, node);
						}
						json_builder_end_array(b);
					} else {
						json_builder_begin_array(b);
						json_builder_end_array(b);
					}
				}
				json_builder_end_object(b);
				json_builder_end_object(b);

				out_root = json_builder_get_root(b);
				json_generator_set_root(gen, out_root);
				resp = json_generator_to_data(gen, NULL);
				respond_json(msg, 200, resp);
				g_free(resp);
			}

			g_hash_table_destroy(board);
		}
	}
}


/* ─── GET /api/people, POST /api/people ──────────────────────────────── */

static void
handle_people(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	{
		const gchar *argv[] = { "people", "list", "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_free(err_out);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_json(msg, 200, "{\"people\":[]}");
			return;
		}
		/* HIGH-5: validate stdout JSON before wrapping */
		{
			g_autofree gchar *resp = build_wrapper_json("people", stdout_str);
			respond_json(msg, 200, resp);
		}
	}
}


/* ─── /api/person/<name>[/action] — show, create ─────────────────────── */

static void
handle_person_dispatch(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	const gchar    *method;
	g_autofree gchar *name = NULL;

	(void)server; (void)query;

	method = soup_server_message_get_method(msg);
	name   = extract_path_segment(path, 2); /* /api/person/<name> */

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* GET /api/person/<name> — show person */
	if (g_strcmp0(method, "GET") == 0) {
		const gchar *argv[6];
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stdout_str = NULL;

		if (!name || !*name) {
			respond_error(msg, 400, "bad_request", "name required");
			return;
		}

		/* HIGH-2: reject names starting with dash */
		if (name[0] == '-') {
			respond_error(msg, 400, "bad_request", "Invalid person name");
			return;
		}

		argv[0] = "people";
		argv[1] = "show";
		argv[2] = name;
		argv[3] = "-f";
		argv[4] = "json";
		argv[5] = NULL;

		rc = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		stdout_str = out;
		g_free(err_out);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_error(msg, 404, "not_found", "Person not found");
			return;
		}
		/* HIGH-5: validate stdout JSON before wrapping */
		{
			g_autofree gchar *resp = build_wrapper_json("person", stdout_str);
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* POST /api/person — create a new person */
	if (g_strcmp0(method, "POST") == 0) {
		g_autoptr(JsonObject) body = NULL;
		const gchar *person_name;
		GPtrArray *args;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body || !json_object_has_member(body, "name")) {
			respond_error(msg, 400, "bad_request", "name required");
			return;
		}
		person_name = json_object_get_string_member(body, "name");

		/* HIGH-2: reject names starting with dash */
		if (person_name && person_name[0] == '-') {
			respond_error(msg, 400, "bad_request", "Invalid person name");
			return;
		}

		args = g_ptr_array_new();
		g_ptr_array_add(args, (gpointer)"people");
		g_ptr_array_add(args, (gpointer)"create");
		g_ptr_array_add(args, (gpointer)person_name);
		g_ptr_array_add(args, (gpointer)"--no-edit");

		if (json_object_has_member(body, "role") && *json_object_get_string_member(body, "role")) {
			g_ptr_array_add(args, (gpointer)"--role");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "role"));
		}
		if (json_object_has_member(body, "team") && *json_object_get_string_member(body, "team")) {
			g_ptr_array_add(args, (gpointer)"--team");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "team"));
		}
		g_ptr_array_add(args, NULL);

		{
			gchar *out = NULL, *err_out = NULL;
			gint   rc  = vimban_cmd(app, (const gchar * const *)args->pdata,
			                        CMD_TIMEOUT_SEC, &out, &err_out);
			g_autofree gchar *stderr_str = err_out;
			g_free(out);

			if (rc != 0) {
				/* CRIT-1: use build_error_json */
				g_autofree gchar *emsg = build_error_json(FALSE,
					stderr_str ? stderr_str : "Failed to create person");
				respond_json(msg, 400, emsg);
			} else {
				respond_json(msg, 200, "{\"success\":true}");
			}
		}

		g_ptr_array_free(args, TRUE);
		return;
	}

	respond_error(msg, 405, "method_not_allowed", "Method not allowed");
}


/* ─── GET /api/projects ──────────────────────────────────────────────── */

static void
handle_projects(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* Fetch all open tickets and extract unique project keys with counts */
	{
		const gchar *argv[] = { "list", "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_free(err_out);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_json(msg, 200, "{\"projects\":[]}");
			return;
		}

		{
			g_autoptr(JsonParser) parser = json_parser_new();
			JsonNode *root_node;
			GHashTable *counts;
			JsonArray  *tix;
			guint       n;
			guint       i;

			if (!json_parser_load_from_data(parser, stdout_str, -1, NULL)) {
				respond_json(msg, 200, "{\"projects\":[]}");
				return;
			}

			root_node = json_parser_get_root(parser);
			if (!root_node || !JSON_NODE_HOLDS_ARRAY(root_node)) {
				respond_json(msg, 200, "{\"projects\":[]}");
				return;
			}

			/* Count tickets per project */
			counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
			tix    = json_node_get_array(root_node);
			n      = json_array_get_length(tix);

			for (i = 0; i < n; i++) {
				JsonObject  *t    = json_array_get_object_element(tix, i);
				const gchar *proj = NULL;

				if (json_object_has_member(t, "project"))
					proj = json_object_get_string_member(t, "project");

				if (proj && *proj) {
					gpointer old_val = g_hash_table_lookup(counts, proj);
					gint cnt = GPOINTER_TO_INT(old_val) + 1;
					g_hash_table_insert(counts, g_strdup(proj), GINT_TO_POINTER(cnt));
				}
			}

			/* Serialise sorted project list */
			{
				g_autoptr(JsonBuilder)   b    = json_builder_new();
				g_autoptr(JsonNode)      out_root = NULL;
				g_autoptr(JsonGenerator) gen  = json_generator_new();
				GList                   *keys = g_list_sort(g_hash_table_get_keys(counts),
				                                            (GCompareFunc)g_strcmp0);
				GList *l   = NULL;
				gchar *resp = NULL;

				json_builder_begin_object(b);
				json_builder_set_member_name(b, "projects");
				json_builder_begin_array(b);
				for (l = keys; l; l = l->next) {
					const gchar *pname = (const gchar *)l->data;
					gint         cnt   = GPOINTER_TO_INT(g_hash_table_lookup(counts, pname));
					json_builder_begin_object(b);
					json_builder_set_member_name(b, "name");
					json_builder_add_string_value(b, pname);
					json_builder_set_member_name(b, "count");
					json_builder_add_int_value(b, cnt);
					json_builder_end_object(b);
				}
				json_builder_end_array(b);
				json_builder_end_object(b);

				out_root = json_builder_get_root(b);
				json_generator_set_root(gen, out_root);
				resp = json_generator_to_data(gen, NULL);
				respond_json(msg, 200, resp);
				g_free(resp);

				g_list_free(keys);
			}

			g_hash_table_destroy(counts);
		}
	}
}


/* ─── POST /api/commit ───────────────────────────────────────────────── */

static void
handle_commit(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autoptr(JsonObject) body = NULL;
	gboolean              do_pull = FALSE;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* LOW-1: require Content-Type for POST if body present */
	if (g_strcmp0(soup_server_message_get_method(msg), "POST") == 0) {
		SoupMessageBody *req_body = soup_server_message_get_request_body(msg);
		if (req_body) {
			g_autoptr(GBytes) flat = soup_message_body_flatten(req_body);
			if (req_body->data && req_body->length > 0) {
				if (!require_json_content_type(msg))
					return;
			}
		}
	}

	body = parse_request_body(msg);

	if (body && json_object_has_member(body, "pull"))
		do_pull = json_object_get_boolean_member(body, "pull");

	{
		GPtrArray *args = g_ptr_array_new();
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stdout_str = NULL;
		g_autofree gchar *stderr_str = NULL;

		g_ptr_array_add(args, (gpointer)"commit");
		if (do_pull)
			g_ptr_array_add(args, (gpointer)"--pull");
		g_ptr_array_add(args, NULL);

		rc = vimban_cmd(app, (const gchar * const *)args->pdata,
		                CMD_TIMEOUT_LONG_SEC, &out, &err_out);
		stdout_str = out;
		stderr_str = err_out;
		g_ptr_array_free(args, TRUE);

		if (rc != 0) {
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "success"); json_builder_add_boolean_value(b, FALSE);
			json_builder_set_member_name(b, "message");
			json_builder_add_string_value(b, stderr_str && *stderr_str ? stderr_str : "Commit failed");
			json_builder_end_object(b);
			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 400, resp);
			g_free(resp);
			return;
		}

		{
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "success"); json_builder_add_boolean_value(b, TRUE);
			json_builder_set_member_name(b, "message");
			json_builder_add_string_value(b,
				stdout_str && *stdout_str ? stdout_str : "Committed");
			json_builder_end_object(b);
			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 200, resp);
			g_free(resp);
		}
	}
}


/* ─── GET /api/validate ──────────────────────────────────────────────── */

static void
handle_validate(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	{
		const gchar *argv[] = { "validate", "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_autofree gchar *stderr_str = err_out;

		if (rc != 0) {
			/* Return errors as content even on non-zero exit */
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "valid"); json_builder_add_boolean_value(b, FALSE);
			json_builder_set_member_name(b, "output");
			json_builder_add_string_value(b, stdout_str ? stdout_str : "");
			json_builder_set_member_name(b, "error");
			json_builder_add_string_value(b, stderr_str ? stderr_str : "");
			json_builder_end_object(b);
			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 200, resp);
			g_free(resp);
			return;
		}

		{
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "valid"); json_builder_add_boolean_value(b, TRUE);
			json_builder_set_member_name(b, "output");
			json_builder_add_string_value(b, stdout_str ? stdout_str : "");
			json_builder_end_object(b);
			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 200, resp);
			g_free(resp);
		}
	}
}


/* ─── GET /api/presence ──────────────────────────────────────────────── */

static void
handle_presence(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp           *app  = (VimbanServeApp *)user_data;
	GHashTable               *seen = NULL;
	g_autoptr(JsonBuilder)    b    = json_builder_new();
	g_autoptr(JsonNode)       root = NULL;
	g_autoptr(JsonGenerator)  gen  = json_generator_new();
	gchar                    *resp = NULL;
	guint                     i;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	/* Build unique user list from sse_clients */
	seen = g_hash_table_new(g_str_hash, g_str_equal);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "presence");
	json_builder_begin_array(b);

	for (i = 0; i < app->sse_clients->len; i++) {
		SseClient   *c    = (SseClient *)app->sse_clients->pdata[i];
		if (g_hash_table_contains(seen, c->user)) continue;
		g_hash_table_add(seen, c->user);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "user");
		json_builder_add_string_value(b, c->user);
		json_builder_set_member_name(b, "connected_at");
		json_builder_add_int_value(b, c->connected_at);
		if (c->viewing) {
			json_builder_set_member_name(b, "viewing");
			json_builder_add_string_value(b, c->viewing);
		}
		json_builder_end_object(b);
	}

	json_builder_end_array(b);
	json_builder_set_member_name(b, "count");
	json_builder_add_int_value(b, (gint64)g_hash_table_size(seen));
	json_builder_end_object(b);

	g_hash_table_destroy(seen);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	resp = json_generator_to_data(gen, NULL);
	respond_json(msg, 200, resp);
	g_free(resp);
}


/* ─── GET /api/events — SSE stream ───────────────────────────────────── */

/*
 * build_presence_list() — build a JSON array node of currently connected users.
 *
 * @app: Application state (owns sse_clients array).
 *
 * Returns: (transfer full) JsonNode* array. Caller must json_node_unref().
 */
static JsonNode *
build_presence_list(
	VimbanServeApp *app
){
	JsonBuilder *b;
	JsonNode    *node;
	guint        i;

	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < app->sse_clients->len; i++) {
		SseClient *c = g_ptr_array_index(app->sse_clients, i);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "user");
		json_builder_add_string_value(b, c->user);
		json_builder_set_member_name(b, "viewing");
		json_builder_add_string_value(b, c->viewing ? c->viewing : "");
		json_builder_set_member_name(b, "connected_at");
		json_builder_add_int_value(b, c->connected_at);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	node = json_builder_get_root(b);
	g_object_unref(b);
	return node;
}

/*
 * on_sse_client_disconnect() — GObject weak-ref callback fires when the
 * underlying SoupServerMessage is finalised (client disconnected).
 *
 * Removes the client from app->sse_clients and broadcasts PRESENCE_LEAVE.
 */
static void
on_sse_client_disconnect(
	SoupServerMessage *msg,
	gpointer           user_data
){
	VimbanServeApp   *app          = (VimbanServeApp *)user_data;
	SseClient        *found        = NULL;
	g_autofree gchar *leaving_user = NULL;
	guint             i;

	for (i = 0; i < app->sse_clients->len; i++) {
		SseClient *c = (SseClient *)app->sse_clients->pdata[i];
		if (c->msg == msg) {
			leaving_user = g_strdup(c->user);
			found = c;
			break;
		}
	}

	if (found) {
		g_ptr_array_remove(app->sse_clients, found);
		if (leaving_user) {
			/* Build PRESENCE_LEAVE event with presence list */
			g_autoptr(JsonBuilder) eb = json_builder_new();
			g_autoptr(JsonGenerator) eg = json_generator_new();
			g_autoptr(JsonNode) eroot = NULL;
			g_autofree gchar *ev_data = NULL;

			json_builder_begin_object(eb);
			json_builder_set_member_name(eb, "user");
			json_builder_add_string_value(eb, leaving_user);
			json_builder_set_member_name(eb, "presence");
			json_builder_add_value(eb, build_presence_list(app));
			json_builder_end_object(eb);

			eroot = json_builder_get_root(eb);
			json_generator_set_root(eg, eroot);
			ev_data = json_generator_to_data(eg, NULL);
			sse_broadcast(app, "PRESENCE_LEAVE", ev_data);
		}
	}

	(void)msg;
}

static void
handle_events(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp     *app    = (VimbanServeApp *)user_data;
	SoupMessageHeaders *hdrs;
	SoupMessageBody    *body;
	g_autofree gchar   *user   = NULL;
	SseClient          *client = NULL;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	/* Auth: check header OR query param */
	if (!check_auth(app, msg, "/api/events", &user)) return;
	if (!user) user = g_strdup("unknown");

	/* HIGH-4: cap on concurrent SSE connections */
	if (app->sse_clients->len >= SSE_MAX_CLIENTS) {
		soup_server_message_set_status(msg, 503, "Too Many Connections");
		return;
	}

	/* Set SSE response headers */
	add_cors_headers(msg);
	soup_server_message_set_status(msg, 200, NULL);
	hdrs = soup_server_message_get_response_headers(msg);
	soup_message_headers_replace(hdrs, "Content-Type",     "text/event-stream");
	soup_message_headers_replace(hdrs, "Cache-Control",    "no-cache");
	soup_message_headers_replace(hdrs, "X-Accel-Buffering","no");
	soup_message_headers_replace(hdrs, "Connection",       "keep-alive");

	/* Register this client */
	client = sse_client_new(user, msg);
	g_ptr_array_add(app->sse_clients, client);

	/* Disconnect signal cleans up when the request ends */
	g_signal_connect(msg, "finished",
	                 G_CALLBACK(on_sse_client_disconnect), app);

	/* Pause the message — we will write chunks manually */
	soup_server_message_pause(msg);

	/* Send initial CONNECTED frame with presence list */
	{
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) root = NULL;
		g_autoptr(JsonGenerator) gen = json_generator_new();
		gchar *payload = NULL;
		g_autofree gchar *frame = NULL;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "CONNECTED");
		json_builder_set_member_name(b, "data");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "client_id");
		json_builder_add_string_value(b, client->id);
		json_builder_set_member_name(b, "user");
		json_builder_add_string_value(b, user);
		json_builder_set_member_name(b, "presence");
		json_builder_add_value(b, build_presence_list(app));
		json_builder_end_object(b);
		json_builder_set_member_name(b, "ts");
		json_builder_add_int_value(b, g_get_real_time() / G_USEC_PER_SEC);
		json_builder_end_object(b);

		root = json_builder_get_root(b);
		json_generator_set_root(gen, root);
		payload = json_generator_to_data(gen, NULL);

		frame = g_strdup_printf("data: %s\n\n", payload);
		body = soup_server_message_get_response_body(msg);
		soup_message_body_append(body, SOUP_MEMORY_COPY, frame, strlen(frame));
		soup_server_message_unpause(msg);
		g_free(payload);
	}

	/* Broadcast new presence with presence list */
	{
		g_autoptr(JsonBuilder) eb = json_builder_new();
		g_autoptr(JsonGenerator) eg = json_generator_new();
		g_autoptr(JsonNode) eroot = NULL;
		g_autofree gchar *ev_data = NULL;

		json_builder_begin_object(eb);
		json_builder_set_member_name(eb, "user");
		json_builder_add_string_value(eb, user);
		json_builder_set_member_name(eb, "presence");
		json_builder_add_value(eb, build_presence_list(app));
		json_builder_end_object(eb);

		eroot = json_builder_get_root(eb);
		json_generator_set_root(eg, eroot);
		ev_data = json_generator_to_data(eg, NULL);
		sse_broadcast(app, "PRESENCE_JOIN", ev_data);
	}
}


/* ─── /api/mentor — mentor meeting management ───────────────────────── */

static void
handle_mentor(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	const gchar    *method;
	g_autofree gchar *action = NULL;

	(void)server; (void)query;

	method = soup_server_message_get_method(msg);
	action = extract_path_segment(path, 2); /* /api/mentor/<action> */

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* GET /api/mentor/list */
	if (g_strcmp0(method, "GET") == 0 && g_strcmp0(action, "list") == 0) {
		g_autofree gchar *person_val = get_query_param(msg, "person");
		GPtrArray        *args       = g_ptr_array_new();
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stdout_str = NULL;

		g_ptr_array_add(args, (gpointer)"mentor");
		g_ptr_array_add(args, (gpointer)"list");
		g_ptr_array_add(args, (gpointer)"-f");
		g_ptr_array_add(args, (gpointer)"json");
		if (person_val && *person_val) {
			/* HIGH-2: reject leading dashes */
			if (person_val[0] == '-') {
				respond_error(msg, 400, "bad_request", "Invalid person name");
				g_ptr_array_free(args, TRUE);
				return;
			}
			g_ptr_array_add(args, (gpointer)"--person");
			g_ptr_array_add(args, (gpointer)person_val);
		}
		g_ptr_array_add(args, NULL);

		rc = vimban_cmd(app, (const gchar * const *)args->pdata,
		                CMD_TIMEOUT_SEC, &out, &err_out);
		stdout_str = out;
		g_free(err_out);
		g_ptr_array_free(args, TRUE);

		if (rc != 0 || !stdout_str || !*stdout_str) {
			respond_json(msg, 200, "{\"meetings\":[]}");
			return;
		}
		/* HIGH-5: validate stdout JSON before wrapping */
		{
			g_autofree gchar *resp = build_wrapper_json("meetings", stdout_str);
			respond_json(msg, 200, resp);
		}
		return;
	}

	/* POST /api/mentor/new */
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(action, "new") == 0) {
		g_autoptr(JsonObject) body = NULL;
		const gchar *person;
		GPtrArray *args;
		gchar *out = NULL, *err_out = NULL;
		gint   rc;
		g_autofree gchar *stderr_str = NULL;

		/* LOW-1: require Content-Type: application/json */
		if (!require_json_content_type(msg))
			return;

		body = parse_request_body(msg);

		if (!body || !json_object_has_member(body, "person")) {
			respond_error(msg, 400, "bad_request", "person required");
			return;
		}
		person = json_object_get_string_member(body, "person");

		/* HIGH-2: reject leading dashes */
		if (person && person[0] == '-') {
			respond_error(msg, 400, "bad_request", "Invalid person name");
			return;
		}

		args = g_ptr_array_new();
		g_ptr_array_add(args, (gpointer)"mentor");
		g_ptr_array_add(args, (gpointer)"new");
		g_ptr_array_add(args, (gpointer)person);
		g_ptr_array_add(args, (gpointer)"--no-edit");

		if (json_object_has_member(body, "date") && *json_object_get_string_member(body, "date")) {
			g_ptr_array_add(args, (gpointer)"--date");
			g_ptr_array_add(args, (gpointer)json_object_get_string_member(body, "date"));
		}
		if (json_object_has_member(body, "mentored_by")
		    && json_object_get_boolean_member(body, "mentored_by"))
		{
			g_ptr_array_add(args, (gpointer)"--mentored-by");
		}
		g_ptr_array_add(args, NULL);

		rc = vimban_cmd(app, (const gchar * const *)args->pdata,
		                CMD_TIMEOUT_SEC, &out, &err_out);
		stderr_str = err_out;
		g_free(out);
		g_ptr_array_free(args, TRUE);

		if (rc != 0) {
			/* CRIT-1: use build_error_json */
			g_autofree gchar *emsg = build_error_json(FALSE,
				stderr_str ? stderr_str : "Failed to create mentor meeting");
			respond_json(msg, 400, emsg);
			return;
		}
		respond_json(msg, 200, "{\"success\":true}");
		return;
	}

	respond_error(msg, 404, "not_found", "Unknown mentor action");
}


/* ─── GET /api/generate-link ─────────────────────────────────────────── */

static void
handle_generate_link(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autofree gchar *ref = NULL;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	ref = get_query_param(msg, "ref");
	if (!ref || !*ref) {
		respond_error(msg, 400, "bad_request", "ref parameter required");
		return;
	}

	/* MED-5: validate ref — length limit and no leading dash */
	if (strlen(ref) > 512 || ref[0] == '-') {
		respond_error(msg, 400, "bad_request", "Invalid ref parameter");
		return;
	}

	{
		const gchar *argv[] = { "generate-link", ref, NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_autofree gchar *stderr_str = err_out;

		{
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "link");
			json_builder_add_string_value(b,
				(rc == 0 && stdout_str) ? g_strstrip(stdout_str) : "");
			if (rc != 0) {
				json_builder_set_member_name(b, "error");
				json_builder_add_string_value(b, stderr_str ? stderr_str : "");
			}
			json_builder_end_object(b);

			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, rc == 0 ? 200 : 400, resp);
			g_free(resp);
		}
	}
}


/* ─── POST /api/convert ──────────────────────────────────────────────── */

static void
handle_convert(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autoptr(JsonObject) body = NULL;
	GPtrArray *args;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	/* LOW-1: require Content-Type for POST if body present */
	if (g_strcmp0(soup_server_message_get_method(msg), "POST") == 0) {
		SoupMessageBody *req_body = soup_server_message_get_request_body(msg);
		if (req_body) {
			g_autoptr(GBytes) flat = soup_message_body_flatten(req_body);
			if (req_body->data && req_body->length > 0) {
				if (!require_json_content_type(msg))
					return;
			}
		}
	}

	body = parse_request_body(msg);

	/* Defaults: find-missing with no filter flags = scan all */
	args = g_ptr_array_new();
	g_ptr_array_add(args, (gpointer)"convert");
	g_ptr_array_add(args, (gpointer)"find-missing");

	if (body) {
		if (json_object_has_member(body, "areas")     && json_object_get_boolean_member(body, "areas"))
			g_ptr_array_add(args, (gpointer)"--areas");
		if (json_object_has_member(body, "resources") && json_object_get_boolean_member(body, "resources"))
			g_ptr_array_add(args, (gpointer)"--resources");
		if (json_object_has_member(body, "meetings")  && json_object_get_boolean_member(body, "meetings"))
			g_ptr_array_add(args, (gpointer)"--meetings");
		if (json_object_has_member(body, "journals")  && json_object_get_boolean_member(body, "journals"))
			g_ptr_array_add(args, (gpointer)"--journals");
		if (json_object_has_member(body, "recipes")   && json_object_get_boolean_member(body, "recipes"))
			g_ptr_array_add(args, (gpointer)"--recipes");
		if (json_object_has_member(body, "people")    && json_object_get_boolean_member(body, "people"))
			g_ptr_array_add(args, (gpointer)"--people");
		if (json_object_has_member(body, "dry_run")   && json_object_get_boolean_member(body, "dry_run"))
			g_ptr_array_add(args, (gpointer)"--dry-run");
	}
	g_ptr_array_add(args, NULL);

	{
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, (const gchar * const *)args->pdata,
		                        CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_autofree gchar *stderr_str = err_out;

		{
			g_autoptr(JsonBuilder) b = json_builder_new();
			g_autoptr(JsonNode) root = NULL;
			g_autoptr(JsonGenerator) gen = json_generator_new();
			gchar *resp = NULL;

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "success");
			json_builder_add_boolean_value(b, rc == 0);
			json_builder_set_member_name(b, "output");
			json_builder_add_string_value(b, stdout_str ? stdout_str : "");
			if (rc != 0) {
				json_builder_set_member_name(b, "error");
				json_builder_add_string_value(b, stderr_str ? stderr_str : "");
			}
			json_builder_end_object(b);

			root = json_builder_get_root(b);
			json_generator_set_root(gen, root);
			resp = json_generator_to_data(gen, NULL);
			respond_json(msg, rc == 0 ? 200 : 400, resp);
			g_free(resp);
		}
	}

	g_ptr_array_free(args, TRUE);
}


/* ─── GET /api/report ────────────────────────────────────────────────── */

static void
handle_report(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	g_autofree gchar *report_type = NULL;

	(void)server; (void)path; (void)query;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	report_type = get_query_param(msg, "type");
	if (!report_type || !*report_type) {
		g_free(report_type);
		report_type = g_strdup("summary");
	}

	/* MED-4: validate report type against allowlist */
	if (!is_in_allowlist(report_type, REPORT_TYPES)) {
		respond_error(msg, 400, "bad_request", "Invalid report type");
		return;
	}

	{
		const gchar *argv_json[] = { "report", report_type, "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint   rc  = vimban_cmd(app, argv_json, CMD_TIMEOUT_SEC, &out, &err_out);
		g_autofree gchar *stdout_str = out;
		g_free(err_out);

		if (rc == 0 && stdout_str && *stdout_str) {
			/* HIGH-5: validate stdout JSON before wrapping */
			g_autoptr(JsonParser) vp = json_parser_new();
			if (json_parser_load_from_data(vp, stdout_str, -1, NULL)) {
				g_autoptr(JsonBuilder) rb = json_builder_new();
				g_autoptr(JsonGenerator) rg = json_generator_new();
				g_autoptr(JsonNode) rn = NULL;
				JsonNode *parsed = json_parser_get_root(vp);
				g_autofree gchar *resp = NULL;

				json_builder_begin_object(rb);
				json_builder_set_member_name(rb, "report");
				json_builder_add_value(rb, json_node_copy(parsed));
				json_builder_set_member_name(rb, "format");
				json_builder_add_string_value(rb, "json");
				json_builder_end_object(rb);

				rn = json_builder_get_root(rb);
				json_generator_set_root(rg, rn);
				resp = json_generator_to_data(rg, NULL);
				respond_json(msg, 200, resp);
				return;
			}
			/* If JSON invalid, fall through to plain text */
		}
		/* g_autofree handles stdout_str cleanup at block exit */
	}

	{
		/* Fallback: plain text */
		{
			const gchar *argv_txt[] = { "report", report_type, NULL };
			gchar *out2 = NULL, *err2 = NULL;
			gint   rc2  = vimban_cmd(app, argv_txt, CMD_TIMEOUT_SEC, &out2, &err2);
			g_autofree gchar *stdout2 = out2;
			g_free(err2);

			{
				g_autoptr(JsonBuilder) b = json_builder_new();
				g_autoptr(JsonNode) root = NULL;
				g_autoptr(JsonGenerator) gen = json_generator_new();
				gchar *resp = NULL;

				json_builder_begin_object(b);
				json_builder_set_member_name(b, "content");
				json_builder_add_string_value(b, (rc2 == 0 && stdout2) ? stdout2 : "");
				json_builder_set_member_name(b, "format");
				json_builder_add_string_value(b, "text");
				json_builder_end_object(b);

				root = json_builder_get_root(b);
				json_generator_set_root(gen, root);
				resp = json_generator_to_data(gen, NULL);
				respond_json(msg, 200, resp);
				g_free(resp);
			}
		}
	}
}


/* ─── POST /api/sync ─────────────────────────────────────────────────── */

static void
handle_sync(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	const gchar    *method;
	gchar          *out = NULL, *err_out = NULL;
	gint            rc;
	g_autofree gchar *stdout_str = NULL;
	g_autofree gchar *stderr_str = NULL;
	g_autofree gchar *resp = NULL;

	(void)server; (void)query;

	method = soup_server_message_get_method(msg);

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	add_cors_headers(msg);

	if (g_strcmp0(method, "POST") != 0) {
		respond_error(msg, 405, "method_not_allowed", "POST only");
		return;
	}

	{
		const gchar *argv[] = { "commit", NULL };
		rc = vimban_cmd(app, argv, CMD_TIMEOUT_LONG_SEC, &out, &err_out);
		stdout_str = out;
		stderr_str = err_out;
	}

	resp = build_error_json(rc == 0,
		rc == 0 ? "Sync complete" : (stderr_str && *stderr_str ? stderr_str : "Sync failed"));
	respond_json(msg, 200, resp);
}


/* ─── GET /api/snapshot ──────────────────────────────────────────────── */

static void
handle_snapshot(
	SoupServer        *server,
	SoupServerMessage *msg,
	const gchar       *path,
	GHashTable        *query,
	gpointer           user_data
){
	VimbanServeApp *app = (VimbanServeApp *)user_data;
	const gchar    *method;

	(void)server; (void)query;

	method = soup_server_message_get_method(msg);

	if (g_strcmp0(method, "OPTIONS") == 0) {
		add_cors_headers(msg);
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	{
		g_autofree gchar *user = NULL;
		if (!check_auth(app, msg, path, &user)) return;
	}

	add_cors_headers(msg);

	if (g_strcmp0(method, "GET") != 0) {
		respond_error(msg, 405, "method_not_allowed", "GET only");
		return;
	}

	{
		const gchar *argv[] = { "list", "-f", "json", NULL };
		gchar *out = NULL, *err_out = NULL;
		gint rc;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonGenerator) gen = json_generator_new();
		g_autoptr(JsonNode) root = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autofree gchar *stdout_str = NULL;
		g_autofree gchar *stderr_str = NULL;
		guint i;

		rc = vimban_cmd(app, argv, CMD_TIMEOUT_SEC, &out, &err_out);
		stdout_str = out;
		stderr_str = err_out;

		json_builder_begin_object(b);

		/* tickets array */
		json_builder_set_member_name(b, "tickets");
		if (rc == 0 && stdout_str && json_parser_load_from_data(parser, stdout_str, -1, NULL)) {
			json_builder_add_value(b, json_node_copy(json_parser_get_root(parser)));
		} else {
			json_builder_begin_array(b);
			json_builder_end_array(b);
		}

		/* presence array */
		json_builder_set_member_name(b, "presence");
		json_builder_begin_array(b);
		for (i = 0; i < app->sse_clients->len; i++) {
			SseClient *c = g_ptr_array_index(app->sse_clients, i);
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "user");
			json_builder_add_string_value(b, c->user);
			json_builder_set_member_name(b, "viewing");
			json_builder_add_string_value(b, c->viewing ? c->viewing : "");
			json_builder_end_object(b);
		}
		json_builder_end_array(b);

		/* timestamp */
		json_builder_set_member_name(b, "ts");
		json_builder_add_double_value(b, (gdouble)g_get_real_time() / 1000000.0);

		json_builder_end_object(b);

		root = json_builder_get_root(b);
		json_generator_set_root(gen, root);
		{
			g_autofree gchar *resp = json_generator_to_data(gen, NULL);
			respond_json(msg, 200, resp);
		}
	}
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Route registration
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * register_auth_handler() — wrapper that injects auth checking before every
 * handler callback.  We use a trampoline struct because libsoup route
 * callbacks do not carry per-route user_data beyond the single void*.
 *
 * In practice we pass the VimbanServeApp* as user_data and each handler is
 * responsible for calling check_auth() itself via the passed app pointer.
 */

static void
register_routes(
	VimbanServeApp *app
){
	/*
	 * libsoup 3 prefix matching: a path ending in '*' matches all URIs that
	 * start with that prefix. Exact paths take priority over prefix matches.
	 */

	/* Exact routes */
	soup_server_add_handler(app->server, "/",
	                        handle_index,         app, NULL);
	soup_server_add_handler(app->server, "/api/health",
	                        handle_health,        app, NULL);
	soup_server_add_handler(app->server, "/api/tickets",
	                        handle_tickets,       app, NULL);
	soup_server_add_handler(app->server, "/api/search",
	                        handle_search,        app, NULL);
	soup_server_add_handler(app->server, "/api/kanban",
	                        handle_kanban,        app, NULL);
	soup_server_add_handler(app->server, "/api/people",
	                        handle_people,        app, NULL);
	soup_server_add_handler(app->server, "/api/projects",
	                        handle_projects,      app, NULL);
	soup_server_add_handler(app->server, "/api/commit",
	                        handle_commit,        app, NULL);
	soup_server_add_handler(app->server, "/api/validate",
	                        handle_validate,      app, NULL);
	soup_server_add_handler(app->server, "/api/presence",
	                        handle_presence,      app, NULL);
	soup_server_add_handler(app->server, "/api/events",
	                        handle_events,        app, NULL);
	soup_server_add_handler(app->server, "/api/generate-link",
	                        handle_generate_link, app, NULL);
	soup_server_add_handler(app->server, "/api/convert",
	                        handle_convert,       app, NULL);
	soup_server_add_handler(app->server, "/api/report",
	                        handle_report,        app, NULL);
	soup_server_add_handler(app->server, "/api/sync",
	                        handle_sync,          app, NULL);
	soup_server_add_handler(app->server, "/api/snapshot",
	                        handle_snapshot,       app, NULL);

	/* Prefix routes (libsoup matches longest prefix) */
	soup_server_add_handler(app->server, "/api/ticket",
	                        handle_ticket_dispatch, app, NULL);
	soup_server_add_handler(app->server, "/api/person",
	                        handle_person_dispatch, app, NULL);
	soup_server_add_handler(app->server, "/api/mentor",
	                        handle_mentor,          app, NULL);
	soup_server_add_handler(app->server, "/api/dashboard",
	                        handle_dashboard,       app, NULL);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Request auth trampoline (before-request equivalent)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * libsoup 3 does not have a true "before request" hook.  Instead we rely
 * on each handler calling check_auth() before any work.  The
 * register_routes() function above wires all handlers to receive the
 * VimbanServeApp* so they can perform this check.
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * main()
 * ═══════════════════════════════════════════════════════════════════════════ */

static const gchar LICENSE_TEXT[] =
	"vimban_serve - Web UI for vimban ticket management\n"
	"Copyright (C) 2025  Zach Podbielniak\n\n"
	"This program is free software: you can redistribute it and/or modify\n"
	"it under the terms of the GNU Affero General Public License as published\n"
	"by the Free Software Foundation, either version 3 of the License, or\n"
	"(at your option) any later version.\n\n"
	"This program is distributed in the hope that it will be useful,\n"
	"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
	"GNU Affero General Public License for more details.\n\n"
	"You should have received a copy of the GNU Affero General Public License\n"
	"along with this program.  If not, see <https://www.gnu.org/licenses/>.";


int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(VimbanServeApp) app = vimban_serve_app_new();
	g_autoptr(GError)         err = NULL;

	/* ── CLI option definitions ─────────────────────────────────────── */
	gchar    *opt_bind      = NULL;
	gint      opt_port      = DEFAULT_PORT;
	gboolean  opt_no_token  = FALSE;
	gchar    *opt_token_file = NULL;
	gchar    *opt_directory  = NULL;
	gboolean  opt_work       = FALSE;
	gboolean  opt_personal   = FALSE;
	gboolean  opt_archived   = FALSE;
	gboolean  opt_license    = FALSE;
	gboolean  opt_version    = FALSE;

	GOptionEntry entries[] = {
		{ "bind",       'b', 0, G_OPTION_ARG_STRING,  &opt_bind,
		  "Address to bind to (default: " DEFAULT_BIND ")", "ADDR" },
		{ "port",       'p', 0, G_OPTION_ARG_INT,     &opt_port,
		  "Port to listen on (default: 5005)", "PORT" },
		{ "directory",  'd', 0, G_OPTION_ARG_STRING,  &opt_directory,
		  "Vimban working directory", "DIR" },
		{ "work",        0,  0, G_OPTION_ARG_NONE,    &opt_work,
		  "Operate on work tickets", NULL },
		{ "personal",    0,  0, G_OPTION_ARG_NONE,    &opt_personal,
		  "Operate on personal tickets", NULL },
		{ "archived",    0,  0, G_OPTION_ARG_NONE,    &opt_archived,
		  "Include archived items", NULL },
		{ "no-token",    0,  0, G_OPTION_ARG_NONE,    &opt_no_token,
		  "Disable token authentication", NULL },
		{ "token-file",  0,  0, G_OPTION_ARG_STRING,  &opt_token_file,
		  "Path to token file", "FILE" },
		{ "version",     0,  0, G_OPTION_ARG_NONE,    &opt_version,
		  "Show version", NULL },
		{ "license",     0,  0, G_OPTION_ARG_NONE,    &opt_license,
		  "Show license text", NULL },
		{ NULL }
	};

	g_autoptr(GOptionContext) ctx = g_option_context_new("-- [options]");
	g_option_context_set_summary(ctx,
		"vimban_serve " VIMBAN_SERVE_VERSION
		" — Web UI server for vimban ticket management\n\n"
		"Examples:\n"
		"  vimban_serve                  Start on default port (5005)\n"
		"  vimban_serve --port 8080      Start on port 8080\n"
		"  vimban_serve --bind 0.0.0.0   Listen on all interfaces\n"
		"  vimban_serve --no-token       Disable token authentication");
	g_option_context_add_main_entries(ctx, entries, NULL);

	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		g_printerr("Error: %s\n", err->message);
		return 1;
	}

	if (opt_version)
	{
		g_print ("vimban_serve %s\n", VIMBAN_SERVE_VERSION);
		g_free (opt_bind);
		g_free (opt_token_file);
		g_free (opt_directory);
		return 0;
	}

	if (opt_license)
	{
		g_print ("%s\n", LICENSE_TEXT);
		g_free (opt_bind);
		g_free (opt_token_file);
		g_free (opt_directory);
		return 0;
	}

	/* set directory on app */
	if (opt_directory)
		app->directory = g_steal_pointer (&opt_directory);
	app->work     = opt_work;
	app->personal = opt_personal;
	app->archived = opt_archived;

	/* ── Token file path ────────────────────────────────────────────── */
	if (opt_token_file) {
		app->token_file = g_steal_pointer(&opt_token_file);
	} else {
		app->token_file = g_build_filename(g_get_home_dir(),
		                                   TOKEN_FILE_RELPATH, NULL);
	}

	/* ── Auth configuration ─────────────────────────────────────────── */
	app->no_token = opt_no_token;
	if (opt_no_token) {
		app->auth_enabled = FALSE;
		g_print("  Auth    : DISABLED (--no-token)\n");
	} else {
		load_tokens(app);
		if (g_hash_table_size(app->tokens) > 0) {
			app->auth_enabled = TRUE;
			g_print("  Auth    : enabled (%u token(s) loaded from %s)\n",
			        g_hash_table_size(app->tokens), app->token_file);
		} else {
			app->auth_enabled = FALSE;
			g_print("  Auth    : DISABLED (no tokens found in %s)\n",
			        app->token_file);
		}
	}

	/* ── SoupServer setup ───────────────────────────────────────────── */
	app->server = soup_server_new(NULL, NULL);
	register_routes(app);

	/* Resolve bind address once — used for listen and startup banner */
	{
		const gchar *bind_addr = opt_bind ? opt_bind : DEFAULT_BIND;
		GInetAddress *ia    = g_inet_address_new_from_string(bind_addr);
		g_autoptr(GSocketAddress) sa = NULL;
		if (!ia) {
			g_printerr("Error: invalid bind address '%s'\n", bind_addr);
			g_free(opt_bind);
			return 1;
		}
		sa = (GSocketAddress *)g_inet_socket_address_new(ia, (guint16)opt_port);
		g_object_unref(ia);

		if (!soup_server_listen(app->server, sa, 0, &err)) {
			g_printerr("Failed to start server: %s\n", err->message);
			g_free(opt_bind);
			return 1;
		}

		g_print("Starting vimban web server on http://%s:%d\n", bind_addr, opt_port);
		g_print("  Kanban  : http://%s:%d\n",           bind_addr, opt_port);
		g_print("  API     : http://%s:%d/api/tickets\n\n", bind_addr, opt_port);
	}

	g_free(opt_bind);

	/* ── SSE keepalive timer ────────────────────────────────────────── */
	g_timeout_add_seconds(SSE_KEEPALIVE_INTERVAL, sse_keepalive_cb, app);

	/* ── Main loop ──────────────────────────────────────────────────── */
	app->loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(app->loop);

	return 0;
}
