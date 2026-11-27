/*
 * test-web-schedule.c - The web Schedule page, drawn with JavaScript off
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no browser.  The page's sources are included, as
 * test-web-office and test-web-render do, and sched_render_panel() is
 * fed canned routine.occurrences, routine.upcoming, agent.list and
 * team.list replies.  Every assertion is on the HTML a browser would
 * receive.  Nothing here needs a script to work: the grid, the "+N more"
 * link, the folded group's link and the Previous / Today / Next links are
 * all ordinary hrefs, and htmx's hx-get is only an addition to them.
 *
 * 14:00Z is 09:00 in America/Detroit in November.  A page that formatted
 * in UTC would show 14:00, which is the wrong hour on the grid.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "clawt-test-util.h"

#include "../clients/web/web-app.c"
#include "../clients/web/web-files.c"
#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"
#include "../clients/lrg/lrg-schedule-model.c"
#include "../clients/web/web-schedule.c"

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

/* The same box web-fleet.c draws; the page uses it for a refusal. */
HtmxDiv *
clawt_web_notice(const gchar *text, const gchar *tone)
{
	g_autoptr(HtmxDiv) box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "notice");

	if (tone != NULL && *tone != '\0') {
		g_autofree gchar *css = g_strdup_printf("notice-%s", tone);

		htmx_element_add_class(HTMX_ELEMENT(box), css);
	}

	htmx_node_set_text_content(HTMX_NODE(box), text != NULL ? text : "");

	return (HtmxDiv *)g_steal_pointer(&box);
}

/* ── Canned replies ──────────────────────────────────────────────── */

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	JsonNode *node = json_from_string(text, &error);

	g_assert_no_error(error);
	g_assert_nonnull(node);

	return node;
}

static void
add_occurrence(GString *out, const gchar *routine, const gchar *title,
               const gchar *agent, const gchar *at, const gchar *status,
               const gchar *extra)
{
	g_string_append_printf(
	    out,
	    "%s{\"source\":\"routine\",\"routine\":\"%s\",\"agent\":\"%s\","
	    "\"title\":\"%s\",\"at\":\"%s\",\"earliest\":\"%s\","
	    "\"latest\":\"%s\",\"kind\":\"scheduled\",\"repeat\":\"daily\","
	    "\"status\":\"%s\"%s}",
	    out->str[out->len - 1] == '[' ? "" : ",", routine, agent, title, at,
	    at, at, status, extra != NULL ? extra : "");
}

/*
 * The week of Monday 2026-11-23.  Tuesday has ten runs, which is two
 * more than a week cell shows.  Wednesday has an hourly routine already
 * folded into a group.  Thursday has a failed run whose title and detail
 * are hostile.  Friday has a past run with a task and a future one.
 */
static JsonNode *
canned_week(void)
{
	g_autoptr(GString) text = g_string_new(
	    "{\"time_zone\":\"America/Detroit\","
	    "\"from\":\"2026-11-23T05:00:00Z\","
	    "\"to\":\"2026-11-30T05:00:00Z\",\"occurrences\":[");
	guint i;

	for (i = 1; i <= 10; i++) {
		g_autofree gchar *routine = g_strdup_printf("r%u", i);
		g_autofree gchar *title = g_strdup_printf("Run %u", i);
		g_autofree gchar *at = g_strdup_printf(
		    "2026-11-24T14:%02u:00Z", i - 1);

		add_occurrence(text, routine, title, "ada", at, "ok", NULL);
	}

	add_occurrence(text, "bad", "<script>alert(1)</script>", "eve",
	               "2026-11-26T15:00:00Z", "failed",
	               ",\"detail\":\"x\\\" onmouseover=\\\"y<i>\"");
	add_occurrence(text, "done", "Morning", "ada",
	               "2026-11-27T14:00:00Z", "ok",
	               ",\"task\":\"task-1\",\"room\":\"room-1\"");
	add_occurrence(text, "later", "Evening", "ada",
	               "2026-11-27T23:00:00Z", "upcoming", NULL);

	g_string_append(
	    text,
	    "],\"groups\":[{\"source\":\"routine\",\"routine\":\"hourly\","
	    "\"agent\":\"ada\",\"title\":\"Hourly check\","
	    "\"day\":\"2026-11-25\",\"first\":\"2026-11-25T13:00:00Z\","
	    "\"last\":\"2026-11-25T23:00:00Z\",\"count\":11,"
	    "\"every_seconds\":3600,"
	    "\"label\":\"every hour, 08:00-18:00\",\"status\":\"ok\","
	    "\"statuses\":{\"ok\":11},\"occurrences\":[");
	add_occurrence(text, "hourly", "Hourly one", "ada",
	               "2026-11-25T13:00:00Z", "ok", NULL);
	add_occurrence(text, "hourly", "Hourly two", "ada",
	               "2026-11-25T14:00:00Z", "ok", NULL);
	g_string_append(text, "]}],\"unscheduled\":[]}");

	return parse(text->str);
}

