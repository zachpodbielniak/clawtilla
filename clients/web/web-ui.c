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
#include <libsoup/soup.h>

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
    { "triggers", "Triggers" },
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

HtmxElement *
clawt_web_avatar(const gchar *name, const gchar *agent_id,
                 gboolean has_avatar, const gchar *color,
                 const gchar *css_class)
{
    const gchar *ink = clawt_color_ink(color);

    if (has_avatar && agent_id != NULL) {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *url = g_strdup_printf("/a/%s/avatar", escaped);
        g_autoptr(HtmxImg) picture = htmx_img_new_with_src(url, name);

        htmx_element_add_class(HTMX_ELEMENT(picture), css_class);
        htmx_element_add_class(HTMX_ELEMENT(picture), "web-avatar-img");

        return HTMX_ELEMENT(g_steal_pointer(&picture));
    }

    {
        g_autoptr(HtmxSpan) face = htmx_span_new();
        gunichar first;
        gchar initial[8] = { 0 };

        htmx_element_add_class(HTMX_ELEMENT(face), css_class);

        first = g_utf8_get_char_validated(name, -1);

        if (first != (gunichar)-1 && first != (gunichar)-2)
            g_unichar_to_utf8(g_unichar_toupper(first), initial);

        htmx_node_set_text_content(HTMX_NODE(face), initial);

        if (ink != NULL) {
            g_autofree gchar *style = g_strdup_printf(
                "background:%s;color:%s", color, ink);

            htmx_element_set_attribute(HTMX_ELEMENT(face), "style", style);
        } else {
            /*
             * One of the sheet's own tones, chosen by the name, rather
             * than a colour computed here -- so a palette recolours the
             * faces with everything else.
             */
            g_autofree gchar *cls = g_strdup_printf(
                "avatar-tone-%u", g_str_hash(name) % 6);

            htmx_element_add_class(HTMX_ELEMENT(face), cls);
        }

        return HTMX_ELEMENT(g_steal_pointer(&face));
    }
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


/* ── What this browser looks like ────────────────────────────────── */

void
clawt_web_look_free(ClawtWebLook *self)
{
    if (self == NULL)
        return;

    g_free(self->theme);
    g_free(self->font);
    g_free(self->mono);
    g_free(self);
}

/*
 * One cookie's value, or NULL.
 *
 * Read by hand rather than through a parser because there is one header
 * and five names; what matters is matching a whole name, so that
 * "clawt_font_size" is not found when looking for "clawt_font".
 */
static gchar *
cookie_value(const gchar *header, const gchar *name)
{
    g_auto(GStrv) parts = NULL;
    gsize i;

    if (header == NULL)
        return NULL;

    parts = g_strsplit(header, ";", -1);

    for (i = 0; parts[i] != NULL; i++) {
        g_autofree gchar *pair = g_strdup(parts[i]);
        const gchar *equals;

        g_strstrip(pair);
        equals = strchr(pair, '=');

        if (equals == NULL)
            continue;

        if (strncmp(pair, name, (gsize)(equals - pair)) != 0 ||
            strlen(name) != (gsize)(equals - pair))
            continue;

        return g_uri_unescape_string(equals + 1, NULL);
    }

    return NULL;
}

ClawtWebLook *
clawt_web_look_from_request(HtmxRequest *request)
{
    ClawtWebLook *look = g_new0(ClawtWebLook, 1);
    const gchar *cookies = NULL;

    /*
     * Off the SoupServerMessage rather than htmx_request_get_headers(),
     * which is the parsed HX-* set. A request built by
     * htmx_request_new_for_path() has no message, and then every field
     * stays zeroed -- which is "defer", and correct.
     */
    if (request != NULL) {
        SoupServerMessage *message = htmx_request_get_message(request);

        if (message != NULL)
            cookies = soup_message_headers_get_one(
                soup_server_message_get_request_headers(message), "Cookie");
    }

    look->theme = cookie_value(cookies, "clawt_theme");
    look->font = cookie_value(cookies, "clawt_font");
    look->mono = cookie_value(cookies, "clawt_mono");

    {
        g_autofree gchar *size = cookie_value(cookies, "clawt_font_size");
        g_autofree gchar *mono_size = cookie_value(cookies,
                                                   "clawt_mono_size");

        look->font_size = (size != NULL)
                          ? (gint)g_ascii_strtoll(size, NULL, 10) : 0;
        look->mono_size = (mono_size != NULL)
                          ? (gint)g_ascii_strtoll(mono_size, NULL, 10) : 0;
    }

    {
        g_autofree gchar *measure = cookie_value(cookies, "clawt_measure");
        g_autofree gchar *run_gap = cookie_value(cookies, "clawt_run_gap");

        /*
         * Through the library's own reader, so a spelling the GTK
         * client's appearance file accepts is a spelling this cookie
         * accepts.  A cookie is edited as easily as a config file, and
         * two readers of one format is how one of them comes to take a
         * value the other silently drops.
         */
        clawt_measure_parse(measure, &look->measure_unit, &look->measure);

        look->run_gap = (run_gap != NULL)
                        ? (gint)g_ascii_strtoll(run_gap, NULL, 10) : 0;
    }

    return look;
}

/*
 * A font family fit to put inside a CSS declaration.
 *
 * Sanitised rather than escaped, and by an allowlist rather than a
 * denylist. CSS string escapes are their own small language and there is
 * nothing in a font name to preserve, so the safe move is to keep only
 * what a family can legitimately contain and drop the rest.
 *
 * A denylist was the first attempt and was already wrong: it stopped a
 * quote closing the string but let a slash-star through, which opens a
 * CSS comment and swallows the rest of the sheet. (Spelled out rather
 * than written, because a comment-opener inside a C comment is its own
 * warning.)
 */
static gchar *
sanitise_family(const gchar *family)
{
    g_autoptr(GString) out = NULL;
    const gchar *p;

    if (family == NULL)
        return NULL;

    out = g_string_new(NULL);

    for (p = family; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && strchr(" -_.,", *p) == NULL)
            continue;

        g_string_append_c(out, *p);
    }

    g_strstrip(out->str);

    if (out->str[0] == '\0')
        return NULL;

    return g_strdup(out->str);
}

