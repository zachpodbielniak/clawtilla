/*
 * clawt-mcp-directory.c - A server with too many tools, as three tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The shape, the thresholds and the ranking follow OpenMausBot's
 * server/mcp-directory.ts, which measured them against a server with
 * 425 tools; this is a port of the idea into the relay, not of the code.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-directory.h"

#include <math.h>
#include <string.h>

#include <glib/gstdio.h>
#include <libreclaw.h>
#include <yaml-glib.h>

/* How many matches a search returns unless asked, and at most. */
#define SEARCH_LIMIT_DEFAULT (8)
#define SEARCH_LIMIT_MAX     (20)

/* A one-line description in a search result is cut here. */
#define DESCRIPTION_CHARS    (200)

/* The input fields of a match, as one short signature. */
#define SIGNATURE_CHARS      (300)

/* The overview in the search tool's own description. */
#define AREAS_CHARS          (2000)

/* Property names and descriptions read from a schema for ranking. */
#define SCHEMA_TEXT_CHARS    (4000)

/* A query longer than this is not a query. */
#define QUERY_CHARS          (1000)

/*
 * A describe answer stays below what the engines keep of one tool
 * result (Claude Code and Pi keep about 50 KiB); a bigger schema is
 * outlined rather than cut mid-JSON, which would be unreadable.
 */
#define DESCRIBE_BYTES       (40000)

/*
 * Pages of a tool list the relay will follow by itself.  A server that
 * hands out a cursor for ever would otherwise hold the client's
 * `tools/list` unanswered for ever.
 */
#define MAX_PAGES            (64)

/* Tools named per area in the answer to an empty search. */
#define AREA_EXAMPLES        (6)

/* BM25's usual constants, and how much each part of a tool counts. */
#define BM25_K1              (1.2)
#define BM25_B               (0.75)
#define WEIGHT_NAME          (2.0)
#define WEIGHT_TEXT          (1.0)
#define WEIGHT_SCHEMA        (0.5)

/* ── The modes ───────────────────────────────────────────────────── */

typedef struct {
	ClawtToolDirectoryMode  mode;
	const gchar            *nick;
	const gchar            *label;
} ModeInfo;

static const ModeInfo mode_table[] = {
	{ CLAWT_TOOL_DIRECTORY_AUTO, "auto",
	  "Auto -- when it has too many tools" },
	{ CLAWT_TOOL_DIRECTORY_ALWAYS, "always",
	  "Always" },
	{ CLAWT_TOOL_DIRECTORY_NEVER, "never",
	  "Never -- list every tool" }
};

guint
clawt_tool_directory_mode_count(
	void
){
	return (guint)G_N_ELEMENTS(mode_table);
}

ClawtToolDirectoryMode
clawt_tool_directory_mode_nth(
	guint n
){
	g_return_val_if_fail(n < G_N_ELEMENTS(mode_table),
			     CLAWT_TOOL_DIRECTORY_AUTO);

	return mode_table[n].mode;
}

const gchar *
clawt_tool_directory_mode_nth_nick(
	guint n
){
	g_return_val_if_fail(n < G_N_ELEMENTS(mode_table), "auto");

	return mode_table[n].nick;
}

const gchar *
clawt_tool_directory_mode_nth_label(
	guint n
){
	g_return_val_if_fail(n < G_N_ELEMENTS(mode_table), mode_table[0].label);

	return mode_table[n].label;
}

gboolean
clawt_tool_directory_mode_from_nick(
	const gchar            *nick,
	ClawtToolDirectoryMode *out_mode
){
	gint value;

	*out_mode = CLAWT_TOOL_DIRECTORY_AUTO;

	if (nick == NULL || *nick == '\0')
		return TRUE;

	/*
	 * Through the enum's own nicknames, so the table above and the
	 * config file cannot spell a mode two ways.
	 */
	if (!clawt_enum_from_nick(CLAWT_TYPE_TOOL_DIRECTORY_MODE, nick, &value))
		return FALSE;

	*out_mode = (ClawtToolDirectoryMode)value;
	return TRUE;
}

/* ── The rule ────────────────────────────────────────────────────── */

gboolean
clawt_mcp_directory_is_big(
	guint64 n_tools,
	guint64 n_chars,
	guint64 min_tools,
	guint64 min_chars
){
	return n_tools > min_tools || n_chars > min_chars;
}

gboolean
clawt_mcp_directory_decide(
	ClawtToolDirectoryMode mode,
	gboolean               cli_searches_tools,
	guint64                n_tools,
	guint64                n_chars,
	guint64                min_tools,
	guint64                min_chars
){
	/*
	 * No default: a mode added to the enum has to be classified here,
	 * and -Wswitch says so.
	 */
	switch (mode) {
	case CLAWT_TOOL_DIRECTORY_ALWAYS:
		return TRUE;

	case CLAWT_TOOL_DIRECTORY_NEVER:
		return FALSE;

	case CLAWT_TOOL_DIRECTORY_AUTO:
		if (cli_searches_tools)
			return FALSE;

		return clawt_mcp_directory_is_big(n_tools, n_chars,
						  min_tools, min_chars);
	}

	return FALSE;
}

gboolean
clawt_mcp_directory_cli_searches_tools(
	const gchar *provider
){
	const gchar *cli;

	/*
	 * TODO(ai-glib): ask the client class.  This belongs beside
	 * AiCliClientClass.config_dir_env as a class datum the provider
	 * declares; until ai-glib has one, this is the only table of it and
	 * the roadmap carries the move.
	 */
	cli = lc_provider_type_normalize(provider, "agent");

	return g_strcmp0(cli, "claude-code") == 0 ||
	       g_strcmp0(cli, "claude-tmux") == 0;
}

gchar *
clawt_mcp_directory_status_label(
	const gchar *mode_nick,
	gint64       n_tools,
	gint64       n_chars,
	gint64       min_tools,
	gint64       min_chars
){
	ClawtToolDirectoryMode mode = CLAWT_TOOL_DIRECTORY_AUTO;
	g_autofree gchar *size = NULL;
	const gchar *how = NULL;

	(void)clawt_tool_directory_mode_from_nick(mode_nick, &mode);

	/*
	 * Unknown is said as unknown.  Zero would read as a server with no
	 * tools, which is a different and alarming thing.
	 */
	if (n_tools < 0) {
		switch (mode) {
		case CLAWT_TOOL_DIRECTORY_ALWAYS:
			return g_strdup("Presented as a directory. No agent has "
					"used it yet, so its size is unknown");

		case CLAWT_TOOL_DIRECTORY_NEVER:
			return g_strdup("Every tool listed. No agent has used it "
					"yet, so its size is unknown");

		case CLAWT_TOOL_DIRECTORY_AUTO:
			return g_strdup("No agent has used it yet, so its size is "
					"unknown");
		}

		return g_strdup("");
	}

	if (n_chars >= 1000)
		size = g_strdup_printf("%" G_GINT64_FORMAT " tool%s, %"
				       G_GINT64_FORMAT "k characters",
				       n_tools, n_tools == 1 ? "" : "s",
				       (n_chars + 500) / 1000);
	else
		size = g_strdup_printf("%" G_GINT64_FORMAT " tool%s", n_tools,
				       n_tools == 1 ? "" : "s");

	switch (mode) {
	case CLAWT_TOOL_DIRECTORY_ALWAYS:
		how = "a directory for every agent";
		break;

	case CLAWT_TOOL_DIRECTORY_NEVER:
		how = "every tool listed";
		break;

	case CLAWT_TOOL_DIRECTORY_AUTO:
		how = clawt_mcp_directory_is_big((guint64)n_tools,
						 (guint64)MAX(n_chars, 0),
						 (guint64)MAX(min_tools, 0),
						 (guint64)MAX(min_chars, 0))
			? "a directory, except for Claude Code, which searches "
			  "tools itself"
			: "listed whole";
		break;
	}

	return g_strdup_printf("%s -- %s", size, how != NULL ? how : "");
}

static gchar *
render_node(JsonNode *node)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();

	json_generator_set_root(generator, node);
	json_generator_set_pretty(generator, FALSE);

	return json_generator_to_data(generator, NULL);
}

static gchar *
render_object(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

	json_node_set_object(node, object);

	return render_node(node);
}

