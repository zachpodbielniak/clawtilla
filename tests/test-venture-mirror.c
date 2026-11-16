/*
 * test-venture-mirror.c - Tasks copied into VENTURE tickets
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The mirror against a fake VENTURE: a request function that answers
 * from an idle callback, as the real transport answers from the loop,
 * and records every request -- which token, which body, which key.  No
 * socket anywhere.  The two failures that matter: a write made as the
 * wrong agent, and a change the outbox loses or sends twice.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/* ── The fake VENTURE ────────────────────────────────────────────── */

typedef struct {
    gchar *method;
    gchar *url;
    gchar *token;
    gchar *key;
    gchar *body;
} Sent;

static void
sent_free(gpointer data)
{
    Sent *sent = data;

    g_free(sent->method);
    g_free(sent->url);
    g_free(sent->token);
    g_free(sent->key);
    g_free(sent->body);
    g_free(sent);
}

typedef struct {
    gchar          *dir;
    ClawtTaskStore *store;
    ClawtVentureMirror *mirror;
    GPtrArray      *sent;          /* Sent */
    gchar          *remote;        /* what every task's checkout says */
    gint64          next_ticket;
    gint64          next_comment;
    guint           create_conflicts;   /* answer this many POST ticket 409 */
    guint           patch_conflicts;    /* and this many PATCH 409 */
    guint           unavailable;        /* and this many anything 503 */
} Fixture;

typedef struct {
    ClawtVentureMirrorReply reply;
    gpointer                data;
    guint                   status;
    gchar                  *body;
} Answer;

static gboolean
deliver(gpointer data)
{
    Answer *answer = data;

    answer->reply(answer->status, answer->body, answer->data);
    g_free(answer->body);
    g_free(answer);
    return G_SOURCE_REMOVE;
}

static void
fake_send(ClawtVentureMirror     *mirror,
          const gchar            *method,
          const gchar            *url,
          const gchar            *token,
          const gchar            *key,
          const gchar            *body,
          ClawtVentureMirrorReply reply,
          gpointer                reply_data,
          gpointer                user_data)
{
    Fixture *fixture = user_data;
    Sent *sent = g_new0(Sent, 1);
    Answer *answer = g_new0(Answer, 1);
    const gchar *path = url + strlen("http://v.test");

    (void)mirror;

    sent->method = g_strdup(method);
    sent->url = g_strdup(path);
    sent->token = g_strdup(token);
    sent->key = g_strdup(key);
    sent->body = g_strdup(body);
    g_ptr_array_add(fixture->sent, sent);

    answer->reply = reply;
    answer->data = reply_data;
    answer->status = 200;

    if (fixture->unavailable > 0) {
        fixture->unavailable--;
        answer->status = 503;
    } else if (g_str_has_prefix(path, "/api/v1/whoami")) {
        if (g_strcmp0(token, "t-bad") == 0) {
            answer->status = 401;
        } else {
            answer->body = g_strdup_printf(
                "{\"actor\":\"API token #%s\",\"user\":{\"username\":\"%s-bot\"}}",
                token, token + 2);
        }
    } else if (g_str_has_prefix(path, "/api/v1/forge_repo/resolve")) {
        if (strstr(path, "known") != NULL)
            answer->body = g_strdup("{\"id\":7,\"organization_id\":3}");
        else
            answer->status = 404;
    } else if (g_strcmp0(method, "POST") == 0 &&
               g_strcmp0(path, "/api/v1/ticket") == 0) {
        if (fixture->create_conflicts > 0) {
            fixture->create_conflicts--;
            answer->status = 409;
        } else {
            answer->status = 201;
            answer->body = g_strdup_printf(
                "{\"id\":%" G_GINT64_FORMAT ",\"version\":1,"
                "\"issue_type\":\"task\"}", ++fixture->next_ticket);
        }
    } else if (g_strcmp0(method, "GET") == 0 &&
               g_str_has_prefix(path, "/api/v1/ticket?external_ref=")) {
        answer->body = g_strdup("{\"items\":[{\"id\":42,\"version\":5,"
                                "\"issue_type\":\"task\"}]}");
    } else if (g_strcmp0(method, "PATCH") == 0) {
        if (fixture->patch_conflicts > 0) {
            fixture->patch_conflicts--;
            answer->status = 409;
        } else {
            answer->body = g_strdup("{\"version\":9}");
        }
    } else if (g_strcmp0(method, "GET") == 0 &&
               g_str_has_prefix(path, "/api/v1/ticket/")) {
        answer->body = g_strdup("{\"version\":8}");
    } else if (g_strcmp0(path, "/api/v1/ticket_comment") == 0) {
        answer->status = 201;
        answer->body = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT "}",
                                       ++fixture->next_comment);
    } else {
        answer->status = 404;
    }

    g_idle_add(deliver, answer);
}

