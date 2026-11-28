/*
 * clawt-board.c - The agent board: one agent's work, column by column
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * See clawt-board.h for the three layers and docs/tasks.org, "The agent
 * board", for the rules in words.  The rules live in
 * clawt_board_classify() and nowhere else: the builder gathers facts and
 * writes JSON, the renderer draws JSON, and neither decides a column.
 */

#include "clawtilla.h"
#include "task/clawt-board.h"

#include <string.h>

/* How long a note is on a text card before it is cut. */
#define BOARD_NOTE_BYTES (110)

/* How long a title is on a text card or chip before it is cut. */
#define BOARD_TITLE_BYTES (72)

/* The most completed tasks a window longer than a day reads back. */
#define BOARD_HISTORY_LIMIT (1000)

/* The longest window clawt_board_parse_window() accepts, in seconds. */
#define BOARD_WINDOW_MAX (90 * 86400)

/* ── Small pieces ──────────────────────────────────────────────────── */

ClawtAgentActivity
clawt_board_agent_activity(gboolean turn_running,
                           guint    waits_pending)
{
    /*
     * A turn first: whatever it has pending, an agent in a turn is doing
     * something now, and a wait it set is a thing it will hear about
     * later.  Then a wait, which is the agent's own word that it is not
     * idle -- a build it started in its computer, a deploy, a task it
     * handed on -- and the reason it went quiet.  Only with neither is it
     * idle.  No other input: the run state is reported beside this, not
     * folded into it (see ClawtAgentActivity).
     */
    if (turn_running)
        return CLAWT_AGENT_ACTIVITY_WORKING;

    if (waits_pending > 0)
        return CLAWT_AGENT_ACTIVITY_WAITING;

    return CLAWT_AGENT_ACTIVITY_IDLE;
}

const gchar *
clawt_board_agent_activity_nick(ClawtAgentActivity activity)
{
    return clawt_enum_to_nick(CLAWT_TYPE_AGENT_ACTIVITY, activity);
}

gchar *
clawt_board_waiting_label(const gchar *label,
                          gint64       since,
                          gint64       now)
{
    g_autofree gchar *age = NULL;
    const gchar *what = (label != NULL && *label != '\0')
        ? label : "something in the background";

    if (since <= 0)
        return g_strdup_printf("waiting: %s", what);

    age = clawt_board_duration_label(now - since);

    return g_strdup_printf("waiting: %s (%s)", what, age);
}

const gchar *
clawt_board_column_nick(ClawtBoardColumn column)
{
    return clawt_enum_to_nick(CLAWT_TYPE_BOARD_COLUMN, column);
}

const gchar *
clawt_board_column_label(ClawtBoardColumn column)
{
    /*
     * Every value is named and there is no default, so a column added to
     * the enum is a -Wswitch warning here rather than a heading nobody
     * wrote.
     */
    switch (column) {
    case CLAWT_BOARD_HIDDEN:    return "Not on the board";
    case CLAWT_BOARD_BACKLOG:   return "Backlog";
    case CLAWT_BOARD_PLANNING:  return "Planning";
    case CLAWT_BOARD_DELEGATED: return "Delegated";
    case CLAWT_BOARD_REVIEW:    return "In review";
    case CLAWT_BOARD_GATING:    return "Gating";
    case CLAWT_BOARD_DONE:      return "Done";
    case CLAWT_BOARD_BLOCKED:   return "Blocked";
    }

    return "Unknown";
}

const gchar *
clawt_board_block_nick(ClawtBoardBlocks block)
{
    switch (block) {
    case CLAWT_BOARD_BLOCK_FAILED:        return "failed";
    case CLAWT_BOARD_BLOCK_STALLED:       return "stalled";
    case CLAWT_BOARD_BLOCK_CHILD_FAILED:  return "child_failed";
    case CLAWT_BOARD_BLOCK_CHILD_STALLED: return "child_stalled";
    case CLAWT_BOARD_BLOCK_DECISION:      return "decision";
    case CLAWT_BOARD_BLOCK_WAITING:       return "waiting";
    case CLAWT_BOARD_BLOCK_QUIET:         return "quiet";
    case CLAWT_BOARD_BLOCK_PHASE:         return "phase";
    case CLAWT_BOARD_BLOCK_NONE:          return NULL;
    default:                              return NULL;
    }
}

gchar *
clawt_board_duration_label(gint64 seconds)
{
    if (seconds < 0)
        seconds = 0;

    if (seconds < 60)
        return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);

    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m", seconds / 60);

    if (seconds < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h%02" G_GINT64_FORMAT "m",
                               seconds / 3600, (seconds % 3600) / 60);

    return g_strdup_printf("%" G_GINT64_FORMAT "d%02" G_GINT64_FORMAT "h",
                           seconds / 86400, (seconds % 86400) / 3600);
}

gboolean
clawt_board_parse_window(const gchar  *spec,
                         gint64        now,
                         gint64       *since,
                         GError      **error)
{
    gchar *end = NULL;
    guint64 count;
    gint64 span;

    g_return_val_if_fail(since != NULL, FALSE);

    /*
     * Today, the default: since local midnight, which is what "done
     * today" means to the person reading it -- not the last 24 hours,
     * which at nine in the morning is mostly yesterday.
     */
    if (spec == NULL || *spec == '\0' || g_strcmp0(spec, "today") == 0) {
        g_autoptr(GDateTime) here = g_date_time_new_from_unix_local(now);
        g_autoptr(GDateTime) midnight = NULL;

        if (here == NULL) {
            *since = now - 86400;
            return TRUE;
        }

        midnight = g_date_time_new_local(g_date_time_get_year(here),
                                         g_date_time_get_month(here),
                                         g_date_time_get_day_of_month(here),
                                         0, 0, 0.0);
        *since = midnight != NULL ? g_date_time_to_unix(midnight)
                                  : now - 86400;
        return TRUE;
    }

    count = g_ascii_strtoull(spec, &end, 10);

    if (end == spec || count == 0 || end == NULL ||
        (g_strcmp0(end, "h") != 0 && g_strcmp0(end, "d") != 0)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "window must be today, <N>h or <N>d, not '%s'", spec);
        return FALSE;
    }

    if (count > (guint64)BOARD_WINDOW_MAX) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "window '%s' is longer than 90 days", spec);
        return FALSE;
    }

    span = (gint64)count * (g_strcmp0(end, "h") == 0 ? 3600 : 86400);

    if (span > BOARD_WINDOW_MAX) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "window '%s' is longer than 90 days", spec);
        return FALSE;
    }

    *since = now - span;
    return TRUE;
}

/* ── Classification: the rules ─────────────────────────────────────── */

/*
 * Whether the card's assignee has said anything since @child finished:
 * a progress note or a phase report after a failure is the agent having
 * seen it and gone on, so the failure no longer pins the card.  Without
 * this a fan-out that replaced one failed child with a fresh delegation
 * would stay blocked until the day dropped the failure out of memory.
 */
static gboolean
child_acknowledged(ClawtTask *card, ClawtTask *child)
{
    gint64 finished = clawt_task_get_finished_at(child);
    gint64 said = MAX(clawt_task_get_progress_at(card),
                      clawt_task_get_phase_at(card));

    return finished > 0 && said > finished;
}

gboolean
clawt_board_is_quiet(ClawtTask *task,
                     GPtrArray *children,
                     gint64     now,
                     guint      quiet_minutes,
                     gboolean   wait_pending,
                     gint64    *quiet_at)
{
    gint64 last;
    gint64 when;
    guint i;

    if (quiet_at != NULL)
        *quiet_at = 0;

    g_return_val_if_fail(task != NULL, FALSE);

    /*
     * Only work in hand can go quiet.  A pending or held task has not
     * been picked up, one in review is somebody else's to answer, and a
     * wait the assignee set is the assignee saying why it will be silent.
     */
    if (quiet_minutes == 0 ||
        clawt_task_get_state(task) != CLAWT_TASK_RUNNING || wait_pending)
        return FALSE;

    last = clawt_task_get_last_sign_of_life(task);

    for (i = 0; children != NULL && i < children->len; i++) {
        ClawtTask *child = g_ptr_array_index(children, i);
        ClawtTaskState state = clawt_task_get_state(child);

        if (clawt_task_state_is_finished(state))
            continue;

        /*
         * A child queued with its worker, held for its own blockers or
         * waiting for a verdict is waiting on somebody, and so is the
         * card: that is not silence.
         */
        if (state != CLAWT_TASK_RUNNING)
            return FALSE;

        last = MAX(last, clawt_task_get_last_sign_of_life(child));
    }

    when = last + (gint64)quiet_minutes * 60;

    if (quiet_at != NULL)
        *quiet_at = when;

    return now >= when;
}

