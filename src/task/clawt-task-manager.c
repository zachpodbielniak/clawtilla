/*
 * clawt-task-manager.c - Every delegated task
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-task-manager.h"

enum {
    SIGNAL_TASK_CHANGED,
    SIGNAL_BLOCKERS_CHANGED,
    SIGNAL_REVIEW_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtTaskManager {
    GObject parent_instance;

    GHashTable *tasks;    /* task_id -> ClawtTask (owned) */
    GPtrArray  *order;    /* task ids, oldest first, unowned */
    guint       max_depth;

    /*
     * The progress guard's record per open task: see
     * clawt_task_manager_note_continuation().  Kept here rather than on
     * the task because nothing outside the guard reads it, and dropped
     * as soon as the task ends.
     */
    GHashTable *guards;   /* task_id -> ProgressGuard (owned) */

    /*
     * Where every change is written, and where anything no longer in
     * memory is read back from.  NULL in a manager built without one --
     * the unit tests, and a daemon whose store would not open -- which
     * then behaves exactly as it always did, in memory only.
     */
    ClawtTaskStore *store;

    /*
     * Tasks read back from the store on demand: finished long enough ago
     * to have left the live set, and asked about again (a result, a
     * status).  Kept apart from `tasks` so they never appear in a live
     * listing, and owned here so a borrowed pointer handed out by
     * clawt_task_manager_get() stays valid until the next forget.
     */
    GHashTable *cold;     /* task_id -> ClawtTask (owned) */

    /*
     * Each task's children, by parent id.  The cancel cascade and the
     * unfinished-children count walked every task for every node, which
     * was fine for a list a restart emptied and is not for one that
     * persists.
     */
    GHashTable *children; /* parent_id -> GPtrArray of child ids (owned) */
};

/*
 * How many continuations in a row may pass with no successful tool
 * call, and how many identical failures in a row, before the work is
 * ended as stalled.  The runtime contract's numbers, and fixed: they
 * describe a turn that is not moving, which is not something a fleet
 * tunes.
 */
#define CLAWT_TASK_IDLE_CONTINUATIONS (2)
#define CLAWT_TASK_SAME_ERRORS        (3)

typedef struct {
    guint  idle;      /* consecutive continuations with no work */
    guint  repeats;   /* consecutive continuations ending in @error */
    gchar *error;
} ProgressGuard;

static void
progress_guard_free(gpointer data)
{
    ProgressGuard *guard = data;

    g_free(guard->error);
    g_free(guard);
}

G_DEFINE_FINAL_TYPE(ClawtTaskManager, clawt_task_manager, G_TYPE_OBJECT)

ClawtTaskManager *
clawt_task_manager_new(void)
{
    return g_object_new(CLAWT_TYPE_TASK_MANAGER, NULL);
}

void
clawt_task_manager_set_max_depth(ClawtTaskManager *self, guint max_depth)
{
    g_return_if_fail(CLAWT_IS_TASK_MANAGER(self));

    self->max_depth = max_depth;
}

/*
 * Writes @task through to the store.  A failure is said and survived:
 * the task is still right in memory, and refusing the transition because
 * the disk would not take it would turn a full disk into stuck work.
 */
static void
persist(ClawtTaskManager *self, ClawtTask *task, const gchar *what)
{
    g_autoptr(GError) error = NULL;

    if (self->store == NULL)
        return;

    if (!clawt_task_store_save(self->store, task, what, &error))
        g_warning("task %s: could not save it: %s", clawt_task_get_id(task),
                  error->message);
}

/*
 * Saved before the signal, so a handler that reads the task back from
 * the store -- a client asking for its history -- sees the change it is
 * being told about.
 */
static void
emit_changed(ClawtTaskManager *self, ClawtTask *task, const gchar *what)
{
    persist(self, task, what);

    g_signal_emit(self, signals[SIGNAL_TASK_CHANGED], 0,
                  clawt_task_get_id(task), clawt_task_get_state(task));
}

/* Records @task as one of its parent's children. */
static void
index_child(ClawtTaskManager *self, ClawtTask *task)
{
    const gchar *parent_id = clawt_task_get_parent_id(task);
    GPtrArray *kids;

    if (parent_id == NULL)
        return;

    kids = g_hash_table_lookup(self->children, parent_id);

    if (kids == NULL) {
        kids = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(self->children, g_strdup(parent_id), kids);
    }

    g_ptr_array_add(kids, g_strdup(clawt_task_get_id(task)));
}

/* Takes ownership of @task and adds it to the live set, in order. */
static void
adopt(ClawtTaskManager *self, ClawtTask *task)
{
    g_hash_table_insert(self->tasks, g_strdup(clawt_task_get_id(task)), task);
    g_ptr_array_add(self->order, (gpointer)clawt_task_get_id(task));
    index_child(self, task);
}

/*
 * A note in the task's thread, when there is a store to keep it in.
 * Quietly nothing without one: the progress note and the result are
 * still on the task itself, as they always were.
 */
static void
note(ClawtTaskManager *self, ClawtTask *task, const gchar *author,
     const gchar *kind, const gchar *body)
{
    g_autoptr(GError) error = NULL;

    if (self->store == NULL || body == NULL || *body == '\0')
        return;

    if (clawt_task_store_add_note(self->store, clawt_task_get_id(task),
                                  author, kind, body, &error) == 0)
        g_warning("task %s: could not keep a note: %s",
                  clawt_task_get_id(task), error->message);
}

void
clawt_task_manager_set_store(ClawtTaskManager *self, ClawtTaskStore *store)
{
    g_return_if_fail(CLAWT_IS_TASK_MANAGER(self));
    g_return_if_fail(store == NULL || CLAWT_IS_TASK_STORE(store));

    g_set_object(&self->store, store);
}

ClawtTaskStore *
clawt_task_manager_get_store(ClawtTaskManager *self)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    return self->store;
}

