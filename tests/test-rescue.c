/*
 * test-rescue.c - First-reply rescue: a turn that has shown nothing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The watch is keyed on agent and room, so every test of it uses two
 * rooms: one room's silence must say nothing about another's.  The
 * daemon half raises the typing edge the way libreclaw would -- through
 * the link server's own signal -- and moves the turn watch's fake clock,
 * since twenty seconds cannot be reached by waiting in a test that must
 * not hang.  The switch is tested at the mailbox, where the message is
 * handed back with its model, and through `turn.rescue` itself with a
 * runtime whose interrupt succeeds, since a hermetic test has no CLI to
 * interrupt.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

#define SECOND G_USEC_PER_SEC

static guint
twenty(
	const gchar *agent_id,
	gpointer     data
){
	(void)data;

	return g_strcmp0(agent_id, "quiet") == 0 ? 0 : 20;
}

/* Once only, per room, and a sign of life withdraws only its own room. */
static void
test_the_watch_offers_once_per_room(
	void
){
	g_autoptr(ClawtRescueWatch) watch = clawt_rescue_watch_new();
	g_autoptr(GPtrArray) early = NULL;
	g_autoptr(GPtrArray) due = NULL;
	g_autoptr(GPtrArray) again = NULL;
	g_autoptr(GPtrArray) offered = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *room = NULL;

	clawt_rescue_watch_begin(watch, "alpha", "room-a", 0);
	clawt_rescue_watch_begin(watch, "alpha", "room-b", 0);
	clawt_rescue_watch_begin(watch, "quiet", "room-a", 0);

	early = clawt_rescue_watch_collect_due(watch, 19 * SECOND, twenty, NULL);
	g_assert_cmpuint(early->len, ==, 0);

	/* A step in room-b: that room is not silent; room-a still is. */
	g_assert_false(clawt_rescue_watch_note_life(watch, "alpha", "room-b"));

	due = clawt_rescue_watch_collect_due(watch, 20 * SECOND, twenty, NULL);
	g_assert_cmpuint(due->len, ==, 1);
	clawt_rescue_key_split(g_ptr_array_index(due, 0), &agent, &room);
	g_assert_cmpstr(agent, ==, "alpha");
	g_assert_cmpstr(room, ==, "room-a");

	/* Not offered twice, and the agent with 0 never at all. */
	again = clawt_rescue_watch_collect_due(watch, 600 * SECOND, twenty,
		NULL);
	g_assert_cmpuint(again->len, ==, 0);

	offered = clawt_rescue_watch_offered(watch, "alpha");
	g_assert_cmpuint(offered->len, ==, 1);
	g_assert_cmpint(clawt_rescue_watch_silent_for(watch, "alpha", "room-a",
		25 * SECOND), ==, 25);

	/* Life after the offer says the offer is withdrawn. */
	g_assert_true(clawt_rescue_watch_note_life(watch, "alpha", "room-a"));
	g_assert_cmpint(clawt_rescue_watch_silent_for(watch, "alpha", "room-a",
		25 * SECOND), ==, -1);
}

/*
 * Only the agent's own CLI and login: one turn's model is a name on the
 * CLI it is already running, so a codex entry on a claude agent is
 * listed with the reason, and "claude" and "claude-code" are one CLI.
 */
static void
test_switch_choices_stay_on_the_running_cli(
	void
){
	g_autoptr(GPtrArray) fallbacks = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_model_fallback_free);
	g_autoptr(GPtrArray) refused = NULL;
	g_autoptr(GPtrArray) choices = NULL;

	g_ptr_array_add(fallbacks, clawt_model_fallback_new("codex-cli",
		"gpt-5.5", NULL));
	g_ptr_array_add(fallbacks, clawt_model_fallback_new("claude-code",
		"sonnet", NULL));
	g_ptr_array_add(fallbacks, clawt_model_fallback_new("claude-code",
		"haiku", "backup"));
	g_ptr_array_add(fallbacks, clawt_model_fallback_new("claude-code",
		"sonnet", NULL));

	choices = clawt_rescue_switch_choices("claude-code", NULL, fallbacks,
		&refused);
	g_assert_cmpuint(choices->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(choices, 0), ==, "sonnet");
	g_assert_cmpuint(refused->len, ==, 2);
	g_assert_nonnull(strstr(g_ptr_array_index(refused, 0), "another CLI"));
	g_assert_nonnull(strstr(g_ptr_array_index(refused, 1),
		"another login"));
}

