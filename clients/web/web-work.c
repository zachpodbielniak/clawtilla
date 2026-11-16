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
                leaf, leaf, entry->doc,
                clawt_web_schema_flag(existing, leaf)));
            continue;
        }

        if (g_strcmp0(leaf, "instructions") == 0) {
            clawt_web_add(form, clawt_web_textarea_field(
                "Instructions", leaf, value, 6));
            continue;
        }

        /*
         * A number reads as a number.  clawt_web_member() answers only
         * strings, so an INT field drew empty -- and saving the form
         * sent that emptiness back as 0, which for
         * `max_start_failures` is "never turn it off".  A new routine
         * shows the schema default for the same reason.
         */
        if (entry->type == CLAWT_SCHEMA_INT) {
            g_autofree gchar *number = (existing != NULL)
                ? g_strdup_printf("%" G_GINT64_FORMAT,
                                  clawt_web_member_int(
                                      existing, leaf,
                                      g_ascii_strtoll(
                                          entry->default_value != NULL
                                              ? entry->default_value : "0",
                                          NULL, 10)))
                : g_strdup(entry->default_value != NULL
                               ? entry->default_value : "0");

            /*
             * The start-failure limit gets words rather than its key,
             * and libclawt's one-line explanation under it: it is the
             * one setting here somebody meets because something went
             * wrong, and `max_start_failures` explains nothing.
             */
            if (g_strcmp0(leaf, "max_start_failures") == 0) {
                clawt_web_add(form, clawt_web_field(
                    "Turn off after failed starts", leaf, number, NULL));
                clawt_web_add(form, clawt_web_text(
                    clawt_routine_start_failures_hint(), "small muted"));
                continue;
            }

            clawt_web_add(form, clawt_web_field(leaf, leaf, number,
                                                entry->doc));
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

        /*
         * Turned off because its computer would not start: why, in
         * libclawt's words, and the one click that turns it back on.
         */
        if (json_object_has_member(routine, "turned_off_after")) {
            g_autofree gchar *note = clawt_routine_turned_off_note(
                (guint)clawt_web_member_int(routine, "turned_off_after", 0),
                clawt_web_member(routine, "turned_off_error", NULL));
            g_autofree gchar *escaped = g_uri_escape_string(
                clawt_web_member(routine, "id", ""), NULL, FALSE);
            g_autofree gchar *enable = g_strdup_printf(
                "/routines/%s/enable", escaped);
            g_autoptr(HtmxDiv) notice = clawt_web_notice(note, "bad");

            clawt_web_add(notice, clawt_web_post_button(
                "Turn back on", enable, "primary", NULL));
            clawt_web_add(pad, g_steal_pointer(&notice));
        }

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

/*
 * The "Problems to look at" section above the tasks: who hears about a
 * broken run, and every open incident with its Retry.  The settings sit
 * with the incidents, as in the GTK client, because "who was told about
 * this" is asked while looking at one.
 */
static void
add_incidents_section(ClawtWebApp *app, HtmxDiv *pad)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    HtmxElement *body;
    JsonObject *root;
    JsonArray *incidents;
    guint i;

    reply = clawt_web_app_call(app, "incident.list", NULL);
    root = clawt_web_root(reply);

    if (root == NULL)
        return;

    card = clawt_web_card("Problems to look at",
                          "Failed, stalled and unstartable runs, and "
                          "routines that did not run. Each is reported to "
                          "its team's lead in the team's Problems to look "
                          "at room.");
    body = clawt_web_card_body(card);

    /* The recipients, walked from the library rather than named here. */
    for (i = 0; i < clawt_incident_recipient_count(); i++) {
        ClawtIncidentRecipient value = clawt_incident_recipient_nth(i);

        g_ptr_array_add(values,
                        (gpointer)clawt_incident_recipient_nick(value));
        g_ptr_array_add(labels,
                        (gpointer)clawt_incident_recipient_label(value));
    }

    g_ptr_array_add(values, NULL);
    g_ptr_array_add(labels, NULL);

    {
        g_autoptr(HtmxForm) form = clawt_web_form("/incidents/configure");
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");

        clawt_web_add(form, clawt_web_switch_field(
            "Report broken runs", "enabled",
            "Off, nothing is filed and nobody is told.",
            clawt_web_schema_flag(root, "enabled")));
        clawt_web_add(form, clawt_web_select_field(
            "Who hears about them", "to",
            (const gchar *const *)values->pdata,
            (const gchar *const *)labels->pdata,
            clawt_web_member(root, "to", "lead")));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    }

    incidents = clawt_web_member_array(root, "incidents");

    if (incidents == NULL || json_array_get_length(incidents) == 0) {
        clawt_web_add(body, clawt_web_text("Nothing has broken.",
                                           "list-item-sub"));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(incidents); i++) {
            JsonObject *incident = json_array_get_object_element(incidents, i);
            const gchar *id = clawt_web_member(incident, "id", "?");
            const gchar *recipient = clawt_web_member(incident, "recipient",
                                                      NULL);
            const gchar *note = clawt_web_member(incident, "note", NULL);
            const gchar *reason = clawt_web_member(incident, "reason", NULL);
            const gchar *retried_by = clawt_web_member(incident, "retried_by",
                                                       NULL);
            gint64 occurrences = clawt_web_member_int(incident, "occurrences",
                                                      1);
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            g_autoptr(GString) status = g_string_new(NULL);
            g_autofree gchar *title = g_strdup_printf(
                "%s's run %s", clawt_web_member(incident, "agent", "?"),
                clawt_web_member(incident, "kind_label", "broke"));

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(label),
                                       "list-item-title");
                htmx_node_set_text_content(HTMX_NODE(label), title);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
            }

            clawt_web_add(head, clawt_web_badge(
                clawt_web_member(incident, "kind", "?"), "bad"));
            clawt_web_add(head, clawt_web_badge(
                clawt_web_member(incident, "room", "?"), "neutral"));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

            if (reason != NULL) {
                g_autofree gchar *line = clawt_web_one_line(reason, 300);

                clawt_web_add(row, clawt_web_text(line, "list-item-sub"));
            }

            if (note != NULL)
                g_string_append_printf(status, "Recorded for you: %s", note);
            else if (clawt_web_schema_flag(incident, "queued") &&
                     recipient != NULL)
                g_string_append_printf(status,
                                       "Waiting for %s to finish a turn",
                                       recipient);
            else if (recipient != NULL)
                g_string_append_printf(status, "Reported to %s", recipient);
            else
                g_string_append(status, "Not reported yet");

            if (occurrences > 1)
                g_string_append_printf(status, " \302\267 broke %"
                                       G_GINT64_FORMAT " times", occurrences);

            if (retried_by != NULL)
                g_string_append_printf(status, " \302\267 last retried by %s",
                                       g_strcmp0(retried_by, "user") == 0
                                       ? "you" : retried_by);

            clawt_web_add(row, clawt_web_notice(status->str, "info"));

            if (clawt_web_schema_flag(incident, "retryable")) {
                g_autoptr(HtmxDiv) actions = htmx_div_new();
                g_autofree gchar *escaped = g_uri_escape_string(id, NULL,
                                                                FALSE);
                g_autofree gchar *action = g_strdup_printf(
                    "/incidents/%s/retry", escaped);

                htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
                clawt_web_add(actions, clawt_web_post_button(
                    "Retry", action, "default", NULL));
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
            }

            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(list));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
}