guint
clawt_task_manager_load(ClawtTaskManager  *self,
                        gint64             finished_since,
                        GError           **error)
{
    g_autoptr(GPtrArray) tasks = NULL;
    guint loaded = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    if (self->store == NULL)
        return 0;

    tasks = clawt_task_store_load_live(self->store, finished_since, error);

    if (tasks == NULL)
        return 0;

    /*
     * Silently: no ::task-changed, because nothing changed.  A restored
     * task that emitted would send its settle notice again, raise its
     * incident again and wake its delegator for news it already had.
     */
    for (i = 0; i < tasks->len; i++) {
        ClawtTask *task = g_ptr_array_index(tasks, i);

        if (g_hash_table_contains(self->tasks, clawt_task_get_id(task)))
            continue;

        adopt(self, clawt_task_copy(task));
        loaded++;
    }

    return loaded;
}

/*
 * Whether @task is filed under @parent_id in the children index.
 */
static gboolean
indexed_under(ClawtTaskManager *self, ClawtTask *task, const gchar *parent_id)
{
    GPtrArray *kids;
    guint i;

    if (parent_id == NULL)
        return TRUE;

    kids = g_hash_table_lookup(self->children, parent_id);

    for (i = 0; kids != NULL && i < kids->len; i++)
        if (g_strcmp0(g_ptr_array_index(kids, i),
                      clawt_task_get_id(task)) == 0)
            return TRUE;

    return FALSE;
}

/*
 * Files @task under its current parent and nowhere else.  Only a parent
 * set after creation needs this, which is rare, so a walk over every
 * list is the right cost.
 */
static void
reindex_child(ClawtTaskManager *self, ClawtTask *task)
{
    GHashTableIter iter;
    gpointer value;

    if (indexed_under(self, task, clawt_task_get_parent_id(task)))
        return;

    g_hash_table_iter_init(&iter, self->children);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        GPtrArray *kids = value;
        guint i;

        for (i = 0; i < kids->len; i++) {
            if (g_strcmp0(g_ptr_array_index(kids, i),
                          clawt_task_get_id(task)) == 0) {
                g_ptr_array_remove_index(kids, i);
                break;
            }
        }
    }

    index_child(self, task);
}

void
clawt_task_manager_save(ClawtTaskManager *self,
                        ClawtTask        *task,
                        const gchar      *what)
{
    g_return_if_fail(CLAWT_IS_TASK_MANAGER(self));
    g_return_if_fail(task != NULL);

    /* A parent set after creation moves it in the children index. */
    if (g_hash_table_lookup(self->tasks, clawt_task_get_id(task)) == task)
        reindex_child(self, task);

    persist(self, task, what);
}

guint
clawt_task_manager_forget_finished(ClawtTaskManager *self,
                                   gint64            finished_before)
{
    g_autoptr(GPtrArray) gone = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    /*
     * Only with a store to read them back from.  Without one, forgetting
     * a task would lose it, which is the opposite of what this is for.
     */
    if (self->store == NULL)
        return 0;

    gone = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < self->order->len; i++) {
        ClawtTask *task = g_hash_table_lookup(self->tasks,
                                              g_ptr_array_index(self->order, i));

        if (task == NULL || !clawt_task_is_finished(task))
            continue;

        if (clawt_task_get_finished_at(task) >= finished_before)
            continue;

        /*
         * A finished task with a child still running stays: the child's
         * settle notice and the ancestor walks read it.
         */
        if (clawt_task_manager_count_unfinished_children(
                self, clawt_task_get_id(task)) > 0)
            continue;

        g_ptr_array_add(gone, g_strdup(clawt_task_get_id(task)));
    }

    for (i = 0; i < gone->len; i++) {
        const gchar *task_id = g_ptr_array_index(gone, i);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);
        const gchar *parent_id = clawt_task_get_parent_id(task);
        GPtrArray *kids = parent_id != NULL
            ? g_hash_table_lookup(self->children, parent_id) : NULL;
        guint k;

        /* The order array borrows the id the hash owns: drop it first. */
        g_ptr_array_remove(self->order, (gpointer)clawt_task_get_id(task));

        for (k = 0; kids != NULL && k < kids->len; k++) {
            if (g_strcmp0(g_ptr_array_index(kids, k), task_id) == 0) {
                g_ptr_array_remove_index(kids, k);
                break;
            }
        }

        if (kids != NULL && kids->len == 0)
            g_hash_table_remove(self->children, parent_id);

        g_hash_table_remove(self->guards, task_id);
        g_hash_table_remove(self->tasks, task_id);
    }

    /* Whatever was read back on demand goes too; it is all on disk. */
    g_hash_table_remove_all(self->cold);

    return gone->len;
}

GPtrArray *
clawt_task_manager_list_history(ClawtTaskManager  *self,
                                const ClawtTaskQuery *query,
                                GError           **error)
{
    GPtrArray *out;
    guint limit;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (self->store != NULL)
        return clawt_task_store_list(self->store, query, error);

    /*
     * No store: the same answer from memory, which is everything there
     * is, so the caller does not need two code paths.
     */
    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_task_free);
    limit = (query != NULL && query->limit > 0) ? query->limit
                                                : CLAWT_TASK_STORE_PAGE;

    for (i = self->order->len; i > 0 && out->len < limit; i--) {
        ClawtTask *task = g_hash_table_lookup(self->tasks,
                                              g_ptr_array_index(self->order,
                                                                i - 1));

        if (task == NULL)
            continue;
        if (query != NULL && query->assignee != NULL &&
            g_strcmp0(clawt_task_get_assignee(task), query->assignee) != 0)
            continue;
        if (query != NULL && query->state >= 0 &&
            (gint)clawt_task_get_state(task) != query->state)
            continue;
        if (query != NULL && query->before > 0 &&
            clawt_task_get_created_at(task) >= query->before)
            continue;

        g_ptr_array_add(out, clawt_task_copy(task));
    }

    return out;
}

JsonNode *
clawt_task_manager_journal(ClawtTaskManager  *self,
                           const gchar       *task_id,
                           GError           **error)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    if (self->store == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task store, so task %s has no history "
                    "beyond its current state", task_id);
        return NULL;
    }

    return clawt_task_store_journal(self->store, task_id, error);
}

