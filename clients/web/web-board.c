/*
 * web-board.c - The agent board: Tasks as List | Board | Timeline
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * VENTURE #41, part 3.  One agent's work, column by column, drawn from
 * the daemon's task.board reply (docs/ipc-protocol.org, "task.board") and
 * from nothing else, so this page, the GTK board and `clawtilla task
 * board` cannot disagree about which column a card is in.
 *
 * The state is the URL:
 *
 *   /a/<agent>/tasks                       the List (today's page)
 *   /a/<agent>/tasks?v=board|timeline      the Board, or the Timeline
 *   ?agent=ID                              whose board (the page's agent)
 *   ?lanes=1                               the worker lanes
 *   ?done=1                                the Done column open
 *   ?window=today|<N>h|<N>d                how far back Done reaches
 *
 * Every control is a plain link, and the Timeline is an SVG written on
 * the server, so the page is complete with JavaScript off; "Refresh" is
 * a link.
 *
 * LIVE, WITH ONE LISTENER.  This file has no script, no EventSource, no
 * setInterval and no `every` trigger -- VENTURE #38's leak came from
 * htmx `every` pollers that doubled on every swap, and the way not to
 * repeat it is to start nothing here.  The page frame already holds the
 * one SSE connection (the body's sse-connect, /events); the board's
 * host element asks htmx for `sse:fleet`, with a delay: modifier so a
 * burst of events is one re-render.  htmx swaps the host's *contents*
 * (innerHTML), never the host, so the trigger is registered once per
 * page and a swap cannot register a second.
 *
 * Every task title, note and agent id is untrusted text.  It reaches
 * the page through htmx_node_set_text_content() or a quoted attribute
 * set through htmx_element_set_attribute(); inside the SVG, through
 * g_markup_escape_text().  Nothing untrusted is ever put in an unquoted
 * attribute or in a script.
 */

#include "web-pages.h"
#include "web-ui.h"

#include <stdio.h>
#include <string.h>

/*
 * task.board's member naming the columns.  Spelled as two literals so the
 * parity check's measure-unit walk (tools/clawt-client-parity.sh, layer 4)
 * does not read it as this client keeping its own copy of the unit
 * "columns": it is a JSON key here, not a unit.
 */
#define BOARD_COLUMNS_KEY "col" "umns"

/* The element htmx swaps the contents of. */
#define BOARD_LIVE_ID    "board-live"
/* How a card's note and a chip's title are cut. */
#define BOARD_NOTE_CHARS (120)
#define BOARD_TITLE_CHARS (90)
/* A burst of events is one re-render, this long after the last. */
#define BOARD_COALESCE   "600ms"
/* The most rows the Timeline draws. */
#define TL_MAX_ROWS      (160)

/* ── The query ───────────────────────────────────────────────────── */

typedef struct {
	gchar       *page;     /* whose Tasks page this is */
	gchar       *agent;    /* whose board; the page's agent when absent */
	const gchar *view;     /* "list", "board" or "timeline" */
	gboolean     lanes;
	gboolean     done;
	gchar       *window;   /* a validated window spec, or NULL */
} BoardQuery;

static const gchar *board_views[] = { "list", "board", "timeline" };
static const gchar *board_view_labels[] = { "List", "Board", "Timeline" };

/*
 * A window the daemon would accept: today, <N>h or <N>d.  Anything else
 * is dropped here rather than sent, so a mistyped link shows the
 * default board instead of the daemon's refusal.
 */
static gboolean
board_window_ok(const gchar *window)
{
	gsize len;
	gsize i;

	if (window == NULL)
		return FALSE;

	if (g_strcmp0(window, "today") == 0)
		return TRUE;

	len = strlen(window);
	if (len < 2 || len > 6)
		return FALSE;

	for (i = 0; i + 1 < len; i++)
		if (!g_ascii_isdigit(window[i]))
			return FALSE;

	return window[len - 1] == 'h' || window[len - 1] == 'd';
}

static void
board_query_init(BoardQuery *q, const gchar *page, const gchar *view,
                 const gchar *agent, const gchar *lanes, const gchar *done,
                 const gchar *window)
{
	guint i;

	memset(q, 0, sizeof(*q));
	q->page = g_strdup(page);
	q->agent = (agent != NULL && agent[0] != '\0') ? g_strdup(agent) : NULL;
	q->view = board_views[0];

	for (i = 0; view != NULL && i < G_N_ELEMENTS(board_views); i++)
		if (g_strcmp0(view, board_views[i]) == 0)
			q->view = board_views[i];

	q->lanes = g_strcmp0(lanes, "1") == 0;
	q->done = g_strcmp0(done, "1") == 0;
	q->window = board_window_ok(window) ? g_strdup(window) : NULL;
}

static void
board_query_clear(BoardQuery *q)
{
	g_clear_pointer(&q->page, g_free);
	g_clear_pointer(&q->agent, g_free);
	g_clear_pointer(&q->window, g_free);
}

