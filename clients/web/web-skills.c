/*
 * web-skills.c - The Skills page, and `/name` in the composer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Built from typed nodes throughout, never by appending to a string.  A
 * skill's name and description come from a file somebody else wrote --
 * an imported one from a stranger -- and everything here is served back
 * over HTTP to whoever opened the page.  htmx_node_set_text_content()
 * escapes; g_string_append() does not, and forgetting once is enough.
 */

#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/* ── The page ────────────────────────────────────────────────────── */

/*
 * The warnings, above everything else about the skill.
 *
 * Placement is the point.  A person on this page is deciding whether to
 * enable something, and a warning under a fold is a warning that gets
 * enabled around.
 */
static void
add_notes(HtmxElement *parent, JsonObject *skill)
{
    JsonArray *warnings = clawt_web_member_array(skill, "warnings");
    JsonArray *skipped = clawt_web_member_array(skill, "skipped");
    guint i;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++)
        clawt_web_add(parent,
                      clawt_web_notice(
                          json_array_get_string_element(warnings, i), ""));

    if (skipped != NULL && json_array_get_length(skipped) > 0) {
        g_autoptr(GString) list = g_string_new(NULL);

        for (i = 0; i < json_array_get_length(skipped); i++) {
            if (i > 0)
                g_string_append(list, ", ");

            g_string_append(list, json_array_get_string_element(skipped, i));
        }

        /*
         * Said, not hidden.  A skill whose steps say "run
         * scripts/setup.sh" fails in a way that reads as clawtilla being
         * broken rather than as the deliberate refusal it is.
         */
        clawt_web_add(parent, clawt_web_row("Not copied", list->str));
    }
}

static void
add_skill_card(HtmxElement *parent, JsonObject *skill)
{
    const gchar *name = clawt_web_member(skill, "name", "");
    gboolean enabled = clawt_web_member_bool(skill, "enabled", FALSE);
    g_autoptr(HtmxDiv) card = NULL;
    g_autofree gchar *escaped = NULL;
    g_autofree gchar *action = NULL;
    HtmxElement *body;

    card = clawt_web_card(name, clawt_web_member(skill, "description", NULL));
    body = clawt_web_card_body(card);

    clawt_web_add(body, clawt_web_badge(enabled ? "enabled" : "disabled",
                                        enabled ? "good" : "warn"));
    clawt_web_add(body, clawt_web_badge(
        clawt_web_member(skill, "source", "user"), "neutral"));

    if (!enabled)
        clawt_web_add(body, clawt_web_text(
            "Nothing in this skill reaches an agent until it is enabled. "
            "Read it first: what a skill says goes into a model's context "
            "with your agent's own authority.", "lede"));

    add_notes(body, skill);

    if (clawt_web_member(skill, "origin", NULL) != NULL)
        clawt_web_add(body, clawt_web_row("From",
                                          clawt_web_member(skill, "origin",
                                                           "")));

    if (clawt_web_member(skill, "sha256", NULL) != NULL)
        clawt_web_add(body, clawt_web_row("SHA-256",
                                          clawt_web_member(skill, "sha256",
                                                           "")));

    clawt_web_add(body, clawt_web_row("Directory",
                                      clawt_web_member(skill, "directory",
                                                       "")));

    escaped = g_uri_escape_string(name, NULL, FALSE);
    action = g_strdup_printf("/skills/%s/%s", escaped,
                             enabled ? "disable" : "enable");

    clawt_web_add(body, clawt_web_post_button(
        enabled ? "Disable" : "Enable", action,
        enabled ? "default" : "primary", NULL));

    /* Its saved tests, and running them. */
    {
        g_autofree gchar *tests = g_strdup_printf("/skills/%s/tests", escaped);
        HtmxA *link = htmx_a_new_with_href(tests);

        htmx_node_set_text_content(HTMX_NODE(link), "Tests");
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(link));
    }

    g_free(action);
    action = g_strdup_printf("/skills/%s/remove", escaped);

    clawt_web_add(body, clawt_web_post_button(
        "Remove", action, "danger",
        "Delete this skill and take its links out of every workspace?"));

    /*
     * The body is shown in full rather than behind a link.
     *
     * This page exists so that somebody reads a skill before enabling
     * it, and a review that needs a second click is a review that does
     * not happen.
     */
    clawt_web_add(body, clawt_web_text(clawt_web_member(skill, "body", ""),
                                       "pre"));

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── Teaching a task ─────────────────────────────────────────────── */

