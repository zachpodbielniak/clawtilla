/*
 * test-venture.c - The VENTURE connector, and the confirmations bridge
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every one of these runs against a fixture.  `make test` reaches no
 * network and needs no VENTURE server, which is why the bridge takes
 * its transport as a function rather than dialling for itself: the
 * mapping, the deduplication and the retry are the whole of what can go
 * wrong here, and none of them needs a socket to be wrong in front of.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/*
 * The daemon half reaches core/clawt-daemon-private.h directly, because
 * that header *is* the interface of src/core/daemon-venture.c.  There is
 * no way in through the IPC surface: the bridge has no verbs of its own
 * -- answering a venture decision is `decision.answer`, which is the
 * point -- so the only thing an outside test could assert on is that a
 * request came back OK, which it would with the wire cut.
 */
#include "core/clawt-daemon-private.h"

/* ── The catalogue entry ─────────────────────────────────────────── */

static const ClawtConnectorInfo *
venture_entry(void)
{
    gsize n = 0;
    const ClawtConnectorInfo *builtin = clawt_connector_catalog_builtin(&n);
    gsize i;

    for (i = 0; i < n; i++) {
        if (g_strcmp0(builtin[i].id, "venture") == 0)
            return &builtin[i];
    }

    return NULL;
}

static void
test_venture_is_in_the_builtin_catalogue(void)
{
    const ClawtConnectorInfo *info = venture_entry();

    g_assert_nonnull(info);

    /*
     * default_instance is what makes every URL field relative, and it
     * is the whole of what says "this is a service you run yourself".
     * Without it the entry would be read as absolute URLs and pointing
     * an agent at a second server would silently reach the first.
     */
    g_assert_cmpstr(info->default_instance, ==, "http://localhost:8747");

    /* The credential goes in an environment variable, never a header
     * here: `venturectl mcp` is a stdio server and reads it. */
    g_assert_cmpint(info->placement, ==, CLAWT_CREDENTIAL_PLACEMENT_ENV);
    g_assert_cmpstr(info->credential_name, ==, "VENTURE_TOKEN");
    g_assert_cmpstr(info->instance_var, ==, "VENTURE_URL");
    g_assert_cmpstr(info->server_command, ==, "venturectl");
    g_assert_nonnull(info->server_args);
    g_assert_cmpstr(info->server_args[0], ==, "mcp");
}

/*
 * The nine tools venturectl mcp serves.
 *
 * Written down here as well as in the catalogue on purpose: this is the
 * one place the two projects agree in writing, and a tool renamed
 * upstream should fail here rather than as an agent quietly narrowed to
 * nothing by its own `tools:` list.
 */
static void
test_the_nine_tools_are_declared(void)
{
    static const gchar *const expected[] = {
        "venture_schema", "venture_list", "venture_get", "venture_create",
        "venture_update", "venture_delete", "venture_reports",
        "venture_report", "venture_confirmations", NULL
    };

    const ClawtConnectorInfo *info = venture_entry();
    gsize i;
    gsize count = 0;

    g_assert_nonnull(info->known_tools);

    for (i = 0; info->known_tools[i] != NULL; i++)
        count++;

    g_assert_cmpuint(count, ==, 9);

    for (i = 0; expected[i] != NULL; i++) {
        gsize j;
        gboolean found = FALSE;

        for (j = 0; info->known_tools[j] != NULL; j++) {
            if (g_strcmp0(expected[i], info->known_tools[j]) == 0) {
                found = TRUE;
                break;
            }
        }

        g_assert_true(found);
    }
}

/*
 * Every URL field joined onto the instance, and none left absolute.
 *
 * A mixture is how a self-hosted connector ends up quietly
 * authenticating against somebody else's server: one field that is
 * absolute goes on pointing at the flagship host however the instance
 * is configured.
 */
static void
test_every_url_field_joins_onto_the_instance(void)
{
    const ClawtConnectorInfo *info = venture_entry();
    const gchar *const fields[] = {
        info->auth_url, info->token_url, info->revoke_url, info->server_url
    };
    g_autofree gchar *base = NULL;
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(fields); i++) {
        g_autofree gchar *resolved = NULL;

        if (fields[i] == NULL)
            continue;

        g_assert_false(g_str_has_prefix(fields[i], "http://"));
        g_assert_false(g_str_has_prefix(fields[i], "https://"));

        resolved = clawt_connector_resolve_url(info, fields[i],
                                               "https://books.example");
        g_assert_true(g_str_has_prefix(resolved, "https://books.example"));
    }

    /* And the empty endpoint, which is how the bridge asks for the
     * instance itself. */
    base = clawt_connector_resolve_url(info, "", "https://books.example");
    g_assert_true(g_str_has_prefix(base, "https://books.example"));
}