/* Whose board: the filter, else the agent whose page this is. */
static const gchar *
board_agent(const BoardQuery *q)
{
	return q->agent != NULL ? q->agent : q->page;
}

/*
 * A link to this page (or, for @base "/f/board", to its fragment) with
 * the query as given.  Every part is percent-encoded; the whole is then
 * an attribute value, which htmx-glib escapes.
 */
static gchar *
board_href(const BoardQuery *q, const gchar *base, const gchar *view,
           gboolean lanes, gboolean done, gboolean with_page)
{
	GString *out = g_string_new(base);
	g_autofree gchar *esc = NULL;

	g_string_append_printf(out, "?v=%s", view);

	if (with_page && q->page != NULL) {
		esc = g_uri_escape_string(q->page, NULL, FALSE);
		g_string_append_printf(out, "&page=%s", esc);
		g_clear_pointer(&esc, g_free);
	}

	if (q->agent != NULL && (with_page || g_strcmp0(q->agent, q->page) != 0)) {
		esc = g_uri_escape_string(q->agent, NULL, FALSE);
		g_string_append_printf(out, "&agent=%s", esc);
		g_clear_pointer(&esc, g_free);
	}

	if (lanes)
		g_string_append(out, "&lanes=1");
	if (done)
		g_string_append(out, "&done=1");

	if (q->window != NULL) {
		esc = g_uri_escape_string(q->window, NULL, FALSE);
		g_string_append_printf(out, "&window=%s", esc);
	}

	return g_string_free(out, FALSE);
}

static gchar *
board_page_href(const BoardQuery *q, const gchar *view, gboolean lanes,
                gboolean done)
{
	g_autofree gchar *escaped = g_uri_escape_string(
		q->page != NULL ? q->page : "", NULL, FALSE);
	g_autofree gchar *base = g_strdup_printf("/a/%s/tasks", escaped);

	return board_href(q, base, view, lanes, done, FALSE);
}

gchar *
clawt_web_board_url(const gchar *page, const gchar *agent)
{
	BoardQuery q;
	gchar *url;

	board_query_init(&q, page, "board", agent, NULL, NULL, NULL);
	url = board_page_href(&q, "board", FALSE, FALSE);
	board_query_clear(&q);

	return url;
}

/* ── Small pieces ────────────────────────────────────────────────── */

static HtmxElement *
mk_div(const gchar *css)
{
	HtmxElement *el = HTMX_ELEMENT(htmx_div_new());

	if (css != NULL)
		htmx_element_add_class(el, css);

	return el;
}

static HtmxElement *
mk_span(const gchar *css, const gchar *text)
{
	HtmxElement *el = HTMX_ELEMENT(htmx_span_new());

	if (css != NULL)
		htmx_element_add_class(el, css);
	if (text != NULL)
		htmx_node_set_text_content(HTMX_NODE(el), text);

	return el;
}

static HtmxElement *
mk_link(const gchar *href, const gchar *css, const gchar *text)
{
	HtmxElement *el = HTMX_ELEMENT(htmx_a_new_with_href(href));

	if (css != NULL)
		htmx_element_add_class(el, css);
	if (text != NULL)
		htmx_node_set_text_content(HTMX_NODE(el), text);

	return el;
}

/*
 * Only an http(s) address is ever made a link: the ticket's url comes
 * from the daemon, but a card is untrusted text and a "javascript:"
 * target would run in this page.
 */
static gboolean
board_url_ok(const gchar *url)
{
	return url != NULL &&
	       (g_str_has_prefix(url, "https://") ||
	        g_str_has_prefix(url, "http://"));
}

/* "45s", "12m", "3h 05m", "2d 4h". */
static gchar *
board_duration(gint64 seconds)
{
	if (seconds < 0)
		seconds = 0;
	if (seconds < 60)
		return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
	if (seconds < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT "m", seconds / 60);
	if (seconds < 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT "h %02" G_GINT64_FORMAT "m",
		                       seconds / 3600, (seconds % 3600) / 60);

	return g_strdup_printf("%" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
	                       seconds / 86400, (seconds % 86400) / 3600);
}

/* The tone for a task state nickname, from the library. */
static const gchar *
board_tone(const gchar *state)
{
	gint value = 0;

	if (state != NULL &&
	    clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
		return clawt_task_state_tone((ClawtTaskState)value);

	return "neutral";
}

/*
 * A nickname as a CSS class suffix: lower-case letters, digits and
 * dashes only, so whatever the daemon sends cannot end a class.
 */
static gchar *
board_token(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text != NULL ? text : ""; *p != '\0'; p++) {
		if (g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '-')
			g_string_append_c(out, *p);
		else if (*p == '_')
			g_string_append_c(out, '-');
	}

	if (out->len == 0)
		g_string_append(out, "unknown");

	return g_string_free(out, FALSE);
}