ClawtTask *
clawt_task_manager_create(ClawtTaskManager  *self,
                          const gchar       *origin_agent,
                          const gchar       *assignee,
                          const gchar       *prompt,
                          const gchar       *parent_id,
                          GError           **error)
{
    ClawtTask *task;
    gint depth = 0;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(prompt != NULL, NULL);

    if (parent_id != NULL) {
        ClawtTask *parent = clawt_task_manager_get(self, parent_id);

        if (parent != NULL)
            depth = clawt_task_get_depth(parent) + 1;
    }

    /*
     * Depth is bounded here as well as on messages, because delegation can
     * nest without any message hop: a task spawning a task spawning a task
     * is three levels deep and each message in it is one hop.
     */
    if (self->max_depth > 0 && depth >= (gint)self->max_depth) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "delegation is already %d levels deep and the limit is "
                    "%u. Do this yourself rather than delegating again.",
                    depth, self->max_depth);
        return NULL;
    }

    task = clawt_task_new(origin_agent, assignee, prompt);
    clawt_task_set_depth(task, depth);
    clawt_task_set_parent_id(task, parent_id);

    /*
     * No assignee is the operator's backlog: a task somebody wrote down
     * and nobody has yet.  It waits there until clawt_task_manager_assign()
     * gives it one, and nothing that watches pending work -- a turn
     * timeout, a crash, a restart -- treats it as with anybody.
     */
    if (assignee == NULL)
        clawt_task_set_state(task, CLAWT_TASK_UNASSIGNED);

    adopt(self, task);

    {
        g_autofree gchar *what = assignee != NULL
            ? g_strdup_printf("created by %s for %s",
                              origin_agent != NULL ? origin_agent : "nobody",
                              assignee)
            : g_strdup_printf("put on the backlog by %s",
                              origin_agent != NULL ? origin_agent : "nobody");

        emit_changed(self, task, what);
    }

    return task;
}

ClawtTask *
clawt_task_manager_get(ClawtTaskManager *self, const gchar *task_id)
{
    ClawtTask *task;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (task_id == NULL)
        return NULL;

    task = g_hash_table_lookup(self->tasks, task_id);

    if (task != NULL)
        return task;

    task = g_hash_table_lookup(self->cold, task_id);

    if (task != NULL || self->store == NULL)
        return task;

    /*
     * A task that left the live set is still answerable: its result, its
     * reason, who cancelled it.  Read back once and kept in the cold set,
     * which is never listed, so asking about an old task does not put it
     * back on anybody's Tasks page.
     */
    task = clawt_task_store_get(self->store, task_id);

    if (task != NULL)
        g_hash_table_insert(self->cold, g_strdup(task_id), task);

    return task;
}

/*
 * One loop behind both public listings.  They differ only in which
 * field an agent id is compared against, and two copies of "newest
 * first, skip the finished ones" would differ exactly once.
 */
static GPtrArray *
list_filtered(ClawtTaskManager *self,
              const gchar      *assignee,
              const gchar      *involving,
              gboolean          include_finished)
{
    GPtrArray *out;
    guint i;

    out = g_ptr_array_new();

    /* Newest first: the thing somebody just asked about is at the top. */
    for (i = self->order->len; i > 0; i--) {
        const gchar *task_id = g_ptr_array_index(self->order, i - 1);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        if (task == NULL)
            continue;

        if (assignee != NULL &&
            g_strcmp0(clawt_task_get_assignee(task), assignee) != 0)
            continue;

        if (involving != NULL &&
            g_strcmp0(clawt_task_get_assignee(task), involving) != 0 &&
            g_strcmp0(clawt_task_get_origin(task), involving) != 0)
            continue;

        if (!include_finished && clawt_task_is_finished(task))
            continue;

        g_ptr_array_add(out, task);
    }

    return out;
}

GPtrArray *
clawt_task_manager_list(ClawtTaskManager *self,
                        const gchar      *assignee,
                        gboolean          include_finished)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    return list_filtered(self, assignee, NULL, include_finished);
}

GPtrArray *
clawt_task_manager_list_involving(ClawtTaskManager *self,
                                  const gchar      *agent_id,
                                  gboolean          include_finished)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return list_filtered(self, NULL, agent_id, include_finished);
}

gboolean
clawt_task_manager_start(ClawtTaskManager *self, const gchar *task_id)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_get_state(task) != CLAWT_TASK_PENDING)
        return FALSE;

    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    emit_changed(self, task, NULL);

    return TRUE;
}

gboolean
clawt_task_manager_complete(ClawtTaskManager *self,
                            const gchar      *task_id,
                            const gchar      *result)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL)
        return FALSE;

    /*
     * A task that already ended stays ended.  A late result arriving after
     * a cancellation must not quietly un-cancel it -- somebody stopped that
     * work on purpose.  And one that is not with its assignee -- on the
     * backlog, held for its blockers, or being reviewed -- is not the
     * assignee's to finish: a turn ending there must not overwrite a
     * result somebody is checking.
     */
    if (!clawt_task_state_is_active(clawt_task_get_state(task)))
        return FALSE;

    clawt_task_set_result(task, result);
    note(self, task, clawt_task_get_assignee(task), CLAWT_TASK_NOTE_RESULT,
         result);

    /*
     * Reviewed work is not done when its assignee says so: it goes to
     * its reviewer with the result, and only an accept completes it.
     */
    if (clawt_task_get_reviewer(task) != NULL) {
        g_autofree gchar *what = g_strdup_printf(
            "finished by %s; with %s for review",
            clawt_task_get_assignee(task) != NULL
                ? clawt_task_get_assignee(task) : "its assignee",
            g_strcmp0(clawt_task_get_reviewer(task), "user") == 0
                ? "the operator" : clawt_task_get_reviewer(task));

        clawt_task_set_state(task, CLAWT_TASK_REVIEW);
        emit_changed(self, task, what);
        g_signal_emit(self, signals[SIGNAL_REVIEW_REQUESTED], 0, task_id);
        return TRUE;
    }

    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    emit_changed(self, task, NULL);

    return TRUE;
}

/*
 * How many tasks this one handed on that have not ended.
 *
 * Direct children only, and that is enough by induction: a child cannot
 * finish while a child of its own is running, so an unfinished
 * grandchild keeps its parent unfinished and this sees the parent.  The
 * one gap is a branch that was cancelled or failed out from under a
 * still-running grandchild, and cancellation already cascades.
 */
