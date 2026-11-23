/*
 * daemon-misc.c - The client surface: the verbs with no family of their own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

/* One agent's line in usage.summary, read before any is written. */
typedef struct {
    ClawtAgent       *agent;     /* borrowed: the manager holds it */
    ClawtUsageTotals  totals;
    gint64            last_turn_at;
    gint64           *daily;     /* days entries, or NULL */
} UsageRow;

static void
usage_row_clear(gpointer data)
{
    UsageRow *row = data;

    g_clear_pointer(&row->daily, g_free);
}

/* Most spent first; ties by id, so the order is the same every time. */
static gint
usage_row_by_cost(gconstpointer a, gconstpointer b)
{
    const UsageRow *x = a;
    const UsageRow *y = b;

    if (x->totals.cost_micros != y->totals.cost_micros)
        return x->totals.cost_micros > y->totals.cost_micros ? -1 : 1;
    return g_strcmp0(clawt_agent_get_id(x->agent), clawt_agent_get_id(y->agent));
}

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * Says that a decision is no longer waiting.
 *
 * The counterpart of `decision.asked`, and it was missing.  Answering
 * published nothing, so every client's badge went stale the moment
 * *anything else* settled one: a second window, the CLI, or the venture
 * bridge answering on the operator's behalf.  Nothing warned, because a
 * count that is merely too high looks exactly like an inbox somebody
 * has not got to yet.
 *
 * The agent is the subject, matching `decision.asked`, and @how says
 * which of the two ways it ended -- a client that only wants to
 * decrement a counter can ignore it, and one drawing a line in a list
 * cannot.
 */
void
clawt_daemon_publish_decision_settled(ClawtDaemon *self, ClawtDecision *decision,
                                      const gchar *how)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL || decision == NULL)
        return;

    event = clawt_event_new("decision.settled",
                            clawt_decision_get_agent(decision));
    clawt_event_set_detail(event, "decision", clawt_decision_get_id(decision));
    clawt_event_set_detail(event, "how", how);

    /*
     * And where, with what and by which surface -- so a chat holding the
     * inline view of this decision can find it by room and redraw it as
     * settled without asking again.  Without these a client could only
     * decrement a counter: the inline view would go on offering buttons
     * the daemon was going to refuse.
     */
    clawt_event_set_detail(event, "room", clawt_decision_get_room(decision));
    clawt_event_set_detail(event, "answer",
                           clawt_decision_get_answer(decision));
    clawt_event_set_detail(event, "via",
                           clawt_decision_get_settled_via(decision));

    clawt_event_bus_publish(self->bus, event);
}

/*
 * Says that a decision is waiting, and where it was asked.
 *
 * Every door a decision comes in by publishes through here: an agent's
 * own ask, a policy rule that asks or hands back, a venture staging.
 * The emits it replaced named only the agent, which was enough to bump a
 * badge and not enough for a chat to know the question belonged in the
 * room on screen -- the other half of the publish-both-ends rule, since
 * decision.settled carries the room too.
 */
void
clawt_daemon_publish_decision_asked(ClawtDaemon   *self,
                                    ClawtDecision *decision)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL || decision == NULL)
        return;

    event = clawt_event_new("decision.asked",
                            clawt_decision_get_agent(decision));
    clawt_event_set_detail(event, "decision", clawt_decision_get_id(decision));
    clawt_event_set_detail(event, "room", clawt_decision_get_room(decision));

    clawt_event_bus_publish(self->bus, event);
}

/*
 * The room a decision asked with none named belongs to, when that is
 * not a guess.
 *
 * A tool call names its turn's room when libreclaw handed it one; a
 * policy ask is raised from a hook that carries no room at all.  An
 * agent mid-turn in exactly one room asked from that room.  An agent in
 * several, or in none, could have asked from any of them -- and a
 * question drawn in the wrong conversation is worse than one that only
 * reaches the inbox, so the room stays unset.
 */
void
clawt_daemon_decision_place(ClawtDaemon   *self,
                            ClawtDecision *decision)
{
    ClawtAgent *agent;
    g_autoptr(GPtrArray) rooms = NULL;

    if (decision == NULL || clawt_decision_get_room(decision) != NULL ||
        self->agents == NULL)
        return;

    agent = clawt_agent_manager_get(self->agents,
                                    clawt_decision_get_agent(decision));
    if (agent == NULL)
        return;

    /*
     * A turn whose typing frame named no room is kept under a sentinel
     * beginning with \x01 (clawt-agent.c), which is bookkeeping and not
     * a room anybody can open -- the same thing
     * clawt_agent_get_last_turn_room() refuses to hand out.
     */
    rooms = clawt_agent_dup_running_rooms(agent);
    if (rooms != NULL && rooms->len == 1 &&
        ((const gchar *)g_ptr_array_index(rooms, 0))[0] != '\x01')
        clawt_decision_set_room(decision, g_ptr_array_index(rooms, 0));
}

/* Answers a deferred policy.check once its rule has been resolved. */
static void
on_policy_check_reply(gboolean allowed, const gchar *refusal, gpointer data)
{
    ClawtIpcPending *pending = data;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "allowed");
    json_builder_add_boolean_value(builder, allowed);
    if (refusal != NULL) {
        json_builder_set_member_name(builder, "reason");
        json_builder_add_string_value(builder, refusal);
    }
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(pending),
                               json_builder_get_root(builder)));
}

