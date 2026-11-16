/*
 * clawt-task.c - A unit of delegated work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-task.h"

struct _ClawtTask {
    gint ref_count;

    gchar *id;
    gchar *origin;
    gchar *assignee;
    gchar *prompt;
    gchar *result;
    gchar *room;
    gchar *parent_id;
    gchar *reason;
    gchar *model;
    gchar *session_key;

    /*
     * What the task is called in a list.  Given by whoever created it,
     * or derived from the prompt's first line when they did not: a list
     * of prompts is unreadable, and a prompt is often a page long.
     */
    gchar *title;

    /*
     * The room the work itself happens in, when the delegator asked for
     * `thread: new` -- a session of its own.  Distinct from `room`,
     * which is where the delegation was *made* and where the settle
     * notice goes back to.
     */
    gchar *thread_room;

    /* `mode: consult`: the assignee's turn is offered no tool that acts. */
    gboolean consult;
    gchar *progress_note;
    gchar *cancelled_by;
    gchar *retried_by;
    guint  retries;

    /*
     * Everyone who has owned it, oldest first, with the current
     * assignee always last.  One array rather than a "previous owner"
     * field: a task can be handed on twice, and the second handoff would
     * have overwritten the record of the first.
     */
    GPtrArray *owners;

    /*
     * Two facts about how a task ends, kept apart because they answer
     * different questions.
     *
     * holds_completion is the assignee saying "my turn is over and the
     * work is not", set by clawt_task_manager_note_progress() and spent
     * by the next clawt_task_manager_complete_on_turn_end().  One-shot,
     * because a hold that persisted would mean the task could never
     * finish by inference again -- and the assignee is the one who knows,
     * so it says so each turn it is still going.
     *
     * result_inferred records that nothing ever called
     * clawtilla_task_complete: the result is whatever the assignee
     * happened to write as its turn ended.  A delegator cannot otherwise
     * tell "they said it was done" from "they stopped talking", and the
     * two need different follow-ups.
     */
    gboolean holds_completion;
    gboolean result_inferred;

    ClawtTaskState state;
    gint           depth;
    gint64         created_at;
    gint64         started_at;
    gint64         finished_at;
};

static ClawtTask *
clawt_task_ref(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtTask, clawt_task, clawt_task_ref, clawt_task_free)

ClawtTask *
clawt_task_new(const gchar *origin_agent,
               const gchar *assignee,
               const gchar *prompt)
{
    ClawtTask *self;

    g_return_val_if_fail(prompt != NULL, NULL);

    self = g_new0(ClawtTask, 1);
    self->ref_count = 1;
    self->id = clawt_generate_id("task");
    self->origin = g_strdup(origin_agent);
    self->assignee = g_strdup(assignee);
    self->prompt = g_strdup(prompt);
    self->state = CLAWT_TASK_PENDING;
    self->created_at = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * Seeded with the first assignee rather than left empty, so the
     * history and clawt_task_get_assignee() can never disagree about
     * who owns it now: the answer is always the last entry.
     */
    self->owners = g_ptr_array_new_with_free_func(g_free);

    if (assignee != NULL)
        g_ptr_array_add(self->owners, g_strdup(assignee));

    /*
     * What a session key *would* be if a task had a session of its own.
     *
     * It does not.  Nothing outside a test has ever read this: libreclaw
     * keys a session on channel, room and sender, and clawtilla delivers
     * a task in the room the delegator and the assignee already share --
     * so every task on an agent runs in that one session, and this
     * string reaches nobody.  Kept because it is what the field would
     * have to be, and removing a public getter is a break; the comment
     * that used to sit here asserted the isolation as fact and was the
     * only reason anybody believed it.
     *
     * A task does get one when it asks: `thread: new` on
     * clawtilla_delegate routes the work into a room named for it, and
     * that is clawt_task_get_thread_room(), not this.  It is opt-in for
     * the reason this comment used to give for not doing it: it moves
     * the work out of the transcript somebody is reading.
     */
    self->session_key = g_strdup_printf("clawtilla-task-%s", self->id);

    return self;
}

