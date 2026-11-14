/*
 * test-grok.c - Grok bots: naming them, sending them work, hearing back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: nothing here reaches Grok, tailscaled or the network.  The
 * library half is pure; the daemon half drives clawt_daemon_grok_send(),
 * clawt_daemon_grok_handle() and clawt_daemon_grok_sweep() directly,
 * which is the whole of what the listener and the timer call -- so a
 * report, an MCP call and an expiry are exercised exactly as a bot and a
 * clock would, without a socket.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

/* ── Which bot ───────────────────────────────────────────────────── */

static void
test_a_bot_is_found_by_name_case_and_prefix(void)
{
    static const gchar *const names[] = { "chief", "research", "ops", NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(clawt_grok_resolve_bot(names, "chief", NULL), ==, 0);
    g_assert_cmpint(clawt_grok_resolve_bot(names, "Chief", NULL), ==, 0);
    g_assert_cmpint(clawt_grok_resolve_bot(names, "res", NULL), ==, 1);
    g_assert_cmpint(clawt_grok_resolve_bot(names, "OPS", NULL), ==, 2);

    /* Unknown: refused, and every reachable name is listed. */
    g_assert_cmpint(clawt_grok_resolve_bot(names, "butler", &error), ==, -1);
    g_assert_nonnull(strstr(error->message, "chief"));
    g_assert_nonnull(strstr(error->message, "research"));
    g_assert_nonnull(strstr(error->message, "ops"));
}

/*
 * A prefix two names share is refused rather than guessed, because a job
 * handed to the wrong bot does its work somewhere nobody looks.
 */
static void
test_an_ambiguous_prefix_is_refused_with_the_names(void)
{
    static const gchar *const names[] = { "research", "reports", NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(clawt_grok_resolve_bot(names, "re", &error), ==, -1);
    g_assert_nonnull(strstr(error->message, "more than one"));
    g_assert_nonnull(strstr(error->message, "research"));
    g_assert_nonnull(strstr(error->message, "reports"));
}

static void
test_no_bots_is_its_own_refusal(void)
{
    static const gchar *const none[] = { NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(clawt_grok_resolve_bot(none, "chief", &error), ==, -1);
    g_assert_nonnull(strstr(error->message, "no Grok bot has been shared"));
}

/* ── Tokens, envelopes and answers ───────────────────────────────── */

static void
test_a_token_is_kept_only_as_a_digest(void)
{
    g_autofree gchar *digest = clawt_grok_token_digest("tok-123");

    g_assert_cmpuint(strlen(digest), ==, 64);
    g_assert_null(strstr(digest, "tok-123"));
    g_assert_true(clawt_grok_token_matches(digest, "tok-123"));
    g_assert_false(clawt_grok_token_matches(digest, "tok-124"));
    g_assert_false(clawt_grok_token_matches(digest, ""));
    g_assert_false(clawt_grok_token_matches(digest, NULL));
    g_assert_false(clawt_grok_token_matches(NULL, "tok-123"));
}

static JsonObject *
parse_object(JsonParser *parser, const gchar *text)
{
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));

    return json_node_get_object(json_parser_get_root(parser));
}

/*
 * The envelope says how to answer, every time, and offers the MCP route
 * only when the bot has been connected to it -- a tool named that the bot
 * cannot reach is a report sent nowhere.
 */
static void
test_the_envelope_says_how_to_report(void)
{
    g_autoptr(ClawtGrokJob) job = clawt_grok_job_new("gj-1", "chief");
    g_autofree gchar *plain = NULL;
    g_autofree gchar *with_mcp = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonParser) parser2 = json_parser_new();
    JsonObject *root;
    JsonObject *inner;
    JsonObject *report;
    JsonObject *http;

    clawt_grok_job_set_agent_id(job, "oryx");
    clawt_grok_job_set_expires_at(job, (gint64)1800000000 * G_USEC_PER_SEC);

    plain = clawt_grok_envelope(job, "find three venues", "near the office",
                                "http://box.ts.net:8788/grok/gj-1", "tok",
                                NULL);
    root = parse_object(parser, plain);
    inner = json_object_get_object_member(root, "clawtilla");

    g_assert_cmpstr(json_object_get_string_member(inner, "id"), ==, "gj-1");
    g_assert_cmpstr(json_object_get_string_member(inner, "from_agent"), ==,
                    "oryx");
    g_assert_cmpstr(json_object_get_string_member(inner, "instructions"), ==,
                    "find three venues");
    g_assert_cmpstr(json_object_get_string_member(inner, "context"), ==,
                    "near the office");
    g_assert_true(g_str_has_prefix(
        json_object_get_string_member(inner, "expires_at"), "2027-01-15"));

    report = json_object_get_object_member(inner, "report");
    http = json_object_get_object_member(report, "http");
    g_assert_cmpstr(json_object_get_string_member(http, "url"), ==,
                    "http://box.ts.net:8788/grok/gj-1");
    g_assert_cmpstr(json_object_get_string_member(
                        json_object_get_object_member(http, "headers"),
                        "Authorization"), ==, "Bearer tok");
    g_assert_false(json_object_has_member(report, "mcp"));

    with_mcp = clawt_grok_envelope(job, "x", NULL,
                                   "http://box.ts.net:8788/grok/gj-1", "tok",
                                   "http://box.ts.net:8788/mcp/grok");
    root = parse_object(parser2, with_mcp);
    report = json_object_get_object_member(
        json_object_get_object_member(root, "clawtilla"), "report");
    g_assert_cmpstr(json_object_get_string_member(
                        json_object_get_object_member(report, "mcp"),
                        "job_token"), ==, "tok");
}