/*
 * One recording, with what can be done to it.
 *
 * The caveat is inside the card rather than once at the top of the
 * page: this is where somebody decides whether the trace is safe to
 * keep, and a warning several cards away is one they have scrolled
 * past.
 */
static void
add_recording_card(ClawtWebApp *app, HtmxElement *parent, JsonObject *trace)
{
    const gchar *id = clawt_web_member(trace, "id", "?");
    gboolean active = clawt_web_member_bool(trace, "active", FALSE);
    g_autofree gchar *subtitle = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    JsonArray *caveats;
    guint i;

    (void)app;

    subtitle = g_strdup_printf(
        "%s, agent %s, %" G_GINT64_FORMAT " step(s)%s",
        clawt_web_member(trace, "source", "?"),
        clawt_web_member(trace, "agent", "-"),
        json_object_get_int_member(trace, "step_count"),
        active ? " -- recording now" : "");

    card = clawt_web_card(clawt_web_member(trace, "goal", id), subtitle);
    body = clawt_web_card_body(card);

    caveats = clawt_web_member_array(trace, "caveats");

    for (i = 0; caveats != NULL && i < json_array_get_length(caveats); i++)
        clawt_web_add(body, clawt_web_notice(
            json_array_get_string_element(caveats, i), "warn"));

    if (json_object_get_int_member(trace, "dropped") > 0) {
        g_autofree gchar *note = g_strdup_printf(
            "%" G_GINT64_FORMAT " step(s) were not recorded: the recording "
            "reached skills.teach_max_events.",
            json_object_get_int_member(trace, "dropped"));

        clawt_web_add(body, clawt_web_text(note, NULL));
    }

    {
        g_autoptr(HtmxDiv) actions = htmx_div_new();
        g_autofree gchar *base = g_strdup_printf("/teach/%s", id);

        htmx_element_add_class(HTMX_ELEMENT(actions), "row-actions");

        if (active) {
            g_autofree gchar *path = g_strconcat(base, "/stop", NULL);

            clawt_web_add(actions, clawt_web_post_button("Stop", path,
                                                         "danger", NULL));
        } else {
            g_autofree gchar *steps = g_strconcat(base, "/steps", NULL);
            g_autofree gchar *draft = g_strconcat(base, "/draft", NULL);
            g_autofree gchar *commit = g_strconcat(base, "/commit", NULL);
            g_autofree gchar *remove = g_strconcat(base, "/remove", NULL);

            clawt_web_add(actions, clawt_web_post_button("Steps", steps,
                                                         "default", NULL));
            clawt_web_add(actions, clawt_web_post_button("Draft a skill",
                                                         draft, "primary",
                                                         NULL));
            clawt_web_add(actions, clawt_web_post_button("Commit", commit,
                                                         "default", NULL));
            clawt_web_add(actions, clawt_web_post_button("Remove", remove,
                                                         "danger", NULL));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(actions));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * The teach section of the skills page.
 *
 * Built here rather than on a page of its own: a recording exists to
 * become a skill, and putting the two on separate pages would mean the
 * draft you are reviewing and the library it lands in are never on the
 * screen together.
 */
static void
add_teach_section(ClawtWebApp *app, HtmxElement *parent)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "teach.list", NULL);
    JsonArray *recordings = NULL;
    const gchar *kinds[8] = { NULL };
    const gchar *labels[8] = { NULL };
    guint count;
    guint i;

    clawt_web_add(parent, clawt_web_section_title("Teach a task"));
    clawt_web_add(parent, clawt_web_text(
        "Record a task being done, then have a model write the procedure "
        "up as a skill. Watching the agent captures the calls it makes; "
        "demonstrating captures every key you press, in any window. The "
        "draft lands disabled, with the same checks an imported skill "
        "gets.", "lede"));

    if (reply != NULL)
        recordings = clawt_web_member_array(clawt_web_root(reply),
                                            "recordings");

    if (recordings == NULL || json_array_get_length(recordings) == 0)
        clawt_web_add(parent, clawt_web_empty(
            "No recordings",
            "Record one below, then draft a skill from it."));

    for (i = 0; recordings != NULL && i < json_array_get_length(recordings);
         i++)
        add_recording_card(app, parent,
                           json_array_get_object_element(recordings, i));

    /*
     * The kinds are walked from the library, never spelled out here.
     * A client with its own copy is a client that can offer a recorder
     * the daemon does not have, or miss one it does.
     */
    count = clawt_teach_source_count();

    if (count > G_N_ELEMENTS(kinds) - 1)
        count = G_N_ELEMENTS(kinds) - 1;

    for (i = 0; i < count; i++) {
        kinds[i] = clawt_teach_source_nth_nick(i);
        labels[i] = clawt_teach_source_nth_label(i);
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Record a task",
            "The agent has to exist and, for a demonstration, to have "
            "computer.desktop.allow_recording turned on.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/teach/start");

        clawt_web_add(form, clawt_web_field("Agent", "agent", NULL,
                                            "builder"));
        clawt_web_add(form, clawt_web_select_field(
            "What to watch", "source", kinds, labels, kinds[0]));
        clawt_web_add(form, clawt_web_field("What you are teaching", "goal",
                                            NULL, "cut a release"));
        clawt_web_add(form, clawt_web_button("Record", "primary"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
    }
}

/*
 * What agents have proposed and nobody has answered yet.
 *
 * Here as well as in the inbox, because this is where somebody looks
 * when a disabled skill they did not write has appeared -- and the
 * answer is who proposed it and why.  Reviewing happens on the
 * Decisions page, where the text is shown: Approve here would be a way
 * to approve something without having read it.
 */
static void
web_pending_proposals(ClawtWebApp *app, HtmxDiv *pad)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "skill.proposals", NULL);
    JsonArray *proposals = clawt_web_member_array(clawt_web_root(reply),
                                                  "proposals");
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    guint i;

    if (proposals == NULL || json_array_get_length(proposals) == 0)
        return;

    card = clawt_web_card(
        "Proposed by agents",
        "Waiting for you in Decisions. A new skill is in the library "
        "disabled; a change is staged and the skill is unchanged.");
    body = clawt_web_card_body(card);

    for (i = 0; i < json_array_get_length(proposals); i++) {
        JsonObject *proposal = json_array_get_object_element(proposals, i);
        const gchar *reason = clawt_web_member(proposal, "reason", NULL);
        gboolean update = g_strcmp0(clawt_web_member(proposal, "kind", ""),
                                    "update") == 0;
        g_autofree gchar *age = clawt_web_relative_time(
            clawt_web_member_int(proposal, "created_at", 0) / G_USEC_PER_SEC);
        g_autofree gchar *line = g_strdup_printf(
            "%s proposes %s%s%s",
            clawt_web_member(proposal, "agent", "an agent"),
            update ? "a change to this skill" : "a new skill",
            (age != NULL && *age != '\0') ? ", " : "",
            age != NULL ? age : "");
        g_autoptr(HtmxDiv) item = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autoptr(HtmxA) review = htmx_a_new_with_href("/decisions");

        htmx_element_add_class(HTMX_ELEMENT(item), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
        clawt_web_add(head, clawt_web_badge(
            clawt_web_member(proposal, "name", "?"),
            update ? "info" : "neutral"));
        htmx_node_set_text_content(HTMX_NODE(review), "Review");
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(review));
        htmx_node_add_child(HTMX_NODE(item), HTMX_NODE(head));

        clawt_web_add(item, clawt_web_text(line, "small muted"));

        if (reason != NULL && *reason != '\0')
            clawt_web_add(item, clawt_web_text(reason, "small"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(item));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
}

HtmxElement *
clawt_web_skills_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "skill.list", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *skills;
    JsonArray *problems;
    guint i;

    (void)agent_id;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Skills"));
    clawt_web_add(pad, clawt_web_text(
        "A procedure written once and linked into whichever agents need "
        "it, at whatever path each agent's own CLI reads. An imported "
        "skill arrives disabled and its scripts are left behind -- "
        "markdown only, on purpose.", "lede"));

    web_pending_proposals(app, pad);

    problems = clawt_web_member_array(root, "problems");

    for (i = 0; problems != NULL && i < json_array_get_length(problems); i++)
        clawt_web_add(pad, clawt_web_notice(
            json_array_get_string_element(problems, i), "bad"));

    skills = clawt_web_member_array(root, "skills");

    if (skills == NULL || json_array_get_length(skills) == 0) {
        /*
         * Two causes, and they send a reader to different places.
         * Anywhere an empty result could read as an answer, say why it
         * is empty.
         */
        if (root != NULL && !clawt_web_member_bool(root, "enabled", FALSE))
            clawt_web_add(pad, clawt_web_empty(
                "Skills are turned off",
                "Set skills.enabled to scan and link them."));
        else
            clawt_web_add(pad, clawt_web_empty(
                "No skills yet",
                clawt_web_member(root, "directory",
                                 "Nothing in the skills directory.")));
    }

    for (i = 0; skills != NULL && i < json_array_get_length(skills); i++)
        add_skill_card(HTMX_ELEMENT(pad), json_array_get_object_element(
                           skills, i));

    /*
     * Rescan.
     *
     * The library follows its directory with a GFileMonitor, so this is
     * rarely needed -- but a filesystem with no inotify (a network
     * share, a container bind) silently has no watch at all, and
     * without a button the only remedy is restarting the daemon.
     */
    clawt_web_add(pad, clawt_web_post_button("Rescan", "/skills/reload",
                                             "default", NULL));

    /* ── Importing ── */
    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Import a skill",
            "A directory holding a SKILL.md, on this machine. It arrives "
            "disabled, and any script beside it is left where it is.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/skills/import");

        clawt_web_add(form, clawt_web_field(
            "Directory", "source", NULL, "/home/you/src/repo/skills/release"));
        clawt_web_add(form, clawt_web_field(
            "Where it came from", "origin", NULL,
            "https://example.org/some-repo"));
        clawt_web_add(form, clawt_web_button("Import", "primary"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    /* ── Writing one ── */
    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Write a skill",
            "Lowercase letters, digits and single hyphens. The description "
            "is the only part an agent reads before deciding to open it, so "
            "write it as \"use this when ...\".");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/skills/new");

        clawt_web_add(form, clawt_web_field("Name", "name", NULL,
                                            "release-notes"));
        clawt_web_add(form, clawt_web_field(
            "Description", "description", NULL,
            "Use this when cutting a release."));
        clawt_web_add(form, clawt_web_textarea_field("Body", "body", NULL, 8));
        clawt_web_add(form, clawt_web_button("Create", "primary"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    add_teach_section(app, HTMX_ELEMENT(pad));

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── The composer's `/` completions ──────────────────────────────── */

/*
 * The list a `/` in the composer drops down.
 *
 * Served as a fragment rather than baked into the chat page, because a
 * skill can be assigned or enabled while somebody has the page open and
 * a list rendered at page load would be stale exactly when it mattered.
 * It is still not fetched *at* page load -- htmx asks for it on the
 * first keystroke.
 */
static HtmxResponse *
on_commands(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) list = htmx_div_new();
    JsonArray *commands;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(list), "slash-list");
    htmx_element_set_id(HTMX_ELEMENT(list), "slash-list");

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "skill.commands",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    commands = clawt_web_member_array(clawt_web_root(reply), "commands");

    for (i = 0; commands != NULL && i < json_array_get_length(commands);
         i++) {
        JsonObject *command = json_array_get_object_element(commands, i);
        g_autoptr(HtmxButton) button = NULL;
        g_autoptr(HtmxSpan) label = htmx_span_new();
        g_autoptr(HtmxSpan) hint = htmx_span_new();
        g_autofree gchar *slash = g_strconcat("/",
                                              clawt_web_member(command,
                                                               "name", ""),
                                              NULL);

        button = htmx_button_new();
        htmx_element_add_class(HTMX_ELEMENT(button), "slash-item");
        htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "button");
        htmx_element_set_attribute(HTMX_ELEMENT(button), "data-command",
                                   slash);

        htmx_element_add_class(HTMX_ELEMENT(label), "slash-name");
        htmx_node_set_text_content(HTMX_NODE(label), slash);
        clawt_web_add(button, g_steal_pointer(&label));

        htmx_element_add_class(HTMX_ELEMENT(hint), "slash-hint");
        htmx_node_set_text_content(
            HTMX_NODE(hint),
            clawt_web_member(command, "description", ""));
        clawt_web_add(button, g_steal_pointer(&hint));

        clawt_web_add(list, g_steal_pointer(&button));
    }

    if (commands == NULL || json_array_get_length(commands) == 0)
        clawt_web_add(list, clawt_web_text(
            "This agent has no commands. Assign it a skill.", "slash-hint"));

    return clawt_web_fragment_response(HTMX_ELEMENT(list));
}

