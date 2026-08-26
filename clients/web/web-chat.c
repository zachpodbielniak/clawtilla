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

/*
 * The `clawt:<id>` entries in an attachment block, and the prose without
 * it.
 *
 * The marker is CLAWT_ATTACHMENT_MARKER, from libclawt, so this client
 * and the GTK one recognise the same line -- one that spelled it
 * differently would draw no attachments and say nothing about why.
 *
 * Returns: (transfer full) (nullable) (element-type utf8): the ids
 */
static GPtrArray *
attachment_ids(const gchar *body, gchar **out_prose)
{
    g_auto(GStrv) lines = NULL;
    g_autoptr(GString) prose = NULL;
    GPtrArray *ids = NULL;
    gboolean in_block = FALSE;
    gsize i;

    *out_prose = NULL;

    if (body == NULL || strstr(body, CLAWT_ATTACHMENT_MARKER) == NULL)
        return NULL;

    lines = g_strsplit(body, "\n", -1);
    prose = g_string_new(NULL);

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *at;

        if (strstr(lines[i], CLAWT_ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block) {
            if (prose->len > 0)
                g_string_append_c(prose, '\n');

            g_string_append(prose, lines[i]);
            continue;
        }

        /*
         * The block is its list items and their indented continuations.
         * Anything at the left margin ends it, so a line an agent wrote
         * after the files is prose again.
         */
        if (lines[i][0] != '\0' && lines[i][0] != ' ' &&
            lines[i][0] != '-' && lines[i][0] != '\t') {
            in_block = FALSE;
            g_string_append_c(prose, '\n');
            g_string_append(prose, lines[i]);
            continue;
        }

        at = strstr(lines[i], "clawt:");

        if (at == NULL)
            continue;

        if (ids == NULL)
            ids = g_ptr_array_new_with_free_func(g_free);

        {
            gchar *id = g_strdup(at + strlen("clawt:"));

            g_ptr_array_add(ids, g_strchomp(id));
        }
    }

    *out_prose = g_strdup(g_strchomp(prose->str));

    return ids;
}

static gboolean
looks_like_an_image(const gchar *name)
{
    static const gchar *const extensions[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", NULL
    };
    g_autofree gchar *lowered = g_ascii_strdown(name, -1);
    gsize i;

    for (i = 0; extensions[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, extensions[i]))
            return TRUE;
    }

    return FALSE;
}

/*
 * One file, as a picture or as a link.
 *
 * Both point at /f/attachment/<id>, which streams the bytes from the
 * daemon: the browser may be on another machine, so a filesystem path
 * would render as a broken image rather than as an unsupported setup.
 */
static HtmxElement *
attachment_element(const gchar *id)
{
    g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
    g_autofree gchar *url = g_strdup_printf("/f/attachment/%s", escaped);
    g_autofree gchar *name = NULL;
    const gchar *dash = strchr(id, '-');

    name = g_strdup((dash != NULL && dash[1] != '\0') ? dash + 1 : id);

    if (looks_like_an_image(name)) {
        g_autoptr(HtmxImg) picture = htmx_img_new_with_src(url, name);

        htmx_element_add_class(HTMX_ELEMENT(picture), "attachment-image");
        htmx_element_set_attribute(HTMX_ELEMENT(picture), "loading", "lazy");

        return HTMX_ELEMENT(g_steal_pointer(&picture));
    }

    {
        g_autoptr(HtmxA) link = htmx_a_new_with_href(url);

        htmx_element_add_class(HTMX_ELEMENT(link), "attachment-file");
        htmx_element_set_attribute(HTMX_ELEMENT(link), "download", name);
        htmx_node_set_text_content(HTMX_NODE(link), name);

        return HTMX_ELEMENT(g_steal_pointer(&link));
    }
}

/*
 * "Today", "Yesterday" or a weekday, between two rules.
 *
 * The label comes from libclawt so this and the GTK client cannot answer
 * the same date differently.
 */
static HtmxElement *
day_divider(gint64 ts)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxSpan) label = htmx_span_new();
    g_autoptr(GDateTime) when = (ts > 0)
        ? g_date_time_new_from_unix_local(ts)
        : g_date_time_new_now_local();
    g_autofree gchar *text = clawt_chat_day_label(when, NULL);

    htmx_element_add_class(HTMX_ELEMENT(row), "day-divider");
    htmx_node_set_text_content(HTMX_NODE(label), text);
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(label));

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