/* ── The operator's own tasks ────────────────────────────────────── */

/*
 * The agent ids and names for a picker, from agent.list.  With
 * @with_nobody the first choice is the backlog: the point of writing a
 * task down before knowing who should do it.
 */
static void
agent_choices(ClawtWebApp *app, gboolean with_nobody, GPtrArray *values,
              GPtrArray *labels)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "agent.list", NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(reply),
                                               "agents");
    guint i;

    if (with_nobody) {
        g_ptr_array_add(values, g_strdup(""));
        g_ptr_array_add(labels, g_strdup("Nobody yet (the backlog)"));
    }

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_web_member(agent, "id", "");

        g_ptr_array_add(values, g_strdup(id));
        g_ptr_array_add(labels, g_strdup(clawt_web_member(agent, "name", id)));
    }

    g_ptr_array_add(values, NULL);
    g_ptr_array_add(labels, NULL);
}

static HtmxDiv *
agent_select(ClawtWebApp *app, const gchar *label, gboolean with_nobody)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);

    agent_choices(app, with_nobody, values, labels);

    return clawt_web_select_field(label, "agent",
                                  (const gchar *const *)values->pdata,
                                  (const gchar *const *)labels->pdata, "");
}

/* A submit button in a form row. */
static void
add_submit(HtmxForm *form, const gchar *label)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxButton) button = clawt_web_button(label, "primary");

    htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
    htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(button));
    htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
}

