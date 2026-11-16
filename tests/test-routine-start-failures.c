/*
 * test-routine-start-failures.c - routines.max_start_failures
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A routine whose agent's computer will not start used to fail on every
 * tick, silently.  The runner now counts those runs and, at the limit,
 * remembers the routine as turned off so the daemon can switch it off
 * and say why.  These hold it to the edges that matter: the count
 * resets on a start that worked and on an edit, a run that began before
 * the edit does not count, 0 never turns anything off, and two routines
 * keep two counts.
 *
 * The daemon half includes core/clawt-daemon-private.h, as
 * test-routine-read-only does, to drive the start results directly: a
 * real computer refusing to start needs podman or libvirt, and this
 * suite stays hermetic.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

typedef struct {
	gchar              *dir;
	gchar              *state_path;
	ClawtConfig        *config;
	ClawtRoutineRunner *runner;
} Fixture;

#define DOWN "podman: image 'fedora-45' is not pulled"

/*
 * A fleet of one agent and whatever routines @routines describes.
 */
static ClawtConfig *
load_config(
	const gchar *dir,
	const gchar *routines
){
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;
	ClawtConfig *config;

	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s/state\"\n"
		"agents:\n"
		"  - id: researcher\n"
		"%s",
		dir, routines);

	config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(config);

	return config;
}

static void
fixture_setup(
	Fixture     *fixture,
	const gchar *routines
){
	fixture->dir = g_dir_make_tmp("clawt-routine-start-XXXXXX", NULL);
	g_assert_nonnull(fixture->dir);
	fixture->state_path = g_build_filename(fixture->dir, "routines.yaml",
	                                       NULL);
	fixture->config = load_config(fixture->dir, routines);
	fixture->runner = clawt_routine_runner_new(fixture->config,
	                                           fixture->state_path);
}

/*
 * Swaps in a config built from @routines, as a save-and-reload or a
 * hand edit picked up by a reload does.
 */
static void
fixture_reload(
	Fixture     *fixture,
	const gchar *routines
){
	g_clear_object(&fixture->config);
	fixture->config = load_config(fixture->dir, routines);
	clawt_routine_runner_set_config(fixture->runner, fixture->config);
}

static void
fixture_teardown(
	Fixture *fixture
){
	g_clear_object(&fixture->runner);
	g_clear_object(&fixture->config);
	g_clear_pointer(&fixture->state_path, g_free);

	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);

	g_clear_pointer(&fixture->dir, g_free);
}

static const gchar STANDUP[] =
	"routines:\n"
	"  - id: standup\n"
	"    agent: researcher\n"
	"    instructions: \"Summarise the commits.\"\n"
	"    schedule: daily\n"
	"    at: \"09:00\"\n";

/* The same routine, edited: different instructions. */
static const gchar STANDUP_EDITED[] =
	"routines:\n"
	"  - id: standup\n"
	"    agent: researcher\n"
	"    instructions: \"Summarise the commits and the open reviews.\"\n"
	"    schedule: daily\n"
	"    at: \"09:00\"\n";

/* The same routine, turned off -- what the daemon writes at the limit. */
static const gchar STANDUP_OFF[] =
	"routines:\n"
	"  - id: standup\n"
	"    agent: researcher\n"
	"    instructions: \"Summarise the commits.\"\n"
	"    schedule: daily\n"
	"    at: \"09:00\"\n"
	"    enabled: false\n";

/* ── Counting ────────────────────────────────────────────────────── */

/*
 * The default is three: two failures are counted and nothing more, the
 * third reaches the limit, and the routine is remembered as turned off
 * with the computer's error.
 */
static void
test_three_failures_turn_it_off(void)
{
	Fixture fixture = { 0 };
	guint failures = 0;
	const gchar *error = NULL;

	fixture_setup(&fixture, STANDUP);

	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 2);
	g_assert_false(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", NULL, NULL));

	g_assert_true(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_true(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", &failures, &error));
	g_assert_cmpuint(failures, ==, 3);
	g_assert_cmpstr(error, ==, DOWN);

	/*
	 * Once.  A fourth failure arriving before the daemon has turned it
	 * off must not raise the alert a second time.
	 */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));

	fixture_teardown(&fixture);
}

