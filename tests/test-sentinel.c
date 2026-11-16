/*
 * test-sentinel.c - A read-only scan of the event log for recurring trouble
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The log is written by hand here, one line per event in the daemon's
 * own shape, so each grouping is reached exactly: a tool failing at the
 * threshold and one failing just below it, a wait at the line and one
 * under it.  The read-only claim is checked against the files' bytes and
 * times, not against the code.
 *
 * The daemon half runs the real `clawtilla sentinel` beside this test
 * binary (the Makefile builds it first), because the whole point of the
 * frame is that the scan is a separate process.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

/* 2026-11-14 12:00:00 UTC, in microseconds. */
#define NOON (G_GINT64_CONSTANT(1794657600) * G_USEC_PER_SEC)
#define MINUTE (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

static void
add_line(
	GString     *log,
	const gchar *kind,
	const gchar *subject,
	gint64       ts,
	const gchar *detail_json
){
	g_string_append_printf(log, "{\"kind\":\"%s\",\"subject\":\"%s\","
		"\"ts\":%" G_GINT64_FORMAT ",\"cursor\":1%s%s}\n", kind, subject, ts,
		detail_json != NULL ? ",\"detail\":" : "",
		detail_json != NULL ? detail_json : "");
}

/*
 * One day's log with every kind of trouble the sentinel groups, and the
 * day before with something that a one-day window must not see.
 */
static gchar *
write_log(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-sentinel-XXXXXX", NULL);
	g_autofree gchar *events = g_build_filename(dir, "events", NULL);
	g_autoptr(GString) today = g_string_new(NULL);
	g_autoptr(GString) yesterday = g_string_new(NULL);
	g_autofree gchar *today_path = NULL;
	g_autofree gchar *yesterday_path = NULL;
	gint i;

	g_assert_cmpint(g_mkdir_with_parents(events, 0700), ==, 0);

	/* alpha's bash fails three times: exactly the threshold. */
	for (i = 0; i < 3; i++)
		add_line(today, "turn.step", "alpha", NOON + i * MINUTE,
			"{\"step_kind\":\"tool\",\"tool\":\"bash\",\"failed\":\"1\","
			"\"detail\":\"make: *** [all] Error 2\"}");

	/* A working step is not a failure. */
	add_line(today, "turn.step", "alpha", NOON, "{\"tool\":\"bash\","
		"\"failed\":\"0\"}");

	/* beta's read fails twice: under it. */
	for (i = 0; i < 2; i++)
		add_line(today, "turn.step", "beta", NOON, "{\"tool\":\"read\","
			"\"failed\":\"1\"}");

	/* beta's computer_exec exits non-zero nine times: bad. */
	for (i = 0; i < 9; i++)
		add_line(today, "computer.exec", "beta", NOON + i * MINUTE,
			"{\"command\":\"podman ps\",\"exit\":\"125\"}");

	/*
	 * Queue waits for gamma: 20 minutes (over the 10-minute line), then
	 * 5 (under).  The second queued message is delivered second, since
	 * the pairing is first in, first out.
	 */
	add_line(today, "mailbox.queued", "gamma", NOON, "{\"room\":\"r\"}");
	add_line(today, "mailbox.queued", "gamma", NOON + 16 * MINUTE,
		"{\"room\":\"r\"}");
	add_line(today, "mailbox.delivered", "gamma", NOON + 20 * MINUTE, NULL);
	add_line(today, "mailbox.delivered", "gamma", NOON + 21 * MINUTE, NULL);

	/* One still waiting at the end: an hour old by the scan's clock. */
	add_line(today, "mailbox.queued", "delta", NOON, NULL);

	/* Spend: alpha at $12 over the day, beta at $2. */
	add_line(today, "turn.cost", "alpha", NOON, "{\"cost_micros\":\"7000000\"}");
	add_line(today, "turn.cost", "alpha", NOON + MINUTE,
		"{\"cost_micros\":\"5000000\"}");
	add_line(today, "turn.cost", "beta", NOON, "{\"cost_micros\":\"2000000\"}");
	add_line(today, "spending.limit-reached", "agent:alpha", NOON,
		"{\"cost_micros\":\"12000000\",\"limit_micros\":\"12000000\"}");

	/* A watchdog stop. */
	add_line(today, "turn.timed_out", "alpha", NOON,
		"{\"reason\":\"turn-timeout\",\"detail\":\"produced nothing\"}");

	/* A torn last line from a daemon that died mid-write. */
	g_string_append(today, "{\"kind\":\"turn.st");

	/* Yesterday: a failing tool a one-day window must not count. */
	for (i = 0; i < 5; i++)
		add_line(yesterday, "turn.step", "omega", NOON - 1440 * MINUTE,
			"{\"tool\":\"bash\",\"failed\":\"1\"}");

	today_path = g_build_filename(events, "2026-11-14.ndjson", NULL);
	yesterday_path = g_build_filename(events, "2026-11-13.ndjson", NULL);
	g_assert_true(g_file_set_contents(today_path, today->str, -1, NULL));
	g_assert_true(g_file_set_contents(yesterday_path, yesterday->str, -1,
		NULL));

	return g_steal_pointer(&dir);
}

