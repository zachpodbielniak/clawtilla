/*
 * daemon-office-media.c - Office characters from PixelLab and Meshy
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The direct road (`clawtilla office character AGENT --direct`) made
 * one picture: a character that stood still in every pose.  These two
 * roads make what the renderer already knows how to draw:
 *
 * - pixellab: a character in four directions, then its walk in each
 *   direction, laid out as a sheet with rows idle, walk_down, walk_up,
 *   walk_left and walk_right (clawt_office_sheet_compose()).  Without
 *   cairo the front view alone, standing.
 * - meshy: a model from the agent's picture (or its description), then
 *   rigged; Meshy's rig comes with a walking clip, a GLB whose animation
 *   the 3D view plays when the character walks.
 *
 * One card for everything a request will make, priced as well as each
 * service can price it, and nothing runs before a person approves it.
 * Several steps per road, each waited for with the generator's own
 * backoff; a step that fails stops its road, keeps what the road already
 * made, and says so in the operator's chat with the agent.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"
#include "office/clawt-office-pack.h"

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* An office media card: still a generation card, so settle reaches it. */
#define MEDIA_PREFIX "office-generate-media-"

/* PixelLab's four directions, in the order the sheet's walk rows use. */
static const gchar *const walk_directions[] = { "south", "north", "west", "east" };

/* ── Which roads ─────────────────────────────────────────────────── */

static gboolean
is_media_road(
	const gchar *road
){
	return g_strcmp0(road, "pixellab") == 0 || g_strcmp0(road, "meshy") == 0;
}

/*
 * The roads one request makes: office.asset_api when it is a media
 * service, and office.asset_api_3d (Meshy) beside it.  @all_roads is
 * FALSE when office.asset_api is another road that runs on its own, so
 * only the 3D one is left here.
 */
static GPtrArray *
roads_for(
	ClawtDaemon *self,
	gboolean     all_roads
){
	const gchar *flat = clawt_config_get_string(self->config, "office.asset_api");
	const gchar *deep = clawt_config_get_string(self->config, "office.asset_api_3d");
	GPtrArray *roads = g_ptr_array_new();

	if (all_roads && is_media_road(flat))
		g_ptr_array_add(roads, (gpointer)(g_strcmp0(flat, "meshy") == 0
		                                  ? "meshy" : "pixellab"));
	if (g_strcmp0(deep, "meshy") == 0 &&
	    !(roads->len > 0 && g_strcmp0(g_ptr_array_index(roads, 0), "meshy") == 0))
		g_ptr_array_add(roads, (gpointer)"meshy");

	return roads;
}

gboolean
clawt_daemon_office_media_wanted(
	ClawtDaemon *self
){
	return is_media_road(clawt_config_get_string(self->config, "office.asset_api"));
}

/* The size a PixelLab character is drawn at, kept within what it takes. */
static guint
pixel_size(
	ClawtDaemon *self
){
	gint64 size = clawt_config_get_int(self->config, "office.pixel_size");

	return (guint)CLAMP(size > 0 ? size : 48, 16, 128);
}

static gchar *
describe(
	ClawtAgentConfig *agent
){
	const gchar *id = clawt_agent_config_get_id(agent);
	const gchar *name = clawt_agent_config_get_string(agent, "name");
	const gchar *description = clawt_agent_config_get_string(agent, "description");

	return g_strdup_printf("%s%s%s, an office worker, friendly, simple clean shapes",
	                       name != NULL ? name : id,
	                       description != NULL && *description != '\0' ? ", who " : "",
	                       description != NULL ? description : "");
}

/* The agent's picture as a data URI, or NULL when it has none. */
static gchar *
picture_uri(
	ClawtAgentConfig *agent
){
	g_autofree gchar *path = clawt_daemon_office_agent_picture(agent);
	g_autofree gchar *data = NULL;
	g_autofree gchar *encoded = NULL;
	const gchar *mime;
	gsize length = 0;

	if (path == NULL || !g_file_get_contents(path, &data, &length, NULL))
		return NULL;
	mime = clawt_avatar_sniff_mime_type((const guchar *)data, length);
	if (mime == NULL || g_strcmp0(mime, "image/svg+xml") == 0)
		return NULL;
	encoded = g_base64_encode((const guchar *)data, length);
	return g_strdup_printf("data:%s;base64,%s", mime, encoded);
}

