/*
 * lrg-tasks.c - The Tasks page (mirrors clients/gtk/gtk-tasks.c)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What has been delegated, to whom, and whether it is still going.
 *
 * Drawn as a tree, each task under the one that handed it on (the
 * `parent` member), as nested cards indented by depth.  GTK draws the
 * same rows flat; a chain of delegations read flat is a list of
 * strangers, and the shape is the thing a person is usually asking
 * about -- which piece of the work is the one that stalled.  Nested
 * cards rather than an LrgTreeView because a row here is several lines
 * of variable height (the prompt, a progress note, a Cancel button),
 * and the tree view draws fixed-height rows.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

/* how far each level of delegation is indented */
#define TASK_INDENT (28.0f)

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *list;
    guint        seq;

    /*
     * The "Problems to look at" section above the tasks, its own
     * request generation, and the two settings controls the Save button
     * reads.
     */
    LrgWidget   *incidents;
    guint        incident_seq;
    LrgWidget   *incidents_enabled;
    LrgWidget   *incidents_to;
} Tasks;

static Tasks tasks;

static void refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags);

static void
on_cancelled(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    gint64 cancelled;
    g_autofree gchar *text = NULL;

    if (root == NULL)
        return;

    cancelled = clawt_ipc_payload_int(root, "cancelled", 0);
    text = (cancelled == 1)
        ? g_strdup("Cancelled")
        : g_strdup_printf("Cancelled %" G_GINT64_FORMAT " tasks", cancelled);
    clawt_lrg_toast(app, text);
    refresh_list(app, CLAWT_LRG_QUIET);
}

static void
on_cancel_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "task.cancel",
                      clawt_ipc_payload_new("task", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_cancelled, NULL, NULL);
}

/*
 * Asked first, because cancelling cascades: everything the task handed
 * on is cancelled with it, and the delegating agent is told it was
 * cancelled over its head.
 */
static void
on_cancel_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "task-id");

    if (id == NULL)
        return;

    clawt_lrg_confirm(tasks.app, "Cancel this task?",
                      "It stops, and so does everything it handed on. The "
                      "agent that delegated it is told.",
                      "Cancel task", TRUE, on_cancel_confirmed, g_strdup(id),
                      g_free);
}

static void refresh_incidents(ClawtLrgApp *app, ClawtLrgRequestFlags flags);

static void
on_retried(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    if (root == NULL)
        return;

    clawt_lrg_toast(app, clawt_ipc_payload_boolean(root, "rerun", FALSE)
                         ? "Running the routine again"
                         : "Queued for its assignee again");
    refresh_list(app, CLAWT_LRG_QUIET);
    refresh_incidents(app, CLAWT_LRG_QUIET);
}

/*
 * Sends a failed or stalled run round again.  The button carries the key
 * its row had -- `task` on a task card, `incident` on an incident card,
 * so a routine that never made a task can still be retried.
 */
static void
on_retry_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *key = g_object_get_data(G_OBJECT(button), "retry-key");
    const gchar *id = g_object_get_data(G_OBJECT(button), "retry-id");

    if (key == NULL || id == NULL)
        return;

    clawt_lrg_request(tasks.app, "task.retry",
                      clawt_ipc_payload_new(key, id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_retried, NULL, NULL);
}

static LrgWidget *
retry_button(const gchar *key, const gchar *id)
{
    LrgWidget *retry = clawt_lrg_button("Retry", "flat",
                                        G_CALLBACK(on_retry_clicked), NULL);

    g_object_set_data(G_OBJECT(retry), "retry-key", (gpointer)key);
    g_object_set_data_full(G_OBJECT(retry), "retry-id", g_strdup(id),
                           g_free);

    return retry;
}

static void
on_incidents_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    if (clawt_ipc_reply_object(reply) == NULL)
        return;

    clawt_lrg_toast(app, "Saved");
    refresh_incidents(app, CLAWT_LRG_QUIET);
}

static void
on_incidents_save(LrgButton *button, gpointer user_data)
{
    JsonObject *payload;
    JsonNode *node;
    const gchar *to;

    if (tasks.incidents_enabled == NULL || tasks.incidents_to == NULL)
        return;

    payload = json_object_new();
    json_object_set_boolean_member(
        payload, "enabled",
        clawt_lrg_switch_active(tasks.incidents_enabled));
    to = clawt_lrg_combo_id(tasks.incidents_to);

    if (to != NULL)
        json_object_set_string_member(payload, "to", to);

    node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, payload);

    clawt_lrg_request(tasks.app, "incident.configure", node,
                      CLAWT_LRG_REQUEST_NONE, on_incidents_saved, NULL, NULL);
}

/*
 * What became of an incident's report, in one line: who has it, who it
 * is waiting for, or why nobody was given it.
 */
