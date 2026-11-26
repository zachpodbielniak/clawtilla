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
    gint64          create_org;         /* organization_id in a created ticket */
    gchar          *intake_url;         /* the url intake was handed */
    gchar          *intake_task;        /* and the task it made */
    gboolean        name_intake;        /* intake names its task "#N title" */

    /*
     * Organizations.  VENTURE answers a list that names no organization_id
     * from the token's default organization only; these are what it
     * answers for organization_id=2, a second one the account is in.
     */
    gchar          *memberships;        /* whoami's, as JSON; NULL: none */
    gchar          *changed_2;
    gchar          *comments_2;
    gchar          *assigned_2;
    gint64          adopt_org;          /* adoption finds only with this id */
    guint           comment_conflicts;  /* answer this many POST comment 409 */

    /*
     * Teams.  Todo tickets with a team and nobody assigned, as VENTURE
     * lists them (team_id=N&assignee=, paged by id__gt), in the
     * default organization and the second; the team memberships, as
     * the fleet's users' are asked for; and each agent's standing, as the daemon would
     * answer it: its open tasks, and whether it leads a clawtilla team.
     */
    gchar          *team_tickets;
    gchar          *team_tickets_2;
    gchar          *team_members;
    GHashTable     *open_tasks;         /* agent -> GINT_TO_POINTER(n) */
    gchar          *leads;              /* the one agent that leads, or NULL */
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

/*
 * A list as VENTURE answers it: the records of @rows (an array, or an
 * object with `records`) in the request's team_id, above its id__gt and
 * not in its id__not_in, the first ten.
 */
