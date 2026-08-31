/*
 * clawt-event.c - Something that happened in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-event.h"

struct _ClawtEvent {
    gchar      *kind;
    gchar      *subject;
    gint64      timestamp;
    guint64     cursor;
    GHashTable *details;   /* gchar* -> gchar*, insertion order not kept */
    GPtrArray  *order;     /* keys in the order they were set */
};

G_DEFINE_BOXED_TYPE(ClawtEvent, clawt_event, clawt_event_copy,
                    clawt_event_free)

ClawtEvent *
clawt_event_new(const gchar *kind, const gchar *subject)
{
    ClawtEvent *self;

    g_return_val_if_fail(kind != NULL, NULL);

    self = g_new0(ClawtEvent, 1);
    self->kind = g_strdup(kind);
    self->subject = g_strdup(subject);
    self->timestamp = g_get_real_time();
    self->details = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_free);

    /*
     * Field order is kept alongside the table so a rendered event is
     * byte-stable.  An event log whose field order wanders is painful to
     * diff and impossible to golden-test.
     */
    self->order = g_ptr_array_new_with_free_func(g_free);

    return self;
}

ClawtEvent *
clawt_event_copy(ClawtEvent *self)
{
    ClawtEvent *copy;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_event_new(self->kind, self->subject);
    copy->timestamp = self->timestamp;
    copy->cursor = self->cursor;

    for (i = 0; i < self->order->len; i++) {
        const gchar *key = g_ptr_array_index(self->order, i);

        clawt_event_set_detail(copy, key,
                               g_hash_table_lookup(self->details, key));
    }

    return copy;
}

void
clawt_event_free(ClawtEvent *self)
{
    if (self == NULL)
        return;

    g_free(self->kind);
    g_free(self->subject);
    g_clear_pointer(&self->details, g_hash_table_unref);
    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_free(self);
}

const gchar *
clawt_event_get_kind(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->kind;
}

const gchar *
clawt_event_get_subject(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->subject;
}

gint64
clawt_event_get_timestamp(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->timestamp;
}

void
clawt_event_set_timestamp(ClawtEvent *self, gint64 timestamp)
{
    g_return_if_fail(self != NULL);

    self->timestamp = timestamp;
}

guint64
clawt_event_get_cursor(ClawtEvent *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->cursor;
}

void
clawt_event_set_cursor(ClawtEvent *self, guint64 cursor)
{
    g_return_if_fail(self != NULL);

    self->cursor = cursor;
}

void
clawt_event_set_detail(ClawtEvent *self, const gchar *key, const gchar *value)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    if (value == NULL)
        return;

    if (!g_hash_table_contains(self->details, key))
        g_ptr_array_add(self->order, g_strdup(key));

    /*
     * Redacted on the way in, not on the way out.
     *
     * Events are teed to the event log and replayed into transcripts, so
     * a secret scrubbed only at display time is already on disk by then --
     * and stays there.
     */
    g_hash_table_replace(self->details, g_strdup(key),
                         clawt_redact_secrets(value));
}

void
clawt_event_set_detail_int(ClawtEvent *self, const gchar *key, gint64 value)
{
    g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

    clawt_event_set_detail(self, key, text);
}

gint64
clawt_event_get_detail_int(ClawtEvent *self, const gchar *key)
{
    const gchar *text = clawt_event_get_detail(self, key);

    if (text == NULL)
        return 0;

    return g_ascii_strtoll(text, NULL, 10);
}

const gchar *
clawt_event_get_detail(ClawtEvent *self, const gchar *key)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    return g_hash_table_lookup(self->details, key);
}

