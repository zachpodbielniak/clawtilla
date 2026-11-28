/*
 * web-schedule.c - The Schedule page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Day, Week and Month grids and an Upcoming runs panel, asked of
 * routine.occurrences and routine.upcoming (VENTURE #35).  The windows,
 * the request bodies, the card text, the tones and the sentence a
 * refused window keeps all come from the lrg client's schedule model,
 * the same one the GTK and lrg pages use, so the three clients cannot
 * disagree about a card.
 *
 * The state is the URL:
 *
 *   ?view=day|week|month   which grid (week when absent)
 *   ?date=YYYY-MM-DD       the anchor day (today when absent)
 *   ?agent= ?team=         filters, passed to the daemon as they are
 *   ?disabled=1            include routines that are turned off
 *   ?more=DAY,DAY          days whose "+N more" is open
 *   ?expand=KEY,KEY        folded groups whose runs are open
 *
 * Every control is a plain link or a GET form, so the page works with
 * JavaScript off.  htmx only enhances it: the links also carry hx-get
 * for /f/schedule, which answers the panel alone, so the grid swaps
 * without a reload (and the address bar still moves, through
 * hx-push-url).  Every string the daemon supplies (titles, agent
 * names, details) reaches the page through htmx_node_set_text_content()
 * or htmx_element_set_attribute(), which escape.
 */

#include "web-pages.h"
#include "web-ui.h"
#include "../lrg/clawt-lrg-schedule-model.h"

#include <stdio.h>
#include <string.h>

/* The id the htmx links swap, and the card limit per day cell. */
#define SCHED_PANEL_ID    "schedule-panel"
#define SCHED_WEEK_CARDS  (8)
#define SCHED_MONTH_CARDS (3)

/* The three grids.  Upcoming is a panel beside them, not a view. */
static const gchar *sched_slugs[] = { "day", "week", "month" };

