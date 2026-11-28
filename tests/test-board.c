/*
 * test-board.c - The agent board: columns, quiet, phases, timeline, text
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The board answers "where is this agent, and what is still in front of
 * it?" for the CLI, the GTK client and the web client alike, so its
 * rules are pinned here rather than in any of them:
 *
 *   - clawt_board_classify() over a table of states x children x phase;
 *   - the quiet rule against an injected clock;
 *   - phase validation, and a phase lasting until the task goes back to
 *     pending;
 *   - the timeline from journal rows, and from an old task's stamps;
 *   - the commit and branch a done card reads out of a result;
 *   - the acceptance case: an agent's top-level tasks give the expected
 *     columns, chips, review wait and totals, a failed child or an
 *     operator decision pins a card under blocked, and the CLI's text is
 *     the JSON column for column.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <string.h>

/*
 * The fixed clock every test reads: 2027-01-15 08:00 UTC.  Nothing here
 * asks the real time, so a case never passes or fails by the minute it
 * ran in.
 */
#define NOW ((gint64)1800000000)
#define MINUTE ((gint64)60)
#define HOUR ((gint64)3600)
#define WINDOW_START (NOW - 24 * HOUR)

/* ── Building tasks ──────────────────────────────────────────────── */

/*
 * A task as the store would read it back: every time given, so the
 * rules see exactly what a case says.  @extra is more JSON members
 * (`"reviewer":"oxpecker",`), each ending in a comma, or NULL.
 */
static ClawtTask *
make_task(
	const gchar *id,
	const gchar *assignee,
	const gchar *state,
	const gchar *parent,
	gint64       created,
	gint64       started,
	gint64       state_at,
	gint64       finished,
	const gchar *extra
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *parent_member = parent != NULL
		? g_strdup_printf("\"parent_id\":\"%s\",", parent) : g_strdup("");
	g_autofree gchar *assignee_member = assignee != NULL
		? g_strdup_printf("\"assignee\":\"%s\",", assignee) : g_strdup("");
	g_autofree gchar *text = g_strdup_printf(
		"{\"id\":\"%s\",\"prompt\":\"Fix bug %s\\nin detail\",\"origin\":\"chief\","
		"%s%s%s\"state\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
		"\"started_at\":%" G_GINT64_FORMAT ",\"state_at\":%" G_GINT64_FORMAT ","
		"\"finished_at\":%" G_GINT64_FORMAT "}",
		id, id, assignee_member, parent_member, extra != NULL ? extra : "",
		state, created, started, state_at, finished);
	ClawtTask *task;

	json_parser_load_from_data(parser, text, -1, &error);
	g_assert_no_error(error);

	task = clawt_task_new_from_json(json_parser_get_root(parser), &error);
	g_assert_no_error(error);
	g_assert_nonnull(task);

	return task;
}

/* A running task of @owner's, last heard of @ago seconds before NOW. */
static ClawtTask *
running_task(
	const gchar *id,
	const gchar *owner,
	gint64       ago,
	const gchar *extra
){
	return make_task(id, owner, "running", NULL, NOW - 2 * HOUR, NOW - ago,
	                 NOW - ago, 0, extra);
}

/* ── The classification table ────────────────────────────────────── */

/*
 * One row: the card's state, phase and own wait; its children as a
 * comma-separated list of kinds; open decisions; whether it is quiet
 * (heard of 30 minutes ago against a 20-minute window) and whether its
 * assignee has a wait pending; and what it must come out as.
 *
 * Child kinds: running, pending, completed, failed, failed-ack (failed,
 * and the card reported since), stalled, review-owner, review-other,
 * waits-operator.
 */
typedef struct {
	const gchar      *name;
	const gchar      *state;
	const gchar      *phase;
	const gchar      *waiting_on;
	const gchar      *children;
	guint             decisions;
	gboolean          quiet;
	gboolean          wait_pending;
	const gchar      *owner;
	gint64            finished;
	ClawtBoardColumn  expect;
	ClawtBoardBlocks  blocks;
} ClassifyRow;

#define FIN_IN  (NOW - HOUR)
#define FIN_OLD (NOW - 30 * HOUR)

