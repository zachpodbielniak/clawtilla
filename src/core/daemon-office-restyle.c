/*
 * daemon-office-restyle.c - A world pack redrawn by PixelLab and Meshy
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `office.world.restyle {base, name, style, two_d, three_d}` -- the CLI's
 * `clawtilla office world restyle`, and the custodial team's
 * clawtilla_office_world_restyle -- makes a new pack under
 * `<state_dir>/office/worlds/<name>/` with the base's layout and new
 * art:
 *
 * - two_d (default on): every tile kind the base map uses, redrawn by
 *   PixelLab at the map's tile size into the same slot of a new
 *   tiles.png, so the map needs no change;
 * - three_d (off unless asked): every piece of furniture the 3D level
 *   draws as one node, made by Meshy as a GLB under models/, and the
 *   level's nodes of that kind pointed at it.
 *
 * One card first, itemised and priced as well as each service can, and
 * nothing runs before a person approves it.  Then one step at a time,
 * each part written to `<state_dir>/office/restyle/<name>/parts/` as it
 * arrives: a restart, a failed step or a declined retry loses nothing
 * paid for, and asking again for the same name picks up where it
 * stopped.  The pack is assembled in a hidden directory and renamed into
 * place, so a half-made pack is never listed; office.world chooses it
 * only when somebody does.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"
#include "office/clawt-office-pack.h"
#include "office/clawt-office-world.h"

#include <ai-glib.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define RESTYLE_PREFIX "office-generate-restyle-"

/* ── Where things are ────────────────────────────────────────────── */

static gchar *
work_dir(
	ClawtDaemon *self,
	const gchar *name
){
	return g_build_filename(self->state_dir, "office", "restyle", name, NULL);
}

static gboolean
name_is_safe(
	const gchar *name
){
	const gchar *p;

	if (name == NULL || *name == '\0' || strlen(name) > 64 || *name == '-' ||
	    *name == '.')
		return FALSE;
	for (p = name; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return FALSE;
	return TRUE;
}

/* A world by id, from every directory the office reads. */
static ClawtOfficeWorld *
find_world(
	ClawtDaemon *self,
	const gchar *id
){
	g_auto(GStrv) dirs = clawt_daemon_office_world_dirs(self);
	g_autoptr(GPtrArray) worlds = clawt_office_worlds_discover(
		(const gchar *const *)dirs, NULL);
	guint i;

	for (i = 0; i < worlds->len; i++) {
		ClawtOfficeWorld *world = g_ptr_array_index(worlds, i);

		if (g_strcmp0(clawt_office_world_get_id(world), id) == 0)
			return g_object_ref(world);
	}
	return NULL;
}

/* A file of the base pack, read whole. */
static gchar *
read_base(
	const gchar  *dir,
	const gchar  *relative,
	gsize        *length,
	GError      **error
){
	g_autofree gchar *path = NULL;
	gchar *text = NULL;

	if (relative == NULL || !clawt_office_pack_path_is_safe(relative)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the base pack names no usable %s", relative != NULL ? relative
		                                                                 : "file");
		return NULL;
	}
	path = g_build_filename(dir, relative, NULL);
	if (!g_file_get_contents(path, &text, length, error))
		return NULL;
	return text;
}

/* ── The request ─────────────────────────────────────────────────── */

typedef struct {
	gchar     *base_dir;
	gchar     *manifest;     /* the base's office.yaml text */
	gchar     *map_path;     /* relative, from the manifest */
	gchar     *tmj;
	gchar     *level_path;   /* relative, or NULL */
	gchar     *rlevel;
	gchar     *tiles_path;   /* relative, from the map's tileset */
	guint      tile;
	guint      columns;
	JsonArray *kinds;
} Base;

static void
base_free(
	Base *base
){
	if (base == NULL)
		return;
	g_free(base->base_dir);
	g_free(base->manifest);
	g_free(base->map_path);
	g_free(base->tmj);
	g_free(base->level_path);
	g_free(base->rlevel);
	g_free(base->tiles_path);
	g_clear_pointer(&base->kinds, json_array_unref);
	g_free(base);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Base, base_free)

/*
 * A renderer's file as a path inside the pack: the world answers with
 * where it is on disk, and the new pack needs where it is in the pack.
 */