ClawtBoardColumn
clawt_board_classify(const ClawtBoardFacts *facts,
                     ClawtBoardBlocks      *blocks)
{
    ClawtTask *task;
    ClawtTaskState state;
    ClawtTaskPhase phase;
    ClawtBoardBlocks why = CLAWT_BOARD_BLOCK_NONE;
    gboolean review_for_owner = FALSE;
    gboolean live_children = FALSE;
    guint i;

    if (blocks != NULL)
        *blocks = CLAWT_BOARD_BLOCK_NONE;

    g_return_val_if_fail(facts != NULL, CLAWT_BOARD_HIDDEN);

    task = facts->task;

    if (task == NULL)
        return CLAWT_BOARD_HIDDEN;

    state = clawt_task_get_state(task);

    /* 1. Not this board's. */
    if (facts->owner != NULL &&
        g_strcmp0(clawt_task_get_assignee(task), facts->owner) != 0)
        return CLAWT_BOARD_HIDDEN;

    /* 1 and 2. Ended: shown only inside the window, and how it ended. */
    switch (state) {
    case CLAWT_TASK_UNASSIGNED:
    case CLAWT_TASK_CANCELLED:
        return CLAWT_BOARD_HIDDEN;

    case CLAWT_TASK_COMPLETED:
        return clawt_task_get_finished_at(task) >= facts->done_since
            ? CLAWT_BOARD_DONE : CLAWT_BOARD_HIDDEN;

    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_STALLED:
        if (clawt_task_get_finished_at(task) < facts->done_since)
            return CLAWT_BOARD_HIDDEN;

        if (blocks != NULL)
            *blocks = (state == CLAWT_TASK_FAILED)
                ? CLAWT_BOARD_BLOCK_FAILED : CLAWT_BOARD_BLOCK_STALLED;
        return CLAWT_BOARD_BLOCKED;

    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_REVIEW:
        break;
    }

    /* What the children say. */
    for (i = 0; facts->children != NULL && i < facts->children->len; i++) {
        ClawtTask *child = g_ptr_array_index(facts->children, i);
        ClawtTaskState child_state = clawt_task_get_state(child);

        if (!clawt_task_state_is_finished(child_state))
            live_children = TRUE;

        if (child_state == CLAWT_TASK_REVIEW &&
            facts->owner != NULL &&
            g_strcmp0(clawt_task_get_reviewer(child), facts->owner) == 0)
            review_for_owner = TRUE;

        if (child_state == CLAWT_TASK_FAILED &&
            !child_acknowledged(task, child))
            why |= CLAWT_BOARD_BLOCK_CHILD_FAILED;

        if (child_state == CLAWT_TASK_STALLED &&
            !child_acknowledged(task, child))
            why |= CLAWT_BOARD_BLOCK_CHILD_STALLED;

        /* A child that waits on the operator is the operator's decision. */
        if (child_state == CLAWT_TASK_BLOCKED &&
            g_strcmp0(clawt_task_get_waiting_on(child), "operator") == 0)
            why |= CLAWT_BOARD_BLOCK_DECISION;
    }

    /* 3. Blocked: everything that needs somebody, pinned first. */
    if (facts->decisions > 0)
        why |= CLAWT_BOARD_BLOCK_DECISION;

    if (state == CLAWT_TASK_BLOCKED &&
        clawt_task_get_waiting_on(task) != NULL)
        why |= CLAWT_BOARD_BLOCK_WAITING;

    phase = clawt_task_get_phase(task);

    if (state == CLAWT_TASK_RUNNING && phase == CLAWT_TASK_PHASE_BLOCKED)
        why |= CLAWT_BOARD_BLOCK_PHASE;

    if (clawt_board_is_quiet(task, facts->children, facts->now,
                             facts->quiet_minutes, facts->wait_pending, NULL))
        why |= CLAWT_BOARD_BLOCK_QUIET;

    if (why != CLAWT_BOARD_BLOCK_NONE) {
        if (blocks != NULL)
            *blocks = why;
        return CLAWT_BOARD_BLOCKED;
    }

    /* 4. Not started, or held for the tasks it waits for. */
    if (state == CLAWT_TASK_PENDING || state == CLAWT_TASK_BLOCKED)
        return CLAWT_BOARD_BACKLOG;

    /* 5. Its own result, waiting for its reviewer. */
    if (state == CLAWT_TASK_REVIEW)
        return CLAWT_BOARD_REVIEW;

    /* 6. Running, and its assignee said where it is. */
    switch (phase) {
    case CLAWT_TASK_PHASE_PLANNING:  return CLAWT_BOARD_PLANNING;
    case CLAWT_TASK_PHASE_DELEGATED: return CLAWT_BOARD_DELEGATED;
    case CLAWT_TASK_PHASE_REVIEWING: return CLAWT_BOARD_REVIEW;
    case CLAWT_TASK_PHASE_GATING:    return CLAWT_BOARD_GATING;
    case CLAWT_TASK_PHASE_LANDING:   return CLAWT_BOARD_GATING;
    case CLAWT_TASK_PHASE_BLOCKED:   return CLAWT_BOARD_BLOCKED;
    case CLAWT_TASK_PHASE_NONE:      break;
    }

    /* 7. Running, inferred from the children. */
    if (review_for_owner)
        return CLAWT_BOARD_REVIEW;

    if (live_children)
        return CLAWT_BOARD_DELEGATED;

    return CLAWT_BOARD_PLANNING;
}

/* ── Where a result says the work landed ───────────────────────────── */

/* A commit: 7-40 hex digits, with a digit and a letter among them. */
static gboolean
looks_like_commit(const gchar *word)
{
    gsize len = strlen(word);
    gboolean digit = FALSE;
    gboolean letter = FALSE;
    gsize i;

    if (len < 7 || len > 40)
        return FALSE;

    for (i = 0; i < len; i++) {
        if (g_ascii_isdigit(word[i]))
            digit = TRUE;
        else if (word[i] >= 'a' && word[i] <= 'f')
            letter = TRUE;
        else
            return FALSE;
    }

    return digit && letter;
}

/*
 * A branch: a conventional prefix, a slash, and something after it.  The
 * prefixes rather than "anything with a slash", which would take every
 * path a result quotes (src/task/...) for a branch.
 */
static gboolean
looks_like_branch(const gchar *word)
{
    static const gchar *const prefixes[] = {
        "feat/", "feature/", "fix/", "bugfix/", "hotfix/", "chore/",
        "docs/", "refactor/", "test/", "perf/", "build/", "ci/", "style/",
        "release/", "epic/", "clawtilla/", NULL
    };
    guint i;

    for (i = 0; prefixes[i] != NULL; i++) {
        if (g_str_has_prefix(word, prefixes[i]) &&
            strlen(word) > strlen(prefixes[i]))
            return TRUE;
    }

    return FALSE;
}

/* A word may be a ref when it is made of what refs are made of. */
static gboolean
is_ref_char(gchar c)
{
    return g_ascii_isalnum(c) || c == '/' || c == '-' || c == '_' ||
           c == '.';
}

gboolean
clawt_board_result_refs(const gchar  *result,
                        gchar       **commit,
                        gchar       **branch)
{
    g_autofree gchar *found_commit = NULL;
    g_autofree gchar *found_branch = NULL;
    g_autofree gchar *previous = NULL;
    const gchar *p;

    if (commit != NULL)
        *commit = NULL;
    if (branch != NULL)
        *branch = NULL;

    if (result == NULL)
        return FALSE;

    /*
     * Word by word, where a word is a run of ref characters: backquotes,
     * brackets and commas around a commit or a branch are not part of it.
     * A trailing full stop is a sentence ending, not the ref's.
     */
    p = result;

    while (*p != '\0' && (found_commit == NULL || found_branch == NULL)) {
        const gchar *start;
        g_autofree gchar *word = NULL;
        gsize len;

        while (*p != '\0' && !is_ref_char(*p))
            p++;

        if (*p == '\0')
            break;

        start = p;

        while (*p != '\0' && is_ref_char(*p))
            p++;

        len = (gsize)(p - start);

        while (len > 0 && start[len - 1] == '.')
            len--;

        if (len == 0)
            continue;

        word = g_strndup(start, len);

        if (found_commit == NULL && looks_like_commit(word))
            found_commit = g_strdup(word);
        else if (found_branch == NULL &&
                 (looks_like_branch(word) ||
                  (previous != NULL &&
                   g_ascii_strcasecmp(previous, "branch") == 0 &&
                   strchr(word, '/') != NULL)))
            found_branch = g_strdup(word);

        g_free(previous);
        previous = g_steal_pointer(&word);
    }

    if (commit != NULL)
        *commit = g_strdup(found_commit);
    if (branch != NULL)
        *branch = g_strdup(found_branch);

    return found_commit != NULL || found_branch != NULL;
}

