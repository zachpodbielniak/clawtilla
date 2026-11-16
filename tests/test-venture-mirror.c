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

    /* the reverse direction: what VENTURE answers, and what was done */
    gchar          *changed;            /* GET ticket?external_ref__like */
    gchar          *comments;           /* GET ticket_comment?ticket_id__in */
    gchar          *assigned;           /* GET ticket?assignee=scout-bot */
    GPtrArray      *inbound;            /* "cancel <task>", ... */
    gboolean        budget_refuses;     /* POST runs/external answers 409 */
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
               g_strcmp0(path, "/api/v1/ticket") == 0 &&
               g_strcmp0(token, "t-viewer") == 0) {
        answer->status = 202;
        answer->body = g_strdup("{\"staged\":true}");
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
    } else if (g_strcmp0(path, "/api/v1/activity/ticket/30") == 0) {
        answer->body = g_strdup(
            "[{\"kind\":\"change\",\"action\":\"update\","
            "\"actor\":\"API token #t-lead\",\"changes\":{\"assignee\":"
            "{\"from\":null,\"to\":\"scout-bot\"}}},"
            " {\"kind\":\"change\",\"action\":\"create\","
            "\"actor\":\"alice\",\"changes\":null}]");
    } else if (g_str_has_prefix(path, "/api/v1/activity/ticket/")) {
        answer->body = g_strdup(
            "[{\"kind\":\"change\",\"action\":\"create\","
            "\"actor\":\"alice\",\"changes\":null}]");
    } else if (g_strcmp0(path, "/api/v1/ticket_link") == 0) {
        answer->status = 201;
        answer->body = g_strdup("{\"id\":90}");
    } else if (g_strcmp0(path, "/api/v1/milestone/5") == 0) {
        answer->body = g_strdup("{\"id\":5,\"name\":\"Beta\","
                                "\"due_on\":\"2026-12-01\",\"status\":\"open\"}");
    } else if (g_strcmp0(path, "/api/v1/goal/6") == 0) {
        answer->body = g_strdup("{\"id\":6,\"name\":\"Ship titles\","
                                "\"current_value\":4,\"target_value\":12,"
                                "\"unit\":\"titles\"}");
    } else if (g_strcmp0(path, "/api/v1/runs/external") == 0) {
        if (fixture->budget_refuses) {
            answer->status = 409;
            answer->body = g_strdup("{\"message\":\"budget Fleet is spent\"}");
        } else {
            answer->status = 201;
            answer->body = g_strdup("{\"id\":70}");
        }
    } else if (g_str_has_prefix(path, "/api/v1/runs/external/")) {
        answer->body = g_strdup("{\"id\":70}");
    } else if (g_str_has_prefix(path, "/api/v1/ticket?external_ref__like=")) {
        answer->body = g_strdup(fixture->changed != NULL ? fixture->changed
                                                         : "[]");
    } else if (g_str_has_prefix(path, "/api/v1/ticket_comment?created_at__lt=")) {
        answer->body = g_strdup("{\"items\":[{\"id\":5}]}");
    } else if (g_str_has_prefix(path, "/api/v1/ticket_comment?ticket_id__in=")) {
        answer->body = g_strdup(fixture->comments != NULL ? fixture->comments
                                                          : "[]");
    } else if (g_str_has_prefix(path, "/api/v1/ticket?assignee=")) {
        answer->body = g_strdup(strstr(path, "assignee=scout-bot") != NULL &&
                                fixture->assigned != NULL
                                    ? fixture->assigned : "[]");
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
    fixture->inbound = g_ptr_array_new_with_free_func(g_free);
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
    g_ptr_array_unref(fixture->inbound);
    g_free(fixture->remote);
    g_free(fixture->changed);
    g_free(fixture->comments);
    g_free(fixture->assigned);
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

/* ── The reverse direction (0.45.0) ──────────────────────────────── */

static void
rec_cancel(gpointer data, const gchar *task_id, const gchar *who)
{
    Fixture *fixture = data;

    (void)who;
    g_ptr_array_add(fixture->inbound, g_strdup_printf("cancel %s", task_id));
}

static void
rec_review(gpointer data, const gchar *task_id, gboolean accept,
           const gchar *notes, const gchar *who)
{
    Fixture *fixture = data;

    (void)notes;
    (void)who;
    g_ptr_array_add(fixture->inbound, g_strdup_printf(
        "%s %s", accept ? "accept" : "changes", task_id));
}

static void
rec_comment(gpointer data, const gchar *task_id, const gchar *who,
            const gchar *text)
{
    Fixture *fixture = data;

    g_ptr_array_add(fixture->inbound, g_strdup_printf(
        "comment %s %s: %s", task_id, who, text));
}

static gchar *
rec_intake(gpointer data, const gchar *agent_id, gint64 ticket,
           const gchar *title, const gchar *description, const gchar *url,
           const gchar *from_agent)
{
    Fixture *fixture = data;
    g_autoptr(ClawtTask) task = save_task(fixture, "user", agent_id,
                                          CLAWT_TASK_PENDING);

    (void)description;
    (void)url;
    g_ptr_array_add(fixture->inbound, g_strdup_printf(
        "intake %" G_GINT64_FORMAT " %s %s%s%s", ticket, agent_id, title,
        from_agent != NULL ? " from " : "",
        from_agent != NULL ? from_agent : ""));

    return g_strdup(clawt_task_get_id(task));
}

static const ClawtVentureMirrorInbound recorder = {
    rec_cancel, rec_review, rec_comment, rec_intake
};

static gboolean
inbound_has(Fixture *fixture, const gchar *line)
{
    guint i;

    for (i = 0; i < fixture->inbound->len; i++) {
        if (g_strcmp0(g_ptr_array_index(fixture->inbound, i), line) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
poll(Fixture *fixture)
{
    clawt_venture_mirror_poll(fixture->mirror);
    drain(fixture);
}

/* A ticket JSON as VENTURE lists it, for one of this host's tasks. */
static gchar *
changed_ticket(const gchar *task_id, gint64 version, const gchar *status)
{
    return g_strdup_printf(
        "[{\"id\":1,\"version\":%" G_GINT64_FORMAT ",\"status\":\"%s\","
        "\"external_ref\":\"clawtilla:%s:%s\","
        "\"updated_at\":\"2026-11-16T10:00:00Z\"}]",
        version, status, g_get_host_name(), task_id);
}

/*
 * A person cancels the ticket: the task is cancelled, once.  The mirror's
 * own write -- no newer than its receipt -- is not read back as anybody's.
 */
static void
test_cancel_in_venture_cancels_once(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    const gchar *id = clawt_task_get_id(task);

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    /* its own write, version 1: nothing */
    fixture->changed = changed_ticket(id, 1, "cancelled");
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 0);

    /* a person's, version 2: cancelled */
    g_free(fixture->changed);
    fixture->changed = changed_ticket(id, 2, "cancelled");
    poll(fixture);
    {
        g_autofree gchar *line = g_strdup_printf("cancel %s", id);

        g_assert_true(inbound_has(fixture, line));
    }
    g_assert_cmpuint(fixture->inbound->len, ==, 1);

    /* read again: not again */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/* Review has two doors: done on the ticket accepts the task in review. */
static void
test_done_in_venture_accepts_review(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_REVIEW);
    g_autofree gchar *line = g_strdup_printf("accept %s",
                                             clawt_task_get_id(task));

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    fixture->changed = changed_ticket(clawt_task_get_id(task), 2, "done");
    poll(fixture);
    g_assert_true(inbound_has(fixture, line));
}

/*
 * Comments: none on the first pass (a baseline, not a replay); then a
 * person's is delivered, and the mirror's own and the assignee's own
 * are not.
 */
static void
test_comments_come_back(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    g_autofree gchar *line = NULL;

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    fixture->comments = g_strdup(
        "[{\"id\":4,\"ticket_id\":1,\"author\":\"alice\",\"body\":\"old\"}]");
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 0);
    /* the baseline is what came before the mirror started */
    g_assert_nonnull(find_sent(fixture, "GET",
                               "/api/v1/ticket_comment?created_at__lt="));

    g_free(fixture->comments);
    fixture->comments = g_strdup_printf(
        "[{\"id\":6,\"ticket_id\":1,\"author\":\"alice\",\"body\":\"why?\"},"
        " {\"id\":7,\"ticket_id\":1,\"author\":\"API token #t-scout\","
        "  \"body\":\"mine\",\"external_ref\":\"clawtilla:%s:note:3\"},"
        " {\"id\":8,\"ticket_id\":1,\"author\":\"scout-bot\",\"body\":\"me\"}]",
        g_get_host_name());
    poll(fixture);

    line = g_strdup_printf("comment %s alice: why?", clawt_task_get_id(task));
    g_assert_true(inbound_has(fixture, line));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
    g_assert_nonnull(find_sent(fixture, "GET",
        "/api/v1/ticket_comment?ticket_id__in=1&id__gt=5"));
}

/*
 * A todo ticket assigned to an agent's VENTURE user, from no other
 * system, becomes a task once; the outbox then writes its reference.
 */
static void
test_intake_once_then_referenced(Fixture *fixture, gconstpointer data)
{
    g_autoptr(JsonNode) patch = NULL;
    Sent *sent;

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->assigned = g_strdup(
        "[{\"id\":30,\"version\":3,\"title\":\"Do X\",\"description\":\"d\","
        "  \"status\":\"todo\",\"external_ref\":\"\"},"
        " {\"id\":31,\"version\":1,\"title\":\"Theirs\",\"status\":\"todo\","
        "  \"external_ref\":\"jira:ABC-1\"},"
        " {\"id\":32,\"version\":1,\"title\":\"From a person\","
        "  \"status\":\"todo\"}]");

    poll(fixture);
    /* lead's own account assigned it: a delegation from lead */
    g_assert_true(inbound_has(fixture, "intake 30 scout Do X from lead"));
    /* a person's assignment: from the operator */
    g_assert_true(inbound_has(fixture, "intake 32 scout From a person"));
    g_assert_cmpuint(fixture->inbound->len, ==, 2);

    /* the outbox brings the ticket up to the task, reference first */
    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/30");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-scout");
    g_assert_nonnull(g_strrstr(json_object_get_string_member(
        body_of(sent, &patch), "external_ref"), "clawtilla:"));
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(patch),
                                               "version"), ==, 3);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/ticket"));

    /* the same tickets on the next pass: not again */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 2);
}

