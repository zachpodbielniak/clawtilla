/*
 * web-computer.c - The exec console, the mounts, and the exchange
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "web-pages.h"

#include <string.h>

/* ── The console ─────────────────────────────────────────────────── */

static void
add_console(HtmxElement *parent, const gchar *agent_id,
            const gchar *command, JsonObject *result)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Run a command",
        "Runs inside the agent's computer, over its own transport.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/exec", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);

    clawt_web_add(form, clawt_web_field(
        "Command", "command", command, "uname -a"));

    /*
     * Said here rather than left to be discovered. Both backends quote
     * each argument and join them, so a redirection or a pipe arrives as
     * literal text -- and the failure is the bad kind: `echo hi >
     * /dev/console` exits 0 and prints `hi > /dev/console` to stdout, so
     * it reports success and does nothing. An agent worked this out by
     * trial and error once already.
     */
    clawt_web_add(form, clawt_web_text(
        "This is a command, not a shell line. >, |, && and $VAR arrive as "
        "literal text. To use them, run them through a shell: "
        "sh -c 'echo hi > /dev/console'", "small muted"));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) run = clawt_web_button("Run", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(run), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(run));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    if (result != NULL) {
        /*
         * "exit", which is what computer.exec actually replies with.
         * Reading "exit_status" took the -1 fallback on every call, so
         * the panel drew a red `failed` badge and `exit -1` above
         * perfectly good output -- and a command that really did fail
         * was indistinguishable from one that worked.
         */
        gint64 status = clawt_web_member_int(result, "exit", -1);
        const gchar *out = clawt_web_member(result, "stdout", "");
        const gchar *err = clawt_web_member(result, "stderr", "");
        g_autoptr(HtmxDiv) head = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(head), "btn-row");
        clawt_web_add(head, clawt_web_badge(
            status == 0 ? "exit 0" : "failed",
            status == 0 ? "good" : "bad"));

        if (status != 0) {
            g_autofree gchar *text =
                g_strdup_printf("exit %" G_GINT64_FORMAT, status);

            clawt_web_add(head, clawt_web_badge(text, "neutral"));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(head));

        if (*out != '\0') {
            g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

            htmx_element_add_class(pre, "console");
            htmx_node_set_text_content(HTMX_NODE(pre), out);
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(pre));
        }

        if (*err != '\0') {
            g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

            clawt_web_add(body, clawt_web_text("stderr", "small muted"));
            htmx_element_add_class(pre, "console");
            htmx_node_set_text_content(HTMX_NODE(pre), err);
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(pre));
        }

        if (*out == '\0' && *err == '\0')
            clawt_web_add(body, clawt_web_text("No output.", "small muted"));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── Mounts ──────────────────────────────────────────────────────── */