static void
test_the_words(
	void
){
	g_autofree gchar *offer = clawt_rescue_offer_text("alpha", 20);
	g_autofree gchar *label = clawt_rescue_switch_label("sonnet");

	g_assert_nonnull(strstr(offer, "alpha has not replied"));
	g_assert_nonnull(strstr(offer, "20 seconds"));
	g_assert_null(strstr(offer, "is stuck"));
	g_assert_cmpstr(label, ==, "Try sonnet instead");
}

/* ── Handing the message back ────────────────────────────────────── */

static gchar *
delivered(
	ClawtMailbox *mailbox,
	const gchar  *room,
	gint64       *turn
){
	g_autoptr(ClawtMailboxItem) item = clawt_mailbox_item_new("user", "alpha",
		"summarise the report");
	g_autoptr(ClawtMailboxItem) leased = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *ids[2] = { NULL, NULL };
	gchar *id;

	clawt_mailbox_item_set_room(item, room);
	id = clawt_mailbox_post(mailbox, item, &error);
	g_assert_no_error(error);
	leased = clawt_mailbox_lease_id(mailbox, id, 0);
	g_assert_nonnull(leased);
	g_assert_true(clawt_mailbox_ack(mailbox, id, &error));

	ids[0] = id;
	*turn = clawt_mailbox_turn_open(mailbox, room, ids, 0, &error);
	g_assert_no_error(error);

	return id;
}

/*
 * The running turn's message goes back into the queue with the chosen
 * model riding on it; a turn that already acted outward is refused,
 * since running it again could do that twice; and the other room's
 * turn is left alone.
 */
static void
test_hand_back_carries_the_model(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-rescue-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autoptr(ClawtMailbox) mailbox = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMailboxItem) back = NULL;
	g_autofree gchar *here = NULL;
	g_autofree gchar *there = NULL;
	gint64 turn_here = 0;
	gint64 turn_there = 0;

	mailbox = clawt_mailbox_new("alpha", path, &error);
	g_assert_no_error(error);

	here = delivered(mailbox, "dm:alpha:user", &turn_here);
	there = delivered(mailbox, "planning", &turn_there);

	g_assert_cmpuint(clawt_mailbox_turn_hand_back(mailbox, "dm:alpha:user",
		"sonnet", &error), ==, 1);
	g_assert_no_error(error);

	back = clawt_mailbox_get(mailbox, here);
	g_assert_cmpint(clawt_mailbox_item_get_state(back), ==,
		CLAWT_MAILBOX_PENDING);
	g_assert_cmpstr(clawt_mailbox_item_get_model(back), ==, "sonnet");

	/* The other room's turn is still open. */
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);

	/* Nothing open in the first room any more. */
	g_assert_cmpuint(clawt_mailbox_turn_hand_back(mailbox, "dm:alpha:user",
		"sonnet", &error), ==, 0);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A turn that already sent something is not run again. */
	g_assert_true(clawt_mailbox_turn_mark_outward(mailbox, turn_there,
		"sent an email", &error));
	g_assert_cmpuint(clawt_mailbox_turn_hand_back(mailbox, "planning",
		"sonnet", &error), ==, 0);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "outside the fleet"));

	g_clear_object(&mailbox);
	clawt_test_remove_tree(dir);
}

/* ── Through the daemon ──────────────────────────────────────────── */

static gint64 fake_now;

static gint64
fake_clock(
	gpointer user_data
){
	(void)user_data;

	return fake_now;
}

typedef struct {
	guint  offered;
	guint  cleared;
	gchar *room;
	gchar *choices;
} Seen;

static void
on_event(
	ClawtEventBus *bus,
	ClawtEvent    *event,
	gpointer       user_data
){
	Seen *seen = user_data;

	(void)bus;

	if (g_strcmp0(clawt_event_get_kind(event), "turn.no_reply") == 0) {
		seen->offered++;
		g_free(seen->room);
		seen->room = g_strdup(clawt_event_get_detail(event, "room"));
		g_free(seen->choices);
		seen->choices = g_strdup(clawt_event_get_detail(event, "choices"));
	}

	if (g_strcmp0(clawt_event_get_kind(event), "turn.no_reply_cleared") == 0)
		seen->cleared++;
}

/*
 * A silent turn is offered after the agent's allowance and withdrawn by
 * its first step; an agent whose steps are not streamed is never
 * offered, because silence is all there is until it answers.
 */