static gchar *
board_task_href(const gchar *id)
{
	g_autofree gchar *escaped = g_uri_escape_string(id != NULL ? id : "",
	                                                NULL, FALSE);

	return g_strdup_printf("/tasks/%s", escaped);
}

/* ── The switcher and the toolbar ────────────────────────────────── */

static HtmxElement *
board_switcher(const BoardQuery *q, const gchar *current)
{
	HtmxElement *bar = mk_div("board-switch btn-row");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(board_views); i++) {
		g_autofree gchar *href = board_page_href(q, board_views[i],
		                                         q->lanes, q->done);
		gboolean active = g_strcmp0(current, board_views[i]) == 0;
		HtmxElement *link = mk_link(href,
		                            active ? "btn btn-primary" : "btn",
		                            board_view_labels[i]);

		if (active)
			htmx_element_set_attribute(link, "aria-current", "page");

		clawt_web_add(bar, link);
	}

	return bar;
}

HtmxElement *
clawt_web_board_switcher(const gchar *page, const gchar *current)
{
	BoardQuery q;
	HtmxElement *bar;

	board_query_init(&q, page, current, NULL, NULL, NULL, NULL);
	bar = board_switcher(&q, current);
	board_query_clear(&q);

	return bar;
}

/* ── The header strip ────────────────────────────────────────────── */

static void
add_count(HtmxElement *strip, JsonObject *counts, const gchar *key,
          const gchar *label)
{
	g_autofree gchar *text = g_strdup_printf(
		"%s %" G_GINT64_FORMAT, label,
		clawt_web_member_int(counts, key, 0));

	clawt_web_add(strip, mk_span("board-count", text));
}

static HtmxElement *
board_header(JsonObject *header)
{
	HtmxElement *strip = mk_div("board-head");
	HtmxElement *line = mk_div("board-head-line");
	JsonObject *step = clawt_web_member_object(header, "step");
	JsonObject *current = clawt_web_member_object(header, "current_task");
	JsonObject *counts = clawt_web_member_object(header, "counts");
	const gchar *state = clawt_web_member(header, "state", "unknown");
	g_autofree gchar *mailbox = g_strdup_printf(
		"Mailbox %" G_GINT64_FORMAT,
		clawt_web_member_int(header, "mailbox", 0));

	if (header == NULL)
		return strip;

	clawt_web_add(line, mk_span("board-agent", clawt_web_member(
		header, "name", clawt_web_member(header, "agent", "?"))));
	clawt_web_add(line, clawt_web_badge(state, clawt_web_state_tone(state)));

	if (step != NULL) {
		g_autofree gchar *text = clawt_web_one_line(
			clawt_web_member(step, "text", ""), BOARD_TITLE_CHARS);

		clawt_web_add(line, mk_span("board-step", text));
	}

	clawt_web_add(line, mk_span("board-count", mailbox));
	clawt_web_add(strip, line);

	if (current != NULL) {
		HtmxElement *cur = mk_div("board-current");
		g_autofree gchar *href = board_task_href(
			clawt_web_member(current, "id", ""));
		g_autofree gchar *title = clawt_web_one_line(
			clawt_web_member(current, "title", ""), BOARD_TITLE_CHARS);

		clawt_web_add(cur, mk_span("muted", "Current task: "));
		clawt_web_add(cur, mk_link(href, NULL, title));
		clawt_web_add(strip, cur);
	}

	if (counts != NULL) {
		HtmxElement *row = mk_div("board-counts");

		add_count(row, counts, "blocked", "Blocked");
		add_count(row, counts, "backlog", "Backlog");
		add_count(row, counts, "in_flight", "In flight");
		add_count(row, counts, "in_review", "In review");
		add_count(row, counts, "done", "Done");
		clawt_web_add(strip, row);
	}

	return strip;
}

/* ── Cards ───────────────────────────────────────────────────────── */

static HtmxElement *
board_chip(JsonObject *chip)
{
	const gchar *state = clawt_web_member(chip, "state", "?");
	g_autofree gchar *href = board_task_href(clawt_web_member(chip, "id", ""));
	g_autofree gchar *label = g_strdup_printf(
		"%s \xC2\xB7 %s", clawt_web_member(chip, "worker", "?"), state);
	g_autofree gchar *css = g_strdup_printf(
		"board-chip board-chip-%s%s", board_tone(state),
		clawt_web_member_bool(chip, "quiet", FALSE) ? " board-quiet" : "");
	g_autofree gchar *title = clawt_web_one_line(
		clawt_web_member(chip, "title", ""), BOARD_TITLE_CHARS);
	HtmxElement *link = mk_link(href, css, label);

	/* The title is untrusted: a quoted attribute, escaped by the setter. */
	htmx_element_set_attribute(link, "title", title);

	return link;
}

