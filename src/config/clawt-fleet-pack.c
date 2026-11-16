/*
 * clawt-fleet-pack.c - A fleet as a directory: exported, read, taken in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <string.h>
#include <yaml-glib.h>

#include "clawtilla.h"
#include "config/clawt-fleet-pack.h"

/* The config sections a pack carries, each a list of items with an id. */
static const gchar *const SECTIONS[] = {
	"agents", "teams", "routines", "triggers", "projects", NULL
};

/* What one item of each section is called, in to_fill and resolutions. */
static const gchar *
kind_of(const gchar *section)
{
	if (g_strcmp0(section, "agents") == 0)
		return "agent";
	if (g_strcmp0(section, "teams") == 0)
		return "team";
	if (g_strcmp0(section, "routines") == 0)
		return "routine";
	if (g_strcmp0(section, "triggers") == 0)
		return "trigger";
	return "project";
}

/*
 * Keys that name this machine rather than the fleet: left behind, and
 * said so.  An agent's `workspace` is a path here; on the new machine
 * the default under defaults.workspace_root is the right one.
 */
static gboolean
machine_specific(const gchar *path)
{
	return g_strcmp0(path, "agents.workspace") == 0;
}

/*
 * A credential by its schema type, or -- for keys the schema types as
 * plain strings -- by its name.  Both are left behind: a secret
 * reference names a file or variable on this machine, and a value is a
 * value.
 */
static gboolean
credential(const gchar *path, const gchar *key)
{
	const ClawtSchemaEntry *entry = clawt_config_schema_lookup(path);
	static const gchar *const names[] = {
		"token", "secret", "password", "api_key", "token_file",
		"secret_file", "password_file", NULL
	};
	guint i;

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
 * The member names of @mapping, copied: a walk that removes members must
 * not be reading names the mapping owns and frees.
 */
static GStrv
member_names(YamlMapping *mapping)
{
	GList *members = yaml_mapping_get_members(mapping);
	g_autoptr(GStrvBuilder) names = g_strv_builder_new();
	GList *l;

	for (l = members; l != NULL; l = l->next) {
		if (l->data != NULL)
			g_strv_builder_add(names, l->data);
	}

	g_list_free(members);

	return g_strv_builder_end(names);
}

/*
 * Walks an item, removing credentials and machine-specific keys, and
 * says each one in @to_fill as `<kind> <id>: <key path>`.
 */
static void
strip_item(YamlMapping *mapping,
           const gchar *path,
           const gchar *label,
           GPtrArray   *to_fill)
{
	g_auto(GStrv) keys = member_names(mapping);
	guint k;

	for (k = 0; keys[k] != NULL; k++) {
		const gchar *key = keys[k];
		g_autofree gchar *child = g_strconcat(path, ".", key, NULL);
		YamlNode *node = yaml_mapping_get_member(mapping, key);
		const gchar *inner = strchr(child, '.') + 1;

		if (credential(child, key) || machine_specific(child)) {
			g_ptr_array_add(to_fill, g_strdup_printf("%s: %s", label, inner));
			yaml_mapping_remove_member(mapping, key);
			continue;
		}

		if (node != NULL && yaml_node_get_node_type(node) ==
		                    YAML_NODE_MAPPING)
			strip_item(yaml_node_get_mapping(node), child, label, to_fill);
	}
}

/* Whether a scalar reads as "on" -- false, 0, no and empty do not. */
static gboolean
truthy(YamlNode *node)
{
	const gchar *text;

	if (node == NULL)
		return FALSE;
	if (yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return TRUE;

	text = yaml_node_get_string(node);

	return text != NULL && *text != '\0' &&
	       g_ascii_strcasecmp(text, "false") != 0 &&
	       g_ascii_strcasecmp(text, "no") != 0 && g_strcmp0(text, "0") != 0;
}

/* The dangerous settings an item turns on, as key paths. */
static void
find_dangerous(YamlMapping *mapping,
               const gchar *path,
               GPtrArray   *found)
{
	g_auto(GStrv) keys = member_names(mapping);
	guint k;

	for (k = 0; keys[k] != NULL; k++) {
		const gchar *key = keys[k];
		g_autofree gchar *child = g_strconcat(path, ".", key, NULL);
		YamlNode *node = yaml_mapping_get_member(mapping, key);
		const ClawtSchemaEntry *entry = clawt_config_schema_lookup(child);

		if (entry != NULL && (entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) &&
		    entry->type != CLAWT_SCHEMA_SECTION && truthy(node))
			g_ptr_array_add(found, g_strdup(strchr(child, '.') + 1));

		if (node != NULL && yaml_node_get_node_type(node) ==
		                    YAML_NODE_MAPPING)
			find_dangerous(yaml_node_get_mapping(node), child, found);
	}

	/* A host computer is the host: that is the widening in itself. */
	if (g_strcmp0(path, "agents") == 0) {
		YamlNode *computer = yaml_mapping_get_member(mapping, "computer");

		if (computer != NULL &&
		    yaml_node_get_node_type(computer) == YAML_NODE_MAPPING &&
		    g_strcmp0(yaml_mapping_get_string_member(
		                  yaml_node_get_mapping(computer), "type"),
		              "host") == 0)
			g_ptr_array_add(found, g_strdup("computer.type: host"));
	}
}

/* A text as a fresh, writable YAML tree. */
static YamlNode *
parse_tree(const gchar *text, GError **error)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	YamlNode *root;

	if (!yaml_parser_load_from_data(parser, text, -1, error))
		return NULL;

	root = yaml_parser_get_root(parser);

	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		                    "not a mapping");
		return NULL;
	}

	return yaml_node_copy(root);
}