/*
 * A task the operator writes down: what, optionally a title, and who --
 * or nobody yet, which puts it on the backlog.  Folded away until
 * opened, so the list stays the first thing on the page.
 */
static void
add_new_task_form(ClawtWebApp *app, HtmxDiv *pad)
{
    g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
    g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
    g_autoptr(HtmxForm) form = clawt_web_form("/tasks/new");

    htmx_node_set_text_content(HTMX_NODE(summary), "New task");
    clawt_web_add(form, clawt_web_field("Title (optional)", "title", NULL,
                                        "Fix the export"));
    clawt_web_add(form, clawt_web_textarea_field("What to do", "prompt",
                                                 NULL, 5));
    clawt_web_add(form, clawt_web_text(
        "Write it so it stands alone: the agent reads this and nothing else "
        "about why it is wanted.", "small muted"));
    clawt_web_add(form, agent_select(app, "Who does it", TRUE));
    clawt_web_add(form, clawt_web_field("Wait for (task ids)", "after", NULL,
                                        "task-01m..., task-01m..."));
    add_submit(form, "Create");

    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
    htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(details));
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

    /*
     * What the two lists below cover, in the library's words: the open
     * tasks and the last day's, kept across restarts -- or everything
     * since the daemon started, from one with no task store.
     */
    if (reply != NULL) {
        JsonObject *root = clawt_web_root(reply);
        g_autofree gchar *since = clawt_task_list_caption(
            clawt_web_member_int(root, "since", 0),
            clawt_web_member_bool(root, "kept", FALSE),
            clawt_web_member_int(root, "retention_days", 0),
            g_get_real_time() / G_USEC_PER_SEC);

        clawt_web_add(pad, clawt_web_text(since, "small muted"));
    }

    add_incidents_section(app, pad);
    add_new_task_form(app, pad);

    tasks = clawt_web_member_array(clawt_web_root(reply), "tasks");

    if (reply == NULL) {
        clawt_web_add(pad, clawt_web_empty(
            "Tasks are unavailable",
            "Check the daemon connection, then reopen Tasks to retry."));
    } else if (tasks == NULL || json_array_get_length(tasks) == 0) {
        clawt_web_add(pad, clawt_web_empty(
            "No tasks yet",
            "Ask an agent to delegate work. Its progress and result will appear here."));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(tasks); i++) {
            JsonObject *task = json_array_get_object_element(tasks, i);
            const gchar *id = clawt_web_member(task, "id", "?");
            const gchar *state = clawt_web_member(task, "state", "?");
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            g_autofree gchar *who = clawt_web_member(task, "assignee", NULL)
                ? g_strdup_printf("%s → %s",
                                  clawt_web_member(task, "origin", "?"),
                                  clawt_web_member(task, "assignee", "?"))
                : g_strdup_printf("%s · on the backlog",
                                  clawt_web_member(task, "origin", "?"));
            g_autofree gchar *escaped_id = g_uri_escape_string(id, NULL,
                                                               FALSE);
            g_autofree gchar *prompt = clawt_web_one_line(
                clawt_web_member(task, "prompt", ""), 200);

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                /*
                 * The title, linking to the task's page -- its thread,
                 * history and a box to comment in.  Who it is between
                 * goes underneath; the prompt is on that page whole.
                 */
                g_autofree gchar *href = g_strdup_printf("/tasks/%s",
                                                         escaped_id);
                HtmxA *link = htmx_a_new_with_href(href);

                htmx_element_add_class(HTMX_ELEMENT(label),
                                       "list-item-title");
                htmx_node_set_text_content(
                    HTMX_NODE(link),
                    clawt_web_member(task, "title",
                                     clawt_web_member(task, "prompt", "")));
                htmx_node_add_child(HTMX_NODE(label), HTMX_NODE(link));
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
            }

            clawt_web_add(head, clawt_web_badge(state, task_tone(state)));
            clawt_web_add(head, clawt_web_badge(id, "neutral"));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

            clawt_web_add(row, clawt_web_text(who, "list-item-sub"));
            clawt_web_add(row, clawt_web_text(prompt, "small muted"));

            /* A held task says what it is waiting for. */
            {
                JsonArray *waiting = clawt_web_member_array(task,
                                                            "waiting_on");

                if (waiting != NULL && json_array_get_length(waiting) > 0) {
                    g_autofree gchar *caption =
                        clawt_task_waiting_caption(waiting);

                    clawt_web_add(row, clawt_web_notice(caption, "warn"));
                }
            }

            /* Off the backlog: who does it, and the work goes to them. */
            if (g_strcmp0(state, "unassigned") == 0) {
                g_autofree gchar *action = g_strdup_printf("/tasks/%s/assign",
                                                           escaped_id);
                g_autoptr(HtmxForm) form = clawt_web_form(action);

                clawt_web_add(form, agent_select(app, "Assign to", FALSE));
                add_submit(form, "Assign");
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
            }

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

            /*
             * The freshest thing anybody knows, and the caveat when
             * nothing knows anything: a row reading `completed` and no
             * more cannot tell a reported result from a turn that simply
             * stopped, and the two need different follow-ups.
             */
            if (clawt_web_member(task, "progress_note", NULL) != NULL)
                clawt_web_add(row, clawt_web_notice(
                    clawt_web_member(task, "progress_note", ""), "info"));
            /*
             * A JSON boolean, so the string reader always answered NULL
             * and this branch was unreachable: a task that ended because
             * the assignee stopped talking rather than because it
             * reported was drawn as a plain `completed`, with the
             * agent's last words presented as its result.  Both the GTK
             * client and the CLI draw the caveat.
             */
            else if (clawt_web_schema_flag(task, "result_inferred"))
                clawt_web_add(row, clawt_web_notice(
                    "Nobody reported this finished -- it is the last thing "
                    "the assignee wrote.", "warn"));

            /* A consultation: its result is an answer, not work done. */
            if (clawt_web_schema_flag(task, "consult"))
                clawt_web_add(head, clawt_web_badge(
                    clawt_task_consult_label(), "info"));

            /*
             * The room the work runs in, when it has one of its own
             * (`thread: new`), and a link into it: the work is not in
             * the conversation the two agents share, so without this
             * the transcript the result came out of cannot be found.
             */
            {
                const gchar *room = clawt_web_member(task, "thread_room",
                                                     NULL);
                g_autofree gchar *summary = clawt_task_thread_summary(room);

                if (summary != NULL) {
                    g_autoptr(HtmxDiv) line = htmx_div_new();
                    g_autofree gchar *escaped = g_uri_escape_string(
                        room, NULL, FALSE);
                    g_autofree gchar *url = g_strdup_printf("/r/%s",
                                                            escaped);
                    HtmxA *open = htmx_a_new_with_href(url);

                    htmx_element_add_class(HTMX_ELEMENT(line), "btn-row");
                    clawt_web_add(line, clawt_web_text(summary, "muted"));
                    htmx_node_set_text_content(HTMX_NODE(open), "Open room");
                    htmx_element_add_class(HTMX_ELEMENT(open),
                                           "btn btn-default");
                    clawt_web_add(line, open);
                    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(line));
                }
            }

            /* Anything not ended, from the library: blocked and in review too. */
            if (clawt_task_state_nick_is_cancellable(state)) {
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

            /*
             * Retry where the daemon would accept it -- the library's
             * rule, so the button and the refusal cannot disagree.
             */
            {
                gint value = 0;

                if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state,
                                         &value) &&
                    clawt_task_state_is_retryable((ClawtTaskState)value)) {
                    g_autoptr(HtmxDiv) actions = htmx_div_new();
                    g_autofree gchar *escaped = g_uri_escape_string(
                        id, NULL, FALSE);
                    g_autofree gchar *action = g_strdup_printf(
                        "/tasks/%s/retry", escaped);

                    htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
                    clawt_web_add(actions, clawt_web_post_button(
                        "Retry", action, "default", NULL));
                    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
                }
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

        /*
         * Where a cross-talk summary's "View in Flow" lands: the card
         * carries the anchor libclawt derives from the room id.
         */
        {
            g_autofree gchar *anchor = clawt_crosstalk_anchor(id);

            htmx_element_set_attribute(HTMX_ELEMENT(card), "id", anchor);
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
routine_payload(HtmxRequest *request, const gchar *id, GError **error)
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

        /* An empty number is no answer, not zero. */
        if (entry->type == CLAWT_SCHEMA_INT && *value == '\0')
            continue;

        if (entry->type == CLAWT_SCHEMA_INT) {
            gchar *end = NULL;
            gint64 number = g_ascii_strtoll(value, &end, 10);

            /*
             * Refused rather than read as 0: strtoll() makes 0 of
             * anything it cannot read, and for max_start_failures 0 is
             * "never turn it off" -- a typo would switch the guard off
             * and the page would say "Saved."
             */
            if (end == NULL || *end != '\0' || number < 0) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "%s is a whole number, 0 or more.", leaf);
                return NULL;
            }

            clawt_web_payload_set_int(payload, leaf, number);
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
    g_autoptr(GError) error = NULL;
    JsonNode *body;

    (void)params;

    if (id == NULL || *id == '\0')
        return routines_page(app, request, "A routine needs an id.", TRUE);

    body = routine_payload(request, id, &error);

    if (body == NULL)
        return routines_page(app, request, error->message, TRUE);

    reply = clawt_web_app_call(app, "routine.add", body);

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
    g_autoptr(GError) error = NULL;
    JsonNode *body;

    body = routine_payload(request, id, &error);

    if (body == NULL)
        return routines_page(app, request, error->message, TRUE);

    reply = clawt_web_app_call(app, "routine.update", body);

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