/* ── The requests ────────────────────────────────────────────────── */

static AiMediaRequest *
pixellab_character(
	ClawtDaemon      *self,
	ClawtAgentConfig *agent
){
	g_autofree gchar *prompt = describe(agent);
	AiMediaRequest *request = ai_media_request_new(NULL, prompt);
	g_autoptr(JsonNode) size = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	guint px = pixel_size(self);

	json_object_set_int_member(object, "width", px);
	json_object_set_int_member(object, "height", px);
	json_node_take_object(size, object);

	ai_media_request_set_param_string(request, "operation", "character-4");
	ai_media_request_set_param_json(request, "image_size", size);
	ai_media_request_set_param_string(request, "view", "low top-down");
	return request;
}

static AiMediaRequest *
pixellab_walk(
	const gchar *character_id,
	const gchar *direction,
	const gchar *group_id
){
	AiMediaRequest *request = ai_media_request_new(NULL, "walking");
	g_autoptr(JsonNode) directions = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(JsonArray) one = json_array_new();

	/*
	 * The template, not a custom action: PixelLab's custom modes animate
	 * the south view only unless told otherwise, and the first live run
	 * came back with a single "walk" row in which she turned to face east
	 * halfway.  The walking template defaults to every direction the
	 * character has, one generation each, on its own skeleton.
	 */
	ai_media_request_set_param_string(request, "operation", "character-animate");
	ai_media_request_set_param_string(request, "character_id", character_id);
	ai_media_request_set_param_string(request, "mode", "template");
	ai_media_request_set_param_string(request, "template_animation_id", "walking");

	/*
	 * One direction per call, the later ones added to the first's group.
	 * All four in one call is four generations at once, and PixelLab's
	 * trial and lower plans allow one at a time: the second live run was
	 * refused with HTTP 403 for exactly that.  Asking in turn works on
	 * every plan and costs the same.
	 */
	json_array_add_string_element(one, direction);
	json_node_set_array(directions, one);
	ai_media_request_set_param_json(request, "directions", directions);
	if (group_id != NULL)
		ai_media_request_set_param_string(request, "animation_group_id", group_id);
	return request;
}

static AiMediaRequest *
meshy_model(
	ClawtDaemon      *self,
	ClawtAgentConfig *agent
){
	g_autofree gchar *uri = picture_uri(agent);
	g_autofree gchar *prompt = describe(agent);
	g_autoptr(JsonNode) formats = json_from_string("[\"glb\"]", NULL);
	gint64 polycount = clawt_config_get_int(self->config, "office.mesh_polycount");
	AiMediaRequest *request;

	/* the agent's own picture when it has one: it is the face people know */
	if (uri != NULL) {
		request = ai_media_request_new(NULL, NULL);
		ai_media_request_set_param_string(request, "operation", "image-to-3d");
		ai_media_request_set_param_string(request, "image_url", uri);
	} else {
		g_autofree gchar *full = g_strdup_printf(
			"%s, standing in an A-pose, a full-body game character", prompt);

		request = ai_media_request_new(NULL, full);
		ai_media_request_set_param_string(request, "operation", "text-to-3d");
	}

	ai_media_request_set_param_string(request, "pose_mode", "a-pose");
	ai_media_request_set_param_json(request, "target_formats", formats);
	ai_media_request_set_param_boolean(request, "should_remesh", TRUE);
	ai_media_request_set_param_int(request, "target_polycount",
	                               CLAMP(polycount > 0 ? polycount : 8000, 100, 300000));
	return request;
}

static AiMediaRequest *
meshy_rig(
	const gchar *task_id
){
	AiMediaRequest *request = ai_media_request_new(NULL, NULL);

	ai_media_request_set_param_string(request, "operation", "rig");
	ai_media_request_set_param_string(request, "input_task_id", task_id);
	ai_media_request_set_param_string(request, "animation_type", "biped");
	return request;
}

/* ── Asking ──────────────────────────────────────────────────────── */