/*
 * The face beside a run's first message.
 *
 * Initials and a colour derived from the name when nothing is
 * configured, which is what makes an avatar cost no config at all;
 * `agents.color` as the background when it is set.
 *
 * The colour goes through clawt_color_ink(), which refuses anything that
 * is not `#rgb` or `#rrggbb` -- it is spliced into a style attribute and
 * nothing else validates that key.
 */
static HtmxElement *
run_avatar(const gchar *name, const gchar *color)
{
    g_autoptr(HtmxSpan) face = htmx_span_new();
    const gchar *ink = clawt_color_ink(color);
    gunichar first;
    gchar initial[8] = { 0 };

    htmx_element_add_class(HTMX_ELEMENT(face), "msg-avatar");

    first = g_utf8_get_char_validated(name, -1);

    if (first != (gunichar)-1 && first != (gunichar)-2)
        g_unichar_to_utf8(g_unichar_toupper(first), initial);

    htmx_node_set_text_content(HTMX_NODE(face), initial);

    if (ink != NULL) {
        g_autofree gchar *style = g_strdup_printf("background:%s;color:%s",
                                                  color, ink);

        htmx_element_set_attribute(HTMX_ELEMENT(face), "style", style);
    } else {
        /*
         * One of the sheet's own tones, chosen by the name, rather than
         * a colour computed here -- so a palette recolours the avatars
         * with everything else.
         */
        g_autofree gchar *cls = g_strdup_printf("avatar-tone-%u",
                                                g_str_hash(name) % 6);

        htmx_element_add_class(HTMX_ELEMENT(face), cls);
    }

    return HTMX_ELEMENT(g_steal_pointer(&face));
}

