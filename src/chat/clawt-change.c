/*
 * clawt-change.c - A change to an agent's configuration, said in its chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * When one agent reconfigured another -- disabled its routine, changed
 * its model, mounted a path into its computer -- the only trace was the
 * acting agent's own tool step, in the acting agent's chat.  Reading the
 * agent that was changed showed nothing, so a routine that stopped
 * firing looked like a scheduler bug.  This finds the changes by
 * comparing the configuration before and after each save, which is the
 * one path every writer takes, rather than at the dozens of call sites
 * that can change something.
 */

#include "clawtilla.h"

#include <string.h>
#include <yaml-glib.h>

G_DEFINE_ENUM_TYPE(ClawtChangeKind, clawt_change_kind,
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_SETTING, "setting"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_ROUTINE, "routine"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_TRIGGER, "trigger"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_MOUNT, "mount"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_INTEGRATION, "integration"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_SKILL, "skill"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_PERSONA, "persona"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_TEAM, "team"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_CREATED, "created"),
	G_DEFINE_ENUM_VALUE(CLAWT_CHANGE_REMOVED, "removed"))

G_DEFINE_ENUM_TYPE(ClawtConfigNotes, clawt_config_notes,
	G_DEFINE_ENUM_VALUE(CLAWT_CONFIG_NOTES_AGENTS, "agents"),
	G_DEFINE_ENUM_VALUE(CLAWT_CONFIG_NOTES_ALL, "all"),
	G_DEFINE_ENUM_VALUE(CLAWT_CONFIG_NOTES_OFF, "off"))

G_DEFINE_BOXED_TYPE(ClawtChange, clawt_change, clawt_change_copy,
                    clawt_change_free)

ClawtChange *
clawt_change_new(
	ClawtChangeKind  kind,
	const gchar     *agent,
	const gchar     *verb,
	const gchar     *label,
	const gchar     *detail
){
	ClawtChange *self;

	g_return_val_if_fail(agent != NULL, NULL);
	g_return_val_if_fail(verb != NULL, NULL);

	self = g_new0(ClawtChange, 1);
	self->kind = kind;
	self->agent = g_strdup(agent);
	self->verb = g_strdup(verb);
	self->label = g_strdup(label);
	self->detail = g_strdup(detail);

	return self;
}

ClawtChange *
clawt_change_copy(
	const ClawtChange *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return clawt_change_new(self->kind, self->agent, self->verb, self->label,
	                        self->detail);
}

void
clawt_change_free(
	ClawtChange *self
){
	if (self == NULL)
		return;

	g_free(self->agent);
	g_free(self->verb);
	g_free(self->label);
	g_free(self->detail);
	g_free(self);
}

/*
 * A switch with no default: a kind added later is a -Wswitch warning
 * here, not a row drawn with no icon.
 */
const gchar *
clawt_change_kind_icon(
	ClawtChangeKind kind
){
	switch (kind) {
	case CLAWT_CHANGE_SETTING:
		return "\xe2\x9a\x99";          /* ⚙ */
	case CLAWT_CHANGE_ROUTINE:
		return "\xe2\x8f\xb1";          /* ⏱ */
	case CLAWT_CHANGE_TRIGGER:
		return "\xe2\x9a\xa1";          /* ⚡ */
	case CLAWT_CHANGE_MOUNT:
		return "\xf0\x9f\x93\x81";      /* 📁 */
	case CLAWT_CHANGE_INTEGRATION:
		return "\xf0\x9f\x94\x8c";      /* 🔌 */
	case CLAWT_CHANGE_SKILL:
		return "\xf0\x9f\x93\x9c";      /* 📜 */
	case CLAWT_CHANGE_PERSONA:
		return "\xf0\x9f\x93\x9d";      /* 📝 */
	case CLAWT_CHANGE_TEAM:
		return "\xf0\x9f\x91\xa5";      /* 👥 */
	case CLAWT_CHANGE_CREATED:
		return "\xe2\x9e\x95";          /* ➕ */
	case CLAWT_CHANGE_REMOVED:
		return "\xe2\x9e\x96";          /* ➖ */
	}

	return "\xe2\x9a\x99";
}