static gchar *
fake_page(const gchar *rows, const gchar *path)
{
    g_autoptr(JsonNode) node = json_from_string(rows, NULL);
    g_autoptr(JsonNode) out = json_node_new(JSON_NODE_ARRAY);
    JsonArray *page = json_array_new();
    JsonArray *all;
    const gchar *gt = strstr(path, "id__gt=");
    gint64 after = (gt != NULL) ? g_ascii_strtoll(gt + 7, NULL, 10) : 0;
    const gchar *in_team = strstr(path, "team_id=");
    gint64 team = (in_team != NULL) ? g_ascii_strtoll(in_team + 8, NULL, 10)
                                    : 0;
    const gchar *not_in = strstr(path, "id__not_in=");
    g_auto(GStrv) skip = NULL;
    guint i;

    if (not_in != NULL) {
        g_autofree gchar *list = g_strndup(not_in + 11,
                                           strcspn(not_in + 11, "&"));

        skip = g_strsplit(list, ",", -1);
    }

    g_assert_nonnull(node);
    all = JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node)
        : json_object_get_array_member(json_node_get_object(node), "records");

    for (i = 0; i < json_array_get_length(all) &&
                json_array_get_length(page) < 10; i++) {
        JsonObject *t = json_array_get_object_element(all, i);

        g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT,
            json_object_get_int_member(t, "id"));

        if (json_object_get_int_member(t, "id") <= after ||
            (team > 0 && json_object_get_int_member_with_default(
                             t, "team_id", 0) != team) ||
            (skip != NULL && g_strv_contains((const gchar *const *)skip, id)))
            continue;

        json_array_add_element(page,
            json_node_copy(json_array_get_element(all, i)));
    }

    json_node_take_array(out, page);
    return json_to_string(out, FALSE);
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
    /* organization_id=2 is the second organization; none is the default */
    gboolean in_second = (strstr(path, "organization_id=2") != NULL);
    g_autofree gchar *adopt_scope = g_strdup_printf(
        "organization_id=%" G_GINT64_FORMAT, fixture->adopt_org);
    gboolean adoptable = (fixture->adopt_org == 0 ||
                          strstr(path, adopt_scope) != NULL);

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
            /* t-lead is user 11, t-scout 12, t-helper 13 */
            gint64 user_id = g_strcmp0(token, "t-lead") == 0 ? 11
                : g_strcmp0(token, "t-scout") == 0 ? 12
                : g_strcmp0(token, "t-helper") == 0 ? 13 : 90;

            answer->body = g_strdup_printf(
                "{\"actor\":\"API token #%s\",\"user\":{\"id\":%"
                G_GINT64_FORMAT ",\"username\":\"%s-bot\"}"
                "%s%s}", token, user_id, token + 2,
                fixture->memberships != NULL ? ",\"memberships\":" : "",
                fixture->memberships != NULL ? fixture->memberships : "");
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
                "\"issue_type\":\"task\",\"organization_id\":%"
                G_GINT64_FORMAT "}", ++fixture->next_ticket,
                fixture->create_org);
        }
    } else if (g_strcmp0(path, "/api/v1/activity/ticket/30") == 0) {
        answer->body = g_strdup(
            "[{\"kind\":\"change\",\"action\":\"update\","
            "\"actor\":\"API token #t-lead\",\"changes\":{\"assignee\":"
            "{\"from\":null,\"to\":\"scout-bot\"}}},"
            " {\"kind\":\"change\",\"action\":\"create\","
            "\"actor\":\"alice\",\"changes\":null}]");
    } else if (g_strcmp0(path, "/api/v1/activity/ticket/51") == 0) {
        /* a person made it; scout's own account put it in the team */
        answer->body = g_strdup(
            "[{\"kind\":\"change\",\"action\":\"update\","
            "\"actor\":\"API token #t-scout\",\"changes\":{\"team_id\":"
            "{\"from\":null,\"to\":7}}},"
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
        const gchar *rows = in_second ? fixture->changed_2 : fixture->changed;

        answer->body = g_strdup(rows != NULL ? rows : "[]");
    } else if (g_str_has_prefix(path, "/api/v1/ticket_comment?created_at__lt=")) {
        answer->body = g_strdup("{\"items\":[{\"id\":5}]}");
    } else if (g_str_has_prefix(path, "/api/v1/ticket_comment?ticket_id__in=")) {
        const gchar *rows = in_second ? fixture->comments_2 : fixture->comments;

        answer->body = g_strdup(rows != NULL ? rows : "[]");
    } else if (g_str_has_prefix(path, "/api/v1/ticket?team_id=")) {
        const gchar *rows = in_second ? fixture->team_tickets_2
                                      : fixture->team_tickets;

        answer->body = fake_page(rows != NULL ? rows : "[]", path);
    } else if (g_str_has_prefix(path, "/api/v1/team_membership?user_id__in=")) {
        answer->body = g_strdup(fixture->team_members != NULL
                                ? fixture->team_members : "[]");
    } else if (g_str_has_prefix(path, "/api/v1/ticket?assignee=")) {
        const gchar *rows = in_second ? fixture->assigned_2 : fixture->assigned;

        answer->body = g_strdup(strstr(path, "assignee=scout-bot") != NULL &&
                                rows != NULL ? rows : "[]");
    } else if (g_strcmp0(method, "GET") == 0 &&
               g_str_has_prefix(path, "/api/v1/ticket?external_ref=")) {
        answer->body = g_strdup(adoptable
            ? "{\"items\":[{\"id\":42,\"version\":5,\"issue_type\":\"task\"}]}"
            : "{\"items\":[]}");
    } else if (g_strcmp0(method, "GET") == 0 &&
               g_str_has_prefix(path, "/api/v1/ticket_comment?external_ref=")) {
        answer->body = g_strdup(adoptable ? "{\"items\":[{\"id\":77}]}"
                                          : "{\"items\":[]}");
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
    } else if (g_strcmp0(path, "/api/v1/ticket_comment") == 0 &&
               fixture->comment_conflicts > 0) {
        fixture->comment_conflicts--;
        answer->status = 409;
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
    fixture->open_tasks = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);

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
    g_free(fixture->memberships);
    g_free(fixture->changed_2);
    g_free(fixture->comments_2);
    g_free(fixture->assigned_2);
    g_free(fixture->intake_url);
    g_free(fixture->intake_task);
    g_free(fixture->team_tickets);
    g_free(fixture->team_tickets_2);
    g_free(fixture->team_members);
    g_clear_pointer(&fixture->open_tasks, g_hash_table_unref);
    g_free(fixture->leads);
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

/* Counts ::account-learned, keeping the last agent and username. */
typedef struct {
    guint  count;
    gchar *agent;
    gchar *user;
} Learned;

static void
on_learned(ClawtVentureMirror *mirror, const gchar *agent_id,
           const gchar *username, gpointer data)
{
    Learned *learned = data;

    (void)mirror;
    learned->count++;
    g_free(learned->agent);
    g_free(learned->user);
    learned->agent = g_strdup(agent_id);
    learned->user = g_strdup(username);
}

/*
 * Each account's username is announced once, when whoami first answers:
 * the daemon tells the agent who it is in VENTURE from this, and a
 * signal on every pass would rewrite its TOOLS.org for nothing.
 */
static void
test_account_learned_once(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(ClawtTask) again = NULL;
    Learned learned = { 0 };
    gulong handler;

    (void)data;
    handler = g_signal_connect(fixture->mirror, "account-learned",
                               G_CALLBACK(on_learned), &learned);

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);
    g_assert_cmpuint(learned.count, ==, 2);
    g_assert_nonnull(learned.agent);
    g_assert_true(g_str_has_suffix(learned.user, "-bot"));

    /* another task, the same accounts: nothing new to say */
    again = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);
    g_assert_cmpuint(learned.count, ==, 2);

    g_signal_handler_disconnect(fixture->mirror, handler);
    g_free(learned.agent);
    g_free(learned.user);
}


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
    /* the repository's organization (3) is what the link switches to */
    g_assert_cmpint(ref->organization_id, ==, 3);
    g_assert_cmpstr(ref->url, ==,
                    "http://v.test/entity/3?back=%2Fe%2Fticket%2F1");

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
    g_free(fixture->intake_url);
    fixture->intake_url = g_strdup(url);
    g_free(fixture->intake_task);
    fixture->intake_task = g_strdup(clawt_task_get_id(task));

    /* as the daemon does: "#<ticket> <title>", cut to the task limit */
    if (fixture->name_intake) {
        g_autofree gchar *named = g_strdup_printf("#%" G_GINT64_FORMAT " %s",
                                                  ticket, title);

        clawt_task_set_title(task, named);
        g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    }

    g_ptr_array_add(fixture->inbound, g_strdup_printf(
        "intake %" G_GINT64_FORMAT " %s %s%s%s", ticket, agent_id, title,
        from_agent != NULL ? " from " : "",
        from_agent != NULL ? from_agent : ""));

    /* the task it made is one more open task, as the daemon counts */
    g_hash_table_insert(fixture->open_tasks, g_strdup(agent_id),
        GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(
            fixture->open_tasks, agent_id)) + 1));

    return g_strdup(clawt_task_get_id(task));
}

/* Every account in the fixture is a fleet agent; "user" is not. */
static gint
rec_standing(gpointer data, const gchar *agent_id, gboolean *leads)
{
    Fixture *fixture = data;

    if (g_strcmp0(agent_id, "user") == 0)
        return -1;

    *leads = (g_strcmp0(agent_id, fixture->leads) == 0);
    return GPOINTER_TO_INT(g_hash_table_lookup(fixture->open_tasks, agent_id));
}