/* ── What reaches the tool server ────────────────────────────────── */

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
    GPtrArray   *catalog;
} PlanFixture;

static ClawtIntegrationBinding *
build_venture_binding(PlanFixture *fixture, const gchar *body,
                      const ClawtConnectorInfo **out_info)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    ClawtIntegrationConfig *instance;

    fixture->dir = g_dir_make_tmp("clawt-venture-XXXXXX", NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "integrations:\n"
        "  - name: books\n"
        "    type: connector\n"
        "    provider: venture\n"
        "%s",
        fixture->dir, body);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->catalog = clawt_connector_catalog_load(NULL, NULL);

    instance = clawt_config_get_integration(fixture->config, "books");
    g_assert_nonnull(instance);

    *out_info = clawt_connector_catalog_find(fixture->catalog, "venture");
    g_assert_nonnull(*out_info);

    return clawt_integration_binding_for_instance(
        instance, clawt_integration_find("connector"), NULL);
}

static void
plan_fixture_teardown(PlanFixture *fixture)
{
    g_clear_pointer(&fixture->catalog, g_ptr_array_unref);
    g_clear_object(&fixture->config);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static gboolean
strv_contains_substring(GStrv strv, const gchar *needle)
{
    gsize i;

    if (strv == NULL)
        return FALSE;

    for (i = 0; strv[i] != NULL; i++) {
        if (strstr(strv[i], needle) != NULL)
            return TRUE;
    }

    return FALSE;
}

/*
 * The assertion this file shares with test-connector.c, and the reason
 * both exist: the token reaches the environment and *nothing else*.  An
 * argv is readable from the process table by everything on the machine.
 */
static void
test_the_token_is_in_the_environment_and_not_the_argv(void)
{
    PlanFixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;

    binding = build_venture_binding(&fixture, "", &info);
    plan = clawt_connector_plan_new(info, binding, "vt_liveT0ken", &error);

    g_assert_no_error(error);
    g_assert_nonnull(plan);

    g_assert_true(strv_contains_substring(plan->envp,
                                          "VENTURE_TOKEN=vt_liveT0ken"));
    g_assert_false(strv_contains_substring(plan->argv, "vt_liveT0ken"));
    g_assert_null(plan->header_value);

    /* The server, as the catalogue names it. */
    g_assert_nonnull(plan->argv);
    g_assert_cmpstr(plan->argv[1], ==, "mcp");

    plan_fixture_teardown(&fixture);
}

/*
 * And the instance, which a stdio server has no other way to learn.
 *
 * Without it `venturectl mcp` falls back to its own default of
 * localhost:8747 -- so a connector pointed at a second server would
 * start a tool server that quietly talks to the first, which is exactly
 * the failure `default_instance` exists to prevent.
 */
static void
test_the_instance_reaches_the_server_environment(void)
{
    PlanFixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;

    binding = build_venture_binding(&fixture,
                                    "    instance: https://books.example\n",
                                    &info);
    plan = clawt_connector_plan_new(info, binding, "vt_x", &error);

    g_assert_no_error(error);
    g_assert_true(strv_contains_substring(
        plan->envp, "VENTURE_URL=https://books.example"));

    /* No trailing slash: the servers want an origin, not a URL. */
    g_assert_false(strv_contains_substring(plan->envp,
                                           "VENTURE_URL=https://books.example/"));

    plan_fixture_teardown(&fixture);
}

/* ── Two agents, one token ───────────────────────────────────────── */

static gchar *
warning_mentioning(GPtrArray *warnings, const gchar *needle)
{
    guint i;

    for (i = 0; warnings != NULL && i < warnings->len; i++) {
        const gchar *text = g_ptr_array_index(warnings, i);

        if (strstr(text, needle) != NULL)
            return g_strdup(text);
    }

    return NULL;
}

/*
 * venture audits by actor.  Two agents holding one token are one actor
 * in that trail, so which of them filed a change stops being
 * answerable -- and so does whether a person, a rule or the AI did.
 *
 * The `connector` integration type declares no identity keys, because
 * a fleet-wide GitHub account is what "give all of them GitHub" means.
 * This is the entry saying otherwise for itself, and the message has to
 * say *why*, or it reads as pedantry.
 */
static void
test_two_agents_on_one_venture_token_are_warned_about(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-venture-id-XXXXXX", NULL);
    g_autofree gchar *yaml = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *found = NULL;

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "connectors:\n"
        "  dir: \"%s/connectors.d\"\n"
        "integrations:\n"
        "  - name: books\n"
        "    type: connector\n"
        "    provider: venture\n"
        "    scope: all\n"
        "    token_file: \"%s/secrets/connector-books.json\"\n"
        "agents:\n"
        "  - id: bookkeeper\n"
        "  - id: analyst\n",
        dir, dir, dir, dir, dir, dir);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    g_assert_false(clawt_integration_validate_fleet(config, &warnings));

    found = warning_mentioning(warnings, "token_file");
    g_assert_nonnull(found);

    /* Both agents named, so the file can be fixed without guessing. */
    g_assert_nonnull(strstr(found, "bookkeeper"));
    g_assert_nonnull(strstr(found, "analyst"));

    /* And what it costs, which is the half a bare instruction lacks. */
    g_assert_nonnull(strstr(found, "audits by actor"));

    clawt_test_remove_tree(dir);
}

