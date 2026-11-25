/*
 * test-venture-routing.c - Many connectors on one VENTURE server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The shape the fleet actually runs: one VENTURE server, one connector
 * per agent, each with its own token.  VENTURE's queue is install-wide,
 * so every connector sees every card -- and before this, every response
 * was matched to its source by URL, which on a shared server is the
 * first source in hash order.  One agent's decisions were filed under
 * another, and answered with the other's token.
 *
 * Everything here runs against a fake transport that can hold its
 * answers back and deliver them in any order, because "the responses
 * came back in a different order from the requests" is the whole of the
 * failure and a transport that always answers at once cannot show it.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

#define SERVER     "https://venture.example"
#define ALPHA      "alpha-venture"
#define BETA       "beta-venture"
#define ALPHA_KEY  "vt_alpha"
#define BETA_KEY   "vt_beta"

/* One request the bridge handed the transport. */
typedef struct {
    guint     id;
    gchar    *method;
    gchar    *url;
    gchar    *token;
    gboolean  done;
} Sent;

static void
sent_free(Sent *self)
{
    if (self == NULL)
        return;

    g_free(self->method);
    g_free(self->url);
    g_free(self->token);
    g_free(self);
}

typedef struct {
    gchar              *dir;
    ClawtDecisionStore *decisions;
    ClawtVentureBridge *bridge;
    GPtrArray          *sent;     /* Sent*, in the order they were asked */
    GPtrArray          *raised;   /* "decision agent", from the signal */
    gboolean            defer;    /* hold every answer until delivered */
    const gchar        *queue;    /* what the queue holds, or NULL */
    const gchar        *broken;   /* a token whose whoami fails, or NULL */
    gboolean            omit_token_id; /* whoami answered before token_id */
    GPtrArray          *said;     /* every g_message the bridge logged */
} RoutingFixture;

/*
 * What VENTURE would say to one request.
 *
 * whoami is answered from the token the request *carried*, so a bridge
 * that credited one source's answer to another would learn the wrong
 * actor -- and route the wrong cards -- where a test can see it.
 */
static const gchar *
reply_for(RoutingFixture *fixture, Sent *sent)
{
    if (g_strcmp0(sent->method, "POST") == 0)
        return "";

    if (g_str_has_suffix(sent->url, "/api/v1/health"))
        return "{\"staged_writes\": true}";

    if (g_str_has_suffix(sent->url, "/api/v1/whoami")) {
        /*
         * whoami as an older server sent it: no token_id.  A known
         * source is not asked again, so this is how a daemon that
         * stayed up across the upgrade still looks.
         */
        if (fixture->omit_token_id) {
            if (g_strcmp0(sent->token, ALPHA_KEY) == 0)
                return "{\"actor\": \"API token #1\", "
                       "\"role\": \"editor\", "
                       "\"user\": {\"id\": 1, \"username\": \"alpha\"}}";

            if (g_strcmp0(sent->token, BETA_KEY) == 0)
                return "{\"actor\": \"API token #2\", "
                       "\"role\": \"editor\", "
                       "\"user\": {\"id\": 2, \"username\": \"beta\"}}";

            return "{\"actor\": \"API token #3\", "
                   "\"role\": \"viewer\", "
                   "\"user\": {\"id\": 3, \"username\": \"gamma\"}}";
        }

        if (g_strcmp0(sent->token, ALPHA_KEY) == 0)
            return "{\"actor\": \"API token #1\", \"token_id\": 10, "
                   "\"role\": \"editor\", "
                   "\"user\": {\"id\": 1, \"username\": \"alpha\"}}";

        if (g_strcmp0(sent->token, BETA_KEY) == 0)
            return "{\"actor\": \"API token #2\", \"token_id\": 20, "
                   "\"role\": \"editor\", "
                   "\"user\": {\"id\": 2, \"username\": \"beta\"}}";

        return "{\"actor\": \"API token #3\", \"token_id\": 30, "
               "\"role\": \"viewer\", "
               "\"user\": {\"id\": 3, \"username\": \"gamma\"}}";

    }

    return fixture->queue != NULL ? fixture->queue : "[]";
}

static void
deliver(RoutingFixture *fixture, Sent *sent, gboolean fail)
{
    g_autoptr(GError) error = NULL;

    g_assert_false(sent->done);
    sent->done = TRUE;

    if (fail) {
        g_set_error_literal(&error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "Connection refused");
        clawt_venture_bridge_complete(fixture->bridge, sent->id, NULL, 0,
                                      error);
        return;
    }

    clawt_venture_bridge_complete(fixture->bridge, sent->id,
                                  reply_for(fixture, sent), -1, NULL);
}

