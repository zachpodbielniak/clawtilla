/*
 * web-decisions.c - Choices agents need a person to make
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Beside the alerts rather than inside them.  An alert is something that
 * happened; a decision is something that needs you, and one badge
 * meaning both is a badge nobody can act on.
 *
 * What makes the list honest rather than a queue of stalled work is that
 * every item states what the agent will do anyway.  The reader is
 * redirecting, not unblocking, and the page says so on every row --
 * because an operator who thinks the fleet is stopped on them reads this
 * list very differently from one who knows it is not.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/*
 * "in 3h", "in 2 days", or nothing at all.
 *
 * Nothing for an item with no stated deadline, rather than "never":
 * the agent did not say, which is a different thing from promising it
 * will keep.
 */
static gchar *
deadline_text(gint64 until, gint64 now)
{
    gint64 left;

    if (until <= 0)
        return NULL;

    left = until - now;

    if (left <= 0)
        return g_strdup("the default has already taken effect");

    /*
     * Pluralised, because "1 more hours" in a list somebody is meant to
     * act on reads as a page that was not finished.
     */
    if (left < 3600) {
        gint minutes = (gint)(left / 60);

        return g_strdup_printf("reversible for %d more minute%s", minutes,
                               minutes == 1 ? "" : "s");
    }

    if (left < (48 * 3600)) {
        gint hours = (gint)(left / 3600);

        return g_strdup_printf("reversible for %d more hour%s", hours,
                               hours == 1 ? "" : "s");
    }

    {
        gint days = (gint)(left / (24 * 3600));

        return g_strdup_printf("reversible for %d more day%s", days,
                               days == 1 ? "" : "s");
    }
}

/* "/decisions/<verb>?decision=<id>", escaped: an id is daemon data */
static gchar *
decision_action(const gchar *verb, const gchar *id)
{
    g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);

    return g_strdup_printf("/decisions/%s?decision=%s", verb, escaped);
}

/* ── Skill proposals ─────────────────────────────────────────────── */

/* Agent-written text in a box that keeps its lines; set as text, never HTML. */
static HtmxElement *
proposal_pre(const gchar *text)
{
    HtmxElement *pre = HTMX_ELEMENT(htmx_pre_new());

    htmx_node_set_text_content(HTMX_NODE(pre), text != NULL ? text : "");
    htmx_element_add_class(pre, "proposal-text");

    return pre;
}

/*
 * A skill proposal's card body: what the agent proposes, and the three
 * answers.
 *
 * The question's first line only, as the heading -- the agent's id and a
 * validated name, nothing it chose freely -- and the full text or the
 * diff under it in a box of its own, because the question's fenced copy
 * is written for a terminal and reads as noise in a page.  Approve and
 * Reject share a form so the note goes with either; Edit is a form of its
 * own inside a disclosure, prefilled with the escaped text the daemon
 * sent, so a forged notice line cannot be saved back in unseen.
 */