/*
 * The control: a connector that does *not* declare identity keys must
 * go on being shareable, because sharing one is what a fleet-wide
 * account is for.  Without this the test above would pass against a
 * change that warned about every connector.
 */
static void
test_a_shared_github_connector_is_not_warned_about(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-venture-gh-XXXXXX", NULL);
    g_autofree gchar *yaml = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *found = NULL;

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "connectors:\n"
        "  dir: \"%s/connectors.d\"\n"
        "integrations:\n"
        "  - name: code\n"
        "    type: connector\n"
        "    provider: github\n"
        "    scope: all\n"
        "    token_file: \"%s/secrets/connector-code.json\"\n"
        "agents:\n"
        "  - id: one\n"
        "  - id: two\n",
        dir, dir, dir, dir, dir, dir);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    clawt_integration_validate_fleet(config, &warnings);

    found = warning_mentioning(warnings, "token_file");
    g_assert_null(found);

    clawt_test_remove_tree(dir);
}

/* ── Reading the queue ───────────────────────────────────────────── */

static gchar *
fixture_body(const gchar *name)
{
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, name, NULL);
    gchar *body = NULL;

    g_assert_true(g_file_get_contents(path, &body, NULL, NULL));

    return body;
}

static void
test_a_confirmation_parses_into_a_card(void)
{
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autoptr(GPtrArray) cards = NULL;
    g_autoptr(GError) error = NULL;
    ClawtVentureConfirmation *first;
    ClawtVentureConfirmation *second;

    cards = clawt_venture_confirmations_parse(body, -1, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(cards->len, ==, 2);

    first = g_ptr_array_index(cards, 0);
    g_assert_cmpstr(first->id, ==, "a3f9c118");
    g_assert_cmpstr(first->summary, ==, "Create expense \"Cover art\"");
    g_assert_cmpstr(first->record_type, ==, "expense");
    g_assert_cmpstr(first->origin_name, ==, "books-agent");

    /*
     * A creation carries no record id, and venture omits the member
     * rather than sending 0 -- a client that read 0 as an id would go
     * looking for a row that does not exist.
     */
    g_assert_cmpint(first->record_id, ==, 0);

    /* The diff is rendered so somebody can answer from the card alone. */
    g_assert_nonnull(first->diff);
    g_assert_nonnull(strstr(first->diff, "Cover art"));
    g_assert_nonnull(strstr(first->diff, "25000"));

    second = g_ptr_array_index(cards, 1);
    g_assert_cmpint(second->record_id, ==, 42);
}

/*
 * A body that is not a list is not an empty queue.
 *
 * A proxy's error page, a login redirect and the wrong port all parse
 * as *something*, and reading any of them as "nothing is waiting" would
 * report a healthy inbox about a server nobody reached.
 */
static void
test_a_body_that_is_not_a_list_is_an_error(void)
{
    g_autoptr(GPtrArray) cards = NULL;
    g_autoptr(GError) error = NULL;

    cards = clawt_venture_confirmations_parse("{\"error\":\"unauthorized\"}",
                                              -1, &error);

    g_assert_null(cards);
    g_assert_nonnull(error);
}

/* ── The bridge ──────────────────────────────────────────────────── */

typedef struct {
    gchar              *dir;
    ClawtDecisionStore *decisions;
    ClawtVentureBridge *bridge;
    GPtrArray          *sent;      /* gchar*, "METHOD url" */
    gboolean            fail;      /* answer every request with an error */
} BridgeFixture;

/*
 * The transport, as a test provides it.
 *
 * It records what it was asked for and answers however the fixture
 * says, which is what lets "an answer given while the server is
 * unreachable" be a case rather than an outage somebody has to arrange.
 */
static void
record_request(ClawtVentureBridge *bridge, const gchar *method,
               const gchar *url, const gchar *token, gpointer user_data)
{
    BridgeFixture *fixture = user_data;

    g_ptr_array_add(fixture->sent, g_strdup_printf("%s %s", method, url));

    /* The token must be handed over, or nothing would authenticate. */
    g_assert_nonnull(token);

    if (fixture->fail) {
        g_autoptr(GError) error = NULL;

        g_set_error_literal(&error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "Connection refused");
        clawt_venture_bridge_complete(bridge, url, NULL, 0, error);
        return;
    }

    /*
     * An empty queue, spelled as venture spells it.  Answering a GET
     * with an empty *string* would be a parse failure, which is a
     * different case and has its own test.
     */
    if (g_strcmp0(method, "GET") == 0) {
        clawt_venture_bridge_complete(bridge, url, "[]", -1, NULL);
        return;
    }

    clawt_venture_bridge_complete(bridge, url, "", 0, NULL);
}

static void
bridge_fixture_setup(BridgeFixture *fixture)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-venture-br-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "decisions.db", NULL);

    fixture->decisions = clawt_decision_store_new(path, &error);
    g_assert_no_error(error);

    fixture->sent = g_ptr_array_new_with_free_func(g_free);
    fixture->bridge = clawt_venture_bridge_new(fixture->decisions, NULL);

    clawt_venture_bridge_set_request_func(fixture->bridge, record_request,
                                          fixture, NULL);
    clawt_venture_bridge_set_source(fixture->bridge, "books",
                                    "https://books.example", "vt_secret",
                                    "bookkeeper");
}

