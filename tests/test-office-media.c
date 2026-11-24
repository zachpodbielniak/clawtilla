/*
 * test-office-media.c - Office characters from PixelLab and Meshy
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The sheet layout is pure and tested directly.  The two roads run
 * through the daemon against one mock generator behind the
 * media_client_factory seam, which answers each step in the shapes
 * ai-glib's clients do (ids `<operation>:<...>`, named results), so no
 * network and no key are involved.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"
#include "office/clawt-office-pack.h"

#include <glib/gstdio.h>
#include <string.h>

/* 16x24 and 12x20 PNGs: frames of different sizes, so the cell is the largest. */
#define TALL "iVBORw0KGgoAAAANSUhEUgAAABAAAAAYCAYAAADzoH0MAAAAHklEQVR4nGP4z8DwnxLMMGrAqAGjBowaMGrAcDIAAGqt/R9owhblAAAAAElFTkSuQmCC"
#define SMALL "iVBORw0KGgoAAAANSUhEUgAAAAwAAAAUCAYAAAC58NwRAAAAGUlEQVR4nGP4z8DwnxTMMKphVMOohsGsAQBisd4wgmGwrwAAAABJRU5ErkJggg=="

static GBytes *
png(const gchar *b64)
{
	gsize length = 0;
	guchar *data = g_base64_decode(b64, &length);

	return g_bytes_new_take(data, length);
}

/* ── The sheet ───────────────────────────────────────────────────── */

static GPtrArray *
row_of(guint n, const gchar *b64)
{
	GPtrArray *row = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	guint i;

	for (i = 0; i < n; i++)
		g_ptr_array_add(row, png(b64));
	return row;
}

static void
test_layout(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_ptr_array_unref);
	g_autoptr(JsonObject) animations = NULL;

	g_ptr_array_add(rows, row_of(1, TALL));
	g_ptr_array_add(rows, row_of(4, SMALL));
	g_ptr_array_add(rows, row_of(0, SMALL));   /* a walk PixelLab left out */
	g_ptr_array_add(rows, row_of(2, SMALL));
	g_ptr_array_add(rows, row_of(2, SMALL));

	animations = clawt_office_sheet_walk_layout(rows, 8);
	g_assert_true(json_object_has_member(animations, "idle"));
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(animations, "idle"), "frames"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(animations, "walk_down"), "frames"), ==, 4);
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(animations, "walk_left"), "row"), ==, 3);
	/* an empty row is left out, so the renderer falls back to idle */
	g_assert_false(json_object_has_member(animations, "walk_up"));
}

static void
test_compose(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_ptr_array_unref);
	g_autoptr(GBytes) sheet = NULL;
	g_autoptr(GError) error = NULL;
	guint fw = 0;
	guint fh = 0;

	g_ptr_array_add(rows, row_of(1, TALL));
	g_ptr_array_add(rows, row_of(3, SMALL));

	sheet = clawt_office_sheet_compose(rows, &fw, &fh, &error);
	if (!clawt_office_sheet_available()) {
		g_assert_null(sheet);
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
		return;
	}

	g_assert_no_error(error);
	g_assert_nonnull(sheet);
	/* the cell is the largest frame; the sheet three cells by two */
	g_assert_cmpuint(fw, ==, 16);
	g_assert_cmpuint(fh, ==, 24);
	{
		g_autofree gchar *dir = g_dir_make_tmp("clawt-sheet-XXXXXX", NULL);
		g_autofree gchar *path = g_build_filename(dir, "s.png", NULL);
		guint w = 0;
		guint h = 0;

		g_assert_true(g_file_set_contents(path, g_bytes_get_data(sheet, NULL),
		                                  (gssize)g_bytes_get_size(sheet), NULL));
		g_assert_true(clawt_office_png_size(path, &w, &h));
		g_assert_cmpuint(w, ==, 48);
		g_assert_cmpuint(h, ==, 48);
		clawt_test_remove_tree(dir);
	}
}

/* ── A mock of both services ─────────────────────────────────────── */