static void
test_the_daemon_offers_and_withdraws(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-rescue-d-XXXXXX", NULL);
	g_autofree gchar *config_path = g_build_filename(dir, "config.yaml",
		NULL);
	g_autofree gchar *yaml = NULL;
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(JsonNode) frame = NULL;
	g_autoptr(ClawtTurnStep) step = NULL;
	ClawtLinkServer *links;
	Seen seen = { 0 };
	JsonArray *offers;

	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: alpha\n"
		"    model:\n"
		"      provider: claude-code\n"
		"      fallbacks:\n"
		"        - provider: claude-code\n"
		"          model: sonnet\n"
		"    runtime:\n"
		"      stream_steps: true\n"
		"      first_reply_seconds: 20\n"
		"  - id: blind\n"
		"    runtime:\n"
		"      stream_steps: false\n",
		dir, dir, dir, dir, dir, dir);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	daemon = clawt_daemon_new(config_path, context);
	g_assert_true(clawt_daemon_start(daemon, NULL));

	fake_now = 1000 * SECOND;
	clawt_turn_watch_set_clock(daemon->turn_watch, fake_clock, NULL, NULL);
	g_signal_connect(clawt_daemon_get_event_bus(daemon), "event",
		G_CALLBACK(on_event), &seen);

	links = clawt_daemon_get_link_server(daemon);
	g_signal_emit_by_name(links, "typing", "alpha", "dm:alpha:user", TRUE);
	g_signal_emit_by_name(links, "typing", "blind", "dm:blind:user", TRUE);

	fake_now += 15 * SECOND;
	clawt_daemon_turn_sweep_now(daemon);
	g_assert_cmpuint(seen.offered, ==, 0);

	fake_now += 10 * SECOND;
	clawt_daemon_turn_sweep_now(daemon);
	g_assert_cmpuint(seen.offered, ==, 1);
	g_assert_cmpstr(seen.room, ==, "dm:alpha:user");
	g_assert_cmpstr(seen.choices, ==, "sonnet");

	/* A client that connects now still finds the offer. */
	frame = clawt_ipc_request_new("turn.rescue", "t1");
	listing = clawt_daemon_handle_request(daemon, frame);
	offers = json_object_get_array_member(json_object_get_object_member(
		json_node_get_object(listing), "payload"), "offers");
	g_assert_cmpuint(json_array_get_length(offers), ==, 1);

	/* Its first step withdraws it, and says so. */
	step = clawt_turn_step_new(CLAWT_STEP_THINKING, "alpha",
		"dm:alpha:user", "Reading the report first.", NULL, NULL, FALSE);
	clawt_daemon_note_step(daemon, step);
	g_assert_cmpuint(seen.cleared, ==, 1);

	/* blind never: its steps are not streamed. */
	fake_now += 600 * SECOND;
	clawt_daemon_turn_sweep_now(daemon);
	g_assert_cmpuint(seen.offered, ==, 1);

	g_signal_handlers_disconnect_by_data(clawt_daemon_get_event_bus(daemon),
		&seen);
	clawt_daemon_stop(daemon);
	g_clear_object(&daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	g_free(seen.room);
	g_free(seen.choices);
	clawt_test_remove_tree(dir);
}

/* ── Stop and switch, through turn.rescue ────────────────────────── */

/*
 * A runtime whose interrupt succeeds, so the daemon's half of Stop and
 * Switch runs end to end without a libreclaw child.  It records what
 * the room's message looked like at the moment of the interrupt: a
 * switch must hand the message back *before* interrupting, because the
 * interrupt's settle is what drains it again.
 */
typedef struct {
	ClawtAgentRuntime  parent_instance;
	guint              interrupts;
	ClawtMailbox      *mailbox;     /* borrowed */
	gchar             *watch_id;
	gint               state_at_interrupt;
	gchar             *model_at_interrupt;
} FakeRuntime;

typedef struct {
	ClawtAgentRuntimeClass parent_class;
} FakeRuntimeClass;

static GType fake_runtime_get_type(void);
G_DEFINE_TYPE(FakeRuntime, fake_runtime, CLAWT_TYPE_AGENT_RUNTIME)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FakeRuntime, g_object_unref)

static gboolean
fake_runtime_interrupt(
	ClawtAgentRuntime  *runtime,
	guint              *out_killed,
	GError            **error
){
	FakeRuntime *self = (FakeRuntime *)runtime;

	(void)error;

	self->interrupts++;

	if (self->mailbox != NULL && self->watch_id != NULL) {
		g_autoptr(ClawtMailboxItem) item =
			clawt_mailbox_get(self->mailbox, self->watch_id);

		self->state_at_interrupt = (gint)clawt_mailbox_item_get_state(item);
		g_free(self->model_at_interrupt);
		self->model_at_interrupt =
			g_strdup(clawt_mailbox_item_get_model(item));
	}

	*out_killed = 2;

	return TRUE;
}

