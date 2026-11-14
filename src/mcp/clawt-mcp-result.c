/*
 * clawt-mcp-result.c - A tool result bigger than the budget
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-result.h"
#include "config/clawt-config-render.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

/* What the query tool answers with when the result says nothing of a budget. */
#define DEFAULT_QUERY_CAP (64 * 1024)

/* How much of a string is shown around each match of a search. */
#define SEARCH_CONTEXT 160

/* At most this many matches are listed; the count says how many there were. */
#define SEARCH_MAX_MATCHES 20

/* At most this many places a trim made are named in its note. */
#define MAX_NOTES 6

/* ── Sizes, labels and boundaries ────────────────────────────────── */

gint64
clawt_mcp_result_budget_resolve(
	gint64 connector_value,
	gint64 fleet_value
){
	gint64 value;

	/* The connector's own value wins; a negative one means it set none. */
	value = (connector_value >= 0) ? connector_value : fleet_value;

	if (value <= 0)
		return 0;

	return MAX(value, (gint64)CLAWT_MCP_RESULT_MIN_BUDGET);
}

gint64
clawt_mcp_result_default_budget(void)
{
	const ClawtSchemaEntry *entry =
		clawt_config_schema_lookup("connectors.max_result_bytes");

	if (entry == NULL || entry->default_value == NULL)
		return 0;

	return g_ascii_strtoll(entry->default_value, NULL, 10);
}

/* A byte count with thousands separators, the way the notes print one. */
static gchar *
format_count(guint64 value)
{
	g_autofree gchar *digits = g_strdup_printf("%" G_GUINT64_FORMAT, value);
	GString *out = g_string_new(NULL);
	gsize length = strlen(digits);
	gsize i;

	for (i = 0; i < length; i++) {
		if (i > 0 && (length - i) % 3 == 0)
			g_string_append_c(out, ',');

		g_string_append_c(out, digits[i]);
	}

	return g_string_free(out, FALSE);
}

gchar *
clawt_mcp_result_budget_label(gint64 budget)
{
	if (budget <= 0)
		return g_strdup("no budget");

	/*
	 * Whole kilobytes when it is a whole number of them, which is how
	 * people write the setting; one decimal for anything bigger.
	 */
	if (budget < 1024 * 1024) {
		if (budget % 1024 == 0)
			return g_strdup_printf("%" G_GINT64_FORMAT " KB", budget / 1024);

		return g_strdup_printf("%.1f KB", (gdouble)budget / 1024.0);
	}

	return g_strdup_printf("%.1f MB", (gdouble)budget / (1024.0 * 1024.0));
}

/*
 * How many backslashes end just before @end.  An odd count means the
 * byte at @end is escaped; an even one means it is not.
 */
static gsize
backslash_run(const gchar *text, gsize end)
{
	gsize count = 0;

	while (end > count && text[end - count - 1] == '\\')
		count++;

	return count;
}

static gboolean
is_hex(gchar c)
{
	return g_ascii_isxdigit(c);
}

/*
 * Whether an unescaped `\uXXXX` starts at @at and is a high surrogate --
 * the first half of a pair, which means nothing without the second.
 */
static gboolean
high_surrogate_at(const gchar *text, gsize length, gsize at)
{
	gchar first;
	gchar second;

	if (at + 6 > length || text[at] != '\\' || text[at + 1] != 'u')
		return FALSE;

	if (backslash_run(text, at) % 2 != 0)
		return FALSE;

	if (!is_hex(text[at + 2]) || !is_hex(text[at + 3]) ||
	    !is_hex(text[at + 4]) || !is_hex(text[at + 5]))
		return FALSE;

	first = g_ascii_tolower(text[at + 2]);
	second = g_ascii_tolower(text[at + 3]);

	return first == 'd' && (second == '8' || second == '9' ||
	                        second == 'a' || second == 'b');
}

gsize
clawt_mcp_result_safe_cut(
	const gchar *text,
	gsize        length,
	gsize        max
){
	gsize cut;
	gboolean moved = TRUE;

	if (text == NULL || max >= length)
		return length;

	cut = max;

	/*
	 * Back to a character boundary first: a continuation byte is
	 * 10xxxxxx, and the cut must land on the byte that starts a
	 * character, never inside one.
	 */
	while (cut > 0 && ((guchar)text[cut] & 0xC0) == 0x80)
		cut--;

	/*
	 * Then out of an escape, which the boundary test cannot see because
	 * every byte of `\ud83d` is ASCII.  Repeated, because backing out of
	 * one escape can land just after another.
	 */
	while (moved && cut > 0) {
		gsize i;
		gsize lowest = (cut > 5) ? cut - 5 : 0;

		moved = FALSE;

		/* Inside a \uXXXX: an unescaped backslash, a `u`, past the cut. */
		for (i = cut; i > lowest; i--) {
			gsize at = i - 1;

			if (text[at] == '\\' && at + 1 < length &&
			    text[at + 1] == 'u' && backslash_run(text, at) % 2 == 0 &&
			    at + 6 > cut) {
				cut = at;
				moved = TRUE;
				break;
			}
		}

		if (moved)
			continue;

		/* A backslash with what it escapes cut off. */
		if (backslash_run(text, cut) % 2 != 0) {
			cut--;
			moved = TRUE;
			continue;
		}

		/* The first half of an escaped surrogate pair, without the second. */
		if (cut >= 6 && high_surrogate_at(text, length, cut - 6)) {
			cut -= 6;
			moved = TRUE;
		}
	}

	return cut;
}

/* ── Ids, paths and the held results ─────────────────────────────── */

gchar *
clawt_mcp_result_new_id(void)
{
	g_autofree gchar *token = clawt_generate_token(NULL);

	if (token == NULL || strlen(token) < 16)
		return NULL;

	return g_strdup_printf("r%.16s", token);
}

gboolean
clawt_mcp_result_id_is_valid(const gchar *id)
{
	gsize i;

	if (id == NULL || id[0] != 'r' || strlen(id) != 17)
		return FALSE;

	for (i = 1; i < 17; i++) {
		if (!g_ascii_isxdigit(id[i]) || g_ascii_isupper(id[i]))
			return FALSE;
	}

	return TRUE;
}

gchar *
clawt_mcp_result_spill_dir(
	ClawtConfig *config,
	const gchar *agent_id
){
	g_autofree gchar *state = NULL;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(agent_id != NULL, NULL);

	state = clawt_config_agent_state_dir(config, agent_id);

	return g_build_filename(state, "mcp-results", NULL);
}

gchar *
clawt_mcp_result_path(
	const gchar *dir,
	const gchar *id
){
	g_autofree gchar *name = NULL;

	if (dir == NULL || !clawt_mcp_result_id_is_valid(id))
		return NULL;

	name = g_strconcat(id, ".json", NULL);

	return g_build_filename(dir, name, NULL);
}

static gchar *
meta_path(const gchar *dir, const gchar *id)
{
	g_autofree gchar *name = NULL;

	if (dir == NULL || !clawt_mcp_result_id_is_valid(id))
		return NULL;

	name = g_strconcat(id, ".meta", NULL);

	return g_build_filename(dir, name, NULL);
}

gboolean
clawt_mcp_result_write_meta(
	const gchar  *dir,
	const gchar  *id,
	const gchar  *connector,
	const gchar  *tool,
	gsize         bytes,
	gsize         budget,
	GError      **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autofree gchar *path = meta_path(dir, id);
	g_autofree gchar *text = NULL;

	if (path == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a held-result id", id != NULL ? id : "");
		return FALSE;
	}

	json_builder_begin_object(builder);

	if (connector != NULL) {
		json_builder_set_member_name(builder, "connector");
		json_builder_add_string_value(builder, connector);
	}

	if (tool != NULL) {
		json_builder_set_member_name(builder, "tool");
		json_builder_add_string_value(builder, tool);
	}

	json_builder_set_member_name(builder, "bytes");
	json_builder_add_int_value(builder, (gint64)bytes);
	json_builder_set_member_name(builder, "budget");
	json_builder_add_int_value(builder, (gint64)budget);
	json_builder_set_member_name(builder, "created_at");
	json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);
	json_builder_end_object(builder);

	json_generator_set_root(generator, json_builder_get_root(builder));
	text = json_generator_to_data(generator, NULL);

	return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

gboolean
clawt_mcp_result_spill(
	const gchar  *dir,
	const gchar  *id,
	const gchar  *data,
	gsize         length,
	const gchar  *connector,
	const gchar  *tool,
	gsize         budget,
	GError      **error
){
	g_autofree gchar *path = clawt_mcp_result_path(dir, id);

	g_return_val_if_fail(data != NULL || length == 0, FALSE);

	if (path == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a held-result id", id != NULL ? id : "");
		return FALSE;
	}

	/*
	 * 0700 and 0600: a tool result is whatever the account it came from
	 * could read, and it is somebody's account.
	 */
	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	if (!clawt_write_file_atomic(path, data, (gssize)length, 0600, FALSE,
	                             error))
		return FALSE;

	if (!clawt_mcp_result_write_meta(dir, id, connector, tool, length, budget,
	                                 error))
		return FALSE;

	clawt_mcp_result_sweep(dir, g_get_real_time() / G_USEC_PER_SEC,
	                       CLAWT_MCP_RESULT_KEEP_COUNT,
	                       CLAWT_MCP_RESULT_KEEP_SECONDS);

	return TRUE;
}

typedef struct {
	gchar  *id;
	gint64  mtime;
} HeldEntry;

static void
held_entry_free(gpointer data)
{
	HeldEntry *entry = data;

	g_free(entry->id);
	g_free(entry);
}

static gint
held_entry_newest_first(gconstpointer a, gconstpointer b)
{
	const HeldEntry *left = *(const HeldEntry * const *)a;
	const HeldEntry *right = *(const HeldEntry * const *)b;

	if (left->mtime != right->mtime)
		return (left->mtime > right->mtime) ? -1 : 1;

	return g_strcmp0(right->id, left->id);
}

static void
remove_held(const gchar *dir, const gchar *id)
{
	g_autofree gchar *path = clawt_mcp_result_path(dir, id);
	g_autofree gchar *meta = meta_path(dir, id);

	if (path != NULL)
		(void)g_unlink(path);

	if (meta != NULL)
		(void)g_unlink(meta);
}