static const ClassifyRow classify_rows[] = {
	/* Not started, or held. */
	{ "pending", "pending", NULL, NULL, NULL, 0, FALSE, FALSE, NULL, 0,
	  CLAWT_BOARD_BACKLOG, 0 },
	{ "held-after", "blocked", NULL, NULL, NULL, 0, FALSE, FALSE, NULL, 0,
	  CLAWT_BOARD_BACKLOG, 0 },
	{ "pending-phase-ignored", "pending", "gating", NULL, NULL, 0, FALSE,
	  FALSE, NULL, 0, CLAWT_BOARD_BACKLOG, 0 },

	/* Waiting on somebody it named. */
	{ "waits-agent", "blocked", NULL, "scout", NULL, 0, FALSE, FALSE, NULL,
	  0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_WAITING },
	{ "waits-operator", "blocked", NULL, "operator", NULL, 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_WAITING },

	/* Running, inferred from children. */
	{ "running-alone", "running", NULL, NULL, NULL, 0, FALSE, FALSE, NULL, 0,
	  CLAWT_BOARD_PLANNING, 0 },
	{ "child-running", "running", NULL, NULL, "running", 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_DELEGATED, 0 },
	{ "child-pending", "running", NULL, NULL, "pending", 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_DELEGATED, 0 },
	{ "children-all-done", "running", NULL, NULL, "completed,completed", 0,
	  FALSE, FALSE, NULL, 0, CLAWT_BOARD_PLANNING, 0 },
	{ "child-review-owner", "running", NULL, NULL, "running,review-owner", 0,
	  FALSE, FALSE, NULL, 0, CLAWT_BOARD_REVIEW, 0 },
	{ "child-review-other", "running", NULL, NULL, "review-other", 0, FALSE,
	  FALSE, NULL, 0, CLAWT_BOARD_DELEGATED, 0 },

	/* Pinned under blocked. */
	{ "child-failed", "running", NULL, NULL, "running,failed", 0, FALSE,
	  FALSE, NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_CHILD_FAILED },
	{ "child-failed-acknowledged", "running", NULL, NULL, "failed-ack", 0,
	  FALSE, FALSE, NULL, 0, CLAWT_BOARD_PLANNING, 0 },
	{ "child-stalled", "running", NULL, NULL, "stalled", 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_CHILD_STALLED },
	{ "child-waits-operator", "running", NULL, NULL, "waits-operator", 0,
	  FALSE, FALSE, NULL, 0, CLAWT_BOARD_BLOCKED,
	  CLAWT_BOARD_BLOCK_DECISION },
	{ "decision", "running", NULL, NULL, NULL, 1, FALSE, FALSE, NULL, 0,
	  CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_DECISION },
	{ "quiet", "running", NULL, NULL, NULL, 0, TRUE, FALSE, NULL, 0,
	  CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_QUIET },
	{ "quiet-but-waiting", "running", NULL, NULL, NULL, 0, TRUE, TRUE, NULL,
	  0, CLAWT_BOARD_PLANNING, 0 },
	{ "quiet-but-child-queued", "running", NULL, NULL, "pending", 0, TRUE,
	  FALSE, NULL, 0, CLAWT_BOARD_DELEGATED, 0 },
	{ "decision-and-failed-child", "running", NULL, NULL, "failed", 2, FALSE,
	  FALSE, NULL, 0, CLAWT_BOARD_BLOCKED,
	  CLAWT_BOARD_BLOCK_DECISION | CLAWT_BOARD_BLOCK_CHILD_FAILED },

	/* A reported phase overrides inference ... */
	{ "phase-planning-over-children", "running", "planning", NULL, "running",
	  0, FALSE, FALSE, NULL, 0, CLAWT_BOARD_PLANNING, 0 },
	{ "phase-delegated-alone", "running", "delegated", NULL, NULL, 0, FALSE,
	  FALSE, NULL, 0, CLAWT_BOARD_DELEGATED, 0 },
	{ "phase-reviewing", "running", "reviewing", NULL, NULL, 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_REVIEW, 0 },
	{ "phase-gating", "running", "gating", NULL, "running", 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_GATING, 0 },
	{ "phase-landing", "running", "landing", NULL, NULL, 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_GATING, 0 },
	{ "phase-gating-over-review", "running", "gating", NULL, "review-owner",
	  0, FALSE, FALSE, NULL, 0, CLAWT_BOARD_GATING, 0 },
	{ "phase-blocked", "running", "blocked", NULL, NULL, 0, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_PHASE },

	/* ... but not what needs somebody. */
	{ "phase-gating-child-failed", "running", "gating", NULL, "failed", 0,
	  FALSE, FALSE, NULL, 0, CLAWT_BOARD_BLOCKED,
	  CLAWT_BOARD_BLOCK_CHILD_FAILED },
	{ "phase-gating-quiet", "running", "gating", NULL, NULL, 0, TRUE, FALSE,
	  NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_QUIET },
	{ "phase-gating-after-failure-seen", "running", "gating", NULL,
	  "failed-ack", 0, FALSE, FALSE, NULL, 0, CLAWT_BOARD_GATING, 0 },

	/* Its own result in review. */
	{ "own-review", "review", NULL, NULL, NULL, 0, FALSE, FALSE, NULL, 0,
	  CLAWT_BOARD_REVIEW, 0 },
	{ "own-review-decision", "review", NULL, NULL, NULL, 1, FALSE, FALSE,
	  NULL, 0, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_DECISION },

	/* Ended. */
	{ "completed-in-window", "completed", NULL, NULL, NULL, 0, FALSE, FALSE,
	  NULL, FIN_IN, CLAWT_BOARD_DONE, 0 },
	{ "completed-before-window", "completed", NULL, NULL, NULL, 0, FALSE,
	  FALSE, NULL, FIN_OLD, CLAWT_BOARD_HIDDEN, 0 },
	{ "failed-in-window", "failed", NULL, NULL, NULL, 0, FALSE, FALSE, NULL,
	  FIN_IN, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_FAILED },
	{ "stalled-in-window", "stalled", NULL, NULL, NULL, 0, FALSE, FALSE,
	  NULL, FIN_IN, CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BLOCK_STALLED },
	{ "failed-before-window", "failed", NULL, NULL, NULL, 0, FALSE, FALSE,
	  NULL, FIN_OLD, CLAWT_BOARD_HIDDEN, 0 },
	{ "cancelled", "cancelled", NULL, NULL, NULL, 0, FALSE, FALSE, NULL,
	  FIN_IN, CLAWT_BOARD_HIDDEN, 0 },

	/* Not this board's. */
	{ "somebody-elses", "running", NULL, NULL, NULL, 0, FALSE, FALSE,
	  "leopard", 0, CLAWT_BOARD_HIDDEN, 0 },
};

/* One child of a table row, by kind. */
static ClawtTask *
make_child(
	const gchar *kind,
	guint        index
){
	g_autofree gchar *id = g_strdup_printf("child-%u", index);

	if (g_strcmp0(kind, "running") == 0)
		return make_task(id, "wildebuild", "running", "card", NOW - HOUR,
		                 NOW - 5 * MINUTE, NOW - 5 * MINUTE, 0, NULL);
	if (g_strcmp0(kind, "pending") == 0)
		return make_task(id, "wildebuild", "pending", "card", NOW - HOUR, 0,
		                 NOW - HOUR, 0, NULL);
	if (g_strcmp0(kind, "completed") == 0)
		return make_task(id, "wildebuild", "completed", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 40 * MINUTE, NOW - 40 * MINUTE, NULL);
	if (g_strcmp0(kind, "failed") == 0 || g_strcmp0(kind, "failed-ack") == 0)
		return make_task(id, "wildebuild", "failed", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 10 * MINUTE, NOW - 10 * MINUTE,
		                 "\"reason\":\"the build broke\",");
	if (g_strcmp0(kind, "stalled") == 0)
		return make_task(id, "wildebuild", "stalled", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 10 * MINUTE, NOW - 10 * MINUTE, NULL);
	if (g_strcmp0(kind, "review-owner") == 0)
		return make_task(id, "wildebuild", "review", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 30 * MINUTE, 0,
		                 "\"reviewer\":\"oxpecker\",");
	if (g_strcmp0(kind, "review-other") == 0)
		return make_task(id, "wildebuild", "review", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 30 * MINUTE, 0,
		                 "\"reviewer\":\"user\",");
	if (g_strcmp0(kind, "waits-operator") == 0)
		return make_task(id, "wildebuild", "blocked", "card", NOW - HOUR,
		                 NOW - HOUR, NOW - 3 * MINUTE, 0,
		                 "\"waiting_on\":\"operator\",\"question\":\"which?\",");

	g_assert_not_reached();
	return NULL;
}

static void
test_classify_table(void)
{
	guint r;

	for (r = 0; r < G_N_ELEMENTS(classify_rows); r++) {
		const ClassifyRow *row = &classify_rows[r];
		g_autoptr(GPtrArray) children = g_ptr_array_new_with_free_func(
			(GDestroyNotify)clawt_task_free);
		g_autoptr(GString) extra = g_string_new(NULL);
		g_autoptr(ClawtTask) task = NULL;
		gint64 heard = row->quiet ? 30 * MINUTE : 5 * MINUTE;
		ClawtBoardFacts facts;
		ClawtBoardBlocks blocks = CLAWT_BOARD_BLOCK_NONE;
		ClawtBoardColumn column;

		/*
		 * The phase was reported 20 minutes ago (or when last heard of, if
		 * that was longer): before a child failed 10 minutes ago, so the
		 * report is not the agent acknowledging the failure.
		 */
		if (row->phase != NULL)
			g_string_append_printf(extra, "\"phase\":\"%s\",\"phase_at\":%"
			                       G_GINT64_FORMAT ",", row->phase,
			                       NOW - MAX(heard, 20 * MINUTE));
		if (row->waiting_on != NULL)
			g_string_append_printf(extra, "\"waiting_on\":\"%s\","
			                       "\"question\":\"go?\",", row->waiting_on);
		if (g_strcmp0(row->state, "review") == 0)
			g_string_append(extra, "\"reviewer\":\"chief\",");

		/* A note after the child failed: the agent has seen it. */
		if (row->children != NULL && strstr(row->children, "failed-ack"))
			g_string_append_printf(extra, "\"progress_at\":%" G_GINT64_FORMAT
			                       ",", NOW - 2 * MINUTE);

		task = make_task("card", row->owner != NULL ? row->owner : "oxpecker",
		                 row->state, NULL, NOW - 3 * HOUR,
		                 g_strcmp0(row->state, "pending") == 0 ? 0 : NOW - heard,
		                 NOW - heard, row->finished, extra->str);

		if (row->children != NULL) {
			g_auto(GStrv) kinds = g_strsplit(row->children, ",", -1);
			guint k;

			for (k = 0; kinds[k] != NULL; k++)
				g_ptr_array_add(children, make_child(kinds[k], k));
		}

		facts.task = task;
		facts.children = children;
		facts.owner = "oxpecker";
		facts.now = NOW;
		facts.done_since = WINDOW_START;
		facts.quiet_minutes = 20;
		facts.decisions = row->decisions;
		facts.wait_pending = row->wait_pending;

		column = clawt_board_classify(&facts, &blocks);

		if (column != row->expect || blocks != row->blocks)
			g_error("%s: got %s (blocks 0x%x), wanted %s (blocks 0x%x)",
			        row->name, clawt_board_column_nick(column), blocks,
			        clawt_board_column_nick(row->expect), row->blocks);
	}
}

