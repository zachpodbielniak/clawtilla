/*
 * web-work.c - Routines, tasks, and what the agents are saying to each other
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three views in one file because they are three answers to one question
 * -- what is this fleet doing -- and they share their row rendering.
 */

#include "web-pages.h"

#include <string.h>

/* ── Routines ────────────────────────────────────────────────────── */

static void
add_routine_form(HtmxElement *parent, JsonObject *existing)
{
    const gchar *id = clawt_web_member(existing, "id", NULL);
    g_autofree gchar *action = NULL;
    g_autoptr(HtmxForm) form = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    if (id != NULL) {
        g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);

        action = g_strdup_printf("/routines/%s/save", escaped);
    } else {
        action = g_strdup("/routines/add");
    }

    card = clawt_web_card(
        id != NULL ? id : "New routine",
        id != NULL ? NULL
                   : "Work that happens on a schedule rather than because "
                     "somebody asked.");
    body = clawt_web_card_body(card);
    form = clawt_web_form(action);

    if (id == NULL)
        clawt_web_add(form, clawt_web_field("Id", "id", NULL,
                                            "morning-standup"));

    /*
     * The fields come from the schema, the same table `routine.add`
     * walks on the other side. Two lists of a routine's keys would
     * differ the first time one was added, and the symptom is a field
     * that is accepted, reported as saved, and never read.
     */
    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "routines."))
            continue;

        leaf = entry->key + strlen("routines.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        if (entry->type == CLAWT_SCHEMA_SECTION ||
            entry->type == CLAWT_SCHEMA_MAPPING ||
            entry->type == CLAWT_SCHEMA_LIST_OF)
            continue;

        value = clawt_web_member(existing, leaf, NULL);

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            clawt_web_add(form, clawt_web_switch_field(
                leaf, leaf, entry->doc, g_strcmp0(value, "true") == 0));
            continue;
        }

        if (g_strcmp0(leaf, "instructions") == 0) {
            clawt_web_add(form, clawt_web_textarea_field(
                "Instructions", leaf, value, 6));
            continue;
        }

        clawt_web_add(form, clawt_web_field(leaf, leaf, value, entry->doc));
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button(
            id != NULL ? "Save" : "Add routine", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));

        if (id != NULL) {
            g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
            g_autofree gchar *run = g_strdup_printf("/routines/%s/run",
                                                    escaped);
            g_autofree gchar *remove = g_strdup_printf("/routines/%s/remove",
                                                       escaped);

            clawt_web_add(row, clawt_web_post_button("Run now", run,
                                                     "default", NULL));
            clawt_web_add(row, clawt_web_post_button(
                "Remove", remove, "danger", "Remove this routine?"));
        }

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

HtmxElement *
clawt_web_routines_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "routine.list", NULL);
    JsonArray *routines;
    guint i;

    (void)agent_id;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Routines"));
    clawt_web_add(pad, clawt_web_text(
        "A routine that did not fire because the machine was asleep has "
        "not failed. catch_up runs it once however many were missed -- a "
        "laptop opened after a long weekend should not deliver a stack of "
        "good mornings.", "lede"));

    routines = clawt_web_member_array(clawt_web_root(reply), "routines");

    if (routines == NULL || json_array_get_length(routines) == 0)
        clawt_web_add(pad, clawt_web_empty("No routines", NULL));

    for (i = 0; routines != NULL && i < json_array_get_length(routines); i++) {
        JsonObject *routine = json_array_get_object_element(routines, i);

        /*
         * A schedule that can never fire is shown where it is set. The
         * daemon reports it as `problem` because a cron expression is
         * easy to get subtly wrong -- `0 0 13 * 5` is the thirteenth
         * *and* every Friday, not Friday the thirteenth.
         */
        if (clawt_web_member(routine, "problem", NULL) != NULL)
            clawt_web_add(pad, clawt_web_notice(
                clawt_web_member(routine, "problem", ""), "bad"));

        add_routine_form(HTMX_ELEMENT(pad), routine);
    }

    add_routine_form(HTMX_ELEMENT(pad), NULL);

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Tasks ───────────────────────────────────────────────────────── */

