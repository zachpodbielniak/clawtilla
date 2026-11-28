/*
 * test-web-board.c - The web agent board, drawn with JavaScript off
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no browser.  The page's source is included, as
 * test-web-schedule does, and board_render() is fed a canned task.board
 * reply (the contract in docs/ipc-protocol.org).  Every assertion is on
 * the HTML a browser would receive:
 *
 *   - the seven columns, Blocked first, Done collapsed until asked;
 *   - a hostile title, note, worker, agent id, branch and ticket url
 *     reach the page escaped, in text, in attributes and in the SVG;
 *   - the page is complete with no script: every control is a link and
 *     the Timeline is an SVG;
 *   - the page has exactly one event listener: the frame's own
 *     sse-connect, and one `sse:fleet` trigger on a host that is never
 *     swapped.  There is no EventSource, timer or `every` trigger here,
 *     which is what VENTURE #38's leak was made of.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "clawt-test-util.h"

#include "../clients/web/web-app.c"
#include "../clients/web/web-files.c"
#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"
#include "../clients/web/web-board.c"

/* The frame needs the fleet and the topbar; nothing here draws one. */
HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtPage view,
                  const ClawtWebLook *look)
{
	(void)app;
	(void)selected;
	(void)view;
	(void)look;
	return HTMX_ELEMENT(htmx_div_new());
}

HtmxElement *
clawt_web_topbar(ClawtWebApp *app, const gchar *agent_id, ClawtPage view)
{
	(void)app;
	(void)agent_id;
	(void)view;
	return HTMX_ELEMENT(htmx_div_new());
}

/* The List is another module's; the board only links to it. */
HtmxElement *
clawt_web_tasks_body(ClawtWebApp *app, const gchar *agent_id)
{
	(void)app;
	(void)agent_id;
	return HTMX_ELEMENT(htmx_div_new());
}

/* ── A canned task.board reply ───────────────────────────────────── */

#define HOSTILE_TITLE "<script>alert(1)</script>"
#define HOSTILE_NOTE  "</div><img src=x onerror=alert(2)>"