static gchar *
inside(
	const gchar *dir,
	gchar       *path
){
	g_autofree gchar *owned = path;
	g_autofree gchar *prefix = NULL;

	if (owned == NULL)
		return NULL;
	if (!g_path_is_absolute(owned))
		return g_steal_pointer(&owned);
	prefix = g_strconcat(dir, G_DIR_SEPARATOR_S, NULL);
	return g_str_has_prefix(owned, prefix) ? g_strdup(owned + strlen(prefix)) : NULL;
}

/* Everything about the base pack the restyle reads. */
static Base *
read_base_pack(
	ClawtDaemon  *self,
	const gchar  *id,
	GError      **error
){
	g_autoptr(ClawtOfficeWorld) world = find_world(self, id);
	g_autoptr(Base) base = NULL;
	g_autoptr(JsonNode) tmj = NULL;
	JsonObject *tileset;
	JsonArray *tilesets;

	if (world == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "there is no world '%s' (clawtilla office worlds)", id);
		return NULL;
	}

	base = g_new0(Base, 1);
	base->base_dir = g_strdup(clawt_office_world_get_directory(world));
	base->manifest = read_base(base->base_dir, "office.yaml", NULL, error);
	base->map_path = inside(base->base_dir, clawt_office_world_get_renderer(world, "2d"));
	base->level_path = inside(base->base_dir,
	                          clawt_office_world_get_renderer(world, "3d"));
	if (base->manifest == NULL)
		return NULL;
	base->tmj = read_base(base->base_dir, base->map_path, NULL, error);
	if (base->tmj == NULL)
		return NULL;
	if (base->level_path != NULL)
		base->rlevel = read_base(base->base_dir, base->level_path, NULL, NULL);

	/* the tileset the map draws from: its image, tile size and columns */
	tmj = json_from_string(base->tmj, error);
	if (tmj == NULL)
		return NULL;
	tilesets = JSON_NODE_HOLDS_OBJECT(tmj) &&
	           json_object_has_member(json_node_get_object(tmj), "tilesets")
		? json_object_get_array_member(json_node_get_object(tmj), "tilesets") : NULL;
	tileset = tilesets != NULL && json_array_get_length(tilesets) == 1
		? json_array_get_object_element(tilesets, 0) : NULL;
	if (tileset == NULL || !json_object_has_member(tileset, "image")) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a restyle needs a map with one embedded tileset");
		return NULL;
	}
	base->tiles_path = g_strdup(json_object_get_string_member(tileset, "image"));
	base->tile = (guint)json_object_get_int_member_with_default(tileset, "tilewidth", 32);
	base->columns = (guint)json_object_get_int_member_with_default(tileset, "columns", 8);

	base->kinds = clawt_office_restyle_kinds(base->manifest, base->tmj, base->rlevel,
	                                         error);
	if (base->kinds == NULL)
		return NULL;

	return g_steal_pointer(&base);
}

/* The steps a request makes, each `{service, gid, name, floor}`. */
static JsonArray *
steps_for(
	Base     *base,
	gboolean  two_d,
	gboolean  three_d
){
	JsonArray *steps = json_array_new();
	guint i;

	for (i = 0; i < json_array_get_length(base->kinds); i++) {
		JsonObject *kind = json_array_get_object_element(base->kinds, i);

		if (two_d) {
			JsonObject *step = json_object_new();

			json_object_set_string_member(step, "service", "pixellab");
			json_object_set_object_member(step, "kind", json_object_ref(kind));
			json_array_add_object_element(steps, step);
		}
		if (three_d && json_object_get_boolean_member_with_default(kind, "model",
		                                                           FALSE)) {
			JsonObject *step = json_object_new();

			json_object_set_string_member(step, "service", "meshy");
			json_object_set_object_member(step, "kind", json_object_ref(kind));
			json_array_add_object_element(steps, step);
		}
	}
	return steps;
}

/* What Meshy is asked to make for one piece of furniture. */
static AiMediaRequest *
model_request(
	JsonObject  *kind,
	const gchar *style
){
	g_autofree gchar *prompt = clawt_office_restyle_prompt(kind, style, TRUE);
	AiMediaRequest *request = ai_media_request_new(NULL, prompt);
	g_autoptr(JsonNode) formats = json_from_string("[\"glb\"]", NULL);

	/* the cheaper mesh model, textured: a room holds dozens of these */
	ai_media_request_set_param_string(request, "operation", "text-to-3d");
	ai_media_request_set_param_string(request, "ai_model", "meshy-6-lite");
	ai_media_request_set_param_json(request, "target_formats", formats);
	ai_media_request_set_param_int(request, "target_polycount", 3000);
	ai_media_request_set_param_string(request, "origin_at", "bottom");
	ai_media_request_set_param_boolean(request, "auto_size", TRUE);
	return request;
}

