/*
 * web-ui.c - The page shell and the pieces every view is built from
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "web-ui.h"
#include "web-pages.h"

#include <string.h>

const gchar *clawt_web_stylesheet(void);

/* ── Views ───────────────────────────────────────────────────────── */

static const struct {
    const gchar *slug;
    const gchar *title;
} views[CLAWT_WEB_N_VIEWS] = {
    { "chat",     "Chat" },
    { "agent",    "Agent" },
    { "mailbox",  "Mailbox" },
    { "computer", "Computer" },
    { "routines", "Routines" },
    { "tasks",    "Tasks" },
    { "flow",     "Flow" }
};

const gchar *
clawt_web_view_slug(ClawtWebView view)
{
    if (view >= CLAWT_WEB_N_VIEWS)
        return views[CLAWT_WEB_VIEW_CHAT].slug;

    return views[view].slug;
}

const gchar *
clawt_web_view_title(ClawtWebView view)
{
    if (view >= CLAWT_WEB_N_VIEWS)
        return views[CLAWT_WEB_VIEW_CHAT].title;

    return views[view].title;
}

ClawtWebView
clawt_web_view_from_slug(const gchar *slug)
{
    guint i;

    if (slug != NULL) {
        for (i = 0; i < CLAWT_WEB_N_VIEWS; i++) {
            if (g_strcmp0(slug, views[i].slug) == 0)
                return (ClawtWebView)i;
        }
    }

    return CLAWT_WEB_VIEW_CHAT;
}

/* ── Small helpers ───────────────────────────────────────────────── */

void
clawt_web_add(gpointer parent, gpointer child)
{
    g_return_if_fail(HTMX_IS_NODE(parent));
    g_return_if_fail(HTMX_IS_NODE(child));

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
    g_object_unref(child);
}

gchar *
clawt_web_agent_url(const gchar *agent_id, ClawtWebView view)
{
    g_autofree gchar *escaped = NULL;

    if (agent_id == NULL)
        return g_strdup("/");

    /*
     * Percent-encoded because an agent id reaches this from a config file
     * somebody edits.  The daemon does not require it to be a word, and a
     * "?" or "#" in one would otherwise silently take the rest of the
     * path into the query string.
     */
    escaped = g_uri_escape_string(agent_id, NULL, FALSE);

    return g_strdup_printf("/a/%s/%s", escaped, clawt_web_view_slug(view));
}