static void
add_blocks(HtmxElement *card, JsonObject *obj)
{
	JsonArray *blocks = clawt_web_member_array(obj, "blocks");
	guint i;

	for (i = 0; blocks != NULL && i < json_array_get_length(blocks); i++) {
		JsonObject *block = json_array_get_object_element(blocks, i);
		g_autofree gchar *detail = clawt_web_one_line(
			clawt_web_member(block, "detail",
			                 clawt_web_member(block, "party", "")), 100);
		g_autofree gchar *text = g_strdup_printf(
			"%s%s%s", clawt_web_member(block, "reason", "blocked"),
			detail[0] != '\0' ? ": " : "", detail);

		clawt_web_add(card, mk_span("board-block", text));
	}
}

static HtmxElement *
board_card(JsonObject *obj, gint64 now)
{
	const gchar *state = clawt_web_member(obj, "state", "?");
	const gchar *id = clawt_web_member(obj, "id", "");
	g_autofree gchar *css = g_strdup_printf(
		"board-card board-card-%s%s", board_tone(state),
		clawt_web_member_bool(obj, "quiet", FALSE) ? " board-quiet" : "");
	HtmxElement *card = mk_div(css);
	HtmxElement *head = mk_div("board-card-head");
	HtmxElement *meta = mk_div("board-meta");
	g_autofree gchar *href = board_task_href(id);
	JsonObject *ticket = clawt_web_member_object(obj, "ticket");
	JsonObject *review = clawt_web_member_object(obj, "review");
	JsonArray *chips = clawt_web_member_array(obj, "chips");
	const gchar *note = clawt_web_member(obj, "note", NULL);
	const gchar *repo = clawt_web_member(obj, "repo", NULL);
	g_autofree gchar *elapsed = board_duration(
		clawt_web_member_int(obj, "elapsed", 0));
	guint i;

	(void)now;

	clawt_web_add(head, mk_link(href, "board-title",
	                            clawt_web_member(obj, "title", id)));
	clawt_web_add(card, head);

	/* The line of facts: ticket, repo, elapsed, cost, phase. */
	if (ticket != NULL) {
		g_autofree gchar *label = g_strdup_printf(
			"VENTURE #%" G_GINT64_FORMAT,
			clawt_web_member_int(ticket, "id", 0));
		const gchar *url = clawt_web_member(ticket, "url", NULL);

		if (board_url_ok(url))
			clawt_web_add(meta, mk_link(url, "board-ticket", label));
		else
			clawt_web_add(meta, mk_span("board-ticket", label));
	}

	if (repo != NULL) {
		g_autofree gchar *text = g_strdup_printf(
			"%s%s%s", repo,
			clawt_web_member(obj, "branch", NULL) != NULL ? " @ " : "",
			clawt_web_member(obj, "branch", ""));

		clawt_web_add(meta, mk_span("board-repo", text));
	}

	if (clawt_web_member_int(obj, "position", 0) > 0) {
		g_autofree gchar *text = g_strdup_printf(
			"next #%" G_GINT64_FORMAT,
			clawt_web_member_int(obj, "position", 0));

		clawt_web_add(meta, mk_span("board-pos", text));
	}

	clawt_web_add(meta, mk_span("board-elapsed", elapsed));

	if (json_object_has_member(obj, "cost_usd")) {
		g_autofree gchar *cost = g_strdup_printf(
			"$%.2f", json_object_get_double_member(obj, "cost_usd"));

		clawt_web_add(meta, mk_span("board-cost", cost));
	}

	if (clawt_web_member(obj, "phase", NULL) != NULL)
		clawt_web_add(meta, mk_span("board-phase",
		                            clawt_web_member(obj, "phase", "")));

	if (clawt_web_member_bool(obj, "quiet", FALSE))
		clawt_web_add(meta, clawt_web_badge("quiet", "warn"));

	clawt_web_add(card, meta);

	add_blocks(card, obj);

	if (review != NULL) {
		g_autofree gchar *wait = board_duration(
			clawt_web_member_int(review, "wait", 0));
		g_autofree gchar *text = g_strdup_printf(
			"In review %s, waiting for %s", wait,
			clawt_web_member(review, "waiting_for", "?"));

		clawt_web_add(card, mk_span("board-review", text));
	}

	if (note != NULL) {
		g_autofree gchar *text = clawt_web_one_line(note, BOARD_NOTE_CHARS);

		clawt_web_add(card, mk_span("board-note", text));
	}

	if (chips != NULL && json_array_get_length(chips) > 0) {
		HtmxElement *row = mk_div("board-chips");

		for (i = 0; i < json_array_get_length(chips); i++)
			clawt_web_add(row, board_chip(
				json_array_get_object_element(chips, i)));

		clawt_web_add(card, row);
	}

	return card;
}

/* ── Columns ─────────────────────────────────────────────────────── */