/* What PixelLab is asked to draw for one tile. */
static AiMediaRequest *
tile_request(
	JsonObject  *kind,
	const gchar *style,
	guint        tile
){
	g_autofree gchar *prompt = clawt_office_restyle_prompt(kind, style, FALSE);
	AiMediaRequest *request = ai_media_request_new(NULL, prompt);
	g_autoptr(JsonNode) size = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	gboolean floor = json_object_get_boolean_member_with_default(kind, "floor", FALSE);

	/* PixelLab draws at least 32 a side; a smaller tile is scaled down */
	json_object_set_int_member(object, "width", MAX(tile, 32u));
	json_object_set_int_member(object, "height", MAX(tile, 32u));
	json_node_take_object(size, object);

	ai_media_request_set_param_string(request, "operation", "image");
	ai_media_request_set_param_json(request, "image_size", size);
	ai_media_request_set_param_string(request, "view", "high top-down");
	/* furniture sits on the floor drawn under it; a floor covers its cell */
	ai_media_request_set_param_boolean(request, "no_background", !floor);
	return request;
}

/* The part a step writes, inside the work directory. */
static gchar *
part_path(
	ClawtDaemon *self,
	const gchar *name,
	JsonObject  *step
){
	JsonObject *kind = json_object_get_object_member(step, "kind");
	g_autofree gchar *file = g_strdup_printf(
		g_strcmp0(json_object_get_string_member(step, "service"), "meshy") == 0
		? "model-%" G_GINT64_FORMAT ".glb" : "tile-%" G_GINT64_FORMAT ".png",
		json_object_get_int_member(kind, "gid"));
	g_autofree gchar *dir = work_dir(self, name);

	return g_build_filename(dir, "parts", file, NULL);
}

/* ── Asking ──────────────────────────────────────────────────────── */

/*
 * Checks a request and puts it to a person.  Everything that can be
 * refused is refused here, where whoever asked reads it: the base, the
 * name, cairo, both keys.  The request is written beside the parts it
 * will make, so the card can be approved after a restart.
 */