static const gchar *sched_days[] = {
	"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

static const gchar *sched_months[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const gchar *sched_month_names[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"
};

/* ── The query ───────────────────────────────────────────────────── */

/*
 * What the address says.  The strings are owned; a copy made for a link
 * (SchedQuery next = *q) borrows them and is never cleared.
 */
typedef struct {
	ClawtLrgSchedView  view;
	gboolean           has_date;
	gint               year;
	gint               month;
	gint               day;
	gchar             *agent;
	gchar             *team;
	gboolean           disabled;
	gchar             *more;
	gchar             *expand;
	gchar             *page_agent;
} SchedQuery;

/* Everything the panel is drawn from, so a test can supply it. */
typedef struct {
	JsonNode    *occurrences;   /* NULL when the window was refused */
	const gchar *occ_error;
	JsonNode    *upcoming;
	const gchar *up_error;
	JsonNode    *agents;        /* an agent.list reply, or NULL */
	JsonNode    *teams;         /* a team.list reply, or NULL */
} SchedData;

/* What every card needs besides its row. */
typedef struct {
	const SchedQuery *query;
	GHashTable       *agents;   /* id -> JsonObject, borrowed */
} SchedCtx;

static void
sched_query_clear(SchedQuery *q)
{
	g_clear_pointer(&q->agent, g_free);
	g_clear_pointer(&q->team, g_free);
	g_clear_pointer(&q->more, g_free);
	g_clear_pointer(&q->expand, g_free);
	g_clear_pointer(&q->page_agent, g_free);
}

/* A view's query word; Week when it names nothing. */
static ClawtLrgSchedView
sched_view_from_slug(const gchar *slug)
{
	guint i;

	for (i = 0; slug != NULL && i < G_N_ELEMENTS(sched_slugs); i++) {
		if (g_strcmp0(sched_slugs[i], slug) == 0)
			return (ClawtLrgSchedView)i;
	}

	return CLAWT_LRG_SCHED_WEEK;
}

/* YYYY-MM-DD into its parts.  %FALSE for anything that is not a date. */
static gboolean
sched_parse_date(const gchar *text, gint *year, gint *month, gint *day)
{
	gint y = 0;
	gint m = 0;
	gint d = 0;

	if (text == NULL || sscanf(text, "%d-%d-%d", &y, &m, &d) != 3)
		return FALSE;

	if (!g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y))
		return FALSE;

	*year = y;
	*month = m;
	*day = d;

	return TRUE;
}

/* A list parameter, or %NULL when it is empty or absurdly long. */
static gchar *
sched_list_param(const gchar *text)
{
	if (text == NULL || text[0] == '\0' || strlen(text) > 2048)
		return NULL;

	return g_strdup(text);
}

/*
 * The query from its parts, so the handlers and the tests build it the
 * same way.  A missing or impossible date is today in the viewer's zone:
 * the first request has no reply to name the daemon's yet.
 */
static void
sched_query_init(SchedQuery *q, const gchar *page_agent, const gchar *view,
                 const gchar *date, const gchar *agent, const gchar *team,
                 const gchar *disabled, const gchar *more,
                 const gchar *expand)
{
	memset(q, 0, sizeof(*q));

	q->view = sched_view_from_slug(view);
	q->has_date = sched_parse_date(date, &q->year, &q->month, &q->day);

	if (!q->has_date)
		clawt_lrg_schedule_today(NULL, &q->year, &q->month, &q->day);

	q->agent = (agent != NULL && agent[0] != '\0') ? g_strdup(agent) : NULL;
	q->team = (team != NULL && team[0] != '\0') ? g_strdup(team) : NULL;
	q->disabled = g_strcmp0(disabled, "1") == 0;
	q->more = sched_list_param(more);
	q->expand = sched_list_param(expand);
	q->page_agent = (page_agent != NULL && page_agent[0] != '\0')
	                ? g_strdup(page_agent) : NULL;
}

static void
sched_query_from_request(SchedQuery *q, HtmxRequest *request,
                         const gchar *page_agent)
{
	if (request == NULL) {
		sched_query_init(q, page_agent, NULL, NULL, NULL, NULL, NULL,
		                 NULL, NULL);
		return;
	}

	sched_query_init(
	    q, page_agent,
	    htmx_request_get_query_param(request, "view"),
	    htmx_request_get_query_param(request, "date"),
	    htmx_request_get_query_param(request, "agent"),
	    htmx_request_get_query_param(request, "team"),
	    htmx_request_get_query_param(request, "disabled"),
	    htmx_request_get_query_param(request, "more"),
	    htmx_request_get_query_param(request, "expand"));
}

/* ── Comma lists ─────────────────────────────────────────────────── */

static gboolean
sched_list_has(const gchar *list, const gchar *item)
{
	g_auto(GStrv) parts = NULL;
	guint i;

	if (list == NULL || list[0] == '\0' || item == NULL)
		return FALSE;

	parts = g_strsplit(list, ",", -1);

	for (i = 0; parts[i] != NULL; i++) {
		if (g_strcmp0(parts[i], item) == 0)
			return TRUE;
	}

	return FALSE;
}

/* @list with @item added, or taken out when it was there. */
static gchar *
sched_list_toggle(const gchar *list, const gchar *item)
{
	g_auto(GStrv) parts = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	gboolean found = FALSE;
	guint i;

	if (list != NULL && list[0] != '\0') {
		parts = g_strsplit(list, ",", -1);

		for (i = 0; parts[i] != NULL; i++) {
			if (parts[i][0] == '\0')
				continue;

			if (g_strcmp0(parts[i], item) == 0) {
				found = TRUE;
				continue;
			}

			g_string_append_printf(out, "%s%s", out->len > 0 ? "," : "",
			                       parts[i]);
		}
	}

	if (!found)
		g_string_append_printf(out, "%s%s", out->len > 0 ? "," : "",
		                       item);

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Addresses and links ─────────────────────────────────────────── */

static void
sched_append_param(GString *url, const gchar *name, const gchar *value)
{
	g_autofree gchar *escaped = NULL;

	if (value == NULL || value[0] == '\0')
		return;

	escaped = g_uri_escape_string(value, NULL, FALSE);
	g_string_append_printf(url, "&%s=%s", name, escaped);
}

/* The path with no query: the page itself, or the panel's fragment. */
static gchar *
sched_base(const SchedQuery *q, gboolean fragment)
{
	if (fragment)
		return g_strdup("/f/schedule");

	if (q->page_agent != NULL)
		return clawt_web_agent_url(q->page_agent, CLAWT_PAGE_SCHEDULE);

	return g_strdup("/schedule");
}

/* This query as an address: the page's, or the fragment's. */
static gchar *
sched_url(const SchedQuery *q, gboolean fragment)
{
	g_autofree gchar *base = sched_base(q, fragment);
	g_autoptr(GString) url = g_string_new(base);

	g_string_append_printf(url, "?view=%s", sched_slugs[q->view]);

	if (q->has_date)
		g_string_append_printf(url, "&date=%04d-%02d-%02d", q->year,
		                       q->month, q->day);

	sched_append_param(url, "agent", q->agent);
	sched_append_param(url, "team", q->team);

	if (q->disabled)
		g_string_append(url, "&disabled=1");

	sched_append_param(url, "more", q->more);
	sched_append_param(url, "expand", q->expand);

	/* the fragment cannot know whose page it is drawn on without this */
	if (fragment)
		sched_append_param(url, "page", q->page_agent);

	return g_string_free(g_steal_pointer(&url), FALSE);
}

/* htmx on top of a plain link: swap the panel, keep the address honest. */
static void
sched_enhance(HtmxElement *element, const gchar *page, const gchar *fragment)
{
	htmx_element_set_attribute(element, "hx-get", fragment);
	htmx_element_set_attribute(element, "hx-target", "#" SCHED_PANEL_ID);
	htmx_element_set_attribute(element, "hx-swap", "outerHTML");
	htmx_element_set_attribute(element, "hx-push-url", page);
}

/* A link to @q, as a button when @css says so; the current one marked. */
static HtmxA *
sched_link(const SchedQuery *q, const gchar *label, const gchar *css,
           gboolean current)
{
	g_autofree gchar *page = sched_url(q, FALSE);
	g_autofree gchar *fragment = sched_url(q, TRUE);
	g_autoptr(HtmxA) link = htmx_a_new_with_href(page);

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(link), css);

	if (current)
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current",
		                           "page");

	sched_enhance(HTMX_ELEMENT(link), page, fragment);
	htmx_node_set_text_content(HTMX_NODE(link), label);

	return (HtmxA *)g_steal_pointer(&link);
}