gchar *
clawt_web_relative_time(gint64 timestamp)
{
    gint64 now;
    gint64 delta;

    if (timestamp <= 0)
        return g_strdup("");

    now = g_get_real_time() / G_USEC_PER_SEC;
    delta = now - timestamp;

    if (delta < 0)
        return g_strdup("just now");
    if (delta < 60)
        return g_strdup_printf("%" G_GINT64_FORMAT "s ago", delta);
    if (delta < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", delta / 60);
    if (delta < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", delta / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", delta / 86400);
}

gchar *
clawt_web_one_line(const gchar *text, glong limit)
{
    g_autofree gchar *flat = NULL;
    gchar *p;
    glong length;

    if (text == NULL)
        return g_strdup("");

    flat = g_strdup(text);

    for (p = flat; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }

    g_strstrip(flat);

    /*
     * Measured in characters rather than bytes.  Truncating a UTF-8
     * string by byte count cuts a multi-byte character in half, and the
     * result is not a string -- htmx-glib escapes it into the page and
     * the browser renders a replacement glyph where a word was.
     */
    length = g_utf8_strlen(flat, -1);

    if (limit <= 0 || length <= limit)
        return g_steal_pointer(&flat);

    {
        const gchar *cut = g_utf8_offset_to_pointer(flat, limit);
        g_autofree gchar *head = g_strndup(flat, (gsize)(cut - flat));

        return g_strdup_printf("%s…", head);
    }
}

/* ── Pieces ──────────────────────────────────────────────────────── */

HtmxSpan *
clawt_web_badge(const gchar *text, const gchar *tone)
{
    g_autoptr(HtmxSpan) span = htmx_span_new();
    g_autofree gchar *css = NULL;

    htmx_element_add_class(HTMX_ELEMENT(span), "badge");

    css = g_strdup_printf("badge-%s", tone != NULL ? tone : "neutral");
    htmx_element_add_class(HTMX_ELEMENT(span), css);
    htmx_node_set_text_content(HTMX_NODE(span), text != NULL ? text : "");

    return (HtmxSpan *)g_steal_pointer(&span);
}

const gchar *
clawt_web_state_tone(const gchar *state)
{
    if (g_strcmp0(state, "running") == 0)
        return "good";
    if (g_strcmp0(state, "error") == 0 || g_strcmp0(state, "shadow") == 0)
        return "bad";
    if (g_strcmp0(state, "starting") == 0 ||
        g_strcmp0(state, "stopping") == 0 ||
        g_strcmp0(state, "degraded") == 0)
        return "warn";

    return "neutral";
}

HtmxHeading *
clawt_web_section_title(const gchar *text)
{
    g_autoptr(HtmxHeading) heading = htmx_heading_new(2);

    htmx_element_add_class(HTMX_ELEMENT(heading), "section");
    htmx_node_set_text_content(HTMX_NODE(heading), text != NULL ? text : "");

    return (HtmxHeading *)g_steal_pointer(&heading);
}

HtmxP *
clawt_web_text(const gchar *text, const gchar *css_class)
{
    g_autoptr(HtmxP) para = htmx_p_new();

    if (css_class != NULL)
        htmx_element_add_class(HTMX_ELEMENT(para), css_class);

    htmx_node_set_text_content(HTMX_NODE(para), text != NULL ? text : "");

    return (HtmxP *)g_steal_pointer(&para);
}

HtmxDiv *
clawt_web_card(const gchar *title, const gchar *subtitle)
{
    g_autoptr(HtmxDiv) card = htmx_div_new();
    g_autoptr(HtmxDiv) body = htmx_div_new();

    htmx_element_add_class(HTMX_ELEMENT(card), "card");

    if (title != NULL) {
        g_autoptr(HtmxHeading) heading = htmx_heading_new(3);

        htmx_element_add_class(HTMX_ELEMENT(heading), "card-title");
        htmx_node_set_text_content(HTMX_NODE(heading), title);
        htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(heading));
    }

    if (subtitle != NULL) {
        g_autoptr(HtmxP) sub = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(sub), "card-sub");
        htmx_node_set_text_content(HTMX_NODE(sub), subtitle);
        htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(sub));
    }

    htmx_element_add_class(HTMX_ELEMENT(body), "card-body");
    htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(body));

    return (HtmxDiv *)g_steal_pointer(&card);
}

HtmxElement *
clawt_web_card_body(HtmxDiv *card)
{
    GList *children;
    GList *last;

    g_return_val_if_fail(HTMX_IS_DIV(card), NULL);

    /*
     * The body is the last child by construction, which is why it is
     * added last in clawt_web_card().  Searching by class would be a
     * second answer to the same question and would go wrong the first
     * time somebody added a class to it.
     */
    children = htmx_node_get_children(HTMX_NODE(card));
    last = g_list_last(children);

    if (last == NULL)
        return HTMX_ELEMENT(card);

    return HTMX_ELEMENT(last->data);
}

HtmxDiv *
clawt_web_row(const gchar *title, const gchar *value)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxSpan) label = htmx_span_new();
    g_autoptr(HtmxSpan) text = htmx_span_new();

    htmx_element_add_class(HTMX_ELEMENT(row), "row");

    htmx_element_add_class(HTMX_ELEMENT(label), "row-title");
    htmx_node_set_text_content(HTMX_NODE(label), title != NULL ? title : "");
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(label));

    htmx_element_add_class(HTMX_ELEMENT(text), "row-value");
    htmx_node_set_text_content(HTMX_NODE(text), value != NULL ? value : "—");
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(text));

    return (HtmxDiv *)g_steal_pointer(&row);
}

HtmxDiv *
clawt_web_empty(const gchar *text, const gchar *detail)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(HtmxP) title = htmx_p_new();

    htmx_element_add_class(HTMX_ELEMENT(box), "empty");

    htmx_element_add_class(HTMX_ELEMENT(title), "empty-title");
    htmx_node_set_text_content(HTMX_NODE(title), text != NULL ? text : "");
    htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(title));

    if (detail != NULL) {
        g_autoptr(HtmxP) sub = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(sub), "empty-detail");
        htmx_node_set_text_content(HTMX_NODE(sub), detail);
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(sub));
    }

    return (HtmxDiv *)g_steal_pointer(&box);
}

