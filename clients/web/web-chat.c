/*
 * web-chat.c - The transcript and the composer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Markdown is rendered through clawt_markdown_to_pango()'s sibling for
 * HTML -- which is to say it is not rendered at all here, and every
 * message body is set as *text*.  The rule the GTK client keeps is that
 * model output never reaches a markup parser; the web version of that
 * rule is that it never reaches the page as HTML.  A reply containing
 * "<script>" is a thing an agent can be talked into writing, and this
 * client serves it to a browser.
 */

#include "web-pages.h"

#include <string.h>

/* ── The transcript ──────────────────────────────────────────────── */

static HtmxElement *
message_element(JsonObject *message, const gchar *agent_id)
{
    const gchar *sender = clawt_web_member(message, "sender", "?");
    const gchar *body = clawt_web_member(message, "body", "");
    const gchar *task = clawt_web_member(message, "task", NULL);
    gint64 ts = clawt_web_member_int(message, "ts", 0);
    gint64 depth = clawt_web_member_int(message, "depth", 0);
    g_autoptr(HtmxElement) row = HTMX_ELEMENT(htmx_article_new());
    g_autoptr(HtmxDiv) who = htmx_div_new();
    g_autoptr(HtmxDiv) text = htmx_div_new();
    g_autofree gchar *when = clawt_web_relative_time(ts);

    (void)agent_id;

    htmx_element_add_class(row, "msg");

    if (g_strcmp0(sender, "user") == 0)
        htmx_element_add_class(row, "msg-self");

    htmx_element_add_class(HTMX_ELEMENT(who), "msg-who");

    {
        g_autoptr(HtmxSpan) name = htmx_span_new();

        htmx_node_set_text_content(HTMX_NODE(name), sender);
        htmx_node_add_child(HTMX_NODE(who), HTMX_NODE(name));
    }

    if (*when != '\0') {
        g_autoptr(HtmxSpan) stamp = htmx_span_new();
        g_autofree gchar *dotted = g_strdup_printf(" · %s", when);

        htmx_element_add_class(HTMX_ELEMENT(stamp), "muted");
        htmx_node_set_text_content(HTMX_NODE(stamp), dotted);
        htmx_node_add_child(HTMX_NODE(who), HTMX_NODE(stamp));
    }

    /*
     * The task and the hop depth, which are what turn a transcript into
     * something you can follow.  A delegated reply is otherwise just
     * another line from an agent with no sign of what asked for it, and a
     * hop count climbing towards max_hops is the only visible difference
     * between a conversation and a loop.
     */
    if (task != NULL)
        clawt_web_add(who, clawt_web_badge(task, "info"));

    if (depth > 1) {
        g_autofree gchar *hops =
            g_strdup_printf("%" G_GINT64_FORMAT " hops", depth);

        clawt_web_add(who, clawt_web_badge(hops, "warn"));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(who));

    htmx_element_add_class(HTMX_ELEMENT(text), "msg-body");
    htmx_node_set_text_content(HTMX_NODE(text), body);
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(text));

    return g_steal_pointer(&row);
}

