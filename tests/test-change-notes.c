/*
 * test-change-notes.c - An agent's configuration changed, said in its chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two halves.  The comparison (clawt_change_diff()) is pure and is tested
 * on configurations written inline.  The daemon half drives real saves
 * -- an agent's tool call, the CLI's hint, the operator -- and reads the
 * room the note should have landed in, as test-replay does; it includes
 * core/clawt-daemon-private.h for the room manager and the tools.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* ── The comparison ──────────────────────────────────────────────── */

static GPtrArray *
diff_texts(
	const gchar *before,
	const gchar *after
){
	g_autoptr(JsonNode) a = clawt_change_config_to_json(before);
	g_autoptr(JsonNode) b = clawt_change_config_to_json(after);

	return clawt_change_diff(a, b);
}

/* The change at @index, asserted to exist. */
static ClawtChange *
nth(
	GPtrArray *changes,
	guint      index
){
	g_assert_cmpuint(changes->len, >, index);
	return g_ptr_array_index(changes, index);
}

static void
test_setting_old_and_new(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"agents:\n  - id: alpha\n    model:\n      name: small\n",
		"agents:\n  - id: alpha\n    model:\n      name: large\n");

	g_assert_cmpuint(changes->len, ==, 1);
	g_assert_cmpint(nth(changes, 0)->kind, ==, CLAWT_CHANGE_SETTING);
	g_assert_cmpstr(nth(changes, 0)->agent, ==, "alpha");
	g_assert_cmpstr(nth(changes, 0)->verb, ==, "set");
	g_assert_cmpstr(nth(changes, 0)->label, ==, "model.name");
	g_assert_cmpstr(nth(changes, 0)->detail, ==, "small -> large");
}

static void
test_nothing_changed_is_no_note(void)
{
	const gchar *text = "agents:\n  - id: alpha\n    model:\n      name: x\n"
		"routines:\n  - id: r\n    agent: alpha\n    instructions: go\n";
	g_autoptr(GPtrArray) changes = diff_texts(text, text);

	g_assert_cmpuint(changes->len, ==, 0);
}

/*
 * A credential's value is never in a note, and nor is anything in an
 * agent's env: block, whatever it is called.
 */
static void
test_credentials_never_shown(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"agents:\n  - id: alpha\n    env:\n      OPENAI_KEY: old-secret\n"
		"    integrations:\n      matrix:\n        password: hunter2\n",
		"agents:\n  - id: alpha\n    env:\n      OPENAI_KEY: new-secret\n"
		"    integrations:\n      matrix:\n        password: hunter3\n");
	g_autofree gchar *body = NULL;

	g_assert_cmpuint(changes->len, ==, 2);
	body = clawt_change_note_body(changes, "beta", NULL);
	g_assert_null(strstr(body, "secret"));
	g_assert_null(strstr(body, "hunter"));
	g_assert_nonnull(strstr(body, "env.OPENAI_KEY"));
	g_assert_cmpint(nth(changes, 1)->kind, ==, CLAWT_CHANGE_INTEGRATION);
}

/* A routine turned off, added, removed and moved: each told to its agent. */
static void
test_routines_by_id(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"routines:\n"
		"  - id: watch\n    agent: alpha\n    description: Bass Pro watch\n"
		"    instructions: look\n"
		"  - id: gone\n    agent: alpha\n    instructions: x\n"
		"  - id: moved\n    agent: alpha\n    instructions: y\n",
		"routines:\n"
		"  - id: watch\n    agent: alpha\n    description: Bass Pro watch\n"
		"    instructions: look\n    enabled: false\n"
		"  - id: fresh\n    agent: beta\n    instructions: z\n"
		"  - id: moved\n    agent: beta\n    instructions: y\n");
	guint i;
	gboolean disabled = FALSE, added = FALSE, removed = FALSE;
	gboolean moved_out = FALSE, moved_in = FALSE;

	for (i = 0; i < changes->len; i++) {
		ClawtChange *c = nth(changes, i);

		g_assert_cmpint(c->kind, ==, CLAWT_CHANGE_ROUTINE);
		if (g_strcmp0(c->verb, "disabled") == 0)
			disabled = g_strcmp0(c->agent, "alpha") == 0 &&
			           g_strcmp0(c->label, "Bass Pro watch") == 0;
		if (g_strcmp0(c->verb, "added") == 0 &&
		    g_strcmp0(c->label, "fresh") == 0)
			added = g_strcmp0(c->agent, "beta") == 0;
		if (g_strcmp0(c->verb, "removed") == 0 &&
		    g_strcmp0(c->label, "gone") == 0)
			removed = g_strcmp0(c->agent, "alpha") == 0;
		if (g_strcmp0(c->label, "moved") == 0 &&
		    g_strcmp0(c->agent, "alpha") == 0)
			moved_out = g_strcmp0(c->verb, "removed") == 0;
		if (g_strcmp0(c->label, "moved") == 0 &&
		    g_strcmp0(c->agent, "beta") == 0)
			moved_in = g_strcmp0(c->verb, "added") == 0;
	}

	g_assert_true(disabled);
	g_assert_true(added);
	g_assert_true(removed);
	g_assert_true(moved_out);
	g_assert_true(moved_in);
	g_assert_cmpuint(changes->len, ==, 5);

	/* grouped by agent: every alpha change before every beta one */
	g_assert_cmpstr(nth(changes, 0)->agent, ==, "alpha");
	g_assert_cmpstr(nth(changes, changes->len - 1)->agent, ==, "beta");
}

