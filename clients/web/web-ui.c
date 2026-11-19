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

/*
 * An object member, or NULL.  Here rather than clawt_web_member_object(),
 * which lives with the app and is not linked into the render test.
 */
static JsonObject *
object_member(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static JsonArray *
array_member(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

HtmxSpan *
clawt_web_failover_badge(JsonObject *agent)
{
    g_autoptr(HtmxSpan) badge = NULL;
    g_autofree gchar *text = NULL;
    const gchar *message;

    /*
     * The text and the question "is it on one" are the library's, which
     * the GTK and lrg clients ask too -- three spellings of the badge
     * would differ the first time somebody reworded one.
     */
    text = clawt_failover_badge(object_member(agent, "failover"));

    if (text == NULL)
        return NULL;

    message = clawt_ipc_payload_string(agent, "failover_message");
    badge = clawt_web_badge(text, "warn");
    htmx_element_set_attribute(HTMX_ELEMENT(badge), "title",
                               message != NULL ? message : text);

    return (HtmxSpan *)g_steal_pointer(&badge);
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
             * One of the sheet's own tones, rather than a colour
             * computed here -- so a palette recolours the faces with
             * everything else.  Which tone is clawt_avatar_tone()'s,
             * keyed on the agent's id: the transcript passes the sender
             * and the fleet the display name, and hashing the name made
             * one agent two colours.
             */
            g_autofree gchar *cls = g_strdup_printf(
                "avatar-tone-%u", clawt_avatar_tone(agent_id, name));

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

HtmxElement *
clawt_web_agent_row_menu(const gchar *agent_id, guint unread, gboolean muted)
{
    g_autoptr(HtmxElement) details = NULL;
    g_autoptr(HtmxElement) summary = NULL;
    g_autoptr(HtmxDiv) items = NULL;
    g_autofree gchar *escaped = NULL;

    if (agent_id == NULL)
        return NULL;

    details = HTMX_ELEMENT(htmx_details_new());
    summary = HTMX_ELEMENT(htmx_summary_new());
    items = htmx_div_new();
    escaped = g_uri_escape_string(agent_id, NULL, FALSE);

    htmx_element_add_class(details, "row-menu");
    htmx_element_set_attribute(details, "data-agent", agent_id);

    /*
     * A summary that says what the menu is for, for a reader who opens
     * it with the keyboard; the script opens it on right-click.
     */
    htmx_node_set_text_content(HTMX_NODE(summary), "Actions");
    htmx_element_set_attribute(summary, "aria-label", "Agent actions");
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
    htmx_element_add_class(HTMX_ELEMENT(items), "row-menu-items");

    /*
     * Mark as read, worded with the count -- and present but disabled
     * with nothing to read, as the GTK and lrg menus grey it: the menu
     * always exists now that Mute is in it, and an item that comes and
     * goes moves the one under it.
     */
    {
        g_autofree gchar *action = g_strdup_printf("/a/%s/read", escaped);
        g_autofree gchar *label = clawt_agent_menu_read_label(unread);
        g_autoptr(HtmxButton) read = clawt_web_post_button(label, action,
                                                           "default", NULL);

        if (unread == 0)
            htmx_element_set_attribute(HTMX_ELEMENT(read), "disabled",
                                       "disabled");

        clawt_web_add(items, g_steal_pointer(&read));
    }

    /*
     * Mute notifications / Unmute, one item that flips, in libclawt's
     * words.  A mute holds everywhere: every client's totals and the
     * push and desktop notifiers.
     */
    {
        g_autofree gchar *action = g_strdup_printf(
            muted ? "/a/%s/unmute" : "/a/%s/mute", escaped);
        g_autofree gchar *label = clawt_agent_menu_mute_label(muted);

        clawt_web_add(items, clawt_web_post_button(label, action, "default",
                                                   NULL));
    }

    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(items));

    return g_steal_pointer(&details);
}

/* ── The notification list (web-alerts.c draws the page) ─────────── */

/*
 * Readers for a reply's members, here rather than clawt_web_member() and
 * its siblings, which live with the app and are not linked into the
 * render test -- the reason object_member() above exists too.  A member
 * that is missing, null, or of another type is the fallback.
 */
static const gchar *
notice_string(JsonObject *object, const gchar *name, const gchar *fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return fallback;

    node = json_object_get_member(object, name);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;

    return json_node_get_string(node);
}

static gint64
notice_int(JsonObject *object, const gchar *name, gint64 fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return fallback;

    node = json_object_get_member(object, name);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64)
        return fallback;

    return json_node_get_int(node);
}

static gboolean
notice_bool(JsonObject *object, const gchar *name, gboolean fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return fallback;

    node = json_object_get_member(object, name);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

/* A member array of a notice.list reply, or NULL. */
static JsonArray *
notices_array(JsonObject *reply, const gchar *member)
{
    if (reply == NULL || !json_object_has_member(reply, member) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(reply, member)))
        return NULL;

    return json_object_get_array_member(reply, member);
}

static gboolean
notice_in_scope(const gchar *agent, const gchar *scope)
{
    return scope == NULL || g_strcmp0(agent, scope) == 0;
}

/* `?<key>=<value>`, escaped, on @path. */
static gchar *
notice_action(const gchar *path, const gchar *key, const gchar *value)
{
    g_autofree gchar *escaped = g_uri_escape_string(value != NULL ? value
                                                                  : "",
                                                    NULL, FALSE);

    return g_strdup_printf("%s?%s=%s", path, key, escaped);
}

/*
 * One row: a line, a caption, where it goes, and its menu as the next
 * sibling -- the shape the agent list uses, so the page script's
 * right-click handler opens it the same way.
 */
static void
notice_row(HtmxDiv *list, const gchar *line, const gchar *caption,
           const gchar *href, const gchar *tone, gboolean unread,
           HtmxDiv *items)
{
    g_autoptr(HtmxElement) row = NULL;
    g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
    g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
    g_autoptr(HtmxDiv) text = htmx_div_new();

    row = (href != NULL) ? HTMX_ELEMENT(htmx_a_new_with_href(href))
                         : HTMX_ELEMENT(htmx_div_new());
    htmx_element_add_class(row, "notice-row");
    htmx_element_add_class(row, "alert-row");

    if (tone != NULL)
        htmx_element_add_class(row, tone);

    if (unread)
        htmx_element_add_class(row, "alert-unread");

    htmx_element_add_class(HTMX_ELEMENT(text), "alert-text");
    htmx_node_set_text_content(HTMX_NODE(text), line != NULL ? line : "");
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(text));

    if (caption != NULL && *caption != '\0') {
        g_autoptr(HtmxDiv) meta = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(meta), "alert-meta");
        htmx_element_add_class(HTMX_ELEMENT(meta), "muted");
        htmx_node_set_text_content(HTMX_NODE(meta), caption);
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(meta));
    }

    htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));

    htmx_element_add_class(details, "row-menu");
    htmx_node_set_text_content(HTMX_NODE(summary), "Actions");
    htmx_element_set_attribute(summary, "aria-label", "Notification actions");
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
    htmx_element_add_class(HTMX_ELEMENT(items), "row-menu-items");
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(items));
    htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(details));
}

static void
notice_heading(HtmxDiv *view, const gchar *title, guint count)
{
    g_autofree gchar *text = g_strdup_printf("%s (%u)", title, count);
    g_autoptr(HtmxHeading) heading = htmx_heading_new(3);

    htmx_element_add_class(HTMX_ELEMENT(heading), "notice-section");
    htmx_node_set_text_content(HTMX_NODE(heading), text);
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(heading));
}

/* Appends @list under a heading when it holds anything. */
static void
notice_section(HtmxDiv *view, const gchar *title, guint count, HtmxDiv *list)
{
    if (count == 0)
        return;

    notice_heading(view, title, count);
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(list));
}

static guint
notice_chats(HtmxDiv *view, JsonObject *reply, const gchar *scope)
{
    JsonArray *chats = notices_array(reply, "chats");
    g_autoptr(HtmxDiv) list = htmx_div_new();
    guint count = 0;
    guint i;

    for (i = 0; chats != NULL && i < json_array_get_length(chats); i++) {
        JsonObject *chat = json_array_get_object_element(chats, i);
        const gchar *agent = notice_string(chat, "agent", NULL);
        const gchar *room = notice_string(chat, "room", NULL);
        guint unread = (guint)notice_int(chat, "unread", 0);
        g_autofree gchar *line = NULL;
        g_autofree gchar *href = NULL;
        g_autofree gchar *read = NULL;
        g_autofree gchar *label = NULL;
        g_autoptr(HtmxDiv) items = htmx_div_new();

        if (room == NULL || !notice_in_scope(agent, scope))
            continue;

        if (agent != NULL) {
            href = clawt_web_agent_url(agent, CLAWT_PAGE_CHAT);
        } else {
            g_autofree gchar *escaped = g_uri_escape_string(room, NULL,
                                                            FALSE);

            href = g_strdup_printf("/r/%s", escaped);
        }

        line = g_strdup_printf("%s · %u unread",
                               agent != NULL ? agent
                                   : notice_string(chat, "name", room),
                               unread);
        read = notice_action("/alerts/chat-read", "room", room);
        label = clawt_agent_menu_read_label(unread);
        clawt_web_add(items, clawt_web_post_button(label, read, "default",
                                                   NULL));

        if (agent != NULL) {
            g_autofree gchar *mute = notice_action("/alerts/mute", "agent",
                                                   agent);
            g_autofree gchar *words = clawt_agent_menu_mute_label(FALSE);

            clawt_web_add(items, clawt_web_post_button(words, mute,
                                                       "default", NULL));
        }

        notice_row(list, line, NULL, href, NULL, TRUE, items);
        count++;
    }

    notice_section(view, "Chats", count, list);

    return count;
}