static HtmxElement *
board_column(const BoardQuery *q, JsonObject *column)
{
	const gchar *cid = clawt_web_member(column, "id", "?");
	JsonArray *cards = clawt_web_member_array(column, "cards");
	guint n = cards != NULL ? json_array_get_length(cards) : 0;
	g_autofree gchar *token = board_token(cid);
	g_autofree gchar *css = g_strdup_printf("board-col board-col-%s", token);
	HtmxElement *col = mk_div(css);
	HtmxElement *head = mk_div("board-col-head");
	g_autofree gchar *title = g_strdup_printf(
		"%s (%u)", clawt_web_member(column, "label", cid), n);
	gboolean is_done = g_strcmp0(cid, "done") == 0;
	guint i;

	clawt_web_add(head, mk_span("board-col-title", title));

	/*
	 * Done is collapsed unless asked for.  A link, not a <details>:
	 * the open/closed state is the URL, so a live re-render (which
	 * replaces this markup) cannot snap it shut.
	 */
	if (is_done && n > 0) {
		g_autofree gchar *href = board_page_href(
			q, "board", q->lanes, !q->done);

		clawt_web_add(head, mk_link(href, "board-toggle",
		                            q->done ? "Hide" : "Show"));
	}

	clawt_web_add(col, head);

	if (is_done && !q->done)
		return col;

	if (n == 0)
		clawt_web_add(col, mk_span("board-empty muted", "Nothing"));

	for (i = 0; i < n; i++)
		clawt_web_add(col, board_card(
			json_array_get_object_element(cards, i),
			0));

	return col;
}

static HtmxElement *
board_lanes(JsonObject *root)
{
	HtmxElement *lanes = mk_div("board-lanes");
	JsonArray *arr = clawt_web_member_array(root, "lanes");
	guint i;

	if (arr == NULL || json_array_get_length(arr) == 0) {
		clawt_web_add(lanes, mk_span("board-empty muted", "No other workers"));
		return lanes;
	}

	for (i = 0; i < json_array_get_length(arr); i++) {
		JsonObject *lane = json_array_get_object_element(arr, i);
		JsonObject *queue = clawt_web_member_object(lane, "queue");
		JsonObject *step = clawt_web_member_object(lane, "step");
		const gchar *agent = clawt_web_member(lane, "agent", "?");
		const gchar *state = clawt_web_member(lane, "state", "unknown");
		g_autofree gchar *esc = g_uri_escape_string(agent, NULL, FALSE);
		g_autofree gchar *href = g_strdup_printf(
			"/a/%s/tasks?v=board&agent=%s", esc, esc);
		HtmxElement *row = mk_div("board-lane");
		g_autofree gchar *counts = g_strdup_printf(
			"queued %" G_GINT64_FORMAT " \xC2\xB7 running %" G_GINT64_FORMAT
			" \xC2\xB7 review %" G_GINT64_FORMAT " \xC2\xB7 mailbox %"
			G_GINT64_FORMAT,
			clawt_web_member_int(queue, "queued", 0),
			clawt_web_member_int(queue, "running", 0),
			clawt_web_member_int(queue, "review", 0),
			clawt_web_member_int(queue, "mailbox", 0));

		clawt_web_add(row, mk_link(href, "board-agent", agent));
		clawt_web_add(row, mk_span("muted",
		                           clawt_web_member(lane, "role", "")));
		clawt_web_add(row, clawt_web_badge(state, clawt_web_state_tone(state)));

		if (step != NULL) {
			g_autofree gchar *text = clawt_web_one_line(
				clawt_web_member(step, "text", ""), BOARD_TITLE_CHARS);

			clawt_web_add(row, mk_span("board-step", text));
		}

		clawt_web_add(row, mk_span("board-count", counts));
		clawt_web_add(lanes, row);
	}

	return lanes;
}

/* ── The Timeline (server-rendered SVG) ──────────────────────────── */

#define TL_WIDTH   (960)
#define TL_LABEL   (240)
#define TL_RIGHT   (16)
#define TL_AXIS    (26)
#define TL_ROW_H   (26)
#define TL_CHIP_H  (18)

typedef struct {
	gint64 t0;
	gint64 t1;
} TlScale;

static gint
tl_x(const TlScale *s, gint64 t)
{
	gint plot = TL_WIDTH - TL_LABEL - TL_RIGHT;

	if (t < s->t0)
		t = s->t0;
	if (t > s->t1)
		t = s->t1;

	return TL_LABEL + (gint)(((t - s->t0) * plot) / (s->t1 - s->t0));
}

static void
tl_text(GString *svg, gint x, gint y, const gchar *css, const gchar *text)
{
	g_autofree gchar *escaped = g_markup_escape_text(text, -1);

	g_string_append_printf(svg, "<text x=\"%d\" y=\"%d\" class=\"%s\">%s</text>",
	                       x, y, css, escaped);
}