guint64
clawt_mcp_directory_measure(
	JsonArray *tools
){
	guint64 total = 0;
	guint i;

	if (tools == NULL)
		return 0;

	for (i = 0; i < json_array_get_length(tools); i++) {
		g_autofree gchar *text =
			render_node(json_array_get_element(tools, i));

		/* One for the comma between them in the real array. */
		total += (guint64)strlen(text) + 1;
	}

	return total;
}

gchar *
clawt_mcp_directory_tool_area(
	const gchar *name
){
	const gchar *underscore;
	const gchar *p;
	GString *word;

	if (name == NULL || *name == '\0')
		return g_strdup("");

	/* `issues_create` is about issues. */
	underscore = strchr(name, '_');

	if (underscore != NULL && underscore > name) {
		gsize length = (gsize)(underscore - name);

		return g_ascii_strdown(name, (gssize)MIN(length, (gsize)40));
	}

	/*
	 * Otherwise the first word of the first segment, where a word ends
	 * at a change of case: `createIssue` is `create`, `HTTPServer` is
	 * `http`.
	 */
	p = name;

	while (*p != '\0' && strchr(" _./:-", *p) != NULL)
		p++;

	word = g_string_new(NULL);

	if (g_ascii_isupper(*p) && g_ascii_islower(p[1])) {
		g_string_append_c(word, *p++);

		while (g_ascii_islower(*p) || g_ascii_isdigit(*p))
			g_string_append_c(word, *p++);
	} else if (g_ascii_isupper(*p)) {
		while (g_ascii_isupper(*p) && !g_ascii_islower(p[1]))
			g_string_append_c(word, *p++);

		if (word->len == 0)
			g_string_append_c(word, *p);
	} else {
		while (g_ascii_islower(*p) || g_ascii_isdigit(*p))
			g_string_append_c(word, *p++);
	}

	if (word->len == 0) {
		while (*p != '\0' && strchr(" _./:-", *p) == NULL)
			g_string_append_c(word, *p++);
	}

	if (word->len > 40)
		g_string_truncate(word, 40);

	{
		g_autofree gchar *raw = g_string_free(word, FALSE);

		return g_ascii_strdown(raw, -1);
	}
}

/* ── Terms ───────────────────────────────────────────────────────── */

static const gchar *const stop_words[] = {
	"a", "about", "all", "am", "an", "and", "any", "are", "as", "at",
	"be", "been", "being", "but", "by", "can", "could", "did", "do",
	"does", "doing", "for", "from", "had", "has", "have", "having", "he",
	"her", "here", "him", "his", "how", "i", "if", "in", "into", "is",
	"it", "its", "just", "like", "many", "me", "might", "more", "most",
	"much", "must", "my", "need", "no", "not", "of", "on", "or", "our",
	"ours", "please", "she", "should", "so", "some", "than", "that",
	"the", "their", "them", "then", "there", "these", "they", "this",
	"those", "to", "too", "us", "very", "want", "was", "we", "were",
	"what", "when", "where", "which", "while", "who", "whom", "whose",
	"why", "will", "with", "would", "you", "your", "yours", NULL
};

static gboolean
is_stop_word(const gchar *word)
{
	gsize i;

	for (i = 0; stop_words[i] != NULL; i++) {
		if (strcmp(stop_words[i], word) == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * A small suffix stripper, so `issues`, `issue` and `listing`/`list`
 * meet.  Deliberately crude: a tool search needs plural and tense to
 * agree, not linguistics.
 */
static void
stem(GString *w)
{
	gsize n = w->len;

	if (n > 4 && g_str_has_suffix(w->str, "ies")) {
		g_string_truncate(w, n - 3);
		g_string_append_c(w, 'y');
	} else if (n > 4 && g_str_has_suffix(w->str, "sses")) {
		g_string_truncate(w, n - 2);
	} else if (n > 4 && (g_str_has_suffix(w->str, "ses") ||
			     g_str_has_suffix(w->str, "xes") ||
			     g_str_has_suffix(w->str, "zes") ||
			     g_str_has_suffix(w->str, "ches") ||
			     g_str_has_suffix(w->str, "shes"))) {
		g_string_truncate(w, n - 2);
	} else if (n > 3 && w->str[n - 1] == 's' &&
		   !g_str_has_suffix(w->str, "ss") &&
		   !g_str_has_suffix(w->str, "us") &&
		   !g_str_has_suffix(w->str, "is")) {
		g_string_truncate(w, n - 1);
	}

	n = w->len;

	if (n > 5 && g_str_has_suffix(w->str, "ing"))
		g_string_truncate(w, n - 3);
	else if (n > 4 && g_str_has_suffix(w->str, "ed"))
		g_string_truncate(w, n - 2);

	n = w->len;

	if (n > 3 && w->str[n - 1] == 'e')
		g_string_truncate(w, n - 1);
}

static gboolean
is_word_byte(guchar c)
{
	/* Bytes of a multibyte character are kept inside their word. */
	return g_ascii_isalnum(c) || c >= 0x80;
}

/*
 * Calls @add once per term in @text.
 *
 * camelCase is split first (`createIssue` is two words, `HTTPServer` is
 * `http server`), then everything is lower-cased and cut on anything
 * that is not a letter or a digit.
 */
static void
for_each_term(const gchar *text,
	      void (*add)(const gchar *term, gpointer data),
	      gpointer data)
{
	g_autoptr(GString) spaced = NULL;
	g_autoptr(GString) word = NULL;
	const guchar *p;
	gsize i;

	if (text == NULL)
		return;

	spaced = g_string_new(NULL);

	for (p = (const guchar *)text; *p != '\0'; p++) {
		guchar prev = (p > (const guchar *)text) ? p[-1] : 0;

		if (g_ascii_isupper(*p) &&
		    (g_ascii_islower(prev) || g_ascii_isdigit(prev)))
			g_string_append_c(spaced, ' ');
		else if (g_ascii_isupper(*p) && g_ascii_isupper(prev) &&
			 g_ascii_islower(p[1]))
			g_string_append_c(spaced, ' ');

		g_string_append_c(spaced, (gchar)g_ascii_tolower(*p));
	}

	word = g_string_new(NULL);

	for (i = 0; i <= spaced->len; i++) {
		guchar c = (i < spaced->len) ? (guchar)spaced->str[i] : 0;
		gboolean ascii = TRUE;
		gsize j;

		if (c != 0 && is_word_byte(c)) {
			g_string_append_c(word, (gchar)c);
			continue;
		}

		if (word->len == 0)
			continue;

		for (j = 0; j < word->len; j++) {
			if ((guchar)word->str[j] >= 0x80)
				ascii = FALSE;
		}

		/* A lone letter or digit says nothing about a tool. */
		if (!(ascii && word->len == 1) && !is_stop_word(word->str)) {
			if (ascii)
				stem(word);

			if (word->len > 0)
				add(word->str, data);
		}

		g_string_truncate(word, 0);
	}
}

typedef struct {
	GHashTable *counts;
	guint       length;
} Bag;

static void
bag_add(const gchar *term, gpointer data)
{
	Bag *bag = data;
	gpointer old = g_hash_table_lookup(bag->counts, term);

	g_hash_table_replace(bag->counts, g_strdup(term),
			     GUINT_TO_POINTER(GPOINTER_TO_UINT(old) + 1));
	bag->length++;
}

static void
bag_init(Bag *bag)
{
	bag->counts = g_hash_table_new_full(g_str_hash, g_str_equal,
					    g_free, NULL);
	bag->length = 0;
}

static void
bag_clear(Bag *bag)
{
	g_clear_pointer(&bag->counts, g_hash_table_unref);
}

static guint
bag_count(const Bag *bag, const gchar *term)
{
	return GPOINTER_TO_UINT(g_hash_table_lookup(bag->counts, term));
}

/*
 * What a schema says in words: property names, descriptions and enum
 * values, so a search for "label" finds the tool that takes `labels`
 * even when its description never says so.
 */
static void
schema_text(JsonNode *node, GString *out, guint depth)
{
	JsonObject *object;
	GList *members;
	GList *l;

	if (node == NULL || depth > 8 || out->len >= SCHEMA_TEXT_CHARS ||
	    !JSON_NODE_HOLDS_OBJECT(node))
		return;

	object = json_node_get_object(node);

	if (json_object_has_member(object, "description") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(object,
							 "description"))) {
		const gchar *d = json_object_get_string_member_with_default(
			object, "description", NULL);

		if (d != NULL)
			g_string_append_printf(out, " %s", d);
	}

	if (json_object_has_member(object, "enum") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "enum"))) {
		JsonArray *values = json_object_get_array_member(object, "enum");
		guint i;

		for (i = 0; i < json_array_get_length(values); i++) {
			JsonNode *v = json_array_get_element(values, i);

			if (JSON_NODE_HOLDS_VALUE(v) &&
			    json_node_get_value_type(v) == G_TYPE_STRING)
				g_string_append_printf(out, " %s",
						       json_node_get_string(v));
		}
	}

	if (json_object_has_member(object, "properties") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object,
							  "properties"))) {
		JsonObject *props = json_object_get_object_member(object,
								  "properties");

		members = json_object_get_members(props);

		for (l = members; l != NULL; l = l->next) {
			g_string_append_printf(out, " %s", (const gchar *)l->data);
			schema_text(json_object_get_member(props, l->data), out,
				    depth + 1);
		}

		g_list_free(members);
	}

	if (json_object_has_member(object, "items"))
		schema_text(json_object_get_member(object, "items"), out,
			    depth + 1);
}