/* ── Dates ───────────────────────────────────────────────────────── */

/* "Fri 27 Nov". */
static gchar *
sched_date_label(const GDate *date)
{
	GDateWeekday weekday = g_date_get_weekday(date);
	guint month = (guint)g_date_get_month(date);

	return g_strdup_printf("%s %u %s",
	                       sched_days[((guint)weekday + 6) % 7],
	                       (guint)g_date_get_day(date),
	                       sched_months[(month + 11) % 12]);
}

/* The span the grid covers, in words. */
static gchar *
sched_range_text(const SchedQuery *q, const gchar *from)
{
	gint y = q->year;
	gint m = q->month;
	gint d = q->day;
	g_autoptr(GDate) first = NULL;
	g_autoptr(GDate) last = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *right = NULL;

	if (q->view == CLAWT_LRG_SCHED_MONTH)
		return g_strdup_printf("%s %d",
		                       sched_month_names[(m + 11) % 12], y);

	sched_parse_date(from, &y, &m, &d);
	first = g_date_new_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y);

	if (q->view == CLAWT_LRG_SCHED_DAY) {
		left = sched_date_label(first);

		return g_strdup_printf("%s %d", left, y);
	}

	last = g_date_new_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y);
	g_date_add_days(last, 6);
	left = sched_date_label(first);
	right = sched_date_label(last);

	return g_strdup_printf("%s \xe2\x80\x93 %s %d", left, right,
	                       (gint)g_date_get_year(last));
}

/* ── Cards ───────────────────────────────────────────────────────── */

/* The model's tone words, in the web client's badge vocabulary. */
static const gchar *
sched_badge_tone(const gchar *tone)
{
	if (g_strcmp0(tone, "success") == 0)
		return "good";

	if (g_strcmp0(tone, "error") == 0)
		return "bad";

	if (g_strcmp0(tone, "warning") == 0)
		return "warn";

	if (g_strcmp0(tone, "accent") == 0)
		return "info";

	return "neutral";
}

/*
 * The mark on a card: done, failed, skipped or running in words as well
 * as a colour.  A run that has not happened yet has none -- the card is
 * dimmed instead.
 */
static const gchar *
sched_mark_text(const ClawtLrgSchedRow *row)
{
	if (g_strcmp0(row->status, "upcoming") == 0)
		return NULL;

	if (g_strcmp0(row->status, "ok") == 0)
		return "\xe2\x9c\x93 done";

	if (g_strcmp0(row->status, "failed") == 0)
		return "\xe2\x9c\x97 failed";

	if (g_strcmp0(row->status, "skipped") == 0)
		return "\xe2\x80\x93 skipped";

	if (g_strcmp0(row->status, "running") == 0)
		return "\xe2\x97\x8f running";

	return row->status_label;
}

/* The agent's face, or %NULL for a row with no agent. */
static HtmxElement *
sched_face(const SchedCtx *ctx, const gchar *agent_id)
{
	JsonObject *agent = NULL;
	const gchar *name = agent_id;

	if (agent_id == NULL)
		return NULL;

	if (ctx->agents != NULL)
		agent = g_hash_table_lookup(ctx->agents, agent_id);

	if (agent != NULL)
		name = clawt_web_member(agent, "name", agent_id);

	return clawt_web_avatar(
	    name, agent_id, clawt_web_member_bool(agent, "has_avatar", FALSE),
	    clawt_web_member(agent, "color", NULL), "sched-face");
}

/*
 * Where a card goes.  A past run that made a task opens that run's page
 * (the task's); a future run of a routine opens the routines page.  Else
 * nowhere: a libreclaw cron, or a run that never started a task, has
 * only its tooltip.
 */
static gchar *
sched_card_href(const ClawtLrgSchedRow *row)
{
	if (row->action == CLAWT_LRG_SCHED_ACT_TASK && row->task != NULL) {
		g_autofree gchar *escaped = g_uri_escape_string(row->task, NULL,
		                                                FALSE);

		return g_strdup_printf("/tasks/%s", escaped);
	}

	if (row->action == CLAWT_LRG_SCHED_ACT_ROUTINE && row->agent != NULL)
		return clawt_web_agent_url(row->agent, CLAWT_PAGE_ROUTINES);

	return NULL;
}

static HtmxSpan *
sched_span(const gchar *text, const gchar *css)
{
	g_autoptr(HtmxSpan) span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), css);
	htmx_node_set_text_content(HTMX_NODE(span), text != NULL ? text : "");

	return (HtmxSpan *)g_steal_pointer(&span);
}

