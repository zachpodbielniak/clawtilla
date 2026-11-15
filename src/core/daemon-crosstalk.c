/*
 * daemon-crosstalk.c - Telling the operator about agent-to-agent exchanges
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A Grok bot reaching out to the chief, or a delegation settling between
 * two agents, happens in Flow -- and the person running the fleet saw it
 * only by going to look.  With `orchestration.crosstalk_summary` (chief
 * by default, per agent `crosstalk_summary`), each exchange that ends is
 * summarised into the operator's own conversation with every agent that
 * took part and has it on -- or, with `crosstalk_summary_to: source` (the
 * default, since 0.33.0), back where the request came from: the Matrix
 * room or the clawtilla room whose turn began the exchange.
 *
 * The note is recorded, never sent: clawt_mailbox_router_record() puts
 * it in the room, the recall index and one `message` event, and queues
 * it into no mailbox -- so it starts no turn, invites no reply, is no
 * hop and passes no loop guard, tree budget, incident desk or
 * acknowledgement check.  It lands in a room with the operator in it,
 * which is not cross-talk, so it is never summarised itself.
 *
 * What has been summarised is kept in <state_dir>/crosstalk-summaries.json
 * -- the last message of each room's last summarised exchange -- so a
 * restart picks up an exchange that was still going and never repeats
 * one that was already told.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

#include <json-glib/json-glib.h>

#include <string.h>

/* How often ended exchanges are looked for. */
#define CROSSTALK_TICK_SECONDS (2)

/* How far back a restart looks for an exchange it did not finish. */
#define CROSSTALK_BACKFILL_SECONDS (6 * 3600)

/* How much of a room's history a restart reads. */
#define CROSSTALK_BACKFILL_MESSAGES (200)

/* More than this many waiting for one conversation are posted at once. */
#define CROSSTALK_MAX_SECTIONS (8)

#define CROSSTALK_LEDGER "crosstalk-summaries.json"

/*
 * Where a note goes.  Your chat with the agent unless the exchange came
 * from somewhere else that should hear how it went (crosstalk_summary_to).
 */
typedef enum {
	DEST_CHAT = 0,   /* your direct room with the agent */
	DEST_ROOM,       /* a clawtilla room you are in: a group */
	DEST_MATRIX      /* a room on the agent's Matrix channel */
} Dest;

/* Summaries ended for one destination, waiting to be posted together. */
typedef struct {
	GPtrArray *exchanges;   /* ClawtCrosstalkExchange* */
	gint64     last_added;  /* microseconds */
	gchar     *agent_id;
	Dest       dest;
	gchar     *target;      /* DEST_ROOM, DEST_MATRIX: the room */
} Waiting;

/* Where a room was last summarised up to. */
typedef struct {
	gchar  *through;        /* message id */
	gint64  at;             /* seconds */
} Through;

struct _ClawtDaemonCrosstalk {
	ClawtCrosstalk *tracker;
	GSource        *tick;
	gulong          bus_handler;
	GHashTable     *waiting;    /* agent + destination -> Waiting* */
	GHashTable     *ledger;     /* room id -> Through* */
	gchar          *ledger_path;
	AiProvider     *provider;
	gchar          *provider_spec;
	AiProvider     *provider_override;
	guint           rewrites;   /* in flight */

	/* A test's stand-in for posting into a channel (Matrix). */
	ClawtDaemonCrosstalkChannelSend channel_send;
	gpointer                        channel_send_data;
};

static void
waiting_free(
	gpointer data
){
	Waiting *waiting = data;

	g_ptr_array_unref(waiting->exchanges);
	g_free(waiting->agent_id);
	g_free(waiting->target);
	g_free(waiting);
}

static void
through_free(
	gpointer data
){
	Through *through = data;

	g_free(through->through);
	g_free(through);
}

/* ── The ledger ──────────────────────────────────────────────────── */

static void
ledger_save(
	ClawtDaemon *daemon
){
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (self->ledger_path == NULL)
		return;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 1);
	json_builder_set_member_name(builder, "rooms");
	json_builder_begin_object(builder);

	g_hash_table_iter_init(&iter, self->ledger);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Through *through = value;

		json_builder_set_member_name(builder, key);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "through");
		json_builder_add_string_value(builder, through->through);
		json_builder_set_member_name(builder, "at");
		json_builder_add_int_value(builder, through->at);
		json_builder_end_object(builder);
	}

	json_builder_end_object(builder);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);

	/*
	 * A failure is warned about and nothing else: the worst it costs is
	 * an exchange told twice after the next restart, which is better
	 * than one never told because the disk was full.
	 */
	if (!clawt_write_file_atomic(self->ledger_path, text, -1, 0600, FALSE,
	                             &error))
		g_warning("crosstalk: %s could not be written: %s",
		          self->ledger_path, error->message);
}