static const ClawtVentureMirrorInbound recorder = {
    rec_cancel, rec_review, rec_comment, rec_intake, rec_standing
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

/* The record link: the organization switch, the bare page, the base. */
static void
test_record_url(void)
{
    g_autofree gchar *a = clawt_venture_record_url("https://v.example",
                                                   "ticket", 12, 2);
    g_autofree gchar *b = clawt_venture_record_url("https://v.example",
                                                   "ticket", 12, 0);
    g_autofree gchar *c = clawt_venture_record_url("https://v.example/",
                                                   "ticket", 12, 2);
    g_autofree gchar *d = clawt_venture_record_url("https://v.example/",
                                                   "ticket", 12, -1);

    g_assert_cmpstr(a, ==,
                    "https://v.example/entity/2?back=%2Fe%2Fticket%2F12");
    g_assert_cmpstr(b, ==, "https://v.example/e/ticket/12");
    g_assert_cmpstr(c, ==, a);
    g_assert_cmpstr(d, ==, b);
}

/* A created ticket in another organization: the link carries it. */
static void
test_created_ticket_link_carries_its_organization(Fixture *fixture,
                                                  gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    fixture->create_org = 2;
    drain(fixture);

    ref = clawt_task_store_venture_ref(fixture->store,
                                       clawt_task_get_id(task));
    g_assert_nonnull(ref);
    g_assert_cmpint(ref->organization_id, ==, 2);
    g_assert_cmpstr(ref->url, ==,
                    "http://v.test/entity/2?back=%2Fe%2Fticket%2F1");
}

/* An intake ticket in organization 2: stored ref and body url agree. */
static void
test_intake_link_carries_the_organization(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->assigned = g_strdup(
        "[{\"id\":40,\"version\":1,\"title\":\"Other org\","
        "  \"organization_id\":2,\"status\":\"todo\"}]");

    poll(fixture);
    g_assert_cmpstr(fixture->intake_url, ==,
                    "http://v.test/entity/2?back=%2Fe%2Fticket%2F40");

    /* and it holds after the outbox has written the ticket back */
    drain(fixture);
    ref = clawt_task_store_venture_ref(fixture->store, fixture->intake_task);
    g_assert_nonnull(ref);
    g_assert_cmpint(ref->organization_id, ==, 2);
    g_assert_cmpstr(ref->url, ==, fixture->intake_url);
}

/* How many PATCHes to a ticket carried a title member. */
static guint
patches_with_title(Fixture *fixture, const gchar *path, guint *patches)
{
    guint with = 0;
    guint i;

    *patches = 0;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);
        g_autoptr(JsonNode) holder = NULL;

        if (g_strcmp0(sent->method, "PATCH") != 0 ||
            g_strcmp0(sent->url, path) != 0)
            continue;

        (*patches)++;
        if (json_object_has_member(body_of(sent, &holder), "title"))
            with++;
    }

    return with;
}

/* A ticket taken in keeps its title: never written, however often. */
static void
test_intake_title_is_never_written(Fixture *fixture, gconstpointer data)
{
    g_autofree gchar *long_title = g_strnfill(130, 'w');
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(JsonNode) holder = NULL;
    Sent *sent;
    guint patches = 0;

    (void)data;
    fixture->name_intake = TRUE;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->assigned = g_strdup_printf(
        "[{\"id\":50,\"version\":2,\"title\":\"%s\","
        "  \"status\":\"todo\"}]", long_title);

    poll(fixture);

    /* the first update: reference, status and assignee -- not the title */
    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/50");
    g_assert_nonnull(sent);
    g_assert_false(json_object_has_member(body_of(sent, &holder), "title"));
    g_assert_true(json_object_has_member(json_node_get_object(holder),
                                         "external_ref"));

    /* a second update, on the task's next move: still not */
    task = clawt_task_store_get(fixture->store, fixture->intake_task);
    g_assert_nonnull(task);
    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    g_assert_cmpuint(patches_with_title(fixture, "/api/v1/ticket/50",
                                        &patches), ==, 0);
    g_assert_cmpuint(patches, >=, 2);
}

/* A ticket the mirror filed still has its title kept current. */
static void
test_outbound_title_is_still_written(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_PENDING);
    g_autoptr(JsonNode) holder = NULL;
    Sent *sent;

    (void)data;
    drain(fixture);

    clawt_task_set_title(task, "A better title");
    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/1");
    g_assert_nonnull(sent);
    g_assert_cmpstr(json_object_get_string_member(body_of(sent, &holder),
                                                  "title"), ==,
                    "A better title");
}

/*
 * A ref stored before intake wrote `_intake`: a receipt of the ticket as
 * it was, with no `_repo`.  It is still an intake ticket.
 */
static void
test_old_intake_ref_is_recognised(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "user", "scout",
                                          CLAWT_TASK_PENDING);
    ClawtVentureRef ref = { 0 };
    g_autoptr(JsonNode) holder = NULL;
    Sent *sent;

    (void)data;
    ref.task_id = (gchar *)clawt_task_get_id(task);
    ref.instance = (gchar *)"books-scout";
    ref.ticket_id = 60;
    ref.version = 1;
    ref.snapshot = (gchar *)"{\"title\":\"Theirs\",\"status\":\"todo\","
                            "\"assignee\":\"scout-bot\"}";
    ref.state = (gchar *)"synced";
    g_assert_true(clawt_task_store_put_venture_ref(fixture->store, &ref,
                                                   NULL));

    clawt_task_set_state(task, CLAWT_TASK_RUNNING);
    g_assert_true(clawt_task_store_save(fixture->store, task, NULL, NULL));
    drain(fixture);

    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/60");
    g_assert_nonnull(sent);
    g_assert_false(json_object_has_member(body_of(sent, &holder), "title"));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(holder),
                                                  "status"), ==,
                    "in_progress");
}

