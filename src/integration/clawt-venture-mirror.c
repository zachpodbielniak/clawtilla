/*
 * clawt-venture-mirror.c - Tasks copied into VENTURE tickets
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One outbox row at a time, as a chain of requests:
 *
 *   row ─► who is acting? ─► whoami (once per account)
 *        ─► sync:  no ticket ─► repository (pinned, or resolve each
 *                               remote) ─► POST ticket (409: adopt it)
 *                  ticket    ─► PATCH what changed since the receipt
 *                               (409: re-read the version, once more)
 *        ─► note:  the ticket first, then POST ticket_comment
 *
 * and every answer is sorted into one of: done, try again later (with
 * a doubling backoff to half an hour), or dead -- kept, with the reason,
 * until the operator retries the task.  Sequential on purpose: VENTURE
 * sees the fleet's work in the order it happened, and one slow answer
 * holds the drain, not the agents.
 */

#include "clawtilla.h"
#include "integration/clawt-venture-mirror.h"

#include <string.h>

#define CLAWT_VENTURE_MIRROR_BATCH          (20)
#define CLAWT_VENTURE_MIRROR_BACKOFF_FIRST  (30)
#define CLAWT_VENTURE_MIRROR_BACKOFF_MAX    (1800)
#define CLAWT_VENTURE_MIRROR_PROGRESS_GAP   (600)
#define CLAWT_VENTURE_MIRROR_DESCRIPTION_MAX (8192)
#define CLAWT_VENTURE_MIRROR_COMMENT_MAX    (16384)

/* ── Accounts ────────────────────────────────────────────────────── */

typedef struct {
    gchar   *agent_id;
    gchar   *instance;
    gchar   *base;
    gchar   *token;
    gchar   *user;     /* whoami's username, once known */
    gchar   *actor;    /* whoami's actor, e.g. "API token #12" */
    gboolean known;
} Account;

static void
account_free(gpointer data)
{
    Account *account = data;

    if (account == NULL)
        return;

    g_free(account->agent_id);
    g_free(account->instance);
    g_free(account->base);

    /* The token is wiped, not merely freed: it is the one value here
     * worth reading out of a core dump. */
    if (account->token != NULL) {
        memset(account->token, 0, strlen(account->token));
        g_free(account->token);
    }

    g_free(account->user);
    g_free(account->actor);
    g_free(account);
}

/* ── The object ──────────────────────────────────────────────────── */

struct _ClawtVentureMirror {
    GObject         parent_instance;

    ClawtTaskStore *store;
    GMainContext   *context;
    GSource        *timer;
    GSource        *kick;

    GHashTable     *accounts;       /* agent id -> Account */
    GHashTable     *last_progress;  /* task id -> gint64 time, in a pointer */

    ClawtVentureMirrorRequestFunc request;
    gpointer                      request_data;
    GDestroyNotify                request_notify;

    ClawtVentureMirrorTargetFunc  target;
    gpointer                      target_data;
    GDestroyNotify                target_notify;

    GQueue         *rows;           /* ClawtVentureOutboxRow, being drained */
    gboolean        busy;
    gboolean        warned_no_transport;
};

G_DEFINE_FINAL_TYPE(ClawtVentureMirror, clawt_venture_mirror, G_TYPE_OBJECT)

/* ── One job: one outbox row being sent ──────────────────────────── */

typedef enum {
    STEP_WHOAMI_CREATOR,
    STEP_WHOAMI_WRITER,
    STEP_REPO,
    STEP_CREATE,
    STEP_ADOPT,
    STEP_UPDATE,
    STEP_REFRESH,
    STEP_COMMENT,
    STEP_COMMENT_ADOPT
} Step;

typedef struct {
    ClawtVentureMirror    *mirror;   /* ref held */
    ClawtVentureOutboxRow *row;
    ClawtTask             *task;
    ClawtVentureRef       *ref;
    JsonNode              *note;     /* for a `note` row */
    Account               *creator;  /* not owned */
    Account               *writer;   /* not owned */
    Account               *author;   /* not owned: the note's */
    Account               *acting;   /* not owned: who the request in
                                      * flight is sent as */
    Step                   step;
    GStrv                  remotes;
    guint                  remote;
    gint64                 repo_id;
    gint64                 organization_id;
    guint                  conflicts;
} Job;

static void job_next(Job *job);
static void drain_one(ClawtVentureMirror *self);

static void
job_free(Job *job)
{
    clawt_venture_outbox_row_free(job->row);
    g_clear_pointer(&job->task, clawt_task_free);
    g_clear_pointer(&job->ref, clawt_venture_ref_free);
    g_clear_pointer(&job->note, json_node_unref);
    g_strfreev(job->remotes);
    g_object_unref(job->mirror);
    g_free(job);
}

/* ── Small helpers ───────────────────────────────────────────────── */

/* A node holding a new, empty object -- and the only reference to it. */
static JsonNode *
object_node_new(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);

    json_node_take_object(node, json_object_new());
    return node;
}

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

const gchar *
clawt_venture_mirror_ticket_status(gint state)
{
    switch ((ClawtTaskState)state) {
    case CLAWT_TASK_UNASSIGNED: return "triage";
    case CLAWT_TASK_PENDING:    return "todo";
    case CLAWT_TASK_RUNNING:    return "in_progress";
    case CLAWT_TASK_REVIEW:     return "review";
    case CLAWT_TASK_COMPLETED:  return "done";
    case CLAWT_TASK_CANCELLED:  return "cancelled";
    case CLAWT_TASK_BLOCKED:
    case CLAWT_TASK_FAILED:
    case CLAWT_TASK_STALLED:
    default:                    return "blocked";
    }
}

