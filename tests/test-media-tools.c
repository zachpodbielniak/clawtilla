/*
 * test-media-tools.c - Meshy and PixelLab for agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The pure half (clawt-media.c: requests, prices, file names, spend) is
 * tested directly.  The daemon half is driven through the tools with a
 * mock generator behind the media_client_factory seam, as the office's
 * tests do: no network, no service, no key.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* A 1x1 PNG, as PixelLab answers a picture: inline, as a data URL. */
#define PNG_B64 "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="

/* ── The pure half ───────────────────────────────────────────────── */

static JsonObject *
object_from(
	const gchar *json
){
	g_autoptr(JsonNode) node = json_from_string(json, NULL);

	g_assert_nonnull(node);
	return json_object_ref(json_node_get_object(node));
}

static void
test_service_for_tool(void)
{
	g_assert_cmpstr(clawt_media_service_for_tool("clawtilla_meshy_job"), ==, "meshy");
	g_assert_cmpstr(clawt_media_service_for_tool("clawtilla_pixellab_generate"), ==,
	                "pixellab");
	g_assert_null(clawt_media_service_for_tool("clawtilla_web_search"));
	g_assert_null(clawt_media_service_for_tool(NULL));
}

static void
test_request(void)
{
	g_autoptr(JsonObject) args = object_from(
		"{\"operation\":\"image\",\"prompt\":\"a red desk\","
		"\"params\":{\"image_size\":{\"width\":32,\"height\":32},"
		"\"operation\":\"sneaky\"}}");
	g_autoptr(JsonObject) none = object_from("{\"prompt\":\"x\"}");
	g_autoptr(AiMediaRequest) request = NULL;
	g_autoptr(GError) error = NULL;

	request = clawt_media_request_new("pixellab", args, &error);
	g_assert_no_error(error);
	g_assert_true(ai_media_request_has_param(request, "image_size"));
	g_assert_cmpstr(ai_media_request_get_prompt(request), ==, "a red desk");

	/* the operation is said once, by its own argument */
	{
		g_autofree gchar *json = ai_media_request_to_json(request);

		g_assert_null(strstr(json, "sneaky"));
	}

	g_assert_null(clawt_media_request_new("meshy", none, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

static void
test_cost_line(void)
{
	g_autoptr(JsonObject) meshy = object_from(
		"{\"operation\":\"rig\"}");
	g_autoptr(JsonObject) pixel = object_from(
		"{\"operation\":\"character-4\"}");
	g_autofree gchar *m = clawt_media_cost_line("meshy", meshy);
	g_autofree gchar *p = clawt_media_cost_line("pixellab", pixel);

	g_assert_cmpstr(m, ==, "Meshy rig: about 5 credits, from Meshy's price list.");
	g_assert_nonnull(strstr(p, "priced by PixelLab when it is done"));
	g_assert_nonnull(strstr(p, CLAWT_MEDIA_PIXELLAB_TYPICAL));
}

static void
test_file_names(void)
{
	g_autofree gchar *a = clawt_media_file_name("south:0", "data:image/png;base64,AA");
	g_autofree gchar *b = clawt_media_file_name("glb", "https://x.invalid/a?b=1");
	g_autofree gchar *c = clawt_media_file_name("rig:fbx", NULL);
	g_autofree gchar *d = clawt_media_file_name("../../etc/passwd",
	                                            "https://x.invalid/t.png");
	g_autofree gchar *e = clawt_media_file_name("thumb",
	                                            "https://x.invalid/a/thumb.PNG");

	g_assert_cmpstr(a, ==, "south-0.png");
	g_assert_cmpstr(b, ==, "glb.glb");
	g_assert_cmpstr(c, ==, "rig-fbx.fbx");
	g_assert_null(strchr(d, '/'));
	g_assert_true(d[0] != '.');
	g_assert_cmpstr(e, ==, "thumb.png");

	g_assert_cmpuint(clawt_media_max_bytes_for(b), ==, CLAWT_MEDIA_MAX_MODEL_BYTES);
	g_assert_cmpuint(clawt_media_max_bytes_for(a), ==, CLAWT_MEDIA_MAX_IMAGE_BYTES);
}

static void
test_spend(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-media-XXXXXX", NULL);
	g_autofree gchar *path = clawt_media_spend_path(dir);
	g_autoptr(GError) error = NULL;
	gint64 micros = -1, unpriced = -1, calls = -1;

	/* missing is zero */
	clawt_media_spend_since(path, 0, 0, &micros, &unpriced, &calls);
	g_assert_cmpint(micros, ==, 0);
	g_assert_cmpint(calls, ==, 0);

	g_assert_true(clawt_media_spend_record(path, "pixellab", "image", "j1",
	                                       40000, -1, &error));
	g_assert_true(clawt_media_spend_record(path, "meshy", "rig", "j2",
	                                       -1, 5, &error));
	g_assert_no_error(error);

	/* credits unpriced without a price, converted with one */
	clawt_media_spend_since(path, 0, 0, &micros, &unpriced, &calls);
	g_assert_cmpint(micros, ==, 40000);
	g_assert_cmpint(unpriced, ==, 5);
	g_assert_cmpint(calls, ==, 2);

	clawt_media_spend_since(path, 0, 20000, &micros, &unpriced, NULL);
	g_assert_cmpint(micros, ==, 140000);
	g_assert_cmpint(unpriced, ==, 0);

	/* a later window sees nothing */
	clawt_media_spend_since(path, G_MAXINT64 / 2, 0, &micros, NULL, &calls);
	g_assert_cmpint(calls, ==, 0);

	clawt_test_remove_tree(dir);
}

/* ── A mock service ──────────────────────────────────────────────── */

typedef struct {
	GObject  parent_instance;
	guint    submits;
	gchar   *last_operation;
	gchar   *last_polled;
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
	g_free(((MockMedia *)object)->last_operation);
	g_free(((MockMedia *)object)->last_polled);
	G_OBJECT_CLASS(mock_media_parent_class)->finalize(object);
}

static void
mock_media_init(MockMedia *self)
{
	(void)self;
}

static void
mock_media_class_init(MockMediaClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = mock_media_finalize;
}

static void
mock_submit_async(AiMediaGenerator *self, AiMediaRequest *request,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	g_autofree gchar *json = ai_media_request_to_json(request);
	MockMedia *mock = (MockMedia *)self;

	mock->submits++;
	g_free(mock->last_operation);
	mock->last_operation = g_strdup(json);
	g_task_return_pointer(task, g_strdup(strstr(json, "text-to-3d") != NULL
	                                     ? "text-to-3d:p1" : "job-7"), g_free);
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
	MockMedia *mock = (MockMedia *)self;
	/* a Meshy text-to-3d job takes a new id once its refine starts */
	AiMediaJob *job = ai_media_job_new(g_str_has_prefix(job_id, "text-to-3d:")
	                                   ? "text-to-3d:p1:r2" : job_id,
	                                   AI_MEDIA_JOB_STATE_DONE);

	g_free(mock->last_polled);
	mock->last_polled = g_strdup(job_id);

	ai_media_job_add_named_result(job, "image",
	                              "data:image/png;base64," PNG_B64);
	ai_media_job_add_named_result(job, "elsewhere", "http://plain.invalid/x.png");
	ai_media_job_set_cost_micro_usd(job, 40000);
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
mock_download_finish(AiMediaGenerator *self, GAsyncResult *result,
                     GError **error)
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
	return g_object_ref(AI_MEDIA_GENERATOR(the_mock));
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
} Fixture;

static void
setup(Fixture *fixture, const gchar *rules)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-media-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"  exchange_dir: \"%s/exchange\"\n"
		"integrations:\n"
		"  - name: pl\n    type: pixellab\n    scope: selected\n"
		"    agents: [alpha]\n    token: {env: CLAWT_TEST_PIXELLAB}\n"
		"  - name: me\n    type: meshy\n    scope: selected\n"
		"    agents: [alpha]\n    token: {env: CLAWT_TEST_MESHY}\n"
		"agents:\n"
		"  - id: alpha\n    computer:\n      type: none\n"
		"    runtime:\n      autostart: false\n%s"
		"  - id: beta\n    computer:\n      type: none\n"
		"    runtime:\n      autostart: false\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, fixture->dir, rules);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));

	the_mock = g_object_new(mock_media_get_type(), NULL);
	fixture->daemon->media_client_factory = mock_factory;
}

static void
teardown(Fixture *fixture)
{
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
	g_clear_object(&the_mock);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
tools_call(const gchar *tool, const gchar *arguments)
{
	g_autofree gchar *json = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);

	return json_from_string(json, NULL);
}

typedef struct {
	JsonNode *response;
	gboolean  done;
} Waiter;

static void
on_called(GObject *source, GAsyncResult *result, gpointer data)
{
	Waiter *waiter = data;

	waiter->response = clawt_mcp_tools_call_finish(CLAWT_MCP_TOOLS(source),
	                                               result);
	waiter->done = TRUE;
}

static gboolean
on_watchdog(gpointer data)
{
	(void)data;
	g_error("the media call did not answer");
	return G_SOURCE_REMOVE;
}

/* A call on the deferred path, waited for with a watchdog. */
static JsonNode *
call_async(Fixture *fixture, const gchar *agent, const gchar *tool,
           const gchar *arguments)
{
	g_autoptr(JsonNode) request = tools_call(tool, arguments);
	Waiter waiter = { NULL, FALSE };
	GSource *watchdog = g_timeout_source_new_seconds(20);

	g_source_set_callback(watchdog, on_watchdog, NULL, NULL);
	g_source_attach(watchdog, fixture->context);

	g_assert_true(clawt_mcp_tools_call_defers(fixture->daemon->mcp_tools,
	                                          agent, request));
	g_main_context_push_thread_default(fixture->context);
	clawt_mcp_tools_call_async(fixture->daemon->mcp_tools, agent, NULL,
	                           request, on_called, &waiter);
	while (!waiter.done)
		g_main_context_iteration(fixture->context, TRUE);
	g_main_context_pop_thread_default(fixture->context);

	g_source_destroy(watchdog);
	g_source_unref(watchdog);
	return waiter.response;
}

/* The text of a tool response, joined. */
static gchar *
response_text(JsonNode *response)
{
	JsonObject *result = json_object_get_object_member(
		json_node_get_object(response), "result");
	JsonArray *content = json_object_get_array_member(result, "content");
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(content); i++) {
		JsonObject *item = json_array_get_object_element(content, i);

		if (g_strcmp0(json_object_get_string_member(item, "type"), "text") == 0)
			g_string_append(out, json_object_get_string_member(item, "text"));
	}
	return g_string_free(out, FALSE);
}

static gboolean
offered(Fixture *fixture, const gchar *agent, const gchar *tool)
{
	return clawt_mcp_tools_is_permitted(fixture->daemon->mcp_tools, agent, tool);
}

/* Only the agent the service is bound to is offered its tools. */
static void
test_gate(void)
{
	Fixture fixture = { 0 };

	setup(&fixture, "");
	g_assert_true(offered(&fixture, "alpha", "clawtilla_pixellab_generate"));
	g_assert_true(offered(&fixture, "alpha", "clawtilla_pixellab_job"));
	g_assert_false(offered(&fixture, "beta", "clawtilla_pixellab_generate"));
	g_assert_true(offered(&fixture, "alpha", "clawtilla_meshy_generate"));
	g_assert_false(offered(&fixture, "beta", "clawtilla_meshy_balance"));
	teardown(&fixture);
}

/*
 * With no rule, a generation is asked of a person: the route that cannot
 * wait refuses it saying so, and the deferred route would ask.
 */
static void
test_asks_by_default(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) request = tools_call("clawtilla_pixellab_generate",
		"{\"operation\":\"image\",\"prompt\":\"a desk\"}");
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *text = NULL;

	setup(&fixture, "");
	response = clawt_mcp_tools_call(fixture.daemon->mcp_tools, "alpha", NULL,
	                                request);
	text = response_text(response);
	g_assert_nonnull(strstr(text, "a paid service"));
	g_assert_cmpuint(the_mock->submits, ==, 0);

	/* a read is not asked */
	g_assert_false(clawt_mcp_tools_call_defers(fixture.daemon->mcp_tools,
		"beta", request));
	teardown(&fixture);
}