static gchar *
tree_text(YamlNode *root)
{
	g_autoptr(YamlGenerator) generator = yaml_generator_new();

	yaml_generator_set_root(generator, root);
	return yaml_generator_to_data(generator, NULL, NULL);
}

static YamlSequence *
section_of(YamlNode *root, const gchar *section, gboolean create)
{
	YamlMapping *mapping = yaml_node_get_mapping(root);
	YamlNode *node = yaml_mapping_get_member(mapping, section);

	if (node == NULL && create) {
		g_autoptr(YamlNode) fresh = yaml_node_new_sequence(NULL);

		yaml_mapping_set_member(mapping, section, fresh);
		node = yaml_mapping_get_member(mapping, section);
	}

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
		return NULL;

	return yaml_node_get_sequence(node);
}

static const gchar *
item_id(YamlNode *item)
{
	if (item == NULL || yaml_node_get_node_type(item) != YAML_NODE_MAPPING)
		return NULL;

	return yaml_mapping_get_string_member(yaml_node_get_mapping(item), "id");
}

/* Copies one file, creating its directory; refuses to follow a link. */
static gboolean
copy_file(const gchar *from, const gchar *to, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = g_path_get_dirname(to);
	gsize length = 0;

	if (g_file_test(from, G_FILE_TEST_IS_SYMLINK))
		return TRUE;

	if (!g_file_get_contents(from, &text, &length, error))
		return FALSE;

	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	return clawt_write_file_atomic(to, text, (gssize)length, 0600, FALSE,
	                               error);
}

/* A skill's tests (tests/<name>.yaml), from one directory to another. */
static guint
copy_tests(const gchar *from_dir, const gchar *to_dir, GError **error)
{
	g_autofree gchar *from = g_build_filename(from_dir, "tests", NULL);
	g_autoptr(GDir) listing = g_dir_open(from, 0, NULL);
	const gchar *name;
	guint copied = 0;

	if (listing == NULL)
		return 0;

	while ((name = g_dir_read_name(listing)) != NULL) {
		g_autofree gchar *source = NULL;
		g_autofree gchar *target = NULL;

		if (!g_str_has_suffix(name, ".yaml") && !g_str_has_suffix(name,
		                                                           ".yml"))
			continue;

		source = g_build_filename(from, name, NULL);
		target = g_build_filename(to_dir, "tests", name, NULL);

		if (!copy_file(source, target, error))
			return copied;
		copied++;
	}

	return copied;
}

/* ── Export ──────────────────────────────────────────────────────── */

