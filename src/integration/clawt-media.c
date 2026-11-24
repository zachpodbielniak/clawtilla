/*
 * clawt-media.c - Paid media services (Meshy, PixelLab): the pure half
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What a generate call asks for, what it costs, and what its results
 * are called on disk -- everything about the two services that needs no
 * network and no daemon, so it is tested without either.  The HTTP is
 * ai-glib's (AiMeshyClient, AiPixellabClient); the daemon half, which
 * resolves keys, asks the operator and writes files, is
 * core/daemon-media.c.
 */

#include "clawtilla.h"

#include <errno.h>
#include <string.h>

const gchar *
clawt_media_service_for_tool(
	const gchar *tool
){
	if (tool == NULL)
		return NULL;
	if (g_str_has_prefix(tool, "clawtilla_meshy_"))
		return "meshy";
	if (g_str_has_prefix(tool, "clawtilla_pixellab_"))
		return "pixellab";

	return NULL;
}

const gchar *
clawt_media_service_label(
	const gchar *service
){
	if (g_strcmp0(service, "meshy") == 0)
		return "Meshy";
	if (g_strcmp0(service, "pixellab") == 0)
		return "PixelLab";

	return service != NULL ? service : "?";
}

/* A string argument, or NULL when absent, empty or not a string. */
static const gchar *
argument(
	JsonObject  *arguments,
	const gchar *name
){
	JsonNode *node;

	if (arguments == NULL || !json_object_has_member(arguments, name))
		return NULL;

	node = json_object_get_member(arguments, name);
	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING ||
	    *json_node_get_string(node) == '\0')
		return NULL;

	return json_node_get_string(node);
}

AiMediaRequest *
clawt_media_request_new(
	const gchar  *service,
	JsonObject   *arguments,
	GError      **error
){
	const gchar *operation = argument(arguments, "operation");
	const gchar *prompt = argument(arguments, "prompt");
	const gchar *model = argument(arguments, "model");
	g_autoptr(AiMediaRequest) request = NULL;
	JsonNode *params;

	g_return_val_if_fail(service != NULL, NULL);

	if (operation == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s needs an operation: say which kind of job this is",
		            clawt_media_service_label(service));
		return NULL;
	}

	request = ai_media_request_new(model, prompt);
	ai_media_request_set_param_string(request, "operation", operation);

	/*
	 * `params` goes to the service untouched: both services have dozens
	 * of fields and add more, and a list of them here would be one more
	 * copy to fall behind.  The client refuses what it cannot send.
	 */
	params = (arguments != NULL && json_object_has_member(arguments, "params"))
		? json_object_get_member(arguments, "params") : NULL;

	if (params != NULL && !JSON_NODE_HOLDS_NULL(params)) {
		JsonObject *object;
		GList *members;
		GList *l;

		if (!JSON_NODE_HOLDS_OBJECT(params)) {
			g_set_error_literal(error, CLAWT_ERROR,
			                    CLAWT_ERROR_INVALID_ARGUMENT,
			                    "params must be an object of the service's "
			                    "own fields");
			return NULL;
		}

		object = json_node_get_object(params);
		members = json_object_get_members(object);

		for (l = members; l != NULL; l = l->next) {
			const gchar *name = l->data;

			/* the operation is said once, by its own argument */
			if (g_strcmp0(name, "operation") == 0)
				continue;
			ai_media_request_set_param_json(request, name,
				json_object_get_member(object, name));
		}

		g_list_free(members);
	}

	return g_steal_pointer(&request);
}

gboolean
clawt_media_request_add_image(
	const gchar     *service,
	AiMediaRequest  *request,
	const gchar     *param,
	const gchar     *path,
	GError         **error
){
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(request != NULL, FALSE);
	g_return_val_if_fail(param != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (g_strcmp0(service, "meshy") == 0) {
		g_autofree gchar *uri = ai_meshy_image_data_uri(path, error);

		if (uri == NULL)
			return FALSE;
		ai_media_request_set_param_string(request, param, uri);
		return TRUE;
	}

	if (g_strcmp0(service, "pixellab") == 0) {
		g_autoptr(GMappedFile) file = g_mapped_file_new(path, FALSE, error);
		g_autoptr(GBytes) bytes = NULL;
		g_autoptr(JsonNode) node = NULL;

		if (file == NULL)
			return FALSE;
		if (g_mapped_file_get_length(file) > CLAWT_MEDIA_MAX_IMAGE_BYTES) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "%s is larger than a picture PixelLab would take",
			            path);
			return FALSE;
		}

		bytes = g_mapped_file_get_bytes(file);
		node = ai_pixellab_image_node_new(bytes, error);
		if (node == NULL)
			return FALSE;
		ai_media_request_set_param_json(request, param, node);
		return TRUE;
	}

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
	            "no media service called '%s'", service);
	return FALSE;
}

