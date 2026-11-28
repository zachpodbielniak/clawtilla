/*
 * test-web-fleet.c - The web Fleet row for an agent that is waiting (VENTURE #71)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The Fleet row of an agent with pending waits reads
 * `waiting: <label> (<age>)`, with ` +N` for N more waits.  The text
 * is the daemon's agent.list `activity`, `waits_pending`, `wait_label`
 * and `waiting_since`, worded by clawt_board_waiting_label().  The label
 * is written by the agent, so it is asserted escaped on the rendered
 * output.  The row moves on the sidebar's own `sse:fleet` trigger, and
 * these tests count that no second listener or timer came with it.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "clawt-test-util.h"

#include "../clients/web/web-app.c"
#include "../clients/web/web-files.c"
#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"
#include "../clients/web/web-fleet.c"

#define NOW (1000000)

/*
 * The pages the Fleet's router names but this test never opens.  Each is
 * another module's; the row is drawn without any of them.
 */
#define STUB_BODY(name, arg) \
	HtmxElement * \
	name(ClawtWebApp *app, const gchar *arg) \
	{ \
		(void)app; \
		(void)arg; \
		return NULL; \
	}

STUB_BODY(clawt_web_activity_body, agent_id)
STUB_BODY(clawt_web_agent_body, agent_id)
STUB_BODY(clawt_web_chat_body, agent_id)
STUB_BODY(clawt_web_computer_body, agent_id)
STUB_BODY(clawt_web_decisions_body, agent_id)
STUB_BODY(clawt_web_fleet_usage_body, agent_id)
STUB_BODY(clawt_web_flow_body, agent_id)
STUB_BODY(clawt_web_grok_body, agent_id)
STUB_BODY(clawt_web_mailbox_body, agent_id)
STUB_BODY(clawt_web_memory_body, agent_id)
STUB_BODY(clawt_web_office_body, agent_id)
STUB_BODY(clawt_web_room_body, room_id)
STUB_BODY(clawt_web_routines_body, agent_id)
STUB_BODY(clawt_web_skills_body, agent_id)
STUB_BODY(clawt_web_tasks_body, agent_id)
STUB_BODY(clawt_web_triggers_body, agent_id)

HtmxElement *
clawt_web_chat_body_full(ClawtWebApp *app, const gchar *agent_id,
                         gboolean cleared, const gchar *peer)
{
	(void)app;
	(void)agent_id;
	(void)cleared;
	(void)peer;
	return NULL;
}

HtmxElement *
clawt_web_schedule_body(ClawtWebApp *app, const gchar *agent_id,
                        HtmxRequest *request)
{
	(void)app;
	(void)agent_id;
	(void)request;
	return NULL;
}

HtmxElement *
clawt_web_tasks_page(ClawtWebApp *app, const gchar *agent_id,
                     HtmxRequest *request)
{
	(void)app;
	(void)agent_id;
	(void)request;
	return NULL;
}

void
clawt_web_register_navigation(HtmxRouter *router, ClawtWebApp *app)
{
	(void)router;
	(void)app;
}

static guint
count_of(const gchar *haystack, const gchar *needle)
{
	guint n = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}

	return n;
}

/*
 * An agent object as agent.list gives one.  The caller owns the parser.
 */
static JsonObject *
make_agent(JsonParser *parser, const gchar *json)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);

	return json_node_get_object(json_parser_get_root(parser));
}

static gchar *
row_html(JsonObject *agent)
{
	HtmxElement *row = agent_row(agent, NULL, CLAWT_PAGE_CHAT, 0, FALSE,
	                             TRUE);
	g_autofree gchar *html = htmx_element_render(row);

	g_object_unref(row);
	return g_steal_pointer(&html);
}

static void
test_waiting_text(void)
{
	g_autoptr(JsonParser) one = json_parser_new();
	g_autoptr(JsonParser) three = json_parser_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	a = clawt_web_agent_waiting_text(make_agent(one,
		"{\"id\":\"x\",\"activity\":\"waiting\",\"waits_pending\":1,"
		"\"wait_label\":\"gate g39\",\"waiting_since\":999280}"), NOW);
	g_assert_cmpstr(a, ==, "waiting: gate g39 (12m)");

	b = clawt_web_agent_waiting_text(make_agent(three,
		"{\"id\":\"x\",\"activity\":\"waiting\",\"waits_pending\":3,"
		"\"wait_label\":\"gate g39\",\"waiting_since\":999280}"), NOW);
	g_assert_cmpstr(b, ==, "waiting: gate g39 (12m) +2");
}