/*
 * A paid call is held until the operator answers, and the agent is told
 * so in the listing it reads: the first live run's agent gave up on a
 * held call after four minutes, ending its turn, which withdrew the ask.
 */
static void
test_paid_tools_say_they_wait(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) listing = NULL;
	JsonArray *tools;
	guint paid = 0;
	guint i;

	setup(&fixture, "");
	listing = clawt_mcp_tools_list(fixture.daemon->mcp_tools, "alpha");
	tools = json_object_get_array_member(json_node_get_object(listing), "tools");
	for (i = 0; i < json_array_get_length(tools); i++) {
		JsonObject *tool = json_array_get_object_element(tools, i);
		const gchar *name = json_object_get_string_member(tool, "name");
		const gchar *description = json_object_get_string_member(tool, "description");

		if (!g_str_has_suffix(name, "_generate"))
			continue;
		paid++;
		g_assert_nonnull(strstr(description, "ending the turn withdraws the request"));
	}
	/* alpha holds both services */
	g_assert_cmpuint(paid, ==, 2);
	teardown(&fixture);
}

/*
 * Allowed by a rule: the call goes to the service, the result is saved
 * beside a provenance.yaml where the agent can read it, a non-https link
 * is refused rather than fetched, and what it cost is recorded where the
 * daily caps read it.
 */