static void
test_an_ipv6_host_is_bracketed(void)
{
    g_autofree gchar *url = clawt_grok_callback_url("fd7a:115c:a1e0::1",
                                                    8788, "gj-1");

    g_assert_cmpstr(url, ==, "http://[fd7a:115c:a1e0::1]:8788/grok/gj-1");
}

/*
 * What each of Grok's answers means.  The run id is read off the body
 * GitHub-style -- `runUuid` -- and a refusal says what to change.
 */
static void
test_groks_answers_are_read(void)
{
    g_autofree gchar *run = NULL;
    g_autofree gchar *why = NULL;

    g_assert_cmpint(clawt_grok_dispatch_reading(
                        200, "{\"success\":true,\"runUuid\":\"r-9\"}", -1,
                        &run, &why),
                    ==, CLAWT_GROK_JOB_ACCEPTED);
    g_assert_cmpstr(run, ==, "r-9");
    g_assert_null(why);

    g_free(g_steal_pointer(&run));
    g_assert_cmpint(clawt_grok_dispatch_reading(
                        400, "{\"success\":false,\"error\":\"Automation x is "
                             "disabled\"}", -1, &run, &why),
                    ==, CLAWT_GROK_JOB_REFUSED);
    g_assert_nonnull(strstr(why, "switched off"));

    g_free(g_steal_pointer(&why));
    g_assert_cmpint(clawt_grok_dispatch_reading(401, "{}", -1, NULL, &why),
                    ==, CLAWT_GROK_JOB_REFUSED);
    g_assert_nonnull(strstr(why, "key"));

    g_free(g_steal_pointer(&why));
    g_assert_cmpint(clawt_grok_dispatch_reading(429, NULL, 0, NULL, &why),
                    ==, CLAWT_GROK_JOB_REFUSED);
    g_assert_nonnull(strstr(why, "hourly"));
}

static void
test_a_report_is_read(void)
{
    ClawtGrokJobState state;
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_grok_report_parse(
        "{\"status\":\"done\",\"result\":\"pong\"}", -1, &state, &text,
        NULL));
    g_assert_cmpint(state, ==, CLAWT_GROK_JOB_DONE);
    g_assert_cmpstr(text, ==, "pong");

    g_free(g_steal_pointer(&text));
    g_assert_true(clawt_grok_report_parse(
        "{\"status\":\"progress\",\"note\":\"halfway\"}", -1, &state, &text,
        NULL));
    g_assert_cmpint(state, ==, CLAWT_GROK_JOB_WORKING);
    g_assert_cmpstr(text, ==, "halfway");

    /* Only the four words a bot may say: never one of clawtilla's. */
    g_free(g_steal_pointer(&text));
    g_assert_false(clawt_grok_report_parse(
        "{\"status\":\"expired\",\"result\":\"x\"}", -1, &state, &text,
        &error));
    g_assert_nonnull(error);
}

/* ── State, as every client draws it ─────────────────────────────── */

