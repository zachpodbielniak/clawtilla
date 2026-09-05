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

/* ── Where a write came from ────────────────────────────────── */

/*
 * "host:port", from a URL or from a Host header.
 *
 * Both are parsed the same way so the comparison is between two things of
 * the same shape.  A bare `Host` is not a URL, so it is given a scheme to
 * make one -- which also gets the bracket form of an IPv6 address right,
 * where splitting on the last colon does not.
 */
static gchar *
authority_of(const gchar *value, gboolean is_url)
{
    g_autofree gchar *url = NULL;
    g_autofree gchar *host = NULL;
    gint port = -1;

    if (value == NULL || *value == '\0')
        return NULL;

    url = is_url ? g_strdup(value) : g_strconcat("http://", value, NULL);

    if (!g_uri_split_network(url, G_URI_FLAGS_NONE, NULL, &host, &port, NULL))
        return NULL;

    if (host == NULL || *host == '\0')
        return NULL;

    if (port < 0)
        return g_ascii_strdown(host, -1);

    {
        g_autofree gchar *lowered = g_ascii_strdown(host, -1);

        return g_strdup_printf("%s:%d", lowered, port);
    }
}

gboolean
clawt_web_write_is_cross_site(HtmxMethod   method,
                              const gchar *sec_fetch_site,
                              const gchar *origin,
                              const gchar *referer,
                              const gchar *host)
{
    g_autofree gchar *mine = NULL;
    g_autofree gchar *theirs = NULL;

    /*
     * A read changes nothing, and refusing one would break every link
     * anybody has ever sent somebody else.
     */
    if (method == HTMX_METHOD_GET)
        return FALSE;

    /*
     * The browser's own answer, when it gives one.  "none" is a request
     * the person started themselves -- a typed address or a bookmark --
     * which no page can cause.
     */
    if (sec_fetch_site != NULL && *sec_fetch_site != '\0')
        return g_ascii_strcasecmp(sec_fetch_site, "same-origin") != 0 &&
               g_ascii_strcasecmp(sec_fetch_site, "none") != 0;

    mine = authority_of(host, FALSE);

    /*
     * Without a Host there is nothing to compare against, and a write
     * whose destination cannot be established is refused rather than
     * guessed at.  HTTP/1.1 requires the header.
     */
    if (mine == NULL)
        return TRUE;

    /*
     * Origin is sent on every cross-origin write, including the form
     * post that has no preflight; Referer is the fallback for the older
     * browser that sends neither of the other two.
     */
    theirs = authority_of(origin, TRUE);

    if (theirs == NULL)
        theirs = authority_of(referer, TRUE);

    if (theirs == NULL)
        return FALSE;

    return g_strcmp0(mine, theirs) != 0;
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
clawt_web_agent_url(const gchar *agent_id, ClawtPage view)
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

    return g_strdup_printf("/a/%s/%s", escaped, clawt_page_nick(view));
}

HtmxElement *
clawt_web_section_subnav(const gchar *agent_id, ClawtPage view)
{
    ClawtSection section = clawt_page_section(view);
    guint n = clawt_section_page_count(section);
    g_autoptr(HtmxDiv) nav = NULL;
    guint i;

    /*
     * Nothing at all for a section that is one page.  A row holding a
     * single tab reads as a control that does nothing, and it would cost
     * every page in Chat and Computer a strip of the window to say so.
     */
    if (agent_id == NULL || n < 2)
        return NULL;

    nav = htmx_div_new();
    htmx_element_add_class(HTMX_ELEMENT(nav), "subnav");
    htmx_element_add_class(HTMX_ELEMENT(nav), "section-subnav");

    for (i = 0; i < n; i++) {
        ClawtPage page = clawt_section_page_nth(section, i);
        g_autofree gchar *url = clawt_web_agent_url(agent_id, page);
        g_autoptr(HtmxA) tab = htmx_a_new_with_href(url);

        htmx_element_add_class(HTMX_ELEMENT(tab), "subnav-tab");
        htmx_node_set_text_content(HTMX_NODE(tab), clawt_page_label(page));

        if (page == view)
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current",
                                       "page");

        htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(tab));
    }

    return HTMX_ELEMENT(g_steal_pointer(&nav));
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