HtmxButton *
clawt_web_button(const gchar *label, const gchar *variant)
{
    g_autoptr(HtmxButton) button = htmx_button_new_with_label(
        label != NULL ? label : "");

    htmx_element_add_class(HTMX_ELEMENT(button), "btn");

    if (variant != NULL && g_strcmp0(variant, "default") != 0) {
        g_autofree gchar *css = g_strdup_printf("btn-%s", variant);

        htmx_element_add_class(HTMX_ELEMENT(button), css);
    }

    return (HtmxButton *)g_steal_pointer(&button);
}

HtmxButton *
clawt_web_post_button(const gchar *label, const gchar *action,
                      const gchar *variant, const gchar *confirm)
{
    g_autoptr(HtmxButton) button = clawt_web_button(label, variant);

    htmx_element_set_hx_post(HTMX_ELEMENT(button), action);

    /*
     * The whole page is replaced rather than a fragment.  An action here
     * changes an agent's state, and the state is shown in the sidebar,
     * the topbar and the view at once -- three targets, which is three
     * chances for one of them to be left showing the previous answer.
     */
    htmx_element_set_hx_target(HTMX_ELEMENT(button), "body");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-swap",
                               "innerHTML show:no-scroll");

    if (confirm != NULL)
        htmx_element_set_hx_confirm(HTMX_ELEMENT(button), confirm);

    return (HtmxButton *)g_steal_pointer(&button);
}

HtmxForm *
clawt_web_form(const gchar *action)
{
    g_autoptr(HtmxForm) form = htmx_form_new_with_action(action, HTMX_METHOD_POST);

    htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "post");
    htmx_element_set_hx_post(HTMX_ELEMENT(form), action);
    htmx_element_set_hx_target(HTMX_ELEMENT(form), "body");
    htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-swap",
                               "innerHTML show:no-scroll");

    return (HtmxForm *)g_steal_pointer(&form);
}

HtmxInput *
clawt_web_hidden(const gchar *name, const gchar *value)
{
    g_autoptr(HtmxInput) input = htmx_input_new_hidden(name, value);

    return (HtmxInput *)g_steal_pointer(&input);
}

HtmxDiv *
clawt_web_field(const gchar *label, const gchar *name, const gchar *value,
                const gchar *placeholder)
{
    g_autoptr(HtmxDiv) field = htmx_div_new();
    g_autoptr(HtmxLabel) tag = htmx_label_new_with_text(
        label != NULL ? label : "");
    g_autoptr(HtmxInput) input = htmx_input_new_text(name);

    if (value != NULL)
        htmx_input_set_value(input, value);

    htmx_element_add_class(HTMX_ELEMENT(field), "field");
    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));

    if (placeholder != NULL)
        htmx_input_set_placeholder(input, placeholder);

    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(input));

    return (HtmxDiv *)g_steal_pointer(&field);
}

HtmxDiv *
clawt_web_textarea_field(const gchar *label, const gchar *name,
                         const gchar *value, guint rows)
{
    g_autoptr(HtmxDiv) field = htmx_div_new();
    g_autoptr(HtmxLabel) tag = htmx_label_new_with_text(
        label != NULL ? label : "");
    g_autoptr(HtmxTextarea) area = htmx_textarea_new_with_name(name);
    g_autofree gchar *rows_text = g_strdup_printf("%u", rows);

    htmx_element_add_class(HTMX_ELEMENT(field), "field");
    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));

    htmx_element_set_attribute(HTMX_ELEMENT(area), "rows", rows_text);

    if (value != NULL)
        htmx_node_set_text_content(HTMX_NODE(area), value);

    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(area));

    return (HtmxDiv *)g_steal_pointer(&field);
}