/* Every column has a nickname and a label, so no client draws "(null)". */
static void
test_columns_named(void)
{
	GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_BOARD_COLUMN);
	guint i;

	for (i = 0; i < klass->n_values; i++) {
		ClawtBoardColumn column = (ClawtBoardColumn)klass->values[i].value;

		g_assert_nonnull(clawt_board_column_nick(column));
		g_assert_cmpstr(clawt_board_column_label(column), !=, "Unknown");
	}

	g_assert_cmpstr(clawt_board_block_nick(CLAWT_BOARD_BLOCK_QUIET), ==,
	                "quiet");
	g_assert_null(clawt_board_block_nick(CLAWT_BOARD_BLOCK_QUIET |
	                                     CLAWT_BOARD_BLOCK_PHASE));
	g_type_class_unref(klass);
}

/* ── Quiet, against an injected clock ────────────────────────────── */

static void
test_quiet_clock(void)
{
	g_autoptr(ClawtTask) task = running_task("q", "oxpecker", 0, NULL);
	g_autoptr(GPtrArray) children = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_task_free);
	gint64 start = clawt_task_get_started_at(task);
	gint64 quiet_at = 0;

	/* Started at NOW: quiet 20 minutes later, and not a second before. */
	g_assert_false(clawt_board_is_quiet(task, NULL, start + 20 * MINUTE - 1, 20,
	                                    FALSE, &quiet_at));
	g_assert_cmpint(quiet_at, ==, start + 20 * MINUTE);
	g_assert_true(clawt_board_is_quiet(task, NULL, start + 20 * MINUTE, 20,
	                                   FALSE, &quiet_at));

	/* 0 turns the rule off. */
	g_assert_false(clawt_board_is_quiet(task, NULL, start + 10 * HOUR, 0,
	                                    FALSE, &quiet_at));
	g_assert_cmpint(quiet_at, ==, 0);

	/* A wait the agent set is a reason for silence. */
	g_assert_false(clawt_board_is_quiet(task, NULL, start + HOUR, 20, TRUE,
	                                    NULL));

	/* A progress report moves the window... */
	clawt_task_set_progress_at(task, start + 10 * MINUTE);
	g_assert_false(clawt_board_is_quiet(task, NULL, start + 29 * MINUTE, 20,
	                                    FALSE, NULL));
	g_assert_true(clawt_board_is_quiet(task, NULL, start + 30 * MINUTE, 20,
	                                   FALSE, NULL));

	/* ... so does a streamed step, and an older one changes nothing. */
	clawt_task_note_activity(task, start + 25 * MINUTE);
	clawt_task_note_activity(task, start + 15 * MINUTE);
	g_assert_cmpint(clawt_task_get_activity_at(task), ==, start + 25 * MINUTE);
	g_assert_false(clawt_board_is_quiet(task, NULL, start + 44 * MINUTE, 20,
	                                    FALSE, &quiet_at));
	g_assert_cmpint(quiet_at, ==, start + 45 * MINUTE);

	/* A running child heard of recently keeps its parent from being quiet. */
	g_ptr_array_add(children, make_task("c1", "leopard", "running", "q",
	                                    start, start, start + 50 * MINUTE, 0,
	                                    NULL));
	g_assert_false(clawt_board_is_quiet(task, children, start + 60 * MINUTE, 20,
	                                    FALSE, NULL));
	g_assert_true(clawt_board_is_quiet(task, children, start + 70 * MINUTE, 20,
	                                   FALSE, NULL));

	/* One queued with its worker is waiting on somebody: never quiet. */
	g_ptr_array_add(children, make_task("c2", "leopard", "pending", "q",
	                                    start, 0, start, 0, NULL));
	g_assert_false(clawt_board_is_quiet(task, children, start + 10 * HOUR,
	                                    20, FALSE, &quiet_at));
	g_assert_cmpint(quiet_at, ==, 0);

	/* Only running work goes quiet. */
	{
		g_autoptr(ClawtTask) pending = make_task("p", "oxpecker", "pending",
		                                         NULL, NOW - 10 * HOUR, 0,
		                                         NOW - 10 * HOUR, 0, NULL);

		g_assert_false(clawt_board_is_quiet(pending, NULL, NOW, 20, FALSE,
		                                    NULL));
	}
}

/* ── Phase: validated, kept, cleared on a new attempt ────────────── */