static GStrv
fake_target(ClawtVentureMirror *mirror, const gchar *task_id, gint64 *repo_id,
            gpointer user_data)
{
    Fixture *fixture = user_data;
    const gchar *one[] = { fixture->remote, NULL };

    (void)mirror;
    (void)task_id;
    *repo_id = 0;

    return (fixture->remote != NULL) ? g_strdupv((GStrv)one) : NULL;
}

static void
fixture_setup(Fixture *fixture, gconstpointer data)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    (void)data;

    fixture->dir = g_dir_make_tmp("clawt-mirror-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "tasks.db", NULL);
    fixture->store = clawt_task_store_new(path, &error);
    g_assert_no_error(error);
    clawt_task_store_set_mirror(fixture->store, TRUE);

    fixture->sent = g_ptr_array_new_with_free_func(sent_free);
    fixture->remote = g_strdup("git@forge.test:acme/known.git");

    fixture->mirror = clawt_venture_mirror_new(fixture->store, NULL);
    clawt_venture_mirror_set_request_func(fixture->mirror, fake_send,
                                          fixture, NULL);
    clawt_venture_mirror_set_target_func(fixture->mirror, fake_target,
                                         fixture, NULL);
    clawt_venture_mirror_set_account(fixture->mirror, "lead", "books-lead",
                                     "http://v.test", "t-lead");
    clawt_venture_mirror_set_account(fixture->mirror, "scout", "books-scout",
                                     "http://v.test", "t-scout");
}

static void
fixture_teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;

    g_clear_object(&fixture->mirror);
    g_clear_object(&fixture->store);
    g_ptr_array_unref(fixture->sent);
    g_free(fixture->remote);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

/* Drains until the mirror has nothing in flight and nothing due. */
static void
drain(Fixture *fixture)
{
    guint spins = 0;

    clawt_venture_mirror_kick(fixture->mirror);

    do {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        spins++;
    } while (clawt_venture_mirror_is_busy(fixture->mirror) && spins < 10000);

    g_assert_false(clawt_venture_mirror_is_busy(fixture->mirror));
}

static ClawtTask *
save_task(Fixture *fixture, const gchar *origin, const gchar *assignee,
          ClawtTaskState state)
{
    ClawtTask *task = clawt_task_new(origin, assignee, "Fix the header\n\n"
                                     "It wraps on narrow screens.");
    g_autoptr(GError) error = NULL;

    clawt_task_set_title(task, "Fix the header");
    clawt_task_set_state(task, state);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, &error));
    g_assert_no_error(error);

    return task;
}

static Sent *
find_sent(Fixture *fixture, const gchar *method, const gchar *prefix)
{
    guint i;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, method) == 0 &&
            g_str_has_prefix(sent->url, prefix))
            return sent;
    }

    return NULL;
}

static guint
count_sent(Fixture *fixture, const gchar *method, const gchar *prefix)
{
    guint i;
    guint n = 0;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, method) == 0 &&
            g_str_has_prefix(sent->url, prefix))
            n++;
    }

    return n;
}

static JsonObject *
body_of(Sent *sent, JsonNode **holder)
{
    *holder = json_from_string(sent->body, NULL);
    g_assert_nonnull(*holder);
    return json_node_get_object(*holder);
}

/* ── Tests ───────────────────────────────────────────────────────── */

/*
 * The delegator files the ticket as its own account, assigned to the
 * assignee's VENTURE username; the assignee's account then writes only
 * what changed.
 *
 * What breaks if this regresses: VENTURE's activity log names the wrong
 * agent for the work, or a status change rewrites every field a person
 * may have edited.
 */