typedef struct {
	GObject  parent_instance;
	GString *submitted;   /* each operation, space-separated */
	gchar   *rig_input;   /* what the rig was asked to rig */
	gchar   *walk_for;    /* which character the walk was for */
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
mock_media_finalize(GObject *object)
{
	MockMedia *self = (MockMedia *)object;

	g_string_free(self->submitted, TRUE);
	g_free(self->rig_input);
	g_free(self->walk_for);
	G_OBJECT_CLASS(mock_media_parent_class)->finalize(object);
}

static void
mock_media_init(MockMedia *self)
{
	self->submitted = g_string_new(NULL);
}

static void
mock_media_class_init(MockMediaClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = mock_media_finalize;
}

/* A string parameter of a request, read back from its JSON. */
static gchar *
param(AiMediaRequest *request, const gchar *name)
{
	g_autofree gchar *json = ai_media_request_to_json(request);
	g_autoptr(JsonNode) node = json_from_string(json, NULL);
	JsonObject *root = json_node_get_object(node);
	JsonObject *params = json_object_has_member(root, "params")
		? json_object_get_object_member(root, "params") : root;

	return json_object_has_member(params, name)
		? g_strdup(json_object_get_string_member(params, name)) : NULL;
}

static void
mock_submit_async(AiMediaGenerator *self, AiMediaRequest *request,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	MockMedia *mock = (MockMedia *)self;
	g_autofree gchar *operation = param(request, "operation");
	const gchar *id = "unknown:x";

	g_string_append_printf(mock->submitted, "%s ", operation);
	if (g_strcmp0(operation, "character-4") == 0)
		id = "character-4:char9:-:j1";
	else if (g_strcmp0(operation, "character-animate") == 0) {
		g_free(mock->walk_for);
		mock->walk_for = param(request, "character_id");
		id = "character-animate:char9:g1:j2";
	} else if (g_strcmp0(operation, "image-to-3d") == 0 ||
	           g_strcmp0(operation, "text-to-3d") == 0)
		id = "text-to-3d:prev1:task5";
	else if (g_strcmp0(operation, "rig") == 0) {
		g_free(mock->rig_input);
		mock->rig_input = param(request, "input_task_id");
		id = "rig:task6";
	}
	g_task_return_pointer(task, g_strdup(id), g_free);
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

	if (g_str_has_prefix(job_id, "character-4:")) {
		ai_media_job_add_named_result(job, "south", "data:image/png;base64," TALL);
		ai_media_job_add_named_result(job, "north", "data:image/png;base64," TALL);
	} else if (g_str_has_prefix(job_id, "character-animate:")) {
		ai_media_job_add_named_result(job, "south:0", "data:image/png;base64," SMALL);
		ai_media_job_add_named_result(job, "south:1", "data:image/png;base64," SMALL);
		ai_media_job_add_named_result(job, "north:0", "data:image/png;base64," SMALL);
		ai_media_job_add_named_result(job, "west:0", "data:image/png;base64," SMALL);
		ai_media_job_add_named_result(job, "east:0", "data:image/png;base64," SMALL);
		/* not a direction the sheet draws: never fetched */
		ai_media_job_add_named_result(job, "spritesheet", "http://plain.invalid/s.zip");
		ai_media_job_set_cost_micro_usd(job, 30000);
	} else if (g_str_has_prefix(job_id, "rig:")) {
		/* "glTF" and padding: enough to be a .glb on disk */
		ai_media_job_add_named_result(job, "anim:walking",
		                              "data:model/gltf-binary;base64,Z2xURgIAAAA=");
		ai_media_job_set_credits(job, 5);
	} else {
		ai_media_job_set_credits(job, 20);
	}
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
setup(Fixture *fixture, const gchar *office)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-office-media-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"office:\n  enabled: true\n%s"
		"  world_dirs: [\"%s/../../data/office/worlds\"]\n"
		"agents:\n"
		"  - id: kudu\n    description: finds deals\n"
		"    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, office, CLAWT_TEST_FIXTURES);
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

/* The open media card, once posted. */
static ClawtDecision *
media_card(Fixture *fixture)
{
	g_autoptr(GPtrArray) open = clawt_decision_store_list(fixture->daemon->decisions,
	                                                      TRUE);
	guint i;

	for (i = 0; i < open->len; i++) {
		ClawtDecision *d = g_ptr_array_index(open, i);

		if (g_str_has_prefix(clawt_decision_get_id(d), "office-generate-media-"))
			return clawt_decision_copy(d);
	}
	return NULL;
}

static void
approve(Fixture *fixture, ClawtDecision *decision)
{
	g_autoptr(JsonNode) frame = clawt_ipc_request_new("decision.answer", "a1");
	g_autoptr(JsonNode) reply = NULL;

	clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new(
		"decision", clawt_decision_get_id(decision), "answer", "approve", NULL));
	reply = clawt_daemon_handle_request(fixture->daemon, frame);
	g_assert_false(clawt_ipc_frame_is_error(reply));
}

/* kudu's character, once @done says it is made, iterating with a watchdog. */
static ClawtOfficeCharacter *
wait_for_character(Fixture *fixture, gboolean (*done)(ClawtOfficeCharacter *))
{
	g_autofree gchar *chars = clawt_daemon_office_characters_dir(fixture->daemon);
	g_autofree gchar *kudu = g_build_filename(chars, "kudu", NULL);
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	ClawtOfficeCharacter *made = NULL;

	while (g_get_monotonic_time() < deadline) {
		g_clear_pointer(&made, clawt_office_character_free);
		made = clawt_office_character_load(kudu, NULL);
		if (made != NULL && done(made))
			return made;
		g_main_context_iteration(fixture->context, FALSE);
	}
	return made;
}

/* The row called @name, or NULL. */
static ClawtOfficeAnimation *
animation(ClawtOfficeCharacter *made, const gchar *name)
{
	guint i;

	for (i = 0; made->animations != NULL && i < made->animations->len; i++) {
		ClawtOfficeAnimation *one = g_ptr_array_index(made->animations, i);

		if (g_strcmp0(one->name, name) == 0)
			return one;
	}
	return NULL;
}

static gboolean
has_sheet(ClawtOfficeCharacter *made)
{
	return made->sheet != NULL;
}

static gboolean
has_both(ClawtOfficeCharacter *made)
{
	return made->sheet != NULL && made->mesh != NULL;
}

/*
 * PixelLab: asked once, nothing before the yes; then a character, its
 * walk for that character, and a sheet with a row per direction.
 */
static void
test_pixellab_road(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtDecision) card = NULL;
	g_autoptr(ClawtOfficeCharacter) made = NULL;
	g_autofree gchar *service = NULL;

	the_mock = g_object_new(mock_media_get_type(), NULL);
	setup(&fixture, "  asset_api: pixellab\n  pixel_size: 32\n");

	g_assert_true(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", &service,
	                                               &error));
	g_assert_no_error(error);
	g_assert_cmpstr(service, ==, "pixellab");
	card = media_card(&fixture);
	g_assert_nonnull(card);
	g_assert_nonnull(strstr(clawt_decision_get_question(card), "32x32 character"));
	g_assert_cmpuint(the_mock->submitted->len, ==, 0);

	approve(&fixture, card);
	made = wait_for_character(&fixture, has_sheet);
	g_assert_nonnull(made);
	g_assert_cmpstr(made->sheet, ==, "generated/sheet.png");
	g_assert_cmpstr(the_mock->submitted->str, ==, "character-4 character-animate ");
	g_assert_cmpstr(the_mock->walk_for, ==, "char9");

	if (clawt_office_sheet_available()) {
		/* the cell is the tallest frame; the walk has a row per direction */
		g_assert_cmpuint(made->frame_width, ==, 16);
		g_assert_cmpuint(made->frame_height, ==, 24);
		g_assert_nonnull(animation(made, "walk_down"));
		g_assert_cmpuint(animation(made, "walk_down")->frames, ==, 2);
		g_assert_nonnull(animation(made, "walk_right"));
	} else {
		g_assert_null(animation(made, "walk_down"));
	}

	/* what it cost is where the daily caps read it */
	{
		g_autofree gchar *state = clawt_config_agent_state_dir(fixture.daemon->config,
		                                                       "kudu");
		g_autofree gchar *spend = clawt_media_spend_path(state);
		gint64 micros = 0;

		clawt_media_spend_since(spend, 0, 0, &micros, NULL, NULL);
		g_assert_cmpint(micros, ==, 30000);
	}

	teardown(&fixture);
	g_clear_object(&the_mock);
}

