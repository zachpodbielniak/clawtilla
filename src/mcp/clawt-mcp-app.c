/*
 * clawt-mcp-app.c - An MCP server's app, shown in the conversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-app.h"
#include "mcp/clawt-mcp-result.h"
#include "config/clawt-config-render.h"

#include <string.h>

#include <glib/gstdio.h>

/* Origins per list a view may ask for; past it the rest are refused. */
#define MAX_DOMAINS 16

/* What of a tool's result an app record keeps for its view to show. */
#define MAX_RESULT_BYTES (512 * 1024)

/* The relay's own requests carry ids no MCP client would choose. */
#define READ_ID_PREFIX "clawtilla-app-read-"

/* ── Tool definitions ────────────────────────────────────────────── */

static JsonObject *
ui_meta(JsonObject *item)
{
	JsonObject *meta;

	if (item == NULL || !json_object_has_member(item, "_meta") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(item, "_meta")))
		return NULL;

	meta = json_object_get_object_member(item, "_meta");

	if (!json_object_has_member(meta, "ui") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(meta, "ui")))
		return NULL;

	return json_object_get_object_member(meta, "ui");
}

gchar *
clawt_mcp_app_tool_resource(JsonObject *tool)
{
	JsonObject *ui;
	const gchar *uri = NULL;

	g_return_val_if_fail(tool != NULL, NULL);

	ui = ui_meta(tool);

	if (ui != NULL)
		uri = json_object_get_string_member_with_default(ui, "resourceUri",
		                                                 NULL);

	/* The flat spelling earlier drafts used, still sent by some servers. */
	if (uri == NULL && json_object_has_member(tool, "_meta") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(tool, "_meta")))
		uri = json_object_get_string_member_with_default(
			json_object_get_object_member(tool, "_meta"), "ui/resourceUri",
			NULL);

	if (uri == NULL || !g_str_has_prefix(uri, "ui://") ||
	    strlen(uri) > 2048)
		return NULL;

	return g_strdup(uri);
}

/*
 * Whether `_meta.ui.visibility` names @who.  Absent means both, which is
 * the extension's default; present, it is the whole list.
 */