JsonNode *
clawt_fleet_pack_export(ClawtConfig        *config,
                        ClawtSkillLibrary  *skills,
                        const gchar        *destination,
                        GError            **error)
{
	g_autofree gchar *text = NULL;
	g_autoptr(YamlNode) current = NULL;
	g_autoptr(YamlNode) manifest = NULL;
	g_autoptr(GPtrArray) to_fill = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(JsonBuilder) report = json_builder_new();
	g_autofree gchar *manifest_path = NULL;
	g_autofree gchar *manifest_text = NULL;
	YamlMapping *out;
	guint files = 0;
	guint n_skills = 0;
	guint s;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(destination != NULL, NULL);

	/* Fresh, or empty: a pack never lands on top of something else. */
	if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
		g_autoptr(GDir) dir = g_dir_open(destination, 0, NULL);

		if (dir == NULL || g_dir_read_name(dir) != NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
			            "%s is not empty; a pack is written into a new "
			            "directory", destination);
			return NULL;
		}
	}

	if (!clawt_ensure_dir(destination, 0700, error))
		return NULL;

	text = clawt_config_to_string(config);
	current = parse_tree(text, error);
	if (current == NULL)
		return NULL;

	manifest = yaml_node_new_mapping(NULL);
	out = yaml_node_get_mapping(manifest);
	yaml_mapping_set_string_member(out, "format", "clawtilla-fleet");
	yaml_mapping_set_int_member(out, "format_version", 1);
	yaml_mapping_set_string_member(out, "exported_by",
		G_STRINGIFY(CLAWT_VERSION_MAJOR) "." G_STRINGIFY(CLAWT_VERSION_MINOR)
		"." G_STRINGIFY(CLAWT_VERSION_MICRO));
	{
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		g_autofree gchar *at = g_date_time_format_iso8601(now);

		yaml_mapping_set_string_member(out, "exported_at", at);
	}

	json_builder_begin_object(report);

	/* The five sections, each item stripped of what must not travel. */
	for (s = 0; SECTIONS[s] != NULL; s++) {
		YamlSequence *items = section_of(current, SECTIONS[s], FALSE);
		g_autoptr(YamlNode) copy = yaml_node_new_sequence(NULL);
		guint length = (items != NULL) ? yaml_sequence_get_length(items) : 0;

		for (i = 0; i < length; i++) {
			YamlNode *item = yaml_sequence_get_element(items, i);
			g_autoptr(YamlNode) mine = NULL;
			g_autofree gchar *label = NULL;

			if (item_id(item) == NULL)
				continue;

			mine = yaml_node_copy(item);
			label = g_strdup_printf("%s %s", kind_of(SECTIONS[s]),
			                        item_id(item));
			strip_item(yaml_node_get_mapping(mine), SECTIONS[s], label,
			           to_fill);
			yaml_sequence_add_element(yaml_node_get_sequence(copy), mine);
		}

		if (yaml_sequence_get_length(yaml_node_get_sequence(copy)) > 0)
			yaml_mapping_set_member(out, SECTIONS[s], copy);

		json_builder_set_member_name(report, SECTIONS[s]);
		json_builder_add_int_value(report, yaml_sequence_get_length(
			yaml_node_get_sequence(copy)));
	}

	/* Each agent's persona: the org files, never the generated loaders. */
	{
		GPtrArray *agents = clawt_config_get_agents(config);
		const ClawtWorkspaceFile *workspace_files;
		guint n_files = 0;
		guint a;

		workspace_files = clawt_workspace_files(&n_files);

		for (a = 0; agents != NULL && a < agents->len; a++) {
			ClawtAgentConfig *agent = g_ptr_array_index(agents, a);

			if (clawt_agent_config_is_shadow(agent))
				continue;

			for (i = 0; i < n_files; i++) {
				g_autofree gchar *from = NULL;
				g_autofree gchar *to = NULL;

				if (workspace_files[i].generated ||
				    !g_str_has_suffix(workspace_files[i].name, ".org"))
					continue;

				from = clawt_workspace_file_path(agent,
				                                 workspace_files[i].name);
				if (from == NULL || !g_file_test(from,
				                                 G_FILE_TEST_IS_REGULAR))
					continue;

				to = g_build_filename(destination, "agents",
				                      clawt_agent_config_get_id(agent),
				                      workspace_files[i].name, NULL);
				if (!copy_file(from, to, error))
					return NULL;
				files++;
			}
		}
	}

	/* The library's own skills -- not bundled ones, not extra roots. */
	if (skills != NULL && clawt_skill_library_get_directory(skills) != NULL) {
		g_autoptr(GPtrArray) all = clawt_skill_library_list(skills);
		const gchar *root = clawt_skill_library_get_directory(skills);

		for (i = 0; all != NULL && i < all->len; i++) {
			ClawtSkill *skill = g_ptr_array_index(all, i);
			const gchar *dir = clawt_skill_get_directory(skill);
			g_autofree gchar *from = NULL;
			g_autofree gchar *target = NULL;
			g_autofree gchar *to = NULL;

			if (dir == NULL || !clawt_path_is_within(dir, root))
				continue;

			from = g_build_filename(dir, "SKILL.md", NULL);
			target = g_build_filename(destination, "skills",
			                          clawt_skill_get_name(skill), NULL);
			to = g_build_filename(target, "SKILL.md", NULL);

			if (!copy_file(from, to, error))
				return NULL;
			files += 1 + copy_tests(dir, target, NULL);
			n_skills++;
		}
	}

	/* What the new fleet will have to supply itself. */
	{
		g_autoptr(YamlNode) list = yaml_node_new_sequence(NULL);

		for (i = 0; i < to_fill->len; i++)
			yaml_sequence_add_string_element(yaml_node_get_sequence(list),
			                                 g_ptr_array_index(to_fill, i));
		yaml_mapping_set_member(out, "to_fill", list);
	}

	manifest_path = g_build_filename(destination, CLAWT_FLEET_PACK_MANIFEST,
	                                 NULL);
	manifest_text = tree_text(manifest);

	if (!clawt_write_file_atomic(manifest_path, manifest_text, -1, 0600,
	                             FALSE, error))
		return NULL;

	json_builder_set_member_name(report, "skills");
	json_builder_add_int_value(report, n_skills);
	json_builder_set_member_name(report, "files");
	json_builder_add_int_value(report, files);
	json_builder_set_member_name(report, "to_fill");
	json_builder_begin_array(report);
	for (i = 0; i < to_fill->len; i++)
		json_builder_add_string_value(report, g_ptr_array_index(to_fill, i));
	json_builder_end_array(report);
	json_builder_end_object(report);

	return json_builder_get_root(report);
}