gchar *
clawt_web_look_css(const ClawtWebLook *look)
{
    g_autoptr(GString) css = g_string_new(NULL);
    g_autofree gchar *family = NULL;
    g_autofree gchar *mono = NULL;

    if (look == NULL)
        return g_strdup("");

    family = sanitise_family(look->font);
    mono = sanitise_family(look->mono);

    /*
     * Nothing at all is emitted for an unset field, rather than a rule
     * naming what the browser would have used anyway. The two look
     * identical and diverge for ever afterwards.
     */
    if (family != NULL)
        g_string_append_printf(css, "--sans:\"%s\",system-ui,sans-serif;",
                               family);

    if (mono != NULL)
        g_string_append_printf(css, "--mono:\"%s\",ui-monospace,monospace;",
                               mono);

    if (look->font_size >= 8 && look->font_size <= 32)
        g_string_append_printf(css, "--font-size:%dpx;", look->font_size);

    if (look->mono_size >= 8 && look->mono_size <= 32)
        g_string_append_printf(css, "--mono-size:%dpx;", look->mono_size);

    /*
     * The column and the run gap, rendered by the library rather than
     * by this client's own idea of the units -- a cookie is edited as
     * easily as a config file, and two clients disagreeing about what
     * is allowed is the drift `make parity` exists for.
     *
     * The unit survives all the way into the declaration: a percentage
     * stays a percentage so the column keeps following the window, and
     * a character count stays `ch` so it keeps following the font.
     * Resolving either to pixels here would freeze it at whatever the
     * browser happened to be when the page was served.
     */
    {
        g_autofree gchar *measure = clawt_measure_to_css(
            look->measure_unit, look->measure, "var(--chat-gutter)");

        if (measure != NULL)
            g_string_append_printf(css, "--chat-measure:%s;", measure);
    }

    if (look->run_gap > 0 &&
        look->run_gap <= CLAWT_APPEARANCE_MAX_RUN_SPACING)
        g_string_append_printf(css, "--chat-run-gap:%dpx;", look->run_gap);

    if (css->len == 0)
        return g_strdup("");

    return g_strdup_printf(":root{%s}", css->str);
}

