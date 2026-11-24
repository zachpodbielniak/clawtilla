/*
 * test-office-restyle.c - A world pack redrawn by PixelLab and Meshy
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The pure half runs on the shipped modern-office pack, read from the
 * source tree.  The daemon half restyles that pack end to end against a
 * mock of both services behind the media_client_factory seam, and checks
 * the result the way the office will: it loads, every checksum holds,
 * the map is untouched and the furniture points at its models.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"
#include "office/clawt-office-world.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#ifdef CLAWT_HAVE_POPPLER
#include <cairo.h>
#endif

/* A 32x32 PNG: one tile, as PixelLab draws it. */
#define TILE_PNG "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAKklEQVR4nO3OMQ0AAAACIPuX1hg+jAIkPRMQEBAQEBAQEBAQEBAQEHgHBlfR+GotGw+YAAAAAElFTkSuQmCC"

static gchar *
shipped(const gchar *file)
{
	g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, "..", "..", "data",
	                                          "office", "worlds", "modern-office", file,
	                                          NULL);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* The kind called @name, or NULL. */
static JsonObject *
kind_named(JsonArray *kinds, const gchar *name)
{
	guint i;

	for (i = 0; i < json_array_get_length(kinds); i++) {
		JsonObject *kind = json_array_get_object_element(kinds, i);

		if (g_strcmp0(json_object_get_string_member(kind, "name"), name) == 0)
			return kind;
	}
	return NULL;
}

/* ── The pure half ───────────────────────────────────────────────── */

static void
test_kinds(void)
{
	g_autofree gchar *manifest = shipped("office.yaml");
	g_autofree gchar *tmj = shipped("map.tmj");
	g_autofree gchar *level = shipped("office.rlevel");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonArray) kinds = clawt_office_restyle_kinds(manifest, tmj, level, &error);

	g_assert_no_error(error);
	g_assert_nonnull(kinds);

	/* a desk is furniture with one node: a model can stand in for it */
	g_assert_nonnull(kind_named(kinds, "desk"));
	g_assert_true(json_object_get_boolean_member(kind_named(kinds, "desk"), "model"));
	g_assert_false(json_object_get_boolean_member(kind_named(kinds, "desk"), "floor"));
	g_assert_cmpint(json_object_get_int_member(kind_named(kinds, "desk"), "gid"), ==, 5);

	/* carpet is floor; a booth is four nodes; a wall is the building */
	g_assert_true(json_object_get_boolean_member(kind_named(kinds, "carpet"), "floor"));
	g_assert_false(json_object_get_boolean_member(kind_named(kinds, "booth"), "model"));
	g_assert_false(json_object_get_boolean_member(kind_named(kinds, "wall"), "model"));
	/* a wall fills its cell: asked for with no background it came back blank */
	g_assert_true(json_object_get_boolean_member(kind_named(kinds, "wall"), "structure"));
	g_assert_false(json_object_get_boolean_member(kind_named(kinds, "desk"), "structure"));

	/* a kind the map never uses is not drawn */
	g_assert_null(kind_named(kinds, "empty"));
}

static void
test_kinds_need_names(void)
{
	g_autofree gchar *tmj = shipped("map.tmj");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonArray) kinds = clawt_office_restyle_kinds("name: old\nlicense: CC0-1.0\n",
	                                                        tmj, NULL, &error);

	g_assert_null(kinds);
	g_assert_nonnull(strstr(error->message, "names no tiles"));
}

static void
test_level(void)
{
	g_autofree gchar *tmj = shipped("map.tmj");
	g_autofree gchar *level = shipped("office.rlevel");
	g_autoptr(JsonObject) models = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	guint before;
	guint after;

	json_object_set_string_member(models, "5", "models/desk.glb");
	out = clawt_office_restyle_level(level, tmj, models, &error);
	g_assert_no_error(error);
	g_assert_nonnull(out);

	/* every desk node is the model; every other node is still a primitive */
	after = clawt_test_count_substrings(out, "models/desk.glb");
	g_assert_cmpuint(after, >, 0);
	g_assert_cmpuint(clawt_test_count_substrings(out, "mesh-asset"), ==, after);
	before = clawt_test_count_substrings(level, "primitive: cube");
	g_assert_cmpuint(clawt_test_count_substrings(out, "primitive: cube"), ==,
	                 before - after);
}