static void
web_proposal_review(HtmxDiv *row, JsonObject *decision, JsonObject *proposal)
{
    const gchar *id = clawt_web_member(decision, "id", "");
    const gchar *question = clawt_web_member(decision, "question", "");
    const gchar *kind = clawt_web_member(proposal, "kind", "create");
    const gchar *reason = clawt_web_member(proposal, "reason", NULL);
    JsonArray *warnings = clawt_web_member_array(proposal, "warnings");
    const gchar *end = strchr(question, '\n');
    g_autofree gchar *headline = (end != NULL)
        ? g_strndup(question, (gsize)(end - question)) : g_strdup(question);
    guint i;

    clawt_web_add(row, clawt_web_text(headline, "decision-question"));
    clawt_web_add(row, clawt_web_text(
        "Nothing reaches a prompt until you answer.", "small muted"));

    if (reason != NULL && *reason != '\0') {
        g_autofree gchar *why = g_strdup_printf("Why: %s", reason);

        clawt_web_add(row, clawt_web_text(why, "small"));
    }

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++)
        clawt_web_add(row, clawt_web_notice(
            json_array_get_string_element(warnings, i), ""));

    if (clawt_web_member_bool(proposal, "stale", FALSE))
        clawt_web_add(row, clawt_web_notice(
            "The file changed since this was proposed, so Approve is "
            "refused. Edit shows what is there now; or reject it.", "bad"));

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(proposal_pre(
        g_strcmp0(kind, "update") == 0
            ? clawt_web_member(proposal, "diff", "")
            : clawt_web_member(proposal, "text", ""))));

    {
        g_autoptr(HtmxForm) form = clawt_web_form("/decisions/proposal");
        g_autoptr(HtmxDiv) actions = htmx_div_new();
        g_autoptr(HtmxButton) approve = clawt_web_button("Approve", "primary");
        g_autoptr(HtmxButton) reject = clawt_web_button("Reject", "danger");
        g_autofree gchar *dismiss = decision_action("dismiss", id);
        g_autoptr(HtmxButton) drop = clawt_web_post_button(
            "Does not need me", dismiss, "default", NULL);

        clawt_web_add(form, clawt_web_hidden("proposal", id));
        clawt_web_add(form, clawt_web_field(
            "Why (optional, sent to the agent)", "note", NULL, NULL));

        htmx_element_set_attribute(HTMX_ELEMENT(approve), "type", "submit");
        htmx_element_set_attribute(HTMX_ELEMENT(approve), "name", "action");
        htmx_element_set_attribute(HTMX_ELEMENT(approve), "value", "approve");
        htmx_element_set_attribute(HTMX_ELEMENT(reject), "type", "submit");
        htmx_element_set_attribute(HTMX_ELEMENT(reject), "name", "action");
        htmx_element_set_attribute(HTMX_ELEMENT(reject), "value", "reject");

        htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
        htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(approve));
        htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(reject));
        htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(drop));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(actions));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
    }

    {
        g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
        g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
        g_autoptr(HtmxForm) form = clawt_web_form("/decisions/proposal");

        htmx_node_set_text_content(HTMX_NODE(summary), "Edit");
        clawt_web_add(form, clawt_web_hidden("proposal", id));
        clawt_web_add(form, clawt_web_hidden("action", "edit"));
        clawt_web_add(form, clawt_web_field(
            "Description", "description",
            clawt_web_member(proposal, "description", ""), NULL));
        clawt_web_add(form, clawt_web_textarea_field(
            "Body", "body", clawt_web_member(proposal, "body", ""), 14));
        clawt_web_add(form, clawt_web_button("Save and approve", "primary"));

        htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
        htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(details));
    }
}

/* The pending proposal @id names, out of one `skill.proposals` reply. */
static JsonObject *
proposal_for(JsonNode *proposals, const gchar *id)
{
    JsonArray *items = clawt_web_member_array(clawt_web_root(proposals),
                                              "proposals");
    guint i;

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *proposal = json_array_get_object_element(items, i);

        if (g_strcmp0(clawt_web_member(proposal, "id", NULL), id) == 0)
            return proposal;
    }

    return NULL;
}

