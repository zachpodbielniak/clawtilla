/*
 * test-busy-watch.c - A turn whose processes are using the CPU is working
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every test here runs against a fake process table.  A real tree cannot
 * be made to burn a known number of ticks, and a test that spins a CPU
 * to prove a CPU check is one that fails on a loaded build machine.
 *
 * The daemon half reaches the watchdog through
 * core/clawt-daemon-private.h, as test-daemon.c does: that header *is*
 * daemon-turn.c's interface, and a hermetic test has no libreclaw to
 * raise a typing frame for it.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

/* ── A fake /proc ────────────────────────────────────────────────── */

/* pid -> stat line, rewritten by each test as its processes "run". */
static GHashTable *fake_proc;

static gchar **
fake_list(
	gpointer user_data
){
	g_autoptr(GPtrArray) out = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;

	(void)user_data;

	g_hash_table_iter_init(&iter, fake_proc);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(out, g_strdup(key));

	g_ptr_array_add(out, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

static gchar *
fake_read(
	const gchar *pid,
	gpointer     user_data
){
	(void)user_data;

	return g_strdup(g_hash_table_lookup(fake_proc, pid));
}

/*
 * One process.  The name has a space and a parenthesis in it on purpose,
 * because a parser counting from the first ')' reads the wrong fields.
 */
static void
fake_process(
	gint   pid,
	gint   ppid,
	gint64 utime,
	gint64 cutime
){
	g_autofree gchar *line = NULL;

	line = g_strdup_printf("%d (my (odd) name) S %d %d %d 0 -1 4194304 "
		"100 0 0 0 %" G_GINT64_FORMAT " 0 %" G_GINT64_FORMAT " 0 20 0 1 0 "
		"12345 1000 100\n", pid, ppid, pid, pid, utime, cutime);

	g_hash_table_insert(fake_proc, g_strdup_printf("%d", pid),
		g_steal_pointer(&line));
}

static void
fake_reset(
	void
){
	if (fake_proc == NULL)
		fake_proc = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			g_free);

	g_hash_table_remove_all(fake_proc);
}

static ClawtBusyWatch *
fake_watch(
	void
){
	ClawtBusyWatch *watch = clawt_busy_watch_new();

	clawt_busy_watch_set_reader(watch, fake_list, fake_read, NULL, NULL);
	clawt_busy_watch_set_ticks_per_second(watch, 100);

	return watch;
}

/* ── Parsing ─────────────────────────────────────────────────────── */

static void
test_parse_counts_from_the_last_parenthesis(
	void
){
	gint ppid = 0;
	gint64 own = 0;
	gint64 children = 0;

	g_assert_true(clawt_busy_parse_stat(
		"42 (a) b) S 7 42 42 0 -1 0 0 0 0 0 30 12 5 3 20 0 1 0 1 1 1",
		&ppid, &own, &children));
	g_assert_cmpint(ppid, ==, 7);
	g_assert_cmpint(own, ==, 42);
	g_assert_cmpint(children, ==, 8);

	g_assert_false(clawt_busy_parse_stat("garbage", &ppid, &own, &children));
	g_assert_false(clawt_busy_parse_stat("1 (x) S 0", &ppid, &own,
		&children));
	g_assert_false(clawt_busy_parse_stat(NULL, &ppid, &own, &children));
}

/* ── The tree ────────────────────────────────────────────────────── */

/*
 * The root's own time is bookkeeping and is left out; its reaped
 * children count, and so does every descendant however deep -- listed
 * child-before-parent, because a reused low pid can be the child of a
 * high one and a single pass would miss it.
 */
static void
test_tree_sums_descendants_not_the_root(
	void
){
	g_autoptr(ClawtBusyWatch) watch = NULL;
	guint processes = 0;

	fake_reset();
	fake_process(100, 1, 9000, 7);       /* the root: own time ignored */
	fake_process(50, 300, 40, 2);        /* grandchild, lower pid */
	fake_process(300, 100, 10, 0);       /* child */
	fake_process(400, 1, 5000, 0);       /* somebody else's process */

	watch = fake_watch();
	g_assert_cmpuint(clawt_busy_watch_refresh(watch), ==, 4);

	g_assert_cmpint(clawt_busy_watch_tree_ticks(watch, 100, &processes),
		==, 7 + 10 + 42);
	g_assert_cmpuint(processes, ==, 2);

	/* Not in the table is a different answer from "used nothing". */
	g_assert_cmpint(clawt_busy_watch_tree_ticks(watch, 999, NULL), ==, -1);
}

/* ── Sampling ────────────────────────────────────────────────────── */

/*
 * Reaches the threshold exactly: 5 % of a core over ten seconds is fifty
 * ticks at a hundred a second, and forty-nine is not enough.
 */
static void
test_sample_reaches_the_threshold_exactly(
	void
){
	g_autoptr(ClawtBusyWatch) watch = NULL;
	gint64 now = 1000 * G_USEC_PER_SEC;
	gint percent = 0;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(200, 100, 0, 0);

	watch = fake_watch();
	clawt_busy_watch_refresh(watch);

	/* A first reading has nothing to compare with. */
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5,
		&percent));
	g_assert_cmpint(percent, ==, -1);

	fake_process(200, 100, 49, 0);
	clawt_busy_watch_refresh(watch);
	now += 10 * G_USEC_PER_SEC;
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5,
		&percent));
	g_assert_cmpint(percent, ==, 4);

	fake_process(200, 100, 49 + 50, 0);
	clawt_busy_watch_refresh(watch);
	now += 10 * G_USEC_PER_SEC;
	g_assert_true(clawt_busy_watch_sample(watch, "a", 100, now, 5,
		&percent));
	g_assert_cmpint(percent, ==, 5);

	/* Zero is off, however busy the tree is. */
	fake_process(200, 100, 5000, 0);
	clawt_busy_watch_refresh(watch);
	now += 10 * G_USEC_PER_SEC;
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 0,
		&percent));
	g_assert_cmpint(percent, >, 0);
}