static void
test_phase_validation(void)
{
	static const gchar *const good[] = {
		"planning", "delegated", "reviewing", "gating", "landing", "blocked"
	};
	static const gchar *const bad[] = {
		"none", "NONE", "", "building", "review", "done", "gating "
	};
	guint i;

	/*
	 * Case is forgiven, as for every nickname clawtilla reads
	 * (clawt_enum_from_nick()); what is stored is the canonical spelling.
	 */
	{
		ClawtTaskPhase phase = CLAWT_TASK_PHASE_NONE;

		g_assert_true(clawt_task_phase_parse("Gating", &phase, NULL));
		g_assert_cmpstr(clawt_task_phase_to_nick(phase), ==, "gating");
	}

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		ClawtTaskPhase phase = CLAWT_TASK_PHASE_NONE;
		g_autoptr(GError) error = NULL;

		g_assert_true(clawt_task_phase_parse(good[i], &phase, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(clawt_task_phase_to_nick(phase), ==, good[i]);
	}

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_task_phase_parse(bad[i], NULL, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
		/* The message says what would have been accepted. */
		g_assert_nonnull(strstr(error->message, "planning, delegated, "
		                        "reviewing, gating, landing or blocked"));
	}

	{
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_task_phase_parse(NULL, NULL, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	}
}

static void
test_phase_lifecycle(void)
{
	g_autoptr(ClawtTaskManager) manager = clawt_task_manager_new();
	g_autoptr(GError) error = NULL;
	ClawtTask *task;
	g_autoptr(JsonNode) saved = NULL;
	g_autoptr(ClawtTask) restored = NULL;
	gint64 at = 0;
	guint i;

	task = clawt_task_manager_create(manager, "chief", "oxpecker",
	                                 "Fix the export", NULL, &error);
	g_assert_no_error(error);

	/* Reported with progress: running now, and in that phase. */
	g_assert_true(clawt_task_manager_note_progress_in_phase(
		manager, clawt_task_get_id(task), "building", CLAWT_TASK_PHASE_GATING));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);
	g_assert_cmpint(clawt_task_get_phase(task), ==, CLAWT_TASK_PHASE_GATING);
	g_assert_cmpint(clawt_task_get_progress_at(task), >, 0);

	/* The same phase again is not a new report. */
	g_assert_false(clawt_task_set_phase(task, CLAWT_TASK_PHASE_GATING, 0));
	g_assert_cmpuint(clawt_task_get_phase_count(task), ==, 1);

	g_assert_true(clawt_task_manager_set_phase(manager,
	                                           clawt_task_get_id(task),
	                                           CLAWT_TASK_PHASE_LANDING,
	                                           &error));
	g_assert_no_error(error);
	g_assert_cmpuint(clawt_task_get_phase_count(task), ==, 2);
	g_assert_cmpint(clawt_task_get_phase_nth(task, 1, &at), ==,
	                CLAWT_TASK_PHASE_LANDING);
	g_assert_cmpint(at, >, 0);

	/* Kept across a save. */
	saved = clawt_task_to_json(task);
	restored = clawt_task_new_from_json(saved, &error);
	g_assert_no_error(error);
	g_assert_cmpint(clawt_task_get_phase(restored), ==,
	                CLAWT_TASK_PHASE_LANDING);
	g_assert_cmpuint(clawt_task_get_phase_count(restored), ==, 2);
	g_assert_cmpint(clawt_task_get_state_at(restored), ==,
	                clawt_task_get_state_at(task));
	g_assert_cmpint(clawt_task_get_progress_at(restored), ==,
	                clawt_task_get_progress_at(task));

	/* Going back to pending -- here a retry -- clears it, with a mark. */
	g_assert_true(clawt_task_manager_fail(manager, clawt_task_get_id(task),
	                                      "the runner died"));
	g_assert_true(clawt_task_manager_retry(manager, clawt_task_get_id(task),
	                                       "user", &error));
	g_assert_no_error(error);
	g_assert_cmpint(clawt_task_get_phase(task), ==, CLAWT_TASK_PHASE_NONE);
	g_assert_cmpuint(clawt_task_get_phase_count(task), ==, 3);
	g_assert_cmpint(clawt_task_get_phase_nth(task, 2, NULL), ==,
	                CLAWT_TASK_PHASE_NONE);

	/* Completing with a phase records it before the state moves. */
	g_assert_true(clawt_task_manager_complete_in_phase(
		manager, clawt_task_get_id(task), "landed 15864d8e",
		CLAWT_TASK_PHASE_LANDING));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);
	g_assert_cmpint(clawt_task_get_phase(task), ==, CLAWT_TASK_PHASE_LANDING);

	/* A finished task takes no more reports. */
	g_assert_false(clawt_task_manager_set_phase(manager,
	                                            clawt_task_get_id(task),
	                                            CLAWT_TASK_PHASE_GATING,
	                                            &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(clawt_task_manager_set_phase(manager, "task-nope",
	                                            CLAWT_TASK_PHASE_GATING,
	                                            &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* The history is bounded: the oldest go first. */
	for (i = 0; i < 40; i++)
		clawt_task_set_phase(task, (i % 2) ? CLAWT_TASK_PHASE_GATING
		                                   : CLAWT_TASK_PHASE_PLANNING,
		                     1000 + i);
	g_assert_cmpuint(clawt_task_get_phase_count(task), ==, 32);
	(void)clawt_task_get_phase_nth(task, 31, &at);
	g_assert_cmpint(at, ==, 1039);
	(void)clawt_task_get_phase_nth(task, 0, &at);
	g_assert_cmpint(at, ==, 1008);
}

/* A task saved before 0.78.0 has no state_at: the latest stamp stands in. */
static void
test_old_task_reads(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtTask) task = NULL;

	json_parser_load_from_data(parser,
		"{\"id\":\"task-old\",\"prompt\":\"x\",\"state\":\"completed\","
		"\"created_at\":100,\"started_at\":150,\"finished_at\":400,"
		"\"phase\":\"from-a-later-build\"}", -1, &error);
	g_assert_no_error(error);

	task = clawt_task_new_from_json(json_parser_get_root(parser), &error);
	g_assert_no_error(error);
	g_assert_cmpint(clawt_task_get_state_at(task), ==, 400);
	g_assert_cmpint(clawt_task_get_phase(task), ==, CLAWT_TASK_PHASE_NONE);
	g_assert_cmpuint(clawt_task_get_phase_count(task), ==, 0);
}

/* ── The timeline ────────────────────────────────────────────────── */

static JsonArray *
spans(JsonNode *timeline, const gchar *name)
{
	return json_object_get_array_member(json_node_get_object(timeline), name);
}

static void
assert_span(JsonArray *array, guint index, const gchar *kind,
            const gchar *name, gint64 from, gint64 to)
{
	JsonObject *span = json_array_get_object_element(array, index);

	g_assert_cmpstr(json_object_get_string_member(span, kind), ==, name);
	g_assert_cmpint(json_object_get_int_member(span, "from"), ==, from);
	if (to > 0)
		g_assert_cmpint(json_object_get_int_member(span, "to"), ==, to);
	else
		g_assert_false(json_object_has_member(span, "to"));
}

static void
test_timeline_from_journal(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtTask) task = make_task(
		"t", "oxpecker", "running", NULL, 100, 150, 450, 0,
		"\"phases\":[{\"phase\":\"gating\",\"at\":200},"
		"{\"phase\":\"landing\",\"at\":250},{\"phase\":\"none\",\"at\":400},"
		"{\"phase\":\"planning\",\"at\":460}],\"phase\":\"planning\",");
	g_autoptr(JsonNode) timeline = NULL;
	JsonArray *states;
	JsonArray *phases;

	/*
	 * A row per change and per event that changed nothing: the progress
	 * note at 160 extends the running span rather than starting one.
	 */
	json_parser_load_from_data(parser,
		"[{\"at\":100,\"state\":\"pending\",\"what\":\"created\"},"
		"{\"at\":150,\"state\":\"running\"},"
		"{\"at\":160,\"state\":\"running\",\"what\":\"progress noted\"},"
		"{\"at\":300,\"state\":\"review\"},"
		"{\"at\":400,\"state\":\"pending\",\"what\":\"changes asked\"},"
		"{\"at\":450,\"state\":\"running\"}]", -1, &error);
	g_assert_no_error(error);

	timeline = clawt_board_timeline(task,
	                                json_node_get_array(json_parser_get_root(parser)),
	                                NOW);
	g_assert_cmpstr(json_object_get_string_member(
		json_node_get_object(timeline), "source"), ==, "journal");

	states = spans(timeline, "states");
	g_assert_cmpuint(json_array_get_length(states), ==, 5);
	assert_span(states, 0, "state", "pending", 100, 150);
	assert_span(states, 1, "state", "running", 150, 300);
	assert_span(states, 2, "state", "review", 300, 400);
	assert_span(states, 3, "state", "pending", 400, 450);
	assert_span(states, 4, "state", "running", 450, 0);

	/* The none mark ends landing and starts nothing of its own. */
	phases = spans(timeline, "phases");
	g_assert_cmpuint(json_array_get_length(phases), ==, 3);
	assert_span(phases, 0, "phase", "gating", 200, 250);
	assert_span(phases, 1, "phase", "landing", 250, 400);
	assert_span(phases, 2, "phase", "planning", 460, 0);
}

static void
test_timeline_from_stamps(void)
{
	g_autoptr(ClawtTask) done = make_task("old", "oxpecker", "completed", NULL,
	                                      100, 150, 500, 500, NULL);
	g_autoptr(ClawtTask) queued = make_task("q", "oxpecker", "pending", NULL,
	                                        100, 0, 100, 0, NULL);
	g_autoptr(JsonNode) timeline = clawt_board_timeline(done, NULL, NOW);
	g_autoptr(JsonNode) waiting = clawt_board_timeline(queued, NULL, NOW);
	JsonArray *states;

	/* All an old task has: made, first run, finished. */
	g_assert_cmpstr(json_object_get_string_member(
		json_node_get_object(timeline), "source"), ==, "stamps");
	states = spans(timeline, "states");
	g_assert_cmpuint(json_array_get_length(states), ==, 3);
	assert_span(states, 0, "state", "pending", 100, 150);
	assert_span(states, 1, "state", "running", 150, 500);
	assert_span(states, 2, "state", "completed", 500, 0);
	g_assert_cmpuint(json_array_get_length(spans(timeline, "phases")), ==, 0);

	states = spans(waiting, "states");
	g_assert_cmpuint(json_array_get_length(states), ==, 1);
	assert_span(states, 0, "state", "pending", 100, 0);
}

/* ── Where a result landed; windows; durations ───────────────────── */

static void
test_result_refs(void)
{
	static const struct {
		const gchar *result;
		const gchar *commit;
		const gchar *branch;
	} cases[] = {
		{ "Done: committed 15864d8e on feat/lql-9-query.", "15864d8e",
		  "feat/lql-9-query" },
		{ "Landed `ba688083` (branch epic/5-lql-filtering)", "ba688083",
		  "epic/5-lql-filtering" },
		{ "pushed to branch zach/try-this", NULL, "zach/try-this" },
		/* Hex-looking words without a digit, and paths, are neither. */
		{ "decade of added faced src/task/clawt-board.c", NULL, NULL },
		{ "fix in 0b2bf57a, see also 42be1fe5", "0b2bf57a", NULL },
		{ NULL, NULL, NULL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *commit = NULL;
		g_autofree gchar *branch = NULL;
		gboolean found = clawt_board_result_refs(cases[i].result, &commit,
		                                         &branch);

		g_assert_cmpstr(commit, ==, cases[i].commit);
		g_assert_cmpstr(branch, ==, cases[i].branch);
		g_assert_true(found == (cases[i].commit != NULL ||
		                        cases[i].branch != NULL));
	}
}

static void
test_window_and_durations(void)
{
	g_autoptr(GError) error = NULL;
	gint64 since = 0;
	g_autofree gchar *a = clawt_board_duration_label(45);
	g_autofree gchar *b = clawt_board_duration_label(12 * MINUTE + 30);
	g_autofree gchar *c = clawt_board_duration_label(3 * HOUR + 5 * MINUTE);
	g_autofree gchar *d = clawt_board_duration_label(2 * 86400 + 4 * HOUR);
	g_autofree gchar *e = clawt_board_duration_label(-5);

	g_assert_true(clawt_board_parse_window("6h", NOW, &since, &error));
	g_assert_cmpint(since, ==, NOW - 6 * HOUR);
	g_assert_true(clawt_board_parse_window("7d", NOW, &since, &error));
	g_assert_cmpint(since, ==, NOW - 7 * 86400);

	/* Today is local midnight: inside the last day, never after now. */
	g_assert_true(clawt_board_parse_window(NULL, NOW, &since, &error));
	g_assert_cmpint(since, <=, NOW);
	g_assert_cmpint(since, >, NOW - 86400);
	g_assert_no_error(error);

	g_assert_false(clawt_board_parse_window("yesterday", NOW, &since, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(clawt_board_parse_window("0h", NOW, &since, &error));
	g_clear_error(&error);
	g_assert_false(clawt_board_parse_window("91d", NOW, &since, &error));
	g_clear_error(&error);

	g_assert_cmpstr(a, ==, "45s");
	g_assert_cmpstr(b, ==, "12m");
	g_assert_cmpstr(c, ==, "3h05m");
	g_assert_cmpstr(d, ==, "2d04h");
	g_assert_cmpstr(e, ==, "0s");
}

/* ── The acceptance case: a fixture store, a board, its text ─────── */

typedef struct {
	gchar            *dir;
	ClawtTaskStore   *store;
	ClawtTaskManager *manager;
	GHashTable       *decisions;   /* task id -> count */
} BoardFixture;

static gboolean
fake_describe(const gchar *agent_id, ClawtBoardAgent *out, gpointer data)
{
	(void)data;

	out->state = g_strdup("running");
	out->team = g_strdup("forge");
	out->mailbox = g_strcmp0(agent_id, "oxpecker") == 0 ? 3 : 1;

	if (g_strcmp0(agent_id, "oxpecker") == 0) {
		out->name = g_strdup("Oxpecker");
		out->step_text = g_strdup("Bash: make -j20 test");
		out->step_tool = g_strdup("Bash");
		out->step_at = NOW - 2 * MINUTE;
		out->turn_task = g_strdup("plan");
		out->turn_started = NOW - 10 * MINUTE;
	} else {
		out->last_active = NOW - 7 * MINUTE;
	}

	return TRUE;
}

static guint
fake_decisions(const gchar *task_id, gpointer data)
{
	BoardFixture *fixture = data;

	return GPOINTER_TO_UINT(g_hash_table_lookup(fixture->decisions, task_id));
}

static gdouble
fake_cost(const gchar *task_id, gpointer data)
{
	(void)data;

	return g_str_has_prefix(task_id, "del") ? 1.25 : -1.0;
}

static GPtrArray *
fake_members(const gchar *team_id, gpointer data)
{
	GPtrArray *members = g_ptr_array_new_with_free_func(g_free);

	(void)data;

	if (g_strcmp0(team_id, "forge") == 0) {
		g_ptr_array_add(members, g_strdup("oxpecker"));
		g_ptr_array_add(members, g_strdup("wildebuild"));
		g_ptr_array_add(members, g_strdup("bufferlo"));
	}

	return members;
}

static const ClawtBoardSource fake_source = {
	fake_describe, fake_decisions, NULL, fake_cost, fake_members
};

static void
save(BoardFixture *fixture, ClawtTask *task)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(clawt_task_store_save(fixture->store, task, "fixture",
	                                    &error));
	g_assert_no_error(error);
	clawt_task_free(task);
}

/*
 * Oxpecker's eight top-level bug tasks -- four in the backlog (one held
 * for another), one it is on, two it delegated, one whose child waits
 * for its verdict -- with the workers' tasks under them.
 */
static void
board_setup(
	BoardFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;

	(void)data;

	fixture->dir = g_dir_make_tmp("clawt-board-XXXXXX", NULL);
	{
		g_autofree gchar *path = g_build_filename(fixture->dir, "tasks.db",
		                                          NULL);

		fixture->store = clawt_task_store_new(path, &error);
		g_assert_no_error(error);
	}
	fixture->decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, NULL);

	/* The backlog: three handed over, one held until bug-1 completes. */
	save(fixture, make_task("bug-1", "oxpecker", "pending", NULL,
	                        NOW - 4 * HOUR, 0, NOW - 4 * HOUR, 0, NULL));
	save(fixture, make_task("bug-2", "oxpecker", "pending", NULL,
	                        NOW - 3 * HOUR, 0, NOW - 3 * HOUR, 0, NULL));
	save(fixture, make_task("bug-3", "oxpecker", "pending", NULL,
	                        NOW - 2 * HOUR, 0, NOW - 2 * HOUR, 0, NULL));
	save(fixture, make_task("bug-4", "oxpecker", "blocked", NULL,
	                        NOW - 5 * HOUR, 0, NOW - 5 * HOUR, 0, NULL));
	g_assert_true(clawt_task_store_add_link(fixture->store, "bug-1", "bug-4",
	                                        &error));

	/* The one it is on, with its checkout. */
	save(fixture, running_task("plan", "oxpecker", 4 * MINUTE,
	     "\"checkout_repo\":\"/src/clawtilla\",\"checkout_mode\":\"clone\","
	     "\"checkout_branch\":\"clawtilla/plan\","
	     "\"progress_note\":\"reading the task store\\nand more\","));

	/* Two delegated, with workers' chips; del-b's second waits for its first. */
	save(fixture, running_task("del-a", "oxpecker", 6 * MINUTE, NULL));
	save(fixture, make_task("del-a-1", "wildebuild", "running", "del-a",
	                        NOW - HOUR, NOW - 50 * MINUTE, NOW - 3 * MINUTE, 0, NULL));
	save(fixture, running_task("del-b", "oxpecker", 6 * MINUTE, NULL));
	save(fixture, make_task("del-b-1", "bufferlo", "running", "del-b",
	                        NOW - HOUR, NOW - 40 * MINUTE, NOW - 2 * MINUTE, 0, NULL));
	save(fixture, make_task("del-b-2", "leopard", "blocked", "del-b",
	                        NOW - HOUR + MINUTE, 0, NOW - HOUR + MINUTE, 0, NULL));
	g_assert_true(clawt_task_store_add_link(fixture->store, "del-b-1",
	                                        "del-b-2", &error));

	/* One whose child has been waiting half an hour for oxpecker's verdict. */
	save(fixture, running_task("rev", "oxpecker", 30 * MINUTE, NULL));
	save(fixture, make_task("rev-1", "wildebuild", "review", "rev",
	                        NOW - 2 * HOUR, NOW - 2 * HOUR, NOW - 30 * MINUTE, 0,
	                        "\"reviewer\":\"oxpecker\","
	                        "\"result\":\"fixed in 0b2bf57a\","));

	fixture->manager = clawt_task_manager_new();
	clawt_task_manager_set_store(fixture->manager, fixture->store);
	g_assert_cmpuint(clawt_task_manager_load(fixture->manager, WINDOW_START,
	                                         &error), ==, 12);
	g_assert_no_error(error);
}

static void
board_teardown(
	BoardFixture  *fixture,
	gconstpointer  data
){
	(void)data;

	g_clear_object(&fixture->manager);
	g_clear_object(&fixture->store);
	g_clear_pointer(&fixture->decisions, g_hash_table_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
build_board(BoardFixture *fixture, gboolean timeline)
{
	ClawtBoardRequest request;

	memset(&request, 0, sizeof(request));
	request.agent = "oxpecker";
	request.window = "24h";
	request.done_since = WINDOW_START;
	request.now = NOW;
	request.quiet_minutes = 20;
	request.timeline = timeline;

	return clawt_board_build(fixture->manager, &request, &fake_source,
	                         fixture);
}

/* The cards of column @id, by their ids, in order. */
static gchar *
column_ids(JsonObject *board, const gchar *id)
{
	JsonArray *columns = json_object_get_array_member(board, "columns");
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;
	guint j;

	for (i = 0; i < json_array_get_length(columns); i++) {
		JsonObject *column = json_array_get_object_element(columns, i);
		JsonArray *cards;

		if (g_strcmp0(json_object_get_string_member(column, "id"), id) != 0)
			continue;

		cards = json_object_get_array_member(column, "cards");

		for (j = 0; j < json_array_get_length(cards); j++)
			g_string_append_printf(out, "%s%s", j > 0 ? "," : "",
			                       json_object_get_string_member(
			                           json_array_get_object_element(cards, j),
			                           "id"));
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

static JsonObject *
find_card(JsonObject *board, const gchar *task_id)
{
	JsonArray *columns = json_object_get_array_member(board, "columns");
	guint i;
	guint j;

	for (i = 0; i < json_array_get_length(columns); i++) {
		JsonArray *cards = json_object_get_array_member(
			json_array_get_object_element(columns, i), "cards");

		for (j = 0; j < json_array_get_length(cards); j++) {
			JsonObject *card = json_array_get_object_element(cards, j);

			if (g_strcmp0(json_object_get_string_member(card, "id"),
			              task_id) == 0)
				return card;
		}
	}

	return NULL;
}

static void
test_board_acceptance(
	BoardFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(JsonNode) node = build_board(fixture, TRUE);
	g_autoptr(GPtrArray) listed = NULL;
	JsonObject *board = json_node_get_object(node);
	JsonObject *header = json_object_get_object_member(board, "header");
	JsonObject *counts = json_object_get_object_member(header, "counts");
	JsonObject *totals = json_object_get_object_member(board, "totals");
	JsonObject *card;
	JsonObject *review;
	JsonArray *chips;
	JsonArray *columns;
	g_autofree gchar *backlog = column_ids(board, "backlog");
	g_autofree gchar *planning = column_ids(board, "planning");
	g_autofree gchar *delegated = column_ids(board, "delegated");
	g_autofree gchar *in_review = column_ids(board, "review");
	g_autofree gchar *blocked = column_ids(board, "blocked");

	(void)data;

	/* 4 backlog / 1 planning / 2 delegated / 1 review, nothing blocked. */
	g_assert_cmpstr(backlog, ==, "bug-1,bug-2,bug-3,bug-4");
	g_assert_cmpstr(planning, ==, "plan");
	g_assert_cmpstr(delegated, ==, "del-a,del-b");
	g_assert_cmpstr(in_review, ==, "rev");
	g_assert_cmpstr(blocked, ==, "");

	g_assert_cmpint(json_object_get_int_member(counts, "backlog"), ==, 4);
	g_assert_cmpint(json_object_get_int_member(counts, "planning"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(counts, "delegated"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(counts, "in_review"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(counts, "in_flight"), ==, 3);

	/* All seven columns, blocked first, always. */
	columns = json_object_get_array_member(board, "columns");
	g_assert_cmpuint(json_array_get_length(columns), ==, 7);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(columns, 0), "id"), ==, "blocked");

	/* The totals are what task.list {agent: oxpecker} reports. */
	listed = clawt_task_manager_list(fixture->manager, "oxpecker", TRUE);
	g_assert_cmpint(json_object_get_int_member(totals, "listed"), ==,
	                listed->len);
	g_assert_cmpint(json_object_get_int_member(totals, "cards"), ==, 8);
	g_assert_cmpint(json_object_get_int_member(totals, "cards") +
	                json_object_get_int_member(totals, "hidden") +
	                json_object_get_int_member(totals, "nested") -
	                json_object_get_int_member(totals, "from_history"), ==,
	                listed->len);

	/* The backlog in the order it will be taken; the held one last. */
	card = find_card(board, "bug-4");
	g_assert_cmpint(json_object_get_int_member(card, "position"), ==, 4);
	g_assert_cmpstr(json_array_get_string_element(
		json_object_get_array_member(card, "waiting_for"), 0), ==, "bug-1");
	g_assert_cmpint(json_object_get_int_member(find_card(board, "bug-1"),
	                                           "position"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(find_card(board, "bug-1"),
	                                           "age"), ==, 4 * HOUR);

	/* Worker chips, with the after: link between del-b's two. */
	card = find_card(board, "del-b");
	chips = json_object_get_array_member(card, "chips");
	g_assert_cmpuint(json_array_get_length(chips), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(chips, 0), "worker"), ==, "bufferlo");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(chips, 0), "state"), ==, "running");
	g_assert_cmpint(json_object_get_int_member(
		json_array_get_object_element(chips, 0), "elapsed"), ==, 40 * MINUTE);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(chips, 1), "worker"), ==, "leopard");
	g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(
		json_array_get_object_element(chips, 1), "after"), 0), ==, "del-b-1");
	g_assert_cmpfloat(json_object_get_double_member(card, "cost_usd"), ==,
	                  1.25);

	/* The review card says who waits on whom, and for how long. */
	review = json_object_get_object_member(find_card(board, "rev"), "review");
	g_assert_cmpstr(json_object_get_string_member(review, "task"), ==, "rev-1");
	g_assert_cmpstr(json_object_get_string_member(review, "worker"), ==,
	                "wildebuild");
	g_assert_cmpstr(json_object_get_string_member(review, "waiting_for"), ==,
	                "oxpecker");
	g_assert_cmpint(json_object_get_int_member(review, "wait"), ==, 30 * MINUTE);

	/* The card it is on: repository, branch, note, and its timeline. */
	card = find_card(board, "plan");
	g_assert_cmpstr(json_object_get_string_member(card, "repo"), ==,
	                "clawtilla");
	g_assert_cmpstr(json_object_get_string_member(card, "branch"), ==,
	                "clawtilla/plan");
	g_assert_true(json_object_has_member(card, "timeline"));
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(card, "timeline"), "source"), ==,
		"journal");
	g_assert_false(json_object_get_boolean_member(card, "quiet"));
	g_assert_cmpint(json_object_get_int_member(card, "quiet_at"), ==,
	                NOW - 4 * MINUTE + 20 * MINUTE);

	/* The header, from the source. */
	g_assert_cmpstr(json_object_get_string_member(header, "state"), ==,
	                "running");
	g_assert_cmpint(json_object_get_int_member(header, "mailbox"), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(header, "current_task"), "id"), ==,
		"plan");
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(header, "step"), "text"), ==,
		"Bash: make -j20 test");

	/*
	 * Lanes: the team (less oxpecker itself), then the chip workers not
	 * on it.  bufferlo is on del-b-1, which is its current task.
	 */
	{
		JsonArray *lanes = json_object_get_array_member(board, "lanes");
		JsonObject *lane;

		g_assert_cmpuint(json_array_get_length(lanes), ==, 3);
		g_assert_cmpstr(json_object_get_string_member(
			json_array_get_object_element(lanes, 0), "agent"), ==,
			"wildebuild");
		lane = json_array_get_object_element(lanes, 1);
		g_assert_cmpstr(json_object_get_string_member(lane, "agent"), ==,
		                "bufferlo");
		g_assert_cmpstr(json_object_get_string_member(
			json_object_get_object_member(lane, "current_task"), "id"), ==,
			"del-b-1");
		g_assert_cmpint(json_object_get_int_member(lane, "idle"), ==, 7 * MINUTE);
		g_assert_cmpstr(json_object_get_string_member(
			json_array_get_object_element(lanes, 2), "agent"), ==, "leopard");
		g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
			json_array_get_object_element(lanes, 2), "queue"), "queued"), ==, 1);
	}
}