/* One road's line on the card: what it makes and what it costs. */
static gchar *
road_line(
	ClawtDaemon      *self,
	ClawtAgentConfig *agent,
	const gchar      *road
){
	if (g_strcmp0(road, "pixellab") == 0) {
		guint px = pixel_size(self);

		return g_strdup_printf(
			"PixelLab: a %ux%u character in four directions, then its walk "
			"in each -- five calls, one at a time, priced by PixelLab when each is done (%s)",
			px, px, CLAWT_MEDIA_PIXELLAB_TYPICAL);
	}

	{
		g_autoptr(AiMediaRequest) model = meshy_model(self, agent);
		g_autoptr(AiMediaRequest) rig = meshy_rig("x");
		const gchar *operation = ai_media_request_has_param(model, "image_url")
			? "image-to-3d" : "text-to-3d";
		gint64 first = ai_meshy_estimate_credits(operation, model, NULL);
		gint64 second = ai_meshy_estimate_credits("rig", rig, NULL);

		if (first >= 0 && second >= 0)
			return g_strdup_printf(
				"Meshy: a 3D model from %s (%s), then rigged with a walk -- "
				"about %" G_GINT64_FORMAT " credits",
				g_strcmp0(operation, "image-to-3d") == 0
				? "its picture" : "its description",
				operation, first + second);

		return g_strdup_printf(
			"Meshy: a 3D model from %s, then rigged with a walk -- a price "
			"Meshy's list does not give for these settings",
			g_strcmp0(operation, "image-to-3d") == 0
			? "its picture" : "its description");
	}
}

gboolean
clawt_daemon_office_media_ask(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	gboolean      all_roads,
	GError      **error
){
	static const gchar *const options[] = { "approve", "decline", NULL };
	ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
	g_autoptr(GPtrArray) roads = roads_for(self, all_roads);
	g_autoptr(GString) question = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autofree gchar *uuid = g_uuid_string_random();
	g_autofree gchar *id = g_strconcat(MEDIA_PREFIX, uuid, NULL);
	g_autofree gchar *stored = NULL;
	guint i;

	if (agent == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "there is no agent '%s'", agent_id != NULL ? agent_id : "");
		return FALSE;
	}
	if (roads->len == 0)
		return TRUE;

	/* A road with no key is refused now, where the person asking reads it. */
	for (i = 0; i < roads->len; i++) {
		g_autoptr(AiMediaGenerator) client =
			clawt_daemon_media_office_client(self, g_ptr_array_index(roads, i),
			                                 error);

		if (client == NULL)
			return FALSE;
	}

	question = g_string_new(NULL);
	g_string_append_printf(question, "Make %s's office character?\n", agent_id);
	for (i = 0; i < roads->len; i++) {
		g_autofree gchar *line = road_line(self, agent, g_ptr_array_index(roads, i));

		g_string_append_printf(question, "- %s.\n", line);
	}
	g_string_append(question, "Approve runs them and imports what they make; "
	                          "decline spends nothing.");

	decision = clawt_decision_new(id, agent_id, question->str);
	clawt_decision_set_options(decision, options);
	/* no deadline: nothing spends money but a person's yes */
	clawt_decision_set_default(decision, "decline",
	                           "nobody approved it, so nothing was spent");
	stored = clawt_decision_store_post(self->decisions, decision, error);
	if (stored == NULL)
		return FALSE;
	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "decision.asked", agent_id);
	return TRUE;
}

/* ── Making it ───────────────────────────────────────────────────── */

typedef struct {
	ClawtDaemon       *self;      /* owned */
	gchar             *agent;
	gchar             *road;
	AiMediaGenerator  *client;    /* owned */
	gchar             *object_id; /* PixelLab character, Meshy model task */
	AiMediaJob        *first;     /* owned: the character or the model */
	AiMediaJob        *second;    /* owned: the walk or the rig */
	GPtrArray         *walks;     /* owned: PixelLab's earlier walk jobs */
	guint              walk_at;   /* which of walk_directions is next */
	gchar             *group_id;  /* PixelLab's animation group */
	gchar            **names;     /* what is being fetched, in order */
	guint              at;
	GHashTable        *fetched;   /* name -> GBytes */
	gchar             *trouble;   /* what went missing, for the note */
} Road;