static HtmxElement *
transcript(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) scroll = htmx_div_new();
    g_autoptr(HtmxDiv) inner = htmx_div_new();
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(scroll), "transcript");
    htmx_element_set_id(HTMX_ELEMENT(scroll), "transcript");

    /*
     * Re-fetched on any fleet event rather than polled. A reply can take
     * minutes, and a poll short enough to feel live is a request every
     * second or two for the whole time nothing is happening.
     */
    {
        g_autofree gchar *url = NULL;
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);

        url = g_strdup_printf("/f/a/%s/transcript", escaped);
        htmx_element_set_attribute(HTMX_ELEMENT(scroll), "hx-get", url);
        /*
         * The umbrella event, not the individual ones. The daemon's kinds
         * are dotted -- `mailbox.queued`, `agent.changed` -- and a dot in
         * an hx-trigger is a class selector, so naming them here works
         * for exactly the undotted ones. Listening for "fleet" also means
         * a kind the daemon grows later arrives without an edit; naming
         * them one by one is how a client quietly stops updating for
         * whichever was added last.
         */
        htmx_element_set_attribute(HTMX_ELEMENT(scroll), "hx-trigger",
                                   "sse:fleet");
        htmx_element_set_attribute(HTMX_ELEMENT(scroll), "hx-swap",
                                   "outerHTML");
    }

    htmx_element_add_class(HTMX_ELEMENT(inner), "transcript-inner");

    clawt_web_payload_set(payload, "room", agent_id);
    clawt_web_payload_set(payload, "as", "user");
    clawt_web_payload_set_int(payload, "limit", 200);

    reply = clawt_web_app_call(app, "room.history",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    messages = clawt_web_member_array(clawt_web_root(reply), "messages");

    if (messages == NULL || json_array_get_length(messages) == 0) {
        clawt_web_add(inner,
                      clawt_web_empty("Nothing said yet",
                                      "Send something below. A stopped agent "
                                      "still receives it -- the message waits "
                                      "in its mailbox until it starts."));
    }

    for (i = 0; messages != NULL && i < json_array_get_length(messages); i++) {
        clawt_web_add(inner,
                      message_element(
                          json_array_get_object_element(messages, i),
                          agent_id));
    }

    /*
     * An anchor at the end plus a scroll on load, because a transcript
     * that opens at the top shows the oldest message -- which for a long
     * conversation is the one line nobody wants.
     */
    {
        g_autoptr(HtmxDiv) anchor = htmx_div_new();

        htmx_element_set_id(HTMX_ELEMENT(anchor), "transcript-end");
        htmx_node_add_child(HTMX_NODE(inner), HTMX_NODE(anchor));
    }

    htmx_node_add_child(HTMX_NODE(scroll), HTMX_NODE(inner));

    return HTMX_ELEMENT(g_steal_pointer(&scroll));
}

/* ── The composer ────────────────────────────────────────────────── */

static HtmxElement *
composer(const gchar *agent_id)
{
    g_autoptr(HtmxElement) foot = HTMX_ELEMENT(htmx_footer_new());
    g_autoptr(HtmxDiv) inner = htmx_div_new();
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/send", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    g_autoptr(HtmxTextarea) area = htmx_textarea_new_with_name("body");

    htmx_element_add_class(foot, "composer");
    htmx_element_add_class(HTMX_ELEMENT(inner), "composer-inner");

    htmx_element_set_attribute(HTMX_ELEMENT(area), "rows", "1");
    htmx_element_set_attribute(HTMX_ELEMENT(area), "placeholder",
                               "Message, or /help for commands");
    htmx_element_set_attribute(HTMX_ELEMENT(area), "autofocus", "autofocus");
    htmx_node_add_child(HTMX_NODE(inner), HTMX_NODE(area));

    {
        g_autoptr(HtmxButton) send = clawt_web_button("Send", "primary");

        /*
         * Explicitly a submit. htmx_button_new_with_label() makes a
         * type="button", which does nothing inside a form -- so the
         * composer looked complete and Enter did nothing.
         */
        htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
        htmx_node_add_child(HTMX_NODE(inner), HTMX_NODE(send));
    }

    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(inner));

    /*
     * Cleared after a successful send, and only then. htmx resets a form
     * on any 2xx, so a refusal that came back as a rendered page would
     * also take the text with it -- and a message lost because the daemon
     * said no is worse than the refusal.
     */
    htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-on::after-request",
                               "if(event.detail.successful)this.reset()");

    htmx_element_add_class(HTMX_ELEMENT(form), "composer-form");
    htmx_node_add_child(HTMX_NODE(foot), HTMX_NODE(form));

    /*
     * A separate form, because it posts multipart and the message form
     * does not. Sharing one would mean encoding every message as a file
     * upload for the sake of the attachment nobody added.
     */
    {
        g_autofree gchar *attach_action =
            g_strdup_printf("/a/%s/attach", escaped);
        g_autoptr(HtmxForm) attach = clawt_web_form(attach_action);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxInput) picker = htmx_input_new(HTMX_INPUT_FILE);
        g_autoptr(HtmxButton) send = clawt_web_button("Attach", "default");

        htmx_element_set_attribute(HTMX_ELEMENT(attach), "enctype",
                                   "multipart/form-data");
        htmx_element_set_attribute(HTMX_ELEMENT(attach), "hx-encoding",
                                   "multipart/form-data");

        htmx_input_set_name(picker, "file");
        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(picker));

        htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(send));
        htmx_node_add_child(HTMX_NODE(attach), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(foot), HTMX_NODE(attach));
    }

    return g_steal_pointer(&foot);
}