static void
ledger_mark(
	ClawtDaemon *daemon,
	const gchar *room_id,
	const gchar *message_id,
	gint64       at
){
	Through *through;

	if (room_id == NULL || message_id == NULL)
		return;

	through = g_new0(Through, 1);
	through->through = g_strdup(message_id);
	through->at = at;
	g_hash_table_replace(daemon->crosstalk->ledger, g_strdup(room_id),
	                     through);
}

/*
 * Reads the ledger.  Returns FALSE when there is none, which is a fleet
 * that has never had this feature: everything already said is history,
 * not news, and must not arrive as a flood of summaries on first start.
 */
static gboolean
ledger_load(
	ClawtDaemon *daemon
){
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *root;
	JsonObject *rooms;
	GList *names;
	GList *l;

	if (!g_file_test(self->ledger_path, G_FILE_TEST_EXISTS))
		return FALSE;

	if (!json_parser_load_from_file(parser, self->ledger_path, &error) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		/*
		 * Unreadable is treated as present and empty, not absent: the
		 * backfill window still bounds what is replayed, which is a few
		 * hours rather than the fleet's whole history.
		 */
		g_warning("crosstalk: %s is unreadable (%s); starting it again",
		          self->ledger_path,
		          error != NULL ? error->message : "not an object");
		return TRUE;
	}

	root = json_node_get_object(json_parser_get_root(parser));
	rooms = json_object_has_member(root, "rooms")
		? json_object_get_object_member(root, "rooms") : NULL;

	if (rooms == NULL)
		return TRUE;

	names = json_object_get_members(rooms);

	for (l = names; l != NULL; l = l->next) {
		JsonNode *node = json_object_get_member(rooms, l->data);
		JsonObject *entry;

		if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
			continue;

		entry = json_node_get_object(node);

		if (!json_object_has_member(entry, "through"))
			continue;

		ledger_mark(daemon, l->data,
		            json_object_get_string_member(entry, "through"),
		            json_object_has_member(entry, "at")
		                ? json_object_get_int_member(entry, "at") : 0);
	}

	g_list_free(names);

	return TRUE;
}

static GStrv
room_members(
	ClawtRoom *room
){
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	GPtrArray *members = clawt_room_get_members(room);
	guint i;

	for (i = 0; members != NULL && i < members->len; i++)
		g_strv_builder_add(builder, g_ptr_array_index(members, i));

	return g_strv_builder_end(builder);
}

/*
 * The first start with this feature: every cross-talk room is marked
 * summarised up to its last message.  A restart after that replays what
 * came after the mark, so an exchange the daemon stopped in the middle
 * of is told once it ends -- and one it already told is not.
 */
static void
ledger_baseline_or_backfill(
	ClawtDaemon *daemon,
	gboolean     have_ledger
){
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	g_autoptr(GPtrArray) rooms = NULL;
	gint64 now = g_get_real_time();
	gint64 oldest = now / G_USEC_PER_SEC - CROSSTALK_BACKFILL_SECONDS;
	guint i;

	if (daemon->rooms == NULL)
		return;

	rooms = clawt_room_manager_list(daemon->rooms);

	for (i = 0; i < rooms->len; i++) {
		ClawtRoom *room = g_ptr_array_index(rooms, i);
		const gchar *room_id = clawt_room_get_id(room);
		g_auto(GStrv) members = room_members(room);
		g_autoptr(GPtrArray) history = NULL;
		Through *through;
		guint start = 0;
		guint m;

		if (!clawt_crosstalk_room_counts(room_id,
		                                 (const gchar *const *)members))
			continue;

		history = clawt_room_get_history(room, CROSSTALK_BACKFILL_MESSAGES);

		if (history == NULL || history->len == 0)
			continue;

		if (!have_ledger) {
			ClawtMessage *last = g_ptr_array_index(history,
			                                       history->len - 1);

			ledger_mark(daemon, room_id, clawt_message_get_id(last),
			            clawt_message_get_timestamp(last));
			continue;
		}

		through = g_hash_table_lookup(self->ledger, room_id);

		if (through != NULL) {
			for (m = 0; m < history->len; m++) {
				if (g_strcmp0(clawt_message_get_id(
				        g_ptr_array_index(history, m)),
				        through->through) == 0)
					start = m + 1;
			}
		}

		for (m = start; m < history->len; m++) {
			ClawtMessage *message = g_ptr_array_index(history, m);
			gint64 ts = clawt_message_get_timestamp(message);

			/*
			 * A mark that has scrolled out of the history read is
			 * placed by its time instead; and nothing older than the
			 * window is news, mark or no mark.
			 */
			if (ts < oldest ||
				(through != NULL && start == 0 && ts <= through->at))
				continue;

			clawt_crosstalk_note_message(self->tracker, room_id,
				(const gchar *const *)members,
				clawt_message_get_id(message),
				clawt_message_get_sender_id(message),
				clawt_message_get_body(message),
				clawt_message_get_task_id(message),
				clawt_message_get_round(message),
				ts * G_USEC_PER_SEC);
		}
	}

	if (!have_ledger)
		ledger_save(daemon);
}

/* ── Observing ───────────────────────────────────────────────────── */