gchar *
clawt_daemon_office_restyle_ask(
	ClawtDaemon  *self,
	const gchar  *base_id,
	const gchar  *name,
	const gchar  *style,
	gboolean      two_d,
	gboolean      three_d,
	const gchar  *asked_by,
	GError      **error
){
	static const gchar *const options[] = { "approve", "decline", NULL };
	g_autoptr(Base) base = NULL;
	g_autoptr(JsonArray) steps = NULL;
	g_autoptr(ClawtOfficeWorld) taken = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(GString) question = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *id = NULL;
	gint64 credits = 0;
	guint tiles = 0;
	guint models = 0;
	guint done = 0;
	guint i;

	if (!name_is_safe(name)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a world name: letters, digits, - and _",
		            name != NULL ? name : "");
		return NULL;
	}
	if (style == NULL || *style == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "say what style to draw it in (--style)");
		return NULL;
	}
	if (!two_d && !three_d) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "neither the 2D tiles nor the 3D models were asked for");
		return NULL;
	}
	taken = find_world(self, name);
	if (taken != NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "there is a world called '%s' already", name);
		return NULL;
	}
	if (two_d && !clawt_office_sheet_available()) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "this daemon was built without cairo (POPPLER=1), so it "
		                    "cannot redraw a tile sheet; ask for the 3D models only");
		return NULL;
	}

	base = read_base_pack(self, base_id, error);
	if (base == NULL)
		return NULL;
	steps = steps_for(base, two_d, three_d);
	if (json_array_get_length(steps) == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' has nothing to redraw%s", base_id,
		            three_d && !two_d ? ": its 3D level has no furniture node a "
		                                "model could stand in for" : "");
		return NULL;
	}

	/* a service with no key is refused now, not after the yes */
	{
		g_autoptr(AiMediaGenerator) pixellab = NULL;
		g_autoptr(AiMediaGenerator) meshy = NULL;

		if (two_d && (pixellab = clawt_daemon_media_office_client(self, "pixellab",
		                                                          error)) == NULL)
			return NULL;
		if (three_d && (meshy = clawt_daemon_media_office_client(self, "meshy",
		                                                         error)) == NULL)
			return NULL;
	}

	dir = work_dir(self, name);
	for (i = 0; i < json_array_get_length(steps); i++) {
		JsonObject *step = json_array_get_object_element(steps, i);
		g_autofree gchar *part = part_path(self, name, step);

		if (g_file_test(part, G_FILE_TEST_EXISTS)) {
			done++;
			continue;
		}
		if (g_strcmp0(json_object_get_string_member(step, "service"), "meshy") == 0) {
			g_autoptr(AiMediaRequest) model = model_request(
				json_object_get_object_member(step, "kind"), style);
			gint64 one = ai_meshy_estimate_credits("text-to-3d", model, NULL);

			models++;
			credits += one > 0 ? one : 0;
		} else {
			tiles++;
		}
	}

	/* the request, beside the parts, for the approval to find */
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "base");
	json_builder_add_string_value(builder, base_id);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "style");
	json_builder_add_string_value(builder, style);
	json_builder_set_member_name(builder, "two_d");
	json_builder_add_boolean_value(builder, two_d);
	json_builder_set_member_name(builder, "three_d");
	json_builder_add_boolean_value(builder, three_d);
	json_builder_set_member_name(builder, "asked_by");
	json_builder_add_string_value(builder, asked_by != NULL ? asked_by : "");
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	text = json_to_string(request, TRUE);
	path = g_build_filename(dir, "request.json", NULL);
	if (g_mkdir_with_parents(dir, 0700) != 0 ||
	    !clawt_write_file_atomic(path, text, -1, 0600, FALSE, error))
		return NULL;

	question = g_string_new(NULL);
	g_string_append_printf(question, "Restyle the office world '%s' as a new world "
	                       "'%s', in the style \"%s\"?\n", base_id, name, style);
	if (tiles > 0)
		g_string_append_printf(question, "- PixelLab: %u tile%s, one call each, "
		                       "priced by PixelLab when each is done (%s).\n",
		                       tiles, tiles == 1 ? "" : "s",
		                       CLAWT_MEDIA_PIXELLAB_TYPICAL);
	if (models > 0)
		g_string_append_printf(question, "- Meshy: %u model%s of the furniture, "
		                       "about %" G_GINT64_FORMAT " credits in all.\n",
		                       models, models == 1 ? "" : "s", credits);
	if (done > 0)
		g_string_append_printf(question, "- %u part%s already made by an earlier "
		                       "run %s kept, not made again.\n", done,
		                       done == 1 ? "" : "s", done == 1 ? "is" : "are");
	g_string_append(question, "Approve makes them and the new world beside the "
	                          "old one; nothing switches to it until you choose it. "
	                          "Decline spends nothing.");

	uuid = g_uuid_string_random();
	uuid[8] = '\0';
	id = g_strdup_printf(RESTYLE_PREFIX "%s:%s", name, uuid);
	decision = clawt_decision_new(id, asked_by != NULL && *asked_by != '\0'
	                                  ? asked_by : CLAWT_SYSTEM_SENDER,
	                              question->str);
	clawt_decision_set_options(decision, options);
	clawt_decision_set_default(decision, "decline",
	                           "nobody approved it, so nothing was spent");
	{
		g_autofree gchar *stored = clawt_decision_store_post(self->decisions,
		                                                     decision, error);

		if (stored == NULL)
			return NULL;
	}
	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "decision.asked",
		                     asked_by != NULL ? asked_by : CLAWT_SYSTEM_SENDER);

	return g_strdup(id);
}

/* ── Making it ───────────────────────────────────────────────────── */

typedef struct {
	ClawtDaemon       *self;      /* owned */
	gchar             *name;
	gchar             *style;
	gchar             *asked_by;
	Base              *base;
	JsonArray         *steps;
	guint              at;
	AiMediaGenerator  *client;    /* owned: the current step's */
	JsonObject        *provenance;
	GString           *trouble;
} Run;

static void
run_free(
	Run *run
){
	g_clear_object(&run->self);
	g_free(run->name);
	g_free(run->style);
	g_free(run->asked_by);
	base_free(run->base);
	g_clear_pointer(&run->steps, json_array_unref);
	g_clear_object(&run->client);
	g_clear_pointer(&run->provenance, json_object_unref);
	if (run->trouble != NULL)
		g_string_free(run->trouble, TRUE);
	g_free(run);
}