/* The key a folded group is opened by: routine and local day. */
static gchar *
sched_group_key(const ClawtLrgSchedRow *row)
{
	g_autofree gchar *routine = g_uri_escape_string(
	    row->routine != NULL ? row->routine : "", NULL, FALSE);

	return g_strdup_printf("%s@%s", routine,
	                       row->day != NULL ? row->day : "");
}

static HtmxElement *sched_card(const SchedCtx *ctx,
                               const ClawtLrgSchedRow *row,
                               gboolean show_day);

/*
 * One card: time, face, title and mark.  A folded day-group adds its
 * label and a link that opens the runs it folds, which is a link so it
 * works with no script.
 */
static HtmxElement *
sched_card(const SchedCtx *ctx, const ClawtLrgSchedRow *row,
           gboolean show_day)
{
	g_autoptr(HtmxDiv) card = htmx_div_new();
	g_autoptr(HtmxElement) line = NULL;
	g_autofree gchar *href = sched_card_href(row);
	g_autofree gchar *stamp = NULL;
	g_autofree gchar *status_class = NULL;
	const gchar *mark = sched_mark_text(row);
	HtmxElement *face;

	status_class = g_strdup_printf("sched-%s",
	                               row->status != NULL ? row->status
	                                                   : "unknown");
	htmx_element_add_class(HTMX_ELEMENT(card), "sched-card");
	htmx_element_add_class(HTMX_ELEMENT(card), status_class);

	/* what has not happened yet is dimmed, except in the upcoming list */
	if (!show_day && g_strcmp0(row->status, "upcoming") == 0)
		htmx_element_add_class(HTMX_ELEMENT(card), "sched-future");

	if (row->group)
		htmx_element_add_class(HTMX_ELEMENT(card), "sched-group");

	if (row->tooltip != NULL && row->tooltip[0] != '\0')
		htmx_element_set_attribute(HTMX_ELEMENT(card), "title",
		                           row->tooltip);

	if (href != NULL)
		line = HTMX_ELEMENT(htmx_a_new_with_href(href));
	else
		line = HTMX_ELEMENT(htmx_span_new());

	htmx_element_add_class(line, "sched-line");

	stamp = (show_day && row->day != NULL && row->day[0] != '\0')
	        ? g_strdup_printf("%s %s", row->day, row->when != NULL
	                                              ? row->when : "")
	        : g_strdup(row->when != NULL ? row->when : "");
	clawt_web_add(line, sched_span(stamp, "sched-time"));

	face = sched_face(ctx, row->agent);

	if (face != NULL)
		clawt_web_add(line, face);

	clawt_web_add(line, sched_span(row->title, "sched-title"));

	if (mark != NULL) {
		g_autoptr(HtmxSpan) badge = clawt_web_badge(
		    mark, sched_badge_tone(row->tone));

		htmx_element_add_class(HTMX_ELEMENT(badge), "sched-mark");
		htmx_node_add_child(HTMX_NODE(line), HTMX_NODE(badge));
	}

	htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(line));

	if (row->source_note != NULL)
		clawt_web_add(card, sched_span(row->source_note, "sched-note"));

	if (row->group) {
		g_autofree gchar *key = sched_group_key(row);
		gboolean open = sched_list_has(ctx->query->expand, key);
		SchedQuery next = *ctx->query;
		g_autofree gchar *toggled = sched_list_toggle(
		    ctx->query->expand, key);
		g_autofree gchar *detail = g_strdup_printf(
		    "%s%s%s", row->summary != NULL ? row->summary : "",
		    row->counts != NULL ? " \xc2\xb7 " : "",
		    row->counts != NULL ? row->counts : "");
		g_autofree gchar *label = open
		    ? g_strdup("Hide runs")
		    : g_strdup_printf("Show %u runs", row->count);

		next.expand = toggled;
		clawt_web_add(card, sched_span(detail, "sched-sub"));
		clawt_web_add(card, sched_link(&next, label, "sched-expand",
		                               FALSE));

		if (open && row->children != NULL) {
			g_autoptr(HtmxDiv) runs = htmx_div_new();
			guint i;

			htmx_element_add_class(HTMX_ELEMENT(runs), "sched-runs");

			for (i = 0; i < row->children->len; i++)
				clawt_web_add(runs, sched_card(
				    ctx, g_ptr_array_index(row->children, i), FALSE));

			htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(runs));
		}
	}

	return HTMX_ELEMENT(g_steal_pointer(&card));
}

/* ── The grid ────────────────────────────────────────────────────── */

/*
 * One day: its heading (a link to that day, except in the Day view),
 * its cards up to the limit and, past it, "+N more" that opens the day.
 */
