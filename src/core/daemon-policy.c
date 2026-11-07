/*
 * daemon-policy.c - Answering the action rules that need somebody
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawt_policy_mode_for() settles allow and deny from the config alone.
 * The other three modes need the daemon: `ask` posts a decision and waits
 * for a person, `review` asks a decision model first and becomes `ask`
 * when it is unsure, and `hand_back` refuses and leaves the work in the
 * operator's inbox. Every route a call can arrive by -- clawtilla's own
 * tools, the Claude Code hook, the connector and desktop relays -- comes
 * through clawt_daemon_policy_request(), so the three cannot disagree.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <ai-glib.h>
#include <string.h>

/* How often open decisions are checked for a lapsed deadline. */
#define POLICY_DEFAULT_TICK_SECONDS 15

/* A review at or above this is let through; anything less asks a person. */
#define POLICY_REVIEW_THRESHOLD 0.8

#define POLICY_ID_PREFIX "policy-"

typedef struct {
    gchar                *agent_id;
    ClawtPolicyReplyFunc  reply;
    gpointer              reply_data;
} PolicyWait;

static void
policy_wait_free(gpointer data)
{
    PolicyWait *wait = data;

    g_free(wait->agent_id);
    g_free(wait);
}

/* ── Asking a person ──────────────────────────────────────────────── */

static gint
ask_timeout_seconds(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *config = clawt_config_get_agent(self->config, agent_id);
    gint seconds = config != NULL
        ? clawt_agent_config_get_int(config, "policy.ask_timeout_seconds")
        : clawt_config_get_int(self->config, "defaults.policy.ask_timeout_seconds");

    return seconds > 0 ? seconds : 600;
}

static void
post_ask(ClawtDaemon *self, const gchar *agent_id, const gchar *rule,
         const gchar *summary, const gchar *doubt,
         ClawtPolicyReplyFunc reply, gpointer reply_data)
{
    static const gchar * const options[] = { "approve", "deny", NULL };
    g_autofree gchar *uuid = g_uuid_string_random();
    g_autofree gchar *id = g_strconcat(POLICY_ID_PREFIX, uuid, NULL);
    g_autofree gchar *question = NULL;
    g_autofree gchar *stored = NULL;
    g_autoptr(ClawtDecision) decision = NULL;
    g_autoptr(GError) error = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint timeout = ask_timeout_seconds(self, agent_id);
    PolicyWait *wait;

    if (self->decisions == NULL) {
        reply(FALSE, "The action rule needs a person's answer, and this daemon "
                     "keeps no decision inbox to ask in.", reply_data);
        return;
    }

    question = g_strdup_printf("%s wants to run: %s\n\nThe action rule '%s' asks first.%s%s",
                               agent_id, summary, rule,
                               doubt != NULL ? "\n\n" : "", doubt != NULL ? doubt : "");
    decision = clawt_decision_new(id, agent_id, question);
    clawt_decision_set_options(decision, options);
    clawt_decision_set_default(decision, "deny", "nobody answered in time");
    clawt_decision_set_reversible_until(decision, now + timeout);

    stored = clawt_decision_store_post(self->decisions, decision, &error);
    if (stored == NULL) {
        g_autofree gchar *message = g_strdup_printf(
            "The action rule '%s' needs a person's answer, and asking failed: %s",
            rule, error->message);

        reply(FALSE, message, reply_data);
        return;
    }

    wait = g_new0(PolicyWait, 1);
    wait->agent_id = g_strdup(agent_id);
    wait->reply = reply;
    wait->reply_data = reply_data;
    g_hash_table_insert(self->policy_waits, g_strdup(stored), wait);

    if (self->bus != NULL)
        clawt_event_bus_emit(self->bus, "decision.asked", agent_id);

    /* Waiting on a person is not a stall: the turn budgets hold. */
    clawt_daemon_turn_hold(self, agent_id);
}

/* ── Handing back ─────────────────────────────────────────────────── */