/* The stable reference a ticket carries for its task. */
static gchar *
task_ref_text(const gchar *task_id)
{
    return g_strdup_printf("clawtilla:%s:%s", g_get_host_name(), task_id);
}

static gchar *
note_ref_text(gint64 note_id)
{
    return g_strdup_printf("clawtilla:%s:note:%" G_GINT64_FORMAT,
                           g_get_host_name(), note_id);
}

/* The parsed object of an answer, or NULL. */
static JsonObject *
answer_object(const gchar *body, JsonNode **holder)
{
    *holder = (body != NULL) ? json_from_string(body, NULL) : NULL;

    if (*holder == NULL || !JSON_NODE_HOLDS_OBJECT(*holder))
        return NULL;

    return json_node_get_object(*holder);
}

/* The first record of a list answer: a bare array or {items|data|records}. */
static JsonObject *
answer_first(const gchar *body, JsonNode **holder)
{
    JsonArray *array = NULL;

    *holder = (body != NULL) ? json_from_string(body, NULL) : NULL;

    if (*holder == NULL)
        return NULL;

    if (JSON_NODE_HOLDS_ARRAY(*holder)) {
        array = json_node_get_array(*holder);
    } else if (JSON_NODE_HOLDS_OBJECT(*holder)) {
        JsonObject *o = json_node_get_object(*holder);
        static const gchar *const keys[] = { "items", "data", "records", NULL };
        guint i;

        for (i = 0; keys[i] != NULL && array == NULL; i++) {
            if (json_object_has_member(o, keys[i]) &&
                JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, keys[i])))
                array = json_object_get_array_member(o, keys[i]);
        }
    }

    if (array == NULL || json_array_get_length(array) == 0)
        return NULL;

    return json_array_get_object_element(array, 0);
}

/* VENTURE's own sentence for a refusal, when it gave one. */
static gchar *
answer_message(guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o = answer_object(body, &holder);
    const gchar *message = (o != NULL)
        ? json_object_get_string_member_with_default(o, "message", NULL) : NULL;

    if (message != NULL)
        return g_strdup_printf("VENTURE answered %u: %s", status, message);

    if (status == 0)
        return g_strdup("VENTURE did not answer");

    return g_strdup_printf("VENTURE answered %u", status);
}

/* ── Ending a job ────────────────────────────────────────────────── */

static void
job_done(Job *job)
{
    ClawtVentureMirror *self = job->mirror;

    clawt_task_store_outbox_finish(self->store, job->row->id, job->row->gen);
    job_free(job);
    self->busy = FALSE;
    clawt_venture_mirror_kick(self);
}

/* Not now: the row waits out a doubling backoff. */
static void
job_later(Job *job, const gchar *why)
{
    ClawtVentureMirror *self = job->mirror;
    gint attempts = job->row->attempts + 1;
    gint64 wait = CLAWT_VENTURE_MIRROR_BACKOFF_FIRST;
    gint i;

    for (i = 1; i < attempts && wait < CLAWT_VENTURE_MIRROR_BACKOFF_MAX; i++)
        wait *= 2;

    wait = MIN(wait, CLAWT_VENTURE_MIRROR_BACKOFF_MAX);

    clawt_task_store_outbox_retry(self->store, job->row->id, attempts,
                                  now_seconds() + wait, why);
    job_free(job);
    self->busy = FALSE;
    clawt_venture_mirror_kick(self);
}

/*
 * Given up: kept with @why for the operator.  Said in the log too,
 * because a dead row is otherwise visible only on the task.
 */
static void
job_dead(Job *job, const gchar *why)
{
    ClawtVentureMirror *self = job->mirror;

    g_warning("venture mirror: task %s: %s", job->row->task_id, why);
    clawt_task_store_outbox_kill(self->store, job->row->id, why);
    job_free(job);
    self->busy = FALSE;
    clawt_venture_mirror_kick(self);
}

/* Records that this task is not mirrored, and why, then ends the job. */
static void
job_skip(Job *job, const gchar *why)
{
    ClawtVentureRef ref = { 0 };

    ref.task_id = job->row->task_id;
    ref.instance = (job->creator != NULL) ? job->creator->instance : NULL;
    ref.state = (gchar *)"skipped";
    ref.reason = (gchar *)why;

    clawt_task_store_put_venture_ref(job->mirror->store, &ref, NULL);
    job_done(job);
}

/*
 * The usual sorting of an answer that was not what the step wanted.
 * A 202 means the token's role stages writes -- a viewer -- which no
 * retry will change.
 */
static void
job_refused(Job *job, guint status, const gchar *body, const gchar *what)
{
    g_autofree gchar *message = answer_message(status, body);
    g_autofree gchar *why = NULL;
    Account *account = job->acting;

    if (status == 0 || status == 408 || status == 429 || status >= 500) {
        why = g_strdup_printf("%s: %s", what, message);
        job_later(job, why);
        return;
    }

    if (status == 202)
        why = g_strdup_printf("%s: the token of %s stages its writes (a "
                              "viewer?); the mirror needs an editor "
                              "membership", what,
                              account != NULL ? account->agent_id : "?");
    else if (status == 401)
        why = g_strdup_printf("%s: VENTURE refused %s's token; mint a new "
                              "one and run `clawtilla connector connect %s`",
                              what,
                              account != NULL ? account->agent_id : "?",
                              account != NULL ? account->instance : "?");
    else
        why = g_strdup_printf("%s: %s", what, message);

    job_dead(job, why);
}