const gchar *
clawt_change_kind_label(
	ClawtChangeKind kind
){
	switch (kind) {
	case CLAWT_CHANGE_SETTING:
		return "setting";
	case CLAWT_CHANGE_ROUTINE:
		return "routine";
	case CLAWT_CHANGE_TRIGGER:
		return "trigger";
	case CLAWT_CHANGE_MOUNT:
		return "mount";
	case CLAWT_CHANGE_INTEGRATION:
		return "integration";
	case CLAWT_CHANGE_SKILL:
		return "skills";
	case CLAWT_CHANGE_PERSONA:
		return "persona file";
	case CLAWT_CHANGE_TEAM:
		return "team";
	case CLAWT_CHANGE_CREATED:
		return "agent";
	case CLAWT_CHANGE_REMOVED:
		return "agent";
	}

	return "setting";
}

gboolean
clawt_change_kind_from_nick(
	const gchar     *nick,
	ClawtChangeKind *out_kind
){
	g_autoptr(GEnumClass) klass = NULL;
	GEnumValue *value;
	GEnumValue *values;
	guint i;

	g_return_val_if_fail(out_kind != NULL, FALSE);

	if (nick == NULL)
		return FALSE;

	klass = g_type_class_ref(CLAWT_TYPE_CHANGE_KIND);
	values = klass->values;
	value = NULL;

	/* Our own comparison: g_enum_get_value_by_nick's case rules vary. */
	for (i = 0; i < klass->n_values; i++) {
		if (g_strcmp0(values[i].value_nick, nick) == 0) {
			value = &values[i];
			break;
		}
	}

	if (value == NULL)
		return FALSE;

	*out_kind = (ClawtChangeKind)value->value;
	return TRUE;
}

gboolean
clawt_change_key_is_credential(
	const gchar *path,
	const gchar *key
){
	const ClawtSchemaEntry *entry;
	static const gchar *const names[] = {
		"token", "secret", "password", "api_key", "token_file",
		"secret_file", "password_file", NULL
	};
	guint i;

	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	entry = clawt_config_schema_lookup(path);
	if (entry != NULL && entry->type == CLAWT_SCHEMA_SECRET)
		return TRUE;

	for (i = 0; names[i] != NULL; i++) {
		if (g_strcmp0(key, names[i]) == 0)
			return TRUE;
	}

	return g_str_has_suffix(key, "_token") ||
	       g_str_has_suffix(key, "_secret") ||
	       g_str_has_suffix(key, "_password");
}

/*
 * What a note never shows: a credential, and anything in an agent's
 * `env:` or `credentials:` block -- those hold whatever the operator put
 * there, under names nobody can predict, and an API key called
 * OPENAI_KEY is still a key.  The fleet pack exports `env:` values on
 * purpose (they are configuration as often as secrets) and lists the
 * credential-shaped ones, which is why this wider rule is the note's
 * and not the shared one.
 */
static gboolean
hidden(
	const gchar *path,
	const gchar *key
){
	return clawt_change_key_is_credential(path, key) ||
	       g_str_has_prefix(path, "agents.env.") ||
	       g_str_has_prefix(path, "agents.credentials.") ||
	       strstr(path, ".env.") != NULL;
}

JsonNode *
clawt_change_config_to_json(
	const gchar *yaml_text
){
	g_autoptr(YamlParser) parser = NULL;
	JsonNode *json = NULL;
	YamlNode *root;

	if (yaml_text != NULL) {
		parser = yaml_parser_new();

		if (yaml_parser_load_from_data(parser, yaml_text, -1, NULL)) {
			root = yaml_parser_get_root(parser);

			if (root != NULL)
				json = yaml_node_to_json_node(root);
		}
	}

	if (json == NULL || !JSON_NODE_HOLDS_OBJECT(json) ||
	    json_node_get_object(json) == NULL) {
		g_clear_pointer(&json, json_node_unref);
		/* take, not init: init refs the object, and a new one would leak */
		json = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(json, json_object_new());
	}

	return json;
}