static gchar *
incident_status(JsonObject *incident)
{
    const gchar *recipient = clawt_ipc_payload_string(incident, "recipient");
    const gchar *note = clawt_ipc_payload_string(incident, "note");
    const gchar *retried_by = clawt_ipc_payload_string(incident,
                                                       "retried_by");
    gint64 occurrences = clawt_ipc_payload_int(incident, "occurrences", 1);
    g_autoptr(GString) out = g_string_new(NULL);

    if (note != NULL)
        g_string_append_printf(out, "Recorded for you: %s", note);
    else if (clawt_ipc_payload_boolean(incident, "queued", FALSE) &&
             recipient != NULL)
        g_string_append_printf(out, "Waiting for %s to finish a turn",
                               recipient);
    else if (recipient != NULL)
        g_string_append_printf(out, "Reported to %s", recipient);
    else
        g_string_append(out, "Not reported yet");

    if (occurrences > 1)
        g_string_append_printf(out, " \302\267 broke %" G_GINT64_FORMAT
                               " times", occurrences);

    if (retried_by != NULL)
        g_string_append_printf(out, " \302\267 last retried by %s",
                               g_strcmp0(retried_by, "user") == 0
                               ? "you" : retried_by);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static LrgWidget *
incident_card(JsonObject *incident)
{
    const gchar *reason = clawt_ipc_payload_string(incident, "reason");
    g_autofree gchar *title = g_strdup_printf(
        "%s's run %s",
        clawt_ipc_payload_string(incident, "agent") != NULL
            ? clawt_ipc_payload_string(incident, "agent") : "?",
        clawt_ipc_payload_string(incident, "kind_label") != NULL
            ? clawt_ipc_payload_string(incident, "kind_label") : "broke");
    g_autofree gchar *status = incident_status(incident);
    LrgWidget *body = clawt_lrg_vbox(6.0f);
    LrgWidget *row = clawt_lrg_row(title, NULL);
    const gchar *kind = clawt_ipc_payload_string(incident, "kind");

    clawt_lrg_row_add_suffix(row, clawt_lrg_badge(kind != NULL ? kind : "?",
                                                  "bad"));

    if (clawt_ipc_payload_boolean(incident, "retryable", FALSE))
        clawt_lrg_row_add_suffix(
            row, retry_button("incident",
                              clawt_ipc_payload_string(incident, "id")));

    clawt_lrg_take(body, row);

    if (reason != NULL && *reason != '\0')
        clawt_lrg_take(body, clawt_lrg_paragraph(reason, "muted"));

    clawt_lrg_take(body, clawt_lrg_paragraph(status, "caption"));

    return clawt_lrg_card(body);
}

/*
 * The "Problems to look at" section: who hears about a broken run, and
 * each open incident.  The settings sit with the incidents, as in the
 * GTK and web clients, because "who was told about this" is asked while
 * looking at one.
 */
static void
on_incidents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *incidents;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgWidget *form;
    LrgWidget *save;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != tasks.incident_seq ||
        tasks.incidents == NULL)
        return;

    clawt_lrg_clear(tasks.incidents);
    tasks.incidents_enabled = NULL;
    tasks.incidents_to = NULL;

    if (root == NULL)
        return;

    clawt_lrg_take(tasks.incidents, clawt_lrg_section("Problems to look at"));

    /* The recipients, walked from the library rather than named here. */
    for (i = 0; i < clawt_incident_recipient_count(); i++) {
        ClawtIncidentRecipient value = clawt_incident_recipient_nth(i);

        g_ptr_array_add(ids, (gpointer)clawt_incident_recipient_nick(value));
        g_ptr_array_add(labels,
                        (gpointer)clawt_incident_recipient_label(value));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    form = clawt_lrg_form_new();
    tasks.incidents_enabled = clawt_lrg_switch(
        "Report broken runs",
        clawt_ipc_payload_boolean(root, "enabled", TRUE));
    clawt_lrg_form_add(form, "Report", tasks.incidents_enabled,
                       "Failed, stalled and unstartable runs, and routines "
                       "that did not run");
    tasks.incidents_to = clawt_lrg_combo(
        (const gchar *const *)ids->pdata,
        (const gchar *const *)labels->pdata,
        clawt_ipc_payload_string(root, "to"));
    clawt_lrg_form_add(form, "Who hears about them", tasks.incidents_to,
                       "Reports go to the room called Problems to look at, "
                       "one per team");
    save = clawt_lrg_button("Save", "primary",
                            G_CALLBACK(on_incidents_save), NULL);
    clawt_lrg_form_add(form, NULL, save, NULL);
    clawt_lrg_take(tasks.incidents, form);

    incidents = json_object_has_member(root, "incidents")
                ? json_object_get_array_member(root, "incidents") : NULL;

    if (incidents == NULL || json_array_get_length(incidents) == 0) {
        clawt_lrg_take(tasks.incidents,
                       clawt_lrg_paragraph("Nothing has broken.", "muted"));
        return;
    }

    for (i = 0; i < json_array_get_length(incidents); i++) {
        LrgWidget *card = incident_card(
            json_array_get_object_element(incidents, i));

        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(tasks.incidents, card);
    }
}

static void
refresh_incidents(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (tasks.incidents == NULL)
        return;

    tasks.incident_seq++;
    clawt_lrg_request(app, "incident.list", NULL, flags, on_incidents,
                      GUINT_TO_POINTER(tasks.incident_seq), NULL);
}

