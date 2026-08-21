/*
 * main.c - clawtilla-web, the HTMX web client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A ClawtClient talking to the daemon over the same socket protocol every
 * other client uses, rendered server-side with htmx-glib.  This is the one
 * surface that is deliberately incomplete: agent list, transcript and send
 * work; everything else is marked TODO rather than faked.
 */

#include <clawtilla.h>
#include <htmx-glib.h>

#include <stdlib.h>
#include <string.h>

static gint   opt_port = 8790;
static gchar *opt_socket = NULL;
static gboolean opt_version = FALSE;

static GOptionEntry entries[] = {
    {
        "port", 'p', 0, G_OPTION_ARG_INT, &opt_port,
        "Port to listen on (default: 8790)", "PORT"
    },
    {
        "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket,
        "Path to the clawtilla daemon socket", "PATH"
    },
    {
        "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
        "Print version information and exit", NULL
    },
    { NULL }
};

static const gchar *description_text =
    "\n"
    "Examples:\n"
    "  # Serve the web client on the default port\n"
    "  clawtilla-web\n"
    "\n"
    "  # Serve on another port against an explicit daemon socket\n"
    "  clawtilla-web --port 9000 --socket /run/user/1000/clawtilla/daemon.sock\n";


/* ── Rendering ───────────────────────────────────────────────────── */

/*
 * Everything the daemon returns is escaped before it reaches the page.
 *
 * Agent names, descriptions and message bodies all come from somewhere a
 * person or a model wrote.  This client serves them back over HTTP, so an
 * unescaped "<" is not a cosmetic problem -- it is script injection into
 * whoever opens the page.
 */
static void
append_escaped(GString *out, const gchar *text)
{
    g_autofree gchar *escaped = NULL;

    if (text == NULL)
        return;

    escaped = g_markup_escape_text(text, -1);
    g_string_append(out, escaped);
}

static const gchar *page_style =
    "<style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#1b1b1b;"
    "color:#eee;display:flex;height:100vh}"
    "aside{width:16rem;border-right:1px solid #333;overflow-y:auto}"
    "main{flex:1;display:flex;flex-direction:column}"
    "h1{font-size:1rem;padding:1rem;margin:0;border-bottom:1px solid #333}"
    "a{display:block;padding:.6rem 1rem;color:#eee;text-decoration:none;"
    "border-bottom:1px solid #262626}"
    "a:hover{background:#262626}"
    ".state{font-size:.75rem;color:#999}"
    ".queue{color:#7aa2f7}"
    ".host{color:#f7768e;font-weight:600}"
    "#log{flex:1;overflow-y:auto;padding:1rem}"
    ".msg{margin-bottom:.8rem}"
    ".who{font-size:.75rem;color:#999}"
    "form{display:flex;gap:.5rem;padding:1rem;border-top:1px solid #333}"
    "input[type=text]{flex:1;padding:.5rem;background:#262626;color:#eee;"
    "border:1px solid #333;border-radius:4px}"
    "button{padding:.5rem 1rem;background:#7aa2f7;color:#1b1b1b;border:0;"
    "border-radius:4px;cursor:pointer}"
    ".todo{padding:1rem;color:#999;font-size:.85rem;border-top:1px solid #333}"
    "</style>";

static const gchar *
member_or(JsonObject *object, const gchar *key, const gchar *fallback)
{
    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_STRING)
        return fallback;

    return json_object_get_string_member(object, key);
}

static JsonNode *
call(ClawtClient *client, const gchar *kind, JsonNode *payload)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    reply = clawt_client_request(client, kind, payload, &error);

    if (reply == NULL)
        g_warning("clawtilla-web: %s: %s", kind, error->message);

    return reply;
}

static void
render_sidebar(GString *out, ClawtClient *client, const gchar *selected)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    guint i;

    g_string_append(out, "<aside><h1>Agents</h1>");

    reply = call(client, "agent.list", NULL);

    if (reply == NULL) {
        g_string_append(out,
                        "<p class=\"todo\">The daemon is not answering.</p>"
                        "</aside>");
        return;
    }

    agents = json_object_get_array_member(json_node_get_object(reply),
                                          "agents");

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = member_or(agent, "id", "?");
        gint64 depth = json_object_get_int_member(agent, "mailbox_depth");

        g_string_append(out, "<a href=\"/?agent=");
        append_escaped(out, id);
        g_string_append(out, "\"");

        if (g_strcmp0(id, selected) == 0)
            g_string_append(out, " style=\"background:#262626\"");

        g_string_append_c(out, '>');
        append_escaped(out, member_or(agent, "name", id));
        g_string_append(out, "<br><span class=\"state\">");
        append_escaped(out, member_or(agent, "state", "?"));

        if (depth > 0)
            g_string_append_printf(out,
                                   " <span class=\"queue\">%"
                                   G_GINT64_FORMAT " waiting</span>", depth);

        if (strstr(member_or(agent, "caps", ""), "host-control") != NULL)
            g_string_append(out, " <span class=\"host\">HOST</span>");

        g_string_append(out, "</span></a>");
    }

    g_string_append(out, "</aside>");
}