/* ── One tool, indexed ───────────────────────────────────────────── */

typedef struct {
	gchar      *name;
	gchar      *area;
	JsonObject *tool;
	Bag         name_bag;
	Bag         text_bag;
	Bag         schema_bag;
	Bag         area_bag;
} Entry;

static void
entry_free(gpointer data)
{
	Entry *entry = data;

	g_free(entry->name);
	g_free(entry->area);
	json_object_unref(entry->tool);
	bag_clear(&entry->name_bag);
	bag_clear(&entry->text_bag);
	bag_clear(&entry->schema_bag);
	bag_clear(&entry->area_bag);
	g_free(entry);
}

static Entry *
entry_new(JsonObject *tool, const gchar *name)
{
	Entry *entry = g_new0(Entry, 1);
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(GString) schema = g_string_new(NULL);
	const gchar *title;
	const gchar *description;

	entry->name = g_strdup(name);
	entry->area = clawt_mcp_directory_tool_area(name);
	entry->tool = json_object_ref(tool);

	title = json_object_get_string_member_with_default(tool, "title", NULL);
	description = json_object_get_string_member_with_default(
		tool, "description", NULL);

	if (title != NULL)
		g_string_append_printf(text, "%s ", title);

	if (description != NULL)
		g_string_append(text, description);

	if (json_object_has_member(tool, "inputSchema"))
		schema_text(json_object_get_member(tool, "inputSchema"),
			    schema, 0);

	bag_init(&entry->name_bag);
	bag_init(&entry->text_bag);
	bag_init(&entry->schema_bag);
	bag_init(&entry->area_bag);

	for_each_term(name, bag_add, &entry->name_bag);
	for_each_term(text->str, bag_add, &entry->text_bag);
	for_each_term(schema->str, bag_add, &entry->schema_bag);
	for_each_term(entry->area, bag_add, &entry->area_bag);

	return entry;
}

/* ── The object ──────────────────────────────────────────────────── */

struct _ClawtMcpDirectory {
	GObject                 parent_instance;

	gchar                  *server;
	gchar                  *search_name;
	gchar                  *describe_name;
	gchar                  *call_name;

	ClawtToolDirectoryMode  mode;
	gboolean                cli_searches_tools;
	guint64                 min_tools;
	guint64                 min_chars;

	/* The catalogue: what this agent may use, in the server's order. */
	GPtrArray              *entries;
	GHashTable             *by_name;
	guint64                 n_chars;
	gboolean                active;

	/*
	 * A tool list in the middle of arriving.  `pending` maps the id a
	 * `tools/list` went out under -- the client's own, or one of ours
	 * for a page the directory fetched itself -- to the client's id,
	 * which is what the finished answer has to carry.
	 */
	JsonArray              *collecting;
	GHashTable             *pending;
	guint                   pages;
	guint                   next_private;
};

enum {
	SIGNAL_CATALOGUED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtMcpDirectory, clawt_mcp_directory, G_TYPE_OBJECT)

static void
clawt_mcp_directory_finalize(GObject *object)
{
	ClawtMcpDirectory *self = CLAWT_MCP_DIRECTORY(object);

	g_free(self->server);
	g_free(self->search_name);
	g_free(self->describe_name);
	g_free(self->call_name);
	g_ptr_array_unref(self->entries);
	g_hash_table_unref(self->by_name);
	g_clear_pointer(&self->collecting, json_array_unref);
	g_hash_table_unref(self->pending);

	G_OBJECT_CLASS(clawt_mcp_directory_parent_class)->finalize(object);
}

static void
clawt_mcp_directory_class_init(ClawtMcpDirectoryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = clawt_mcp_directory_finalize;

	/**
	 * ClawtMcpDirectory::catalogued:
	 * @self: the directory
	 *
	 * A whole tool list has arrived and been counted, whether or not it
	 * became a directory.  The relay reports the count to the daemon
	 * from here, which is how the Connectors page knows a server's
	 * size without the daemon ever starting it.
	 */
	signals[SIGNAL_CATALOGUED] =
		g_signal_new("catalogued", G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
			     G_TYPE_NONE, 0);
}

static void
clawt_mcp_directory_init(ClawtMcpDirectory *self)
{
	self->entries = g_ptr_array_new_with_free_func(entry_free);
	self->by_name = g_hash_table_new(g_str_hash, g_str_equal);
	self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					      (GDestroyNotify)json_node_unref);
}

/*
 * A tool name may hold letters, digits, `_` and `-`; a connector's name
 * is somebody's free text.  Anything else becomes `_`, and the prefix is
 * kept short because the CLI adds its own (`mcp__clawtilla-x__`) and some
 * cap the whole at 64.
 */
static gchar *
tool_prefix(const gchar *server)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = (server != NULL) ? server : "server"; *p != '\0'; p++) {
		if (out->len >= 32)
			break;

		g_string_append_c(out, (g_ascii_isalnum(*p) || *p == '_' ||
					*p == '-') ? *p : '_');
	}

	if (out->len == 0)
		g_string_append(out, "server");

	return g_string_free(out, FALSE);
}

ClawtMcpDirectory *
clawt_mcp_directory_new(
	const gchar            *server,
	ClawtToolDirectoryMode  mode,
	gboolean                cli_searches_tools,
	guint64                 min_tools,
	guint64                 min_chars
){
	ClawtMcpDirectory *self = g_object_new(CLAWT_TYPE_MCP_DIRECTORY, NULL);
	g_autofree gchar *prefix = tool_prefix(server);

	self->server = g_strdup(server != NULL ? server : "server");
	self->search_name = g_strdup_printf("%s_search_tools", prefix);
	self->describe_name = g_strdup_printf("%s_describe_tool", prefix);
	self->call_name = g_strdup_printf("%s_call_tool", prefix);
	self->mode = mode;
	self->cli_searches_tools = cli_searches_tools;
	self->min_tools = min_tools;
	self->min_chars = min_chars;

	return self;
}

const gchar *
clawt_mcp_directory_get_server(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	return self->server;
}

const gchar *
clawt_mcp_directory_get_search_name(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	return self->search_name;
}

const gchar *
clawt_mcp_directory_get_describe_name(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	return self->describe_name;
}

const gchar *
clawt_mcp_directory_get_call_name(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	return self->call_name;
}

/*
 * Whether this relay may ever present a directory -- before anything is
 * counted.  A mode of `never`, or `auto` for a CLI that searches by
 * itself, leaves the conversation exactly as it was; the directory then
 * only counts.
 */
static gboolean
may_present(ClawtMcpDirectory *self)
{
	return clawt_mcp_directory_decide(self->mode, self->cli_searches_tools,
					  G_MAXUINT64, G_MAXUINT64, 0, 0);
}

gboolean
clawt_mcp_directory_load(
	ClawtMcpDirectory *self,
	JsonArray         *tools
){
	guint i;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), FALSE);

	g_hash_table_remove_all(self->by_name);
	g_ptr_array_set_size(self->entries, 0);

	for (i = 0; tools != NULL && i < json_array_get_length(tools); i++) {
		JsonNode *element = json_array_get_element(tools, i);
		JsonObject *tool;
		const gchar *name;
		Entry *entry;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		tool = json_node_get_object(element);
		name = json_object_get_string_member_with_default(tool, "name",
								  NULL);

		/* The first of two tools with one name is the one a call reaches. */
		if (name == NULL || g_hash_table_contains(self->by_name, name))
			continue;

		entry = entry_new(tool, name);
		g_ptr_array_add(self->entries, entry);
		g_hash_table_insert(self->by_name, entry->name, entry);
	}

	self->n_chars = clawt_mcp_directory_measure(tools);
	self->active = clawt_mcp_directory_decide(self->mode,
						  self->cli_searches_tools,
						  self->entries->len,
						  self->n_chars,
						  self->min_tools,
						  self->min_chars);

	g_signal_emit(self, signals[SIGNAL_CATALOGUED], 0);

	return self->active;
}