static JsonNode *
canned_upcoming(void)
{
	return parse(
	    "{\"time_zone\":\"America/Detroit\",\"now\":\"2026-11-27T17:00:00Z\","
	    "\"runs\":[{\"source\":\"routine\",\"routine\":\"later\","
	    "\"agent\":\"ada\",\"title\":\"Evening\","
	    "\"at\":\"2026-11-27T23:00:00Z\","
	    "\"earliest\":\"2026-11-27T23:00:00Z\","
	    "\"latest\":\"2026-11-27T23:00:00Z\",\"kind\":\"scheduled\","
	    "\"repeat\":\"daily\",\"status\":\"upcoming\"}]}");
}

static JsonNode *
canned_agents(void)
{
	return parse(
	    "{\"agents\":[{\"id\":\"ada\",\"name\":\"Ada\"},"
	    "{\"id\":\"eve\",\"name\":\"Eve <b>x</b>\"}]}");
}

static JsonNode *
canned_teams(void)
{
	return parse("{\"teams\":[{\"id\":\"ops\",\"name\":\"Ops <u>team</u>\"}]}");
}

/* The page as a browser gets it, for a query on the canned week. */
static gchar *
draw(const gchar *view, const gchar *date, const gchar *agent,
     const gchar *team, const gchar *disabled, const gchar *more,
     const gchar *expand)
{
	g_autoptr(JsonNode) occ = canned_week();
	g_autoptr(JsonNode) up = canned_upcoming();
	g_autoptr(JsonNode) agents = canned_agents();
	g_autoptr(JsonNode) teams = canned_teams();
	g_autoptr(HtmxElement) panel = NULL;
	SchedQuery q;
	SchedData data;
	gchar *html;

	sched_query_init(&q, "ada", view, date, agent, team, disabled, more,
	                 expand);
	memset(&data, 0, sizeof(data));
	data.occurrences = occ;
	data.upcoming = up;
	data.agents = agents;
	data.teams = teams;

	panel = sched_render_panel(&q, &data);
	html = htmx_element_render(panel);
	sched_query_clear(&q);

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

/* ── The grid ────────────────────────────────────────────────────── */

static void
test_the_week_is_seven_days_in_the_daemons_zone(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "sched-grid-week"));
	g_assert_cmpuint(count_of(html, "class=\"sched-day-head\""), ==, 7);

	/* the range and the zone the times are in */
	g_assert_nonnull(strstr(html,
	                        "Mon 23 Nov \xe2\x80\x93 Sun 29 Nov 2026"));
	g_assert_nonnull(strstr(html, "Times in America/Detroit"));

	/* 14:00Z is 09:00 there */
	g_assert_nonnull(strstr(html, ">09:00<"));
	g_assert_null(strstr(html, ">14:00<"));
}