static gboolean
tool_visible_to(JsonObject *tool, const gchar *who)
{
	JsonObject *ui;
	JsonArray *visibility;
	guint i;

	ui = ui_meta(tool);

	if (ui == NULL || !json_object_has_member(ui, "visibility") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(ui, "visibility")))
		return TRUE;

	visibility = json_object_get_array_member(ui, "visibility");

	for (i = 0; i < json_array_get_length(visibility); i++) {
		JsonNode *entry = json_array_get_element(visibility, i);

		if (JSON_NODE_HOLDS_VALUE(entry) &&
		    json_node_get_value_type(entry) == G_TYPE_STRING &&
		    g_strcmp0(json_node_get_string(entry), who) == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
clawt_mcp_app_tool_model_visible(JsonObject *tool)
{
	g_return_val_if_fail(tool != NULL, TRUE);

	return tool_visible_to(tool, "model");
}

gboolean
clawt_mcp_app_tool_app_visible(JsonObject *tool)
{
	g_return_val_if_fail(tool != NULL, FALSE);

	return tool_visible_to(tool, "app");
}

/* ── The policy and the document ─────────────────────────────────── */

static gboolean
host_ok(const gchar *host, gboolean allow_wildcard)
{
	const gchar *p = host;
	gboolean dotted = FALSE;

	if (allow_wildcard && g_str_has_prefix(p, "*."))
		p += 2;

	if (*p == '\0' || *p == '.' || *p == '-')
		return FALSE;

	for (; *p != '\0'; p++) {
		if (*p == '.') {
			if (p[1] == '\0' || p[1] == '.')
				return FALSE;

			dotted = TRUE;
			continue;
		}

		if (!g_ascii_isalnum(*p) && *p != '-')
			return FALSE;
	}

	return dotted;
}

gboolean
clawt_mcp_app_domain_ok(
	const gchar *entry,
	gboolean     websocket,
	gboolean     loopback
){
	g_autofree gchar *host = NULL;
	const gchar *rest;
	const gchar *colon;

	if (entry == NULL || strlen(entry) > 253 + 16)
		return FALSE;

	if (g_str_has_prefix(entry, "https://"))
		rest = entry + strlen("https://");
	else if (websocket && g_str_has_prefix(entry, "wss://"))
		rest = entry + strlen("wss://");
	else if (loopback && g_str_has_prefix(entry, "http://"))
		rest = entry + strlen("http://");
	else
		return FALSE;

	colon = strchr(rest, ':');
	host = (colon != NULL) ? g_strndup(rest, (gsize)(colon - rest))
	                       : g_strdup(rest);

	/* A port is digits and nothing after them. */
	if (colon != NULL) {
		const gchar *p = colon + 1;
		gsize digits = 0;

		for (; *p != '\0'; p++, digits++) {
			if (!g_ascii_isdigit(*p))
				return FALSE;
		}

		if (digits == 0 || digits > 5)
			return FALSE;
	}

	/* Plain http only ever to this machine, and only for a local server. */
	if (g_str_has_prefix(entry, "http://"))
		return g_strcmp0(host, "localhost") == 0 ||
		       g_strcmp0(host, "127.0.0.1") == 0;

	if (g_strcmp0(host, "localhost") == 0 || g_strcmp0(host, "127.0.0.1") == 0)
		return loopback;

	return host_ok(host, TRUE);
}

static void
collect_domains(JsonObject *csp, const gchar *member, gboolean websocket,
                gboolean loopback, GString *into, GPtrArray *refused)
{
	JsonArray *list;
	guint kept = 0;
	guint i;

	if (csp == NULL || !json_object_has_member(csp, member) ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(csp, member)))
		return;

	list = json_object_get_array_member(csp, member);

	for (i = 0; i < json_array_get_length(list); i++) {
		JsonNode *node = json_array_get_element(list, i);
		const gchar *entry = JSON_NODE_HOLDS_VALUE(node) &&
		                     json_node_get_value_type(node) == G_TYPE_STRING
			? json_node_get_string(node) : NULL;

		if (entry == NULL)
			continue;

		if (kept >= MAX_DOMAINS ||
		    !clawt_mcp_app_domain_ok(entry, websocket, loopback)) {
			if (refused != NULL)
				g_ptr_array_add(refused, g_strdup(entry));
			continue;
		}

		g_string_append_c(into, ' ');
		g_string_append(into, entry);
		kept++;
	}
}

gchar *
clawt_mcp_app_build_csp(
	JsonObject  *ui,
	gboolean     loopback,
	GStrv       *out_refused
){
	g_autoptr(GString) resources = g_string_new(NULL);
	g_autoptr(GString) connect = g_string_new(NULL);
	GPtrArray *refused = g_ptr_array_new_with_free_func(g_free);
	JsonObject *csp = NULL;
	gchar *policy;

	if (ui != NULL && json_object_has_member(ui, "csp") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(ui, "csp")))
		csp = json_object_get_object_member(ui, "csp");

	collect_domains(csp, "resourceDomains", FALSE, loopback, resources,
	                refused);
	collect_domains(csp, "connectDomains", TRUE, loopback, connect, refused);

	/*
	 * The spec's construction with the host's filter applied.  No
	 * 'self': the frame has an opaque origin, so 'self' would match
	 * nothing useful and suggest it matched the client.
	 */
	policy = g_strdup_printf(
		"default-src 'none'; "
		"script-src 'unsafe-inline' 'unsafe-eval' 'wasm-unsafe-eval'%s; "
		"style-src 'unsafe-inline'%s; "
		"font-src data:%s; "
		"img-src data: blob:%s; "
		"media-src data: blob:%s; "
		"connect-src%s; "
		"worker-src blob:; "
		"frame-src 'none'; object-src 'none'; base-uri 'none'; "
		"form-action 'none'",
		resources->str, resources->str, resources->str, resources->str,
		resources->str, connect->len > 0 ? connect->str : " 'none'");

	if (out_refused != NULL) {
		g_ptr_array_add(refused, NULL);
		*out_refused = (GStrv)g_ptr_array_free(refused, FALSE);
	} else {
		g_ptr_array_unref(refused);
	}

	return policy;
}