static HtmxElement *
decision_row(ClawtWebApp *app, JsonObject *decision, gint64 now,
             JsonObject *proposal)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    const gchar *id = clawt_web_member(decision, "id", "");
    const gchar *agent = clawt_web_member(decision, "agent", "");
    const gchar *question = clawt_web_member(decision, "question", "");
    const gchar *fallback = clawt_web_member(decision, "default", NULL);
    const gchar *reason = clawt_web_member(decision, "default_reason", NULL);
    const gchar *task = clawt_web_member(decision, "task", NULL);
    gint64 until = clawt_web_member_int(decision, "reversible_until", 0);
    gboolean urgent = clawt_web_member_bool(decision, "urgent", FALSE);
    gboolean settled = clawt_web_member_bool(decision, "settled_by_default",
                                              FALSE);
    const gchar *answer = clawt_web_member(decision, "answer", NULL);
    gint64 state = clawt_web_member_int(decision, "state", 0);
    g_autofree gchar *deadline = deadline_text(until, now);

    (void)app;

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(row), "decision-row");

    if (urgent)
        htmx_element_add_class(HTMX_ELEMENT(row), "decision-urgent");

    if (settled)
        htmx_element_add_class(HTMX_ELEMENT(row), "decision-settled");

    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
    clawt_web_add(head, clawt_web_badge(agent, "neutral"));

    if (task != NULL && *task != '\0')
        clawt_web_add(head, clawt_web_badge(task, "info"));

    if (urgent)
        clawt_web_add(head, clawt_web_badge("closing soon", "warn"));

    if (settled)
        clawt_web_add(head, clawt_web_badge("already decided", "bad"));

    /*
     * What became of it, in the settled view.  Answered and dismissed
     * are different things -- both mean the work went a particular way,
     * only one of them means somebody chose -- and a list that showed
     * neither would be a list of questions with no outcomes.
     */
    if (state == 1)
        clawt_web_add(head, clawt_web_badge("answered", "good"));
    else if (state == 2)
        clawt_web_add(head, clawt_web_badge("defaulted", "neutral"));
    else if (state == 3)
        clawt_web_add(head, clawt_web_badge("dismissed", "neutral"));

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    /* A skill proposal draws its own body; only Delete is shared. */
    if (proposal != NULL && state == 0) {
        web_proposal_review(row, decision, proposal);
    } else {
        clawt_web_add(row, clawt_web_text(question, "decision-question"));

        /*
         * The default, always, and phrased as what *is happening* rather
         * than what would happen.  An operator who reads "it will do X if
         * you do not answer" waits; one who reads "it is doing X" answers
         * or moves on, which is the behaviour this whole feature is for.
         */
        if (fallback != NULL && *fallback != '\0') {
            g_autofree gchar *line = (reason != NULL && *reason != '\0')
                ? g_strdup_printf("Going ahead with: %s \xe2\x80\x94 %s",
                                  fallback, reason)
                : g_strdup_printf("Going ahead with: %s", fallback);

            clawt_web_add(row, clawt_web_text(line, "small muted"));
        }

        if (answer != NULL && *answer != '\0') {
            g_autofree gchar *line = g_strdup_printf("You said: %s", answer);

            clawt_web_add(row, clawt_web_text(line, "small"));
        }

        if (deadline != NULL && state == 0)
            clawt_web_add(row, clawt_web_text(deadline, "small muted"));

        /*
         * A free-text answer beside the offered options, because an
         * operator whose answer is "neither, do X" is giving the most
         * valuable answer there is -- and a form that could not carry it
         * would push them back into the conversation this exists to keep
         * them out of.
         */
        /*
         * A settled row carries no form.  Answering twice is refused by the
         * daemon -- the first answer has already reached the agent -- so
         * offering the control would be offering a refusal.
         */
        if (state == 0) {
            g_autoptr(HtmxForm) form = clawt_web_form("/decisions/answer");
            g_autoptr(HtmxDiv) buttons = htmx_div_new();
            JsonArray *options = clawt_web_member_array(decision, "options");
            guint i;

            clawt_web_add(form, clawt_web_hidden("decision", id));

            /*
             * One option per line, not a row of buttons.
             *
             * An option is a sentence -- "Re-provision clawt-oryx from a
             * proper Fedora cloud image, so the exchange mounts and the
             * default-user config land too" is a real one -- and a wrapping
             * button row centres each of them, which gives the eye no left
             * edge to come back to on the second line.  The GTK client
             * stacks them for the same reason.
             */
            htmx_element_add_class(HTMX_ELEMENT(buttons), "decision-options");

            for (i = 0; options != NULL && i < json_array_get_length(options);
                 i++) {
                const gchar *option = json_array_get_string_element(options, i);
                g_autoptr(HtmxButton) pick = NULL;

                if (option == NULL || *option == '\0')
                    continue;

                pick = clawt_web_button(option, "default");
                htmx_element_set_attribute(HTMX_ELEMENT(pick), "type", "submit");
                htmx_element_set_attribute(HTMX_ELEMENT(pick), "name", "answer");
                htmx_element_set_attribute(HTMX_ELEMENT(pick), "value", option);
                htmx_node_add_child(HTMX_NODE(buttons), HTMX_NODE(pick));
            }

            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(buttons));

            clawt_web_add(form, clawt_web_field(
                "Or say something else", "answer", NULL,
                "\"neither, do X\" is a perfectly good answer"));

            {
                g_autoptr(HtmxDiv) actions = htmx_div_new();
                g_autoptr(HtmxButton) send = clawt_web_button("Answer",
                                                              "primary");
                g_autofree gchar *dismiss = decision_action("dismiss", id);
                g_autoptr(HtmxButton) drop = clawt_web_post_button(
                    "Does not need me", dismiss, "default", NULL);

                htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
                htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
                htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(send));
                htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(drop));
                htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(actions));
            }

            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
        }

    }

    /*
     * Delete, on every row and outside the answer form: a settled row
     * has no form, and it is the stale ones -- answered long ago, or
     * left open past a deadline by a daemon from before 0.3.0 -- that
     * somebody most wants gone.
     */
    {
        g_autoptr(HtmxDiv) closing = htmx_div_new();
        g_autofree gchar *remove = decision_action("delete", id);

        htmx_element_add_class(HTMX_ELEMENT(closing), "btn-row");
        g_autoptr(HtmxButton) wipe = clawt_web_post_button("Delete", remove,
                                                           "danger", NULL);

        /* one click, as in the other clients: the agent's default stands */
        htmx_element_set_attribute(HTMX_ELEMENT(wipe), "title",
            "Remove it from the inbox for good; an open one is withdrawn "
            "and the agent carries on with its default");
        htmx_element_add_class(HTMX_ELEMENT(closing), "btn-row");
        htmx_node_add_child(HTMX_NODE(closing), HTMX_NODE(wipe));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(closing));
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static HtmxElement *
decisions_body(ClawtWebApp *app, gboolean all)
{
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) proposals = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    JsonObject *payload;
    JsonArray *items;
    guint i;

    {
        ClawtWebPayload *query = clawt_web_payload_new();

        clawt_web_payload_set_bool(query, "open", !all);
        reply = clawt_web_app_call(app, "decision.list",
                                   clawt_web_payload_take(query));
    }

    if (reply == NULL) {
        /*
         * Copied at the point of failure.  Rendering makes calls of its
         * own and every one of them frees the app's last error, so a
         * message read afterwards is whatever now lives at that address.
         */
        g_autofree gchar *why =
            g_strdup(clawt_web_app_last_error(app));

        clawt_web_add(view, clawt_web_text(
            why != NULL ? why : "the daemon did not answer", "error"));

        return HTMX_ELEMENT(g_steal_pointer(&view));
    }

    payload = clawt_web_root(reply);
    items = clawt_web_member_array(payload, "decisions");

    {
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autoptr(HtmxA) toggle = htmx_a_new_with_href(
            all ? "/decisions" : "/decisions?all=1");

        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
        htmx_node_set_text_content(HTMX_NODE(toggle),
                                   all ? "Only the open ones"
                                       : "Include settled ones");
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(toggle));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(head));
    }

    if (items == NULL || json_array_get_length(items) == 0) {
        /*
         * An empty inbox is the normal state and says so, rather than
         * looking like a page that failed to load.  It also states the
         * bar, because the failure mode is agents being polite rather
         * than selective.
         */
        clawt_web_add(view, clawt_web_text(
            all ? "Nothing has been asked yet."
                : "Nothing is waiting on you. Agents file a decision here "
                  "only when the branches produce materially different "
                  "work, or the choice is not cheaply reversible \xe2\x80\x94 "
                  "everything else they simply decide.",
            "muted"));

        return HTMX_ELEMENT(g_steal_pointer(&view));
    }

    /*
     * The proposals, asked for once and only when the inbox holds one --
     * the library's own rule says which decisions are proposals.
     */
    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *decision = json_array_get_object_element(items, i);

        if (clawt_skill_proposal_id_is_valid(
                clawt_web_member(decision, "id", NULL))) {
            proposals = clawt_web_app_call(app, "skill.proposals", NULL);
            break;
        }
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *decision = json_array_get_object_element(items, i);
        const gchar *id = clawt_web_member(decision, "id", NULL);
        JsonObject *proposal = (proposals != NULL &&
                                clawt_skill_proposal_id_is_valid(id))
                               ? proposal_for(proposals, id) : NULL;

        clawt_web_add(view, decision_row(app, decision, now, proposal));
    }

    return HTMX_ELEMENT(g_steal_pointer(&view));
}