static void
test_prompt(void)
{
	g_autoptr(JsonObject) floor = json_object_new();
	g_autoptr(JsonObject) desk = json_object_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;

	json_object_set_string_member(floor, "name", "carpet");
	json_object_set_boolean_member(floor, "floor", TRUE);
	json_object_set_string_member(desk, "name", "desk");
	json_object_set_boolean_member(desk, "floor", FALSE);

	a = clawt_office_restyle_prompt(floor, "neon", FALSE);
	b = clawt_office_restyle_prompt(desk, "neon", FALSE);
	c = clawt_office_restyle_prompt(desk, "neon", TRUE);
	/*
	 * Each says what it must not be: from "a carpet seen from above, for a
	 * top-down pixel-art office game" PixelLab drew whole little offices
	 */
	g_assert_nonnull(strstr(a, "seamless carpet floor texture"));
	g_assert_nonnull(strstr(a, "no furniture"));
	g_assert_nonnull(strstr(b, "One single desk"));
	g_assert_nonnull(strstr(b, "no other furniture"));
	g_assert_nonnull(strstr(b, "neon"));
	g_assert_null(strstr(b, "office"));
	g_assert_nonnull(strstr(c, "desk for an office"));
	g_assert_nonnull(strstr(c, "neon"));

	/* a wall is a texture, not a thing standing in a room */
	{
		g_autoptr(JsonObject) wall = json_object_new();
		g_autofree gchar *w = NULL;

		json_object_set_string_member(wall, "name", "wall");
		json_object_set_boolean_member(wall, "floor", FALSE);
		w = clawt_office_restyle_prompt(wall, "neon", FALSE);
		g_assert_nonnull(strstr(w, "flat wall texture"));
		g_assert_nonnull(strstr(w, "no furniture"));
	}
}

#ifdef CLAWT_HAVE_POPPLER
/* Writes a PNG cairo hands over into the GByteArray it is given. */
static cairo_status_t
append_png(void *closure, const unsigned char *data, unsigned int length)
{
	g_byte_array_append(closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}
#endif

/*
 * The tile a restyle starts from: one slot of the base sheet, scaled up
 * without smoothing.  A sheet of two 8-pixel tiles, red then blue.
 */
static void
test_cut(void)
{
#ifdef CLAWT_HAVE_POPPLER
	cairo_surface_t *sheet = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 8);
	cairo_t *cr = cairo_create(sheet);
	GByteArray *png = g_byte_array_new();
	g_autoptr(GBytes) base = NULL;
	g_autoptr(GBytes) cell = NULL;
	g_autoptr(GBytes) none = NULL;
	g_autoptr(GError) error = NULL;
	cairo_surface_t *got;
	guint32 *pixels;

	cairo_set_source_rgb(cr, 1, 0, 0);
	cairo_rectangle(cr, 0, 0, 8, 8);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 1);
	cairo_rectangle(cr, 8, 0, 8, 8);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_write_to_png_stream(sheet, append_png, png);
	cairo_surface_destroy(sheet);
	base = g_byte_array_free_to_bytes(png);

	cell = clawt_office_restyle_cut(base, 8, 2, 2, 32, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cell);

	/* 32 a side, and blue all the way to its edges */
	{
		gchar *tmp = NULL;
		gint fd = g_file_open_tmp("clawt-cut-XXXXXX.png", &tmp, NULL);

		g_assert_cmpint(fd, >=, 0);
		close(fd);
		g_assert_true(g_file_set_contents(tmp, g_bytes_get_data(cell, NULL),
		                                  (gssize)g_bytes_get_size(cell), NULL));
		got = cairo_image_surface_create_from_png(tmp);
		g_unlink(tmp);
		g_free(tmp);
	}
	g_assert_cmpint(cairo_image_surface_get_width(got), ==, 32);
	g_assert_cmpint(cairo_image_surface_get_height(got), ==, 32);
	cairo_surface_flush(got);
	pixels = (guint32 *)cairo_image_surface_get_data(got);
	g_assert_cmphex(pixels[0] & 0xffffff, ==, 0x0000ff);
	g_assert_cmphex(pixels[32 * 31 + 31] & 0xffffff, ==, 0x0000ff);
	cairo_surface_destroy(got);

	/* a tile the sheet does not have is refused, not cut from nothing */
	none = clawt_office_restyle_cut(base, 8, 2, 3, 32, &error);
	g_assert_null(none);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