gchar *
clawt_media_cost_line(
	const gchar *service,
	JsonObject  *arguments
){
	const gchar *operation = argument(arguments, "operation");

	if (g_strcmp0(service, "meshy") == 0) {
		g_autoptr(AiMediaRequest) request = NULL;
		g_autoptr(GError) error = NULL;
		gint64 credits = -1;

		request = clawt_media_request_new(service, arguments, NULL);
		if (request != NULL && operation != NULL)
			credits = ai_meshy_estimate_credits(operation, request, &error);

		if (credits >= 0)
			return g_strdup_printf("Meshy %s: about %" G_GINT64_FORMAT
			                       " credits, from Meshy's price list.",
			                       operation, credits);

		return g_strdup_printf("Meshy %s: no price is known for these "
		                       "settings%s%s.",
		                       operation != NULL ? operation : "(no operation)",
		                       error != NULL ? " -- " : "",
		                       error != NULL ? error->message : "");
	}

	return g_strdup_printf("PixelLab %s: priced by PixelLab when it is done "
	                       "(%s).",
	                       operation != NULL ? operation : "(no operation)",
	                       CLAWT_MEDIA_PIXELLAB_TYPICAL);
}

/* Formats that are 3D models, which get the larger cap. */
static const gchar *const model_extensions[] = {
	".glb", ".gltf", ".fbx", ".obj", ".usdz", ".stl", ".3mf", ".mtl", NULL
};

gchar *
clawt_media_file_name(
	const gchar *name,
	const gchar *url
){
	g_autofree gchar *base = NULL;
	g_autofree gchar *extension = NULL;
	gchar *p;

	g_return_val_if_fail(name != NULL, NULL);

	base = g_strdup(name);
	for (p = base; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			*p = '-';
	}
	if (*base == '\0' || base[0] == '-' || base[0] == '.')
		base[0] = 'r';

	/* A model format named as such: `glb`, `fbx`, `rig:glb`. */
	{
		const gchar *last = strrchr(name, ':');
		const gchar *format = last != NULL ? last + 1 : name;
		g_autofree gchar *dotted = g_strconcat(".", format, NULL);
		guint i;

		for (i = 0; model_extensions[i] != NULL; i++) {
			if (g_ascii_strcasecmp(dotted, model_extensions[i]) == 0)
				return g_strconcat(base, model_extensions[i], NULL);
		}
	}

	if (url != NULL && g_str_has_prefix(url, "data:")) {
		extension = g_strdup(g_str_has_prefix(url, "data:application/json")
		                     ? ".json" : ".png");
	} else if (url != NULL) {
		g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
		const gchar *path = uri != NULL ? g_uri_get_path(uri) : NULL;
		const gchar *slash = path != NULL ? strrchr(path, '/') : NULL;
		const gchar *dot = slash != NULL ? strrchr(slash, '.') : NULL;

		/* letters and digits only: a path is the server's to write */
		if (dot != NULL && strlen(dot) >= 2 && strlen(dot) <= 6) {
			const gchar *q;
			gboolean clean = TRUE;

			for (q = dot + 1; *q != '\0'; q++)
				clean = clean && g_ascii_isalnum(*q);
			if (clean)
				extension = g_ascii_strdown(dot, -1);
		}
	}

	return g_strconcat(base, extension != NULL ? extension : ".bin", NULL);
}

gsize
clawt_media_max_bytes_for(
	const gchar *file_name
){
	guint i;

	for (i = 0; file_name != NULL && model_extensions[i] != NULL; i++) {
		if (g_str_has_suffix(file_name, model_extensions[i]))
			return CLAWT_MEDIA_MAX_MODEL_BYTES;
	}

	return CLAWT_MEDIA_MAX_IMAGE_BYTES;
}

/* A YAML double-quoted scalar: every value here came from somebody else. */
static void
append_quoted(
	GString     *out,
	const gchar *key,
	const gchar *value
){
	const gchar *p;

	g_string_append_printf(out, "%s: \"", key);
	for (p = value != NULL ? value : ""; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n')
			g_string_append(out, "\\n");
		else if ((guchar)*p < 0x20)
			g_string_append_c(out, ' ');
		else
			g_string_append_c(out, *p);
	}
	g_string_append(out, "\"\n");
}