/* Said where whoever asked will read it, and in the daemon's log. */
static void
tell(
	Run         *run,
	const gchar *body
){
	g_message("office: %s", body);
	if (run->asked_by != NULL && *run->asked_by != '\0' &&
	    clawt_config_get_agent(run->self->config, run->asked_by) != NULL)
		clawt_daemon_office_tell_operator(run->self, run->asked_by, body);
	if (run->self->bus != NULL)
		clawt_event_bus_emit(run->self->bus, "office.changed", run->name);
}

static void step_next(Run *run);

static void
step_trouble(
	Run          *run,
	JsonObject   *step,
	const gchar  *what,
	const GError *error
){
	JsonObject *kind = json_object_get_object_member(step, "kind");

	g_string_append_printf(run->trouble, "%s%s (%s: %s%s%s)",
	                       run->trouble->len > 0 ? ", " : "",
	                       json_object_get_string_member(kind, "name"),
	                       json_object_get_string_member(step, "service"), what,
	                       error != NULL ? ": " : "", error != NULL ? error->message : "");
	g_clear_object(&run->client);
	run->at++;
	step_next(run);
}

static void
on_part_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Run *run = data;
	JsonObject *step = json_array_get_object_element(run->steps, run->at);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = clawt_daemon_media_fetch_finish(result, &error);
	g_autofree gchar *path = part_path(run->self, run->name, step);
	g_autofree gchar *dir = g_path_get_dirname(path);
	gsize size = 0;
	gconstpointer data_ptr;

	(void)source;

	if (bytes == NULL) {
		step_trouble(run, step, "not fetched", error);
		return;
	}
	data_ptr = g_bytes_get_data(bytes, &size);
	if (g_mkdir_with_parents(dir, 0700) != 0 ||
	    !clawt_write_file_atomic(path, data_ptr, (gssize)size, 0600, FALSE, &error)) {
		step_trouble(run, step, "not saved", error);
		return;
	}

	g_clear_object(&run->client);
	run->at++;
	step_next(run);
}

static void
on_step_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Run *run = data;
	JsonObject *step = json_array_get_object_element(run->steps, run->at);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = ai_media_generator_wait_finish(
		AI_MEDIA_GENERATOR(source), result, &error);
	gboolean model = g_strcmp0(json_object_get_string_member(step, "service"),
	                           "meshy") == 0;
	const gchar *url;

	if (job == NULL || ai_media_job_get_state(job) != AI_MEDIA_JOB_STATE_DONE) {
		step_trouble(run, step, "did not finish", error);
		return;
	}

	/* what made it, for provenance.yaml */
	json_object_set_string_member(run->provenance,
		json_object_get_string_member(json_object_get_object_member(step, "kind"),
		                              "name"),
		ai_media_job_get_id(job));

	url = ai_media_job_get_result_url(job, model ? "glb" : "image");
	clawt_daemon_media_fetch_async(run->self, run->client, url,
	                               model ? CLAWT_MEDIA_MAX_MODEL_BYTES
	                                     : CLAWT_MEDIA_MAX_IMAGE_BYTES,
	                               on_part_fetched, run);
}

static void
on_step_submitted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Run *run = data;
	JsonObject *step = json_array_get_object_element(run->steps, run->at);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(source),
	                                                        result, &error);
	guint first = run->self->office_generate_poll_ms > 0
		? run->self->office_generate_poll_ms : 5000;

	if (id == NULL) {
		step_trouble(run, step, "refused", error);
		return;
	}

	g_main_context_push_thread_default(run->self->main_context);
	ai_media_generator_wait_async(run->client, id, first, MAX(first, 30000u),
	                              60 * 60 * 1000, NULL, on_step_done, run);
	g_main_context_pop_thread_default(run->self->main_context);
}

static void assemble(Run *run);