static void
road_free(
	Road *road
){
	g_clear_object(&road->self);
	g_free(road->agent);
	g_free(road->road);
	g_clear_object(&road->client);
	g_free(road->object_id);
	g_clear_object(&road->first);
	g_clear_object(&road->second);
	g_clear_pointer(&road->walks, g_ptr_array_unref);
	g_free(road->group_id);
	g_strfreev(road->names);
	g_clear_pointer(&road->fetched, g_hash_table_unref);
	g_free(road->trouble);
	g_free(road);
}

static void
road_failed(
	Road         *road,
	const gchar  *what,
	const GError *error
){
	g_autofree gchar *body = g_strdup_printf(
		"%s's %s office character was not made: %s%s%s.",
		road->agent, clawt_media_service_label(road->road), what,
		error != NULL ? ": " : "", error != NULL ? error->message : "");

	g_warning("office: %s", body);
	clawt_daemon_office_tell_operator(road->self, road->agent, body);
	road_free(road);
}

/* What both jobs cost, into the agent's media spend: the caps read it. */
static void
record_spend(
	Road *road
){
	g_autofree gchar *state_dir = clawt_config_agent_state_dir(road->self->config,
	                                                           road->agent);
	g_autofree gchar *path = NULL;
	g_autoptr(GPtrArray) jobs = g_ptr_array_new();
	guint i;

	if (state_dir == NULL)
		return;
	path = clawt_media_spend_path(state_dir);

	/* every call billed: the character, each walk direction, the rig */
	if (road->first != NULL)
		g_ptr_array_add(jobs, road->first);
	for (i = 0; road->walks != NULL && i < road->walks->len; i++)
		g_ptr_array_add(jobs, g_ptr_array_index(road->walks, i));
	if (road->second != NULL)
		g_ptr_array_add(jobs, road->second);

	for (i = 0; i < jobs->len; i++) {
		AiMediaJob *job = g_ptr_array_index(jobs, i);
		g_autoptr(GError) error = NULL;

		if (!clawt_media_spend_record(path, road->road, "office character",
		                              ai_media_job_get_id(job),
		                              ai_media_job_get_cost_micro_usd(job),
		                              ai_media_job_get_credits(job), &error))
			g_warning("office: %s: spend not recorded: %s", road->agent,
			          error->message);
	}
}

/* Writes @bytes to a temporary file and imports it into the character. */
static gboolean
import_bytes(
	Road         *road,
	GBytes       *bytes,
	const gchar  *relative,
	const gchar  *provenance,
	GError      **error
){
	g_autofree gchar *root = clawt_daemon_office_characters_dir(road->self);
	g_autofree gchar *tmp = NULL;
	gsize size = 0;
	gconstpointer data = g_bytes_get_data(bytes, &size);
	gint fd = g_file_open_tmp("clawt-character-XXXXXX", &tmp, error);
	gboolean done;

	if (fd < 0)
		return FALSE;
	close(fd);
	if (!g_file_set_contents(tmp, data, (gssize)size, error)) {
		g_unlink(tmp);
		return FALSE;
	}
	done = clawt_office_character_import_asset(root, road->agent, tmp, relative,
	                                           NULL, provenance, NULL, error);
	g_unlink(tmp);
	return done;
}

static void
road_done(
	Road        *road,
	const gchar *made
){
	g_autofree gchar *body = NULL;

	record_spend(road);
	body = g_strdup_printf("%s's office character: %s, by %s%s%s.", road->agent,
	                       made, clawt_media_service_label(road->road),
	                       road->trouble != NULL ? ". Missing: " : "",
	                       road->trouble != NULL ? road->trouble : "");
	g_message("office: %s", body);
	clawt_daemon_office_tell_operator(road->self, road->agent, body);
	if (road->self->bus != NULL)
		clawt_event_bus_emit(road->self->bus, "office.changed", road->agent);
	road_free(road);
}

static gchar *
provenance_for(
	Road *road
){
	return g_strdup_printf("%s, %s and %s for clawtilla's office; %s's terms "
	                       "apply (docs/media-generation.org)",
	                       clawt_media_service_label(road->road),
	                       road->first != NULL ? ai_media_job_get_id(road->first) : "?",
	                       road->second != NULL ? ai_media_job_get_id(road->second) : "?",
	                       clawt_media_service_label(road->road));
}

