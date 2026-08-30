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

static HtmxElement *
decision_row(ClawtWebApp *app, JsonObject *decision, gint64 now)
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
            g_autoptr(HtmxButton) drop = clawt_web_post_button(
                "Does not need me", "/decisions/dismiss", "default", id);

            htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
            htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(send));
            htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(drop));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(actions));
        }

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static HtmxElement *
decisions_body(ClawtWebApp *app, gboolean all)
{
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(JsonNode) reply = NULL;
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

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *decision = json_array_get_object_element(items, i);

        clawt_web_add(view, decision_row(app, decision, now));
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
        reply = clawt_web_app_call(app, "decision.answer",
                                   clawt_web_payload_take(payload));
    }

    return clawt_web_redirect(request, "/decisions");
}

static HtmxResponse *
on_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "id");

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

void
clawt_web_register_decisions(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/decisions", on_decisions, app);
    htmx_router_post(router, "/decisions/answer", on_answer, app);
    htmx_router_post(router, "/decisions/dismiss", on_dismiss, app);
}