static gboolean
fake_runtime_is_alive(
	ClawtAgentRuntime *runtime
){
	(void)runtime;

	return TRUE;
}

static void
fake_runtime_finalize(
	GObject *object
){
	FakeRuntime *self = (FakeRuntime *)object;

	g_free(self->watch_id);
	g_free(self->model_at_interrupt);

	G_OBJECT_CLASS(fake_runtime_parent_class)->finalize(object);
}

static void
fake_runtime_class_init(
	FakeRuntimeClass *klass
){
	ClawtAgentRuntimeClass *runtime_class = CLAWT_AGENT_RUNTIME_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = fake_runtime_finalize;
	runtime_class->interrupt = fake_runtime_interrupt;
	runtime_class->is_alive = fake_runtime_is_alive;
}

static void
fake_runtime_init(
	FakeRuntime *self
){
	self->state_at_interrupt = -1;
}

static JsonObject *
rescue_reply(
	ClawtDaemon *daemon,
	const gchar *action,
	const gchar *room,
	const gchar *model,
	JsonNode   **keep
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new("turn.rescue", "r1");
	JsonObject *payload = json_object_new();

	json_object_set_string_member(payload, "action", action);
	json_object_set_string_member(payload, "agent", "alpha");

	if (room != NULL)
		json_object_set_string_member(payload, "room", room);
	if (model != NULL)
		json_object_set_string_member(payload, "model", model);

	json_object_set_object_member(json_node_get_object(frame), "payload",
		payload);
	*keep = clawt_daemon_handle_request(daemon, frame);

	return json_node_get_object(*keep);
}

static guint
offers_now(
	ClawtDaemon *daemon
){
	g_autoptr(JsonNode) frame = clawt_ipc_request_new("turn.rescue", "l1");
	g_autoptr(JsonNode) listing = clawt_daemon_handle_request(daemon, frame);

	return json_array_get_length(json_object_get_array_member(
		json_object_get_object_member(json_node_get_object(listing),
			"payload"), "offers"));
}

/*
 * The two answers to an offer, through the IPC verb the clients send.
 * Stop interrupts and withdraws the offer.  Switch refuses a model that
 * is not one of the turn's choices without touching anything, refuses
 * without a room, and otherwise hands the running turn's message back
 * with the chosen model *before* the interrupt -- the order is the
 * whole mechanism, so the fake runtime checks it at the moment of the
 * interrupt -- and withdraws the offer.  Two rooms, so the switch is
 * seen to take its own room's message and not the other's.
 */