gboolean
clawt_mcp_directory_is_active(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), FALSE);

	return self->active;
}

guint
clawt_mcp_directory_get_tool_count(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), 0);

	return self->entries->len;
}

guint64
clawt_mcp_directory_get_char_count(
	ClawtMcpDirectory *self
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), 0);

	return self->n_chars;
}

gboolean
clawt_mcp_directory_has_tool(
	ClawtMcpDirectory *self,
	const gchar       *name
){
	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), FALSE);

	return name != NULL && g_hash_table_contains(self->by_name, name);
}

/* ── The overview ────────────────────────────────────────────────── */

typedef struct {
	const gchar *area;
	guint        count;
	guint        first;
} AreaCount;

static gint
compare_areas(gconstpointer a, gconstpointer b)
{
	const AreaCount *x = *(const AreaCount * const *)a;
	const AreaCount *y = *(const AreaCount * const *)b;

	if (x->count != y->count)
		return (x->count > y->count) ? -1 : 1;

	return g_strcmp0(x->area, y->area);
}

/* The areas, biggest first; ties alphabetical, so the text is stable. */
static GPtrArray *
count_areas(ClawtMcpDirectory *self)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GPtrArray *areas = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; i < self->entries->len; i++) {
		Entry *entry = g_ptr_array_index(self->entries, i);
		AreaCount *count = g_hash_table_lookup(seen, entry->area);

		if (count == NULL) {
			count = g_new0(AreaCount, 1);
			count->area = entry->area;
			count->first = i;
			g_hash_table_insert(seen, entry->area, count);
			g_ptr_array_add(areas, count);
		}

		count->count++;
	}

	g_ptr_array_sort(areas, compare_areas);

	return areas;
}

gchar *
clawt_mcp_directory_overview(
	ClawtMcpDirectory *self,
	gsize              max_chars
){
	g_autoptr(GPtrArray) areas = NULL;
	GString *out;
	guint shown = 0;
	guint i;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	areas = count_areas(self);
	out = g_string_new(NULL);

	for (i = 0; i < areas->len; i++) {
		const AreaCount *area = g_ptr_array_index(areas, i);
		g_autofree gchar *part = g_strdup_printf("%s%s (%u)",
							 shown > 0 ? ", " : "",
							 area->area,
							 area->count);
		gboolean last = (i + 1 == areas->len);

		/* Room is kept for the "and N more" that may have to follow. */
		if (out->len + strlen(part) + (last ? 0 : 24) > max_chars)
			break;

		g_string_append(out, part);
		shown++;
	}

	if (shown < areas->len)
		g_string_append_printf(out, "%s... and %u more",
				       shown > 0 ? ", " : "",
				       areas->len - shown);

	return g_string_free(out, FALSE);
}

/* ── Signatures ──────────────────────────────────────────────────── */

static void type_text(JsonNode *schema, GString *out, guint depth);

static void
literal_text(JsonNode *value, GString *out)
{
	g_autofree gchar *text = render_node(value);

	if (strlen(text) <= 30)
		g_string_append(out, text);
	else
		g_string_append_printf(out, "%.29s...", text);
}

/* A schema's type, as somebody would write it in a signature. */
static void
type_text(JsonNode *node, GString *out, guint depth)
{
	JsonObject *schema;
	const gchar *type;

	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) || depth > 3) {
		g_string_append(out, "unknown");
		return;
	}

	schema = json_node_get_object(node);

	if (json_object_has_member(schema, "const")) {
		literal_text(json_object_get_member(schema, "const"), out);
		return;
	}

	if (json_object_has_member(schema, "enum") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(schema, "enum"))) {
		JsonArray *values = json_object_get_array_member(schema, "enum");
		guint n = json_array_get_length(values);
		guint i;

		if (n > 0) {
			for (i = 0; i < n && i < 6; i++) {
				if (i > 0)
					g_string_append(out, " | ");

				literal_text(json_array_get_element(values, i),
					     out);
			}

			if (n > 6)
				g_string_append(out, " | ...");

			return;
		}
	}

	{
		const gchar *choice_keys[] = { "anyOf", "oneOf", NULL };
		gsize k;

		for (k = 0; choice_keys[k] != NULL; k++) {
			JsonArray *choices;
			guint i;

			if (!json_object_has_member(schema, choice_keys[k]) ||
			    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(
				    schema, choice_keys[k])))
				continue;

			choices = json_object_get_array_member(schema,
							       choice_keys[k]);

			for (i = 0; i < json_array_get_length(choices); i++) {
				if (i > 0)
					g_string_append(out, " | ");

				type_text(json_array_get_element(choices, i), out,
					  depth + 1);
			}

			return;
		}
	}

	type = json_object_get_string_member_with_default(schema, "type", NULL);

	if (g_strcmp0(type, "integer") == 0 || g_strcmp0(type, "number") == 0) {
		g_string_append(out, "number");
	} else if (g_strcmp0(type, "array") == 0) {
		g_autoptr(GString) item = g_string_new(NULL);

		type_text(json_object_get_member(schema, "items"), item,
			  depth + 1);

		if (strchr(item->str, ' ') != NULL)
			g_string_append_printf(out, "(%s)[]", item->str);
		else
			g_string_append_printf(out, "%s[]", item->str);
	} else if (type != NULL) {
		g_string_append(out, type);
	} else if (json_object_has_member(schema, "properties")) {
		g_string_append(out, "object");
	} else {
		g_string_append(out, "unknown");
	}
}

static gboolean
in_array(JsonArray *array, const gchar *value)
{
	guint i;

	for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
		JsonNode *element = json_array_get_element(array, i);

		if (JSON_NODE_HOLDS_VALUE(element) &&
		    json_node_get_value_type(element) == G_TYPE_STRING &&
		    g_strcmp0(json_node_get_string(element), value) == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * `{ owner: string; repo: string; state?: "open" | "closed" }`, required
 * fields first, cut with a count of what was left out.
 */
static gchar *
input_signature(JsonObject *tool)
{
	JsonObject *schema = NULL;
	JsonObject *props = NULL;
	JsonArray *required = NULL;
	g_autoptr(GPtrArray) fields = g_ptr_array_new_with_free_func(g_free);
	GList *members;
	GList *l;
	GString *out;
	guint pass;
	guint kept;

	if (json_object_has_member(tool, "inputSchema") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(tool, "inputSchema")))
		schema = json_object_get_object_member(tool, "inputSchema");

	if (schema != NULL && json_object_has_member(schema, "properties") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(schema, "properties")))
		props = json_object_get_object_member(schema, "properties");

	if (schema != NULL && json_object_has_member(schema, "required") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(schema, "required")))
		required = json_object_get_array_member(schema, "required");

	if (props == NULL)
		return g_strdup("{}");

	members = json_object_get_members(props);

	for (pass = 0; pass < 2; pass++) {
		for (l = members; l != NULL; l = l->next) {
			const gchar *key = l->data;
			gboolean is_required = in_array(required, key);
			g_autoptr(GString) type = g_string_new(NULL);

			if (is_required != (pass == 0))
				continue;

			type_text(json_object_get_member(props, key), type, 0);
			g_ptr_array_add(fields,
					g_strdup_printf("%s%s: %s", key,
							is_required ? "" : "?",
							type->str));
		}
	}

	g_list_free(members);

	if (fields->len == 0)
		return g_strdup("{}");

	out = g_string_new("{ ");

	for (kept = 0; kept < fields->len; kept++) {
		const gchar *field = g_ptr_array_index(fields, kept);
		guint left = fields->len - kept - 1;
		gsize tail = (left > 0) ? 16 : 2;

		if (out->len + strlen(field) + 2 + tail > SIGNATURE_CHARS &&
		    kept > 0)
			break;

		if (kept > 0)
			g_string_append(out, "; ");

		if (strlen(field) + 2 + tail > SIGNATURE_CHARS)
			g_string_append_printf(out, "%.*s...",
					       (gint)(SIGNATURE_CHARS - tail - 8),
					       field);
		else
			g_string_append(out, field);
	}

	if (kept < fields->len)
		g_string_append_printf(out, "; ... %u more", fields->len - kept);

	g_string_append(out, " }");

	return g_string_free(out, FALSE);
}

