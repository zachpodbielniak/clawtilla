/*
 * daemon-memory.c - The client surface: memory.* and operator.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Split out of daemon-agent.c when recall and the operator profile
 * arrived: `memory.list` and `memory.search` are about one agent, but
 * `memory.recall` searches the whole fleet's conversations and
 * `operator.*` is about a person rather than an agent -- and a family
 * file whose name no longer describes what is in it is the beginning of
 * the chain this split undid.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_memory(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;

    /* The review's proposals and its setting have a file of their own. */
    if (g_str_has_prefix(kind, "memory.proposal") ||
        g_str_has_prefix(kind, "memory.review"))
        return clawt_daemon_handle_memory_review(self, kind, request,
                                                 payload, handled);

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "memory.list") == 0 ||
        g_strcmp0(kind, "memory.search") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtMemoryStore *store;
        g_autoptr(GPtrArray) memories = NULL;
        guint i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        store = clawt_agent_get_memory(agent);

        if (store == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "that agent has no memory store; memories.enabled is off");

        /*
         * Through the manager, so a client sees exactly what the agent
         * sees: its own memories, its team's and the fleet's, each row
         * saying which it came from.  Reading the agent's own store
         * directly here would have made the clients disagree with
         * clawtilla_memory_list about what an agent knows.
         */
        memories = clawt_agent_manager_memory_search(
            self->agents, agent_id,
            (g_strcmp0(kind, "memory.search") == 0)
                ? clawt_ipc_payload_string(payload, "query") : NULL,
            clawt_ipc_payload_string(payload, "category"),
            clawt_ipc_payload_boolean(payload, "pinned", FALSE),
            (guint)clawt_ipc_payload_int(payload, "limit", 20));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "total");
        json_builder_add_int_value(builder,
                                   clawt_memory_store_count(store, FALSE));
        json_builder_set_member_name(builder, "memories");
        json_builder_begin_array(builder);

        for (i = 0; memories != NULL && i < memories->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(memories, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, memory->id);
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, memory->content);

            if (memory->summary != NULL) {
                json_builder_set_member_name(builder, "summary");
                json_builder_add_string_value(builder, memory->summary);
            }

            json_builder_set_member_name(builder, "category");
            json_builder_add_string_value(builder, memory->category);
            json_builder_set_member_name(builder, "importance");
            json_builder_add_string_value(builder, memory->importance);

            /*
             * Which database it came out of.  A listing that mixes three
             * scopes and does not say which is which turns "the fleet
             * believes this" and "this agent worked it out" into the
             * same row.
             */
            if (memory->scope != NULL) {
                json_builder_set_member_name(builder, "scope");
                json_builder_add_string_value(builder, memory->scope);
            }

            if (memory->tags != NULL) {
                json_builder_set_member_name(builder, "tags");
                json_builder_add_string_value(builder, memory->tags);
            }

            json_builder_set_member_name(builder, "pinned");
            json_builder_add_boolean_value(builder, memory->pinned);
            json_builder_set_member_name(builder, "created_at");
            json_builder_add_int_value(builder, memory->created_at);
            json_builder_set_member_name(builder, "access_count");
            json_builder_add_int_value(builder, memory->access_count);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── the journal, and undo ── */
    if (g_strcmp0(kind, "memory.journal") == 0)
        return clawt_daemon_memory_journal(self, request, payload);

    if (g_strcmp0(kind, "memory.undo") == 0)
        return clawt_daemon_memory_undo(self, request, payload);

    /* ── recall ── */
    if (g_strcmp0(kind, "memory.recall") == 0) {
        const gchar *query = clawt_ipc_payload_string(payload, "query");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        g_autoptr(GPtrArray) hits = NULL;
        g_auto(GStrv) rooms = NULL;
        gint64 days = clawt_ipc_payload_int(payload, "days", 0);
        gint64 since = 0;
        guint i;

        if (self->transcripts == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "this daemon has no transcript index");

        if (query == NULL || *query == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "say what to look for");

        if (days > 0)
            since = (g_get_real_time() / G_USEC_PER_SEC) - (days * 86400);

        /*
         * NULL rooms: every room.  A client speaks for the operator, who
         * can already open any transcript in the state directory -- the
         * room filter exists to hold an *agent* to the conversations it
         * was part of, and applying it here would hide a person's own
         * fleet from them.
         *
         * `agent` narrows to what one agent said, which is a different
         * question from which rooms may be read.
         */
        hits = clawt_transcript_index_search(
            self->transcripts, query, (const gchar * const *)rooms, agent_id,
            since, (guint)clawt_ipc_payload_int(payload, "limit", 50), NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "hits");
        json_builder_begin_array(builder);

        for (i = 0; hits != NULL && i < hits->len; i++) {
            ClawtTranscriptHit *hit = g_ptr_array_index(hits, i);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "id", hit->id);
            clawt_daemon_add_string_member(builder, "room", hit->room_id);
            clawt_daemon_add_string_member(builder, "from", hit->sender_id);
            clawt_daemon_add_string_member(builder, "from_name",
                                           hit->sender_name);
            clawt_daemon_add_string_member(builder, "body", hit->body);
            json_builder_set_member_name(builder, "at");
            json_builder_add_int_value(builder, hit->timestamp);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        /*
         * Whether the search was ranked or a substring scan, so a result
         * that looks thin can be told apart from a sqlite without FTS5.
         */
        json_builder_set_member_name(builder, "full_text");
        json_builder_add_boolean_value(
            builder, clawt_transcript_index_has_full_text(self->transcripts));

        json_builder_set_member_name(builder, "indexed");
        json_builder_add_int_value(
            builder, clawt_transcript_index_count(self->transcripts));

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── the operator model ── */
    if (g_strcmp0(kind, "operator.get") == 0) {
        g_autofree gchar *text = NULL;
        g_autoptr(GPtrArray) learned = NULL;
        guint i;

        if (self->operator_profile == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no operator profile");

        text = clawt_operator_profile_read_text(self->operator_profile);
        learned = clawt_operator_profile_learned(self->operator_profile, 0);

        json_builder_begin_object(builder);

        /*
         * The two halves separately rather than the rendered whole: what
         * a person wrote is editable and what the fleet recorded is not,
         * and a client that could only see the merged text would have to
         * guess which lines it may offer to change.
         */
        clawt_daemon_add_string_member(builder, "text", text);
        clawt_daemon_add_string_member(
            builder, "path",
            clawt_operator_profile_path(self->operator_profile));

        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(
            builder, clawt_config_get_boolean(self->config,
                                              "memories.operator_profile"));

        json_builder_set_member_name(builder, "learned");
        json_builder_begin_array(builder);

        for (i = 0; learned != NULL && i < learned->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(learned, i);

            json_builder_begin_object(builder);
            clawt_daemon_add_string_member(builder, "id", memory->id);
            clawt_daemon_add_string_member(builder, "content",
                                           memory->content);
            clawt_daemon_add_string_member(builder, "source", memory->source);
            json_builder_set_member_name(builder, "created_at");
            json_builder_add_int_value(builder, memory->created_at);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "operator.set") == 0) {
        const gchar *text = clawt_ipc_payload_string(payload, "text");
        g_autoptr(GError) write_error = NULL;
        g_autoptr(GPtrArray) refusals = NULL;

        if (self->operator_profile == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no operator profile");

        if (text == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "operator.set needs text");

        if (!clawt_operator_profile_write_text(self->operator_profile, text,
                                               &write_error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       write_error->message);

        /*
         * Re-rendered into every agent's workspace at once.  A profile a
         * person has just corrected and that reaches no agent until the
         * next daemon start is a correction they will make twice.
         */
        refusals = clawt_daemon_render_refusals_new();
        clawt_daemon_render_all_agents_into(self, refusals);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(
            builder, "path",
            clawt_operator_profile_path(self->operator_profile));
        clawt_daemon_add_render_refusals(builder, refusals);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}

/* ── The journal ─────────────────────────────────────────────────── */

static gint journal_newest_first(gconstpointer a, gconstpointer b);

/*
 * One agent's view of the journals: everything in its own database,
 * whoever made the change, and its *own* changes in its team's and the
 * fleet's -- what it did to shared memories is its review, and what a
 * teammate did is the teammate's.
 */
JsonNode *
clawt_daemon_memory_journal(
	ClawtDaemon *self,
	JsonNode    *request,
	JsonObject  *payload
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) all = NULL;
	const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
	ClawtAgent *agent = (agent_id != NULL)
		? clawt_agent_manager_get(self->agents, agent_id) : NULL;
	guint limit = (guint)CLAMP(clawt_ipc_payload_int(payload, "limit", 50),
		1, 500);
	guint i;

	if (agent == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such agent");

	if (clawt_agent_get_memory(agent) == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
			"that agent has no memory store; memories.enabled is off");

	all = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_memory_journal_entry_free);

	for (i = 0; i < clawt_memory_scope_count(); i++) {
		ClawtMemoryScope scope = clawt_memory_scope_nth(i);
		ClawtMemoryStore *store = clawt_agent_manager_memory_store_for_scope(
			self->agents, agent_id, scope);
		g_autoptr(GPtrArray) rows = NULL;
		guint j;

		if (store == NULL)
			continue;

		rows = clawt_memory_store_journal(store,
			(scope == CLAWT_MEMORY_SCOPE_AGENT) ? NULL : agent_id, limit,
			NULL);

		for (j = 0; rows != NULL && j < rows->len; j++) {
			ClawtMemoryJournalEntry *entry = g_ptr_array_index(rows, j);

			entry->scope = g_strdup(clawt_memory_scope_nth_nick(i));
			g_ptr_array_add(all, clawt_memory_journal_entry_copy(entry));
		}
	}

	/* Newest first across the three, then cut to the limit. */
	g_ptr_array_sort(all, journal_newest_first);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, agent_id);
	json_builder_set_member_name(builder, "keep");
	json_builder_add_int_value(builder,
		clawt_config_get_int(self->config, "memories.journal_keep"));
	json_builder_set_member_name(builder, "entries");
	json_builder_begin_array(builder);

	for (i = 0; i < all->len && i < limit; i++) {
		ClawtMemoryJournalEntry *entry = g_ptr_array_index(all, i);
		g_autofree gchar *line = clawt_memory_journal_describe(entry);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, entry->id);
		clawt_daemon_add_string_member(builder, "scope", entry->scope);
		json_builder_set_member_name(builder, "at");
		json_builder_add_int_value(builder, entry->at);
		clawt_daemon_add_string_member(builder, "action", entry->action);
		clawt_daemon_add_string_member(builder, "memory_id",
			entry->memory_id);
		clawt_daemon_add_string_member(builder, "actor", entry->actor);
		clawt_daemon_add_string_member(builder, "excerpt", entry->excerpt);
		clawt_daemon_add_string_member(builder, "line", line);

		if (entry->undoes > 0) {
			json_builder_set_member_name(builder, "undoes");
			json_builder_add_int_value(builder, entry->undoes);
		}

		json_builder_set_member_name(builder, "undone_at");
		json_builder_add_int_value(builder, entry->undone_at);
		clawt_daemon_add_string_member(builder, "undone_by",
			entry->undone_by);
		json_builder_set_member_name(builder, "undoable");
		json_builder_add_boolean_value(builder, entry->undoable);
		clawt_daemon_add_string_member(builder, "why_not", entry->why_not);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

static gint
journal_newest_first(
	gconstpointer a,
	gconstpointer b
){
	const ClawtMemoryJournalEntry *x =
		*(const ClawtMemoryJournalEntry * const *)a;
	const ClawtMemoryJournalEntry *y =
		*(const ClawtMemoryJournalEntry * const *)b;

	if (x->at != y->at)
		return (x->at > y->at) ? -1 : 1;

	/* Same second: the row id is each database's own order. */
	if (x->id != y->id)
		return (x->id > y->id) ? -1 : 1;

	return 0;
}

JsonNode *
clawt_daemon_memory_undo(
	ClawtDaemon *self,
	JsonNode    *request,
	JsonObject  *payload
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;
	const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
	const gchar *scope_nick = clawt_ipc_payload_string(payload, "scope");
	gint64 entry_id = clawt_ipc_payload_int(payload, "id", 0);
	gint scope = CLAWT_MEMORY_SCOPE_AGENT;
	ClawtMemoryStore *store;
	g_autoptr(GPtrArray) mine = NULL;
	gboolean allowed = FALSE;
	guint i;

	if (agent_id == NULL ||
		clawt_agent_manager_get(self->agents, agent_id) == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"no such agent");

	if (entry_id <= 0)
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"memory.undo needs the journal row's id");

	if (scope_nick != NULL &&
		!clawt_enum_from_nick(CLAWT_TYPE_MEMORY_SCOPE, scope_nick, &scope))
		return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			"scope is agent, team or fleet");

	store = clawt_agent_manager_memory_store_for_scope(self->agents,
		agent_id, (ClawtMemoryScope)scope);

	if (store == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			"that agent has no such memory database");

	/*
	 * In a shared database a row is this agent's to review only when it
	 * made the change.  The same rule as the listing, applied where the
	 * undo is decided rather than trusted to the client that drew it.
	 */
	if (scope != CLAWT_MEMORY_SCOPE_AGENT) {
		mine = clawt_memory_store_journal(store, agent_id, G_MAXINT, NULL);

		for (i = 0; mine != NULL && i < mine->len; i++)
			if (((ClawtMemoryJournalEntry *)
					g_ptr_array_index(mine, i))->id == entry_id)
				allowed = TRUE;

		if (!allowed)
			return clawt_ipc_error_new(request,
				CLAWT_ERROR_PERMISSION_DENIED,
				"that change to a shared memory was not made by this "
				"agent; review it from the agent that made it");
	}

	if (!clawt_memory_store_undo(store, entry_id, "operator", &error))
		return clawt_ipc_error_new(request, error->code, error->message);

	if (self->bus != NULL) {
		g_autoptr(ClawtEvent) event = clawt_event_new("memory.changed",
			agent_id);

		clawt_event_bus_publish(self->bus, event);
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "undone");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}