/* ── Sending ─────────────────────────────────────────────────────── */

static void on_answer(guint status, const gchar *body, gpointer data);

static void
job_send(Job *job, Account *account, const gchar *method, const gchar *path,
         const gchar *key, JsonNode *body)
{
    ClawtVentureMirror *self = job->mirror;
    g_autofree gchar *url = g_strconcat(account->base, path, NULL);
    g_autofree gchar *text = (body != NULL) ? json_to_string(body, FALSE)
                                            : NULL;

    job->acting = account;
    self->request(self, method, url, account->token, key, text,
                  on_answer, job, self->request_data);
}

/* ── The ticket's fields ─────────────────────────────────────────── */

/* The fields the mirror owns, as it would write them now. */
static JsonObject *
desired_fields(Job *job)
{
    JsonObject *o = json_object_new();
    const gchar *title = clawt_task_get_title(job->task);
    const gchar *assignee = clawt_task_get_assignee(job->task);
    Account *owner = (assignee != NULL)
        ? g_hash_table_lookup(job->mirror->accounts, assignee) : NULL;

    json_object_set_string_member(o, "title",
        (title != NULL && *title != '\0') ? title : clawt_task_get_id(job->task));
    json_object_set_string_member(o, "status",
        clawt_venture_mirror_ticket_status(
            (gint)clawt_task_get_state(job->task)));

    /*
     * The assignee as VENTURE knows them: the username its own token
     * answered whoami with.  An agent without an account is nobody in
     * VENTURE, and naming its agent id would assign the ticket to
     * whichever person happens to share it.
     */
    json_object_set_string_member(o, "assignee",
        (owner != NULL && owner->user != NULL) ? owner->user : "");

    return o;
}

static gchar *
description_of(ClawtTask *task)
{
    const gchar *prompt = clawt_task_get_prompt(task);
    g_autofree gchar *body = g_strndup(prompt != NULL ? prompt : "",
                                       CLAWT_VENTURE_MIRROR_DESCRIPTION_MAX);

    return g_strdup_printf("%s\n\n-- mirrored from clawtilla task %s",
                           body, clawt_task_get_id(task));
}

/* ── The steps ───────────────────────────────────────────────────── */

static void
step_whoami(Job *job, Account *account)
{
    job_send(job, account, "GET", "/api/v1/whoami", NULL, NULL);
}

/* The repository: pinned, or the next remote resolved. */
static void
step_repo(Job *job)
{
    g_autofree gchar *path = NULL;

    job->step = STEP_REPO;

    if (job->repo_id > 0 && job->organization_id == 0) {
        path = g_strdup_printf("/api/v1/forge_repo/%" G_GINT64_FORMAT,
                               job->repo_id);
        job_send(job, job->creator, "GET", path, NULL, NULL);
        return;
    }

    if (job->remotes == NULL || job->remotes[job->remote] == NULL) {
        job_skip(job, (job->remotes == NULL || job->remotes[0] == NULL)
            ? "no repository: the task has no checkout, and its project "
              "pins no venture_repo_id"
            : "no repository in VENTURE matches the task's remotes, or "
              "the delegator's account cannot see it");
        return;
    }

    {
        g_autofree gchar *escaped = g_uri_escape_string(
            job->remotes[job->remote], NULL, FALSE);

        path = g_strdup_printf("/api/v1/forge_repo/resolve?url=%s", escaped);
    }

    job_send(job, job->creator, "GET", path, NULL, NULL);
}

static void
step_create(Job *job)
{
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *o = desired_fields(job);
    g_autofree gchar *ref = task_ref_text(clawt_task_get_id(job->task));
    g_autofree gchar *description = description_of(job->task);
    g_autofree gchar *key = g_strconcat("clawtilla-task-",
                                        clawt_task_get_id(job->task), NULL);
    const gchar *parent = clawt_task_get_parent_id(job->task);
    const gchar *issue_type = "task";

    json_object_set_string_member(o, "description", description);
    json_object_set_string_member(o, "kind", "internal");
    json_object_set_int_member(o, "organization_id", job->organization_id);
    json_object_set_int_member(o, "repo_id", job->repo_id);
    json_object_set_string_member(o, "external_ref", ref);

    /*
     * Under its parent's ticket, when the parent is mirrored.  VENTURE
     * allows no subtask under a subtask, so a grandchild goes under the
     * nearest ticket that is not one.
     */
    if (parent != NULL) {
        g_autoptr(ClawtVentureRef) up =
            clawt_task_store_venture_ref(job->mirror->store, parent);

        if (up != NULL && up->ticket_id > 0) {
            g_autoptr(JsonNode) snap = (up->snapshot != NULL)
                ? json_from_string(up->snapshot, NULL) : NULL;
            JsonObject *so = (snap != NULL && JSON_NODE_HOLDS_OBJECT(snap))
                ? json_node_get_object(snap) : NULL;
            gint64 under = up->ticket_id;

            if (so != NULL &&
                g_strcmp0(json_object_get_string_member_with_default(
                              so, "issue_type", NULL), "subtask") == 0)
                under = json_object_get_int_member_with_default(
                    so, "parent_ticket", up->ticket_id);

            json_object_set_int_member(o, "parent_id", under);
            issue_type = "subtask";
        }
    }

    json_object_set_string_member(o, "issue_type", issue_type);
    json_node_take_object(node, o);

    job->step = STEP_CREATE;
    job_send(job, job->creator, "POST", "/api/v1/ticket", key, node);
}