/* ── Reading a pack ──────────────────────────────────────────────── */

static YamlNode *
read_manifest(const gchar *source, GError **error)
{
	g_autofree gchar *path = g_build_filename(source,
	                                          CLAWT_FLEET_PACK_MANIFEST, NULL);
	g_autofree gchar *text = NULL;
	YamlNode *root;

	if (!g_file_get_contents(path, &text, NULL, error))
		return NULL;

	root = parse_tree(text, error);

	if (root != NULL &&
	    g_strcmp0(yaml_mapping_get_string_member(yaml_node_get_mapping(root),
	                                             "format"),
	              "clawtilla-fleet") != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s is not a clawtilla fleet pack", path);
		yaml_node_unref(root);
		return NULL;
	}

	return root;
}

static gboolean
item_exists(ClawtConfig *config, const gchar *section, const gchar *id)
{
	if (g_strcmp0(section, "agents") == 0)
		return clawt_config_get_agent(config, id) != NULL;
	if (g_strcmp0(section, "routines") == 0)
		return clawt_config_get_routine(config, id) != NULL;
	if (g_strcmp0(section, "triggers") == 0)
		return clawt_config_get_trigger(config, id) != NULL;
	if (g_strcmp0(section, "teams") == 0) {
		g_autoptr(ClawtTeamSpec) team = clawt_config_get_team(config, id);

		return team != NULL;
	} else {
		g_autoptr(ClawtProjectSpec) project = clawt_config_get_project(config,
		                                                               id);

		return project != NULL;
	}
}