static gchar *
attribute_escape(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		switch (*p) {
		case '&':  g_string_append(out, "&amp;");  break;
		case '"':  g_string_append(out, "&quot;"); break;
		case '<':  g_string_append(out, "&lt;");   break;
		case '>':  g_string_append(out, "&gt;");   break;
		default:   g_string_append_c(out, *p);     break;
		}
	}

	return g_string_free(out, FALSE);
}

gchar *
clawt_mcp_app_build_document(
	const gchar *html,
	const gchar *csp
){
	g_autofree gchar *escaped = NULL;
	const gchar *p;

	g_return_val_if_fail(csp != NULL, NULL);

	p = (html != NULL) ? html : "";

	/*
	 * Nothing of the view's may come before the policy: a byte-order
	 * mark, leading space and its own doctype are taken off so that the
	 * <meta> really is the first thing the parser meets.  Comments are
	 * left, since a comment cannot run anything before it.
	 */
	if ((guchar)p[0] == 0xEF && (guchar)p[1] == 0xBB && (guchar)p[2] == 0xBF)
		p += 3;

	while (g_ascii_isspace(*p))
		p++;

	if (g_ascii_strncasecmp(p, "<!doctype", 9) == 0) {
		const gchar *close = strchr(p, '>');

		if (close != NULL)
			p = close + 1;
	}

	escaped = attribute_escape(csp);

	return g_strdup_printf("<!DOCTYPE html><meta http-equiv=\"Content-Security-"
	                       "Policy\" content=\"%s\"><meta name=\"referrer\" "
	                       "content=\"no-referrer\">%s", escaped, p);
}

/* ── Ids, links and the store ────────────────────────────────────── */

gchar *
clawt_mcp_app_new_id(void)
{
	g_autofree gchar *token = clawt_generate_token(NULL);

	if (token == NULL || strlen(token) < 16)
		return NULL;

	return g_strdup_printf("a%.16s", token);
}

gboolean
clawt_mcp_app_id_is_valid(const gchar *id)
{
	gsize i;

	if (id == NULL || id[0] != 'a' || strlen(id) != 17)
		return FALSE;

	for (i = 1; i < 17; i++) {
		if (!g_ascii_isxdigit(id[i]) || g_ascii_isupper(id[i]))
			return FALSE;
	}

	return TRUE;
}

gchar *
clawt_mcp_app_web_path(
	const gchar *agent_id,
	const gchar *app_id
){
	g_autofree gchar *agent = NULL;
	g_autofree gchar *app = NULL;

	g_return_val_if_fail(agent_id != NULL && app_id != NULL, NULL);

	agent = g_uri_escape_string(agent_id, NULL, FALSE);
	app = g_uri_escape_string(app_id, NULL, FALSE);

	return g_strdup_printf("/a/%s/app/%s", agent, app);
}

gchar *
clawt_mcp_app_web_url(
	const gchar *base,
	const gchar *agent_id,
	const gchar *app_id
){
	g_autofree gchar *path = clawt_mcp_app_web_path(agent_id, app_id);
	g_autofree gchar *trimmed = NULL;
	gsize length;

	trimmed = g_strdup((base != NULL && *base != '\0')
	                   ? base : "http://127.0.0.1:8790");
	length = strlen(trimmed);

	while (length > 0 && trimmed[length - 1] == '/')
		trimmed[--length] = '\0';

	return g_strconcat(trimmed, path, NULL);
}

gchar *
clawt_mcp_app_link_label(const gchar *title)
{
	if (title == NULL || *title == '\0')
		return g_strdup("Open the app in the browser");

	return g_strdup_printf("Open %s in the browser", title);
}

gchar *
clawt_mcp_app_store_dir(
	ClawtConfig *config,
	const gchar *agent_id
){
	g_autofree gchar *state = NULL;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(agent_id != NULL, NULL);

	state = clawt_config_agent_state_dir(config, agent_id);

	return g_build_filename(state, "mcp-apps", NULL);
}

static gchar *
record_path(const gchar *dir, const gchar *id)
{
	g_autofree gchar *name = NULL;

	if (dir == NULL || !clawt_mcp_app_id_is_valid(id))
		return NULL;

	name = g_strconcat(id, ".json", NULL);

	return g_build_filename(dir, name, NULL);
}

