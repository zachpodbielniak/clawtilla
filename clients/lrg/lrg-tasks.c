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
#include "clawt-lrg-settings-model.h"

/* how far each level of delegation is indented */
#define TASK_INDENT (28.0f)

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *list;
    guint        seq;

    /* what both lists cover: clawt_task_list_caption() */
    LrgWidget   *since;

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

/*
 * Opens the room a task runs in when it was delegated with `thread:
 * new`: the work is not in the conversation the two agents share, so
 * this is the only way from the task to the transcript it came out of.
 * Copied off the button first, because selecting a room redraws this
 * page and frees the button.
 */
static void
on_open_room_clicked(LrgButton *button, gpointer user_data)
{
    g_autofree gchar *room = g_strdup(
        g_object_get_data(G_OBJECT(button), "room-id"));

    (void)user_data;

    if (room != NULL)
        clawt_lrg_select_room(tasks.app, room);
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

/* ── The operator's own tasks ────────────────────────────────────── */

/*
 * What the three dialogs read back: the controls, and the task they are
 * about.  Freed with the dialog.
 */
typedef struct {
    gchar     *task_id;
    LrgWidget *title;
    LrgWidget *prompt;
    LrgWidget *agent;
    LrgWidget *after;
    LrgWidget *comment;
} TaskForm;

static void
task_form_free(gpointer data)
{
    TaskForm *form = data;

    g_free(form->task_id);
    g_free(form);
}

/*
 * The agent picker, from the fleet as the window last read it.  With
 * @with_nobody the first choice is the backlog.
 */
static LrgWidget *
agent_combo(ClawtLrgApp *app, gboolean with_nobody)
{
    JsonArray *agents = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(app->agents), "agents");
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    guint i;

    if (with_nobody) {
        g_ptr_array_add(ids, g_strdup(""));
        g_ptr_array_add(labels, g_strdup("Nobody yet (the backlog)"));
    }

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        const gchar *name = clawt_ipc_payload_string(agent, "name");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup(name != NULL ? name : id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    return clawt_lrg_combo((const gchar *const *)ids->pdata,
                           (const gchar *const *)labels->pdata,
                           with_nobody ? "" : NULL);
}

static void
on_task_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, user_data);
    refresh_list(app, CLAWT_LRG_QUIET);
}