static void
bridge_fixture_teardown(BridgeFixture *fixture)
{
    g_clear_object(&fixture->bridge);
    g_clear_object(&fixture->decisions);
    g_clear_pointer(&fixture->sent, g_ptr_array_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static void
test_a_confirmation_becomes_a_decision(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autoptr(GPtrArray) open = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDecision) raised = NULL;
    g_autofree gchar *id = NULL;

    bridge_fixture_setup(&fixture);

    g_assert_cmpuint(clawt_venture_bridge_ingest(fixture.bridge, "books",
                                                 body, -1, &error), ==, 2);
    g_assert_no_error(error);

    open = clawt_decision_store_list(fixture.decisions, TRUE);
    g_assert_cmpuint(open->len, ==, 2);

    id = clawt_venture_decision_id("books", "a3f9c118");
    raised = clawt_decision_store_get(fixture.decisions, id);
    g_assert_nonnull(raised);

    /* Filed against the agent the connector is bound to. */
    g_assert_cmpstr(clawt_decision_get_agent(raised), ==, "bookkeeper");

    /*
     * The default is reject, and honestly so: nothing is written until
     * somebody approves, and venture drops a card nobody answers.
     */
    g_assert_cmpstr(clawt_decision_get_default(raised), ==, "reject");

    /*
     * And reversible_until stays unset.  venture's own soft delete is
     * the undo; a deadline invented here would be a promise clawtilla
     * cannot keep, and clawt_decision_is_urgent() reads it as pressure.
     */
    g_assert_cmpint(clawt_decision_get_reversible_until(raised), ==, 0);

    /* The question carries what the change is, so it can be answered
     * without opening venture's own web interface. */
    g_assert_nonnull(strstr(clawt_decision_get_question(raised),
                            "Cover art"));

    /* And never the credential. */
    g_assert_null(strstr(clawt_decision_get_question(raised), "vt_secret"));

    bridge_fixture_teardown(&fixture);
}

/*
 * The same card, polled again.
 *
 * venture keeps a pending confirmation in its queue until somebody
 * answers it or its TTL runs out, so a one-minute poll sees the same
 * card sixty times an hour.  Raising it each time would fill the inbox
 * with one copy per interval of every change nobody has got to yet.
 */
static void
test_polling_twice_raises_one_decision(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autoptr(GPtrArray) open = NULL;

    bridge_fixture_setup(&fixture);

    g_assert_cmpuint(clawt_venture_bridge_ingest(fixture.bridge, "books",
                                                 body, -1, NULL), ==, 2);
    g_assert_cmpuint(clawt_venture_bridge_ingest(fixture.bridge, "books",
                                                 body, -1, NULL), ==, 0);

    open = clawt_decision_store_list(fixture.decisions, TRUE);
    g_assert_cmpuint(open->len, ==, 2);

    bridge_fixture_teardown(&fixture);
}

/*
 * One unreadable card must not hide every other change waiting on
 * somebody.  The next poll would fail identically, so a hard error here
 * would mean an inbox that quietly stopped updating.
 */
static void
test_a_malformed_card_is_skipped_and_the_rest_kept(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body =
        fixture_body("venture-confirmations-malformed.json");
    g_autoptr(GError) error = NULL;
    guint raised;

    bridge_fixture_setup(&fixture);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*could not be read and was skipped*");

    raised = clawt_venture_bridge_ingest(fixture.bridge, "books", body, -1,
                                         &error);

    g_test_assert_expected_messages();

    g_assert_no_error(error);
    g_assert_cmpuint(raised, ==, 1);

    bridge_fixture_teardown(&fixture);
}

/* ── Answering ───────────────────────────────────────────────────── */

static void
test_approve_and_reject_post_the_right_endpoint(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    bridge_fixture_setup(&fixture);
    clawt_venture_bridge_ingest(fixture.bridge, "books", body, -1, NULL);

    first = clawt_venture_decision_id("books", "a3f9c118");
    second = clawt_venture_decision_id("books", "77bd0e42");

    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, first,
                                              "approve"));
    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, second,
                                              "reject"));

    g_assert_cmpuint(fixture.sent->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 0), ==,
                    "POST https://books.example"
                    "/api/v1/confirmations/a3f9c118/approve");
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 1), ==,
                    "POST https://books.example"
                    "/api/v1/confirmations/77bd0e42/reject");

    /* Delivered, so nothing is still owed. */
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);

    bridge_fixture_teardown(&fixture);
}