/* ── The timeline ──────────────────────────────────────────────────── */

static void
add_span(JsonBuilder *builder, const gchar *kind, const gchar *name,
         gint64 from, gint64 to)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, kind);
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "from");
    json_builder_add_int_value(builder, from);

    /* The span still open -- or the state a task ended in -- has no end. */
    if (to > 0) {
        json_builder_set_member_name(builder, "to");
        json_builder_add_int_value(builder, to);
    }

    json_builder_end_object(builder);
}

/*
 * The state spans an older task's own stamps allow: when it was made,
 * when it first ran and when it finished.  Coarse -- a task that was
 * held, reviewed and sent back shows none of that -- but never wrong
 * about what it does show.
 */
static void
add_stamp_spans(JsonBuilder *builder, ClawtTask *task)
{
    ClawtTaskState state = clawt_task_get_state(task);
    const gchar *nick = clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, state);
    gint64 created = clawt_task_get_created_at(task);
    gint64 started = clawt_task_get_started_at(task);
    gint64 finished = clawt_task_get_finished_at(task);

    if (started > 0) {
        if (started > created)
            add_span(builder, "state", "pending", created, started);

        if (finished > 0) {
            if (finished > started)
                add_span(builder, "state", "running", started, finished);
            add_span(builder, "state", nick, finished, 0);
        } else if (state == CLAWT_TASK_RUNNING) {
            add_span(builder, "state", "running", started, 0);
        } else {
            gint64 moved = MAX(clawt_task_get_state_at(task), started);

            if (moved > started)
                add_span(builder, "state", "running", started, moved);
            add_span(builder, "state", nick, moved, 0);
        }

        return;
    }

    if (finished > 0) {
        if (finished > created)
            add_span(builder, "state", "pending", created, finished);
        add_span(builder, "state", nick, finished, 0);
        return;
    }

    add_span(builder, "state", nick, created, 0);
}