static guint
notice_decisions(HtmxDiv *view, JsonObject *reply, const gchar *scope)
{
    JsonArray *decisions = notices_array(reply, "decisions");
    g_autoptr(HtmxDiv) list = htmx_div_new();
    guint count = 0;
    guint i;

    for (i = 0; decisions != NULL && i < json_array_get_length(decisions);
         i++) {
        JsonObject *decision = json_array_get_object_element(decisions, i);
        const gchar *agent = notice_string(decision, "agent", NULL);
        g_autoptr(HtmxDiv) items = htmx_div_new();
        g_autoptr(HtmxA) open = htmx_a_new_with_href("/decisions");

        if (!notice_in_scope(agent, scope))
            continue;

        htmx_element_add_class(HTMX_ELEMENT(open), "btn");
        htmx_node_set_text_content(HTMX_NODE(open), "Open Decisions");
        htmx_node_add_child(HTMX_NODE(items), HTMX_NODE(open));

        notice_row(list, notice_string(decision, "question", ""), agent,
                   "/decisions", NULL, FALSE, items);
        count++;
    }

    notice_section(view, "Decisions waiting", count, list);

    return count;
}

static guint
notice_grok(HtmxDiv *view, JsonObject *reply, const gchar *scope)
{
    JsonArray *jobs = notices_array(reply, "grok");
    g_autoptr(HtmxDiv) list = htmx_div_new();
    guint count = 0;
    guint i;

    for (i = 0; jobs != NULL && i < json_array_get_length(jobs); i++) {
        JsonObject *job = json_array_get_object_element(jobs, i);
        const gchar *agent = notice_string(job, "agent", NULL);
        const gchar *note = notice_string(job, "note", NULL);
        g_autofree gchar *line = NULL;
        g_autofree gchar *href = NULL;
        g_autofree gchar *escaped = NULL;
        g_autofree gchar *remove = NULL;
        g_autoptr(HtmxDiv) items = htmx_div_new();

        if (!notice_in_scope(agent, scope))
            continue;

        line = g_strdup_printf("%s · %s", notice_string(job, "bot", "?"),
                               notice_string(job, "state", "?"));
        href = clawt_web_agent_url(agent, CLAWT_PAGE_GROK);
        escaped = g_uri_escape_string(notice_string(job, "id", ""), NULL,
                                      FALSE);
        remove = g_strdup_printf("/grok/jobs/%s/remove", escaped);

        /* asked first, in the daemon's words for that job's state */
        clawt_web_add(items, clawt_web_post_button(
            "Remove", remove, "danger",
            notice_string(job, "remove_question",
                             "Remove this job from the Grok page?")));

        notice_row(list, line, note != NULL ? note : agent, href, NULL,
                   FALSE, items);
        count++;
    }

    notice_section(view, "Grok jobs needing attention", count, list);

    return count;
}

static guint
notice_alerts(HtmxDiv *view, JsonObject *reply, const gchar *scope,
              gint64 now_us)
{
    JsonArray *notices = notices_array(reply, "notices");
    g_autoptr(HtmxDiv) list = htmx_div_new();
    guint count = 0;
    guint i;

    for (i = 0; notices != NULL && i < json_array_get_length(notices); i++) {
        JsonObject *notice = json_array_get_object_element(notices, i);
        const gchar *agent = notice_string(notice, "agent", NULL);
        gboolean error = g_strcmp0(notice_string(notice, "tier", ""),
                                   "error") == 0;
        gboolean read = notice_bool(notice, "read", TRUE);
        g_autofree gchar *id = g_strdup_printf(
            "%" G_GINT64_FORMAT, notice_int(notice, "id", 0));
        g_autofree gchar *when = clawt_time_ago_label(
            notice_int(notice, "created_at", 0) * G_USEC_PER_SEC,
            now_us);
        g_autofree gchar *caption = g_strdup_printf(
            "%s%s%s · %s", agent != NULL ? agent : "",
            agent != NULL ? " · " : "", when,
            notice_string(notice, "source", ""));
        g_autofree gchar *dismiss = notice_action("/alerts/dismiss", "id",
                                                  id);
        g_autoptr(HtmxDiv) items = htmx_div_new();

        if (!notice_in_scope(agent, scope))
            continue;

        if (!read) {
            g_autofree gchar *mark = notice_action("/alerts/read", "id", id);

            clawt_web_add(items, clawt_web_post_button("Mark read", mark,
                                                       "default", NULL));
        }

        clawt_web_add(items, clawt_web_post_button("Dismiss", dismiss,
                                                   "default", NULL));

        if (agent != NULL && !notice_bool(notice, "muted", FALSE)) {
            g_autofree gchar *mute = notice_action("/alerts/mute", "agent",
                                                   agent);
            g_autofree gchar *words = clawt_agent_menu_mute_label(FALSE);

            clawt_web_add(items, clawt_web_post_button(words, mute,
                                                       "default", NULL));
        }

        notice_row(list, notice_string(notice, "title", ""), caption, NULL,
                   error ? "alert-error" : "alert-notice", !read, items);
        count++;
    }

    notice_section(view, "Alerts", count, list);

    return count;
}

HtmxElement *
clawt_web_notices_sections(JsonObject  *reply,
                           const gchar *agent,
                           gint64       now_us,
                           guint       *out_shown)
{
    g_autoptr(HtmxDiv) view = htmx_div_new();
    guint shown = 0;

    htmx_element_add_class(HTMX_ELEMENT(view), "notices");

    /*
     * By how much each wants somebody, as every client orders them: a
     * conversation waiting to be read, an agent waiting for an answer,
     * work stuck, and something that happened.
     */
    shown += notice_chats(view, reply, agent);
    shown += notice_decisions(view, reply, agent);
    shown += notice_grok(view, reply, agent);
    shown += notice_alerts(view, reply, agent, now_us);

    if (out_shown != NULL)
        *out_shown = shown;

    return HTMX_ELEMENT(g_steal_pointer(&view));
}

HtmxElement *
clawt_web_mark_all_read_button(guint total)
{
    g_autoptr(HtmxButton) button = NULL;

    if (total == 0)
        return NULL;

    button = clawt_web_post_button("Mark all as read", "/read-all",
                                   "default", NULL);
    htmx_element_add_class(HTMX_ELEMENT(button), "mark-all-read");

    return HTMX_ELEMENT(g_steal_pointer(&button));
}

HtmxForm *
clawt_web_form(const gchar *action)
{
    /*
     * htmx_form_new() and the attributes by hand, not
     * htmx_form_new_with_action(): that one lowercases the method into a
     * string it never frees, a leak per form rendered that LeakSanitizer
     * reports the first time a test draws one.  htmx-glib is a dependency
     * and is not patched from here.
     */
    g_autoptr(HtmxForm) form = htmx_form_new();

    if (action != NULL)
        htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);

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


/* ── The fallback editor ─────────────────────────────────────────── */

/*
 * The providers an agent can run on, from a model.list reply, and
 * @current whatever it is (clawt_web_select_field() adds a value its
 * list does not have rather than replacing it).  A provider with no
 * `agent` member is an older daemon's, and kept: an empty list is a
 * worse failure than one entry the daemon refuses.
 */
static void
fallback_providers(JsonObject *catalog, GPtrArray *ids, GPtrArray *labels)
{
    JsonArray *providers = array_member(catalog, "providers");
    guint i;

    for (i = 0; providers != NULL && i < json_array_get_length(providers); i++) {
        JsonObject *provider = json_array_get_object_element(providers, i);
        const gchar *id = clawt_ipc_payload_string(provider, "id");
        const gchar *label = clawt_ipc_payload_string(provider, "label");

        if (id == NULL ||
            (json_object_has_member(provider, "agent") &&
             !clawt_ipc_payload_boolean(provider, "agent", TRUE)))
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup(label != NULL ? label : id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
}

/*
 * One <datalist> of a provider's catalogue models, so the model is a
 * text field that suggests rather than a select that forbids: the
 * catalogue is curated and goes stale, and every model picker in every
 * client also takes a name typed by hand.  @provider NULL lists every
 * agent provider's models, for the new entry, whose provider is not
 * chosen until it is posted.
 */
static HtmxDatalist *
fallback_models(JsonObject *catalog, const gchar *provider, const gchar *id)
{
    g_autoptr(HtmxDatalist) list = htmx_datalist_new_with_id(id);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    JsonArray *providers = array_member(catalog, "providers");
    guint i;

    for (i = 0; providers != NULL && i < json_array_get_length(providers); i++) {
        JsonObject *object = json_array_get_object_element(providers, i);
        JsonArray *models = array_member(object, "models");
        guint m;

        if (provider != NULL
            ? g_strcmp0(clawt_ipc_payload_string(object, "id"), provider) != 0
            : (json_object_has_member(object, "agent") &&
               !clawt_ipc_payload_boolean(object, "agent", TRUE)))
            continue;

        for (m = 0; models != NULL && m < json_array_get_length(models); m++) {
            JsonObject *model = json_array_get_object_element(models, m);
            const gchar *model_id = clawt_ipc_payload_string(model, "id");
            const gchar *label = clawt_ipc_payload_string(model, "label");
            g_autoptr(HtmxOption) option = NULL;

            if (model_id == NULL || g_hash_table_contains(seen, model_id))
                continue;

            g_hash_table_add(seen, (gpointer)model_id);
            option = htmx_option_new_with_value(model_id,
                                                label != NULL ? label : model_id);
            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(option));
        }
    }

    return (HtmxDatalist *)g_steal_pointer(&list);
}

/*
 * The logins an entry for @provider may name: the CLI's own, the
 * accounts whose provider is that CLI (clawt_model_fallback_account_fits(),
 * the rule every client's picker asks), and @current whatever it is.
 * @provider NULL is the new entry: every account, each saying which CLI
 * it is a login for.
 */
static void
fallback_accounts(JsonObject *accounts, const gchar *provider,
                  const gchar *current, GPtrArray *ids, GPtrArray *labels)
{
    JsonArray *list = array_member(accounts, "accounts");
    guint i;

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(labels, g_strdup("The CLI's own login"));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *account = json_array_get_object_element(list, i);
        const gchar *id = clawt_ipc_payload_string(account, "id");
        const gchar *cli = clawt_ipc_payload_string(account, "provider");
        const gchar *description = clawt_ipc_payload_string(account,
                                                            "description");
        gboolean fits = provider == NULL ||
                        clawt_model_fallback_account_fits(cli, provider);

        if (id == NULL || (!fits && g_strcmp0(id, current) != 0))
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup_printf(
            "%s%s%s%s%s%s", id,
            description != NULL ? " -- " : "",
            description != NULL ? description : "",
            provider == NULL ? " (" : "",
            provider == NULL ? (cli != NULL ? cli : "?") : "",
            provider == NULL ? ")"
                             : (fits ? "" : " (a login for another CLI)")));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
}