static void
on_bus_event(
	ClawtEventBus *bus,
	ClawtEvent    *event,
	gpointer       user_data
){
	ClawtDaemon *daemon = user_data;
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	/*
	 * `what`, not `kind`: tools/clawt-client-parity.sh reads
	 * `kind, "..."` in the daemon as an IPC verb it answers, and these
	 * are events it publishes.
	 */
	const gchar *what = clawt_event_get_kind(event);
	const gchar *subject = clawt_event_get_subject(event);
	gint64 now = g_get_real_time();

	(void)bus;

	if (self == NULL || what == NULL)
		return;

	if (g_strcmp0(what, "message") == 0) {
		ClawtRoom *room;
		g_auto(GStrv) members = NULL;

		/* A summary is about cross-talk, never part of it. */
		if (clawt_event_get_detail(event, "crosstalk_room") != NULL ||
			daemon->rooms == NULL)
			return;

		room = clawt_room_manager_get(daemon->rooms, subject);

		if (room == NULL)
			return;

		/*
		 * The message itself, from the room, rather than the event's
		 * copy of it: event details pass through clawt_redact_secrets(),
		 * which has mistaken an id for a token before -- every task id
		 * read as an `sk-` key, "ta[REDACTED]", until 0.28.3 -- and a
		 * task id that names no task would end a delegation's exchange
		 * before the delegation did.  The router publishes right after
		 * it appends, on this stack, so the room's newest message is
		 * this one; the sender is compared to be sure.
		 */
		{
			g_autoptr(GPtrArray) last = clawt_room_get_history(room, 1);
			ClawtMessage *message;

			if (last == NULL || last->len == 0)
				return;

			message = g_ptr_array_index(last, 0);

			if (g_strcmp0(clawt_message_get_sender_id(message),
			              clawt_event_get_detail(event, "from")) != 0 ||
				clawt_message_get_crosstalk_rooms(message) != NULL)
				return;

			members = room_members(room);
			clawt_crosstalk_note_message(self->tracker, subject,
				(const gchar *const *)members,
				clawt_message_get_id(message),
				clawt_message_get_sender_id(message),
				clawt_message_get_body(message),
				clawt_message_get_task_id(message),
				clawt_message_get_round(message), now);

			/*
			 * Where the sender was asked, when it was answering
			 * somebody else: a Matrix room's id, or a group's.  Only
			 * held in memory -- a room's history keeps no request
			 * context -- so an exchange replayed after a restart has
			 * none, and its summary comes to the operator's chat.
			 */
			clawt_crosstalk_note_source(self->tracker, subject,
				clawt_message_get_id(message),
				clawt_message_get_sender_id(message),
				clawt_message_get_request_room(message));
		}
		return;
	}

	if (g_strcmp0(what, "turn.step") == 0) {
		if (g_strcmp0(clawt_event_get_detail(event, CLAWT_STEP_MEMBER_KIND),
		              "tool") == 0)
			clawt_crosstalk_note_step(self->tracker, subject,
				clawt_event_get_detail(event, CLAWT_STEP_MEMBER_ROOM),
				clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TOOL), now);
		else
			clawt_crosstalk_note_activity(self->tracker,
				clawt_event_get_detail(event, CLAWT_STEP_MEMBER_ROOM), now);
		return;
	}

	/*
	 * A turn starting or ending restarts the quiet: libreclaw drops its
	 * indicator before it posts the answer, so the quiet counted from
	 * the last message would end the exchange in the moment before the
	 * answer arrives, and the answer would open a second one.
	 */
	if (g_strcmp0(what, "agent.typing") == 0) {
		clawt_crosstalk_note_activity(self->tracker,
		                              clawt_event_get_detail(event, "room"),
		                              now);
		return;
	}

	if (g_strcmp0(what, "task.changed") == 0) {
		ClawtTask *task = daemon->tasks != NULL
			? clawt_task_manager_get(daemon->tasks, subject) : NULL;

		if (task == NULL)
			return;

		clawt_crosstalk_note_task(self->tracker, subject,
		                          clawt_task_get_room(task),
		                          clawt_task_get_origin(task),
		                          clawt_task_get_assignee(task),
		                          (gint)clawt_task_get_state(task), now);
		return;
	}

	if (g_strcmp0(what, "decision.asked") == 0) {
		clawt_crosstalk_note_decision(self->tracker, subject,
		                              clawt_event_get_detail(event, "room"),
		                              now);
		return;
	}

	if (g_strcmp0(what, "message.refused") == 0)
		clawt_crosstalk_note_refused(self->tracker, subject, now);
}

/*
 * Whether something still holds an exchange open: a turn running in its
 * room, a message for its room waiting in somebody's mailbox (held
 * results included -- they are pending items), or a task it carried
 * still running.  The quiet alone cannot see any of these: a turn on a
 * non-streaming agent can run ten silent minutes.
 */