/* ── Comparing ───────────────────────────────────────────────────── */

/*
 * A value as a note shows it: a string bare, anything else as compact
 * JSON, cut at CLAWT_CHANGE_VALUE_CHARS characters.
 */
static gchar *
show_value(
	JsonNode *node
){
	g_autofree gchar *text = NULL;

	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("(none)");

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    json_node_get_value_type(node) == G_TYPE_STRING)
		text = g_strdup(json_node_get_string(node));
	else
		text = json_to_string(node, FALSE);

	if (text == NULL)
		return g_strdup("");

	if (g_utf8_strlen(text, -1) > CLAWT_CHANGE_VALUE_CHARS) {
		gchar *end = g_utf8_offset_to_pointer(text, CLAWT_CHANGE_VALUE_CHARS);

		return g_strdup_printf("%.*s...", (gint)(end - text), text);
	}

	return g_steal_pointer(&text);
}

/* A member of an object, or NULL; never asserts on a missing one. */
static JsonNode *
member(
	JsonObject  *object,
	const gchar *name
){
	if (object == NULL || !json_object_has_member(object, name))
		return NULL;

	return json_object_get_member(object, name);
}

/* A string member, or NULL when absent or not a string. */
static const gchar *
member_string(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

/*
 * A boolean member, which YAML may have written as a boolean or -- in a
 * hand-edited file -- as a string; @fallback when absent.
 */
static gboolean
member_boolean(
	JsonObject  *object,
	const gchar *name,
	gboolean     fallback
){
	JsonNode *node = member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return fallback;

	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(node);

	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_ascii_strcasecmp(json_node_get_string(node), "true") == 0 ||
		       g_ascii_strcasecmp(json_node_get_string(node), "yes") == 0;

	return fallback;
}

/*
 * The objects of a top-level list keyed on @key -- agents and routines
 * on `id` -- in file order.  An item with no key cannot be matched
 * across a save and is skipped.
 */
static GHashTable *
index_list(
	JsonObject  *root,
	const gchar *list,
	const gchar *key,
	GPtrArray   *order
){
	GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
	JsonNode *node = member(root, list);
	JsonArray *array;
	guint i;

	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return index;

	array = json_node_get_array(node);

	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *item = json_array_get_element(array, i);
		JsonObject *object;
		const gchar *id;

		if (!JSON_NODE_HOLDS_OBJECT(item))
			continue;

		object = json_node_get_object(item);
		id = member_string(object, key);

		if (id == NULL || g_hash_table_contains(index, id))
			continue;

		g_hash_table_insert(index, (gpointer)id, object);
		if (order != NULL)
			g_ptr_array_add(order, (gpointer)id);
	}

	return index;
}

/*
 * Every leaf under @object as `prefix.path` -> node.  An array is a
 * leaf: a list of mounts or skills changed as a whole is one change,
 * and comparing it element by element would report a reordering as
 * three.
 */
static void
flatten(
	JsonObject  *object,
	const gchar *prefix,
	GHashTable  *out
){
	GList *members;
	GList *l;

	if (object == NULL)
		return;

	members = json_object_get_members(object);

	for (l = members; l != NULL; l = l->next) {
		const gchar *name = l->data;
		JsonNode *node = json_object_get_member(object, name);
		g_autofree gchar *path = (prefix == NULL)
			? g_strdup(name)
			: g_strconcat(prefix, ".", name, NULL);

		if (JSON_NODE_HOLDS_OBJECT(node) && json_node_get_object(node) != NULL)
			flatten(json_node_get_object(node), path, out);
		else
			g_hash_table_insert(out, g_steal_pointer(&path), node);
	}

	g_list_free(members);
}