/*
 * One click to turn a routine back on: `enabled: true` through
 * routine.update, which the daemon treats as the edit it is -- the
 * count of failed starts and the note go with it.
 */
static HtmxResponse *
on_routine_enable(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *id = clawt_web_param(params, "routine");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;

	clawt_web_payload_set(payload, "id", id);
	clawt_web_payload_set_bool(payload, "enabled", TRUE);

	reply = clawt_web_app_call(app, "routine.update",
	                           clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return routines_page(app, request, clawt_web_app_last_error(app), TRUE);

	return routines_page(app, request, "Turned back on.", FALSE);
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

static HtmxResponse *
on_task_new(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *first = clawt_web_first_agent(app);
    const gchar *prompt = clawt_web_form_value(request, "prompt");
    const gchar *title = clawt_web_form_value(request, "title");
    const gchar *agent = clawt_web_form_value(request, "agent");
    const gchar *after = clawt_web_form_value(request, "after");

    (void)params;

    if (after != NULL && *after != '\0')
        clawt_web_payload_set(payload, "after", after);

    if (prompt != NULL && *prompt != '\0')
        clawt_web_payload_set(payload, "prompt", prompt);
    if (title != NULL && *title != '\0')
        clawt_web_payload_set(payload, "title", title);
    if (agent != NULL && *agent != '\0')
        clawt_web_payload_set(payload, "agent", agent);

    reply = clawt_web_app_call(app, "task.create",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, first, CLAWT_PAGE_TASKS,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, first, CLAWT_PAGE_TASKS,
        (agent != NULL && *agent != '\0')
            ? "Created, and handed over."
            : "Created on the backlog. Assign it when you know who.");
}

static HtmxResponse *
on_task_assign(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "task");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *first = clawt_web_first_agent(app);
    const gchar *agent = clawt_web_form_value(request, "agent");

    clawt_web_payload_set(payload, "task", id);

    if (agent != NULL && *agent != '\0')
        clawt_web_payload_set(payload, "agent", agent);

    reply = clawt_web_app_call(app, "task.assign",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, first, CLAWT_PAGE_TASKS,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, first, CLAWT_PAGE_TASKS,
                                  "Assigned.");
}

/* One line of a thread or a history: who or what, when, the text. */
static void
add_dated_line(HtmxDiv *list, const gchar *title, gint64 at,
               const gchar *body)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(GDateTime) when = (at > 0)
        ? g_date_time_new_from_unix_local(at) : NULL;
    g_autofree gchar *stamp = (when != NULL)
        ? g_date_time_format(when, "%-d %b %H:%M") : g_strdup("");
    g_autofree gchar *heading = g_strdup_printf("%s · %s", title, stamp);

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    clawt_web_add(row, clawt_web_text(heading, "list-item-title"));

    if (body != NULL && *body != '\0') {
        g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

        htmx_element_add_class(pre, "console");
        htmx_node_set_text_content(HTMX_NODE(pre), body);
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(pre));
    }

    htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));
}