guint
clawt_task_manager_count_unfinished_children(ClawtTaskManager *self,
                                             const gchar      *task_id)
{
    guint unfinished = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    if (task_id == NULL)
        return 0;

    {
        GPtrArray *kids = g_hash_table_lookup(self->children, task_id);

        for (i = 0; kids != NULL && i < kids->len; i++) {
            ClawtTask *child = g_hash_table_lookup(
                self->tasks, g_ptr_array_index(kids, i));

            if (child != NULL && !clawt_task_is_finished(child))
                unfinished++;
        }
    }

    return unfinished;
}

/*
 * Whether anything above @task was delegated by @agent_id.
 *
 * Bounded rather than trusted to terminate.  A parent always exists
 * before its child, so the chain cannot loop -- but a walk that hangs
 * the daemon is a worse answer than one that gives up, and this runs on
 * the main context while a client blocks.
 */
static gboolean
has_ancestor_from(ClawtTaskManager *self,
                  ClawtTask        *task,
                  const gchar      *agent_id)
{
    const gchar *walk = clawt_task_get_parent_id(task);
    guint steps;

    for (steps = 0; walk != NULL && steps < 1024; steps++) {
        ClawtTask *parent = clawt_task_manager_get(self, walk);

        if (parent == NULL)
            return FALSE;

        if (g_strcmp0(clawt_task_get_origin(parent), agent_id) == 0)
            return TRUE;

        walk = clawt_task_get_parent_id(parent);
    }

    if (walk != NULL)
        g_warning("task %s: parent chain is longer than 1024 tasks; giving "
                  "up walking it", clawt_task_get_id(task));

    return FALSE;
}

/*
 * The fan-out below the tasks this agent handed out.
 *
 * clawt_task_manager_list_involving() answers "what am I a party to",
 * which for a chief-of-staff stops one level down: it sees the task it
 * gave a lead and nothing the lead gave anybody.  So a fan-out was
 * invisible from the only place anybody was watching -- the parent read
 * `completed`, the children could not be listed at all, and nothing
 * distinguished finished from never-started.
 *
 * Kept separate from _list_involving() rather than folded into it,
 * because they are different claims: one is work this agent is doing or
 * waiting on, the other is work its work turned into.  A caller that
 * merged them would report somebody else's task as its own.
 */
GPtrArray *
clawt_task_manager_list_descendants(ClawtTaskManager *self,
                                    const gchar      *agent_id,
                                    gboolean          include_finished)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    out = g_ptr_array_new();

    for (i = self->order->len; i > 0; i--) {
        const gchar *task_id = g_ptr_array_index(self->order, i - 1);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        if (task == NULL)
            continue;

        if (!include_finished && clawt_task_is_finished(task))
            continue;

        /*
         * What the agent is itself a party to belongs to the other
         * listing.  Returning it here as well would have a chief seeing
         * its own delegation twice under two headings that mean
         * different things.
         */
        if (g_strcmp0(clawt_task_get_origin(task), agent_id) == 0 ||
            g_strcmp0(clawt_task_get_assignee(task), agent_id) == 0)
            continue;

        if (has_ancestor_from(self, task, agent_id))
            g_ptr_array_add(out, task);
    }

    return out;
}

/*
 * The assignee saying its turn is over and the work is not.
 *
 * Also marks the task running, because it is proof somebody picked it
 * up: clawtilla_delegate does not, so an agent-delegated task otherwise
 * reads `pending` for its whole life and a delegator that takes that
 * literally delegates it again.
 */
gboolean
clawt_task_manager_note_progress(ClawtTaskManager *self,
                                 const gchar      *task_id,
                                 const gchar      *progress)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    /* Progress is the assignee's to report, on work it has. */
    if (task == NULL || !clawt_task_state_is_active(clawt_task_get_state(task)))
        return FALSE;

    clawt_task_set_progress_note(task, progress);
    clawt_task_hold_completion(task);
    note(self, task, clawt_task_get_assignee(task), CLAWT_TASK_NOTE_PROGRESS,
         progress);

    if (clawt_task_get_state(task) == CLAWT_TASK_PENDING)
        clawt_task_set_state(task, CLAWT_TASK_RUNNING);

    emit_changed(self, task, "progress noted");

    return TRUE;
}

/*
 * Completing a task because its assignee's turn ended.
 *
 * An inference, not a report: an AI CLI cannot end a turn without
 * writing something, so the last thing it wrote is all there is to go
 * on.  It is right for the ordinary case -- work, answer, done -- and
 * wrong for every assignee that batches, which is the behaviour the rest
 * of the guidance asks for.  So the two ways it can be wrong are checked
 * here, once, rather than at whichever call site last noticed:
 *
 *   - the assignee said so, through clawtilla_task_progress;
 *   - the task has work outstanding that it handed on itself.
 *
 * The second could not fire at all until clawtilla_delegate started
 * recording a parent: every agent-delegated task was a root, so a fan-out
 * had no children to find and the parent closed over the top of them.
 *
 * A task that does end this way is marked as having been inferred, so
 * clawtilla_task_result can say which of "they said it was done" and
 * "they stopped talking" it is looking at.
 */