guint
clawt_mcp_result_sweep(
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

	if (dir == NULL)
		return 0;

	handle = g_dir_open(dir, 0, NULL);

	if (handle == NULL)
		return 0;

	entries = g_ptr_array_new_with_free_func(held_entry_free);

	/*
	 * Only names this file makes.  The directory is under the agent's
	 * state, which the agent can write to, and a sweep that removed
	 * whatever it found would be a way to delete things by naming them.
	 */
	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *id = NULL;
		g_autofree gchar *path = NULL;
		GStatBuf st;
		HeldEntry *entry;

		if (!g_str_has_suffix(name, ".json"))
			continue;

		id = g_strndup(name, strlen(name) - strlen(".json"));

		if (!clawt_mcp_result_id_is_valid(id))
			continue;

		path = g_build_filename(dir, name, NULL);

		if (g_lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		entry = g_new0(HeldEntry, 1);
		entry->id = g_steal_pointer(&id);
		entry->mtime = (gint64)st.st_mtime;
		g_ptr_array_add(entries, entry);
	}

	g_ptr_array_sort(entries, held_entry_newest_first);

	for (i = 0; i < entries->len; i++) {
		HeldEntry *entry = g_ptr_array_index(entries, i);

		if (i >= keep_count || now - entry->mtime > keep_seconds) {
			remove_held(dir, entry->id);
			removed++;
		}
	}

	return removed;
}

/* ── Trimming ────────────────────────────────────────────────────── */

typedef struct {
	GPtrArray *notes;

	/* The first place cut, so the note can show a query that continues it. */
	gchar     *first_path;
	gsize      first_offset;
	gboolean   first_is_items;
} Trim;

static void
trim_note(Trim *trim, const gchar *path, gsize kept, gsize total,
          gboolean items)
{
	g_autofree gchar *kept_text = format_count(kept);
	g_autofree gchar *total_text = format_count(total);

	if (trim->first_path == NULL) {
		trim->first_path = g_strdup(path);
		trim->first_offset = kept;
		trim->first_is_items = items;
	}

	if (trim->notes->len >= MAX_NOTES)
		return;

	g_ptr_array_add(trim->notes,
	                g_strdup_printf("%s: %s of %s %s", path, kept_text,
	                                total_text, items ? "items" : "bytes"));
}

static gsize
node_size(JsonNode *node)
{
	g_autofree gchar *text = json_to_string(node, FALSE);

	return (text != NULL) ? strlen(text) : 0;
}

static gboolean
is_identifier(const gchar *key)
{
	const gchar *p;

	if (key == NULL || *key == '\0' || g_ascii_isdigit(*key))
		return FALSE;

	for (p = key; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_')
			return FALSE;
	}

	return TRUE;
}

static gchar *
path_member(const gchar *path, const gchar *key)
{
	g_autoptr(JsonNode) quoted = NULL;
	g_autofree gchar *text = NULL;

	if (is_identifier(key))
		return g_strdup_printf("%s.%s", path, key);

	quoted = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(quoted, key);
	text = json_to_string(quoted, FALSE);

	return g_strdup_printf("%s[%s]", path, text);
}

/*
 * Shares of @room for items of @sizes, water-filled: an item smaller
 * than an equal share keeps its whole size and gives the rest back, so
 * a short member is never cut to make room nobody needed.
 */
static gsize *
fair_shares(const gsize *sizes, guint n, gsize room)
{
	gsize *shares = g_new0(gsize, MAX(n, 1));
	gboolean *settled = g_new0(gboolean, MAX(n, 1));
	gsize left = room;
	guint open = n;
	gboolean changed = TRUE;
	guint i;

	while (changed && open > 0) {
		gsize share = left / open;

		changed = FALSE;

		for (i = 0; i < n; i++) {
			if (settled[i] || sizes[i] > share)
				continue;

			shares[i] = sizes[i];
			settled[i] = TRUE;
			left -= sizes[i];
			open--;
			changed = TRUE;
		}
	}

	for (i = 0; i < n; i++) {
		if (!settled[i])
			shares[i] = (open > 0) ? left / open : 0;
	}

	g_free(settled);

	return shares;
}

static JsonNode *fit_node(Trim *trim, JsonNode *node, gsize room,
                          const gchar *path);

/*
 * A string that is itself JSON -- a tool that returns its records as
 * text, which is most of them -- cut structurally, so what the agent
 * reads is still JSON it can parse, just shorter.
 */
static gchar *
fit_json_text(Trim *trim, const gchar *text, gsize length, gsize room,
              const gchar *path)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(JsonNode) fitted = NULL;
	const gchar *p = text;
	JsonNode *root;

	while (p < text + length && g_ascii_isspace(*p))
		p++;

	if (p >= text + length || (*p != '{' && *p != '['))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, text, (gssize)length, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if (root == NULL || (!JSON_NODE_HOLDS_OBJECT(root) &&
	                     !JSON_NODE_HOLDS_ARRAY(root)))
		return NULL;

	/*
	 * Into a scratch record of what was cut: if the result still does not
	 * fit -- a number cannot be shortened -- the text is cut as prose and
	 * these notes would describe a cut that never reached the agent.
	 */
	{
		Trim scratch = { NULL, NULL, 0, FALSE };
		gchar *text_out;
		guint i;

		scratch.notes = g_ptr_array_new_with_free_func(g_free);
		fitted = fit_node(&scratch, root, room, path);
		text_out = json_to_string(fitted, FALSE);

		if (strlen(text_out) <= room) {
			if (trim->first_path == NULL && scratch.first_path != NULL) {
				trim->first_path = g_strdup(scratch.first_path);
				trim->first_offset = scratch.first_offset;
				trim->first_is_items = scratch.first_is_items;
			}

			for (i = 0; i < scratch.notes->len &&
			            trim->notes->len < MAX_NOTES; i++)
				g_ptr_array_add(trim->notes,
				                g_strdup(g_ptr_array_index(scratch.notes, i)));
		}

		g_ptr_array_unref(scratch.notes);
		g_free(scratch.first_path);

		return text_out;
	}
}

/* Prose, or JSON that would not parse: cut on a boundary. */
static gchar *
fit_text(Trim *trim, const gchar *text, gsize room, const gchar *path)
{
	gsize length = strlen(text);
	gsize cut;
	gchar *structured;

	if (length <= room)
		return g_strdup(text);

	structured = fit_json_text(trim, text, length, room, path);

	if (structured != NULL && strlen(structured) <= room)
		return structured;

	g_free(structured);

	cut = clawt_mcp_result_safe_cut(text, length, room);
	trim_note(trim, path, cut, length, FALSE);

	return g_strndup(text, cut);
}

static JsonNode *
fit_string(Trim *trim, JsonNode *node, gsize room, const gchar *path)
{
	const gchar *text = json_node_get_string(node);
	gsize length = (text != NULL) ? strlen(text) : 0;
	gsize inner = (room > 2) ? room - 2 : 0;
	gsize cut;
	JsonNode *out;

	/*
	 * A string's room in JSON includes its quotes and escapes, which the
	 * cut cannot see; start from the room less the quotes and shrink by
	 * whatever the escapes cost until it fits.
	 */
	for (;;) {
		g_autofree gchar *kept = NULL;
		gsize size;

		cut = clawt_mcp_result_safe_cut(text, length, inner);
		kept = g_strndup(text != NULL ? text : "", cut);
		out = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(out, kept);
		size = node_size(out);

		if (size <= room || cut == 0)
			break;

		json_node_unref(out);
		inner = (size - room < cut) ? cut - (size - room) : 0;
	}

	if (cut < length)
		trim_note(trim, path, cut, length, FALSE);

	return out;
}

static JsonNode *
fit_array(Trim *trim, JsonNode *node, gsize room, const gchar *path)
{
	JsonArray *array = json_node_get_array(node);
	JsonArray *kept = json_array_new();
	JsonNode *out = json_node_new(JSON_NODE_ARRAY);
	guint length = json_array_get_length(array);
	gsize used = 2;
	guint i;

	for (i = 0; i < length; i++) {
		JsonNode *element = json_array_get_element(array, i);
		gsize size = node_size(element) + (i > 0 ? 1 : 0);

		if (used + size <= room) {
			json_array_add_element(kept, json_node_copy(element));
			used += size;
			continue;
		}

		/*
		 * The first record alone does not fit: cut into it rather than
		 * answer with an empty array, which reads as "there are none".
		 */
		if (i == 0 && room > used + 16) {
			g_autofree gchar *inner = g_strdup_printf("%s[0]", path);

			json_array_add_element(kept, fit_node(trim, element,
			                                      room - used, inner));
			i = 1;
		}

		break;
	}

	if (i < length)
		trim_note(trim, path, json_array_get_length(kept), length, TRUE);

	json_node_take_array(out, kept);

	return out;
}

static JsonNode *
fit_object(Trim *trim, JsonNode *node, gsize room, const gchar *path)
{
	JsonObject *object = json_node_get_object(node);
	g_autoptr(GList) members = json_object_get_members(object);
	JsonObject *kept = json_object_new();
	JsonNode *out = json_node_new(JSON_NODE_OBJECT);
	guint n = g_list_length(members);
	gsize *sizes = g_new0(gsize, MAX(n, 1));
	gsize *shares;
	gsize skeleton = 2;
	GList *l;
	guint i;

	/* What every member costs apart from its value: the key, ':' and ','. */
	for (l = members, i = 0; l != NULL; l = l->next, i++) {
		const gchar *key = l->data;
		g_autoptr(JsonNode) quoted = json_node_new(JSON_NODE_VALUE);

		json_node_set_string(quoted, key);
		skeleton += node_size(quoted) + 2;
		sizes[i] = node_size(json_object_get_member(object, key));
	}

	shares = fair_shares(sizes, n, (room > skeleton) ? room - skeleton : 0);

	for (l = members, i = 0; l != NULL; l = l->next, i++) {
		const gchar *key = l->data;
		JsonNode *value = json_object_get_member(object, key);
		g_autofree gchar *inner = path_member(path, key);

		if (sizes[i] <= shares[i])
			json_object_set_member(kept, key, json_node_copy(value));
		else
			json_object_set_member(kept, key,
			                       fit_node(trim, value, shares[i], inner));
	}

	g_free(sizes);
	g_free(shares);
	json_node_take_object(out, kept);

	return out;
}