/* Whitespace folded and cut, for a description on one line. */
static gchar *
one_line(const gchar *text, gsize max_chars)
{
	GString *out = g_string_new(NULL);
	gboolean space = FALSE;
	const gchar *p;

	for (p = (text != NULL) ? text : ""; *p != '\0'; p++) {
		if (g_ascii_isspace(*p)) {
			space = (out->len > 0);
			continue;
		}

		if (space)
			g_string_append_c(out, ' ');

		space = FALSE;
		g_string_append_c(out, *p);
	}

	if (out->len > max_chars) {
		const gchar *end = out->str + max_chars - 3;

		/* Never cut a character in half. */
		end = g_utf8_find_prev_char(out->str, end + 1);
		g_string_truncate(out, (end != NULL) ? (gsize)(end - out->str)
						     : max_chars - 3);
		g_string_append(out, "...");
	}

	return g_string_free(out, FALSE);
}

/* ── Search ──────────────────────────────────────────────────────── */

typedef struct {
	Entry  *entry;
	gdouble score;
} Scored;

static gint
compare_scored(gconstpointer a, gconstpointer b)
{
	const Scored *x = a;
	const Scored *y = b;

	if (x->score != y->score)
		return (x->score > y->score) ? -1 : 1;

	return g_strcmp0(x->entry->name, y->entry->name);
}

static void
term_add(const gchar *term, gpointer data)
{
	GPtrArray *terms = data;
	guint i;

	for (i = 0; i < terms->len; i++) {
		if (strcmp(g_ptr_array_index(terms, i), term) == 0)
			return;
	}

	g_ptr_array_add(terms, g_strdup(term));
}

static gdouble
rarity(guint total, guint holding)
{
	return log(1.0 + ((gdouble)total - (gdouble)holding + 0.5) /
		   ((gdouble)holding + 0.5));
}

static gdouble
saturated(gdouble tf)
{
	return (tf * (BM25_K1 + 1.0)) / (tf + BM25_K1);
}

static gdouble
weighted_tf(const Bag *bag, const gchar *term, gdouble weight,
	    gdouble average)
{
	guint count = bag_count(bag, term);

	if (count == 0)
		return 0.0;

	return (weight * (gdouble)count) /
	       (1.0 - BM25_B + BM25_B * (gdouble)bag->length / average);
}

/*
 * BM25 over each tool's name, words and schema, the name counting most;
 * the area is scored as a field of its own so "issues" finds everything
 * in the issues area even where a description never says the word.  A
 * query that is a tool's exact name always puts that tool first.
 */
static GArray *
rank(ClawtMcpDirectory *self, const gchar *query)
{
	g_autoptr(GPtrArray) wanted = g_ptr_array_new_with_free_func(g_free);
	GArray *scored = g_array_new(FALSE, FALSE, sizeof(Scored));
	g_autofree gchar *exact = g_ascii_strdown(query, -1);
	gdouble avg_name = 0.0;
	gdouble avg_text = 0.0;
	gdouble avg_schema = 0.0;
	gdouble avg_area = 0.0;
	guint n = self->entries->len;
	guint i;
	guint t;

	g_strstrip(exact);
	for_each_term(query, term_add, wanted);

	for (i = 0; i < n; i++) {
		Entry *e = g_ptr_array_index(self->entries, i);

		avg_name += e->name_bag.length;
		avg_text += e->text_bag.length;
		avg_schema += e->schema_bag.length;
		avg_area += e->area_bag.length;
	}

	if (n > 0) {
		avg_name /= n;
		avg_text /= n;
		avg_schema /= n;
		avg_area /= n;
	}

	/* An empty field everywhere must not divide by zero. */
	avg_name = MAX(avg_name, 1.0);
	avg_text = MAX(avg_text, 1.0);
	avg_schema = MAX(avg_schema, 1.0);
	avg_area = MAX(avg_area, 1.0);

	for (i = 0; i < n; i++) {
		Entry *e = g_ptr_array_index(self->entries, i);
		g_autofree gchar *lower = g_ascii_strdown(e->name, -1);
		gdouble score = (g_strcmp0(lower, exact) == 0) ? 1000.0 : 0.0;

		for (t = 0; t < wanted->len; t++) {
			const gchar *term = g_ptr_array_index(wanted, t);
			guint df = 0;
			guint area_df = 0;
			guint j;
			gdouble tf;
			gdouble area_tf;

			/*
			 * Document frequency is counted per term per query
			 * rather than kept as an index: a few hundred tools
			 * and a few words is a few thousand lookups.
			 */
			for (j = 0; j < n; j++) {
				Entry *o = g_ptr_array_index(self->entries, j);

				if (bag_count(&o->name_bag, term) > 0 ||
				    bag_count(&o->text_bag, term) > 0 ||
				    bag_count(&o->schema_bag, term) > 0)
					df++;

				if (bag_count(&o->area_bag, term) > 0)
					area_df++;
			}

			tf = weighted_tf(&e->name_bag, term, WEIGHT_NAME,
					 avg_name) +
			     weighted_tf(&e->text_bag, term, WEIGHT_TEXT,
					 avg_text) +
			     weighted_tf(&e->schema_bag, term, WEIGHT_SCHEMA,
					 avg_schema);

			if (tf > 0.0)
				score += rarity(n, df) * saturated(tf);

			area_tf = weighted_tf(&e->area_bag, term, WEIGHT_NAME,
					      avg_area);

			if (area_tf > 0.0)
				score += rarity(n, area_df) * saturated(area_tf);
		}

		if (score > 0.0) {
			Scored s;

			s.entry = e;
			s.score = score;
			g_array_append_val(scored, s);
		}
	}

	g_array_sort(scored, compare_scored);

	return scored;
}

static gchar *
how_to_call(ClawtMcpDirectory *self)
{
	return g_strdup_printf(
		"Run one with %s({\"name\": \"...\", \"arguments\": {...}}). "
		"%s gives a tool's exact input schema when its input is cut "
		"short.", self->call_name, self->describe_name);
}

/* The answer to an empty search: what is here, area by area. */
static gchar *
search_overview(ClawtMcpDirectory *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(GPtrArray) areas = count_areas(self);
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "tools");
	json_builder_add_int_value(b, self->entries->len);
	json_builder_set_member_name(b, "areas");
	json_builder_begin_array(b);

	for (i = 0; i < areas->len; i++) {
		const AreaCount *area = g_ptr_array_index(areas, i);
		guint j;
		guint named = 0;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "area");
		json_builder_add_string_value(b, area->area);
		json_builder_set_member_name(b, "count");
		json_builder_add_int_value(b, area->count);
		json_builder_set_member_name(b, "examples");
		json_builder_begin_array(b);

		for (j = area->first; j < self->entries->len &&
				      named < AREA_EXAMPLES; j++) {
			Entry *e = g_ptr_array_index(self->entries, j);

			if (strcmp(e->area, area->area) != 0)
				continue;

			json_builder_add_string_value(b, e->name);
			named++;
		}

		json_builder_end_array(b);
		json_builder_end_object(b);
	}

	json_builder_end_array(b);
	json_builder_set_member_name(b, "next");
	{
		g_autofree gchar *next = g_strdup_printf(
			"Search again with a few words saying what you want to "
			"do, for example %s({\"query\": \"create an issue\"}).",
			self->search_name);

		json_builder_add_string_value(b, next);
	}
	json_builder_end_object(b);

	root = json_builder_get_root(b);

	return render_node(root);
}

