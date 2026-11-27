/*
 * web-schedule.c - The Schedule page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Day, week, month and upcoming runs, asked of routine.occurrences and
 * routine.upcoming.  The windows, the request bodies, the card text,
 * the tones and the sentence a refused window keeps all come from the
 * lrg client's schedule model, the same one the GTK and lrg pages use,
 * so the three clients cannot disagree about a card.
 *
 * The state is the URL: ?view=day|week|month|upcoming, ?date=YYYY-MM-DD,
 * ?agent= and ?disabled=1.  Every control is a plain link, so the page
 * needs no script.
 */

#include "web-pages.h"
#include "web-ui.h"
#include "clawt-lrg-schedule-model.h"

#include <stdio.h>
#include <string.h>

/* The model's tone words, in the web client's badge vocabulary. */
static const gchar *
badge_tone(const gchar *tone)
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

/* A view's name as it appears in the query. */
static gchar *
view_slug(ClawtLrgSchedView view)
{
	return g_ascii_strdown(clawt_lrg_schedule_view_label(view), -1);
}

/* The view a query word names; Day when it names nothing. */
static ClawtLrgSchedView
view_from_slug(const gchar *slug)
{
	guint i;

	for (i = 0; slug != NULL && i < clawt_lrg_schedule_view_count(); i++) {
		g_autofree gchar *name = view_slug((ClawtLrgSchedView)i);

		if (g_strcmp0(name, slug) == 0)
			return (ClawtLrgSchedView)i;
	}

	return CLAWT_LRG_SCHED_DAY;
}

/* YYYY-MM-DD into its parts.  %FALSE for anything that is not a date. */
static gboolean
parse_date(const gchar *text, gint *year, gint *month, gint *day)
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

/* One link back to this page, with the state it should show. */
static gchar *
page_url(const gchar *agent_id, ClawtLrgSchedView view, gboolean with_date,
         gint year, gint month, gint day, const gchar *filter,
         gboolean disabled)
{
	g_autofree gchar *base = clawt_web_agent_url(agent_id,
	                                             CLAWT_PAGE_SCHEDULE);
	g_autofree gchar *slug = view_slug(view);
	g_autoptr(GString) url = g_string_new(base);

	g_string_append_printf(url, "?view=%s", slug);

	if (with_date)
		g_string_append_printf(url, "&date=%04d-%02d-%02d", year, month,
		                       day);

	if (filter != NULL && filter[0] != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(filter, NULL,
		                                                FALSE);

		g_string_append_printf(url, "&agent=%s", escaped);
	}

	if (disabled)
		g_string_append(url, "&disabled=1");

	return g_string_free(g_steal_pointer(&url), FALSE);
}

/* A link styled as a button; the current one is marked. */
static HtmxA *
nav_link(const gchar *label, const gchar *url, gboolean current)
{
	g_autoptr(HtmxA) link = htmx_a_new_with_href(url);

	htmx_element_add_class(HTMX_ELEMENT(link), "btn");

	if (current)
		htmx_element_add_class(HTMX_ELEMENT(link), "primary");

	htmx_node_set_text_content(HTMX_NODE(link), label);

	return (HtmxA *)g_steal_pointer(&link);
}

/* One card.  A collapsed day-group lists the runs it folds beneath it. */
static HtmxDiv *
row_card(const gchar *agent_id, const ClawtLrgSchedRow *row)
{
	g_autoptr(HtmxDiv) card = NULL;
	HtmxElement *body;
	g_autofree gchar *subtitle = NULL;

	if (row->group)
		subtitle = g_strdup_printf("%s%s%s",
		    row->summary != NULL ? row->summary : "",
		    row->counts != NULL ? " - " : "",
		    row->counts != NULL ? row->counts : "");
	else
		subtitle = g_strdup_printf("%s - %s", row->when, row->agent);

	card = clawt_web_card(row->title, subtitle);
	body = clawt_web_card_body(card);

	if (!row->group) {
		g_autoptr(HtmxSpan) badge = clawt_web_badge(
		    row->status_label, badge_tone(row->tone));

		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(badge));

		if (row->source_note != NULL)
			clawt_web_add(body, clawt_web_text(row->source_note,
			                                   "check-sub"));

		if (row->tooltip != NULL && row->tooltip[0] != '\0')
			clawt_web_add(body, clawt_web_text(row->tooltip,
			                                   "check-sub"));

		if (row->task != NULL)
			clawt_web_add(body, clawt_web_row("Task", row->task));

		if (row->action == CLAWT_LRG_SCHED_ACT_ROUTINE &&
		    row->routine != NULL) {
			g_autofree gchar *url = clawt_web_agent_url(
			    agent_id, CLAWT_PAGE_ROUTINES);
			g_autoptr(HtmxA) open = htmx_a_new_with_href(url);

			htmx_node_set_text_content(HTMX_NODE(open), "Open routines");
			htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(open));
		}
	} else if (row->children != NULL) {
		guint i;

		for (i = 0; i < row->children->len; i++) {
			const ClawtLrgSchedRow *child = g_ptr_array_index(
			    row->children, i);
			g_autofree gchar *line = g_strdup_printf(
			    "%s - %s", child->when, child->status_label);

			clawt_web_add(body, clawt_web_row(child->title, line));
		}
	}

	return (HtmxDiv *)g_steal_pointer(&card);
}

/* The cards, under a heading each time the local day changes. */
static void
add_rows(HtmxElement *pad, const gchar *agent_id, GPtrArray *rows,
         gboolean by_day)
{
	const gchar *last_day = NULL;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);

		if (by_day && row->day != NULL &&
		    g_strcmp0(row->day, last_day) != 0) {
			g_autoptr(HtmxHeading) heading = htmx_heading_new(3);

			htmx_element_add_class(HTMX_ELEMENT(heading), "card-title");
			htmx_node_set_text_content(HTMX_NODE(heading), row->day);
			htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(heading));
			last_day = row->day;
		}

		clawt_web_add(pad, row_card(agent_id, row));
	}
}