/* An edit other than on/off names the fields that changed. */
static void
test_routine_edit_names_fields(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"routines:\n  - id: r\n    agent: alpha\n    instructions: a\n"
		"    at: \"09:00\"\n",
		"routines:\n  - id: r\n    agent: alpha\n    instructions: b\n"
		"    at: \"10:00\"\n");

	g_assert_cmpuint(changes->len, ==, 1);
	g_assert_cmpstr(nth(changes, 0)->verb, ==, "updated");
	g_assert_cmpstr(nth(changes, 0)->detail, ==, "at, instructions");
}

/* A trigger delivering to several agents tells each one. */
static void
test_trigger_tells_every_agent(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"triggers: []\n",
		"triggers:\n  - id: hook\n    agent: alpha\n    agents: [alpha, beta]\n");

	g_assert_cmpuint(changes->len, ==, 2);
	g_assert_cmpint(nth(changes, 0)->kind, ==, CLAWT_CHANGE_TRIGGER);
	g_assert_cmpstr(nth(changes, 0)->agent, ==, "alpha");
	g_assert_cmpstr(nth(changes, 1)->agent, ==, "beta");
}

static void
test_created_and_removed(void)
{
	g_autoptr(GPtrArray) changes = diff_texts(
		"agents:\n  - id: alpha\n",
		"agents:\n  - id: beta\n    description: A new one\n");

	g_assert_cmpuint(changes->len, ==, 2);
	g_assert_cmpint(nth(changes, 0)->kind, ==, CLAWT_CHANGE_CREATED);
	g_assert_cmpstr(nth(changes, 0)->agent, ==, "beta");
	g_assert_cmpint(nth(changes, 1)->kind, ==, CLAWT_CHANGE_REMOVED);
	g_assert_cmpstr(nth(changes, 1)->agent, ==, "alpha");
}

/* A long save is a few lines and a count, then who made it. */
static void
test_note_body(void)
{
	g_autoptr(GPtrArray) changes = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_change_free);
	g_autofree gchar *body = NULL;
	g_autofree gchar *cli = NULL;
	guint i;

	for (i = 0; i < CLAWT_CHANGE_NOTE_LINES + 2; i++) {
		g_autofree gchar *key = g_strdup_printf("key%u", i);

		g_ptr_array_add(changes, clawt_change_new(CLAWT_CHANGE_SETTING,
			"alpha", "set", key, "a -> b"));
	}

	body = clawt_change_note_body(changes, "beta", NULL);
	g_assert_true(g_str_has_prefix(body, "Set key0: a -> b\n"));
	g_assert_nonnull(strstr(body, "\n+2 more\nby beta"));
	g_assert_null(strstr(body, "key5"));

	cli = clawt_change_note_body(changes, "fleet:lab:atlas", "cli");
	g_assert_true(g_str_has_suffix(cli, "by atlas@lab, via the CLI"));
}