#else
	g_test_skip("built without cairo (POPPLER=1)");
#endif
}

/* ── A mock of both services ─────────────────────────────────────── */

typedef struct {
	GObject parent_instance;
	guint   submits;
	guint   tiles;        /* tile requests */
	guint   started_from; /* tile requests carrying their base tile */
} MockMedia;

typedef struct {
	GObjectClass parent_class;
} MockMediaClass;

static void mock_media_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MockMedia, mock_media, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_MEDIA_GENERATOR,
                                              mock_media_iface_init))

static MockMedia *the_mock;

static void
mock_media_init(MockMedia *self)
{
	(void)self;
}

static void
mock_media_class_init(MockMediaClass *klass)
{
	(void)klass;
}

static void
mock_submit_async(AiMediaGenerator *self, AiMediaRequest *request,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	g_autofree gchar *json = ai_media_request_to_json(request);

	((MockMedia *)self)->submits++;
	if (strstr(json, "text-to-3d") == NULL)
		((MockMedia *)self)->tiles++;
	if (strstr(json, "\"init_image\"") != NULL &&
	    strstr(json, "\"init_image_strength\"") != NULL)
		((MockMedia *)self)->started_from++;
	g_task_return_pointer(task, g_strdup(strstr(json, "text-to-3d") != NULL
	                                     ? "text-to-3d:p:r" : "sync:1"), g_free);
}

static gchar *
mock_submit_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
	(void)self;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_poll_async(AiMediaGenerator *self, const gchar *job_id,
                GCancellable *cancellable, GAsyncReadyCallback callback,
                gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	AiMediaJob *job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_DONE);

	if (g_str_has_prefix(job_id, "text-to-3d:"))
		ai_media_job_add_named_result(job, "glb",
		                              "data:model/gltf-binary;base64,Z2xURgIAAAA=");
	else
		ai_media_job_add_named_result(job, "image", "data:image/png;base64," TILE_PNG);
	g_task_return_pointer(task, job, g_object_unref);
}

static AiMediaJob *
mock_poll_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
	(void)self;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_download_async(AiMediaGenerator *self, const gchar *url,
                    GCancellable *cancellable, GAsyncReadyCallback callback,
                    gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	const gchar *comma = strchr(url, ',');
	gsize length = 0;
	guchar *data = g_base64_decode(comma != NULL ? comma + 1 : "", &length);

	g_task_return_pointer(task, g_bytes_new_take(data, length),
	                      (GDestroyNotify)g_bytes_unref);
}

static GBytes *
mock_download_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
	(void)self;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_media_iface_init(AiMediaGeneratorInterface *iface)
{
	iface->submit_async = mock_submit_async;
	iface->submit_finish = mock_submit_finish;
	iface->poll_async = mock_poll_async;
	iface->poll_finish = mock_poll_finish;
	iface->download_async = mock_download_async;
	iface->download_finish = mock_download_finish;
}

static AiMediaGenerator *
mock_factory(ClawtDaemon *self, const gchar *service)
{
	(void)self;
	(void)service;
	return the_mock != NULL ? g_object_ref(AI_MEDIA_GENERATOR(the_mock)) : NULL;
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} Fixture;