static JsonNode *
fit_node(Trim *trim, JsonNode *node, gsize room, const gchar *path)
{
	if (node_size(node) <= room)
		return json_node_copy(node);

	switch (json_node_get_node_type(node)) {
	case JSON_NODE_OBJECT:
		return fit_object(trim, node, room, path);

	case JSON_NODE_ARRAY:
		return fit_array(trim, node, room, path);

	case JSON_NODE_VALUE:
		if (json_node_get_value_type(node) == G_TYPE_STRING)
			return fit_string(trim, node, room, path);

		/* A number or a boolean cannot be made shorter. */
		return json_node_copy(node);

	case JSON_NODE_NULL:
		return json_node_copy(node);
	}

	return json_node_copy(node);
}

/* What the model reads of a content block: a text's own bytes, else its JSON. */
static gsize
block_size(JsonNode *block)
{
	JsonObject *object;

	if (!JSON_NODE_HOLDS_OBJECT(block))
		return node_size(block);

	object = json_node_get_object(block);

	if (g_strcmp0(json_object_get_string_member_with_default(object, "type",
	                                                         ""),
	              "text") == 0 &&
	    json_object_has_member(object, "text") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(object, "text")))
		return strlen(json_object_get_string_member_with_default(object,
		                                                         "text", ""));

	return node_size(block);
}

static gsize
result_size(JsonObject *result)
{
	gsize size = 0;

	if (json_object_has_member(result, "content") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "content"))) {
		JsonArray *content = json_object_get_array_member(result, "content");
		guint i;

		for (i = 0; i < json_array_get_length(content); i++)
			size += block_size(json_array_get_element(content, i));
	}

	if (json_object_has_member(result, "structuredContent"))
		size += node_size(json_object_get_member(result,
		                                         "structuredContent"));

	return size;
}

/*
 * A new object holding the same members.  json_node_copy() shares an
 * object rather than copying it, so setting a member on a "copy" would
 * change the server's message underneath whoever else holds it.
 */
static JsonObject *
object_clone(JsonObject *object)
{
	JsonObject *clone = json_object_new();
	g_autoptr(GList) members = json_object_get_members(object);
	GList *l;

	for (l = members; l != NULL; l = l->next)
		json_object_set_member(clone, l->data, json_node_copy(
			json_object_get_member(object, l->data)));

	return clone;
}

static JsonNode *
text_block(const gchar *text)
{
	JsonObject *block = json_object_new();
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);

	json_object_set_string_member(block, "type", "text");
	json_object_set_string_member(block, "text", text);
	json_node_take_object(node, block);

	return node;
}

/*
 * One content block within @share.  Text is cut; a text resource has its
 * text cut; anything else -- an image, an audio clip, a blob -- cannot
 * be shortened without becoming garbage, so it stays if it fits and is
 * otherwise named in a sentence instead.
 */
static JsonNode *
fit_block(Trim *trim, JsonNode *block, gsize share, guint index)
{
	JsonObject *object;
	const gchar *type;
	g_autofree gchar *path = g_strdup_printf(".content[%u]", index);

	if (!JSON_NODE_HOLDS_OBJECT(block))
		return fit_node(trim, block, share, path);

	object = json_node_get_object(block);
	type = json_object_get_string_member_with_default(object, "type", "");

	if (g_strcmp0(type, "text") == 0 &&
	    json_object_has_member(object, "text")) {
		g_autofree gchar *text_path = g_strconcat(path, ".text", NULL);
		g_autofree gchar *fitted = fit_text(
			trim, json_object_get_string_member_with_default(object, "text",
			                                                 ""),
			share, text_path);
		g_autofree gchar *escaped = clawt_preamble_escape(fitted);

		return text_block(escaped);
	}

	if (g_strcmp0(type, "resource") == 0 &&
	    json_object_has_member(object, "resource") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "resource"))) {
		JsonObject *resource = json_object_get_object_member(object,
		                                                     "resource");

		if (json_object_has_member(resource, "text")) {
			g_autofree gchar *text_path = g_strconcat(path, ".resource.text",
			                                          NULL);
			g_autofree gchar *fitted = fit_text(
				trim,
				json_object_get_string_member_with_default(resource, "text",
				                                           ""),
				share, text_path);
			g_autofree gchar *escaped = clawt_preamble_escape(fitted);
			JsonObject *outer = object_clone(object);
			JsonObject *inner = object_clone(resource);
			JsonNode *copy = json_node_new(JSON_NODE_OBJECT);

			json_object_set_string_member(inner, "text", escaped);
			json_object_set_object_member(outer, "resource", inner);
			json_node_take_object(copy, outer);

			return copy;
		}
	}

	if (node_size(block) <= share)
		return json_node_copy(block);

	{
		const gchar *mime = json_object_get_string_member_with_default(
			object, "mimeType", NULL);
		g_autofree gchar *size = format_count(node_size(block));
		g_autofree gchar *sentence = g_strdup_printf(
			"[clawtilla] A %s block%s%s%s of %s bytes was left out to keep "
			"this result within its budget; it is in the held result at %s.",
			*type != '\0' ? type : "content",
			mime != NULL ? " (" : "", mime != NULL ? mime : "",
			mime != NULL ? ")" : "", size, path);

		trim_note(trim, path, 0, node_size(block), FALSE);

		return text_block(sentence);
	}
}

static gchar *
example_query(Trim *trim, const gchar *result_id)
{
	if (trim->first_path == NULL)
		return g_strdup_printf("{\"result_id\": \"%s\", \"search\": "
		                       "\"<text>\"}", result_id);

	return g_strdup_printf("{\"result_id\": \"%s\", \"path\": \"%s\", "
	                       "\"offset\": %" G_GSIZE_FORMAT "}",
	                       result_id, trim->first_path, trim->first_offset);
}

/* "from github's list_issues", or as much of that as is known. */
static gchar *
describe_source(const gchar *connector, const gchar *tool)
{
	if (connector != NULL && tool != NULL)
		return g_strdup_printf(" from %s's %s", connector, tool);

	if (connector != NULL || tool != NULL)
		return g_strdup_printf(" from %s", connector != NULL ? connector
		                                                     : tool);

	return g_strdup("");
}

static gchar *
header_note(Trim *trim, gsize original, gsize kept, gsize budget,
            const gchar *connector, const gchar *tool, const gchar *result_id)
{
	GString *out = g_string_new("[clawtilla] TRIMMED: ");
	g_autofree gchar *original_text = format_count(original);
	g_autofree gchar *kept_text = format_count(kept);
	g_autofree gchar *budget_text = format_count(budget);
	g_autofree gchar *source = describe_source(connector, tool);
	guint i;

	g_string_append_printf(out, "this result%s was %s bytes, over the "
	                       "%s-byte budget, so only %s bytes of it are shown",
	                       source, original_text, budget_text, kept_text);

	if (trim->notes->len > 0) {
		g_string_append(out, " (");

		for (i = 0; i < trim->notes->len; i++) {
			if (i > 0)
				g_string_append(out, "; ");

			g_string_append(out, g_ptr_array_index(trim->notes, i));
		}

		g_string_append(out, ")");
	}

	g_string_append(out, ". The rest was NOT shown -- do not assume you "
	                     "have seen everything. ");

	if (result_id != NULL) {
		g_autofree gchar *example = example_query(trim, result_id);

		g_string_append_printf(out,
		                       "The whole result is held as %s. Read the "
		                       "part you need with " CLAWT_MCP_RESULT_QUERY_TOOL
		                       " -- %s continues where this stops; \"search\" "
		                       "finds text in it -- rather than calling the "
		                       "tool again.", result_id, example);
	} else {
		g_string_append(out, "The whole result could not be kept, so narrow "
		                     "the call -- a filter, a page, a smaller query -- "
		                     "to see the rest.");
	}

	return g_string_free(out, FALSE);
}

static gchar *
footer_note(gsize original, gsize kept, const gchar *result_id)
{
	g_autofree gchar *original_text = format_count(original);
	g_autofree gchar *kept_text = format_count(kept);

	if (result_id != NULL)
		return g_strdup_printf("[clawtilla] END OF TRIMMED RESULT: %s of %s "
		                       "bytes shown. The rest is held as %s; read it "
		                       "with " CLAWT_MCP_RESULT_QUERY_TOOL " (see the "
		                       "note at the start).", kept_text,
		                       original_text, result_id);

	return g_strdup_printf("[clawtilla] END OF TRIMMED RESULT: %s of %s bytes "
	                       "shown; the rest was not kept (see the note at the "
	                       "start).", kept_text, original_text);
}