static void
add_mounts(ClawtWebApp *app, HtmxElement *parent, const gchar *agent_id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Mounts",
        "Paths from this machine, inside the agent's computer.");
    HtmxElement *body = clawt_web_card_body(card);
    JsonArray *mounts;
    guint i;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, "agent.mount.list",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    mounts = clawt_web_member_array(clawt_web_root(reply), "mounts");

    if (mounts == NULL || json_array_get_length(mounts) == 0) {
        clawt_web_add(body, clawt_web_empty(
            "No mounts of its own",
            "Every computer still gets the agent's workspace and the "
            "exchange directory."));
    }

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autofree gchar *pair = NULL;
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *source_escaped = NULL;
        g_autofree gchar *action = NULL;

        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

        /*
         * Both names, host first. An agent's own read/write/bash run on
         * the *host*, and only computer_exec enters the guest -- so a
         * path given without saying which side it is on gets looked for
         * on the wrong one, which is exactly what happened the first
         * time a share was added.
         */
        pair = g_strdup_printf("%s  =  %s",
                               clawt_web_member(mount, "source", "?"),
                               clawt_web_member(mount, "target", "?"));

        {
            g_autoptr(HtmxSpan) label = htmx_span_new();

            htmx_element_add_class(HTMX_ELEMENT(label), "mono");
            htmx_node_set_text_content(HTMX_NODE(label), pair);
            htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
        }

        clawt_web_add(head, clawt_web_badge(
            clawt_web_member(mount, "mode", "rw"), "info"));
        clawt_web_add(head, clawt_web_badge(
            clawt_web_member(mount, "type", "bind"), "neutral"));

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

        clawt_web_add(row, clawt_web_text(
            "host path = the path inside", "small muted"));

        /*
         * Keyed on the target, which is what the daemon removes by. Two
         * mounts can share a source -- the same directory offered at two
         * paths -- so the source does not identify one.
         */
        source_escaped = g_uri_escape_string(
            clawt_web_member(mount, "target", ""), NULL, FALSE);
        action = g_strdup_printf("/a/%s/mount/remove?target=%s",
                                 escaped, source_escaped);

        {
            g_autoptr(HtmxDiv) actions = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
            clawt_web_add(actions, clawt_web_post_button(
                "Remove", action, "danger", "Remove this mount?"));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    {
        static const gchar *const modes[] = { "rw", "ro", NULL };
        static const gchar *const types[] = {
            "bind", "virtiofs", "tmpfs", "volume", NULL
        };
        g_autoptr(GPtrArray) relabels = g_ptr_array_new();
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/a/%s/mount/add", escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxDiv) grid = htmx_div_new();
        guint r;

        /*
         * Walked from the library rather than listed here.  This was
         * three nicks written out, and the default offered was `none`
         * while the daemon's default -- what the YAML and every other
         * client get -- is `shared`: the same form filled in the same
         * way produced different mounts depending on which client typed
         * it.
         */
        for (r = 0; r < clawt_relabel_count(); r++)
            g_ptr_array_add(relabels, (gpointer)clawt_relabel_nth_nick(r));

        g_ptr_array_add(relabels, NULL);

        clawt_web_add(body, clawt_web_section_title("Add a mount"));

        htmx_element_add_class(HTMX_ELEMENT(grid), "field-inline");
        clawt_web_add(grid, clawt_web_field("Source (on this machine)",
                                            "source", NULL, "~/src/notes"));
        clawt_web_add(grid, clawt_web_field("Target (inside)", "target", NULL,
                                            "/work/notes"));
        clawt_web_add(grid, clawt_web_select_field("Mode", "mode", modes,
                                                   NULL, "rw"));
        clawt_web_add(grid, clawt_web_select_field("Type", "type", types,
                                                   NULL, "bind"));
        clawt_web_add(grid, clawt_web_select_field(
            "SELinux relabel", "relabel",
            (const gchar *const *)relabels->pdata, NULL,
            clawt_enum_to_nick(CLAWT_TYPE_RELABEL, CLAWT_RELABEL_SHARED)));
        clawt_web_add(grid, clawt_web_field("Size (tmpfs only)", "size", NULL,
                                            "512M"));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(grid));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Add mount",
                                                         "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── The exchange ────────────────────────────────────────────────── */

static void
add_exchange(ClawtWebApp *app, HtmxElement *parent)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "exchange.list", NULL);
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Exchange",
        "The shared drop-box, mounted into every computer. Agents hand "
        "files around through it without a mount set up by hand.");
    HtmxElement *body = clawt_web_card_body(card);
    JsonArray *files;
    guint i;

    files = clawt_web_member_array(clawt_web_root(reply), "entries");

    if (files == NULL)
        files = clawt_web_member_array(clawt_web_root(reply), "files");

    if (files == NULL || json_array_get_length(files) == 0) {
        clawt_web_add(body, clawt_web_empty("Nothing in the exchange", NULL));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(files); i++) {
            JsonNode *node = json_array_get_element(files, i);
            g_autoptr(HtmxDiv) row = htmx_div_new();
            const gchar *name;

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");

            if (JSON_NODE_HOLDS_OBJECT(node))
                name = clawt_web_member(json_node_get_object(node), "path",
                                        "?");
            else
                name = json_node_get_string(node);

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(label), "mono");
                htmx_node_set_text_content(HTMX_NODE(label),
                                           name != NULL ? name : "?");
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(label));
            }

            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(list));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}


/* ── The screen ──────────────────────────────────────────────────── */

/*
 * The panel that shows the picture, refetched on its own.
 *
 * Its own fragment rather than part of the page, because the page has a
 * mounts listing and an exchange listing on it and neither of those
 * wants rebuilding once a second.
 */