HtmxDiv *
clawt_web_switch_field(const gchar *label, const gchar *name,
                       const gchar *subtitle, gboolean on)
{
    g_autoptr(HtmxDiv) field = htmx_div_new();
    g_autoptr(HtmxInput) input = htmx_input_new(HTMX_INPUT_CHECKBOX);
    g_autoptr(HtmxDiv) text = htmx_div_new();
    g_autoptr(HtmxLabel) tag = htmx_label_new_with_text(
        label != NULL ? label : "");

    htmx_element_add_class(HTMX_ELEMENT(field), "field-check");

    htmx_input_set_name(input, name);
    htmx_input_set_value(input, "true");
    htmx_element_set_id(HTMX_ELEMENT(input), name);

    if (on)
        htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");

    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(input));

    htmx_element_set_attribute(HTMX_ELEMENT(tag), "for", name);
    htmx_node_add_child(HTMX_NODE(text), HTMX_NODE(tag));

    if (subtitle != NULL) {
        g_autoptr(HtmxP) sub = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(sub), "check-sub");
        htmx_node_set_text_content(HTMX_NODE(sub), subtitle);
        htmx_node_add_child(HTMX_NODE(text), HTMX_NODE(sub));
    }

    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(text));

    /*
     * An unticked checkbox posts nothing at all, so a form cannot tell
     * "off" from "the field was not on this page".  The hidden field
     * before it always posts, and the browser sends both when the box is
     * ticked -- the daemon takes the last, which is the checkbox.
     */
    {
        g_autoptr(HtmxDiv) wrap = htmx_div_new();
        g_autoptr(HtmxInput) marker = NULL;
        g_autofree gchar *marker_name = g_strdup_printf("%s__present", name);

        marker = htmx_input_new_hidden(marker_name, "1");
        htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(marker));
        htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(wrap));
    }

    return (HtmxDiv *)g_steal_pointer(&field);
}

HtmxDiv *
clawt_web_select_field(const gchar *label, const gchar *name,
                       const gchar *const *values, const gchar *const *labels,
                       const gchar *current)
{
    g_autoptr(HtmxDiv) field = htmx_div_new();
    g_autoptr(HtmxLabel) tag = htmx_label_new_with_text(
        label != NULL ? label : "");
    g_autoptr(HtmxSelect) select = htmx_select_new_with_name(name);
    gboolean found = FALSE;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(field), "field");
    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));

    for (i = 0; values != NULL && values[i] != NULL; i++) {
        const gchar *text = (labels != NULL && labels[i] != NULL)
                            ? labels[i] : values[i];
        g_autoptr(HtmxOption) option = htmx_option_new_with_value(values[i],
                                                                 text);

        if (g_strcmp0(values[i], current) == 0) {
            htmx_element_set_attribute(HTMX_ELEMENT(option), "selected",
                                       "selected");
            found = TRUE;
        }

        htmx_node_add_child(HTMX_NODE(select), HTMX_NODE(option));
    }

    /*
     * A value the list does not have is added to it rather than dropped.
     * A select that cannot represent what is set opens showing its first
     * entry, and saving the form -- without anybody touching this control
     * -- writes that first entry back over whatever was there.
     */
    if (!found && current != NULL && *current != '\0') {
        g_autofree gchar *text = g_strdup_printf("%s (set by hand)", current);
        g_autoptr(HtmxOption) option = htmx_option_new_with_value(current,
                                                                 text);
        htmx_element_set_attribute(HTMX_ELEMENT(option), "selected",
                                   "selected");
        htmx_node_add_child(HTMX_NODE(select), HTMX_NODE(option));
    }

    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(select));

    return (HtmxDiv *)g_steal_pointer(&field);
}

/* ── The document ────────────────────────────────────────────────── */

/*
 * The <head>, the frame and the scripts.
 *
 * HtmxBuilder rather than typed elements, which is htmx-glib's own rule:
 * the document wrapper, inline style and script are the three things the
 * element classes deliberately do not model.
 */