/* ── Every organization the token reaches ────────────────────────── */

/*
 * VENTURE answers a list without organization_id from the token's
 * default organization only, so whoami's memberships are what the
 * reverse poll and adoption have to name.  The fake answers the same way.
 */
#define TWO_ORGANIZATIONS \
    "[{\"organization_id\":1,\"organization\":\"Anyware\",\"role\":\"editor\"}," \
    " {\"organization_id\":2,\"organization\":\"Personal\",\"role\":\"editor\"}]"

/* A todo ticket in each organization the account is in becomes a task. */
static void
test_intake_reads_every_organization(Fixture *fixture, gconstpointer data)
{
    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);
    fixture->assigned = g_strdup(
        "[{\"id\":30,\"version\":1,\"title\":\"Do X\",\"status\":\"todo\","
        "  \"organization_id\":1}]");
    fixture->assigned_2 = g_strdup(
        "[{\"id\":40,\"version\":1,\"title\":\"At home\",\"status\":\"todo\","
        "  \"organization_id\":2}]");

    poll(fixture);

    g_assert_true(inbound_has(fixture, "intake 30 scout Do X from lead"));
    g_assert_true(inbound_has(fixture, "intake 40 scout At home"));
    g_assert_cmpuint(fixture->inbound->len, ==, 2);
    g_assert_nonnull(find_sent(fixture, "GET",
        "/api/v1/ticket?assignee=scout-bot&status=todo&kind=internal"
        "&order=id&limit=10&organization_id=2"));

    /* taken in once, whichever organization it came from */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 2);
}

/* Without memberships in whoami, the one list is asked as it always was. */
static void
test_intake_without_memberships_asks_once(Fixture *fixture,
                                          gconstpointer data)
{
    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->assigned = g_strdup(
        "[{\"id\":30,\"version\":1,\"title\":\"Do X\",\"status\":\"todo\"}]");

    poll(fixture);

    g_assert_true(inbound_has(fixture, "intake 30 scout Do X from lead"));
    g_assert_cmpuint(count_sent(fixture, "GET",
        "/api/v1/ticket?assignee=scout-bot"), ==, 1);
    g_assert_null(strstr(find_sent(fixture, "GET",
        "/api/v1/ticket?assignee=scout-bot")->url, "organization_id"));
}

/* Ten an hour is the agent's, not ten an hour in each organization. */
static void
test_intake_cap_spans_organizations(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GString) first = g_string_new("[");
    guint i;

    (void)data;
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);

    /* nine in the first, two in the second: the eleventh waits */
    for (i = 0; i < 9; i++)
        g_string_append_printf(first,
            "%s{\"id\":%u,\"version\":1,\"title\":\"T%u\",\"status\":\"todo\"}",
            i > 0 ? "," : "", 100 + i, i);
    g_string_append(first, "]");
    fixture->assigned = g_string_free(g_steal_pointer(&first), FALSE);
    fixture->assigned_2 = g_strdup(
        "[{\"id\":200,\"version\":1,\"title\":\"H0\",\"status\":\"todo\"},"
        " {\"id\":201,\"version\":1,\"title\":\"H1\",\"status\":\"todo\"}]");

    poll(fixture);

    g_assert_cmpuint(fixture->inbound->len, ==, 10);
    g_assert_true(inbound_has(fixture, "intake 200 scout H0"));
    g_assert_false(inbound_has(fixture, "intake 201 scout H1"));
}

/* A person cancelling a ticket in the second organization is heard. */
static void
test_changes_read_in_every_organization(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    g_autofree gchar *line = g_strdup_printf("cancel %s",
                                             clawt_task_get_id(task));

    (void)data;
    fixture->create_org = 2;
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    fixture->changed_2 = changed_ticket(clawt_task_get_id(task), 2,
                                        "cancelled");
    poll(fixture);

    g_assert_true(inbound_has(fixture, line));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/* A person's comment on a ticket in the second organization is heard. */
static void
test_comments_read_in_every_organization(Fixture *fixture, gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    g_autofree gchar *line = g_strdup_printf("comment %s alice: why?",
                                             clawt_task_get_id(task));

    (void)data;
    fixture->create_org = 2;
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    /* the first pass only finds where each organization is up to */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 0);

    fixture->comments_2 = g_strdup(
        "[{\"id\":6,\"ticket_id\":1,\"author\":\"alice\",\"body\":\"why?\"}]");
    poll(fixture);

    g_assert_true(inbound_has(fixture, line));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/*
 * An organization read for the first time is read as a first pass is,
 * whatever the account's cursor from before says: changes from a day
 * back, comments from a baseline -- never a replay from that cursor,
 * which only ever said where the default organization was up to.
 */
static void
test_a_new_organization_starts_from_its_own_baseline(Fixture *fixture,
                                                     gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    Sent *changes = NULL;
    guint i;

    (void)data;
    fixture->create_org = 2;
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     FALSE);
    drain(fixture);

    /* what an earlier version kept: the default organization, quiet */
    clawt_task_store_put_venture_cursor(fixture->store,
                                        "http://v.test|scout|comments", "0");
    clawt_task_store_put_venture_cursor(fixture->store,
                                        "http://v.test|scout|changes",
                                        "2026-11-26T10:00:00Z");
    fixture->comments_2 = g_strdup(
        "[{\"id\":4,\"ticket_id\":1,\"author\":\"alice\",\"body\":\"old\"}]");
    poll(fixture);

    /* nothing replayed: a baseline, in the second organization too */
    g_assert_cmpuint(fixture->inbound->len, ==, 0);
    g_assert_null(find_sent(fixture, "GET",
        "/api/v1/ticket_comment?ticket_id__in=1&id__gt=0"));

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_str_has_prefix(sent->url, "/api/v1/ticket?external_ref__like=") &&
            g_strcmp0(sent->token, "t-scout") == 0 &&
            strstr(sent->url, "organization_id=2") != NULL)
            changes = sent;
    }

    g_assert_nonnull(changes);
    g_assert_null(strstr(changes->url, "2026-11-26T10%3A00%3A00Z"));
    g_assert_null(strstr(changes->url, "2026-11-26T10:00:00Z"));
}