static HtmxElement *
build_screen_panel(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) status = NULL;
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *poll = NULL;
    JsonObject *root;
    gint64 stamp;
    gboolean held;
    gboolean stale;

    htmx_element_add_class(HTMX_ELEMENT(panel), "screen-panel");

    /*
     * The poll subscribes as well as reading.
     *
     * Nothing is captured unless somebody is watching, so a panel that
     * only read would show "no frame yet" for ever. The subscription is
     * a lease keyed by this client's name, so polling twice a second
     * does not stack up watchers and a tab that goes away stops being
     * one within CLAWT_OBSERVE_LEASE_SECONDS -- which is the only way a
     * browser can say goodbye.
     */
    {
        g_autoptr(ClawtWebPayload) watch = clawt_web_payload_new();
        g_autoptr(JsonNode) started = NULL;

        clawt_web_payload_set(watch, "agent", agent_id);
        clawt_web_payload_set(watch, "watcher", CLAWT_WEB_WATCHER_NAME);
        started = clawt_web_app_call(app, "computer.observe",
                                     clawt_web_payload_take(
                                         g_steal_pointer(&watch)));
        (void)started;
    }

    clawt_web_payload_set(payload, "agent", agent_id);
    status = clawt_web_app_call(app, "computer.screen",
                                clawt_web_payload_take(
                                    g_steal_pointer(&payload)));

    if (status == NULL) {
        clawt_web_add(panel, clawt_web_empty(
            "The screen cannot be read", clawt_web_app_last_error(app)));

        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    root = clawt_web_root(status);
    stamp = clawt_web_member_int(root, "stamp", 0);
    held = clawt_web_member_bool(root, "held", FALSE);
    stale = clawt_web_member_bool(root, "stale", FALSE);

    /*
     * Polled, and how fast depends on the agent.
     *
     * Slower when nothing is happening, because every poll ends in a
     * grab down the connection the agent works over -- and suspended
     * altogether while the page is hidden, since a tab left open in a
     * background window would otherwise cost an agent a frame a second
     * for as long as the browser was running. The filter is htmx's own:
     * a trigger with a condition in brackets fires only when it holds.
     */
    poll = g_strdup_printf("every %ds [!document.hidden]",
                           clawt_web_member_int(root, "watchers", 0) > 0
                           ? 2 : 5);

    {
        g_autofree gchar *url = g_strdup_printf("/f/a/%s/screen", escaped);

        htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", url);
        htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger", poll);
        htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap",
                                   "outerHTML");
    }

    if (!clawt_web_member_bool(root, "observable", FALSE)) {
        clawt_web_add(panel, clawt_web_empty(
            "No screen to watch yet",
            clawt_web_member(root, "error",
                             "The agent is not running, or it has no "
                             "desktop. computer.desktop.enabled is the "
                             "grant; a VM installs one at first boot, so "
                             "turning it on afterwards needs a rebuild.")));

        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    /*
     * The picture, with the cache-buster the stamp already provides. A
     * fixed URL would be served out of the browser's cache for ever,
     * which reads as a screen where nothing ever happens.
     */
    {
        g_autofree gchar *src =
            g_strdup_printf("/a/%s/frame?t=%" G_GINT64_FORMAT, escaped,
                            stamp);
        g_autoptr(HtmxDiv) frame = htmx_div_new();
        g_autoptr(HtmxImg) picture = htmx_img_new_with_src(src, "The screen");

        htmx_element_add_class(HTMX_ELEMENT(frame), "screen-frame");
        htmx_element_add_class(HTMX_ELEMENT(picture), "screen-image");
        htmx_node_add_child(HTMX_NODE(frame), HTMX_NODE(picture));
        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(frame));
    }

    {
        g_autoptr(HtmxDiv) badges = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(badges), "btn-row");

        /*
         * A stale frame is labelled with its age rather than shown as
         * current. The threshold is the library's, so the browser and
         * the window agree about when a picture stopped being news.
         */
        if (stamp <= 0)
            clawt_web_add(badges, clawt_web_badge("no frame yet", "neutral"));
        else if (stale) {
            g_autofree gchar *ago = clawt_time_ago_label(stamp,
                                                         g_get_real_time());
            g_autofree gchar *text = g_strdup_printf("stale \xe2\x80\x94 %s",
                                                     ago);

            clawt_web_add(badges, clawt_web_badge(text, "warn"));
        } else
            clawt_web_add(badges, clawt_web_badge("live", "good"));

        if (held) {
            g_autofree gchar *who = g_strdup_printf(
                "held by %s", clawt_web_member(root, "holder", "somebody"));

            clawt_web_add(badges, clawt_web_badge(who, "info"));
        }

        if (clawt_web_member(root, "error", NULL) != NULL)
            clawt_web_add(badges, clawt_web_badge(
                clawt_web_member(root, "error", ""), "bad"));

        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(badges));
    }

    /*
     * What the agent asked for, if it asked. Beside the picture rather
     * than in the transcript, because that is where somebody is looking
     * when they are about to take the screen.
     */
    if (clawt_web_member(root, "request", NULL) != NULL) {
        g_autofree gchar *asked = g_strdup_printf(
            "It has asked for hands: %s",
            clawt_web_member(root, "request", ""));

        clawt_web_add(panel, clawt_web_text(asked, "lede"));
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autofree gchar *take = g_strdup_printf("/a/%s/screen/take",
                                                 escaped);
        g_autofree gchar *release = g_strdup_printf("/a/%s/screen/release",
                                                    escaped);

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");

        if (held)
            clawt_web_add(row, clawt_web_post_button(
                "Give the screen back", release, "primary", NULL));
        else
            clawt_web_add(row, clawt_web_post_button(
                "Take the screen", take, NULL, NULL));

        /*
         * Letting go now rather than in half a minute.
         *
         * Leaving this tab already stops the poll and the lease lapses
         * on its own, which is what makes a closed browser harmless --
         * but somebody who has finished watching should be able to stop
         * costing the agent frames straight away rather than waiting
         * for a timer they cannot see.
         */
        {
            g_autofree gchar *unwatch =
                g_strdup_printf("/a/%s/screen/unwatch", escaped);

            clawt_web_add(row, clawt_web_post_button(
                "Stop watching", unwatch, NULL, NULL));
        }

        /*
         * The real viewer, offered beside the preview rather than
         * instead of it. A frame every second or two is right for
         * watching and hopeless for using, and a VM already has a VNC
         * server on it -- so the two are different jobs rather than one
         * pretending to be the other. No noVNC is shipped: this page has
         * to work on a tailnet with no route out.
         */
        if (clawt_web_member(root, "viewer", NULL) != NULL) {
            g_autoptr(HtmxA) viewer = htmx_a_new_with_href(
                clawt_web_member(root, "viewer", ""));
            g_autofree gchar *vv = g_strdup_printf("/a/%s/screen/viewer.vv",
                                                   escaped);
            g_autoptr(HtmxA) download = htmx_a_new_with_href(vv);

            htmx_element_add_class(HTMX_ELEMENT(viewer), "btn");
            htmx_node_set_text_content(HTMX_NODE(viewer),
                                       "Open in a viewer");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(viewer));

            htmx_element_add_class(HTMX_ELEMENT(download), "btn");
            htmx_node_set_text_content(HTMX_NODE(download),
                                       "Download a .vv file");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(download));
        }

        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(row));
    }

    /*
     * Input, offered only while the screen is held.
     *
     * Not greyed out when it is not: a control that is there and does
     * nothing teaches somebody to click it twice. Taking the screen is
     * what makes these appear, which is also what stops the agent.
     */
    if (held) {
        g_autofree gchar *action = g_strdup_printf("/a/%s/screen/input",
                                                   escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxDiv) grid = htmx_div_new();
        guint screen_width = (guint)clawt_web_member_int(root, "screen_width",
                                                         0);
        guint frame_width = (guint)clawt_web_member_int(root, "frame_width",
                                                        0);

        htmx_element_add_class(HTMX_ELEMENT(form), "screen-input");
        htmx_element_add_class(HTMX_ELEMENT(grid), "field-inline");

        clawt_web_add(grid, clawt_web_field("Type", "text", NULL,
                                            "hello there"));
        clawt_web_add(grid, clawt_web_field("Key or combo", "key", NULL,
                                            "ctrl+l"));
        clawt_web_add(grid, clawt_web_field("Click x", "x", NULL, "640"));
        clawt_web_add(grid, clawt_web_field("Click y", "y", NULL, "400"));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(grid));

        /*
         * Which pixels the numbers are in, said rather than assumed.
         * The picture is downscaled in the compositor, so a coordinate
         * read off it is not a coordinate on the screen -- and the
         * ratio is a thing the daemon knows and a person does not.
         */
        if (screen_width > 0 && frame_width > 0) {
            g_autofree gchar *note = g_strdup_printf(
                "Coordinates are in the screen's own pixels (%d x %d). The "
                "picture above is %d wide, so multiply what you measure on "
                "it by %.2f.",
                (gint)screen_width,
                (gint)clawt_web_member_int(root, "screen_height", 0),
                (gint)frame_width,
                (gdouble)screen_width / (gdouble)frame_width);

            clawt_web_add(form, clawt_web_text(note, "small muted"));
        } else
            clawt_web_add(form, clawt_web_text(
                "Coordinates are in the picture's own pixels: this backend "
                "does not report the screen's size, so nothing here can "
                "scale them for you.", "small muted"));

        {
            g_autoptr(HtmxDiv) buttons = htmx_div_new();
            g_autoptr(HtmxButton) send = clawt_web_button("Send", "primary");

            htmx_element_add_class(HTMX_ELEMENT(buttons), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
            htmx_node_add_child(HTMX_NODE(buttons), HTMX_NODE(send));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(buttons));
        }

        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(form));
    }

    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