/* A task's page body: what was asked, its thread, a comment box, history. */
static HtmxDiv *
task_page_body(ClawtWebApp *app, const gchar *id, const gchar *notice)
{
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonObject *task;
    JsonArray *rows;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_payload_set(payload, "task", id);
    clawt_web_payload_set_bool(payload, "journal", TRUE);
    reply = clawt_web_app_call(app, "task.show",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    root = clawt_web_root(reply);
    task = (root != NULL && json_object_has_member(root, "task"))
        ? json_object_get_object_member(root, "task") : NULL;

    if (task == NULL) {
        clawt_web_add(pad, clawt_web_section_title("Task"));
        clawt_web_add(pad, clawt_web_empty("No such task",
            "It may have been removed after the retention window."));
        return (HtmxDiv *)g_steal_pointer(&pad);
    }

    clawt_web_add(pad, clawt_web_section_title(
        clawt_web_member(task, "title", id)));

    if (notice != NULL)
        clawt_web_add(pad, clawt_web_notice(notice, "info"));

    {
        const gchar *state = clawt_web_member(task, "state", "?");
        g_autofree gchar *who = clawt_web_member(task, "assignee", NULL)
            ? g_strdup_printf("%s → %s", clawt_web_member(task, "origin", "?"),
                              clawt_web_member(task, "assignee", "?"))
            : g_strdup_printf("%s · on the backlog",
                              clawt_web_member(task, "origin", "?"));
        g_autoptr(HtmxElement) pre = HTMX_ELEMENT(htmx_pre_new());

        clawt_web_add(pad, clawt_web_badge(state, task_tone(state)));
        clawt_web_add(pad, clawt_web_text(who, "lede"));
        htmx_element_add_class(pre, "console");
        htmx_node_set_text_content(HTMX_NODE(pre),
                                   clawt_web_member(task, "prompt", ""));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(pre));

        if (clawt_web_member(task, "result", NULL) != NULL) {
            g_autoptr(HtmxDiv) result = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(result), "list");
            add_dated_line(result, "Result",
                           clawt_web_member_int(task, "finished_at", 0),
                           clawt_web_member(task, "result", ""));
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(result));
        }
    }

    /*
     * What it waits for: each with Stop waiting, and for a backlog or
     * held task a form to add one.
     */
    {
        JsonArray *after = clawt_web_member_array(task, "after");
        const gchar *state = clawt_web_member(task, "state", "");
        g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
        guint w;

        clawt_web_add(pad, clawt_web_section_title("Waits for"));

        for (w = 0; after != NULL && w < json_array_get_length(after); w++) {
            const gchar *blocker = json_array_get_string_element(after, w);
            g_autofree gchar *action = g_strdup_printf("/tasks/%s/unlink",
                                                       escaped);
            g_autoptr(HtmxForm) form = clawt_web_form(action);

            clawt_web_add(form, clawt_web_text(blocker, "list-item-title"));
            clawt_web_add(form, clawt_web_hidden("after", blocker));
            add_submit(form, "Stop waiting");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(form));
        }

        if (g_strcmp0(state, "unassigned") == 0 ||
            g_strcmp0(state, "blocked") == 0) {
            g_autofree gchar *action = g_strdup_printf("/tasks/%s/link",
                                                       escaped);
            g_autoptr(HtmxForm) form = clawt_web_form(action);

            clawt_web_add(form, clawt_web_field("Task id", "after", NULL,
                                                "task-01m..."));
            add_submit(form, "Wait for it");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(form));
        } else if (after == NULL) {
            clawt_web_add(pad, clawt_web_text(
                "Nothing. Only a task on the backlog can be made to wait.",
                "small muted"));
        }
    }

    clawt_web_add(pad, clawt_web_section_title("Thread"));
    rows = clawt_web_member_array(root, "notes");

    if (rows == NULL || json_array_get_length(rows) == 0) {
        clawt_web_add(pad, clawt_web_text("Nothing said about it yet.",
                                          "small muted"));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; i < json_array_get_length(rows); i++) {
            JsonObject *note = json_array_get_object_element(rows, i);
            g_autofree gchar *title = g_strdup_printf(
                "%s (%s)", clawt_web_member(note, "author", "clawtilla"),
                clawt_web_member(note, "kind", "note"));

            add_dated_line(list, title, clawt_web_member_int(note, "at", 0),
                           clawt_web_member(note, "body", ""));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(list));
    }

    {
        g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/tasks/%s/comment",
                                                   escaped);
        g_autoptr(HtmxForm) form = clawt_web_form(action);

        clawt_web_add(form, clawt_web_textarea_field("Comment", "text", NULL,
                                                     3));
        clawt_web_add(form, clawt_web_text(
            "Kept on the task. While it is with its assignee they hear it "
            "too, in the conversation the work is in.", "small muted"));
        add_submit(form, "Comment");
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(form));
    }

    /* Who reviews it, and the operator's two answers while it waits. */
    if (clawt_web_member(task, "reviewer", NULL) != NULL) {
        g_autofree gchar *who = g_strdup_printf(
            "Reviewed by %s.",
            g_strcmp0(clawt_web_member(task, "reviewer", ""), "user") == 0
                ? "you" : clawt_web_member(task, "reviewer", ""));

        clawt_web_add(pad, clawt_web_section_title("Review"));
        clawt_web_add(pad, clawt_web_text(who, "small muted"));

        if (g_strcmp0(clawt_web_member(task, "state", ""), "review") == 0) {
            g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
            g_autofree gchar *action = g_strdup_printf("/tasks/%s/review",
                                                       escaped);
            g_autoptr(HtmxForm) accept = clawt_web_form(action);
            g_autoptr(HtmxForm) changes = clawt_web_form(action);

            clawt_web_add(accept, clawt_web_hidden("verdict", "accept"));
            add_submit(accept, "Accept");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(accept));

            clawt_web_add(changes, clawt_web_hidden("verdict", "changes"));
            clawt_web_add(changes, clawt_web_textarea_field(
                "What to change", "notes", NULL, 3));
            add_submit(changes, "Request changes");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(changes));
        }
    }

    clawt_web_add(pad, clawt_web_section_title("History"));
    rows = clawt_web_member_array(root, "journal");

    {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
            JsonObject *entry = json_array_get_object_element(rows, i);

            add_dated_line(list, clawt_web_member(entry, "state", "?"),
                           clawt_web_member_int(entry, "at", 0),
                           clawt_web_member(entry, "what", ""));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(list));
    }

    return (HtmxDiv *)g_steal_pointer(&pad);
}