/* ── The view ────────────────────────────────────────────────────── */

HtmxElement *
clawt_web_chat_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) main_el = HTMX_ELEMENT(htmx_main_new());

    htmx_element_add_class(main_el, "chat");

    if (agent_id == NULL) {
        clawt_web_add(main_el,
                      clawt_web_empty("No agent selected",
                                      "Pick one from the sidebar."));

        return g_steal_pointer(&main_el);
    }

    clawt_web_add(main_el, transcript(app, agent_id));
    clawt_web_add(main_el, composer(agent_id));

    return g_steal_pointer(&main_el);
}

/* ── Slash commands ──────────────────────────────────────────────── */

/*
 * The commands the GTK composer answers locally.
 *
 * They are handled here rather than sent to the agent for the same
 * reason they are there: /reset and /stop are things to do *to* an
 * agent, and typing one into a chat that forwarded it would ask the
 * agent to reset itself, which it cannot do.
 */
static const struct {
    const gchar *name;
    const gchar *kind;
    const gchar *summary;
    const gchar *done;
} commands[] = {
    { "/start",   "agent.start",   "start this agent",            "Starting." },
    { "/stop",    "agent.stop",    "stop it",                     "Stopped." },
    { "/restart", "agent.restart", "stop it and start it again",  "Restarting." },
    { "/reset",   "agent.reset",   "clear its AI session and start the "
                                   "conversation over",           "Session cleared." }
};

static HtmxResponse *
run_command(ClawtWebApp *app, HtmxRequest *request, const gchar *agent_id,
            const gchar *text)
{
    guint i;

    if (g_str_has_prefix(text, "/help")) {
        g_autoptr(GString) help = g_string_new("Commands: ");

        for (i = 0; i < G_N_ELEMENTS(commands); i++)
            g_string_append_printf(help, "%s%s (%s)",
                                   i > 0 ? ", " : "",
                                   commands[i].name, commands[i].summary);

        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, help->str);
    }

    for (i = 0; i < G_N_ELEMENTS(commands); i++) {
        g_autoptr(ClawtWebPayload) payload = NULL;
        g_autoptr(JsonNode) reply = NULL;

        if (g_strcmp0(text, commands[i].name) != 0)
            continue;

        payload = clawt_web_payload_new();
        clawt_web_payload_set(payload, "agent", agent_id);

        reply = clawt_web_app_call(app, commands[i].kind,
                                   clawt_web_payload_take(
                                       g_steal_pointer(&payload)));

        if (reply == NULL)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_WEB_VIEW_CHAT,
                                        clawt_web_app_last_error(app));

        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, commands[i].done);
    }

    {
        g_autofree gchar *unknown =
            g_strdup_printf("No such command: %s. Try /help.", text);

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT, unknown);
    }
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_send(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *body = clawt_web_form_value(request, "body");
    g_autofree gchar *trimmed = NULL;
    g_autoptr(ClawtWebPayload) payload = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (body == NULL)
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, NULL);

    trimmed = g_strdup(body);
    g_strstrip(trimmed);

    if (*trimmed == '\0')
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, NULL);

    if (trimmed[0] == '/')
        return run_command(app, request, agent_id, trimmed);

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "target", agent_id);
    clawt_web_payload_set(payload, "body", trimmed);
    clawt_web_payload_set(payload, "from", "user");

    reply = clawt_web_app_call(app, "msg.send",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, NULL);
}

static HtmxResponse *
on_transcript_fragment(HtmxRequest *request, GHashTable *params,
                       gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) fragment = NULL;

    (void)request;

    fragment = transcript(app, agent_id);

    return clawt_web_fragment_response(fragment);
}

void
clawt_web_register_chat(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/send", on_send, app);
    htmx_router_get(router, "/f/a/:id/transcript", on_transcript_fragment, app);
}