static void
step_next(
	Run *run
){
	JsonObject *step;
	JsonObject *kind;
	g_autofree gchar *part = NULL;
	g_autoptr(AiMediaRequest) request = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *service;

	/* past the parts an earlier run already made */
	while (run->at < json_array_get_length(run->steps)) {
		g_free(part);
		part = part_path(run->self, run->name,
		                 json_array_get_object_element(run->steps, run->at));
		if (!g_file_test(part, G_FILE_TEST_EXISTS))
			break;
		run->at++;
	}

	if (run->at >= json_array_get_length(run->steps)) {
		assemble(run);
		return;
	}

	step = json_array_get_object_element(run->steps, run->at);
	kind = json_object_get_object_member(step, "kind");
	service = json_object_get_string_member(step, "service");
	run->client = clawt_daemon_media_office_client(run->self, service, &error);
	if (run->client == NULL) {
		step_trouble(run, step, "no key", error);
		return;
	}

	request = g_strcmp0(service, "meshy") == 0
		? model_request(kind, run->style)
		: tile_request(kind, run->style, run->base->tile);

	g_main_context_push_thread_default(run->self->main_context);
	ai_media_generator_submit_async(run->client, request, NULL, on_step_submitted, run);
	g_main_context_pop_thread_default(run->self->main_context);
}

/* A slug for a kind's model file: `coffee machine` -> `coffee-machine`. */
static gchar *
slug(
	const gchar *name
){
	gchar *out = g_ascii_strdown(name != NULL ? name : "thing", -1);
	gchar *p;

	for (p = out; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p))
			*p = '-';
	return out;
}

static gboolean
write_file(
	const gchar  *dir,
	const gchar  *relative,
	gconstpointer data,
	gsize         length,
	GError      **error
){
	g_autofree gchar *path = g_build_filename(dir, relative, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);

	return g_mkdir_with_parents(parent, 0700) == 0 &&
	       clawt_write_file_atomic(path, data, (gssize)length, 0600, FALSE, error);
}

static void
add_asset(
	JsonArray   *assets,
	const gchar *dir,
	const gchar *relative,
	const gchar *source
){
	g_autofree gchar *path = g_build_filename(dir, relative, NULL);
	g_autofree gchar *data = NULL;
	gsize length = 0;
	JsonObject *asset;

	if (!g_file_get_contents(path, &data, &length, NULL))
		return;

	asset = json_object_new();
	json_object_set_string_member(asset, "path", relative);
	{
		g_autofree gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
		                                                    (const guchar *)data,
		                                                    length);

		json_object_set_string_member(asset, "sha256", sum);
	}
	json_object_set_string_member(asset, "source", source);
	json_array_add_object_element(assets, asset);
}

/*
 * The new pack: the base copied, then its tile sheet, level, models,
 * provenance and manifest replaced, in a hidden directory renamed into
 * place once it loads and every checksum holds.
 */