/* Whether this is the whoami of the token the fixture says is revoked. */
static gboolean
is_broken(RoutingFixture *fixture, Sent *sent)
{
    return fixture->broken != NULL &&
           g_str_has_suffix(sent->url, "/api/v1/whoami") &&
           g_strcmp0(sent->token, fixture->broken) == 0;
}

static void
fake_request(ClawtVentureBridge *bridge, guint request_id,
             const gchar *method, const gchar *url, const gchar *token,
             gpointer user_data)
{
    RoutingFixture *fixture = user_data;
    Sent *sent = g_new0(Sent, 1);

    (void)bridge;

    g_assert_cmpuint(request_id, !=, 0);

    sent->id = request_id;
    sent->method = g_strdup(method);
    sent->url = g_strdup(url);
    sent->token = g_strdup(token);

    g_ptr_array_add(fixture->sent, sent);

    if (!fixture->defer)
        deliver(fixture, sent, is_broken(fixture, sent));
}

/*
 * The one request not yet answered that was sent to @suffix with
 * @token (or with any token, for %NULL).
 */
static Sent *
waiting(RoutingFixture *fixture, const gchar *method, const gchar *suffix,
        const gchar *token)
{
    Sent *found = NULL;
    guint i;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (sent->done || g_strcmp0(sent->method, method) != 0 ||
            !g_str_has_suffix(sent->url, suffix))
            continue;

        if (token != NULL && g_strcmp0(sent->token, token) != 0)
            continue;

        g_assert_null(found);
        found = sent;
    }

    g_assert_nonnull(found);

    return found;
}

/* The first request sent to @url, answered or not. */
static Sent *
first_sent(RoutingFixture *fixture, const gchar *method, const gchar *url)
{
    guint i;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, method) == 0 &&
            g_strcmp0(sent->url, url) == 0)
            return sent;
    }

    g_assert_not_reached();
    return NULL;
}

/* How many requests went to @url at all. */
static guint
count_sent(RoutingFixture *fixture, const gchar *method, const gchar *url)
{
    guint count = 0;
    guint i;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, method) == 0 &&
            g_strcmp0(sent->url, url) == 0)
            count++;
    }

    return count;
}

/* The token the answer to @url went out with, or NULL if none did. */
static const gchar *
token_posted_to(RoutingFixture *fixture, const gchar *url)
{
    guint i;

    for (i = 0; i < fixture->sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture->sent, i);

        if (g_strcmp0(sent->method, "POST") == 0 &&
            g_strcmp0(sent->url, url) == 0)
            return sent->token;
    }

    return NULL;
}

/*
 * Every g_message, kept, so a test can say what the bridge did *not*
 * tell the operator.  Messages are never fatal, so expecting them is no
 * use for an absence.
 */
static void
on_message(const gchar *domain, GLogLevelFlags level, const gchar *message,
           gpointer user_data)
{
    RoutingFixture *fixture = user_data;

    (void)domain;
    (void)level;

    g_ptr_array_add(fixture->said, g_strdup(message));
}

/* Whether any message mentioned @needle. */
static gboolean
said_about(RoutingFixture *fixture, const gchar *needle)
{
    guint i;

    for (i = 0; i < fixture->said->len; i++) {
        if (strstr(g_ptr_array_index(fixture->said, i), needle) != NULL)
            return TRUE;
    }

    return FALSE;
}

static void
on_raised(ClawtVentureBridge *bridge, const gchar *decision_id,
          const gchar *agent_id, gpointer user_data)
{
    RoutingFixture *fixture = user_data;

    (void)bridge;

    g_ptr_array_add(fixture->raised,
                    g_strdup_printf("%s %s", decision_id, agent_id));
}

static gchar *
fixture_body(const gchar *name)
{
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, name, NULL);
    gchar *body = NULL;

    g_assert_true(g_file_get_contents(path, &body, NULL, NULL));

    return body;
}

/*
 * Two agents' connectors on one server.  Spelt differently on purpose
 * -- one with a trailing slash -- since a config file will, and they
 * are still one server with one queue.
 */