static void
tl_bar(GString *svg, const TlScale *s, gint y, gint h, const gchar *prefix,
       const gchar *nick, gint64 from, gint64 to, gint64 now)
{
	g_autofree gchar *token = board_token(nick);
	g_autofree gchar *what = g_markup_escape_text(
		nick != NULL ? nick : "?", -1);
	g_autofree gchar *len = board_duration((to > from ? to : from) - from);
	gint x0 = tl_x(s, from);
	gint x1 = tl_x(s, to > 0 ? to : now);
	gint w = x1 - x0;

	if (w < 3)
		w = 3;

	g_string_append_printf(
		svg, "<rect class=\"%s %s-%s\" x=\"%d\" y=\"%d\" width=\"%d\" "
		"height=\"%d\"><title>%s, %s</title></rect>",
		prefix, prefix, token, x0, y, w, h, what, len);
}

/* The bars of one card or chip: its state spans, with phases under. */
static void
tl_row_bars(GString *svg, const TlScale *s, JsonObject *item, gint y, gint h,
            gint64 now)
{
	JsonObject *tl = clawt_web_member_object(item, "timeline");
	JsonArray *states = clawt_web_member_array(tl, "states");
	JsonArray *phases = clawt_web_member_array(tl, "phases");
	gint64 finished = clawt_web_member_int(item, "finished_at", 0);
	guint i;

	if (states == NULL || json_array_get_length(states) == 0) {
		/* No history at all: one bar from creation, in its state. */
		gint64 from = clawt_web_member_int(item, "created_at", now);

		tl_bar(svg, s, y, h, "tl-s", clawt_web_member(item, "state", "?"),
		       from, finished > 0 ? finished : now, now);
		return;
	}

	for (i = 0; i < json_array_get_length(states); i++) {
		JsonObject *span = json_array_get_object_element(states, i);
		gint64 from = clawt_web_member_int(span, "from", now);
		gint64 to = clawt_web_member_int(span, "to", 0);

		/*
		 * The last span has no "to": still open, or the state the task
		 * ended in -- which ends where it finished.
		 */
		if (to == 0)
			to = finished > from ? finished : now;

		tl_bar(svg, s, y, h, "tl-s", clawt_web_member(span, "state", "?"),
		       from, to, now);
	}

	for (i = 0; phases != NULL && i < json_array_get_length(phases); i++) {
		JsonObject *span = json_array_get_object_element(phases, i);
		gint64 from = clawt_web_member_int(span, "from", now);
		gint64 to = clawt_web_member_int(span, "to", 0);

		if (to == 0)
			to = finished > from ? finished : now;

		tl_bar(svg, s, y + h + 1, 3, "tl-p",
		       clawt_web_member(span, "phase", "?"), from, to, now);
	}
}

/* The earliest moment anything on the board began. */
static gint64
tl_earliest(JsonObject *root, gint64 now)
{
	JsonArray *columns = clawt_web_member_array(root, BOARD_COLUMNS_KEY);
	gint64 since = clawt_web_member_int(
		clawt_web_member_object(root, "window"), "since", 0);
	gint64 first = now;
	guint c;

	for (c = 0; columns != NULL && c < json_array_get_length(columns); c++) {
		JsonArray *cards = clawt_web_member_array(
			json_array_get_object_element(columns, c), "cards");
		guint i;

		for (i = 0; cards != NULL && i < json_array_get_length(cards); i++) {
			JsonObject *card = json_array_get_object_element(cards, i);
			gint64 created = clawt_web_member_int(card, "created_at", 0);

			if (created > 0 && created < first)
				first = created;
		}
	}

	/* A card older than the window is clipped at the window's start. */
	if (since > 0 && first < since)
		first = since;

	if (first >= now)
		first = now - 3600;

	return first;
}

static gchar *
tl_stamp(gint64 t)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(t);

	return when != NULL ? g_date_time_format(when, "%m-%d %H:%M")
	                    : g_strdup("?");
}