static void
add_screen(ClawtWebApp *app, HtmxElement *parent, const gchar *agent_id)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Screen",
        "What is on the agent's desktop, while you are looking. Nothing "
        "is captured otherwise -- a frame is latency taken from the work "
        "you are watching.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxElement) panel = build_screen_panel(app, agent_id);

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(panel));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── The view ────────────────────────────────────────────────────── */

/*
 * The four tabs, walked from the library rather than listed here.
 *
 * The GTK client draws the same four from the same enumeration, which
 * is what stops one of them growing a fifth quietly -- `make parity`
 * checks that both walk it.
 */
static void
add_subnav(HtmxElement *parent, const gchar *agent_id, ClawtComputerView on,
           gboolean has_screen)
{
    g_autoptr(HtmxDiv) nav = htmx_div_new();
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(nav), "subnav");

    for (i = 0; i < clawt_computer_view_count(); i++) {
        ClawtComputerView view = clawt_computer_view_nth(i);
        g_autofree gchar *href = NULL;
        g_autoptr(HtmxA) tab = NULL;

        /*
         * A screen tab is only drawn where there could be a screen, and
         * the predicate answers that -- not a list of types here, which
         * would offer it on a backend added later or fail to.
         */
        if (view == CLAWT_COMPUTER_VIEW_SCREEN && !has_screen)
            continue;

        href = g_strdup_printf("/a/%s/computer/%s", escaped,
                               clawt_computer_view_nth_nick(i));
        tab = htmx_a_new_with_href(href);

        htmx_element_add_class(HTMX_ELEMENT(tab), "subnav-tab");
        htmx_node_set_text_content(HTMX_NODE(tab),
                                   clawt_computer_view_nth_label(i));

        if (view == on)
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current",
                                       "page");

        htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(tab));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(nav));
}