/* PixelLab: every frame fetched; the sheet laid out and imported. */
static void
pixellab_finish(
	Road *road
){
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_ptr_array_unref);
	g_autoptr(GBytes) sheet = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) animations = NULL;
	g_autofree gchar *provenance = provenance_for(road);
	g_autofree gchar *root = clawt_daemon_office_characters_dir(road->self);
	GBytes *front = g_hash_table_lookup(road->fetched, "south");
	guint fw = 0;
	guint fh = 0;
	guint d;

	if (front == NULL) {
		road_failed(road, "PixelLab gave no front view", NULL);
		return;
	}

	/* idle: the front view; then a walk row per direction, frames in order */
	{
		GPtrArray *idle = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

		g_ptr_array_add(idle, g_bytes_ref(front));
		g_ptr_array_add(rows, idle);
	}
	for (d = 0; d < G_N_ELEMENTS(walk_directions); d++) {
		GPtrArray *row = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
		guint n;

		for (n = 0; n < 64; n++) {
			g_autofree gchar *name = g_strdup_printf("%s:%u", walk_directions[d], n);
			GBytes *frame = g_hash_table_lookup(road->fetched, name);

			if (frame == NULL)
				break;
			g_ptr_array_add(row, g_bytes_ref(frame));
		}
		g_ptr_array_add(rows, row);
	}

	sheet = clawt_office_sheet_compose(rows, &fw, &fh, &error);
	if (sheet != NULL) {
		animations = clawt_office_sheet_walk_layout(rows, 8);
	} else {
		/* no cairo, or no frames: the front view, standing */
		g_autofree gchar *why = g_strdup_printf("no walk (%s)", error->message);
		g_autofree gchar *was = road->trouble;

		road->trouble = was != NULL ? g_strdup_printf("%s, %s", was, why)
		                            : g_steal_pointer(&why);
		g_clear_error(&error);
		g_ptr_array_set_size(rows, 1);
		sheet = clawt_office_sheet_compose(rows, &fw, &fh, NULL);
		animations = clawt_office_sheet_walk_layout(rows, 8);
		if (sheet == NULL) {
			g_autofree gchar *tmp = NULL;
			gint fd = g_file_open_tmp("clawt-front-XXXXXX.png", &tmp, NULL);

			sheet = g_bytes_ref(front);
			if (fd >= 0) {
				close(fd);
				if (g_file_set_contents(tmp, g_bytes_get_data(front, NULL),
				                        (gssize)g_bytes_get_size(front), NULL))
					(void)clawt_office_png_size(tmp, &fw, &fh);
				g_unlink(tmp);
			}
		}
	}

	if (fw == 0 || fh == 0) {
		road_failed(road, "its picture could not be read", NULL);
		return;
	}

	if (!import_bytes(road, sheet, "generated/sheet.png", provenance, &error) ||
	    !clawt_office_character_set_sheet(root, road->agent, "generated/sheet.png",
	                                      fw, fh, animations, &error)) {
		road_failed(road, "it could not be imported", error);
		return;
	}

	road_done(road, json_object_has_member(animations, "walk_down")
	                ? "a walking sprite sheet" : "a standing sprite");
}

/* Meshy: the walking model fetched and attached. */
static void
meshy_finish(
	Road *road
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *provenance = provenance_for(road);
	g_autofree gchar *root = clawt_daemon_office_characters_dir(road->self);
	GBytes *model = g_hash_table_lookup(road->fetched, "anim:walking");
	gboolean walks = model != NULL;

	if (model == NULL)
		model = g_hash_table_lookup(road->fetched, "rig:glb");
	if (model == NULL) {
		road_failed(road, "Meshy's rig gave no model", NULL);
		return;
	}

	if (!import_bytes(road, model, "generated/model.glb", provenance, &error) ||
	    !clawt_office_character_set_mesh(root, road->agent, "generated/model.glb",
	                                     &error)) {
		road_failed(road, "it could not be imported", error);
		return;
	}

	road_done(road, walks ? "a rigged 3D model that walks"
	                      : "a rigged 3D model (Meshy gave no walk)");
}

static void fetch_next(Road *road);