JsonNode *
clawt_event_to_json(ClawtEvent *self)
{
    g_autoptr(JsonBuilder) builder = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(builder, self->kind);

    if (self->subject != NULL) {
        json_builder_set_member_name(builder, "subject");
        json_builder_add_string_value(builder, self->subject);
    }

    json_builder_set_member_name(builder, "ts");
    json_builder_add_int_value(builder, self->timestamp);

    if (self->cursor != 0) {
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(builder, (gint64)self->cursor);
    }

    if (self->order->len > 0) {
        json_builder_set_member_name(builder, "detail");
        json_builder_begin_object(builder);

        for (i = 0; i < self->order->len; i++) {
            const gchar *key = g_ptr_array_index(self->order, i);

            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(
                builder, g_hash_table_lookup(self->details, key));
        }

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

ClawtAlertTier
clawt_alert_tier_for_event(ClawtEvent *event)
{
    const gchar *kind;

    g_return_val_if_fail(event != NULL, CLAWT_ALERT_SKIP);

    kind = clawt_event_get_kind(event);

    if (kind == NULL)
        return CLAWT_ALERT_SKIP;

    /*
     * One per percent, and a spinner.  Neither is a thing that happened.
     */
    if (g_strcmp0(kind, "image.progress") == 0 ||
        g_strcmp0(kind, "agent.typing") == 0)
        return CLAWT_ALERT_SKIP;

    /*
     * The two that arrive on their own.  Every other place a client says
     * something is answering a question somebody is holding right now,
     * and belongs in a toast rather than in a list.
     */
    if (g_strcmp0(kind, "message.refused") == 0)
        return CLAWT_ALERT_ERROR;

    /*
     * A turn or an exchange that clawtilla ended.  Errors rather than
     * notices: work was stopped, and somebody has to decide whether to
     * restart it -- an exchange stays ended until a person says
     * something in the room.
     */
    if (g_strcmp0(kind, "turn.timed_out") == 0 ||
        g_strcmp0(kind, "exchange.stalled") == 0)
        return CLAWT_ALERT_ERROR;

    /*
     * Two processes serving one agent id.  An error rather than a
     * notice, and the reason is the opposite of the usual one: nothing
     * about this agent looks wrong.  It is running, it has a link and
     * it answers, so there is no other surface where somebody would
     * come across it -- this line is the only one, and a person has to
     * go and end the second process.
     */
    if (g_strcmp0(kind, "agent.contested") == 0)
        return CLAWT_ALERT_ERROR;

    /*
     * A repeated tool call is a warning about a turn that is still
     * running.  Nothing has been stopped, so it is not an error; but it
     * arrived on its own while nobody was looking at that agent, which
     * is what makes it a notice rather than a toast.
     */
    if (g_strcmp0(kind, "turn.repeating") == 0)
        return CLAWT_ALERT_NOTICE;

    /*
     * A steer is what the person just did, so a client showing it in the
     * conversation has already answered the question.  Listing it would
     * be telling somebody about their own keystroke.
     */
    if (g_strcmp0(kind, "message.steered") == 0)
        return CLAWT_ALERT_SKIP;

    /*
     * A download that *succeeded* is routine; only the failure arrived on
     * its own with nobody watching.
     */
    if (g_strcmp0(kind, "image.finished") == 0)
        return (clawt_event_get_detail(event, "error") != NULL)
                   ? CLAWT_ALERT_ERROR : CLAWT_ALERT_ROUTINE;

    /*
     * An ownership transfer that did *not* happen.
     *
     * `done` is routine: the exchange is already written into the pair's
     * room and both threads, so a badge would be telling somebody about
     * something they can see.  Every other outcome means work did not
     * move and is still where it was -- and it arrived on its own, while
     * nobody was watching that pair, which is exactly what makes it a
     * notice rather than a toast.
     *
     * Classified on the detail rather than on the kind, for the reason
     * `image.finished` already is: a kind-only rule would put every
     * successful handoff in the loud list and teach somebody to stop
     * reading it.
     */
    if (g_strcmp0(kind, "handoff.settled") == 0)
        return (g_strcmp0(clawt_event_get_detail(event, "state"),
                          "done") == 0)
                   ? CLAWT_ALERT_ROUTINE : CLAWT_ALERT_NOTICE;

    /*
     * A persona that has outgrown what a command line can carry.  It only
     * arrives at all past 80% of the limit, so there is nothing routine
     * about it -- and it arrives while somebody is starting an agent
     * rather than while they are looking at one, which is what makes it a
     * thing that happened elsewhere.
     */
    if (g_strcmp0(kind, "agent.identity") == 0)
        return CLAWT_ALERT_NOTICE;

    if (g_strcmp0(kind, "agent.state") == 0) {
        const gchar *state = clawt_event_get_detail(event, "state");

        if (g_strcmp0(state, "error") == 0 ||
            g_strcmp0(state, "degraded") == 0)
            return CLAWT_ALERT_NOTICE;
    }

    return CLAWT_ALERT_ROUTINE;
}

gboolean
clawt_toast_should_show(const gchar *previous, gint64 previous_at_us,
                        const gchar *text, gint64 now_us)
{
    if (text == NULL || *text == '\0')
        return FALSE;

    /* Nothing said yet, or something else said last. */
    if (previous == NULL || previous_at_us <= 0 ||
        g_strcmp0(previous, text) != 0)
        return TRUE;

    /*
     * A clock that went backwards is not evidence the toast has gone.
     * Treated as "just now" rather than as a long time ago, because
     * being wrong in that direction shows one copy too few and the other
     * shows the stack this exists to prevent.
     */
    if (now_us < previous_at_us)
        return FALSE;

    return (now_us - previous_at_us) >
           ((gint64)CLAWT_TOAST_REPEAT_SECONDS * G_USEC_PER_SEC);
}

gboolean
clawt_alert_arrives_read(
    gboolean        surface_showing,
    ClawtAlertTier  tier
){
    /*
     * The routine stream is never counted, so there is nothing for it to
     * have been seen *for*.  Recording it unread would leave a flag that
     * no badge reads and that a later widening of the filter would
     * silently start believing.
     */
    if (tier == CLAWT_ALERT_ROUTINE)
        return TRUE;

    return surface_showing;
}

void
clawt_team_tally(JsonArray   *agents,
                 const gchar *team_id,
                 guint       *total,
                 guint       *running,
                 guint       *busy)
{
    const gchar *wanted = (team_id != NULL) ? team_id : "";
    guint i;

    g_return_if_fail(total != NULL && running != NULL && busy != NULL);

    *total = 0;
    *running = 0;
    *busy = 0;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *team;
        gboolean is_running;

        if (agent == NULL)
            continue;

        /*
         * Absent and empty are the same answer.  The daemon omits the
         * member for an agent in no team; the two clients defaulted it
         * differently, and only one of them would have matched an agent
         * whose team was written as "".
         */
        team = json_object_has_member(agent, "team")
               ? json_object_get_string_member(agent, "team") : "";

        if (team == NULL)
            team = "";

        if (g_strcmp0(team, wanted) != 0)
            continue;

        (*total)++;

        is_running = json_object_has_member(agent, "state") &&
                     g_strcmp0(json_object_get_string_member(agent, "state"),
                               "running") == 0;

        if (!is_running)
            continue;

        (*running)++;

        /*
         * Only a running agent can be working.  Nothing should report a
         * stopped agent as busy, and if something does, a heading
         * claiming it is mid-turn is worse than one that misses it.
         */
        if (json_object_has_member(agent, "busy") &&
            json_object_get_boolean_member(agent, "busy"))
            (*busy)++;
    }
}