/*
 * Expand daemon-side and post the result.
 *
 * The client never substitutes anything itself: both clients send the
 * same text for the same `/name args` because there is one
 * implementation of the substitution, in the daemon.
 */
HtmxResponse *
clawt_web_run_skill_command(ClawtWebApp *app, HtmxRequest *request,
                            const gchar *agent_id, const gchar *name,
                            const gchar *arguments)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *prompt;

    if (name == NULL || *name == '\0')
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_CHAT,
                                    "no command was named");

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set(payload, "arguments", arguments);

    reply = clawt_web_app_call(app, "skill.expand",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    /*
     * The GTK client's sentence, word for word.
     *
     * It reaches the same conclusion the same way -- ask the daemon to
     * expand it, and treat a refusal as "there is no such command" --
     * so the two must say the same thing.  The daemon's own wording is
     * accurate and stops short of the useful half: which is that the
     * list of what does exist is one command away.
     */
    if (reply == NULL) {
        g_autofree gchar *message = g_strdup_printf(
            "There is no /%s. Type /help for the list.", name);

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_CHAT, message);
    }

    prompt = clawt_web_member(clawt_web_root(reply), "prompt", NULL);

    if (prompt == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_PAGE_CHAT,
                                    "that command expanded to nothing");

    /*
     * Sent through the one function that sends a message.
     *
     * This built its own frame and named it `message.send`, which no
     * handler answers -- the daemon's kind is `msg.send`, and that
     * spelling appeared exactly once in the tree.  So a skill command
     * expanded correctly and then went nowhere, and the page said the
     * action had happened.  `make parity` could not see it either: it
     * intersects the two clients' kinds with the daemon's, which
     * compares the clients against each other and says nothing about a
     * kind neither of them shares with the daemon.
     *
     * Going through clawt_web_send_message() also picks up the two
     * things this copy never did: clearing the draft, and saying so
     * when the message was held because the agent is mid-turn.
     */
    return clawt_web_send_message(app, request, agent_id, prompt);
}