static HtmxElement *
sched_day_cell(const SchedCtx *ctx, const GDate *date, const gchar *key,
               GPtrArray *cards, gboolean outside, gboolean today)
{
	const SchedQuery *q = ctx->query;
	g_autoptr(HtmxDiv) cell = htmx_div_new();
	g_autoptr(HtmxHeading) head = htmx_heading_new(3);
	g_autofree gchar *label = sched_date_label(date);
	guint limit = 0;
	guint shown;
	guint i;
	gboolean expanded = sched_list_has(q->more, key);

	htmx_element_add_class(HTMX_ELEMENT(cell), "sched-day");

	if (today)
		htmx_element_add_class(HTMX_ELEMENT(cell), "sched-today");

	if (outside)
		htmx_element_add_class(HTMX_ELEMENT(cell), "sched-outside");

	htmx_element_add_class(HTMX_ELEMENT(head), "sched-day-head");

	if (q->view == CLAWT_LRG_SCHED_DAY) {
		htmx_node_set_text_content(HTMX_NODE(head), label);
	} else {
		SchedQuery next = *q;

		next.view = CLAWT_LRG_SCHED_DAY;
		next.has_date = TRUE;
		next.year = (gint)g_date_get_year(date);
		next.month = (gint)g_date_get_month(date);
		next.day = (gint)g_date_get_day(date);
		next.more = NULL;
		clawt_web_add(head, sched_link(&next, label, NULL, FALSE));
	}

	htmx_node_add_child(HTMX_NODE(cell), HTMX_NODE(head));

	if (q->view == CLAWT_LRG_SCHED_WEEK)
		limit = SCHED_WEEK_CARDS;
	else if (q->view == CLAWT_LRG_SCHED_MONTH)
		limit = SCHED_MONTH_CARDS;

	shown = (cards != NULL) ? cards->len : 0;

	if (limit > 0 && shown > limit && !expanded)
		shown = limit;

	for (i = 0; i < shown; i++)
		clawt_web_add(cell, sched_card(ctx, g_ptr_array_index(cards, i),
		                               FALSE));

	if (limit > 0 && cards != NULL && cards->len > limit) {
		SchedQuery next = *q;
		g_autofree gchar *toggled = sched_list_toggle(q->more, key);
		g_autofree gchar *text = expanded
		    ? g_strdup("Show fewer")
		    : g_strdup_printf("+%u more", cards->len - limit);

		next.more = toggled;
		clawt_web_add(cell, sched_link(&next, text, "sched-more", FALSE));
	}

	return HTMX_ELEMENT(g_steal_pointer(&cell));
}

/*
 * The days of the window, each with its cards.  @from is the window the
 * daemon was asked for, so the grid starts where the request did.
 */
static HtmxElement *
sched_grid(const SchedCtx *ctx, GPtrArray *rows, const gchar *from,
           const gchar *zone)
{
	const SchedQuery *q = ctx->query;
	g_autoptr(HtmxDiv) grid = htmx_div_new();
	g_autoptr(GHashTable) by_day = g_hash_table_new_full(
	    g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	g_autoptr(GDate) cursor = NULL;
	g_autofree gchar *css = NULL;
	gint start_y = q->year;
	gint start_m = q->month;
	gint start_d = q->day;
	gint today_y;
	gint today_m;
	gint today_d;
	guint count = 1;
	guint i;

	if (q->view == CLAWT_LRG_SCHED_WEEK)
		count = 7;
	else if (q->view == CLAWT_LRG_SCHED_MONTH)
		count = 42;

	sched_parse_date(from, &start_y, &start_m, &start_d);
	cursor = g_date_new_dmy((GDateDay)start_d, (GDateMonth)start_m,
	                        (GDateYear)start_y);
	clawt_lrg_schedule_today(zone, &today_y, &today_m, &today_d);

	/* the rows by local day, still in time order */
	for (i = 0; i < rows->len; i++) {
		const ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);
		GPtrArray *list;

		if (row->day == NULL || row->day[0] == '\0')
			continue;

		list = g_hash_table_lookup(by_day, row->day);

		if (list == NULL) {
			list = g_ptr_array_new();
			g_hash_table_insert(by_day, g_strdup(row->day), list);
		}

		g_ptr_array_add(list, (gpointer)row);
	}

	css = g_strdup_printf("sched-grid-%s", sched_slugs[q->view]);
	htmx_element_add_class(HTMX_ELEMENT(grid), "sched-grid");
	htmx_element_add_class(HTMX_ELEMENT(grid), css);

	for (i = 0; i < count; i++) {
		gint y = (gint)g_date_get_year(cursor);
		gint m = (gint)g_date_get_month(cursor);
		gint d = (gint)g_date_get_day(cursor);
		g_autofree gchar *key = g_strdup_printf("%04d-%02d-%02d", y, m, d);

		clawt_web_add(grid, sched_day_cell(
		    ctx, cursor, key, g_hash_table_lookup(by_day, key),
		    q->view == CLAWT_LRG_SCHED_MONTH && m != q->month,
		    y == today_y && m == today_m && d == today_d));
		g_date_add_days(cursor, 1);
	}

	return HTMX_ELEMENT(g_steal_pointer(&grid));
}

/* ── The controls ────────────────────────────────────────────────── */