/*
 * Walked, not listed: every state has a tone the stylesheets paint, a
 * label, and a nick that round-trips -- and the open ones are exactly the
 * ones that do not need anybody.
 */
static void
test_every_state_is_classified(void)
{
    static const gchar *const tones[] = {
        "good", "bad", "warn", "info", "neutral", NULL
    };
    guint i;

    g_assert_cmpuint(clawt_grok_job_state_count(), ==, 8);

    for (i = 0; i < clawt_grok_job_state_count(); i++) {
        ClawtGrokJobState state = clawt_grok_job_state_nth(i);
        gint back = -1;

        g_assert_cmpint((gint)state, ==, (gint)i);
        g_assert_true(g_strv_contains(tones,
                                      clawt_grok_job_state_tone(state)));
        g_assert_cmpstr(clawt_grok_job_state_label(state), !=, "Unknown");
        g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_GROK_JOB_STATE,
                                           clawt_grok_job_state_nth_nick(i),
                                           &back));
        g_assert_cmpint(back, ==, (gint)state);

        if (clawt_grok_job_state_needs_attention(state))
            g_assert_true(clawt_grok_job_state_is_settled(state));
    }

    g_assert_false(clawt_grok_job_state_is_settled(CLAWT_GROK_JOB_WORKING));
    g_assert_false(clawt_grok_job_state_needs_attention(CLAWT_GROK_JOB_DONE));
    g_assert_true(clawt_grok_job_state_needs_attention(
        CLAWT_GROK_JOB_NEEDS_INPUT));
}

static void
test_the_routine_prompt_names_mcp_only_when_connected(void)
{
    g_autofree gchar *plain = clawt_grok_routine_prompt("chief",
                                                        "Chief of staff",
                                                        NULL);
    g_autofree gchar *mcp = clawt_grok_routine_prompt(
        "chief", NULL, "http://box.ts.net:8788/mcp/grok");

    g_assert_nonnull(strstr(plain, "\"chief\""));
    g_assert_nonnull(strstr(plain, "Chief of staff"));
    g_assert_null(strstr(plain, "clawtilla_grok_report"));
    g_assert_nonnull(strstr(mcp, "clawtilla_grok_report"));
    g_assert_nonnull(strstr(mcp, "http://box.ts.net:8788/mcp/grok"));
}

/* ── The store ───────────────────────────────────────────────────── */

static ClawtGrokJob *
make_job(const gchar *id, const gchar *bot, gint64 expires)
{
    ClawtGrokJob *job = clawt_grok_job_new(id, bot);
    gint64 now = g_get_real_time();

    clawt_grok_job_set_agent_id(job, "oryx");
    clawt_grok_job_set_instructions(job, "do the thing");
    clawt_grok_job_set_created_at(job, now);
    clawt_grok_job_set_updated_at(job, now);
    clawt_grok_job_set_expires_at(job, expires);

    return job;
}

/*
 * A job outlives the daemon that sent it -- a stored question with an
 * in-memory asker is the shape AGENTS.md warns about -- and settles once,
 * however many reports race for it.
 */
static void
test_the_store_survives_a_reopen_and_settles_once(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-grok-store-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "grok-jobs.db", NULL);
    g_autofree gchar *digest = clawt_grok_token_digest("tok");
    gint64 later = g_get_real_time() + (gint64)3600 * G_USEC_PER_SEC;
    g_autoptr(GError) error = NULL;

    {
        g_autoptr(ClawtGrokStore) store = clawt_grok_store_new(path, &error);
        g_autoptr(ClawtGrokJob) job = make_job("gj-a", "chief", later);

        g_assert_no_error(error);
        g_assert_true(clawt_grok_store_add(store, job, digest, &error));
        g_assert_no_error(error);
    }

    {
        g_autoptr(ClawtGrokStore) store = clawt_grok_store_new(path, &error);
        g_autoptr(ClawtGrokJob) back = clawt_grok_store_get(store, "gj-a");
        g_autoptr(ClawtGrokJob) settled = NULL;
        g_autoptr(ClawtGrokJob) again = NULL;
        g_autoptr(GPtrArray) open = NULL;

        g_assert_nonnull(back);
        g_assert_cmpstr(clawt_grok_job_get_bot(back), ==, "chief");
        g_assert_cmpint(clawt_grok_job_get_state(back), ==,
                        CLAWT_GROK_JOB_SENT);
        g_assert_true(clawt_grok_store_check_token(store, "gj-a", "tok"));
        g_assert_false(clawt_grok_store_check_token(store, "gj-a", "nope"));

        open = clawt_grok_store_list(store, NULL, NULL, TRUE, 10);
        g_assert_cmpuint(open->len, ==, 1);

        settled = clawt_grok_store_settle(store, "gj-a", CLAWT_GROK_JOB_DONE,
                                          "pong", g_get_real_time(), &error);
        g_assert_no_error(error);
        g_assert_nonnull(settled);
        g_assert_cmpstr(clawt_grok_job_get_result(settled), ==, "pong");

        again = clawt_grok_store_settle(store, "gj-a",
                                        CLAWT_GROK_JOB_EXPIRED, NULL,
                                        g_get_real_time(), &error);
        g_assert_null(again);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);

        g_clear_pointer(&open, g_ptr_array_unref);
        open = clawt_grok_store_list(store, NULL, NULL, TRUE, 10);
        g_assert_cmpuint(open->len, ==, 0);
    }

    clawt_test_remove_tree(dir);
}