static void
test_generate_saves(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *spend = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *image = NULL;
	g_autofree gchar *provenance = NULL;
	gint64 micros = 0;
	const gchar *at;
	gchar *line_end;

	setup(&fixture, "    policy:\n      rules: [\"clawtilla_pixellab_*=allow\"]\n");
	response = call_async(&fixture, "alpha", "clawtilla_pixellab_generate",
	                      "{\"operation\":\"image\",\"prompt\":\"a desk\"}");
	text = response_text(response);

	g_assert_cmpuint(the_mock->submits, ==, 1);
	g_assert_nonnull(strstr(the_mock->last_operation, "\"image\""));
	g_assert_nonnull(strstr(text, "is done, costing $0.0400"));
	g_assert_nonnull(strstr(text, "image.png"));
	g_assert_nonnull(strstr(text, "elsewhere.png (not an https link)"));

	at = strstr(text, "Saved to ");
	g_assert_nonnull(at);
	saved = g_strdup(at + strlen("Saved to "));
	line_end = strchr(saved, ':');
	g_assert_nonnull(line_end);
	*line_end = '\0';
	g_assert_true(g_str_has_prefix(saved, fixture.dir));

	image = g_build_filename(saved, "image.png", NULL);
	provenance = g_build_filename(saved, "provenance.yaml", NULL);
	g_assert_true(g_file_test(image, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(provenance, G_FILE_TEST_IS_REGULAR));

	state_dir = clawt_config_agent_state_dir(fixture.daemon->config, "alpha");
	spend = clawt_media_spend_path(state_dir);
	clawt_media_spend_since(spend, 0, 0, &micros, NULL, NULL);
	g_assert_cmpint(micros, ==, 40000);

	/*
	 * alpha has no computer, so it is told the path its tools read and
	 * nothing else: offered the guest spelling too, the first live agent
	 * reported /mnt/clawtilla/... back, where nothing exists.
	 */
	g_assert_null(strstr(text, "/mnt/clawtilla"));

	/* with a computer that mounts the exchange, both, each said for what */
	{
		g_autoptr(JsonNode) again = NULL;
		g_autofree gchar *with_computer = NULL;

		g_assert_true(clawt_agent_config_set_string(
			clawt_config_get_agent(fixture.daemon->config, "alpha"),
			"computer.type", "container"));
		again = call_async(&fixture, "alpha", "clawtilla_pixellab_generate",
		                   "{\"operation\":\"image\",\"prompt\":\"a chair\"}");
		with_computer = response_text(again);
		g_assert_nonnull(strstr(with_computer, "Saved to "));
		g_assert_nonnull(strstr(with_computer, "(clawtilla_computer_exec) sees it as "
		                                       "/mnt/clawtilla/exchange/alpha/media/"));
	}

	teardown(&fixture);
}

/* A job is only the agent's that started it. */
static void
test_job_is_its_own(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) theirs = NULL;
	g_autoptr(JsonNode) unknown = NULL;
	g_autofree gchar *text = NULL;

	setup(&fixture, "    policy:\n      rules: [\"clawtilla_pixellab_*=allow\"]\n");
	first = call_async(&fixture, "alpha", "clawtilla_pixellab_generate",
	                   "{\"operation\":\"image\",\"prompt\":\"a desk\"}");

	/* beta has no binding: not offered, so refused on the quick path */
	g_assert_false(offered(&fixture, "beta", "clawtilla_pixellab_job"));

	unknown = call_async(&fixture, "alpha", "clawtilla_pixellab_job",
	                     "{\"job_id\":\"job-999\"}");
	text = response_text(unknown);
	g_assert_nonnull(strstr(text, "no job by that id that you started"));

	theirs = call_async(&fixture, "alpha", "clawtilla_pixellab_job",
	                    "{\"job_id\":\"job-7\"}");
	g_free(text);
	text = response_text(theirs);
	g_assert_nonnull(strstr(text, "is done"));

	teardown(&fixture);
}