static void
open_document(HtmxBuilder *builder, const gchar *title)
{
    htmx_builder_doctype(builder);
    htmx_builder_begin(builder, "html");
    htmx_builder_attr(builder, "lang", "en");

    htmx_builder_begin(builder, "head");

    htmx_builder_begin_void(builder, "meta");
    htmx_builder_attr(builder, "charset", "utf-8");
    htmx_builder_end_void(builder);

    htmx_builder_begin_void(builder, "meta");
    htmx_builder_attr(builder, "name", "viewport");
    htmx_builder_attr(builder, "content",
                      "width=device-width, initial-scale=1");
    htmx_builder_end_void(builder);

    htmx_builder_begin(builder, "title");
    htmx_builder_text(builder, title);
    htmx_builder_end(builder);

    htmx_builder_begin(builder, "style");
    htmx_builder_raw_html(builder, clawt_web_stylesheet());
    htmx_builder_end(builder);

    /*
     * Local, never a CDN.  This page starts and stops agents and runs
     * commands inside their computers, so a script fetched from a third
     * party on every load is a way for somebody else to do that.  It is
     * also the difference between working and not working on a tailnet
     * with no route to the open internet, which is the case this client
     * was built for.
     */
    htmx_builder_begin(builder, "script");
    htmx_builder_attr(builder, "src", "/static/htmx.min.js");
    htmx_builder_attr(builder, "defer", "defer");
    htmx_builder_end(builder);

    htmx_builder_begin(builder, "script");
    htmx_builder_attr(builder, "src", "/static/htmx-ext-sse.js");
    htmx_builder_attr(builder, "defer", "defer");
    htmx_builder_end(builder);

    /*
     * The theme is applied before the body paints, from a cookie the
     * appearance page writes.  Done after paint it is a visible flash of
     * the wrong palette on every navigation.
     */
    htmx_builder_begin(builder, "script");
    htmx_builder_raw_html(builder,
        "(function(){var m=document.cookie.match(/(?:^|; )clawt_theme=([^;]*)/);"
        "if(m&&m[1]!=='system'){document.documentElement."
        "setAttribute('data-theme',decodeURIComponent(m[1]));}})();");
    htmx_builder_end(builder);

    htmx_builder_end(builder);  /* head */

    htmx_builder_begin(builder, "body");
    htmx_builder_attr(builder, "hx-ext", "sse");
    htmx_builder_attr(builder, "sse-connect", "/events");
}

static void
close_document(HtmxBuilder *builder)
{
    htmx_builder_end(builder);  /* body */
    htmx_builder_end(builder);  /* html */
}

gchar *
clawt_web_page(ClawtWebApp *app, const gchar *agent_id, ClawtWebView view,
               HtmxElement *body)
{
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autoptr(HtmxDiv) frame = htmx_div_new();
    g_autoptr(HtmxDiv) content = htmx_div_new();
    g_autofree gchar *title = NULL;

    title = (agent_id != NULL)
            ? g_strdup_printf("%s · %s · clawtilla", agent_id,
                              clawt_web_view_title(view))
            : g_strdup("clawtilla");

    open_document(builder, title);

    htmx_element_add_class(HTMX_ELEMENT(frame), "app");
    clawt_web_add(frame, clawt_web_sidebar(app, agent_id, view));

    htmx_element_add_class(HTMX_ELEMENT(content), "content");
    clawt_web_add(content, clawt_web_topbar(app, agent_id, view));

    if (body != NULL)
        htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(body));

    htmx_node_add_child(HTMX_NODE(frame), HTMX_NODE(content));

    {
        g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(frame));

        htmx_builder_raw_html(builder, html);
    }

    close_document(builder);

    return htmx_builder_render(builder);
}

gchar *
clawt_web_shell_page(ClawtWebApp *app, const gchar *title, HtmxElement *body)
{
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autofree gchar *full = g_strdup_printf("%s · clawtilla",
                                             title != NULL ? title : "");

    (void)app;

    open_document(builder, full);

    if (body != NULL) {
        g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(body));

        htmx_builder_raw_html(builder, html);
    }

    close_document(builder);

    return htmx_builder_render(builder);
}

HtmxResponse *
clawt_web_html_response(const gchar *html)
{
    HtmxResponse *response = htmx_response_new_with_content(html);

    htmx_response_set_content_type(response, "text/html; charset=utf-8");

    return response;
}

HtmxResponse *
clawt_web_fragment_response(HtmxElement *element)
{
    g_autofree gchar *html = htmx_element_render(element);

    return clawt_web_html_response(html);
}

HtmxResponse *
clawt_web_redirect(HtmxRequest *request, const gchar *location)
{
    HtmxResponse *response = htmx_response_new();

    if (htmx_request_is_htmx(request)) {
        /*
         * htmx follows a 303 by fetching it and swapping the result into
         * whatever target the element named -- so a whole page would be
         * spliced into a div.  HX-Redirect makes the browser navigate
         * instead, which is what a form that changed the world wants.
         */
        htmx_response_add_header(response, "HX-Redirect", location);
        htmx_response_set_status(response, 200);
        htmx_response_set_content(response, "");

        return response;
    }

    htmx_response_set_status(response, 303);
    htmx_response_add_header(response, "Location", location);

    return response;
}