static void
test_files_then_updates_as_each_agent(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(ClawtVentureRef) ref = NULL;
    g_autoptr(JsonNode) create = NULL;
    g_autoptr(JsonNode) patch = NULL;
    g_autofree gchar *expected_ref = NULL;
    Sent *sent;
    JsonObject *o;

    (void)data;
    drain(fixture);

    /* each account asked who it is, once */
    g_assert_cmpuint(count_sent(fixture, "GET", "/api/v1/whoami"), ==, 2);
    g_assert_cmpstr(clawt_venture_mirror_account_user(fixture->mirror,
                                                      "scout"), ==, "scout-bot");

    sent = find_sent(fixture, "POST", "/api/v1/ticket");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-lead");
    {
        g_autofree gchar *key = g_strconcat("clawtilla-task-",
                                            clawt_task_get_id(task), NULL);

        g_assert_cmpstr(sent->key, ==, key);
    }
    o = body_of(sent, &create);
    expected_ref = g_strdup_printf("clawtilla:%s:%s", g_get_host_name(),
                                   clawt_task_get_id(task));
    g_assert_cmpstr(json_object_get_string_member(o, "assignee"), ==,
                    "scout-bot");
    g_assert_cmpstr(json_object_get_string_member(o, "status"), ==, "todo");
    g_assert_cmpstr(json_object_get_string_member(o, "kind"), ==, "internal");
    g_assert_cmpstr(json_object_get_string_member(o, "external_ref"), ==,
                    expected_ref);
    g_assert_cmpint(json_object_get_int_member(o, "repo_id"), ==, 7);
    g_assert_cmpint(json_object_get_int_member(o, "organization_id"), ==, 3);

    ref = clawt_task_store_venture_ref(fixture->store,
                                       clawt_task_get_id(task));
    g_assert_cmpstr(ref->state, ==, "synced");
    g_assert_cmpint(ref->ticket_id, ==, 1);
    g_assert_cmpstr(ref->url, ==, "http://v.test/tickets/1");

    /* running: one PATCH, by the assignee, with only the status */
    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/1");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-scout");
    o = body_of(sent, &patch);
    g_assert_cmpstr(json_object_get_string_member(o, "status"), ==,
                    "in_progress");
    g_assert_cmpint(json_object_get_int_member(o, "version"), ==, 1);
    g_assert_false(json_object_has_member(o, "title"));
    g_assert_false(json_object_has_member(o, "assignee"));

    /* saved again unchanged: nothing sent */
    {
        guint before = fixture->sent->len;

        g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
        drain(fixture);
        g_assert_cmpuint(fixture->sent->len, ==, before);
    }
}

/*
 * No repository VENTURE knows: no ticket, and the reason kept -- and the
 * task's notes are dropped rather than chased.
 */
static void
test_unmatched_is_skipped(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(JsonNode) described = NULL;

    (void)data;
    g_free(fixture->remote);
    fixture->remote = g_strdup("https://elsewhere.test/acme/other.git");

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);

    g_assert_null(find_sent(fixture, "POST", "/api/v1/ticket"));
    described = clawt_venture_mirror_describe(fixture->store,
                                              clawt_task_get_id(task));
    g_assert_cmpstr(json_object_get_string_member(
        json_node_get_object(described), "state"), ==, "skipped");
    g_assert_nonnull(strstr(json_object_get_string_member(
        json_node_get_object(described), "reason"), "no repository"));

    g_assert_cmpint(clawt_task_store_add_note(fixture->store,
        clawt_task_get_id(task), "scout", "result", "done", NULL), >, 0);
    drain(fixture);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/ticket_comment"));
}

/* Nobody involved has an account: nothing is sent at all. */
static void
test_no_account_sends_nothing(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "user", "nobody",
                                          CLAWT_TASK_PENDING);
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    drain(fixture);

    g_assert_cmpuint(fixture->sent->len, ==, 0);
    ref = clawt_task_store_venture_ref(fixture->store, clawt_task_get_id(task));
    g_assert_cmpstr(ref->state, ==, "skipped");
}

/*
 * A refused token is dead at once, named, and kept for the operator's
 * Retry -- which queues it again.
 */