/* Without intake on for the account, nothing is taken in. */
static void
test_no_intake_without_the_switch(Fixture *fixture, gconstpointer data)
{
    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    fixture->assigned = g_strdup(
        "[{\"id\":30,\"version\":1,\"title\":\"Do X\",\"status\":\"todo\"}]");

    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 0);
    g_assert_null(find_sent(fixture, "GET", "/api/v1/ticket?assignee="));
}

/* A note that came from VENTURE is never mirrored back as a comment. */
static void
test_venture_notes_not_echoed(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);

    (void)data;
    drain(fixture);
    clawt_task_store_add_note(fixture->store, clawt_task_get_id(task),
                              "venture:alice", "comment", "why?", NULL);
    drain(fixture);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/ticket_comment"));
}

/* ── External runs (0.45.0) ──────────────────────────────────────── */

static gint64
fake_cost(ClawtVentureMirror *mirror, const gchar *task_id, gpointer data)
{
    (void)mirror;
    (void)task_id;
    (void)data;

    return 420000;   /* $0.42 */
}

/*
 * A task that ran is one VENTURE run: opened by the assignee once it
 * started, closed with its state, cost (with its currency) and branch
 * once it finished -- and only once each.
 */
static void
test_a_run_is_opened_and_closed(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(JsonNode) start = NULL;
    g_autoptr(JsonNode) finish = NULL;
    Sent *sent;

    (void)data;
    clawt_venture_mirror_set_cost_func(fixture->mirror, fake_cost, NULL, NULL);

    drain(fixture);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/runs/external"));

    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "POST", "/api/v1/runs/external");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-scout");
    g_assert_cmpint(json_object_get_int_member(body_of(sent, &start),
                                               "ticket_id"), ==, 1);

    clawt_task_set_result(task, "Fixed.");
    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "PATCH", "/api/v1/runs/external/70");
    g_assert_nonnull(sent);
    g_assert_cmpstr(json_object_get_string_member(body_of(sent, &finish),
                                                  "state"), ==, "succeeded");
    g_assert_cmpstr(json_object_get_string_member(
        json_node_get_object(finish), "cost"), ==, "0.420000 USD");

    /* saved again: neither is sent again */
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);
    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/runs/external"),
                     ==, 1);
    g_assert_cmpuint(count_sent(fixture, "PATCH", "/api/v1/runs/external/"),
                     ==, 1);
}