/* A ticket filed in its repository's organization is adopted from there. */
static void
test_adoption_reads_the_tickets_organization(Fixture *fixture,
                                             gconstpointer data)
{
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(ClawtVentureRef) ref = NULL;

    (void)data;
    fixture->create_conflicts = 1;
    fixture->adopt_org = 3;    /* the known repository's, from resolve */

    task = save_task(fixture, "lead", "scout", CLAWT_TASK_PENDING);
    drain(fixture);

    g_assert_nonnull(find_sent(fixture, "GET",
                               "/api/v1/ticket?external_ref=clawtilla"));
    g_assert_nonnull(strstr(find_sent(fixture, "GET",
        "/api/v1/ticket?external_ref=clawtilla")->url, "&organization_id=3"));

    ref = clawt_task_store_venture_ref(fixture->store, clawt_task_get_id(task));
    g_assert_nonnull(ref);
    g_assert_cmpint(ref->ticket_id, ==, 42);
    g_assert_cmpstr(ref->state, !=, "dead");
}

/* And a comment already made is found in its ticket's organization. */
static void
test_comment_adoption_reads_the_tickets_organization(Fixture *fixture,
                                                     gconstpointer data)
{
    g_autoptr(ClawtTask) task = save_task(fixture, "lead", "scout",
                                          CLAWT_TASK_RUNNING);
    gint64 note;

    (void)data;
    fixture->create_org = 3;
    drain(fixture);

    fixture->comment_conflicts = 1;
    fixture->adopt_org = 3;
    note = clawt_task_store_add_note(fixture->store, clawt_task_get_id(task),
                                     "scout", "result", "Fixed.", NULL);
    drain(fixture);

    g_assert_nonnull(strstr(find_sent(fixture, "GET",
        "/api/v1/ticket_comment?external_ref=")->url, "&organization_id=3"));
    g_assert_cmpint(clawt_task_store_venture_comment(fixture->store, note),
                    ==, 77);
}

/* ── Tickets for a team ──────────────────────────────────────────── */

/*
 * lead (user 11), scout (12) and helper (13), every one with intake on,
 * and a team ticket waiting in the default organization -- nobody's and
 * no other system's written null, as VENTURE answers a ticket never
 * given either.  The team's
 * memberships name all three, a person who is no fleet agent (user 40)
 * and an old, inactive membership of helper's beside its active one.
 */
static void
team_setup(Fixture *fixture)
{
    clawt_venture_mirror_set_account(fixture->mirror, "helper", "books-helper",
                                     "http://v.test", "t-helper");
    clawt_venture_mirror_set_inbound(fixture->mirror, &recorder, fixture,
                                     TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "lead", TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "scout", TRUE);
    clawt_venture_mirror_set_account_intake(fixture->mirror, "helper", TRUE);
    fixture->team_members = g_strdup(
        "{\"records\":["
        "{\"id\":1,\"team_id\":7,\"user_id\":40,\"active\":true},"
        "{\"id\":2,\"team_id\":7,\"user_id\":12,\"active\":true},"
        "{\"id\":3,\"team_id\":7,\"user_id\":13,\"active\":false},"
        "{\"id\":4,\"team_id\":7,\"user_id\":11,\"active\":true},"
        "{\"id\":5,\"team_id\":7,\"user_id\":13,\"active\":true}],"
        "\"count\":5,\"total\":5}");
    fixture->team_tickets = g_strdup(
        "{\"records\":[{\"id\":50,\"version\":2,\"title\":\"Team work\","
        "\"description\":\"d\",\"status\":\"todo\",\"kind\":\"internal\","
        "\"team_id\":7,\"assignee\":null,\"external_ref\":null,"
        "\"organization_id\":1}],\"count\":1,\"total\":1}");
}

static void
set_open(Fixture *fixture, const gchar *agent, gint n)
{
    g_hash_table_insert(fixture->open_tasks, g_strdup(agent),
                        GINT_TO_POINTER(n));
}

/*
 * A member that leads a clawtilla team takes the team's ticket, however
 * busy it is; the team's tickets are read once a pass per organization,
 * not once per account.
 */