static HtmxElement *
message_element(JsonObject *message, const gchar *agent_id,
                gboolean run_start, const gchar *color)
{
    const gchar *sender = clawt_web_member(message, "sender", "?");
    const gchar *body = clawt_web_member(message, "body", "");
    const gchar *task = clawt_web_member(message, "task", NULL);
    gint64 ts = clawt_web_member_int(message, "ts", 0);
    gint64 depth = clawt_web_member_int(message, "depth", 0);
    gboolean from_user = (g_strcmp0(sender, "user") == 0);
    g_autoptr(HtmxElement) row = HTMX_ELEMENT(htmx_article_new());
    g_autoptr(HtmxDiv) who = htmx_div_new();
    g_autoptr(HtmxDiv) text = htmx_div_new();
    g_autofree gchar *when = clawt_web_relative_time(ts);

    (void)agent_id;

    htmx_element_add_class(row, "msg");
    htmx_element_add_class(row, run_start ? "run-start" : "run-cont");

    if (from_user)
        htmx_element_add_class(row, "msg-self");

    htmx_element_add_class(HTMX_ELEMENT(who), "msg-who");

    /*
     * One header per run.  Consecutive messages from one sender carry no
     * name and no face, at the same indent, so the left edge of the text
     * is unbroken down the run -- a column of identical faces is noise
     * rather than identity.  The operator's own turns get neither: the
     * bubble already says who is speaking, and a face on every one of
     * your own messages carries no information.
     */
    if (run_start && !from_user) {
        g_autoptr(HtmxSpan) name = htmx_span_new();

        clawt_web_add(who, run_avatar(sender, color));
        htmx_node_set_text_content(HTMX_NODE(name), sender);
        htmx_node_add_child(HTMX_NODE(who), HTMX_NODE(name));
    }

    if (run_start && *when != '\0') {
        g_autoptr(HtmxSpan) stamp = htmx_span_new();
        g_autofree gchar *dotted = from_user
            ? g_strdup(when)
            : g_strdup_printf(" · %s", when);

        htmx_element_add_class(HTMX_ELEMENT(stamp), "muted");
        htmx_node_set_text_content(HTMX_NODE(stamp), dotted);
        htmx_node_add_child(HTMX_NODE(who), HTMX_NODE(stamp));
    }

    /*
     * A continuation row carries its own time in the gutter, where the
     * face sits on the first row of a run and nothing sits on the rest.
     *
     * The same rule the GTK client applies, for the same reason: a new
     * message inside a run is separated by six pixels more than a
     * paragraph inside one message, which is perceptible and not
     * nameable.  The time says "new message" outright, and the space it
     * uses was already reserved for the avatar.
     */
    if (!run_start && !from_user && ts > 0) {
        g_autoptr(HtmxSpan) stamp = htmx_span_new();
        g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(ts);
        g_autofree gchar *clock = (at != NULL)
            ? g_date_time_format(at, "%H:%M")
            : NULL;

        /*
         * The clock time here, not the relative one the rest of the
         * transcript uses, because this slot is the avatar's column and
         * is 28px wide.  Measured in a real browser: "46s ago" wraps to
         * two lines in it, so a run of continuations drew a column of
         * broken two-line stamps -- and the longer the conversation, the
         * longer the strings get.
         *
         * `HH:MM` is four digits and a colon whatever the age of the
         * message, which is the property the slot needs and the reason
         * the GTK transcript has always used it here.
         */
        htmx_element_add_class(HTMX_ELEMENT(stamp), "msg-time");
        htmx_node_set_text_content(HTMX_NODE(stamp),
                                   clock != NULL ? clock : when);
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(stamp));
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

    /*
     * A continuation carries no header -- unless it has a task or a hop
     * count on it, which are the two things a reader needs per message
     * rather than per run: a delegated reply is otherwise just another
     * line, and a hop count climbing towards max_hops is the only
     * visible difference between a conversation and a loop.
     */
    if (run_start || task != NULL || depth > 1)
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(who));

    htmx_element_add_class(HTMX_ELEMENT(text), "msg-body");

    /*
     * Files an agent sent arrive as a `clawt:<id>` block at the end of
     * the body.  The prose goes in the bubble; the files go beneath it,
     * as pictures where they are pictures and as links otherwise -- and
     * the block itself is taken out of the text, because a reader has no
     * use for a list of ids.
     */
    {
        g_autofree gchar *prose = NULL;
        g_autoptr(GPtrArray) files = attachment_ids(body, &prose);

        htmx_node_set_text_content(HTMX_NODE(text),
                                   prose != NULL ? prose : body);
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(text));

        if (files != NULL) {
            g_autoptr(HtmxDiv) tray = htmx_div_new();
            guint n;

            htmx_element_add_class(HTMX_ELEMENT(tray), "attachments");

            for (n = 0; n < files->len; n++)
                clawt_web_add(tray,
                              attachment_element(g_ptr_array_index(files, n)));

            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(tray));
        }
    }

    return g_steal_pointer(&row);
}