/* A submit button that posts which edit it is, as `op`. */
static HtmxButton *
fallback_op_button(const gchar *label, const gchar *variant, const gchar *op,
                   gboolean enabled)
{
    g_autoptr(HtmxButton) button = clawt_web_button(label, variant);

    htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "name", "op");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "value", op);

    if (!enabled)
        htmx_element_set_attribute(HTMX_ELEMENT(button), "disabled",
                                   "disabled");

    return (HtmxButton *)g_steal_pointer(&button);
}

/* The model field: a text box suggesting from @list_id. */
static HtmxDiv *
fallback_model_field(const gchar *name, const gchar *value,
                     const gchar *list_id)
{
    g_autoptr(HtmxDiv) field = htmx_div_new();
    g_autoptr(HtmxLabel) tag = htmx_label_new_with_text("model");
    g_autoptr(HtmxInput) input = htmx_input_new_text(name);

    htmx_element_add_class(HTMX_ELEMENT(field), "field");
    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));

    if (value != NULL)
        htmx_input_set_value(input, value);

    htmx_input_set_placeholder(input, "pick or type a model name");
    htmx_element_set_attribute(HTMX_ELEMENT(input), "list", list_id);
    htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(input));

    return (HtmxDiv *)g_steal_pointer(&field);
}