static LrgWidget *
task_card(JsonObject *task, guint depth)
{
    const gchar *state = clawt_ipc_payload_string(task, "state");
    const gchar *prompt = clawt_ipc_payload_string(task, "prompt");
    const gchar *reason = clawt_ipc_payload_string(task, "reason");
    const gchar *note = clawt_lrg_task_note(task);
    g_autofree gchar *title = clawt_lrg_task_title(task);
    LrgWidget *body = clawt_lrg_vbox(6.0f);
    LrgWidget *row = clawt_lrg_row(title, NULL);
    LrgWidget *card;
    LrgInsets indent = { TASK_INDENT * (gfloat)depth, 0.0f, 0.0f, 0.0f };

    if (depth > 0)
        clawt_lrg_row_add_prefix(row, clawt_lrg_label("\342\206\263",
                                                      "muted"));

    /*
     * Coloured by the library's rule, the one the web client uses: the
     * GTK badges were one grey whatever the state, so a failed task and
     * a pending one looked alike.
     */
    clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
                                      (state != NULL) ? state : "?",
                                      clawt_lrg_task_tone(state)));

    if (clawt_lrg_task_cancellable(state)) {
        LrgWidget *cancel = clawt_lrg_button("Cancel", "flat",
                                             G_CALLBACK(on_cancel_clicked),
                                             NULL);

        g_object_set_data_full(
            G_OBJECT(cancel), "task-id",
            g_strdup(clawt_ipc_payload_string(task, "id")), g_free);
        clawt_lrg_row_add_suffix(row, cancel);
    }

    /*
     * Retry where the daemon would accept it -- the library's rule, so
     * the button and the refusal cannot disagree.
     */
    {
        gint value = 0;

        if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value) &&
            clawt_task_state_is_retryable((ClawtTaskState)value))
            clawt_lrg_row_add_suffix(
                row, retry_button("task",
                                  clawt_ipc_payload_string(task, "id")));
    }

    clawt_lrg_take(body, row);

    if (prompt != NULL && *prompt != '\0')
        clawt_lrg_take(body, clawt_lrg_paragraph(prompt, NULL));

    /*
     * The freshest thing anybody knows -- the assignee's own progress
     * note, or the caveat that nobody reported the work finished.  A
     * row that reads `completed` and nothing else cannot tell a reported
     * result from a turn that simply stopped.
     */
    if (note != NULL)
        clawt_lrg_take(body, clawt_lrg_paragraph(note, "caption"));

    /* why it ended, which GTK carries as the badge's tooltip */
    if (reason != NULL && *reason != '\0')
        clawt_lrg_take(body, clawt_lrg_paragraph(reason, "muted"));

    card = clawt_lrg_card(body);
    lrg_widget_set_margin(card, &indent);

    if (g_strcmp0(state, "running") == 0)
        lrg_widget_add_class(card, "clawt-busy");

    return card;
}

static void
on_tasks(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    g_autoptr(GPtrArray) rows = NULL;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != tasks.seq || tasks.list == NULL)
        return;

    clawt_lrg_clear(tasks.list);

    if (reply == NULL) {
        clawt_lrg_take(tasks.list,
                       clawt_lrg_empty_state(
                           "Tasks are unavailable",
                           "Check the daemon connection, then reopen Tasks "
                           "to retry."));
        return;
    }

    rows = clawt_lrg_task_tree(reply);

    if (rows->len == 0) {
        clawt_lrg_take(tasks.list,
                       clawt_lrg_empty_state(
                           "No tasks yet",
                           "Ask an agent to delegate work. Its progress and "
                           "result will appear here."));
        return;
    }

    for (i = 0; i < rows->len; i++) {
        ClawtLrgTaskRow *row = g_ptr_array_index(rows, i);
        LrgWidget *card = task_card(row->task, row->depth);

        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(tasks.list, card);
    }
}

static void
refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (tasks.list == NULL)
        return;

    tasks.seq++;
    clawt_lrg_request(app, "task.list", NULL, flags, on_tasks,
                      GUINT_TO_POINTER(tasks.seq), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Tasks", "What has been delegated, to whom, and whether it is still "
                 "going");

    tasks.app = app;
    tasks.incidents = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(clawt_lrg_page_body(page), tasks.incidents);
    g_object_unref(tasks.incidents);
    tasks.list = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(clawt_lrg_page_body(page), tasks.list);
    g_object_unref(tasks.list);

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_incidents(app, CLAWT_LRG_REQUEST_NONE);
    refresh_list(app, CLAWT_LRG_REQUEST_NONE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    if (kind == NULL || app->page != CLAWT_PAGE_TASKS)
        return;

    /*
     * Filing an incident changes no task, so it has its own prefix:
     * without it a report reached the lead and this page went on saying
     * nothing had broken.
     */
    if (g_str_has_prefix(kind, "task."))
        refresh_list(app, CLAWT_LRG_QUIET);

    if (g_str_has_prefix(kind, "task.") || g_str_has_prefix(kind, "incident."))
        refresh_incidents(app, CLAWT_LRG_QUIET);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_TASKS, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_tasks_page(void)
{
    return &page_impl;
}