/* The receipt: what was written, so a later change is told from it. */
static void
remember(Job *job, gint64 ticket, gint64 version, JsonObject *written,
         JsonObject *created)
{
    g_autoptr(JsonNode) snap = NULL;
    JsonObject *so;
    ClawtVentureRef ref = { 0 };
    g_autofree gchar *text = NULL;
    g_autofree gchar *url = NULL;

    snap = (job->ref != NULL && job->ref->snapshot != NULL)
        ? json_from_string(job->ref->snapshot, NULL) : NULL;

    if (snap == NULL || !JSON_NODE_HOLDS_OBJECT(snap)) {
        g_clear_pointer(&snap, json_node_unref);
        snap = object_node_new();
    }

    so = json_node_get_object(snap);

    if (written != NULL) {
        GList *members = json_object_get_members(written);
        GList *l;

        for (l = members; l != NULL; l = l->next)
            json_object_set_member(so, l->data,
                json_node_copy(json_object_get_member(written, l->data)));

        g_list_free(members);
    }

    if (created != NULL) {
        json_object_set_string_member(so, "issue_type",
            json_object_get_string_member_with_default(created, "issue_type",
                                                       "task"));
        if (json_object_has_member(created, "parent_id"))
            json_object_set_int_member(so, "parent_ticket",
                json_object_get_int_member(created, "parent_id"));
    }

    text = json_to_string(snap, FALSE);
    url = g_strdup_printf("%s/tickets/%" G_GINT64_FORMAT,
                          job->writer->base, ticket);

    ref.task_id = job->row->task_id;
    ref.instance = job->writer->instance;
    ref.ticket_id = ticket;
    ref.version = version;
    ref.snapshot = text;
    ref.state = (gchar *)"synced";
    ref.url = url;

    clawt_task_store_put_venture_ref(job->mirror->store, &ref, NULL);

    g_clear_pointer(&job->ref, clawt_venture_ref_free);
    job->ref = clawt_task_store_venture_ref(job->mirror->store,
                                            job->row->task_id);
}

/* The fields that differ from the receipt; NULL when none do. */
static JsonObject *
changed_fields(Job *job)
{
    JsonObject *want = desired_fields(job);
    g_autoptr(JsonNode) snap = (job->ref->snapshot != NULL)
        ? json_from_string(job->ref->snapshot, NULL) : NULL;
    JsonObject *so = (snap != NULL && JSON_NODE_HOLDS_OBJECT(snap))
        ? json_node_get_object(snap) : NULL;
    GList *members = json_object_get_members(want);
    GList *l;

    for (l = members; l != NULL; l = l->next) {
        const gchar *name = l->data;

        if (so != NULL && json_object_has_member(so, name) &&
            g_strcmp0(json_object_get_string_member_with_default(so, name, NULL),
                      json_object_get_string_member(want, name)) == 0)
            json_object_remove_member(want, name);
    }

    g_list_free(members);

    if (json_object_get_size(want) == 0) {
        json_object_unref(want);
        return NULL;
    }

    return want;
}

static void
step_update(Job *job)
{
    JsonObject *changed = changed_fields(job);
    g_autoptr(JsonNode) node = NULL;
    g_autofree gchar *path = NULL;

    if (changed == NULL) {
        if (g_strcmp0(job->row->kind, "note") == 0) {
            job->step = STEP_COMMENT;
            job_next(job);
        } else {
            job_done(job);
        }
        return;
    }

    json_object_set_int_member(changed, "version", job->ref->version);
    node = json_node_init_object(json_node_alloc(), changed);
    json_object_unref(changed);

    path = g_strdup_printf("/api/v1/ticket/%" G_GINT64_FORMAT,
                           job->ref->ticket_id);
    job->step = STEP_UPDATE;
    job_send(job, job->writer, "PATCH", path, NULL, node);
}

static void
step_comment(Job *job)
{
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *note = json_node_get_object(job->note);
    JsonObject *o = json_object_new();
    const gchar *author = json_object_get_string_member_with_default(
        note, "author", NULL);
    g_autofree gchar *body = g_strndup(
        json_object_get_string_member_with_default(note, "body", ""),
        CLAWT_VENTURE_MIRROR_COMMENT_MAX);
    g_autofree gchar *text = g_strdup_printf("[%s%s%s]\n\n%s",
        json_object_get_string_member_with_default(note, "kind", "note"),
        author != NULL ? " by " : "", author != NULL ? author : "", body);
    g_autofree gchar *ref = note_ref_text(job->row->note_id);
    g_autofree gchar *key = g_strdup_printf("clawtilla-note-%" G_GINT64_FORMAT,
                                            job->row->note_id);

    json_object_set_int_member(o, "ticket_id", job->ref->ticket_id);
    json_object_set_string_member(o, "body", text);
    json_object_set_boolean_member(o, "internal", TRUE);
    json_object_set_string_member(o, "external_ref", ref);
    json_node_take_object(node, o);

    job->step = STEP_COMMENT;
    job_send(job, job->author != NULL ? job->author : job->writer, "POST",
             "/api/v1/ticket_comment", key, node);
}

/*
 * What to do next for the job's row, from where it stands.  Called to
 * begin, and after each step that does not itself send.
 */