gchar *
clawt_mcp_directory_search(
	ClawtMcpDirectory *self,
	const gchar       *query,
	guint              limit
){
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(GArray) scored = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *trimmed = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	trimmed = g_strstrip(g_strdup(query != NULL ? query : ""));

	if (*trimmed == '\0')
		return search_overview(self);

	if (strlen(trimmed) > QUERY_CHARS)
		trimmed[QUERY_CHARS] = '\0';

	if (limit == 0)
		limit = SEARCH_LIMIT_DEFAULT;

	limit = MIN(limit, (guint)SEARCH_LIMIT_MAX);
	scored = rank(self, trimmed);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "matches");
	json_builder_begin_array(b);

	for (i = 0; i < scored->len && i < limit; i++) {
		Entry *e = g_array_index(scored, Scored, i).entry;
		JsonObject *annotations = NULL;
		g_autofree gchar *line = one_line(
			json_object_get_string_member_with_default(
				e->tool, "description",
				json_object_get_string_member_with_default(
					e->tool, "title", "")),
			DESCRIPTION_CHARS);
		g_autofree gchar *signature = input_signature(e->tool);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, e->name);
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, line);
		json_builder_set_member_name(b, "input");
		json_builder_add_string_value(b, signature);

		if (json_object_has_member(e->tool, "annotations") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(e->tool,
								  "annotations")))
			annotations = json_object_get_object_member(e->tool,
								    "annotations");

		/*
		 * The two hints worth carrying through: a call that only
		 * reads, and one that cannot be undone.
		 */
		if (annotations != NULL &&
		    json_object_get_boolean_member_with_default(annotations,
								"readOnlyHint",
								FALSE)) {
			json_builder_set_member_name(b, "readOnly");
			json_builder_add_boolean_value(b, TRUE);
		} else if (annotations != NULL &&
			   json_object_get_boolean_member_with_default(
				   annotations, "destructiveHint", FALSE)) {
			json_builder_set_member_name(b, "destructive");
			json_builder_add_boolean_value(b, TRUE);
		}

		json_builder_end_object(b);
	}

	json_builder_end_array(b);
	json_builder_set_member_name(b, "next");

	if (scored->len > 0) {
		g_autofree gchar *next = how_to_call(self);

		json_builder_add_string_value(b, next);
	} else {
		g_autofree gchar *areas = clawt_mcp_directory_overview(self, 600);
		g_autofree gchar *next = g_strdup_printf(
			"Nothing matched. Try other words, or one of these "
			"areas: %s.", areas);

		json_builder_add_string_value(b, next);
	}

	json_builder_end_object(b);
	root = json_builder_get_root(b);

	return render_node(root);
}

/* ── Describe ────────────────────────────────────────────────────── */

gchar *
clawt_mcp_directory_describe(
	ClawtMcpDirectory *self,
	const gchar       *name
){
	g_autoptr(JsonObject) definition = NULL;
	g_autofree gchar *whole = NULL;
	Entry *entry;
	const gchar *keys[] = { "name", "title", "description", "inputSchema",
				"annotations", NULL };
	gsize k;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	entry = (name != NULL) ? g_hash_table_lookup(self->by_name, name)
			       : NULL;

	if (entry == NULL)
		return NULL;

	definition = json_object_new();

	for (k = 0; keys[k] != NULL; k++) {
		if (json_object_has_member(entry->tool, keys[k]))
			json_object_set_member(definition, keys[k],
					       json_node_copy(json_object_get_member(
						       entry->tool, keys[k])));
	}

	if (!json_object_has_member(definition, "inputSchema")) {
		JsonObject *empty = json_object_new();

		json_object_set_string_member(empty, "type", "object");
		json_object_set_object_member(definition, "inputSchema", empty);
	}

	whole = render_object(definition);

	if (strlen(whole) <= DESCRIBE_BYTES)
		return g_steal_pointer(&whole);

	/*
	 * Too big to hand back whole, and cutting JSON in the middle leaves
	 * something no model can read.  The fields, their types and which
	 * are required is what a call needs.
	 */
	{
		g_autoptr(JsonObject) outline = json_object_new();
		g_autofree gchar *description = one_line(
			json_object_get_string_member_with_default(
				entry->tool, "description", ""), 4000);
		g_autofree gchar *signature = input_signature(entry->tool);

		json_object_set_string_member(outline, "name", entry->name);
		json_object_set_string_member(outline, "description", description);
		json_object_set_string_member(outline, "input", signature);
		json_object_set_string_member(
			outline, "compacted",
			"This schema is too large to show whole: the input above "
			"lists its fields, their types and which are required.");

		return render_object(outline);
	}
}

/* ── The three tools ─────────────────────────────────────────────── */

static JsonObject *
string_property(const gchar *description)
{
	JsonObject *property = json_object_new();

	json_object_set_string_member(property, "type", "string");
	json_object_set_string_member(property, "description", description);

	return property;
}

static JsonObject *
read_only(void)
{
	JsonObject *annotations = json_object_new();

	json_object_set_boolean_member(annotations, "readOnlyHint", TRUE);

	return annotations;
}

JsonArray *
clawt_mcp_directory_list_tools(
	ClawtMcpDirectory *self
){
	JsonArray *tools;
	JsonObject *tool;
	JsonObject *schema;
	JsonObject *props;
	JsonArray *required;
	g_autofree gchar *areas = NULL;
	g_autofree gchar *who = NULL;
	g_autofree gchar *name_help = NULL;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), NULL);

	tools = json_array_new();
	areas = clawt_mcp_directory_overview(self, AREAS_CHARS);
	who = g_strdup_printf("the \"%s\" MCP server", self->server);
	name_help = g_strdup_printf("The tool's exact name, as %s gave it.",
				    self->search_name);

	/* search */
	tool = json_object_new();
	json_object_set_string_member(tool, "name", self->search_name);
	{
		g_autofree gchar *description = g_strdup_printf(
			"Search the %u tools of %s. There are too many to list, "
			"so find the one you need here: say what you want to do "
			"in a few words. Each match comes with what it does and "
			"its input fields; run one with %s. An empty query "
			"answers with what the server covers, area by area.\n"
			"Areas (tools in each): %s.",
			self->entries->len, who, self->call_name, areas);

		json_object_set_string_member(tool, "description", description);
	}
	schema = json_object_new();
	props = json_object_new();
	json_object_set_string_member(schema, "type", "object");
	json_object_set_object_member(props, "query",
				      string_property("What you want to do, in "
						      "a few words. Empty for "
						      "an overview."));
	{
		JsonObject *limit = json_object_new();
		g_autofree gchar *help = g_strdup_printf(
			"How many matches to return. Default %d.",
			SEARCH_LIMIT_DEFAULT);

		json_object_set_string_member(limit, "type", "integer");
		json_object_set_int_member(limit, "minimum", 1);
		json_object_set_int_member(limit, "maximum", SEARCH_LIMIT_MAX);
		json_object_set_string_member(limit, "description", help);
		json_object_set_object_member(props, "limit", limit);
	}
	json_object_set_object_member(schema, "properties", props);
	json_object_set_boolean_member(schema, "additionalProperties", FALSE);
	json_object_set_object_member(tool, "inputSchema", schema);
	json_object_set_object_member(tool, "annotations", read_only());
	json_array_add_object_element(tools, tool);

	/* describe */
	tool = json_object_new();
	json_object_set_string_member(tool, "name", self->describe_name);
	{
		g_autofree gchar *description = g_strdup_printf(
			"Get the exact description and input schema of one tool "
			"of %s. Use it when a match's input fields are cut short "
			"or you need their exact types.", who);

		json_object_set_string_member(tool, "description", description);
	}
	schema = json_object_new();
	props = json_object_new();
	required = json_array_new();
	json_object_set_string_member(schema, "type", "object");
	json_object_set_object_member(props, "name", string_property(name_help));
	json_object_set_object_member(schema, "properties", props);
	json_array_add_string_element(required, "name");
	json_object_set_array_member(schema, "required", required);
	json_object_set_boolean_member(schema, "additionalProperties", FALSE);
	json_object_set_object_member(tool, "inputSchema", schema);
	json_object_set_object_member(tool, "annotations", read_only());
	json_array_add_object_element(tools, tool);

	/* call */
	tool = json_object_new();
	json_object_set_string_member(tool, "name", self->call_name);
	{
		g_autofree gchar *description = g_strdup_printf(
			"Run one tool of %s by its exact name, with its input as "
			"arguments, and get its result. Find the name with %s "
			"first.", who, self->search_name);

		json_object_set_string_member(tool, "description", description);
	}
	schema = json_object_new();
	props = json_object_new();
	required = json_array_new();
	json_object_set_string_member(schema, "type", "object");
	json_object_set_object_member(props, "name", string_property(name_help));
	{
		JsonObject *arguments = json_object_new();

		json_object_set_string_member(arguments, "type", "object");
		json_object_set_string_member(arguments, "description",
					      "The tool's input, matching its "
					      "input fields.");
		json_object_set_boolean_member(arguments, "additionalProperties",
					       TRUE);
		json_object_set_object_member(props, "arguments", arguments);
	}
	json_object_set_object_member(schema, "properties", props);
	json_array_add_string_element(required, "name");
	json_object_set_array_member(schema, "required", required);
	json_object_set_boolean_member(schema, "additionalProperties", FALSE);
	json_object_set_object_member(tool, "inputSchema", schema);
	json_array_add_object_element(tools, tool);

	return tools;
}