/*
 * A budget that says no is said once and not asked again; the ticket
 * itself is still kept current.
 */
static void
test_a_refused_run_is_not_retried(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);

    (void)data;
    fixture->budget_refuses = TRUE;
    clawt_venture_mirror_set_cost_func(fixture->mirror, fake_cost, NULL, NULL);
    drain(fixture);
    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/runs/external"),
                     ==, 1);

    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/runs/external"),
                     ==, 1);
    g_assert_null(find_sent(fixture, "PATCH", "/api/v1/runs/external/"));
    g_assert_nonnull(find_sent(fixture, "PATCH", "/api/v1/ticket/1"));
}

/* Without a cost function, no runs at all. */
static void
test_no_runs_without_cost(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);

    (void)data;
    (void)task;
    drain(fixture);
    g_assert_null(find_sent(fixture, "POST", "/api/v1/runs/external"));
}

/* ── Links, the operator's account, milestones and goals (0.46.0) ── */

static void
fake_context(ClawtVentureMirror *mirror, const gchar *task_id,
             gint64 *milestone_id, gint64 *goal_id, gpointer data)
{
    (void)mirror;
    (void)task_id;
    (void)data;
    *milestone_id = 5;
    *goal_id = 6;
}

/*
 * A task with a checkout puts its branch on the ticket, once, as the
 * assignee, against the repository the ticket was filed for.
 */