JsonNode *
clawt_board_timeline(ClawtTask *task, JsonArray *journal, gint64 now)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gboolean from_journal;
    guint count;
    guint i;

    g_return_val_if_fail(task != NULL, NULL);

    (void)now;

    from_journal = journal != NULL && json_array_get_length(journal) > 0;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "source");
    json_builder_add_string_value(builder, from_journal ? "journal" : "stamps");

    /*
     * States.  The journal has a row for every state change and for
     * events that changed none (a progress note, a phase); rows in the
     * state already open extend its span rather than starting another.
     */
    json_builder_set_member_name(builder, "states");
    json_builder_begin_array(builder);

    if (from_journal) {
        const gchar *open_state = NULL;
        gint64 open_from = 0;

        for (i = 0; i < json_array_get_length(journal); i++) {
            JsonNode *element = json_array_get_element(journal, i);
            JsonObject *row;
            const gchar *state;
            gint64 at;

            if (!JSON_NODE_HOLDS_OBJECT(element))
                continue;

            row = json_node_get_object(element);
            state = json_object_get_string_member_with_default(row, "state",
                                                               NULL);
            at = json_object_get_int_member_with_default(row, "at", 0);

            if (state == NULL || g_strcmp0(state, open_state) == 0)
                continue;

            if (open_state != NULL)
                add_span(builder, "state", open_state, open_from, at);

            open_state = state;
            open_from = at;
        }

        if (open_state != NULL)
            add_span(builder, "state", open_state, open_from, 0);
    } else {
        add_stamp_spans(builder, task);
    }

    json_builder_end_array(builder);

    /*
     * Phases, from the task's own record of them.  A `none` mark is the
     * task going back to pending: it ends the span before it and starts
     * none of its own.  The last phase runs until the task finished, or
     * is still open.
     */
    json_builder_set_member_name(builder, "phases");
    json_builder_begin_array(builder);

    count = clawt_task_get_phase_count(task);

    for (i = 0; i < count; i++) {
        gint64 from = 0;
        gint64 to = 0;
        ClawtTaskPhase phase = clawt_task_get_phase_nth(task, i, &from);

        if (phase == CLAWT_TASK_PHASE_NONE)
            continue;

        if (i + 1 < count)
            (void)clawt_task_get_phase_nth(task, i + 1, &to);
        else if (clawt_task_get_finished_at(task) > 0)
            to = clawt_task_get_finished_at(task);

        add_span(builder, "phase", clawt_task_phase_to_nick(phase), from, to);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── The builder ───────────────────────────────────────────────────── */

void
clawt_board_agent_clear(ClawtBoardAgent *agent)
{
    if (agent == NULL)
        return;

    g_free(agent->name);
    g_free(agent->state);
    g_free(agent->team);
    g_free(agent->step_text);
    g_free(agent->step_tool);
    g_free(agent->turn_task);
    g_free(agent->wait_label);
    memset(agent, 0, sizeof(*agent));
}

/* One card while the board is put together. */
typedef struct {
    ClawtTask        *task;       /* borrowed, or held by the history page */
    GPtrArray        *children;   /* container; tasks borrowed */
    ClawtBoardColumn  column;
    ClawtBoardBlocks  blocks;
    gboolean          quiet;
    gint64            quiet_at;
    guint             decisions;
    guint             position;   /* backlog order, from 1; 0 elsewhere */
} Card;

static void
card_free(gpointer data)
{
    Card *card = data;

    if (card == NULL)
        return;

    g_clear_pointer(&card->children, g_ptr_array_unref);
    g_free(card);
}

/* Everything the builder carries from one card to the next. */
typedef struct {
    ClawtTaskManager        *tasks;
    ClawtTaskStore          *store;
    const ClawtBoardRequest *request;
    const ClawtBoardSource  *source;
    gpointer                 user_data;
    gboolean                 wait_pending;
} Build;

static gboolean
source_describe(const Build *b, const gchar *agent_id, ClawtBoardAgent *out)
{
    memset(out, 0, sizeof(*out));

    if (b->source == NULL || b->source->describe_agent == NULL ||
        agent_id == NULL)
        return FALSE;

    return b->source->describe_agent(agent_id, out, b->user_data);
}

static guint
source_decisions(const Build *b, const gchar *task_id)
{
    if (b->source == NULL || b->source->open_decisions == NULL)
        return 0;

    return b->source->open_decisions(task_id, b->user_data);
}

static gdouble
source_cost(const Build *b, const gchar *task_id)
{
    if (b->source == NULL || b->source->task_cost == NULL)
        return -1.0;

    return b->source->task_cost(task_id, b->user_data);
}

/*
 * A task of the agent's that hangs under another of its own: it is
 * drawn as a chip on that card, not as a card of its own.
 */
static gboolean
is_nested(ClawtTaskManager *tasks, ClawtTask *task, const gchar *agent)
{
    const gchar *parent_id = clawt_task_get_parent_id(task);
    ClawtTask *parent;

    if (parent_id == NULL)
        return FALSE;

    parent = clawt_task_manager_get(tasks, parent_id);

    return parent != NULL &&
           g_strcmp0(clawt_task_get_assignee(parent), agent) == 0;
}

static Card *
card_new(const Build *b, ClawtTask *task)
{
    Card *card = g_new0(Card, 1);
    ClawtBoardFacts facts;
    guint i;

    card->task = task;
    card->children = clawt_task_manager_list_children(
        b->tasks, clawt_task_get_id(task));

    /*
     * Decisions on the card and on what it handed on: a worker's question
     * to the operator holds the card as surely as the agent's own.
     */
    card->decisions = source_decisions(b, clawt_task_get_id(task));
    for (i = 0; i < card->children->len; i++)
        card->decisions += source_decisions(
            b, clawt_task_get_id(g_ptr_array_index(card->children, i)));

    facts.task = task;
    facts.children = card->children;
    facts.owner = b->request->agent;
    facts.now = b->request->now;
    facts.done_since = b->request->done_since;
    facts.quiet_minutes = b->request->quiet_minutes;
    facts.decisions = card->decisions;
    facts.wait_pending = b->wait_pending;

    card->column = clawt_board_classify(&facts, &card->blocks);
    card->quiet = clawt_board_is_quiet(task, card->children, facts.now,
                                       facts.quiet_minutes,
                                       facts.wait_pending, &card->quiet_at);

    return card;
}

static gint64
started_or_created(ClawtTask *task)
{
    gint64 started = clawt_task_get_started_at(task);

    return started > 0 ? started : clawt_task_get_created_at(task);
}

/*
 * The order cards are drawn in within a column.  The backlog in the
 * order the agent will take them -- what it was handed before what is
 * still held for other work, oldest first within each -- done newest
 * first, and the rest by how long they have been going.
 */
static gint
card_compare(gconstpointer a, gconstpointer b)
{
    const Card *x = *(const Card *const *)a;
    const Card *y = *(const Card *const *)b;
    gint64 kx;
    gint64 ky;

    if (x->column != y->column)
        return (gint)x->column - (gint)y->column;

    switch (x->column) {
    case CLAWT_BOARD_BACKLOG: {
        gboolean hx = clawt_task_get_state(x->task) == CLAWT_TASK_BLOCKED;
        gboolean hy = clawt_task_get_state(y->task) == CLAWT_TASK_BLOCKED;

        if (hx != hy)
            return hx ? 1 : -1;

        kx = clawt_task_get_created_at(x->task);
        ky = clawt_task_get_created_at(y->task);
        break;
    }

    case CLAWT_BOARD_DONE:
        kx = -clawt_task_get_finished_at(x->task);
        ky = -clawt_task_get_finished_at(y->task);
        break;

    case CLAWT_BOARD_HIDDEN:
    case CLAWT_BOARD_PLANNING:
    case CLAWT_BOARD_DELEGATED:
    case CLAWT_BOARD_REVIEW:
    case CLAWT_BOARD_GATING:
    case CLAWT_BOARD_BLOCKED:
    default:
        kx = started_or_created(x->task);
        ky = started_or_created(y->task);
        break;
    }

    if (kx != ky)
        return kx < ky ? -1 : 1;

    return g_strcmp0(clawt_task_get_id(x->task), clawt_task_get_id(y->task));
}

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

/* Present only when it happened: 0 is "never" in every task stamp. */
static void
add_time(JsonBuilder *builder, const gchar *name, gint64 value)
{
    if (value > 0)
        add_int(builder, name, value);
}

static void
add_id_array(JsonBuilder *builder, const gchar *name, GPtrArray *ids)
{
    guint i;

    if (ids == NULL || ids->len == 0)
        return;

    json_builder_set_member_name(builder, name);
    json_builder_begin_array(builder);
    for (i = 0; i < ids->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(ids, i));
    json_builder_end_array(builder);
}

static void
add_cost(JsonBuilder *builder, const Build *b, const gchar *task_id)
{
    gdouble cost = source_cost(b, task_id);

    if (cost < 0.0)
        return;

    json_builder_set_member_name(builder, "cost_usd");
    json_builder_add_double_value(builder, cost);
}

static void
add_timeline(JsonBuilder *builder, const Build *b, ClawtTask *task)
{
    g_autoptr(JsonNode) journal = NULL;

    if (!b->request->timeline)
        return;

    journal = clawt_task_manager_journal(b->tasks, clawt_task_get_id(task),
                                         NULL);

    json_builder_set_member_name(builder, "timeline");
    json_builder_add_value(builder, clawt_board_timeline(
        task, (journal != NULL && JSON_NODE_HOLDS_ARRAY(journal))
                  ? json_node_get_array(journal) : NULL,
        b->request->now));
}

/* A short {id, title} object, for the header's and lanes' current task. */
static void
add_task_ref(JsonBuilder *builder, const gchar *name, ClawtTask *task)
{
    if (task == NULL)
        return;

    json_builder_set_member_name(builder, name);
    json_builder_begin_object(builder);
    add_string(builder, "id", clawt_task_get_id(task));
    add_string(builder, "title", clawt_task_get_title(task));
    json_builder_end_object(builder);
}

/*
 * One chip: a task the card handed on.  Its `after` names only the
 * siblings it waits for, which is what a client draws as a link between
 * chips; the card's own `after` has the full list.
 */
static void
add_chip(JsonBuilder *builder, const Build *b, Card *card, ClawtTask *child)
{
    g_autoptr(GPtrArray) blockers = clawt_task_manager_blockers(
        b->tasks, clawt_task_get_id(child));
    g_autoptr(GPtrArray) siblings = g_ptr_array_new();
    gint64 now = b->request->now;
    gint64 quiet_at = 0;
    gboolean quiet;
    gint64 begun = started_or_created(child);
    gint64 finished = clawt_task_get_finished_at(child);
    guint i;
    guint j;

    for (i = 0; blockers != NULL && i < blockers->len; i++) {
        const gchar *id = g_ptr_array_index(blockers, i);

        for (j = 0; j < card->children->len; j++)
            if (g_strcmp0(id, clawt_task_get_id(
                    g_ptr_array_index(card->children, j))) == 0)
                g_ptr_array_add(siblings, (gpointer)id);
    }

    quiet = clawt_board_is_quiet(child, NULL, now, b->request->quiet_minutes,
                                 FALSE, &quiet_at);

    json_builder_begin_object(builder);
    add_string(builder, "id", clawt_task_get_id(child));
    add_string(builder, "title", clawt_task_get_title(child));
    add_string(builder, "worker", clawt_task_get_assignee(child));
    add_string(builder, "state", clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                                    clawt_task_get_state(child)));
    if (clawt_task_get_phase(child) != CLAWT_TASK_PHASE_NONE)
        add_string(builder, "phase",
                   clawt_task_phase_to_nick(clawt_task_get_phase(child)));
    add_time(builder, "created_at", clawt_task_get_created_at(child));
    add_time(builder, "started_at", clawt_task_get_started_at(child));
    add_time(builder, "finished_at", finished);
    add_time(builder, "state_at", clawt_task_get_state_at(child));
    add_int(builder, "elapsed", MAX((finished > 0 ? finished : now) - begun,
                                    0));
    add_id_array(builder, "after", siblings);
    if (clawt_task_get_state(child) == CLAWT_TASK_REVIEW)
        add_string(builder, "review_by", clawt_task_get_reviewer(child));
    add_string(builder, "waiting_on", clawt_task_get_waiting_on(child));
    json_builder_set_member_name(builder, "quiet");
    json_builder_add_boolean_value(builder, quiet);
    add_time(builder, "last_activity_at",
             clawt_task_get_last_sign_of_life(child));
    if (!quiet)
        add_time(builder, "quiet_at", quiet_at);
    add_cost(builder, b, clawt_task_get_id(child));
    add_timeline(builder, b, child);
    json_builder_end_object(builder);
}

/* Each reason a card is blocked, with what a person needs to act on it. */
static void
add_blocks(JsonBuilder *builder, const Build *b, Card *card)
{
    ClawtTask *task = card->task;
    guint bit;
    guint i;

    if (card->blocks == CLAWT_BOARD_BLOCK_NONE)
        return;

    json_builder_set_member_name(builder, "blocks");
    json_builder_begin_array(builder);

    for (bit = 1; bit <= CLAWT_BOARD_BLOCK_PHASE; bit <<= 1) {
        ClawtBoardBlocks block = (ClawtBoardBlocks)bit;

        if ((card->blocks & block) == 0)
            continue;

        /*
         * A failed or stalled child is named, one entry each, so the
         * client can link to the task that needs the retry.
         */
        if (block == CLAWT_BOARD_BLOCK_CHILD_FAILED ||
            block == CLAWT_BOARD_BLOCK_CHILD_STALLED) {
            ClawtTaskState want = (block == CLAWT_BOARD_BLOCK_CHILD_FAILED)
                ? CLAWT_TASK_FAILED : CLAWT_TASK_STALLED;

            for (i = 0; i < card->children->len; i++) {
                ClawtTask *child = g_ptr_array_index(card->children, i);

                if (clawt_task_get_state(child) != want ||
                    child_acknowledged(task, child))
                    continue;

                json_builder_begin_object(builder);
                add_string(builder, "reason", clawt_board_block_nick(block));
                add_string(builder, "task", clawt_task_get_id(child));
                add_string(builder, "worker", clawt_task_get_assignee(child));
                add_string(builder, "detail", clawt_task_get_reason(child));
                json_builder_end_object(builder);
            }
            continue;
        }

        json_builder_begin_object(builder);
        add_string(builder, "reason", clawt_board_block_nick(block));

        switch (block) {
        case CLAWT_BOARD_BLOCK_FAILED:
        case CLAWT_BOARD_BLOCK_STALLED:
            add_string(builder, "detail", clawt_task_get_reason(task));
            break;
        case CLAWT_BOARD_BLOCK_DECISION:
            add_int(builder, "count", card->decisions);
            break;
        case CLAWT_BOARD_BLOCK_WAITING:
            add_string(builder, "party", clawt_task_get_waiting_on(task));
            add_string(builder, "detail", clawt_task_get_question(task));
            break;
        case CLAWT_BOARD_BLOCK_QUIET:
            add_int(builder, "since",
                    clawt_task_get_last_sign_of_life(task));
            break;
        case CLAWT_BOARD_BLOCK_NONE:
        case CLAWT_BOARD_BLOCK_CHILD_FAILED:
        case CLAWT_BOARD_BLOCK_CHILD_STALLED:
        case CLAWT_BOARD_BLOCK_PHASE:
        default:
            break;
        }

        json_builder_end_object(builder);
    }

    (void)b;
    json_builder_end_array(builder);
}