HtmxDiv *
clawt_web_fallback_card(const gchar *agent_id, const gchar *spelled,
                        const gchar *own_provider, JsonObject *catalog,
                        JsonObject *accounts)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Fallbacks",
        "Where this agent's conversations continue, in this order, when its "
        "own backend hits a usage limit, a rate limit, a sign-out or an "
        "outage. It comes back at the reset. A different CLI or login starts "
        "a fresh conversation; another sign-in to the same subscription is "
        "refused. Each button here saves the list.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = clawt_model_fallback_parse_list(spelled, &error);
    g_autoptr(GPtrArray) provider_ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) provider_labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) lists = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                        g_free, g_free);
    g_autofree gchar *escaped = g_uri_escape_string(agent_id != NULL ? agent_id
                                                                     : "",
                                                    NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/fallbacks", escaped);
    g_autoptr(HtmxForm) form = NULL;
    g_autofree gchar *count = NULL;
    guint i;

    /*
     * A list this page cannot show is not offered for editing: an empty
     * form saved back would write the empty list over the real one.
     */
    if (list == NULL) {
        g_autoptr(HtmxDiv) notice = htmx_div_new();
        g_autofree gchar *why = g_strdup_printf(
            "%s. Fix it in clawtilla.yaml or with `clawtilla agent set`; this "
            "page will not save over a list it cannot show.", error->message);

        htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
        htmx_element_add_class(HTMX_ELEMENT(notice), "notice-bad");
        htmx_node_set_text_content(HTMX_NODE(notice), why);
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(notice));

        return (HtmxDiv *)g_steal_pointer(&card);
    }

    fallback_providers(catalog, provider_ids, provider_labels);
    form = clawt_web_form(action);

    if (list->len == 0)
        clawt_web_add(form, clawt_web_text(
            "No fallbacks: at a limit this agent waits for the reset.",
            "small muted"));

    for (i = 0; i < list->len; i++) {
        ClawtModelFallback *entry = g_ptr_array_index(list, i);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) buttons = htmx_div_new();
        g_autoptr(GPtrArray) account_ids = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) account_labels =
            g_ptr_array_new_with_free_func(g_free);
        g_autofree gchar *heading = g_strdup_printf("Fallback %u", i + 1);
        g_autofree gchar *provider_name = g_strdup_printf("fb_provider_%u", i);
        g_autofree gchar *model_name = g_strdup_printf("fb_model_%u", i);
        g_autofree gchar *account_name = g_strdup_printf("fb_account_%u", i);
        g_autofree gchar *up = g_strdup_printf("up:%u", i);
        g_autofree gchar *down = g_strdup_printf("down:%u", i);
        g_autofree gchar *remove = g_strdup_printf("remove:%u", i);
        const gchar *list_id = g_hash_table_lookup(lists, entry->provider);

        /* One datalist per provider, however many entries use it. */
        if (list_id == NULL) {
            gchar *id = g_strdup_printf("fallback-models-%u",
                                        g_hash_table_size(lists));

            g_hash_table_insert(lists, g_strdup(entry->provider), id);
            clawt_web_add(form, fallback_models(catalog, entry->provider, id));
            list_id = id;
        }

        fallback_accounts(accounts, entry->provider, entry->account,
                          account_ids, account_labels);

        htmx_element_add_class(HTMX_ELEMENT(row), "fallback-entry");
        clawt_web_add(row, clawt_web_text(heading, "small"));
        clawt_web_add(row, clawt_web_select_field(
            "provider", provider_name,
            (const gchar *const *)provider_ids->pdata,
            (const gchar *const *)provider_labels->pdata, entry->provider));
        clawt_web_add(row, fallback_model_field(model_name, entry->model,
                                                list_id));
        clawt_web_add(row, clawt_web_select_field(
            "account", account_name,
            (const gchar *const *)account_ids->pdata,
            (const gchar *const *)account_labels->pdata,
            entry->account != NULL ? entry->account : ""));

        htmx_element_add_class(HTMX_ELEMENT(buttons), "btn-row");
        clawt_web_add(buttons, fallback_op_button("Earlier", "default", up,
                                                  i > 0));
        clawt_web_add(buttons, fallback_op_button("Later", "default", down,
                                                  i + 1 < list->len));
        clawt_web_add(buttons, fallback_op_button("Remove", "danger", remove,
                                                  TRUE));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(buttons));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    count = g_strdup_printf("%u", list->len);
    clawt_web_add(form, clawt_web_hidden("fb_count", count));

    if (list->len > 0) {
        g_autoptr(HtmxDiv) row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        clawt_web_add(row, fallback_op_button("Save fallbacks", "primary",
                                              "save", TRUE));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    /*
     * The entry to add, starting from the agent's own provider: the
     * commonest fallback is a smaller model on the same CLI, or the same
     * model on another login.
     */
    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) buttons = htmx_div_new();
        g_autoptr(GPtrArray) account_ids = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) account_labels =
            g_ptr_array_new_with_free_func(g_free);

        fallback_accounts(accounts, NULL, NULL, account_ids, account_labels);

        htmx_element_add_class(HTMX_ELEMENT(row), "fallback-entry");
        clawt_web_add(row, clawt_web_text("Add a fallback", "small"));
        clawt_web_add(row, fallback_models(catalog, NULL,
                                           "fallback-models-new"));
        clawt_web_add(row, clawt_web_select_field(
            "provider", "fb_new_provider",
            (const gchar *const *)provider_ids->pdata,
            (const gchar *const *)provider_labels->pdata, own_provider));
        clawt_web_add(row, fallback_model_field("fb_new_model", NULL,
                                                "fallback-models-new"));
        clawt_web_add(row, clawt_web_select_field(
            "account", "fb_new_account",
            (const gchar *const *)account_ids->pdata,
            (const gchar *const *)account_labels->pdata, ""));

        htmx_element_add_class(HTMX_ELEMENT(buttons), "btn-row");
        clawt_web_add(buttons, fallback_op_button("Add", "default", "add",
                                                  TRUE));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(buttons));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    return (HtmxDiv *)g_steal_pointer(&card);
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

    /*
     * Installable: the manifest and its icons, all served by this
     * binary (the icons from data/web, vendored), so adding the page to
     * a phone's home screen or as a desktop app fetches nothing from
     * anywhere else.
     *
     * `use-credentials` because a manifest is fetched *without* cookies
     * by default.  On the loopback or a tailnet that changes nothing,
     * but behind a proxy that asks for a login -- which is what the "it
     * has no login" note tells people to put in front of this -- the
     * uncredentialed fetch is refused, and the browser decides the page
     * is not installable without saying why.
     */
    htmx_builder_begin_void(builder, "link");
    htmx_builder_attr(builder, "rel", "manifest");
    htmx_builder_attr(builder, "href", CLAWT_WEB_MANIFEST_PATH);
    htmx_builder_attr(builder, "crossorigin", "use-credentials");
    htmx_builder_end_void(builder);

    htmx_builder_begin_void(builder, "link");
    htmx_builder_attr(builder, "rel", "icon");
    htmx_builder_attr(builder, "type", "image/png");
    htmx_builder_attr(builder, "href", "/static/icons/clawtilla-192.png");
    htmx_builder_end_void(builder);

    htmx_builder_begin_void(builder, "link");
    htmx_builder_attr(builder, "rel", "apple-touch-icon");
    htmx_builder_attr(builder, "href", "/static/icons/clawtilla-180.png");
    htmx_builder_end_void(builder);

    htmx_builder_begin_void(builder, "meta");
    htmx_builder_attr(builder, "name", "theme-color");
    htmx_builder_attr(builder, "content", "#FBFBFA");
    htmx_builder_end_void(builder);

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
        /* Save the composer before leaving: its ordinary one-second draft
         * debounce has not fired when somebody types and immediately jumps. */
        "var navigating=false;"
        "async function navigation_go(url){if(navigating){return;}navigating=true;"
        "var a=document.getElementById('composer-body');"
        "try{if(a&&a.getAttribute('hx-post')){"
        "var r=await fetch(a.getAttribute('hx-post'),{method:'POST',"
        "body:new URLSearchParams({body:a.value})});"
        "if(!r.ok){throw new Error('draft');}}window.location.href=url;"
        "}catch(error){navigating=false;"
        "var n=document.getElementById('navigation-error');"
        "if(!n){n=document.createElement('p');n.id='navigation-error';"
        "n.setAttribute('role','alert');a.parentNode.appendChild(n);}"
        "n.textContent='Could not save your draft. Try Go to again.';}}"
        "document.addEventListener('click',function(e){"
        "var a=e.target.closest?e.target.closest('#navigation-link'):null;"
        "if(a&&e.button===0&&!e.ctrlKey&&!e.metaKey&&!e.shiftKey&&!e.altKey){"
        "e.preventDefault();navigation_go(a.href);}});"
        /* Keyboard access survives fragment replacements through delegation. */
        "document.addEventListener('keydown',function(e){"
        "if(e.ctrlKey&&!e.altKey&&!e.shiftKey&&e.key.toLowerCase()==='k'){"
        "var q=document.getElementById('navigation-query');"
        "var a=document.getElementById('navigation-link');if(!q&&!a){return;}"
        "e.preventDefault();"
        "if(q){q.focus();q.select();return;}"
        "if(a){navigation_go(a.href);}}});"
        /*
         * Right-click on an agent row, or a notification row, opens its
         * menu -- the `<details class="row-menu">` drawn as the row's
         * next sibling -- so the gesture that works in the GTK and lrg
         * clients works here.  Delegated on `document`, because the
         * sidebar is replaced on every fleet event.  A row with no menu
         * keeps the browser's own.  The first item that is not disabled
         * takes the focus.  A click anywhere else, or Escape, closes it.
         */
        "function close_menus(keep){"
        "var all=document.querySelectorAll('details.row-menu[open]');"
        "for(var i=0;i<all.length;i++){if(all[i]!==keep){all[i].open=false;}}}"
        "document.addEventListener('contextmenu',function(e){"
        "var row=e.target.closest?e.target.closest('.agent-row,.notice-row'):null;"
        "if(!row){return;}"
        "var m=row.nextElementSibling;"
        "if(!m||!m.classList.contains('row-menu')){return;}"
        "e.preventDefault();close_menus(m);m.open=true;"
        "var b=m.querySelector('button:not([disabled])');if(b){b.focus();}});"
        "document.addEventListener('click',function(e){"
        "var m=e.target.closest?e.target.closest('details.row-menu'):null;"
        "close_menus(m);});"
        "document.addEventListener('keydown',function(e){"
        "if(e.key==='Escape'){close_menus(null);}});"
        "var seen=-1,mark=false,keep=0,held=-1;"
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
        "seen=at_end(b)?-1:b.querySelectorAll('.msg').length;"
        /*
         * And where a "New messages" rule stands, counted in messages
         * above it.  The daemon draws one on opening a conversation,
         * from its read mark; the mark then moves to the end, so the
         * next fragment comes back without it, and it is put back here
         * at the same place rather than vanishing on the first event.
         */
        "held=-1;var r=document.getElementById('unread-rule');"
        "if(r){var all=b.querySelectorAll('.msg,#unread-rule');held=0;"
        "for(var i=0;i<all.length&&all[i]!==r;i++){held++;}}});"
        /* After: follow, or say what came in while we were not. */
        "document.addEventListener('htmx:afterSwap',function(e){"
        "var b=box();if(!b||e.detail.target.id!=='transcript'){return;}"
        "var msgs=b.querySelectorAll('.msg');"
        "if(held>=0&&held<msgs.length&&!document.getElementById('unread-rule')){"
        "var q=document.createElement('div');"
        "q.className='unread-rule';q.id='unread-rule';"
        "q.appendChild(document.createTextNode('New messages'));"
        "msgs[held].parentNode.insertBefore(q,msgs[held]);}"
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
        "if(mark||document.getElementById('unread-rule')){return;}"
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
         * Return sends; Shift+Return is a newline.
         *
         * The GTK composer has always worked this way and this one
         * never did: a textarea does not submit its form on Return the
         * way a single-line input does, so the only way to send from
         * here was to reach for the mouse -- and Shift+Return, the
         * gesture people use to avoid sending, did the same thing as
         * Return, which is the confusing half.
         *
         * Registered after the completion handler and deferring to it
         * through `defaultPrevented`: both listen on `document`, and
         * preventing the default does not stop the event reaching the
         * next listener.  So a Return that took a highlighted name must
         * not also send the half-written message it just completed.
         */
        "document.addEventListener('keydown',function(e){"
        "var a=e.target;"
        "if(!a||a.id!=='composer-body'||a.tagName!=='TEXTAREA'){return;}"
        "if(e.key!=='Enter'||e.shiftKey||e.ctrlKey||e.metaKey||e.altKey){"
        "return;}"
        /*
         * Mid-composition Return chooses a candidate; it is not a
         * send.  Without this an IME user cannot type a message at
         * all, because the first character they confirm posts it.
         * `keyCode===229` is the older browsers' spelling of the same
         * thing and costs one comparison.
         */
        "if(e.isComposing||e.keyCode===229){return;}"
        "if(e.defaultPrevented){return;}"
        "var f=a.form;if(!f){return;}"
        "e.preventDefault();"
        /*
         * The Send button rather than the form: this form is both a
         * native POST and an htmx one, and clicking the button somebody
         * could have clicked themselves goes down whichever path is
         * actually wired -- where requestSubmit() would depend on htmx
         * seeing the synthetic submit event.
         */
        "var b=f.querySelector('button[type=\"submit\"]');"
        "if(b){b.click();}"
        "else if(f.requestSubmit){f.requestSubmit();}});"
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
         * Files sent with a message: paste, drop, and the chips.
         *
         * Everything goes through the composer's one file input, so
         * what is posted is exactly what the chips show -- the chips
         * are redrawn from `input.files` rather than kept beside it.
         * DataTransfer is the only way a page may set an input's
         * files, which is what lets a pasted screenshot and a dropped
         * PDF join whatever the picker already chose.  Delegated from
         * the document, as everything here is, because htmx replaces
         * the composer on a swap.
         */
        "function picked(){return document.getElementById('composer-files');}"
        "function chips(){var i=picked(),c=document.getElementById("
        "'composer-chips');if(!i||!c){return;}c.textContent='';"
        "Array.prototype.forEach.call(i.files,function(f,n){"
        "var s=document.createElement('span');s.className='file-chip';"
        "s.appendChild(document.createTextNode(f.name));"
        "var x=document.createElement('button');x.type='button';"
        "x.className='file-chip-drop';x.dataset.index=String(n);"
        "x.setAttribute('aria-label','Do not send '+f.name);"
        "x.textContent='\\u00d7';s.appendChild(x);c.appendChild(s);});}"
        "function add_files(list){var i=picked();"
        "if(!i||!list||!list.length||typeof DataTransfer==='undefined'){"
        "return false;}var d=new DataTransfer();"
        "Array.prototype.forEach.call(i.files,function(f){d.items.add(f);});"
        "Array.prototype.forEach.call(list,function(f){d.items.add(f);});"
        "i.files=d.files;chips();return true;}"
        "document.addEventListener('change',function(e){"
        "if(e.target&&e.target.id==='composer-files'){chips();}});"
        "document.addEventListener('click',function(e){"
        "var b=e.target.closest?e.target.closest('.file-chip-drop'):null;"
        "var i=picked();if(!b||!i){return;}e.preventDefault();"
        "var k=parseInt(b.dataset.index,10),d=new DataTransfer();"
        "Array.prototype.forEach.call(i.files,function(f,n){"
        "if(n!==k){d.items.add(f);}});i.files=d.files;chips();});"
        /*
         * A paste with files in it is an attachment; a paste of text
         * is left alone, so copying a paragraph still types it.
         */
        "document.addEventListener('paste',function(e){"
        "if(!e.target||e.target.id!=='composer-body'){return;}"
        "var c=e.clipboardData;if(!c||!c.files||!c.files.length){return;}"
        "if(add_files(c.files)){e.preventDefault();}});"
        "function over_composer(e){return e.target&&e.target.closest&&"
        "e.target.closest('.composer')&&picked();}"
        "document.addEventListener('dragover',function(e){"
        "if(over_composer(e)){e.preventDefault();}});"
        "document.addEventListener('drop',function(e){"
        "if(!over_composer(e)||!e.dataTransfer||"
        "!e.dataTransfer.files.length){return;}"
        "if(add_files(e.dataTransfer.files)){e.preventDefault();}});"
        /*
         * After a send the form is reset, which empties the input but
         * fires before it does -- so the chips are redrawn a tick later.
         */
        "document.addEventListener('reset',function(e){"
        "if(e.target&&e.target.querySelector&&"
        "e.target.querySelector('#composer-files')){setTimeout(chips,0);}});"
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
        /*
         * A folded request's "Show less" closes the <details> it is in,
         * and the exchange panel's Close (or Escape) empties the panel,
         * which the stylesheet hides when it is empty.  Delegated from
         * the document, because the transcript is swapped on every
         * fleet event and a listener bound to a button would go with it.
         */
        "function unexchange(){var x=document.getElementById("
        "'exchange-panel');if(x){x.innerHTML='';}}"
        "document.addEventListener('click',function(e){"
        "var l=e.target.closest?e.target.closest('.fold-less'):null;"
        "if(l){var d=l.closest('details');if(d){d.open=false;}return;}"
        "if(e.target.closest&&e.target.closest('.exchange-close')){"
        "unexchange();}});"
        "document.addEventListener('keydown',function(e){"
        "if(e.key==='Escape'){unexchange();}});"
        "})();");
    htmx_builder_end(builder);

    /*
     * The pairing code's countdown, ticking in the browser.
     *
     * The page carries how many seconds the code had left when it was
     * rendered, not the daemon's clock: a browser on another machine
     * counting down to the daemon's `expires_at` is only right when the
     * two clocks agree.  The text matches clawt_device_countdown_text(),
     * which renders the first value server-side, so nothing jumps when
     * the first tick lands.  Polled from the document rather than bound
     * to the element, because an htmx swap replaces the element.
     */
    htmx_builder_begin(builder, "script");
    htmx_builder_raw_html(builder,
        "(function(){"
        "function tick(){"
        "document.querySelectorAll('[data-pair-expires-in]').forEach("
        "function(n){"
        "if(!n.dataset.pairDeadline){n.dataset.pairDeadline=String("
        "Date.now()+1000*parseInt(n.dataset.pairExpiresIn,10));}"
        "var left=Math.round((parseInt(n.dataset.pairDeadline,10)-"
        "Date.now())/1000);"
        "if(left<=0){n.textContent='expired';"
        "document.querySelectorAll('.pair-code').forEach(function(c){"
        "c.classList.add('expired');});return;}"
        "var s=left%60;n.textContent=Math.floor(left/60)+':'+"
        "(s<10?'0':'')+s;});}"
        "setInterval(tick,1000);"
        "document.addEventListener('htmx:afterSwap',tick);"
        "})();");
    htmx_builder_end(builder);

    /*
     * The host side of an MCP app's view (the MCP Apps extension, over
     * postMessage).  A message counts only when it came from one of the
     * app frames on this page -- its source is that frame's window, the
     * one check an opaque origin leaves, since it cannot be named.  The
     * view is answered `ui/initialize`, handed the tool's input and
     * result once it says it is ready, and resized when it asks.  A
     * `tools/call` is forwarded, as the view sent it, to this app's own
     * route (`data-app-call`), where the web client reads it
     * (clawt_web_app_bridge_parse()) and the daemon runs it through the
     * agent's relay and rules; the JSON-RPC answer comes back whole and
     * is posted into the view.  The fetch is same-origin, made by this
     * page and never by the view, whose origin is opaque.  Every other
     * request -- opening links, writing into the conversation -- is
     * refused.  On `window`, which exists in the head; `document.body`
     * does not.
     */
    htmx_builder_begin(builder, "script");
    htmx_builder_raw_html(builder,
        "(function(){"
        "function find(src){var f=document.querySelectorAll('iframe.app-view');"
        "for(var i=0;i<f.length;i++){if(f[i].contentWindow===src){return f[i];}}"
        "return null;}"
        "function data(f,k){var h=f.closest('.app-frame');"
        "try{return JSON.parse(h.getAttribute(k)||'null');}catch(x){return null;}}"
        "function post(f,m){f.contentWindow.postMessage(m,'*');}"
        "function theme(){var t=document.documentElement.getAttribute('data-theme');"
        "if(t==='dark'||t==='light'){return t;}"
        "return(window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)')"
        ".matches)?'dark':'light';}"
        "window.addEventListener('message',function(e){"
        "var f=find(e.source);if(!f){return;}"
        "var m=e.data;if(!m||typeof m!=='object'||m.jsonrpc!=='2.0'){return;}"
        "if(m.method==='ui/initialize'&&m.id!==undefined){"
        "post(f,{jsonrpc:'2.0',id:m.id,result:{protocolVersion:'2026-01-26',"
        "hostInfo:{name:'clawtilla'},hostCapabilities:{},"
        "hostContext:{theme:theme()}}});return;}"
        "if(m.method==='ui/notifications/initialized'){"
        "post(f,{jsonrpc:'2.0',method:'ui/notifications/tool-input',"
        "params:{arguments:data(f,'data-app-input')||{}}});"
        "var r=data(f,'data-app-result');"
        "if(r){post(f,{jsonrpc:'2.0',method:'ui/notifications/tool-result',"
        "params:r});}return;}"
        "if(m.method==='ui/notifications/size-changed'){"
        "var h=m.params&&m.params.height;if(typeof h==='number'){"
        "f.style.height=Math.max(120,Math.min(1600,Math.round(h)))+'px';}"
        "return;}"
        "if(m.method==='tools/call'&&m.id!==undefined){"
        "var u=f.closest('.app-frame').getAttribute('data-app-call');"
        "if(!u){post(f,{jsonrpc:'2.0',id:m.id,error:{code:-32601,"
        "message:'this app cannot call its server here'}});return;}"
        "fetch(u,{method:'POST',credentials:'same-origin',"
        "headers:{'Content-Type':'application/json'},body:JSON.stringify(m)})"
        ".then(function(r){return r.json();})"
        ".then(function(a){if(a&&a.jsonrpc==='2.0'){post(f,a);}})"
        ".catch(function(){post(f,{jsonrpc:'2.0',id:m.id,error:{code:-32603,"
        "message:'the call did not reach clawtilla'}});});return;}"
        "if(m.id!==undefined&&typeof m.method==='string'){"
        "post(f,{jsonrpc:'2.0',id:m.id,error:{code:-32601,"
        "message:'not supported by clawtilla'}});}"
        "});"
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
        htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Show conversations");
        htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-controls", "sidebar");
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
/* Typed nodes escape names and URLs independently: a destination's label
 * must never become markup, even when its name came from a workspace. */
guint
clawt_web_navigation_result(HtmxElement *list, const gchar *query, const gchar *name,
                  const gchar *detail, const gchar *id, const gchar *url)
{
	g_autofree gchar *search = g_strdup_printf("%s %s %s", name, detail, id);
	g_autoptr(HtmxA) link = NULL;

	if (!clawt_navigation_matches(query, search))
		return 0;
	link = htmx_a_new_with_href(url);
	htmx_element_add_class(HTMX_ELEMENT(link), "navigation-result");
	clawt_web_add(link, clawt_web_text(name, "navigation-name"));
	clawt_web_add(link, clawt_web_text(detail, "muted"));
	htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(link));
	return 1;
}

