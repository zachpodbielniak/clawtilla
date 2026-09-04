/*
 * clawt-turn-step.c - One step of a turn that is still running
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clawtilla.h"

#include "chat/clawt-turn-step.h"

#include <string.h>

struct _ClawtTurnStep {
    ClawtStepKind  kind;
    gchar         *agent_id;
    gchar         *room_id;
    gchar         *text;
    gchar         *tool_name;
    gchar         *detail;
    gboolean       failed;
    gint64         timestamp;
};

G_DEFINE_BOXED_TYPE(ClawtTurnStep, clawt_turn_step,
                    clawt_turn_step_copy, clawt_turn_step_free)

ClawtTurnStep *
clawt_turn_step_new(ClawtStepKind  kind,
                    const gchar   *agent_id,
                    const gchar   *room_id,
                    const gchar   *text,
                    const gchar   *tool_name,
                    const gchar   *detail,
                    gboolean       failed)
{
    ClawtTurnStep *self;

    self = g_new0(ClawtTurnStep, 1);

    self->kind      = kind;
    self->agent_id  = g_strdup(agent_id);
    self->room_id   = g_strdup(room_id);
    self->text      = g_strdup(text);
    self->tool_name = g_strdup(tool_name);
    self->detail    = g_strdup(detail);
    self->failed    = failed;
    self->timestamp = g_get_real_time();

    return self;
}

ClawtTurnStep *
clawt_turn_step_new_from_event(ClawtEvent *event)
{
    ClawtTurnStep *self;
    const gchar   *nick;
    gint           value = CLAWT_STEP_STATUS;

    g_return_val_if_fail(event != NULL, NULL);

    if (g_strcmp0(clawt_event_get_kind(event), "turn.step") != 0)
        return NULL;

    /*
     * Resolved through the enum rather than compared against spelled-out
     * strings.  A nickname written out here is a second copy of the set,
     * and an unrecognised one would have to fall through to something --
     * which, for an enum whose zero value is a real member, means a step
     * this build does not understand would be drawn as ordinary prose.
     */
    nick = clawt_event_get_detail(event, CLAWT_STEP_MEMBER_KIND);

    if (!clawt_enum_from_nick(CLAWT_TYPE_STEP_KIND, nick, &value))
        value = CLAWT_STEP_STATUS;

    self = clawt_turn_step_new(
        (ClawtStepKind)value,
        clawt_event_get_subject(event),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_ROOM),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TEXT),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TOOL),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_DETAIL),
        clawt_event_get_detail_int(event, CLAWT_STEP_MEMBER_FAILED) != 0);

    /*
     * The step's own time when it carries one, and the event's
     * otherwise.  They are the same for a step arriving live; they
     * differ for one replayed out of a room's history, where the event
     * is a fresh envelope around an old step.
     */
    if (clawt_event_get_detail_int(event, CLAWT_STEP_MEMBER_TS) > 0)
        self->timestamp = clawt_event_get_detail_int(event,
                                                     CLAWT_STEP_MEMBER_TS);
    else
        self->timestamp = clawt_event_get_timestamp(event);

    return self;
}

/*
 * Reads a string member, or NULL when it is absent or is not a string.
 *
 * The type check matters as much as the presence check: a member that
 * arrived as a number would otherwise come back through json-glib's own
 * fallback rather than as the absence it effectively is.
 */