HtmxElement *
clawt_web_computer_view_body(ClawtWebApp       *app,
                             const gchar       *agent_id,
                             ClawtComputerView  tab)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(ClawtWebPayload) payload = NULL;
    g_autoptr(JsonNode) status = NULL;
    g_autoptr(JsonNode) shown = NULL;
    JsonObject *agent = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    if (agent_id == NULL) {
        clawt_web_add(pad, clawt_web_empty("No agent selected", NULL));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        return g_steal_pointer(&view);
    }

    shown = clawt_web_find_agent(app, agent_id);
    agent = clawt_web_member_object(clawt_web_root(shown), "agent");

    clawt_web_add(pad, clawt_web_section_title("Computer"));

    if (g_strcmp0(clawt_web_member(agent, "computer", "none"), "none") == 0) {
        clawt_web_add(pad, clawt_web_text(
            "This agent has no computer -- it is chat only.", "lede"));
        clawt_web_add(pad, clawt_web_empty(
            "Nothing to run commands on",
            "Give it one on the Agent page: computer.type is where the "
            "choice lives. A VM also needs computer.vm.image, or it will "
            "boot nothing."));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        return g_steal_pointer(&view);
    }

    add_subnav(HTMX_ELEMENT(pad), agent_id, tab,
               clawt_web_member_bool(agent, "computer_screen", FALSE));

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "agent", agent_id);
    status = clawt_web_app_call(app, "computer.status",
                                clawt_web_payload_take(
                                    g_steal_pointer(&payload)));

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("State", NULL);
        HtmxElement *body = clawt_web_card_body(card);
        JsonObject *root = clawt_web_root(status);

        clawt_web_add(body, clawt_web_row(
            "Type", clawt_web_member(agent, "computer", "?")));
        clawt_web_add(body, clawt_web_row(
            "State", clawt_web_member(root, "state", "unknown")));

        if (clawt_web_member(root, "detail", NULL) != NULL)
            clawt_web_add(body, clawt_web_row(
                "Detail", clawt_web_member(root, "detail", NULL)));

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    /*
     * Powering the machine on and off, for the types that have one.
     *
     * Drawn from `computer_machine`, which the daemon answers with
     * clawt_computer_type_has_machine() -- not from a list of types
     * here, which would offer Stop on a backend added later or fail to
     * offer it, with nothing to say which. The GTK client puts the same
     * three verbs under a Computer submenu on the agent's right-click
     * menu; this is the surface a browser has.
     */
    if (json_object_has_member(agent, "computer_machine") &&
        json_object_get_boolean_member(agent, "computer_machine")) {
        gboolean removes =
            json_object_has_member(agent, "computer_stop_removes") &&
            json_object_get_boolean_member(agent, "computer_stop_removes");
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Power",
            removes
            ? "This machine does not survive a stop -- keep is false -- so "
              "stopping it takes everything inside with it, and starting "
              "it builds a fresh one."
            : "The machine itself, separately from the agent. Stopping it "
              "leaves it there to be started again.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autofree gchar *start = g_strdup_printf("/a/%s/computer/start",
                                                  escaped);
        g_autofree gchar *stop = g_strdup_printf("/a/%s/computer/stop",
                                                 escaped);
        g_autofree gchar *restart = g_strdup_printf("/a/%s/computer/restart",
                                                    escaped);

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_add_class(HTMX_ELEMENT(row), "computer-power");

        clawt_web_add(row, clawt_web_post_button("Start", start, NULL, NULL));

        /*
         * Asked only when there is something irreversible about it. A
         * confirmation on every stop is one people click through, which
         * is worse than none on the day it matters.
         */
        clawt_web_add(row, clawt_web_post_button(
            "Stop", stop, removes ? "danger" : NULL,
            removes ? "Everything inside this machine goes with it. "
                      "Continue?" : NULL));
        clawt_web_add(row, clawt_web_post_button(
            "Restart", restart, removes ? "danger" : NULL,
            removes ? "This machine is rebuilt rather than rebooted, so "
                      "everything inside it goes. Continue?" : NULL));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    switch (tab) {
    case CLAWT_COMPUTER_VIEW_SCREEN:
        add_screen(app, HTMX_ELEMENT(pad), agent_id);
        break;

    case CLAWT_COMPUTER_VIEW_MOUNTS:
        add_mounts(app, HTMX_ELEMENT(pad), agent_id);
        break;

    case CLAWT_COMPUTER_VIEW_EXCHANGE:
        add_exchange(app, HTMX_ELEMENT(pad));
        break;

    case CLAWT_COMPUTER_VIEW_SHELL:
        add_console(HTMX_ELEMENT(pad), agent_id, NULL, NULL);
        break;
    }

    if (tab == CLAWT_COMPUTER_VIEW_SHELL) {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Rebuild",
            "Destroys the machine and builds it again from the current "
            "settings. Some of them -- the login, the desktop, the package "
            "list -- are read once at first boot and cannot change any "
            "other way.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/a/%s/rebuild", escaped);
        g_autoptr(HtmxDiv) row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        clawt_web_add(row, clawt_web_post_button(
            "Rebuild the computer", action, "danger",
            "This destroys the machine and everything on it. Continue?"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

HtmxElement *
clawt_web_computer_body(ClawtWebApp *app, const gchar *agent_id)
{
    /*
     * The console, because it is the sub-view every computer has -- a
     * container with no desktop still runs commands. This is what
     * /a/:id/computer resolves to, so a link with no tab on it lands
     * somewhere useful rather than somewhere empty.
     */
    return clawt_web_computer_view_body(app, agent_id,
                                        CLAWT_COMPUTER_VIEW_SHELL);
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_exec(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *command = clawt_web_form_value(request, "command");
    g_autoptr(ClawtWebPayload) payload = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *html = NULL;

    if (command == NULL || *command == '\0')
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_PAGE_COMPUTER, NULL);

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "command", command);

    reply = clawt_web_app_call(app, "computer.exec",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    /*
     * The whole page again, with the output on it and the command still
     * in the box. Re-rendering the console alone would be less code and
     * would leave the state badge above it showing whatever it said
     * before the command ran.
     */
    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Computer"));
    add_console(HTMX_ELEMENT(pad), agent_id, command, clawt_web_root(reply));
    add_mounts(app, HTMX_ELEMENT(pad), agent_id);
    add_exchange(app, HTMX_ELEMENT(pad));

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_PAGE_COMPUTER, view, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_rebuild(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, "computer.rebuild",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_COMPUTER,
                                  "Rebuilt. Start the agent to bring it up.");
}

static HtmxResponse *
on_mount_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *source = clawt_web_form_value(request, "source");
    const gchar *target = clawt_web_form_value(request, "target");
    const gchar *size = clawt_web_form_value(request, "size");

    if (target == NULL || *target == '\0')
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    "A mount needs a target path inside the "
                                    "computer.");

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "source", source);
    clawt_web_payload_set(payload, "target", target);
    clawt_web_payload_set(payload, "mode",
                          clawt_web_form_value(request, "mode"));
    clawt_web_payload_set(payload, "type",
                          clawt_web_form_value(request, "type"));
    clawt_web_payload_set(payload, "relabel",
                          clawt_web_form_value(request, "relabel"));

    if (size != NULL && *size != '\0')
        clawt_web_payload_set(payload, "size", size);

    reply = clawt_web_app_call(app, "agent.mount.add",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, agent_id, CLAWT_PAGE_COMPUTER,
        "Added. Restart the agent for it to appear inside the computer.");
}