/* A picture outside the agent's workspace and the exchange is not sent. */
static void
test_images_stay_inside(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *text = NULL;

	setup(&fixture, "    policy:\n      rules: [\"clawtilla_pixellab_*=allow\"]\n");
	response = call_async(&fixture, "alpha", "clawtilla_pixellab_generate",
		"{\"operation\":\"rotate\",\"images\":{\"from_image\":\"/etc/hostname\"}}");
	text = response_text(response);
	g_assert_nonnull(strstr(text, "outside your workspace"));
	g_assert_cmpuint(the_mock->submits, ==, 0);

	teardown(&fixture);
}

/* The paid tool asks; a rule that allows it is still read first. */
static void
test_cost_in_the_question(void)
{
	g_autoptr(JsonObject) args = object_from("{\"operation\":\"rig\"}");
	g_autofree gchar *line = clawt_media_cost_line("meshy", args);

	g_assert_true(g_str_has_prefix(line, "Meshy rig: about 5 credits"));
}

/*
 * A job whose id changes as it goes is polled by its newest id, and the
 * reply names the service's task id a next step needs.
 */
static void
test_newest_id(void)
{
	Fixture fixture = { 0 };
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) again = NULL;
	g_autofree gchar *text = NULL;

	setup(&fixture, "    policy:\n      rules: [\"clawtilla_meshy_*=allow\"]\n");
	first = call_async(&fixture, "alpha", "clawtilla_meshy_generate",
	                   "{\"operation\":\"text-to-3d\",\"prompt\":\"a chair\"}");
	text = response_text(first);
	g_assert_nonnull(strstr(text, "rig_task_id in a next step: r2"));

	again = call_async(&fixture, "alpha", "clawtilla_meshy_job",
	                   "{\"job_id\":\"text-to-3d:p1\"}");
	g_assert_cmpstr(the_mock->last_polled, ==, "text-to-3d:p1:r2");

	teardown(&fixture);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/media/service-for-tool", test_service_for_tool);
	g_test_add_func("/media/request", test_request);
	g_test_add_func("/media/cost-line", test_cost_line);
	g_test_add_func("/media/file-names", test_file_names);
	g_test_add_func("/media/spend", test_spend);
	g_test_add_func("/media/cost-in-the-question", test_cost_in_the_question);
	g_test_add_func("/media/daemon/gate", test_gate);
	g_test_add_func("/media/daemon/asks-by-default", test_asks_by_default);
	g_test_add_func("/media/daemon/paid-tools-say-they-wait", test_paid_tools_say_they_wait);
	g_test_add_func("/media/daemon/generate-saves", test_generate_saves);
	g_test_add_func("/media/daemon/job-is-its-own", test_job_is_its_own);
	g_test_add_func("/media/daemon/images-stay-inside", test_images_stay_inside);
	g_test_add_func("/media/daemon/newest-id", test_newest_id);

	return g_test_run();
}