static HtmxElement *
transcript(ClawtWebApp *app, const gchar *agent_id, gboolean cleared)
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

    /*
     * /clear hides the transcript here and nowhere else. The history is
     * the daemon's, and a command that tidied the view must not be one
     * that destroyed it -- /reset is the one that clears a session, and
     * it says so. The next refresh brings it back, which is the point.
     */
    if (cleared) {
        clawt_web_add(inner, clawt_web_empty(
            "Cleared on screen",
            "Nothing was deleted. Reload, or send anything, to see the "
            "conversation again. /reset is the one that clears the "
            "agent's session."));

        htmx_node_add_child(HTMX_NODE(scroll), HTMX_NODE(inner));

        return HTMX_ELEMENT(g_steal_pointer(&scroll));
    }

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

    {
        g_autofree gchar *run_sender = NULL;
        g_autofree gchar *run_day = NULL;
        g_autoptr(JsonNode) agent_reply = clawt_web_find_agent(app, agent_id);
        g_autofree gchar *color = g_strdup(clawt_web_member(
            clawt_web_member_object(clawt_web_root(agent_reply), "settings"),
            "color", NULL));

        for (i = 0; messages != NULL && i < json_array_get_length(messages);
             i++) {
            JsonObject *one = json_array_get_object_element(messages, i);
            gint64 ts = clawt_web_member_int(one, "ts", 0);
            g_autoptr(GDateTime) when = (ts > 0)
                ? g_date_time_new_from_unix_local(ts)
                : g_date_time_new_now_local();
            g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
            const gchar *sender = clawt_web_member(one, "sender", "?");
            gboolean new_day;
            gboolean run_start;

            /*
             * The rule comes from libclawt, so this client and the GTK
             * one cannot disagree about where a run begins.
             */
            run_start = clawt_chat_run_is_start(run_sender, run_day, sender,
                                                day, &new_day);

            if (new_day)
                clawt_web_add(inner, day_divider(ts));

            g_free(run_sender);
            run_sender = g_strdup(sender);
            g_free(run_day);
            run_day = g_steal_pointer(&day);

            clawt_web_add(inner,
                          message_element(one, agent_id, run_start, color));
        }
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
clawt_web_chat_body_full(ClawtWebApp *app, const gchar *agent_id,
                         gboolean cleared)
{
    g_autoptr(HtmxElement) main_el = HTMX_ELEMENT(htmx_main_new());

    htmx_element_add_class(main_el, "chat");

    if (agent_id == NULL) {
        clawt_web_add(main_el,
                      clawt_web_empty("No agent selected",
                                      "Pick one from the sidebar."));

        return g_steal_pointer(&main_el);
    }

    /*
     * The transcript and the pill share a wrapper so the pill can be
     * positioned against the transcript rather than the window: it
     * should sit just above the composer, not float in the middle of a
     * tall screen. The wrapper is also what survives the transcript
     * being swapped out from under it on every fleet event -- the pill
     * is outside the swap target on purpose, so its state is not thrown
     * away by the arrival that set it.
     */
    {
        g_autoptr(HtmxDiv) body = htmx_div_new();
        g_autoptr(HtmxButton) pill = htmx_button_new_with_label("New messages");

        htmx_element_add_class(HTMX_ELEMENT(body), "chat-body");

        htmx_element_add_class(HTMX_ELEMENT(pill), "jump-pill");
        htmx_element_set_id(HTMX_ELEMENT(pill), "jump-pill");
        htmx_element_set_attribute(HTMX_ELEMENT(pill), "type", "button");
        /*
         * Words rather than a bare arrow, for the reason the GTK client
         * gives: an arrow says "go down", which the scrollbar already
         * says. What was missing is "something is down there".
         */
        htmx_element_set_attribute(HTMX_ELEMENT(pill), "aria-label",
                                   "Jump to the newest message");

        clawt_web_add(body, transcript(app, agent_id, cleared));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(pill));
        htmx_node_add_child(HTMX_NODE(main_el), HTMX_NODE(body));
    }

    clawt_web_add(main_el, composer(agent_id));

    return g_steal_pointer(&main_el);
}

HtmxElement *
clawt_web_chat_body(ClawtWebApp *app, const gchar *agent_id)
{
    return clawt_web_chat_body_full(app, agent_id, FALSE);
}

/* ── Slash commands ──────────────────────────────────────────────── */

/*
 * The same eighteen the GTK composer answers, with the same names.
 *
 * They are handled here rather than sent to the agent for the reason they
 * are there: /reset and /stop are things to do *to* an agent, and a chat
 * that forwarded them would be asking the agent to reset itself, which it
 * cannot do.
 *
 * Three of them mean something slightly different in a browser, and the
 * table says so rather than leaving somebody to find out: /compose opens
 * a full-page box instead of $EDITOR, /copy shows the text to select
 * instead of reaching a clipboard on a machine that may not be yours, and
 * /edit opens the file in the page for the same reason.
 */
static const struct {
    const gchar *name;
    const gchar *argument;
    const gchar *summary;
} commands[] = {
    { "/help",    NULL,      "list these commands" },
    { "/start",   NULL,      "start this agent" },
    { "/stop",    NULL,      "stop this agent" },
    { "/restart", NULL,      "restart this agent" },
    { "/attach",  NULL,      "send a file with the next message" },
    { "/compose", NULL,      "write the message in a full-page box" },
    { "/edit",    "[file]",  "open a workspace file to edit here" },
    { "/files",   NULL,      "list this agent's workspace files" },
    { "/memory",  "<query>", "search what this agent has remembered" },
    { "/agents",  NULL,      "who is in the fleet" },
    { "/flow",    NULL,      "go to the conversations between agents" },
    { "/tasks",   NULL,      "go to the task board" },
    { "/reset",   NULL,      "start the agent's AI session again, from nothing" },
    { "/retry",   NULL,      "send your last message again" },
    { "/export",  "[org]",   "download the conversation: text, markdown or org" },
    { "/copy",    "[org]",   "show the conversation to copy: text, markdown or org" },
    { "/clear",   NULL,      "clear this transcript on screen only" },
    { "/new",     NULL,      "create an agent" }
};