/*
 * What waits for a verdict, and for how long: the bottleneck signal.  A
 * child's result waiting for this agent, oldest first, or the card's own
 * result waiting for its reviewer.
 */
static void
add_review(JsonBuilder *builder, const Build *b, Card *card)
{
    ClawtTask *task = card->task;
    ClawtTask *waiting = NULL;
    const gchar *waiting_for = NULL;
    gint64 since;
    guint i;

    if (clawt_task_get_state(task) == CLAWT_TASK_REVIEW) {
        waiting = task;
        waiting_for = clawt_task_get_reviewer(task);
    } else {
        for (i = 0; i < card->children->len; i++) {
            ClawtTask *child = g_ptr_array_index(card->children, i);

            if (clawt_task_get_state(child) != CLAWT_TASK_REVIEW ||
                g_strcmp0(clawt_task_get_reviewer(child),
                          b->request->agent) != 0)
                continue;

            if (waiting == NULL ||
                clawt_task_get_state_at(child) <
                    clawt_task_get_state_at(waiting))
                waiting = child;
        }

        waiting_for = b->request->agent;
    }

    if (waiting == NULL)
        return;

    since = clawt_task_get_state_at(waiting);

    json_builder_set_member_name(builder, "review");
    json_builder_begin_object(builder);
    add_string(builder, "task", clawt_task_get_id(waiting));
    add_string(builder, "title", clawt_task_get_title(waiting));
    add_string(builder, "worker", clawt_task_get_assignee(waiting));
    add_string(builder, "waiting_for", waiting_for);
    add_int(builder, "since", since);
    add_int(builder, "wait", MAX(b->request->now - since, 0));
    json_builder_end_object(builder);
}

static void
add_card(JsonBuilder *builder, const Build *b, Card *card)
{
    ClawtTask *task = card->task;
    gint64 now = b->request->now;
    gint64 begun = started_or_created(task);
    gint64 finished = clawt_task_get_finished_at(task);
    g_autoptr(GPtrArray) after = clawt_task_manager_blockers(
        b->tasks, clawt_task_get_id(task));
    g_autoptr(GPtrArray) waiting_for = clawt_task_manager_waiting_on(
        b->tasks, clawt_task_get_id(task));
    guint i;

    json_builder_begin_object(builder);

    add_string(builder, "id", clawt_task_get_id(task));
    add_string(builder, "title", clawt_task_get_title(task));
    add_string(builder, "state", clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                                    clawt_task_get_state(task)));
    add_string(builder, "column", clawt_board_column_nick(card->column));
    if (clawt_task_get_phase(task) != CLAWT_TASK_PHASE_NONE) {
        add_string(builder, "phase",
                   clawt_task_phase_to_nick(clawt_task_get_phase(task)));
        add_time(builder, "phase_at", clawt_task_get_phase_at(task));
    }
    if (card->position > 0)
        add_int(builder, "position", card->position);
    add_string(builder, "origin", clawt_task_get_origin(task));
    add_string(builder, "project", clawt_task_get_project(task));

    /* The repository by its last component: a card is narrow. */
    if (clawt_task_get_checkout_repo(task) != NULL) {
        g_autofree gchar *repo = g_path_get_basename(
            clawt_task_get_checkout_repo(task));

        add_string(builder, "repo", repo);
    }
    add_string(builder, "branch", clawt_task_get_checkout_branch(task));

    add_time(builder, "created_at", clawt_task_get_created_at(task));
    add_time(builder, "started_at", clawt_task_get_started_at(task));
    add_time(builder, "finished_at", finished);
    add_time(builder, "state_at", clawt_task_get_state_at(task));
    add_int(builder, "age", MAX(now - clawt_task_get_created_at(task), 0));
    add_int(builder, "elapsed", MAX((finished > 0 ? finished : now) - begun,
                                    0));

    add_string(builder, "note", clawt_task_get_progress_note(task));
    add_time(builder, "note_at", clawt_task_get_progress_at(task));
    add_cost(builder, b, clawt_task_get_id(task));

    /* Its VENTURE ticket, when the mirror has made or matched one. */
    if (b->store != NULL) {
        g_autoptr(ClawtVentureRef) ref = clawt_task_store_venture_ref(
            b->store, clawt_task_get_id(task));

        if (ref != NULL && ref->ticket_id > 0) {
            json_builder_set_member_name(builder, "ticket");
            json_builder_begin_object(builder);
            add_int(builder, "id", ref->ticket_id);
            add_string(builder, "url", ref->url);
            add_string(builder, "state", ref->state);
            json_builder_end_object(builder);
        }
    }

    add_id_array(builder, "after", after);
    add_id_array(builder, "waiting_for", waiting_for);
    add_string(builder, "waiting_on", clawt_task_get_waiting_on(task));
    add_string(builder, "question", clawt_task_get_question(task));
    add_blocks(builder, b, card);

    json_builder_set_member_name(builder, "quiet");
    json_builder_add_boolean_value(builder, card->quiet);
    add_time(builder, "last_activity_at",
             clawt_task_get_last_sign_of_life(task));
    if (!card->quiet)
        add_time(builder, "quiet_at", card->quiet_at);

    add_review(builder, b, card);

    json_builder_set_member_name(builder, "chips");
    json_builder_begin_array(builder);
    for (i = 0; i < card->children->len; i++)
        add_chip(builder, b, card, g_ptr_array_index(card->children, i));
    json_builder_end_array(builder);

    /* Where a completed task landed, and how long it took to. */
    if (card->column == CLAWT_BOARD_DONE) {
        g_autofree gchar *commit = NULL;
        g_autofree gchar *branch = NULL;

        clawt_board_result_refs(clawt_task_get_result(task), &commit, &branch);

        json_builder_set_member_name(builder, "done");
        json_builder_begin_object(builder);
        add_string(builder, "commit", commit);
        add_string(builder, "branch",
                   clawt_task_get_checkout_branch(task) != NULL
                       ? clawt_task_get_checkout_branch(task) : branch);
        add_int(builder, "land",
                MAX(finished - clawt_task_get_created_at(task), 0));
        json_builder_end_object(builder);
    }

    add_timeline(builder, b, task);

    json_builder_end_object(builder);
}

/* The agent's live state and latest step, as the header and lanes have it. */
static void
add_agent_live(JsonBuilder *builder, const Build *b, ClawtBoardAgent *info)
{
    gint64 now = b->request->now;

    add_string(builder, "name", info->name);
    add_string(builder, "state", info->state != NULL ? info->state : "unknown");
    add_string(builder, "team", info->team);

    /*
     * What it is doing, by the one rule (clawt_board_agent_activity()),
     * with "in a turn" read the way busy_since below reads it.  The wait
     * members ride whenever it has a wait pending -- also while it works,
     * so a client that shows "and waiting on a build" has it -- and are
     * absent otherwise, the same as agent.list (since 0.78.0).
     */
    add_string(builder, "activity", clawt_board_agent_activity_nick(
        clawt_board_agent_activity(info->turn_started > 0,
                                   info->waits_pending)));
    add_int(builder, "waits_pending", (gint64)info->waits_pending);

    if (info->waits_pending > 0) {
        add_string(builder, "wait_label", info->wait_label);
        add_time(builder, "waiting_since", info->waiting_since);
    }

    if (info->step_text != NULL || info->step_tool != NULL) {
        json_builder_set_member_name(builder, "step");
        json_builder_begin_object(builder);
        add_string(builder, "text", info->step_text);
        add_string(builder, "tool", info->step_tool);
        add_time(builder, "at", info->step_at);
        json_builder_end_object(builder);
    }

    add_time(builder, "busy_since", info->turn_started);

    /*
     * Idle: how long since it was last seen doing anything.  0 while it
     * is in a turn; absent when nothing says.
     */
    if (info->turn_started > 0)
        add_int(builder, "idle", 0);
    else if (info->last_active > 0)
        add_int(builder, "idle", MAX(now - info->last_active, 0));

    add_int(builder, "mailbox", info->mailbox);
}

