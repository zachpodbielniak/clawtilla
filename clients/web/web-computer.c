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
        gint64 status = clawt_web_member_int(result, "exit_status", -1);
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
        static const gchar *const relabels[] = {
            "none", "shared", "private", NULL
        };
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/a/%s/mount/add", escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxDiv) grid = htmx_div_new();

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
            "SELinux relabel", "relabel", relabels, NULL, "none"));
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

/* ── The view ────────────────────────────────────────────────────── */

HtmxElement *
clawt_web_computer_body(ClawtWebApp *app, const gchar *agent_id)
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

    add_console(HTMX_ELEMENT(pad), agent_id, NULL, NULL);
    add_mounts(app, HTMX_ELEMENT(pad), agent_id);
    add_exchange(app, HTMX_ELEMENT(pad));

    {
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
                                      CLAWT_WEB_VIEW_COMPUTER, NULL);

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "command", command);

    reply = clawt_web_app_call(app, "computer.exec",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_COMPUTER,
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

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_COMPUTER, view, request);

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
                                    CLAWT_WEB_VIEW_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_COMPUTER,
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
                                    CLAWT_WEB_VIEW_COMPUTER,
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
                                    CLAWT_WEB_VIEW_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, agent_id, CLAWT_WEB_VIEW_COMPUTER,
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
                                    CLAWT_WEB_VIEW_COMPUTER,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_COMPUTER, "Mount removed.");
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
                                    CLAWT_WEB_VIEW_COMPUTER,
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
                                    CLAWT_WEB_VIEW_COMPUTER,
                                    clawt_web_app_last_error(app));

    outcome = clawt_web_member(clawt_web_root(reply), "state", "?");

    {
        g_autofree gchar *said = g_strdup_printf("The machine is %s.",
                                                 outcome);

        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_COMPUTER, said);
    }
}

void
clawt_web_register_computer(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/exec", on_exec, app);
    htmx_router_post(router, "/a/:id/rebuild", on_rebuild, app);
    htmx_router_post(router, "/a/:id/mount/add", on_mount_add, app);
    htmx_router_post(router, "/a/:id/mount/remove", on_mount_remove, app);
    htmx_router_post(router, "/a/:id/computer/:verb", on_computer_power, app);
}