/*
 * A process that exits takes its time with it until its parent waits,
 * so the total can drop.  Down is not work, and the drop becomes the new
 * baseline rather than a debt the next sweep has to pay off.
 */
static void
test_a_falling_total_is_not_work(
	void
){
	g_autoptr(ClawtBusyWatch) watch = NULL;
	gint64 now = 1000 * G_USEC_PER_SEC;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(200, 100, 500, 0);

	watch = fake_watch();
	clawt_busy_watch_refresh(watch);
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));

	g_hash_table_remove(fake_proc, "200");
	clawt_busy_watch_refresh(watch);
	now += 5 * G_USEC_PER_SEC;
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));

	/* Measured from the drop, not from the 500 before it. */
	fake_process(201, 100, 30, 0);
	clawt_busy_watch_refresh(watch);
	now += 5 * G_USEC_PER_SEC;
	g_assert_true(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));
}

/*
 * A restarted agent is a different process.  Its whole total is not
 * "gained since last time", or a restart would be counted as a burst of
 * work.
 */
static void
test_a_new_pid_starts_a_new_baseline(
	void
){
	g_autoptr(ClawtBusyWatch) watch = NULL;
	gint64 now = 1000 * G_USEC_PER_SEC;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(700, 1, 0, 0);
	fake_process(701, 700, 9000, 0);

	watch = fake_watch();
	clawt_busy_watch_refresh(watch);
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));

	now += 5 * G_USEC_PER_SEC;
	g_assert_false(clawt_busy_watch_sample(watch, "a", 700, now, 5, NULL));
}

/*
 * Two keys, because a scalar baseline would compare one agent's tree
 * with the other's -- the shape the turn watch's budget once had.
 */
static void
test_two_agents_keep_their_own_baselines(
	void
){
	g_autoptr(ClawtBusyWatch) watch = NULL;
	gint64 now = 1000 * G_USEC_PER_SEC;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(101, 100, 0, 0);
	fake_process(200, 1, 0, 0);
	fake_process(201, 200, 0, 0);

	watch = fake_watch();
	clawt_busy_watch_refresh(watch);
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));
	g_assert_false(clawt_busy_watch_sample(watch, "b", 200, now, 5, NULL));

	fake_process(101, 100, 1000, 0);
	clawt_busy_watch_refresh(watch);
	now += 5 * G_USEC_PER_SEC;
	g_assert_true(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));
	g_assert_false(clawt_busy_watch_sample(watch, "b", 200, now, 5, NULL));

	/* Forgetting one leaves the other comparing as before. */
	clawt_busy_watch_forget(watch, "a");
	fake_process(101, 100, 2000, 0);
	fake_process(201, 200, 1000, 0);
	clawt_busy_watch_refresh(watch);
	now += 5 * G_USEC_PER_SEC;
	g_assert_false(clawt_busy_watch_sample(watch, "a", 100, now, 5, NULL));
	g_assert_true(clawt_busy_watch_sample(watch, "b", 200, now, 5, NULL));
}