gboolean
clawt_mcp_app_save(
	const gchar  *dir,
	const gchar  *id,
	JsonObject   *record,
	GError      **error
){
	g_autofree gchar *path = record_path(dir, id);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;

	g_return_val_if_fail(record != NULL, FALSE);

	if (path == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not an app id", id != NULL ? id : "");
		return FALSE;
	}

	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	json_node_set_object(node, record);
	text = json_to_string(node, FALSE);

	if (!clawt_write_file_atomic(path, text, -1, 0600, FALSE, error))
		return FALSE;

	clawt_mcp_app_sweep(dir, g_get_real_time() / G_USEC_PER_SEC,
	                    CLAWT_MCP_APP_KEEP_COUNT, CLAWT_MCP_APP_KEEP_SECONDS);

	return TRUE;
}

JsonObject *
clawt_mcp_app_load(
	const gchar  *dir,
	const gchar  *id,
	GError      **error
){
	g_autofree gchar *path = record_path(dir, id);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	/*
	 * Not there, malformed and somebody else's are one answer, so an id
	 * cannot be used to find out what another agent's tools showed.
	 */
	if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR) ||
	    !json_parser_load_from_file(parser, path, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL ||
	    !JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "there is no app '%s' in this conversation",
		            id != NULL ? id : "");
		return NULL;
	}

	return json_object_ref(json_node_get_object(root));
}

typedef struct {
	gchar  *path;
	gint64  mtime;
} AppEntry;

static void
app_entry_free(gpointer data)
{
	AppEntry *entry = data;

	g_free(entry->path);
	g_free(entry);
}

static gint
app_entry_newest_first(gconstpointer a, gconstpointer b)
{
	const AppEntry *left = *(const AppEntry * const *)a;
	const AppEntry *right = *(const AppEntry * const *)b;

	if (left->mtime != right->mtime)
		return (left->mtime > right->mtime) ? -1 : 1;

	return g_strcmp0(right->path, left->path);
}

guint
clawt_mcp_app_sweep(
	const gchar *dir,
	gint64       now,
	guint        keep_count,
	gint64       keep_seconds
){
	g_autoptr(GDir) handle = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	const gchar *name;
	guint removed = 0;
	guint i;

	if (dir == NULL || (handle = g_dir_open(dir, 0, NULL)) == NULL)
		return 0;

	entries = g_ptr_array_new_with_free_func(app_entry_free);

	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *id = NULL;
		GStatBuf st;
		AppEntry *entry;

		if (!g_str_has_suffix(name, ".json"))
			continue;

		id = g_strndup(name, strlen(name) - strlen(".json"));

		/* Only files this made: the directory is the agent's to write in. */
		if (!clawt_mcp_app_id_is_valid(id))
			continue;

		entry = g_new0(AppEntry, 1);
		entry->path = g_build_filename(dir, name, NULL);

		if (g_lstat(entry->path, &st) != 0 || !S_ISREG(st.st_mode)) {
			app_entry_free(entry);
			continue;
		}

		entry->mtime = (gint64)st.st_mtime;
		g_ptr_array_add(entries, entry);
	}

	g_ptr_array_sort(entries, app_entry_newest_first);

	for (i = 0; i < entries->len; i++) {
		AppEntry *entry = g_ptr_array_index(entries, i);

		if (i >= keep_count || now - entry->mtime > keep_seconds) {
			(void)g_unlink(entry->path);
			removed++;
		}
	}

	return removed;
}

/* ── The relay's side ────────────────────────────────────────────── */

typedef struct {
	gchar      *tool;
	gchar      *title;
	gchar      *resource_uri;
	JsonNode   *input;
} AppCall;

static void
app_call_free(gpointer data)
{
	AppCall *call = data;

	if (call == NULL)
		return;

	g_free(call->tool);
	g_free(call->title);
	g_free(call->resource_uri);

	if (call->input != NULL)
		json_node_unref(call->input);

	g_free(call);
}

typedef struct {
	gchar      *html;
	JsonObject *ui;
	gchar      *blocked;
} AppView;

static void
app_view_free(gpointer data)
{
	AppView *view = data;

	g_free(view->html);

	if (view->ui != NULL)
		json_object_unref(view->ui);

	g_free(view->blocked);
	g_free(view);
}