static void
test_branch_is_linked_once(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = clawt_task_new("lead", "scout", "Fix it");
    g_autoptr(JsonNode) link = NULL;
    Sent *sent;
    JsonObject *o;

    (void)data;
    clawt_task_set_checkout(task, "/srv/site", "/w/tasks/x", "clawtilla/x",
                            "abc123", CLAWT_CHECKOUT_WORKTREE);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "POST", "/api/v1/ticket_link");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-scout");
    o = body_of(sent, &link);
    g_assert_cmpstr(json_object_get_string_member(o, "branch"), ==,
                    "clawtilla/x");
    g_assert_cmpstr(json_object_get_string_member(o, "origin"), ==, "venture");
    g_assert_cmpint(json_object_get_int_member(o, "repo_id"), ==, 7);

    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);
    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/ticket_link"), ==, 1);
}

/*
 * The operator's own account files the operator's tasks, and any task
 * whose agents have no account -- which without it are not mirrored.
 */
static void
test_service_account_files_what_no_agent_can(Fixture *fixture,
                                             gconstpointer data)
{
    g_autoptr(ClawtTask) theirs = NULL;
    g_autoptr(ClawtTask) nobodys = NULL;
    guint i;
    guint by_service = 0;

    (void)data;
    clawt_venture_mirror_set_account(fixture->mirror, "user", "books",
                                     "http://v.test", "t-svc");

    theirs = save_task(fixture, "user", "scout", CLAWT_TASK_PENDING);
    nobodys = save_task(fixture, "user", "nobody", CLAWT_TASK_PENDING);
    drain(fixture);

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, "POST") == 0 &&
            g_strcmp0(sent->url, "/api/v1/ticket") == 0) {
            g_assert_cmpstr(sent->token, ==, "t-svc");
            by_service++;
        }
    }

    g_assert_cmpuint(by_service, ==, 2);
}

/* A new ticket is filed under its project's milestone and goal. */
static void
test_filed_under_milestone_and_goal(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(JsonNode) create = NULL;
    JsonObject *o;

    (void)data;
    clawt_venture_mirror_set_context_func(fixture->mirror, fake_context,
                                          NULL, NULL);
    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);

    o = body_of(find_sent(fixture, "POST", "/api/v1/ticket"), &create);
    g_assert_cmpint(json_object_get_int_member(o, "milestone_id"), ==, 5);
    g_assert_cmpint(json_object_get_int_member(o, "goal_id"), ==, 6);
}

static void
on_context_changed(ClawtVentureMirror *mirror, gpointer data)
{
    (void)mirror;
    (*(guint *)data)++;
}

/*
 * The milestone and goal are read once a day into tasks.db, said once
 * when they changed, and shown as one line.
 */