static JsonObject *
find_issue(
	JsonNode    *report,
	const gchar *kind,
	const gchar *subject,
	const gchar *key
){
	JsonArray *issues = json_object_get_array_member(
		json_node_get_object(report), "issues");
	guint i;

	for (i = 0; i < json_array_get_length(issues); i++) {
		JsonObject *issue = json_array_get_object_element(issues, i);

		if (g_strcmp0(json_object_get_string_member(issue, "kind"), kind) == 0 &&
			g_strcmp0(json_object_get_string_member(issue, "subject"),
				subject) == 0 &&
			(key == NULL || g_strcmp0(json_object_get_string_member(issue,
				"key"), key) == 0))
			return issue;
	}

	return NULL;
}

static ClawtSentinelOptions
one_day(
	void
){
	ClawtSentinelOptions options;

	clawt_sentinel_options_init(&options);
	options.days = 1;
	options.min_failures = 3;
	options.queue_wait_seconds = 600;
	options.daily_spend_micros = 10000000;
	options.now_usec = NOON + 60 * MINUTE;

	return options;
}

/* Every grouping, each reached at its line and missed just under it. */
static void
test_the_scan_groups_into_issues(
	void
){
	g_autofree gchar *dir = write_log();
	g_autofree gchar *events = g_build_filename(dir, "events", NULL);
	ClawtSentinelOptions options = one_day();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	JsonObject *header;
	JsonObject *issue;

	report = clawt_sentinel_scan(events, &options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);

	header = json_object_get_object_member(json_node_get_object(report),
		"sentinel");
	g_assert_cmpint(json_object_get_int_member(header, "files"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(header, "skipped"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(header, "since"), ==,
		"2026-11-14");

	issue = find_issue(report, "tool_failure", "alpha", "bash");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_object_get_int_member(issue, "count"), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(issue, "severity"), ==,
		"warning");
	g_assert_cmpstr(json_object_get_string_member(issue, "sample"), ==,
		"make: *** [all] Error 2");

	g_assert_null(find_issue(report, "tool_failure", "beta", "read"));

	issue = find_issue(report, "tool_failure", "beta",
		"clawtilla_computer_exec");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_object_get_int_member(issue, "count"), ==, 9);
	g_assert_cmpstr(json_object_get_string_member(issue, "severity"), ==,
		"bad");

	/* Twenty minutes counted; five not. */
	issue = find_issue(report, "queue_wait", "gamma", "delivered late");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_object_get_int_member(issue, "count"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(issue, "value"), ==, 1200);

	issue = find_issue(report, "queue_wait", "delta", "still queued");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_object_get_int_member(issue, "value"), ==, 3600);

	issue = find_issue(report, "spend", "alpha", "2026-11-14");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_object_get_int_member(issue, "value"), ==,
		12000000);
	g_assert_null(find_issue(report, "spend", "beta", NULL));
	g_assert_nonnull(find_issue(report, "spend", "agent:alpha",
		"daily cap"));

	issue = find_issue(report, "stall", "alpha", "turn.timed_out");
	g_assert_nonnull(issue);
	g_assert_cmpstr(json_object_get_string_member(issue, "severity"), ==,
		"bad");

	/* Yesterday was outside the window. */
	g_assert_null(find_issue(report, "tool_failure", "omega", NULL));

	/* Worst first: the first issue is a bad one. */
	issue = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(report), "issues"), 0);
	g_assert_cmpstr(json_object_get_string_member(issue, "severity"), ==,
		"bad");

	clawt_test_remove_tree(dir);
}

/*
 * Read-only, checked against the files: their bytes and their times are
 * what they were, and a report aimed into the log is refused.
 */