static gboolean
exchange_busy(
	ClawtCrosstalkExchange *exchange,
	gpointer                user_data
){
	ClawtDaemon *daemon = user_data;
	const gchar *room_id = clawt_crosstalk_exchange_get_room_id(exchange);
	g_auto(GStrv) members = clawt_crosstalk_exchange_dup_members(exchange);
	g_auto(GStrv) participants =
		clawt_crosstalk_exchange_dup_participants(exchange);
	g_auto(GStrv) tasks = clawt_crosstalk_exchange_dup_open_tasks(exchange);
	GStrv lists[2];
	guint l;
	guint i;

	lists[0] = members;
	lists[1] = participants;

	for (l = 0; l < G_N_ELEMENTS(lists); l++) {
		for (i = 0; lists[l] != NULL && lists[l][i] != NULL; i++) {
			ClawtAgent *agent = daemon->agents != NULL
				? clawt_agent_manager_get(daemon->agents, lists[l][i])
				: NULL;
			ClawtMailbox *mailbox;

			if (agent == NULL)
				continue;

			if (clawt_agent_is_typing_in(agent, room_id))
				return TRUE;

			mailbox = clawt_agent_get_mailbox(agent);

			if (mailbox != NULL && clawt_mailbox_depth(mailbox) > 0) {
				g_autoptr(GPtrArray) items = clawt_mailbox_list(mailbox,
				                                                NULL);
				guint k;

				for (k = 0; items != NULL && k < items->len; k++) {
					if (g_strcmp0(clawt_mailbox_item_get_room(
					        g_ptr_array_index(items, k)), room_id) == 0)
						return TRUE;
				}
			}
		}
	}

	/*
	 * A task the manager no longer knows -- a restart -- cannot be
	 * waited on; one it knows and has not finished is the delegation
	 * still going.
	 */
	for (i = 0; tasks != NULL && tasks[i] != NULL; i++) {
		ClawtTask *task = daemon->tasks != NULL
			? clawt_task_manager_get(daemon->tasks, tasks[i]) : NULL;

		if (task != NULL && !clawt_task_is_finished(task))
			return TRUE;
	}

	return FALSE;
}

/* ── Posting ─────────────────────────────────────────────────────── */

/*
 * Whether @agent_id's conversation with its operator gets summaries:
 * its own `crosstalk_summary`, through the fleet's key, and whether it
 * is the chief of staff -- either way it can be made one.
 */
static gboolean
wants_summaries(
	ClawtDaemon *daemon,
	const gchar *agent_id
){
	ClawtAgent *agent;
	ClawtAgent *chief;
	ClawtAgentConfig *config;

	if (daemon->agents == NULL)
		return FALSE;

	agent = clawt_agent_manager_get(daemon->agents, agent_id);

	if (agent == NULL)
		return FALSE;

	config = clawt_agent_get_config(agent);

	if (config == NULL)
		return FALSE;

	chief = clawt_agent_manager_get_chief_of_staff(daemon->agents);

	return clawt_crosstalk_summary_applies(
		(ClawtCrosstalkSummary)clawt_agent_config_get_enum(config,
		                                                   "crosstalk_summary"),
		chief == agent);
}

static void
record_note(
	ClawtDaemon *daemon,
	const gchar *agent_id,
	const gchar *room_id,
	const gchar *body,
	const gchar *rooms,
	const gchar *messages
){
	g_autoptr(ClawtMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	ClawtRoom *room;

	if (daemon->router == NULL || daemon->rooms == NULL ||
		daemon->agents == NULL ||
		clawt_agent_manager_get(daemon->agents, agent_id) == NULL)
		return;

	/* NULL: the operator's own conversation with the agent. */
	room = room_id != NULL
		? clawt_room_manager_get(daemon->rooms, room_id)
		: clawt_room_manager_get_direct(daemon->rooms, "user", agent_id);

	if (room == NULL)
		return;

	message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER,
	                            body);
	clawt_message_set_depth(message, 0);
	clawt_message_set_invites_reply(message, FALSE);
	clawt_message_set_crosstalk(message, rooms, messages);

	/*
	 * Recorded, never sent: no mailbox, so no turn, no hop, no guard, no
	 * budget.  See the top of this file.
	 */
	if (!clawt_mailbox_router_record(daemon->router, message, &error))
		g_warning("crosstalk: the summary for %s could not be recorded: %s",
		          agent_id, error->message);
}

/* The agent's Matrix channel binding, or NULL. */
static ClawtIntegrationBinding *
matrix_binding(
	ClawtDaemon  *daemon,
	const gchar  *agent_id,
	GPtrArray   **bindings
){
	ClawtAgent *agent;
	ClawtAgentConfig *config;

	*bindings = NULL;

	if (daemon->config == NULL || daemon->agents == NULL)
		return NULL;

	agent = clawt_agent_manager_get(daemon->agents, agent_id);
	config = agent != NULL ? clawt_agent_get_config(agent) : NULL;

	if (config == NULL)
		return NULL;

	*bindings = clawt_integration_resolve_for_agent(daemon->config, config);

	return clawt_integration_find_binding(*bindings, "matrix");
}

/*
 * Whether @room_id is a room on @agent_id's Matrix channel: a Matrix
 * room id, and one the binding listens in (an empty `rooms` is every
 * room the account has joined).
 */