static HtmxResponse *
on_mount_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *target = htmx_request_get_query_param(request, "target");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "target", target);

    reply = clawt_web_app_call(app, "agent.mount.remove",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_COMPUTER, "Mount removed.");
}

/*
 * Start, stop or restart the machine.
 *
 * One handler for the three, taking the verb from the path: three
 * copies of this would be three chances for one of them to forget the
 * `remove` flag, and the one that forgot would be the one that fails
 * against a container.
 */
static HtmxResponse *
on_computer_power(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *verb = clawt_web_param(params, "verb");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *outcome;
    const gchar *kind;

    /*
     * Spelled out rather than assembled, for the reason the GTK client
     * spells them out: `make parity` reads the frame kinds each client
     * mentions, and one built with g_strconcat() is a kind it cannot
     * see -- so the check would report OK with this in one client only.
     *
     * It also means the verb from the URL is matched against a closed
     * set rather than pasted into a frame name.
     */
    if (g_strcmp0(verb, "start") == 0)
        kind = "computer.start";
    else if (g_strcmp0(verb, "stop") == 0)
        kind = "computer.stop";
    else if (g_strcmp0(verb, "restart") == 0)
        kind = "computer.restart";
    else
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    "That is not a power verb.");

    clawt_web_payload_set(payload, "agent", agent_id);

    /*
     * The daemon refuses a stop that destroys the machine unless it is
     * told to go ahead. The button already carried the warning and the
     * browser already asked, so the answer to that fence is yes -- the
     * fence is there for a client that does not know to warn.
     */
    clawt_web_payload_set_bool(payload, "remove", TRUE);

    reply = clawt_web_app_call(app, kind,
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    outcome = clawt_web_member(clawt_web_root(reply), "state", "?");

    {
        g_autofree gchar *said = g_strdup_printf("The machine is %s.",
                                                 outcome);

        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_PAGE_COMPUTER, said);
    }
}