/*
 * The same, from the skills page's own form.
 */
static HtmxResponse *
on_expand(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");

    return clawt_web_run_skill_command(
               app, request, agent_id,
               clawt_web_form_value(request, "command"),
               clawt_web_form_value(request, "arguments"));
}

/* ── Actions ─────────────────────────────────────────────────────── */

typedef struct {
    ClawtWebApp *app;
    gboolean     enable;
} EnableAction;

static HtmxResponse *
on_enable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    EnableAction *action = user_data;
    g_autofree gchar *name = clawt_web_param(params, "skill");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set_bool(payload, "enabled", action->enable);

    reply = clawt_web_app_call(action->app, "skill.enable",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied at the point of failure: the borrowed string is freed
         * by the next call, and rendering makes several.
         */
        g_autofree gchar *message =
            g_strdup(clawt_web_app_last_error(action->app));

        return clawt_web_error_page(action->app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(action->app, request, NULL,
                                  CLAWT_PAGE_SKILLS,
                                  action->enable ? "Enabled." : "Disabled.");
}

static HtmxResponse *
on_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "skill");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "skill.remove",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_SKILLS,
                                  "Removed.");
}

/*
 * A skill's tests: each with how it last ran, and a form to run them on
 * an agent that has the skill.
 */
static HtmxResponse *
tests_page(ClawtWebApp *app, HtmxRequest *request, const gchar *name,
           const gchar *notice)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/skills/%s/tests/run",
                                               escaped);
    g_autofree gchar *heading = g_strdup_printf("Tests of %s", name);
    g_autofree gchar *html = NULL;
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    JsonArray *tests;
    guint i;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
    clawt_web_add(pad, clawt_web_section_title(heading));

    if (notice != NULL)
        clawt_web_add(pad, clawt_web_text(notice, "lede"));

    clawt_web_payload_set(payload, "skill", name);
    reply = clawt_web_app_call(app, "skill.tests",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    tests = clawt_web_member_array(clawt_web_root(reply), "tests");

    if (tests == NULL || json_array_get_length(tests) == 0)
        clawt_web_add(pad, clawt_web_empty("No tests yet",
            "Add tests/<name>.yaml beside the SKILL.md: input, and what the "
            "answer must (expect) and must not (reject) say."));

    for (i = 0; tests != NULL && i < json_array_get_length(tests); i++) {
        JsonObject *test = json_array_get_object_element(tests, i);
        JsonObject *last = json_object_has_member(test, "last")
            ? json_object_get_object_member(test, "last") : NULL;

        clawt_web_add(pad, clawt_web_row(clawt_web_member(test, "name", ""),
            last != NULL ? clawt_web_member(last, "state", "") : "never run"));
        if (last != NULL && clawt_web_member(last, "detail", NULL) != NULL)
            clawt_web_add(pad, clawt_web_text(clawt_web_member(last, "detail",
                                                               ""),
                                              "small muted"));
    }

    clawt_web_add(form, clawt_web_field("Agent", "agent", NULL,
                                        "one that has this skill"));
    {
        g_autoptr(HtmxButton) run = clawt_web_button("Run them", "primary");

        htmx_element_set_attribute(HTMX_ELEMENT(run), "type", "submit");
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(run));
    }
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(g_steal_pointer(&pad)));
    html = clawt_web_page(app, NULL, CLAWT_PAGE_SKILLS, view, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_tests(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *name = clawt_web_param(params, "skill");

    return tests_page(user_data, request, name, NULL);
}

static HtmxResponse *
on_tests_run(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "skill");
    const gchar *agent = clawt_web_form_value(request, "agent");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "skill", name);
    if (agent != NULL && *agent != '\0')
        clawt_web_payload_set(payload, "agent", agent);

    reply = clawt_web_app_call(app, "skill.test_run",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    return tests_page(app, request, name,
                      reply == NULL ? clawt_web_app_last_error(app)
                                    : "Running; reload for the verdicts once "
                                      "they finish.");
}