/* A failed child or an operator decision pins that card under blocked. */
static void
test_board_pins_blocked(
	BoardFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(JsonNode) node = NULL;
	JsonObject *board;
	JsonArray *blocks;
	g_autofree gchar *blocked = NULL;
	g_autofree gchar *delegated = NULL;

	(void)data;

	g_assert_true(clawt_task_manager_fail(fixture->manager, "del-a-1",
	                                      "the build broke"));
	g_hash_table_insert(fixture->decisions, g_strdup("del-b"),
	                    GUINT_TO_POINTER(1));

	node = build_board(fixture, FALSE);
	board = json_node_get_object(node);
	blocked = column_ids(board, "blocked");
	delegated = column_ids(board, "delegated");

	g_assert_cmpstr(blocked, ==, "del-a,del-b");
	g_assert_cmpstr(delegated, ==, "");

	blocks = json_object_get_array_member(find_card(board, "del-a"), "blocks");
	g_assert_cmpuint(json_array_get_length(blocks), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(blocks, 0), "reason"), ==,
		"child_failed");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(blocks, 0), "task"), ==, "del-a-1");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(blocks, 0), "detail"), ==,
		"the build broke");

	blocks = json_object_get_array_member(find_card(board, "del-b"), "blocks");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(blocks, 0), "reason"), ==, "decision");
	g_assert_cmpint(json_object_get_int_member(
		json_array_get_object_element(blocks, 0), "count"), ==, 1);

	/* Without timeline asked for, there is none. */
	g_assert_false(json_object_has_member(find_card(board, "plan"),
	                                      "timeline"));

	/* The agent reporting since the failure unpins it. */
	g_assert_true(clawt_task_manager_note_progress(fixture->manager, "del-a",
	                                               "retrying it elsewhere"));
	clawt_task_set_progress_at(clawt_task_manager_get(fixture->manager,
	                                                  "del-a"), NOW);
	{
		g_autoptr(JsonNode) again = build_board(fixture, FALSE);
		g_autofree gchar *now_blocked = column_ids(json_node_get_object(again),
		                                           "blocked");

		g_assert_cmpstr(now_blocked, ==, "del-b");
	}
}