/* View switcher, Previous / Today / Next, and the disabled toggle. */
static HtmxElement *
sched_bar(const SchedQuery *q)
{
	g_autoptr(HtmxDiv) bar = htmx_div_new();
	gint today_y;
	gint today_m;
	gint today_d;
	guint i;
	SchedQuery prev = *q;
	SchedQuery next = *q;
	SchedQuery today = *q;
	SchedQuery toggle = *q;

	htmx_element_add_class(HTMX_ELEMENT(bar), "sched-bar");

	for (i = 0; i < G_N_ELEMENTS(sched_slugs); i++) {
		SchedQuery pick = *q;

		pick.view = (ClawtLrgSchedView)i;
		pick.more = NULL;
		clawt_web_add(bar, sched_link(
		    &pick, clawt_lrg_schedule_view_label((ClawtLrgSchedView)i),
		    "btn", (ClawtLrgSchedView)i == q->view));
	}

	clawt_lrg_schedule_today(NULL, &today_y, &today_m, &today_d);

	/* the same anchor, moved: a link each, so no script is needed */
	clawt_lrg_schedule_move(q->view, -1, today_y, today_m, today_d,
	                        &prev.year, &prev.month, &prev.day);
	clawt_lrg_schedule_move(q->view, 1, today_y, today_m, today_d,
	                        &next.year, &next.month, &next.day);
	prev.has_date = TRUE;
	next.has_date = TRUE;
	prev.more = NULL;
	next.more = NULL;
	today.has_date = FALSE;
	today.more = NULL;

	clawt_web_add(bar, sched_link(&prev, "Previous", "btn", FALSE));
	clawt_web_add(bar, sched_link(&today, "Today", "btn", FALSE));
	clawt_web_add(bar, sched_link(&next, "Next", "btn", FALSE));

	toggle.disabled = !q->disabled;
	clawt_web_add(bar, sched_link(
	    &toggle, q->disabled ? "Hide disabled" : "Show disabled", "btn",
	    FALSE));

	return HTMX_ELEMENT(g_steal_pointer(&bar));
}

/* A select's values and labels, "All" first, from a list reply. */
static void
sched_choices(JsonNode *reply, const gchar *key, const gchar *all,
              GPtrArray *values, GPtrArray *labels)
{
	JsonArray *list = clawt_web_member_array(clawt_web_root(reply), key);
	guint i;

	g_ptr_array_add(values, g_strdup(""));
	g_ptr_array_add(labels, g_strdup(all));

	for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
		JsonObject *one = json_array_get_object_element(list, i);
		const gchar *id = clawt_web_member(one, "id", NULL);

		if (id == NULL)
			continue;

		g_ptr_array_add(values, g_strdup(id));
		g_ptr_array_add(labels,
		                g_strdup(clawt_web_member(one, "name", id)));
	}

	g_ptr_array_add(values, NULL);
	g_ptr_array_add(labels, NULL);
}

/*
 * The agent and team filters, as a GET form so the page filters with
 * JavaScript off.  The window and the disabled toggle ride along as
 * hidden fields; the open "+N more" and groups do not, because they
 * name days and groups of the old result.
 */
static HtmxElement *
sched_filters(const SchedQuery *q, const SchedData *data)
{
	g_autoptr(HtmxForm) form = htmx_form_new();
	g_autoptr(GPtrArray) agent_values = g_ptr_array_new_with_free_func(
	    g_free);
	g_autoptr(GPtrArray) agent_labels = g_ptr_array_new_with_free_func(
	    g_free);
	g_autoptr(GPtrArray) team_values = g_ptr_array_new_with_free_func(
	    g_free);
	g_autoptr(GPtrArray) team_labels = g_ptr_array_new_with_free_func(
	    g_free);
	g_autoptr(HtmxButton) apply = htmx_button_new_submit();
	g_autofree gchar *action = sched_base(q, FALSE);
	g_autofree gchar *date = NULL;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "sched-filters");

	sched_choices(data->agents, "agents", "All agents", agent_values,
	              agent_labels);
	sched_choices(data->teams, "teams", "All teams", team_values,
	              team_labels);

	clawt_web_add(form, clawt_web_hidden("view", sched_slugs[q->view]));

	if (q->has_date) {
		date = g_strdup_printf("%04d-%02d-%02d", q->year, q->month,
		                       q->day);
		clawt_web_add(form, clawt_web_hidden("date", date));
	}

	if (q->disabled)
		clawt_web_add(form, clawt_web_hidden("disabled", "1"));

	clawt_web_add(form, clawt_web_select_field(
	    "Agent", "agent", (const gchar *const *)agent_values->pdata,
	    (const gchar *const *)agent_labels->pdata, q->agent));
	clawt_web_add(form, clawt_web_select_field(
	    "Team", "team", (const gchar *const *)team_values->pdata,
	    (const gchar *const *)team_labels->pdata, q->team));

	htmx_element_add_class(HTMX_ELEMENT(apply), "btn");
	htmx_node_set_text_content(HTMX_NODE(apply), "Filter");
	htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(apply));

	return HTMX_ELEMENT(g_steal_pointer(&form));
}

/* ── The panel ───────────────────────────────────────────────────── */