/*
 * A decision nobody here raised is not ours to answer.
 *
 * `decision.answer` offers every answer to the bridge, so this is the
 * gate that keeps an agent's own `clawtilla_ask` from being posted at
 * somebody's books.
 */
static void
test_a_decision_from_elsewhere_is_not_ours(void)
{
    BridgeFixture fixture = { 0 };

    bridge_fixture_setup(&fixture);

    g_assert_false(clawt_venture_bridge_answer(fixture.bridge,
                                               "dec-1234", "approve"));
    g_assert_cmpuint(fixture.sent->len, ==, 0);

    bridge_fixture_teardown(&fixture);
}

/*
 * An answer given while venture is unreachable is owed, not lost.
 *
 * The operator answered once and has no reason to look again; a POST
 * that vanished would leave the change waiting in venture until its TTL
 * dropped it, and nobody would ever know the answer had not landed.
 */
static void
test_an_answer_that_cannot_be_delivered_is_retried(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autofree gchar *id = NULL;

    bridge_fixture_setup(&fixture);
    clawt_venture_bridge_ingest(fixture.bridge, "books", body, -1, NULL);

    id = clawt_venture_decision_id("books", "a3f9c118");
    fixture.fail = TRUE;

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*it stays queued*");
    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, id, "approve"));
    g_test_assert_expected_messages();

    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 1);
    g_assert_cmpstr(clawt_venture_bridge_pending_answer_url(fixture.bridge, 0),
                    ==,
                    "https://books.example"
                    "/api/v1/confirmations/a3f9c118/approve");

    /* And the next poll sends it again, this time to a server that
     * answers. */
    fixture.fail = FALSE;
    g_ptr_array_set_size(fixture.sent, 0);

    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 0), ==,
                    "POST https://books.example"
                    "/api/v1/confirmations/a3f9c118/approve");

    bridge_fixture_teardown(&fixture);
}