static gboolean
matrix_takes(
	ClawtDaemon *daemon,
	const gchar *agent_id,
	const gchar *room_id
){
	g_autoptr(GPtrArray) bindings = NULL;
	ClawtIntegrationBinding *binding;
	g_auto(GStrv) rooms = NULL;

	if (room_id == NULL || room_id[0] != '!' || strchr(room_id, ':') == NULL)
		return FALSE;

	binding = matrix_binding(daemon, agent_id, &bindings);

	if (binding == NULL)
		return FALSE;

	rooms = clawt_integration_binding_get_string_list(binding, "rooms");

	return rooms == NULL || rooms[0] == NULL ||
	       g_strv_contains((const gchar *const *)rooms, room_id);
}

/*
 * Where @agent_id's summary of @exchange goes.  The source only for the
 * agent whose turn began the exchange, and only when it is somewhere the
 * operator, or the person who asked, will read: a clawtilla room the
 * operator and the agent are both in, or a room on the agent's Matrix
 * channel.  A routine's room, a task's, an incident's: the chat.
 */
static Dest
destination_for(
	ClawtDaemon            *daemon,
	const gchar            *agent_id,
	ClawtCrosstalkExchange *exchange,
	gchar                 **target
){
	const gchar *source = clawt_crosstalk_exchange_get_source_room(exchange);
	ClawtAgent *agent;
	ClawtAgentConfig *config;
	ClawtRoom *room;

	*target = NULL;

	if (source == NULL || daemon->agents == NULL ||
		g_strcmp0(clawt_crosstalk_exchange_get_source_agent(exchange),
		          agent_id) != 0)
		return DEST_CHAT;

	agent = clawt_agent_manager_get(daemon->agents, agent_id);
	config = agent != NULL ? clawt_agent_get_config(agent) : NULL;

	if (config == NULL ||
		(ClawtCrosstalkSummaryTo)clawt_agent_config_get_enum(
			config, "crosstalk_summary_to") == CLAWT_CROSSTALK_SUMMARY_TO_CHAT)
		return DEST_CHAT;

	room = daemon->rooms != NULL
		? clawt_room_manager_get(daemon->rooms, source) : NULL;

	if (room != NULL) {
		g_auto(GStrv) members = room_members(room);

		if (room == clawt_room_manager_get_direct(daemon->rooms, "user",
		                                          agent_id) ||
			members == NULL ||
			!g_strv_contains((const gchar *const *)members, "user") ||
			!g_strv_contains((const gchar *const *)members, agent_id))
			return DEST_CHAT;

		*target = g_strdup(source);
		return DEST_ROOM;
	}

	if (matrix_takes(daemon, agent_id, source)) {
		*target = g_strdup(source);
		return DEST_MATRIX;
	}

	return DEST_CHAT;
}

/* A Matrix post that did not land comes to the chat, saying why. */
static void
matrix_fallback(
	ClawtDaemon *daemon,
	const gchar *agent_id,
	const gchar *room_id,
	const gchar *body,
	const gchar *rooms,
	const gchar *messages,
	const gchar *why
){
	g_autofree gchar *note = g_strdup_printf(
		"%s\n\n(This summary was for the Matrix room %s, where the request "
		"came from, but posting it there failed: %s.)",
		body, room_id, why != NULL ? why : "no reason given");

	g_warning("crosstalk: the summary for %s could not be posted in Matrix "
	          "room %s: %s; it is in the operator's chat instead",
	          agent_id, room_id, why != NULL ? why : "no reason given");
	record_note(daemon, agent_id, NULL, note, rooms, messages);
}

/* One Matrix post in flight. */
typedef struct {
	ClawtDaemon *daemon;   /* owned */
	gchar       *agent_id;
	gchar       *room_id;
	gchar       *body;
	gchar       *rooms;
	gchar       *messages;
} MatrixPost;

static void
matrix_post_free(
	MatrixPost *post
){
	g_clear_object(&post->daemon);
	g_free(post->agent_id);
	g_free(post->room_id);
	g_free(post->body);
	g_free(post->rooms);
	g_free(post->messages);
	g_free(post);
}

static void
on_matrix_posted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	MatrixPost *post = data;
	g_autoptr(GError) error = NULL;

	(void)source;

	if (!clawt_matrix_send_text_finish(result, &error) &&
		post->daemon->crosstalk != NULL)
		matrix_fallback(post->daemon, post->agent_id, post->room_id,
		                post->body, post->rooms, post->messages,
		                error != NULL ? error->message : NULL);

	matrix_post_free(post);
}

/*
 * Posts @body in @room_id from @agent_id's own Matrix account.  The
 * homeserver keeps one copy of a retried post: the transaction id is
 * made from the agent, the room and the exchanges' first messages.
 */