JsonObject *
clawt_mcp_result_trim(
	JsonObject  *result,
	gsize        budget,
	gsize        original_bytes,
	const gchar *connector,
	const gchar *tool,
	const gchar *result_id,
	gsize       *out_kept
){
	g_autoptr(JsonObject) trimmed = NULL;
	JsonArray *content = NULL;
	JsonArray *fitted;
	g_autofree gchar *header = NULL;
	g_autofree gchar *footer = NULL;
	Trim trim = { NULL, NULL, 0, FALSE };
	gsize content_size = 0;
	gsize structured_size = 0;
	gsize content_room;
	gsize structured_room;
	gsize kept;
	guint n;
	guint i;

	g_return_val_if_fail(result != NULL, NULL);

	if (out_kept != NULL)
		*out_kept = 0;

	if (budget == 0 || result_size(result) <= budget)
		return NULL;

	trim.notes = g_ptr_array_new_with_free_func(g_free);

	trimmed = object_clone(result);

	if (json_object_has_member(trimmed, "content") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(trimmed, "content")))
		content = json_object_get_array_member(trimmed, "content");

	n = (content != NULL) ? json_array_get_length(content) : 0;

	for (i = 0; i < n; i++)
		content_size += block_size(json_array_get_element(content, i));

	if (json_object_has_member(trimmed, "structuredContent"))
		structured_size = node_size(json_object_get_member(
			trimmed, "structuredContent"));

	/*
	 * The two halves are usually the same data twice -- the records as
	 * text and as structuredContent -- so neither gets the whole budget.
	 * One that fits in half keeps its size and the other gets the rest.
	 */
	if (structured_size == 0) {
		content_room = budget;
		structured_room = 0;
	} else if (n == 0) {
		content_room = 0;
		structured_room = budget;
	} else if (structured_size <= budget / 2) {
		structured_room = structured_size;
		content_room = budget - structured_size;
	} else if (content_size <= budget / 2) {
		content_room = content_size;
		structured_room = budget - content_size;
	} else {
		content_room = budget / 2;
		structured_room = budget - content_room;
	}

	fitted = json_array_new();

	if (n > 0) {
		gsize *sizes = g_new0(gsize, n);
		gsize *shares;

		for (i = 0; i < n; i++)
			sizes[i] = block_size(json_array_get_element(content, i));

		shares = fair_shares(sizes, n, content_room);

		for (i = 0; i < n; i++)
			json_array_add_element(fitted,
			                       fit_block(&trim,
			                                 json_array_get_element(content, i),
			                                 shares[i], i));

		g_free(sizes);
		g_free(shares);
	}

	if (structured_size > 0) {
		JsonNode *structured = json_object_get_member(trimmed,
		                                              "structuredContent");

		json_object_set_member(trimmed, "structuredContent",
		                       fit_node(&trim, structured, structured_room,
		                                ".structuredContent"));
	}

	json_object_set_array_member(trimmed, "content", fitted);
	kept = result_size(trimmed);

	/* Both ends: a quiet note at the bottom reads as the end of the data. */
	header = header_note(&trim, original_bytes, kept, budget, connector, tool,
	                     result_id);
	footer = footer_note(original_bytes, kept, result_id);

	{
		JsonArray *framed = json_array_new();

		json_array_add_element(framed, text_block(header));

		for (i = 0; i < json_array_get_length(fitted); i++)
			json_array_add_element(framed, json_node_copy(
				json_array_get_element(fitted, i)));

		json_array_add_element(framed, text_block(footer));
		json_object_set_array_member(trimmed, "content", framed);
	}

	if (out_kept != NULL)
		*out_kept = kept;

	g_ptr_array_unref(trim.notes);
	g_free(trim.first_path);

	return (JsonObject *)g_steal_pointer(&trimmed);
}

static gchar *
render_line(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

	json_node_set_object(node, object);

	return json_to_string(node, FALSE);
}

gchar *
clawt_mcp_result_oversized_reply(
	const gchar *id_json,
	gsize        total,
	const gchar *head,
	gsize        head_length,
	gsize        budget,
	const gchar *connector,
	const gchar *tool,
	const gchar *result_id
){
	g_autoptr(JsonObject) reply = json_object_new();
	JsonObject *result = json_object_new();
	JsonArray *content = json_array_new();
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *shown = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *total_text = format_count(total);
	g_autofree gchar *kept_text = NULL;
	g_autofree gchar *header = NULL;
	g_autofree gchar *footer = NULL;
	g_autofree gchar *source = describe_source(connector, tool);
	gsize room = (budget > 0) ? budget : CLAWT_MCP_RESULT_MIN_BUDGET;
	gsize cut;

	cut = clawt_mcp_result_safe_cut(head, head_length, room);
	shown = g_strndup(head, cut);

	/* Raw bytes off the wire, so not necessarily UTF-8: make it so. */
	if (!g_utf8_validate(shown, -1, NULL)) {
		gchar *valid = g_utf8_make_valid(shown, -1);

		g_free(shown);
		shown = valid;
	}

	escaped = clawt_preamble_escape(shown);
	kept_text = format_count(cut);

	header = g_strdup_printf(
		"[clawtilla] TRIMMED: this result%s was %s bytes, more than "
		"clawtilla reads into memory, so it was never parsed. What follows "
		"is its first %s bytes of raw JSON. The rest was NOT shown -- do not "
		"assume you have seen everything. %s",
		source, total_text, kept_text,
		result_id != NULL
			? "The whole of it is held; read it with "
			  CLAWT_MCP_RESULT_QUERY_TOOL " using \"search\" or "
			  "\"offset\" (a path cannot be followed into a result this "
			  "big), or narrow the call."
			: "It could not be kept, so narrow the call to see the rest.");
	footer = footer_note(total, cut, result_id);

	json_array_add_element(content, text_block(header));
	json_array_add_element(content, text_block(escaped));
	json_array_add_element(content, text_block(footer));
	json_object_set_array_member(result, "content", content);

	json_object_set_string_member(reply, "jsonrpc", "2.0");

	if (id_json != NULL && json_parser_load_from_data(parser, id_json, -1,
	                                                  NULL) &&
	    json_parser_get_root(parser) != NULL)
		json_object_set_member(reply, "id",
		                       json_node_copy(json_parser_get_root(parser)));
	else
		json_object_set_null_member(reply, "id");

	json_object_set_object_member(reply, "result", result);

	return render_line(reply);
}

gchar *
clawt_mcp_result_notice(
	const gchar *connector,
	const gchar *tool,
	gsize        original_bytes,
	gsize        kept_bytes,
	const gchar *result_id
){
	g_autofree gchar *original = format_count(original_bytes);
	g_autofree gchar *kept = format_count(kept_bytes);
	g_autofree gchar *what = NULL;

	if (connector != NULL && tool != NULL)
		what = g_strdup_printf("%s's %s", connector, tool);
	else
		what = g_strdup(tool != NULL ? tool
		                : connector != NULL ? connector : "a connector");

	if (result_id != NULL)
		return g_strdup_printf(CLAWT_MCP_RESULT_NOTICE_PREFIX " %s returned %s bytes; the "
		                       "agent was shown %s, and the whole result is "
		                       "held as %s for it to query.", what, original,
		                       kept, result_id);

	return g_strdup_printf(CLAWT_MCP_RESULT_NOTICE_PREFIX " %s returned %s bytes; the agent "
	                       "was shown %s, and the rest was not kept.", what,
	                       original, kept);
}

gboolean
clawt_mcp_result_is_notice(const gchar *text)
{
	return text != NULL && g_str_has_prefix(text,
	                                        CLAWT_MCP_RESULT_NOTICE_PREFIX);
}

/* ── Reading a held result ───────────────────────────────────────── */

typedef enum {
	SEGMENT_KEY,
	SEGMENT_INDEX,
	SEGMENT_SLICE
} SegmentKind;

typedef struct {
	SegmentKind  kind;
	gchar       *key;
	gint64       start;
	gint64       end;
	gboolean     has_start;
	gboolean     has_end;
} Segment;

static void
segment_free(gpointer data)
{
	Segment *segment = data;

	g_free(segment->key);
	g_free(segment);
}

/*
 * A jq-like path: `.a.b`, `[3]`, `[-1]`, `[2:5]`, `["odd key"]`.  Small
 * on purpose -- enough to point at a part of a result, not a language.
 */
static GPtrArray *
parse_path(const gchar *path, gchar **error)
{
	g_autoptr(GPtrArray) segments = g_ptr_array_new_with_free_func(
		segment_free);
	const gchar *p = (path != NULL) ? path : "";

	while (*p == ' ')
		p++;

	while (*p != '\0') {
		Segment *segment = g_new0(Segment, 1);

		if (*p == '.') {
			const gchar *start = ++p;

			if (*p == '\0' || *p == '[') {
				g_free(segment);
				continue;
			}

			while (*p != '\0' && *p != '.' && *p != '[')
				p++;

			segment->kind = SEGMENT_KEY;
			segment->key = g_strndup(start, (gsize)(p - start));
		} else if (*p == '[') {
			const gchar *close;

			p++;

			if (*p == '"') {
				GString *key = g_string_new(NULL);

				p++;

				while (*p != '\0' && *p != '"') {
					if (*p == '\\' && p[1] != '\0')
						p++;

					g_string_append_c(key, *p);
					p++;
				}

				if (*p != '"' || p[1] != ']') {
					g_string_free(key, TRUE);
					g_free(segment);
					*error = g_strdup("a quoted key needs a closing \" and ]");
					return NULL;
				}

				p += 2;
				segment->kind = SEGMENT_KEY;
				segment->key = g_string_free(key, FALSE);
				g_ptr_array_add(segments, segment);
				continue;
			}

			close = strchr(p, ']');

			if (close == NULL) {
				g_free(segment);
				*error = g_strdup("a [ with no ]");
				return NULL;
			}

			{
				g_autofree gchar *inside = g_strndup(p, (gsize)(close - p));
				gchar *colon = strchr(inside, ':');
				gchar *end = NULL;

				if (colon != NULL) {
					*colon = '\0';
					segment->kind = SEGMENT_SLICE;

					if (*g_strstrip(inside) != '\0') {
						segment->start = g_ascii_strtoll(inside, &end, 10);
						segment->has_start = (end != NULL && *end == '\0');

						if (!segment->has_start) {
							g_free(segment);
							*error = g_strdup("a slice takes whole numbers, "
							                  "as in [20:40]");
							return NULL;
						}
					}

					if (*g_strstrip(colon + 1) != '\0') {
						segment->end = g_ascii_strtoll(colon + 1, &end, 10);
						segment->has_end = (end != NULL && *end == '\0');

						if (!segment->has_end) {
							g_free(segment);
							*error = g_strdup("a slice takes whole numbers, "
							                  "as in [20:40]");
							return NULL;
						}
					}
				} else {
					segment->kind = SEGMENT_INDEX;
					segment->start = g_ascii_strtoll(g_strstrip(inside), &end,
					                                 10);

					if (end == NULL || *end != '\0' || *inside == '\0') {
						g_free(segment);
						*error = g_strdup_printf("[%s] is not an index; "
						                         "quote a key as [\"key\"]",
						                         inside);
						return NULL;
					}
				}
			}

			p = close + 1;
		} else {
			/* A bare first key, as in `content[0]`. */
			const gchar *start = p;

			while (*p != '\0' && *p != '.' && *p != '[')
				p++;

			segment->kind = SEGMENT_KEY;
			segment->key = g_strndup(start, (gsize)(p - start));
		}

		g_ptr_array_add(segments, segment);
	}

	return (GPtrArray *)g_steal_pointer(&segments);
}