gboolean
clawt_task_manager_complete_on_turn_end(ClawtTaskManager  *self,
                                        const gchar       *task_id,
                                        const gchar       *who,
                                        const gchar       *result,
                                        gchar            **held_reason)
{
    ClawtTask *task;
    guint unfinished;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);
    g_return_val_if_fail(who != NULL, FALSE);

    if (held_reason != NULL)
        *held_reason = NULL;

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL || !clawt_task_state_is_active(clawt_task_get_state(task)))
        return FALSE;

    /*
     * Only the assignee's turn says anything about the work.  Everyone
     * in a task's thread ends turns there -- a delegator acknowledging a
     * progress note ends one -- and completing on whichever turn ended
     * next recorded "Thanks, carry on" as the result of work that was
     * still running.  Before the hold check, because the hold check
     * *consumes* the hold: a delegator's turn ending in the thread must
     * not spend what the assignee armed against its own next one.
     */
    if (g_strcmp0(clawt_task_get_assignee(task), who) != 0) {
        if (held_reason != NULL)
            *held_reason = g_strdup_printf(
                "the turn was %s's, and only its assignee %s can end it",
                who, clawt_task_get_assignee(task));

        return FALSE;
    }

    if (clawt_task_take_completion_hold(task)) {
        if (held_reason != NULL)
            *held_reason = g_strdup("its assignee reported progress rather "
                                    "than a result");

        /*
         * The note is deliberately *not* overwritten here.  The assignee
         * has just chosen words for this exact purpose through
         * clawtilla_task_progress; @result is whatever an AI CLI wrote
         * to end its turn, which is often "I will check back shortly".
         * Replacing the first with the second is a strictly worse answer
         * for the delegator reading it.
         */
        emit_changed(self, task, NULL);

        return FALSE;
    }

    unfinished = clawt_task_manager_count_unfinished_children(self, task_id);

    if (unfinished > 0) {
        if (held_reason != NULL)
            *held_reason = g_strdup_printf(
                "%u task%s it handed on %s still running", unfinished,
                unfinished == 1 ? "" : "s", unfinished == 1 ? "is" : "are");

        /*
         * And here it is recorded, because nothing else did: a task held
         * open by its own fan-out has no deliberate note, so the end of
         * the turn is the freshest thing anybody knows about it.
         */
        clawt_task_set_progress_note(task, result);
        emit_changed(self, task, NULL);

        return FALSE;
    }

    /*
     * Marked before the completion, not after: clawt_task_manager_complete()
     * emits ::task-changed, and a client that reads the task from that
     * signal would otherwise see the flag still clear on the one
     * notification that tells it the task ended.
     */
    clawt_task_set_result_inferred(task, TRUE);

    if (!clawt_task_manager_complete(self, task_id, result)) {
        clawt_task_set_result_inferred(task, FALSE);
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_task_manager_fail(ClawtTaskManager *self,
                        const gchar      *task_id,
                        const gchar      *reason)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_reason(task, reason);
    clawt_task_set_state(task, CLAWT_TASK_FAILED);
    emit_changed(self, task, reason);

    return TRUE;
}

gboolean
clawt_task_manager_retry(ClawtTaskManager  *self,
                         const gchar       *task_id,
                         const gchar       *who,
                         GError           **error)
{
    ClawtTask *task;
    ClawtTaskState state;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s; it may have been removed after "
                    "tasks.retention_days, or the id is mistyped",
                    task_id != NULL ? task_id : "(none)");
        return FALSE;
    }

    state = clawt_task_get_state(task);

    /*
     * Every state named, no default: -Wswitch is what makes the next
     * state somebody adds a decision rather than a silent "retryable".
     */
    switch (state) {
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_STALLED:
        break;

    case CLAWT_TASK_PENDING:
    case CLAWT_TASK_RUNNING:
    case CLAWT_TASK_UNASSIGNED:
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_REVIEW:
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s is %s, not failed or stalled: it has not "
                    "ended -- %s has it, or it is waiting on somebody -- so "
                    "there is nothing to retry yet", task_id,
                    clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, state),
                    clawt_task_get_assignee(task) != NULL
                        ? clawt_task_get_assignee(task) : "its assignee");
        return FALSE;

    case CLAWT_TASK_COMPLETED:
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s already completed; retrying it would redo "
                    "finished work. Delegate anything further as new work.",
                    task_id);
        return FALSE;

    case CLAWT_TASK_CANCELLED:
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s was cancelled%s%s, which stopped it on "
                    "purpose; delegate it again as new work if it is "
                    "wanted after all", task_id,
                    clawt_task_get_cancelled_by(task) != NULL ? " by " : "",
                    clawt_task_get_cancelled_by(task) != NULL
                        ? clawt_task_get_cancelled_by(task) : "");
        return FALSE;
    }

    clawt_task_reopen(task, who);

    {
        g_autofree gchar *what = g_strdup_printf("retried by %s",
                                                 who != NULL ? who : "nobody");

        emit_changed(self, task, what);
    }

    return TRUE;
}

/*
 * A task the loop guard ended.
 *
 * Its own verb rather than clawt_task_manager_fail(), because "the two
 * agents went in circles and clawtilla stopped them" and "the work broke"
 * need different answers from whoever reads the receipt: the first is
 * work somebody can pick up and finish, the second needs diagnosing.
 * Recording a stall as a failure sends the reader hunting a bug that is
 * not there.
 */
gboolean
clawt_task_manager_stall(ClawtTaskManager *self,
                         const gchar      *task_id,
                         const gchar      *reason)
{
    ClawtTask *task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_is_finished(task))
        return FALSE;

    clawt_task_set_reason(task, reason);
    clawt_task_set_state(task, CLAWT_TASK_STALLED);
    emit_changed(self, task, reason);

    return TRUE;
}

guint
clawt_task_manager_cancel(ClawtTaskManager *self,
                          const gchar      *task_id,
                          const gchar      *reason,
                          const gchar      *who)
{
    ClawtTask *task;
    guint cancelled = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);

    task = clawt_task_manager_get(self, task_id);
    if (task == NULL)
        return 0;

    if (!clawt_task_is_finished(task)) {
        clawt_task_set_reason(task, reason);

        /*
         * Before the state, so the ::task-changed handler that turns a
         * terminal transition into a settle notice can already see who
         * did it -- a delegator is not told about its own cancel, and
         * the state emission is the one moment that decision is made.
         */
        clawt_task_set_cancelled_by(task, who);
        clawt_task_set_state(task, CLAWT_TASK_CANCELLED);

        {
            g_autofree gchar *what = g_strdup_printf(
                "cancelled by %s%s%s", who != NULL ? who : "nobody",
                reason != NULL ? ": " : "", reason != NULL ? reason : "");

            emit_changed(self, task, what);
        }

        cancelled++;
    }

    /*
     * And everything it spawned.  Cancelling only the parent leaves its
     * children running and reporting into a task nobody is waiting for --
     * which is the runaway cancellation exists to stop.
     */
    {
        GPtrArray *indexed = g_hash_table_lookup(self->children, task_id);
        g_autoptr(GPtrArray) kids = g_ptr_array_new_with_free_func(g_free);

        /*
         * Copied first: cancelling a child emits, and a handler that
         * creates a task would grow the array being walked.
         */
        for (i = 0; indexed != NULL && i < indexed->len; i++)
            g_ptr_array_add(kids, g_strdup(g_ptr_array_index(indexed, i)));

        /*
         * The original canceller, all the way down.  A cascade-cancelled
         * child's delegator is usually somewhere inside the chain being
         * cancelled -- and it is exactly the party that must stop
         * waiting, so the notice logic needs to see that somebody
         * *else* ended this one.
         */
        for (i = 0; i < kids->len; i++)
            cancelled += clawt_task_manager_cancel(
                self, g_ptr_array_index(kids, i), reason, who);
    }

    return cancelled;
}