/* ── Folded requests and the exchange ───────────────────────────── */

gchar *
clawt_web_exchange_url(const gchar *agent_id, const gchar *peer_id,
                       gboolean fragment)
{
    g_autofree gchar *agent = g_uri_escape_string(agent_id != NULL
                                                  ? agent_id : "",
                                                  NULL, FALSE);
    g_autofree gchar *peer = g_uri_escape_string(peer_id != NULL
                                                 ? peer_id : "",
                                                 NULL, FALSE);

    return g_strdup_printf("%s/a/%s/exchange?with=%s",
                           fragment ? "/f" : "", agent, peer);
}

HtmxElement *
clawt_web_fold_element(HtmxElement *body, const gchar *agent_id,
                       const gchar *peer_id, const gchar *message_id,
                       const gchar *text)
{
    g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
    g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
    g_autoptr(HtmxSpan) line = htmx_span_new();
    g_autoptr(HtmxDiv) actions = htmx_div_new();
    g_autoptr(HtmxButton) less = htmx_button_new_with_label(
        clawt_chat_fold_show_less_label());
    g_autofree gchar *preview = clawt_chat_fold_preview(text);
    g_autofree gchar *page = clawt_web_exchange_url(agent_id, peer_id,
                                                    FALSE);
    g_autofree gchar *panel = clawt_web_exchange_url(agent_id, peer_id,
                                                     TRUE);
    g_autoptr(HtmxA) exchange = htmx_a_new_with_href(page);

    htmx_element_add_class(details, "fold");

    /*
     * An id and hx-preserve, or the request closes on every fleet event:
     * the transcript is swapped whole, and a <details> rebuilt from the
     * server is a closed one.  No id, no preserving -- a message the
     * daemon sent without one simply folds again after a swap.
     */
    if (message_id != NULL && *message_id != '\0') {
        g_autofree gchar *id = g_strdup_printf("fold-%s", message_id);

        htmx_element_set_id(details, id);
        htmx_element_set_attribute(details, "hx-preserve", "true");
    }

    /* the preview is plain text: a body's markdown is shown, not run */
    htmx_element_add_class(HTMX_ELEMENT(line), "fold-preview");
    htmx_node_set_text_content(HTMX_NODE(line), preview);
    htmx_element_add_class(summary, "fold-line");
    htmx_element_set_attribute(summary, "title", "Show the whole request");
    htmx_node_add_child(HTMX_NODE(summary), HTMX_NODE(line));
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));

    clawt_web_add(details, body);

    /*
     * "Show less" is a button the page script answers (it closes the
     * <details> around it); with scripting off the summary above still
     * closes it.  type="button" so it is never mistaken for a submit.
     */
    htmx_element_add_class(HTMX_ELEMENT(actions), "fold-actions");
    htmx_element_set_attribute(HTMX_ELEMENT(less), "type", "button");
    htmx_element_add_class(HTMX_ELEMENT(less), "fold-less");
    htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(less));

    htmx_node_set_text_content(HTMX_NODE(exchange),
                               clawt_chat_fold_view_exchange_label());
    htmx_element_add_class(HTMX_ELEMENT(exchange), "fold-exchange");
    htmx_element_set_attribute(HTMX_ELEMENT(exchange), "hx-get", panel);
    htmx_element_set_attribute(HTMX_ELEMENT(exchange), "hx-target",
                               "#exchange-panel");
    htmx_element_set_attribute(HTMX_ELEMENT(exchange), "hx-swap",
                               "innerHTML");
    htmx_node_add_child(HTMX_NODE(actions), HTMX_NODE(exchange));
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(actions));

    return g_steal_pointer(&details);
}