/*
 * The inbox is every agent's, so the agent whose tabs somebody came in
 * through does not narrow it.  Taken and ignored to keep one shape for
 * clawt_web_view_body()'s dispatch -- the page says so itself, in the
 * card's own wording, rather than leaving a reader to wonder whose
 * decisions these are.
 */
HtmxElement *
clawt_web_decisions_body(ClawtWebApp *app, const gchar *agent_id)
{
    (void)agent_id;

    return decisions_body(app, FALSE);
}

static HtmxResponse *
on_decisions(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *all = htmx_request_get_query_param(request, "all");
    g_autofree gchar *html = NULL;
    g_autoptr(HtmxElement) body = NULL;

    (void)params;

    body = decisions_body(app, all != NULL && *all != '\0');
    html = clawt_web_page(app, NULL, CLAWT_PAGE_DECISIONS, body,
                          request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_answer(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "decision");
    const gchar *answer = clawt_web_form_value(request, "answer");

    (void)params;

    if (id != NULL && answer != NULL && *answer != '\0') {
        ClawtWebPayload *payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;

        clawt_web_payload_set(payload, "decision", id);
        clawt_web_payload_set(payload, "answer", answer);
        clawt_web_payload_set(payload, "via", CLAWT_DECISION_VIA_INBOX);
        reply = clawt_web_app_call(app, "decision.answer",
                                   clawt_web_payload_take(payload));
    }

    return clawt_web_redirect(request, "/decisions");
}

/*
 * The decision as the daemon now has it, found in its room's list -- or
 * in the whole inbox when the room is not known -- for redrawing a card
 * after a press the daemon refused.
 */
static JsonNode *
current_decision(ClawtWebApp *app, const gchar *id, JsonObject **found)
{
    ClawtWebPayload *query = clawt_web_payload_new();
    JsonNode *reply;
    JsonArray *items;
    guint i;

    *found = NULL;
    clawt_web_payload_set_bool(query, "open", FALSE);
    reply = clawt_web_app_call(app, "decision.list",
                               clawt_web_payload_take(query));
    items = clawt_web_member_array(clawt_web_root(reply), "decisions");

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *one = json_array_get_object_element(items, i);

        if (g_strcmp0(clawt_web_member(one, "id", NULL), id) == 0) {
            *found = one;
            break;
        }
    }

    return reply;
}