/* A string that is JSON, parsed so a path can go on into it. */
static JsonNode *
parse_string_node(JsonNode *node)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	const gchar *text = json_node_get_string(node);
	JsonNode *root;

	if (text == NULL || strlen(text) > CLAWT_MCP_RESULT_HOLD_MAX)
		return NULL;

	if (!json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	return (root != NULL) ? json_node_copy(root) : NULL;
}

static gint64
resolve_index(gint64 index, guint length)
{
	return (index < 0) ? (gint64)length + index : index;
}

/*
 * Follows @segments from @root.  Every node handed back is owned by the
 * caller, because descending into a string makes a tree that belongs to
 * nobody else.
 */
static JsonNode *
follow(JsonNode *root, GPtrArray *segments, gchar **error)
{
	g_autoptr(JsonNode) current = json_node_copy(root);
	guint i;

	for (i = 0; i < segments->len; i++) {
		Segment *segment = g_ptr_array_index(segments, i);
		JsonNode *next = NULL;

		if (JSON_NODE_HOLDS_VALUE(current) &&
		    json_node_get_value_type(current) == G_TYPE_STRING) {
			JsonNode *parsed = parse_string_node(current);

			if (parsed == NULL) {
				*error = g_strdup("that is a string of text, not JSON, so a "
				                  "path cannot go into it; use offset, limit "
				                  "or search on it instead");
				return NULL;
			}

			json_node_unref(current);
			current = parsed;
		}

		switch (segment->kind) {
		case SEGMENT_KEY:
			if (!JSON_NODE_HOLDS_OBJECT(current) ||
			    !json_object_has_member(json_node_get_object(current),
			                            segment->key)) {
				*error = g_strdup_printf("there is no member \"%s\" there",
				                         segment->key);
				return NULL;
			}

			next = json_node_copy(json_object_get_member(
				json_node_get_object(current), segment->key));
			break;

		case SEGMENT_INDEX: {
			JsonArray *array;
			gint64 index;

			if (!JSON_NODE_HOLDS_ARRAY(current)) {
				*error = g_strdup("an index needs an array there");
				return NULL;
			}

			array = json_node_get_array(current);
			index = resolve_index(segment->start,
			                      json_array_get_length(array));

			if (index < 0 || index >= (gint64)json_array_get_length(array)) {
				*error = g_strdup_printf("index %" G_GINT64_FORMAT " is past "
				                         "the %u items there", segment->start,
				                         json_array_get_length(array));
				return NULL;
			}

			next = json_node_copy(json_array_get_element(array,
			                                             (guint)index));
			break;
		}

		case SEGMENT_SLICE: {
			JsonArray *array;
			JsonArray *slice;
			guint length;
			gint64 start;
			gint64 end;
			gint64 k;

			if (!JSON_NODE_HOLDS_ARRAY(current)) {
				*error = g_strdup("a slice needs an array there");
				return NULL;
			}

			array = json_node_get_array(current);
			length = json_array_get_length(array);
			start = segment->has_start
				? resolve_index(segment->start, length) : 0;
			end = segment->has_end ? resolve_index(segment->end, length)
			                       : (gint64)length;
			start = CLAMP(start, 0, (gint64)length);
			end = CLAMP(end, start, (gint64)length);
			slice = json_array_new();

			for (k = start; k < end; k++)
				json_array_add_element(slice, json_node_copy(
					json_array_get_element(array, (guint)k)));

			next = json_node_new(JSON_NODE_ARRAY);
			json_node_take_array(next, slice);
			break;
		}
		}

		json_node_unref(current);
		current = next;
	}

	return (JsonNode *)g_steal_pointer(&current);
}

/* Case-insensitive (ASCII) search, from @from. */
static const gchar *
find_ascii_nocase(const gchar *haystack, gsize length, gsize from,
                  const gchar *needle)
{
	gsize n = strlen(needle);
	gsize i;

	if (n == 0 || n > length)
		return NULL;

	for (i = from; i + n <= length; i++) {
		if (g_ascii_strncasecmp(haystack + i, needle, n) == 0)
			return haystack + i;
	}

	return NULL;
}

/* @text[@start, @end) widened or narrowed to character boundaries. */
static gchar *
slice_text(const gchar *text, gsize length, gsize start, gsize end,
           gsize *out_start, gsize *out_end)
{
	gsize cut;

	start = MIN(start, length);
	end = MIN(MAX(end, start), length);

	/* The start moves forward to a boundary, so it never begins mid-character. */
	while (start < length && ((guchar)text[start] & 0xC0) == 0x80)
		start++;

	cut = (end > start)
		? start + clawt_mcp_result_safe_cut(text + start, length - start,
		                                    end - start)
		: start;

	if (out_start != NULL)
		*out_start = start;

	if (out_end != NULL)
		*out_end = cut;

	return g_strndup(text + start, cut - start);
}

static void
append_quoted(GString *out, const gchar *text)
{
	g_autofree gchar *escaped = clawt_preamble_escape(text);

	g_string_append(out, escaped);
}

static void
answer_string(GString *out, const gchar *text, const gchar *where,
              const gchar *search, gint64 offset, gint64 limit, gsize cap)
{
	gsize length = strlen(text);
	g_autofree gchar *length_text = format_count(length);

	if (search != NULL && *search != '\0') {
		gsize from = (offset > 0) ? (gsize)offset : 0;
		guint shown = 0;
		guint found = 0;
		guint most = (limit > 0) ? (guint)MIN(limit, SEARCH_MAX_MATCHES)
		                         : SEARCH_MAX_MATCHES;
		const gchar *at;

		g_string_append_printf(out, "Searching %s (%s bytes) for \"%s\":\n",
		                       where, length_text, search);

		while ((at = find_ascii_nocase(text, length, from, search)) != NULL) {
			gsize position = (gsize)(at - text);

			found++;

			if (shown < most && out->len < cap) {
				gsize start = (position > SEARCH_CONTEXT)
					? position - SEARCH_CONTEXT : 0;
				g_autofree gchar *context = slice_text(
					text, length, start,
					position + strlen(search) + SEARCH_CONTEXT, NULL, NULL);

				g_string_append_printf(out, "\n-- at byte %" G_GSIZE_FORMAT
				                       ":\n", position);
				append_quoted(out, context);
				g_string_append_c(out, '\n');
				shown++;
			}

			from = position + 1;
		}

		if (found == 0)
			g_string_append(out, "\nNo matches.");
		else if (found > shown)
			g_string_append_printf(out, "\n%u matches; %u shown. Pass "
			                       "\"offset\" past the last one shown for "
			                       "more.", found, shown);
		else
			g_string_append_printf(out, "\n%u match%s.", found,
			                       found == 1 ? "" : "es");
		return;
	}

	{
		gsize want = (limit > 0) ? (gsize)limit : cap;
		gsize start = 0;
		gsize end = 0;
		g_autofree gchar *part = NULL;
		g_autofree gchar *start_text = NULL;
		g_autofree gchar *end_text = NULL;

		want = MIN(want, cap);
		part = slice_text(text, length, (offset > 0) ? (gsize)offset : 0,
		                  ((offset > 0) ? (gsize)offset : 0) + want, &start,
		                  &end);
		start_text = format_count(start);
		end_text = format_count(end);

		g_string_append_printf(out, "Bytes %s to %s of %s of %s%s\n\n",
		                       start_text, end_text, length_text, where,
		                       end < length ? ":" : " (the end):");
		append_quoted(out, part);

		if (end < length)
			g_string_append_printf(out, "\n\n[clawtilla] More follows: "
			                       "\"offset\": %" G_GSIZE_FORMAT " continues "
			                       "from here.", end);
	}
}

static void
answer_array(GString *out, JsonArray *array, const gchar *where,
             const gchar *search, gint64 offset, gint64 limit, gsize cap)
{
	guint length = json_array_get_length(array);
	guint start = (offset > 0) ? (guint)MIN(offset, (gint64)length) : 0;
	guint most = (limit > 0) ? (guint)MIN(limit, (gint64)G_MAXUINT) : 20;
	guint shown = 0;
	guint matched = 0;
	guint next = length;
	guint i;

	g_string_append_printf(out, "%s is an array of %u items%s%s%s:\n", where,
	                       length,
	                       (search != NULL && *search != '\0')
	                           ? "; those containing \"" : "",
	                       (search != NULL && *search != '\0') ? search : "",
	                       (search != NULL && *search != '\0') ? "\"" : "");

	for (i = start; i < length; i++) {
		g_autofree gchar *json = json_to_string(json_array_get_element(array,
		                                                               i),
		                                        FALSE);

		if (search != NULL && *search != '\0' &&
		    find_ascii_nocase(json, strlen(json), 0, search) == NULL)
			continue;

		matched++;

		if (shown >= most || out->len >= cap) {
			next = i;
			break;
		}

		g_string_append_printf(out, "[%u] ", i);

		/* One item bigger than what is left is cut, and says how to read it. */
		if (out->len + strlen(json) > cap) {
			gsize room = (cap > out->len) ? cap - out->len : 0;
			gsize cut = clawt_mcp_result_safe_cut(json, strlen(json), room);
			g_autofree gchar *part = g_strndup(json, cut);

			append_quoted(out, part);
			g_string_append_printf(out, "\n[clawtilla] Item %u was cut; give "
			                       "its path to read it whole.\n", i);
		} else {
			append_quoted(out, json);
			g_string_append_c(out, '\n');
		}

		shown++;
	}

	if (next < length)
		g_string_append_printf(out, "\n[clawtilla] More follows: \"offset\": "
		                       "%u continues from here.", next);
	else if (matched == 0)
		g_string_append(out, "\nNothing there.");
}