/*
 * And it survives the daemon going away.
 *
 * The retry queue is in memory, so a restart loses it -- but venture is
 * still holding the card, and a card still pending against a decision
 * that was answered is proof the answer never landed.  Re-queueing from
 * that is what makes the durable half durable; an in-memory list alone
 * would have quietly dropped the answer.
 */
static void
test_an_answer_lost_to_a_restart_is_re_queued_from_the_store(void)
{
    BridgeFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtDecision) settled = NULL;
    g_autoptr(GError) error = NULL;

    bridge_fixture_setup(&fixture);
    clawt_venture_bridge_ingest(fixture.bridge, "books", body, -1, NULL);

    id = clawt_venture_decision_id("books", "a3f9c118");

    /* Answered in the store, and nothing queued -- which is the state a
     * daemon comes back up in. */
    settled = clawt_decision_store_answer(fixture.decisions, id, "approve",
                                          &error);
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);

    /* venture still lists it, so the answer never arrived. */
    clawt_venture_bridge_ingest(fixture.bridge, "books", body, -1, NULL);

    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);
    g_assert_cmpuint(fixture.sent->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 0), ==,
                    "POST https://books.example"
                    "/api/v1/confirmations/a3f9c118/approve");

    bridge_fixture_teardown(&fixture);
}

/*
 * A free-text answer that is not recognisably an approval is a
 * rejection.  That is the safe direction: a misread "yes" writes to
 * somebody's books and a misread "no" leaves a card to answer again.
 */
static void
test_an_unrecognised_answer_rejects(void)
{
    g_assert_true(clawt_venture_answer_is_approval("approve"));
    g_assert_true(clawt_venture_answer_is_approval("Approved"));
    g_assert_true(clawt_venture_answer_is_approval("yes"));

    g_assert_false(clawt_venture_answer_is_approval("reject"));
    g_assert_false(clawt_venture_answer_is_approval("no"));
    g_assert_false(clawt_venture_answer_is_approval(NULL));

    /*
     * And the one a substring match gets wrong.  "do not approve" holds
     * the word and means the opposite of it.
     */
    g_assert_false(clawt_venture_answer_is_approval("do not approve this"));
}

/*
 * A confirmation id arrives from another program and lands in a path
 * segment.  One carrying a slash would reach a different route
 * entirely, and the one thing a POST here must not do is arrive
 * somewhere nobody named.
 */
static void
test_a_confirmation_id_cannot_escape_its_path_segment(void)
{
    g_autofree gchar *url =
        clawt_venture_answer_url("https://books.example",
                                 "../../../admin", TRUE);

    g_assert_nonnull(url);
    g_assert_null(strstr(url, "/admin"));
    g_assert_true(g_str_has_prefix(url,
                                   "https://books.example"
                                   "/api/v1/confirmations/"));
}

/* ── The capability URL ──────────────────────────────────────────── */

/*
 * podomation's webhook module takes a URL and nothing else -- no
 * Authorization, no signature header -- so the venture recipe cannot
 * authenticate any other way.  The generic provider exists for callers
 * that are not a forge, and this is the shape of caller it was written
 * for.
 */
static void
test_a_url_secret_opens_a_generic_trigger(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_trigger_provider_accepts_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC));

    g_assert_true(clawt_trigger_verify_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC, "s3cret", "s3cret", &error));
    g_assert_no_error(error);

    /* Wrong by one byte. */
    g_assert_false(clawt_trigger_verify_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC, "s3cret", "s3crev", NULL));

    /* Absent entirely. */
    g_assert_false(clawt_trigger_verify_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC, "s3cret", NULL, NULL));
    g_assert_false(clawt_trigger_verify_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC, "s3cret", "", NULL));

    /* And a trigger with no secret is never opened by one. */
    g_assert_false(clawt_trigger_verify_url_secret(
        CLAWT_TRIGGER_PROVIDER_GENERIC, NULL, "s3cret", NULL));
}

/*
 * And it can never open a forge.
 *
 * Every forge can sign, and letting one be opened by a string in a
 * query would mean the weakest scheme any caller could reach for was
 * the scheme every trigger accepted -- which is what "sniffing may
 * never widen a configured trigger" already forbids one layer up.
 */