static void
job_next(Job *job)
{
    if (job->creator != NULL && !job->creator->known) {
        job->step = STEP_WHOAMI_CREATOR;
        step_whoami(job, job->creator);
        return;
    }

    if (job->writer != NULL && !job->writer->known) {
        job->step = STEP_WHOAMI_WRITER;
        step_whoami(job, job->writer);
        return;
    }

    if (job->author != NULL && !job->author->known) {
        job->step = STEP_WHOAMI_WRITER;
        step_whoami(job, job->author);
        return;
    }

    if (job->ref == NULL || g_strcmp0(job->ref->state, "synced") != 0 ||
        job->ref->ticket_id == 0) {
        step_repo(job);
        return;
    }

    if (job->step == STEP_COMMENT) {
        step_comment(job);
        return;
    }

    step_update(job);
}

/* ── Answers ─────────────────────────────────────────────────────── */

static void
on_whoami(Job *job, Account *account, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;
    JsonObject *user;

    if (status != 200) {
        job_refused(job, status, body, "asking VENTURE who this token is");
        return;
    }

    o = answer_object(body, &holder);
    user = (o != NULL && json_object_has_member(o, "user") &&
            JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "user")))
        ? json_object_get_object_member(o, "user") : NULL;

    g_free(account->actor);
    account->actor = g_strdup(o != NULL
        ? json_object_get_string_member_with_default(o, "actor", NULL) : NULL);
    g_free(account->user);
    account->user = g_strdup(user != NULL
        ? json_object_get_string_member_with_default(user, "username", NULL)
        : NULL);
    account->known = TRUE;

    job_next(job);
}

static void
on_repo(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status == 404) {
        /* Pinned and gone, or this remote unknown: the next remote. */
        if (job->repo_id > 0 && job->organization_id == 0) {
            job_skip(job, "the project's venture_repo_id is not a "
                          "repository the delegator's account can see");
            return;
        }

        job->remote++;
        step_repo(job);
        return;
    }

    if (status != 200) {
        job_refused(job, status, body, "finding the task's repository");
        return;
    }

    o = answer_object(body, &holder);

    if (o == NULL) {
        job_later(job, "VENTURE's repository answer was not an object");
        return;
    }

    job->repo_id = json_object_get_int_member_with_default(o, "id",
                                                           job->repo_id);
    job->organization_id = json_object_get_int_member_with_default(
        o, "organization_id", 0);

    step_create(job);
}

static void
on_created(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status == 409) {
        /* Made by an earlier attempt: find it by its reference. */
        g_autofree gchar *ref = task_ref_text(job->row->task_id);
        g_autofree gchar *escaped = g_uri_escape_string(ref, NULL, FALSE);
        g_autofree gchar *path = g_strdup_printf(
            "/api/v1/ticket?external_ref=%s", escaped);

        job->step = STEP_ADOPT;
        job_send(job, job->creator, "GET", path, NULL, NULL);
        return;
    }

    if (status != 200 && status != 201) {
        job_refused(job, status, body, "filing the ticket");
        return;
    }

    o = answer_object(body, &holder);

    if (o == NULL || json_object_get_int_member_with_default(o, "id", 0) <= 0) {
        job_later(job, "VENTURE's answer to the ticket did not name it");
        return;
    }

    {
        JsonObject *written = desired_fields(job);

        remember(job, json_object_get_int_member(o, "id"),
                 json_object_get_int_member_with_default(o, "version", 1),
                 written, o);
        json_object_unref(written);
    }

    /* A note row filed the ticket on the way to its comment. */
    if (g_strcmp0(job->row->kind, "note") == 0) {
        job->step = STEP_COMMENT;
        job_next(job);
        return;
    }

    job_done(job);
}

static void
on_adopted(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status != 200) {
        job_refused(job, status, body, "finding the ticket already filed");
        return;
    }

    o = answer_first(body, &holder);

    if (o == NULL) {
        job_dead(job, "VENTURE says the ticket exists, but no ticket carries "
                      "this task's reference");
        return;
    }

    /* No receipt yet: everything it holds is compared, and set. */
    remember(job, json_object_get_int_member(o, "id"),
             json_object_get_int_member_with_default(o, "version", 1),
             NULL, o);

    if (g_strcmp0(job->row->kind, "note") == 0)
        job->step = STEP_COMMENT;

    job_next(job);
}

static void
on_updated(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status == 409 && job->conflicts < 2) {
        g_autofree gchar *path = g_strdup_printf(
            "/api/v1/ticket/%" G_GINT64_FORMAT, job->ref->ticket_id);

        /* Somebody changed it: the version again, and the same fields. */
        job->conflicts++;
        job->step = STEP_REFRESH;
        job_send(job, job->writer, "GET", path, NULL, NULL);
        return;
    }

    if (status == 404) {
        ClawtVentureRef ref = *job->ref;

        ref.state = (gchar *)"dead";
        ref.reason = (gchar *)"the ticket was deleted in VENTURE";
        clawt_task_store_put_venture_ref(job->mirror->store, &ref, NULL);
        job_dead(job, "the ticket was deleted in VENTURE");
        return;
    }

    if (status != 200) {
        job_refused(job, status, body, "updating the ticket");
        return;
    }

    o = answer_object(body, &holder);

    {
        JsonObject *written = changed_fields(job);

        remember(job, job->ref->ticket_id,
                 (o != NULL) ? json_object_get_int_member_with_default(
                     o, "version", job->ref->version + 1)
                             : job->ref->version + 1,
                 written, NULL);

        if (written != NULL)
            json_object_unref(written);
    }

    if (g_strcmp0(job->row->kind, "note") == 0) {
        job->step = STEP_COMMENT;
        job_next(job);
        return;
    }

    job_done(job);
}