static void
test_the_scan_never_writes_the_log(
	void
){
	g_autofree gchar *dir = write_log();
	g_autofree gchar *events = g_build_filename(dir, "events", NULL);
	g_autofree gchar *path = g_build_filename(events, "2026-11-14.ndjson",
		NULL);
	g_autofree gchar *inside = g_build_filename(events, "report.yaml", NULL);
	g_autofree gchar *outside = g_build_filename(dir, "sentinel.yaml", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	ClawtSentinelOptions options = one_day();
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;
	GStatBuf was;
	GStatBuf now;

	g_assert_true(g_file_get_contents(path, &before, NULL, NULL));
	g_assert_cmpint(g_stat(path, &was), ==, 0);

	report = clawt_sentinel_scan(events, &options, NULL);
	g_assert_nonnull(report);

	g_assert_false(clawt_sentinel_write_report(report, inside, events,
		&error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_false(g_file_test(inside, G_FILE_TEST_EXISTS));
	g_clear_error(&error);

	g_assert_true(clawt_sentinel_write_report(report, outside, events,
		&error));
	g_assert_no_error(error);

	g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
	g_assert_cmpint(g_stat(path, &now), ==, 0);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(was.st_mtime, ==, now.st_mtime);

	clawt_test_remove_tree(dir);
}

/*
 * The file is YAML and the clients read JSON, so numbers must survive the
 * trip as numbers -- a count read with the integer reader off a string
 * is its fallback, silently.
 */
static void
test_the_yaml_round_trip(
	void
){
	g_autofree gchar *dir = write_log();
	g_autofree gchar *events = g_build_filename(dir, "events", NULL);
	ClawtSentinelOptions options = one_day();
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(JsonNode) back = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *issue;

	report = clawt_sentinel_scan(events, &options, NULL);
	text = clawt_sentinel_report_to_yaml(report, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(text, "issues:"));

	back = clawt_sentinel_report_from_yaml(text, &error);
	g_assert_no_error(error);

	issue = find_issue(back, "tool_failure", "beta",
		"clawtilla_computer_exec");
	g_assert_nonnull(issue);
	g_assert_cmpint(json_node_get_value_type(
		json_object_get_member(issue, "count")), ==, G_TYPE_INT64);
	g_assert_cmpint(json_object_get_int_member(issue, "count"), ==, 9);

	/* Not a report at all is refused rather than read as an empty one. */
	g_clear_pointer(&back, json_node_unref);
	back = clawt_sentinel_report_from_yaml("hello: world\n", &error);
	g_assert_null(back);
	g_assert_nonnull(error);

	clawt_test_remove_tree(dir);
}

static void
test_the_words(
	void
){
	g_autofree gchar *never = clawt_sentinel_age_text(0, 100);
	g_autofree gchar *recent = clawt_sentinel_age_text(1000, 1180);

	g_assert_cmpstr(clawt_sentinel_kind_label("queue_wait"), ==,
		"Slow queue");
	g_assert_cmpstr(clawt_sentinel_kind_label("new_kind"), ==, "new_kind");
	g_assert_cmpstr(never, ==, "Not scanned yet");
	g_assert_true(g_str_has_prefix(recent, "Scanned "));
}

/* ── Through the daemon, out of process ──────────────────────────── */

static gboolean finished;

static void
on_event(
	ClawtEventBus *bus,
	ClawtEvent    *event,
	gpointer       user_data
){
	(void)bus;
	(void)user_data;

	if (g_strcmp0(clawt_event_get_kind(event), "sentinel.finished") == 0)
		finished = TRUE;
}

static gboolean
on_timeout(
	gpointer user_data
){
	*(gboolean *)user_data = TRUE;

	return G_SOURCE_REMOVE;
}

static JsonNode *
request(
	ClawtDaemon *daemon,
	const gchar *kind
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");

	return clawt_daemon_handle_request(daemon, frame);
}

static JsonObject *
payload_of(
	JsonNode *reply
){
	g_assert_false(json_object_has_member(json_node_get_object(reply),
		"error"));

	return json_object_get_object_member(json_node_get_object(reply),
		"payload");
}

/*
 * sentinel.scan starts `clawtilla sentinel` as a child and answers at
 * once; sentinel.finished says it ended; sentinel.report reads the YAML
 * it wrote.  The CLI is the one built beside this test.
 */
static void
test_the_daemon_runs_it_out_of_process(
	void
){
	g_autofree gchar *dir = write_log();
	g_autofree gchar *config_path = g_build_filename(dir, "config.yaml",
		NULL);
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *self_exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *tests_dir = g_path_get_dirname(self_exe);
	g_autofree gchar *build_dir = g_path_get_dirname(tests_dir);
	g_autofree gchar *cli = g_build_filename(build_dir, "clawtilla", NULL);
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(JsonNode) before = NULL;
	g_autoptr(JsonNode) started = NULL;
	g_autoptr(JsonNode) after = NULL;
	JsonObject *report;
	gboolean timed_out = FALSE;
	GSource *timeout;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *events = NULL;
	g_autoptr(GDateTime) now_utc = g_date_time_new_now_utc();
	g_autofree gchar *daemon_day = g_date_time_format(now_utc, "%Y-%m-%d");
	gint64 expected_files;

	/*
	 * The fixture writes two dated files, and the running daemon writes
	 * its own events into the same directory under today's UTC date.
	 * When that date is one of the fixture's, the daemon appends to it;
	 * on any other day (the fixture's dates are fixed) it is a third
	 * file.  Counting it from the clock keeps the test true on every day
	 * instead of only on the day it was written.
	 */
	expected_files = (g_strcmp0(daemon_day, "2026-11-14") == 0 ||
	                  g_strcmp0(daemon_day, "2026-11-13") == 0) ? 2 : 3;

	if (!g_file_test(cli, G_FILE_TEST_IS_EXECUTABLE)) {
		g_test_skip("the clawtilla CLI is not built beside this test");
		clawt_test_remove_tree(dir);
		return;
	}

	/* The state directory's own events/, as the daemon keeps it. */
	events = g_build_filename(dir, "events", NULL);
	moved = g_build_filename(dir, "state", "events", NULL);
	{
		g_autofree gchar *state = g_build_filename(dir, "state", NULL);

		g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
		g_assert_cmpint(g_rename(events, moved), ==, 0);
	}

	/* days: 0 reads both files, so the window is the CLI's, from config. */
	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"sentinel:\n  days: 0\n  min_failures: 4\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n",
		dir, dir, dir, dir, dir, dir);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	daemon = clawt_daemon_new(config_path, context);
	g_assert_true(clawt_daemon_start(daemon, NULL));
	clawt_daemon_sentinel_set_command(daemon, cli);

	g_signal_connect(clawt_daemon_get_event_bus(daemon), "event",
		G_CALLBACK(on_event), NULL);

	/* Nothing scanned yet: no report, and no error either. */
	before = request(daemon, "sentinel.report");
	g_assert_false(json_object_has_member(payload_of(before), "report"));
	g_assert_false(json_object_has_member(payload_of(before), "error"));

	finished = FALSE;
	started = request(daemon, "sentinel.scan");
	g_assert_true(json_object_get_boolean_member(payload_of(started),
		"started"));

	/* Bounded: a test that can hang is worse than one that fails. */
	timeout = g_timeout_source_new_seconds(30);
	g_source_set_callback(timeout, on_timeout, &timed_out, NULL);
	g_source_attach(timeout, context);

	while (!finished && !timed_out)
		g_main_context_iteration(context, TRUE);

	g_source_destroy(timeout);
	g_source_unref(timeout);
	g_assert_false(timed_out);

	after = request(daemon, "sentinel.report");
	g_assert_false(json_object_has_member(payload_of(after), "error"));
	report = json_object_get_object_member(payload_of(after), "report");
	g_assert_nonnull(report);
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(report, "sentinel"), "files"), ==,
		expected_files);

	/*
	 * The config reached the child: min_failures 4 drops alpha's three
	 * bash failures and keeps omega's five from the older file.
	 */
	{
		g_autoptr(JsonNode) wrapped = json_node_new(JSON_NODE_OBJECT);

		json_node_set_object(wrapped, report);
		g_assert_null(find_issue(wrapped, "tool_failure", "alpha", "bash"));
		g_assert_nonnull(find_issue(wrapped, "tool_failure", "omega",
			"bash"));
	}

	clawt_daemon_stop(daemon);
	g_clear_object(&daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	clawt_test_remove_tree(dir);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sentinel/scan/groups", test_the_scan_groups_into_issues);
	g_test_add_func("/sentinel/scan/read-only",
		test_the_scan_never_writes_the_log);
	g_test_add_func("/sentinel/yaml/round-trip", test_the_yaml_round_trip);
	g_test_add_func("/sentinel/words", test_the_words);
	g_test_add_func("/sentinel/daemon/out-of-process",
		test_the_daemon_runs_it_out_of_process);

	return g_test_run();
}