static JsonNode *
canned_board(void)
{
	g_autoptr(GError) error = NULL;
	JsonNode *node = json_from_string(
	    "{\"agent\":\"ada\",\"team\":\"ops\",\"now\":1800000000,"
	    "\"window\":{\"spec\":\"today\",\"since\":1799971200},"
	    "\"quiet_minutes\":20,"
	    "\"header\":{\"agent\":\"ada\",\"name\":\"Ada\",\"state\":\"running\","
	    "\"team\":\"ops\","
	    "\"step\":{\"text\":\"Bash: make <b>x</b>\",\"tool\":\"Bash\","
	    "\"at\":1799999880},\"mailbox\":3,"
	    "\"current_task\":{\"id\":\"task-d\",\"title\":\"Current <i>t</i>\"},"
	    "\"counts\":{\"blocked\":1,\"backlog\":1,\"planning\":0,"
	    "\"delegated\":1,\"review\":1,\"gating\":0,\"done\":1,"
	    "\"in_flight\":1,\"in_review\":1}},"
	    "\"columns\":["
	    "{\"id\":\"blocked\",\"label\":\"Blocked\",\"cards\":["
	    "{\"id\":\"task-b\",\"title\":\"Blocked one\",\"state\":\"blocked\","
	    "\"column\":\"blocked\",\"elapsed\":600,\"age\":700,\"quiet\":false,"
	    "\"created_at\":1799999000,"
	    "\"blocks\":[{\"reason\":\"failed\",\"detail\":\"it <b>broke</b>\"}],"
	    "\"chips\":[]}]},"
	    "{\"id\":\"backlog\",\"label\":\"Backlog\",\"cards\":["
	    "{\"id\":\"task-c\",\"title\":\"Backlog one\",\"state\":\"pending\","
	    "\"column\":\"backlog\",\"position\":1,\"elapsed\":30,\"age\":30,"
	    "\"quiet\":false,\"created_at\":1799999970,\"chips\":[]}]},"
	    "{\"id\":\"planning\",\"label\":\"Planning\",\"cards\":[]},"
	    "{\"id\":\"delegated\",\"label\":\"Delegated\",\"cards\":["
	    "{\"id\":\"task-d\",\"title\":\"" HOSTILE_TITLE "\","
	    "\"state\":\"running\",\"column\":\"delegated\","
	    "\"repo\":\"repo<x>\",\"branch\":\"feat/a\","
	    "\"note\":\"" "</div><img src=x onerror=alert(2)>" "\","
	    "\"cost_usd\":1.5,\"elapsed\":4000,\"age\":4000,\"quiet\":true,"
	    "\"created_at\":1799990000,\"started_at\":1799990100,"
	    "\"ticket\":{\"id\":41,\"url\":\"https://v.example/e/ticket/41\","
	    "\"state\":\"open\"},"
	    "\"chips\":[{\"id\":\"task-e\","
	    "\"title\":\"x\\\" onmouseover=\\\"alert(1)\","
	    "\"worker\":\"wb<i>\",\"state\":\"running\","
	    "\"created_at\":1799995000,\"started_at\":1799995100,"
	    "\"quiet\":false,\"timeline\":{\"source\":\"journal\","
	    "\"states\":[{\"state\":\"pending\",\"from\":1799995000,"
	    "\"to\":1799995100},{\"state\":\"running\",\"from\":1799995100}],"
	    "\"phases\":[]}}],"
	    "\"timeline\":{\"source\":\"journal\",\"states\":["
	    "{\"state\":\"pending\",\"from\":1799990000,\"to\":1799990100},"
	    "{\"state\":\"running\",\"from\":1799990100}],"
	    "\"phases\":[{\"phase\":\"delegated\",\"from\":1799990200}]}}]},"
	    "{\"id\":\"review\",\"label\":\"In review\",\"cards\":["
	    "{\"id\":\"task-r\",\"title\":\"Reviewed\",\"state\":\"review\","
	    "\"column\":\"review\",\"elapsed\":900,\"age\":900,\"quiet\":false,"
	    "\"created_at\":1799999100,"
	    "\"ticket\":{\"id\":42,\"url\":\"javascript:alert(3)\","
	    "\"state\":\"open\"},"
	    "\"review\":{\"task\":\"task-r\",\"worker\":\"wb\","
	    "\"waiting_for\":\"user\",\"since\":1799999100,\"wait\":900},"
	    "\"chips\":[]}]},"
	    "{\"id\":\"gating\",\"label\":\"Gating\",\"cards\":[]},"
	    "{\"id\":\"done\",\"label\":\"Done\",\"cards\":["
	    "{\"id\":\"task-z\",\"title\":\"Finished thing\","
	    "\"state\":\"completed\",\"column\":\"done\",\"elapsed\":120,"
	    "\"age\":300,\"quiet\":false,\"created_at\":1799999700,"
	    "\"finished_at\":1799999820,\"chips\":[]}]}],"
	    "\"lanes\":[{\"agent\":\"ev<il>\\\"&\",\"role\":\"member\","
	    "\"state\":\"idle\",\"queue\":{\"queued\":1,\"running\":0,"
	    "\"review\":0,\"mailbox\":2}}],"
	    "\"totals\":{\"listed\":5,\"cards\":5,\"nested\":0,\"hidden\":0,"
	    "\"from_history\":0}}", &error);

	g_assert_no_error(error);
	g_assert_nonnull(node);

	return node;
}

/* The panel's contents, as /f/board answers them. */
static gchar *
draw(const gchar *view, const gchar *lanes, const gchar *done)
{
	g_autoptr(JsonNode) reply = canned_board();
	g_autoptr(HtmxElement) panel = NULL;
	BoardQuery q;
	gchar *html;

	board_query_init(&q, "ada", view, NULL, lanes, done, NULL);
	panel = board_render(&q, reply, NULL);
	html = htmx_element_render(panel);
	board_query_clear(&q);

	return html;
}

/* The page body a browser first gets: switcher, host and panel. */
static gchar *
draw_page(const gchar *view)
{
	g_autoptr(JsonNode) reply = canned_board();
	g_autoptr(HtmxElement) page = NULL;
	BoardQuery q;
	gchar *html;

	board_query_init(&q, "ada", view, NULL, NULL, NULL, NULL);
	page = board_page(&q, board_render(&q, reply, NULL));
	html = htmx_element_render(page);
	board_query_clear(&q);

	return html;
}

static guint
count_of(const gchar *html, const gchar *needle)
{
	guint n = 0;
	const gchar *at = html;

	while ((at = strstr(at, needle)) != NULL) {
		n++;
		at += strlen(needle);
	}

	return n;
}

/* ── Columns ─────────────────────────────────────────────────────── */