static HtmxResponse *
on_reload(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    reply = clawt_web_app_call(app, "skill.reload", NULL);

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_SKILLS,
                                  "Rescanned.");
}

static HtmxResponse *
on_import(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;

    (void)params;

    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "source",
                          clawt_web_form_value(request, "source"));
    clawt_web_payload_set(payload, "origin",
                          clawt_web_form_value(request, "origin"));

    reply = clawt_web_app_call(app, "skill.import",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(
        app, request, NULL, CLAWT_PAGE_SKILLS,
        "Imported, disabled. Read it, then enable it.");
}

static HtmxResponse *
on_new(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;

    (void)params;

    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "name",
                          clawt_web_form_value(request, "name"));
    clawt_web_payload_set(payload, "description",
                          clawt_web_form_value(request, "description"));
    clawt_web_payload_set(payload, "body",
                          clawt_web_form_value(request, "body"));

    reply = clawt_web_app_call(app, "skill.create",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_SKILLS,
                                  "Written.");
}

/* ── Teaching a task: the routes ─────────────────────────────────── */

/*
 * One of the id-only teach verbs.
 *
 * Four routes that differ in the frame kind and the sentence afterwards,
 * so they share one handler and a table rather than being four copies
 * of the same twenty lines -- the copies would drift on the error path,
 * which is the one nobody exercises.
 */