HtmxElement *
clawt_web_exchange_element(JsonArray *exchange, const gchar *agent_id,
                           const gchar *peer_id, gboolean in_panel)
{
    g_autoptr(HtmxDiv) root = htmx_div_new();
    g_autoptr(HtmxDiv) header = htmx_div_new();
    g_autoptr(HtmxElement) list = HTMX_ELEMENT(htmx_ol_new());
    g_autofree gchar *title = clawt_chat_exchange_title(agent_id, peer_id);
    guint length = (exchange != NULL) ? json_array_get_length(exchange) : 0;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(root), "exchange");
    htmx_element_set_attribute(HTMX_ELEMENT(root), "role", "dialog");
    htmx_element_set_attribute(HTMX_ELEMENT(root), "aria-label", title);
    htmx_element_add_class(HTMX_ELEMENT(header), "exchange-head");
    clawt_web_add(header, clawt_web_section_title(title));

    if (in_panel) {
        g_autoptr(HtmxButton) close = htmx_button_new_with_label("Close");

        htmx_element_set_attribute(HTMX_ELEMENT(close), "type", "button");
        htmx_element_add_class(HTMX_ELEMENT(close), "exchange-close");
        htmx_node_add_child(HTMX_NODE(header), HTMX_NODE(close));
    } else {
        g_autofree gchar *escaped_agent = g_uri_escape_string(
            agent_id != NULL ? agent_id : "", NULL, FALSE);
        g_autofree gchar *escaped_peer = g_uri_escape_string(
            peer_id != NULL ? peer_id : "", NULL, FALSE);
        g_autofree gchar *back = g_strdup_printf("/a/%s/chat?with=%s",
                                                 escaped_agent, escaped_peer);
        g_autoptr(HtmxA) link = htmx_a_new_with_href(back);

        htmx_node_set_text_content(HTMX_NODE(link),
                                   "Back to the conversation");
        htmx_node_add_child(HTMX_NODE(header), HTMX_NODE(link));
    }

    htmx_node_add_child(HTMX_NODE(root), HTMX_NODE(header));

    if (length == 0) {
        clawt_web_add(root, clawt_web_text(
            "Nothing between them in this conversation's recent history.",
            "muted"));

        return HTMX_ELEMENT(g_steal_pointer(&root));
    }

    htmx_element_add_class(list, "exchange-list");

    for (i = 0; i < length; i++) {
        JsonObject *message = json_array_get_object_element(exchange, i);
        /*
         * Read with json-glib's own guarded readers rather than
         * clawt_web_member(): this renderer is pure, and the test that
         * renders it links no daemon client.
         */
        const gchar *sender = json_object_get_string_member_with_default(
            message, "sender", "?");
        gint64 ts = json_object_get_int_member_with_default(message, "ts",
                                                            0);
        g_autoptr(GDateTime) at = (ts > 0)
            ? g_date_time_new_from_unix_local(ts) : NULL;
        g_autofree gchar *when = clawt_chat_time_label(at);
        g_autofree gchar *html = clawt_markdown_to_html(
            json_object_get_string_member_with_default(message, "body",
                                                       ""));
        g_autofree gchar *who_text = (when != NULL)
            ? g_strdup_printf("%s · %s", sender, when) : g_strdup(sender);
        g_autoptr(HtmxElement) item = HTMX_ELEMENT(htmx_li_new());
        g_autoptr(HtmxDiv) who = htmx_div_new();
        g_autoptr(HtmxDiv) text = htmx_div_new();

        /* the peer's requests set apart from this agent's answers */
        htmx_element_add_class(item, g_strcmp0(sender, peer_id) == 0
                                     ? "exchange-request"
                                     : "exchange-answer");
        htmx_element_add_class(HTMX_ELEMENT(who), "exchange-who");
        htmx_node_set_text_content(HTMX_NODE(who), who_text);
        htmx_element_add_class(HTMX_ELEMENT(text), "msg-body");
        htmx_node_set_html_content(HTMX_NODE(text), html);
        htmx_node_add_child(HTMX_NODE(item), HTMX_NODE(who));
        htmx_node_add_child(HTMX_NODE(item), HTMX_NODE(text));
        htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(item));
    }

    htmx_node_add_child(HTMX_NODE(root), HTMX_NODE(list));

    return HTMX_ELEMENT(g_steal_pointer(&root));
}

/* ── The web app manifest ────────────────────────────────────────── */

gchar *
clawt_web_manifest_json(void)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    static const struct {
        const gchar *src;
        const gchar *sizes;
        const gchar *purpose;
    } icons[] = {
        { "/static/icons/clawtilla-192.png", "192x192", "any" },
        { "/static/icons/clawtilla-512.png", "512x512", "any" },
        { "/static/icons/clawtilla-512.png", "512x512", "maskable" },
    };
    guint i;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "clawtilla");
    json_builder_set_member_name(builder, "short_name");
    json_builder_add_string_value(builder, "clawtilla");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "The clawtilla fleet: agents, rooms and their computers");

    /*
     * Root-relative, never an absolute URL: the client is reached at
     * the loopback, a tailnet address and whatever a proxy calls it, and
     * the manifest must be right at each.  The id is fixed so an
     * installed app survives a change of start page.
     */
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, "/");
    json_builder_set_member_name(builder, "start_url");
    json_builder_add_string_value(builder, "/");
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(builder, "/");
    json_builder_set_member_name(builder, "display");
    json_builder_add_string_value(builder, "standalone");

    /* the light canvas, as the stylesheet's :root spells it */
    json_builder_set_member_name(builder, "background_color");
    json_builder_add_string_value(builder, "#FBFBFA");
    json_builder_set_member_name(builder, "theme_color");
    json_builder_add_string_value(builder, "#FBFBFA");

    json_builder_set_member_name(builder, "icons");
    json_builder_begin_array(builder);

    for (i = 0; i < G_N_ELEMENTS(icons); i++) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "src");
        json_builder_add_string_value(builder, icons[i].src);
        json_builder_set_member_name(builder, "sizes");
        json_builder_add_string_value(builder, icons[i].sizes);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "image/png");
        json_builder_set_member_name(builder, "purpose");
        json_builder_add_string_value(builder, icons[i].purpose);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);

    return json_generator_to_data(generator, NULL);
}

HtmxResponse *
clawt_web_manifest_response(void)
{
    g_autofree gchar *json = clawt_web_manifest_json();
    HtmxResponse *response = htmx_response_new_with_content(json);

    htmx_response_set_content_type(response, CLAWT_WEB_MANIFEST_TYPE);

    /*
     * An hour: long enough that a phone does not re-ask on every page,
     * short enough that an upgraded client's manifest is seen the same
     * day.
     */
    htmx_response_add_header(response, "Cache-Control", "max-age=3600");

    return response;
}

/*
 * One decision, drawn inline in the room it was asked from.
 *
 * The same record the inbox lists, through the same library sentences
 * (clawt_decision_default_text(), clawt_decision_outcome_text()), so a
 * person who answered on one page reads the same words on the other.
 * The form swaps only the card: the answer comes back as this card,
 * redrawn from the daemon's reply -- settled, or still open with the
 * refusal written into it -- and the transcript's own refresh on the
 * fleet event that follows draws the same thing again.
 */
HtmxElement *
clawt_web_decision_inline(JsonObject *object, const gchar *notice)
{
    g_autoptr(HtmxDiv) card = htmx_div_new();
    g_autoptr(ClawtDecision) decision = clawt_decision_new_from_object(object);
    g_autofree gchar *who = NULL;
    g_autofree gchar *dom_id = NULL;
    const gchar * const *options;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(card), "decision-inline");

    if (decision == NULL)
        return HTMX_ELEMENT(g_steal_pointer(&card));

    /* an id the swap can find again; the uuid is daemon data, so escaped */
    dom_id = g_strdup_printf("decision-%s", clawt_decision_get_id(decision));
    htmx_element_set_id(HTMX_ELEMENT(card), dom_id);

    who = g_strdup_printf("%s asks", clawt_decision_get_agent(decision));
    clawt_web_add(card, clawt_web_text(who, "small muted"));

    /* Who it was put to first, as the inbox says it. */
    {
        g_autofree gchar *routed = clawt_decision_routed_text(decision);

        if (routed != NULL)
            clawt_web_add(card, clawt_web_text(routed, "small muted"));
    }
    clawt_web_add(card, clawt_web_text(clawt_decision_get_question(decision),
                                       "decision-question"));

    if (notice != NULL && *notice != '\0')
        clawt_web_add(card, clawt_web_text(notice, "error"));

    if (clawt_decision_get_state(decision) != CLAWT_DECISION_OPEN) {
        g_autofree gchar *outcome = clawt_decision_outcome_text(decision);

        htmx_element_add_class(HTMX_ELEMENT(card), "decision-inline-settled");
        clawt_web_add(card, clawt_web_text(outcome, "small"));

        return HTMX_ELEMENT(g_steal_pointer(&card));
    }

    options = clawt_decision_get_options(decision);

    if (options == NULL || options[0] == NULL) {
        /*
         * A free-text question cannot be answered with a button; the
         * inbox has the box for it, and says so rather than drawing a
         * card with nothing to press.
         */
        g_autoptr(HtmxA) inbox = htmx_a_new_with_href("/decisions");

        htmx_node_set_text_content(HTMX_NODE(inbox),
                                   "No options offered: answer it in Decisions");
        htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(inbox));
    } else {
        g_autoptr(HtmxForm) form = clawt_web_form("/f/decision/answer");
        g_autoptr(HtmxDiv) buttons = htmx_div_new();

        /*
         * The card, not the page: an answer redraws this one entry and
         * leaves the transcript where the reader had it.  hx-sync drops
         * a second press while the first is in flight and every button
         * goes disabled for the wait -- the client's half of refusing a
         * double tap; the daemon refuses whatever still gets through.
         */
        /*
         * `closest`, not `#id`: the id is daemon data, and a selector
         * built from it is one more thing that would need escaping --
         * one that does not parse makes htmx swap nothing at all.
         */
        htmx_element_set_hx_target(HTMX_ELEMENT(form),
                                   "closest .decision-inline");
        htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-swap", "outerHTML");
        htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-sync", "this:drop");
        htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-disabled-elt",
                                   "find button");

        clawt_web_add(form, clawt_web_hidden("decision",
                                             clawt_decision_get_id(decision)));
        clawt_web_add(form, clawt_web_hidden("via", CLAWT_DECISION_VIA_CHAT));

        htmx_element_add_class(HTMX_ELEMENT(buttons), "decision-options");

        for (i = 0; options[i] != NULL; i++) {
            g_autoptr(HtmxButton) pick = clawt_web_button(options[i], "default");

            htmx_element_set_attribute(HTMX_ELEMENT(pick), "type", "submit");
            htmx_element_set_attribute(HTMX_ELEMENT(pick), "name", "answer");
            htmx_element_set_attribute(HTMX_ELEMENT(pick), "value", options[i]);
            htmx_node_add_child(HTMX_NODE(buttons), HTMX_NODE(pick));
        }

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(buttons));
        htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(form));
    }

    {
        g_autofree gchar *fallback = clawt_decision_default_text(decision, NULL);

        if (fallback != NULL)
            clawt_web_add(card, clawt_web_text(fallback, "small muted"));
    }

    return HTMX_ELEMENT(g_steal_pointer(&card));
}