/* Which kind of change a setting's path is, from its first segment. */
static ClawtChangeKind
kind_for_path(
	const gchar *path
){
	if (g_str_has_prefix(path, "integrations"))
		return CLAWT_CHANGE_INTEGRATION;
	if (g_str_has_prefix(path, "skills"))
		return CLAWT_CHANGE_SKILL;
	if (g_strcmp0(path, "team") == 0 || g_strcmp0(path, "lead") == 0 ||
	    g_str_has_prefix(path, "team."))
		return CLAWT_CHANGE_TEAM;
	if (strstr(path, "mounts") != NULL)
		return CLAWT_CHANGE_MOUNT;

	return CLAWT_CHANGE_SETTING;
}

/* g_list_sort comparator for key paths. */
static gint
compare_strings(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(a, b);
}

/*
 * One agent's own settings, before and after: each leaf added, removed
 * or changed is one change.  A credential's value is never shown, only
 * that it changed.
 */
static void
diff_agent(
	const gchar *agent,
	JsonObject  *before,
	JsonObject  *after,
	GPtrArray   *changes
){
	g_autoptr(GHashTable) old_leaves = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) new_leaves = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GList *paths = NULL;
	GList *l;

	flatten(before, NULL, old_leaves);
	flatten(after, NULL, new_leaves);

	/* Sorted, so a note lists its keys in an order a test can assert. */
	paths = g_list_concat(g_hash_table_get_keys(old_leaves),
	                      g_hash_table_get_keys(new_leaves));
	paths = g_list_sort(paths, compare_strings);

	for (l = paths; l != NULL; l = l->next) {
		const gchar *path = l->data;
		JsonNode *was = g_hash_table_lookup(old_leaves, path);
		JsonNode *now = g_hash_table_lookup(new_leaves, path);
		g_autofree gchar *schema_path = NULL;
		const gchar *key;
		const gchar *verb;
		g_autofree gchar *detail = NULL;

		if (g_hash_table_contains(seen, path))
			continue;
		g_hash_table_add(seen, (gpointer)path);

		if (g_strcmp0(path, "id") == 0)
			continue;
		if (was != NULL && now != NULL && json_node_equal(was, now))
			continue;

		schema_path = g_strconcat("agents.", path, NULL);
		key = strrchr(path, '.');
		key = (key != NULL) ? key + 1 : path;

		if (g_strcmp0(key, "enabled") == 0 && now != NULL &&
		    JSON_NODE_HOLDS_VALUE(now) &&
		    json_node_get_value_type(now) == G_TYPE_BOOLEAN) {
			verb = json_node_get_boolean(now) ? "enabled" : "disabled";
		} else if (now == NULL) {
			verb = "cleared";
		} else {
			verb = "set";
		}

		if (hidden(schema_path, key)) {
			detail = g_strdup("(a credential; its value is not shown)");
		} else if (g_strcmp0(verb, "enabled") != 0 &&
		           g_strcmp0(verb, "disabled") != 0) {
			g_autofree gchar *old_text = show_value(was);
			g_autofree gchar *new_text = show_value(now);

			detail = g_strdup_printf("%s -> %s", old_text, new_text);
		}

		g_ptr_array_add(changes, clawt_change_new(kind_for_path(path), agent,
		                                          verb, path, detail));
	}

	g_list_free(paths);
}

/* What a routine or trigger is called in a note: its description, or id. */
static const gchar *
item_label(
	JsonObject  *item,
	const gchar *id
){
	const gchar *description = member_string(item, "description");

	return (description != NULL && *description != '\0') ? description : id;
}

/*
 * The agents a routine or trigger belongs to: `agent`, and a trigger's
 * `agents` list as well.
 */
static GPtrArray *
item_agents(
	JsonObject *item
){
	GPtrArray *agents = g_ptr_array_new();
	const gchar *agent = member_string(item, "agent");
	JsonNode *list = member(item, "agents");

	if (agent != NULL)
		g_ptr_array_add(agents, (gpointer)agent);

	if (list != NULL && JSON_NODE_HOLDS_ARRAY(list)) {
		JsonArray *array = json_node_get_array(list);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++) {
			const gchar *name = json_array_get_string_element(array, i);
			guint j;
			gboolean dup = FALSE;

			if (name == NULL)
				continue;
			for (j = 0; j < agents->len; j++)
				dup = dup || g_strcmp0(g_ptr_array_index(agents, j), name) == 0;
			if (!dup)
				g_ptr_array_add(agents, (gpointer)name);
		}
	}

	return agents;
}

