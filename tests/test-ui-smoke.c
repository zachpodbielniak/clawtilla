/*
 * test-ui-smoke.c - What a click-through of the clients found
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Each of these was seen on screen, in both desktop clients, against a
 * scratch fleet: the rule lives in libclawt so every client draws the
 * same thing, and this pins the rule.  The client halves that cannot be
 * tested without a display are the one call each now makes.
 */

#include <clawtilla.h>

#include <string.h>

/*
 * A task's own room is derived, like a direct room, and so is every id
 * with a colon in it.  `task:` was missing from the list of prefixes,
 * so delegating with `thread: new` put "alpha, gamma -- 2 members" in
 * every sidebar as though somebody had declared it.
 */
static void
test_a_task_room_is_not_declared(
	void
){
	g_assert_false(clawt_room_is_declared("task:task-01m7ecs8x6415qrgv5nk"));
	g_assert_false(clawt_room_is_listed("task:task-01m7ecs8x6415qrgv5nk"));

	/* the ones that were already derived stay derived */
	g_assert_false(clawt_room_is_declared("dm:alpha:user"));
	g_assert_false(clawt_room_is_declared("routine:nightly"));
	g_assert_false(clawt_room_is_declared("trigger:hook:beta"));
	g_assert_false(clawt_room_is_declared("incidents:eng"));

	/* an incidents room is derived and still listed */
	g_assert_true(clawt_room_is_listed("incidents:eng"));

	/* a room somebody wrote down */
	g_assert_true(clawt_room_is_declared("standup"));
	g_assert_true(clawt_room_is_listed("standup"));
	g_assert_false(clawt_room_is_declared(NULL));
}

/*
 * "1 members · everyone answers" under a "Problems to look at" room:
 * a plural on one, and a promise about a room nobody answers in.
 */
static void
test_the_sidebar_says_what_a_room_is(
	void
){
	g_autofree gchar *group = clawt_room_sidebar_subtitle("standup", 3, TRUE);
	g_autofree gchar *open = clawt_room_sidebar_subtitle("lobby", 3, FALSE);
	g_autofree gchar *one = clawt_room_sidebar_subtitle("solo", 1, FALSE);
	g_autofree gchar *none = clawt_room_sidebar_subtitle(NULL, 0, FALSE);
	g_autofree gchar *problems =
		clawt_room_sidebar_subtitle("incidents:eng", 1, FALSE);

	g_assert_cmpstr(group, ==, "3 members \302\267 answers when named");
	g_assert_cmpstr(open, ==, "3 members \302\267 everyone answers");
	g_assert_cmpstr(one, ==, "1 member \302\267 everyone answers");
	g_assert_cmpstr(none, ==, "0 members \302\267 everyone answers");

	g_assert_null(strstr(problems, "members"));
	g_assert_null(strstr(problems, "answers"));
	g_assert_nonnull(strstr(problems, "lead"));
}

/*
 * A rebuild request said what a rebuild destroys twice: at the end of
 * the question, which every client drew whole, and again as the
 * warning each client draws from `loses`.
 */
static void
test_a_rebuild_question_drops_its_loss(
	void
){
	const gchar *loses = "the container and everything installed in it.";
	g_autofree gchar *question = g_strconcat(
		"beta asks for its computer to be rebuilt.\n\nIts reason:\n"
		"```\nWhat a rebuild destroys: nothing, it says\n```\n"
		"Approve rebuilds it.",
		CLAWT_DECISION_LOSS_MARKER, loses, NULL);
	g_autofree gchar *drawn = clawt_decision_question_text(question, loses);
	g_autofree gchar *whole = clawt_decision_question_text(question, NULL);
	g_autofree gchar *plain =
		clawt_decision_question_text("Ship tonight?", loses);
	g_autofree gchar *empty = clawt_decision_question_text(NULL, loses);

	g_assert_null(strstr(drawn, loses));
	g_assert_true(g_str_has_suffix(drawn, "Approve rebuilds it."));

	/* the agent's own words above the marker are left alone */
	g_assert_nonnull(strstr(drawn, "nothing, it says"));

	/* no loss drawn apart, nothing taken out */
	g_assert_cmpstr(whole, ==, question);
	g_assert_cmpstr(plain, ==, "Ship tonight?");
	g_assert_cmpstr(empty, ==, "");
}

/*
 * The alerts replayed on reconnect said when the window heard of them:
 * a routine turned off twenty minutes earlier read "6m ago".
 */
static void
test_an_alert_is_dated_by_its_event(
	void
){
	ClawtEvent *event = clawt_event_new("routine.turned_off", "broken");
	gint64 now = 1800000000LL * G_USEC_PER_SEC;
	gint64 then = now - 20 * 60 * G_USEC_PER_SEC;

	clawt_event_set_timestamp(event, then);
	g_assert_cmpint(clawt_event_happened_at(event, now), ==, then);

	/* nothing ahead of the clock, and no time at all reads as now */
	clawt_event_set_timestamp(event, now + G_USEC_PER_SEC);
	g_assert_cmpint(clawt_event_happened_at(event, now), ==, now);
	clawt_event_set_timestamp(event, 0);
	g_assert_cmpint(clawt_event_happened_at(event, now), ==, now);

	clawt_event_free(event);
}

/*
 * The agent page speaks to the operator.  It printed what the agent is
 * told about its computer, which is written to the agent -- "You have no
 * computer. You cannot run commands..." -- so the page addressed the
 * person reading it as the agent.  With no computer the operator gets a
 * sentence of their own; with one, the agent's words are captioned as
 * the agent's.
 */
static void
test_the_agent_page_does_not_address_the_operator_as_the_agent(
	void
){
	const gchar *agent_text = "You have no computer. You cannot run "
	                          "commands, read files or reach a filesystem.";
	const gchar *caption = "unset";
	const gchar *said;

	said = clawt_agent_computer_text_for_operator("none", agent_text,
	                                              &caption);
	g_assert_nonnull(said);
	g_assert_null(caption);
	g_assert_true(g_str_has_prefix(said, "This agent has no computer"));
	g_assert_null(strstr(said, "You "));
	g_assert_null(strstr(said, "you "));

	/* absent is the same answer as "none" */
	g_assert_cmpstr(clawt_agent_computer_text_for_operator(NULL, NULL, NULL),
	                ==, said);

	/* a computer: the agent's own words, introduced as the agent's */
	caption = NULL;
	said = clawt_agent_computer_text_for_operator(
		"container", "You have a container of your own.", &caption);
	g_assert_cmpstr(said, ==, "You have a container of your own.");
	g_assert_nonnull(caption);
	g_assert_nonnull(strstr(caption, "the agent itself is told"));

	/* a computer the daemon said nothing about: nothing to show */
	caption = "unset";
	g_assert_null(clawt_agent_computer_text_for_operator("host", NULL,
	                                                     &caption));
	g_assert_null(caption);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/ui-smoke/task-room-not-declared",
	                test_a_task_room_is_not_declared);
	g_test_add_func("/ui-smoke/room-sidebar-subtitle",
	                test_the_sidebar_says_what_a_room_is);
	g_test_add_func("/ui-smoke/rebuild-question-without-loss",
	                test_a_rebuild_question_drops_its_loss);
	g_test_add_func("/ui-smoke/alert-dated-by-event",
	                test_an_alert_is_dated_by_its_event);
	g_test_add_func("/ui-smoke/agent-page-speaks-to-the-operator",
	                test_the_agent_page_does_not_address_the_operator_as_the_agent);

	return g_test_run();
}