static void
answer_object(GString *out, JsonObject *object, const gchar *where,
              const gchar *search, gsize cap)
{
	g_autoptr(GList) members = json_object_get_members(object);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	GList *l;

	json_node_set_object(node, object);
	json = json_to_string(node, FALSE);

	if ((search == NULL || *search == '\0') && strlen(json) + out->len <= cap) {
		g_string_append_printf(out, "%s:\n", where);
		append_quoted(out, json);
		return;
	}

	/*
	 * Too big to show, or searched: its members one per line, with each
	 * one's size and kind, so the next call can name the part it wants.
	 */
	g_string_append_printf(out, "%s is an object of %u members%s%s%s:\n",
	                       where, g_list_length(members),
	                       (search != NULL && *search != '\0')
	                           ? "; those containing \"" : "",
	                       (search != NULL && *search != '\0') ? search : "",
	                       (search != NULL && *search != '\0') ? "\"" : "");

	for (l = members; l != NULL && out->len < cap; l = l->next) {
		const gchar *key = l->data;
		JsonNode *value = json_object_get_member(object, key);
		g_autofree gchar *value_json = json_to_string(value, FALSE);
		g_autofree gchar *size = format_count(strlen(value_json));
		const gchar *kind = JSON_NODE_HOLDS_OBJECT(value) ? "object"
			: JSON_NODE_HOLDS_ARRAY(value) ? "array"
			: JSON_NODE_HOLDS_NULL(value) ? "null"
			: json_node_get_value_type(value) == G_TYPE_STRING ? "string"
			: "value";

		if (search != NULL && *search != '\0' &&
		    find_ascii_nocase(key, strlen(key), 0, search) == NULL &&
		    find_ascii_nocase(value_json, strlen(value_json), 0,
		                      search) == NULL)
			continue;

		g_string_append(out, "  ");
		append_quoted(out, key);
		g_string_append_printf(out, ": %s, %s bytes", kind, size);

		if (JSON_NODE_HOLDS_ARRAY(value))
			g_string_append_printf(out, ", %u items",
			                       json_array_get_length(
			                           json_node_get_array(value)));

		g_string_append_c(out, '\n');
	}
}

static JsonObject *
load_meta(const gchar *dir, const gchar *id)
{
	g_autofree gchar *path = meta_path(dir, id);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (path == NULL || !json_parser_load_from_file(parser, path, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_object_ref(json_node_get_object(root));
}

/*
 * A result too big to parse: searched and paged as raw bytes, a chunk at
 * a time, so the query costs what it shows and not what was held.
 */
static void
answer_raw(GString *out, const gchar *path, gsize size, const gchar *search,
           gint64 offset, gint64 limit, gsize cap)
{
	FILE *stream = fopen(path, "rb");
	g_autofree gchar *size_text = format_count(size);

	/*
	 * Plain stdio rather than GFile: a GFile goes through the session's
	 * VFS, which loads gvfs and may reach for a bus -- in a daemon and in
	 * a hermetic test alike, for a local file.
	 */
	if (stream == NULL) {
		g_string_append(out, "The held result could not be read.");
		return;
	}

	if (search != NULL && *search != '\0') {
		gsize chunk = 1024 * 1024;
		gsize overlap = strlen(search) + SEARCH_CONTEXT * 2;
		g_autofree gchar *buffer = g_malloc(chunk + overlap + 1);
		gsize carried = 0;
		gsize base = 0;
		guint found = 0;
		guint shown = 0;

		g_string_append_printf(out, "Searching the raw JSON (%s bytes) for "
		                       "\"%s\":\n", size_text, search);

		for (;;) {
			gsize got = fread(buffer + carried, 1, chunk, stream);
			gsize have;
			gsize from = 0;
			const gchar *at;

			if (got == 0)
				break;

			have = carried + (gsize)got;
			buffer[have] = '\0';

			while ((at = find_ascii_nocase(buffer, have, from, search))
			       != NULL) {
				gsize position = (gsize)(at - buffer);

				/* Seen in the overlap of the last chunk already. */
				if (position + strlen(search) <= carried &&
				    base > 0) {
					from = position + 1;
					continue;
				}

				found++;

				if (base + position >= (gsize)MAX(offset, 0) &&
				    shown < SEARCH_MAX_MATCHES && out->len < cap) {
					gsize start = (position > SEARCH_CONTEXT)
						? position - SEARCH_CONTEXT : 0;
					g_autofree gchar *context = slice_text(
						buffer, have, start,
						position + strlen(search) + SEARCH_CONTEXT, NULL,
						NULL);
					g_autofree gchar *valid = g_utf8_make_valid(context, -1);

					g_string_append_printf(out, "\n-- at byte %"
					                       G_GSIZE_FORMAT ":\n",
					                       base + position);
					append_quoted(out, valid);
					g_string_append_c(out, '\n');
					shown++;
				}

				from = position + 1;
			}

			/* Keep the tail, so a match across the boundary is found. */
			carried = MIN(overlap, have);
			memmove(buffer, buffer + have - carried, carried);
			base += have - carried;
		}

		if (found == 0)
			g_string_append(out, "\nNo matches.");
		else
			g_string_append_printf(out, "\n%u match%s; %u shown.", found,
			                       found == 1 ? "" : "es", shown);

		fclose(stream);
		return;
	}

	{
		gsize start = (offset > 0) ? MIN((gsize)offset, size) : 0;
		gsize want = MIN((limit > 0) ? (gsize)limit : cap, cap);
		g_autofree gchar *buffer = g_malloc(want + 8);
		gsize got = 0;
		gsize cut;
		g_autofree gchar *part = NULL;
		g_autofree gchar *valid = NULL;
		g_autofree gchar *start_text = NULL;
		g_autofree gchar *end_text = NULL;

		if (fseeko(stream, (off_t)start, SEEK_SET) != 0) {
			fclose(stream);
			g_string_append(out, "The held result could not be read.");
			return;
		}

		got = fread(buffer, 1, want, stream);
		fclose(stream);

		/* Off a boundary at the start, then cut safely at the end. */
		while (got > 0 && ((guchar)buffer[0] & 0xC0) == 0x80) {
			memmove(buffer, buffer + 1, --got);
			start++;
		}

		/*
		 * More follows, so the last byte read may begin a character or
		 * an escape the next read would finish: cut short of it.
		 */
		cut = (start + got < size && got > 0)
			? clawt_mcp_result_safe_cut(buffer, got, got - 1) : got;

		part = g_strndup(buffer, cut);
		valid = g_utf8_make_valid(part, -1);
		start_text = format_count(start);
		end_text = format_count(start + cut);

		g_string_append_printf(out, "Bytes %s to %s of %s of the raw JSON:\n\n",
		                       start_text, end_text, size_text);
		append_quoted(out, valid);

		if (start + cut < size)
			g_string_append_printf(out, "\n\n[clawtilla] More follows: "
			                       "\"offset\": %" G_GSIZE_FORMAT " continues "
			                       "from here.", start + cut);
	}
}

gchar *
clawt_mcp_result_query(
	const gchar *dir,
	const gchar *result_id,
	const gchar *path,
	const gchar *search,
	gint64       offset,
	gint64       limit,
	gboolean    *is_error
){
	g_autofree gchar *file = NULL;
	g_autoptr(JsonObject) meta = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GPtrArray) segments = NULL;
	g_autoptr(JsonNode) selected = NULL;
	g_autofree gchar *error = NULL;
	g_autofree gchar *where = NULL;
	GString *out;
	GStatBuf st;
	JsonNode *root;
	gsize cap = DEFAULT_QUERY_CAP;
	gboolean has_path = (path != NULL && *path != '\0' &&
	                     g_strcmp0(path, ".") != 0);

	*is_error = TRUE;

	file = clawt_mcp_result_path(dir, result_id);

	/*
	 * Not there, not well formed and not this agent's are one answer.
	 * Telling them apart would let one agent find out what another's
	 * tools returned by asking for ids until the message changed.
	 */
	if (file == NULL || g_stat(file, &st) != 0 || !S_ISREG(st.st_mode))
		return g_strdup_printf("There is no held result '%s' for you. A "
		                       "held result is kept for a day, and only the "
		                       "agent whose tool returned it can read it.",
		                       result_id != NULL ? result_id : "");

	meta = load_meta(dir, result_id);

	if (meta != NULL &&
	    json_object_get_int_member_with_default(meta, "budget", 0) > 0)
		cap = (gsize)MAX(json_object_get_int_member_with_default(meta,
		                                                         "budget", 0),
		                 CLAWT_MCP_RESULT_MIN_BUDGET);

	out = g_string_new(NULL);
	g_string_append_printf(out, "[clawtilla] Held result %s", result_id);

	if (meta != NULL &&
	    json_object_get_string_member_with_default(meta, "tool", NULL) != NULL)
		g_string_append_printf(out, " (from %s)",
		                       json_object_get_string_member(meta, "tool"));

	g_string_append(out, ". The text below is the tool's, not clawtilla's.\n");

	/* Too big to parse: raw bytes only, and no path can be followed. */
	if ((gsize)st.st_size > CLAWT_MCP_RESULT_HOLD_MAX) {
		if (has_path) {
			g_string_free(out, TRUE);
			return g_strdup_printf("Result %s is too big to follow a path "
			                       "into; use \"search\" or \"offset\" and "
			                       "\"limit\" on its raw JSON.", result_id);
		}

		answer_raw(out, file, (gsize)st.st_size, search, offset, limit, cap);
		*is_error = FALSE;
		return g_string_free(out, FALSE);
	}

	parser = json_parser_new();

	if (!json_parser_load_from_file(parser, file, NULL) ||
	    json_parser_get_root(parser) == NULL) {
		answer_raw(out, file, (gsize)st.st_size, search, offset, limit, cap);
		*is_error = FALSE;
		return g_string_free(out, FALSE);
	}

	/* The answer's `result` is what a trimmed result's paths start from. */
	root = json_parser_get_root(parser);

	if (JSON_NODE_HOLDS_OBJECT(root) &&
	    json_object_has_member(json_node_get_object(root), "result"))
		root = json_object_get_member(json_node_get_object(root), "result");

	segments = parse_path(path, &error);

	if (segments == NULL) {
		g_string_free(out, TRUE);
		return g_strdup_printf("The path '%s' could not be read: %s.", path,
		                       error);
	}

	selected = follow(root, segments, &error);

	if (selected == NULL) {
		g_string_free(out, TRUE);
		return g_strdup_printf("The path '%s' does not lead anywhere in %s: "
		                       "%s.", path, result_id, error);
	}

	where = has_path ? g_strdup(path) : g_strdup("the whole result");

	if (JSON_NODE_HOLDS_VALUE(selected) &&
	    json_node_get_value_type(selected) == G_TYPE_STRING)
		answer_string(out, json_node_get_string(selected), where, search,
		              offset, limit, cap);
	else if (JSON_NODE_HOLDS_ARRAY(selected))
		answer_array(out, json_node_get_array(selected), where, search,
		             offset, limit, cap);
	else if (JSON_NODE_HOLDS_OBJECT(selected))
		answer_object(out, json_node_get_object(selected), where, search,
		              cap);
	else {
		g_autofree gchar *json = json_to_string(selected, FALSE);

		g_string_append_printf(out, "%s: ", where);
		append_quoted(out, json);
	}

	/*
	 * Whatever was asked, the answer stays within the result's budget:
	 * there is no query that returns the whole of something too big to
	 * have been shown in the first place.
	 */
	if (out->len > cap + 1024) {
		gsize cut = clawt_mcp_result_safe_cut(out->str, out->len, cap);

		g_string_truncate(out, cut);
		g_string_append(out, "\n[clawtilla] This answer was cut at the "
		                     "budget; narrow the path, or use offset and "
		                     "limit.");
	}

	*is_error = FALSE;

	return g_string_free(out, FALSE);
}

/* ── The reader ──────────────────────────────────────────────────── */

struct _ClawtMcpReader {
	ClawtMcpFraming     framing;
	gsize               ceiling;
	gchar              *spill_dir;
	ClawtMcpReaderFunc  func;
	gpointer            user_data;

	GByteArray         *buffer;
	gsize               total;

	/* Past the ceiling: where the rest goes, and under what id. */
	FILE               *spill;
	gchar              *spill_id;
	gboolean            over;

	/*
	 * SSE: the first bytes of a line, held until it can be told whether
	 * it is a `data:` line; then whether the line is being kept at all.
	 */
	gchar               prefix[8];
	gsize               prefix_length;
	gboolean            prefix_done;
	gboolean            skipping;

	/*
	 * A scan of the message as it goes past, just deep enough to find
	 * the top-level `id` of one too big to hold -- which may come after
	 * the result, so it cannot be read off the head.
	 */
	gint                depth;
	gboolean            in_string;
	gboolean            escaped;
	gboolean            expect_key;
	gboolean            string_is_key;
	gboolean            capturing_value;
	GString            *key;
	GString            *last_key;
	GString            *id_value;
	gboolean            have_id;
};

ClawtMcpReader *
clawt_mcp_reader_new(
	ClawtMcpFraming     framing,
	gsize               ceiling,
	const gchar        *spill_dir,
	ClawtMcpReaderFunc  func,
	gpointer            user_data
){
	ClawtMcpReader *self = g_new0(ClawtMcpReader, 1);

	self->framing = framing;
	self->ceiling = MAX(ceiling, (gsize)1024);
	self->spill_dir = g_strdup(spill_dir);
	self->func = func;
	self->user_data = user_data;
	self->buffer = g_byte_array_new();
	self->key = g_string_new(NULL);
	self->last_key = g_string_new(NULL);
	self->id_value = g_string_new(NULL);

	return self;
}

static void
scan_reset(ClawtMcpReader *self)
{
	self->depth = 0;
	self->in_string = FALSE;
	self->escaped = FALSE;
	self->expect_key = FALSE;
	self->string_is_key = FALSE;
	self->capturing_value = FALSE;
	g_string_truncate(self->key, 0);
	g_string_truncate(self->last_key, 0);
	g_string_truncate(self->id_value, 0);
	self->have_id = FALSE;
}

static void
end_capture(ClawtMcpReader *self)
{
	if (!self->capturing_value)
		return;

	self->capturing_value = FALSE;
	g_strstrip(self->id_value->str);
	g_string_set_size(self->id_value, strlen(self->id_value->str));
	self->have_id = self->id_value->len > 0;
}

static void
scan_byte(ClawtMcpReader *self, gchar c)
{
	/* The id's value, at depth one, until the comma or brace that ends it. */
	if (self->capturing_value && self->depth == 1 && !self->in_string &&
	    (c == ',' || c == '}')) {
		end_capture(self);
	} else if (self->capturing_value && self->id_value->len < 256) {
		g_string_append_c(self->id_value, c);
	}

	if (self->in_string) {
		if (self->escaped) {
			self->escaped = FALSE;

			if (self->string_is_key && self->key->len < 64)
				g_string_append_c(self->key, c);
		} else if (c == '\\') {
			self->escaped = TRUE;

			if (self->string_is_key && self->key->len < 64)
				g_string_append_c(self->key, c);
		} else if (c == '"') {
			self->in_string = FALSE;

			if (self->string_is_key) {
				g_string_assign(self->last_key, self->key->str);
				self->string_is_key = FALSE;
			}
		} else if (self->string_is_key && self->key->len < 64) {
			g_string_append_c(self->key, c);
		}

		return;
	}

	switch (c) {
	case '"':
		self->in_string = TRUE;
		self->string_is_key = (self->depth == 1 && self->expect_key);
		self->expect_key = FALSE;
		g_string_truncate(self->key, 0);
		break;

	case '{':
	case '[':
		self->depth++;
		self->expect_key = (c == '{' && self->depth == 1);
		break;

	case '}':
	case ']':
		self->depth--;
		break;

	case ',':
		if (self->depth == 1)
			self->expect_key = TRUE;
		break;

	case ':':
		if (self->depth == 1 && g_strcmp0(self->last_key->str, "id") == 0 &&
		    !self->have_id) {
			self->capturing_value = TRUE;
			g_string_truncate(self->id_value, 0);
		}
		break;

	default:
		break;
	}
}

static void
start_spill(ClawtMcpReader *self)
{
	g_autofree gchar *path = NULL;
	gint fd;

	self->over = TRUE;

	if (self->spill_dir == NULL ||
	    !clawt_ensure_dir(self->spill_dir, 0700, NULL))
		return;

	self->spill_id = clawt_mcp_result_new_id();
	path = clawt_mcp_result_path(self->spill_dir, self->spill_id);

	if (path == NULL) {
		g_clear_pointer(&self->spill_id, g_free);
		return;
	}

	fd = g_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0) {
		g_printerr("clawtilla: relay: cannot hold a large result at %s: %s\n",
		           path, g_strerror(errno));
		g_clear_pointer(&self->spill_id, g_free);
		return;
	}

	self->spill = fdopen(fd, "wb");

	if (self->spill == NULL) {
		close(fd);
		g_clear_pointer(&self->spill_id, g_free);
		return;
	}

	/* What was held so far goes first; the rest follows as it arrives. */
	if (fwrite(self->buffer->data, 1, self->buffer->len, self->spill)
	    != self->buffer->len)
		g_printerr("clawtilla: relay: short write holding a result\n");
}