static void
test_only_past_deadlines_are_due(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-grok-due-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "grok-jobs.db", NULL);
    g_autoptr(ClawtGrokStore) store = clawt_grok_store_new(path, NULL);
    gint64 now = g_get_real_time();
    g_autoptr(ClawtGrokJob) old = make_job("gj-old", "chief", now - 1);
    g_autoptr(ClawtGrokJob) fresh = make_job("gj-new", "chief",
                                             now + (gint64)3600 * G_USEC_PER_SEC);
    g_autoptr(GPtrArray) due = NULL;

    g_assert_true(clawt_grok_store_add(store, old, "d1", NULL));
    g_assert_true(clawt_grok_store_add(store, fresh, "d2", NULL));

    due = clawt_grok_store_due(store, now);
    g_assert_cmpuint(due->len, ==, 1);
    g_assert_cmpstr(clawt_grok_job_get_id(g_ptr_array_index(due, 0)), ==,
                    "gj-old");

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    GMainContext *context;
    ClawtDaemon  *daemon;
    GPtrArray    *messages;    /* "from -> subject", every `message` event */
    gchar        *refusal;
    gboolean      answered;
} Fixture;

static void
on_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    Fixture *fixture = user_data;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "message") == 0)
        g_ptr_array_add(fixture->messages,
                        g_strdup_printf("%s",
                                        clawt_event_get_detail(event, "from")
                                            != NULL
                                            ? clawt_event_get_detail(event,
                                                                     "from")
                                            : "?"));
}

/*
 * Three bots: chief and research shared with oryx, ops shared with ibex
 * alone -- two keys, because a scoping rule tested with one agent cannot
 * tell "shared with this agent" from "shared with everyone".
 */
static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *key = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-grok-d-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->messages = g_ptr_array_new_with_free_func(g_free);

    key = g_build_filename(fixture->dir, "grok.key", NULL);
    g_assert_true(g_file_set_contents(key, "crsr_test", -1, NULL));

    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "secrets:\n  dir: \"%s/secrets\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "integrations:\n"
        "  - name: chief\n"
        "    type: grokbot\n"
        "    url: \"https://grok.invalid/automations/webhook/1\"\n"
        "    token: \"file:%s\"\n"
        "    description: \"Chief of staff\"\n"
        "    scope: selected\n"
        "    agents: [oryx]\n"
        "  - name: research\n"
        "    type: grokbot\n"
        "    url: \"https://grok.invalid/automations/webhook/2\"\n"
        "    token: \"file:%s\"\n"
        "    scope: selected\n"
        "    agents: [oryx]\n"
        "  - name: ops\n"
        "    type: grokbot\n"
        "    url: \"https://grok.invalid/automations/webhook/3\"\n"
        "    token: \"file:%s\"\n"
        "    scope: selected\n"
        "    agents: [ibex]\n"
        "agents:\n  - id: oryx\n  - id: ibex\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->dir, key, key, key);

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1,
                                      &error));

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);
    g_assert_nonnull(clawt_daemon_grok_get_store(fixture->daemon));

    g_signal_connect(fixture->daemon->bus, "event", G_CALLBACK(on_event),
                     fixture);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        g_signal_handlers_disconnect_by_data(fixture->daemon->bus, fixture);
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);
    g_clear_pointer(&fixture->messages, g_ptr_array_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
    g_free(fixture->refusal);
}