static void
test_context_is_read_and_said(Fixture *fixture, gconstpointer data)
{
    const gint64 milestones[] = { 5 };
    const gint64 goals[] = { 6 };
    g_autofree gchar *line = NULL;
    guint changed = 0;

    (void)data;
    g_signal_connect(fixture->mirror, "context-changed",
                     G_CALLBACK(on_context_changed), &changed);
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    clawt_venture_mirror_set_context_ids(fixture->mirror, milestones, 1,
                                         goals, 1);

    poll(fixture);
    g_assert_cmpuint(changed, ==, 1);
    g_assert_nonnull(find_sent(fixture, "GET", "/api/v1/milestone/5"));
    g_assert_nonnull(find_sent(fixture, "GET", "/api/v1/goal/6"));

    line = clawt_venture_mirror_context_line(fixture->store, 5, 6);
    g_assert_cmpstr(line, ==, "milestone *Beta*, due 2026-12-01 (open); "
                    "goal *Ship titles*: 4 of 12 titles");

    /* within the day: not read again */
    poll(fixture);
    g_assert_cmpuint(count_sent(fixture, "GET", "/api/v1/milestone/5"), ==, 1);
    g_assert_cmpuint(changed, ==, 1);
}

/* ── Pull requests (0.48.0) ──────────────────────────────────────── */

/* Every forge's spelling of a pull or merge request, and what is not one. */
static void
test_finds_pull_requests(void)
{
    static const struct {
        const gchar *text;
        const gchar *url;
        gint64       number;
    } cases[] = {
        { "Opened https://github.com/acme/site/pull/42 for review.",
          "https://github.com/acme/site/pull/42", 42 },
        { "PR: https://git.example.test/acme/site/pulls/7",
          "https://git.example.test/acme/site/pulls/7", 7 },
        { "MR (https://gitlab.com/acme/site/-/merge_requests/1234)",
          "https://gitlab.com/acme/site/-/merge_requests/1234", 1234 },
        { "Committed on clawtilla/x; no pull request opened.", NULL, 0 },
        { "See https://github.com/acme/site/issues/9", NULL, 0 },
        { NULL, NULL, 0 },
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        gint64 number = -1;
        g_autofree gchar *url = clawt_venture_mirror_find_pull_request(
            cases[i].text, &number);

        g_assert_cmpstr(url, ==, cases[i].url);
        g_assert_cmpint(number, ==, cases[i].number);
    }
}

/*
 * A task whose result names the pull request it opened: the branch's
 * link gets its number and URL, and the run its number -- once.
 */
static void
test_pull_request_is_linked(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = clawt_task_new("lead", "scout", "Fix it");
    g_autoptr(JsonNode) patch = NULL;
    g_autoptr(JsonNode) finish = NULL;
    Sent *sent;

    (void)data;
    clawt_venture_mirror_set_cost_func(fixture->mirror, fake_cost, NULL, NULL);
    clawt_task_set_checkout(task, "/srv/site", "/w/tasks/x", "clawtilla/x",
                            "abc123", CLAWT_CHECKOUT_WORKTREE);
    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);
    g_assert_null(find_sent(fixture, "PATCH", "/api/v1/ticket_link/"));

    clawt_task_set_result(task, "Done: https://forge.test/acme/site/pulls/12");
    clawt_task_set_state(task, CLAWT_TASK_COMPLETED);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "PATCH", "/api/v1/ticket_link/90");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-scout");
    g_assert_cmpint(json_object_get_int_member(body_of(sent, &patch),
                                               "pull_request_number"), ==, 12);
    g_assert_cmpstr(json_object_get_string_member(
        json_node_get_object(patch), "pull_request_url"), ==,
        "https://forge.test/acme/site/pulls/12");

    sent = find_sent(fixture, "PATCH", "/api/v1/runs/external/70");
    g_assert_nonnull(sent);
    g_assert_cmpint(json_object_get_int_member(body_of(sent, &finish),
                                               "pull_request_number"), ==, 12);

    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);
    g_assert_cmpuint(count_sent(fixture, "PATCH", "/api/v1/ticket_link/"),
                     ==, 1);
}

/* ── Accounts, as the plan names them ────────────────────────────── */

/*
 * A handoff moves the ticket with the work: the new owner's account
 * writes it, assigned to the new owner's VENTURE user.
 */
static void
test_handoff_reassigns_the_ticket(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    g_autoptr(JsonNode) patch = NULL;
    Sent *sent = NULL;
    guint i;

    (void)data;
    drain(fixture);

    g_assert_true(clawt_task_transfer_owner(task, "lead"));
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *s = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(s->method, "PATCH") == 0 &&
            g_strcmp0(s->url, "/api/v1/ticket/1") == 0)
            sent = s;
    }

    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-lead");
    g_assert_cmpstr(json_object_get_string_member(body_of(sent, &patch),
                                                  "assignee"), ==, "lead-bot");
}