static void
test_the_controls_are_links_that_work_without_script(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	/* Day, Week, Month, Previous, Today, Next: ordinary hrefs */
	g_assert_nonnull(strstr(html, "href=\"/a/ada/schedule?view=day"));
	g_assert_nonnull(strstr(html, "href=\"/a/ada/schedule?view=month"));
	g_assert_nonnull(strstr(html, ">Previous<"));
	g_assert_nonnull(strstr(html, ">Today<"));
	g_assert_nonnull(strstr(html, ">Next<"));

	/* a week back, and a week on */
	g_assert_nonnull(strstr(html, "date=2026-11-17"));
	g_assert_nonnull(strstr(html, "date=2026-12-01"));

	/* the current view is marked */
	g_assert_nonnull(strstr(html, "aria-current=\"page\""));

	/* htmx only enhances them */
	g_assert_nonnull(strstr(html, "hx-get=\"/f/schedule?view=week"));
	g_assert_nonnull(strstr(html, "hx-target=\"#schedule-panel\""));
	g_assert_nonnull(strstr(html, "id=\"schedule-panel\""));
}

static void
test_a_day_is_one_column_with_every_card(void)
{
	g_autofree gchar *html = draw("day", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "sched-grid-day"));
	g_assert_cmpuint(count_of(html, "class=\"sched-day-head\""), ==, 1);
	g_assert_nonnull(strstr(html, ">Run 10<"));
	g_assert_null(strstr(html, " more<"));
}

static void
test_a_month_is_six_weeks(void)
{
	g_autofree gchar *html = draw("month", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "sched-grid-month"));
	g_assert_cmpuint(count_of(html, "class=\"sched-day-head\""), ==, 42);
	g_assert_nonnull(strstr(html, "November 2026"));

	/* three cards a day in a month, so the ten become three and "+7" */
	g_assert_nonnull(strstr(html, "+7 more"));
}

/* ── "+N more" ───────────────────────────────────────────────────── */

static void
test_an_overflowing_day_says_how_many_more_and_links_to_them(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	/* a week cell shows eight; the ninth and tenth are behind the link */
	g_assert_nonnull(strstr(html, ">Run 8<"));
	g_assert_null(strstr(html, ">Run 9<"));
	g_assert_null(strstr(html, ">Run 10<"));
	g_assert_nonnull(strstr(html, "+2 more"));
	g_assert_nonnull(strstr(html, "more=2026-11-24"));
}

static void
test_the_more_link_opens_the_day_and_can_close_it(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              "2026-11-24", NULL);

	g_assert_nonnull(strstr(html, ">Run 9<"));
	g_assert_nonnull(strstr(html, ">Run 10<"));
	g_assert_null(strstr(html, "+2 more"));
	g_assert_nonnull(strstr(html, ">Show fewer<"));
}

/* ── Folding ─────────────────────────────────────────────────────── */

static void
test_an_hourly_routine_is_folded_with_a_link_that_opens_it(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "every hour, 08:00-18:00"));
	g_assert_nonnull(strstr(html, "Show 11 runs"));
	g_assert_nonnull(strstr(html, "expand=hourly%40"));

	/* the runs it folds are not drawn until it is opened */
	g_assert_null(strstr(html, "Hourly one"));
	g_assert_null(strstr(html, "sched-runs"));
}

static void
test_an_opened_group_lists_its_runs(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, "hourly@2026-11-25");

	g_assert_nonnull(strstr(html, "sched-runs"));
	g_assert_nonnull(strstr(html, "Hourly one"));
	g_assert_nonnull(strstr(html, "Hourly two"));
	g_assert_nonnull(strstr(html, ">Hide runs<"));
}

/* ── Cards ───────────────────────────────────────────────────────── */

static void
test_cards_carry_a_mark_in_words_and_future_ones_are_dimmed(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "\xe2\x9c\x93 done"));
	g_assert_nonnull(strstr(html, "\xe2\x9c\x97 failed"));
	g_assert_nonnull(strstr(html, "sched-future"));
}

static void
test_a_past_card_opens_its_run_and_a_future_one_its_routine(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "href=\"/tasks/task-1\""));
	g_assert_nonnull(strstr(html, "href=\"/a/ada/routines\""));
}

