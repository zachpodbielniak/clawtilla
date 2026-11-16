/*
 * daemon-fleet.c - fleet.export, fleet.import_preview, fleet.import
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's side of a fleet pack (clawt-fleet-pack.h): written where
 * the operator says, read back with what it would change, and taken in
 * the way any edit is -- the merged config written, then reloaded, so
 * the import is a version in the history like any other save; then the
 * persona files and skills put in place.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

/*
 * What to do with each item: an object of `"<kind>:<id>": "skip" |
 * "replace" | "rename:<id>"`, or -- from a client whose payloads are
 * strings -- the same as `kind:id=what,kind:id=what`.
 */
static JsonObject *
resolutions_of(JsonObject *payload)
{
	JsonNode *node;
	JsonObject *out;
	g_auto(GStrv) pairs = NULL;
	guint i;

	if (payload == NULL || !json_object_has_member(payload, "resolutions"))
		return NULL;

	node = json_object_get_member(payload, "resolutions");

	if (JSON_NODE_HOLDS_OBJECT(node))
		return json_object_ref(json_node_get_object(node));

	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	out = json_object_new();
	pairs = g_strsplit(json_node_get_string(node), ",", -1);

	for (i = 0; pairs[i] != NULL; i++) {
		g_auto(GStrv) halves = g_strsplit(g_strstrip(pairs[i]), "=", 2);

		if (g_strv_length(halves) == 2 && *halves[0] != '\0')
			json_object_set_string_member(out, g_strstrip(halves[0]),
			                              g_strstrip(halves[1]));
	}

	return out;
}

/* A directory named over the wire must be absolute: the daemon's cwd is
 * nobody's business. */
static const gchar *
directory_of(JsonObject *payload, GError **error)
{
	const gchar *dir = clawt_ipc_payload_string(payload, "dir");

	if (dir == NULL || !g_path_is_absolute(dir)) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "dir must be an absolute path on the daemon's "
		                    "machine");
		return NULL;
	}

	return dir;
}

JsonNode *
clawt_daemon_handle_fleet(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(GError) error = NULL;
	const gchar *dir;

	if (!g_str_has_prefix(kind, "fleet.")) {
		*handled = FALSE;
		return NULL;
	}

	if (g_strcmp0(kind, "fleet.export") != 0 &&
	    g_strcmp0(kind, "fleet.import_preview") != 0 &&
	    g_strcmp0(kind, "fleet.import") != 0) {
		*handled = FALSE;
		return NULL;
	}

	*handled = TRUE;
	dir = directory_of(payload, &error);

	if (dir == NULL)
		return clawt_ipc_error_new(request, error->code, error->message);

	if (g_strcmp0(kind, "fleet.export") == 0) {
		JsonNode *report = clawt_fleet_pack_export(self->config, self->skills,
		                                           dir, &error);

		if (report == NULL)
			return clawt_ipc_error_new(request, error->code, error->message);

		return clawt_ipc_response_new(request, report);
	}

	if (g_strcmp0(kind, "fleet.import_preview") == 0) {
		JsonNode *preview = clawt_fleet_pack_preview(self->config,
		                                             self->skills, dir,
		                                             &error);

		if (preview == NULL)
			return clawt_ipc_error_new(request, error->code, error->message);

		return clawt_ipc_response_new(request, preview);
	}

	/* fleet.import */
	{
		g_autoptr(JsonObject) resolutions = resolutions_of(payload);
		g_autoptr(GPtrArray) taken = NULL;
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autofree gchar *text = NULL;
		guint i;

		text = clawt_fleet_pack_merge(self->config, dir, resolutions,
		    clawt_ipc_payload_boolean(payload, "allow_dangerous", FALSE),
		    &taken, &error);

		if (text == NULL)
			return clawt_ipc_error_new(request, error->code, error->message);

		/*
		 * The persona files first, into the workspaces the merged config
		 * names: the reload scaffolds a new agent's workspace, and it only
		 * fills in files that are missing -- so the pack's own go in now,
		 * or the templates would be there before them.
		 */
		{
			g_autoptr(ClawtConfig) merged = clawt_config_load_from_string(
				text, &error);

			if (merged == NULL ||
			    !clawt_fleet_pack_install_files(merged, self->skills, dir,
			                                    taken, resolutions, &error))
				return clawt_ipc_error_new(request, error->code,
				                           error->message);
		}

		/*
		 * Written, then taken up as a hand edit would be: the reload is
		 * what records it in the history, and what starts nothing -- the
		 * agents arrived with autostart off.
		 */
		self->revision_author = "import";
		if (!clawt_write_file_atomic(self->config_path, text, -1, 0600, TRUE,
		                             &error) ||
		    !clawt_daemon_reload(self, &error)) {
			self->revision_author = NULL;
			return clawt_ipc_error_new(request, error->code, error->message);
		}
		self->revision_author = NULL;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "taken");
		json_builder_begin_array(builder);
		for (i = 0; i < taken->len; i++)
			json_builder_add_string_value(builder, g_ptr_array_index(taken,
			                                                         i));
		json_builder_end_array(builder);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}
}