static void
on_sent(ClawtGrokJob *job, const gchar *refusal, gpointer user_data)
{
    Fixture *fixture = user_data;

    (void)job;

    fixture->answered = TRUE;
    g_free(fixture->refusal);
    fixture->refusal = g_strdup(refusal);
}

static void
send(Fixture *fixture, const gchar *agent, const gchar *bot)
{
    fixture->answered = FALSE;
    g_clear_pointer(&fixture->refusal, g_free);

    clawt_daemon_grok_send(fixture->daemon, agent, NULL, NULL, 1, bot,
                           "do the thing", NULL, 0, on_sent, fixture);

    /* Every refusal below is answered before any network: synchronously. */
    g_assert_true(fixture->answered);
}

/*
 * Which bot comes first and where it reports second: so a bot outside an
 * agent's scope is refused naming the ones it may reach, and a bot it may
 * reach is refused only because nothing could hear its answer -- naming
 * the setting that would fix it.
 */
static void
test_dispatch_is_scoped_and_refuses_without_a_listener(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture);

    send(&fixture, "ibex", "chief");
    g_assert_nonnull(strstr(fixture.refusal, "no Grok bot called"));
    g_assert_nonnull(strstr(fixture.refusal, "ops"));
    g_assert_null(strstr(fixture.refusal, "research"));

    send(&fixture, "oryx", "ops");
    g_assert_nonnull(strstr(fixture.refusal, "chief"));
    g_assert_nonnull(strstr(fixture.refusal, "research"));

    /* Reachable, and refused for the listener -- before anything is sent. */
    send(&fixture, "oryx", "Chief");
    g_assert_nonnull(strstr(fixture.refusal, "daemon.webhook_enabled"));

    {
        g_autoptr(GPtrArray) jobs = clawt_grok_store_list(
            clawt_daemon_grok_get_store(fixture.daemon), NULL, NULL, FALSE,
            10);

        g_assert_cmpuint(jobs->len, ==, 0);
    }

    fixture_teardown(&fixture);
}

static void
add_job(Fixture *fixture, const gchar *id, const gchar *bot,
        const gchar *token, gint64 expires)
{
    g_autoptr(ClawtGrokJob) job = make_job(id, bot, expires);
    g_autofree gchar *digest = clawt_grok_token_digest(token);

    g_assert_true(clawt_grok_store_add(
        clawt_daemon_grok_get_store(fixture->daemon), job, digest, NULL));
}

static guint
report(Fixture *fixture, const gchar *job_id, const gchar *token,
       const gchar *body)
{
    g_autoptr(GHashTable) headers = clawt_trigger_headers_new();
    g_autofree gchar *bearer = NULL;
    g_autofree gchar *reply = NULL;
    const gchar *type = NULL;
    guint status = 0;

    if (token != NULL) {
        bearer = g_strconcat("Bearer ", token, NULL);
        clawt_trigger_headers_add(headers, "Authorization", bearer);
    }

    reply = clawt_daemon_grok_handle(fixture->daemon,
                                     CLAWT_WEBHOOK_ROUTE_GROK_REPORT, job_id,
                                     headers, (const guchar *)body,
                                     strlen(body), &status, &type);

    return status;
}

/*
 * The HTTP route: a wrong token learns nothing, progress repeats and
 * starts no turn, the report settles once and is delivered to the agent
 * as a message from the bot, and anything after it is refused.
 */