static void
test_upcoming_runs_are_listed_beside_the_grid(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "Upcoming runs"));
	g_assert_nonnull(strstr(html, "2026-11-27 18:00"));
}

/* ── Filters ─────────────────────────────────────────────────────── */

static void
test_filters_ride_every_link(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", "ada", "ops", "1",
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "agent=ada"));
	g_assert_nonnull(strstr(html, "team=ops"));
	g_assert_nonnull(strstr(html, "disabled=1"));
	g_assert_nonnull(strstr(html, ">Hide disabled<"));
}

static void
test_the_filters_are_a_get_form(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_nonnull(strstr(html, "method=\"get\""));
	g_assert_nonnull(strstr(html, "name=\"agent\""));
	g_assert_nonnull(strstr(html, "name=\"team\""));
	g_assert_nonnull(strstr(html, ">Show disabled<"));
}

/* ── Refusal ─────────────────────────────────────────────────────── */

/*
 * A window past a bound is refused with a sentence naming it.  It is
 * shown whole, and no grid is drawn beside it: a grid would read as a
 * calendar that has nothing on it.
 */
static void
test_a_refused_window_keeps_the_daemons_sentence(void)
{
	g_autoptr(JsonNode) up = canned_upcoming();
	g_autoptr(HtmxElement) panel = NULL;
	g_autofree gchar *html = NULL;
	SchedQuery q;
	SchedData data;

	sched_query_init(&q, "ada", "month", "2026-11-24", NULL, NULL, NULL,
	                 NULL, NULL);
	memset(&data, 0, sizeof(data));
	data.occ_error = "That window is longer than 62 days "
	                 "(daemon.calendar_max_days).";
	data.upcoming = up;

	panel = sched_render_panel(&q, &data);
	html = htmx_element_render(panel);
	sched_query_clear(&q);

	g_assert_nonnull(strstr(html, "That window is longer than 62 days"));
	g_assert_nonnull(strstr(html, "daemon.calendar_max_days"));
	g_assert_null(strstr(html, "sched-grid"));

	/* the upcoming panel is a separate read and still draws */
	g_assert_nonnull(strstr(html, "Upcoming runs"));
}

/* ── Escaping ────────────────────────────────────────────────────── */

/*
 * only_inside_attributes:
 * @html: a rendered page
 * @needle: markup a hostile string carries, e.g. "<b>x</b>"
 *
 * htmx-glib escapes '"' and '&' in attribute values but not '<', and a
 * '<' inside a quoted attribute value is inert.  So a hostile name may
 * appear unescaped there (a face's title, an img alt) and nowhere else.
 * Each occurrence must sit after an opening quote that comes after the
 * last '>' before it: inside the value of a tag still being written.
 *
 * Returns: %TRUE when every occurrence of @needle is inside a quoted
 *   attribute value
 */
static gboolean
only_inside_attributes(
	const gchar	*html,
	const gchar	*needle
){
	const gchar *p;

	for (p = strstr(html, needle); p != NULL; p = strstr(p + 1, needle)) {
		const gchar *q;
		const gchar *last_quote = NULL;
		const gchar *last_close = NULL;

		for (q = p; q > html; q--) {
			if (last_quote == NULL && q[-1] == '"')
				last_quote = q - 1;
			if (q[-1] == '>') {
				last_close = q - 1;
				break;
			}
		}

		if (last_quote == NULL || (last_close != NULL &&
		                           last_quote < last_close))
			return FALSE;
	}

	return TRUE;
}

/*
 * Titles, agent names, team names and details are written by people and
 * models.  A "<" that reaches the page as markup is script injection, and
 * a quote that closes a title attribute is the same thing by another
 * road.
 */