static void
on_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Road *road = data;
	g_autoptr(GError) error = NULL;
	GBytes *bytes = clawt_daemon_media_fetch_finish(result, &error);
	const gchar *name = road->names[road->at];

	(void)source;

	if (bytes == NULL) {
		g_autofree gchar *was = road->trouble;

		road->trouble = g_strdup_printf("%s%s%s (%s)", was != NULL ? was : "",
		                                was != NULL ? ", " : "", name,
		                                error->message);
	} else {
		g_hash_table_insert(road->fetched, g_strdup(name), bytes);
	}

	road->at++;
	fetch_next(road);
}

/* The named result @name of whichever job made it. */
static const gchar *
result_url(
	Road        *road,
	const gchar *name
){
	const gchar *url = road->second != NULL
		? ai_media_job_get_result_url(road->second, name) : NULL;

	if (url == NULL && road->first != NULL)
		url = ai_media_job_get_result_url(road->first, name);
	return url;
}

static void
fetch_next(
	Road *road
){
	const gchar *name;

	if (road->names == NULL || road->names[road->at] == NULL) {
		if (g_strcmp0(road->road, "pixellab") == 0)
			pixellab_finish(road);
		else
			meshy_finish(road);
		return;
	}

	name = road->names[road->at];
	clawt_daemon_media_fetch_async(road->self, road->client, result_url(road, name),
	                               g_str_has_prefix(name, "anim:") ||
	                               g_str_has_prefix(name, "rig:")
	                               ? CLAWT_MEDIA_MAX_MODEL_BYTES
	                               : CLAWT_MEDIA_MAX_IMAGE_BYTES,
	                               on_fetched, road);
}

/* What to fetch once both steps are done. */
static void
fetch_all(
	Road *road
){
	g_autoptr(GStrvBuilder) names = g_strv_builder_new();

	if (g_strcmp0(road->road, "pixellab") == 0) {
		const gchar * const *made = ai_media_job_get_result_names(road->second);
		guint d;
		guint i;

		g_strv_builder_add(names, "south");
		/* the walk's frames, for the four directions the sheet draws */
		for (i = 0; made != NULL && made[i] != NULL; i++) {
			for (d = 0; d < G_N_ELEMENTS(walk_directions); d++) {
				g_autofree gchar *prefix = g_strconcat(walk_directions[d], ":", NULL);

				if (g_str_has_prefix(made[i], prefix))
					g_strv_builder_add(names, made[i]);
			}
		}
	} else {
		if (ai_media_job_get_result_url(road->second, "anim:walking") != NULL)
			g_strv_builder_add(names, "anim:walking");
		else
			g_strv_builder_add(names, "rig:glb");
	}

	road->names = g_strv_builder_end(names);
	road->fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)g_bytes_unref);
	road->at = 0;
	fetch_next(road);
}

static void wait_for(Road *road, const gchar *job_id, GAsyncReadyCallback done);
static void submit_walk(Road *road);

static void
on_second_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Road *road = data;
	g_autoptr(GError) error = NULL;

	road->second = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(source),
	                                              result, &error);
	if (road->second == NULL ||
	    ai_media_job_get_state(road->second) != AI_MEDIA_JOB_STATE_DONE) {
		road_failed(road, g_strcmp0(road->road, "meshy") == 0
		                  ? "the rig did not finish" : "the walk did not finish",
		            error);
		return;
	}

	/* PixelLab: the next direction joins this one's group */
	if (g_strcmp0(road->road, "pixellab") == 0 &&
	    road->walk_at + 1 < G_N_ELEMENTS(walk_directions)) {
		if (road->group_id == NULL) {
			/* `character-animate:<character>:<group>:<jobs>` */
			g_auto(GStrv) parts = g_strsplit(ai_media_job_get_id(road->second), ":", -1);

			if (g_strv_length(parts) < 3 || parts[2][0] == '\0' ||
			    g_strcmp0(parts[2], "-") == 0) {
				road_failed(road, "PixelLab named no animation group for the walk", NULL);
				return;
			}
			road->group_id = g_strdup(parts[2]);
		}
		g_ptr_array_add(road->walks, g_steal_pointer(&road->second));
		road->walk_at++;
		submit_walk(road);
		return;
	}
	fetch_all(road);
}

static void
on_second_submitted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Road *road = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(source),
	                                                        result, &error);

	if (id == NULL) {
		road_failed(road, "the second step was refused", error);
		return;
	}
	wait_for(road, id, on_second_done);
}