/* One call waiting on the view the relay asked for. */
typedef struct {
	AppCall *call;
	JsonNode *result;
} AppWait;

static void
app_wait_free(gpointer data)
{
	AppWait *wait = data;

	app_call_free(wait->call);

	if (wait->result != NULL)
		json_node_unref(wait->result);

	g_free(wait);
}

struct _ClawtMcpApps {
	gchar                *connector;
	gboolean              loopback;
	ClawtMcpAppsSeenFunc  seen;
	gpointer              seen_data;

	/* tool name -> its definition, for tools that ship a view */
	GHashTable           *tools;

	/* tools the app's view may call and the model may not */
	GHashTable           *app_only;

	/* request id, as JSON text -> AppCall, for open calls of those tools */
	GHashTable           *calls;

	/* resource URI -> AppView, once read */
	GHashTable           *views;

	/* our own read's id -> AppWait */
	GHashTable           *waits;
	guint                 next_read;

	/*
	 * Set when the caller is an app's view, not the agent: tools the
	 * server lets its apps call, seen in tools/list -- the only ones a
	 * call may name.
	 */
	gboolean              app_caller;
	GHashTable           *app_visible;
};

ClawtMcpApps *
clawt_mcp_apps_new(
	const gchar *connector,
	gboolean     loopback
){
	ClawtMcpApps *self = g_new0(ClawtMcpApps, 1);

	self->connector = g_strdup(connector);
	self->loopback = loopback;
	self->tools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    (GDestroyNotify)json_object_unref);
	self->app_only = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       NULL);
	self->calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    app_call_free);
	self->views = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    app_view_free);
	self->waits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    app_wait_free);
	self->app_visible = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                          NULL);

	return self;
}

void
clawt_mcp_apps_set_app_caller(
	ClawtMcpApps *self,
	gboolean      app_caller
){
	g_return_if_fail(self != NULL);

	self->app_caller = app_caller;
}

void
clawt_mcp_apps_free(ClawtMcpApps *self)
{
	if (self == NULL)
		return;

	g_free(self->connector);
	g_hash_table_unref(self->tools);
	g_hash_table_unref(self->app_only);
	g_hash_table_unref(self->calls);
	g_hash_table_unref(self->views);
	g_hash_table_unref(self->waits);
	g_hash_table_unref(self->app_visible);
	g_free(self);
}

void
clawt_mcp_apps_set_seen_func(
	ClawtMcpApps         *self,
	ClawtMcpAppsSeenFunc  func,
	gpointer              user_data
){
	g_return_if_fail(self != NULL);

	self->seen = func;
	self->seen_data = user_data;
}

static JsonObject *
parse_object(const gchar *line)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (line == NULL || !json_parser_load_from_data(parser, line, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_object_ref(json_node_get_object(root));
}

static gchar *
render(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

	json_node_set_object(node, object);

	return json_to_string(node, FALSE);
}

static gchar *
id_text(JsonObject *message)
{
	JsonNode *id;

	if (!json_object_has_member(message, "id"))
		return NULL;

	id = json_object_get_member(message, "id");

	return JSON_NODE_HOLDS_NULL(id) ? NULL : json_to_string(id, FALSE);
}

static JsonObject *
member_object(JsonObject *object, const gchar *name)
{
	if (object == NULL || !json_object_has_member(object, name) ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, name)))
		return NULL;

	return json_object_get_object_member(object, name);
}

/*
 * `initialize` with the extension advertised, so a server that ships
 * views knows somebody will show them.  The spec has servers leave
 * `_meta.ui` out for a client that does not say so.
 */
static gchar *
advertise(JsonObject *message)
{
	JsonObject *params = member_object(message, "params");
	JsonObject *capabilities;
	JsonObject *extensions;
	JsonObject *ui = json_object_new();
	JsonArray *mimes = json_array_new();

	if (params == NULL) {
		params = json_object_new();
		json_object_set_object_member(message, "params", params);
	}

	capabilities = member_object(params, "capabilities");

	if (capabilities == NULL) {
		capabilities = json_object_new();
		json_object_set_object_member(params, "capabilities", capabilities);
	}

	extensions = member_object(capabilities, "extensions");

	if (extensions == NULL) {
		extensions = json_object_new();
		json_object_set_object_member(capabilities, "extensions", extensions);
	}

	json_array_add_string_element(mimes, CLAWT_MCP_APP_MIME);
	json_object_set_array_member(ui, "mimeTypes", mimes);
	json_object_set_object_member(extensions, CLAWT_MCP_APP_EXTENSION, ui);

	return render(message);
}