/*
 * One frame, one agent, one sentence afterwards.
 */
static HtmxResponse *
simple_agent_action(ClawtWebApp *app, HtmxRequest *request,
                    const gchar *agent_id, const gchar *kind,
                    const gchar *done)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, done);
}

/*
 * The word after the command, or NULL.
 */
static gchar *
command_argument(const gchar *text)
{
    const gchar *space = strchr(text, ' ');

    if (space == NULL)
        return NULL;

    {
        g_autofree gchar *rest = g_strdup(space + 1);

        g_strstrip(rest);

        if (*rest == '\0')
            return NULL;

        return g_steal_pointer(&rest);
    }
}

static ClawtExportFormat
format_from_word(const gchar *word)
{
    if (g_strcmp0(word, "org") == 0)
        return CLAWT_EXPORT_ORG;
    if (g_strcmp0(word, "text") == 0 || g_strcmp0(word, "plain") == 0)
        return CLAWT_EXPORT_PLAIN;

    return CLAWT_EXPORT_MARKDOWN;
}

/*
 * The conversation as a document.
 *
 * Built through clawt_export_transcript() rather than by formatting the
 * reply here, so the web client's export is the same bytes the GTK
 * client's is. Two renderers would differ the first time either changed.
 */
static gchar *
conversation_document(ClawtWebApp *app, const gchar *agent_id,
                      ClawtExportFormat format, GError **error)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) messages =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_message_free);
    JsonObject *root;
    JsonArray *list;
    const gchar *room;
    guint i;

    clawt_web_payload_set(payload, "room", agent_id);
    clawt_web_payload_set(payload, "as", "user");
    clawt_web_payload_set_int(payload, "limit", 1000);

    reply = clawt_web_app_call(app, "room.history",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    root = clawt_web_root(reply);
    list = clawt_web_member_array(root, "messages");
    room = clawt_web_member(root, "room", agent_id);

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *message = json_array_get_object_element(list, i);

        g_ptr_array_add(messages,
                        clawt_message_new(room,
                                          clawt_web_member(message, "sender",
                                                           "?"),
                                          clawt_web_member(message, "body",
                                                           "")));
    }

    return clawt_export_transcript(room, messages, format, error);
}