static void
routing_setup(RoutingFixture *fixture)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-venture-rt-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "decisions.db", NULL);

    fixture->decisions = clawt_decision_store_new(path, &error);
    g_assert_no_error(error);

    fixture->sent = g_ptr_array_new_with_free_func((GDestroyNotify)sent_free);
    fixture->raised = g_ptr_array_new_with_free_func(g_free);
    fixture->said = g_ptr_array_new_with_free_func(g_free);
    fixture->bridge = clawt_venture_bridge_new(fixture->decisions, NULL);

    clawt_venture_bridge_set_request_func(fixture->bridge, fake_request,
                                          fixture, NULL);
    g_signal_connect(fixture->bridge, "decision-raised",
                     G_CALLBACK(on_raised), fixture);

    clawt_venture_bridge_set_source(fixture->bridge, ALPHA, SERVER,
                                    ALPHA_KEY, "alpha");
    clawt_venture_bridge_set_source(fixture->bridge, BETA, SERVER "/",
                                    BETA_KEY, "beta");
}

static void
routing_teardown(RoutingFixture *fixture)
{
    g_clear_object(&fixture->bridge);
    g_clear_object(&fixture->decisions);
    g_clear_pointer(&fixture->sent, g_ptr_array_unref);
    g_clear_pointer(&fixture->raised, g_ptr_array_unref);
    g_clear_pointer(&fixture->said, g_ptr_array_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

/* The agent a stored decision is filed under, or NULL if there is none. */
static gchar *
agent_of(RoutingFixture *fixture, const gchar *source, const gchar *card)
{
    g_autofree gchar *id = clawt_venture_decision_id(source, card);
    g_autoptr(ClawtDecision) decision =
        clawt_decision_store_get(fixture->decisions, id);

    return decision != NULL ? g_strdup(clawt_decision_get_agent(decision))
                            : NULL;
}

static guint
open_count(RoutingFixture *fixture)
{
    g_autoptr(GPtrArray) open =
        clawt_decision_store_list(fixture->decisions, TRUE);

    return open != NULL ? open->len : 0;
}

/* ── Routing ─────────────────────────────────────────────────────── */

/*
 * Two cards, two tokens, one queue: exactly one decision each, under
 * the agent whose token staged it.  And again on the next poll, which
 * is where fifteen connectors used to make fifteen of everything.
 */
static void
test_each_card_goes_to_the_agent_that_staged_it(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-shared.json");
    g_autofree gchar *alpha_card = NULL;
    g_autofree gchar *beta_card = NULL;
    g_autofree gchar *crossed_a = NULL;
    g_autofree gchar *crossed_b = NULL;
    g_autofree gchar *alpha_id = NULL;
    g_autofree gchar *beta_id = NULL;
    g_autofree gchar *alpha_signal = NULL;
    g_autofree gchar *beta_signal = NULL;

    routing_setup(&fixture);
    fixture.queue = queue;

    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA), ==,
                    "API token #1");
    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, BETA), ==,
                    "API token #2");

    g_assert_cmpuint(open_count(&fixture), ==, 2);

    alpha_card = agent_of(&fixture, ALPHA, "c-alpha");
    beta_card = agent_of(&fixture, BETA, "c-beta");
    g_assert_cmpstr(alpha_card, ==, "alpha");
    g_assert_cmpstr(beta_card, ==, "beta");

    /* And under nobody else's name. */
    crossed_a = agent_of(&fixture, BETA, "c-alpha");
    crossed_b = agent_of(&fixture, ALPHA, "c-beta");
    g_assert_null(crossed_a);
    g_assert_null(crossed_b);

    /* The badge that moves is the right agent's. */
    alpha_id = clawt_venture_decision_id(ALPHA, "c-alpha");
    beta_id = clawt_venture_decision_id(BETA, "c-beta");
    alpha_signal = g_strdup_printf("%s alpha", alpha_id);
    beta_signal = g_strdup_printf("%s beta", beta_id);
    g_assert_cmpuint(fixture.raised->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fixture.raised, 0), ==, alpha_signal);
    g_assert_cmpstr(g_ptr_array_index(fixture.raised, 1), ==, beta_signal);

    /* Polled again: the same two, and nothing new. */
    clawt_venture_bridge_poll(fixture.bridge);
    g_assert_cmpuint(open_count(&fixture), ==, 2);
    g_assert_cmpuint(fixture.raised->len, ==, 2);

    routing_teardown(&fixture);
}

/*
 * An answer goes out with the token of the agent that staged the card,
 * never with the token that happened to read the queue -- which here is
 * alpha's, for both cards.
 */