static void
post_hand_back(ClawtDaemon *self, const gchar *agent_id, const gchar *rule,
               const gchar *summary, ClawtPolicyReplyFunc reply, gpointer reply_data)
{
    static const gchar * const options[] = { "done", "dropped", NULL };
    g_autofree gchar *question = NULL;
    g_autofree gchar *stored = NULL;
    g_autofree gchar *message = NULL;
    g_autoptr(ClawtDecision) decision = NULL;

    question = g_strdup_printf("%s handed this to you instead of doing it, under the "
                               "action rule '%s':\n\n%s", agent_id, rule, summary);
    decision = clawt_decision_new(NULL, agent_id, question);
    clawt_decision_set_options(decision, options);
    clawt_decision_set_default(decision, "dropped", "nobody took it");

    if (self->decisions != NULL)
        stored = clawt_decision_store_post(self->decisions, decision, NULL);
    if (stored != NULL && self->bus != NULL)
        clawt_event_bus_emit(self->bus, "decision.asked", agent_id);

    message = stored != NULL
        ? g_strdup_printf("Handed to the operator as decision %s under the action rule "
                          "'%s'. Do not retry it; carry on with the rest of your work.",
                          stored, rule)
        : g_strdup_printf("Refused by the action rule '%s', which hands this to the "
                          "operator, and there is no inbox to hand it to.", rule);
    reply(FALSE, message, reply_data);
}

/* ── Asking a model ───────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon          *daemon;
    gchar                *agent_id;
    gchar                *rule;
    gchar                *summary;
    ClawtPolicyReplyFunc  reply;
    gpointer              reply_data;
} Review;

static void
review_free(Review *review)
{
    g_object_unref(review->daemon);
    g_free(review->agent_id);
    g_free(review->rule);
    g_free(review->summary);
    g_free(review);
}

static void
on_reviewed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Review *review = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiDecisionResponse) response =
        ai_decider_decide_finish(AI_DECIDER(source), result, &error);
    gdouble p = response != NULL
        ? ai_decision_response_get_probability(response, "fits", NULL) : -1.0;

    /*
     * A failed or doubtful review becomes a question for a person, never
     * a pass: a reviewer that is down must not turn into "allow".
     */
    if (p >= POLICY_REVIEW_THRESHOLD) {
        review->reply(TRUE, NULL, review->reply_data);
    } else {
        g_autofree gchar *doubt = response != NULL
            ? g_strdup_printf("The reviewer was not sure enough (%.2f).", p)
            : g_strdup_printf("The reviewer could not answer: %s", error->message);

        post_ask(review->daemon, review->agent_id, review->rule, review->summary,
                 doubt, review->reply, review->reply_data);
    }

    review_free(review);
}

static void
start_review(ClawtDaemon *self, const gchar *agent_id, const gchar *rule,
             const gchar *tool, const gchar *summary,
             ClawtPolicyReplyFunc reply, gpointer reply_data)
{
    const gchar *reviewer = clawt_config_get_string(self->config, "defaults.policy.reviewer");
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GObject) decider = NULL;
    g_autoptr(AiDecisionRequest) request = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) state = NULL;
    g_autofree gchar *state_text = NULL;
    g_autoptr(GError) error = NULL;
    Review *review;

    /* No reviewer configured is `ask`, as the schema says. */
    if (reviewer == NULL || *reviewer == '\0') {
        post_ask(self, agent_id, rule, summary, NULL, reply, reply_data);
        return;
    }

    decider = ai_provider_factory_new_decider(reviewer, NULL, &error);
    if (decider == NULL || !AI_IS_DECIDER(decider)) {
        g_autofree gchar *doubt = g_strdup_printf(
            "The reviewer '%s' could not be built: %s", reviewer,
            error != NULL ? error->message : "not a decision model");

        post_ask(self, agent_id, rule, summary, doubt, reply, reply_data);
        return;
    }

    /* The state a reviewer judges: who the agent is meant to be, and the call. */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent_id);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, agent != NULL
        ? clawt_agent_config_get_string(agent, "description") : NULL);
    json_builder_set_member_name(builder, "tool");
    json_builder_add_string_value(builder, tool);
    json_builder_set_member_name(builder, "call");
    json_builder_add_string_value(builder, summary);
    json_builder_end_object(builder);
    state = json_builder_get_root(builder);
    state_text = json_to_string(state, FALSE);

    request = ai_decision_request_new(state_text);
    if (!ai_decision_request_add_noul(request, "fits",
            "The call is something an agent with this role would be expected to do, "
            "and is safe to do without asking a person first.",
            "the call does what the role describes and risks nothing irreversible",
            "the call is outside the role, irreversible, or costly to undo",
            &error)) {
        g_autofree gchar *doubt = g_strdup_printf("The review could not be framed: %s",
                                                  error->message);

        post_ask(self, agent_id, rule, summary, doubt, reply, reply_data);
        return;
    }

    review = g_new0(Review, 1);
    review->daemon = g_object_ref(self);
    review->agent_id = g_strdup(agent_id);
    review->rule = g_strdup(rule);
    review->summary = g_strdup(summary);
    review->reply = reply;
    review->reply_data = reply_data;

    ai_decider_decide_async(AI_DECIDER(decider), request, NULL, on_reviewed, review);
}