static gboolean
on_new_task_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                     gpointer user_data)
{
    TaskForm *form = user_data;
    g_autofree gchar *prompt = clawt_lrg_editor_text(form->prompt);
    const gchar *title = clawt_lrg_entry_text(form->title);
    const gchar *agent = clawt_lrg_combo_id(form->agent);
    const gchar *after = clawt_lrg_entry_text(form->after);

    (void)dialog;
    (void)response;

    if (prompt == NULL || *g_strstrip(prompt) == '\0') {
        clawt_lrg_toast(app, "Say what the task is first");
        return FALSE;
    }

    if (agent != NULL && *agent == '\0')
        agent = NULL;

    clawt_lrg_request(app, "task.create",
                      clawt_ipc_payload_new(
                          "prompt", prompt,
                          "title", (title != NULL && *title != '\0')
                              ? title : NULL,
                          "agent", agent,
                          "after", (after != NULL && *after != '\0')
                              ? after : NULL, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_done,
                      (agent != NULL)
                          ? (gpointer)"Created, and handed over"
                          : (gpointer)"On the backlog -- assign it when you "
                                      "know who",
                      NULL);
    return TRUE;
}

/*
 * A task the operator writes down: what, optionally a title, and who --
 * or nobody yet, which puts it on the backlog.
 */
static void
on_new_task_clicked(LrgButton *button, gpointer user_data)
{
    TaskForm *form = g_new0(TaskForm, 1);
    LrgWidget *content = clawt_lrg_form_new();

    (void)button;
    (void)user_data;

    form->title = clawt_lrg_entry(NULL, "Fix the export");
    form->prompt = clawt_lrg_editor("", NULL);
    form->agent = agent_combo(tasks.app, TRUE);
    clawt_lrg_form_add(content, "Title (optional)", form->title, NULL);
    clawt_lrg_form_add(content, "What to do", form->prompt,
                       "Write it so it stands alone: the agent reads this "
                       "and nothing else about why it is wanted.");
    clawt_lrg_form_add(content, "Who does it", form->agent, NULL);
    form->after = clawt_lrg_entry(NULL, "task-01m..., task-01m...");
    clawt_lrg_form_add(content, "Wait for (task ids)", form->after,
                       "Held until these complete, then handed over.");

    g_object_unref(clawt_lrg_dialog(tasks.app, "New task", NULL, content,
                                    "Create", FALSE, on_new_task_response,
                                    form, task_form_free));
    g_object_unref(content);
}

static gboolean
on_assign_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                   gpointer user_data)
{
    TaskForm *form = user_data;
    const gchar *agent = clawt_lrg_combo_id(form->agent);

    (void)dialog;
    (void)response;

    if (agent == NULL || *agent == '\0')
        return FALSE;

    clawt_lrg_request(app, "task.assign",
                      clawt_ipc_payload_new("task", form->task_id,
                                            "agent", agent, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_done,
                      (gpointer)"Assigned", NULL);
    return TRUE;
}

/* Takes a task off the backlog: pick who does it. */
static void
on_assign_clicked(LrgButton *button, gpointer user_data)
{
    TaskForm *form = g_new0(TaskForm, 1);
    LrgWidget *content = clawt_lrg_form_new();

    (void)user_data;

    form->task_id = g_strdup(g_object_get_data(G_OBJECT(button), "task-id"));
    form->agent = agent_combo(tasks.app, FALSE);
    clawt_lrg_form_add(content, "Who does it", form->agent,
                       "The work goes to them now, from you, with the "
                       "task's id.");

    g_object_unref(clawt_lrg_dialog(tasks.app, "Assign", NULL, content,
                                    "Assign", FALSE, on_assign_response,
                                    form, task_form_free));
    g_object_unref(content);
}

static gboolean
on_comment_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                    gpointer user_data)
{
    TaskForm *form = user_data;
    g_autofree gchar *text = clawt_lrg_editor_text(form->comment);

    (void)dialog;
    (void)response;

    if (text == NULL || *g_strstrip(text) == '\0')
        return TRUE;

    clawt_lrg_request(app, "task.comment",
                      clawt_ipc_payload_new("task", form->task_id,
                                            "text", text, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_done,
                      (gpointer)"Noted on the task", NULL);
    return TRUE;
}

static LrgWidget *task_button(const gchar *label, const gchar *task_id,
                              GCallback callback);

static void
on_link_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    LrgWidget *entry = g_object_get_data(G_OBJECT(button), "entry");
    const gchar *blocker = (entry != NULL) ? clawt_lrg_entry_text(entry)
                                           : NULL;

    (void)user_data;

    if (blocker == NULL || *blocker == '\0')
        return;

    clawt_lrg_request(tasks.app, "task.link",
                      clawt_ipc_payload_new("task", task_id, "after", blocker,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_done,
                      (gpointer)"It waits for that task now", NULL);
}

static void
on_unlink_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    const gchar *blocker = g_object_get_data(G_OBJECT(button), "blocker");

    (void)user_data;

    clawt_lrg_request(tasks.app, "task.unlink",
                      clawt_ipc_payload_new("task", task_id, "after", blocker,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_done,
                      (gpointer)"No longer waits for it", NULL);
}

/* One line of a thread or a history: who or what, when, the text. */
static LrgWidget *
dated_line(const gchar *title, gint64 at, const gchar *body)
{
    LrgWidget *box = clawt_lrg_vbox(2.0f);
    g_autoptr(GDateTime) when = (at > 0)
        ? g_date_time_new_from_unix_local(at) : NULL;
    g_autofree gchar *stamp = (when != NULL)
        ? g_date_time_format(when, "%-d %b %H:%M") : g_strdup("");
    g_autofree gchar *heading = g_strdup_printf("%s \302\267 %s", title,
                                                stamp);

    clawt_lrg_take(box, clawt_lrg_label(heading, "caption"));

    if (body != NULL && *body != '\0')
        clawt_lrg_take(box, clawt_lrg_paragraph(body, NULL));

    return box;
}

/*
 * Everything known about one task -- what was asked, its thread, its
 * history -- and a box to comment in, the dialog's primary action.
 */
static void
on_task_shown(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonObject *task = (root != NULL && json_object_has_member(root, "task"))
        ? json_object_get_object_member(root, "task") : NULL;
    TaskForm *form;
    LrgWidget *content;
    JsonArray *rows;
    g_autofree gchar *who = NULL;
    guint i;

    if (task == NULL)
        return;

    form = g_new0(TaskForm, 1);
    form->task_id = g_strdup(user_data);
    content = clawt_lrg_vbox(10.0f);
    who = clawt_lrg_task_title(task);

    clawt_lrg_take(content, clawt_lrg_badge(
        clawt_ipc_payload_string(task, "state"),
        clawt_lrg_task_tone(clawt_ipc_payload_string(task, "state"))));
    clawt_lrg_take(content, clawt_lrg_paragraph(who, "muted"));
    clawt_lrg_take(content, clawt_lrg_paragraph(
        clawt_ipc_payload_string(task, "prompt"), NULL));

    if (clawt_ipc_payload_string(task, "result") != NULL)
        clawt_lrg_take(content, dated_line(
            "Result", clawt_ipc_payload_int(task, "finished_at", 0),
            clawt_ipc_payload_string(task, "result")));

    clawt_lrg_take(content, clawt_lrg_label("Thread", "heading"));
    rows = json_object_has_member(root, "notes")
        ? json_object_get_array_member(root, "notes") : NULL;

    for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
        JsonObject *note = json_array_get_object_element(rows, i);
        g_autofree gchar *title = g_strdup_printf(
            "%s (%s)",
            clawt_ipc_payload_string(note, "author") != NULL
                ? clawt_ipc_payload_string(note, "author") : "clawtilla",
            clawt_ipc_payload_string(note, "kind"));

        clawt_lrg_take(content, dated_line(
            title, clawt_ipc_payload_int(note, "at", 0),
            clawt_ipc_payload_string(note, "body")));
    }

    if (rows == NULL || json_array_get_length(rows) == 0)
        clawt_lrg_take(content, clawt_lrg_paragraph(
            "Nothing said about it yet.", "muted"));

    /*
     * What it waits for, each with Stop waiting, and for a backlog or
     * held task an id to add.
     */
    {
        JsonArray *after = json_object_has_member(task, "after")
            ? json_object_get_array_member(task, "after") : NULL;
        const gchar *state = clawt_ipc_payload_string(task, "state");
        guint w;

        clawt_lrg_take(content, clawt_lrg_label("Waits for", "heading"));

        for (w = 0; after != NULL && w < json_array_get_length(after); w++) {
            const gchar *blocker = json_array_get_string_element(after, w);
            LrgWidget *row = clawt_lrg_row(blocker, NULL);
            LrgWidget *stop = task_button("Stop waiting", form->task_id,
                                          G_CALLBACK(on_unlink_clicked));

            g_object_set_data_full(G_OBJECT(stop), "blocker",
                                   g_strdup(blocker), g_free);
            clawt_lrg_row_add_suffix(row, stop);
            clawt_lrg_take(content, row);
        }

        if (g_strcmp0(state, "unassigned") == 0 ||
            g_strcmp0(state, "blocked") == 0) {
            LrgWidget *line = clawt_lrg_hbox(8.0f);
            LrgWidget *entry = clawt_lrg_entry(NULL, "task id");
            LrgWidget *add = task_button("Wait for it", form->task_id,
                                         G_CALLBACK(on_link_clicked));

            g_object_set_data(G_OBJECT(add), "entry", entry);
            clawt_lrg_take(line, entry);
            clawt_lrg_take(line, add);
            clawt_lrg_take(content, line);
        } else if (after == NULL) {
            clawt_lrg_take(content, clawt_lrg_paragraph(
                "Nothing. Only a task on the backlog can be made to wait.",
                "muted"));
        }
    }

    form->comment = clawt_lrg_editor("", NULL);
    clawt_lrg_take(content, clawt_lrg_label("Comment", "heading"));
    clawt_lrg_take(content, clawt_lrg_paragraph(
        "Kept on the task. While it is with its assignee they hear it too, "
        "in the conversation the work is in.", "muted"));
    clawt_lrg_take(content, form->comment);

    clawt_lrg_take(content, clawt_lrg_label("History", "heading"));
    rows = json_object_has_member(root, "journal")
        ? json_object_get_array_member(root, "journal") : NULL;

    for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
        JsonObject *entry = json_array_get_object_element(rows, i);

        clawt_lrg_take(content, dated_line(
            clawt_ipc_payload_string(entry, "state"),
            clawt_ipc_payload_int(entry, "at", 0),
            clawt_ipc_payload_string(entry, "what")));
    }

    /*
     * In a scroller: a task's thread and history are as long as the work
     * was, and a dialog taller than the window cannot be read or closed.
     */
    {
        LrgScrollBox *scroll = lrg_scroll_box_new();

        lrg_scroll_box_set_hscroll(scroll, FALSE);
        lrg_scroll_box_set_vscroll(scroll, TRUE);
        lrg_scroll_box_set_kinetic(scroll, TRUE);
        lrg_scroll_box_set_child(scroll, content);
        g_object_unref(content);
        lrg_widget_set_min_size(LRG_WIDGET(scroll),
                                560.0f * (gfloat)app->scale,
                                520.0f * (gfloat)app->scale);

        g_object_unref(clawt_lrg_dialog(
            app, clawt_ipc_payload_string(task, "title") != NULL
                     ? clawt_ipc_payload_string(task, "title")
                     : form->task_id,
            NULL, LRG_WIDGET(scroll), "Comment", FALSE, on_comment_response,
            form, task_form_free));
        g_object_unref(scroll);
    }
}