ClawtTask *
clawt_task_copy(ClawtTask *self)
{
    ClawtTask *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtTask, 1);
    copy->ref_count = 1;
    copy->id = g_strdup(self->id);
    copy->origin = g_strdup(self->origin);
    copy->assignee = g_strdup(self->assignee);
    copy->prompt = g_strdup(self->prompt);
    copy->result = g_strdup(self->result);
    copy->room = g_strdup(self->room);
    copy->parent_id = g_strdup(self->parent_id);
    copy->reason = g_strdup(self->reason);
    copy->model = g_strdup(self->model);
    copy->thread_room = g_strdup(self->thread_room);
    copy->consult = self->consult;
    copy->session_key = g_strdup(self->session_key);
    copy->title = g_strdup(self->title);
    copy->progress_note = g_strdup(self->progress_note);
    copy->cancelled_by = g_strdup(self->cancelled_by);
    copy->retried_by = g_strdup(self->retried_by);
    copy->retries = self->retries;

    /*
     * Deep, because g_ptr_array_copy() carries the source's element-free
     * func and the copy would then own every string twice.
     */
    copy->owners = g_ptr_array_new_with_free_func(g_free);

    {
        guint i;

        for (i = 0; self->owners != NULL && i < self->owners->len; i++)
            g_ptr_array_add(copy->owners,
                            g_strdup(g_ptr_array_index(self->owners, i)));
    }

    copy->holds_completion = self->holds_completion;
    copy->result_inferred = self->result_inferred;
    copy->state = self->state;
    copy->depth = self->depth;
    copy->created_at = self->created_at;
    copy->started_at = self->started_at;
    copy->finished_at = self->finished_at;

    return copy;
}

void
clawt_task_free(ClawtTask *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->id);
    g_free(self->origin);
    g_free(self->assignee);
    g_free(self->prompt);
    g_free(self->result);
    g_free(self->room);
    g_free(self->parent_id);
    g_free(self->reason);
    g_free(self->model);
    g_free(self->session_key);
    g_free(self->title);
    g_free(self->thread_room);
    g_free(self->progress_note);
    g_free(self->cancelled_by);
    g_free(self->retried_by);
    g_clear_pointer(&self->owners, g_ptr_array_unref);
    g_free(self);
}

#define GETTER(name, field)                              \
    const gchar *                                        \
    clawt_task_get_##name(ClawtTask *self)               \
    {                                                    \
        g_return_val_if_fail(self != NULL, NULL);        \
        return self->field;                              \
    }

GETTER(id, id)
GETTER(origin, origin)
GETTER(assignee, assignee)
GETTER(prompt, prompt)
GETTER(result, result)
GETTER(room, room)
GETTER(parent_id, parent_id)
GETTER(reason, reason)
GETTER(model, model)
GETTER(session_key, session_key)
GETTER(thread_room, thread_room)
GETTER(progress_note, progress_note)
GETTER(cancelled_by, cancelled_by)
GETTER(retried_by, retried_by)

#undef GETTER

guint
clawt_task_get_retries(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->retries;
}

void
clawt_task_reopen(ClawtTask *self, const gchar *who)
{
    g_return_if_fail(self != NULL);

    /*
     * Everything that described the attempt that ended goes, because
     * each of it would otherwise describe the next one: a stale result
     * read as the new answer, a stale finish time making "how long did
     * this take" negative, and a stale completion hold swallowing the
     * new attempt's first real ending.  The reason goes too -- the
     * incident keeps what the last attempt said.
     */
    g_clear_pointer(&self->result, g_free);
    g_clear_pointer(&self->reason, g_free);
    g_clear_pointer(&self->progress_note, g_free);
    g_clear_pointer(&self->cancelled_by, g_free);
    self->holds_completion = FALSE;
    self->result_inferred = FALSE;
    self->finished_at = 0;
    self->state = CLAWT_TASK_PENDING;

    g_free(self->retried_by);
    self->retried_by = g_strdup(who);
    self->retries++;
}