JsonNode *
clawt_fleet_pack_preview(ClawtConfig        *config,
                         ClawtSkillLibrary  *skills,
                         const gchar        *source,
                         GError            **error)
{
	g_autoptr(YamlNode) manifest = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	YamlMapping *mapping;
	YamlNode *to_fill;
	guint s;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	manifest = read_manifest(source, error);
	if (manifest == NULL)
		return NULL;

	mapping = yaml_node_get_mapping(manifest);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);

	for (s = 0; SECTIONS[s] != NULL; s++) {
		YamlSequence *items = section_of(manifest, SECTIONS[s], FALSE);

		for (i = 0; items != NULL && i < yaml_sequence_get_length(items);
		     i++) {
			YamlNode *item = yaml_sequence_get_element(items, i);
			const gchar *id = item_id(item);
			g_autoptr(GPtrArray) dangerous = NULL;
			guint d;

			if (id == NULL)
				continue;

			dangerous = g_ptr_array_new_with_free_func(g_free);
			find_dangerous(yaml_node_get_mapping(item), SECTIONS[s],
			               dangerous);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "kind");
			json_builder_add_string_value(builder, kind_of(SECTIONS[s]));
			json_builder_set_member_name(builder, "id");
			json_builder_add_string_value(builder, id);
			json_builder_set_member_name(builder, "exists");
			json_builder_add_boolean_value(builder,
				item_exists(config, SECTIONS[s], id));
			json_builder_set_member_name(builder, "dangerous");
			json_builder_begin_array(builder);
			for (d = 0; d < dangerous->len; d++)
				json_builder_add_string_value(builder,
				                              g_ptr_array_index(dangerous, d));
			json_builder_end_array(builder);
			json_builder_end_object(builder);
		}
	}

	/* The skills in the pack, by their directories. */
	{
		g_autofree gchar *dir = g_build_filename(source, "skills", NULL);
		g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
		const gchar *name;

		while (listing != NULL &&
		       (name = g_dir_read_name(listing)) != NULL) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "kind");
			json_builder_add_string_value(builder, "skill");
			json_builder_set_member_name(builder, "id");
			json_builder_add_string_value(builder, name);
			json_builder_set_member_name(builder, "exists");
			json_builder_add_boolean_value(builder, skills != NULL &&
				clawt_skill_library_lookup(skills, name) != NULL);
			json_builder_set_member_name(builder, "dangerous");
			json_builder_begin_array(builder);
			json_builder_end_array(builder);
			json_builder_end_object(builder);
		}
	}

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "to_fill");
	json_builder_begin_array(builder);
	to_fill = yaml_mapping_get_member(mapping, "to_fill");
	if (to_fill != NULL &&
	    yaml_node_get_node_type(to_fill) == YAML_NODE_SEQUENCE) {
		YamlSequence *list = yaml_node_get_sequence(to_fill);

		for (i = 0; i < yaml_sequence_get_length(list); i++)
			json_builder_add_string_value(builder, yaml_node_get_string(
				yaml_sequence_get_element(list, i)));
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ── Taking one in ───────────────────────────────────────────────── */

/* What to do with `<kind>:<id>`: skip, replace, add or rename:<new>. */
static const gchar *
resolution_for(JsonObject  *resolutions,
               const gchar *kind,
               const gchar *id,
               gboolean     exists)
{
	g_autofree gchar *key = g_strdup_printf("%s:%s", kind, id);
	const gchar *said = (resolutions != NULL &&
	                     json_object_has_member(resolutions, key))
		? json_object_get_string_member(resolutions, key) : NULL;

	if (said != NULL)
		return said;

	return exists ? "skip" : "add";
}

/* Removes the item called @id from @items; TRUE if one was there. */
static gboolean
remove_item(YamlSequence *items, const gchar *id)
{
	guint i;

	for (i = 0; items != NULL && i < yaml_sequence_get_length(items); i++) {
		if (g_strcmp0(item_id(yaml_sequence_get_element(items, i)), id) == 0) {
			yaml_sequence_remove_element(items, i);
			return TRUE;
		}
	}

	return FALSE;
}

/* `runtime.autostart: false`, whatever the pack said: arrived held. */
static void
hold_agent(YamlNode *item)
{
	YamlMapping *mapping = yaml_node_get_mapping(item);
	YamlNode *runtime = yaml_mapping_get_member(mapping, "runtime");

	if (runtime == NULL || yaml_node_get_node_type(runtime) !=
	                       YAML_NODE_MAPPING) {
		g_autoptr(YamlNode) fresh = yaml_node_new_mapping(NULL);

		yaml_mapping_set_member(mapping, "runtime", fresh);
		runtime = yaml_mapping_get_member(mapping, "runtime");
	}

	yaml_mapping_set_boolean_member(yaml_node_get_mapping(runtime),
	                                "autostart", FALSE);
}

gchar *
clawt_fleet_pack_merge(ClawtConfig  *config,
                       const gchar  *source,
                       JsonObject   *resolutions,
                       gboolean      allow_dangerous,
                       GPtrArray   **out_taken,
                       GError      **error)
{
	g_autoptr(YamlNode) manifest = NULL;
	g_autoptr(YamlNode) merged = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) taken = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) refused = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(ClawtConfig) check = NULL;
	guint s;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	manifest = read_manifest(source, error);
	if (manifest == NULL)
		return NULL;

	current = clawt_config_to_string(config);
	merged = parse_tree(current, error);
	if (merged == NULL)
		return NULL;

	for (s = 0; SECTIONS[s] != NULL; s++) {
		YamlSequence *items = section_of(manifest, SECTIONS[s], FALSE);
		const gchar *kind = kind_of(SECTIONS[s]);

		for (i = 0; items != NULL && i < yaml_sequence_get_length(items);
		     i++) {
			YamlNode *item = yaml_sequence_get_element(items, i);
			const gchar *id = item_id(item);
			const gchar *what;
			g_autoptr(YamlNode) copy = NULL;
			g_autoptr(GPtrArray) dangerous = NULL;
			const gchar *as;

			if (id == NULL)
				continue;

			what = resolution_for(resolutions, kind, id,
			                      item_exists(config, SECTIONS[s], id));

			if (g_strcmp0(what, "skip") == 0)
				continue;

			/* Nothing comes in that widens what agents may do, unasked. */
			dangerous = g_ptr_array_new_with_free_func(g_free);
			find_dangerous(yaml_node_get_mapping(item), SECTIONS[s],
			               dangerous);
			if (dangerous->len > 0 && !allow_dangerous) {
				g_autofree gchar *keys = NULL;

				g_ptr_array_add(dangerous, NULL);
				keys = g_strjoinv(", ", (gchar **)dangerous->pdata);
				g_ptr_array_add(refused, g_strdup_printf("%s %s (%s)", kind,
				                                         id, keys));
				continue;
			}

			copy = yaml_node_copy(item);
			as = id;

			if (g_str_has_prefix(what, "rename:")) {
				as = what + strlen("rename:");

				if (!clawt_is_valid_id(as) ||
				    item_exists(config, SECTIONS[s], as)) {
					g_set_error(error, CLAWT_ERROR,
					            CLAWT_ERROR_INVALID_ARGUMENT,
					            "%s %s cannot be renamed to '%s': not a "
					            "usable id, or already taken", kind, id, as);
					return NULL;
				}

				yaml_mapping_set_string_member(yaml_node_get_mapping(copy),
				                               "id", as);
			} else if (g_strcmp0(what, "replace") == 0) {
				remove_item(section_of(merged, SECTIONS[s], FALSE), id);
			} else if (g_strcmp0(what, "add") != 0) {
				g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
				            "%s %s: '%s' is not skip, replace, add or "
				            "rename:<id>", kind, id, what);
				return NULL;
			} else if (item_exists(config, SECTIONS[s], id)) {
				g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
				            "%s %s already exists: skip it, replace it or "
				            "rename it", kind, id);
				return NULL;
			}

			if (g_strcmp0(SECTIONS[s], "agents") == 0)
				hold_agent(copy);

			yaml_sequence_add_element(section_of(merged, SECTIONS[s], TRUE),
			                          copy);
			g_ptr_array_add(taken, g_strdup_printf("%s:%s", kind, as));
		}
	}

	if (refused->len > 0) {
		g_autofree gchar *list = NULL;

		g_ptr_array_add(refused, NULL);
		list = g_strjoinv("; ", (gchar **)refused->pdata);
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		            "these would turn on dangerous settings, so nothing was "
		            "taken in: %s. Import with allow_dangerous to take them "
		            "anyway, or skip them", list);
		return NULL;
	}

	/* Loaded before it is handed back: a merge that will not load is not. */
	text = tree_text(merged);
	check = clawt_config_load_from_string(text, error);
	if (check == NULL)
		return NULL;

	if (out_taken != NULL)
		*out_taken = g_steal_pointer(&taken);

	return g_steal_pointer(&text);
}