static void
test_an_answer_carries_the_owners_token(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-shared.json");
    g_autofree gchar *alpha_id = NULL;
    g_autofree gchar *beta_id = NULL;

    routing_setup(&fixture);
    fixture.queue = queue;

    clawt_venture_bridge_poll(fixture.bridge);

    /* Both read with one token: the queue was asked for once. */
    g_assert_cmpuint(count_sent(&fixture, "GET",
                                SERVER "/api/v1/confirmations"), ==, 1);
    g_assert_cmpstr(first_sent(&fixture, "GET",
                               SERVER "/api/v1/confirmations")->token,
                    ==, ALPHA_KEY);

    alpha_id = clawt_venture_decision_id(ALPHA, "c-alpha");
    beta_id = clawt_venture_decision_id(BETA, "c-beta");

    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, beta_id,
                                              "approve"));
    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, alpha_id,
                                              "reject"));

    g_assert_cmpstr(token_posted_to(&fixture,
                                    SERVER "/api/v1/confirmations/c-beta/approve"),
                    ==, BETA_KEY);
    g_assert_cmpstr(token_posted_to(&fixture,
                                    SERVER "/api/v1/confirmations/c-alpha/reject"),
                    ==, ALPHA_KEY);
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);

    routing_teardown(&fixture);
}

/*
 * Every response matched to its own request, in whatever order they
 * come back: the queue before anybody has said who they are, beta's
 * whoami before alpha's with a health answer between them, and two
 * answers whose results arrive the wrong way round.
 */
static void
test_responses_out_of_order_do_not_cross(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-shared.json");
    g_autofree gchar *alpha_card = NULL;
    g_autofree gchar *beta_card = NULL;
    g_autofree gchar *alpha_id = NULL;
    g_autofree gchar *beta_id = NULL;
    Sent *alpha_post;
    Sent *beta_post;

    routing_setup(&fixture);
    fixture.queue = queue;
    fixture.defer = TRUE;

    clawt_venture_bridge_poll(fixture.bridge);

    /*
     * The queue first, with nobody identified.  Routing it now would
     * file every card with nobody -- so it is not routed at all.
     */
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/confirmations",
                              NULL), FALSE);
    g_assert_cmpuint(open_count(&fixture), ==, 0);

    /* Beta's whoami, then a health answer, then alpha's. */
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami", BETA_KEY),
            FALSE);
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/health", ALPHA_KEY),
            FALSE);

    /* Half known is not known: still nothing filed. */
    g_assert_null(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA));
    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, BETA), ==,
                    "API token #2");

    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami", ALPHA_KEY),
            FALSE);
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/health", BETA_KEY),
            FALSE);

    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA), ==,
                    "API token #1");
    g_assert_cmpint(clawt_venture_bridge_get_staging(fixture.bridge, ALPHA),
                    ==, CLAWT_VENTURE_STAGING_SUPPORTED);
    g_assert_cmpint(clawt_venture_bridge_get_staging(fixture.bridge, BETA),
                    ==, CLAWT_VENTURE_STAGING_SUPPORTED);

    /* The next tick asks the queue again, and only the queue. */
    clawt_venture_bridge_poll(fixture.bridge);
    g_assert_cmpuint(fixture.sent->len, ==, 6);
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/confirmations",
                              NULL), FALSE);

    alpha_card = agent_of(&fixture, ALPHA, "c-alpha");
    beta_card = agent_of(&fixture, BETA, "c-beta");
    g_assert_cmpstr(alpha_card, ==, "alpha");
    g_assert_cmpstr(beta_card, ==, "beta");
    g_assert_cmpuint(open_count(&fixture), ==, 2);

    /* Two answers out at once; beta's comes back first, alpha's fails. */
    alpha_id = clawt_venture_decision_id(ALPHA, "c-alpha");
    beta_id = clawt_venture_decision_id(BETA, "c-beta");
    clawt_venture_bridge_answer(fixture.bridge, alpha_id, "approve");
    clawt_venture_bridge_answer(fixture.bridge, beta_id, "approve");

    alpha_post = waiting(&fixture, "POST", "/c-alpha/approve", ALPHA_KEY);
    beta_post = waiting(&fixture, "POST", "/c-beta/approve", BETA_KEY);

    deliver(&fixture, beta_post, FALSE);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*it stays queued*");
    deliver(&fixture, alpha_post, TRUE);
    g_test_assert_expected_messages();

    /* Beta's is settled; alpha's -- and only alpha's -- is still owed. */
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 1);
    g_assert_cmpstr(clawt_venture_bridge_pending_answer_url(fixture.bridge, 0),
                    ==, SERVER "/api/v1/confirmations/c-alpha/approve");

    /* A transport reporting the same request twice changes nothing. */
    clawt_venture_bridge_complete(fixture.bridge, beta_post->id, "", 0, NULL);
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 1);

    routing_teardown(&fixture);
}