/* libreclaw jobs that are not on the grid, and why. */
static HtmxElement *
sched_unscheduled(const SchedCtx *ctx, GPtrArray *rows)
{
	g_autoptr(HtmxDiv) box = htmx_div_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(box), "sched-unscheduled");
	clawt_web_add(box, clawt_web_section_title("Not on the calendar"));

	for (i = 0; i < rows->len; i++) {
		const ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);
		g_autoptr(HtmxDiv) item = htmx_div_new();
		HtmxElement *face = sched_face(ctx, row->agent);

		htmx_element_add_class(HTMX_ELEMENT(item), "sched-card");
		htmx_element_add_class(HTMX_ELEMENT(item), "sched-unknown");

		if (face != NULL)
			clawt_web_add(item, face);

		clawt_web_add(item, sched_span(row->title, "sched-title"));
		clawt_web_add(item, sched_span(row->source_note, "sched-note"));
		clawt_web_add(item, sched_span(row->status_label, "sched-sub"));

		if (row->tooltip != NULL)
			clawt_web_add(item, sched_span(row->tooltip, "sched-sub"));

		htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(item));
	}

	return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* The next runs, from routine.upcoming. */
static HtmxElement *
sched_upcoming(const SchedCtx *ctx, const SchedData *data)
{
	g_autoptr(HtmxElement) aside = HTMX_ELEMENT(htmx_div_new());
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	htmx_element_add_class(aside, "sched-upcoming");
	clawt_web_add(aside, clawt_web_section_title("Upcoming runs"));

	if (data->up_error != NULL) {
		clawt_web_add(aside, clawt_web_notice(data->up_error, "bad"));

		return g_steal_pointer(&aside);
	}

	rows = clawt_lrg_schedule_rows(data->upcoming);

	if (rows->len == 0)
		clawt_web_add(aside, clawt_web_text(
		    "No run is scheduled.", "sched-sub"));

	for (i = 0; i < rows->len; i++)
		clawt_web_add(aside, sched_card(
		    ctx, g_ptr_array_index(rows, i), TRUE));

	return g_steal_pointer(&aside);
}

/*
 * The whole panel: controls, the range and its zone, filters, a refusal
 * if there was one, the grid, and the upcoming runs beside it.  It is
 * what /f/schedule returns and what the page wraps.
 */
static HtmxElement *
sched_render_panel(const SchedQuery *q, const SchedData *data)
{
	g_autoptr(HtmxDiv) panel = htmx_div_new();
	g_autoptr(HtmxDiv) layout = htmx_div_new();
	g_autoptr(HtmxDiv) main_col = htmx_div_new();
	g_autoptr(GHashTable) agents = g_hash_table_new(g_str_hash,
	                                                g_str_equal);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) loose = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *self_url = sched_url(q, TRUE);
	g_autofree gchar *range = NULL;
	g_autofree gchar *zone_line = NULL;
	JsonArray *agent_list;
	JsonObject *occ_root;
	const gchar *zone = NULL;
	SchedCtx ctx;
	guint i;

	/* the agents by id, so a card can draw its face */
	agent_list = clawt_web_member_array(clawt_web_root(data->agents),
	                                    "agents");

	for (i = 0; agent_list != NULL && i < json_array_get_length(agent_list);
	     i++) {
		JsonObject *one = json_array_get_object_element(agent_list, i);
		const gchar *id = clawt_web_member(one, "id", NULL);

		if (id == NULL)
			continue;

		clawt_web_face_note(id, clawt_web_member(one, "face", NULL));
		g_hash_table_insert(agents, (gpointer)id, one);
	}

	ctx.query = q;
	ctx.agents = agents;

	clawt_lrg_schedule_range(q->view, q->year, q->month, q->day, &from,
	                         &to);
	range = sched_range_text(q, from);

	htmx_element_set_id(HTMX_ELEMENT(panel), SCHED_PANEL_ID);
	htmx_element_add_class(HTMX_ELEMENT(panel), "sched");

	/* ask again each minute while the tab is open, as Fleet does */
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", self_url);
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
	                           "every 60s [!document.hidden]");
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap",
	                           "outerHTML");

	clawt_web_add(panel, sched_bar(q));
	clawt_web_add(panel, clawt_web_text(range, "sched-range"));

	/* the zone of whichever answer came back; the daemon's, not ours */
	zone_line = clawt_lrg_schedule_zone_line(
	    data->occurrences != NULL ? data->occurrences : data->upcoming);
	clawt_web_add(panel, clawt_web_text(zone_line, "sched-zone"));

	clawt_web_add(panel, sched_filters(q, data));

	htmx_element_add_class(HTMX_ELEMENT(layout), "sched-layout");
	htmx_element_add_class(HTMX_ELEMENT(main_col), "sched-main");

	if (data->occurrences == NULL) {
		/*
		 * A refused window keeps the daemon's sentence, whole: it names
		 * the bound that was hit, and a shorter one hides which.
		 */
		clawt_web_add(main_col, clawt_web_notice(
		    data->occ_error != NULL ? data->occ_error
		                            : "The daemon did not answer.",
		    "bad"));
	} else {
		occ_root = clawt_web_root(data->occurrences);

		if (occ_root != NULL)
			zone = clawt_web_member(occ_root, "time_zone", NULL);

		rows = clawt_lrg_schedule_rows(data->occurrences);
		clawt_web_add(main_col, sched_grid(&ctx, rows, from, zone));

		if (rows->len == 0)
			clawt_web_add(main_col, clawt_web_text(
			    "Nothing is scheduled in this window.", "sched-sub"));

		loose = clawt_lrg_schedule_unscheduled(data->occurrences);

		if (loose->len > 0)
			clawt_web_add(main_col, sched_unscheduled(&ctx, loose));
	}

	htmx_node_add_child(HTMX_NODE(layout), HTMX_NODE(main_col));
	clawt_web_add(layout, sched_upcoming(&ctx, data));
	htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(layout));

	return HTMX_ELEMENT(g_steal_pointer(&panel));
}