/*
 * One of the four sub-views, as a page of its own.
 *
 * A path rather than a query parameter or a cookie, so a link to
 * somebody's screen survives being pasted into a chat. Registered here,
 * which is before /a/:id/:view -- a catch-all matches every path under
 * an agent, and anything after it is unreachable.
 */
static HtmxResponse *
on_computer_tab(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *tab = clawt_web_param(params, "tab");
    g_autoptr(HtmxElement) view = NULL;
    g_autofree gchar *html = NULL;

    view = clawt_web_computer_view_body(
        app, agent_id, clawt_computer_view_from_nick(tab));
    html = clawt_web_page(app, agent_id, CLAWT_PAGE_COMPUTER, view,
                          request);

    return clawt_web_html_response(html);
}

/*
 * Just the screen panel, for the poll.
 *
 * Under /f/ with the other fragments, so nothing here is a page: an
 * htmx swap that arrived with a whole document in it would nest the
 * sidebar inside the panel.
 */
static HtmxResponse *
on_screen_fragment(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) panel = build_screen_panel(app, agent_id);

    (void)request;

    return clawt_web_fragment_response(panel);
}

/*
 * The frame's bytes.
 *
 * Not a static file: the daemon may be on another machine, and this is
 * the same reasoning `agent.avatar` and `attachment.get` already record.
 * No static-file route reaches the state directory, so a clever agent id
 * cannot turn this into an arbitrary read.
 */
static HtmxResponse *
on_frame(HtmxRequest *request, GHashTable *params, gpointer user_data)
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

    /*
     * The browser asking for the picture is also the browser asking for
     * a fresh one. It has no other way to pace the capture -- the
     * daemon's timer runs whether or not anybody fetches -- and the
     * minimum gap is what stops a reload loop turning this into a grab
     * per request.
     */
    clawt_web_payload_set_bool(payload, "refresh", TRUE);

    reply = clawt_web_app_call(app, "computer.frame",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return htmx_response_not_found();

    encoded = clawt_web_member(clawt_web_root(reply), "base64", NULL);

    if (encoded == NULL)
        return htmx_response_not_found();

    raw = g_base64_decode(encoded, &length);
    bytes = g_bytes_new_take(raw, length);

    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);
    htmx_response_set_content_type(response, "image/png");

    /*
     * Never cached. The URL carries the frame's stamp, so a browser that
     * held one would be holding a picture the page has already replaced
     * -- and the whole point of the panel is that it is current.
     */
    htmx_response_add_header(response, "Cache-Control", "no-store");

    return response;
}

static HtmxResponse *
on_screen_unwatch(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "watcher", CLAWT_WEB_WATCHER_NAME);

    reply = clawt_web_app_call(app, "computer.observe_stop",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, agent_id, CLAWT_PAGE_COMPUTER,
        "Stopped watching. Nothing is being captured for this browser.");
}