/* What leaves the board is counted, so the totals still add up. */
static void
test_board_done_and_hidden(
	BoardFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	JsonObject *board;
	JsonObject *totals;
	JsonObject *done;

	g_autoptr(GError) error = NULL;
	JsonObject *landed;
	JsonArray *chips;

	(void)data;

	/*
	 * Added to the store and read in as a restart would: one shipped an
	 * hour before NOW, and one of oxpecker's own tasks under del-a, which
	 * is a chip there and not a card of its own.
	 */
	save(fixture, make_task("shipped", "oxpecker", "completed", NULL,
	                        NOW - 3 * HOUR, NOW - 2 * HOUR, NOW - HOUR,
	                        NOW - HOUR,
	                        "\"result\":\"Landed: committed 15864d8e on "
	                        "feat/lql-9-query.\","));
	save(fixture, make_task("del-a-mine", "oxpecker", "running", "del-a",
	                        NOW - HOUR, NOW - HOUR, NOW - 5 * MINUTE, 0, NULL));
	g_assert_cmpuint(clawt_task_manager_load(fixture->manager, WINDOW_START,
	                                         &error), ==, 2);
	g_assert_no_error(error);

	/* Cancelled is never on the board. */
	g_assert_cmpuint(clawt_task_manager_cancel(fixture->manager, "bug-3",
	                                           "not wanted", "user"), ==, 1);

	node = build_board(fixture, FALSE);
	board = json_node_get_object(node);
	totals = json_object_get_object_member(board, "totals");
	listed = clawt_task_manager_list(fixture->manager, "oxpecker", TRUE);

	done = find_card(board, "shipped");
	g_assert_nonnull(done);
	landed = json_object_get_object_member(done, "done");
	g_assert_cmpstr(json_object_get_string_member(done, "column"), ==, "done");
	g_assert_cmpstr(json_object_get_string_member(landed, "commit"), ==,
	                "15864d8e");
	g_assert_cmpstr(json_object_get_string_member(landed, "branch"), ==,
	                "feat/lql-9-query");
	g_assert_cmpint(json_object_get_int_member(landed, "land"), ==, 2 * HOUR);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
		json_object_get_object_member(board, "header"), "counts"), "done"),
		==, 1);

	g_assert_null(find_card(board, "bug-3"));
	g_assert_null(find_card(board, "del-a-mine"));
	chips = json_object_get_array_member(find_card(board, "del-a"), "chips");
	g_assert_cmpuint(json_array_get_length(chips), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(chips, 1), "worker"), ==, "oxpecker");

	g_assert_cmpint(json_object_get_int_member(totals, "hidden"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(totals, "nested"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(totals, "cards"), ==, 8);
	g_assert_cmpint(json_object_get_int_member(totals, "cards") +
	                json_object_get_int_member(totals, "hidden") +
	                json_object_get_int_member(totals, "nested") -
	                json_object_get_int_member(totals, "from_history"), ==,
	                listed->len);
}

/*
 * The CLI's text is the JSON drawn: one section per column in the reply's
 * order with the JSON's count, every card under its own column's heading
 * and nowhere else, its worker chips under it, then the lanes.
 */
static void
test_board_text_matches_json(
	BoardFixture  *fixture,
	gconstpointer  data
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *board;
	JsonArray *columns;
	const gchar *cursor;
	guint i;
	guint j;

	(void)data;

	g_assert_true(clawt_task_manager_fail(fixture->manager, "del-a-1",
	                                      "the build broke"));

	node = build_board(fixture, TRUE);
	board = json_node_get_object(node);
	text = clawt_board_render_text(board);
	columns = json_object_get_array_member(board, "columns");

	/* In the TAP log, for a person checking what the CLI prints. */
	g_test_message("%s", text);

	g_assert_true(g_str_has_prefix(text, "oxpecker  running"));
	g_assert_nonnull(strstr(text, "now: Bash: make -j20 test  (2m ago)"));
	g_assert_nonnull(strstr(text, "mailbox 3 | backlog 4 | in flight 2 | "
	                              "in review 1 | blocked 1 | done 0"));

	cursor = text;

	for (i = 0; i < json_array_get_length(columns); i++) {
		JsonObject *column = json_array_get_object_element(columns, i);
		JsonArray *cards = json_object_get_array_member(column, "cards");
		g_autofree gchar *heading = g_strdup_printf("\n== %s (%u) ==\n",
			json_object_get_string_member(column, "label"),
			json_array_get_length(cards));
		const gchar *section = strstr(cursor, heading);
		const gchar *next;

		g_assert_nonnull(section);
		section += strlen(heading);
		next = strstr(section, "\n== ");

		/* Each card here, and before the next heading. */
		for (j = 0; j < json_array_get_length(cards); j++) {
			JsonObject *card = json_array_get_object_element(cards, j);
			JsonArray *chips = json_object_get_array_member(card, "chips");
			g_autofree gchar *line = g_strdup_printf(
				"  %s", json_object_get_string_member(card, "id"));
			const gchar *at = strstr(section, json_object_get_string_member(
				card, "id"));
			guint k;

			g_assert_nonnull(at);
			g_assert_true(next == NULL || at < next);

			for (k = 0; k < json_array_get_length(chips); k++) {
				JsonObject *chip = json_array_get_object_element(chips, k);
				const gchar *chip_at = strstr(at, json_object_get_string_member(
					chip, "id"));

				g_assert_nonnull(chip_at);
				g_assert_true(next == NULL || chip_at < next);
			}

			(void)line;
		}

		cursor = section;
	}

	/* The details a card line carries: blocked reason, review wait, note. */
	g_assert_nonnull(strstr(text, "blocked: child_failed del-a-1 (wildebuild): "
	                              "the build broke"));
	g_assert_nonnull(strstr(text, "review: rev-1 from wildebuild, waiting for "
	                              "oxpecker for 30m"));
	g_assert_nonnull(strstr(text, "note: reading the task store..."));
	g_assert_nonnull(strstr(text, "plan  clawtilla Fix bug plan  4m"));
	g_assert_nonnull(strstr(text, "$1.25"));
	g_assert_nonnull(strstr(text, "4. bug-4"));
	g_assert_nonnull(strstr(text, "(held)"));
	g_assert_nonnull(strstr(text, "after: del-b-1"));
	g_assert_nonnull(strstr(text, "\n== Lanes ==\n"));
	g_assert_nonnull(strstr(text, "  bufferlo     running    idle 7m  queued 0, "
	                              "running 1, mailbox 1\n"));
	g_assert_true(g_str_has_suffix(text, "\n"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/board/classify/table", test_classify_table);
	g_test_add_func("/board/classify/columns-named", test_columns_named);
	g_test_add_func("/board/quiet/clock", test_quiet_clock);
	g_test_add_func("/board/phase/validation", test_phase_validation);
	g_test_add_func("/board/phase/lifecycle", test_phase_lifecycle);
	g_test_add_func("/board/phase/old-task-reads", test_old_task_reads);
	g_test_add_func("/board/timeline/journal", test_timeline_from_journal);
	g_test_add_func("/board/timeline/stamps", test_timeline_from_stamps);
	g_test_add_func("/board/done/result-refs", test_result_refs);
	g_test_add_func("/board/window-and-durations", test_window_and_durations);

	g_test_add("/board/build/acceptance", BoardFixture, NULL, board_setup,
	           test_board_acceptance, board_teardown);
	g_test_add("/board/build/pins-blocked", BoardFixture, NULL, board_setup,
	           test_board_pins_blocked, board_teardown);
	g_test_add("/board/build/done-and-hidden", BoardFixture, NULL,
	           board_setup, test_board_done_and_hidden, board_teardown);
	g_test_add("/board/build/text-matches-json", BoardFixture, NULL,
	           board_setup, test_board_text_matches_json, board_teardown);

	return g_test_run();
}