/* One agent's refused token stops its own tasks, and nobody else's. */
static void
test_a_refused_token_stops_only_its_own(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) theirs = NULL;
    g_autoptr(ClawtTask) ours = NULL;
    g_autoptr(JsonNode) dead = NULL;
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    clawt_venture_mirror_set_account(fixture->mirror, "rogue", "books-rogue",
                                     "http://v.test", "t-bad");

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*refused rogue's token*");
    theirs = save_task(fixture, "rogue", "rogue", CLAWT_TASK_PENDING);
    ours = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);
    g_test_assert_expected_messages();

    dead = clawt_venture_mirror_describe(fixture->store,
                                         clawt_task_get_id(theirs));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(dead),
                                                  "state"), ==, "dead");

    ref = clawt_task_store_venture_ref(fixture->store, clawt_task_get_id(ours));
    g_assert_nonnull(ref);
    g_assert_cmpstr(ref->state, ==, "synced");
}

/*
 * A token whose role stages its writes -- a viewer, answered 202 -- is a
 * configuration error, named, not retried.
 */
static void
test_a_viewer_token_is_flagged(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(JsonNode) dead = NULL;
    const gchar *reason;

    (void)data;
    clawt_venture_mirror_set_account(fixture->mirror, "watcher",
                                     "books-watcher", "http://v.test",
                                     "t-viewer");

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*stages its writes*");
    task = save_task(fixture, "watcher", "watcher", CLAWT_TASK_PENDING);
    drain(fixture);
    g_test_assert_expected_messages();

    dead = clawt_venture_mirror_describe(fixture->store,
                                         clawt_task_get_id(task));
    reason = json_object_get_string_member(json_node_get_object(dead),
                                           "reason");
    g_assert_nonnull(strstr(reason, "watcher"));
    g_assert_nonnull(strstr(reason, "editor membership"));
    g_assert_cmpuint(count_sent(fixture, "POST", "/api/v1/ticket"), ==, 1);
}

/* A reviewer's verdict is written with the reviewer's own token. */
static void
test_review_is_written_by_the_reviewer(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_REVIEW);
    Sent *sent;

    (void)data;
    drain(fixture);

    clawt_task_store_add_note(fixture->store, clawt_task_get_id(task), "lead",
                              "review", "Changes: add a test.", NULL);
    drain(fixture);

    sent = find_sent(fixture, "POST", "/api/v1/ticket_comment");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-lead");
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
    ADD("cancel-in-venture-cancels-once", test_cancel_in_venture_cancels_once);
    ADD("done-in-venture-accepts-review", test_done_in_venture_accepts_review);
    ADD("comments-come-back", test_comments_come_back);
    ADD("intake-once-then-referenced", test_intake_once_then_referenced);
    ADD("no-intake-without-the-switch", test_no_intake_without_the_switch);
    ADD("venture-notes-not-echoed", test_venture_notes_not_echoed);
    ADD("a-run-is-opened-and-closed", test_a_run_is_opened_and_closed);
    ADD("a-refused-run-is-not-retried", test_a_refused_run_is_not_retried);
    ADD("no-runs-without-cost", test_no_runs_without_cost);
    ADD("branch-is-linked-once", test_branch_is_linked_once);
    ADD("service-account-files-what-no-agent-can",
        test_service_account_files_what_no_agent_can);
    ADD("filed-under-milestone-and-goal", test_filed_under_milestone_and_goal);
    ADD("context-is-read-and-said", test_context_is_read_and_said);
    ADD("pull-request-is-linked", test_pull_request_is_linked);
    ADD("handoff-reassigns-the-ticket", test_handoff_reassigns_the_ticket);
    ADD("a-refused-token-stops-only-its-own",
        test_a_refused_token_stops_only_its_own);
    ADD("a-viewer-token-is-flagged", test_a_viewer_token_is_flagged);
    ADD("review-is-written-by-the-reviewer",
        test_review_is_written_by_the_reviewer);
    g_test_add_func("/venture-mirror/finds-pull-requests",
                    test_finds_pull_requests);

    return g_test_run();
}