/* ── Connector budget and apps ───────────────────────────────────── */

/* A Save button in a row of its own, as every small form here ends. */
static void
add_save_row(HtmxForm *form)
{
    g_autoptr(HtmxDiv) buttons = htmx_div_new();
    g_autoptr(HtmxButton) save = clawt_web_button("Save", "default");

    htmx_element_add_class(HTMX_ELEMENT(buttons), "btn-row");
    htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
    htmx_node_add_child(HTMX_NODE(buttons), HTMX_NODE(save));
    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(buttons));
}

/*
 * The connector's own result budget, -1 for the fleet's and 0 for none,
 * with what that comes to beside it -- libclawt's label, so every client
 * says "64 KB" the same way.
 */
HtmxElement *
clawt_web_budget_form(const gchar *escaped, gint64 own,
                      const gchar *effective)
{
    g_autofree gchar *action = g_strdup_printf(
        "/settings/connectors/%s/budget", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, own);
    g_autofree gchar *line = g_strdup_printf(
        "Now: %s. The most of one tool result the agent is shown, in "
        "bytes; a bigger one is trimmed, announced at both ends and kept "
        "whole for clawtilla_mcp_result_query. -1 uses the fleet's "
        "budget, 0 means none.", effective != NULL ? effective : "?");

    htmx_element_add_class(HTMX_ELEMENT(form), "budget-form");
    clawt_web_add(form, clawt_web_field("Result budget (bytes)",
                                        "max_result_bytes", value, "-1"));
    clawt_web_add(form, clawt_web_text(line, "small muted"));
    add_save_row(form);

    return HTMX_ELEMENT(g_steal_pointer(&form));
}

/*
 * "Show its apps in chat": an MCP server's own view for a tool, drawn in
 * a sandboxed frame in the conversation when somebody opens it.
 */
HtmxElement *
clawt_web_apps_form(const gchar *escaped, gboolean on)
{
    g_autofree gchar *action = g_strdup_printf(
        "/settings/connectors/%s/apps", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);

    htmx_element_add_class(HTMX_ELEMENT(form), "apps-form");
    clawt_web_add(form, clawt_web_switch_field(
        "Show its apps in chat", "show_apps",
        "When one of its tools ships an interactive view, the call appears "
        "in the conversation as an app you can open. It runs in a sandboxed "
        "frame with no access to this page; it is the server's own code, "
        "which is why this is off until you turn it on.", on));
    add_save_row(form);

    return HTMX_ELEMENT(g_steal_pointer(&form));
}

/* The fleet's budget, for every connector that does not set its own. */
HtmxElement *
clawt_web_fleet_budget(gint64 value)
{
    g_autoptr(HtmxForm) form = clawt_web_form(
        "/settings/connectors/directory-thresholds");
    g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);
    g_autofree gchar *fallback = g_strdup_printf(
        "%" G_GINT64_FORMAT, clawt_mcp_result_default_budget());

    htmx_element_add_class(HTMX_ELEMENT(form), "fleet-budget");
    clawt_web_add(form, clawt_web_field(
        "Result budget for every connector (bytes, 0 for none)",
        "max_result_bytes", text, fallback));
    add_save_row(form);

    return HTMX_ELEMENT(g_steal_pointer(&form));
}

/* ── Steps, and MCP apps in the transcript ───────────────────────── */

HtmxElement *
clawt_web_app_card(const gchar *agent_id, const gchar *app_id,
                   const gchar *text)
{
    g_autoptr(HtmxDiv) card = htmx_div_new();
    g_autoptr(HtmxDiv) body = htmx_div_new();
    g_autoptr(HtmxButton) open = clawt_web_button("Open app", "default");
    g_autofree gchar *path = NULL;
    g_autofree gchar *frame = NULL;
    g_autofree gchar *card_id = NULL;
    g_autofree gchar *body_id = NULL;
    g_autofree gchar *target = NULL;

    htmx_element_add_class(HTMX_ELEMENT(card), "turn-step");
    htmx_element_add_class(HTMX_ELEMENT(card), "app-card");
    clawt_web_add(card, clawt_web_text(text != NULL ? text : "App",
                                       "app-title"));

    /* An id that is not one of ours is drawn as text, never as a URL. */
    if (agent_id == NULL || !clawt_mcp_app_id_is_valid(app_id)) {
        clawt_web_add(card, clawt_web_text("This app cannot be opened.",
                                           "small muted"));
        return HTMX_ELEMENT(g_steal_pointer(&card));
    }

    path = clawt_mcp_app_web_path(agent_id, app_id);
    frame = g_strconcat(path, "/frame", NULL);
    card_id = g_strconcat("app-", app_id, NULL);
    body_id = g_strconcat("app-body-", app_id, NULL);
    target = g_strconcat("#", body_id, NULL);

    /*
     * Kept across transcript swaps: htmx replaces the transcript whole on
     * every fleet event, and an app somebody is using would close under
     * them.  The id makes the card the same element in the next swap.
     */
    htmx_element_set_id(HTMX_ELEMENT(card), card_id);
    htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-preserve", "true");

    htmx_element_add_class(HTMX_ELEMENT(body), "app-body");
    htmx_element_set_id(HTMX_ELEMENT(body), body_id);

    /*
     * A button, not a frame: the page fetches nothing at load, and the
     * view is fetched only when somebody asks for it.
     */
    htmx_element_add_class(HTMX_ELEMENT(open), "app-open");
    htmx_element_set_attribute(HTMX_ELEMENT(open), "hx-get", frame);
    htmx_element_set_attribute(HTMX_ELEMENT(open), "hx-target", target);
    htmx_element_set_attribute(HTMX_ELEMENT(open), "hx-swap", "innerHTML");
    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(open));

    {
        g_autoptr(HtmxA) page = htmx_a_new_with_href(path);

        htmx_element_add_class(HTMX_ELEMENT(page), "small");
        htmx_node_set_text_content(HTMX_NODE(page), "Open on its own page");
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(page));
    }

    htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(body));

    return HTMX_ELEMENT(g_steal_pointer(&card));
}

static gchar *
member_json(JsonObject *object, const gchar *name)
{
    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    return json_to_string(json_object_get_member(object, name), FALSE);
}

HtmxElement *
clawt_web_app_frame(JsonObject *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    const gchar *document = NULL;
    const gchar *title = NULL;
    const gchar *tool = NULL;
    const gchar *connector = NULL;
    const gchar *blocked = NULL;
    g_autofree gchar *source = NULL;

    htmx_element_add_class(HTMX_ELEMENT(box), "app-frame");

    if (app != NULL) {
        document = json_object_get_string_member_with_default(app, "document",
                                                              NULL);
        title = json_object_get_string_member_with_default(app, "title", NULL);
        tool = json_object_get_string_member_with_default(app, "tool", NULL);
        connector = json_object_get_string_member_with_default(
            app, "connector", NULL);
        blocked = json_object_get_string_member_with_default(app, "blocked",
                                                             NULL);
    }

    source = (connector != NULL && tool != NULL)
        ? g_strdup_printf("From %s's %s.", connector, tool)
        : g_strdup(tool != NULL ? tool : "");

    if (document == NULL || *document == '\0') {
        g_autofree gchar *why = g_strdup_printf(
            "This app's view could not be shown: %s.",
            blocked != NULL ? blocked : "it was not kept");

        htmx_element_add_class(HTMX_ELEMENT(box), "app-blocked");
        clawt_web_add(box, clawt_web_text(why, NULL));
        clawt_web_add(box, clawt_web_text(source, "small muted"));

        return HTMX_ELEMENT(g_steal_pointer(&box));
    }

    /*
     * What the page's host bridge hands the view once it says it is
     * ready.  In attributes, which the renderer escapes, rather than in a
     * script element, whose content is raw text a `</script>` in a tool
     * result would end.
     */
    {
        g_autofree gchar *input = member_json(app, "input");
        g_autofree gchar *result = member_json(app, "result");
        const gchar *agent_id = json_object_get_string_member_with_default(
            app, "agent", NULL);
        const gchar *app_id = json_object_get_string_member_with_default(
            app, "id", NULL);

        /*
         * Where the bridge sends the view's own tools/call: this app's
         * route, built only from an id of the shape the daemon makes.
         */
        if (agent_id != NULL && clawt_mcp_app_id_is_valid(app_id)) {
            g_autofree gchar *path = clawt_mcp_app_web_path(agent_id, app_id);
            g_autofree gchar *call = g_strconcat(path, "/call", NULL);

            htmx_element_set_attribute(HTMX_ELEMENT(box), "data-app-call",
                                       call);
        }

        if (input != NULL)
            htmx_element_set_attribute(HTMX_ELEMENT(box), "data-app-input",
                                       input);

        if (result != NULL)
            htmx_element_set_attribute(HTMX_ELEMENT(box), "data-app-result",
                                       result);
    }

    {
        g_autoptr(HtmxIframe) frame = htmx_iframe_new();

        /*
         * Exactly this sandbox and nothing more: scripts, and an opaque
         * origin -- no allow-same-origin, so the view cannot reach this
         * page, its cookies or the fleet it drives; no forms, popups or
         * top navigation; and no `allow` attribute, so no device either.
         * The document is the daemon's, its policy the first element.
         */
        htmx_element_add_class(HTMX_ELEMENT(frame), "app-view");
        htmx_iframe_set_sandbox(frame, CLAWT_MCP_APP_IFRAME_SANDBOX);
        htmx_iframe_set_srcdoc(frame, document);
        htmx_element_set_attribute(HTMX_ELEMENT(frame), "referrerpolicy",
                                   "no-referrer");
        htmx_element_set_attribute(HTMX_ELEMENT(frame), "title",
                                   title != NULL ? title
                                   : (tool != NULL ? tool : "MCP app"));
        htmx_element_set_attribute(HTMX_ELEMENT(frame), "loading", "lazy");
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(frame));
    }

    if (app != NULL && json_object_has_member(app, "refused") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(app, "refused"))) {
        JsonArray *refused = json_object_get_array_member(app, "refused");
        g_autoptr(GString) list = g_string_new(NULL);
        guint i;

        for (i = 0; i < json_array_get_length(refused); i++) {
            JsonNode *node = json_array_get_element(refused, i);

            if (!JSON_NODE_HOLDS_VALUE(node) ||
                json_node_get_value_type(node) != G_TYPE_STRING)
                continue;

            if (list->len > 0)
                g_string_append(list, ", ");

            g_string_append(list, json_node_get_string(node));
        }

        if (list->len > 0) {
            g_autofree gchar *note = g_strdup_printf(
                "Not allowed to reach: %s.", list->str);

            clawt_web_add(box, clawt_web_text(note, "small muted"));
        }
    }

    {
        g_autofree gchar *note = g_strdup_printf(
            "%s It runs sandboxed, with no access to this page.", source);

        clawt_web_add(box, clawt_web_text(note, "small muted"));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* A JSON-RPC error answer, with @id when the message had one. */
static gchar *
bridge_error(JsonNode *id, gint code, const gchar *message)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) root = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    if (id != NULL)
        json_builder_add_value(builder, json_node_copy(id));
    else
        json_builder_add_null_value(builder);
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);

    return json_to_string(root, FALSE);
}