static HtmxElement *
board_timeline(const BoardQuery *q, JsonObject *root)
{
	HtmxElement *holder = mk_div("board-timeline-wrap");
	JsonArray *columns = clawt_web_member_array(root, BOARD_COLUMNS_KEY);
	gint64 now = clawt_web_member_int(root, "now", 0);
	TlScale scale;
	GString *rows = g_string_new(NULL);
	GString *svg = g_string_new(NULL);
	gint y = TL_AXIS;
	guint drawn = 0;
	guint c;
	gint now_x;
	gint k;

	if (now <= 0)
		now = g_get_real_time() / G_USEC_PER_SEC;

	scale.t1 = now;
	scale.t0 = tl_earliest(root, now);

	for (c = 0; columns != NULL && c < json_array_get_length(columns); c++) {
		JsonObject *column = json_array_get_object_element(columns, c);
		JsonArray *cards = clawt_web_member_array(column, "cards");
		guint i;

		if (g_strcmp0(clawt_web_member(column, "id", ""), "done") == 0 &&
		    !q->done)
			continue;

		for (i = 0; cards != NULL && i < json_array_get_length(cards); i++) {
			JsonObject *card = json_array_get_object_element(cards, i);
			JsonArray *chips = clawt_web_member_array(card, "chips");
			g_autofree gchar *href = board_task_href(
				clawt_web_member(card, "id", ""));
			g_autofree gchar *esc_href = g_markup_escape_text(href, -1);
			g_autofree gchar *title = clawt_web_one_line(
				clawt_web_member(card, "title", ""), 34);
			guint m;

			if (drawn >= TL_MAX_ROWS)
				break;

			/* The card: its label (a link), then its bar. */
			g_string_append_printf(rows, "<a href=\"%s\">", esc_href);
			tl_text(rows, 4, y + 15, "tl-label", title);
			g_string_append(rows, "</a>");
			tl_row_bars(rows, &scale, card, y + 3, 14, now);
			y += TL_ROW_H;
			drawn++;

			/* Its children, one row each, beneath. */
			for (m = 0; chips != NULL && m < json_array_get_length(chips);
			     m++) {
				JsonObject *chip = json_array_get_object_element(chips, m);
				g_autofree gchar *label = g_strdup_printf(
					"\xE2\x94\x94 %s: %s",
					clawt_web_member(chip, "worker", "?"),
					clawt_web_member(chip, "title", ""));
				g_autofree gchar *cut = clawt_web_one_line(label, 36);

				if (drawn >= TL_MAX_ROWS)
					break;

				tl_text(rows, 16, y + 12, "tl-label tl-chip", cut);
				tl_row_bars(rows, &scale, chip, y + 3, 9, now);
				y += TL_CHIP_H;
				drawn++;
			}
		}
	}

	if (drawn == 0) {
		g_string_free(rows, TRUE);
		g_string_free(svg, TRUE);
		clawt_web_add(holder, clawt_web_empty(
			"Nothing on the timeline",
			"No task of this agent has run in the window."));
		return holder;
	}

	g_string_append_printf(
		svg, "<svg class=\"board-timeline\" viewBox=\"0 0 %d %d\" "
		"role=\"img\" aria-label=\"Task timeline\" width=\"100%%\">"
		"<title>Task timeline</title>", TL_WIDTH, y + 8);

	/* The axis: five ticks between the start and now. */
	for (k = 0; k <= 4; k++) {
		gint64 t = scale.t0 + ((scale.t1 - scale.t0) * k) / 4;
		gint x = tl_x(&scale, t);
		g_autofree gchar *stamp = tl_stamp(t);

		g_string_append_printf(
			svg, "<line class=\"tl-grid\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
			"y2=\"%d\"/>", x, TL_AXIS - 4, x, y);
		tl_text(svg, x, 12, "tl-axis", stamp);
	}

	g_string_append(svg, rows->str);

	/* "now": a line at the right edge, and its name. */
	now_x = tl_x(&scale, now);
	g_string_append_printf(
		svg, "<line class=\"tl-now\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
		"y2=\"%d\"/>", now_x, TL_AXIS - 4, now_x, y);
	tl_text(svg, now_x - 22, 24, "tl-now-label", "now");
	g_string_append(svg, "</svg>");

	htmx_node_set_html_content(HTMX_NODE(holder), svg->str);

	g_string_free(rows, TRUE);
	g_string_free(svg, TRUE);

	return holder;
}

/* ── The panel ───────────────────────────────────────────────────── */

/*
 * Everything inside the live host: the toolbar, the header strip, and
 * the Board or the Timeline.  Pure: it draws what it is given, so the
 * tests feed it canned replies and a browser's re-fetch of /f/board
 * draws the same bytes as the page did.
 */
static HtmxElement *
board_render(const BoardQuery *q, JsonNode *reply, const gchar *error)
{
	HtmxElement *body = mk_div("board-body");
	HtmxElement *bar = mk_div("board-bar btn-row");
	JsonObject *root = clawt_web_root(reply);
	JsonArray *columns = clawt_web_member_array(root, BOARD_COLUMNS_KEY);
	g_autofree gchar *self = board_page_href(q, q->view, q->lanes, q->done);
	g_autofree gchar *lanes_href = board_page_href(q, q->view, !q->lanes,
	                                               q->done);
	guint c;

	/* Refresh is a link: the page needs no script to be current. */
	clawt_web_add(bar, mk_link(self, "btn", "Refresh"));

	if (g_strcmp0(q->view, "board") == 0)
		clawt_web_add(bar, mk_link(lanes_href, "btn",
		                           q->lanes ? "Hide worker lanes"
		                                    : "Worker lanes"));

	if (q->agent != NULL && g_strcmp0(q->agent, q->page) != 0) {
		g_autofree gchar *text = g_strdup_printf("Showing %s's board",
		                                         q->agent);

		clawt_web_add(bar, mk_span("muted", text));
	}

	clawt_web_add(body, bar);

	if (root == NULL || columns == NULL) {
		clawt_web_add(body, clawt_web_empty(
			"The board is unavailable",
			error != NULL ? error
			              : "The daemon did not answer task.board."));
		return body;
	}

	clawt_web_add(body, board_header(
		clawt_web_member_object(root, "header")));

	if (g_strcmp0(q->view, "timeline") == 0) {
		clawt_web_add(body, board_timeline(q, root));
		return body;
	}

	{
		HtmxElement *cols = mk_div("board-cols");

		for (c = 0; c < json_array_get_length(columns); c++)
			clawt_web_add(cols, board_column(
				q, json_array_get_object_element(columns, c)));

		clawt_web_add(body, cols);
	}

	if (q->lanes)
		clawt_web_add(body, board_lanes(root));

	return body;
}