/*
 * A card no connector here staged goes to neither agent: an unknown
 * token's, a person's typed into VENTURE under a username one agent's
 * token also belongs to, and one with no origin at all.  None is filed,
 * because nothing clawtilla holds may answer any of them.
 */
static void
test_a_stranger_s_card_goes_to_no_agent(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue =
        fixture_body("venture-confirmations-unowned.json");
    g_autofree gchar *id = NULL;
    const gchar *cards[] = { "c-stranger", "c-person", "c-nobody" };
    guint i;

    routing_setup(&fixture);
    fixture.queue = queue;

    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpuint(open_count(&fixture), ==, 0);
    g_assert_cmpuint(fixture.raised->len, ==, 0);

    for (i = 0; i < G_N_ELEMENTS(cards); i++) {
        g_autofree gchar *under_alpha = agent_of(&fixture, ALPHA, cards[i]);
        g_autofree gchar *under_beta = agent_of(&fixture, BETA, cards[i]);

        g_assert_null(under_alpha);
        g_assert_null(under_beta);
    }

    /*
     * And an answer to one -- a decision filed before routing existed,
     * say -- is sent with nobody's token.
     */
    id = clawt_venture_decision_id(ALPHA, "c-stranger");
    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, id, "approve"));
    g_assert_null(token_posted_to(&fixture,
                                  SERVER "/api/v1/confirmations/c-stranger"
                                  "/approve"));
    g_assert_cmpuint(clawt_venture_bridge_pending_answers(fixture.bridge),
                     ==, 0);

    routing_teardown(&fixture);
}

/*
 * One read of a server's queue per tick, however many connectors share
 * it -- and still one per server when there are two servers.
 */
static void
test_one_poll_per_instance(void)
{
    RoutingFixture fixture = { 0 };

    routing_setup(&fixture);
    clawt_venture_bridge_set_source(fixture.bridge, "gamma-venture",
                                    "https://other.example", "vt_gamma",
                                    "gamma");

    clawt_venture_bridge_poll(fixture.bridge);
    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpuint(count_sent(&fixture, "GET",
                                SERVER "/api/v1/confirmations"), ==, 2);
    g_assert_cmpuint(count_sent(&fixture, "GET",
                                "https://other.example/api/v1/confirmations"),
                     ==, 2);

    /* Identity is per source, and asked once each. */
    g_assert_cmpuint(count_sent(&fixture, "GET", SERVER "/api/v1/whoami"),
                     ==, 2);
    g_assert_cmpuint(count_sent(&fixture, "GET",
                                "https://other.example/api/v1/whoami"), ==, 1);

    routing_teardown(&fixture);
}

/* ── Upgrading over the old ids ──────────────────────────────────── */

/*
 * Before routing, every card on a shared server was filed under the
 * source that read the queue -- `venture-beta-venture-c-alpha` for a
 * card alpha staged.  One somebody already answered must be delivered
 * with alpha's token, not raised again as a fresh question; one still
 * open must not grow a twin, and answering it later still goes out as
 * alpha.
 */