static void
test_should_note(void)
{
	g_assert_true(clawt_change_should_note("agents", "alpha", "beta"));
	g_assert_true(clawt_change_should_note("agents", "alpha", "alpha"));
	g_assert_true(clawt_change_should_note("agents", "fleet:lab:x", "beta"));
	g_assert_false(clawt_change_should_note("agents", "user", "beta"));
	g_assert_false(clawt_change_should_note("agents", "system", "beta"));
	g_assert_false(clawt_change_should_note("agents", NULL, "beta"));
	g_assert_true(clawt_change_should_note("all", "user", "beta"));
	g_assert_false(clawt_change_should_note("off", "alpha", "beta"));
}

static void
test_actor_stack(void)
{
	g_assert_null(clawt_change_actor_get());

	clawt_change_actor_push("user", NULL);
	clawt_change_actor_push("alpha", "cli");
	g_assert_cmpstr(clawt_change_actor_get(), ==, "alpha");
	g_assert_cmpstr(clawt_change_actor_get_via(), ==, "cli");
	clawt_change_actor_pop();
	g_assert_cmpstr(clawt_change_actor_get(), ==, "user");
	g_assert_null(clawt_change_actor_get_via());
	clawt_change_actor_pop();
	g_assert_null(clawt_change_actor_get());
}

/* Every kind has an icon, a label and a nick that comes back as itself. */
static void
test_every_kind_is_drawn(void)
{
	g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_CHANGE_KIND);
	guint i;

	for (i = 0; i < klass->n_values; i++) {
		ClawtChangeKind kind = (ClawtChangeKind)klass->values[i].value;
		ClawtChangeKind back = CLAWT_CHANGE_REMOVED;

		g_assert_true(*clawt_change_kind_icon(kind) != '\0');
		g_assert_true(*clawt_change_kind_label(kind) != '\0');
		g_assert_true(clawt_change_kind_from_nick(
			klass->values[i].value_nick, &back));
		g_assert_cmpint(back, ==, kind);
	}

	g_assert_false(clawt_change_kind_from_nick("nonsense", &(ClawtChangeKind){0}));
}

/* A change note is never unread; an ordinary message still is. */
static void
test_never_unread(void)
{
	g_autoptr(GHashTable) rows = g_hash_table_new(g_str_hash, g_str_equal);

	g_hash_table_add(rows, (gpointer)"dm:alpha:user");
	g_assert_true(clawt_unread_should_count_message("dm:alpha:user", NULL,
		CLAWT_SYSTEM_SENDER, NULL, 2000, 1000, rows));
	g_assert_false(clawt_unread_should_count_message("dm:alpha:user", NULL,
		CLAWT_SYSTEM_SENDER, "routine", 2000, 1000, rows));
}

/* Nor does a change note draw the "New messages" rule above itself. */
static void
test_no_rule_above_a_note(void)
{
	g_autoptr(JsonNode) only_notes = json_from_string(
		"[{\"id\":\"m1\",\"sender\":\"clawtilla\",\"change\":\"routine\"}]",
		NULL);
	g_autoptr(JsonNode) then_words = json_from_string(
		"[{\"id\":\"m1\",\"sender\":\"clawtilla\",\"change\":\"routine\"},"
		"{\"id\":\"m2\",\"sender\":\"alpha\"}]", NULL);

	g_assert_cmpint(clawt_unread_rule_index(
		json_node_get_array(only_notes), ""), ==, -1);
	g_assert_cmpint(clawt_unread_rule_index(
		json_node_get_array(then_words), ""), ==, 1);
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *path;
	GMainContext *context;
	ClawtDaemon  *daemon;
} Fixture;

static const gchar FLEET[] =
	"agents:\n"
	"  - id: alpha\n    computer:\n      type: none\n"
	"    runtime:\n      autostart: false\n"
	"    tools:\n      schedule: true\n"
	"  - id: beta\n    computer:\n      type: none\n"
	"    runtime:\n      autostart: false\n"
	"routines:\n"
	"  - id: watch\n    agent: alpha\n    description: Bass Pro watch\n"
	"    instructions: look at the page\n    schedule: manual\n"
	"  - id: beta-check\n    agent: beta\n    description: Beta check\n"
	"    instructions: check\n    schedule: manual\n";

static void
start_daemon(
	Fixture *fixture
){
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
stop_daemon(
	Fixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
}

static void
setup(
	Fixture     *fixture,
	const gchar *notes
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-change-XXXXXX", NULL);
	fixture->path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"orchestration:\n  config_notes: %s\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir, fixture->dir, notes, FLEET);
	g_assert_true(g_file_set_contents(fixture->path, yaml, -1, &error));
	start_daemon(fixture);
}

static void
teardown(
	Fixture *fixture
){
	stop_daemon(fixture);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->path);
}