/* The task an agent is on: its running turn's, else its latest running. */
static ClawtTask *
current_task_of(const Build *b, const gchar *agent_id, ClawtBoardAgent *info)
{
    g_autoptr(GPtrArray) open = NULL;
    ClawtTask *best = NULL;
    guint i;

    if (info->turn_task != NULL) {
        ClawtTask *task = clawt_task_manager_get(b->tasks, info->turn_task);

        if (task != NULL)
            return task;
    }

    open = clawt_task_manager_list(b->tasks, agent_id, FALSE);

    for (i = 0; i < open->len; i++) {
        ClawtTask *task = g_ptr_array_index(open, i);

        if (clawt_task_get_state(task) != CLAWT_TASK_RUNNING)
            continue;

        if (best == NULL || clawt_task_get_last_sign_of_life(task) >
                            clawt_task_get_last_sign_of_life(best))
            best = task;
    }

    return best;
}

static void
add_header(JsonBuilder *builder, const Build *b, GPtrArray *cards)
{
    ClawtBoardAgent info;
    guint counts[CLAWT_BOARD_BLOCKED + 1];
    guint i;

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < cards->len; i++)
        counts[((Card *)g_ptr_array_index(cards, i))->column]++;

    source_describe(b, b->request->agent, &info);

    json_builder_set_member_name(builder, "header");
    json_builder_begin_object(builder);
    add_string(builder, "agent", b->request->agent);
    add_agent_live(builder, b, &info);
    add_task_ref(builder, "current_task",
                 current_task_of(b, b->request->agent, &info));

    json_builder_set_member_name(builder, "counts");
    json_builder_begin_object(builder);
    add_int(builder, "backlog", counts[CLAWT_BOARD_BACKLOG]);
    add_int(builder, "planning", counts[CLAWT_BOARD_PLANNING]);
    add_int(builder, "delegated", counts[CLAWT_BOARD_DELEGATED]);
    add_int(builder, "review", counts[CLAWT_BOARD_REVIEW]);
    add_int(builder, "gating", counts[CLAWT_BOARD_GATING]);
    add_int(builder, "blocked", counts[CLAWT_BOARD_BLOCKED]);
    add_int(builder, "done", counts[CLAWT_BOARD_DONE]);
    add_int(builder, "in_flight", counts[CLAWT_BOARD_PLANNING] +
                                  counts[CLAWT_BOARD_DELEGATED] +
                                  counts[CLAWT_BOARD_GATING]);
    add_int(builder, "in_review", counts[CLAWT_BOARD_REVIEW]);
    json_builder_end_object(builder);

    json_builder_end_object(builder);
    clawt_board_agent_clear(&info);
}

/* Appends @id to @order unless it is there already or is @skip. */
static void
add_member(GPtrArray *order, GHashTable *seen, const gchar *id,
           const gchar *skip)
{
    if (id == NULL || g_strcmp0(id, skip) == 0 ||
        g_hash_table_contains(seen, id))
        return;

    g_hash_table_add(seen, g_strdup(id));
    g_ptr_array_add(order, g_strdup(id));
}

/*
 * The worker lanes: the team asked for (or the agent's own), then every
 * worker a chip names that is not on it, in the order they appear.  Not
 * the board's own agent, whom the header already describes.
 */
static void
add_lanes(JsonBuilder *builder, const Build *b, GPtrArray *cards)
{
    g_autoptr(GPtrArray) order = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    g_autofree gchar *own_team = NULL;
    const gchar *team = b->request->team;
    gint64 now = b->request->now;
    guint i;
    guint j;

    if (team == NULL) {
        ClawtBoardAgent info;

        if (source_describe(b, b->request->agent, &info))
            own_team = g_strdup(info.team);
        clawt_board_agent_clear(&info);
        team = own_team;
    }

    if (team != NULL && b->source != NULL &&
        b->source->team_members != NULL) {
        g_autoptr(GPtrArray) members = b->source->team_members(team,
                                                               b->user_data);

        for (i = 0; members != NULL && i < members->len; i++)
            add_member(order, seen, g_ptr_array_index(members, i),
                       b->request->agent);
    }

    for (i = 0; i < cards->len; i++) {
        Card *card = g_ptr_array_index(cards, i);

        for (j = 0; j < card->children->len; j++)
            add_member(order, seen, clawt_task_get_assignee(
                           g_ptr_array_index(card->children, j)),
                       b->request->agent);
    }

    json_builder_set_member_name(builder, "lanes");
    json_builder_begin_array(builder);

    for (i = 0; i < order->len; i++) {
        const gchar *agent_id = g_ptr_array_index(order, i);
        g_autoptr(GPtrArray) open = clawt_task_manager_list(b->tasks, agent_id,
                                                            FALSE);
        ClawtBoardAgent info;
        guint queued = 0;
        guint running = 0;
        guint review = 0;

        source_describe(b, agent_id, &info);

        for (j = 0; j < open->len; j++) {
            switch (clawt_task_get_state(g_ptr_array_index(open, j))) {
            case CLAWT_TASK_PENDING:
            case CLAWT_TASK_BLOCKED:
                queued++;
                break;
            case CLAWT_TASK_RUNNING:
                running++;
                break;
            case CLAWT_TASK_REVIEW:
                review++;
                break;
            case CLAWT_TASK_COMPLETED:
            case CLAWT_TASK_FAILED:
            case CLAWT_TASK_CANCELLED:
            case CLAWT_TASK_STALLED:
            case CLAWT_TASK_UNASSIGNED:
            default:
                break;
            }
        }

        json_builder_begin_object(builder);
        add_string(builder, "agent", agent_id);
        add_string(builder, "role", team != NULL &&
                   g_strcmp0(info.team, team) == 0 ? "member" : "worker");
        add_agent_live(builder, b, &info);
        add_task_ref(builder, "current_task",
                     current_task_of(b, agent_id, &info));

        json_builder_set_member_name(builder, "queue");
        json_builder_begin_object(builder);
        add_int(builder, "queued", queued);
        add_int(builder, "running", running);
        add_int(builder, "review", review);
        add_int(builder, "mailbox", info.mailbox);
        json_builder_end_object(builder);

        json_builder_end_object(builder);
        clawt_board_agent_clear(&info);
    }

    (void)now;
    json_builder_end_array(builder);
}