static void
test_the_seven_columns_in_order_with_blocked_first(void)
{
	g_autofree gchar *html = draw("board", NULL, NULL);
	const gchar *blocked = strstr(html, ">Blocked (1)<");
	const gchar *backlog = strstr(html, ">Backlog (1)<");
	const gchar *planning = strstr(html, ">Planning (0)<");
	const gchar *delegated = strstr(html, ">Delegated (1)<");
	const gchar *review = strstr(html, ">In review (1)<");
	const gchar *gating = strstr(html, ">Gating (0)<");
	const gchar *done = strstr(html, ">Done (1)<");

	g_assert_nonnull(blocked);
	g_assert_nonnull(backlog);
	g_assert_nonnull(planning);
	g_assert_nonnull(delegated);
	g_assert_nonnull(review);
	g_assert_nonnull(gating);
	g_assert_nonnull(done);
	g_assert_true(blocked < backlog && backlog < planning &&
	              planning < delegated && delegated < review &&
	              review < gating && gating < done);
	g_assert_cmpuint(count_of(html, "class=\"board-col board-col-"), ==, 7);

	/* an empty column says so rather than drawing nothing */
	g_assert_nonnull(strstr(html, ">Nothing<"));
}

static void
test_the_header_strip_and_the_cards(void)
{
	g_autofree gchar *html = draw("board", NULL, NULL);

	/* live state and step, current task, mailbox, counts */
	g_assert_nonnull(strstr(html, ">Ada<"));
	g_assert_nonnull(strstr(html, ">running<"));
	g_assert_nonnull(strstr(html, "Bash: make &lt;b&gt;x&lt;/b&gt;"));
	g_assert_nonnull(strstr(html, "href=\"/tasks/task-d\""));
	g_assert_nonnull(strstr(html, ">Mailbox 3<"));
	g_assert_nonnull(strstr(html, ">In flight 1<"));
	g_assert_nonnull(strstr(html, ">In review 1<"));

	/* a card: title link, repo, elapsed, cost, chips, quiet, position */
	g_assert_nonnull(strstr(html, "href=\"/tasks/task-b\""));
	g_assert_nonnull(strstr(html, ">next #1<"));
	g_assert_nonnull(strstr(html, "repo&lt;x&gt; @ feat/a"));
	g_assert_nonnull(strstr(html, ">1h 06m<"));
	g_assert_nonnull(strstr(html, ">$1.50<"));
	g_assert_nonnull(strstr(html, ">quiet<"));
	g_assert_nonnull(strstr(html, "board-chip"));

	/* blocked says why; in review says how long it has waited */
	g_assert_nonnull(strstr(html, "failed: it &lt;b&gt;broke&lt;/b&gt;"));
	g_assert_nonnull(strstr(html, "In review 15m, waiting for user"));
}

static void
test_done_is_collapsed_until_asked(void)
{
	g_autofree gchar *closed = draw("board", NULL, NULL);
	g_autofree gchar *open = draw("board", NULL, "1");

	g_assert_null(strstr(closed, "Finished thing"));
	g_assert_nonnull(strstr(closed, ">Show<"));
	g_assert_nonnull(strstr(closed, "done=1"));

	g_assert_nonnull(strstr(open, "Finished thing"));
	g_assert_nonnull(strstr(open, ">Hide<"));
}

static void
test_ticket_links_are_only_ever_http(void)
{
	g_autofree gchar *html = draw("board", NULL, NULL);

	/* a real ticket is a link, labelled with its number */
	g_assert_nonnull(strstr(html, "href=\"https://v.example/e/ticket/41\""));
	g_assert_nonnull(strstr(html, ">VENTURE #41<"));

	/* a javascript: address is shown as text and never linked */
	g_assert_nonnull(strstr(html, ">VENTURE #42<"));
	g_assert_null(strstr(html, "javascript:"));
}

static void
test_worker_lanes_are_a_toggle_link(void)
{
	g_autofree gchar *off = draw("board", NULL, NULL);
	g_autofree gchar *on = draw("board", "1", NULL);

	g_assert_null(strstr(off, "board-lane\""));
	g_assert_nonnull(strstr(off, ">Worker lanes<"));
	g_assert_nonnull(strstr(off, "lanes=1"));

	g_assert_nonnull(strstr(on, "class=\"board-lane\""));
	g_assert_nonnull(strstr(on, ">Hide worker lanes<"));
	g_assert_nonnull(strstr(on, "queued 1"));
}

static void
test_the_agent_filter_rides_the_links(void)
{
	g_autoptr(JsonNode) reply = canned_board();
	g_autoptr(HtmxElement) panel = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *url = clawt_web_board_url("ada", "wild build");
	BoardQuery q;

	g_assert_cmpstr(url, ==, "/a/ada/tasks?v=board&agent=wild%20build");

	board_query_init(&q, "ada", "board", "wildebuild", NULL, NULL, "3d");
	panel = board_render(&q, reply, NULL);
	html = htmx_element_render(panel);
	board_query_clear(&q);

	g_assert_nonnull(strstr(html, "Showing wildebuild"));
	g_assert_nonnull(strstr(html, "agent=wildebuild"));
	g_assert_nonnull(strstr(html, "window=3d"));
}