HtmxElement *
clawt_web_schedule_body(ClawtWebApp *app, const gchar *agent_id,
                        HtmxRequest *request)
{
	g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
	g_autoptr(HtmxDiv) pad = htmx_div_new();
	g_autoptr(HtmxDiv) bar = htmx_div_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) loose = NULL;
	g_autofree gchar *zone_line = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *error_text = NULL;
	const gchar *slug = NULL;
	const gchar *date = NULL;
	const gchar *filter = NULL;
	gboolean disabled = FALSE;
	ClawtLrgSchedView which;
	gint today_y;
	gint today_m;
	gint today_d;
	gint year;
	gint month;
	gint day;
	gint py;
	gint pm;
	gint pd;
	gint ny;
	gint nm;
	gint nd;
	guint i;

	if (request != NULL) {
		slug = htmx_request_get_query_param(request, "view");
		date = htmx_request_get_query_param(request, "date");
		filter = htmx_request_get_query_param(request, "agent");
		disabled = g_strcmp0(
		    htmx_request_get_query_param(request, "disabled"), "1") == 0;
	}

	which = view_from_slug(slug);

	/*
	 * The first request has no reply to name the daemon's zone yet, so
	 * "today" is the viewer's, as the model documents.
	 */
	clawt_lrg_schedule_today(NULL, &today_y, &today_m, &today_d);

	if (!parse_date(date, &year, &month, &day)) {
		year = today_y;
		month = today_m;
		day = today_d;
	}

	/* Ask the daemon.  Upcoming is one read; the rest are a window. */
	if (which == CLAWT_LRG_SCHED_UPCOMING) {
		reply = clawt_web_app_request(
		    app, "routine.upcoming",
		    clawt_lrg_schedule_upcoming_payload(filter, NULL), &error);
	} else {
		clawt_lrg_schedule_range(which, year, month, day, &from, &to);
		reply = clawt_web_app_request(
		    app, "routine.occurrences",
		    clawt_lrg_schedule_occurrences_payload(
		        from, to, filter, NULL, disabled,
		        clawt_lrg_schedule_wants_collapse(which)),
		    &error);
	}

	htmx_element_add_class(view, "view");
	htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
	clawt_web_add(pad, clawt_web_section_title("Schedule"));

	/* The view switcher. */
	htmx_element_add_class(HTMX_ELEMENT(bar), "btn-row");

	for (i = 0; i < clawt_lrg_schedule_view_count(); i++) {
		g_autofree gchar *url = page_url(
		    agent_id, (ClawtLrgSchedView)i, TRUE, year, month, day,
		    filter, disabled);

		clawt_web_add(bar, nav_link(
		    clawt_lrg_schedule_view_label((ClawtLrgSchedView)i), url,
		    (ClawtLrgSchedView)i == which));
	}

	/* Previous, today, next.  Upcoming has no window to move. */
	if (which != CLAWT_LRG_SCHED_UPCOMING) {
		g_autofree gchar *prev_url = NULL;
		g_autofree gchar *today_url = NULL;
		g_autofree gchar *next_url = NULL;

		py = year;
		pm = month;
		pd = day;
		ny = year;
		nm = month;
		nd = day;
		clawt_lrg_schedule_move(which, -1, today_y, today_m, today_d,
		                        &py, &pm, &pd);
		clawt_lrg_schedule_move(which, 1, today_y, today_m, today_d,
		                        &ny, &nm, &nd);

		prev_url = page_url(agent_id, which, TRUE, py, pm, pd, filter,
		                    disabled);
		today_url = page_url(agent_id, which, FALSE, 0, 0, 0, filter,
		                     disabled);
		next_url = page_url(agent_id, which, TRUE, ny, nm, nd, filter,
		                    disabled);

		clawt_web_add(bar, nav_link("Previous", prev_url, FALSE));
		clawt_web_add(bar, nav_link("Today", today_url, FALSE));
		clawt_web_add(bar, nav_link("Next", next_url, FALSE));
	}

	/* Show or hide disabled routines. */
	{
		g_autofree gchar *url = page_url(agent_id, which, TRUE, year,
		                                 month, day, filter, !disabled);

		clawt_web_add(bar, nav_link(
		    disabled ? "Hide disabled" : "Show disabled", url, FALSE));
	}

	htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(bar));

	/*
	 * A refused window keeps the daemon's sentence, whole: it names the
	 * bound that was hit, and a shorter one hides which.
	 */
	error_text = clawt_lrg_schedule_error_text(error);

	if (error_text != NULL) {
		clawt_web_add(pad, clawt_web_notice(error_text, "bad"));
		htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

		return g_steal_pointer(&view);
	}

	zone_line = clawt_lrg_schedule_zone_line(reply);
	clawt_web_add(pad, clawt_web_text(zone_line, "lede"));

	rows = clawt_lrg_schedule_rows(reply);

	if (rows->len == 0)
		clawt_web_add(pad, clawt_web_empty("Nothing scheduled", NULL));
	else
		add_rows(HTMX_ELEMENT(pad), agent_id, rows,
		         which != CLAWT_LRG_SCHED_UPCOMING);

	/* Jobs that are not on the grid, and why. */
	loose = clawt_lrg_schedule_unscheduled(reply);

	if (loose->len > 0) {
		clawt_web_add(pad, clawt_web_section_title("Not on the calendar"));
		add_rows(HTMX_ELEMENT(pad), agent_id, loose, FALSE);
	}

	htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

	return g_steal_pointer(&view);
}