static gchar *
refusal(JsonObject *message, const gchar *text)
{
	g_autoptr(JsonObject) reply = json_object_new();
	JsonObject *error = json_object_new();

	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id", json_node_copy(
		json_object_get_member(message, "id")));
	json_object_set_int_member(error, "code", -32602);
	json_object_set_string_member(error, "message", text);
	json_object_set_object_member(reply, "error", error);

	return render(reply);
}

void
clawt_mcp_apps_outbound(
	ClawtMcpApps  *self,
	const gchar   *line,
	gchar        **to_server,
	gchar        **to_client
){
	g_autoptr(JsonObject) message = NULL;
	const gchar *method;

	*to_server = NULL;
	*to_client = NULL;

	g_return_if_fail(self != NULL);

	message = parse_object(line);

	if (message == NULL) {
		*to_server = g_strdup(line);
		return;
	}

	method = json_object_get_string_member_with_default(message, "method", "");

	if (g_strcmp0(method, "initialize") == 0) {
		*to_server = advertise(message);
		return;
	}

	if (g_strcmp0(method, "tools/call") == 0) {
		JsonObject *params = member_object(message, "params");
		const gchar *name = (params != NULL)
			? json_object_get_string_member_with_default(params, "name", NULL)
			: NULL;
		g_autofree gchar *id = id_text(message);
		JsonObject *tool;

		/*
		 * An app's own call: only a tool the server listed as visible
		 * to apps, and nothing else about it -- no view is read for a
		 * call the view itself made.  A tool not seen in tools/list is
		 * refused like one hidden from apps: the relay cannot vouch for
		 * what it was not shown.
		 */
		if (self->app_caller) {
			if (name == NULL ||
			    !g_hash_table_contains(self->app_visible, name)) {
				if (json_object_has_member(message, "id"))
					*to_client = refusal(message, "This tool is not one "
					                              "the server lets its "
					                              "apps call.");
				return;
			}

			*to_server = g_strdup(line);
			return;
		}

		/*
		 * Taken out of the list and refused when called: a wall with
		 * no door.  An app's own calls come through a relay started
		 * for them (clawt_mcp_apps_set_app_caller()), never this one.
		 */
		if (name != NULL && g_hash_table_contains(self->app_only, name)) {
			if (json_object_has_member(message, "id"))
				*to_client = refusal(message, "This tool belongs to an "
				                              "app's view, not to the "
				                              "agent.");
			return;
		}

		tool = (name != NULL) ? g_hash_table_lookup(self->tools, name) : NULL;

		if (tool != NULL && id != NULL) {
			AppCall *call = g_new0(AppCall, 1);

			call->tool = g_strdup(name);
			call->title = g_strdup(json_object_get_string_member_with_default(
				tool, "title", NULL));
			call->resource_uri = clawt_mcp_app_tool_resource(tool);
			call->input = (params != NULL &&
			               json_object_has_member(params, "arguments"))
				? json_node_copy(json_object_get_member(params, "arguments"))
				: NULL;

			if (g_hash_table_size(self->calls) >= 256)
				g_hash_table_remove_all(self->calls);

			g_hash_table_replace(self->calls, g_steal_pointer(&id), call);
		}
	}

	*to_server = g_strdup(line);
}