/* The change notes in @agent's chat with the operator, oldest first. */
static GPtrArray *
notes_for(
	Fixture     *fixture,
	const gchar *agent
){
	ClawtRoom *room = clawt_room_manager_get_direct(fixture->daemon->rooms,
	                                                "user", agent);
	g_autoptr(GPtrArray) history = clawt_room_get_history(room, 0);
	GPtrArray *notes = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_message_free);
	guint i;

	for (i = 0; history != NULL && i < history->len; i++) {
		ClawtMessage *message = g_ptr_array_index(history, i);

		if (clawt_message_get_change(message) != NULL)
			g_ptr_array_add(notes, clawt_message_copy(message));
	}

	return notes;
}

/* One request through the handler every client's request reaches. */
static void
request(
	Fixture     *fixture,
	const gchar *kind,
	const gchar *payload_json,
	const gchar *acting
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
	g_autoptr(JsonNode) payload = json_from_string(payload_json, NULL);
	g_autoptr(JsonNode) reply = NULL;

	clawt_ipc_frame_set_payload(frame, g_steal_pointer(&payload));
	if (acting != NULL)
		json_object_set_string_member(json_node_get_object(frame),
		                              "acting_agent", acting);

	reply = clawt_daemon_handle_request(fixture->daemon, frame);
	g_assert_nonnull(reply);
	g_assert_false(clawt_ipc_frame_is_error(reply));
}

/*
 * An agent switching its own routine off through its tool: one note in
 * its own chat, naming it, recorded and never delivered -- and nothing
 * in the other agent's chat.  The first daemon half of the screenshot
 * that asked for this.
 */
static void
test_tool_call_is_noted(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) alpha = NULL;
	g_autoptr(GPtrArray) beta = NULL;
	g_autoptr(JsonNode) call = json_from_string(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"clawtilla_routine_update\","
		"\"arguments\":{\"id\":\"watch\",\"enabled\":false}}}", NULL);
	g_autoptr(JsonNode) response = NULL;
	ClawtMessage *note;
	ClawtAgent *agent;

	setup(&fixture, "agents");

	response = clawt_mcp_tools_call(fixture.daemon->mcp_tools, "alpha", NULL,
	                                call);
	g_assert_nonnull(response);
	g_assert_null(clawt_change_actor_get());

	alpha = notes_for(&fixture, "alpha");
	beta = notes_for(&fixture, "beta");
	g_assert_cmpuint(alpha->len, ==, 1);
	g_assert_cmpuint(beta->len, ==, 0);

	note = g_ptr_array_index(alpha, 0);
	g_assert_cmpstr(clawt_message_get_change(note), ==, "routine");
	g_assert_cmpstr(clawt_message_get_change_actor(note), ==, "alpha");
	g_assert_true(clawt_message_is_from_system(note));
	g_assert_false(clawt_message_get_invites_reply(note));
	g_assert_cmpstr(clawt_message_get_body(note), ==,
	                "Disabled routine Bass Pro watch\nby alpha");

	/* recorded, never delivered: nothing waits in the agent's mailbox */
	agent = clawt_agent_manager_get(fixture.daemon->agents, "alpha");
	g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(agent)), ==, 0);

	teardown(&fixture);
}

/*
 * The CLI run in alpha's shell changing beta's routine: noted in beta's
 * chat as alpha's, "via the CLI".  Two agents and two rooms, because the
 * room is keyed and one key cannot show a shared-scalar bug.
 */
static void
test_cli_hint_is_attribution(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) alpha = NULL;
	g_autoptr(GPtrArray) beta = NULL;

	setup(&fixture, "agents");
	request(&fixture, "routine.update",
	        "{\"id\":\"beta-check\",\"enabled\":false}", "alpha");

	alpha = notes_for(&fixture, "alpha");
	beta = notes_for(&fixture, "beta");
	g_assert_cmpuint(alpha->len, ==, 0);
	g_assert_cmpuint(beta->len, ==, 1);
	g_assert_cmpstr(clawt_message_get_change_actor(
		g_ptr_array_index(beta, 0)), ==, "alpha");
	g_assert_true(g_str_has_suffix(clawt_message_get_body(
		g_ptr_array_index(beta, 0)), "by alpha, via the CLI"));

	teardown(&fixture);
}