JsonNode *
clawt_daemon_handle_misc(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "attachment.get") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *path = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *contents = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;

        /*
         * The bytes, not the path.
         *
         * A client may be on another machine entirely -- that is what
         * connection profiles are for -- so handing it a filename would
         * work on this host and show nothing anywhere else, which reads
         * as a broken image rather than as an unsupported setup.
         *
         * The id is checked rather than trusted: clawt_attachment_path()
         * refuses anything outside the character set an id is made of,
         * which is what stops a request for a path of somebody's
         * choosing reading a file this was never meant to serve.
         */
        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which attachment?");

        if (self->attachment_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no attachments");

        path = clawt_attachment_path(self->attachment_dir, id);

        if (path == NULL || !g_file_get_contents(path, &contents, &length,
                                                 NULL))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such attachment");

        name = clawt_attachment_name(id);
        encoded = g_base64_encode((const guchar *)contents, length);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * Every file one conversation carried, for the clients' Files
     * gallery.
     *
     * Read from the room's whole history, which every room keeps in
     * memory: the attachment directory is shared by all conversations
     * and records nothing about which one a file came from, while each
     * message's block says exactly that.  `room` takes a room id or an
     * agent id (resolved to the operator's direct room with it, as
     * room.history does); `agent` takes only an agent.
     */
    if (g_strcmp0(kind, "attachment.list") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtRoom *room = NULL;
        g_autoptr(GPtrArray) history = NULL;
        g_autoptr(GPtrArray) items = NULL;
        guint i;

        if (room_id != NULL)
            room = clawt_room_manager_get(self->rooms, room_id);

        /*
         * An agent's conversation is its direct room with the operator,
         * named by the room manager rather than spelled here: a direct
         * room's id is derived, and a second spelling of the rule is a
         * second chance to name a room that does not exist.
         */
        if (room == NULL) {
            const gchar *agent = (agent_id != NULL) ? agent_id : room_id;

            if (agent != NULL &&
                clawt_agent_manager_get(self->agents, agent) != NULL)
                room = clawt_room_manager_get_direct(self->rooms, "user",
                                                     agent);
        }

        if (room_id == NULL && agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which room or agent?");

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room or agent");

        history = clawt_room_get_history(room, 0);
        items = clawt_attachment_list(self->attachment_dir, history);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, clawt_room_get_id(room));
        json_builder_set_member_name(builder, "items");
        json_builder_begin_array(builder);

        for (i = 0; i < items->len; i++) {
            ClawtAttachmentItem *item = g_ptr_array_index(items, i);

            json_builder_begin_object(builder);

            /* `id` or `path`, whichever the block named the file by */
            if (item->id != NULL) {
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, item->id);
            }

            if (item->path != NULL) {
                json_builder_set_member_name(builder, "path");
                json_builder_add_string_value(builder, item->path);
            }

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, item->name);
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(builder, item->kind);
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, item->bytes);
            json_builder_set_member_name(builder, "exists");
            json_builder_add_boolean_value(builder, item->exists);
            json_builder_set_member_name(builder, "message");
            json_builder_add_string_value(builder, item->message);
            json_builder_set_member_name(builder, "sender");
            json_builder_add_string_value(builder, item->sender);
            json_builder_set_member_name(builder, "ts");
            json_builder_add_int_value(builder, item->ts);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * A small picture of one stored file, for a gallery tile.
     *
     * A PDF's first page as a PNG (poppler, when this build has it); a
     * text file's first lines as `text`.  Pictures are not drawn here --
     * the clients already decode images at thumbnail size from
     * attachment.get -- and anything else has no thumbnail.  The id is
     * checked by clawt_attachment_path() before any file is opened, and
     * `size` is clamped, so a request cannot name a path or a surface
     * of its own choosing.
     */
    if (g_strcmp0(kind, "attachment.thumbnail") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        guint size = clawt_attachment_thumbnail_size(
            clawt_ipc_payload_int(payload, "size", 0));
        g_autofree gchar *path = NULL;
        g_autofree gchar *name = NULL;
        const gchar *file_kind;

        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which attachment?");

        if (self->attachment_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no attachments");

        path = clawt_attachment_path(self->attachment_dir, id);

        if (path == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not an attachment id");

        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such attachment");

        name = clawt_attachment_name(id);
        file_kind = clawt_attachment_kind(name);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder, file_kind);

        if (g_strcmp0(file_kind, "text") == 0) {
            g_autofree gchar *text = clawt_attachment_text_head(
                self->attachment_dir, id, 12, &error);

            if (text == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            json_builder_set_member_name(builder, "text");
            json_builder_add_string_value(builder, text);
        } else if (g_strcmp0(file_kind, "pdf") == 0) {
            guint width = 0;
            guint height = 0;
            g_autoptr(GBytes) png = clawt_attachment_thumbnail(
                self->attachment_dir, id, size, &width, &height, &error);
            g_autofree gchar *encoded = NULL;
            gsize length = 0;
            const guchar *data;

            if (png == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            data = g_bytes_get_data(png, &length);
            encoded = g_base64_encode(data, length);

            json_builder_set_member_name(builder, "mime");
            json_builder_add_string_value(builder, "image/png");
            json_builder_set_member_name(builder, "width");
            json_builder_add_int_value(builder, (gint64)width);
            json_builder_set_member_name(builder, "height");
            json_builder_add_int_value(builder, (gint64)height);
            json_builder_set_member_name(builder, "base64");
            json_builder_add_string_value(builder, encoded);
        } else {
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                g_strcmp0(file_kind, "image") == 0
                    ? "a picture is its own thumbnail: fetch it with "
                      "attachment.get"
                    : "there is no thumbnail for this kind of file");
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.list") == 0) {
        gboolean open_only = clawt_ipc_payload_boolean(payload, "open", TRUE);
        const gchar *room = clawt_ipc_payload_string(payload, "room");
        g_autoptr(GPtrArray) decisions = NULL;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        guint i;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        /*
         * A room narrows it to that conversation's view: every state,
         * oldest first, because a chat draws a settled question where
         * it was asked rather than dropping it out of the transcript.
         * `open` is ignored there for the same reason.
         */
        decisions = (room != NULL)
            ? clawt_decision_store_list_room(self->decisions, room)
            : clawt_decision_store_list(self->decisions, open_only);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decisions");
        json_builder_begin_array(builder);

        for (i = 0; decisions != NULL && i < decisions->len; i++)
            clawt_daemon_add_decision_object(
                builder, g_ptr_array_index(decisions, i), now);

        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "open");
        json_builder_add_int_value(
            builder, clawt_decision_store_count_open(self->decisions));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.answer") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        const gchar *answer = clawt_ipc_payload_string(payload, "answer");
        const gchar *via = clawt_ipc_payload_string(payload, "via");
        g_autoptr(ClawtDecision) settled = NULL;
        g_autoptr(ClawtSkillProposal) proposal = NULL;
        g_autoptr(GError) answer_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        if (id == NULL || answer == NULL || *answer == '\0')
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "answering needs a decision and an answer");

        /*
         * Refused where it arrives: a surface nobody can name would be
         * stored and then drawn as a blank in every view of the
         * decision, which is a refusal postponed to where it cannot be
         * explained.
         */
        if (!clawt_decision_via_is_known(via))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "'via' is chat, inbox or cli");

        /*
         * A skill proposal is carried out before its decision is
         * settled, so a refusal -- a draft changed since it was
         * reviewed, an "edit" with no text -- leaves the question open
         * for another answer instead of reporting one that did nothing.
         */
        if (clawt_skill_proposal_id_is_valid(id)) {
            g_autoptr(ClawtDecision) open = clawt_decision_store_get(self->decisions, id);

            if (open != NULL &&
                clawt_decision_get_state(open) == CLAWT_DECISION_OPEN) {
                if (!clawt_daemon_skill_proposal_check_answer(self, id, answer,
                                                              &answer_error))
                    return clawt_ipc_error_new(request, answer_error->code,
                                               answer_error->message);

                proposal = clawt_daemon_skill_proposal_answer(self, id, answer,
                                                              &answer_error);
                if (proposal == NULL)
                    return clawt_ipc_error_new(request, answer_error->code,
                                               answer_error->message);
            }
        }

        /*
         * The check and the write happen inside one call on the
         * daemon's one main context, so two presses -- a double tap, or
         * the inline view and the inbox at once -- are ordered here, and
         * the second is refused with what the first one did.  The code
         * is passed through so a press on a deleted decision reads as
         * NOT_FOUND rather than as a generic failure.
         */
        settled = clawt_decision_store_answer_via(self->decisions, id, answer,
                                                  via, &answer_error);

        if (settled == NULL)
            return clawt_ipc_error_new(request, answer_error->code,
                                       answer_error->message);

        /*
         * And it goes back to whoever asked.
         *
         * Without this the inbox is a suggestion box: the operator
         * answers into the void and the agent never learns.  Routed as
         * an ordinary message so it costs the agent a turn and reaches
         * it through the machinery everything else uses -- and carrying
         * the task id, so an answer that arrives after the agent has
         * moved on can still be attached to what it was about.
         */
        /*
         * A policy question has a call waiting on it, not a turn: the
         * answer releases (or refuses) that call, and a message as well
         * would tell the agent twice and cost it a turn to read.
         */
        /*
         * A proposal has told its agent already, in its own words; and a
         * rebuild request is answered by doing it, or by saying it was
         * not done -- not by quoting the answer back.
         */
        if (proposal == NULL &&
            !clawt_daemon_rebuild_settle(self, settled, "answered") &&
            !clawt_daemon_office_generate_settle(self, settled, "answered") &&
            !clawt_daemon_policy_settle(self, settled, "answered") &&
            !clawt_daemon_review_settle(self, settled, "answered") &&
            !clawt_daemon_federation_hold_settle(self, settled, "answered"))
            clawt_daemon_deliver_decision_answer(self, settled);

        /*
         * And back to whichever system staged the change, if one did.
         *
         * A decision raised from VENTURE's queue is only half answered
         * by telling the agent: the change is still sitting there
         * waiting for an approve or a reject, and venture drops it
         * unanswered when its own TTL runs out.  The bridge takes the
         * ones that are its own and says so; every other decision
         * answers here exactly as it always did.
         */
        clawt_daemon_venture_answer(self, settled);

        clawt_daemon_publish_decision_settled(self, settled, "answered");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decision");
        clawt_daemon_add_decision_object(builder, settled,
                                         g_get_real_time() / G_USEC_PER_SEC);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.dismiss") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        g_autoptr(ClawtDecision) dismissed = NULL;
        g_autoptr(GError) dismiss_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        /*
         * Read before it is dismissed, because the turn budget it was
         * holding has to be released and only the record says whose turn
         * that was.  A dismissal that did not release would leave the
         * agent's clock parked for ever on a question nobody is going to
         * answer, which is a turn that can never time out.
         */
        dismissed = clawt_decision_store_get(self->decisions, id);

        if (!clawt_decision_store_dismiss(self->decisions, id,
                                          &dismiss_error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       dismiss_error->message);

        if (dismissed != NULL) {
            /* settle() releases its own hold; anything else is released here. */
            if (!clawt_daemon_rebuild_settle(self, dismissed, "dismissed") &&
                !clawt_daemon_office_generate_settle(self, dismissed, "dismissed") &&
                !clawt_daemon_policy_settle(self, dismissed, "dismissed") &&
                !clawt_daemon_federation_hold_settle(self, dismissed, "dismissed"))
                clawt_daemon_turn_release(self,
                                          clawt_decision_get_agent(dismissed));
            /* a draft nobody will be asked about again is rejected */
            if (clawt_decision_get_state(dismissed) == CLAWT_DECISION_OPEN)
                clawt_daemon_skill_proposal_withdraw(self, dismissed);
            clawt_daemon_publish_decision_settled(self, dismissed, "dismissed");
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "dismissed");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.delete") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        g_autoptr(ClawtDecision) deleted = NULL;
        g_autoptr(GError) delete_error = NULL;
        g_autoptr(ClawtEvent) event = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");
        if (id == NULL || *id == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "deleting needs a decision");

        /*
         * Read first, for the same reason dismiss does: an open question
         * may be holding an agent's turn clock or a policy call, and only
         * the record says whose.  Forgetting the row without releasing
         * them would leave a turn that can never time out and a call that
         * waits for ever on a question that no longer exists anywhere.
         */
        deleted = clawt_decision_store_get(self->decisions, id);

        if (!clawt_decision_store_delete(self->decisions, id, &delete_error))
            return clawt_ipc_error_new(request, delete_error->code,
                                       delete_error->message);

        if (deleted != NULL &&
            clawt_decision_get_state(deleted) == CLAWT_DECISION_OPEN) {
            if (!clawt_daemon_rebuild_settle(self, deleted, "deleted") &&
                !clawt_daemon_office_generate_settle(self, deleted, "deleted") &&
                !clawt_daemon_policy_settle(self, deleted, "deleted") &&
                !clawt_daemon_federation_hold_settle(self, deleted, "deleted"))
                clawt_daemon_turn_release(self,
                                          clawt_decision_get_agent(deleted));
            clawt_daemon_skill_proposal_withdraw(self, deleted);
            /* only an open one was counted, so only it may bring a badge down */
            clawt_daemon_publish_decision_settled(self, deleted, "deleted");
        }

        /*
         * Every list holding the row has to drop it, settled or not --
         * decision.settled says "no longer waiting", which for an
         * already-answered row is not news and would double-count.
         */
        if (self->bus != NULL) {
            event = clawt_event_new("decision.deleted",
                                    deleted != NULL
                                        ? clawt_decision_get_agent(deleted)
                                        : NULL);
            clawt_event_set_detail(event, "decision", id);
            /* so a chat drawing it inline can take it out of the room too */
            if (deleted != NULL)
                clawt_event_set_detail(event, "room",
                                       clawt_decision_get_room(deleted));
            clawt_event_bus_publish(self->bus, event);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "deleted");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "event.list") == 0) {
        const gchar *subject = clawt_ipc_payload_string(payload, "subject");
        guint limit = (guint)clawt_ipc_payload_int(payload, "limit", 200);
        g_autoptr(GPtrArray) events = NULL;
        guint i;

        /*
         * What the fleet has been doing, from the log that has been
         * recording it all along.
         *
         * ClawtEventLog has written every published event to NDJSON
         * since the daemon was written, sweeps on `daemon.event_log_days`
         * -- and was read back by nobody.  A client can hold the recent
         * ones in memory; anything older than that was on disk and
         * unreachable, which is why diagnosing a message loop meant
         * running sqlite3 and grep on the host.
         *
         * Fleet-wide unless a subject is named, because the case that
         * sends somebody to the shell is watching several agents at
         * once.
         */
        if (self->log == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no event log");

        events = clawt_event_log_read(self->log, subject, limit);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "events");
        json_builder_begin_array(builder);

        for (i = 0; events != NULL && i < events->len; i++) {
            ClawtEvent *event = g_ptr_array_index(events, i);
            g_autoptr(JsonNode) node = clawt_ipc_event_new(event);
            JsonObject *frame = json_node_get_object(node);

            /*
             * The event frame's own payload, rather than a second
             * spelling of what an event is.  clawt_ipc_event_new() is
             * what a subscriber receives, so a client reading history
             * and a client receiving live events parse one shape.
             */
            json_builder_add_value(
                builder,
                json_node_ref(json_object_get_member(frame, "payload")));
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "attachment.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;

        if (agent_id == NULL || name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and name are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * Rebuilt from its basename and resolved through the exchange,
         * exactly as attachment.put does. A client asking to delete
         * "../../../etc/passwd" gets a refusal about a file in its own
         * drop-box that does not exist.
         */
        safe = g_path_get_basename(name);
        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (g_unlink(host_path) != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       g_strerror(errno));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_string_value(builder, host_path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── usage ── */
    if (g_strcmp0(kind, "usage.summary") == 0) {
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        ClawtUsageTotals fleet = { 0, 0, 0, 0 };
        gint64 since = clawt_ipc_payload_int(payload, "since", 0);
        gint64 days_asked = clawt_ipc_payload_int(payload, "days", 0);
        guint days = (guint)CLAMP(days_asked, 0, 92);
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        gint64 idle_hours = clawt_config_get_int(self->config,
                                                 "orchestration.idle_hours");
        g_autofree gint64 *day_starts = NULL;
        g_autofree gint64 *fleet_daily = NULL;
        g_autoptr(GArray) rows = NULL;
        guint i;

        /*
         * The Fleet page's read, on request: the last turn and the idle
         * state always (they are cheap and every caller wants them), a
         * per-day series only when asked for, since it is the part that
         * grows with the window.  Capped at a quarter so a typo cannot
         * ask for a decade of empty days.
         */
        if (days > 0) {
            day_starts = clawt_usage_day_starts(now, days);
            fleet_daily = g_new0(gint64, days);
        }

        /*
         * Read every agent first, then write: "sort": "cost" orders the
         * rows by what they spent in the period, most first -- ordering
         * is the daemon's to decide, as the fleet grouping is, so the
         * clients do not each sort a list their own way.
         */
        rows = g_array_new(FALSE, TRUE, sizeof(UsageRow));
        g_array_set_clear_func(rows, usage_row_clear);

        for (i = 0; i < agents->len; i++) {
            ClawtAgent *agent = g_ptr_array_index(agents, i);
            const gchar *agent_id = clawt_agent_get_id(agent);
            g_autofree gchar *state_dir = NULL;
            g_autofree gchar *db_path = NULL;
            g_autoptr(GError) read_error = NULL;
            UsageRow row = { 0 };

            state_dir = clawt_config_agent_state_dir(self->config, agent_id);
            if (state_dir == NULL)
                continue;

            db_path = clawt_usage_database_path(state_dir);
            row.agent = agent;
            if (days > 0)
                row.daily = g_new0(gint64, days);

            /*
             * One unreadable database does not fail the summary.  A
             * fleet report that refuses because one agent's file is
             * mid-write tells you nothing about the other nine.
             */
            if (!clawt_usage_read_report(db_path, since, day_starts, days,
                                         &row.totals, &row.last_turn_at, row.daily,
                                         &read_error)) {
                g_debug("usage: %s: %s", agent_id,
                        read_error != NULL ? read_error->message : "unknown");
            }

            g_array_append_val(rows, row);
        }

        if (g_strcmp0(clawt_ipc_payload_string(payload, "sort"), "cost") == 0)
            g_array_sort(rows, usage_row_by_cost);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; i < rows->len; i++) {
            UsageRow *row = &g_array_index(rows, UsageRow, i);
            ClawtAgent *agent = row->agent;
            ClawtIdleState idle;
            guint d;

            clawt_usage_totals_add(&fleet, &row->totals);
            idle = clawt_usage_idle_state(row->last_turn_at, now, idle_hours);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, clawt_agent_get_id(agent));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder,
                                          clawt_agent_get_name(agent));
            json_builder_set_member_name(builder, "turns");
            json_builder_add_int_value(builder, row->totals.turns);
            json_builder_set_member_name(builder, "input_tokens");
            json_builder_add_int_value(builder, row->totals.input_tokens);
            json_builder_set_member_name(builder, "output_tokens");
            json_builder_add_int_value(builder, row->totals.output_tokens);
            json_builder_set_member_name(builder, "cost_micros");
            json_builder_add_int_value(builder, row->totals.cost_micros);

            /* 0 is never: an agent idle longer than the period is the point */
            json_builder_set_member_name(builder, "last_turn_at");
            json_builder_add_int_value(builder, row->last_turn_at);
            json_builder_set_member_name(builder, "idle");
            json_builder_add_string_value(builder, clawt_usage_idle_label(idle));
            json_builder_set_member_name(builder, "idle_tone");
            json_builder_add_string_value(builder, clawt_usage_idle_tone(idle));
            json_builder_set_member_name(builder, "state");
            json_builder_add_string_value(builder,
                clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                   clawt_agent_get_state(agent)));
            json_builder_set_member_name(builder, "idle_stop_hours");
            json_builder_add_int_value(builder,
                clawt_agent_config_get_int(clawt_agent_get_config(agent),
                                           "runtime.idle_stop_hours"));
            json_builder_set_member_name(builder, "asleep");
            json_builder_add_boolean_value(builder,
                clawt_daemon_idle_is_asleep(self, clawt_agent_get_id(agent)));

            /* The Fleet page badges an agent on a fallback, as the sidebar does. */
            clawt_daemon_add_failover_members(builder, agent);

            if (days > 0) {
                json_builder_set_member_name(builder, "daily");
                json_builder_begin_array(builder);
                for (d = 0; d < days; d++) {
                    json_builder_add_int_value(builder, row->daily[d]);
                    fleet_daily[d] += row->daily[d];
                }
                json_builder_end_array(builder);
            }
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        if (days > 0) {
            guint d;

            /* the fleet's spend per day, oldest first, and where each starts */
            json_builder_set_member_name(builder, "days");
            json_builder_begin_array(builder);
            for (d = 0; d < days; d++) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "start");
                json_builder_add_int_value(builder, day_starts[d]);
                json_builder_set_member_name(builder, "cost_micros");
                json_builder_add_int_value(builder, fleet_daily[d]);
                json_builder_end_object(builder);
            }
            json_builder_end_array(builder);
        }

        json_builder_set_member_name(builder, "now");
        json_builder_add_int_value(builder, now);
        json_builder_set_member_name(builder, "idle_hours");
        json_builder_add_int_value(builder, idle_hours);
        json_builder_set_member_name(builder, "daily_fleet_budget_micros");
        json_builder_add_int_value(builder,
            clawt_config_get_int(self->config,
                                 "orchestration.daily_fleet_budget_micros"));

        json_builder_set_member_name(builder, "total");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "turns");
        json_builder_add_int_value(builder, fleet.turns);
        json_builder_set_member_name(builder, "input_tokens");
        json_builder_add_int_value(builder, fleet.input_tokens);
        json_builder_set_member_name(builder, "output_tokens");
        json_builder_add_int_value(builder, fleet.output_tokens);
        json_builder_set_member_name(builder, "cost_micros");
        json_builder_add_int_value(builder, fleet.cost_micros);
        json_builder_end_object(builder);

        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, since);

        /*
         * What the budget would refuse right now, so a client can show
         * the cap beside the spend rather than making somebody go and
         * read the config to find out what the number means.
         */
        json_builder_set_member_name(builder, "task_budget_usd");
        json_builder_add_double_value(
            builder,
            clawt_config_get_double(self->config,
                                    "orchestration.task_budget_usd"));

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "usage.ledger") == 0) {
		GPtrArray *agents = clawt_agent_manager_list(self->agents);
		gint64 since = clawt_ipc_payload_int(payload, "since", 0);
		const gchar *by = clawt_ipc_payload_string(payload, "by");
		const gchar *only = clawt_ipc_payload_string(payload, "agent");
		g_autoptr(ClawtUsageLedger) ledger = NULL;
		JsonNode *body;
		JsonObject *object;
		guint i;

		/*
		 * Refused rather than quietly read as "agent": a client asking
		 * for a grouping this daemon does not have would otherwise draw
		 * a table under the wrong heading and call it the answer.
		 */
		if (by != NULL && *by != '\0' && clawt_usage_group_index(by) < 0)
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
				"usage.ledger groups by agent, model, origin or day");

		if (only != NULL && *only != '\0' &&
		    clawt_agent_manager_get(self->agents, only) == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
						   "no such agent");

		ledger = clawt_usage_ledger_new(by);

		for (i = 0; agents != NULL && i < agents->len; i++) {
			const gchar *agent_id = clawt_agent_get_id(
				g_ptr_array_index(agents, i));
			g_autofree gchar *state_dir = NULL;
			g_autoptr(GError) read_error = NULL;

			if (only != NULL && *only != '\0' && g_strcmp0(only, agent_id) != 0)
				continue;

			state_dir = clawt_config_agent_state_dir(self->config, agent_id);
			if (state_dir == NULL)
				continue;

			/* one unreadable file does not refuse a report on the rest */
			if (!clawt_usage_ledger_add_agent(ledger, agent_id, state_dir,
							  since, &read_error))
				g_debug("usage ledger: %s: %s", agent_id,
					read_error != NULL ? read_error->message : "unknown");
		}

		body = clawt_usage_ledger_to_json(ledger);
		object = json_node_get_object(body);
		json_object_set_int_member(object, "since", since);
		json_object_set_int_member(object, "now",
					   g_get_real_time() / G_USEC_PER_SEC);
		if (only != NULL && *only != '\0')
			json_object_set_string_member(object, "agent", only);

		return clawt_ipc_response_new(request, body);
	}

    if (g_strcmp0(kind, "attachment.put") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *encoded = clawt_ipc_payload_string(payload, "data");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        g_autofree guchar *bytes = NULL;
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;
        gsize length = 0;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (name == NULL || encoded == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "name and data are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * The name is taken apart and rebuilt rather than trusted.  It
         * comes from a filename a person dragged in or a clipboard
         * suggestion, and "../../.ssh/authorized_keys" is a name.
         */
        safe = g_path_get_basename(name);

        if (safe[0] == '\0' || g_strcmp0(safe, ".") == 0 ||
            g_strcmp0(safe, "..") == 0 || g_strcmp0(safe, G_DIR_SEPARATOR_S) == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not a usable file name");

        bytes = g_base64_decode(encoded, &length);

        if (bytes == NULL || length == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "the attachment is empty");

        /*
         * The agent's own directory, made if this is the first thing
         * ever put in it. resolve() answers where a path *would* be, so
         * without this the very first attachment failed on a directory
         * that had never been created.
         */
        if (!clawt_exchange_prepare(self->exchange, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_write_file_atomic(host_path, (const gchar *)bytes,
                                     (gssize)length, 0600, FALSE, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, safe);
        json_builder_set_member_name(builder, "host_path");
        json_builder_add_string_value(builder, host_path);

        /*
         * The path to *tell the agent*, which is not the host path when
         * it lives in a container: the exchange is mounted, so the
         * agent sees it somewhere else entirely and a host path would
         * send it looking for a file that is not there.
         */
        json_builder_set_member_name(builder, "path");

        {
            const gchar *computer = clawt_agent_config_get_string(
                clawt_agent_get_config(agent), "computer.type");

            if (g_strcmp0(computer, "container") == 0 ||
                g_strcmp0(computer, "vm") == 0) {
                g_autofree gchar *guest = g_build_filename(
                    CLAWT_EXCHANGE_MOUNT_POINT, agent_id, safe, NULL);

                json_builder_add_string_value(builder, guest);
            } else {
                json_builder_add_string_value(builder, host_path);
            }
        }

        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);

        /*
         * And a copy in the daemon's own keeping, named by id, as an
         * agent's file is -- so the operator's message can carry
         * `clawt:<id>` and every client shows the picture by asking for
         * it over IPC.  The host path alone only drew a preview for a
         * client that could open that exact path itself: a remote or
         * sandboxed one showed nothing, and the exchange's size cap
         * would have taken even a local one's picture in time.  Best
         * effort: the agent has its file either way.
         */
        if (self->attachment_dir != NULL) {
            g_autoptr(GError) store_error = NULL;
            g_autofree gchar *id = clawt_attachment_store(
                self->attachment_dir, host_path, &store_error);

            if (id != NULL) {
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, id);
            } else {
                g_warning("attachment.put: %s was saved for %s but not "
                          "kept for the clients: %s", safe, agent_id,
                          store_error != NULL ? store_error->message : "?");
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "exchange.list") == 0) {
        g_autoptr(GPtrArray) entries = NULL;
        guint i;

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this fleet has no exchange "
                                       "directory");

        entries = clawt_exchange_list(self->exchange,
                                      clawt_ipc_payload_string(payload,
                                                               "path"));

        if (entries == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such directory in the exchange");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "entries");
        json_builder_begin_array(builder);

        for (i = 0; i < entries->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(entries, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * One call an agent is about to make, judged by its action rules.
     *
     * Asked by the routes that are not clawtilla's own tools: the Claude
     * Code PreToolUse hook (the CLI's Bash, Write, ...) and the connector
     * and desktop relays. Each names the tool as the agent called it and,
     * when it knows better than the native table, the class. Deferred,
     * because `ask` waits for a person and `review` for a model.
     */
    if (g_strcmp0(kind, "policy.check") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *tool = clawt_ipc_payload_string(payload, "tool");
        const gchar *class_nick = clawt_ipc_payload_string(payload, "class");
        const gchar *summary = clawt_ipc_payload_string(payload, "summary");
        ClawtAgentConfig *config;
        ClawtActionClass action = CLAWT_ACTION_EXEC;
        ClawtPolicyMode mode;
        g_autofree gchar *rule = NULL;
        gint parsed = 0;
        ClawtIpcPending *pending;

        if (agent_id == NULL || tool == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and tool are both required");
        /*
         * A token, when one is sent, must be the agent's. The relays send
         * none -- they run from the agent's own CLI with no token to hand
         * -- and are let in on the strength of the socket, which is the
         * user's alone: a check can only ever restrict the caller.
         */
        if (clawt_ipc_payload_string(payload, "token") != NULL &&
            !clawt_daemon_authenticate_agent(agent_id,
                                             clawt_ipc_payload_string(payload, "token"),
                                             self))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "that is not this agent's token");

        config = clawt_config_get_agent(self->config, agent_id);

        /*
         * clawtilla's own tools, reached through the CLI's MCP client,
         * are gated by the daemon when they arrive at tool.rpc; asking
         * here as well would ask a person twice for one call.
         */
        if (clawt_policy_native_is_clawtilla(tool)) {
            mode = CLAWT_POLICY_ALLOW;
        } else {
            if (class_nick != NULL &&
                clawt_enum_from_nick(CLAWT_TYPE_ACTION_CLASS, class_nick, &parsed))
                action = (ClawtActionClass)parsed;
            else if (!clawt_mcp_tools_action_class(tool, &action))
                action = clawt_policy_class_for_native_tool(tool);
            mode = clawt_policy_mode_for(config, tool, action, &rule);

            /* A read-only routine's room outranks every rule. */
            if (action != CLAWT_ACTION_READ &&
                clawt_mcp_tools_room_is_read_only(self->mcp_tools,
                                                  clawt_ipc_payload_string(payload, "room"))) {
                g_free(rule);
                rule = g_strdup("this is a read-only routine run; report what you find "
                                "with clawtilla_ask_decision instead of acting on it");
                mode = CLAWT_POLICY_DENY;
            }

            /*
             * Nor may a consultation's own CLI act: its Bash, its edits,
             * a connector's writes.  The clawtilla tools it was offered
             * already leave these out; this is the same rule for the
             * tools clawtilla does not list.
             */
            if (mode != CLAWT_POLICY_DENY &&
                clawt_mcp_tools_consult_withholds(tool, action) &&
                clawt_mcp_tools_turn_is_consult(self->mcp_tools, agent_id,
                                                clawt_ipc_payload_string(payload, "room"))) {
                g_free(rule);
                rule = g_strdup(CLAWT_CONSULT_RULE);
                mode = CLAWT_POLICY_DENY;
            }

            /*
             * Work done for another fleet is capped by that fleet's grant
             * (0.69.0): the stricter of the two answers, named as the cap
             * when it is the cap that decided.
             */
            {
                ClawtPolicyMode cap = clawt_daemon_federation_cap(
                    self, agent_id, clawt_ipc_payload_string(payload, "room"), tool, action);
                ClawtPolicyMode both = clawt_fed_policy_stricter(mode, cap);

                if (both != mode) {
                    g_free(rule);
                    rule = g_strdup("this turn works for a linked fleet, and its grant "
                                    "caps what may be done for it (docs/federation.org#caps)");
                    mode = both;
                }
            }
        }

        /*
         * A call a relay is about to pass on, marked on the agent's turn
         * when it reaches outside the fleet -- a connector's write, a
         * click.  Here, before the call, because a relay reports nothing
         * after it: a call that then fails is counted anyway, which is
         * the direction that costs a rerun rather than a second email.
         * Not for a denial, which did nothing.
         */
        /*
         * An MCP app's own call (`origin: app`) is not the agent's turn
         * reaching outward: the person pressed something in the app, and
         * marking whichever turn happens to be running would make a
         * restart refuse to rerun work the agent never did.
         */
        if (mode != CLAWT_POLICY_DENY &&
            g_strcmp0(clawt_ipc_payload_string(payload, "origin"),
                      "app") != 0) {
            g_autofree gchar *outward =
                clawt_mcp_tools_relay_outward_label(tool, action);
            ClawtAgent *caller = clawt_agent_manager_get(self->agents, agent_id);

            if (outward != NULL && caller != NULL &&
                !clawt_policy_native_is_clawtilla(tool))
                clawt_agent_mark_outward(caller,
                    clawt_ipc_payload_string(payload, "room"), outward);
        }

        if (mode == CLAWT_POLICY_ALLOW || mode == CLAWT_POLICY_DENY) {
            g_autoptr(JsonBuilder) answer = json_builder_new();

            json_builder_begin_object(answer);
            json_builder_set_member_name(answer, "allowed");
            json_builder_add_boolean_value(answer, mode == CLAWT_POLICY_ALLOW);
            if (mode == CLAWT_POLICY_DENY) {
                g_autofree gchar *reason = g_strdup_printf(
                    "Refused by the action rule '%s'. This is a standing rule, not "
                    "a mistake in your call: do not retry it.", rule);

                json_builder_set_member_name(answer, "reason");
                json_builder_add_string_value(answer, reason);
            }
            json_builder_end_object(answer);
            return clawt_ipc_response_new(request, json_builder_get_root(answer));
        }

        pending = clawt_ipc_server_defer(self->ipc_server, request);
        if (pending == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered later");

        clawt_daemon_policy_request(self, agent_id, tool, action, mode, rule,
                                    summary != NULL ? summary : tool,
                                    on_policy_check_reply, pending);
        return NULL;
    }

    if (g_strcmp0(kind, "tool.rpc") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *token = clawt_ipc_payload_string(payload, "token");
        JsonNode *rpc = (payload != NULL &&
                         json_object_has_member(payload, "request"))
                        ? json_object_get_member(payload, "request") : NULL;
        g_autoptr(JsonNode) rpc_response = NULL;

        /*
         * The orchestration tools, reachable over IPC.
         *
         * `room` says which of the agent's conversations the call
         * belongs to.  clawtilla-mcp-server reads it from the
         * environment libreclaw set on the CLI that spawned it -- one
         * room per session -- so it is the runtime's answer rather than
         * the model's.  Absent from an older runtime, and the tools fold
         * across the agent's running turns instead.
         *
         * They were served only over the agent's link, as mcp.request
         * frames -- which assumed something on the agent side would
         * relay them into its AI session. Nothing did, and nothing
         * could: an agent runs a CLI whose only way of being given
         * tools is an --mcp-config pointing at a real MCP server. This
         * is the verb clawtilla-mcp-server speaks so that server can
         * exist.
         */
        if (agent_id == NULL || rpc == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and request are both required");

        /*
         * The agent's own token, checked the same way the link checks
         * it. The socket's permissions are the first line; this stops
         * one agent on this machine calling tools as another.
         */
        if (!clawt_daemon_authenticate_agent(agent_id, token, self))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "that is not this agent's token");

        /*
         * computer.exec waits for a command the agent chose, so it is
         * answered from a worker thread rather than from here.  A
         * handler that waited would hold the daemon's main context for
         * the length of that command -- a build, a test run, something
         * reading from a terminal that is not there -- and while it did,
         * every other agent's link, every client, the event stream and
         * the daemon's own SIGTERM would go unserved.  That is the
         * documented rule about IPC handlers and the network, met on the
         * one path where the wait is not a round trip but an arbitrary
         * command.
         */
        /*
         * A call from a turn on one of the agent's own channels says so
         * (since 0.34.2): remembered, so an answer to something asked
         * from that conversation can be handed back into it.
         */
        {
            const gchar *channel = clawt_ipc_payload_string(payload,
                                                            "channel");
            ClawtAgent *caller = (self->agents != NULL)
                ? clawt_agent_manager_get(self->agents, agent_id) : NULL;

            if (caller != NULL && channel != NULL &&
                g_strcmp0(channel, "clawtilla") != 0)
                clawt_agent_note_channel_room(caller,
                    clawt_ipc_payload_string(payload, "room"), channel,
                    clawt_ipc_payload_string(payload, "peer"));
        }

        if (clawt_mcp_tools_call_defers(self->mcp_tools, agent_id, rpc)) {
            ClawtIpcPending *pending =
                clawt_ipc_server_defer(self->ipc_server, request);

            if (pending == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this request cannot be "
                                           "answered later");

            clawt_mcp_tools_call_async(
                self->mcp_tools, agent_id,
                clawt_ipc_payload_string(payload, "room"), rpc,
                clawt_daemon_on_tool_rpc_finished, pending);

            return NULL;
        }

        rpc_response = clawt_mcp_tools_call(self->mcp_tools, agent_id,
                                            clawt_ipc_payload_string(payload,
                                                                     "room"),
                                            rpc);

        if (rpc_response == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "the tool produced no response");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "response");
        json_builder_add_value(builder, json_node_ref(rpc_response));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "model.list") == 0) {
        const ClawtProviderInfo *catalog;
        gboolean refresh = clawt_ipc_payload_boolean(payload, "refresh",
                                                      FALSE);
        gsize n_providers = 0;
        gsize i;

        catalog = clawt_model_catalog_get(&n_providers);

        /*
         * A stale cache is refreshed behind this request rather than
         * during it. The caller gets whatever is known now; the next
         * one gets the fresh answer.
         */
        if (refresh &&
            (self->model_cache_at == 0 ||
             g_get_monotonic_time() - self->model_cache_at >
                 (gint64)MODEL_CACHE_TTL_SECONDS * G_USEC_PER_SEC))
            clawt_daemon_warm_model_cache(self);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "providers");
        json_builder_begin_array(builder);

        for (i = 0; i < n_providers; i++) {
            gsize j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, catalog[i].id);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, catalog[i].label);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            /*
             * Passed on so a client knows to offer a way to type a name
             * that is not listed.  The catalogue is curated and goes
             * stale; nothing validates against it.
             */
            json_builder_set_member_name(builder, "open_ended");
            json_builder_add_boolean_value(builder, catalog[i].open_ended);

            /*
             * Whether libreclaw can actually run an agent on this
             * provider.  Its provider table is command-line only and
             * rewrites anything else to claude-code with a warning, so a
             * client that offers every provider here lets someone pick
             * OpenAI and quietly get Claude Code with "gpt-4o" in the
             * model field.
             */
            json_builder_set_member_name(builder, "agent");
            json_builder_add_boolean_value(builder, catalog[i].agent);

            /*
             * Whether this provider can be given tools, which decides
             * whether it can design an agent.  A client that offers
             * every provider for designing offers ones that will be
             * refused after the person has filled in the whole form.
             */
            json_builder_set_member_name(builder, "tools");
            json_builder_add_boolean_value(builder, catalog[i].tools);

            json_builder_set_member_name(builder, "models");
            json_builder_begin_array(builder);

            /*
             * The provider's own list, when asked for and reachable.
             *
             * The hardcoded table goes stale -- it offered grok-3 and
             * grok-4 well after 4.5 and 4.6 had shipped -- so a person
             * choosing a model should be shown what the provider
             * actually runs. Falls back to the table rather than
             * failing: no key, or no network, is not a reason to offer
             * nothing.
             */
            if (refresh && catalog[i].tools) {
                /*
                 * From the cache, never by asking now.  Asking here made
                 * the request take as long as the slowest provider, and
                 * both the new-agent dialog and the agent inspector ask
                 * on every build -- so pressing + or clicking an agent
                 * appeared to hang.
                 */
                GStrv live = g_hash_table_lookup(self->model_cache,
                                                  catalog[i].id);

                if (live != NULL && live[0] != NULL) {
                    gsize k;

                    for (k = 0; live[k] != NULL; k++) {
                        json_builder_begin_object(builder);
                        json_builder_set_member_name(builder, "id");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_set_member_name(builder, "label");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_end_object(builder);
                    }

                    json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "live");
                    json_builder_add_boolean_value(builder, TRUE);
                    json_builder_end_object(builder);
                    continue;
                }
            }

            for (j = 0; j < catalog[i].n_models; j++) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].id);
                json_builder_set_member_name(builder, "label");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].label);

                if (catalog[i].models[j].note != NULL) {
                    json_builder_set_member_name(builder, "note");
                    json_builder_add_string_value(
                        builder, catalog[i].models[j].note);
                }

                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