static const gchar *
object_string(JsonObject *object, const gchar *member)
{
    JsonNode *node;

    if (!json_object_has_member(object, member))
        return NULL;

    node = json_object_get_member(object, member);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

ClawtTurnStep *
clawt_turn_step_new_from_object(JsonObject *object, const gchar *agent_id)
{
    ClawtTurnStep *self;
    gint     value  = CLAWT_STEP_STATUS;
    gboolean failed = FALSE;

    g_return_val_if_fail(object != NULL, NULL);

    if (!clawt_enum_from_nick(CLAWT_TYPE_STEP_KIND,
                              object_string(object, CLAWT_STEP_MEMBER_KIND),
                              &value))
        value = CLAWT_STEP_STATUS;

    /*
     * The boolean reader, because the producer writes a JSON boolean.
     */
    if (json_object_has_member(object, CLAWT_STEP_MEMBER_FAILED))
        failed = json_object_get_boolean_member(object,
                                                CLAWT_STEP_MEMBER_FAILED);

    self = clawt_turn_step_new((ClawtStepKind)value, agent_id,
                               object_string(object, CLAWT_STEP_MEMBER_ROOM),
                               object_string(object, CLAWT_STEP_MEMBER_TEXT),
                               object_string(object, CLAWT_STEP_MEMBER_TOOL),
                               object_string(object, CLAWT_STEP_MEMBER_DETAIL),
                               failed);

    /*
     * The wire's time, or none at all.
     *
     * clawt_turn_step_new() stamps "now", which is right for a step
     * being created and wrong for one being read back: a step from a
     * daemon too old to send the member would be given the time it was
     * *parsed*, so it would sort after every message in the room and
     * land in a heap at the bottom -- which is the exact failure the
     * merge exists to avoid, arrived at from the other direction.
     * Zero sorts first instead, which is the harmless end.
     */
    self->timestamp = json_object_has_member(object, CLAWT_STEP_MEMBER_TS)
        ? json_object_get_int_member(object, CLAWT_STEP_MEMBER_TS)
        : 0;

    return self;
}

ClawtTurnStep *
clawt_turn_step_copy(ClawtTurnStep *self)
{
    ClawtTurnStep *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_turn_step_new(self->kind, self->agent_id, self->room_id,
                               self->text, self->tool_name, self->detail,
                               self->failed);
    copy->timestamp = self->timestamp;

    return copy;
}

void
clawt_turn_step_free(ClawtTurnStep *self)
{
    if (self == NULL)
        return;

    g_free(self->agent_id);
    g_free(self->room_id);
    g_free(self->text);
    g_free(self->tool_name);
    g_free(self->detail);
    g_free(self);
}

#define GETTER(name, field, type, fallback)          \
    type                                             \
    clawt_turn_step_get_##name(ClawtTurnStep *self)  \
    {                                                \
        g_return_val_if_fail(self != NULL, fallback);\
        return self->field;                          \
    }

GETTER(kind, kind, ClawtStepKind, CLAWT_STEP_STATUS)
GETTER(agent_id, agent_id, const gchar *, NULL)
GETTER(room_id, room_id, const gchar *, NULL)
GETTER(text, text, const gchar *, NULL)
GETTER(tool_name, tool_name, const gchar *, NULL)
GETTER(detail, detail, const gchar *, NULL)
GETTER(failed, failed, gboolean, FALSE)
GETTER(timestamp, timestamp, gint64, 0)

#undef GETTER

const gchar *
clawt_turn_step_tone(ClawtTurnStep *self)
{
    g_return_val_if_fail(self != NULL, "neutral");

    if (self->kind == CLAWT_STEP_TOOL && self->failed)
        return "bad";

    /*
     * No `default:`, so a kind added to the enum is a -Wswitch warning
     * here rather than one more thing quietly drawn in the neutral
     * colour -- which is the failure mode that reports itself to
     * nobody, because a wrong colour reads as a design decision.
     */
    switch (self->kind) {
    case CLAWT_STEP_TEXT:
        return "neutral";
    case CLAWT_STEP_THINKING:
        return "info";
    case CLAWT_STEP_TOOL:
        return "good";
    case CLAWT_STEP_STATUS:
        return "warn";
    }

    return "neutral";
}

gboolean
clawt_turn_step_joins_run(ClawtTurnStep *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->kind == CLAWT_STEP_TOOL;
}

gboolean
clawt_turn_step_precedes(ClawtTurnStep *self, gint64 message_ts)
{
    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * A step with no time at all -- one from a daemon older than the
     * ts member -- sorts first.  That is the harmless end to be wrong
     * at: it lands at the top of the conversation rather than being
     * interleaved into the middle of somebody else's turn.
     */
    if (self->timestamp <= 0)
        return TRUE;

    return (self->timestamp / G_USEC_PER_SEC) <= message_ts;
}

gboolean
clawt_turn_step_is_call(ClawtTurnStep *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->kind == CLAWT_STEP_TOOL && !self->failed;
}

guint
clawt_turn_step_run_extent(GPtrArray *steps,
                           guint      from,
                           guint      end,
                           guint     *out_calls,
                           guint     *out_failed)
{
    guint calls = 0;
    guint failed = 0;
    guint i;

    g_return_val_if_fail(steps != NULL, from);

    for (i = from; i < end; i++) {
        ClawtTurnStep *step = g_ptr_array_index(steps, i);

        if (!clawt_turn_step_joins_run(step))
            break;

        if (clawt_turn_step_get_failed(step))
            failed++;
        else
            calls++;
    }

    if (out_calls != NULL)
        *out_calls = calls;
    if (out_failed != NULL)
        *out_failed = failed;

    return i;
}

gchar *
clawt_turn_step_run_label(guint tools, guint failed)
{
    g_autofree gchar *head = NULL;

    head = g_strdup_printf("Ran %u command%s", tools, tools == 1 ? "" : "s");

    if (failed == 0)
        return g_steal_pointer(&head);

    return g_strdup_printf("%s (%u failed)", head, failed);
}

gchar *
clawt_turn_step_summary(ClawtTurnStep *self)
{
    g_return_val_if_fail(self != NULL, g_strdup(""));

    if (self->kind == CLAWT_STEP_TOOL) {
        const gchar *name = (self->tool_name != NULL && self->tool_name[0] != '\0')
            ? self->tool_name : "a tool";

        if (self->detail != NULL && self->detail[0] != '\0')
            return g_strdup_printf("%s: %s", name, self->detail);

        return g_strdup(name);
    }

    if (self->text == NULL)
        return g_strdup("");

    /*
     * The first line only.  A summary is drawn where there is room for
     * one line, and a paragraph rendered into that slot pushes every
     * other row off the screen rather than being clipped.
     */
    {
        const gchar *newline = strchr(self->text, '\n');

        if (newline != NULL)
            return g_strndup(self->text, (gsize)(newline - self->text));
    }

    return g_strdup(self->text);
}