static void
test_a_bad_window_is_dropped(void)
{
	BoardQuery q;

	board_query_init(&q, "ada", "board", NULL, NULL, NULL, "yesterday!");
	g_assert_null(q.window);
	board_query_clear(&q);

	board_query_init(&q, "ada", "nonsense", NULL, NULL, NULL, "12h");
	g_assert_cmpstr(q.view, ==, "list");
	g_assert_cmpstr(q.window, ==, "12h");
	board_query_clear(&q);
}

/* ── The Timeline ────────────────────────────────────────────────── */

static void
test_the_timeline_is_a_server_drawn_svg(void)
{
	g_autofree gchar *html = draw("timeline", NULL, NULL);

	g_assert_nonnull(strstr(html, "<svg class=\"board-timeline\""));
	g_assert_nonnull(strstr(html, "</svg>"));

	/* a bar per state span, coloured by state, a thin one per phase */
	g_assert_nonnull(strstr(html, "tl-s tl-s-running"));
	g_assert_nonnull(strstr(html, "tl-s tl-s-pending"));
	g_assert_nonnull(strstr(html, "tl-p tl-p-delegated"));

	/* the child beneath its card, and "now" marked */
	g_assert_nonnull(strstr(html, "tl-chip"));
	g_assert_nonnull(strstr(html, "class=\"tl-now\""));
	g_assert_nonnull(strstr(html, ">now<"));

	/* the columns are not drawn on the Timeline */
	g_assert_null(strstr(html, "board-cols"));
}

/* ── Escaping ────────────────────────────────────────────────────── */

static void
test_hostile_text_is_escaped_everywhere(void)
{
	g_autofree gchar *board = draw("board", "1", "1");
	g_autofree gchar *timeline = draw("timeline", NULL, NULL);
	const gchar *views[2];
	guint i;

	views[0] = board;
	views[1] = timeline;

	for (i = 0; i < G_N_ELEMENTS(views); i++) {
		const gchar *html = views[i];

		/* title, note, worker, agent id: never as markup */
		g_assert_null(strstr(html, "<script>alert(1)"));
		g_assert_null(strstr(html, "<img src=x"));
		g_assert_null(strstr(html, "<i>"));
		g_assert_null(strstr(html, "<il>"));
		g_assert_null(strstr(html, "<b>x"));

		/* an attribute never lets a quote out */
		g_assert_null(strstr(html, "onmouseover=\"alert"));
	}

	g_assert_nonnull(strstr(board, "&lt;script&gt;alert(1)&lt;/script&gt;"));
	g_assert_nonnull(strstr(board, "&lt;/div&gt;&lt;img src=x"));
	g_assert_nonnull(strstr(board, "wb&lt;i&gt; "));
	g_assert_nonnull(strstr(board, "ev&lt;il&gt;"));

	/* the same title inside the SVG, escaped for markup */
	g_assert_nonnull(strstr(timeline, "&lt;script&gt;alert(1)"));
	g_assert_nonnull(strstr(timeline, "wb&lt;i&gt;"));

	/* the lane's link target is percent-encoded, not escaped text */
	g_assert_nonnull(strstr(board, "agent=ev%3Cil%3E%22%26"));
}

/* ── No script, one listener ─────────────────────────────────────── */

static void
test_the_page_is_complete_without_script(void)
{
	g_autofree gchar *board = draw_page("board");
	g_autofree gchar *timeline = draw_page("timeline");

	/* nothing here is a script, and every control is a link */
	g_assert_null(strstr(board, "<script"));
	g_assert_null(strstr(timeline, "<script"));
	g_assert_nonnull(strstr(board, "href=\"/a/ada/tasks?v=list\""));
	g_assert_nonnull(strstr(board, "href=\"/a/ada/tasks?v=timeline\""));
	g_assert_nonnull(strstr(board, ">Refresh<"));
	g_assert_nonnull(strstr(board, "aria-current=\"page\""));

	/* the whole board is already in the markup */
	g_assert_nonnull(strstr(board, ">Delegated (1)<"));
	g_assert_nonnull(strstr(board, "Blocked one"));

	/* the SVG is already in the markup */
	g_assert_nonnull(strstr(timeline, "<svg"));
	g_assert_nonnull(strstr(timeline, ">Refresh<"));
}