JsonNode *
clawt_board_build(ClawtTaskManager        *tasks,
                  const ClawtBoardRequest *request,
                  const ClawtBoardSource  *source,
                  gpointer                 user_data)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GPtrArray) owned = NULL;
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GPtrArray) cards = g_ptr_array_new_with_free_func(card_free);
    g_autoptr(GHashTable) live_ids = g_hash_table_new(g_str_hash, g_str_equal);
    static const ClawtBoardColumn drawn[] = {
        CLAWT_BOARD_BLOCKED, CLAWT_BOARD_BACKLOG, CLAWT_BOARD_PLANNING,
        CLAWT_BOARD_DELEGATED, CLAWT_BOARD_REVIEW, CLAWT_BOARD_GATING,
        CLAWT_BOARD_DONE
    };
    Build b;
    guint nested = 0;
    guint hidden = 0;
    guint from_history = 0;
    guint position = 0;
    guint i;
    guint c;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(tasks), NULL);
    g_return_val_if_fail(request != NULL && request->agent != NULL, NULL);

    b.tasks = tasks;
    b.store = clawt_task_manager_get_store(tasks);
    b.request = request;
    b.source = source;
    b.user_data = user_data;
    b.wait_pending = (source != NULL && source->wait_pending != NULL)
        ? source->wait_pending(request->agent, user_data) : FALSE;

    /*
     * The cards: the agent's tasks in the live set -- what task.list
     * {agent} answers -- less those nested under another of its own, and
     * less those no column shows.  Both are counted, so the totals add up
     * to the list.
     */
    owned = clawt_task_manager_list(tasks, request->agent, TRUE);

    for (i = 0; i < owned->len; i++) {
        ClawtTask *task = g_ptr_array_index(owned, i);
        Card *card;

        g_hash_table_add(live_ids, (gpointer)clawt_task_get_id(task));

        if (is_nested(tasks, task, request->agent)) {
            nested++;
            continue;
        }

        card = card_new(&b, task);

        if (card->column == CLAWT_BOARD_HIDDEN) {
            hidden++;
            card_free(card);
            continue;
        }

        g_ptr_array_add(cards, card);
    }

    /*
     * A window longer than the live set's day reaches into the store for
     * the rest of what it completed.  Bounded, and only completed work:
     * an old failure is history, not something blocking the agent now.
     */
    if (b.store != NULL && request->done_since < request->now - 86400) {
        ClawtTaskQuery query = { NULL, CLAWT_TASK_COMPLETED, 0,
                                 BOARD_HISTORY_LIMIT };

        query.assignee = request->agent;
        history = clawt_task_manager_list_history(tasks, &query, NULL);

        for (i = 0; history != NULL && i < history->len; i++) {
            ClawtTask *task = g_ptr_array_index(history, i);
            Card *card;

            if (g_hash_table_contains(live_ids, clawt_task_get_id(task)) ||
                clawt_task_get_finished_at(task) < request->done_since ||
                is_nested(tasks, task, request->agent))
                continue;

            card = card_new(&b, task);

            if (card->column != CLAWT_BOARD_DONE) {
                card_free(card);
                continue;
            }

            from_history++;
            g_ptr_array_add(cards, card);
        }
    }

    g_ptr_array_sort(cards, card_compare);

    for (i = 0; i < cards->len; i++) {
        Card *card = g_ptr_array_index(cards, i);

        if (card->column == CLAWT_BOARD_BACKLOG)
            card->position = ++position;
    }

    /* ── The reply ── */
    json_builder_begin_object(builder);
    add_string(builder, "agent", request->agent);
    add_string(builder, "team", request->team);
    add_int(builder, "now", request->now);

    json_builder_set_member_name(builder, "window");
    json_builder_begin_object(builder);
    add_string(builder, "spec", request->window != NULL ? request->window
                                                        : "today");
    add_int(builder, "since", request->done_since);
    json_builder_end_object(builder);

    add_int(builder, "quiet_minutes", request->quiet_minutes);

    add_header(builder, &b, cards);

    /*
     * The columns, always all seven and in this order -- blocked pinned
     * first, then the flow from backlog to done -- so a client draws an
     * empty column rather than wondering whether it was left out.
     */
    json_builder_set_member_name(builder, "columns");
    json_builder_begin_array(builder);

    for (c = 0; c < G_N_ELEMENTS(drawn); c++) {
        json_builder_begin_object(builder);
        add_string(builder, "id", clawt_board_column_nick(drawn[c]));
        add_string(builder, "label", clawt_board_column_label(drawn[c]));
        json_builder_set_member_name(builder, "cards");
        json_builder_begin_array(builder);

        for (i = 0; i < cards->len; i++) {
            Card *card = g_ptr_array_index(cards, i);

            if (card->column == drawn[c])
                add_card(builder, &b, card);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    add_lanes(builder, &b, cards);

    json_builder_set_member_name(builder, "totals");
    json_builder_begin_object(builder);
    add_int(builder, "listed", owned->len);
    add_int(builder, "cards", cards->len);
    add_int(builder, "nested", nested);
    add_int(builder, "hidden", hidden);
    add_int(builder, "from_history", from_history);
    json_builder_end_object(builder);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── The text view ─────────────────────────────────────────────────── */

static const gchar *
member_string(JsonObject *object, const gchar *name)
{
    return json_object_get_string_member_with_default(object, name, NULL);
}

static gint64
member_int(JsonObject *object, const gchar *name)
{
    return json_object_get_int_member_with_default(object, name, 0);
}

static JsonObject *
member_object(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);

    return (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
        ? json_node_get_object(node) : NULL;
}

static JsonArray *
member_array(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);

    return (node != NULL && JSON_NODE_HOLDS_ARRAY(node))
        ? json_node_get_array(node) : NULL;
}

/* The first line of @text, cut to @bytes with "..." when it was longer. */
static gchar *
one_line(const gchar *text, gsize bytes)
{
    g_autofree gchar *line = NULL;
    const gchar *newline;
    gchar *cut;

    if (text == NULL)
        return g_strdup("");

    newline = strchr(text, '\n');
    line = newline != NULL ? g_strndup(text, (gsize)(newline - text))
                           : g_strdup(text);
    g_strstrip(line);

    if (strlen(line) <= bytes && newline == NULL)
        return g_steal_pointer(&line);

    cut = clawt_utf8_truncate(line, bytes, FALSE);
    if (strlen(cut) < strlen(line) || newline != NULL) {
        gchar *dotted = g_strconcat(cut, "...", NULL);

        g_free(cut);
        return dotted;
    }

    return cut;
}

static void
render_chip(GString *out, JsonObject *chip)
{
    g_autofree gchar *elapsed = clawt_board_duration_label(
        member_int(chip, "elapsed"));
    g_autofree gchar *title = one_line(member_string(chip, "title"),
                                       BOARD_TITLE_BYTES);
    JsonArray *after = member_array(chip, "after");

    g_string_append_printf(out, "      > %-12s %-9s %6s  %s  %s%s\n",
                           member_string(chip, "worker") != NULL
                               ? member_string(chip, "worker") : "?",
                           member_string(chip, "state") != NULL
                               ? member_string(chip, "state") : "?",
                           elapsed, member_string(chip, "id"), title,
                           json_object_get_boolean_member_with_default(
                               chip, "quiet", FALSE) ? "  (quiet)" : "");

    if (after != NULL && json_array_get_length(after) > 0) {
        guint i;

        g_string_append(out, "          after:");
        for (i = 0; i < json_array_get_length(after); i++)
            g_string_append_printf(out, " %s",
                                   json_array_get_string_element(after, i));
        g_string_append_c(out, '\n');
    }
}

static void
render_card(GString *out, JsonObject *card, gint64 now)
{
    JsonObject *ticket = member_object(card, "ticket");
    JsonObject *review = member_object(card, "review");
    JsonObject *done = member_object(card, "done");
    JsonArray *blocks = member_array(card, "blocks");
    JsonArray *chips = member_array(card, "chips");
    g_autofree gchar *title = one_line(member_string(card, "title"),
                                       BOARD_TITLE_BYTES);
    g_autofree gchar *elapsed = clawt_board_duration_label(
        member_int(card, "elapsed"));
    g_autofree gchar *ticket_label = ticket != NULL
        ? g_strdup_printf("#%" G_GINT64_FORMAT " ", member_int(ticket, "id"))
        : g_strdup("");
    g_autofree gchar *repo_label = member_string(card, "repo") != NULL
        ? g_strdup_printf("%s ", member_string(card, "repo")) : g_strdup("");
    g_autofree gchar *position = member_int(card, "position") > 0
        ? g_strdup_printf("%" G_GINT64_FORMAT ". ", member_int(card, "position"))
        : g_strdup("");
    g_autofree gchar *cost = json_object_has_member(card, "cost_usd")
        ? g_strdup_printf("  $%.2f", json_object_get_double_member(card,
                                                                   "cost_usd"))
        : g_strdup("");
    guint i;

    /*
     * One line for the card: where it is in the queue, the ticket and the
     * repository, the title, and how long it has been going (its age, in
     * the backlog, where nothing has started), with what it cost.
     */
    if (g_strcmp0(member_string(card, "column"), "backlog") == 0) {
        g_autofree gchar *age = clawt_board_duration_label(
            member_int(card, "age"));

        g_string_append_printf(out, "  %s%s  %s%s%s  age %s%s%s\n", position,
                               member_string(card, "id"), ticket_label,
                               repo_label, title, age, cost,
                               member_array(card, "waiting_for") != NULL
                                   ? "  (held)" : "");
    } else {
        g_string_append_printf(out, "  %s  %s%s%s  %s%s\n",
                               member_string(card, "id"), ticket_label,
                               repo_label, title, elapsed, cost);
    }

    if (member_string(card, "phase") != NULL)
        g_string_append_printf(out, "      phase: %s\n",
                               member_string(card, "phase"));

    for (i = 0; blocks != NULL && i < json_array_get_length(blocks); i++) {
        JsonObject *block = json_array_get_object_element(blocks, i);
        const gchar *reason = member_string(block, "reason");
        g_autofree gchar *detail = one_line(member_string(block, "detail"),
                                            BOARD_NOTE_BYTES);

        if (g_strcmp0(reason, "decision") == 0)
            g_string_append_printf(out, "      blocked: %" G_GINT64_FORMAT
                                   " decision(s) for the operator\n",
                                   member_int(block, "count"));
        else if (g_strcmp0(reason, "waiting") == 0)
            g_string_append_printf(out, "      blocked: waiting on %s%s%s\n",
                                   member_string(block, "party"),
                                   *detail != '\0' ? ": " : "", detail);
        else if (g_strcmp0(reason, "quiet") == 0) {
            g_autofree gchar *since = clawt_board_duration_label(
                member_int(block, "since") > 0
                    ? now - member_int(block, "since") : 0);

            g_string_append_printf(out, "      blocked: quiet for %s\n", since);
        } else if (member_string(block, "task") != NULL)
            g_string_append_printf(out, "      blocked: %s %s (%s)%s%s\n",
                                   reason, member_string(block, "task"),
                                   member_string(block, "worker") != NULL
                                       ? member_string(block, "worker") : "?",
                                   *detail != '\0' ? ": " : "", detail);
        else
            g_string_append_printf(out, "      blocked: %s%s%s\n", reason,
                                   *detail != '\0' ? ": " : "", detail);
    }

    if (review != NULL) {
        g_autofree gchar *wait = clawt_board_duration_label(
            member_int(review, "wait"));

        g_string_append_printf(out, "      review: %s from %s, waiting for %s "
                               "for %s\n", member_string(review, "task"),
                               member_string(review, "worker") != NULL
                                   ? member_string(review, "worker") : "?",
                               member_string(review, "waiting_for") != NULL
                                   ? member_string(review, "waiting_for")
                                   : "?", wait);
    }

    if (member_string(card, "note") != NULL) {
        g_autofree gchar *note = one_line(member_string(card, "note"),
                                          BOARD_NOTE_BYTES);

        g_string_append_printf(out, "      note: %s\n", note);
    }

    if (done != NULL) {
        g_autofree gchar *land = clawt_board_duration_label(
            member_int(done, "land"));

        g_string_append_printf(out, "      landed in %s%s%s%s%s\n", land,
                               member_string(done, "commit") != NULL
                                   ? "  commit " : "",
                               member_string(done, "commit") != NULL
                                   ? member_string(done, "commit") : "",
                               member_string(done, "branch") != NULL
                                   ? "  branch " : "",
                               member_string(done, "branch") != NULL
                                   ? member_string(done, "branch") : "");
    }

    for (i = 0; chips != NULL && i < json_array_get_length(chips); i++)
        render_chip(out, json_array_get_object_element(chips, i));
}

/* The current task and step of the header (@indent 2) or a lane (6). */
static void
render_live(GString *out, JsonObject *who, gint64 now, gint indent)
{
    JsonObject *step = member_object(who, "step");
    JsonObject *current = member_object(who, "current_task");

    /* Not in a turn and waiting on its own background work: say on what. */
    if (g_strcmp0(member_string(who, "activity"), "waiting") == 0) {
        g_autofree gchar *raw = clawt_board_waiting_label(
            member_string(who, "wait_label"), member_int(who, "waiting_since"),
            now);
        g_autofree gchar *line = one_line(raw, BOARD_NOTE_BYTES);

        g_string_append_printf(out, "%*s%s\n", indent, "", line);
    }

    if (current != NULL) {
        g_autofree gchar *title = one_line(member_string(current, "title"),
                                           BOARD_TITLE_BYTES);

        g_string_append_printf(out, "%*son %s  %s\n", indent, "",
                               member_string(current, "id"), title);
    }

    if (step != NULL) {
        g_autofree gchar *text = one_line(member_string(step, "text") != NULL
                                              ? member_string(step, "text")
                                              : member_string(step, "tool"),
                                          BOARD_NOTE_BYTES);
        g_autofree gchar *ago = member_int(step, "at") > 0
            ? clawt_board_duration_label(now - member_int(step, "at"))
            : NULL;

        g_string_append_printf(out, "%*snow: %s%s%s%s\n", indent, "", text,
                               ago != NULL ? "  (" : "",
                               ago != NULL ? ago : "",
                               ago != NULL ? " ago)" : "");
    }
}

gchar *
clawt_board_render_text(JsonObject *board)
{
    GString *out;
    JsonObject *header;
    JsonObject *counts;
    JsonObject *window;
    JsonArray *columns;
    JsonArray *lanes;
    gint64 now;
    guint i;
    guint j;

    g_return_val_if_fail(board != NULL, NULL);

    out = g_string_new(NULL);
    now = member_int(board, "now");
    header = member_object(board, "header");
    counts = header != NULL ? member_object(header, "counts") : NULL;
    window = member_object(board, "window");
    columns = member_array(board, "columns");
    lanes = member_array(board, "lanes");

    /* The header: who, doing what, on what, and how much is in front. */
    g_string_append_printf(out, "%s  %s", member_string(board, "agent"),
                           header != NULL && member_string(header, "state")
                               ? member_string(header, "state") : "unknown");
    if (member_string(board, "team") != NULL)
        g_string_append_printf(out, "  team %s", member_string(board, "team"));
    g_string_append_printf(out, "  (done since %s, quiet after %"
                           G_GINT64_FORMAT "m)\n",
                           window != NULL && member_string(window, "spec")
                               ? member_string(window, "spec") : "today",
                           member_int(board, "quiet_minutes"));

    if (header != NULL)
        render_live(out, header, now, 2);

    if (counts != NULL)
        g_string_append_printf(out, "  mailbox %" G_GINT64_FORMAT
                               " | backlog %" G_GINT64_FORMAT
                               " | in flight %" G_GINT64_FORMAT
                               " | in review %" G_GINT64_FORMAT
                               " | blocked %" G_GINT64_FORMAT
                               " | done %" G_GINT64_FORMAT "\n",
                               member_int(header, "mailbox"),
                               member_int(counts, "backlog"),
                               member_int(counts, "in_flight"),
                               member_int(counts, "in_review"),
                               member_int(counts, "blocked"),
                               member_int(counts, "done"));

    /* One section per column, in the reply's order, empty ones included. */
    for (i = 0; columns != NULL && i < json_array_get_length(columns); i++) {
        JsonObject *column = json_array_get_object_element(columns, i);
        JsonArray *cards = member_array(column, "cards");
        guint n = cards != NULL ? json_array_get_length(cards) : 0;

        g_string_append_printf(out, "\n== %s (%u) ==\n",
                               member_string(column, "label") != NULL
                                   ? member_string(column, "label")
                                   : member_string(column, "id"), n);

        for (j = 0; j < n; j++)
            render_card(out, json_array_get_object_element(cards, j), now);
    }

    if (lanes != NULL && json_array_get_length(lanes) > 0) {
        g_string_append(out, "\n== Lanes ==\n");

        for (i = 0; i < json_array_get_length(lanes); i++) {
            JsonObject *lane = json_array_get_object_element(lanes, i);
            JsonObject *queue = member_object(lane, "queue");
            g_autofree gchar *idle = json_object_has_member(lane, "idle") &&
                                     member_int(lane, "busy_since") == 0
                ? clawt_board_duration_label(member_int(lane, "idle")) : NULL;
            g_autofree gchar *busy = member_int(lane, "busy_since") > 0
                ? clawt_board_duration_label(now - member_int(lane,
                                                              "busy_since"))
                : NULL;

            g_string_append_printf(out, "  %-12s %-9s", member_string(lane,
                                                                  "agent"),
                                   member_string(lane, "state") != NULL
                                       ? member_string(lane, "state") : "?");
            /*
             * A waiting worker is not idle: render_live() below says on
             * what, so the lane line only says that it is.
             */
            if (busy != NULL)
                g_string_append_printf(out, "  busy %s", busy);
            else if (g_strcmp0(member_string(lane, "activity"),
                               "waiting") == 0)
                g_string_append(out, "  waiting");
            else if (idle != NULL)
                g_string_append_printf(out, "  idle %s", idle);
            if (queue != NULL)
                g_string_append_printf(out, "  queued %" G_GINT64_FORMAT
                                       ", running %" G_GINT64_FORMAT
                                       ", mailbox %" G_GINT64_FORMAT,
                                       member_int(queue, "queued"),
                                       member_int(queue, "running"),
                                       member_int(queue, "mailbox"));
            if (queue != NULL && member_int(queue, "review") > 0)
                g_string_append_printf(out, ", in review %" G_GINT64_FORMAT,
                                       member_int(queue, "review"));
            g_string_append_c(out, '\n');
            render_live(out, lane, now, 6);
        }
    }

    return g_string_free(out, FALSE);
}