/* The operator's own change is not noted by default; a ghost is the operator. */
static void
test_operator_not_noted_by_default(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) beta = NULL;

	setup(&fixture, "agents");
	request(&fixture, "routine.update",
	        "{\"id\":\"beta-check\",\"enabled\":false}", NULL);
	request(&fixture, "routine.update",
	        "{\"id\":\"beta-check\",\"enabled\":true}", "ghost");

	beta = notes_for(&fixture, "beta");
	g_assert_cmpuint(beta->len, ==, 0);

	teardown(&fixture);
}

/*
 * Under `all` the operator's change is noted; and the note survives a
 * restart with its members, because it is read back from the transcript.
 */
static void
test_all_and_restart(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) before = NULL;
	g_autoptr(GPtrArray) after = NULL;

	setup(&fixture, "all");
	request(&fixture, "routine.update",
	        "{\"id\":\"beta-check\",\"enabled\":false}", NULL);

	before = notes_for(&fixture, "beta");
	g_assert_cmpuint(before->len, ==, 1);
	g_assert_cmpstr(clawt_message_get_change_actor(
		g_ptr_array_index(before, 0)), ==, "user");

	stop_daemon(&fixture);
	start_daemon(&fixture);

	after = notes_for(&fixture, "beta");
	g_assert_cmpuint(after->len, ==, 1);
	g_assert_cmpstr(clawt_message_get_change(g_ptr_array_index(after, 0)),
	                ==, "routine");
	g_assert_cmpstr(clawt_message_get_change_actor(
		g_ptr_array_index(after, 0)), ==, "user");

	teardown(&fixture);
}

/*
 * A hand edit taken up by a reload is not pinned on whoever saves next:
 * the snapshot is rebased, so the next agent's note names only its own
 * change.
 */
static void
test_hand_edit_is_not_attributed(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GPtrArray) alpha = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *edited = NULL;

	setup(&fixture, "agents");

	g_assert_true(g_file_get_contents(fixture.path, &text, NULL, &error));
	edited = g_strconcat(text, "  - id: by-hand\n    agent: alpha\n"
	                     "    instructions: edited\n    schedule: manual\n",
	                     NULL);
	g_assert_true(g_file_set_contents(fixture.path, edited, -1, &error));
	g_assert_true(clawt_daemon_reload(fixture.daemon, &error));
	g_assert_no_error(error);

	request(&fixture, "routine.update",
	        "{\"id\":\"watch\",\"enabled\":false}", "alpha");

	alpha = notes_for(&fixture, "alpha");
	g_assert_cmpuint(alpha->len, ==, 1);
	g_assert_null(strstr(clawt_message_get_body(g_ptr_array_index(alpha, 0)),
	                     "by-hand"));

	teardown(&fixture);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/change/diff/setting", test_setting_old_and_new);
	g_test_add_func("/change/diff/nothing", test_nothing_changed_is_no_note);
	g_test_add_func("/change/diff/credentials", test_credentials_never_shown);
	g_test_add_func("/change/diff/routines", test_routines_by_id);
	g_test_add_func("/change/diff/routine-fields",
	                test_routine_edit_names_fields);
	g_test_add_func("/change/diff/trigger-agents",
	                test_trigger_tells_every_agent);
	g_test_add_func("/change/diff/created-removed", test_created_and_removed);
	g_test_add_func("/change/note/body", test_note_body);
	g_test_add_func("/change/note/should", test_should_note);
	g_test_add_func("/change/actor/stack", test_actor_stack);
	g_test_add_func("/change/kind/every", test_every_kind_is_drawn);
	g_test_add_func("/change/unread/never", test_never_unread);
	g_test_add_func("/change/unread/no-rule", test_no_rule_above_a_note);
	g_test_add_func("/change/daemon/tool-call", test_tool_call_is_noted);
	g_test_add_func("/change/daemon/cli-hint", test_cli_hint_is_attribution);
	g_test_add_func("/change/daemon/operator",
	                test_operator_not_noted_by_default);
	g_test_add_func("/change/daemon/all-and-restart", test_all_and_restart);
	g_test_add_func("/change/daemon/hand-edit",
	                test_hand_edit_is_not_attributed);

	return g_test_run();
}