gboolean
clawt_fleet_pack_install_files(ClawtConfig        *config,
                               ClawtSkillLibrary  *skills,
                               const gchar        *source,
                               GPtrArray          *taken,
                               JsonObject         *resolutions,
                               GError            **error)
{
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), FALSE);

	/* Persona files, into each taken agent's workspace, never over one. */
	for (i = 0; taken != NULL && i < taken->len; i++) {
		const gchar *entry = g_ptr_array_index(taken, i);
		const gchar *as;
		const gchar *from_id;
		ClawtAgentConfig *agent;
		g_autofree gchar *from_dir = NULL;
		g_autoptr(GDir) listing = NULL;
		const gchar *name;
		g_autofree gchar *workspace = NULL;

		if (!g_str_has_prefix(entry, "agent:"))
			continue;

		as = entry + strlen("agent:");
		from_id = as;

		/* a renamed agent's files are under its old name in the pack */
		if (resolutions != NULL) {
			GList *keys = json_object_get_members(resolutions);
			GList *l;

			for (l = keys; l != NULL; l = l->next) {
				const gchar *key = l->data;
				const gchar *value = json_object_get_string_member(
					resolutions, key);

				if (g_str_has_prefix(key, "agent:") &&
				    g_str_has_prefix(value, "rename:") &&
				    g_strcmp0(value + strlen("rename:"), as) == 0)
					from_id = key + strlen("agent:");
			}
			g_list_free(keys);
		}

		agent = clawt_config_get_agent(config, as);
		if (agent == NULL)
			continue;

		workspace = clawt_agent_config_get_workspace(agent);
		from_dir = g_build_filename(source, "agents", from_id, NULL);
		listing = g_dir_open(from_dir, 0, NULL);

		while (listing != NULL && (name = g_dir_read_name(listing)) != NULL) {
			g_autofree gchar *from = NULL;
			g_autofree gchar *to = NULL;

			if (!g_str_has_suffix(name, ".org") || strchr(name, '/') != NULL)
				continue;

			from = g_build_filename(from_dir, name, NULL);
			to = g_build_filename(workspace, name, NULL);

			if (g_file_test(to, G_FILE_TEST_EXISTS))
				continue;

			if (!copy_file(from, to, error))
				return FALSE;
		}
	}

	/* Skills, through the library's own import: disabled, markdown only. */
	if (skills != NULL) {
		g_autofree gchar *dir = g_build_filename(source, "skills", NULL);
		g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
		const gchar *name;

		while (listing != NULL && (name = g_dir_read_name(listing)) != NULL) {
			g_autofree gchar *from = g_build_filename(dir, name, NULL);
			const gchar *what = resolution_for(resolutions, "skill", name,
				clawt_skill_library_lookup(skills, name) != NULL);
			ClawtSkill *imported;

			if (g_strcmp0(what, "skip") == 0)
				continue;

			if (g_strcmp0(what, "replace") == 0 &&
			    !clawt_skill_library_remove(skills, name, error))
				return FALSE;

			imported = clawt_skill_library_import(skills, from,
			                                      "fleet pack", error);
			if (imported == NULL)
				return FALSE;

			copy_tests(from, clawt_skill_get_directory(imported), NULL);
		}
	}

	return TRUE;
}
