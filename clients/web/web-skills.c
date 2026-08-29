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
static HtmxResponse *
on_expand(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *name = clawt_web_form_value(request, "command");
    const gchar *arguments = clawt_web_form_value(request, "arguments");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(ClawtWebPayload) send = NULL;
    g_autoptr(JsonNode) sent = NULL;
    const gchar *prompt;

    if (name == NULL || *name == '\0')
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    "no command was named");

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set(payload, "arguments", arguments);

    reply = clawt_web_app_call(app, "skill.expand",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *message =
            g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT, message);
    }

    prompt = clawt_web_member(clawt_web_root(reply), "prompt", NULL);

    if (prompt == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    "that command expanded to nothing");

    send = clawt_web_payload_new();
    clawt_web_payload_set(send, "agent", agent_id);
    clawt_web_payload_set(send, "body", prompt);

    sent = clawt_web_app_call(app, "message.send",
                              clawt_web_payload_take(g_steal_pointer(&send)));

    if (sent == NULL) {
        g_autofree gchar *message =
            g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT, message);
    }

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, NULL);
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
                                    CLAWT_WEB_VIEW_SKILLS, message);
    }

    return clawt_web_after_action(action->app, request, NULL,
                                  CLAWT_WEB_VIEW_SKILLS,
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
                                    CLAWT_WEB_VIEW_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_WEB_VIEW_SKILLS,
                                  "Removed.");
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
                                    CLAWT_WEB_VIEW_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_WEB_VIEW_SKILLS,
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
                                    CLAWT_WEB_VIEW_SKILLS, message);
    }

    return clawt_web_after_action(
        app, request, NULL, CLAWT_WEB_VIEW_SKILLS,
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
                                    CLAWT_WEB_VIEW_SKILLS, message);
    }

    return clawt_web_after_action(app, request, NULL, CLAWT_WEB_VIEW_SKILLS,
                                  "Written.");
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
}