static void
test_a_card_filed_under_an_old_id_is_not_raised_again(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-shared.json");
    g_autofree gchar *answered_id = NULL;
    g_autofree gchar *open_id = NULL;
    g_autofree gchar *stored = NULL;
    g_autofree gchar *stored_open = NULL;
    g_autofree gchar *twin = NULL;
    g_autofree gchar *open_twin = NULL;
    g_autoptr(ClawtDecision) legacy = NULL;
    g_autoptr(ClawtDecision) legacy_open = NULL;
    g_autoptr(ClawtDecision) settled = NULL;
    g_autoptr(GError) error = NULL;

    routing_setup(&fixture);
    fixture.queue = queue;

    /* c-alpha: filed under beta's name, and answered. */
    answered_id = clawt_venture_decision_id(BETA, "c-alpha");
    legacy = clawt_decision_new(answered_id, "beta", "old question");
    stored = clawt_decision_store_post(fixture.decisions, legacy, &error);
    g_assert_no_error(error);
    settled = clawt_decision_store_answer(fixture.decisions, answered_id,
                                          "approve", &error);
    g_assert_no_error(error);

    /* c-beta: filed under alpha's name, and still open. */
    open_id = clawt_venture_decision_id(ALPHA, "c-beta");
    legacy_open = clawt_decision_new(open_id, "alpha", "old question");
    stored_open = clawt_decision_store_post(fixture.decisions, legacy_open,
                                            &error);
    g_assert_no_error(error);

    clawt_venture_bridge_poll(fixture.bridge);

    /* No new decision for either card. */
    g_assert_cmpuint(fixture.raised->len, ==, 0);
    twin = agent_of(&fixture, ALPHA, "c-alpha");
    open_twin = agent_of(&fixture, BETA, "c-beta");
    g_assert_null(twin);
    g_assert_null(open_twin);

    /* The answer that was owed went out, as alpha. */
    g_assert_cmpstr(token_posted_to(&fixture,
                                    SERVER "/api/v1/confirmations/c-alpha"
                                    "/approve"),
                    ==, ALPHA_KEY);

    /* And the open one, answered now under its old id, goes as beta. */
    g_assert_true(clawt_venture_bridge_answer(fixture.bridge, open_id,
                                              "reject"));
    g_assert_cmpstr(token_posted_to(&fixture,
                                    SERVER "/api/v1/confirmations/c-beta"
                                    "/reject"),
                    ==, BETA_KEY);

    routing_teardown(&fixture);
}

/* ── Identity ────────────────────────────────────────────────────── */

/*
 * A whoami answer that comes back after its source was replaced was
 * about the old token.  A rotation can hand the connector to somebody
 * else, so crediting it to the new one would route cards on a guess.
 */
static void
test_a_late_whoami_is_not_credited_to_a_new_token(void)
{
    RoutingFixture fixture = { 0 };
    Sent *stale;

    routing_setup(&fixture);
    fixture.defer = TRUE;

    clawt_venture_bridge_poll(fixture.bridge);
    stale = waiting(&fixture, "GET", "/api/v1/whoami", ALPHA_KEY);

    clawt_venture_bridge_set_source(fixture.bridge, ALPHA, SERVER,
                                    "vt_rotated", "alpha");
    deliver(&fixture, stale, FALSE);

    g_assert_null(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA));

    /* The new token is asked for itself on the next tick. */
    clawt_venture_bridge_poll(fixture.bridge);
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami",
                              "vt_rotated"), FALSE);
    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA), ==,
                    "API token #3");

    routing_teardown(&fixture);
}

/*
 * A whoami that fails is asked again.  Meanwhile the source that did
 * answer has its card filed -- an actor names one token -- and the card
 * nobody matched is held, since it may be the silent source's.
 */
static void
test_a_failed_whoami_holds_only_its_own_cards(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-shared.json");
    g_autofree gchar *alpha_card = NULL;
    g_autofree gchar *beta_card = NULL;
    guint i;

    routing_setup(&fixture);
    fixture.queue = queue;
    fixture.defer = TRUE;

    clawt_venture_bridge_poll(fixture.bridge);

    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami", ALPHA_KEY),
            FALSE);
    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami", BETA_KEY),
            TRUE);

    for (i = 0; i < fixture.sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture.sent, i);

        if (!sent->done)
            deliver(&fixture, sent, FALSE);
    }

    alpha_card = agent_of(&fixture, ALPHA, "c-alpha");
    g_assert_cmpstr(alpha_card, ==, "alpha");
    g_assert_cmpuint(open_count(&fixture), ==, 1);

    /* Asked again; this time it answers, and beta's card is filed. */
    fixture.defer = FALSE;
    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpuint(count_sent(&fixture, "GET", SERVER "/api/v1/whoami"),
                     ==, 3);
    beta_card = agent_of(&fixture, BETA, "c-beta");
    g_assert_cmpstr(beta_card, ==, "beta");
    g_assert_cmpuint(open_count(&fixture), ==, 2);
    g_assert_cmpuint(fixture.raised->len, ==, 2);

    routing_teardown(&fixture);
}

/*
 * Three connectors on one server, one of them revoked.  The other two
 * agents' changes still reach the inbox, once each and under the right
 * agent; the revoked one's card is held without being called nobody's
 * -- it is not -- and is filed under its agent the tick its whoami
 * answers again.
 */