typedef struct {
    ClawtWebApp *app;
    const gchar *kind;
    const gchar *done;
} TeachAction;

static HtmxResponse *
on_teach_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    TeachAction *action = user_data;
    g_autofree gchar *id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);

    reply = clawt_web_app_call(action->app, action->kind,
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        /*
         * Copied at the point of failure: the borrowed string is freed
         * by the next call, and rendering makes several.
         */
        g_autofree gchar *message =
            g_strdup(clawt_web_app_last_error(action->app));

        return clawt_web_error_page(action->app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(action->app, request, NULL,
                                  CLAWT_PAGE_SKILLS, action->done);
}

/*
 * The steps, which are not in the listing.
 *
 * A demonstration can be twenty thousand steps, so `teach.show` is a
 * separate request and this renders it on its own page rather than
 * making every listing carry it.
 */
static HtmxResponse *
on_teach_steps(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "id");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GString) text = g_string_new(NULL);
    JsonObject *trace;
    JsonArray *steps;
    guint i;

    clawt_web_payload_set(payload, "id", id);

    reply = clawt_web_app_call(app, "teach.show",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    trace = clawt_web_root(reply);
    steps = clawt_web_member_array(trace, "steps");

    for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
        JsonObject *step = json_array_get_object_element(steps, i);

        g_string_append_printf(text, "%u. [%s] %s\n", i + 1,
                               clawt_web_member(step, "kind", "?"),
                               clawt_web_member(step, "label", ""));

        if (clawt_web_member(step, "detail", NULL) != NULL)
            g_string_append_printf(text, "    %s\n",
                                   clawt_web_member(step, "detail", ""));
    }

    return clawt_web_after_action(
        app, request, NULL, CLAWT_PAGE_SKILLS,
        (text->len > 0) ? text->str : "Nothing was captured.");
}