static void
test_idle_and_working_unchanged(void)
{
	g_autoptr(JsonParser) p1 = json_parser_new();
	g_autoptr(JsonParser) p2 = json_parser_new();
	g_autoptr(JsonParser) p3 = json_parser_new();
	g_autofree gchar *idle = NULL;
	g_autofree gchar *working = NULL;
	g_autofree gchar *busy_wait = NULL;

	idle = row_html(make_agent(p1,
		"{\"id\":\"x\",\"state\":\"running\",\"activity\":\"idle\","
		"\"waits_pending\":0}"));
	working = row_html(make_agent(p2,
		"{\"id\":\"x\",\"state\":\"running\",\"busy\":true,"
		"\"activity\":\"working\",\"waits_pending\":0}"));
	/* working while a wait is pending: still the working row */
	busy_wait = row_html(make_agent(p3,
		"{\"id\":\"x\",\"state\":\"running\",\"busy\":true,"
		"\"activity\":\"working\",\"waits_pending\":1,"
		"\"wait_label\":\"build\",\"waiting_since\":999280}"));

	g_assert_null(strstr(idle, "agent-waiting"));
	g_assert_null(strstr(idle, "waiting:"));
	g_assert_null(strstr(working, "agent-waiting"));
	g_assert_null(strstr(busy_wait, "agent-waiting"));
	g_assert_null(strstr(busy_wait, "waiting:"));
}

static void
test_row_says_waiting_and_escapes(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *html = NULL;

	html = row_html(make_agent(parser,
		"{\"id\":\"x\",\"state\":\"running\",\"activity\":\"waiting\","
		"\"waits_pending\":2,\"waiting_since\":1,"
		"\"wait_label\":\"<script>alert(1)</script> \\\"q\\\"\"}"));

	g_assert_nonnull(strstr(html, "class=\"agent-waiting\""));
	g_assert_nonnull(strstr(html, "waiting: &lt;script&gt;alert(1)"));
	g_assert_nonnull(strstr(html, "+1"));
	g_assert_null(strstr(html, "<script>"));
	/* a quote in the label is an entity, never a bare one */
	g_assert_nonnull(strstr(html, "&quot;q&quot;"));
}

/*
 * The live path adds nothing: the row is plain markup, and the sidebar
 * keeps the one `sse:fleet` trigger it always had, however many times it
 * is drawn again with a changed agent.
 */
static void
test_no_second_listener(void)
{
	guint i;

	for (i = 0; i < 5; i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autofree gchar *json = g_strdup_printf(
			"{\"id\":\"x\",\"state\":\"running\","
			"\"activity\":\"waiting\",\"waits_pending\":%u,"
			"\"wait_label\":\"job %u\",\"waiting_since\":999000}",
			i + 1, i);
		g_autofree gchar *html = row_html(make_agent(parser, json));

		g_assert_cmpuint(count_of(html, "sse:"), ==, 0);
		g_assert_cmpuint(count_of(html, "sse-connect"), ==, 0);
		g_assert_cmpuint(count_of(html, "hx-trigger"), ==, 0);
		g_assert_cmpuint(count_of(html, "EventSource"), ==, 0);
		g_assert_cmpuint(count_of(html, "setInterval"), ==, 0);
		g_assert_cmpuint(count_of(html, "<script"), ==, 0);
	}

	/* and the sidebar's own source holds the one trigger */
	{
		g_autofree gchar *src = NULL;
		g_autofree gchar *path = g_build_filename(
			CLAWT_TEST_SRCDIR, "clients", "web", "web-fleet.c", NULL);

		g_assert_true(g_file_get_contents(path, &src, NULL, NULL));
		g_assert_cmpuint(count_of(src, "\"sse:fleet\""), ==, 1);
		g_assert_cmpuint(count_of(src, "EventSource"), ==, 0);
		g_assert_cmpuint(count_of(src, "setInterval"), ==, 0);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web/fleet/waiting-text", test_waiting_text);
	g_test_add_func("/web/fleet/idle-working-unchanged",
	                test_idle_and_working_unchanged);
	g_test_add_func("/web/fleet/waiting-row-escapes",
	                test_row_says_waiting_and_escapes);
	g_test_add_func("/web/fleet/no-second-listener",
	                test_no_second_listener);

	return g_test_run();
}