static void
test_team_ticket_goes_to_the_lead(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    fixture->leads = g_strdup("lead");
    set_open(fixture, "lead", 5);

    poll(fixture);

    g_assert_true(inbound_has(fixture, "intake 50 lead Team work"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
    /* the fleet's teams first, then only their tickets -- once */
    g_assert_cmpuint(count_sent(fixture, "GET",
        "/api/v1/team_membership?user_id__in="), ==, 1);
    g_assert_nonnull(strstr(find_sent(fixture, "GET",
        "/api/v1/team_membership?user_id__in=")->url, "&active=true&"));
    g_assert_cmpuint(count_sent(fixture, "GET",
        "/api/v1/ticket?team_id=7&assignee=&external_ref=&status=todo"
        "&kind=internal&id__gt=0&order=id&limit=10"), ==, 1);
}

/*
 * No member leads: the one with the fewest open tasks takes it.  A
 * member who is no fleet agent counts for nothing.
 */
static void
test_team_ticket_goes_to_the_least_busy(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    set_open(fixture, "lead", 4);
    set_open(fixture, "scout", 1);
    set_open(fixture, "helper", 2);

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 50 scout Team work"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/*
 * A membership that is not active makes nobody a member, even should
 * VENTURE answer it: helper, idle, has only an inactive one.
 */
static void
test_team_inactive_member_is_not_chosen(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    g_free(fixture->team_members);
    fixture->team_members = g_strdup(
        "[{\"id\":2,\"team_id\":7,\"user_id\":12,\"active\":true},"
        " {\"id\":3,\"team_id\":7,\"user_id\":13,\"active\":false}]");
    set_open(fixture, "scout", 5);

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 50 scout Team work"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/* A tie goes to the first by agent id. */
static void
test_team_tie_goes_to_the_first_by_id(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    set_open(fixture, "lead", 1);
    set_open(fixture, "scout", 1);
    set_open(fixture, "helper", 1);

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 50 helper Team work"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/*
 * Whoever took it is written back as the ticket's assignee, with its own
 * token -- so people see who has it, and the next pass leaves it alone.
 */
static void
test_team_assignee_is_written_back(Fixture *fixture, gconstpointer data)
{
    g_autoptr(JsonNode) patch = NULL;
    JsonObject *o;
    Sent *sent;

    (void)data;
    team_setup(fixture);
    set_open(fixture, "lead", 3);
    set_open(fixture, "scout", 3);

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 50 helper Team work"));

    sent = find_sent(fixture, "PATCH", "/api/v1/ticket/50");
    g_assert_nonnull(sent);
    g_assert_cmpstr(sent->token, ==, "t-helper");
    o = body_of(sent, &patch);
    g_assert_cmpstr(json_object_get_string_member(o, "assignee"), ==,
                    "helper-bot");
    g_assert_cmpint(json_object_get_int_member(o, "version"), ==, 2);
    g_assert_nonnull(g_strrstr(json_object_get_string_member(o,
                                                             "external_ref"),
                               "clawtilla:"));
    /* never its title: the author's, as on any ticket taken in */
    g_assert_false(json_object_has_member(o, "title"));

    /* VENTURE still lists it (as it would, before the write lands) */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/* The same ten an hour: a team ticket counts against whoever takes it. */
static void
test_team_cap_is_held(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GString) rows = g_string_new("[");
    guint i;

    (void)data;
    team_setup(fixture);
    /* helper and scout; scout is far busier, so helper is chosen */
    g_free(fixture->team_members);
    fixture->team_members = g_strdup(
        "[{\"id\":2,\"team_id\":7,\"user_id\":12,\"active\":true},"
        " {\"id\":5,\"team_id\":7,\"user_id\":13,\"active\":true}]");
    set_open(fixture, "scout", 100);

    for (i = 0; i < 11; i++)
        g_string_append_printf(rows,
            "%s{\"id\":%u,\"version\":1,\"title\":\"T%u\",\"status\":\"todo\","
            "\"team_id\":7,\"assignee\":null}", i > 0 ? "," : "", 300 + i, i);
    g_string_append(rows, "]");
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_string_free(g_steal_pointer(&rows), FALSE);

    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 10);
    g_assert_true(inbound_has(fixture, "intake 309 helper T9"));
    /* the eleventh waits for helper's next hour: not passed to scout */
    g_assert_false(inbound_has(fixture, "intake 310 helper T10"));
    g_assert_false(inbound_has(fixture, "intake 310 scout T10"));
}

/* A team in the second organization is read there, and its ticket taken. */
static void
test_team_in_the_second_organization(Fixture *fixture, gconstpointer data)
{
    Sent *members;

    (void)data;
    team_setup(fixture);
    fixture->memberships = g_strdup(TWO_ORGANIZATIONS);
    g_free(fixture->team_tickets);
    fixture->team_tickets = NULL;
    fixture->team_tickets_2 = g_strdup(
        "[{\"id\":60,\"version\":1,\"title\":\"At home\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":\"\",\"organization_id\":2}]");
    set_open(fixture, "lead", 2);
    set_open(fixture, "helper", 2);

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 60 scout At home"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
    g_assert_nonnull(find_sent(fixture, "GET",
        "/api/v1/ticket?team_id=7&assignee=&external_ref=&status=todo"
        "&kind=internal&id__gt=0&order=id&limit=10&organization_id=2"));
    members = NULL;
    {
        guint i;

        for (i = 0; i < fixture->sent->len; i++) {
            Sent *sent = g_ptr_array_index(fixture->sent, i);

            if (g_str_has_prefix(sent->url,
                                 "/api/v1/team_membership?user_id__in=") &&
                strstr(sent->url, "organization_id=2") != NULL)
                members = sent;
        }
    }
    g_assert_nonnull(members);
}

/*
 * An agent that put the ticket in the team through its own VENTURE
 * account handed it over: a delegation from that agent, held to the loop
 * limits, as an assignment through VENTURE is.
 */
static void
test_team_handed_over_by_an_agent(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":51,\"version\":1,\"title\":\"Passed on\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":\"\"}]");
    fixture->leads = g_strdup("lead");

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 51 lead Passed on from scout"));
}

/* No member that is a fleet agent, or somebody has it: left alone. */
static void
test_team_ticket_nobody_can_take(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    g_free(fixture->team_members);
    fixture->team_members = g_strdup(
        "[{\"id\":1,\"team_id\":7,\"user_id\":40,\"active\":true}]");
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":52,\"version\":1,\"title\":\"People only\","
        "\"status\":\"todo\",\"team_id\":7,\"assignee\":\"\"}]");

    poll(fixture);
    /* the memberships were read; no team has an agent, so no tickets */
    g_assert_nonnull(find_sent(fixture, "GET",
                               "/api/v1/team_membership?user_id__in="));
    g_assert_null(find_sent(fixture, "GET", "/api/v1/ticket?team_id="));
    g_assert_cmpuint(fixture->inbound->len, ==, 0);
    g_assert_null(find_sent(fixture, "PATCH", "/api/v1/ticket/52"));
}

/* In a team of agents, a ticket somebody already has is theirs. */
static void
test_team_ticket_somebody_has(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":53,\"version\":1,\"title\":\"Somebody has it\","
        "\"status\":\"todo\",\"team_id\":7,\"assignee\":\"alice\"},"
        " {\"id\":54,\"version\":1,\"title\":\"Free\","
        "\"status\":\"todo\",\"team_id\":7,\"assignee\":null}]");

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 54 helper Free"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
    g_assert_null(find_sent(fixture, "PATCH", "/api/v1/ticket/53"));
}

/*
 * An agent that gave the ticket to its own team and was chosen to take
 * it: still a hand-over from that agent, held to the loop limits -- never
 * operator work, which would start a fresh chain with its own budget.
 */
static void
test_team_given_to_its_own_team(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":51,\"version\":1,\"title\":\"Passed on\","
        "\"status\":\"todo\",\"team_id\":7,\"assignee\":\"\"}]");
    fixture->leads = g_strdup("scout");   /* scout set the team, and leads */

    poll(fixture);
    g_assert_true(inbound_has(fixture, "intake 51 scout Passed on from scout"));
    g_assert_false(inbound_has(fixture, "intake 51 scout Passed on"));
    g_assert_cmpuint(fixture->inbound->len, ==, 1);
}