/* A tools/list page: remember the tools with views, drop the app's own. */
static gboolean
note_tools(ClawtMcpApps *self, JsonObject *result)
{
	JsonArray *tools;
	JsonArray *kept;
	gboolean removed = FALSE;
	guint i;

	if (!json_object_has_member(result, "tools") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "tools")))
		return FALSE;

	tools = json_object_get_array_member(result, "tools");
	kept = json_array_new();

	for (i = 0; i < json_array_get_length(tools); i++) {
		JsonNode *element = json_array_get_element(tools, i);
		JsonObject *tool;
		const gchar *name;
		g_autofree gchar *uri = NULL;

		if (!JSON_NODE_HOLDS_OBJECT(element)) {
			json_array_add_element(kept, json_node_copy(element));
			continue;
		}

		tool = json_node_get_object(element);
		name = json_object_get_string_member_with_default(tool, "name", NULL);

		if (name == NULL) {
			json_array_add_element(kept, json_node_copy(element));
			continue;
		}

		uri = clawt_mcp_app_tool_resource(tool);

		if (uri != NULL)
			g_hash_table_replace(self->tools, g_strdup(name),
			                     json_object_ref(tool));

		/* For an app's caller the list is the app's, whole. */
		if (self->app_caller) {
			if (clawt_mcp_app_tool_app_visible(tool))
				g_hash_table_add(self->app_visible, g_strdup(name));
			else
				g_hash_table_remove(self->app_visible, name);

			json_array_add_element(kept, json_node_copy(element));
			continue;
		}

		if (!clawt_mcp_app_tool_model_visible(tool)) {
			g_hash_table_add(self->app_only, g_strdup(name));
			removed = TRUE;
			continue;
		}

		g_hash_table_remove(self->app_only, name);
		json_array_add_element(kept, json_node_copy(element));
	}

	if (removed)
		json_object_set_array_member(result, "tools", kept);
	else
		json_array_unref(kept);

	return removed;
}

/* What of a result a view is handed: all of it, or a trimmed copy. */
static JsonNode *
result_for_record(JsonNode *result)
{
	g_autofree gchar *text = json_to_string(result, FALSE);
	g_autoptr(JsonObject) trimmed = NULL;

	if (strlen(text) <= MAX_RESULT_BYTES || !JSON_NODE_HOLDS_OBJECT(result))
		return json_node_copy(result);

	trimmed = clawt_mcp_result_trim(json_node_get_object(result),
	                                MAX_RESULT_BYTES, strlen(text), NULL,
	                                NULL, NULL, NULL);

	if (trimmed == NULL)
		return json_node_copy(result);

	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);

		json_node_set_object(node, trimmed);

		return node;
	}
}

static void
report(ClawtMcpApps *self, AppCall *call, JsonNode *result, AppView *view)
{
	g_autoptr(JsonObject) out = json_object_new();
	g_auto(GStrv) refused = NULL;
	g_autofree gchar *csp = NULL;
	JsonArray *refused_array = json_array_new();
	gsize i;

	if (self->seen == NULL) {
		json_array_unref(refused_array);
		return;
	}

	if (self->connector != NULL)
		json_object_set_string_member(out, "connector", self->connector);

	json_object_set_string_member(out, "tool", call->tool);

	if (call->title != NULL)
		json_object_set_string_member(out, "title", call->title);

	if (call->resource_uri != NULL)
		json_object_set_string_member(out, "resource_uri",
		                              call->resource_uri);

	csp = clawt_mcp_app_build_csp(view != NULL ? view->ui : NULL,
	                              self->loopback, &refused);

	for (i = 0; refused != NULL && refused[i] != NULL; i++)
		json_array_add_string_element(refused_array, refused[i]);

	json_object_set_string_member(out, "csp", csp);
	json_object_set_array_member(out, "refused", refused_array);

	if (view != NULL && view->html != NULL)
		json_object_set_string_member(out, "html", view->html);
	else
		json_object_set_string_member(
			out, "blocked",
			(view != NULL && view->blocked != NULL)
				? view->blocked : "the server did not send its view");

	if (call->input != NULL)
		json_object_set_member(out, "input", json_node_copy(call->input));

	if (result != NULL)
		json_object_set_member(out, "result", result_for_record(result));

	self->seen(out, self->seen_data);
}