/* Whether @agent is among @agents. */
static gboolean
has_agent(
	GPtrArray   *agents,
	const gchar *agent
){
	guint i;

	for (i = 0; i < agents->len; i++) {
		if (g_strcmp0(g_ptr_array_index(agents, i), agent) == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * The fields of an item that changed, comma-separated: "schedule, at".
 * `enabled` is left out, because it has a verb of its own.
 */
static gchar *
changed_fields(
	JsonObject *before,
	JsonObject *after
){
	g_autoptr(GString) out = g_string_new(NULL);
	g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
	GList *members;
	GList *l;

	members = g_list_concat(json_object_get_members(before),
	                        json_object_get_members(after));
	members = g_list_sort(members, compare_strings);

	for (l = members; l != NULL; l = l->next) {
		const gchar *name = l->data;
		JsonNode *was = member(before, name);
		JsonNode *now = member(after, name);

		if (g_hash_table_contains(names, name) ||
		    g_strcmp0(name, "enabled") == 0)
			continue;
		g_hash_table_add(names, (gpointer)name);

		if (was != NULL && now != NULL && json_node_equal(was, now))
			continue;

		if (out->len > 0)
			g_string_append(out, ", ");
		g_string_append(out, name);
	}

	g_list_free(members);

	return out->len > 0 ? g_strdup(out->str) : NULL;
}

/*
 * Routines or triggers, matched by id: each one added, removed, turned
 * on or off, or edited, told to every agent it belongs to.  One that
 * moved from one agent to another is removed from the first and added
 * to the second.
 */
static void
diff_items(
	JsonObject      *before,
	JsonObject      *after,
	const gchar     *list,
	ClawtChangeKind  kind,
	GPtrArray       *changes
){
	g_autoptr(GPtrArray) old_order = g_ptr_array_new();
	g_autoptr(GPtrArray) new_order = g_ptr_array_new();
	g_autoptr(GHashTable) old_items = index_list(before, list, "id", old_order);
	g_autoptr(GHashTable) new_items = index_list(after, list, "id", new_order);
	guint i;
	guint j;

	for (i = 0; i < new_order->len; i++) {
		const gchar *id = g_ptr_array_index(new_order, i);
		JsonObject *now = g_hash_table_lookup(new_items, id);
		JsonObject *was = g_hash_table_lookup(old_items, id);
		g_autoptr(GPtrArray) now_agents = item_agents(now);
		g_autoptr(GPtrArray) was_agents = (was != NULL) ? item_agents(was)
		                                               : g_ptr_array_new();
		gboolean was_on = (was != NULL) && member_boolean(was, "enabled", TRUE);
		gboolean now_on = member_boolean(now, "enabled", TRUE);
		g_autofree gchar *fields = (was != NULL) ? changed_fields(was, now)
		                                         : NULL;

		for (j = 0; j < now_agents->len; j++) {
			const gchar *agent = g_ptr_array_index(now_agents, j);

			if (was == NULL || !has_agent(was_agents, agent)) {
				g_ptr_array_add(changes, clawt_change_new(kind, agent, "added",
					item_label(now, id), NULL));
				continue;
			}

			if (was_on != now_on)
				g_ptr_array_add(changes, clawt_change_new(kind, agent,
					now_on ? "enabled" : "disabled", item_label(now, id),
					NULL));

			if (fields != NULL)
				g_ptr_array_add(changes, clawt_change_new(kind, agent,
					"updated", item_label(now, id), fields));
		}

		for (j = 0; j < was_agents->len; j++) {
			const gchar *agent = g_ptr_array_index(was_agents, j);

			if (!has_agent(now_agents, agent))
				g_ptr_array_add(changes, clawt_change_new(kind, agent,
					"removed", item_label(was, id), NULL));
		}
	}

	for (i = 0; i < old_order->len; i++) {
		const gchar *id = g_ptr_array_index(old_order, i);
		JsonObject *was = g_hash_table_lookup(old_items, id);
		g_autoptr(GPtrArray) was_agents = NULL;

		if (g_hash_table_contains(new_items, id))
			continue;

		was_agents = item_agents(was);

		for (j = 0; j < was_agents->len; j++)
			g_ptr_array_add(changes, clawt_change_new(kind,
				g_ptr_array_index(was_agents, j), "removed",
				item_label(was, id), NULL));
	}
}

/* Stable grouping by agent, keeping each agent's own order. */
static GPtrArray *
group_by_agent(
	GPtrArray *changes
){
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_change_free);
	g_autoptr(GPtrArray) agents = g_ptr_array_new();
	guint i;
	guint j;

	for (i = 0; i < changes->len; i++) {
		ClawtChange *change = g_ptr_array_index(changes, i);
		gboolean known = FALSE;

		for (j = 0; j < agents->len; j++)
			known = known || g_strcmp0(g_ptr_array_index(agents, j),
			                           change->agent) == 0;
		if (!known)
			g_ptr_array_add(agents, change->agent);
	}

	for (j = 0; j < agents->len; j++) {
		for (i = 0; i < changes->len; i++) {
			ClawtChange *change = g_ptr_array_index(changes, i);

			if (g_strcmp0(change->agent, g_ptr_array_index(agents, j)) == 0)
				g_ptr_array_add(out, clawt_change_copy(change));
		}
	}

	return out;
}

GPtrArray *
clawt_change_diff(
	JsonNode *before,
	JsonNode *after
){
	g_autoptr(GPtrArray) changes = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_change_free);
	JsonObject *old_root;
	JsonObject *new_root;
	g_autoptr(GPtrArray) old_order = g_ptr_array_new();
	g_autoptr(GPtrArray) new_order = g_ptr_array_new();
	g_autoptr(GHashTable) old_agents = NULL;
	g_autoptr(GHashTable) new_agents = NULL;
	guint i;

	g_return_val_if_fail(before != NULL && JSON_NODE_HOLDS_OBJECT(before),
	                     NULL);
	g_return_val_if_fail(after != NULL && JSON_NODE_HOLDS_OBJECT(after), NULL);

	old_root = json_node_get_object(before);
	new_root = json_node_get_object(after);
	old_agents = index_list(old_root, "agents", "id", old_order);
	new_agents = index_list(new_root, "agents", "id", new_order);

	for (i = 0; i < new_order->len; i++) {
		const gchar *id = g_ptr_array_index(new_order, i);
		JsonObject *now = g_hash_table_lookup(new_agents, id);
		JsonObject *was = g_hash_table_lookup(old_agents, id);

		if (was == NULL) {
			g_ptr_array_add(changes, clawt_change_new(CLAWT_CHANGE_CREATED,
				id, "created", member_string(now, "description"), NULL));
			continue;
		}

		diff_agent(id, was, now, changes);
	}

	for (i = 0; i < old_order->len; i++) {
		const gchar *id = g_ptr_array_index(old_order, i);

		if (!g_hash_table_contains(new_agents, id))
			g_ptr_array_add(changes, clawt_change_new(CLAWT_CHANGE_REMOVED,
				id, "removed", NULL, NULL));
	}

	diff_items(old_root, new_root, "routines", CLAWT_CHANGE_ROUTINE, changes);
	diff_items(old_root, new_root, "triggers", CLAWT_CHANGE_TRIGGER, changes);

	return group_by_agent(changes);
}

/* ── Saying it ───────────────────────────────────────────────────── */

/* "Disabled routine Bass Pro watch", "Set setting model.name: a -> b". */
static void
append_line(
	GString           *out,
	const ClawtChange *change
){
	g_autofree gchar *verb = g_strdup(change->verb);

	verb[0] = g_ascii_toupper(verb[0]);

	if (change->kind == CLAWT_CHANGE_CREATED) {
		g_string_append(out, "Created this agent");
	} else if (change->kind == CLAWT_CHANGE_REMOVED) {
		/* Told to the agent that removed it: the removed one is gone. */
		g_string_append_printf(out, "Removed agent %s", change->agent);
	} else if (change->kind == CLAWT_CHANGE_SETTING && change->label != NULL) {
		/* "Set model.name", not "Set setting model.name": the key says it. */
		g_string_append_printf(out, "%s %s", verb, change->label);
	} else {
		g_string_append_printf(out, "%s %s", verb,
		                       clawt_change_kind_label(change->kind));
		if (change->label != NULL)
			g_string_append_printf(out, " %s", change->label);
	}

	if (change->detail != NULL)
		g_string_append_printf(out, ": %s", change->detail);
}

/* Who a note names: an agent by id, another fleet's agent as atlas@lab. */
static gchar *
actor_label(
	const gchar *actor
){
	if (actor == NULL)
		return g_strdup("somebody");
	if (g_strcmp0(actor, "user") == 0)
		return g_strdup("the operator");
	if (g_strcmp0(actor, "system") == 0)
		return g_strdup("clawtilla");
	if (g_str_has_prefix(actor, "fleet:"))
		return clawt_fed_sender_label(actor);

	return g_strdup(actor);
}

gchar *
clawt_change_note_body(
	GPtrArray   *changes,
	const gchar *actor,
	const gchar *via
){
	g_autoptr(GString) out = g_string_new(NULL);
	g_autofree gchar *who = actor_label(actor);
	guint i;

	g_return_val_if_fail(changes != NULL, NULL);

	for (i = 0; i < changes->len && i < CLAWT_CHANGE_NOTE_LINES; i++) {
		if (i > 0)
			g_string_append_c(out, '\n');
		append_line(out, g_ptr_array_index(changes, i));
	}

	if (changes->len > CLAWT_CHANGE_NOTE_LINES)
		g_string_append_printf(out, "\n+%u more",
		                       changes->len - CLAWT_CHANGE_NOTE_LINES);

	g_string_append_printf(out, "\nby %s", who);
	if (g_strcmp0(via, "cli") == 0)
		g_string_append(out, ", via the CLI");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gboolean
clawt_change_should_note(
	const gchar *mode,
	const gchar *actor,
	const gchar *subject
){
	(void)subject;

	if (g_strcmp0(mode, "off") == 0)
		return FALSE;
	if (g_strcmp0(mode, "all") == 0)
		return TRUE;

	/*
	 * `agents`, and anything unknown: an agent's or another fleet's
	 * change.  The operator knows what they changed; a hand edit and the
	 * daemon's own housekeeping have nobody worth naming.
	 */
	return actor != NULL && g_strcmp0(actor, "user") != 0 &&
	       g_strcmp0(actor, "system") != 0;
}

/* ── Who is acting ───────────────────────────────────────────────── */

typedef struct {
	gchar *actor;
	gchar *via;
} Actor;

static GQueue actors = G_QUEUE_INIT;

void
clawt_change_actor_push(
	const gchar *actor,
	const gchar *via
){
	Actor *entry = g_new0(Actor, 1);

	entry->actor = g_strdup(actor);
	entry->via = g_strdup(via);
	g_queue_push_head(&actors, entry);
}

void
clawt_change_actor_pop(void)
{
	Actor *entry = g_queue_pop_head(&actors);

	if (entry == NULL) {
		g_warning("clawt_change_actor_pop() without a push");
		return;
	}

	g_free(entry->actor);
	g_free(entry->via);
	g_free(entry);
}

const gchar *
clawt_change_actor_get(void)
{
	Actor *entry = g_queue_peek_head(&actors);

	return entry != NULL ? entry->actor : NULL;
}

const gchar *
clawt_change_actor_get_via(void)
{
	Actor *entry = g_queue_peek_head(&actors);

	return entry != NULL ? entry->via : NULL;
}