/* ── Through the daemon's watchdog ───────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	ClawtDaemon  *daemon;
	GMainContext *context;
} Fixture;

static gint64 fake_now;

static gint64
fake_clock(
	gpointer user_data
){
	(void)user_data;

	return fake_now;
}

/* busy is pid 100, idle is pid 200; anybody else has no process. */
static gint
fake_pid(
	ClawtDaemon *self,
	const gchar *agent_id
){
	(void)self;

	if (g_strcmp0(agent_id, "busy") == 0)
		return 100;
	if (g_strcmp0(agent_id, "idle") == 0)
		return 200;
	return 0;
}

static void
fixture_setup(
	Fixture     *fixture,
	const gchar *agents_yaml
){
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-busy-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
		NULL);

	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, agents_yaml);

	g_file_set_contents(fixture->config_path, yaml, -1, &error);
	g_assert_no_error(error);

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config_path,
		fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));

	clawt_daemon_turn_set_pid_func(fixture->daemon, fake_pid);
	clawt_busy_watch_set_reader(
		clawt_daemon_turn_get_busy_watch(fixture->daemon), fake_list,
		fake_read, NULL, NULL);
	clawt_busy_watch_set_ticks_per_second(
		clawt_daemon_turn_get_busy_watch(fixture->daemon), 100);

	fake_now = 1000 * G_USEC_PER_SEC;
	clawt_turn_watch_set_clock(fixture->daemon->turn_watch, fake_clock,
		NULL, NULL);
}

static void
fixture_teardown(
	Fixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;

	g_clear_pointer(&fixture->context, g_main_context_unref);
	clawt_test_remove_tree(fixture->dir);
	g_clear_pointer(&fixture->dir, g_free);
	g_clear_pointer(&fixture->config_path, g_free);
}

/* Moves the fake clock and runs one sweep, as the timer would. */
static void
advance_and_sweep(
	Fixture *fixture,
	gint     seconds
){
	fake_now += (gint64)seconds * G_USEC_PER_SEC;
	clawt_daemon_turn_sweep_now(fixture->daemon);
}

/*
 * Reaches the budget both ways in one run, with two agents: a silent
 * turn whose tree keeps using the CPU outlives its budget, and a silent
 * turn beside it whose tree is asleep is stopped on time.  Then the busy
 * one's command stops working, and it is stopped too.
 */
static void
test_a_busy_tree_outlives_the_budget(
	void
){
	Fixture fixture = { 0 };
	ClawtTurnWatch *watch;
	gint64 ticks = 0;
	gint i;

	fixture_setup(&fixture,
		"agents:\n"
		"  - id: busy\n"
		"    runtime:\n"
		"      turn_timeout_seconds: 60\n"
		"  - id: idle\n"
		"    runtime:\n"
		"      turn_timeout_seconds: 60\n");
	watch = fixture.daemon->turn_watch;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(101, 100, 0, 0);
	fake_process(200, 1, 0, 0);
	fake_process(201, 200, 0, 0);

	clawt_daemon_turn_begin(fixture.daemon, "busy", NULL);
	clawt_daemon_turn_begin(fixture.daemon, "idle", NULL);

	/*
	 * Twenty-four sweeps of five seconds: twice the budget.  The busy
	 * tree gains half a core every time.
	 */
	for (i = 0; i < 24; i++) {
		ticks += 250;
		fake_process(101, 100, ticks, 0);
		advance_and_sweep(&fixture, 5);
	}

	g_assert_true(clawt_turn_watch_is_watching(watch, "busy"));
	g_assert_false(clawt_turn_watch_is_watching(watch, "idle"));

	/* The command stops using anything: a minute later it is stopped. */
	for (i = 0; i < 13; i++)
		advance_and_sweep(&fixture, 5);

	g_assert_false(clawt_turn_watch_is_watching(watch, "busy"));

	fixture_teardown(&fixture);
}