static void
setup(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-restyle-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"office:\n  enabled: true\n"
		"  world_dirs: [\"%s/../../data/office/worlds\"]\n"
		"agents:\n  - id: kudu\n    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, CLAWT_TEST_FIXTURES);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
	fixture->daemon->media_client_factory = mock_factory;
	fixture->daemon->office_generate_poll_ms = 5;
}

static void
teardown(Fixture *fixture)
{
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
request(Fixture *fixture, const gchar *kind, JsonNode *payload)
{
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "r1");

	clawt_ipc_frame_set_payload(frame, payload);
	return clawt_daemon_handle_request(fixture->daemon, frame);
}

static JsonNode *
restyle(Fixture *fixture, const gchar *base, const gchar *name, const gchar *style,
        gboolean three_d)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "base");
	json_builder_add_string_value(builder, base);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "style");
	json_builder_add_string_value(builder, style);
	json_builder_set_member_name(builder, "three_d");
	json_builder_add_boolean_value(builder, three_d);
	json_builder_end_object(builder);
	return request(fixture, "office.world.restyle", json_builder_get_root(builder));
}

static gboolean
on_watchdog(gpointer data)
{
	(void)data;
	g_error("the restyle did not finish");
	return G_SOURCE_REMOVE;
}

/* Approves the card and waits for the world to appear. */
static gchar *
approve_and_wait(Fixture *fixture, const gchar *decision, const gchar *name)
{
	g_autofree gchar *made = g_build_filename(fixture->daemon->state_dir, "office",
	                                          "worlds", name, "office.yaml", NULL);
	g_autoptr(JsonNode) reply = request(fixture, "decision.answer",
		clawt_ipc_payload_new("decision", decision, "answer", "approve", NULL));
	GSource *watchdog = g_timeout_source_new_seconds(30);

	g_assert_false(clawt_ipc_frame_is_error(reply));
	g_source_set_callback(watchdog, on_watchdog, NULL, NULL);
	g_source_attach(watchdog, fixture->context);
	while (!g_file_test(made, G_FILE_TEST_EXISTS))
		g_main_context_iteration(fixture->context, TRUE);
	g_source_destroy(watchdog);
	g_source_unref(watchdog);

	return g_path_get_dirname(made);
}

/*
 * Restyled end to end: asked first and nothing spent; then a world that
 * loads with every checksum holding, the map untouched, the furniture
 * pointing at its models, and a licence that is not the base's CC0.
 */