/* One payload byte run of the current message. */
static void
take(ClawtMcpReader *self, const gchar *data, gsize length)
{
	gsize i;

	for (i = 0; i < length; i++)
		scan_byte(self, data[i]);

	self->total += length;

	if (!self->over && self->buffer->len + length <= self->ceiling) {
		g_byte_array_append(self->buffer, (const guint8 *)data, (guint)length);
		return;
	}

	/*
	 * Past the ceiling.  The head stays in memory for the reply; the
	 * rest is written out and never held.
	 */
	if (!self->over) {
		gsize room = self->ceiling - self->buffer->len;

		g_byte_array_append(self->buffer, (const guint8 *)data, (guint)room);
		start_spill(self);
		data += room;
		length -= room;

		if (self->spill != NULL && length > 0 &&
		    fwrite(data, 1, length, self->spill) != length)
			g_printerr("clawtilla: relay: short write holding a result\n");

		return;
	}

	if (self->spill != NULL && fwrite(data, 1, length, self->spill) != length)
		g_printerr("clawtilla: relay: short write holding a result\n");
}

static void
deliver(ClawtMcpReader *self)
{
	if (self->total == 0 && !self->over) {
		scan_reset(self);
		return;
	}

	if (!self->over) {
		/* NUL-terminated for callers that treat it as a string. */
		g_byte_array_append(self->buffer, (const guint8 *)"", 1);
		self->func((const gchar *)self->buffer->data, self->buffer->len - 1,
		           NULL, self->user_data);
	} else {
		ClawtMcpOversized oversized;

		end_capture(self);

		if (self->spill != NULL) {
			fclose(self->spill);
			self->spill = NULL;
		}

		oversized.total = self->total;
		oversized.id_json = self->have_id ? self->id_value->str : NULL;
		oversized.head = (const gchar *)self->buffer->data;
		oversized.head_length = self->buffer->len;
		oversized.result_id = self->spill_id;

		self->func(NULL, 0, &oversized, self->user_data);
	}

	g_byte_array_set_size(self->buffer, 0);
	g_clear_pointer(&self->spill_id, g_free);
	self->total = 0;
	self->over = FALSE;
	scan_reset(self);
}

static void
end_line(ClawtMcpReader *self)
{
	gboolean keep = !self->skipping;

	/* A short SSE line never decided: it was not a data line. */
	if (self->framing == CLAWT_MCP_FRAMING_SSE && !self->prefix_done)
		keep = FALSE;

	/*
	 * A carriage return before the newline is framing too.  Taken off
	 * here, where the line ends, because the chunk that carried it may
	 * have ended between the two.
	 */
	if (keep && !self->over && self->buffer->len > 0 &&
	    self->buffer->data[self->buffer->len - 1] == '\r') {
		g_byte_array_set_size(self->buffer, self->buffer->len - 1);
		self->total--;
	}

	if (keep)
		deliver(self);
	else {
		g_byte_array_set_size(self->buffer, 0);
		self->total = 0;
		scan_reset(self);
	}

	self->prefix_length = 0;
	self->prefix_done = FALSE;
	self->skipping = FALSE;
}

/*
 * The first bytes of an SSE line decide what it is.  `data:` lines carry
 * a message after the colon and any spaces; `event:`, `id:`, comments and
 * blank lines carry nothing this relay passes on.
 */