static void
test_one_broken_connector_holds_up_nobody_else(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue = fixture_body("venture-confirmations-three.json");
    g_autofree gchar *alpha_card = NULL;
    g_autofree gchar *beta_card = NULL;
    g_autofree gchar *gamma_early = NULL;
    g_autofree gchar *gamma_card = NULL;
    guint handler;

    routing_setup(&fixture);
    clawt_venture_bridge_set_source(fixture.bridge, "gamma-venture", SERVER,
                                    "vt_gamma", "gamma");
    fixture.queue = queue;
    fixture.broken = "vt_gamma";

    handler = g_log_set_handler("Clawtilla", G_LOG_LEVEL_MESSAGE, on_message,
                                &fixture);

    /* Two polls with gamma's whoami failing: the second must not re-file. */
    clawt_venture_bridge_poll(fixture.bridge);
    clawt_venture_bridge_poll(fixture.bridge);

    alpha_card = agent_of(&fixture, ALPHA, "c-alpha");
    beta_card = agent_of(&fixture, BETA, "c-beta");
    gamma_early = agent_of(&fixture, "gamma-venture", "c-gamma");
    g_assert_cmpstr(alpha_card, ==, "alpha");
    g_assert_cmpstr(beta_card, ==, "beta");
    g_assert_null(gamma_early);
    g_assert_cmpuint(open_count(&fixture), ==, 2);
    g_assert_cmpuint(fixture.raised->len, ==, 2);

    /* The failure is said; the held card is not called unowned. */
    g_assert_true(said_about(&fixture, "could not learn who 'gamma-venture'"));
    g_assert_false(said_about(&fixture, "c-gamma"));

    /* gamma's token works again: its card is filed under gamma. */
    fixture.broken = NULL;
    clawt_venture_bridge_poll(fixture.bridge);

    gamma_card = agent_of(&fixture, "gamma-venture", "c-gamma");
    g_assert_cmpstr(gamma_card, ==, "gamma");
    g_assert_cmpuint(open_count(&fixture), ==, 3);
    g_assert_cmpuint(fixture.raised->len, ==, 3);
    g_assert_false(said_about(&fixture, "c-gamma"));

    g_log_remove_handler("Clawtilla", handler);
    routing_teardown(&fixture);
}

/*
 * The queue is read with a token VENTURE has accepted.  At startup
 * nobody has answered whoami, so the first source by name reads it; once
 * that source's whoami has failed and a sibling's has answered, the
 * sibling reads it -- a revoked token first in the alphabet must not
 * leave the server unread for ever.
 */
static void
test_the_queue_is_read_with_a_known_token(void)
{
    RoutingFixture fixture = { 0 };
    Sent *first;
    Sent *second;
    guint i;

    routing_setup(&fixture);
    fixture.defer = TRUE;

    clawt_venture_bridge_poll(fixture.bridge);

    /* Startup: nobody known, so alpha -- first by name -- reads it. */
    first = waiting(&fixture, "GET", "/api/v1/confirmations", NULL);
    g_assert_cmpstr(first->token, ==, ALPHA_KEY);

    deliver(&fixture, waiting(&fixture, "GET", "/api/v1/whoami", ALPHA_KEY),
            TRUE);

    for (i = 0; i < fixture.sent->len; i++) {
        Sent *sent = g_ptr_array_index(fixture.sent, i);

        if (!sent->done)
            deliver(&fixture, sent, FALSE);
    }

    g_assert_null(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA));
    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, BETA), ==,
                    "API token #2");

    /* The next tick: beta, the known one, reads it. */
    clawt_venture_bridge_poll(fixture.bridge);
    second = waiting(&fixture, "GET", "/api/v1/confirmations", NULL);
    g_assert_cmpstr(second->token, ==, BETA_KEY);

    routing_teardown(&fixture);
}

/*
 * A card whose display name matches no actor is still filed under the
 * connector whose whoami token id is the card origin token id.  The
 * name "renamed" matches neither actor, so a string match would file
 * it under nobody; the id is what places it.
 *
 * A second card on the same queue carries no token id and still routes
 * by its actor, which is the fallback for an older server.
 */