static HtmxResponse *
run_command(ClawtWebApp *app, HtmxRequest *request, const gchar *agent_id,
            const gchar *text)
{
    g_autofree gchar *argument = command_argument(text);
    g_autofree gchar *verb = NULL;

    {
        const gchar *space = strchr(text, ' ');

        verb = (space != NULL) ? g_strndup(text, (gsize)(space - text))
                               : g_strdup(text);
    }

    /* ── Things to do to the agent ── */

    if (g_strcmp0(verb, "/start") == 0)
        return simple_agent_action(app, request, agent_id, "agent.start",
                                   "Starting.");

    if (g_strcmp0(verb, "/stop") == 0)
        return simple_agent_action(app, request, agent_id, "agent.stop",
                                   "Stopped.");

    if (g_strcmp0(verb, "/restart") == 0)
        return simple_agent_action(app, request, agent_id, "agent.restart",
                                   "Restarting.");

    if (g_strcmp0(verb, "/reset") == 0)
        return simple_agent_action(app, request, agent_id, "agent.reset",
                                   "Session cleared.");

    /* ── Going somewhere ── */

    if (g_strcmp0(verb, "/flow") == 0) {
        g_autofree gchar *url = clawt_web_agent_url(agent_id,
                                                    CLAWT_WEB_VIEW_FLOW);

        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/tasks") == 0) {
        g_autofree gchar *url = clawt_web_agent_url(agent_id,
                                                    CLAWT_WEB_VIEW_TASKS);

        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/new") == 0)
        return clawt_web_redirect(request, "/new");

    if (g_strcmp0(verb, "/agents") == 0)
        return clawt_web_redirect(request, "/fleet");

    if (g_strcmp0(verb, "/files") == 0 || g_strcmp0(verb, "/memory") == 0 ||
        g_strcmp0(verb, "/edit") == 0) {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);
        g_autofree gchar *url = NULL;

        if (g_strcmp0(verb, "/memory") == 0) {
            g_autofree gchar *query = (argument != NULL)
                ? g_uri_escape_string(argument, NULL, FALSE) : NULL;

            url = g_strdup_printf("/a/%s/memories?q=%s", escaped,
                                  query != NULL ? query : "");
        } else if (g_strcmp0(verb, "/edit") == 0 && argument != NULL) {
            g_autofree gchar *file = g_uri_escape_string(argument, NULL,
                                                         FALSE);

            url = g_strdup_printf("/a/%s/file?name=%s", escaped, file);
        } else {
            url = g_strdup_printf("/a/%s/files", escaped);
        }

        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/compose") == 0) {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);
        g_autofree gchar *url = g_strdup_printf("/a/%s/compose", escaped);

        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/attach") == 0)
        return clawt_web_after_action(
            app, request, agent_id, CLAWT_WEB_VIEW_CHAT,
            "Use the file picker under the message box. The file goes to "
            "the agent's workspace and is named to it -- both paths, "
            "because its own tools run on the host and only "
            "clawtilla_computer_exec enters its computer.");

    /* ── The conversation itself ── */

    if (g_strcmp0(verb, "/export") == 0 || g_strcmp0(verb, "/copy") == 0) {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);
        g_autofree gchar *url = g_strdup_printf(
            "/a/%s/%s?format=%s", escaped,
            g_strcmp0(verb, "/export") == 0 ? "export" : "copy",
            argument != NULL ? argument : "markdown");

        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/clear") == 0) {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);
        g_autofree gchar *url = g_strdup_printf("/a/%s/chat?clear=1",
                                                escaped);

        /*
         * On screen only, exactly as in the GTK client. Nothing is sent
         * to the daemon: the transcript is the daemon's, and a command
         * that tidied the view must not be one that destroyed history.
         * /reset is the one that clears a session, and it says so.
         */
        return clawt_web_redirect(request, url);
    }

    if (g_strcmp0(verb, "/retry") == 0) {
        g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *messages;
        g_autofree gchar *last = NULL;
        guint i;

        clawt_web_payload_set(payload, "room", agent_id);
        clawt_web_payload_set(payload, "as", "user");
        clawt_web_payload_set_int(payload, "limit", 100);

        reply = clawt_web_app_call(app, "room.history",
                                   clawt_web_payload_take(
                                       g_steal_pointer(&payload)));
        messages = clawt_web_member_array(clawt_web_root(reply), "messages");

        for (i = 0; messages != NULL && i < json_array_get_length(messages);
             i++) {
            JsonObject *message = json_array_get_object_element(messages, i);

            if (g_strcmp0(clawt_web_member(message, "sender", ""),
                          "user") != 0)
                continue;

            g_free(last);
            last = g_strdup(clawt_web_member(message, "body", ""));
        }

        if (last == NULL)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_WEB_VIEW_CHAT,
                                        "You have not said anything to "
                                        "this agent yet.");

        return clawt_web_send_message(app, request, agent_id, last);
    }

    /* ── Help ── */

    if (g_strcmp0(verb, "/help") == 0) {
        g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
        g_autoptr(HtmxDiv) pad = htmx_div_new();
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Commands",
            "Typed into the message box. They act on the agent or on this "
            "page; nothing here is sent to the agent.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autofree gchar *html = NULL;
        guint i;

        htmx_element_add_class(view, "view");
        htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

        for (i = 0; i < G_N_ELEMENTS(commands); i++) {
            g_autofree gchar *name = commands[i].argument != NULL
                ? g_strdup_printf("%s %s", commands[i].name,
                                  commands[i].argument)
                : g_strdup(commands[i].name);

            clawt_web_add(body, clawt_web_row(name, commands[i].summary));
        }

        {
            g_autofree gchar *back = clawt_web_agent_url(agent_id,
                                                         CLAWT_WEB_VIEW_CHAT);
            g_autoptr(HtmxA) link = htmx_a_new_with_href(back);

            htmx_element_add_class(HTMX_ELEMENT(link), "btn");
            htmx_node_set_text_content(HTMX_NODE(link), "Back to the chat");
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(link));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_CHAT, view,
                              request);

        return clawt_web_html_response(html);
    }

    {
        g_autofree gchar *unknown = g_strdup_printf(
            "No such command: %s. Try /help.", verb);

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT, unknown);
    }
}