#define SETTER(name, field)                              \
    void                                                 \
    clawt_task_set_##name(ClawtTask *self,               \
                          const gchar *value)            \
    {                                                    \
        g_return_if_fail(self != NULL);                  \
        g_free(self->field);                             \
        self->field = g_strdup(value);                   \
    }

SETTER(room, room)
SETTER(parent_id, parent_id)
SETTER(reason, reason)
SETTER(model, model)
SETTER(result, result)
SETTER(progress_note, progress_note)
SETTER(cancelled_by, cancelled_by)

#undef SETTER

gchar *
clawt_task_assignment_guidance(const gchar *task_id)
{
    g_return_val_if_fail(task_id != NULL, NULL);

    /*
     * One spelling, because delegation and handoff both deliver work
     * and the two texts had every chance to drift into two contracts.
     * It names the tools rather than describing behaviours -- the
     * chief-of-staff that inspired it told its assignee "send me ONE
     * message at the end", and the assignee satisfied that by ending
     * its turn, which under the old lifecycle reached nobody.
     */
    return g_strdup_printf(
        "\n\n[clawtilla] This is task %s. When the work is done, just "
        "finish your turn -- your final message becomes the result and "
        "the delegator is notified automatically -- or call "
        "clawtilla_task_complete to say so explicitly. If you are "
        "stopping with work still open, call clawtilla_task_progress. "
        "Do not send status updates as messages: progress goes on the "
        "task, where it can be read without costing anybody a turn.",
        task_id);
}

void
clawt_task_hold_completion(ClawtTask *self)
{
    g_return_if_fail(self != NULL);

    self->holds_completion = TRUE;
}

gboolean
clawt_task_take_completion_hold(ClawtTask *self)
{
    gboolean held;

    g_return_val_if_fail(self != NULL, FALSE);

    held = self->holds_completion;
    self->holds_completion = FALSE;

    return held;
}

gboolean
clawt_task_get_holds_completion(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->holds_completion;
}

gboolean
clawt_task_get_result_inferred(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->result_inferred;
}

void
clawt_task_set_result_inferred(ClawtTask *self, gboolean inferred)
{
    g_return_if_fail(self != NULL);

    self->result_inferred = inferred;
}

ClawtTaskState
clawt_task_get_state(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_TASK_FAILED);
    return self->state;
}

gint
clawt_task_get_depth(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->depth;
}

gint64
clawt_task_get_created_at(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->created_at;
}

gint64
clawt_task_get_finished_at(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->finished_at;
}

gint64
clawt_task_get_started_at(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, 0);
    return self->started_at;
}

/*
 * The longest a title may be, in bytes, cut on a character boundary.
 * Enough for a sentence; a list row that wraps is a list nobody scans.
 */
#define CLAWT_TASK_TITLE_MAX (120)

gchar *
clawt_task_title_from_prompt(const gchar *prompt)
{
    const gchar *start;
    const gchar *end;
    g_autofree gchar *line = NULL;

    if (prompt == NULL)
        return g_strdup("");

    /*
     * The first line that says anything.  A prompt that opens with a
     * blank line or a lone heading marker would otherwise be titled with
     * nothing, which reads as a task with no work in it.
     */
    start = prompt;

    while (*start != '\0') {
        const gchar *p = start;

        while (*p == ' ' || *p == '\t' || *p == '#' || *p == '*' ||
               *p == '-' || *p == '>')
            p++;

        end = strchr(p, '\n');

        if (end == NULL)
            end = p + strlen(p);

        if (end > p) {
            line = g_strndup(p, (gsize)(end - p));
            break;
        }

        if (*end == '\0')
            break;

        start = end + 1;
    }

    if (line == NULL)
        return g_strdup("");

    g_strstrip(line);

    return clawt_utf8_truncate(line, CLAWT_TASK_TITLE_MAX, FALSE);
}