static void
render_transcript(GString *out, ClawtClient *client, const gchar *agent_id)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    g_string_append(out, "<div id=\"log\">");

    if (agent_id == NULL) {
        g_string_append(out, "<p class=\"todo\">Pick an agent.</p></div>");
        return;
    }

    {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, agent_id);
        json_builder_end_object(builder);

        reply = call(client, "room.history", json_builder_get_root(builder));
    }

    if (reply == NULL) {
        g_string_append(out, "<p class=\"todo\">No transcript.</p></div>");
        return;
    }

    messages = json_object_get_array_member(json_node_get_object(reply),
                                            "messages");

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);

        g_string_append(out, "<div class=\"msg\"><div class=\"who\">");
        append_escaped(out, member_or(message, "sender", "?"));
        g_string_append(out, "</div><div>");
        append_escaped(out, member_or(message, "body", ""));
        g_string_append(out, "</div></div>");
    }

    g_string_append(out, "</div>");
}

static HtmxResponse *
on_index(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtClient *client = user_data;

    (void)params;
    g_autoptr(GString) out = g_string_new(NULL);
    const gchar *agent_id = htmx_request_get_query_param(request, "agent");

    g_string_append(out, "<!doctype html><html><head>"
                         "<meta charset=\"utf-8\">"
                         "<title>clawtilla</title>");
    g_string_append(out, page_style);
    g_string_append(out, "</head><body>");

    render_sidebar(out, client, agent_id);

    g_string_append(out, "<main>");
    render_transcript(out, client, agent_id);

    if (agent_id != NULL) {
        g_string_append(out, "<form method=\"post\" action=\"/send\">"
                             "<input type=\"hidden\" name=\"agent\" value=\"");
        append_escaped(out, agent_id);
        g_string_append(out, "\">"
                             "<input type=\"text\" name=\"body\" "
                             "placeholder=\"Message\" autofocus>"
                             "<button type=\"submit\">Send</button></form>");
    }

    /*
     * The unfinished parts are named rather than hidden.  A web client
     * that silently lacks the mailbox and the computer panel would look
     * like the daemon lacks them.
     */
    g_string_append(out,
        "<p class=\"todo\">This is the minimal web client: agent list, "
        "transcript and send. Mailboxes, tasks, the computer console, "
        "agent creation and live streaming are TODO -- use "
        "<code>clawtilla-gtk</code> or the <code>clawtilla</code> CLI for "
        "those.</p>");

    g_string_append(out, "</main></body></html>");

    {
        HtmxResponse *response = htmx_response_new_with_content(out->str);

        htmx_response_set_content_type(response, "text/html; charset=utf-8");

        return response;
    }
}

static HtmxResponse *
on_send(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtClient *client = user_data;

    (void)params;
    const gchar *agent_id = htmx_request_get_form_value(request, "agent");
    const gchar *body = htmx_request_get_form_value(request, "body");
    g_autoptr(JsonNode) reply = NULL;
    HtmxResponse *response;

    if (agent_id != NULL && body != NULL && *body != '\0') {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "target");
        json_builder_add_string_value(builder, agent_id);
        json_builder_set_member_name(builder, "body");
        json_builder_add_string_value(builder, body);
        json_builder_set_member_name(builder, "from");
        json_builder_add_string_value(builder, "user");
        json_builder_end_object(builder);

        reply = call(client, "msg.send", json_builder_get_root(builder));
    }

    /*
     * Redirected rather than rendered in place, so a refresh does not
     * send the message a second time.
     */
    response = htmx_response_new();
    htmx_response_set_status(response, 303);

    {
        /*
         * Percent-encoded, like everything else this client emits.  The
         * value came from a form post, and splicing it raw into a header
         * lets any reserved character -- or a control character -- take
         * the redirect somewhere the user did not ask to go.
         */
        g_autofree gchar *escaped =
            g_uri_escape_string(agent_id != NULL ? agent_id : "", NULL,
                                FALSE);
        g_autofree gchar *location = g_strdup_printf("/?agent=%s", escaped);

        htmx_response_add_header(response, "Location", location);
    }

    return response;
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(HtmxServer) server = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    HtmxRouter *router;

    context = g_option_context_new("- the clawtilla web client");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("clawtilla-web %d.%d.%d (%s)\n",
                CLAWT_VERSION_MAJOR, CLAWT_VERSION_MINOR, CLAWT_VERSION_MICRO,
                CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }

    client = clawt_client_new(opt_socket);
    clawt_client_set_auto_reconnect(client, TRUE);

    if (!clawt_client_connect(client, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);
        return EXIT_FAILURE;
    }

    server = htmx_server_new_with_port((guint16)opt_port);
    router = htmx_server_get_router(server);

    htmx_router_get(router, "/", on_index, client);
    htmx_router_post(router, "/send", on_send, client);

    if (!htmx_server_start(server, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);
        return EXIT_FAILURE;
    }

    /*
     * Bound to localhost by htmx-glib's default and left there.  This
     * client has no authentication of its own -- anything that can reach
     * the port can drive the whole fleet -- so exposing it is a decision
     * for a reverse proxy that can require a login, not a default.
     */
    g_print("clawtilla-web listening on http://127.0.0.1:%d\n", opt_port);
    g_print("No authentication: do not expose this port.\n");

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    htmx_server_stop(server);

    return EXIT_SUCCESS;
}