/* More than a page of team tickets: the next page is read too. */
static void
test_team_tickets_are_paged(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GString) rows = g_string_new("[");
    guint i;

    (void)data;
    team_setup(fixture);
    for (i = 0; i < 12; i++)
        g_string_append_printf(rows,
            "%s{\"id\":%u,\"version\":1,\"title\":\"P%u\","
            "\"status\":\"todo\",\"team_id\":7,\"assignee\":null}",
            i > 0 ? "," : "", 400 + i, i);
    g_string_append(rows, "]");
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_string_free(g_steal_pointer(&rows), FALSE);

    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 12);
    g_assert_nonnull(find_sent(fixture, "GET",
        "/api/v1/ticket?team_id=7&assignee=&external_ref=&status=todo"
        "&kind=internal&id__gt=409&order=id&limit=10"));
}

/*
 * Tickets already linked to tasks are left out of the list once seen
 * there: their assignee may never be written, and a hundred of them
 * would otherwise fill every page ahead of the rest.  The list is still
 * read from the start each pass.
 */
static void
test_team_linked_tickets_are_left_out(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GString) rows = g_string_new("[");
    guint i;

    (void)data;
    team_setup(fixture);
    g_free(fixture->team_members);
    fixture->team_members = g_strdup(
        "[{\"id\":5,\"team_id\":7,\"user_id\":13,\"active\":true}]");
    for (i = 0; i < 12; i++)
        g_string_append_printf(rows,
            "%s{\"id\":%u,\"version\":1,\"title\":\"L%u\","
            "\"status\":\"todo\",\"team_id\":7,\"assignee\":null}",
            i > 0 ? "," : "", 500 + i, i);
    g_string_append(rows, "]");
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_string_free(g_steal_pointer(&rows), FALSE);

    /* ten taken (the cap), two held back */
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 10);

    /* VENTURE still lists all twelve: seen linked, then left out */
    poll(fixture);
    poll(fixture);
    g_assert_cmpuint(count_sent(fixture, "GET",
        "/api/v1/ticket?team_id=7&assignee=&external_ref=&status=todo"
        "&kind=internal&id__gt=0&"), ==, 3);
    g_assert_nonnull(find_sent(fixture, "GET",
        "/api/v1/ticket?team_id=7&assignee=&external_ref=&status=todo"
        "&kind=internal&id__gt=0&id__not_in=500,501,502,503,504,505,506,"
        "507,508,509&order=id&limit=10"));
    g_assert_cmpuint(fixture->inbound->len, ==, 10);
}

/*
 * An old ticket given to the team after newer ones were taken -- a low
 * id behind them -- is still read and taken.
 */
static void
test_team_ticket_given_late_is_taken(Fixture *fixture, gconstpointer data)
{
    (void)data;
    team_setup(fixture);
    set_open(fixture, "lead", 100);
    set_open(fixture, "scout", 100);
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":500,\"version\":1,\"title\":\"N0\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":null},"
        " {\"id\":501,\"version\":1,\"title\":\"N1\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":null}]");

    poll(fixture);
    poll(fixture);
    g_assert_cmpuint(fixture->inbound->len, ==, 2);

    /* a person triages an old ticket into the team; 500 and 501 linger */
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_strdup(
        "[{\"id\":120,\"version\":4,\"title\":\"Old one\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":null},"
        " {\"id\":500,\"version\":1,\"title\":\"N0\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":null},"
        " {\"id\":501,\"version\":1,\"title\":\"N1\",\"status\":\"todo\","
        "\"team_id\":7,\"assignee\":null}]");
    poll(fixture);

    g_assert_true(inbound_has(fixture, "intake 120 helper Old one"));
    g_assert_cmpuint(fixture->inbound->len, ==, 3);
}