static HtmxResponse *
task_page(ClawtWebApp *app, HtmxRequest *request, const gchar *id,
          const gchar *notice)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) body = task_page_body(app, id, notice);
    g_autofree gchar *html = NULL;

    htmx_element_add_class(view, "view");
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(body));
    html = clawt_web_page(app, NULL, CLAWT_PAGE_TASKS, view, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_task_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *id = clawt_web_param(params, "task");

    return task_page(user_data, request, id, NULL);
}

/* task.link and task.unlink from a task's page; the kind is the data. */
static HtmxResponse *
task_link_from(ClawtWebApp *app, HtmxRequest *request, GHashTable *params,
               const gchar *kind)
{
    g_autofree gchar *id = clawt_web_param(params, "task");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *after = clawt_web_form_value(request, "after");

    clawt_web_payload_set(payload, "task", id);

    if (after != NULL && *after != '\0')
        clawt_web_payload_set(payload, "after", after);

    reply = clawt_web_app_call(app, kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return task_page(app, request, id, clawt_web_app_last_error(app));

    return task_page(app, request, id,
                     g_strcmp0(kind, "task.link") == 0
                         ? "It waits for that task now."
                         : "No longer waits for it.");
}

static HtmxResponse *
on_task_review(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "task");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verdict = clawt_web_form_value(request, "verdict");
    const gchar *notes = clawt_web_form_value(request, "notes");

    clawt_web_payload_set(payload, "task", id);

    if (verdict != NULL)
        clawt_web_payload_set(payload, "verdict", verdict);
    if (notes != NULL && *notes != '\0')
        clawt_web_payload_set(payload, "notes", notes);

    reply = clawt_web_app_call(app, "task.review",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return task_page(app, request, id, clawt_web_app_last_error(app));

    return task_page(app, request, id,
                     clawt_web_member(clawt_web_root(reply), "said", "Done."));
}

static HtmxResponse *
on_task_link(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    return task_link_from(user_data, request, params, "task.link");
}

static HtmxResponse *
on_task_unlink(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    return task_link_from(user_data, request, params, "task.unlink");
}

static HtmxResponse *
on_task_comment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "task");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *text = clawt_web_form_value(request, "text");

    clawt_web_payload_set(payload, "task", id);

    if (text != NULL && *text != '\0')
        clawt_web_payload_set(payload, "text", text);

    reply = clawt_web_app_call(app, "task.comment",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return task_page(app, request, id, clawt_web_app_last_error(app));

    return task_page(app, request, id,
                     clawt_web_schema_flag(clawt_web_root(reply), "delivered")
                         ? "Noted, and the assignee has it."
                         : "Noted on the task.");
}