static void
test_refused_token_is_dead_until_retried(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(JsonNode) described = NULL;
    const gchar *reason;
    guint queued = 0;

    (void)data;
    clawt_venture_mirror_set_account(fixture->mirror, "lead", "books-lead",
                                     "http://v.test", "t-bad");

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*refused lead's token*");
    drain(fixture);
    g_test_assert_expected_messages();

    described = clawt_venture_mirror_describe(fixture->store,
                                              clawt_task_get_id(task));
    g_assert_cmpstr(json_object_get_string_member(
        json_node_get_object(described), "state"), ==, "dead");
    reason = json_object_get_string_member(json_node_get_object(described),
                                           "reason");
    g_assert_nonnull(strstr(reason, "clawtilla connector connect books-lead"));

    g_assert_cmpuint(clawt_task_store_outbox_revive(fixture->store,
        clawt_task_get_id(task)), ==, 1);
    clawt_task_store_outbox_state(fixture->store, clawt_task_get_id(task),
                                  &queued, NULL, NULL);
    g_assert_cmpuint(queued, ==, 1);
}

/*
 * A create that timed out and landed is found by its reference, not
 * filed twice; then brought up to date.
 */
static void
test_conflict_adopts_the_ticket(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    fixture->create_conflicts = 1;

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);

    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/ticket"), ==, 1);
    g_assert_nonnull(find_sent(fixture, "GET",
                               "/api/v1/ticket?external_ref=clawtilla"));
    g_assert_nonnull(find_sent(fixture, "PATCH", "/api/v1/ticket/42"));

    ref = clawt_task_store_venture_ref(fixture->store, clawt_task_get_id(task));
    g_assert_cmpint(ref->ticket_id, ==, 42);
    g_assert_cmpint(ref->version, ==, 9);
}

/* A stale version is re-read and the same change sent again. */
static void
test_stale_version_is_reread(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(JsonNode) second = NULL;
    guint i;
    Sent *last = NULL;

    (void)data;
    drain(fixture);

    fixture->patch_conflicts = 1;
    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    g_assert_cmpuint(count_sent(fixture, "PATCH", "/api/v1/ticket/1"), ==, 2);
    g_assert_nonnull(find_sent(fixture, "GET", "/api/v1/ticket/1"));

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, "PATCH") == 0)
            last = sent;
    }

    g_assert_cmpint(json_object_get_int_member(body_of(last, &second),
                                               "version"), ==, 8);
    g_assert_cmpstr(json_object_get_string_member(
        json_node_get_object(second), "status"), ==, "done");
}

/*
 * A note becomes an internal comment, written by its author, once; a
 * second progress note within ten minutes does not.
 */
static void
test_notes_become_comments(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    g_autoptr(JsonNode) comment = NULL;
    const gchar *id = clawt_task_get_id(task);
    gint64 result;
    Sent *sent;
    JsonObject *o;

    (void)data;
    drain(fixture);

    result = clawt_task_store_add_note(fixture->store, id, "scout", "result",
                                       "Fixed in abc123.", NULL);
    clawt_task_store_add_note(fixture->store, id, "scout", "progress",
                              "looking", NULL);
    clawt_task_store_add_note(fixture->store, id, "scout", "progress",
                              "still looking", NULL);
    drain(fixture);

    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/ticket_comment"),
                     ==, 2);
    sent = find_sent(fixture, "POST", "/api/v1/ticket_comment");
    g_assert_cmpstr(sent->token, ==, "t-scout");
    o = body_of(sent, &comment);
    g_assert_true(json_object_get_boolean_member(o, "internal"));
    g_assert_cmpint(json_object_get_int_member(o, "ticket_id"), ==, 1);
    g_assert_nonnull(strstr(json_object_get_string_member(o, "body"),
                            "Fixed in abc123."));
    g_assert_cmpint(clawt_task_store_venture_comment(fixture->store, result),
                    ==, 1);
}

/*
 * VENTURE down: the row waits, with the error, and is not due again
 * until its backoff passes.
 */