/* ── The conversation ────────────────────────────────────────────── */

/*
 * One line into its top-level object.  A batch, or anything else that
 * is not an object, is left to the relay: it refuses batches itself, and
 * the directory has nothing to say about a shape it does not know.
 */
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

static JsonObject *
member_object(JsonObject *object, const gchar *name)
{
	if (object == NULL || !json_object_has_member(object, name) ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, name)))
		return NULL;

	return json_object_get_object_member(object, name);
}

/* An id as a key: `1` and `"1"` are different requests, and stay so. */
static gchar *
id_key(JsonObject *message)
{
	if (!json_object_has_member(message, "id"))
		return NULL;

	return render_node(json_object_get_member(message, "id"));
}

/*
 * A tool result the directory wrote itself.  isError rather than a
 * JSON-RPC error for a call that was understood and could not be done:
 * a model handles "the tool says no" by trying something else, and a
 * protocol error by deciding the tool is broken.
 */
static gchar *
build_result(JsonObject *request, const gchar *text, gboolean is_error)
{
	g_autoptr(JsonObject) reply = json_object_new();
	JsonObject *result = json_object_new();
	JsonArray *content = json_array_new();
	JsonObject *item = json_object_new();

	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id",
			       json_node_copy(json_object_get_member(request,
								     "id")));
	json_object_set_string_member(item, "type", "text");
	json_object_set_string_member(item, "text", text);
	json_array_add_object_element(content, item);
	json_object_set_array_member(result, "content", content);

	if (is_error)
		json_object_set_boolean_member(result, "isError", TRUE);

	json_object_set_object_member(reply, "result", result);

	return render_object(reply);
}

static gchar *
unknown_tool(ClawtMcpDirectory *self, const gchar *name)
{
	g_autofree gchar *shown = one_line(name, 100);

	/*
	 * The same words for a tool that does not exist and one this agent
	 * may not use.  Saying "not permitted" would confirm the tool is
	 * there, which is the half of the wall the search exists to keep.
	 */
	return g_strdup_printf("No tool named \"%s\" here. Find tool names "
			       "with %s({\"query\": \"what you want to do\"}), "
			       "then run one with %s({\"name\": \"...\", "
			       "\"arguments\": {...}}).",
			       shown, self->search_name, self->call_name);
}

gboolean
clawt_mcp_directory_rewrite_outbound(
	ClawtMcpDirectory  *self,
	const gchar        *line,
	gchar             **forward,
	gchar             **reply
){
	g_autoptr(JsonObject) message = NULL;
	JsonObject *params;
	JsonObject *arguments;
	const gchar *method;
	const gchar *tool;
	gboolean notification;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), FALSE);

	*forward = NULL;
	*reply = NULL;

	message = parse_object(line);

	if (message == NULL)
		return FALSE;

	method = json_object_get_string_member_with_default(message, "method",
							    NULL);
	params = member_object(message, "params");
	notification = !json_object_has_member(message, "id");

	/*
	 * A tool list on its way out: note its id, so its answer can be
	 * gathered on the way back.  A first page starts a fresh list; a
	 * page the client asked for by cursor continues the one in hand.
	 */
	if (g_strcmp0(method, "tools/list") == 0 && !notification) {
		g_autofree gchar *key = id_key(message);
		gboolean continues = (params != NULL &&
				      json_object_has_member(params, "cursor"));

		if (!continues) {
			g_clear_pointer(&self->collecting, json_array_unref);
			self->pages = 0;
		}

		g_hash_table_replace(self->pending, g_steal_pointer(&key),
				     json_node_copy(json_object_get_member(
					     message, "id")));
		return FALSE;
	}

	if (g_strcmp0(method, "tools/call") != 0 || params == NULL ||
	    !self->active)
		return FALSE;

	tool = json_object_get_string_member_with_default(params, "name", NULL);
	arguments = member_object(params, "arguments");

	if (g_strcmp0(tool, self->search_name) != 0 &&
	    g_strcmp0(tool, self->describe_name) != 0 &&
	    g_strcmp0(tool, self->call_name) != 0)
		return FALSE;

	/* A notification expects no answer, and a directory tool runs nothing. */
	if (notification)
		return TRUE;

	if (g_strcmp0(tool, self->search_name) == 0) {
		const gchar *query = NULL;
		gint64 limit = 0;
		g_autofree gchar *text = NULL;

		if (arguments != NULL) {
			JsonNode *q = json_object_get_member(arguments, "query");
			JsonNode *n = json_object_get_member(arguments, "limit");

			if (q != NULL && JSON_NODE_HOLDS_VALUE(q) &&
			    json_node_get_value_type(q) == G_TYPE_STRING)
				query = json_node_get_string(q);

			if (n != NULL && JSON_NODE_HOLDS_VALUE(n) &&
			    json_node_get_value_type(n) == G_TYPE_INT64)
				limit = json_node_get_int(n);
			else if (n != NULL && JSON_NODE_HOLDS_VALUE(n) &&
				 json_node_get_value_type(n) == G_TYPE_DOUBLE)
				limit = (gint64)json_node_get_double(n);
		}

		text = clawt_mcp_directory_search(
			self, query, (guint)CLAMP(limit, 0, SEARCH_LIMIT_MAX));
		*reply = build_result(message, text, FALSE);
		return TRUE;
	}

	if (g_strcmp0(tool, self->describe_name) == 0) {
		const gchar *name = (arguments != NULL)
			? json_object_get_string_member_with_default(arguments,
								     "name",
								     NULL)
			: NULL;
		g_autofree gchar *text = clawt_mcp_directory_describe(self, name);

		if (text == NULL) {
			g_autofree gchar *help = unknown_tool(self, name != NULL
								  ? name : "");

			*reply = build_result(message, help, TRUE);
		} else {
			*reply = build_result(message, text, FALSE);
		}

		return TRUE;
	}

	/* The call: becomes an ordinary tools/call, naming the inner tool. */
	{
		const gchar *name = (arguments != NULL)
			? json_object_get_string_member_with_default(arguments,
								     "name",
								     NULL)
			: NULL;
		JsonNode *inner = (arguments != NULL)
			? json_object_get_member(arguments, "arguments")
			: NULL;
		g_autoptr(JsonObject) rewritten = NULL;
		JsonObject *new_params;

		if (name == NULL ||
		    (inner != NULL && !JSON_NODE_HOLDS_OBJECT(inner) &&
		     !JSON_NODE_HOLDS_NULL(inner))) {
			g_autofree gchar *help = g_strdup_printf(
				"%s needs {\"name\": \"the tool's exact name\", "
				"\"arguments\": {...}}. Find the name with %s.",
				self->call_name, self->search_name);

			*reply = build_result(message, help, TRUE);
			return TRUE;
		}

		if (!clawt_mcp_directory_has_tool(self, name)) {
			g_autofree gchar *help = unknown_tool(self, name);

			*reply = build_result(message, help, TRUE);
			return TRUE;
		}

		rewritten = json_object_new();
		new_params = json_object_new();

		json_object_set_string_member(rewritten, "jsonrpc", "2.0");
		json_object_set_member(rewritten, "id",
				       json_node_copy(json_object_get_member(
					       message, "id")));
		json_object_set_string_member(rewritten, "method", "tools/call");
		json_object_set_string_member(new_params, "name", name);

		if (inner != NULL && JSON_NODE_HOLDS_OBJECT(inner))
			json_object_set_member(new_params, "arguments",
					       json_node_copy(inner));
		else
			json_object_set_object_member(new_params, "arguments",
						      json_object_new());

		/* A progress token and its kind ride along unchanged. */
		if (json_object_has_member(params, "_meta"))
			json_object_set_member(new_params, "_meta",
					       json_node_copy(json_object_get_member(
						       params, "_meta")));

		json_object_set_object_member(rewritten, "params", new_params);
		*forward = render_object(rewritten);
		return TRUE;
	}
}