/*
 * The tone comes from the library, and the nickname is turned into the
 * enum rather than compared against.
 *
 * This used to be three string comparisons written from memory, two of
 * which -- "done" and "complete" -- are not #ClawtTaskState nicknames at
 * all. So a finished task fell through to "neutral" and had never been
 * drawn green; nothing reported it because a badge in the wrong colour
 * reads as a decision somebody made.
 *
 * A state the daemon sends that this build does not know is neutral, not
 * a refusal: a client one version behind should draw the row.
 */
static const gchar *
task_tone(const gchar *state)
{
    gint value = 0;

    if (!clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
        return "neutral";

    return clawt_task_state_tone((ClawtTaskState)value);
}

HtmxElement *
clawt_web_tasks_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) usage = NULL;
    JsonArray *tasks;
    guint i;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Tasks"));

    clawt_web_payload_set_bool(payload, "all", TRUE);
    reply = clawt_web_app_call(app, "task.list",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    usage = clawt_web_app_call(app, "usage.summary", NULL);

    {
        gdouble budget = 0.0;
        JsonObject *root = clawt_web_root(usage);

        if (root != NULL && json_object_has_member(root, "task_budget_usd"))
            budget = json_object_get_double_member(root, "task_budget_usd");

        if (budget > 0.0) {
            g_autofree gchar *text = g_strdup_printf(
                "One task -- and everything it spawns -- may spend $%.2f. "
                "Past that its messages are refused, naming the spend and "
                "the cap. Ordinary conversation is not capped: spend is "
                "booked against a task id, and a chat has none.", budget);

            clawt_web_add(pad, clawt_web_text(text, "lede"));
        }
    }

    tasks = clawt_web_member_array(clawt_web_root(reply), "tasks");

    if (tasks == NULL || json_array_get_length(tasks) == 0) {
        clawt_web_add(pad, clawt_web_empty(
            "No tasks",
            "A task is delegated work -- one agent handing another a job, "
            "with its own libreclaw session so one job never contaminates "
            "the next."));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(tasks); i++) {
            JsonObject *task = json_array_get_object_element(tasks, i);
            const gchar *id = clawt_web_member(task, "id", "?");
            const gchar *state = clawt_web_member(task, "state", "?");
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            g_autofree gchar *who = g_strdup_printf(
                "%s → %s", clawt_web_member(task, "origin", "?"),
                clawt_web_member(task, "assignee", "?"));
            g_autofree gchar *prompt = clawt_web_one_line(
                clawt_web_member(task, "prompt", ""), 200);

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(label),
                                       "list-item-title");
                htmx_node_set_text_content(HTMX_NODE(label), who);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
            }

            clawt_web_add(head, clawt_web_badge(state, task_tone(state)));
            clawt_web_add(head, clawt_web_badge(id, "neutral"));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

            clawt_web_add(row, clawt_web_text(prompt, "list-item-sub"));

            if (clawt_web_member(task, "result", NULL) != NULL) {
                g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

                htmx_element_add_class(pre, "console");
                htmx_node_set_text_content(
                    HTMX_NODE(pre), clawt_web_member(task, "result", ""));
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(pre));
            }

            if (clawt_web_member(task, "reason", NULL) != NULL)
                clawt_web_add(row, clawt_web_notice(
                    clawt_web_member(task, "reason", ""), "bad"));

            if (g_strcmp0(state, "running") == 0 ||
                g_strcmp0(state, "pending") == 0) {
                g_autoptr(HtmxDiv) actions = htmx_div_new();
                g_autofree gchar *escaped = g_uri_escape_string(id, NULL,
                                                                FALSE);
                g_autofree gchar *action = g_strdup_printf(
                    "/tasks/%s/cancel", escaped);

                htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
                clawt_web_add(actions, clawt_web_post_button(
                    "Cancel", action, "danger", "Cancel this task?"));
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
            }

            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(list));
    }

    (void)agent_id;
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Flow ────────────────────────────────────────────────────────── */

/*
 * Whether a room has the person in it.
 *
 * The flow view is for what the *agents* are saying to each other, which
 * is the half nobody sees. A room the user is a member of is their own
 * chat, and is already on the Chat tab.
 */
static gboolean
room_involves_user(JsonArray *members)
{
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (g_strcmp0(member, "user") == 0)
            return TRUE;
    }

    return FALSE;
}