static void
on_refreshed(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status != 200) {
        job_refused(job, status, body, "re-reading the ticket");
        return;
    }

    o = answer_object(body, &holder);
    job->ref->version = (o != NULL)
        ? json_object_get_int_member_with_default(o, "version",
                                                  job->ref->version)
        : job->ref->version;

    step_update(job);
}

static void
on_commented(Job *job, guint status, const gchar *body)
{
    g_autoptr(JsonNode) holder = NULL;
    JsonObject *o;

    if (status == 409 && job->step == STEP_COMMENT) {
        g_autofree gchar *ref = note_ref_text(job->row->note_id);
        g_autofree gchar *escaped = g_uri_escape_string(ref, NULL, FALSE);
        g_autofree gchar *path = g_strdup_printf(
            "/api/v1/ticket_comment?external_ref=%s", escaped);

        job->step = STEP_COMMENT_ADOPT;
        job_send(job, job->writer, "GET", path, NULL, NULL);
        return;
    }

    if (status != 200 && status != 201) {
        job_refused(job, status, body, "adding the comment");
        return;
    }

    o = (job->step == STEP_COMMENT_ADOPT) ? answer_first(body, &holder)
                                          : answer_object(body, &holder);

    if (o != NULL && json_object_get_int_member_with_default(o, "id", 0) > 0)
        clawt_task_store_put_venture_comment(job->mirror->store,
                                             job->row->note_id,
                                             job->row->task_id,
                                             json_object_get_int_member(o, "id"));

    job_done(job);
}

static void
on_answer(guint status, const gchar *body, gpointer data)
{
    Job *job = data;

    switch (job->step) {
    case STEP_WHOAMI_CREATOR:
        on_whoami(job, job->creator, status, body);
        break;
    case STEP_WHOAMI_WRITER:
        on_whoami(job, job->acting, status, body);
        break;
    case STEP_REPO:
        on_repo(job, status, body);
        break;
    case STEP_CREATE:
        on_created(job, status, body);
        break;
    case STEP_ADOPT:
        on_adopted(job, status, body);
        break;
    case STEP_UPDATE:
        on_updated(job, status, body);
        break;
    case STEP_REFRESH:
        on_refreshed(job, status, body);
        break;
    case STEP_COMMENT:
    case STEP_COMMENT_ADOPT:
        on_commented(job, status, body);
        break;
    default:
        job_dead(job, "the mirror lost its place");
        break;
    }
}

/* ── Starting a job ──────────────────────────────────────────────── */

/*
 * Whether a note is worth a comment.  Results, reviews and comments
 * always; progress at most once per ten minutes per task, because a
 * chatty agent's every step is noise on a ticket.
 */
static gboolean
note_wanted(ClawtVentureMirror *self, Job *job)
{
    JsonObject *note = json_node_get_object(job->note);
    const gchar *kind = json_object_get_string_member_with_default(note,
                                                                   "kind", "");
    gint64 now = now_seconds();
    gpointer last;

    if (g_strcmp0(kind, "progress") != 0)
        return TRUE;

    last = g_hash_table_lookup(self->last_progress, job->row->task_id);

    if (last != NULL &&
        now - (gint64)GPOINTER_TO_SIZE(last) < CLAWT_VENTURE_MIRROR_PROGRESS_GAP)
        return FALSE;

    g_hash_table_replace(self->last_progress, g_strdup(job->row->task_id),
                         GSIZE_TO_POINTER((gsize)now));
    return TRUE;
}

static void
job_begin(ClawtVentureMirror *self, ClawtVentureOutboxRow *row)
{
    Job *job = g_new0(Job, 1);
    const gchar *origin;
    const gchar *assignee;

    job->mirror = g_object_ref(self);
    job->row = row;
    job->task = clawt_task_store_get(self->store, row->task_id);

    /* Pruned since: nothing left to tell. */
    if (job->task == NULL) {
        job_done(job);
        return;
    }

    job->ref = clawt_task_store_venture_ref(self->store, row->task_id);

    /*
     * Who acts.  The delegator files the ticket and the assignee keeps it
     * -- each as its own account -- with whichever has an account
     * standing in for the other.  Neither: nobody in VENTURE is doing
     * this work, and it is not mirrored.
     */
    origin = clawt_task_get_origin(job->task);
    assignee = clawt_task_get_assignee(job->task);

    job->creator = (origin != NULL)
        ? g_hash_table_lookup(self->accounts, origin) : NULL;
    job->writer = (assignee != NULL)
        ? g_hash_table_lookup(self->accounts, assignee) : NULL;

    if (job->creator == NULL)
        job->creator = job->writer;
    if (job->writer == NULL)
        job->writer = job->creator;

    /*
     * One ticket lives on one server.  Two agents whose connectors point
     * at different VENTUREs cannot share it, so the delegator's account
     * does all of it.
     */
    if (job->creator != NULL && job->writer != NULL &&
        g_strcmp0(job->creator->base, job->writer->base) != 0)
        job->writer = job->creator;

    if (job->creator == NULL) {
        /* Recorded once; a task nobody mirrors needs no ref per save. */
        if (job->ref == NULL)
            job_skip(job, "neither the delegator nor the assignee has a "
                          "VENTURE connector with mirror_tasks on");
        else
            job_done(job);
        return;
    }

    /*
     * A note: dropped when it is already a comment, when it is progress
     * too soon after the last, or when its task is not mirrored -- a
     * task found unmatched is looked at again on its next save, not on
     * every note.  Its author writes it, when the author has an account
     * on the same server.
     */
    if (g_strcmp0(row->kind, "note") == 0) {
        const gchar *author;

        job->note = clawt_task_store_note(self->store, row->note_id);

        if (job->note == NULL ||
            clawt_task_store_venture_comment(self->store, row->note_id) > 0 ||
            (job->ref != NULL && g_strcmp0(job->ref->state, "skipped") == 0) ||
            !note_wanted(self, job)) {
            job_done(job);
            return;
        }

        author = json_object_get_string_member_with_default(
            json_node_get_object(job->note), "author", NULL);
        job->author = (author != NULL)
            ? g_hash_table_lookup(self->accounts, author) : NULL;

        if (job->author != NULL &&
            g_strcmp0(job->author->base, job->writer->base) != 0)
            job->author = NULL;

        job->step = STEP_COMMENT;
    }

    if (job->ref == NULL || job->ref->ticket_id == 0) {
        if (self->target != NULL)
            job->remotes = self->target(self, row->task_id, &job->repo_id,
                                        self->target_data);
    }

    job_next(job);
}