static HtmxResponse *
on_teach_start(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    clawt_web_payload_set(payload, "agent",
                          clawt_web_form_value(request, "agent"));
    clawt_web_payload_set(payload, "source",
                          clawt_web_form_value(request, "source"));
    clawt_web_payload_set(payload, "goal",
                          clawt_web_form_value(request, "goal"));

    reply = clawt_web_app_call(app, "teach.start",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, NULL,
                                    CLAWT_PAGE_SKILLS, message);
    }

    return clawt_web_after_action(
        app, request, NULL, CLAWT_PAGE_SKILLS,
        "Recording. Read the trace before you turn it into a skill.");
}

static TeachAction *
teach_action_new(ClawtWebApp *app, const gchar *kind, const gchar *done)
{
    TeachAction *action = g_new0(TeachAction, 1);

    action->app = app;
    action->kind = kind;
    action->done = done;

    return action;
}

static EnableAction *
enable_action_new(ClawtWebApp *app, gboolean enable)
{
    EnableAction *action = g_new0(EnableAction, 1);

    action->app = app;
    action->enable = enable;

    return action;
}

/*
 * Assignment is deliberately not here.
 *
 * `agents.skills` is an ordinary agent setting, so both clients edit it
 * on the agent page through the same generic schema-driven field --
 * which means there is one place it is done and one implementation
 * behind it. A second route here would be a second way to set the same
 * thing, and the worse one: it would know nothing about the agent's
 * other settings or about whether the save was refused.
 */
void
clawt_web_register_skills(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/a/:id/commands", on_commands, app);
    htmx_router_post(router, "/a/:id/expand", on_expand, app);
    htmx_router_post(router, "/skills/reload", on_reload, app);
    htmx_router_post(router, "/skills/import", on_import, app);
    htmx_router_post(router, "/skills/new", on_new, app);
    htmx_router_post(router, "/skills/:skill/enable", on_enable,
                     enable_action_new(app, TRUE));
    htmx_router_post(router, "/skills/:skill/disable", on_enable,
                     enable_action_new(app, FALSE));
    htmx_router_post(router, "/skills/:skill/remove", on_remove, app);
    htmx_router_get(router, "/skills/:skill/tests", on_tests, app);
    htmx_router_post(router, "/skills/:skill/tests/run", on_tests_run, app);

    htmx_router_post(router, "/teach/start", on_teach_start, app);
    htmx_router_post(router, "/teach/:id/steps", on_teach_steps, app);
    htmx_router_post(router, "/teach/:id/stop", on_teach_action,
                     teach_action_new(app, "teach.stop", "Stopped."));
    htmx_router_post(router, "/teach/:id/draft", on_teach_action,
                     teach_action_new(app, "teach.synthesize",
                                      "Drafted. Read it, then commit it."));
    htmx_router_post(router, "/teach/:id/commit", on_teach_action,
                     teach_action_new(app, "teach.commit",
                                      "Written, and disabled. Read it "
                                      "before you enable it."));
    htmx_router_post(router, "/teach/:id/remove", on_teach_action,
                     teach_action_new(app, "teach.remove", "Removed."));
}