gboolean
clawt_task_manager_assign(ClawtTaskManager  *self,
                          const gchar       *task_id,
                          const gchar       *assignee,
                          const gchar       *who,
                          GError           **error)
{
    ClawtTask *task;
    g_autofree gchar *what = NULL;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);
    g_return_val_if_fail(assignee != NULL, FALSE);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s", task_id != NULL ? task_id : "");
        return FALSE;
    }

    /*
     * Only from the backlog.  A task somebody already has is moved with
     * a handoff, which tells the one losing it; assigning over the top
     * would leave an agent working on something that is no longer its.
     */
    if (clawt_task_get_state(task) != CLAWT_TASK_UNASSIGNED) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s is %s, not on the backlog: only an unassigned "
                    "task is assigned this way", task_id,
                    clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                       clawt_task_get_state(task)));
        return FALSE;
    }

    clawt_task_transfer_owner(task, assignee);
    clawt_task_set_state(task, CLAWT_TASK_PENDING);

    what = g_strdup_printf("assigned to %s by %s", assignee,
                           who != NULL ? who : "nobody");
    emit_changed(self, task, what);

    return TRUE;
}

gint64
clawt_task_manager_add_note(ClawtTaskManager  *self,
                            const gchar       *task_id,
                            const gchar       *author,
                            const gchar       *kind,
                            const gchar       *body,
                            GError           **error)
{
    ClawtTask *task;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);
    g_return_val_if_fail(kind != NULL, 0);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s", task_id != NULL ? task_id : "");
        return 0;
    }

    if (body == NULL || *body == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a note needs some text");
        return 0;
    }

    if (self->store == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "notes are kept in the task store, and this "
                            "daemon has none");
        return 0;
    }

    return clawt_task_store_add_note(self->store, task_id, author, kind,
                                     body, error);
}

JsonNode *
clawt_task_manager_notes(ClawtTaskManager  *self,
                         const gchar       *task_id,
                         GError           **error)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    if (self->store == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task store, so task %s has no notes",
                    task_id);
        return NULL;
    }

    return clawt_task_store_notes(self->store, task_id, error);
}

/* ── Review ──────────────────────────────────────────────────────── */

ClawtTaskReviewOutcome
clawt_task_manager_review(ClawtTaskManager  *self,
                          const gchar       *task_id,
                          const gchar       *who,
                          gboolean           accept,
                          const gchar       *notes,
                          guint              max_rounds,
                          GError           **error)
{
    ClawtTask *task;
    g_autofree gchar *what = NULL;
    guint round;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self),
                         CLAWT_TASK_REVIEW_REFUSED);
    g_return_val_if_fail(who != NULL, CLAWT_TASK_REVIEW_REFUSED);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s", task_id != NULL ? task_id : "");
        return CLAWT_TASK_REVIEW_REFUSED;
    }

    if (clawt_task_get_state(task) != CLAWT_TASK_REVIEW) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s is %s, not in review: there is no result "
                    "waiting for a verdict", task_id,
                    clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                       clawt_task_get_state(task)));
        return CLAWT_TASK_REVIEW_REFUSED;
    }

    /*
     * The reviewer's verdict, or the operator's, who may always step in.
     * Never the assignee's: reviewing your own work is not a review.
     */
    if (g_strcmp0(who, clawt_task_get_reviewer(task)) != 0 &&
        g_strcmp0(who, "user") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "task %s is %s's to review, not %s's", task_id,
                    clawt_task_get_reviewer(task), who);
        return CLAWT_TASK_REVIEW_REFUSED;
    }

    if (accept) {
        what = g_strdup_printf("accepted by %s", who);
        note(self, task, who, CLAWT_TASK_NOTE_REVIEW,
             (notes != NULL && *notes != '\0') ? notes : "accepted");
        clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
        emit_changed(self, task, what);
        return CLAWT_TASK_REVIEW_ACCEPTED;
    }

    if (notes == NULL || *notes == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "asking for changes needs notes saying what to "
                            "change");
        return CLAWT_TASK_REVIEW_REFUSED;
    }

    round = clawt_task_get_review_round(task) + 1;
    clawt_task_set_review_round(task, round);
    note(self, task, who, CLAWT_TASK_NOTE_REVIEW, notes);

    /*
     * Bounded like every other back-and-forth: past the limit a person
     * decides, rather than an agent and its reviewer going round again.
     */
    if (max_rounds > 0 && round >= max_rounds &&
        g_strcmp0(clawt_task_get_reviewer(task), "user") != 0) {
        clawt_task_set_reviewer(task, "user");
        what = g_strdup_printf("changes asked for %u times; now with the "
                               "operator to decide", round);
        emit_changed(self, task, what);
        g_signal_emit(self, signals[SIGNAL_REVIEW_REQUESTED], 0, task_id);
        return CLAWT_TASK_REVIEW_ESCALATED;
    }

    what = g_strdup_printf("changes requested by %s (round %u)", who, round);
    clawt_task_set_state(task, CLAWT_TASK_PENDING);
    emit_changed(self, task, what);

    return CLAWT_TASK_REVIEW_CHANGES;
}

/* ── Blockers ────────────────────────────────────────────────────── */

static gboolean
need_store(ClawtTaskManager *self, GError **error)
{
    if (self->store != NULL)
        return TRUE;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                        "blockers are kept in the task store, and this "
                        "daemon has none");
    return FALSE;
}

/*
 * Whether @target is among @from's blockers, directly or further back.
 * Bounded like every other walk here: a store edited by hand could hold
 * a loop the link check never saw, and a hang is worse than a refusal.
 */