gboolean
clawt_web_app_bridge_parse(const gchar  *body,
                           gchar       **tool,
                           JsonNode    **arguments,
                           JsonNode    **id,
                           gchar       **refusal)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonNode) the_id = NULL;
    JsonObject *message;
    JsonObject *params;
    JsonNode *root;
    JsonNode *args;
    const gchar *method;
    const gchar *name;

    *tool = NULL;
    *arguments = NULL;
    *id = NULL;
    *refusal = NULL;

    if (body == NULL || !json_parser_load_from_data(parser, body, -1, NULL) ||
        (root = json_parser_get_root(parser)) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(root) ||
        (message = json_node_get_object(root)) == NULL) {
        *refusal = bridge_error(NULL, -32700, "not a JSON-RPC message");
        return FALSE;
    }

    if (json_object_has_member(message, "id") &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(message, "id")))
        the_id = json_node_copy(json_object_get_member(message, "id"));

    method = json_object_get_string_member_with_default(message, "method",
                                                        "");

    /*
     * The two the extension defines and clawtilla decided against, named
     * so the view can say why: nothing in a view writes into a
     * conversation, and nothing in it sends the person to an address of
     * its choosing -- the frame has no popups for the same reason.
     */
    if (g_strcmp0(method, "ui/message") == 0) {
        *refusal = bridge_error(the_id, -32601, "clawtilla does not let an "
                                "app write into the conversation");
        return FALSE;
    }

    if (g_strcmp0(method, "ui/open-link") == 0) {
        *refusal = bridge_error(the_id, -32601, "clawtilla does not open "
                                "links for an app; show the address so the "
                                "person can copy it");
        return FALSE;
    }

    if (g_strcmp0(method, "tools/call") != 0) {
        *refusal = bridge_error(the_id, -32601, "not supported by clawtilla");
        return FALSE;
    }

    if (the_id == NULL) {
        *refusal = bridge_error(NULL, -32600, "a call needs an id to be "
                                "answered");
        return FALSE;
    }

    params = (json_object_has_member(message, "params") &&
              JSON_NODE_HOLDS_OBJECT(json_object_get_member(message,
                                                            "params")))
        ? json_object_get_object_member(message, "params") : NULL;
    name = (params != NULL)
        ? json_object_get_string_member_with_default(params, "name", NULL)
        : NULL;

    if (name == NULL || *name == '\0') {
        *refusal = bridge_error(the_id, -32602, "a call names a tool");
        return FALSE;
    }

    args = json_object_has_member(params, "arguments")
        ? json_object_get_member(params, "arguments") : NULL;

    if (args != NULL && !JSON_NODE_HOLDS_OBJECT(args) &&
        !JSON_NODE_HOLDS_NULL(args)) {
        *refusal = bridge_error(the_id, -32602, "a call's arguments are an "
                                "object");
        return FALSE;
    }

    *tool = g_strdup(name);
    *arguments = (args != NULL && JSON_NODE_HOLDS_OBJECT(args))
        ? json_node_copy(args) : NULL;
    *id = g_steal_pointer(&the_id);

    return TRUE;
}

gchar *
clawt_web_app_bridge_response(JsonNode    *id,
                              JsonObject  *result,
                              const gchar *error)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonNode) root = NULL;

    if (result == NULL)
        return bridge_error(id, -32603, error != NULL ? error
                                                      : "the call failed");

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    if (id != NULL)
        json_builder_add_value(builder, json_node_copy(id));
    else
        json_builder_add_null_value(builder);
    json_builder_set_member_name(builder, "result");
    {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);

        json_node_set_object(node, result);
        json_builder_add_value(builder, node);
    }
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);

    return json_to_string(root, FALSE);
}

HtmxElement *
clawt_web_step_element(ClawtTurnStep *step)
{
    g_autofree gchar *text = clawt_turn_step_summary(step);
    g_autofree gchar *marked = NULL;
    g_autofree gchar *css = NULL;

    if (clawt_turn_step_get_app_id(step) != NULL)
        return clawt_web_app_card(clawt_turn_step_get_agent_id(step),
                                  clawt_turn_step_get_app_id(step),
                                  clawt_turn_step_get_text(step));

    if (clawt_turn_step_get_kind(step) == CLAWT_STEP_THINKING)
        marked = g_strdup_printf("thinking: %s", text);

    /*
     * A trimmed result is a warning beside the tool calls, not backend
     * chatter: the agent saw part of what the tool returned.
     */
    if (clawt_turn_step_get_kind(step) == CLAWT_STEP_STATUS &&
        clawt_mcp_result_is_notice(clawt_turn_step_get_text(step)))
        css = g_strdup_printf("turn-step %s trimmed-notice",
                              clawt_turn_step_tone(step));
    else
        css = g_strdup_printf("turn-step %s", clawt_turn_step_tone(step));

    return HTMX_ELEMENT(clawt_web_text(marked != NULL ? marked : text, css));
}

/* ── Drop-downs over libclawt's fixed lists (0.51.3) ─────────────── */

/* A select from parallel value and label arrays, then freed. */
static HtmxDiv *
select_from(const gchar *label, const gchar *name, GPtrArray *values,
            GPtrArray *labels, const gchar *current)
{
    g_ptr_array_add(values, NULL);
    g_ptr_array_add(labels, NULL);
    return clawt_web_select_field(label, name,
                                  (const gchar *const *)values->pdata,
                                  (const gchar *const *)labels->pdata,
                                  current);
}

HtmxDiv *
clawt_web_effort_field(const gchar *name, const gchar *current)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    for (i = 0; i < clawt_effort_count(); i++) {
        g_ptr_array_add(values, (gpointer)clawt_effort_nth(i));
        g_ptr_array_add(labels, (gpointer)clawt_effort_nth_label(i));
    }
    return select_from("Effort", name, values, labels,
                       current != NULL ? current : "");
}

HtmxDiv *
clawt_web_weekday_field(const gchar *name, const gchar *current)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    for (i = 0; i < clawt_weekday_count(); i++) {
        g_ptr_array_add(values, (gpointer)clawt_weekday_nth(i));
        g_ptr_array_add(labels, (gpointer)clawt_weekday_nth_label(i));
    }
    return select_from("Weekday", name, values, labels, current);
}

HtmxDiv *
clawt_web_schedule_field(const gchar *name, const gchar *current)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    for (i = 0; i < clawt_schedule_count(); i++) {
        g_ptr_array_add(values, (gpointer)clawt_schedule_nth(i));
        g_ptr_array_add(labels, (gpointer)clawt_schedule_nth_label(i));
    }
    return select_from("Schedule", name, values, labels, current);
}

HtmxDiv *
clawt_web_resolution_field(const gchar *label, const gchar *name,
                           const gchar *current)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    guint i;

    for (i = 0; i < clawt_resolution_count(); i++)
        g_ptr_array_add(values, (gpointer)clawt_resolution_nth(i));
    g_ptr_array_add(values, NULL);
    return clawt_web_select_field(label, name,
                                  (const gchar *const *)values->pdata, NULL,
                                  current);
}

HtmxDiv *
clawt_web_review_by_field(const gchar *name, const gchar *const *agents,
                          gboolean project, const gchar *current)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; i < clawt_review_by_count(); i++) {
        g_ptr_array_add(values, (gpointer)clawt_review_by_nth(i));
        g_ptr_array_add(labels,
                        g_strdup(clawt_review_by_nth_label(i, project)));
    }
    for (i = 0; agents != NULL && agents[i] != NULL; i++) {
        g_ptr_array_add(values, (gpointer)agents[i]);
        g_ptr_array_add(labels, clawt_review_by_agent_label(agents[i]));
    }
    g_ptr_array_add(values, NULL);
    g_ptr_array_add(labels, NULL);
    return clawt_web_select_field("Reviewed by", name,
                                  (const gchar *const *)values->pdata,
                                  (const gchar *const *)labels->pdata,
                                  current != NULL ? current : "");
}