static HtmxResponse *
on_screen_take(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "holder", "the web client");

    reply = clawt_web_app_call(app, "computer.takeover",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, agent_id, CLAWT_PAGE_COMPUTER,
        "You have the screen. The agent's own clicks are refused until "
        "you give it back, and the hold lapses on its own if you forget.");
}

static HtmxResponse *
on_screen_release(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, "computer.release",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_COMPUTER,
                                  "The agent has the screen back.");
}

/*
 * One event, from whichever field was filled in.
 *
 * Text first, then a key, then a click: a form with three ways to act
 * has to pick one, and typing is the thing somebody reaches for most.
 */
static HtmxResponse *
on_screen_input(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *text = clawt_web_form_value(request, "text");
    const gchar *key = clawt_web_form_value(request, "key");
    const gchar *x = clawt_web_form_value(request, "x");
    const gchar *y = clawt_web_form_value(request, "y");

    clawt_web_payload_set(payload, "agent", agent_id);

    if (text != NULL && *text != '\0') {
        clawt_web_payload_set(payload, "kind", "text");
        clawt_web_payload_set(payload, "text", text);
    } else if (key != NULL && *key != '\0') {
        clawt_web_payload_set(payload, "kind", "key");
        clawt_web_payload_set(payload, "text", key);
    } else if (x != NULL && *x != '\0' && y != NULL && *y != '\0') {
        clawt_web_payload_set(payload, "kind", "click");
        clawt_web_payload_set_int(payload, "x",
                                  g_ascii_strtoll(x, NULL, 10));
        clawt_web_payload_set_int(payload, "y",
                                  g_ascii_strtoll(y, NULL, 10));
    } else {
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    "Nothing to send: fill in text, a key, "
                                    "or a pair of coordinates.");
    }

    reply = clawt_web_app_call(app, "computer.input",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_PAGE_COMPUTER, "Sent.");
}

/*
 * A `.vv` file, which is what a desktop remote-viewer opens on a
 * double-click.
 *
 * Offered beside the `vnc://` link rather than instead of it, because
 * the two fail differently: the link needs a handler registered for the
 * scheme, and the file needs remote-viewer installed. Somebody with
 * neither is told which one they are missing by whichever they tried.
 */
static HtmxResponse *
on_screen_viewer(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *body = NULL;
    g_autoptr(GBytes) bytes = NULL;
    HtmxResponse *response;
    const gchar *uri;
    const gchar *host;
    const gchar *port;
    g_auto(GStrv) parts = NULL;

    (void)request;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, "computer.screen",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL)
        return htmx_response_not_found();

    uri = clawt_web_member(clawt_web_root(reply), "viewer", NULL);

    if (uri == NULL || !g_str_has_prefix(uri, "vnc://"))
        return htmx_response_not_found();

    parts = g_strsplit(uri + strlen("vnc://"), ":", 2);
    host = parts[0];
    port = (parts[0] != NULL) ? parts[1] : NULL;

    if (host == NULL || port == NULL)
        return htmx_response_not_found();

    body = g_strdup_printf("[virt-viewer]\ntype=vnc\nhost=%s\nport=%s\n",
                           host, port);
    bytes = g_bytes_new(body, strlen(body));

    response = htmx_response_new();
    htmx_response_set_bytes(response, bytes);
    htmx_response_set_content_type(response,
                                   "application/x-virt-viewer");

    return response;
}

void
clawt_web_register_computer(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/exec", on_exec, app);
    htmx_router_get(router, "/a/:id/frame", on_frame, app);
    htmx_router_get(router, "/f/a/:id/screen", on_screen_fragment, app);
    htmx_router_get(router, "/a/:id/screen/viewer.vv", on_screen_viewer, app);
    htmx_router_post(router, "/a/:id/screen/unwatch", on_screen_unwatch, app);
    htmx_router_post(router, "/a/:id/screen/take", on_screen_take, app);
    htmx_router_post(router, "/a/:id/screen/release", on_screen_release, app);
    htmx_router_post(router, "/a/:id/screen/input", on_screen_input, app);
    htmx_router_get(router, "/a/:id/computer/:tab", on_computer_tab, app);
    htmx_router_post(router, "/a/:id/rebuild", on_rebuild, app);
    htmx_router_post(router, "/a/:id/mount/add", on_mount_add, app);
    htmx_router_post(router, "/a/:id/mount/remove", on_mount_remove, app);
    htmx_router_post(router, "/a/:id/computer/:verb", on_computer_power, app);
}