/*
 * A press on the inline view in the chat.
 *
 * Answers with `via: chat` and returns the card redrawn from what the
 * daemon now holds: settled by this press, or -- when the daemon refused
 * it because the inbox, another window or the clock settled it first,
 * or because it was a double tap's second press -- settled by whatever
 * did, with the refusal written into the card.  A card that went on
 * offering the buttons would invite the same refusal again.
 */
static HtmxResponse *
on_inline_answer(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "decision");
    const gchar *answer = clawt_web_form_value(request, "answer");
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) listing = NULL;
    g_autofree gchar *refusal = NULL;
    g_autoptr(HtmxElement) card = NULL;
    JsonObject *settled = NULL;

    (void)params;

    if (id == NULL || *id == '\0')
        return clawt_web_redirect(request, "/decisions");

    if (answer != NULL && *answer != '\0') {
        ClawtWebPayload *payload = clawt_web_payload_new();

        clawt_web_payload_set(payload, "decision", id);
        clawt_web_payload_set(payload, "answer", answer);
        clawt_web_payload_set(payload, "via", CLAWT_DECISION_VIA_CHAT);
        reply = clawt_web_app_call(app, "decision.answer",
                                   clawt_web_payload_take(payload));

        /* copied now: the next call frees the app's last error */
        if (reply == NULL)
            refusal = g_strdup(clawt_web_app_last_error(app));
    }

    /* Without htmx there is no card to swap: the inbox shows the result. */
    if (!htmx_request_is_htmx(request))
        return clawt_web_redirect(request, "/decisions");

    settled = clawt_web_member_object(clawt_web_root(reply), "decision");

    if (settled == NULL)
        listing = current_decision(app, id, &settled);

    /*
     * Gone from the store altogether: deleted while the card was on
     * screen.  Said in the card's place rather than leaving it blank.
     */
    if (settled == NULL)
        card = HTMX_ELEMENT(clawt_web_text(
            refusal != NULL ? refusal : "That decision no longer exists.",
            "error"));
    else
        card = clawt_web_decision_inline(settled, refusal);

    return clawt_web_fragment_response(card);
}