/* ── Routes ──────────────────────────────────────────────────────── */

/*
 * The conversation as a file the browser saves.
 *
 * Content-Disposition rather than a link to something on disk: the
 * machine running clawtilla-web is not necessarily the machine looking
 * at it, so "saved to ~/Documents" would be the wrong ~ .
 */
static HtmxResponse *
on_export(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *word = htmx_request_get_query_param(request, "format");
    ClawtExportFormat format = format_from_word(word);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *document = NULL;
    HtmxResponse *response;

    document = conversation_document(app, agent_id, format, &error);

    if (document == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    error != NULL ? error->message
                                                  : "nothing to export");

    response = htmx_response_new_with_content(document);
    htmx_response_set_content_type(response, "text/plain; charset=utf-8");

    {
        /* The helper's extension already carries its dot. */
        g_autofree gchar *name = g_strdup_printf(
            "%s%s", agent_id, clawt_export_format_extension(format));
        g_autofree gchar *disposition = g_strdup_printf(
            "attachment; filename=\"%s\"", name);

        htmx_response_add_header(response, "Content-Disposition",
                                 disposition);
    }

    return response;
}

/*
 * The same document, shown rather than downloaded.
 *
 * /copy in the GTK client reaches the clipboard of the machine somebody
 * is sitting at. A server cannot do that for a browser it is only
 * talking to, so the honest equivalent is putting the text where it can
 * be selected -- with a button for the browsers that will let a script
 * do it.
 */
static HtmxResponse *
on_copy(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *word = htmx_request_get_query_param(request, "format");
    ClawtExportFormat format = format_from_word(word);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *document = NULL;
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    g_autofree gchar *html = NULL;

    document = conversation_document(app, agent_id, format, &error);

    if (document == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    error != NULL ? error->message
                                                  : "nothing to copy");

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    card = clawt_web_card("The conversation",
                          "Select it, or use the button.");
    body = clawt_web_card_body(card);

    {
        g_autoptr(HtmxTextarea) area = htmx_textarea_new_with_name("document");
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) copy = clawt_web_button("Copy", "primary");
        g_autofree gchar *download = NULL;
        g_autoptr(HtmxA) save = NULL;
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);

        htmx_element_set_attribute(HTMX_ELEMENT(area), "rows", "22");
        htmx_element_set_id(HTMX_ELEMENT(area), "document");
        htmx_element_set_attribute(HTMX_ELEMENT(area), "readonly",
                                   "readonly");
        htmx_node_set_text_content(HTMX_NODE(area), document);
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(area));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(
            HTMX_ELEMENT(copy), "onclick",
            "var d=document.getElementById('document');d.select();"
            "navigator.clipboard&&navigator.clipboard.writeText(d.value)");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(copy));

        download = g_strdup_printf(
            "/a/%s/export?format=%s", escaped,
            format == CLAWT_EXPORT_ORG ? "org"
            : format == CLAWT_EXPORT_PLAIN ? "text" : "markdown");
        save = htmx_a_new_with_href(download);
        htmx_element_add_class(HTMX_ELEMENT(save), "btn");
        htmx_node_set_text_content(HTMX_NODE(save), "Download instead");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));

        {
            g_autofree gchar *back = clawt_web_agent_url(
                agent_id, CLAWT_WEB_VIEW_CHAT);
            g_autoptr(HtmxA) link = htmx_a_new_with_href(back);

            htmx_element_add_class(HTMX_ELEMENT(link), "btn");
            htmx_node_set_text_content(HTMX_NODE(link), "Back");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(link));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_CHAT, view, request);

    return clawt_web_html_response(html);
}

/*
 * A full-page box for a long message.
 *
 * /compose in the GTK client opens $EDITOR, which is a program on the
 * machine a person is sitting at. A browser reached over the network has
 * no such thing, so the equivalent is the biggest box the page can offer.
 */