static void
test_unavailable_backs_off(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(GPtrArray) due = NULL;
    g_autofree gchar *last_error = NULL;
    guint queued = 0;

    (void)data;
    fixture->unavailable = 1;

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);

    clawt_task_store_outbox_state(fixture->store, clawt_task_get_id(task),
                                  &queued, NULL, &last_error);
    g_assert_cmpuint(queued, ==, 1);
    g_assert_nonnull(strstr(last_error, "503"));

    due = clawt_task_store_outbox_due(fixture->store,
                                      g_get_real_time() / G_USEC_PER_SEC, 10,
                                      NULL);
    g_assert_cmpuint(due->len, ==, 0);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/ticket"));
}

/*
 * The outbox coalesces a task's saves into one row, and a change made
 * while the row was in flight is kept rather than finished away.
 */
static void
test_outbox_coalesces_and_keeps_late_changes(Fixture *fixture,
                                             gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(GPtrArray) due = NULL;
    g_autoptr(GPtrArray) again = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    ClawtVentureOutboxRow *row;

    (void)data;
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));

    due = clawt_task_store_outbox_due(fixture->store, now, 10, NULL);
    g_assert_cmpuint(due->len, ==, 1);
    row = g_ptr_array_index(due, 0);

    /* changed while "in flight" */
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    g_assert_true(clawt_task_store_outbox_finish(fixture->store, row->id,
                                                 row->gen));

    again = clawt_task_store_outbox_due(fixture->store, now + 1, 10, NULL);
    g_assert_cmpuint(again->len, ==, 1);
    g_assert_true(clawt_task_store_outbox_finish(fixture->store,
        ((ClawtVentureOutboxRow *)g_ptr_array_index(again, 0))->id,
        ((ClawtVentureOutboxRow *)g_ptr_array_index(again, 0))->gen));

    g_clear_pointer(&again, g_ptr_array_unref);
    again = clawt_task_store_outbox_due(fixture->store, now + 1, 10, NULL);
    g_assert_cmpuint(again->len, ==, 0);
}

/* The status a ticket shows for each task state. */
static void
test_status_map(void)
{
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_UNASSIGNED),
                    ==, "triage");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_PENDING),
                    ==, "todo");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_RUNNING),
                    ==, "in_progress");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_BLOCKED),
                    ==, "blocked");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_FAILED),
                    ==, "blocked");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_STALLED),
                    ==, "blocked");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_REVIEW),
                    ==, "review");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_COMPLETED),
                    ==, "done");
    g_assert_cmpstr(clawt_venture_mirror_ticket_status(CLAWT_TASK_CANCELLED),
                    ==, "cancelled");
}

/* Without the mirror on, the store writes no outbox at all. */
static void
test_no_outbox_without_mirror(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(GPtrArray) due = NULL;

    (void)data;
    clawt_task_store_set_mirror(fixture->store, FALSE);
    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    clawt_task_store_add_note(fixture->store, clawt_task_get_id(task),
                              "scout", "result", "x", NULL);

    due = clawt_task_store_outbox_due(fixture->store,
                                      g_get_real_time() / G_USEC_PER_SEC, 10,
                                      NULL);
    g_assert_cmpuint(due->len, ==, 0);
}

#define ADD(name, fn) \
    g_test_add("/venture-mirror/" name, Fixture, NULL, fixture_setup, fn, \
               fixture_teardown)

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    ADD("files-then-updates-as-each-agent",
        test_files_then_updates_as_each_agent);
    ADD("unmatched-is-skipped", test_unmatched_is_skipped);
    ADD("no-account-sends-nothing", test_no_account_sends_nothing);
    ADD("refused-token-is-dead-until-retried",
        test_refused_token_is_dead_until_retried);
    ADD("conflict-adopts-the-ticket", test_conflict_adopts_the_ticket);
    ADD("stale-version-is-reread", test_stale_version_is_reread);
    ADD("notes-become-comments", test_notes_become_comments);
    ADD("unavailable-backs-off", test_unavailable_backs_off);
    ADD("outbox-coalesces-and-keeps-late-changes",
        test_outbox_coalesces_and_keeps_late_changes);
    ADD("no-outbox-without-mirror", test_no_outbox_without_mirror);
    g_test_add_func("/venture-mirror/status-map", test_status_map);

    return g_test_run();
}