/* A limit of its own, read from the routine. */
static void
test_the_limit_is_per_routine(void)
{
	Fixture fixture = { 0 };

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"    max_start_failures: 1\n");

	g_assert_true(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));

	fixture_teardown(&fixture);
}

/* 0 is never, however many runs find the computer down. */
static void
test_zero_never_turns_it_off(void)
{
	Fixture fixture = { 0 };
	guint i;

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"    max_start_failures: 0\n");

	for (i = 0; i < 25; i++)
		g_assert_false(clawt_routine_runner_note_start(
			fixture.runner, "standup", g_get_real_time(), DOWN));

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 25);
	g_assert_false(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", NULL, NULL));

	fixture_teardown(&fixture);
}

/*
 * A negative value is a typo, and it reads as the default -- not as 0,
 * which is a real setting that would quietly switch the guard off.
 */
static void
test_a_negative_limit_is_the_default(void)
{
	Fixture fixture = { 0 };
	ClawtRoutine *routine;

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"    max_start_failures: -3\n");

	routine = clawt_config_get_routine(fixture.config, "standup");
	g_assert_nonnull(routine);
	g_assert_cmpuint(clawt_routine_max_start_failures(routine), ==, 3);

	fixture_teardown(&fixture);
}

/*
 * Two routines, two counts.  A count kept in one place for the runner
 * would turn the second routine off for the first one's computer.
 */
static void
test_each_routine_counts_its_own(void)
{
	Fixture fixture = { 0 };

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"  - id: sweep\n"
		"    agent: researcher\n"
		"    instructions: \"Check the queue.\"\n"
		"    schedule: hourly\n");

	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "sweep", g_get_real_time(), DOWN));

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 2);
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "sweep"), ==, 1);

	/* A start that worked for one says nothing about the other. */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "sweep", g_get_real_time(), NULL));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 2);
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "sweep"), ==, 0);

	fixture_teardown(&fixture);
}

/*
 * A disabled routine run by hand has nothing to turn off, so its runs
 * are not counted towards turning it off.
 */
static void
test_a_disabled_routine_is_not_counted(void)
{
	Fixture fixture = { 0 };
	guint i;

	fixture_setup(&fixture, STANDUP_OFF);

	for (i = 0; i < 5; i++)
		g_assert_false(clawt_routine_runner_note_start(
			fixture.runner, "standup", g_get_real_time(), DOWN));

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	fixture_teardown(&fixture);
}

/* ── Resetting ───────────────────────────────────────────────────── */

/* Any run whose computer started sets the count back to zero. */
static void
test_a_start_resets_the_count(void)
{
	Fixture fixture = { 0 };

	fixture_setup(&fixture, STANDUP);

	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), NULL));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	/* "in a row": two more failures are two, not four. */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 2);

	fixture_teardown(&fixture);
}

/* An edit sets the count back to zero too -- it may well be the fix. */
static void
test_an_edit_resets_the_count(void)
{
	Fixture fixture = { 0 };

	fixture_setup(&fixture, STANDUP);

	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));

	fixture_reload(&fixture, STANDUP_EDITED);

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	fixture_teardown(&fixture);
}

/*
 * A reload that changes nothing about the routine is not an edit.
 * Every save reloads the whole fleet, so treating any reload as an edit
 * would let an unrelated change elsewhere reset this routine's count
 * for ever, and the guard would never fire.
 */
static void
test_a_reload_is_not_an_edit(void)
{
	Fixture fixture = { 0 };

	fixture_setup(&fixture, STANDUP);

	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));

	fixture_reload(&fixture, STANDUP);

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 2);

	fixture_teardown(&fixture);
}