HtmxElement *
clawt_web_flow_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "room.list", NULL);
    JsonArray *rooms;
    g_autofree gchar *flow_run_sender = NULL;
    g_autofree gchar *flow_run_day = NULL;
    guint shown = 0;
    guint i;

    (void)agent_id;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Flow"));
    clawt_web_add(pad, clawt_web_text(
        "What the agents are saying to each other. Two agents answering "
        "each other for ever is polite and looks like work, so each "
        "message carries how far it has travelled.", "lede"));

    rooms = clawt_web_member_array(clawt_web_root(reply), "rooms");

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_web_member(room, "id", "?");
        JsonArray *members = clawt_web_member_array(room, "members");
        g_autoptr(ClawtWebPayload) payload = NULL;
        g_autoptr(JsonNode) history = NULL;
        JsonArray *messages;
        g_autoptr(HtmxDiv) card = NULL;
        HtmxElement *body;
        guint m;

        if (room_involves_user(members))
            continue;

        payload = clawt_web_payload_new();
        clawt_web_payload_set(payload, "room", id);
        clawt_web_payload_set_int(payload, "limit", 40);

        history = clawt_web_app_call(app, "room.history",
                                     clawt_web_payload_take(
                                         g_steal_pointer(&payload)));
        messages = clawt_web_member_array(clawt_web_root(history), "messages");

        if (messages == NULL || json_array_get_length(messages) == 0)
            continue;

        shown++;

        {
            g_autofree gchar *who = NULL;
            g_autoptr(GString) joined = g_string_new(NULL);
            guint k;

            for (k = 0; members != NULL &&
                        k < json_array_get_length(members); k++)
                g_string_append_printf(joined, "%s%s", k > 0 ? " · " : "",
                                       json_array_get_string_element(members,
                                                                     k));

            who = g_strdup(joined->str);
            card = clawt_web_card(who, id);
        }

        body = clawt_web_card_body(card);

        /*
         * This is a digest across every room, not a transcript of one --
         * one truncated line per message, so the whole of what the fleet
         * has been saying fits on a page.  The GTK client's Flow tab
         * shows one room in full instead, which is why that one draws
         * through the chat's row builder and this one does not.
         *
         * What both do share is where a run begins: consecutive messages
         * from one agent do not repeat the name.  The rule comes from
         * libclawt so a digest and a transcript cannot disagree about it.
         */
        g_free(flow_run_sender);
        flow_run_sender = NULL;
        g_free(flow_run_day);
        flow_run_day = NULL;

        for (m = 0; m < json_array_get_length(messages); m++) {
            JsonObject *message = json_array_get_object_element(messages, m);
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            gint64 depth = clawt_web_member_int(message, "depth", 0);
            gint64 ts = clawt_web_member_int(message, "ts", 0);
            const gchar *sender_id = clawt_web_member(message, "sender", "?");
            g_autoptr(GDateTime) at = (ts > 0)
                ? g_date_time_new_from_unix_local(ts)
                : g_date_time_new_now_local();
            g_autofree gchar *day = g_date_time_format(at, "%Y-%m-%d");
            g_autofree gchar *when = clawt_web_relative_time(ts);
            g_autofree gchar *body_text = clawt_web_one_line(
                clawt_web_member(message, "body", ""), 300);
            gboolean run_start = clawt_chat_run_is_start(flow_run_sender,
                                                         flow_run_day,
                                                         sender_id, day,
                                                         NULL);

            g_free(flow_run_sender);
            flow_run_sender = g_strdup(sender_id);
            g_free(flow_run_day);
            flow_run_day = g_steal_pointer(&day);

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            if (run_start) {
                g_autoptr(HtmxSpan) sender = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(sender),
                                       "list-item-title");
                htmx_node_set_text_content(HTMX_NODE(sender), sender_id);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(sender));
            }

            if (depth > 1) {
                g_autofree gchar *hops =
                    g_strdup_printf("%" G_GINT64_FORMAT " hops", depth);

                clawt_web_add(head, clawt_web_badge(hops, "warn"));
            }

            if (clawt_web_member(message, "task", NULL) != NULL)
                clawt_web_add(head, clawt_web_badge(
                    clawt_web_member(message, "task", ""), "info"));

            if (*when != '\0') {
                g_autoptr(HtmxSpan) stamp = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(stamp), "muted small");
                htmx_node_set_text_content(HTMX_NODE(stamp), when);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(stamp));
            }

            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
            clawt_web_add(row, clawt_web_text(body_text, "list-item-sub"));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    if (shown == 0)
        clawt_web_add(pad, clawt_web_empty(
            "Nothing between agents yet",
            "This shows rooms the person is not in. Your own chats with "
            "each agent are on the Chat tab."));

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