/*
 * Which decision a closing button means: the query string, which every
 * such button carries, else the form's hidden field.  Dismiss once read
 * a field called "id" that nothing sent -- the form names it "decision"
 * and the button had put the id in its confirm prompt instead -- so the
 * web client's "Does not need me" posted, redirected and did nothing.
 */
static const gchar *
decision_id(HtmxRequest *request)
{
    const gchar *id = htmx_request_get_query_param(request, "decision");

    if (id == NULL || *id == '\0')
        id = clawt_web_form_value(request, "decision");

    return (id != NULL && *id != '\0') ? id : NULL;
}

static HtmxResponse *
on_delete(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = decision_id(request);

    (void)params;

    if (id != NULL) {
        ClawtWebPayload *payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;

        clawt_web_payload_set(payload, "decision", id);
        reply = clawt_web_app_call(app, "decision.delete",
                                   clawt_web_payload_take(payload));
    }

    return clawt_web_redirect(request, "/decisions");
}

static HtmxResponse *
on_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = decision_id(request);

    (void)params;

    if (id != NULL) {
        ClawtWebPayload *payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;

        clawt_web_payload_set(payload, "decision", id);
        reply = clawt_web_app_call(app, "decision.dismiss",
                                   clawt_web_payload_take(payload));
    }

    return clawt_web_redirect(request, "/decisions");
}

/*
 * Approve, Reject or Edit a skill proposal.  A refusal -- a draft that
 * changed since it was proposed, text the library will not load -- is
 * shown, not swallowed: the proposal is still waiting, and somebody who
 * pressed Approve and saw the page reload would believe it had worked.
 */
static HtmxResponse *
on_proposal(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "proposal");
    const gchar *action = clawt_web_form_value(request, "action");
    const gchar *note = clawt_web_form_value(request, "note");
    g_autoptr(JsonNode) reply = NULL;
    ClawtWebPayload *payload;

    (void)params;

    if (id == NULL || action == NULL)
        return clawt_web_redirect(request, "/decisions");

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "proposal", id);
    clawt_web_payload_set(payload, "action", action);

    if (note != NULL && *note != '\0')
        clawt_web_payload_set(payload, "note", note);

    if (g_strcmp0(action, "edit") == 0) {
        clawt_web_payload_set(payload, "description",
                              clawt_web_form_value(request, "description"));
        clawt_web_payload_set(payload, "body",
                              clawt_web_form_value(request, "body"));
    }

    reply = clawt_web_app_call(app, "skill.proposal_resolve",
                               clawt_web_payload_take(payload));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_DECISIONS,
                                    message);
    }

    return clawt_web_redirect(request, "/decisions");
}

void
clawt_web_register_decisions(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/decisions", on_decisions, app);
    htmx_router_post(router, "/decisions/answer", on_answer, app);
    htmx_router_post(router, "/f/decision/answer", on_inline_answer, app);
    htmx_router_post(router, "/decisions/dismiss", on_dismiss, app);
    htmx_router_post(router, "/decisions/delete", on_delete, app);
    htmx_router_post(router, "/decisions/proposal", on_proposal, app);
}