static void
drain_one(ClawtVentureMirror *self)
{
    ClawtVentureOutboxRow *row;

    if (self->busy)
        return;

    row = g_queue_pop_head(self->rows);

    if (row == NULL)
        return;

    self->busy = TRUE;
    job_begin(self, row);
}

/*
 * Next row: the batch in hand, else whatever is due now -- so a task's
 * rows go one after another, each once its predecessor is done, rather
 * than one per tick.  Reached from an idle source after every job, never
 * recursively, so a run of rows that need no network does not grow the
 * stack.
 */
static void
drain(ClawtVentureMirror *self)
{
    g_autoptr(GPtrArray) rows = NULL;
    guint i;

    if (self->busy)
        return;

    if (!g_queue_is_empty(self->rows)) {
        drain_one(self);
        return;
    }

    if (self->request == NULL) {
        if (!self->warned_no_transport)
            g_warning("venture mirror: no transport installed; the outbox "
                      "is kept and nothing is sent");
        self->warned_no_transport = TRUE;
        return;
    }

    if (g_hash_table_size(self->accounts) == 0)
        return;

    rows = clawt_task_store_outbox_due(self->store, now_seconds(),
                                       CLAWT_VENTURE_MIRROR_BATCH, NULL);

    for (i = 0; rows != NULL && i < rows->len; i++)
        g_queue_push_tail(self->rows, g_ptr_array_steal_index(rows, i--));

    drain_one(self);
}

static gboolean
on_tick(gpointer data)
{
    drain(CLAWT_VENTURE_MIRROR(data));
    return G_SOURCE_CONTINUE;
}

static gboolean
on_kick(gpointer data)
{
    ClawtVentureMirror *self = CLAWT_VENTURE_MIRROR(data);

    g_clear_pointer(&self->kick, g_source_unref);
    drain(self);
    return G_SOURCE_REMOVE;
}

/* ── Public ──────────────────────────────────────────────────────── */

ClawtVentureMirror *
clawt_venture_mirror_new(ClawtTaskStore *store, GMainContext *context)
{
    ClawtVentureMirror *self;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(store), NULL);

    self = g_object_new(CLAWT_TYPE_VENTURE_MIRROR, NULL);
    self->store = g_object_ref(store);
    self->context = (context != NULL) ? g_main_context_ref(context) : NULL;

    return self;
}

void
clawt_venture_mirror_set_request_func(ClawtVentureMirror           *self,
                                      ClawtVentureMirrorRequestFunc func,
                                      gpointer                      user_data,
                                      GDestroyNotify                notify)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    if (self->request_notify != NULL)
        self->request_notify(self->request_data);

    self->request = func;
    self->request_data = user_data;
    self->request_notify = notify;
}

void
clawt_venture_mirror_set_target_func(ClawtVentureMirror          *self,
                                     ClawtVentureMirrorTargetFunc func,
                                     gpointer                     user_data,
                                     GDestroyNotify               notify)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    if (self->target_notify != NULL)
        self->target_notify(self->target_data);

    self->target = func;
    self->target_data = user_data;
    self->target_notify = notify;
}

void
clawt_venture_mirror_set_account(ClawtVentureMirror *self,
                                 const gchar        *agent_id,
                                 const gchar        *instance,
                                 const gchar        *base_url,
                                 const gchar        *token)
{
    Account *account;
    Account *old;

    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));
    g_return_if_fail(agent_id != NULL && base_url != NULL && token != NULL);

    /*
     * The same token again keeps what whoami said; a new one (a rotation)
     * is asked afresh.  Not replaced while a job holds it: a reload in
     * the middle of a request must not free what the answer will read.
     */
    old = g_hash_table_lookup(self->accounts, agent_id);

    if (old != NULL && g_strcmp0(old->token, token) == 0 &&
        g_strcmp0(old->base, base_url) == 0) {
        g_free(old->instance);
        old->instance = g_strdup(instance);
        return;
    }

    if (old != NULL && self->busy) {
        g_free(old->instance);
        old->instance = g_strdup(instance);
        g_free(old->base);
        old->base = g_strdup(base_url);
        memset(old->token, 0, strlen(old->token));
        g_free(old->token);
        old->token = g_strdup(token);
        old->known = FALSE;
        return;
    }

    account = g_new0(Account, 1);
    account->agent_id = g_strdup(agent_id);
    account->instance = g_strdup(instance);
    account->base = g_strdup(base_url);
    account->token = g_strdup(token);

    g_hash_table_replace(self->accounts, g_strdup(agent_id), account);
}