/*
 * Builds a routine payload from the form, from the schema.
 */
static JsonNode *
routine_payload(HtmxRequest *request, const gchar *id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    clawt_web_payload_set(payload, "id", id);

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "routines."))
            continue;

        leaf = entry->key + strlen("routines.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            if (clawt_web_form_had(request, leaf))
                clawt_web_payload_set_bool(payload, leaf,
                                           clawt_web_form_flag(request, leaf));
            continue;
        }

        value = clawt_web_form_value(request, leaf);

        if (value == NULL)
            continue;

        if (entry->type == CLAWT_SCHEMA_INT) {
            clawt_web_payload_set_int(payload, leaf,
                                      g_ascii_strtoll(value, NULL, 10));
            continue;
        }

        clawt_web_payload_set(payload, leaf, value);
    }

    return clawt_web_payload_take(g_steal_pointer(&payload));
}

static HtmxResponse *
routines_page(ClawtWebApp *app, HtmxRequest *request, const gchar *toast,
              gboolean failed)
{
    g_autofree gchar *first = clawt_web_first_agent(app);

    if (failed)
        return clawt_web_error_page(app, request, first,
                                    CLAWT_PAGE_ROUTINES, toast);

    return clawt_web_after_action(app, request, first,
                                  CLAWT_PAGE_ROUTINES, toast);
}

static HtmxResponse *
on_routine_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "id");
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    if (id == NULL || *id == '\0')
        return routines_page(app, request, "A routine needs an id.", TRUE);

    reply = clawt_web_app_call(app, "routine.add",
                               routine_payload(request, id));

    if (reply == NULL)
        return routines_page(app, request, clawt_web_app_last_error(app),
                             TRUE);

    return routines_page(app, request, "Routine added.", FALSE);
}

static HtmxResponse *
on_routine_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "routine");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_web_app_call(app, "routine.update",
                               routine_payload(request, id));

    if (reply == NULL)
        return routines_page(app, request, clawt_web_app_last_error(app),
                             TRUE);

    return routines_page(app, request, "Saved.", FALSE);
}

typedef struct {
    ClawtWebApp *app;
    const gchar *kind;
    const gchar *done;
} RoutineAction;

static HtmxResponse *
on_routine_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    RoutineAction *action = user_data;
    g_autofree gchar *id = clawt_web_param(params, "routine");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);

    reply = clawt_web_app_call(action->app, action->kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return routines_page(action->app, request,
                             clawt_web_app_last_error(action->app), TRUE);

    return routines_page(action->app, request, action->done, FALSE);
}

static HtmxResponse *
on_task_cancel(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "task");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *first = clawt_web_first_agent(app);

    clawt_web_payload_set(payload, "task", id);

    reply = clawt_web_app_call(app, "task.cancel",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, first,
                                    CLAWT_PAGE_TASKS,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, first, CLAWT_PAGE_TASKS,
                                  "Cancelled.");
}

static RoutineAction *
routine_action_new(ClawtWebApp *app, const gchar *kind, const gchar *done)
{
    RoutineAction *action = g_new0(RoutineAction, 1);

    action->app = app;
    action->kind = kind;
    action->done = done;

    return action;
}

void
clawt_web_register_work(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/routines/add", on_routine_add, app);
    htmx_router_post(router, "/routines/:routine/save", on_routine_save, app);
    htmx_router_post(router, "/routines/:routine/run", on_routine_action,
                     routine_action_new(app, "routine.run", "Run."));
    htmx_router_post(router, "/routines/:routine/remove", on_routine_action,
                     routine_action_new(app, "routine.remove", "Removed."));
    htmx_router_post(router, "/tasks/:task/cancel", on_task_cancel, app);
}