static gboolean
reaches(ClawtTaskManager *self, const gchar *from, const gchar *target)
{
    g_autoptr(GPtrArray) queue = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal, g_free,
                                                       NULL);
    guint i;

    g_ptr_array_add(queue, g_strdup(from));

    for (i = 0; i < queue->len && i < 4096; i++) {
        const gchar *at = g_ptr_array_index(queue, i);
        g_autoptr(GPtrArray) blockers = NULL;
        guint b;

        if (g_strcmp0(at, target) == 0)
            return TRUE;

        if (!g_hash_table_add(seen, g_strdup(at)))
            continue;

        blockers = clawt_task_store_blockers_of(self->store, at);

        for (b = 0; b < blockers->len; b++)
            g_ptr_array_add(queue, g_strdup(g_ptr_array_index(blockers, b)));
    }

    return FALSE;
}

gboolean
clawt_task_manager_link(ClawtTaskManager  *self,
                        const gchar       *task_id,
                        const gchar       *blocker_id,
                        GError           **error)
{
    ClawtTask *task;
    ClawtTask *blocker;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    if (!need_store(self, error))
        return FALSE;

    task = clawt_task_manager_get(self, task_id);
    blocker = clawt_task_manager_get(self, blocker_id);

    if (task == NULL || blocker == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s",
                    task == NULL ? (task_id != NULL ? task_id : "")
                                 : blocker_id);
        return FALSE;
    }

    if (g_strcmp0(task_id, blocker_id) == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a task cannot wait for itself");
        return FALSE;
    }

    if (clawt_task_is_finished(task)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s has already ended; there is nothing left for "
                    "it to wait for", task_id);
        return FALSE;
    }

    /*
     * A cycle would hold both tasks for ever, and nothing would say so:
     * each is waiting, and waiting is a state nobody checks on.
     */
    if (reaches(self, blocker_id, task_id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "task %s already waits, directly or further back, for "
                    "task %s; making it wait the other way round would "
                    "hold both for ever", blocker_id, task_id);
        return FALSE;
    }

    if (!clawt_task_store_add_link(self->store, blocker_id, task_id, error))
        return FALSE;

    {
        g_autofree gchar *what = g_strdup_printf("waits for %s", blocker_id);

        persist(self, task, what);
    }

    g_signal_emit(self, signals[SIGNAL_BLOCKERS_CHANGED], 0, task_id);
    return TRUE;
}

gboolean
clawt_task_manager_unlink(ClawtTaskManager  *self,
                          const gchar       *task_id,
                          const gchar       *blocker_id,
                          GError           **error)
{
    ClawtTask *task;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    if (!need_store(self, error))
        return FALSE;

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no task %s", task_id != NULL ? task_id : "");
        return FALSE;
    }

    if (!clawt_task_store_remove_link(self->store, blocker_id, task_id,
                                      error))
        return FALSE;

    {
        g_autofree gchar *what = g_strdup_printf("no longer waits for %s",
                                                 blocker_id);

        persist(self, task, what);
    }

    g_signal_emit(self, signals[SIGNAL_BLOCKERS_CHANGED], 0, task_id);
    return TRUE;
}

GPtrArray *
clawt_task_manager_blockers(ClawtTaskManager *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (self->store == NULL || task_id == NULL)
        return g_ptr_array_new_with_free_func(g_free);

    return clawt_task_store_blockers_of(self->store, task_id);
}

GPtrArray *
clawt_task_manager_dependents(ClawtTaskManager *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (self->store == NULL || task_id == NULL)
        return g_ptr_array_new_with_free_func(g_free);

    return clawt_task_store_blocked_by(self->store, task_id);
}

GPtrArray *
clawt_task_manager_waiting_on(ClawtTaskManager *self, const gchar *task_id)
{
    g_autoptr(GPtrArray) blockers = NULL;
    GPtrArray *open = g_ptr_array_new_with_free_func(g_free);
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), open);

    blockers = clawt_task_manager_blockers(self, task_id);

    /*
     * Only `completed` releases.  A blocker that failed or was cancelled
     * still holds the task: its work was meant to come first, and it did
     * not happen.  One that is gone altogether -- pruned long after it
     * finished -- holds nothing.
     */
    for (i = 0; i < blockers->len; i++) {
        const gchar *id = g_ptr_array_index(blockers, i);
        ClawtTask *blocker = clawt_task_manager_get(self, id);

        if (blocker != NULL &&
            clawt_task_get_state(blocker) != CLAWT_TASK_COMPLETED)
            g_ptr_array_add(open, g_strdup(id));
    }

    return open;
}

gboolean
clawt_task_manager_block(ClawtTaskManager *self, const gchar *task_id)
{
    ClawtTask *task;
    ClawtTaskState state;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL)
        return FALSE;

    state = clawt_task_get_state(task);

    if (state != CLAWT_TASK_PENDING && state != CLAWT_TASK_UNASSIGNED)
        return FALSE;

    clawt_task_set_state(task, CLAWT_TASK_BLOCKED);
    emit_changed(self, task, "held until the tasks it waits for complete");

    return TRUE;
}

gboolean
clawt_task_manager_unblock(ClawtTaskManager *self, const gchar *task_id)
{
    ClawtTask *task;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL || clawt_task_get_state(task) != CLAWT_TASK_BLOCKED)
        return FALSE;

    /* Back to the backlog if nobody had it yet, to its assignee if so. */
    clawt_task_set_state(task, clawt_task_get_assignee(task) != NULL
                                   ? CLAWT_TASK_PENDING
                                   : CLAWT_TASK_UNASSIGNED);
    emit_changed(self, task, "released: everything it waited for completed");

    return TRUE;
}

gboolean
clawt_task_manager_hold_delivery(ClawtTaskManager  *self,
                                 const gchar       *task_id,
                                 JsonNode          *delivery,
                                 GError           **error)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

    if (!need_store(self, error))
        return FALSE;

    return clawt_task_store_hold_delivery(self->store, task_id, delivery,
                                          error);
}

JsonNode *
clawt_task_manager_take_delivery(ClawtTaskManager *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), NULL);

    if (self->store == NULL)
        return NULL;

    return clawt_task_store_take_delivery(self->store, task_id);
}