gchar *
clawt_media_provenance(
	const gchar *service,
	const gchar *operation,
	const gchar *prompt,
	const gchar *job_id,
	const gchar *cost,
	GPtrArray   *files
){
	GString *out = g_string_new("# Made by a paid media service for a clawtilla agent.\n");
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree gchar *when = g_date_time_format_iso8601(now);
	guint i;

	append_quoted(out, "service", clawt_media_service_label(service));
	append_quoted(out, "operation", operation);
	if (prompt != NULL)
		append_quoted(out, "prompt", prompt);
	append_quoted(out, "job", job_id);
	if (cost != NULL)
		append_quoted(out, "cost", cost);
	append_quoted(out, "made", when);
	append_quoted(out, "terms",
	              g_strcmp0(service, "meshy") == 0
	              ? "Meshy's terms of service govern this output: see "
	                "https://www.meshy.ai/terms"
	              : "PixelLab's terms of service govern this output: see "
	                "https://www.pixellab.ai/terms");
	g_string_append(out, "files:\n");
	for (i = 0; files != NULL && i < files->len; i++) {
		g_string_append(out, "  - ");
		append_quoted(out, "name", g_ptr_array_index(files, i));
	}

	return g_string_free(out, FALSE);
}

/* ── What it cost ────────────────────────────────────────────────── */

gchar *
clawt_media_spend_path(
	const gchar *state_dir
){
	g_return_val_if_fail(state_dir != NULL, NULL);

	return g_build_filename(state_dir, "media-spend.ndjson", NULL);
}

gboolean
clawt_media_spend_record(
	const gchar  *path,
	const gchar  *service,
	const gchar  *operation,
	const gchar  *job_id,
	gint64        micro_usd,
	gint64        credits,
	GError      **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *dir = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileOutputStream) stream = NULL;

	g_return_val_if_fail(path != NULL, FALSE);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "ts");
	json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);
	json_builder_set_member_name(builder, "service");
	json_builder_add_string_value(builder, service);
	json_builder_set_member_name(builder, "operation");
	json_builder_add_string_value(builder, operation);
	json_builder_set_member_name(builder, "job");
	json_builder_add_string_value(builder, job_id);
	json_builder_set_member_name(builder, "micro_usd");
	json_builder_add_int_value(builder, micro_usd);
	json_builder_set_member_name(builder, "credits");
	json_builder_add_int_value(builder, credits);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	line = json_to_string(root, FALSE);

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "cannot make %s: %s", dir, g_strerror(errno));
		return FALSE;
	}

	file = g_file_new_for_path(path);
	stream = g_file_append_to(file, G_FILE_CREATE_PRIVATE, NULL, error);
	if (stream == NULL)
		return FALSE;

	return g_output_stream_write_all(G_OUTPUT_STREAM(stream), line,
	                                 strlen(line), NULL, NULL, error) &&
	       g_output_stream_write_all(G_OUTPUT_STREAM(stream), "\n", 1,
	                                 NULL, NULL, error) &&
	       g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, error);
}

void
clawt_media_spend_since(
	const gchar *path,
	gint64       since,
	gint64       credit_micros,
	gint64      *micro_usd,
	gint64      *unpriced_credits,
	gint64      *calls
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	gint64 total = 0;
	gint64 unpriced = 0;
	gint64 count = 0;
	guint i;

	if (path != NULL && g_file_get_contents(path, &text, NULL, NULL))
		lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines != NULL && lines[i] != NULL; i++) {
		g_autoptr(JsonParser) parser = NULL;
		JsonObject *row;
		gint64 micros;
		gint64 credits;

		if (*lines[i] == '\0')
			continue;

		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, lines[i], -1, NULL) ||
		    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
			continue;

		row = json_node_get_object(json_parser_get_root(parser));
		if (json_object_get_int_member_with_default(row, "ts", 0) < since)
			continue;

		micros = json_object_get_int_member_with_default(row, "micro_usd", -1);
		credits = json_object_get_int_member_with_default(row, "credits", -1);
		count++;

		/* Saturating: a corrupt line must not reopen a cap. */
		if (micros >= 0) {
			total = (micros > G_MAXINT64 - total) ? G_MAXINT64 : total + micros;
		} else if (credits > 0 && credit_micros > 0) {
			gint64 priced = (credits > G_MAXINT64 / credit_micros)
				? G_MAXINT64 : credits * credit_micros;

			total = (priced > G_MAXINT64 - total) ? G_MAXINT64 : total + priced;
		} else if (credits > 0) {
			unpriced += credits;
		}
	}

	if (micro_usd != NULL)
		*micro_usd = total;
	if (unpriced_credits != NULL)
		*unpriced_credits = unpriced;
	if (calls != NULL)
		*calls = count;
}