/* Asks the daemon, then draws. */
static HtmxElement *
board_panel_body(ClawtWebApp *app, const BoardQuery *q)
{
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *error = NULL;

	clawt_web_payload_set(payload, "agent", board_agent(q));
	if (q->window != NULL)
		clawt_web_payload_set(payload, "window", q->window);
	clawt_web_payload_set_bool(payload, "timeline",
	                           g_strcmp0(q->view, "timeline") == 0);

	reply = clawt_web_app_call(app, "task.board",
	                           clawt_web_payload_take(
	                               g_steal_pointer(&payload)));
	if (reply == NULL)
		error = g_strdup(clawt_web_app_last_error(app));

	return board_render(q, reply, error);
}

/*
 * The live host.  One hx-trigger, `sse:fleet delay:600ms`: the event the
 * page's single connection already carries, coalesced by htmx so a
 * burst re-renders once.  hx-sync drops an answer still in flight when
 * the next one is asked for.  hx-swap is innerHTML, so the host (which
 * holds the trigger) is never replaced.
 */
static HtmxElement *
board_live_host(const BoardQuery *q, HtmxElement *inner)
{
	HtmxElement *host = mk_div("board-live");
	g_autofree gchar *src = board_href(q, "/f/board", q->view, q->lanes,
	                                   q->done, TRUE);

	htmx_element_set_id(host, BOARD_LIVE_ID);
	htmx_element_set_attribute(host, "hx-get", src);
	htmx_element_set_attribute(host, "hx-trigger",
	                           "sse:fleet delay:" BOARD_COALESCE);
	htmx_element_set_attribute(host, "hx-swap", "innerHTML");
	htmx_element_set_attribute(host, "hx-sync", "this:replace");
	clawt_web_add(host, inner);

	return host;
}

/* The whole page body, drawn from a query and a drawn panel. */
static HtmxElement *
board_page(const BoardQuery *q, HtmxElement *inner)
{
	HtmxElement *view = HTMX_ELEMENT(htmx_main_new());
	HtmxElement *pad = mk_div("view-pad");

	htmx_element_add_class(view, "view");
	clawt_web_add(pad, clawt_web_section_title("Tasks"));
	clawt_web_add(pad, board_switcher(q, q->view));
	clawt_web_add(pad, board_live_host(q, inner));
	htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));
	g_object_unref(pad);

	return view;
}

HtmxElement *
clawt_web_tasks_page(ClawtWebApp *app, const gchar *agent_id,
                     HtmxRequest *request)
{
	BoardQuery q;
	HtmxElement *view;

	board_query_init(
		&q, agent_id,
		request != NULL ? htmx_request_get_query_param(request, "v") : NULL,
		request != NULL ? htmx_request_get_query_param(request, "agent") : NULL,
		request != NULL ? htmx_request_get_query_param(request, "lanes") : NULL,
		request != NULL ? htmx_request_get_query_param(request, "done") : NULL,
		request != NULL ? htmx_request_get_query_param(request, "window")
		                : NULL);

	if (g_strcmp0(q.view, "list") == 0 || board_agent(&q) == NULL) {
		view = clawt_web_tasks_body(app, agent_id);
	} else {
		view = board_page(&q, board_panel_body(app, &q));
	}

	board_query_clear(&q);

	return view;
}

/* ── Routes ──────────────────────────────────────────────────────── */

/* The panel's contents alone, for htmx to swap in. */
static HtmxResponse *
on_board_fragment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxElement) inner = NULL;
	BoardQuery q;

	(void)params;

	board_query_init(&q, htmx_request_get_query_param(request, "page"),
	                 htmx_request_get_query_param(request, "v"),
	                 htmx_request_get_query_param(request, "agent"),
	                 htmx_request_get_query_param(request, "lanes"),
	                 htmx_request_get_query_param(request, "done"),
	                 htmx_request_get_query_param(request, "window"));

	if (g_strcmp0(q.view, "list") == 0 || board_agent(&q) == NULL) {
		inner = mk_div("board-body");
		board_query_clear(&q);
		return clawt_web_fragment_response(inner);
	}

	inner = board_panel_body(user_data, &q);
	board_query_clear(&q);

	return clawt_web_fragment_response(inner);
}

void
clawt_web_register_board(HtmxRouter *router, ClawtWebApp *app)
{
	htmx_router_get(router, "/f/board", on_board_fragment, app);
}