/*
 * The CSS of a palette this reader has chosen, when it came from a file.
 *
 * Built through ClawtAppearance rather than by reading the directory
 * here, so the discovery, the header parsing and the "an unknown nick
 * follows the system" rule are the library's single answer -- two
 * readers of a palette directory would differ exactly once, on the file
 * nobody looked at.
 *
 * Returns: (transfer full): the sheet, or "" for a built-in scheme
 */
static gchar *
clawt_web_palette_css(const ClawtWebLook *look)
{
    g_autoptr(ClawtAppearance) appearance = NULL;

    if (look == NULL || look->theme == NULL)
        return g_strdup("");

    appearance = clawt_appearance_new();
    clawt_appearance_set_scheme(appearance, look->theme);

    /*
     * A built-in scheme is already in the stylesheet, so anything this
     * emits for one would be a second definition of the same colours.
     */
    if (g_strcmp0(clawt_appearance_get_scheme(appearance), look->theme) != 0)
        return g_strdup("");

    if (clawt_appearance_get_palette_css(appearance) == NULL)
        return g_strdup("");

    return g_strdup(clawt_appearance_get_palette_css(appearance));
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
open_document(HtmxBuilder *builder, const gchar *title,
              const ClawtWebLook *look)
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
     * A palette from disk, before the person's own overrides.
     *
     * A palette is a stylesheet, which is the whole point of it no
     * longer being an enum: the same file reaches both clients and each
     * applies it in its own vocabulary.  Emitted verbatim rather than
     * translated, because the two clients name their colours
     * differently and inventing a mapping would decide for the author
     * which half of their palette was real.
     *
     * Nothing at all for a built-in scheme -- those are already in the
     * stylesheet above, keyed by data-theme.
     */
    {
        g_autofree gchar *palette = clawt_web_palette_css(look);

        if (palette != NULL && *palette != '\0') {
            htmx_builder_begin(builder, "style");
            htmx_builder_raw_html(builder, palette);
            htmx_builder_end(builder);
        }
    }

    /*
     * The person's own overrides after the stylesheet, so they win on
     * specificity ties without !important.
     */
    {
        g_autofree gchar *overrides = clawt_web_look_css(look);

        if (*overrides != '\0') {
            htmx_builder_begin(builder, "style");
            htmx_builder_raw_html(builder, overrides);
            htmx_builder_end(builder);
        }
    }

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
     *
     * A palette from a file gets its *base* mode here rather than its
     * own nick.  The stylesheet has a rule for `dark` and none for
     * `inkwell`, so setting the nick would leave every colour the
     * palette does not define falling back to the light defaults --
     * light chrome under dark colours, which is worse than either.  The
     * palette's own sheet is emitted above and still wins on the
     * colours it does define.
     */
    {
        const gchar *attr = NULL;

        if (look != NULL && look->theme != NULL) {
            g_autoptr(ClawtAppearance) probe = clawt_appearance_new();

            clawt_appearance_set_scheme(probe, look->theme);

            /*
             * A built-in keeps its own nick, because the stylesheet has
             * a block for it. Only a file palette is folded down.
             */
            if (g_strcmp0(clawt_appearance_get_scheme(probe),
                          look->theme) == 0 &&
                clawt_appearance_get_palette_css(probe) != NULL)
                attr = clawt_appearance_theme_nick(
                    clawt_appearance_get_theme(probe));
        }

        htmx_builder_begin(builder, "script");

        if (attr != NULL) {
            g_autofree gchar *script = g_strdup_printf(
                "document.documentElement.setAttribute('data-theme','%s');",
                attr);

            htmx_builder_raw_html(builder, script);
        } else {
            htmx_builder_raw_html(builder,
                "(function(){var m=document.cookie.match("
                "/(?:^|; )clawt_theme=([^;]*)/);"
                "if(m&&m[1]!=='system'){document.documentElement."
                "setAttribute('data-theme',decodeURIComponent(m[1]));}})();");
        }

        htmx_builder_end(builder);
    }

    /*
     * Saying that something arrived while you were reading.
     *
     * The GTK client keeps a `following` flag and drives a rule in the
     * transcript and a pill from it, in one place, because a marker with
     * no pill or a pill with no marker is worse than neither. Here the
     * same state is the browser's scroll position, so it has to live in
     * the page -- but the pair and the rule about them are the same.
     *
     * Inline rather than a file, like the theme script above: this page
     * must work on a tailnet with no route to the internet, and a script
     * it fetches is a script that can drive the whole fleet.
     *
     * htmx swaps the whole transcript on every fleet event, so "what is
     * new" is counted rather than diffed: the number of messages before
     * the swap is where the rule goes after it. Both the rule and the
     * pill appear only when a swap actually brought something, never
     * merely because the reader scrolled up -- a control that is always
     * there says nothing.
     */
    htmx_builder_begin(builder, "script");
    htmx_builder_raw_html(builder,
        /*
         * Listeners on `document`, not on `document.body`. This script
         * runs in the head, where document.body is still null -- the
         * first addEventListener threw and took every handler with it,
         * including the one that opens a conversation at its newest
         * message. It reads perfectly and does nothing; the console said
         * so on the first real page load.
         */
        "(function(){"
        "var seen=-1,mark=false,keep=0;"
        "function box(){return document.getElementById('transcript');}"
        "function pill(){return document.getElementById('jump-pill');}"
        "function at_end(b){return b.scrollHeight-b.scrollTop-b.clientHeight<40;}"
        "function to_end(){var b=box();if(b){b.scrollTop=b.scrollHeight;}"
        "var p=pill();if(p){p.classList.remove('on');}"
        "var r=document.getElementById('unread-rule');"
        "if(r&&r.parentNode){r.parentNode.removeChild(r);}mark=false;}"
        "document.addEventListener('click',function(e){"
        "var p=pill();if(p&&(e.target===p||p.contains(e.target))){to_end();}});"
        /* Before the swap: how much was there, and were we at the end. */
        "document.addEventListener('htmx:beforeSwap',function(e){"
        "var b=box();if(!b||e.detail.target.id!=='transcript'){return;}"
        "keep=b.scrollTop;"
        "seen=at_end(b)?-1:b.querySelectorAll('.msg').length;});"
        /* After: follow, or say what came in while we were not. */
        "document.addEventListener('htmx:afterSwap',function(e){"
        "var b=box();if(!b||e.detail.target.id!=='transcript'){return;}"
        "var msgs=b.querySelectorAll('.msg');"
        "if(seen<0){b.scrollTop=b.scrollHeight;return;}"
        /*
         * Put back where they were reading. The swap replaces the whole
         * element, so the browser has nothing to restore from and a
         * reader who had scrolled up was thrown to the top of the
         * conversation on every fleet event -- which is the opposite of
         * what refusing to auto-scroll is for. Messages only ever arrive
         * at the end, so the offset above them has not moved.
         */
        "b.scrollTop=keep;"
        "if(msgs.length<=seen){return;}"
        "var p=pill();if(p){p.classList.add('on');}"
        "if(mark){return;}"
        "var r=document.createElement('div');"
        "r.className='unread-rule';r.id='unread-rule';"
        "r.appendChild(document.createTextNode('New messages'));"
        "msgs[seen].parentNode.insertBefore(r,msgs[seen]);mark=true;});"
        /* Opening a conversation starts at the newest message. */
        "document.addEventListener('DOMContentLoaded',function(){"
        "var b=box();if(b){b.scrollTop=b.scrollHeight;}});"
        /*
         * Scrolling back down by hand clears both, the way returning to
         * the bottom does in the GTK client -- the marker describes
         * where reading stopped, and it has not stopped there any more.
         *
         * On `document` in the capture phase, because scroll does not
         * bubble and because the transcript element itself is replaced
         * on every fleet event: a listener bound to it survives exactly
         * until the next arrival, which is the one moment it is needed.
         * Bound directly, it cleared nothing after the first swap and
         * looked like it worked, since the first swap is also the first
         * test anybody runs.
         */
        "document.addEventListener('scroll',function(e){"
        "var b=box();if(!b||e.target!==b||!mark){return;}"
        "if(at_end(b)){to_end();}},true);"
        "})();");
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
               HtmxElement *body, HtmxRequest *request)
{
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autoptr(HtmxDiv) frame = htmx_div_new();
    g_autoptr(HtmxDiv) content = htmx_div_new();
    g_autofree gchar *title = NULL;

    title = (agent_id != NULL)
            ? g_strdup_printf("%s · %s · clawtilla", agent_id,
                              clawt_web_view_title(view))
            : g_strdup("clawtilla");

    /*
     * Which conversation is being read, so an arrival in it does not
     * accrue a count -- the same rule the GTK client applies from its
     * selected room.  Set before the sidebar is built, because the
     * sidebar is what draws the counts.
     */
    clawt_web_app_set_viewing(app, (view == CLAWT_WEB_VIEW_CHAT)
                                       ? agent_id : NULL);

    open_document(builder, title, look);

    htmx_element_add_class(HTMX_ELEMENT(frame), "app");

    /*
     * The drawer's state, and it has to be here rather than inside the
     * sidebar.  The sidebar carries hx-swap="outerHTML" on `sse:fleet`,
     * so anything remembered inside it is thrown away every time an
     * agent changes state -- a drawer built from <details> would shut
     * itself several times a minute on a live fleet.  A checkbox in
     * front of it is untouched by that swap, and a <label for> in the
     * topbar drives it with no script.
     */
    {
        /*
         * No g_autoptr here: clawt_web_add() takes the reference.  With
         * one the object was unreffed twice and freed while still in the
         * tree, so the checkbox rendered as nothing at all -- and the
         * hamburger, three lines away in web-fleet.c, went on being
         * drawn and toggling something that was not there.  A drawer
         * that cannot be opened, from a helper whose ownership differs
         * from the htmx_node_add_child() used beside it.
         */
        HtmxInput *nav = htmx_input_new(HTMX_INPUT_CHECKBOX);

        htmx_element_set_id(HTMX_ELEMENT(nav), "nav-open");
        htmx_element_add_class(HTMX_ELEMENT(nav), "nav-toggle");
        clawt_web_add(frame, nav);
    }

    clawt_web_add(frame, clawt_web_sidebar(app, agent_id, view));

    htmx_element_add_class(HTMX_ELEMENT(content), "content");
    clawt_web_add(content, clawt_web_topbar(app, agent_id, view));

    /*
     * Under the topbar and over the page, which is where a person looks
     * for a banner -- and inside the content column rather than above
     * the whole frame, so it does not push the agent list down.  That
     * list is navigation and losing a connection is not a reason to move
     * it.  The GTK client puts its AdwBanner in exactly the same place
     * relative to its own header.
     */
    {
        g_autofree gchar *notice = clawt_web_app_connection_notice(app);

        if (notice != NULL) {
            g_autoptr(HtmxDiv) banner = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(banner),
                                   "clawt-connection-banner");
            clawt_web_add(HTMX_ELEMENT(banner),
                          clawt_web_text(notice, NULL));
            htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(banner));
        }
    }

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
clawt_web_shell_page(ClawtWebApp *app, const gchar *title, HtmxElement *body,
                     HtmxRequest *request)
{
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autofree gchar *full = g_strdup_printf("%s · clawtilla",
                                             title != NULL ? title : "");

    (void)app;

    open_document(builder, full, look);

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