static void
test_restyle(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) asked = NULL;
	g_autoptr(ClawtOfficeWorld) world = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *base_map = shipped("map.tmj");
	g_autofree gchar *map = NULL;
	g_autofree gchar *level = NULL;
	g_autofree gchar *path = NULL;
	const gchar *decision;
	guint before;

	the_mock = g_object_new(mock_media_get_type(), NULL);
	setup(&fixture);

	if (!clawt_office_sheet_available()) {
		/* without cairo the tiles are refused up front, naming why */
		asked = restyle(&fixture, "modern-office", "neon", "neon signs", FALSE);
		g_assert_true(clawt_ipc_frame_is_error(asked));
		teardown(&fixture);
		g_clear_object(&the_mock);
		return;
	}

	asked = restyle(&fixture, "modern-office", "neon", "neon signs", TRUE);
	if (clawt_ipc_frame_is_error(asked)) {
		g_autofree gchar *text = json_to_string(asked, FALSE);

		g_test_message("refused: %s", text);
	}
	g_assert_false(clawt_ipc_frame_is_error(asked));
	decision = json_object_get_string_member(
		clawt_ipc_frame_get_payload(asked), "decision");
	g_assert_true(g_str_has_prefix(decision, "office-generate-restyle-neon:"));
	g_assert_cmpuint(the_mock->submits, ==, 0);

	dir = approve_and_wait(&fixture, decision, "neon");
	world = clawt_office_world_load(dir, &error);
	g_assert_no_error(error);
	g_assert_nonnull(world);
	g_assert_true(clawt_office_world_verify_assets(world, &error));
	g_assert_no_error(error);
	g_assert_null(strstr(clawt_office_world_get_license(world), "CC0"));

	/* the map is the base's, byte for byte: the layout is kept */
	path = g_build_filename(dir, "map.tmj", NULL);
	g_assert_true(g_file_get_contents(path, &map, NULL, NULL));
	g_assert_cmpstr(map, ==, base_map);

	g_free(path);
	path = g_build_filename(dir, "office.rlevel", NULL);
	g_assert_true(g_file_get_contents(path, &level, NULL, NULL));
	g_assert_nonnull(strstr(level, "models/desk.glb"));
	g_free(path);
	path = g_build_filename(dir, "models", "desk.glb", NULL);
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
	g_free(path);
	path = g_build_filename(dir, "provenance.yaml", NULL);
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));

	/* every tile was started from its own tile in the base sheet */
	if (clawt_office_sheet_available()) {
		g_assert_cmpuint(the_mock->tiles, >, 0);
		g_assert_cmpuint(the_mock->started_from, ==, the_mock->tiles);
	}

	/*
	 * Asked again under the same name, once the world is moved aside:
	 * the parts are kept, so the card says so and nothing is bought twice.
	 */
	before = the_mock->submits;
	g_assert_cmpuint(before, >, 0);
	{
		g_autoptr(JsonNode) again = NULL;
		g_autoptr(ClawtDecision) card = NULL;
		g_autofree gchar *worlds = g_path_get_dirname(dir);
		const gchar *id;

		clawt_test_remove_tree(dir);
		again = restyle(&fixture, "modern-office", "neon", "neon signs", TRUE);
		g_assert_false(clawt_ipc_frame_is_error(again));
		id = json_object_get_string_member(
			clawt_ipc_frame_get_payload(again), "decision");
		card = clawt_decision_store_get(fixture.daemon->decisions, id);
		g_assert_nonnull(card);
		g_assert_nonnull(strstr(clawt_decision_get_question(card), "already made"));
		g_free(approve_and_wait(&fixture, id, "neon"));
		g_assert_cmpuint(the_mock->submits, ==, before);
		(void)worlds;
	}

	teardown(&fixture);
	g_clear_object(&the_mock);
}

/* What is refused, where whoever asked reads it. */
static void
test_refusals(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) taken = NULL;
	g_autoptr(JsonNode) no_style = NULL;
	g_autoptr(JsonNode) no_base = NULL;
	g_autoptr(JsonNode) bad_name = NULL;

	the_mock = g_object_new(mock_media_get_type(), NULL);
	setup(&fixture);

	taken = restyle(&fixture, "modern-office", "cozy-town", "x", FALSE);
	no_style = restyle(&fixture, "modern-office", "fresh", "", FALSE);
	no_base = restyle(&fixture, "nowhere", "fresh", "x", FALSE);
	bad_name = restyle(&fixture, "modern-office", "../up", "x", FALSE);
	g_assert_true(clawt_ipc_frame_is_error(taken));
	g_assert_true(clawt_ipc_frame_is_error(no_style));
	g_assert_true(clawt_ipc_frame_is_error(no_base));
	g_assert_true(clawt_ipc_frame_is_error(bad_name));
	g_assert_cmpuint(the_mock->submits, ==, 0);

	teardown(&fixture);
	g_clear_object(&the_mock);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/office-restyle/kinds", test_kinds);
	g_test_add_func("/office-restyle/kinds-need-names", test_kinds_need_names);
	g_test_add_func("/office-restyle/level", test_level);
	g_test_add_func("/office-restyle/prompt", test_prompt);
	g_test_add_func("/office-restyle/cut", test_cut);
	g_test_add_func("/office-restyle/end-to-end", test_restyle);
	g_test_add_func("/office-restyle/refusals", test_refusals);

	return g_test_run();
}