/*
 * A run that began before the routine was last edited does not count,
 * either way.  Its failure arriving afterwards must not count against
 * the routine as it now is -- and a stale success must not hide the
 * failures that came after the edit.
 */
static void
test_a_run_from_before_the_edit_does_not_count(void)
{
	Fixture fixture = { 0 };
	gint64 before_edit;

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"    max_start_failures: 1\n");

	before_edit = g_get_real_time() - G_USEC_PER_SEC;

	fixture_reload(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits and the reviews.\"\n"
		"    schedule: daily\n"
		"    max_start_failures: 2\n");

	/* Would reach the old limit of one; counts for nothing. */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", before_edit, DOWN));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	/* A run since the edit counts. */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 1);

	/* And an old success does not wipe it out. */
	g_assert_false(clawt_routine_runner_note_start(
		fixture.runner, "standup", before_edit, NULL));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 1);

	g_assert_true(clawt_routine_runner_note_start(
		fixture.runner, "standup", g_get_real_time(), DOWN));

	fixture_teardown(&fixture);
}

/*
 * Turning it off keeps the note; turning it back on clears it.  The
 * daemon turns a routine off by writing `enabled: false` through the
 * ordinary save, which reloads -- and that reload must not erase the
 * reason it was turned off.  The one-click re-enable is the same save
 * with `enabled: true`, and that is what clears it.
 */
static void
test_turning_it_back_on_clears_the_note(void)
{
	Fixture fixture = { 0 };
	guint i;

	fixture_setup(&fixture, STANDUP);

	for (i = 0; i < 3; i++)
		clawt_routine_runner_note_start(fixture.runner, "standup",
		                                g_get_real_time(), DOWN);

	g_assert_true(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", NULL, NULL));

	fixture_reload(&fixture, STANDUP_OFF);
	g_assert_true(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", NULL, NULL));

	fixture_reload(&fixture, STANDUP);
	g_assert_false(clawt_routine_runner_get_turned_off(
		fixture.runner, "standup", NULL, NULL));
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	fixture_teardown(&fixture);
}

/* ── Remembering ─────────────────────────────────────────────────── */

/*
 * Run state, beside the config and never in it: the count and the note
 * survive a daemon restart, and so does an edit made while it was down.
 */
static void
test_the_count_survives_a_restart(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *config_text = NULL;
	guint failures = 0;
	const gchar *error = NULL;

	fixture_setup(&fixture,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise the commits.\"\n"
		"    schedule: daily\n"
		"  - id: sweep\n"
		"    agent: researcher\n"
		"    instructions: \"Check the queue.\"\n"
		"    schedule: hourly\n"
		"    max_start_failures: 1\n");

	clawt_routine_runner_note_start(fixture.runner, "standup",
	                                g_get_real_time(), DOWN);
	g_assert_true(clawt_routine_runner_note_start(
		fixture.runner, "sweep", g_get_real_time(), DOWN));

	g_clear_object(&fixture.runner);

	/* Never written into the config, which a person keeps in git. */
	config_text = clawt_config_to_string(fixture.config);

	g_assert_nonnull(config_text);
	g_assert_null(strstr(config_text, "turned_off"));
	g_assert_null(strstr(config_text, "fingerprint"));
	g_assert_null(strstr(config_text, " start_failures"));

	fixture.runner = clawt_routine_runner_new(fixture.config,
	                                          fixture.state_path);

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 1);
	g_assert_true(clawt_routine_runner_get_turned_off(
		fixture.runner, "sweep", &failures, &error));
	g_assert_cmpuint(failures, ==, 1);
	g_assert_cmpstr(error, ==, DOWN);

	/* Edited while the daemon was down: still an edit. */
	g_clear_object(&fixture.runner);
	g_clear_object(&fixture.config);
	fixture.config = load_config(fixture.dir,
		"routines:\n"
		"  - id: standup\n"
		"    agent: researcher\n"
		"    instructions: \"Summarise everything.\"\n"
		"    schedule: daily\n");
	fixture.runner = clawt_routine_runner_new(fixture.config,
	                                          fixture.state_path);

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.runner, "standup"), ==, 0);

	fixture_teardown(&fixture);
}