/*
 * PixelLab for the 2D view and Meshy for the 3D one: one card, both
 * roads, the rig given the model's newest task, the walking GLB attached.
 */
static void
test_both_roads(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtDecision) card = NULL;
	g_autoptr(ClawtOfficeCharacter) made = NULL;

	the_mock = g_object_new(mock_media_get_type(), NULL);
	setup(&fixture, "  asset_api: pixellab\n  asset_api_3d: meshy\n");

	g_assert_true(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", NULL, &error));
	card = media_card(&fixture);
	g_assert_nonnull(card);
	g_assert_nonnull(strstr(clawt_decision_get_question(card), "PixelLab"));
	g_assert_nonnull(strstr(clawt_decision_get_question(card), "Meshy: a 3D model"));
	g_assert_nonnull(strstr(clawt_decision_get_question(card), "credits"));

	approve(&fixture, card);
	made = wait_for_character(&fixture, has_both);
	g_assert_nonnull(made);
	g_assert_cmpstr(made->mesh, ==, "generated/model.glb");
	g_assert_cmpstr(the_mock->rig_input, ==, "task5");

	teardown(&fixture);
	g_clear_object(&the_mock);
}

/* With no key at all, the request is refused where the person reads it. */
static void
test_no_key(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;

	the_mock = NULL;
	setup(&fixture, "  asset_api: pixellab\n");
	fixture.daemon->media_client_factory = NULL;

	g_assert_false(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", NULL,
	                                                &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "office api-key pixellab"));
	g_assert_null(media_card(&fixture));

	teardown(&fixture);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/office-media/layout", test_layout);
	g_test_add_func("/office-media/compose", test_compose);
	g_test_add_func("/office-media/pixellab", test_pixellab_road);
	g_test_add_func("/office-media/both", test_both_roads);
	g_test_add_func("/office-media/no-key", test_no_key);

	return g_test_run();
}