/* ── The one entry point ──────────────────────────────────────────── */

void
clawt_daemon_policy_request(ClawtDaemon          *self,
                            const gchar          *agent_id,
                            const gchar          *tool,
                            ClawtActionClass      action,
                            ClawtPolicyMode       mode,
                            const gchar          *rule,
                            const gchar          *summary,
                            ClawtPolicyReplyFunc  reply,
                            gpointer              reply_data)
{
    (void)action;

    switch (mode) {
    case CLAWT_POLICY_ALLOW:
        reply(TRUE, NULL, reply_data);
        return;
    case CLAWT_POLICY_DENY:
        {
            g_autofree gchar *message = g_strdup_printf(
                "Refused by the action rule '%s'. This is a standing rule, not a "
                "mistake in your call: do not retry it.", rule);

            reply(FALSE, message, reply_data);
        }
        return;
    case CLAWT_POLICY_HAND_BACK:
        post_hand_back(self, agent_id, rule, summary, reply, reply_data);
        return;
    case CLAWT_POLICY_ASK:
        post_ask(self, agent_id, rule, summary, NULL, reply, reply_data);
        return;
    case CLAWT_POLICY_REVIEW:
        start_review(self, agent_id, rule, tool, summary, reply, reply_data);
        return;
    }
}

static void
policy_for_tools(const gchar          *agent_id,
                 const gchar          *tool,
                 ClawtActionClass      action,
                 ClawtPolicyMode       mode,
                 const gchar          *rule,
                 const gchar          *summary,
                 ClawtPolicyReplyFunc  reply,
                 gpointer              reply_data,
                 gpointer              user_data)
{
    clawt_daemon_policy_request(CLAWT_DAEMON(user_data), agent_id, tool, action,
                                mode, rule, summary, reply, reply_data);
}

/* ── Settling ─────────────────────────────────────────────────────── */

gboolean
clawt_daemon_policy_settle(ClawtDaemon *self, ClawtDecision *decision, const gchar *how)
{
    PolicyWait *wait;
    gpointer key = NULL;
    gpointer value = NULL;
    const gchar *answer;
    gboolean allowed;
    g_autofree gchar *message = NULL;

    if (self->policy_waits == NULL || decision == NULL ||
        !g_hash_table_steal_extended(self->policy_waits, clawt_decision_get_id(decision),
                                     &key, &value))
        return FALSE;

    wait = value;
    g_free(key);
    answer = clawt_decision_get_answer(decision);
    allowed = g_strcmp0(how, "answered") == 0 && g_strcmp0(answer, "approve") == 0;

    if (!allowed) {
        if (g_strcmp0(how, "defaulted") == 0)
            message = g_strdup("Nobody answered in time, so it was not done. Do not "
                               "retry it; say what you needed in your reply instead.");
        else if (g_strcmp0(how, "dismissed") == 0)
            message = g_strdup("The operator dismissed the request, so it was not done.");
        else
            message = g_strdup_printf("The operator answered '%s', so it was not done.%s",
                                      answer != NULL ? answer : "deny",
                                      g_strcmp0(answer, "deny") != 0
                                          ? " Their words are their instructions." : "");
    }

    clawt_daemon_turn_release(self, wait->agent_id);
    wait->reply(allowed, message, wait->reply_data);
    policy_wait_free(wait);
    return TRUE;
}

/*
 * Withdraws every ask still waiting for @agent_id's turn, which has ended
 * some other way -- interrupted, stopped, or released by the grace timer.
 *
 * Found by stopping an agent from the Activity page while its Bash call
 * was held on an ask: the turn was gone, the hook that asked was killed
 * with it, and the decision stayed in the inbox reading "gate wants to
 * run: hostname" with a live Approve button. Approving it would have done
 * nothing at all, which is worse than it not being there. So it is
 * dismissed, published as settled, and the waiter told no -- the same as
 * a person dismissing it, said by the daemon.
 */
void
clawt_daemon_policy_abandon(ClawtDaemon *self, const gchar *agent_id)
{
    g_autoptr(GPtrArray) ids = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->policy_waits == NULL || agent_id == NULL)
        return;

    /* Collected first: settling steals from the table being walked. */
    ids = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_iter_init(&iter, self->policy_waits);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (g_strcmp0(((PolicyWait *)value)->agent_id, agent_id) == 0)
            g_ptr_array_add(ids, g_strdup(key));

    for (i = 0; i < ids->len; i++) {
        const gchar *id = g_ptr_array_index(ids, i);
        g_autoptr(ClawtDecision) decision = self->decisions != NULL
            ? clawt_decision_store_get(self->decisions, id) : NULL;
        PolicyWait *wait;

        if (decision != NULL &&
            clawt_decision_store_dismiss(self->decisions, id, NULL)) {
            clawt_daemon_policy_settle(self, decision, "dismissed");
            clawt_daemon_publish_decision_settled(self, decision, "dismissed");
            continue;
        }

        /* No record to settle: still tell the waiter, and drop it. */
        wait = g_hash_table_lookup(self->policy_waits, id);
        if (wait != NULL) {
            wait->reply(FALSE, "The turn that asked ended before anybody answered, "
                               "so it was not done.", wait->reply_data);
            g_hash_table_remove(self->policy_waits, id);
        }
    }
}

