/*
 * web-browser.c - The Computer page's Browser view
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors the Browser view in clients/gtk/gtk-computer.c and
 * clients/lrg/lrg-computer.c: the line saying what the agent's browser is
 * on (clawt_browser_session_line()), a picture of it, an address to open,
 * and Stop.  A click on the picture is a click in the page.
 *
 * The picture is an <input type="image"> in a plain form: the browser
 * posts where it was clicked as x and y with no JavaScript at all, which
 * is the one way this page can take a pointer and still work on a
 * tailnet with nothing loaded.  It is drawn at its own size inside a
 * scrolling box, because a scaled picture would send scaled coordinates.
 */

#include "web-pages.h"
#include "web-ui.h"

/* The panel the poll swaps, with the session line and the picture. */
static HtmxElement *
build_browser_panel(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autoptr(JsonNode) listing = NULL;
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *poll = g_strdup_printf("/f/a/%s/browser", escaped);
    g_autofree gchar *line = NULL;
    JsonObject *session;
    gboolean running;

    htmx_element_add_class(HTMX_ELEMENT(panel), "browser-panel");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", poll);
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
                               "every 3s [!document.hidden]");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

    listing = clawt_web_app_call(app, "browser.list", NULL);
    session = clawt_browser_list_find(clawt_web_root(listing), agent_id);
    running = session != NULL &&
              json_object_get_boolean_member_with_default(session, "running", FALSE);
    line = clawt_browser_session_line(session);
    clawt_web_add(panel, clawt_web_text(line, "browser-line"));

    if (running) {
        g_autofree gchar *action = g_strdup_printf("/a/%s/browser/click", escaped);
        g_autofree gchar *src = g_strdup_printf("/a/%s/browser/frame?t=%" G_GINT64_FORMAT,
                                                escaped, g_get_real_time() / G_USEC_PER_SEC);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxDiv) frame = htmx_div_new();
        g_autoptr(HtmxElement) picture = HTMX_ELEMENT(htmx_input_new(HTMX_INPUT_SUBMIT));

        htmx_element_add_class(HTMX_ELEMENT(frame), "browser-frame");
        htmx_element_set_attribute(picture, "type", "image");
        htmx_element_set_attribute(picture, "src", src);
        htmx_element_set_attribute(picture, "alt", "The agent's browser");
        htmx_element_set_attribute(picture, "title", "Click to click in the page");
        htmx_node_add_child(HTMX_NODE(frame), HTMX_NODE(picture));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(frame));
        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(form));
    }

    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

void
clawt_web_add_browser(ClawtWebApp *app, HtmxElement *parent, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Browser",
        "The agent's own web browser: a real one, which runs the page's "
        "JavaScript. Watch it here, open a page, or click in it.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *open = g_strdup_printf("/a/%s/browser/open", escaped);
    g_autofree gchar *stop = g_strdup_printf("/a/%s/browser/stop", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(open);
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxButton) go = clawt_web_button("Open", "primary");

    clawt_web_add(form, clawt_web_field("Address", "url", NULL, "https://example.org"));
    htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
    htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    {
        g_autoptr(HtmxDiv) actions = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
        clawt_web_add(actions, clawt_web_post_button(
            "Stop the browser", stop, NULL,
            "The open page is lost; the profile and its logins stay. Continue?"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(actions));
    }

    clawt_web_add(body, build_browser_panel(app, agent_id));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

static HtmxResponse *
on_browser_fragment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) panel = build_browser_panel(app, agent_id);

    (void)request;
    return clawt_web_fragment_response(panel);
}

/* The picture's bytes; never cached, since the point is that it is current. */
static HtmxResponse *
on_browser_frame(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;
    const gchar *encoded;
    guchar *raw;
    gsize length = 0;

    (void)request;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "browser.frame",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    encoded = reply != NULL ? clawt_web_member(clawt_web_root(reply), "png_base64", NULL)
                            : NULL;
    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);
    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);
    htmx_response_set_content_type(response, "image/png");
    htmx_response_add_header(response, "Cache-Control", "no-store");
    return response;
}

/* Back to the Browser view, saying what happened. */
static HtmxResponse *
after(ClawtWebApp *app, HtmxRequest *request, const gchar *agent_id, JsonNode *reply,
      const gchar *done)
{
    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));
    {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *location = g_strdup_printf("/a/%s/computer/browser", escaped);

        (void)done;
        return clawt_web_redirect(request, location);
    }
}

static HtmxResponse *
on_browser_open(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *url = clawt_web_param(params, "url");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "url", url);
    reply = clawt_web_app_call(app, "browser.open",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    return after(app, request, agent_id, reply, "Opened.");
}

static HtmxResponse *
on_browser_click(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *x = clawt_web_param(params, "x");
    g_autofree gchar *y = clawt_web_param(params, "y");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set_int(payload, "x", x != NULL ? g_ascii_strtoll(x, NULL, 10) : -1);
    clawt_web_payload_set_int(payload, "y", y != NULL ? g_ascii_strtoll(y, NULL, 10) : -1);
    reply = clawt_web_app_call(app, "browser.click",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    return after(app, request, agent_id, reply, "Clicked.");
}

static HtmxResponse *
on_browser_stop(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "browser.stop",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    return after(app, request, agent_id, reply, "Stopped.");
}

void
clawt_web_register_browser(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/f/a/:id/browser", on_browser_fragment, app);
    htmx_router_get(router, "/a/:id/browser/frame", on_browser_frame, app);
    htmx_router_post(router, "/a/:id/browser/open", on_browser_open, app);
    htmx_router_post(router, "/a/:id/browser/click", on_browser_click, app);
    htmx_router_post(router, "/a/:id/browser/stop", on_browser_stop, app);
}