const gchar *
clawt_task_get_title(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    /*
     * Derived on first read rather than at creation, so a task built
     * from JSON written before titles existed gets one too.
     */
    if (self->title == NULL)
        self->title = clawt_task_title_from_prompt(self->prompt);

    return self->title;
}

void
clawt_task_set_title(ClawtTask *self, const gchar *title)
{
    g_return_if_fail(self != NULL);

    g_free(self->title);
    self->title = (title != NULL && *title != '\0')
        ? clawt_utf8_truncate(title, CLAWT_TASK_TITLE_MAX, FALSE) : NULL;
}

void
clawt_task_set_state(ClawtTask *self, ClawtTaskState state)
{
    g_return_if_fail(self != NULL);

    self->state = state;

    /*
     * When somebody first picked it up.  Once: a retry is the same task
     * on its second attempt, and the first start is when it began.
     */
    if (state == CLAWT_TASK_RUNNING && self->started_at == 0)
        self->started_at = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * Stamped once, when the task reaches a state it will not leave.  A
     * finish time that moved would make "how long did this take" unanswerable.
     */
    if (clawt_task_is_finished(self) && self->finished_at == 0)
        self->finished_at = g_get_real_time() / G_USEC_PER_SEC;
}

void
clawt_task_set_depth(ClawtTask *self, gint depth)
{
    g_return_if_fail(self != NULL);
    self->depth = depth;
}

GPtrArray *
clawt_task_get_owner_history(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->owners;
}

gboolean
clawt_task_transfer_owner(ClawtTask *self, const gchar *new_owner)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(new_owner != NULL, FALSE);

    /*
     * Handing a task to whoever already has it is not a move.  Recorded
     * as one it would read as a round trip that never happened, and a
     * chief reading its own history would conclude the work had bounced.
     */
    if (g_strcmp0(self->assignee, new_owner) == 0)
        return FALSE;

    g_free(self->assignee);
    self->assignee = g_strdup(new_owner);

    g_ptr_array_add(self->owners, g_strdup(new_owner));

    return TRUE;
}

gboolean
clawt_task_is_finished(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, TRUE);

    return clawt_task_state_is_finished(self->state);
}

const gchar *
clawt_task_state_tone(ClawtTaskState state)
{
    /*
     * No `default:`, deliberately. -Wswitch then names a state nobody
     * classified -- a warning rather than an error, since this tree does
     * not build with -Werror, so the zero-warning rule is what makes it
     * stop somebody; tests/test-task.c is what actually goes red.
     *
     * Worth the belt and the braces, because the string comparison this
     * replaces named two nicknames the enum has never had, and both
     * clients rendered the result without complaint.
     */
    switch (state) {
    case CLAWT_TASK_COMPLETED:
        return "good";

    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_CANCELLED:
        return "bad";

    case CLAWT_TASK_STALLED:
        return "warn";

    case CLAWT_TASK_RUNNING:
    case CLAWT_TASK_REVIEW:
        return "info";

    case CLAWT_TASK_BLOCKED:
        return "warn";

    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_UNASSIGNED:
        return "neutral";
    }

    /*
     * Only reachable from an integer cast to the type, which is a
     * programming error rather than a state.
     */
    g_return_val_if_reached("neutral");
}

void
clawt_task_set_thread_room(ClawtTask *self, const gchar *room)
{
    g_return_if_fail(self != NULL);

    g_free(self->thread_room);
    self->thread_room = (room != NULL && *room != '\0') ? g_strdup(room) : NULL;
}

gboolean
clawt_task_get_consult(ClawtTask *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->consult;
}

void
clawt_task_set_consult(ClawtTask *self, gboolean consult)
{
    g_return_if_fail(self != NULL);

    self->consult = consult;
}

gchar *
clawt_task_thread_summary(const gchar *thread_room)
{
    if (thread_room == NULL || *thread_room == '\0')
        return NULL;

    return g_strdup_printf("runs in its own room, %s", thread_room);
}

