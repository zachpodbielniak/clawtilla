/*
 * web-grok.c - The Grok page: the bots, sending them work, and what came back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-grok.c: whether a job could be sent right now
 * (the daemon's own answer), the bots with who may reach each and the
 * two things done to a bot once -- its routine prompt and its MCP key --
 * a composer, and every job with its state in libclawt's tone.  The jobs
 * sit in a panel that reloads itself, so a report arriving while the page
 * is open appears without anybody reloading.
 *
 * Everything a bot or an agent wrote is set as text content, never
 * markup: the typed elements escape it.
 */

#include "web-pages.h"

#include <string.h>

/* ── The jobs ────────────────────────────────────────────────────── */

static HtmxElement *
job_element(JsonObject *job, gint64 now)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    gint state = CLAWT_GROK_JOB_FAILED;
    g_autofree gchar *age = clawt_time_ago_label(
        clawt_web_member_int(job, "created_at", 0), now);
    g_autofree gchar *sub = NULL;
    const gchar *result = clawt_web_member(job, "result", NULL);
    const gchar *note = clawt_web_member(job, "note", NULL);
    const gchar *run = clawt_web_member(job, "run_uuid", NULL);

    /* The nick resolved to the enum, never compared as a spelling here. */
    clawt_enum_from_nick(CLAWT_TYPE_GROK_JOB_STATE,
                         clawt_web_member(job, "state", "failed"), &state);

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    clawt_web_add(head, clawt_web_badge(
        clawt_grok_job_state_label((ClawtGrokJobState)state),
        clawt_grok_job_state_tone((ClawtGrokJobState)state)));

    {
        g_autoptr(HtmxSpan) title = htmx_span_new();
        g_autofree gchar *text = g_strdup_printf(
            "%s \xc2\xb7 %s", clawt_web_member(job, "bot", "?"),
            clawt_web_member(job, "instructions", ""));

        htmx_element_add_class(HTMX_ELEMENT(title), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(title), text);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(title));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    sub = g_strdup_printf("%s for %s, %s%s%s",
                          clawt_web_member(job, "id", "?"),
                          clawt_web_member(job, "agent", "?"), age,
                          run != NULL ? " \xc2\xb7 Grok run " : "",
                          run != NULL ? run : "");
    clawt_web_add(row, clawt_web_text(sub, "list-item-sub"));

    {
        const gchar *body = result != NULL && *result != '\0' ? result : note;

        if (body != NULL && *body != '\0') {
            g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
            g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
            g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

            htmx_node_set_text_content(
                HTMX_NODE(summary),
                state == CLAWT_GROK_JOB_REFUSED ? "Why"
                : result != NULL ? "What it reported" : "Latest note");
            /* The bot's own words, as text: it wrote them outside the fleet. */
            htmx_node_set_text_content(HTMX_NODE(pre), body);
            htmx_element_add_class(pre, "grok-report");
            htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
            htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(pre));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(details));
        }
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

/* The part that reloads itself: the jobs. */
static HtmxElement *
jobs_panel(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "grok.jobs", NULL);
    JsonArray *jobs = clawt_web_member_array(clawt_web_root(reply), "jobs");
    gint64 now = g_get_real_time();
    guint i;

    htmx_element_set_id(HTMX_ELEMENT(panel), "grok-jobs-panel");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", "/f/grok/jobs");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
                               "every 15s [!document.hidden]");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

    if (reply == NULL) {
        clawt_web_add(panel, clawt_web_empty(
            "The Grok jobs are unavailable",
            "Check the daemon connection, then reopen Grok to retry."));
        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    if (jobs == NULL || json_array_get_length(jobs) == 0) {
        clawt_web_add(panel, clawt_web_empty(
            "No work has been handed to a Grok bot",
            "Agents send it with clawtilla_grok_dispatch; you can send it "
            "from here."));
        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(jobs); i++)
            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(
                job_element(json_array_get_object_element(jobs, i), now)));

        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(list));
    }

    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

/* ── The bots ────────────────────────────────────────────────────── */