static void
post_matrix(
	ClawtDaemon *daemon,
	const gchar *agent_id,
	const gchar *room_id,
	const gchar *body,
	const gchar *rooms,
	const gchar *messages
){
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	g_autoptr(GPtrArray) bindings = NULL;
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secrets_dir = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *seed = NULL;
	ClawtIntegrationBinding *binding;
	MatrixPost *post;

	/* A test's stand-in answers at once. */
	if (self->channel_send != NULL) {
		if (!self->channel_send(agent_id, "matrix", room_id, body,
		                        self->channel_send_data, &error))
			matrix_fallback(daemon, agent_id, room_id, body, rooms, messages,
			                error != NULL ? error->message : NULL);
		return;
	}

	binding = matrix_binding(daemon, agent_id, &bindings);
	ref = binding != NULL
		? clawt_integration_binding_get_secret(binding, "access_token") : NULL;

	if (ref == NULL) {
		matrix_fallback(daemon, agent_id, room_id, body, rooms, messages,
		                "the agent's Matrix channel has no access token");
		return;
	}

	secrets_dir = clawt_config_get_path_value(daemon->config, "secrets.dir");
	token = clawt_secret_ref_resolve(ref, secrets_dir,
		(guint)clawt_config_get_int(daemon->config,
		                            "secrets.command_timeout_seconds"),
		&error);

	if (token == NULL) {
		matrix_fallback(daemon, agent_id, room_id, body, rooms, messages,
		                error != NULL ? error->message : "no token");
		return;
	}

	post = g_new0(MatrixPost, 1);
	post->daemon = g_object_ref(daemon);
	post->agent_id = g_strdup(agent_id);
	post->room_id = g_strdup(room_id);
	post->body = g_strdup(body);
	post->rooms = g_strdup(rooms);
	post->messages = g_strdup(messages);
	seed = g_strdup_printf("crosstalk\n%s\n%s\n%s", agent_id, room_id,
	                       messages != NULL ? messages : body);

	/* Under the daemon's context, so the answer comes back on its loop. */
	g_main_context_push_thread_default(daemon->main_context);
	clawt_matrix_send_text_async(
		clawt_integration_binding_get_string(binding, "homeserver"),
		token, room_id, body, seed, NULL, on_matrix_posted, post);
	g_main_context_pop_thread_default(daemon->main_context);
	memset(token, 0, strlen(token));
}

/* The note, delivered where it was decided it goes. */
static void
deliver(
	ClawtDaemon *daemon,
	const gchar *agent_id,
	Dest         dest,
	const gchar *target,
	const gchar *body,
	const gchar *rooms,
	const gchar *messages
){
	switch (dest) {
	case DEST_CHAT:
		record_note(daemon, agent_id, NULL, body, rooms, messages);
		return;
	case DEST_ROOM:
		record_note(daemon, agent_id, target, body, rooms, messages);
		return;
	case DEST_MATRIX:
		post_matrix(daemon, agent_id, target, body, rooms, messages);
		return;
	}
}

/* One rewrite waiting on the model. */
typedef struct {
	ClawtDaemon *daemon;   /* owned */
	gchar       *agent_id;
	Dest         dest;
	gchar       *target;
	gchar       *body;
	gchar       *rooms;
	gchar       *messages;
} RewriteJob;

static void
rewrite_job_free(
	RewriteJob *job
){
	g_clear_object(&job->daemon);
	g_free(job->agent_id);
	g_free(job->target);
	g_free(job->body);
	g_free(job->rooms);
	g_free(job->messages);
	g_free(job);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RewriteJob, rewrite_job_free)

static void
on_rewritten(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	g_autoptr(RewriteJob) job = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rewrite = NULL;

	(void)source;

	rewrite = clawt_crosstalk_rewrite_finish(result, &error);

	if (job->daemon->crosstalk != NULL &&
		job->daemon->crosstalk->rewrites > 0)
		job->daemon->crosstalk->rewrites--;

	/* A daemon that stopped meanwhile has nowhere to post it. */
	if (job->daemon->crosstalk == NULL)
		return;

	/*
	 * The record-built note on anything but a usable rewrite: turning
	 * the model on may delay a summary and must never lose one.
	 */
	if (rewrite == NULL)
		g_info("crosstalk: the summary for %s is posted as built (%s)",
		       job->agent_id, error != NULL ? error->message : "no rewrite");

	deliver(job->daemon, job->agent_id, job->dest, job->target,
	        rewrite != NULL ? rewrite : job->body, job->rooms, job->messages);
}

/*
 * The rewriting model, or NULL when the key is empty or names something
 * ai-glib cannot build.  Built under the daemon's own context, for the
 * reason ack_provider() is: a provider's session binds to whichever
 * context is thread-default when it is made.
 */