const gchar *
clawt_task_consult_label(void)
{
    /* The chat says the same word for the same thing. */
    return clawt_message_consult_label(CLAWT_CONSULT_ASKED);
}

gboolean
clawt_task_state_is_retryable(ClawtTaskState state)
{
    /* Every state named, no default: -Wswitch catches the next one. */
    switch (state) {
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_STALLED:
        return TRUE;

    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
    case CLAWT_TASK_COMPLETED:
    case CLAWT_TASK_CANCELLED:
    case CLAWT_TASK_UNASSIGNED:
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_REVIEW:
        return FALSE;
    }

    return FALSE;
}

gboolean
clawt_task_state_is_finished(ClawtTaskState state)
{
    /* Every state named, no default: -Wswitch catches the next one. */
    switch (state) {
    case CLAWT_TASK_COMPLETED:
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_CANCELLED:
    case CLAWT_TASK_STALLED:
        return TRUE;

    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
    case CLAWT_TASK_UNASSIGNED:
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_REVIEW:
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_task_state_is_active(ClawtTaskState state)
{
    /*
     * Handed to its assignee and theirs to move: the states a turn can
     * be working on.  Not the three that wait on somebody else -- a
     * backlog item, a task held for its blockers, a result being
     * checked -- which no turn timeout, crash or idle continuation is
     * about.
     */
    switch (state) {
    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
        return TRUE;

    case CLAWT_TASK_COMPLETED:
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_CANCELLED:
    case CLAWT_TASK_STALLED:
    case CLAWT_TASK_UNASSIGNED:
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_REVIEW:
        return FALSE;
    }

    return FALSE;
}

gboolean
clawt_task_state_is_cancellable(ClawtTaskState state)
{
    /* Anything that has not ended: there is still something to stop. */
    return !clawt_task_state_is_finished(state);
}

gboolean
clawt_task_state_nick_is_cancellable(const gchar *nick)
{
    gint value;

    if (nick == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, nick, &value))
        return FALSE;

    return clawt_task_state_is_cancellable((ClawtTaskState)value);
}

/* ── Saving and restoring ────────────────────────────────────────── */

/*
 * Every field, by name.  The member names are the store's on-disk
 * format, so they are spelled once here and never renamed: a field
 * added later is a new member, and a task saved before it existed reads
 * it as absent -- the zero value, which every field is written to mean
 * "never set".
 */
static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    if (value == NULL)
        return;

    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, value);
}

static void
add_int(JsonBuilder *builder, const gchar *name, gint64 value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_int_value(builder, value);
}

static void
add_bool(JsonBuilder *builder, const gchar *name, gboolean value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_boolean_value(builder, value);
}

JsonNode *
clawt_task_to_json(ClawtTask *self)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    json_builder_begin_object(builder);

    add_string(builder, "id", self->id);
    add_string(builder, "origin", self->origin);
    add_string(builder, "assignee", self->assignee);
    add_string(builder, "prompt", self->prompt);
    add_string(builder, "result", self->result);
    add_string(builder, "room", self->room);
    add_string(builder, "parent_id", self->parent_id);
    add_string(builder, "reason", self->reason);
    add_string(builder, "model", self->model);
    add_string(builder, "session_key", self->session_key);
    add_string(builder, "title", self->title);
    add_string(builder, "thread_room", self->thread_room);
    add_string(builder, "progress_note", self->progress_note);
    add_string(builder, "cancelled_by", self->cancelled_by);
    add_string(builder, "retried_by", self->retried_by);

    add_bool(builder, "consult", self->consult);
    add_bool(builder, "holds_completion", self->holds_completion);
    add_bool(builder, "result_inferred", self->result_inferred);

    add_int(builder, "retries", self->retries);
    add_int(builder, "depth", self->depth);
    add_int(builder, "created_at", self->created_at);
    add_int(builder, "started_at", self->started_at);
    add_int(builder, "finished_at", self->finished_at);

    /* By nickname, so the file survives the enum being reordered. */
    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(builder,
        clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, self->state));

    json_builder_set_member_name(builder, "owners");
    json_builder_begin_array(builder);

    for (i = 0; self->owners != NULL && i < self->owners->len; i++)
        json_builder_add_string_value(builder,
                                      g_ptr_array_index(self->owners, i));

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static gchar *
dup_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return g_strdup(json_node_get_string(node));
}