guint
clawt_task_manager_orphan_agent_tasks(ClawtTaskManager *self,
                                      const gchar      *agent_id)
{
    guint failed = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), 0);
    g_return_val_if_fail(agent_id != NULL, 0);

    for (i = 0; i < self->order->len; i++) {
        const gchar *task_id = g_ptr_array_index(self->order, i);
        ClawtTask *task = g_hash_table_lookup(self->tasks, task_id);

        /* Only what it had: a blocked or in-review task is not lost with it. */
        if (task == NULL ||
            !clawt_task_state_is_active(clawt_task_get_state(task)))
            continue;

        if (g_strcmp0(clawt_task_get_assignee(task), agent_id) != 0)
            continue;

        /*
         * Failed with a reason rather than silently dropped.  Whoever
         * delegated it is waiting, and "the agent stopped" is the answer
         * they need.
         */
        if (clawt_task_manager_fail(self, task_id,
                                    "the agent handling this stopped before "
                                    "finishing"))
            failed++;
    }

    return failed;
}

gboolean
clawt_task_manager_note_continuation(
	ClawtTaskManager  *self,
	const gchar       *task_id,
	const gchar       *who,
	gboolean           known,
	guint              work_calls,
	const gchar       *error
){
	ClawtTask *task;
	ProgressGuard *guard;
	g_autofree gchar *reason = NULL;

	g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);

	if (task_id == NULL)
		return FALSE;

	task = g_hash_table_lookup(self->tasks, task_id);

	if (task == NULL ||
	    !clawt_task_state_is_active(clawt_task_get_state(task))) {
		g_hash_table_remove(self->guards, task_id);
		return FALSE;
	}

	/* The assignee's continuation, and nobody else's turn in the thread. */
	if (g_strcmp0(clawt_task_get_assignee(task), who) != 0)
		return FALSE;

	/*
	 * Waiting on its own children is not a continuation: a parent woken
	 * by each child's settle notice writes "still waiting on the rest"
	 * and ends, and counting that would stop a fan-out of three when two
	 * had reported.  It clears the guard, as any turn that waits does.
	 */
	if (clawt_task_manager_count_unfinished_children(self, task_id) > 0) {
		g_hash_table_remove(self->guards, task_id);
		return FALSE;
	}

	/*
	 * A turn nobody observed -- no streamed step and no clawtilla call
	 * -- is neither a strike nor a reset.  Reading it as zero would stop
	 * a task whose agent simply does not stream; reading it as progress
	 * would let one silent turn wipe out the record of two idle ones.
	 */
	if (!known)
		return FALSE;

	guard = g_hash_table_lookup(self->guards, task_id);

	if (guard == NULL) {
		guard = g_new0(ProgressGuard, 1);
		g_hash_table_insert(self->guards, g_strdup(task_id), guard);
	}

	guard->idle = (work_calls == 0) ? guard->idle + 1 : 0;

	if (error != NULL && *error != '\0') {
		if (g_strcmp0(guard->error, error) == 0) {
			guard->repeats++;
		} else {
			g_free(guard->error);
			guard->error = g_strdup(error);
			guard->repeats = 1;
		}
	} else {
		g_clear_pointer(&guard->error, g_free);
		guard->repeats = 0;
	}

	if (guard->repeats >= CLAWT_TASK_SAME_ERRORS)
		reason = g_strdup_printf(
			"Stopped after %u identical errors in a row: %s",
			guard->repeats, guard->error);
	else if (guard->idle >= CLAWT_TASK_IDLE_CONTINUATIONS)
		reason = g_strdup_printf(
			"Stopped after %u continuations in a row without a "
			"successful tool call: the work was being held open and "
			"nothing was moving it.", guard->idle);

	if (reason == NULL)
		return FALSE;

	g_hash_table_remove(self->guards, task_id);

	return clawt_task_manager_stall(self, task_id, reason);
}

static void
clawt_task_manager_finalize(GObject *object)
{
    ClawtTaskManager *self = CLAWT_TASK_MANAGER(object);

    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_clear_pointer(&self->guards, g_hash_table_unref);
    g_clear_pointer(&self->children, g_hash_table_unref);
    g_clear_pointer(&self->cold, g_hash_table_unref);
    g_clear_pointer(&self->tasks, g_hash_table_unref);
    g_clear_object(&self->store);

    G_OBJECT_CLASS(clawt_task_manager_parent_class)->finalize(object);
}

static void
clawt_task_manager_class_init(ClawtTaskManagerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_task_manager_finalize;

    /**
     * ClawtTaskManager::task-changed:
     * @self: the manager
     * @task_id: which task
     * @state: its new state
     */
    signals[SIGNAL_TASK_CHANGED] =
        g_signal_new("task-changed", CLAWT_TYPE_TASK_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /**
     * ClawtTaskManager::blockers-changed:
     * @self: the manager
     * @task_id: the task whose blockers were added to or taken away
     *
     * A link was added or removed (clawt_task_manager_link(),
     * clawt_task_manager_unlink()).  Its own signal, because a blocker
     * removed can make a held task ready without any task changing state.
     */
    signals[SIGNAL_BLOCKERS_CHANGED] =
        g_signal_new("blockers-changed", CLAWT_TYPE_TASK_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * ClawtTaskManager::review-requested:
     * @self: the manager
     * @task_id: the task whose result is now with its reviewer
     *
     * A result went to a reviewer: its assignee finished it, or changes
     * were asked for so often that the operator now decides.  Its own
     * signal, so the reviewer is asked once per result rather than on
     * every change made while the task sits in review.  Since 0.38.0.
     */
    signals[SIGNAL_REVIEW_REQUESTED] =
        g_signal_new("review-requested", CLAWT_TYPE_TASK_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_task_manager_init(ClawtTaskManager *self)
{
    self->tasks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify)clawt_task_free);

    /* Ids are borrowed from the tasks, which own them and outlive this. */
    self->order = g_ptr_array_new();
    self->max_depth = 8;
    self->guards = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         progress_guard_free);
    self->cold = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)clawt_task_free);
    self->children = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify)g_ptr_array_unref);
}

gboolean
clawt_task_manager_record(ClawtTaskManager *self,
                          const gchar      *task_id,
                          const gchar      *what)
{
    ClawtTask *task;

    g_return_val_if_fail(CLAWT_IS_TASK_MANAGER(self), FALSE);
    g_return_val_if_fail(what != NULL, FALSE);

    task = clawt_task_manager_get(self, task_id);

    if (task == NULL)
        return FALSE;

    /* Saved and journalled; no state changed, so no signal. */
    persist(self, task, what);

    return TRUE;
}