static void
sse_prefix(ClawtMcpReader *self, const gchar **data, gsize *length)
{
	while (!self->prefix_done && *length > 0) {
		self->prefix[self->prefix_length++] = **data;
		(*data)++;
		(*length)--;

		if (self->prefix_length < 5)
			continue;

		self->prefix_done = TRUE;

		if (strncmp(self->prefix, "data:", 5) != 0)
			self->skipping = TRUE;
	}

	/* The spaces after `data:` are framing, not message. */
	if (self->prefix_done && !self->skipping && self->total == 0) {
		while (*length > 0 && **data == ' ') {
			(*data)++;
			(*length)--;
		}
	}
}

void
clawt_mcp_reader_feed(
	ClawtMcpReader *self,
	const gchar    *data,
	gsize           length
){
	g_return_if_fail(self != NULL);

	if (self->framing == CLAWT_MCP_FRAMING_WHOLE) {
		take(self, data, length);
		return;
	}

	while (length > 0) {
		const gchar *newline = memchr(data, '\n', length);
		gsize run = (newline != NULL) ? (gsize)(newline - data) : length;
		const gchar *part = data;
		gsize part_length = run;

		if (self->framing == CLAWT_MCP_FRAMING_SSE)
			sse_prefix(self, &part, &part_length);

		if (!self->skipping && part_length > 0)
			take(self, part, part_length);

		if (newline == NULL)
			return;

		end_line(self);
		data += run + 1;
		length -= run + 1;
	}
}

void
clawt_mcp_reader_finish(ClawtMcpReader *self)
{
	g_return_if_fail(self != NULL);

	if (self->framing == CLAWT_MCP_FRAMING_WHOLE) {
		deliver(self);
		return;
	}

	end_line(self);
}

void
clawt_mcp_reader_free(ClawtMcpReader *self)
{
	if (self == NULL)
		return;

	if (self->spill != NULL)
		fclose(self->spill);

	g_byte_array_unref(self->buffer);
	g_string_free(self->key, TRUE);
	g_string_free(self->last_key, TRUE);
	g_string_free(self->id_value, TRUE);
	g_free(self->spill_id);
	g_free(self->spill_dir);
	g_free(self);
}

/* ── The relay's side ────────────────────────────────────────────── */

struct _ClawtMcpResults {
	gchar                      *connector;
	gsize                       budget;
	gchar                      *spill_dir;
	ClawtMcpResultsTrimmedFunc  trimmed;
	gpointer                    trimmed_data;

	/* id, as JSON text -> the tool that call names */
	GHashTable                 *calls;
};

ClawtMcpResults *
clawt_mcp_results_new(
	const gchar *connector,
	gsize        budget,
	const gchar *spill_dir
){
	ClawtMcpResults *self = g_new0(ClawtMcpResults, 1);

	self->connector = g_strdup(connector);
	self->budget = budget;
	self->spill_dir = g_strdup(spill_dir);
	self->calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);

	return self;
}

void
clawt_mcp_results_free(ClawtMcpResults *self)
{
	if (self == NULL)
		return;

	g_free(self->connector);
	g_free(self->spill_dir);
	g_hash_table_unref(self->calls);
	g_free(self);
}

gsize
clawt_mcp_results_get_budget(ClawtMcpResults *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->budget;
}

const gchar *
clawt_mcp_results_get_spill_dir(ClawtMcpResults *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->spill_dir;
}

void
clawt_mcp_results_set_trimmed_func(
	ClawtMcpResults            *self,
	ClawtMcpResultsTrimmedFunc  func,
	gpointer                    user_data
){
	g_return_if_fail(self != NULL);

	self->trimmed = func;
	self->trimmed_data = user_data;
}

static JsonObject *
parse_message(const gchar *line)
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
id_key(JsonObject *message)
{
	JsonNode *id;

	if (!json_object_has_member(message, "id"))
		return NULL;

	id = json_object_get_member(message, "id");

	if (JSON_NODE_HOLDS_NULL(id))
		return NULL;

	return json_to_string(id, FALSE);
}

void
clawt_mcp_results_note_outbound(
	ClawtMcpResults *self,
	const gchar     *line
){
	g_autoptr(JsonObject) message = NULL;
	g_autofree gchar *key = NULL;
	JsonObject *params;

	g_return_if_fail(self != NULL);

	if (self->budget == 0 || line == NULL)
		return;

	message = parse_message(line);

	if (message == NULL ||
	    g_strcmp0(json_object_get_string_member_with_default(message, "method",
	                                                         ""),
	              "tools/call") != 0)
		return;

	key = id_key(message);

	if (key == NULL)
		return;

	params = (json_object_has_member(message, "params") &&
	          JSON_NODE_HOLDS_OBJECT(json_object_get_member(message,
	                                                        "params")))
		? json_object_get_object_member(message, "params") : NULL;

	/*
	 * Bounded: a server that never answers must not grow this for ever.
	 * A table of a thousand open calls is already a broken server.
	 */
	if (g_hash_table_size(self->calls) >= 1024)
		g_hash_table_remove_all(self->calls);

	g_hash_table_replace(self->calls, g_steal_pointer(&key),
	                     g_strdup(params != NULL
	                              ? json_object_get_string_member_with_default(
	                                    params, "name", "")
	                              : ""));
}

/*
 * Whether @message answers a tool call: by its id when the call was
 * seen, and by its shape when it was not -- a result carrying `content`
 * is a tool result whoever asked.
 */
static gboolean
answers_call(ClawtMcpResults *self, JsonObject *message, gchar **out_tool)
{
	g_autofree gchar *key = id_key(message);
	JsonObject *result;
	gchar *tool = NULL;

	*out_tool = NULL;

	if (!json_object_has_member(message, "result") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(message, "result")))
		return FALSE;

	if (key != NULL) {
		gchar *stolen_key = NULL;

		if (!g_hash_table_steal_extended(self->calls, key,
		                                 (gpointer *)&stolen_key,
		                                 (gpointer *)&tool))
			goto by_shape;

		g_free(stolen_key);
		*out_tool = (tool != NULL && *tool != '\0') ? tool : NULL;

		if (*out_tool == NULL)
			g_free(tool);

		return TRUE;
	}

by_shape:
	result = json_object_get_object_member(message, "result");

	return json_object_has_member(result, "content") &&
	       JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "content"));
}

gchar *
clawt_mcp_results_inbound(
	ClawtMcpResults *self,
	const gchar     *line
){
	g_autoptr(JsonObject) message = NULL;
	g_autoptr(JsonObject) trimmed = NULL;
	g_autofree gchar *tool = NULL;
	g_autofree gchar *result_id = NULL;
	JsonObject *result;
	gsize length;
	gsize kept = 0;

	g_return_val_if_fail(self != NULL, NULL);

	if (line == NULL)
		return NULL;

	length = strlen(line);

	/* Cheap first: a message within the budget whole cannot need trimming. */
	if (self->budget == 0 || length <= self->budget)
		return g_strdup(line);

	message = parse_message(line);

	if (message == NULL || !answers_call(self, message, &tool))
		return g_strdup(line);

	result = json_object_get_object_member(message, "result");

	/* Kept whole first, so the note can name where. */
	if (self->spill_dir != NULL) {
		g_autoptr(GError) error = NULL;

		result_id = clawt_mcp_result_new_id();

		if (result_id == NULL ||
		    !clawt_mcp_result_spill(self->spill_dir, result_id, line, length,
		                            self->connector, tool, self->budget,
		                            &error)) {
			g_printerr("clawtilla: relay: could not keep the whole result: "
			           "%s\n", error != NULL ? error->message : "no id");
			g_clear_pointer(&result_id, g_free);
		}
	}

	trimmed = clawt_mcp_result_trim(result, self->budget, length,
	                                self->connector, tool, result_id, &kept);

	if (trimmed == NULL) {
		/*
		 * Over the budget in bytes on the wire -- escapes, an envelope --
		 * but not in what the agent reads, so nothing was cut and there
		 * is nothing to come back for.
		 */
		if (result_id != NULL)
			remove_held(self->spill_dir, result_id);

		return g_strdup(line);
	}

	json_object_set_object_member(message, "result",
	                              json_object_ref(trimmed));

	if (self->trimmed != NULL)
		self->trimmed(self->connector, tool, length, kept, result_id,
		              self->trimmed_data);

	g_printerr("clawtilla: relay: trimmed %s from %" G_GSIZE_FORMAT " to %"
	           G_GSIZE_FORMAT " bytes\n", tool != NULL ? tool : "a result",
	           length, kept);

	return render_line(message);
}

gchar *
clawt_mcp_results_oversized(
	ClawtMcpResults         *self,
	const ClawtMcpOversized *oversized
){
	g_autofree gchar *tool = NULL;
	gsize budget;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(oversized != NULL, NULL);

	if (oversized->id_json != NULL) {
		gchar *stolen_key = NULL;

		if (g_hash_table_steal_extended(self->calls, oversized->id_json,
		                                (gpointer *)&stolen_key,
		                                (gpointer *)&tool))
			g_free(stolen_key);
	}

	if (oversized->result_id != NULL && self->spill_dir != NULL) {
		g_autoptr(GError) error = NULL;

		if (!clawt_mcp_result_write_meta(self->spill_dir, oversized->result_id,
		                                 self->connector, tool,
		                                 oversized->total, self->budget,
		                                 &error))
			g_printerr("clawtilla: relay: %s\n", error->message);

		clawt_mcp_result_sweep(self->spill_dir,
		                       g_get_real_time() / G_USEC_PER_SEC,
		                       CLAWT_MCP_RESULT_KEEP_COUNT,
		                       CLAWT_MCP_RESULT_KEEP_SECONDS);
	}

	/* A notification too big to hold has nobody to answer. */
	if (oversized->id_json == NULL)
		return NULL;

	budget = (self->budget > 0) ? self->budget : CLAWT_MCP_RESULT_MIN_BUDGET;

	if (self->trimmed != NULL)
		self->trimmed(self->connector, tool, oversized->total,
		              MIN(budget, oversized->head_length),
		              oversized->result_id, self->trimmed_data);

	return clawt_mcp_result_oversized_reply(
		oversized->id_json, oversized->total, oversized->head,
		oversized->head_length, budget, self->connector,
		(tool != NULL && *tool != '\0') ? tool : NULL, oversized->result_id);
}