/* ── What is said ────────────────────────────────────────────────── */

/* The alert names the routine, the count and the computer's error. */
static void
test_the_alert_text(void)
{
	g_autofree gchar *alert = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *silent = NULL;

	alert = clawt_routine_turned_off_alert("standup", 3, DOWN);
	g_assert_cmpstr(alert, ==,
		"Routine 'standup' was turned off after 3 runs in a row whose "
		"computer did not start: " DOWN ". Fix the computer, then turn "
		"the routine back on.");

	one = clawt_routine_turned_off_alert("sweep", 1, DOWN);
	g_assert_nonnull(strstr(one, "after 1 run in a row"));

	silent = clawt_routine_turned_off_alert("sweep", 2, NULL);
	g_assert_nonnull(strstr(silent, "no reason was given"));
}

/* What a turned-off routine says about itself in every client's list. */
static void
test_the_note_text(void)
{
	g_autofree gchar *note = clawt_routine_turned_off_note(3, DOWN);
	g_autofree gchar *one = clawt_routine_turned_off_note(1, NULL);

	g_assert_cmpstr(note, ==, "turned off after 3 failed starts: " DOWN);
	g_assert_cmpstr(one, ==, "turned off after 1 failed start");
	g_assert_nonnull(strstr(clawt_routine_start_failures_hint(), "0 never"));
}

/*
 * An error, not a notice: scheduled work stopped and stays stopped
 * until a person acts, and a disabled routine is otherwise silent.
 */
static void
test_the_alert_is_an_error(void)
{
	g_autoptr(ClawtEvent) event = clawt_event_new("routine.turned_off",
	                                              "standup");

	clawt_event_set_detail_int(event, "failures", 3);
	g_assert_cmpint(clawt_alert_tier_for_event(event), ==,
	                CLAWT_ALERT_ERROR);
}

/*
 * The schema's default is the documented three, and the generated
 * documentation names the key -- the key is not a private spelling.
 */
static void
test_the_schema_default(void)
{
	const ClawtSchemaEntry *entry =
		clawt_config_schema_lookup("routines.max_start_failures");

	g_assert_nonnull(entry);
	g_assert_cmpint(entry->type, ==, CLAWT_SCHEMA_INT);
	g_assert_cmpstr(entry->default_value, ==, "3");
}

/* ── The daemon: turning it off, and back on ─────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
	GPtrArray    *turned_off;   /* ClawtEvent, copies */
} DaemonFixture;

static void
on_bus_event(
	ClawtEventBus *bus,
	ClawtEvent    *event,
	gpointer       user_data
){
	DaemonFixture *fixture = user_data;

	(void)bus;

	if (g_strcmp0(clawt_event_get_kind(event), "routine.turned_off") == 0)
		g_ptr_array_add(fixture->turned_off, clawt_event_copy(event));
}

/*
 * An agent that is not started, so nothing here starts a real process
 * or a real computer: the start results are fed in by hand.
 */
static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-routine-start-XXXXXX", NULL);
	g_assert_nonnull(fixture->dir);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		/*
		 * The same prompt three times in a minute is a loop to the
		 * guard; here it is three scheduled runs.
		 */
		"orchestration:\n  cycle_seconds: 0\n"
		"agents:\n"
		"  - id: worker\n"
		"    runtime:\n      autostart: false\n"
		"routines:\n"
		"  - id: nightly\n"
		"    agent: worker\n"
		"    instructions: \"Check the backups.\"\n"
		"    schedule: daily\n"
		"    at: \"02:00\"\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));

	fixture->turned_off = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_event_free);
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
	g_signal_connect(fixture->daemon->bus, "event",
	                 G_CALLBACK(on_bus_event), fixture);
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	g_signal_handlers_disconnect_by_data(fixture->daemon->bus, fixture);
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;

	g_clear_pointer(&fixture->context, g_main_context_unref);
	g_clear_pointer(&fixture->turned_off, g_ptr_array_unref);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