static AiProvider *
rewrite_provider(
	ClawtDaemon *daemon
){
	ClawtDaemonCrosstalk *self = daemon->crosstalk;
	const gchar *spec;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *model = NULL;
	g_autoptr(AiConfig) ai_config = NULL;
	g_autoptr(GError) error = NULL;
	GObject *provider;

	if (self->provider_override != NULL)
		return self->provider_override;

	spec = clawt_config_get_string(daemon->config,
	                               "orchestration.crosstalk_summary_model");

	if (spec == NULL || *spec == '\0')
		return NULL;

	if (self->provider != NULL && g_strcmp0(self->provider_spec, spec) == 0)
		return self->provider;

	g_clear_object(&self->provider);
	g_free(self->provider_spec);
	self->provider_spec = g_strdup(spec);

	if (!clawt_ack_check_split_model(spec, &provider_name, &model)) {
		g_warning("orchestration.crosstalk_summary_model '%s' is not "
		          "provider/model; summaries are posted as built", spec);
		return NULL;
	}

	ai_config = ai_config_new();
	ai_config_set_default_model(ai_config, model);

	g_main_context_push_thread_default(daemon->main_context);
	provider = ai_provider_factory_new_from_string(provider_name, ai_config,
	                                               &error);
	g_main_context_pop_thread_default(daemon->main_context);

	if (provider == NULL) {
		g_warning("orchestration.crosstalk_summary_model: provider '%s': %s; "
		          "summaries are posted as built", provider_name,
		          error != NULL ? error->message : "unknown");
		return NULL;
	}

	self->provider = AI_PROVIDER(provider);

	return self->provider;
}

static void
post(
	ClawtDaemon *daemon,
	Waiting     *waiting
){
	const gchar *agent_id = waiting->agent_id;
	GPtrArray *exchanges = waiting->exchanges;
	g_autofree gchar *body = clawt_crosstalk_compose(exchanges, agent_id);
	g_autofree gchar *rooms = NULL;
	g_autofree gchar *messages = NULL;
	AiProvider *provider;

	clawt_crosstalk_join_targets(exchanges, &rooms, &messages);
	provider = rewrite_provider(daemon);

	if (provider != NULL) {
		RewriteJob *job = g_new0(RewriteJob, 1);

		job->daemon = g_object_ref(daemon);
		job->agent_id = g_strdup(agent_id);
		job->dest = waiting->dest;
		job->target = g_strdup(waiting->target);
		job->body = g_steal_pointer(&body);
		job->rooms = g_steal_pointer(&rooms);
		job->messages = g_steal_pointer(&messages);
		daemon->crosstalk->rewrites++;

		/* Under the daemon's context, so the timer fires on its loop. */
		g_main_context_push_thread_default(daemon->main_context);
		clawt_crosstalk_rewrite_async(provider, job->body, 0, on_rewritten,
		                              job);
		g_main_context_pop_thread_default(daemon->main_context);
		return;
	}

	deliver(daemon, agent_id, waiting->dest, waiting->target, body, rooms,
	        messages);
}

void
clawt_daemon_crosstalk_tick(
	ClawtDaemon *daemon,
	gint64       now
){
	ClawtDaemonCrosstalk *self;
	g_autoptr(GPtrArray) ended = NULL;
	g_autoptr(GPtrArray) due = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(daemon));

	self = daemon->crosstalk;

	if (self == NULL)
		return;

	ended = clawt_crosstalk_collect(self->tracker, now, exchange_busy, daemon);

	/*
	 * Each ended exchange goes to every agent in it that wants it, and
	 * is marked told whether or not anybody did -- an exchange between
	 * two agents with summaries off is still over, and a later change
	 * of the setting must not dig it up.
	 */
	for (i = 0; i < ended->len; i++) {
		ClawtCrosstalkExchange *exchange = g_ptr_array_index(ended, i);
		g_auto(GStrv) participants =
			clawt_crosstalk_exchange_dup_participants(exchange);
		guint p;

		for (p = 0; participants[p] != NULL; p++) {
			Waiting *waiting;
			g_autofree gchar *target = NULL;
			g_autofree gchar *pile = NULL;
			Dest dest;

			if (!wants_summaries(daemon, participants[p]))
				continue;

			/* One pile per agent and destination: a burst that came
			 * from Matrix and one from the chat are two notes. */
			dest = destination_for(daemon, participants[p], exchange, &target);
			pile = g_strdup_printf("%s\n%d\n%s", participants[p], (gint)dest,
			                       target != NULL ? target : "");
			waiting = g_hash_table_lookup(self->waiting, pile);

			if (waiting == NULL) {
				waiting = g_new0(Waiting, 1);
				waiting->exchanges = g_ptr_array_new_with_free_func(
					(GDestroyNotify)clawt_crosstalk_exchange_unref);
				waiting->agent_id = g_strdup(participants[p]);
				waiting->dest = dest;
				waiting->target = g_steal_pointer(&target);
				g_hash_table_insert(self->waiting, g_steal_pointer(&pile),
				                    waiting);
			}

			g_ptr_array_add(waiting->exchanges,
			                clawt_crosstalk_exchange_ref(exchange));
			waiting->last_added = now;
		}

		ledger_mark(daemon, clawt_crosstalk_exchange_get_room_id(exchange),
		            clawt_crosstalk_exchange_get_last_message_id(exchange),
		            now / G_USEC_PER_SEC);
	}

	if (ended->len > 0)
		ledger_save(daemon);

	/*
	 * A burst becomes one note: posted once nothing has joined the
	 * conversation's pile for a few seconds, or once the pile is big
	 * enough that waiting longer only makes the note longer.
	 */
	due = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iter, self->waiting);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Waiting *waiting = value;

		if (now - waiting->last_added >=
		        (gint64)CLAWT_CROSSTALK_COALESCE_SECONDS * G_USEC_PER_SEC ||
			waiting->exchanges->len >= CROSSTALK_MAX_SECTIONS)
			g_ptr_array_add(due, g_strdup(key));
	}

	for (i = 0; i < due->len; i++) {
		const gchar *pile = g_ptr_array_index(due, i);
		gpointer stolen_key = NULL;
		gpointer stolen = NULL;

		/* Taken out of the table first: posting may add to it. */
		if (!g_hash_table_steal_extended(self->waiting, pile, &stolen_key,
		                                 &stolen))
			continue;

		g_free(stolen_key);
		post(daemon, stolen);
		waiting_free(stolen);
	}
}