static void
assemble(
	Run *run
){
	ClawtDaemon *self = run->self;
	g_autofree gchar *worlds = g_build_filename(self->state_dir, "office", "worlds",
	                                            NULL);
	g_autofree gchar *hidden_name = g_strdup_printf(".%s.making", run->name);
	g_autofree gchar *tmp = g_build_filename(worlds, hidden_name, NULL);
	g_autofree gchar *final = g_build_filename(worlds, run->name, NULL);
	g_autoptr(GHashTable) drawn = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                                    NULL,
	                                                    (GDestroyNotify)g_bytes_unref);
	g_autoptr(JsonObject) models = json_object_new();
	g_autoptr(JsonArray) assets = json_array_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtOfficeWorld) made = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *provenance = NULL;
	g_autofree gchar *body = NULL;
	GPtrArray *base_assets;
	guint i;

	/* what the steps made, from the parts on disk */
	for (i = 0; i < json_array_get_length(run->steps); i++) {
		JsonObject *step = json_array_get_object_element(run->steps, i);
		JsonObject *kind = json_object_get_object_member(step, "kind");
		g_autofree gchar *part = part_path(self, run->name, step);
		g_autofree gchar *data = NULL;
		gsize length = 0;

		if (!g_file_get_contents(part, &data, &length, NULL))
			continue;
		if (g_strcmp0(json_object_get_string_member(step, "service"), "meshy") == 0) {
			g_autofree gchar *file = slug(json_object_get_string_member(kind, "name"));
			g_autofree gchar *relative = g_strdup_printf("models/%s.glb", file);
			g_autofree gchar *gid = g_strdup_printf("%" G_GINT64_FORMAT,
			                                       json_object_get_int_member(kind, "gid"));

			json_object_set_string_member(models, gid, relative);
		} else {
			g_hash_table_insert(drawn,
			                    GUINT_TO_POINTER((guint)json_object_get_int_member(kind,
			                                                                       "gid")),
			                    g_bytes_new_take(g_steal_pointer(&data), length));
		}
	}

	if (g_hash_table_size(drawn) == 0 && json_object_get_size(models) == 0) {
		body = g_strdup_printf("The office world '%s' was not made: nothing was "
		                       "drawn (%s).", run->name, run->trouble->str);
		tell(run, body);
		run_free(run);
		return;
	}

	/* the base, copied whole, so files the restyle does not touch come too */
	if (g_file_test(tmp, G_FILE_TEST_EXISTS))
		clawt_remove_tree(tmp, worlds, NULL);
	if (g_mkdir_with_parents(worlds, 0700) != 0 ||
	    !clawt_copy_tree(run->base->base_dir, tmp, FALSE, NULL, &error))
		goto failed;

	if (g_hash_table_size(drawn) > 0) {
		g_autofree gchar *path = g_build_filename(run->base->base_dir,
		                                          run->base->tiles_path, NULL);
		g_autofree gchar *data = NULL;
		gsize length = 0;
		g_autoptr(GBytes) sheet = NULL;
		g_autoptr(GBytes) base_png = NULL;

		if (!g_file_get_contents(path, &data, &length, &error))
			goto failed;
		base_png = g_bytes_new_take(g_steal_pointer(&data), length);
		sheet = clawt_office_restyle_tiles(base_png, run->base->tile,
		                                   run->base->columns, drawn, &error);
		if (sheet == NULL ||
		    !write_file(tmp, run->base->tiles_path, g_bytes_get_data(sheet, NULL),
		                g_bytes_get_size(sheet), &error))
			goto failed;
	}

	if (json_object_get_size(models) > 0 && run->base->rlevel != NULL) {
		g_autofree gchar *level = clawt_office_restyle_level(run->base->rlevel,
		                                                     run->base->tmj, models,
		                                                     &error);
		GList *members;
		GList *l;

		if (level == NULL ||
		    !write_file(tmp, run->base->level_path, level, strlen(level), &error))
			goto failed;

		/* each model beside the level that names it */
		members = json_object_get_members(models);
		for (l = members; l != NULL; l = l->next) {
			const gchar *relative = json_object_get_string_member(models, l->data);
			guint gid = (guint)g_ascii_strtoull(l->data, NULL, 10);
			guint s;

			for (s = 0; s < json_array_get_length(run->steps); s++) {
				JsonObject *step = json_array_get_object_element(run->steps, s);
				JsonObject *kind = json_object_get_object_member(step, "kind");
				g_autofree gchar *part = NULL;
				g_autofree gchar *data = NULL;
				gsize length = 0;

				if (g_strcmp0(json_object_get_string_member(step, "service"),
				              "meshy") != 0 ||
				    (guint)json_object_get_int_member(kind, "gid") != gid)
					continue;
				part = part_path(self, run->name, step);
				if (g_file_get_contents(part, &data, &length, NULL) &&
				    !write_file(tmp, relative, data, length, &error)) {
					g_list_free(members);
					goto failed;
				}
			}
		}
		g_list_free(members);
	}

	/* provenance: which job made each kind, and whose terms apply */
	{
		g_autoptr(GString) text = g_string_new(
			"# Art in this pack was drawn by paid services for clawtilla.\n"
			"# PixelLab's terms govern the tiles and Meshy's the models;\n"
			"# the layout is the base pack's, under its own licence.\n");
		GList *members = json_object_get_members(run->provenance);
		GList *l;

		g_string_append_printf(text, "base: \"%s\"\nstyle: \"%s\"\njobs:\n",
		                       run->base->base_dir, run->style);
		for (l = members; l != NULL; l = l->next)
			g_string_append_printf(text, "  \"%s\": \"%s\"\n", (const gchar *)l->data,
			                       json_object_get_string_member(run->provenance,
			                                                     l->data));
		g_list_free(members);
		provenance = g_string_free(g_steal_pointer(&text), FALSE);
		if (!write_file(tmp, "provenance.yaml", provenance, strlen(provenance),
		                &error))
			goto failed;
	}

	/* every file the manifest vouches for, with its checksum now */
	{
		g_autoptr(ClawtOfficeWorld) base_world = clawt_office_world_load(
			run->base->base_dir, NULL);
		GList *members;
		GList *l;

		base_assets = base_world != NULL ? clawt_office_world_get_assets(base_world)
		                                 : NULL;
		for (i = 0; base_assets != NULL && i < base_assets->len; i++) {
			const ClawtOfficeAsset *asset = g_ptr_array_index(base_assets, i);
			gboolean redrawn = g_strcmp0(asset->path, run->base->tiles_path) == 0
				? g_hash_table_size(drawn) > 0
				: g_strcmp0(asset->path, run->base->level_path) == 0 &&
				  json_object_get_size(models) > 0;

			add_asset(assets, tmp, asset->path,
			          redrawn ? "clawtilla office world restyle" : "the base pack");
		}
		members = json_object_get_members(models);
		for (l = members; l != NULL; l = l->next)
			add_asset(assets, tmp, json_object_get_string_member(models, l->data),
			          "Meshy");
		g_list_free(members);
		add_asset(assets, tmp, "provenance.yaml", "clawtilla office world restyle");
	}

	manifest = clawt_office_restyle_manifest(run->base->manifest, run->name,
	                                         run->style, assets, &error);
	if (manifest == NULL ||
	    !write_file(tmp, "office.yaml", manifest, strlen(manifest), &error))
		goto failed;

	/* it must load, and every checksum hold, before anybody can choose it */
	made = clawt_office_world_load(tmp, &error);
	if (made == NULL || !clawt_office_world_verify_assets(made, &error))
		goto failed;
	if (g_rename(tmp, final) != 0) {
		g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "cannot move it into "
		            "place: %s", g_strerror(errno));
		goto failed;
	}

	body = g_strdup_printf("The office world '%s' is made: %u tile%s and %u model%s "
	                       "in the style \"%s\"%s%s. Choose it with `clawtilla config "
	                       "edit` (office.world: %s) or a client's Office page.",
	                       run->name, g_hash_table_size(drawn),
	                       g_hash_table_size(drawn) == 1 ? "" : "s",
	                       json_object_get_size(models),
	                       json_object_get_size(models) == 1 ? "" : "s", run->style,
	                       run->trouble->len > 0 ? ". Left as they were: " : "",
	                       run->trouble->str, run->name);
	tell(run, body);
	run_free(run);
	return;