static void
test_a_token_id_beats_a_renamed_actor(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue =
        fixture_body("venture-confirmations-by-token-id.json");
    g_autofree gchar *renamed = NULL;
    g_autofree gchar *renamed_under_beta = NULL;
    g_autofree gchar *by_actor = NULL;
    g_autofree gchar *by_actor_under_alpha = NULL;

    routing_setup(&fixture);
    fixture.queue = queue;

    clawt_venture_bridge_poll(fixture.bridge);

    renamed = agent_of(&fixture, ALPHA, "c-renamed");
    renamed_under_beta = agent_of(&fixture, BETA, "c-renamed");
    by_actor = agent_of(&fixture, BETA, "c-by-actor");
    by_actor_under_alpha = agent_of(&fixture, ALPHA, "c-by-actor");

    g_assert_cmpstr(renamed, ==, "alpha");
    g_assert_null(renamed_under_beta);
    g_assert_cmpstr(by_actor, ==, "beta");
    g_assert_null(by_actor_under_alpha);
    g_assert_cmpuint(open_count(&fixture), ==, 2);

    routing_teardown(&fixture);
}

/*
 * The daemon learned whoami before the server sent token ids, so each
 * source is known with no id and is not asked again.  A card from the
 * upgraded server carries an id and the actor string API token #1.
 * The id matches nobody; the actor still files the card under alpha.
 */
static void
test_an_id_falls_back_to_the_actor_of_a_legacy_whoami(void)
{
    RoutingFixture fixture = { 0 };
    g_autofree gchar *queue =
        fixture_body("venture-confirmations-legacy-whoami.json");
    g_autofree gchar *upgraded = NULL;
    g_autofree gchar *under_beta = NULL;

    routing_setup(&fixture);
    fixture.queue = queue;
    fixture.omit_token_id = TRUE;

    clawt_venture_bridge_poll(fixture.bridge);

    g_assert_cmpstr(clawt_venture_bridge_get_actor(fixture.bridge, ALPHA), ==,
                    "API token #1");

    upgraded = agent_of(&fixture, ALPHA, "c-upgraded");
    under_beta = agent_of(&fixture, BETA, "c-upgraded");
    g_assert_cmpstr(upgraded, ==, "alpha");
    g_assert_null(under_beta);
    g_assert_cmpuint(open_count(&fixture), ==, 1);

    routing_teardown(&fixture);
}

/*
 * whoami parses the shape VENTURE's own contract records -- the copy in
 * tests/fixtures/venture/, checked against VENTURE by
 * `make check-venture-contract`.
 */
static void
test_whoami_reads_the_contract_shape(void)
{
    g_autofree gchar *body = fixture_body("venture/whoami.json");
    g_autofree gchar *actor = NULL;
    g_autofree gchar *username = NULL;
    gint64 token_id = -1;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_venture_whoami_parse(body, -1, &actor, &username,
                                             &token_id, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(actor, ==, "string");
    g_assert_cmpstr(username, ==, "string");
    /* The contract sample names the type, it does not send an id. */
    g_assert_cmpint(token_id, ==, 0);

    /* Not an object is not an identity. */
    g_assert_false(clawt_venture_whoami_parse("[]", -1, NULL, NULL,
                                              NULL, NULL));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add_func("/venture/routing/each-card-to-its-agent",
                    test_each_card_goes_to_the_agent_that_staged_it);
    g_test_add_func("/venture/routing/answer-carries-owners-token",
                    test_an_answer_carries_the_owners_token);
    g_test_add_func("/venture/routing/out-of-order-does-not-cross",
                    test_responses_out_of_order_do_not_cross);
    g_test_add_func("/venture/routing/stranger-goes-to-no-agent",
                    test_a_stranger_s_card_goes_to_no_agent);
    g_test_add_func("/venture/routing/one-poll-per-instance",
                    test_one_poll_per_instance);
    g_test_add_func("/venture/routing/old-id-not-raised-again",
                    test_a_card_filed_under_an_old_id_is_not_raised_again);
    g_test_add_func("/venture/routing/late-whoami-not-credited",
                    test_a_late_whoami_is_not_credited_to_a_new_token);
    g_test_add_func("/venture/routing/failed-whoami-holds-only-its-own",
                    test_a_failed_whoami_holds_only_its_own_cards);
    g_test_add_func("/venture/routing/broken-connector-holds-up-nobody",
                    test_one_broken_connector_holds_up_nobody_else);
    g_test_add_func("/venture/routing/queue-read-with-known-token",
                    test_the_queue_is_read_with_a_known_token);
    g_test_add_func("/venture/routing/token-id-beats-renamed-actor",
                    test_a_token_id_beats_a_renamed_actor);
    g_test_add_func("/venture/routing/id-falls-back-to-legacy-actor",
                    test_an_id_falls_back_to_the_actor_of_a_legacy_whoami);

    g_test_add_func("/venture/routing/whoami-contract-shape",
                    test_whoami_reads_the_contract_shape);

    return g_test_run();
}