static HtmxResponse *
on_compose(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/send", escaped);
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(HtmxDiv) card = clawt_web_card("Compose", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    g_autofree gchar *html = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(form, clawt_web_textarea_field("Message", "body", NULL, 20));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) send = clawt_web_button("Send", "primary");
        g_autofree gchar *back = clawt_web_agent_url(agent_id,
                                                     CLAWT_WEB_VIEW_CHAT);
        g_autoptr(HtmxA) cancel = htmx_a_new_with_href(back);

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(send));

        htmx_element_add_class(HTMX_ELEMENT(cancel), "btn");
        htmx_node_set_text_content(HTMX_NODE(cancel), "Cancel");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(cancel));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_CHAT, view, request);

    return clawt_web_html_response(html);
}



HtmxResponse *
clawt_web_send_message(ClawtWebApp *app, HtmxRequest *request,
                       const gchar *agent_id, const gchar *body)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "target", agent_id);
    clawt_web_payload_set(payload, "body", body);
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
on_send(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *body = clawt_web_form_value(request, "body");
    g_autofree gchar *trimmed = NULL;

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

    return clawt_web_send_message(app, request, agent_id, trimmed);
}

static HtmxResponse *
on_transcript_fragment(HtmxRequest *request, GHashTable *params,
                       gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) fragment = NULL;

    (void)request;

    /*
     * The transcript is re-fetched on every fleet event, so this fires
     * whenever a message arrives for whoever is reading -- which makes
     * it the moment their conversation counts as read.  Without it a
     * reader sitting on a chat would watch its own pill appear in the
     * sidebar and stay: the page render that clears the count only
     * happens on a navigation.
     */
    clawt_web_app_set_viewing(app, agent_id);

    fragment = transcript(app, agent_id, FALSE);

    return clawt_web_fragment_response(fragment);
}

/*
 * The bytes of one attachment.
 *
 * Fetched from the daemon rather than read off disk: clawtilla-web and
 * clawtillad need not be the same process or the same machine, and this
 * is the whole reason the daemon copies an agent's file at send time
 * instead of passing the path along.
 */
static HtmxResponse *
on_attachment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;
    const gchar *encoded;
    const gchar *name;
    guchar *raw;
    gsize length = 0;

    (void)request;

    clawt_web_payload_set(payload, "id", id);

    reply = clawt_web_app_call(app, "attachment.get",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return htmx_response_not_found();

    encoded = clawt_web_member(clawt_web_root(reply), "base64", NULL);
    name = clawt_web_member(clawt_web_root(reply), "name", "file");

    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);

    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);

    /*
     * The type from the name, and `application/octet-stream` when it is
     * not one we recognise.  Guessing from the bytes would mean sniffing
     * content a model produced, which is how a browser is talked into
     * rendering something as HTML.
     */
    htmx_response_set_content_type(
        response, looks_like_an_image(name)
                      ? (g_str_has_suffix(name, ".png") ? "image/png"
                         : g_str_has_suffix(name, ".gif") ? "image/gif"
                         : g_str_has_suffix(name, ".webp") ? "image/webp"
                         : g_str_has_suffix(name, ".svg")
                             ? "image/svg+xml" : "image/jpeg")
                      : "application/octet-stream");

    /*
     * Never inline for anything that is not an image.  A file an agent
     * produced is content this server did not write, and letting a
     * browser render it in this origin is how a transcript becomes a
     * script.
     */
    if (!looks_like_an_image(name)) {
        g_autofree gchar *disposition = g_strdup_printf(
            "attachment; filename=\"%s\"", name);

        htmx_response_add_header(response, "Content-Disposition",
                                 disposition);
    }

    return response;
}

void
clawt_web_register_chat(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/send", on_send, app);
    htmx_router_get(router, "/f/a/:id/transcript", on_transcript_fragment, app);
    htmx_router_get(router, "/f/attachment/:id", on_attachment, app);
    htmx_router_get(router, "/a/:id/export", on_export, app);
    htmx_router_get(router, "/a/:id/copy", on_copy, app);
    htmx_router_get(router, "/a/:id/compose", on_compose, app);
}