static JsonNode *
request(
	DaemonFixture *fixture,
	const gchar   *kind,
	const gchar   *payload_json
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
	clawt_ipc_frame_set_payload(frame,
	                            json_node_copy(json_parser_get_root(parser)));

	return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* Runs `nightly` and hands back nothing; the run is what is watched. */
static void
run_nightly(
	DaemonFixture *fixture
){
	g_autoptr(JsonNode) reply = request(fixture, "routine.run",
	                                    "{\"id\":\"nightly\"}");

	g_assert_nonnull(reply);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(reply),
	                                             "ok"));
}

/* The listing's object for `nightly`, as a client reads it. */
static JsonObject *
listed_nightly(
	JsonNode *reply
){
	JsonObject *root = clawt_ipc_frame_get_payload(reply);
	JsonArray *routines;
	guint i;

	g_assert_nonnull(root);
	routines = json_object_get_array_member(root, "routines");

	for (i = 0; i < json_array_get_length(routines); i++) {
		JsonObject *routine = json_array_get_object_element(routines, i);

		if (g_strcmp0(json_object_get_string_member(routine, "id"),
		              "nightly") == 0)
			return routine;
	}

	g_assert_not_reached();
	return NULL;
}

static void
drain(
	DaemonFixture *fixture
){
	while (g_main_context_iteration(fixture->context, FALSE))
		;
}

/*
 * Three runs whose computer refused: the routine is turned off in the
 * config, the listing says why, the alert names the routine and the
 * error -- and one `enabled: true` turns it back on with a clean count.
 */
static void
test_the_daemon_turns_it_off_and_back_on(void)
{
	DaemonFixture fixture = { 0 };
	ClawtRoutine *routine;
	ClawtEvent *event;
	guint i;

	daemon_setup(&fixture);

	for (i = 0; i < 3; i++) {
		run_nightly(&fixture);
		clawt_daemon_routine_starts_settle(fixture.daemon, "worker", DOWN);
		drain(&fixture);
	}

	/* Off, in the config, where every client reads `enabled`. */
	routine = clawt_config_get_routine(fixture.daemon->config, "nightly");
	g_assert_nonnull(routine);
	g_assert_false(clawt_routine_get_boolean(routine, "enabled"));

	/* One alert, naming the routine, the agent, the count, the error. */
	g_assert_cmpuint(fixture.turned_off->len, ==, 1);
	event = g_ptr_array_index(fixture.turned_off, 0);
	g_assert_cmpstr(clawt_event_get_subject(event), ==, "nightly");
	g_assert_cmpstr(clawt_event_get_detail(event, "agent"), ==, "worker");
	g_assert_cmpint(clawt_event_get_detail_int(event, "failures"), ==, 3);
	g_assert_cmpstr(clawt_event_get_detail(event, "error"), ==, DOWN);

	/* The listing says why, as numbers and the raw error. */
	{
		g_autoptr(JsonNode) listing = request(&fixture, "routine.list", "{}");
		JsonObject *nightly = listed_nightly(listing);

		g_assert_false(json_object_get_boolean_member(nightly, "enabled"));
		g_assert_cmpint(json_object_get_int_member(nightly,
		                                           "turned_off_after"), ==, 3);
		g_assert_cmpstr(json_object_get_string_member(nightly,
		                                              "turned_off_error"),
		                ==, DOWN);
		g_assert_cmpint(json_object_get_int_member(nightly,
		                                           "max_start_failures"),
		                ==, 3);
	}

	/* The one-click re-enable every client sends. */
	{
		g_autoptr(JsonNode) reply = request(
			&fixture, "routine.update",
			"{\"id\":\"nightly\",\"enabled\":true}");
		g_autoptr(JsonNode) listing = NULL;
		JsonObject *nightly;

		g_assert_true(json_object_get_boolean_member(
			json_node_get_object(reply), "ok"));

		listing = request(&fixture, "routine.list", "{}");
		nightly = listed_nightly(listing);
		g_assert_true(json_object_get_boolean_member(nightly, "enabled"));
		g_assert_false(json_object_has_member(nightly, "turned_off_after"));
		g_assert_cmpint(json_object_get_int_member(nightly, "start_failures"),
		                ==, 0);
	}

	daemon_teardown(&fixture);
}