static void
on_details_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "task-id");

    (void)user_data;

    if (id == NULL)
        return;

    clawt_lrg_request(tasks.app, "task.show",
                      clawt_ipc_payload_new("task", id, "journal", "true",
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_task_shown, g_strdup(id),
                      g_free);
}

/* A card button carrying its task's id. */
static LrgWidget *
task_button(const gchar *label, const gchar *task_id, GCallback callback)
{
    LrgWidget *button = clawt_lrg_button(label, "flat", callback, NULL);

    g_object_set_data_full(G_OBJECT(button), "task-id", g_strdup(task_id),
                           g_free);
    return button;
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
    /* the task's title, with who it is between underneath */
    LrgWidget *row = clawt_lrg_row(
        clawt_ipc_payload_string(task, "title") != NULL
            ? clawt_ipc_payload_string(task, "title") : title,
        title);
    LrgWidget *card;
    LrgInsets indent = { TASK_INDENT * (gfloat)depth, 0.0f, 0.0f, 0.0f };
    g_autofree gchar *note_room = NULL;

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

    /* A consultation: its result is an answer, not work done. */
    if (clawt_ipc_payload_boolean(task, "consult", FALSE))
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
                                          clawt_task_consult_label(), "info"));

    /* The room the work runs in, and the way into it. */
    {
        const gchar *room = clawt_ipc_payload_string(task, "thread_room");
        g_autofree gchar *summary = clawt_task_thread_summary(room);

        if (summary != NULL) {
            LrgWidget *open = clawt_lrg_button("Open room", "flat",
                                               G_CALLBACK(on_open_room_clicked),
                                               NULL);

            g_object_set_data_full(G_OBJECT(open), "room-id",
                                   g_strdup(room), g_free);
            clawt_lrg_row_add_suffix(row, open);
            note_room = g_steal_pointer(&summary);
        }
    }

    /* Off the backlog: pick who does it. */
    if (g_strcmp0(state, "unassigned") == 0)
        clawt_lrg_row_add_suffix(row, task_button(
            "Assign\342\200\246", clawt_ipc_payload_string(task, "id"),
            G_CALLBACK(on_assign_clicked)));

    /* Its thread, history and a box to comment in. */
    clawt_lrg_row_add_suffix(row, task_button(
        "Details", clawt_ipc_payload_string(task, "id"),
        G_CALLBACK(on_details_clicked)));

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
    /* A held task says what it is waiting for. */
    if (json_object_has_member(task, "waiting_on") &&
        json_array_get_length(json_object_get_array_member(task,
                                                           "waiting_on")) > 0) {
        g_autofree gchar *waits = clawt_task_waiting_caption(
            json_object_get_array_member(task, "waiting_on"));

        clawt_lrg_take(body, clawt_lrg_paragraph(waits, "caption"));
    }

    if (note != NULL)
        clawt_lrg_take(body, clawt_lrg_paragraph(note, "caption"));

    if (note_room != NULL)
        clawt_lrg_take(body, clawt_lrg_paragraph(note_room, "muted"));

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

    /*
     * What the lists cover, in the library's words: the open tasks and
     * the last day's, kept across restarts -- or everything since the
     * daemon started, from one with no task store.
     */
    if (tasks.since != NULL) {
        JsonObject *root = clawt_ipc_reply_object(reply);
        g_autofree gchar *since = clawt_task_list_caption(
            clawt_ipc_payload_int(root, "since", 0),
            clawt_ipc_payload_boolean(root, "kept", FALSE),
            clawt_ipc_payload_int(root, "retention_days", 0),
            g_get_real_time() / G_USEC_PER_SEC);

        clawt_lrg_paragraph_set_text(tasks.since, since);
        lrg_widget_set_visible(tasks.since, reply != NULL);
    }

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
    tasks.since = clawt_lrg_paragraph("", "muted");
    lrg_widget_set_visible(tasks.since, FALSE);
    clawt_lrg_add(clawt_lrg_page_body(page), tasks.since);
    g_object_unref(tasks.since);
    tasks.incidents = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(clawt_lrg_page_body(page), tasks.incidents);
    g_object_unref(tasks.incidents);
    /* The way to write a task down, above the list. */
    {
        LrgWidget *add = clawt_lrg_button("New task", "primary",
                                          G_CALLBACK(on_new_task_clicked),
                                          NULL);

        clawt_lrg_take(clawt_lrg_page_body(page), add);
    }

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