static void
test_a_report_settles_once_and_reaches_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtGrokJob) job = NULL;

    fixture_setup(&fixture);
    add_job(&fixture, "gj-r", "chief", "tok",
            g_get_real_time() + (gint64)3600 * G_USEC_PER_SEC);

    g_assert_cmpuint(report(&fixture, "gj-r", "wrong",
                            "{\"status\":\"done\",\"result\":\"x\"}"),
                     ==, 404);
    g_assert_cmpuint(report(&fixture, "gj-none", "tok",
                            "{\"status\":\"done\",\"result\":\"x\"}"),
                     ==, 404);

    g_assert_cmpuint(report(&fixture, "gj-r", "tok",
                            "{\"status\":\"progress\",\"note\":\"half\"}"),
                     ==, 200);
    g_assert_cmpuint(report(&fixture, "gj-r", "tok",
                            "{\"status\":\"progress\",\"note\":\"most\"}"),
                     ==, 200);

    job = clawt_grok_store_get(clawt_daemon_grok_get_store(fixture.daemon),
                               "gj-r");
    g_assert_cmpint(clawt_grok_job_get_state(job), ==,
                    CLAWT_GROK_JOB_WORKING);
    g_assert_cmpstr(clawt_grok_job_get_note(job), ==, "most");

    /* Progress is on the record and started no turn: nothing was sent. */
    g_assert_cmpuint(fixture.messages->len, ==, 2);

    /* The token in the body, for a bot that cannot set a header. */
    g_assert_cmpuint(report(&fixture, "gj-r", NULL,
                            "{\"status\":\"done\",\"result\":\"pong\","
                            "\"token\":\"tok\"}"),
                     ==, 200);

    g_clear_pointer(&job, clawt_grok_job_free);
    job = clawt_grok_store_get(clawt_daemon_grok_get_store(fixture.daemon),
                               "gj-r");
    g_assert_cmpint(clawt_grok_job_get_state(job), ==, CLAWT_GROK_JOB_DONE);
    g_assert_cmpstr(clawt_grok_job_get_result(job), ==, "pong");

    /* Delivered as a message from the bot. */
    g_assert_cmpuint(fixture.messages->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(fixture.messages, 2), ==,
                    "grok:chief");

    g_assert_cmpuint(report(&fixture, "gj-r", "tok",
                            "{\"status\":\"failed\",\"result\":\"late\"}"),
                     ==, 410);

    fixture_teardown(&fixture);
}

static gchar *
mcp(Fixture *fixture, const gchar *key, const gchar *body, guint *status)
{
    g_autoptr(GHashTable) headers = clawt_trigger_headers_new();
    g_autofree gchar *bearer = g_strconcat("Bearer ", key, NULL);
    const gchar *type = NULL;

    clawt_trigger_headers_add(headers, "Authorization", bearer);

    return clawt_daemon_grok_handle(fixture->daemon,
                                    CLAWT_WEBHOOK_ROUTE_GROK_MCP, NULL,
                                    headers, (const guchar *)body,
                                    strlen(body), status, &type);
}

static gchar *
mint(Fixture *fixture, const gchar *bot)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("grok.mcp_key", "v1");
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    JsonObject *payload;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "bot");
    json_builder_add_string_value(builder, bot);
    json_builder_end_object(builder);
    clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));

    reply = clawt_daemon_handle_request(fixture->daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    payload = clawt_ipc_frame_get_payload(reply);
    g_assert_true(json_object_get_boolean_member(payload,
                                                 "secret_shown_once"));

    return g_strdup(json_object_get_string_member(payload, "key"));
}

/*
 * The MCP route: a key names a bot, the bot is offered its three tools
 * and nothing an agent has, and it can report only its own jobs.
 */