static void
test_a_url_secret_never_opens_a_forge(void)
{
    static const ClawtTriggerProvider forges[] = {
        CLAWT_TRIGGER_PROVIDER_FORGEJO,
        CLAWT_TRIGGER_PROVIDER_GITEA,
        CLAWT_TRIGGER_PROVIDER_GITHUB,
        CLAWT_TRIGGER_PROVIDER_GITLAB
    };

    gsize i;

    for (i = 0; i < G_N_ELEMENTS(forges); i++) {
        g_assert_false(clawt_trigger_provider_accepts_url_secret(forges[i]));

        g_assert_false(clawt_trigger_verify_url_secret(forges[i], "s3cret",
                                                       "s3cret", NULL));
    }
}

/* ── The wire ────────────────────────────────────────────────────── */

/*
 * Everything above is the bridge in isolation, and this codebase's most
 * expensive recurring bug is a mechanism that works in isolation and
 * reaches nobody.  So: does a venture connector in the config actually
 * become a source, and does an operator's answer actually reach it?
 */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
    GPtrArray    *sent;
} DaemonFixture;

static void
daemon_record_request(ClawtVentureBridge *bridge, const gchar *method,
                      const gchar *url, const gchar *token,
                      gpointer user_data)
{
    DaemonFixture *fixture = user_data;

    (void)token;

    g_ptr_array_add(fixture->sent, g_strdup_printf("%s %s", method, url));

    clawt_venture_bridge_complete(bridge, url, "[]", -1, NULL);
}

static void
daemon_fixture_setup(DaemonFixture *fixture)
{
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *secrets = NULL;
    g_autofree gchar *token_path = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-venture-d-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->sent = g_ptr_array_new_with_free_func(g_free);

    /* The credential lives in a 0600 file, never in the config. */
    secrets = g_build_filename(fixture->dir, "secrets", NULL);
    g_assert_cmpint(g_mkdir_with_parents(secrets, 0700), ==, 0);

    token_path = g_build_filename(secrets, "connector-books.json", NULL);
    g_assert_true(g_file_set_contents(
        token_path, "{\"access_token\": \"vt_live\"}", -1, &error));
    g_assert_no_error(error);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "secrets:\n  dir: \"%s\"\n"
        "connectors:\n  dir: \"%s/connectors.d\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "integrations:\n"
        "  - name: books\n"
        "    type: connector\n"
        "    provider: venture\n"
        "    scope: all\n"
        "    instance: \"https://books.example\"\n"
        "    token_file: \"%s\"\n"
        "agents:\n  - id: bookkeeper\n",
        fixture->dir, fixture->dir, fixture->dir, secrets, fixture->dir,
        fixture->dir, token_path);

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1,
                                      &error));
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);

    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);

    /*
     * The daemon's own transport is replaced before anything can use it.
     * Its first poll is a whole interval away, so nothing has left the
     * machine -- and after this nothing can.
     */
    g_assert_nonnull(fixture->daemon->venture);
    clawt_venture_bridge_set_request_func(fixture->daemon->venture,
                                          daemon_record_request, fixture,
                                          NULL);
}

static void
daemon_fixture_teardown(DaemonFixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);
    g_clear_pointer(&fixture->sent, g_ptr_array_unref);

    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
}

/*
 * A configured connector becomes a source, with the instance it names.
 *
 * The failure this catches is the whole-feature one: a bridge built,
 * a timer armed and nothing bound, which from outside is exactly a
 * VENTURE server whose queue is always empty.
 */
static void
test_a_configured_connector_becomes_a_source(void)
{
    DaemonFixture fixture = { 0 };

    daemon_fixture_setup(&fixture);

    g_assert_cmpuint(
        clawt_venture_bridge_source_count(fixture.daemon->venture), ==, 1);
    g_assert_true(clawt_venture_bridge_has_source(fixture.daemon->venture,
                                                  "books"));

    /* And it polls the instance the config named, not the default. */
    clawt_venture_bridge_poll(fixture.daemon->venture);

    g_assert_cmpuint(fixture.sent->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 0), ==,
                    "GET https://books.example/api/v1/confirmations");

    daemon_fixture_teardown(&fixture);
}