/*
 * One team's backlog held back by its agent's cap does not hold back
 * another team: each team is read on its own, and a capped one stops.
 */
static void
test_team_capped_does_not_hold_another(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GString) rows = g_string_new("[");
    guint i;

    (void)data;
    team_setup(fixture);
    g_free(fixture->team_members);
    fixture->team_members = g_strdup(
        "[{\"id\":5,\"team_id\":7,\"user_id\":13,\"active\":true},"
        " {\"id\":6,\"team_id\":9,\"user_id\":12,\"active\":true}]");

    /* team 7: far more than the pages a pass reads; team 9: one, newer */
    for (i = 0; i < 120; i++)
        g_string_append_printf(rows,
            "{\"id\":%u,\"version\":1,\"title\":\"B%u\",\"status\":\"todo\","
            "\"team_id\":7,\"assignee\":null},", 1 + i, i);
    g_string_append(rows,
        "{\"id\":1000,\"version\":1,\"title\":\"Other team\","
        "\"status\":\"todo\",\"team_id\":9,\"assignee\":null}]");
    g_free(fixture->team_tickets);
    fixture->team_tickets = g_string_free(g_steal_pointer(&rows), FALSE);

    poll(fixture);

    g_assert_true(inbound_has(fixture, "intake 1000 scout Other team"));
    g_assert_cmpuint(fixture->inbound->len, ==, 11);

    /*
     * Each team is read with a member's own token -- one VENTURE lets
     * see the team's tickets whatever its role -- and a ticket's timeline
     * with the taker's: helper reads the organization, but team 9 is
     * scout's.
     */
    {
        Sent *list = find_sent(fixture, "GET", "/api/v1/ticket?team_id=9&");
        Sent *timeline = find_sent(fixture, "GET",
                                   "/api/v1/activity/ticket/1000");

        g_assert_nonnull(list);
        g_assert_cmpstr(list->token, ==, "t-scout");
        g_assert_nonnull(timeline);
        g_assert_cmpstr(timeline->token, ==, "t-scout");
        g_assert_cmpstr(find_sent(fixture, "GET",
            "/api/v1/team_membership?user_id__in=")->token, ==, "t-helper");
    }
    /* team 7 stopped at its cap: two pages, not ten */
    g_assert_cmpuint(count_sent(fixture, "GET", "/api/v1/ticket?team_id=7&"),
                     ==, 2);
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
    ADD("account-learned-once", test_account_learned_once);
    g_test_add_func("/venture-mirror/record-url", test_record_url);
    ADD("created-link-carries-organization",
        test_created_ticket_link_carries_its_organization);
    ADD("intake-link-carries-organization",
        test_intake_link_carries_the_organization);
    ADD("intake-title-is-never-written", test_intake_title_is_never_written);
    ADD("outbound-title-is-still-written",
        test_outbound_title_is_still_written);
    ADD("old-intake-ref-is-recognised", test_old_intake_ref_is_recognised);
    ADD("intake-reads-every-organization",
        test_intake_reads_every_organization);
    ADD("intake-without-memberships-asks-once",
        test_intake_without_memberships_asks_once);
    ADD("intake-cap-spans-organizations",
        test_intake_cap_spans_organizations);
    ADD("changes-read-in-every-organization",
        test_changes_read_in_every_organization);
    ADD("comments-read-in-every-organization",
        test_comments_read_in_every_organization);
    ADD("a-new-organization-starts-from-its-own-baseline",
        test_a_new_organization_starts_from_its_own_baseline);
    ADD("adoption-reads-the-tickets-organization",
        test_adoption_reads_the_tickets_organization);
    ADD("comment-adoption-reads-the-tickets-organization",
        test_comment_adoption_reads_the_tickets_organization);
    ADD("team-ticket-goes-to-the-lead", test_team_ticket_goes_to_the_lead);
    ADD("team-inactive-member-is-not-chosen",
        test_team_inactive_member_is_not_chosen);
    ADD("team-ticket-goes-to-the-least-busy",
        test_team_ticket_goes_to_the_least_busy);
    ADD("team-tie-goes-to-the-first-by-id",
        test_team_tie_goes_to_the_first_by_id);
    ADD("team-assignee-is-written-back", test_team_assignee_is_written_back);
    ADD("team-cap-is-held", test_team_cap_is_held);
    ADD("team-in-the-second-organization",
        test_team_in_the_second_organization);
    ADD("team-handed-over-by-an-agent", test_team_handed_over_by_an_agent);
    ADD("team-ticket-nobody-can-take", test_team_ticket_nobody_can_take);
    ADD("team-ticket-somebody-has", test_team_ticket_somebody_has);
    ADD("team-given-to-its-own-team", test_team_given_to_its_own_team);
    ADD("team-tickets-are-paged", test_team_tickets_are_paged);
    ADD("team-linked-tickets-are-left-out",
        test_team_linked_tickets_are_left_out);
    ADD("team-ticket-given-late-is-taken", test_team_ticket_given_late_is_taken);
    ADD("team-capped-does-not-hold-another",
        test_team_capped_does_not_hold_another);
    g_test_add_func("/venture-mirror/finds-pull-requests",
                    test_finds_pull_requests);

    return g_test_run();
}