static void
test_an_unavailable_board_says_so(void)
{
	g_autoptr(HtmxElement) panel = NULL;
	g_autofree gchar *html = NULL;
	BoardQuery q;

	board_query_init(&q, "ada", "board", NULL, NULL, NULL, NULL);
	panel = board_render(&q, NULL, "agent not found");
	html = htmx_element_render(panel);
	board_query_clear(&q);

	g_assert_nonnull(strstr(html, "The board is unavailable"));
	g_assert_nonnull(strstr(html, "agent not found"));
	g_assert_nonnull(strstr(html, ">Refresh<"));
}

/*
 * The leak VENTURE #38 fixed came from htmx `every` pollers, which
 * leaked an interval per swap.  The board starts nothing: one trigger on
 * the page's one event stream, on a host whose contents (never the host)
 * are swapped, so a swap cannot register a second.
 */
static void
test_exactly_one_listener(void)
{
	const gchar *views[] = { "board", "timeline" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(views); i++) {
		g_autofree gchar *html = draw_page(views[i]);
		g_autofree gchar *panel = draw(views[i], "1", "1");
		g_autoptr(HtmxElement) body = HTMX_ELEMENT(htmx_main_new());
		g_autofree gchar *full = NULL;

		/* the host carries the one trigger, coalesced, innerHTML */
		g_assert_cmpuint(count_of(html, "sse:fleet"), ==, 1);
		g_assert_nonnull(strstr(html,
		                        "hx-trigger=\"sse:fleet delay:600ms\""));
		g_assert_nonnull(strstr(html, "hx-swap=\"innerHTML\""));
		g_assert_nonnull(strstr(html, "hx-sync=\"this:replace\""));
		g_assert_cmpuint(count_of(html, "hx-get="), ==, 1);
		g_assert_nonnull(strstr(html, "hx-get=\"/f/board?v="));

		/* nothing in the board opens a stream, a timer or a poller */
		g_assert_null(strstr(html, "sse-connect"));
		g_assert_null(strstr(html, "hx-ext"));
		g_assert_null(strstr(html, "EventSource"));
		g_assert_null(strstr(html, "setInterval"));
		g_assert_null(strstr(html, "setTimeout"));
		g_assert_null(strstr(html, "every "));
		g_assert_null(strstr(html, "<script"));

		/* the fragment htmx swaps in carries no trigger at all */
		g_assert_cmpuint(count_of(panel, "hx-trigger"), ==, 0);
		g_assert_cmpuint(count_of(panel, "hx-get"), ==, 0);
		g_assert_cmpuint(count_of(panel, "id=\"board-live\""), ==, 0);
		g_assert_nonnull(strstr(html, "id=\"board-live\""));

		/*
		 * In the whole document: the frame's connection is the only
		 * one, and the board's host is the only thing listening on it.
		 */
		htmx_node_set_html_content(HTMX_NODE(body), html);
		full = clawt_web_shell_page(NULL, "Tasks", body, NULL);
		g_assert_cmpuint(count_of(full, "sse-connect="), ==, 1);
		g_assert_cmpuint(count_of(full, "hx-ext=\"sse\""), ==, 1);
		g_assert_cmpuint(count_of(full, "new EventSource"), ==, 0);
		g_assert_cmpuint(count_of(full, "sse:fleet"), ==, 1);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-board/columns",
	                test_the_seven_columns_in_order_with_blocked_first);
	g_test_add_func("/web-board/header-and-cards",
	                test_the_header_strip_and_the_cards);
	g_test_add_func("/web-board/done-collapsed",
	                test_done_is_collapsed_until_asked);
	g_test_add_func("/web-board/ticket-links",
	                test_ticket_links_are_only_ever_http);
	g_test_add_func("/web-board/lanes",
	                test_worker_lanes_are_a_toggle_link);
	g_test_add_func("/web-board/agent-filter",
	                test_the_agent_filter_rides_the_links);
	g_test_add_func("/web-board/bad-window", test_a_bad_window_is_dropped);
	g_test_add_func("/web-board/timeline-svg",
	                test_the_timeline_is_a_server_drawn_svg);
	g_test_add_func("/web-board/escaping",
	                test_hostile_text_is_escaped_everywhere);
	g_test_add_func("/web-board/no-script",
	                test_the_page_is_complete_without_script);
	g_test_add_func("/web-board/unavailable",
	                test_an_unavailable_board_says_so);
	g_test_add_func("/web-board/one-listener", test_exactly_one_listener);

	return g_test_run();
}