/*
 * And `decision.answer` reaches it.
 *
 * Answering a staged change is not a verb of its own -- it is the same
 * inbox every other decision is answered from, which is the entire
 * point of the bridge. So the wire runs through daemon-misc.c, and a
 * missing call there would leave an operator's approval delivered to
 * the agent and never to VENTURE: the change would sit in its queue
 * until the TTL dropped it, and nobody would look again.
 */
static void
test_answering_a_decision_reaches_venture(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *body = fixture_body("venture-confirmations.json");
    g_autofree gchar *id = NULL;
    g_autoptr(JsonNode) frame = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) payload = NULL;
    g_autoptr(JsonBuilder) builder = NULL;

    daemon_fixture_setup(&fixture);

    g_assert_cmpuint(clawt_venture_bridge_ingest(fixture.daemon->venture,
                                                 "books", body, -1, NULL),
                     ==, 2);

    id = clawt_venture_decision_id("books", "a3f9c118");

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "decision");
    json_builder_add_string_value(builder, id);
    json_builder_set_member_name(builder, "answer");
    json_builder_add_string_value(builder, "approve");
    json_builder_end_object(builder);
    payload = json_builder_get_root(builder);

    frame = clawt_ipc_request_new("decision.answer", "v1");
    clawt_ipc_frame_set_payload(frame, json_node_ref(payload));

    reply = clawt_daemon_handle_request(fixture.daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    g_assert_cmpuint(fixture.sent->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.sent, 0), ==,
                    "POST https://books.example"
                    "/api/v1/confirmations/a3f9c118/approve");

    daemon_fixture_teardown(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/venture/in-the-catalogue",
                    test_venture_is_in_the_builtin_catalogue);
    g_test_add_func("/venture/nine-tools-declared",
                    test_the_nine_tools_are_declared);
    g_test_add_func("/venture/url-fields-join-onto-the-instance",
                    test_every_url_field_joins_onto_the_instance);

    g_test_add_func("/venture/token-not-in-argv",
                    test_the_token_is_in_the_environment_and_not_the_argv);
    g_test_add_func("/venture/instance-reaches-the-server",
                    test_the_instance_reaches_the_server_environment);

    g_test_add_func("/venture/two-agents-one-token-warned",
                    test_two_agents_on_one_venture_token_are_warned_about);
    g_test_add_func("/venture/shared-github-not-warned",
                    test_a_shared_github_connector_is_not_warned_about);

    g_test_add_func("/venture/confirmation-parses",
                    test_a_confirmation_parses_into_a_card);
    g_test_add_func("/venture/not-a-list-is-an-error",
                    test_a_body_that_is_not_a_list_is_an_error);

    g_test_add_func("/venture/confirmation-becomes-a-decision",
                    test_a_confirmation_becomes_a_decision);
    g_test_add_func("/venture/polling-twice-raises-one",
                    test_polling_twice_raises_one_decision);
    g_test_add_func("/venture/malformed-card-skipped",
                    test_a_malformed_card_is_skipped_and_the_rest_kept);

    g_test_add_func("/venture/approve-and-reject-endpoints",
                    test_approve_and_reject_post_the_right_endpoint);
    g_test_add_func("/venture/foreign-decision-not-ours",
                    test_a_decision_from_elsewhere_is_not_ours);
    g_test_add_func("/venture/undeliverable-answer-retried",
                    test_an_answer_that_cannot_be_delivered_is_retried);
    g_test_add_func("/venture/answer-re-queued-from-the-store",
                    test_an_answer_lost_to_a_restart_is_re_queued_from_the_store);
    g_test_add_func("/venture/unrecognised-answer-rejects",
                    test_an_unrecognised_answer_rejects);
    g_test_add_func("/venture/confirmation-id-cannot-escape",
                    test_a_confirmation_id_cannot_escape_its_path_segment);

    g_test_add_func("/venture/url-secret-opens-generic",
                    test_a_url_secret_opens_a_generic_trigger);
    g_test_add_func("/venture/url-secret-never-opens-a-forge",
                    test_a_url_secret_never_opens_a_forge);

    g_test_add_func("/venture/configured-connector-becomes-a-source",
                    test_a_configured_connector_becomes_a_source);
    g_test_add_func("/venture/answering-reaches-venture",
                    test_answering_a_decision_reaches_venture);

    return g_test_run();
}