/*
 * Decisions whose deadline has passed are settled with their own default,
 * and say so: DEFAULTED, published as `decision.settled how=defaulted`, the
 * turn hold released. Before this, nothing ever reached that state, so a
 * client's badge stayed up for a question nobody could still answer.
 */
static gboolean
on_default_tick(gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(GPtrArray) open = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (self->decisions == NULL)
        return G_SOURCE_CONTINUE;

    open = clawt_decision_store_list(self->decisions, TRUE);
    for (i = 0; open != NULL && i < open->len; i++) {
        ClawtDecision *decision = g_ptr_array_index(open, i);
        gint64 until = clawt_decision_get_reversible_until(decision);
        g_autoptr(ClawtDecision) settled = NULL;

        if (until <= 0 || until > now || clawt_decision_get_default(decision) == NULL)
            continue;

        settled = clawt_decision_store_settle_default(self->decisions,
                                                      clawt_decision_get_id(decision), NULL);
        if (settled == NULL)
            continue;

        if (!clawt_daemon_policy_settle(self, settled, "defaulted"))
            clawt_daemon_turn_release(self, clawt_decision_get_agent(settled));
        clawt_daemon_publish_decision_settled(self, settled, "defaulted");
    }

    return G_SOURCE_CONTINUE;
}

/*
 * Every open policy ask with nobody waiting on it, dismissed.
 *
 * The decision is stored on disk and the call waiting on it lives in a
 * daemon's memory, so a daemon that stopped -- cleanly or not -- left its
 * asks in the inbox for the next one to show, each with an Approve that
 * could do nothing: the hook that asked had been told no, or killed.
 * Found live by restarting a daemon with an ask open. A fresh daemon is
 * waiting on none, so at setup this is every open one.
 */
static void
sweep_orphaned_asks(ClawtDaemon *self)
{
    g_autoptr(GPtrArray) open = NULL;
    guint i;

    if (self->decisions == NULL)
        return;

    open = clawt_decision_store_list(self->decisions, TRUE);
    for (i = 0; open != NULL && i < open->len; i++) {
        ClawtDecision *decision = g_ptr_array_index(open, i);
        const gchar *id = clawt_decision_get_id(decision);

        if (!g_str_has_prefix(id, POLICY_ID_PREFIX) ||
            g_hash_table_contains(self->policy_waits, id))
            continue;

        if (clawt_decision_store_dismiss(self->decisions, id, NULL))
            clawt_daemon_publish_decision_settled(self, decision, "dismissed");
    }
}

void
clawt_daemon_policy_setup(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->policy_waits == NULL)
        self->policy_waits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   policy_wait_free);
    sweep_orphaned_asks(self);
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_policy_func(self->mcp_tools, policy_for_tools, self, NULL);

    /* On the daemon's own context, named here where the source attaches. */
    if (self->policy_tick == NULL) {
        if (self->main_context != NULL)
            g_main_context_push_thread_default(self->main_context);
        self->policy_tick = clawt_timeout_add_seconds(POLICY_DEFAULT_TICK_SECONDS,
                                                      on_default_tick, self);
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
    }
}

void
clawt_daemon_policy_tick(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    on_default_tick(self);
}

void
clawt_daemon_policy_teardown(ClawtDaemon *self)
{
    GHashTableIter iter;
    gpointer value;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_policy_func(self->mcp_tools, NULL, NULL, NULL);
    if (self->policy_tick != NULL) {
        g_source_destroy(self->policy_tick);
        g_clear_pointer(&self->policy_tick, g_source_unref);
    }

    /* Whoever is still waiting is told no, rather than never told anything. */
    if (self->policy_waits != NULL) {
        g_hash_table_iter_init(&iter, self->policy_waits);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            PolicyWait *wait = value;

            wait->reply(FALSE, "The daemon stopped before anybody answered, so it was "
                               "not done.", wait->reply_data);
        }
        g_clear_pointer(&self->policy_waits, g_hash_table_unref);
    }
}