static void
test_the_mcp_route_reports_only_the_bots_own_jobs(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *chief_key = NULL;
    g_autofree gchar *reply = NULL;
    guint status = 0;

    fixture_setup(&fixture);
    chief_key = mint(&fixture, "chief");

    add_job(&fixture, "gj-c", "chief", "tc",
            g_get_real_time() + (gint64)3600 * G_USEC_PER_SEC);
    add_job(&fixture, "gj-o", "ops", "to",
            g_get_real_time() + (gint64)3600 * G_USEC_PER_SEC);

    /* No key, no answer. */
    g_free(mcp(&fixture, "wrong", "{\"jsonrpc\":\"2.0\",\"id\":1,"
                                  "\"method\":\"tools/list\"}", &status));
    g_assert_cmpuint(status, ==, 401);

    reply = mcp(&fixture, chief_key,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2025-06-18\"}}", &status);
    g_assert_cmpuint(status, ==, 200);
    g_assert_nonnull(strstr(reply, "\"serverInfo\""));

    g_free(g_steal_pointer(&reply));
    reply = mcp(&fixture, chief_key,
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
                &status);
    g_assert_nonnull(strstr(reply, "clawtilla_grok_report"));
    g_assert_nonnull(strstr(reply, "clawtilla_grok_progress"));
    g_assert_nonnull(strstr(reply, "clawtilla_grok_assigned"));
    g_assert_null(strstr(reply, "clawtilla_grok_dispatch"));
    g_assert_null(strstr(reply, "clawtilla_list_agents"));

    /* A notification is acknowledged with nothing. */
    g_free(g_steal_pointer(&reply));
    reply = mcp(&fixture, chief_key,
                "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                &status);
    g_assert_cmpuint(status, ==, 202);

    /* ops's job, with ops's own token, refused for chief's key. */
    g_free(g_steal_pointer(&reply));
    reply = mcp(&fixture, chief_key,
                "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"clawtilla_grok_report\","
                "\"arguments\":{\"job_id\":\"gj-o\",\"job_token\":\"to\","
                "\"status\":\"done\",\"result\":\"x\"}}}", &status);
    g_assert_nonnull(strstr(reply, "another bot"));
    g_assert_nonnull(strstr(reply, "\"isError\":true"));

    g_free(g_steal_pointer(&reply));
    reply = mcp(&fixture, chief_key,
                "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"clawtilla_grok_report\","
                "\"arguments\":{\"job_id\":\"gj-c\",\"job_token\":\"tc\","
                "\"status\":\"needs_input\",\"result\":\"which day?\"}}}",
                &status);
    g_assert_nonnull(strstr(reply, "\"isError\":false"));

    {
        g_autoptr(ClawtGrokJob) job = clawt_grok_store_get(
            clawt_daemon_grok_get_store(fixture.daemon), "gj-c");

        g_assert_cmpint(clawt_grok_job_get_state(job), ==,
                        CLAWT_GROK_JOB_NEEDS_INPUT);
    }

    fixture_teardown(&fixture);
}

/*
 * A job nobody answers is given up once, told to its agent, counted on
 * the badge -- and a report arriving afterwards is refused.
 */
static void
test_an_unanswered_job_expires_once(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("grok.jobs", "v1");
    g_autoptr(JsonNode) reply = NULL;
    gint64 now = g_get_real_time();

    fixture_setup(&fixture);
    add_job(&fixture, "gj-x", "chief", "tok", now + G_USEC_PER_SEC);

    clawt_daemon_grok_sweep(fixture.daemon, now);
    g_assert_cmpuint(fixture.messages->len, ==, 0);

    clawt_daemon_grok_sweep(fixture.daemon, now + 2 * G_USEC_PER_SEC);
    g_assert_cmpuint(fixture.messages->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.messages, 0), ==,
                    "grok:chief");

    clawt_daemon_grok_sweep(fixture.daemon, now + 3 * G_USEC_PER_SEC);
    g_assert_cmpuint(fixture.messages->len, ==, 1);

    g_assert_cmpuint(report(&fixture, "gj-x", "tok",
                            "{\"status\":\"done\",\"result\":\"late\"}"),
                     ==, 410);

    reply = clawt_daemon_handle_request(fixture.daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_assert_cmpint(json_object_get_int_member(
                        clawt_ipc_frame_get_payload(reply), "attention"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/*
 * The listing every client draws from: each bot once, with the agents it
 * is shared with, and the reason no job could be sent right now.
 */
static void
test_the_bot_listing_says_who_and_whether(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("grok.bots", "v1");
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;
    JsonArray *bots;
    guint i;
    gboolean saw_ops = FALSE;

    fixture_setup(&fixture);

    reply = clawt_daemon_handle_request(fixture.daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));
    payload = clawt_ipc_frame_get_payload(reply);

    g_assert_false(json_object_get_boolean_member(payload, "reachable"));
    g_assert_nonnull(strstr(json_object_get_string_member(
                                payload, "unreachable_reason"),
                            "daemon.webhook_enabled"));

    bots = json_object_get_array_member(payload, "bots");
    g_assert_cmpuint(json_array_get_length(bots), ==, 3);

    for (i = 0; i < json_array_get_length(bots); i++) {
        JsonObject *bot = json_array_get_object_element(bots, i);
        JsonArray *agents = json_object_get_array_member(bot, "agents");

        g_assert_true(json_object_get_boolean_member(bot, "key_configured"));

        if (g_strcmp0(json_object_get_string_member(bot, "name"), "ops") ==
            0) {
            saw_ops = TRUE;
            g_assert_cmpuint(json_array_get_length(agents), ==, 1);
            g_assert_cmpstr(json_array_get_string_element(agents, 0), ==,
                            "ibex");
        }
    }

    g_assert_true(saw_ops);

    fixture_teardown(&fixture);
}

/* ── The listener's routes ───────────────────────────────────────── */

static void
test_the_listener_routes_grok(void)
{
    const gchar *id = NULL;

    g_assert_cmpint(clawt_webhook_route("POST", "/grok/gj-1", &id), ==,
                    CLAWT_WEBHOOK_ROUTE_GROK_REPORT);
    g_assert_cmpstr(id, ==, "gj-1");
    g_assert_cmpint(clawt_webhook_route("POST", "/mcp/grok", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_GROK_MCP);
    g_assert_cmpint(clawt_webhook_route("GET", "/grok/gj-1", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_WRONG_METHOD);
    g_assert_cmpint(clawt_webhook_route("POST", "/grok/gj-1/x", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_NONE);
    g_assert_cmpint(clawt_webhook_route("POST", "/grok/", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_NONE);
}

/* ── The agent's view ────────────────────────────────────────────── */

/*
 * The TOOLS.org region names each bot an agent may reach, with what it is
 * for -- and only those -- so "ask our chief Grok bot" can become a call.
 */
static void
test_tools_org_names_the_reachable_bots(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *oryx = NULL;
    g_autofree gchar *ibex = NULL;

    fixture_setup(&fixture);

    oryx = clawt_mcp_tools_describe_for_agent(fixture.daemon->mcp_tools,
                                              "oryx");
    ibex = clawt_mcp_tools_describe_for_agent(fixture.daemon->mcp_tools,
                                              "ibex");

    g_assert_nonnull(strstr(oryx, "| ~chief~ | Chief of staff |"));
    g_assert_nonnull(strstr(oryx, "~research~"));
    g_assert_null(strstr(oryx, "~ops~"));
    g_assert_nonnull(strstr(oryx, "clawtilla_grok_dispatch"));
    g_assert_null(strstr(oryx, "clawtilla_grok_report"));

    g_assert_nonnull(strstr(ibex, "~ops~"));
    g_assert_null(strstr(ibex, "~chief~"));

    fixture_teardown(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/grok/bot/name-case-prefix",
                    test_a_bot_is_found_by_name_case_and_prefix);
    g_test_add_func("/grok/bot/ambiguous-prefix",
                    test_an_ambiguous_prefix_is_refused_with_the_names);
    g_test_add_func("/grok/bot/none-shared",
                    test_no_bots_is_its_own_refusal);

    g_test_add_func("/grok/token/digest-only",
                    test_a_token_is_kept_only_as_a_digest);
    g_test_add_func("/grok/envelope/says-how-to-report",
                    test_the_envelope_says_how_to_report);
    g_test_add_func("/grok/envelope/ipv6-host",
                    test_an_ipv6_host_is_bracketed);
    g_test_add_func("/grok/dispatch/answers-read",
                    test_groks_answers_are_read);
    g_test_add_func("/grok/report/read", test_a_report_is_read);
    g_test_add_func("/grok/state/every-state-classified",
                    test_every_state_is_classified);
    g_test_add_func("/grok/prompt/mcp-only-when-connected",
                    test_the_routine_prompt_names_mcp_only_when_connected);

    g_test_add_func("/grok/store/reopen-and-settle-once",
                    test_the_store_survives_a_reopen_and_settles_once);
    g_test_add_func("/grok/store/due", test_only_past_deadlines_are_due);

    g_test_add_func("/grok/daemon/scoped-and-needs-listener",
                    test_dispatch_is_scoped_and_refuses_without_a_listener);
    g_test_add_func("/grok/daemon/report-settles-once",
                    test_a_report_settles_once_and_reaches_the_agent);
    g_test_add_func("/grok/daemon/mcp-own-jobs-only",
                    test_the_mcp_route_reports_only_the_bots_own_jobs);
    g_test_add_func("/grok/daemon/expires-once",
                    test_an_unanswered_job_expires_once);
    g_test_add_func("/grok/daemon/bot-listing",
                    test_the_bot_listing_says_who_and_whether);
    g_test_add_func("/grok/listener/routes", test_the_listener_routes_grok);
    g_test_add_func("/grok/tools/tools-org-names-bots",
                    test_tools_org_names_the_reachable_bots);

    return g_test_run();
}