static void
test_daemon_strings_are_escaped(void)
{
	g_autofree gchar *html = draw("week", "2026-11-24", NULL, NULL, NULL,
	                              NULL, NULL);

	g_assert_null(strstr(html, "<script>"));
	g_assert_nonnull(strstr(html, "&lt;script&gt;alert(1)"));

	/* the detail lands in a title attribute; its quote cannot close it */
	g_assert_null(strstr(html, "onmouseover=\"y"));
	g_assert_nonnull(strstr(html, "x&quot; onmouseover=&quot;y"));
	g_assert_true(only_inside_attributes(html, "<i>"));

	/* an agent's name and a team's, in the faces and the filters */
	g_assert_true(only_inside_attributes(html, "<b>x</b>"));
	g_assert_true(only_inside_attributes(html, "<u>team</u>"));
	g_assert_nonnull(strstr(html, "Ops &lt;u&gt;team"));
}

/* ── The query ───────────────────────────────────────────────────── */

static void
test_a_bad_query_falls_back_rather_than_failing(void)
{
	SchedQuery q;

	sched_query_init(&q, "ada", "decade", "not-a-date", "", "", "yes", NULL,
	                 NULL);

	g_assert_cmpint(q.view, ==, CLAWT_LRG_SCHED_WEEK);
	g_assert_false(q.has_date);
	g_assert_null(q.agent);
	g_assert_null(q.team);
	g_assert_false(q.disabled);

	sched_query_clear(&q);

	sched_query_init(&q, "ada", "month", "2026-02-30", NULL, NULL, NULL,
	                 NULL, NULL);
	g_assert_false(q.has_date);
	sched_query_clear(&q);
}

static void
test_a_list_toggles_one_entry(void)
{
	g_autofree gchar *added = sched_list_toggle(NULL, "a");
	g_autofree gchar *more = sched_list_toggle(added, "b");
	g_autofree gchar *less = sched_list_toggle(more, "a");

	g_assert_cmpstr(added, ==, "a");
	g_assert_cmpstr(more, ==, "a,b");
	g_assert_cmpstr(less, ==, "b");
	g_assert_true(sched_list_has(more, "b"));
	g_assert_false(sched_list_has(less, "a"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-schedule/week-in-the-daemons-zone",
	                test_the_week_is_seven_days_in_the_daemons_zone);
	g_test_add_func("/web-schedule/controls-are-links",
	                test_the_controls_are_links_that_work_without_script);
	g_test_add_func("/web-schedule/day-is-one-column",
	                test_a_day_is_one_column_with_every_card);
	g_test_add_func("/web-schedule/month-is-six-weeks",
	                test_a_month_is_six_weeks);
	g_test_add_func("/web-schedule/plus-n-more",
	                test_an_overflowing_day_says_how_many_more_and_links_to_them);
	g_test_add_func("/web-schedule/more-opens-and-closes",
	                test_the_more_link_opens_the_day_and_can_close_it);
	g_test_add_func("/web-schedule/hourly-is-folded",
	                test_an_hourly_routine_is_folded_with_a_link_that_opens_it);
	g_test_add_func("/web-schedule/opened-group-lists-runs",
	                test_an_opened_group_lists_its_runs);
	g_test_add_func("/web-schedule/marks-and-dimming",
	                test_cards_carry_a_mark_in_words_and_future_ones_are_dimmed);
	g_test_add_func("/web-schedule/card-links",
	                test_a_past_card_opens_its_run_and_a_future_one_its_routine);
	g_test_add_func("/web-schedule/upcoming-panel",
	                test_upcoming_runs_are_listed_beside_the_grid);
	g_test_add_func("/web-schedule/filters-ride-links",
	                test_filters_ride_every_link);
	g_test_add_func("/web-schedule/filters-are-a-get-form",
	                test_the_filters_are_a_get_form);
	g_test_add_func("/web-schedule/refused-window",
	                test_a_refused_window_keeps_the_daemons_sentence);
	g_test_add_func("/web-schedule/escaping",
	                test_daemon_strings_are_escaped);
	g_test_add_func("/web-schedule/bad-query",
	                test_a_bad_query_falls_back_rather_than_failing);
	g_test_add_func("/web-schedule/list-toggle",
	                test_a_list_toggles_one_entry);

	return g_test_run();
}