/* ── Asking the daemon ───────────────────────────────────────────── */

/*
 * Both reads, and the lists the filters and faces need, then the panel.
 * The window is always asked for with collapse, so an hourly routine is
 * one folded card a day with a link to open it.
 */
static HtmxElement *
sched_panel(ClawtWebApp *app, const SchedQuery *q)
{
	g_autoptr(JsonNode) occ = NULL;
	g_autoptr(JsonNode) up = NULL;
	g_autoptr(JsonNode) agents = NULL;
	g_autoptr(JsonNode) teams = NULL;
	g_autoptr(GError) occ_err = NULL;
	g_autoptr(GError) up_err = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *occ_text = NULL;
	g_autofree gchar *up_text = NULL;
	SchedData data;

	clawt_lrg_schedule_range(q->view, q->year, q->month, q->day, &from,
	                         &to);

	occ = clawt_web_app_request(
	    app, "routine.occurrences",
	    clawt_lrg_schedule_occurrences_payload(from, to, q->agent, q->team,
	                                           q->disabled, TRUE),
	    &occ_err);
	occ_text = clawt_lrg_schedule_error_text(occ_err);

	up = clawt_web_app_request(
	    app, "routine.upcoming",
	    clawt_lrg_schedule_upcoming_payload(q->agent, q->team), &up_err);
	up_text = clawt_lrg_schedule_error_text(up_err);

	agents = clawt_web_app_call(app, "agent.list", NULL);
	teams = clawt_web_app_call(app, "team.list", NULL);

	memset(&data, 0, sizeof(data));
	data.occurrences = occ;
	data.occ_error = occ_text;
	data.upcoming = up;
	data.up_error = (up == NULL && up_text == NULL)
	                ? "The daemon did not answer." : up_text;
	data.agents = agents;
	data.teams = teams;

	return sched_render_panel(q, &data);
}

HtmxElement *
clawt_web_schedule_body(ClawtWebApp *app, const gchar *agent_id,
                        HtmxRequest *request)
{
	g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
	g_autoptr(HtmxDiv) pad = htmx_div_new();
	SchedQuery q;

	sched_query_from_request(&q, request, agent_id);

	htmx_element_add_class(view, "view");
	htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
	clawt_web_add(pad, clawt_web_section_title("Schedule"));
	clawt_web_add(pad, sched_panel(app, &q));
	htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

	sched_query_clear(&q);

	return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

/* The panel alone, for htmx to swap. */
static HtmxResponse *
on_schedule_fragment(HtmxRequest *request, GHashTable *params,
                     gpointer user_data)
{
	g_autoptr(HtmxElement) panel = NULL;
	SchedQuery q;

	(void)params;

	sched_query_from_request(&q, request,
	                         htmx_request_get_query_param(request, "page"));
	panel = sched_panel(user_data, &q);
	sched_query_clear(&q);

	return clawt_web_fragment_response(panel);
}

/*
 * /schedule on its own: the Schedule page of the first agent, because
 * every tab lives under one.  The query is carried across.
 */
static HtmxResponse *
on_schedule_root(HtmxRequest *request, GHashTable *params,
                 gpointer user_data)
{
	g_autoptr(JsonNode) reply = clawt_web_app_call(user_data,
	                                               "agent.list", NULL);
	JsonArray *agents = clawt_web_member_array(clawt_web_root(reply),
	                                           "agents");
	JsonObject *first = NULL;
	const gchar *id = NULL;
	const gchar *query = htmx_request_get_query(request);
	g_autofree gchar *base = NULL;
	g_autofree gchar *where = NULL;

	(void)params;

	if (agents != NULL && json_array_get_length(agents) > 0)
		first = json_array_get_object_element(agents, 0);

	if (first != NULL)
		id = clawt_web_member(first, "id", NULL);

	if (id == NULL)
		return clawt_web_redirect(request, "/");

	base = clawt_web_agent_url(id, CLAWT_PAGE_SCHEDULE);
	where = (query != NULL && query[0] != '\0')
	        ? g_strdup_printf("%s?%s", base, query) : g_strdup(base);

	return clawt_web_redirect(request, where);
}

void
clawt_web_register_schedule(HtmxRouter *router, ClawtWebApp *app)
{
	htmx_router_get(router, "/f/schedule", on_schedule_fragment, app);
	htmx_router_get(router, "/schedule", on_schedule_root, app);
}