static HtmxElement *
bot_element(JsonObject *bot, const gchar *here)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    g_autoptr(HtmxDiv) actions = htmx_div_new();
    const gchar *name = clawt_web_member(bot, "name", "?");
    const gchar *description = clawt_web_member(bot, "description", "");
    JsonArray *agents = clawt_web_member_array(bot, "agents");
    g_autoptr(GString) who = g_string_new("Reachable by ");
    g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
    g_autofree gchar *prompt_url = g_strdup_printf("/f/grok/%s/prompt",
                                                   escaped);
    g_autofree gchar *escaped_here = g_uri_escape_string(
        here != NULL ? here : "", NULL, FALSE);
    g_autofree gchar *mint_url = g_strdup_printf("/grok/%s/mcp-key?here=%s",
                                                 escaped, escaped_here);
    g_autofree gchar *confirm = g_strdup_printf(
        "Mint an MCP key for %s? It is shown once; if the bot already has "
        "one, that one stops working at once.", name);
    guint a;

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    {
        g_autoptr(HtmxSpan) title = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(title), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(title), name);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(title));
    }

    if (!clawt_web_member_bool(bot, "key_configured", FALSE))
        clawt_web_add(head, clawt_web_badge("no key", "bad"));

    clawt_web_add(head, clawt_web_badge(
        clawt_web_member_bool(bot, "mcp_key", FALSE) ? "MCP" : "HTTP only",
        clawt_web_member_bool(bot, "mcp_key", FALSE) ? "good" : "neutral"));
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    if (*description != '\0')
        clawt_web_add(row, clawt_web_text(description, "list-item-sub"));

    for (a = 0; agents != NULL && a < json_array_get_length(agents); a++) {
        if (a > 0)
            g_string_append(who, ", ");
        g_string_append(who, json_array_get_string_element(agents, a));
    }

    clawt_web_add(row, clawt_web_text(who->str, "list-item-sub"));

    htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");

    {
        g_autoptr(HtmxButton) prompt = clawt_web_button("Routine prompt",
                                                        "default");

        htmx_element_set_attribute(HTMX_ELEMENT(prompt), "type", "button");
        htmx_element_set_attribute(HTMX_ELEMENT(prompt), "hx-get",
                                   prompt_url);
        htmx_element_set_attribute(HTMX_ELEMENT(prompt), "hx-target",
                                   "#grok-prompt");
        htmx_element_set_attribute(HTMX_ELEMENT(prompt), "hx-swap",
                                   "innerHTML");
        htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(prompt));
    }

    clawt_web_add(actions, clawt_web_post_button("Mint MCP key", mint_url,
                                                 "default", confirm));
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static HtmxElement *
composer(JsonArray *bots, const gchar *here)
{
    g_autoptr(HtmxForm) form = clawt_web_form("/grok/send");
    g_autoptr(GPtrArray) bot_names = g_ptr_array_new();
    g_autoptr(GPtrArray) agent_ids = g_ptr_array_new();
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    guint i;

    for (i = 0; bots != NULL && i < json_array_get_length(bots); i++) {
        JsonObject *bot = json_array_get_object_element(bots, i);
        JsonArray *agents = clawt_web_member_array(bot, "agents");
        guint a;

        g_ptr_array_add(bot_names,
                        (gpointer)clawt_web_member(bot, "name", "?"));

        for (a = 0; agents != NULL && a < json_array_get_length(agents);
             a++) {
            const gchar *id = json_array_get_string_element(agents, a);

            if (g_hash_table_contains(seen, id))
                continue;

            g_hash_table_add(seen, (gpointer)id);
            g_ptr_array_add(agent_ids, (gpointer)id);
        }
    }

    g_ptr_array_add(bot_names, NULL);
    g_ptr_array_add(agent_ids, NULL);

    if (here != NULL)
        clawt_web_add(form, clawt_web_hidden("here", here));

    clawt_web_add(form, clawt_web_select_field(
        "Bot", "bot", (const gchar *const *)bot_names->pdata, NULL, NULL));
    clawt_web_add(form, clawt_web_select_field(
        "On behalf of", "agent", (const gchar *const *)agent_ids->pdata,
        NULL, here));
    clawt_web_add(form, clawt_web_textarea_field(
        "What the bot should do", "instructions", NULL, 5));
    clawt_web_add(form, clawt_web_field(
        "Context", "context", NULL,
        "Optional. Sent as data, separate from the instructions."));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) send = clawt_web_button("Send to the bot",
                                                      "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(send));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    return HTMX_ELEMENT(g_steal_pointer(&form));
}