/*
 * The operator's Retry, for a task row (`task`) or an incident row
 * (`incident`) -- the route's parameter name is the payload's key.
 */
static HtmxResponse *
retry_from(ClawtWebApp *app, HtmxRequest *request, const gchar *key,
           const gchar *id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *first = clawt_web_first_agent(app);

    clawt_web_payload_set(payload, key, id);

    reply = clawt_web_app_call(app, "task.retry",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, first, CLAWT_PAGE_TASKS,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(
        app, request, first, CLAWT_PAGE_TASKS,
        clawt_web_schema_flag(clawt_web_root(reply), "rerun")
            ? "Running the routine again."
            : "Queued for its assignee again.");
}

static HtmxResponse *
on_task_retry(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *id = clawt_web_param(params, "task");

    return retry_from(user_data, request, "task", id);
}

static HtmxResponse *
on_incident_retry(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    g_autofree gchar *id = clawt_web_param(params, "incident");

    return retry_from(user_data, request, "incident", id);
}

static HtmxResponse *
on_incidents_configure(HtmxRequest *request, GHashTable *params,
                       gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *first = clawt_web_first_agent(app);
    const gchar *to = clawt_web_form_value(request, "to");

    (void)params;

    /*
     * A checkbox that is off sends nothing, so the flag is read as
     * "present" and always sent -- otherwise turning it off could never
     * be saved.
     */
    clawt_web_payload_set_bool(payload, "enabled",
                               clawt_web_form_flag(request, "enabled"));

    if (to != NULL && *to != '\0')
        clawt_web_payload_set(payload, "to", to);

    reply = clawt_web_app_call(app, "incident.configure",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, first, CLAWT_PAGE_TASKS,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, first, CLAWT_PAGE_TASKS,
                                  "Saved.");
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
    htmx_router_post(router, "/routines/:routine/enable", on_routine_enable,
                     app);
    htmx_router_post(router, "/routines/:routine/run", on_routine_action,
                     routine_action_new(app, "routine.run", "Run."));
    htmx_router_post(router, "/routines/:routine/remove", on_routine_action,
                     routine_action_new(app, "routine.remove", "Removed."));
    htmx_router_post(router, "/tasks/new", on_task_new, app);
    htmx_router_get(router, "/tasks/:task", on_task_page, app);
    htmx_router_post(router, "/tasks/:task/assign", on_task_assign, app);
    htmx_router_post(router, "/tasks/:task/comment", on_task_comment, app);
    htmx_router_post(router, "/tasks/:task/link", on_task_link, app);
    htmx_router_post(router, "/tasks/:task/review", on_task_review, app);
    htmx_router_post(router, "/tasks/:task/unlink", on_task_unlink, app);
    htmx_router_post(router, "/tasks/:task/cancel", on_task_cancel, app);
    htmx_router_post(router, "/tasks/:task/retry", on_task_retry, app);
    htmx_router_post(router, "/incidents/:incident/retry", on_incident_retry,
                     app);
    htmx_router_post(router, "/incidents/configure", on_incidents_configure,
                     app);
}