static gboolean
on_tick(
	gpointer user_data
){
	clawt_daemon_crosstalk_tick(user_data, g_get_real_time());

	return G_SOURCE_CONTINUE;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void
clawt_daemon_crosstalk_setup(
	ClawtDaemon *daemon
){
	ClawtDaemonCrosstalk *self;
	gboolean have_ledger;

	g_return_if_fail(CLAWT_IS_DAEMON(daemon));

	if (daemon->crosstalk != NULL)
		return;

	self = g_new0(ClawtDaemonCrosstalk, 1);
	self->tracker = clawt_crosstalk_new();
	self->waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      waiting_free);
	self->ledger = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     through_free);

	if (daemon->state_dir != NULL)
		self->ledger_path = g_build_filename(daemon->state_dir,
		                                     CROSSTALK_LEDGER, NULL);

	daemon->crosstalk = self;

	have_ledger = self->ledger_path != NULL && ledger_load(daemon);
	ledger_baseline_or_backfill(daemon, have_ledger);

	if (daemon->bus != NULL)
		self->bus_handler = g_signal_connect(daemon->bus, "event",
		                                     G_CALLBACK(on_bus_event),
		                                     daemon);

	/* Attached to the daemon's context by name, not the ambient one. */
	self->tick = g_timeout_source_new_seconds(CROSSTALK_TICK_SECONDS);
	g_source_set_callback(self->tick, on_tick, daemon, NULL);
	g_source_attach(self->tick, daemon->main_context);
}

void
clawt_daemon_crosstalk_teardown(
	ClawtDaemon *daemon
){
	ClawtDaemonCrosstalk *self;

	g_return_if_fail(CLAWT_IS_DAEMON(daemon));

	self = daemon->crosstalk;

	if (self == NULL)
		return;

	if (daemon->bus != NULL && self->bus_handler != 0)
		g_signal_handler_disconnect(daemon->bus, self->bus_handler);

	if (self->tick != NULL) {
		g_source_destroy(self->tick);
		g_source_unref(self->tick);
	}

	/*
	 * Cleared before anything is freed, so a rewrite answering after
	 * this finds no crosstalk and posts nothing into a daemon going away.
	 */
	daemon->crosstalk = NULL;

	g_clear_object(&self->tracker);
	g_clear_pointer(&self->waiting, g_hash_table_unref);
	g_clear_pointer(&self->ledger, g_hash_table_unref);
	g_free(self->ledger_path);
	g_clear_object(&self->provider);
	g_free(self->provider_spec);
	g_clear_object(&self->provider_override);
	g_free(self);
}

void
clawt_daemon_crosstalk_set_provider(
	ClawtDaemon *daemon,
	AiProvider  *provider
){
	g_return_if_fail(CLAWT_IS_DAEMON(daemon));

	if (daemon->crosstalk == NULL)
		return;

	g_clear_object(&daemon->crosstalk->provider_override);

	if (provider != NULL)
		daemon->crosstalk->provider_override = g_object_ref(provider);
}

void
clawt_daemon_crosstalk_set_channel_send(
	ClawtDaemon                     *daemon,
	ClawtDaemonCrosstalkChannelSend  send,
	gpointer                         user_data
){
	g_return_if_fail(CLAWT_IS_DAEMON(daemon));

	if (daemon->crosstalk == NULL)
		return;

	daemon->crosstalk->channel_send = send;
	daemon->crosstalk->channel_send_data = user_data;
}

guint
clawt_daemon_crosstalk_rewrites_pending(
	ClawtDaemon *daemon
){
	g_return_val_if_fail(CLAWT_IS_DAEMON(daemon), 0);

	return daemon->crosstalk != NULL ? daemon->crosstalk->rewrites : 0;
}

ClawtCrosstalk *
clawt_daemon_crosstalk_get_tracker(
	ClawtDaemon *daemon
){
	g_return_val_if_fail(CLAWT_IS_DAEMON(daemon), NULL);

	return daemon->crosstalk != NULL ? daemon->crosstalk->tracker : NULL;
}