/*
 * The ceiling is reached: a tree spinning for ever holds its turn only
 * for busy_max_seconds after the last thing the turn said, and then the
 * ordinary budget applies.
 */
static void
test_the_busy_ceiling_is_reached(
	void
){
	Fixture fixture = { 0 };
	ClawtTurnWatch *watch;
	gint64 ticks = 0;
	gint i;

	fixture_setup(&fixture,
		"agents:\n"
		"  - id: busy\n"
		"    runtime:\n"
		"      turn_timeout_seconds: 30\n"
		"      busy_max_seconds: 120\n");
	watch = fixture.daemon->turn_watch;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(101, 100, 0, 0);

	clawt_daemon_turn_begin(fixture.daemon, "busy", NULL);

	/* 115 s of spinning: still inside the ceiling. */
	for (i = 0; i < 23; i++) {
		ticks += 500;
		fake_process(101, 100, ticks, 0);
		advance_and_sweep(&fixture, 5);
	}
	g_assert_true(clawt_turn_watch_is_watching(watch, "busy"));

	/* A step resets the ceiling's clock, exactly as it resets the budget. */
	clawt_daemon_turn_activity(fixture.daemon, "busy");

	for (i = 0; i < 23; i++) {
		ticks += 500;
		fake_process(101, 100, ticks, 0);
		advance_and_sweep(&fixture, 5);
	}
	g_assert_true(clawt_turn_watch_is_watching(watch, "busy"));

	/* Still spinning, past the ceiling and then past the budget. */
	for (i = 0; i < 12; i++) {
		ticks += 500;
		fake_process(101, 100, ticks, 0);
		advance_and_sweep(&fixture, 5);
	}
	g_assert_false(clawt_turn_watch_is_watching(watch, "busy"));

	fixture_teardown(&fixture);
}

/* busy_cpu_percent: 0 is the old watchdog, which CPU does not move. */
static void
test_zero_percent_turns_the_check_off(
	void
){
	Fixture fixture = { 0 };
	ClawtTurnWatch *watch;
	gint64 ticks = 0;
	gint i;

	fixture_setup(&fixture,
		"agents:\n"
		"  - id: busy\n"
		"    runtime:\n"
		"      turn_timeout_seconds: 30\n"
		"      busy_cpu_percent: 0\n");
	watch = fixture.daemon->turn_watch;

	fake_reset();
	fake_process(100, 1, 0, 0);
	fake_process(101, 100, 0, 0);

	clawt_daemon_turn_begin(fixture.daemon, "busy", NULL);

	for (i = 0; i < 8; i++) {
		ticks += 500;
		fake_process(101, 100, ticks, 0);
		advance_and_sweep(&fixture, 5);
	}

	g_assert_false(clawt_turn_watch_is_watching(watch, "busy"));

	fixture_teardown(&fixture);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	gint status;

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/busy/parse/last-parenthesis",
		test_parse_counts_from_the_last_parenthesis);
	g_test_add_func("/busy/tree/descendants-not-the-root",
		test_tree_sums_descendants_not_the_root);
	g_test_add_func("/busy/sample/threshold-exactly",
		test_sample_reaches_the_threshold_exactly);
	g_test_add_func("/busy/sample/falling-total",
		test_a_falling_total_is_not_work);
	g_test_add_func("/busy/sample/new-pid",
		test_a_new_pid_starts_a_new_baseline);
	g_test_add_func("/busy/sample/two-agents",
		test_two_agents_keep_their_own_baselines);
	g_test_add_func("/busy/daemon/outlives-the-budget",
		test_a_busy_tree_outlives_the_budget);
	g_test_add_func("/busy/daemon/ceiling",
		test_the_busy_ceiling_is_reached);
	g_test_add_func("/busy/daemon/zero-is-off",
		test_zero_percent_turns_the_check_off);

	status = g_test_run();

	g_clear_pointer(&fake_proc, g_hash_table_unref);

	return status;
}