HtmxElement *
clawt_web_grok_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "grok.bots", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *bots = clawt_web_member_array(root, "bots");
    guint i;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Grok"));

    if (reply == NULL) {
        clawt_web_add(pad, clawt_web_empty(
            "The Grok bots are unavailable",
            "Check the daemon connection, then reopen Grok to retry."));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));
        return g_steal_pointer(&view);
    }

    if (clawt_web_member_bool(root, "reachable", FALSE)) {
        g_autofree gchar *where = g_strdup_printf(
            "Bots report back to %s over the tailnet.",
            clawt_web_member(root, "callback_host", "?"));

        clawt_web_add(pad, clawt_web_text(where, "lede"));
    } else {
        g_autofree gchar *why = g_strdup_printf(
            "No job can be sent right now: %s.",
            clawt_web_member(root, "unreachable_reason", "unknown"));

        clawt_web_add(pad, clawt_web_text(why, "notice notice-bad"));
    }

    clawt_web_add(pad, clawt_web_section_title("Bots"));

    if (bots == NULL || json_array_get_length(bots) == 0) {
        clawt_web_add(pad, clawt_web_empty(
            "No Grok bot is shared with any agent",
            "Add a grokbot integration in Settings, named for the bot, "
            "with its routine's webhook address and key."));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(bots); i++)
            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(
                bot_element(json_array_get_object_element(bots, i),
                            agent_id)));

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(list));

        {
            g_autoptr(HtmxDiv) prompt = htmx_div_new();

            htmx_element_set_id(HTMX_ELEMENT(prompt), "grok-prompt");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(prompt));
        }

        clawt_web_add(pad, clawt_web_section_title("Hand a bot some work"));
        htmx_node_add_child(HTMX_NODE(pad),
                            HTMX_NODE(composer(bots, agent_id)));
    }

    clawt_web_add(pad, clawt_web_section_title("Jobs"));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(jobs_panel(app)));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_jobs_fragment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autoptr(HtmxElement) panel = jobs_panel(user_data);

    (void)request;
    (void)params;

    return clawt_web_fragment_response(panel);
}

static HtmxResponse *
on_prompt_fragment(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *bot = clawt_web_param(params, "bot");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) box = htmx_div_new();

    (void)request;

    clawt_web_payload_set(payload, "bot", bot);
    reply = clawt_web_app_call(app, "grok.routine_prompt",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        clawt_web_add(box, clawt_web_text(why, "notice notice-bad"));
    } else {
        g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

        clawt_web_add(box, clawt_web_text(
            "Paste this into the bot's routine in Grok, once. Every job "
            "also carries its own reporting instructions.", "lede"));
        htmx_node_set_text_content(
            HTMX_NODE(pre),
            clawt_web_member(clawt_web_root(reply), "text", ""));
        htmx_element_add_class(pre, "grok-report");
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(pre));
    }

    return clawt_web_fragment_response(HTMX_ELEMENT(box));
}

static HtmxResponse *
on_send(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *here = clawt_web_form_value(request, "here");
    const gchar *agent = clawt_web_form_value(request, "agent");
    const gchar *context = clawt_web_form_value(request, "context");
    g_autofree gchar *done = NULL;

    (void)params;

    clawt_web_payload_set(payload, "bot",
                          clawt_web_form_value(request, "bot"));
    clawt_web_payload_set(payload, "agent", agent);
    clawt_web_payload_set(payload, "instructions",
                          clawt_web_form_value(request, "instructions"));

    if (context != NULL && *context != '\0')
        clawt_web_payload_set(payload, "context", context);

    reply = clawt_web_app_call(app, "grok.send",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, here, CLAWT_PAGE_GROK, why);
    }

    {
        JsonObject *job = clawt_web_member_object(clawt_web_root(reply),
                                                  "job");

        done = g_strdup_printf("Sent to %s as %s; the answer goes to %s.",
                               clawt_web_member(job, "bot", "?"),
                               clawt_web_member(job, "id", "?"),
                               agent != NULL ? agent : "?");
    }

    return clawt_web_after_action(app, request, here, CLAWT_PAGE_GROK, done);
}

static HtmxResponse *
on_mint(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *bot = clawt_web_param(params, "bot");
    const gchar *here = htmx_request_get_query_param(request, "here");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *told = NULL;
    JsonObject *root;

    if (here != NULL && *here == '\0')
        here = NULL;

    clawt_web_payload_set(payload, "bot", bot);
    reply = clawt_web_app_call(app, "grok.mcp_key",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, here, CLAWT_PAGE_GROK, why);
    }

    root = clawt_web_root(reply);

    /*
     * The one time the key is rendered, in the banner somebody is already
     * looking at -- as the trigger secret is, for the same reason: a card
     * would be redrawn on the next refresh with nothing to put in it.
     */
    told = g_strdup_printf(
        "In Grok, ask %s to add a remote MCP connection. URL %s, API key "
        "%s.%s", bot,
        clawt_web_member(root, "mcp_url", "(not reachable yet)"),
        clawt_web_member(root, "key", "?"),
        clawt_web_member_bool(root, "secret_shown_once", FALSE)
            ? " This is the only time the key is shown; minting another "
              "replaces it."
            : "");

    return clawt_web_after_action(app, request, here, CLAWT_PAGE_GROK, told);
}

void
clawt_web_register_grok(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/f/grok/jobs", on_jobs_fragment, app);
    htmx_router_get(router, "/f/grok/:bot/prompt", on_prompt_fragment, app);
    htmx_router_post(router, "/grok/send", on_send, app);
    htmx_router_post(router, "/grok/:bot/mcp-key", on_mint, app);
}