static void
on_first_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Road *road = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaRequest) next = NULL;
	g_auto(GStrv) parts = NULL;
	guint n;

	road->first = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(source), result,
	                                             &error);
	if (road->first == NULL ||
	    ai_media_job_get_state(road->first) != AI_MEDIA_JOB_STATE_DONE) {
		road_failed(road, g_strcmp0(road->road, "meshy") == 0
		                  ? "the model did not finish" : "the character did not finish",
		            error);
		return;
	}

	/*
	 * ai-glib's ids are `<operation>:<...>`: PixelLab's character is the
	 * second part, Meshy's newest task the last.
	 */
	parts = g_strsplit(ai_media_job_get_id(road->first), ":", -1);
	n = g_strv_length(parts);
	if (n < 2) {
		road_failed(road, "the service's id could not be read", NULL);
		return;
	}
	road->object_id = g_strdup(g_strcmp0(road->road, "pixellab") == 0
	                           ? parts[1] : parts[n - 1]);
	if (g_strcmp0(road->road, "pixellab") == 0) {
		road->walks = g_ptr_array_new_with_free_func(g_object_unref);
		road->walk_at = 0;
		submit_walk(road);
		return;
	}
	next = meshy_rig(road->object_id);

	g_main_context_push_thread_default(road->self->main_context);
	ai_media_generator_submit_async(road->client, next, NULL, on_second_submitted,
	                                road);
	g_main_context_pop_thread_default(road->self->main_context);
}

/* PixelLab: the walk in walk_directions[walk_at], one call at a time. */
static void
submit_walk(
	Road *road
){
	g_autoptr(AiMediaRequest) next = pixellab_walk(road->object_id,
	                                               walk_directions[road->walk_at],
	                                               road->group_id);

	g_main_context_push_thread_default(road->self->main_context);
	ai_media_generator_submit_async(road->client, next, NULL, on_second_submitted,
	                                road);
	g_main_context_pop_thread_default(road->self->main_context);
}

/*
 * Waited for with the generator's own backoff.  A Meshy model can take
 * most of an hour on a busy day, so its ceiling is an hour; PixelLab's
 * jobs are minutes.
 */
static void
wait_for(
	Road                *road,
	const gchar         *job_id,
	GAsyncReadyCallback  done
){
	guint first = road->self->office_generate_poll_ms > 0
		? road->self->office_generate_poll_ms : 5000;
	guint timeout = g_strcmp0(road->road, "meshy") == 0 ? 60 * 60 * 1000
	                                                    : 20 * 60 * 1000;

	g_main_context_push_thread_default(road->self->main_context);
	ai_media_generator_wait_async(road->client, job_id, first, MAX(first, 30000u),
	                              timeout, NULL, done, road);
	g_main_context_pop_thread_default(road->self->main_context);
}

static void
on_first_submitted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Road *road = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(source),
	                                                        result, &error);

	if (id == NULL) {
		road_failed(road, "it was not accepted", error);
		return;
	}
	wait_for(road, id, on_first_done);
}

static void
start_road(
	ClawtDaemon      *self,
	ClawtAgentConfig *agent,
	const gchar      *name
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaRequest) request = NULL;
	Road *road = g_new0(Road, 1);

	road->self = g_object_ref(self);
	road->agent = g_strdup(clawt_agent_config_get_id(agent));
	road->road = g_strdup(name);
	road->client = clawt_daemon_media_office_client(self, name, &error);
	if (road->client == NULL) {
		road_failed(road, "there is no key to make it with", error);
		return;
	}

	request = g_strcmp0(name, "pixellab") == 0 ? pixellab_character(self, agent)
	                                           : meshy_model(self, agent);

	g_main_context_push_thread_default(self->main_context);
	ai_media_generator_submit_async(road->client, request, NULL, on_first_submitted,
	                                road);
	g_main_context_pop_thread_default(self->main_context);
}

void
clawt_daemon_office_media_start(
	ClawtDaemon *self,
	const gchar *agent_id
){
	ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
	g_autoptr(GPtrArray) roads = NULL;
	guint i;

	if (agent == NULL)
		return;

	/* the same roads the card listed, read from the same keys */
	roads = roads_for(self, clawt_daemon_office_media_wanted(self));
	for (i = 0; i < roads->len; i++)
		start_road(self, agent, g_ptr_array_index(roads, i));
}