/* The answer to the relay's own resources/read, turned into a view. */
static AppView *
read_view(JsonObject *message)
{
	AppView *view = g_new0(AppView, 1);
	JsonObject *result = member_object(message, "result");
	JsonArray *contents;
	guint i;

	if (result == NULL || !json_object_has_member(result, "contents") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "contents"))) {
		view->blocked = g_strdup("the server would not send its view");
		return view;
	}

	contents = json_object_get_array_member(result, "contents");

	for (i = 0; i < json_array_get_length(contents); i++) {
		JsonNode *node = json_array_get_element(contents, i);
		JsonObject *item;
		const gchar *mime;
		const gchar *text;
		const gchar *blob;
		g_autofree gchar *html = NULL;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;

		item = json_node_get_object(node);
		mime = json_object_get_string_member_with_default(item, "mimeType",
		                                                  "");

		if (!g_str_has_prefix(mime, "text/html"))
			continue;

		text = json_object_get_string_member_with_default(item, "text", NULL);
		blob = json_object_get_string_member_with_default(item, "blob", NULL);

		if (text != NULL) {
			html = g_strdup(text);
		} else if (blob != NULL) {
			gsize length = 0;
			guchar *decoded = g_base64_decode(blob, &length);

			html = g_strndup((const gchar *)decoded, length);
			g_free(decoded);
		}

		if (html == NULL)
			continue;

		if (strlen(html) > CLAWT_MCP_APP_MAX_HTML) {
			view->blocked = g_strdup("its view is bigger than clawtilla "
			                         "shows");
			return view;
		}

		if (!g_utf8_validate(html, -1, NULL)) {
			view->blocked = g_strdup("its view is not UTF-8 text");
			return view;
		}

		view->html = g_steal_pointer(&html);

		if (ui_meta(item) != NULL)
			view->ui = json_object_ref(ui_meta(item));

		return view;
	}

	view->blocked = g_strdup("the server sent no HTML view");

	return view;
}

gchar *
clawt_mcp_apps_inbound(
	ClawtMcpApps  *self,
	const gchar   *line,
	gchar        **to_server
){
	g_autoptr(JsonObject) message = NULL;
	g_autofree gchar *id = NULL;
	JsonObject *result;
	AppCall *call;

	*to_server = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	message = parse_object(line);

	if (message == NULL)
		return g_strdup(line);

	id = id_text(message);

	/* Our own read coming back: kept from the client, turned into a report. */
	if (id != NULL && g_str_has_prefix(id, "\"" READ_ID_PREFIX)) {
		AppWait *wait = g_hash_table_lookup(self->waits, id);

		if (wait != NULL) {
			AppView *view = read_view(message);

			report(self, wait->call, wait->result, view);

			if (wait->call->resource_uri != NULL && view->html != NULL)
				g_hash_table_replace(self->views,
				                     g_strdup(wait->call->resource_uri), view);
			else
				app_view_free(view);

			g_hash_table_remove(self->waits, id);
		}

		return NULL;
	}

	result = member_object(message, "result");

	if (result == NULL)
		return g_strdup(line);

	/* A tool list: noted, and passed on without the app's own tools. */
	if (note_tools(self, result))
		return render(message);

	{
		gchar *stolen_key = NULL;

		if (id == NULL ||
		    !g_hash_table_steal_extended(self->calls, id,
		                                 (gpointer *)&stolen_key,
		                                 (gpointer *)&call))
			return g_strdup(line);

		g_free(stolen_key);
	}

	{
		AppView *view = (call->resource_uri != NULL)
			? g_hash_table_lookup(self->views, call->resource_uri) : NULL;
		JsonNode *result_node = json_object_get_member(message, "result");

		if (view != NULL || call->resource_uri == NULL) {
			report(self, call, result_node, view);
			app_call_free(call);
		} else {
			g_autoptr(JsonObject) read = json_object_new();
			JsonObject *params = json_object_new();
			AppWait *wait = g_new0(AppWait, 1);
			g_autofree gchar *read_id = g_strdup_printf(
				READ_ID_PREFIX "%u", ++self->next_read);
			g_autoptr(JsonNode) read_id_node = json_node_new(JSON_NODE_VALUE);

			json_node_set_string(read_id_node, read_id);
			json_object_set_string_member(read, "jsonrpc", "2.0");
			json_object_set_member(read, "id",
			                       json_node_copy(read_id_node));
			json_object_set_string_member(read, "method", "resources/read");
			json_object_set_string_member(params, "uri", call->resource_uri);
			json_object_set_object_member(read, "params", params);

			wait->call = call;
			wait->result = json_node_copy(result_node);
			g_hash_table_replace(self->waits,
			                     json_to_string(read_id_node, FALSE), wait);

			*to_server = render(read);
		}
	}

	return g_strdup(line);
}