void
clawt_venture_mirror_clear_accounts(ClawtVentureMirror *self)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    /* Kept while a job holds them; the next sync replaces them. */
    if (!self->busy)
        g_hash_table_remove_all(self->accounts);
}

gboolean
clawt_venture_mirror_has_account(ClawtVentureMirror *self,
                                 const gchar        *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_MIRROR(self), FALSE);

    return agent_id != NULL &&
           g_hash_table_contains(self->accounts, agent_id);
}

guint
clawt_venture_mirror_account_count(ClawtVentureMirror *self)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_MIRROR(self), 0);

    return g_hash_table_size(self->accounts);
}

const gchar *
clawt_venture_mirror_account_user(ClawtVentureMirror *self,
                                  const gchar        *agent_id)
{
    Account *account;

    g_return_val_if_fail(CLAWT_IS_VENTURE_MIRROR(self), NULL);

    account = (agent_id != NULL)
        ? g_hash_table_lookup(self->accounts, agent_id) : NULL;

    return (account != NULL) ? account->user : NULL;
}

void
clawt_venture_mirror_start(ClawtVentureMirror *self, guint interval_seconds)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    if (self->timer != NULL)
        return;

    self->timer = g_timeout_source_new_seconds(MAX(interval_seconds, 1));
    g_source_set_callback(self->timer, on_tick, self, NULL);
    g_source_attach(self->timer, self->context);
}

void
clawt_venture_mirror_stop(ClawtVentureMirror *self)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_clear_pointer(&self->timer, g_source_unref);
    }

    if (self->kick != NULL) {
        g_source_destroy(self->kick);
        g_clear_pointer(&self->kick, g_source_unref);
    }
}

void
clawt_venture_mirror_kick(ClawtVentureMirror *self)
{
    g_return_if_fail(CLAWT_IS_VENTURE_MIRROR(self));

    if (self->kick != NULL)
        return;

    self->kick = g_idle_source_new();
    g_source_set_callback(self->kick, on_kick, self, NULL);
    g_source_attach(self->kick, self->context);
}

gboolean
clawt_venture_mirror_is_busy(ClawtVentureMirror *self)
{
    g_return_val_if_fail(CLAWT_IS_VENTURE_MIRROR(self), FALSE);

    return self->busy || !g_queue_is_empty(self->rows);
}

JsonNode *
clawt_venture_mirror_describe(ClawtTaskStore *store, const gchar *task_id)
{
    g_autoptr(ClawtVentureRef) ref = NULL;
    g_autofree gchar *last_error = NULL;
    g_autoptr(JsonBuilder) b = NULL;
    guint queued = 0;
    guint dead = 0;
    const gchar *state;

    g_return_val_if_fail(CLAWT_IS_TASK_STORE(store), NULL);
    g_return_val_if_fail(task_id != NULL, NULL);

    ref = clawt_task_store_venture_ref(store, task_id);
    clawt_task_store_outbox_state(store, task_id, &queued, &dead, &last_error);

    if (ref == NULL && queued == 0 && dead == 0)
        return NULL;

    if (dead > 0)
        state = "dead";
    else if (ref != NULL && g_strcmp0(ref->state, "dead") == 0)
        state = "dead";
    else if (ref != NULL && g_strcmp0(ref->state, "skipped") == 0)
        state = "skipped";
    else if (queued > 0)
        state = "queued";
    else
        state = "synced";

    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "state");
    json_builder_add_string_value(b, state);

    if (ref != NULL && ref->ticket_id > 0) {
        json_builder_set_member_name(b, "ticket");
        json_builder_add_int_value(b, ref->ticket_id);
    }

    if (ref != NULL && ref->url != NULL) {
        json_builder_set_member_name(b, "url");
        json_builder_add_string_value(b, ref->url);
    }

    if (ref != NULL && ref->instance != NULL) {
        json_builder_set_member_name(b, "instance");
        json_builder_add_string_value(b, ref->instance);
    }

    if (g_strcmp0(state, "dead") == 0 || g_strcmp0(state, "skipped") == 0 ||
        (g_strcmp0(state, "queued") == 0 && last_error != NULL)) {
        const gchar *why = (dead > 0 || g_strcmp0(state, "queued") == 0)
            ? last_error : ref->reason;

        if (why != NULL) {
            json_builder_set_member_name(b, "reason");
            json_builder_add_string_value(b, why);
        }
    }

    json_builder_end_object(b);
    return json_builder_get_root(b);
}

/* ── GObject ─────────────────────────────────────────────────────── */

static void
clawt_venture_mirror_finalize(GObject *object)
{
    ClawtVentureMirror *self = CLAWT_VENTURE_MIRROR(object);

    clawt_venture_mirror_stop(self);

    if (self->request_notify != NULL)
        self->request_notify(self->request_data);
    if (self->target_notify != NULL)
        self->target_notify(self->target_data);

    g_queue_free_full(self->rows,
                      (GDestroyNotify)clawt_venture_outbox_row_free);
    g_hash_table_unref(self->accounts);
    g_hash_table_unref(self->last_progress);
    g_clear_object(&self->store);
    g_clear_pointer(&self->context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_venture_mirror_parent_class)->finalize(object);
}

static void
clawt_venture_mirror_class_init(ClawtVentureMirrorClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_venture_mirror_finalize;
}

static void
clawt_venture_mirror_init(ClawtVentureMirror *self)
{
    self->accounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           account_free);
    self->last_progress = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);
    self->rows = g_queue_new();
}