/* The answer the client is owed, under its own id, with these tools. */
static gchar *
finished_list(JsonObject *page, JsonNode *client_id, JsonArray *tools)
{
	g_autoptr(JsonObject) reply = json_object_new();
	JsonObject *result = json_object_new();
	JsonObject *original = member_object(page, "result");
	GList *members;
	GList *l;

	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id", json_node_copy(client_id));

	/* Anything else the server said about the list -- _meta -- is kept. */
	members = (original != NULL) ? json_object_get_members(original) : NULL;

	for (l = members; l != NULL; l = l->next) {
		if (g_strcmp0(l->data, "tools") == 0 ||
		    g_strcmp0(l->data, "nextCursor") == 0)
			continue;

		json_object_set_member(result, l->data,
				       json_node_copy(json_object_get_member(
					       original, l->data)));
	}

	g_list_free(members);

	json_object_set_array_member(result, "tools", json_array_ref(tools));
	json_object_set_object_member(reply, "result", result);

	return render_object(reply);
}

gboolean
clawt_mcp_directory_rewrite_inbound(
	ClawtMcpDirectory  *self,
	const gchar        *line,
	gchar             **to_client,
	gchar             **to_server
){
	g_autoptr(JsonObject) message = NULL;
	g_autoptr(JsonNode) client_id = NULL;
	g_autofree gchar *key = NULL;
	gpointer stolen_key = NULL;
	gpointer stolen_value = NULL;
	JsonObject *result;
	const gchar *cursor = NULL;
	gboolean ours;

	g_return_val_if_fail(CLAWT_IS_MCP_DIRECTORY(self), FALSE);

	*to_client = NULL;
	*to_server = NULL;

	message = parse_object(line);

	if (message == NULL || json_object_has_member(message, "method"))
		return FALSE;

	key = id_key(message);

	if (key == NULL ||
	    !g_hash_table_steal_extended(self->pending, key, &stolen_key,
					 &stolen_value))
		return FALSE;

	g_free(stolen_key);
	client_id = stolen_value;

	/* Whether this page was one the directory asked for itself. */
	{
		g_autofree gchar *client_key = render_node(client_id);

		ours = (g_strcmp0(client_key, key) != 0);
	}

	result = member_object(message, "result");

	/*
	 * An error, or a result that is not a tool list, ends the gathering.
	 * The client still gets the answer -- under its own id if the
	 * request was ours -- because a list that never arrives is a hang.
	 */
	if (result == NULL || !json_object_has_member(result, "tools") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "tools"))) {
		g_clear_pointer(&self->collecting, json_array_unref);

		if (!ours)
			return FALSE;

		json_object_set_member(message, "id", json_node_copy(client_id));
		*to_client = render_object(message);
		return TRUE;
	}

	{
		JsonArray *page = json_object_get_array_member(result, "tools");
		guint i;

		if (self->collecting == NULL)
			self->collecting = json_array_new();

		for (i = 0; i < json_array_get_length(page); i++)
			json_array_add_element(self->collecting,
					       json_node_copy(json_array_get_element(
						       page, i)));

		self->pages++;
	}

	if (json_object_has_member(result, "nextCursor") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(result, "nextCursor")))
		cursor = json_object_get_string_member_with_default(
			result, "nextCursor", NULL);

	if (cursor != NULL && *cursor != '\0') {
		g_autoptr(JsonObject) request = NULL;
		JsonObject *params;
		g_autofree gchar *private_id = NULL;
		JsonNode *id_node;

		/*
		 * Not ours to present: the client pages through itself and
		 * the directory only keeps count as it goes.
		 */
		if (!may_present(self) && !ours)
			return FALSE;

		if (self->pages < MAX_PAGES) {
			request = json_object_new();
			params = json_object_new();
			private_id = g_strdup_printf("clawtilla-directory-%u",
						     ++self->next_private);

			json_object_set_string_member(request, "jsonrpc", "2.0");
			json_object_set_string_member(request, "id", private_id);
			json_object_set_string_member(request, "method",
						      "tools/list");
			json_object_set_string_member(params, "cursor", cursor);
			json_object_set_object_member(request, "params", params);

			id_node = json_node_new(JSON_NODE_VALUE);
			json_node_set_string(id_node, private_id);
			g_hash_table_replace(self->pending,
					     render_node(id_node),
					     g_steal_pointer(&client_id));
			json_node_unref(id_node);

			*to_server = render_object(request);
			return TRUE;
		}

		g_warning("tool directory '%s': the server was still paging "
			  "after %d pages; using the %u tools gathered so far",
			  self->server, MAX_PAGES,
			  json_array_get_length(self->collecting));
	}

	/* The whole list is in. */
	{
		g_autoptr(JsonArray) gathered = g_steal_pointer(&self->collecting);

		clawt_mcp_directory_load(self, gathered);

		if (self->active) {
			g_autoptr(JsonArray) three =
				clawt_mcp_directory_list_tools(self);

			*to_client = finished_list(message, client_id, three);
			return TRUE;
		}

		/*
		 * Not a directory after all.  A list the client asked for in
		 * one page goes on untouched; one the directory gathered
		 * across pages goes on whole, as the single page the client
		 * is waiting for.
		 */
		if (!ours && self->pages <= 1)
			return FALSE;

		if (ours) {
			*to_client = finished_list(message, client_id, gathered);
			return TRUE;
		}

		return FALSE;
	}
}

/* ── The census ──────────────────────────────────────────────────── */

ClawtToolCensus *
clawt_tool_census_copy(
	const ClawtToolCensus *self
){
	ClawtToolCensus *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = g_new0(ClawtToolCensus, 1);
	copy->name = g_strdup(self->name);
	copy->tools = self->tools;
	copy->chars = self->chars;
	copy->areas = g_strdup(self->areas);
	copy->seen_at = self->seen_at;

	return copy;
}

void
clawt_tool_census_free(
	ClawtToolCensus *self
){
	if (self == NULL)
		return;

	g_free(self->name);
	g_free(self->areas);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtToolCensus, clawt_tool_census,
		    clawt_tool_census_copy, clawt_tool_census_free)

gchar *
clawt_tool_census_path(
	const gchar *state_dir
){
	g_return_val_if_fail(state_dir != NULL, NULL);

	return g_build_filename(state_dir, "connector-tools.yaml", NULL);
}

/* The whole file as a mapping, or an empty one -- never a failure. */
static YamlMapping *
census_read(const gchar *path)
{
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	YamlNode *root;

	if (path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS))
		return yaml_mapping_new();

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_file(parser, path, &error)) {
		g_warning("tool census: %s could not be read (%s); starting "
			  "afresh", path, error->message);
		return yaml_mapping_new();
	}

	root = yaml_parser_get_root(parser);

	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
		return yaml_mapping_new();

	return yaml_mapping_ref(yaml_node_get_mapping(root));
}

ClawtToolCensus *
clawt_tool_census_load(
	const gchar *path,
	const gchar *name
){
	g_autoptr(YamlMapping) all = NULL;
	YamlMapping *entry;
	ClawtToolCensus *census;

	if (name == NULL)
		return NULL;

	all = census_read(path);
	entry = yaml_mapping_has_member(all, name)
		? yaml_mapping_get_mapping_member(all, name) : NULL;

	if (entry == NULL)
		return NULL;

	census = g_new0(ClawtToolCensus, 1);
	census->name = g_strdup(name);
	census->tools = yaml_mapping_get_int_member(entry, "tools");
	census->chars = yaml_mapping_get_int_member(entry, "chars");
	census->areas = g_strdup(yaml_mapping_get_string_member(entry,
								 "areas"));
	census->seen_at = yaml_mapping_get_int_member(entry, "seen_at");

	return census;
}

gboolean
clawt_tool_census_store(
	const gchar            *path,
	const ClawtToolCensus  *census,
	GError                **error
){
	g_autoptr(YamlMapping) all = NULL;
	g_autoptr(YamlMapping) entry = NULL;
	g_autoptr(YamlNode) root = NULL;
	g_autoptr(YamlGenerator) generator = NULL;
	g_autofree gchar *text = NULL;

	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(census != NULL && census->name != NULL, FALSE);

	all = census_read(path);
	entry = yaml_mapping_new();

	yaml_mapping_set_int_member(entry, "tools", census->tools);
	yaml_mapping_set_int_member(entry, "chars", census->chars);

	if (census->areas != NULL)
		yaml_mapping_set_string_member(entry, "areas", census->areas);

	yaml_mapping_set_int_member(entry, "seen_at", census->seen_at);
	yaml_mapping_set_mapping_member(all, census->name, entry);

	root = yaml_node_new_mapping(all);
	generator = yaml_generator_new();
	yaml_generator_set_root(generator, root);
	text = yaml_generator_to_data(generator, NULL, error);

	if (text == NULL)
		return FALSE;

	return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}