static gint64
int_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return 0;

    return json_node_get_int(node);
}

static gboolean
bool_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return FALSE;

    return json_node_get_boolean(node);
}

ClawtTask *
clawt_task_new_from_json(JsonNode *node, GError **error)
{
    JsonObject *object;
    ClawtTask *self;
    g_autofree gchar *state = NULL;
    gint state_value = CLAWT_TASK_FAILED;
    JsonArray *owners;
    guint i;

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "a saved task is not a JSON object");
        return NULL;
    }

    object = json_node_get_object(node);

    /*
     * The two fields without which there is no task: a row with no id
     * cannot be found again, and one with no prompt says nothing about
     * what was asked.  Refused rather than repaired, so the caller can
     * name the row and skip it.
     */
    if (!json_object_has_member(object, "id") ||
        !json_object_has_member(object, "prompt")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "a saved task has no id or no prompt");
        return NULL;
    }

    self = g_new0(ClawtTask, 1);
    self->ref_count = 1;

    self->id = dup_member(object, "id");
    self->origin = dup_member(object, "origin");
    self->assignee = dup_member(object, "assignee");
    self->prompt = dup_member(object, "prompt");
    self->result = dup_member(object, "result");
    self->room = dup_member(object, "room");
    self->parent_id = dup_member(object, "parent_id");
    self->reason = dup_member(object, "reason");
    self->model = dup_member(object, "model");
    self->session_key = dup_member(object, "session_key");
    self->title = dup_member(object, "title");
    self->thread_room = dup_member(object, "thread_room");
    self->progress_note = dup_member(object, "progress_note");
    self->cancelled_by = dup_member(object, "cancelled_by");
    self->retried_by = dup_member(object, "retried_by");

    self->consult = bool_member(object, "consult");
    self->holds_completion = bool_member(object, "holds_completion");
    self->result_inferred = bool_member(object, "result_inferred");

    self->retries = (guint)MAX(int_member(object, "retries"), 0);
    self->depth = (gint)int_member(object, "depth");
    self->created_at = int_member(object, "created_at");
    self->started_at = int_member(object, "started_at");
    self->finished_at = int_member(object, "finished_at");

    /*
     * A state this build does not know is read as failed, said once:
     * a task restored as "pending" from a nickname nobody recognises
     * would be work that runs again.  Failed is the state a person can
     * look at and retry.
     */
    state = dup_member(object, "state");

    if (state == NULL ||
        !clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &state_value)) {
        g_warning("task %s: saved state '%s' is not one this build knows; "
                  "reading it as failed", self->id,
                  state != NULL ? state : "(none)");
        state_value = CLAWT_TASK_FAILED;
    }

    self->state = (ClawtTaskState)state_value;

    if (self->session_key == NULL)
        self->session_key = g_strdup_printf("clawtilla-task-%s", self->id);

    self->owners = g_ptr_array_new_with_free_func(g_free);
    owners = json_object_has_member(object, "owners")
        ? json_object_get_array_member(object, "owners") : NULL;

    for (i = 0; owners != NULL && i < json_array_get_length(owners); i++) {
        const gchar *owner = json_array_get_string_element(owners, i);

        if (owner != NULL)
            g_ptr_array_add(self->owners, g_strdup(owner));
    }

    /* The history's last entry is the assignee, by the type's own rule. */
    if (self->assignee != NULL &&
        (self->owners->len == 0 ||
         g_strcmp0(g_ptr_array_index(self->owners, self->owners->len - 1),
                   self->assignee) != 0))
        g_ptr_array_add(self->owners, g_strdup(self->assignee));

    return self;
}