/*
 * A start that worked, by whatever path, resets the count; and several
 * runs waiting on one start are counted once.
 */
static void
test_the_daemon_counts_starts_not_runs(void)
{
	DaemonFixture fixture = { 0 };

	daemon_setup(&fixture);

	/* Three runs queued behind one refusal: one failure, not three. */
	run_nightly(&fixture);
	run_nightly(&fixture);
	run_nightly(&fixture);
	clawt_daemon_routine_starts_settle(fixture.daemon, "worker", DOWN);
	drain(&fixture);

	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.daemon->routines, "nightly"), ==, 1);
	g_assert_cmpuint(fixture.turned_off->len, ==, 0);

	/* A start with nothing waiting changes nothing. */
	clawt_daemon_routine_starts_settle(fixture.daemon, "worker", DOWN);
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.daemon->routines, "nightly"), ==, 1);

	/* And one that worked sets it back to zero. */
	run_nightly(&fixture);
	clawt_daemon_routine_starts_settle(fixture.daemon, "worker", NULL);
	g_assert_cmpuint(clawt_routine_runner_get_start_failures(
		fixture.daemon->routines, "nightly"), ==, 0);

	/*
	 * A stopped agent was stopped by somebody: the run waits, and no
	 * start is begun on its behalf.
	 */
	run_nightly(&fixture);
	g_assert_true(fixture.daemon->routine_waking == NULL ||
	              !g_hash_table_contains(fixture.daemon->routine_waking,
	                                     "worker"));

	daemon_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/routine-start/three-turn-it-off",
	                test_three_failures_turn_it_off);
	g_test_add_func("/routine-start/limit-per-routine",
	                test_the_limit_is_per_routine);
	g_test_add_func("/routine-start/zero-is-never",
	                test_zero_never_turns_it_off);
	g_test_add_func("/routine-start/negative-is-default",
	                test_a_negative_limit_is_the_default);
	g_test_add_func("/routine-start/each-routine-its-own",
	                test_each_routine_counts_its_own);
	g_test_add_func("/routine-start/disabled-not-counted",
	                test_a_disabled_routine_is_not_counted);
	g_test_add_func("/routine-start/start-resets",
	                test_a_start_resets_the_count);
	g_test_add_func("/routine-start/edit-resets",
	                test_an_edit_resets_the_count);
	g_test_add_func("/routine-start/reload-is-not-edit",
	                test_a_reload_is_not_an_edit);
	g_test_add_func("/routine-start/pre-edit-run-ignored",
	                test_a_run_from_before_the_edit_does_not_count);
	g_test_add_func("/routine-start/turn-on-clears-note",
	                test_turning_it_back_on_clears_the_note);
	g_test_add_func("/routine-start/survives-restart",
	                test_the_count_survives_a_restart);
	g_test_add_func("/routine-start/alert-text", test_the_alert_text);
	g_test_add_func("/routine-start/note-text", test_the_note_text);
	g_test_add_func("/routine-start/alert-is-error",
	                test_the_alert_is_an_error);
	g_test_add_func("/routine-start/schema-default",
	                test_the_schema_default);
	g_test_add_func("/routine-start/daemon-off-and-on",
	                test_the_daemon_turns_it_off_and_back_on);
	g_test_add_func("/routine-start/daemon-counts-starts",
	                test_the_daemon_counts_starts_not_runs);

	return g_test_run();
}