/*
 * Whether a schema BOOLEAN is on in a reply the daemon just sent.
 *
 * Both spellings, because the daemon uses both and a reader that knows
 * only one is wrong silently.  `routine.list` and `trigger.list` walk
 * the schema and emit a real JSON boolean; `agent.show` stringifies its
 * whole settings map, so the same field arrives there as "true".
 *
 * Both editors read this with clawt_web_member() -- the *string* reader,
 * which returns its fallback for a node that is not a G_TYPE_STRING --
 * and compared the result to "true".  So every switch on the routine and
 * trigger editors rendered unticked however the routine was actually
 * configured, and since clawt_web_switch_field() posts a `__present`
 * marker whether or not the box is ticked, saving the form sent an
 * explicit false.  Opening a live routine to fix a typo in its
 * instructions and pressing Save turned it off, and the page it
 * re-rendered agreed, so nothing looked wrong.
 *
 * Returns: %TRUE when @key is present and on
 */
gboolean
clawt_web_schema_flag(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || key == NULL || !json_object_has_member(object, key))
        return FALSE;

    node = json_object_get_member(object, key);

    if (!JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
        return json_node_get_boolean(node);

    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strcmp0(json_node_get_string(node), "true") == 0;

    return FALSE;
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

/*
 * Whether `clawt_agent_desc` says to keep descriptions off the rows.
 *
 * Its own function so a test can reach the decision: building a
 * request that carries a Cookie header needs a SoupServerMessage,
 * which is not something a test can make, and the interesting half of
 * this is not the header parsing -- it is what an unreadable value
 * means.
 *
 * Absent is "show", which the zeroed field already says.  Only an
 * explicit "0" turns them off, so a cookie somebody has mangled leaves
 * the list as it has always been rather than silently emptying every
 * row of its description -- which would read as the descriptions being
 * gone rather than as a preference that did not survive.
 */
static gboolean
descriptions_hidden(const gchar *cookie)
{
    return cookie != NULL && g_strcmp0(cookie, "0") == 0;
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

    {
        g_autofree gchar *descriptions =
            cookie_value(cookies, "clawt_agent_desc");

        look->hide_descriptions = descriptions_hidden(descriptions);
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
        /*
         * The `/` completions.
         *
         * Delegated on `document` for the same reason everything else
         * here is: htmx replaces the composer on every fleet event, so a
         * listener bound to the textarea survives exactly until the next
         * arrival -- which is the moment it stops working, and the first
         * swap is also the first thing anybody tests.
         *
         * The list is asked for once per composer, on the first `/`.
         * Clicking an entry only fills the box; sending is still the
         * ordinary submit, and the expansion happens in the daemon so
         * that this client and the GTK one send identical text.
         */
        "function slash(){return document.getElementById('slash-popover');}"
        "document.addEventListener('input',function(e){"
        "var a=e.target;if(!a||a.id!=='composer-body'){return;}"
        "var p=slash();if(!p){return;}"
        "var v=a.value;"
        "var on=v.charAt(0)==='/'&&v.indexOf(' ')<0&&v.indexOf('\\n')<0;"
        "if(!on){p.classList.remove('on');return;}"
        "p.classList.add('on');"
        "if(!p.dataset.loaded){p.dataset.loaded='1';"
        "p.dispatchEvent(new CustomEvent('clawtilla:slash'));}"
        "var q=v.slice(1).toLowerCase();"
        "var items=p.querySelectorAll('.slash-item');"
        "for(var i=0;i<items.length;i++){"
        "var n=(items[i].dataset.command||'').slice(1).toLowerCase();"
        "items[i].style.display=n.indexOf(q)===0?'':'none';}});"

        /*
         * The `@` completions.
         *
         * Same delegation and the same reason as the slash list, and
         * deliberately the same markup, so one stylesheet rule paints
         * both.  What differs is the two things a mention cannot share:
         * the prefix is taken at the cursor rather than at the start of
         * the line, because somebody who goes back to correct an `@` is
         * typing in the middle of a written message; and choosing one
         * *inserts*, where a command replaces the whole box.  Replacing
         * here would throw away the message somebody is halfway through.
         *
         * This decides what to offer and nothing else.  Whether an
         * `@name` reaches anybody is settled by clawt_mention_names() in
         * the daemon -- which is why the list is rendered from the
         * room's members and never assembled from what was typed.
         */
        "function mentions(){"
        "return document.getElementById('mention-popover');}"
        /*
         * The half-written name to the left of the cursor, or null when
         * the cursor is not in one.  An empty string is not null: it is
         * an `@` that has just been typed, and reading the two as the
         * same thing would keep the list shut until a letter followed --
         * which is exactly when somebody has stopped needing the names.
         *
         * The two character classes are not the same class, and neither
         * is a typo.  What may precede the `@` is whatever
         * `character_joins_a_word()` accepts, which is Unicode-aware --
         * hence \p{L}\p{N} rather than A-Za-z0-9, so that a name typed
         * after an accented word is judged here the way the daemon
         * judges it.  What may follow it is what an id may contain,
         * which is ASCII.
         */
        "function at_prefix(a){"
        "var v=a.value,i=a.selectionStart,j=i;"
        "if(a.selectionEnd!==i){return null;}"
        "while(j>0){var c=v.charAt(j-1);"
        "if(c==='@'){"
        "var b=j>1?v.charAt(j-2):'';"
        "if(b&&/[\\p{L}\\p{N}_@-]/u.test(b)){return null;}"
        "return v.slice(j,i);}"
        "if(!/[A-Za-z0-9_-]/.test(c)){return null;}"
        "j--;}"
        "return null;}"
        "document.addEventListener('input',function(e){"
        "var a=e.target;if(!a||a.id!=='composer-body'){return;}"
        "var p=mentions();if(!p){return;}"
        "var q=at_prefix(a);"
        "if(q===null){p.classList.remove('on');return;}"
        "q=q.toLowerCase();"
        "var items=p.querySelectorAll('.mention-item'),shown=0;"
        "for(var i=0;i<items.length;i++){"
        "var n=(items[i].dataset.mention||'').toLowerCase();"
        "var on=n.indexOf(q)===0;"
        "items[i].style.display=on?'':'none';if(on){shown++;}}"
        "if(shown){p.classList.add('on');}"
        "else{p.classList.remove('on');}});"
        /*
         * Every name here carries a `cmp_` prefix, because this whole
         * script is one IIFE and its scope is shared.  A plain
         * `function mark()` was silently replaced by the transcript's
         * `var mark=false` -- the declaration hoists, the assignment
         * then runs over it, and the only symptom was a completion list
         * that would not move under the arrow keys, with one
         * "mark is not a function" in a console nobody had open.
         *
         * Choosing one, from a click or from the keyboard.
         *
         * One function for both kinds of row and both ways of picking
         * one, because the alternative is four copies of "what does
         * choosing this mean" and they drift.  A command replaces the
         * whole box -- it has to be the first thing on the line -- and
         * a mention replaces only the half-typed name, so that
         * completing an `@` in the middle of a written message leaves
         * the rest of it alone.
         */
        "function cmp_choose(it){"
        "var a=document.getElementById('composer-body');"
        "if(!it||!a){return false;}"
        "if(it.dataset.command){"
        "a.value=it.dataset.command+' ';a.focus();"
        "var s=slash();if(s){s.classList.remove('on');}"
        "return true;}"
        "if(!it.dataset.mention){return false;}"
        "var q=at_prefix(a);if(q===null){return false;}"
        "var i=a.selectionStart,name=it.dataset.mention+' ';"
        "a.value=a.value.slice(0,i-q.length)+name+a.value.slice(i);"
        "a.focus();"
        "a.selectionStart=a.selectionEnd=i-q.length+name.length;"
        "var m=mentions();if(m){m.classList.remove('on');}"
        "return true;}"
        "document.addEventListener('click',function(e){"
        "var it=e.target.closest?"
        "e.target.closest('.slash-item,.mention-item'):null;"
        "if(!it){return;}"
        "e.preventDefault();cmp_choose(it);});"
        /*
         * Keyboard navigation, over whichever list is open.
         *
         * The two cannot both be open -- `/` and `@` cannot both begin
         * the word under the cursor -- so "the open one" is a complete
         * answer and there is no notion of which list has focus.
         *
         * Only the *visible* items are walked: the lists are filtered
         * by hiding rows, so counting all of them would arrow through
         * names that are not on screen.
         */
        "function cmp_open(){"
        "var l=[slash(),mentions()];"
        "for(var i=0;i<l.length;i++){"
        "if(l[i]&&l[i].classList.contains('on')){return l[i];}}"
        "return null;}"
        "function cmp_shown(p){"
        "var all=p.querySelectorAll('.slash-item,.mention-item'),out=[];"
        "for(var i=0;i<all.length;i++){"
        "if(all[i].style.display!=='none'){out.push(all[i]);}}"
        "return out;}"
        "function cmp_active(p){return p.querySelector('.completion-active');}"
        "function cmp_mark(items,n){"
        "for(var i=0;i<items.length;i++){"
        "items[i].classList.toggle('completion-active',i===n);}"
        "if(items[n]&&items[n].scrollIntoView){"
        "items[n].scrollIntoView({block:'nearest'});}}"
        /*
         * Down from nothing takes the first and Up takes the last, so
         * "open the list, press Up, press Return" reaches the bottom of
         * a long roster without arrowing through it.
         */
        "function cmp_move(p,d){"
        "var items=cmp_shown(p);if(!items.length){return false;}"
        "var cur=items.indexOf(cmp_active(p));"
        "var n=cur<0?(d>0?0:items.length-1)"
        ":((cur+d)+items.length)%items.length;"
        "cmp_mark(items,n);return true;}"
        "document.addEventListener('keydown',function(e){"
        "var a=e.target;if(!a||a.id!=='composer-body'){return;}"
        "var p=cmp_open();if(!p){return;}"
        "if(e.key==='ArrowDown'){"
        "if(cmp_move(p,1)){e.preventDefault();}return;}"
        "if(e.key==='ArrowUp'){"
        "if(cmp_move(p,-1)){e.preventDefault();}return;}"
        "if(e.key==='Escape'){"
        "p.classList.remove('on');cmp_mark(cmp_shown(p),-1);"
        "e.preventDefault();return;}"
        "if(e.key==='Tab'){"
        "var items=cmp_shown(p),it=cmp_active(p)||items[0];"
        "if(cmp_choose(it)){e.preventDefault();}return;}"
        /*
         * Return takes the highlighted row and nothing else.  With none
         * highlighted it is left alone, so Return goes on doing what it
         * did before the list existed -- which in this box is a
         * newline, and in the GTK client is a send.  Either way, the
         * list being open is not a reason to change it.
         */
        "if(e.key==='Enter'&&!e.shiftKey){"
        "if(cmp_choose(cmp_active(p))){e.preventDefault();}}});"
        /*
         * Refiltering invalidates the highlight: it described a row
         * that may now be hidden, or a different name.
         */
        "document.addEventListener('input',function(e){"
        "var a=e.target;if(!a||a.id!=='composer-body'){return;}"
        "var l=[slash(),mentions()];"
        "for(var i=0;i<l.length;i++){"
        "if(!l[i]){continue;}"
        "var act=l[i].querySelector('.completion-active');"
        "if(act){act.classList.remove('completion-active');}}});"
        /*
         * A profile picture, larger.
         *
         * Delegated for the reason everything else here is: htmx
         * replaces the whole transcript on every fleet event, so a
         * listener bound to a face survives until the next arrival --
         * which is the moment it stops working, and the first swap is
         * also the first thing anybody tests.
         *
         * The `src` is set from the button's own attribute rather than
         * being present in the markup, so the overlay costs no request
         * until somebody opens it -- and then costs none either, since
         * it is the URL the face beside it already fetched.  Cleared
         * again on close so a picture that has since changed is
         * re-asked for rather than served from a node holding the old
         * one.
         */
        "function zoom(){return document.getElementById('avatar-zoom');}"
        "function unzoom(){var z=zoom();if(!z){return;}"
        "z.classList.remove('on');var i=z.querySelector('img');"
        "if(i){i.removeAttribute('src');}}"
        "document.addEventListener('click',function(e){"
        "var b=e.target.closest?e.target.closest('.avatar-zoom'):null;"
        "var z=zoom();if(!z){return;}"
        "if(b){e.preventDefault();var i=z.querySelector('img');"
        "if(i){i.src=b.dataset.zoom||'';i.alt=b.dataset.zoomAlt||'';}"
        "z.classList.add('on');return;}"
        "if(e.target===z||z.contains(e.target)){unzoom();}});"
        /* Escape closes it, the way it closes the GTK window. */
        "document.addEventListener('keydown',function(e){"
        "if(e.key==='Escape'){unzoom();}});"
        "})();");
    htmx_builder_end(builder);

    htmx_builder_end(builder);  /* head */

    htmx_builder_begin(builder, "body");
    htmx_builder_attr(builder, "hx-ext", "sse");
    htmx_builder_attr(builder, "sse-connect", "/events");

    /*
     * One picture viewer for the whole document.
     *
     * Here rather than beside each face because a run header is drawn
     * once per run: a conversation with thirty turns from one agent
     * would otherwise carry thirty elements with the same id, which is
     * invalid and which the first `getElementById` resolves in favour
     * of whichever came first.
     *
     * It is also outside the transcript on purpose.  htmx swaps that
     * element whole on every fleet event, and an overlay somebody had
     * open would vanish mid-look.
     */
    htmx_builder_begin(builder, "div");
    htmx_builder_attr(builder, "id", "avatar-zoom");
    htmx_builder_begin(builder, "img");
    htmx_builder_attr(builder, "alt", "");
    htmx_builder_end(builder);
    htmx_builder_end(builder);
}

static void
close_document(HtmxBuilder *builder)
{
    htmx_builder_end(builder);  /* body */
    htmx_builder_end(builder);  /* html */
}

gchar *
clawt_web_page(ClawtWebApp *app, const gchar *agent_id, ClawtPage view,
               HtmxElement *body, HtmxRequest *request)
{
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autoptr(HtmxDiv) frame = htmx_div_new();
    g_autoptr(HtmxDiv) content = htmx_div_new();
    g_autofree gchar *title = NULL;

    title = (agent_id != NULL)
            ? g_strdup_printf("%s · %s · clawtilla", agent_id,
                              clawt_page_label(view))
            : g_strdup("clawtilla");

    /*
     * Which conversation is being read, so an arrival in it does not
     * accrue a count -- the same rule the GTK client applies from its
     * selected room.  Set before the sidebar is built, because the
     * sidebar is what draws the counts.
     */
    clawt_web_app_set_viewing(app, (view == CLAWT_PAGE_CHAT)
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

    clawt_web_add(frame, clawt_web_sidebar(app, agent_id, view, look));

    htmx_element_add_class(HTMX_ELEMENT(content), "content");
    clawt_web_add(content, clawt_web_topbar(app, agent_id, view));

    /*
     * And the section's own page tabs directly under it.
     *
     * In the shell rather than in each body: the Computer page already
     * builds a row of its own for its four sub-views, and a second
     * implementation of the same row is the pair that drifts.  NULL for
     * a section that is a single page.
     */
    {
        HtmxElement *subnav = clawt_web_section_subnav(agent_id, view);

        if (subnav != NULL)
            clawt_web_add(content, subnav);
    }

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