failed:
	clawt_remove_tree(tmp, worlds, NULL);
	body = g_strdup_printf("The office world '%s' was not made: %s. The parts made "
	                       "so far are kept; asking again picks up from them.",
	                       run->name, error != NULL ? error->message : "unknown");
	tell(run, body);
	run_free(run);
}

void
clawt_daemon_office_restyle_start(
	ClawtDaemon *self,
	const gchar *decision_id
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	const gchar *colon;
	JsonObject *request;
	Run *run;

	if (decision_id == NULL || !g_str_has_prefix(decision_id, RESTYLE_PREFIX))
		return;
	colon = strrchr(decision_id, ':');
	if (colon == NULL)
		return;
	name = g_strndup(decision_id + strlen(RESTYLE_PREFIX),
	                 colon - decision_id - strlen(RESTYLE_PREFIX));
	if (!name_is_safe(name))
		return;

	dir = work_dir(self, name);
	path = g_build_filename(dir, "request.json", NULL);
	if (!json_parser_load_from_file(parser, path, &error) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		g_warning("office: the restyle '%s' was approved and its request is gone: %s",
		          name, error != NULL ? error->message : "not an object");
		return;
	}
	request = json_node_get_object(json_parser_get_root(parser));

	run = g_new0(Run, 1);
	run->self = g_object_ref(self);
	run->name = g_strdup(name);
	run->style = g_strdup(json_object_get_string_member_with_default(request, "style",
	                                                                 ""));
	run->asked_by = g_strdup(json_object_get_string_member_with_default(request,
	                                                                    "asked_by", ""));
	run->provenance = json_object_new();
	run->trouble = g_string_new(NULL);
	run->base = read_base_pack(self, json_object_get_string_member_with_default(
		request, "base", ""), &error);
	if (run->base == NULL) {
		g_autofree gchar *body = g_strdup_printf(
			"The office world '%s' was not made: %s.", name, error->message);

		tell(run, body);
		run_free(run);
		return;
	}
	run->steps = steps_for(run->base,
	                       json_object_get_boolean_member_with_default(request, "two_d",
	                                                                   TRUE),
	                       json_object_get_boolean_member_with_default(request, "three_d",
	                                                                   FALSE));
	step_next(run);
}