static void
test_stop_and_switch_through_the_daemon(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-rescue-s-XXXXXX", NULL);
	g_autofree gchar *config_path = g_build_filename(dir, "config.yaml",
		NULL);
	g_autofree gchar *yaml = NULL;
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(FakeRuntime) runtime = NULL;
	g_autoptr(ClawtMailboxItem) back = NULL;
	g_autoptr(ClawtMailboxItem) other = NULL;
	g_autoptr(JsonNode) stopped = NULL;
	g_autoptr(JsonNode) wrong = NULL;
	g_autoptr(JsonNode) roomless = NULL;
	g_autoptr(JsonNode) switched = NULL;
	g_autofree gchar *here = NULL;
	g_autofree gchar *there = NULL;
	ClawtLinkServer *links;
	ClawtAgent *alpha;
	ClawtMailbox *mailbox;
	JsonObject *reply;
	Seen seen = { 0 };
	gint64 turn_here = 0;
	gint64 turn_there = 0;

	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s/secrets\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: alpha\n"
		"    model:\n"
		"      provider: claude-code\n"
		"      fallbacks:\n"
		"        - provider: claude-code\n"
		"          model: sonnet\n"
		"    runtime:\n"
		"      stream_steps: true\n"
		"      first_reply_seconds: 20\n",
		dir, dir, dir, dir, dir, dir);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, NULL));

	daemon = clawt_daemon_new(config_path, context);
	g_assert_true(clawt_daemon_start(daemon, NULL));

	fake_now = 1000 * SECOND;
	clawt_turn_watch_set_clock(daemon->turn_watch, fake_clock, NULL, NULL);
	g_signal_connect(clawt_daemon_get_event_bus(daemon), "event",
		G_CALLBACK(on_event), &seen);

	alpha = clawt_agent_manager_get(clawt_daemon_get_agents(daemon), "alpha");
	mailbox = clawt_agent_get_mailbox(alpha);
	runtime = g_object_new(fake_runtime_get_type(), NULL);
	runtime->mailbox = mailbox;
	clawt_agent_set_runtime(alpha, CLAWT_AGENT_RUNTIME(runtime));

	/* A silent turn in one room, offered. */
	links = clawt_daemon_get_link_server(daemon);
	g_signal_emit_by_name(links, "typing", "alpha", "dm:alpha:user", TRUE);
	fake_now += 25 * SECOND;
	clawt_daemon_turn_sweep_now(daemon);
	g_assert_cmpuint(seen.offered, ==, 1);
	g_assert_cmpuint(offers_now(daemon), ==, 1);

	/* Stop: interrupted, the offer answered and withdrawn. */
	reply = rescue_reply(daemon, "stop", "dm:alpha:user", NULL, &stopped);
	g_assert_false(json_object_has_member(reply, "error"));
	reply = json_object_get_object_member(reply, "payload");
	g_assert_true(json_object_get_boolean_member(reply, "done"));
	g_assert_cmpint(json_object_get_int_member(reply, "killed"), ==, 2);
	g_assert_cmpuint(runtime->interrupts, ==, 1);
	g_assert_cmpuint(seen.cleared, ==, 1);
	g_assert_cmpuint(offers_now(daemon), ==, 0);

	/* A new silent turn, with a message in each of two rooms. */
	here = delivered(mailbox, "dm:alpha:user", &turn_here);
	there = delivered(mailbox, "planning", &turn_there);
	runtime->watch_id = g_strdup(here);
	g_signal_emit_by_name(links, "typing", "alpha", "dm:alpha:user", FALSE);
	g_signal_emit_by_name(links, "typing", "alpha", "dm:alpha:user", TRUE);
	fake_now += 25 * SECOND;
	clawt_daemon_turn_sweep_now(daemon);
	g_assert_cmpuint(offers_now(daemon), ==, 1);

	/* A model that is not a choice: refused, nothing touched. */
	reply = rescue_reply(daemon, "switch", "dm:alpha:user", "opus", &wrong);
	g_assert_true(json_object_has_member(reply, "error"));
	g_assert_cmpuint(runtime->interrupts, ==, 1);
	g_assert_cmpuint(offers_now(daemon), ==, 1);

	/* No room: refused -- a switch reruns one room's turn. */
	reply = rescue_reply(daemon, "switch", NULL, "sonnet", &roomless);
	g_assert_true(json_object_has_member(reply, "error"));
	g_assert_cmpuint(runtime->interrupts, ==, 1);

	/* Switch: handed back with the model, then interrupted. */
	reply = rescue_reply(daemon, "switch", "dm:alpha:user", "sonnet",
		&switched);
	g_assert_false(json_object_has_member(reply, "error"));
	g_assert_true(json_object_get_boolean_member(
		json_object_get_object_member(reply, "payload"), "done"));
	g_assert_cmpuint(runtime->interrupts, ==, 2);
	g_assert_cmpint(runtime->state_at_interrupt, ==, CLAWT_MAILBOX_PENDING);
	g_assert_cmpstr(runtime->model_at_interrupt, ==, "sonnet");
	g_assert_cmpuint(offers_now(daemon), ==, 0);
	g_assert_cmpuint(seen.cleared, ==, 2);

	back = clawt_mailbox_get(mailbox, here);
	g_assert_cmpstr(clawt_mailbox_item_get_model(back), ==, "sonnet");

	/* The other room's message stays delivered, on its own model. */
	other = clawt_mailbox_get(mailbox, there);
	g_assert_cmpint(clawt_mailbox_item_get_state(other), ==,
		CLAWT_MAILBOX_ACKED);
	g_assert_null(clawt_mailbox_item_get_model(other));

	g_signal_handlers_disconnect_by_data(clawt_daemon_get_event_bus(daemon),
		&seen);
	clawt_daemon_stop(daemon);
	g_clear_object(&daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	g_free(seen.room);
	g_free(seen.choices);
	clawt_test_remove_tree(dir);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/rescue/watch/once-per-room",
		test_the_watch_offers_once_per_room);
	g_test_add_func("/rescue/choices/running-cli",
		test_switch_choices_stay_on_the_running_cli);
	g_test_add_func("/rescue/words", test_the_words);
	g_test_add_func("/rescue/mailbox/hand-back",
		test_hand_back_carries_the_model);
	g_test_add_func("/rescue/daemon/offer-and-withdraw",
		test_the_daemon_offers_and_withdraws);
	g_test_add_func("/rescue/daemon/stop-and-switch",
		test_stop_and_switch_through_the_daemon);

	return g_test_run();
}
