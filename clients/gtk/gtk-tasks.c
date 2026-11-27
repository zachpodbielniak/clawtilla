/*
 * gtk-tasks.c - The tasks page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What has been delegated, to whom, and whether it is still going.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Tasks ───────────────────────────────────────────────────────── */

/*
 * Cancels a task that is still going.
 *
 * Offered only while it is, because cancelling a finished one is not a
 * refusal the daemon needs to explain -- it is a button that should not
 * have been there.
 */
static void
on_task_cancel(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "task.cancel",
                                 clawt_build_payload("task", task_id, NULL));

    if (reply != NULL)
        clawt_gtk_refresh_tasks(self);
}

/*
 * Opens the room a task runs in, when it was delegated with `thread:
 * new`.  The work is not in the conversation the two agents share, so
 * without this a person reading the task list has no way to the
 * transcript the result came out of.  The id is copied off the button
 * before anything else runs: selecting a room refreshes this page,
 * which frees the button.
 */
static void
on_task_open_room(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *room = g_strdup(
        g_object_get_data(G_OBJECT(button), "room-id"));

    clawt_gtk_select_room(self, room);
    clawt_gtk_show_page(self, CLAWT_PAGE_CHAT);
}

/*
 * Sends a failed or stalled run round again -- a task re-queued for its
 * assignee, or a routine run again.  The button carries whichever id its
 * row had: a task row the task's, an incident row the incident's, so a
 * routine that never made a task can still be retried.
 */
static void
on_task_retry(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *key = g_object_get_data(G_OBJECT(button), "retry-key");
    const gchar *id = g_object_get_data(G_OBJECT(button), "retry-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "task.retry",
                                 clawt_build_payload(key, id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self,
                       clawt_json_boolean(clawt_payload_of(reply), "rerun",
                                          FALSE)
                       ? "Running the routine again"
                       : "Queued for its assignee again");
    clawt_gtk_refresh_tasks(self);
}

static GtkWidget *
retry_button(ClawtWindow *self, const gchar *key, const gchar *id)
{
    GtkWidget *retry = gtk_button_new_with_label("Retry");

    g_object_set_data(G_OBJECT(retry), "retry-key", (gpointer)key);
    g_object_set_data_full(G_OBJECT(retry), "retry-id", g_strdup(id), g_free);
    g_signal_connect(retry, "clicked", G_CALLBACK(on_task_retry), self);
    gtk_widget_set_valign(retry, GTK_ALIGN_CENTER);

    return retry;
}

/* ── The operator's own tasks ────────────────────────────────────── */

/*
 * One struct for the three dialogs this page opens -- a new task,
 * assigning one off the backlog, and a task's details -- since each is a
 * handful of the same widgets and a window to answer to.  Freed with the
 * dialog.
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *task_id;
    GtkWidget   *title_row;
    GtkTextView *prompt;
    GtkWidget   *agent_row;
    GtkWidget   *after_row;
    GtkWidget   *repo_row;
    GStrv        agent_ids;
    gboolean     with_nobody;
    GtkTextView *comment;
} TaskDialog;

static void
task_dialog_free(gpointer data)
{
    TaskDialog *dialog = data;

    g_free(dialog->task_id);
    g_strfreev(dialog->agent_ids);
    g_free(dialog);
}

static TaskDialog *
task_dialog_new(ClawtWindow *self, const gchar *title, const gchar *task_id)
{
    TaskDialog *dialog = g_new0(TaskDialog, 1);

    dialog->window = self;
    dialog->dialog = adw_dialog_new();
    dialog->task_id = g_strdup(task_id);
    adw_dialog_set_title(dialog->dialog, title);
    adw_dialog_set_content_width(dialog->dialog, 560);
    g_object_set_data_full(G_OBJECT(dialog->dialog), "dialog", dialog,
                           task_dialog_free);

    return dialog;
}

/* Puts @page in the dialog under a header bar and shows it. */
static void
task_dialog_present(TaskDialog *dialog, GtkWidget *page)
{
    GtkWidget *toolbar = adw_toolbar_view_new();

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog->dialog, toolbar);
    adw_dialog_present(dialog->dialog, GTK_WIDGET(dialog->window));
}

/*
 * The agent picker, read from agent.list when the dialog opens.  With
 * @with_nobody the first choice is "Nobody yet": the backlog, which is
 * the point of writing a task down before knowing who should do it.
 */
static GtkWidget *
task_agent_row(TaskDialog *dialog, gboolean with_nobody)
{
    GtkStringList *labels = gtk_string_list_new(NULL);
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(JsonNode) agents = NULL;
    guint i;

    dialog->with_nobody = with_nobody;

    if (with_nobody) {
        gtk_string_list_append(labels, "Nobody yet (the backlog)");
        g_ptr_array_add(ids, g_strdup(""));
    }

    agents = clawt_window_request(dialog->window, "agent.list", NULL);

    if (agents != NULL) {
        JsonArray *array = json_object_get_array_member(
            clawt_payload_of(agents), "agents");

        for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
            JsonObject *agent = json_array_get_object_element(array, i);
            const gchar *id = clawt_json_string(agent, "id", "");

            gtk_string_list_append(labels,
                                   clawt_json_string(agent, "name", id));
            g_ptr_array_add(ids, g_strdup(id));
        }
    }

    g_ptr_array_add(ids, NULL);
    dialog->agent_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    dialog->agent_row = clawt_gtk_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->agent_row),
                                  "Who does it");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->agent_row),
                            G_LIST_MODEL(labels));

    return dialog->agent_row;
}

/* The chosen agent's id, or NULL for "nobody yet". */
static const gchar *
task_dialog_agent(TaskDialog *dialog)
{
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->agent_row));

    if (dialog->agent_ids == NULL ||
        selected >= g_strv_length(dialog->agent_ids))
        return NULL;

    return (*dialog->agent_ids[selected] != '\0')
        ? dialog->agent_ids[selected] : NULL;
}

static gchar *
text_view_text(GtkTextView *view)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return g_strstrip(gtk_text_buffer_get_text(buffer, &start, &end, FALSE));
}

/* A framed, wrapping text view, for a prompt or a comment. */
static GtkWidget *
framed_text_view(GtkTextView **out, gint height)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkTextView *view = GTK_TEXT_VIEW(gtk_text_view_new());

    gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(view, 8);
    gtk_text_view_set_bottom_margin(view, 8);
    gtk_text_view_set_left_margin(view, 8);
    gtk_text_view_set_right_margin(view, 8);
    gtk_widget_set_size_request(GTK_WIDGET(view), -1, height);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(view));
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_margin_top(scroll, 6);

    *out = view;
    return scroll;
}

static void
on_new_task_create(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    g_autofree gchar *prompt = text_view_text(dialog->prompt);
    const gchar *title = gtk_editable_get_text(GTK_EDITABLE(dialog->title_row));
    const gchar *agent = task_dialog_agent(dialog);
    const gchar *after = gtk_editable_get_text(GTK_EDITABLE(dialog->after_row));
    const gchar *repo = gtk_editable_get_text(GTK_EDITABLE(dialog->repo_row));
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    (void)button;

    if (*prompt == '\0') {
        clawt_window_toast(dialog->window, "Say what the task is first");
        return;
    }

    reply = clawt_window_request(dialog->window, "task.create",
                                 clawt_build_payload(
                                     "prompt", prompt,
                                     "title", (*title != '\0') ? title : NULL,
                                     "agent", agent,
                                     "after", (*after != '\0') ? after : NULL,
                                     "repo", (*repo != '\0') ? repo : NULL,
                                     NULL));

    if (reply == NULL)
        return;

    said = (agent != NULL)
        ? g_strdup_printf("Handed to %s", agent)
        : g_strdup("On the backlog -- assign it when you know who");
    clawt_window_toast(dialog->window, said);
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/*
 * A task the operator writes down: what it is, optionally a title, and
 * who does it -- or nobody yet, which puts it on the backlog.
 */
static void
on_new_task(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    TaskDialog *dialog = task_dialog_new(self, "New task", NULL);
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *what = adw_preferences_group_new();
    GtkWidget *who = adw_preferences_group_new();
    GtkWidget *create = gtk_button_new_with_label("Create");

    (void)button;

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(what), "What");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(what),
        "Write it so it stands alone: the agent reads this and nothing "
        "else about why it is wanted.");
    dialog->title_row = clawt_gtk_add_entry(what, "Title (optional)", "");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(what),
                              framed_text_view(&dialog->prompt, 160));

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(who), "Who");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(who),
                              task_agent_row(dialog, TRUE));
    dialog->after_row = clawt_gtk_add_entry(who, "Wait for (task ids)", "");
    dialog->repo_row = clawt_gtk_add_entry(
        who, "Repository (absolute host path, for a checkout of its own)", "");

    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect(create, "clicked", G_CALLBACK(on_new_task_create), dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(who), create);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(what));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(who));
    task_dialog_present(dialog, page);
}

static void
on_assign_confirm(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    const gchar *agent = task_dialog_agent(dialog);
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    (void)button;

    if (agent == NULL)
        return;

    reply = clawt_window_request(dialog->window, "task.assign",
                                 clawt_build_payload("task", dialog->task_id,
                                                     "agent", agent, NULL));

    if (reply == NULL)
        return;

    said = g_strdup_printf("Handed to %s", agent);
    clawt_window_toast(dialog->window, said);
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/* Takes a task off the backlog: pick who does it. */
static void
on_task_assign(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    TaskDialog *dialog = task_dialog_new(self, "Assign", task_id);
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *assign = gtk_button_new_with_label("Assign");

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The work goes to them now, from you, with the task's id.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              task_agent_row(dialog, FALSE));

    gtk_widget_add_css_class(assign, "suggested-action");
    gtk_widget_set_margin_top(assign, 12);
    g_signal_connect(assign, "clicked", G_CALLBACK(on_assign_confirm), dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), assign);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    task_dialog_present(dialog, page);
}

static void
on_comment_send(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    g_autofree gchar *text = text_view_text(dialog->comment);
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (*text == '\0')
        return;

    reply = clawt_window_request(dialog->window, "task.comment",
                                 clawt_build_payload("task", dialog->task_id,
                                                     "text", text, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window,
                       clawt_json_boolean(clawt_payload_of(reply),
                                          "delivered", FALSE)
                       ? "Noted, and the assignee has it"
                       : "Noted on the task");
    adw_dialog_close(dialog->dialog);
}

/*
 * What the task's checkout changed: the diff in a dialog of its own,
 * monospaced, with the untracked files and a note when it was cut short.
 */
static void
on_show_changes(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GString) text = g_string_new(NULL);
    JsonObject *object;
    JsonArray *untracked;
    AdwDialog *view;
    GtkWidget *scroll;
    GtkWidget *body;
    GtkWidget *toolbar;
    guint i;

    (void)button;

    reply = clawt_window_request(dialog->window, "task.diff",
                                 clawt_build_payload("task", dialog->task_id,
                                                     NULL));
    if (reply == NULL)
        return;

    object = clawt_payload_of(reply);
    g_string_append(text, clawt_json_string(object, "diff", ""));

    untracked = json_object_has_member(object, "untracked")
        ? json_object_get_array_member(object, "untracked") : NULL;
    for (i = 0; untracked != NULL && i < json_array_get_length(untracked); i++)
        g_string_append_printf(text, "new, not yet added: %s\n",
                               json_array_get_string_element(untracked, i));

    if (text->len == 0)
        g_string_append(text, "No changes yet.\n");

    if (json_object_get_boolean_member_with_default(object, "truncated",
                                                    FALSE))
        g_string_append(text, "\n(cut short: read the rest in the "
                              "checkout)\n");

    body = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(body), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(body), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(body), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(body), 8);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(body)),
                             text->str, -1);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), body);
    gtk_widget_set_vexpand(scroll, TRUE);

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);

    view = adw_dialog_new();
    adw_dialog_set_title(view, "Changes");
    adw_dialog_set_content_width(view, 900);
    adw_dialog_set_content_height(view, 700);
    adw_dialog_set_child(view, toolbar);
    adw_dialog_present(view, GTK_WIDGET(dialog->window));
}

/* The checkout taken away, its branch kept in the repository. */
static void
on_remove_checkout(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "task.checkout_remove",
                                 clawt_build_payload("task", dialog->task_id,
                                                     NULL));
    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window,
                       "Checkout removed; its branch is kept");
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/* The ticket's page, in the browser. */
static void
on_open_ticket(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    const gchar *url = g_object_get_data(G_OBJECT(button), "url");
    g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);

    gtk_uri_launcher_launch(launcher, GTK_WINDOW(dialog->window), NULL, NULL,
                            NULL);
}

/* The mirror's rows for this task queued again, the task looked at anew. */
static void
on_venture_retry(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "task.venture_retry",
                                 clawt_build_payload("task", dialog->task_id,
                                                     NULL));
    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window, "Sending to VENTURE again");
    adw_dialog_close(dialog->dialog);
}

/*
 * The operator's verdict on a result in review: Accept, or Request
 * changes with the comment box's text as the notes.
 */
static void
on_review_verdict(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    const gchar *verdict = g_object_get_data(G_OBJECT(button), "verdict");
    g_autofree gchar *notes = text_view_text(dialog->comment);
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(verdict, "changes") == 0 && *notes == '\0') {
        clawt_window_toast(dialog->window,
                           "Write what to change in the comment box first");
        return;
    }

    reply = clawt_window_request(dialog->window, "task.review",
                                 clawt_build_payload(
                                     "task", dialog->task_id,
                                     "verdict", verdict,
                                     "notes", (*notes != '\0') ? notes : NULL,
                                     NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window,
                       clawt_json_string(clawt_payload_of(reply), "said",
                                         "Done"));
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_link(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    const gchar *blocker = gtk_editable_get_text(GTK_EDITABLE(dialog->after_row));
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (*blocker == '\0')
        return;

    reply = clawt_window_request(dialog->window, "task.link",
                                 clawt_build_payload("task", dialog->task_id,
                                                     "after", blocker, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window, "It waits for that task now");
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_unlink(GtkButton *button, gpointer user_data)
{
    TaskDialog *dialog = user_data;
    const gchar *blocker = g_object_get_data(G_OBJECT(button), "blocker");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(dialog->window, "task.unlink",
                                 clawt_build_payload("task", dialog->task_id,
                                                     "after", blocker, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window, "No longer waits for it");
    clawt_gtk_refresh_tasks(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/* One line of a thread or a history: who or what, when, and the text. */
static GtkWidget *
dated_row(const gchar *title, gint64 at, const gchar *body)
{
    GtkWidget *row = adw_action_row_new();
    g_autoptr(GDateTime) when = (at > 0)
        ? g_date_time_new_from_unix_local(at) : NULL;
    g_autofree gchar *stamp = (when != NULL)
        ? g_date_time_format(when, "%-d %b %H:%M") : g_strdup("");
    g_autofree gchar *heading = g_strdup_printf("%s · %s", title, stamp);

    clawt_gtk_set_row_text(row, heading, body != NULL ? body : "");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);

    return row;
}

/*
 * Everything known about one task: what was asked, its thread (comments,
 * progress, results), its history, and a box to comment in.
 */
static void
on_task_details(GtkButton *button, gpointer user_data)
{
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");

    clawt_gtk_show_task(user_data, task_id);
}

void
clawt_gtk_show_task(ClawtWindow *self, const gchar *task_id)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonObject *task;
    JsonArray *rows;
    TaskDialog *dialog;
    GtkWidget *page;
    GtkWidget *about;
    GtkWidget *thread;
    GtkWidget *history;
    GtkWidget *say;
    GtkWidget *send;
    GtkWidget *prompt;
    g_autofree gchar *who = NULL;
    guint i;

    if (self == NULL || task_id == NULL || *task_id == '\0')
        return;

    reply = clawt_window_request(self, "task.show",
                                 clawt_build_payload("task", task_id,
                                                     "journal", "true", NULL));

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    task = json_object_get_object_member(root, "task");
    dialog = task_dialog_new(self, clawt_json_string(task, "title", task_id),
                             task_id);
    adw_dialog_set_content_height(dialog->dialog, 720);
    page = adw_preferences_page_new();

    about = adw_preferences_group_new();
    who = clawt_json_string(task, "assignee", NULL) != NULL
        ? g_strdup_printf("%s → %s", clawt_json_string(task, "origin", "?"),
                          clawt_json_string(task, "assignee", "?"))
        : g_strdup_printf("%s · on the backlog",
                          clawt_json_string(task, "origin", "?"));
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(about),
                                    clawt_json_string(task, "state", "?"));
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(about), who);

    prompt = gtk_label_new(clawt_json_string(task, "prompt", ""));
    gtk_label_set_wrap(GTK_LABEL(prompt), TRUE);
    gtk_label_set_selectable(GTK_LABEL(prompt), TRUE);
    /* selectable, not focused: a focused label opens fully selected */
    gtk_widget_set_focusable(prompt, FALSE);
    gtk_label_set_xalign(GTK_LABEL(prompt), 0.0f);
    gtk_widget_add_css_class(prompt, "card");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(about), prompt);

    if (clawt_json_string(task, "result", NULL) != NULL)
        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(about),
            dated_row("Result", clawt_json_int(task, "finished_at", 0),
                      clawt_json_string(task, "result", "")));

    thread = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(thread), "Thread");
    rows = json_object_has_member(root, "notes")
        ? json_object_get_array_member(root, "notes") : NULL;

    for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
        JsonObject *note = json_array_get_object_element(rows, i);
        g_autofree gchar *title = g_strdup_printf(
            "%s (%s)", clawt_json_string(note, "author", "clawtilla"),
            clawt_json_string(note, "kind", "note"));

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(thread),
                                  dated_row(title,
                                            clawt_json_int(note, "at", 0),
                                            clawt_json_string(note, "body",
                                                              "")));
    }

    if (rows == NULL || json_array_get_length(rows) == 0)
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(thread), "Nothing said about it yet.");

    /*
     * What it waits for, each with a way to stop waiting -- the last one
     * taken away hands the work over now -- and, for a task on the
     * backlog or already waiting, a way to add one.
     */
    {
        GtkWidget *waits = adw_preferences_group_new();
        JsonArray *after = json_object_has_member(task, "after")
            ? json_object_get_array_member(task, "after") : NULL;
        const gchar *state = clawt_json_string(task, "state", "");
        guint w;

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(waits),
                                        "Waits for");

        for (w = 0; after != NULL && w < json_array_get_length(after); w++) {
            const gchar *blocker = json_array_get_string_element(after, w);
            GtkWidget *row = adw_action_row_new();
            GtkWidget *stop = gtk_button_new_with_label("Stop waiting");

            clawt_gtk_set_row_text(row, blocker, NULL);
            g_object_set_data_full(G_OBJECT(stop), "blocker",
                                   g_strdup(blocker), g_free);
            g_signal_connect(stop, "clicked", G_CALLBACK(on_unlink), dialog);
            gtk_widget_set_valign(stop, GTK_ALIGN_CENTER);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), stop);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(waits), row);
        }

        if (g_strcmp0(state, "unassigned") == 0 ||
            g_strcmp0(state, "blocked") == 0) {
            GtkWidget *add = gtk_button_new_with_label("Wait for it");

            dialog->after_row = clawt_gtk_add_entry(waits, "Task id", "");
            g_signal_connect(add, "clicked", G_CALLBACK(on_link), dialog);
            gtk_widget_set_margin_top(add, 6);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(waits), add);
        } else if (after == NULL) {
            adw_preferences_group_set_description(
                ADW_PREFERENCES_GROUP(waits),
                "Nothing. Only a task on the backlog can be made to wait.");
        }

        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                                 ADW_PREFERENCES_GROUP(waits));
    }

    say = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(say), "Comment");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(say),
        "Kept on the task. While it is with its assignee they hear it too, "
        "in the conversation the work is in.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(say),
                              framed_text_view(&dialog->comment, 80));
    send = gtk_button_new_with_label("Comment");
    gtk_widget_add_css_class(send, "suggested-action");
    gtk_widget_set_margin_top(send, 6);
    g_signal_connect(send, "clicked", G_CALLBACK(on_comment_send), dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(say), send);

    /*
     * Its checkout: where, on which branch, and what changed -- the diff
     * works after removal too, from the branch the repository kept.
     */
    if (json_object_has_member(task, "checkout")) {
        JsonObject *checkout = json_object_get_object_member(task, "checkout");
        GtkWidget *changes = adw_preferences_group_new();
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *show = gtk_button_new_with_label("Show changes");
        gboolean present = json_object_get_boolean_member_with_default(
            checkout, "present", FALSE);
        g_autofree gchar *where = g_strdup_printf(
            "Branch %s of %s, from %.12s.\n%s",
            clawt_json_string(checkout, "branch", ""),
            clawt_json_string(checkout, "repo", ""),
            clawt_json_string(checkout, "base", ""),
            present ? clawt_json_string(checkout, "path", "")
                    : "The checkout was removed; the branch is kept.");

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(changes),
                                        "Changes");
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(changes),
                                              where);
        g_signal_connect(show, "clicked", G_CALLBACK(on_show_changes),
                         dialog);
        gtk_box_append(GTK_BOX(line), show);

        if (present) {
            GtkWidget *remove = gtk_button_new_with_label("Remove checkout");

            gtk_widget_add_css_class(remove, "destructive-action");
            g_signal_connect(remove, "clicked",
                             G_CALLBACK(on_remove_checkout), dialog);
            gtk_box_append(GTK_BOX(line), remove);
        }

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(changes), line);
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                                 ADW_PREFERENCES_GROUP(changes));
    }

    /*
     * Its VENTURE ticket (0.44.0): the link when it is kept current, or
     * why it is not -- and Retry when the mirror skipped it or gave up.
     */
    if (json_object_has_member(root, "venture")) {
        JsonObject *venture = json_object_get_object_member(root, "venture");
        const gchar *vstate = clawt_json_string(venture, "state", "");
        const gchar *reason = clawt_json_string(venture, "reason", NULL);
        GtkWidget *group = adw_preferences_group_new();
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_autofree gchar *said = NULL;

        if (g_strcmp0(vstate, "synced") == 0)
            said = g_strdup_printf("Ticket #%" G_GINT64_FORMAT
                                   ", kept current as the task moves.",
                                   clawt_json_int(venture, "ticket", 0));
        else if (g_strcmp0(vstate, "queued") == 0)
            said = g_strdup_printf("Waiting to be sent%s%s",
                                   reason != NULL ? ": " : ".",
                                   reason != NULL ? reason : "");
        else if (g_strcmp0(vstate, "skipped") == 0)
            said = g_strdup_printf("Not mirrored: %s",
                                   reason != NULL ? reason : "no reason given");
        else
            said = g_strdup_printf("The mirror gave up: %s",
                                   reason != NULL ? reason : "no reason given");

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                        "VENTURE");
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                              said);

        if (clawt_json_string(venture, "url", NULL) != NULL) {
            GtkWidget *open = gtk_button_new_with_label("Open in VENTURE");

            g_object_set_data_full(G_OBJECT(open), "url",
                g_strdup(clawt_json_string(venture, "url", "")), g_free);
            g_signal_connect(open, "clicked", G_CALLBACK(on_open_ticket),
                             dialog);
            gtk_box_append(GTK_BOX(line), open);
        }

        if (g_strcmp0(vstate, "dead") == 0 ||
            g_strcmp0(vstate, "skipped") == 0) {
            GtkWidget *retry = gtk_button_new_with_label("Retry");

            g_signal_connect(retry, "clicked", G_CALLBACK(on_venture_retry),
                             dialog);
            gtk_box_append(GTK_BOX(line), retry);
        }

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), line);
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                                 ADW_PREFERENCES_GROUP(group));
    }

    /*
     * Who reviews it, and -- while a result is waiting -- the operator's
     * two answers.  The operator may decide whoever the reviewer is.
     */
    if (clawt_json_string(task, "reviewer", NULL) != NULL) {
        GtkWidget *review = adw_preferences_group_new();
        g_autofree gchar *who_reviews = g_strdup_printf(
            "Reviewed by %s%s",
            g_strcmp0(clawt_json_string(task, "reviewer", ""), "user") == 0
                ? "you" : clawt_json_string(task, "reviewer", ""),
            clawt_json_int(task, "review_round", 0) > 0
                ? "; changes asked for before" : "");

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(review),
                                        "Review");
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(review),
                                              who_reviews);

        if (g_strcmp0(clawt_json_string(task, "state", ""), "review") == 0) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *accept = gtk_button_new_with_label("Accept");
            GtkWidget *changes = gtk_button_new_with_label(
                "Request changes (from the comment box)");

            gtk_widget_add_css_class(accept, "suggested-action");
            g_object_set_data(G_OBJECT(accept), "verdict", (gpointer)"accept");
            g_object_set_data(G_OBJECT(changes), "verdict",
                              (gpointer)"changes");
            g_signal_connect(accept, "clicked",
                             G_CALLBACK(on_review_verdict), dialog);
            g_signal_connect(changes, "clicked",
                             G_CALLBACK(on_review_verdict), dialog);
            gtk_box_append(GTK_BOX(row), accept);
            gtk_box_append(GTK_BOX(row), changes);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(review), row);
        }

        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                                 ADW_PREFERENCES_GROUP(review));
    }

    history = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(history), "History");
    rows = json_object_has_member(root, "journal")
        ? json_object_get_array_member(root, "journal") : NULL;

    for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
        JsonObject *entry = json_array_get_object_element(rows, i);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(history),
                                  dated_row(clawt_json_string(entry, "state",
                                                              "?"),
                                            clawt_json_int(entry, "at", 0),
                                            clawt_json_string(entry, "what",
                                                              "")));
    }

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(about));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(thread));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(say));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(history));
    task_dialog_present(dialog, page);
}

/* A row button carrying its task's id. */
static GtkWidget *
task_button(ClawtWindow *self, const gchar *label, const gchar *task_id,
            GCallback callback)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "task-id", g_strdup(task_id),
                           g_free);
    g_signal_connect(button, "clicked", callback, self);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);

    return button;
}

/* Saves one incident setting; the refresh that follows redraws both. */
static void
configure_incidents(ClawtWindow *self, const gchar *member, JsonNode *value)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, member);
    json_builder_add_value(builder, value);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "incident.configure",
                                 json_builder_get_root(builder));

    if (reply != NULL)
        clawt_window_toast(self, "Saved");
}

static void
on_incidents_enabled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    JsonNode *value = json_node_new(JSON_NODE_VALUE);

    (void)pspec;
    json_node_set_boolean(value,
                          adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    configure_incidents(user_data, "enabled", value);
}

static void
on_incidents_to(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    JsonNode *value = json_node_new(JSON_NODE_VALUE);

    (void)pspec;

    if (selected >= clawt_incident_recipient_count()) {
        json_node_unref(value);
        return;
    }

    json_node_set_string(value, clawt_incident_recipient_nick(
                                    clawt_incident_recipient_nth(selected)));
    configure_incidents(user_data, "to", value);
}

/*
 * What became of an incident's report, in one line: who has it, who it
 * is waiting for, or why nobody was given it.
 */
static gchar *
incident_status(JsonObject *incident)
{
    const gchar *recipient = clawt_json_string(incident, "recipient", NULL);
    const gchar *note = clawt_json_string(incident, "note", NULL);
    gint64 occurrences = clawt_json_int(incident, "occurrences", 1);
    const gchar *retried_by = clawt_json_string(incident, "retried_by", NULL);
    g_autoptr(GString) out = g_string_new(NULL);

    if (note != NULL)
        g_string_append_printf(out, "Recorded for you: %s", note);
    else if (clawt_json_boolean(incident, "queued", FALSE) && recipient != NULL)
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

/*
 * The "Problems to look at" group: who hears about broken runs, and
 * each open incident.  The settings sit with the incidents rather than
 * on a preferences page, because the question "who was told about
 * this" is asked while looking at one.
 */
static void
refresh_incidents_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *incidents;
    GtkWidget *enabled;
    GtkWidget *to;
    g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
    const gchar *current;
    guint i;

    clawt_gtk_clear_list(self->incident_list);

    reply = clawt_window_request(self, "incident.list", NULL);

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    current = clawt_json_string(root, "to", "lead");

    /*
     * Built with their values before the handlers are connected, so
     * drawing the current setting does not save it straight back.
     */
    enabled = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(enabled),
                                  "Report broken runs");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(enabled),
                                "Failed, stalled and unstartable runs, and "
                                "routines that did not run");
    adw_switch_row_set_active(ADW_SWITCH_ROW(enabled),
                              clawt_json_boolean(root, "enabled", TRUE));
    g_signal_connect(enabled, "notify::active",
                     G_CALLBACK(on_incidents_enabled), self);
    gtk_list_box_append(self->incident_list, enabled);

    /* Walked from the library, never named here. */
    to = clawt_gtk_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(to),
                                  "Who hears about them");

    for (i = 0; i < clawt_incident_recipient_count(); i++)
        gtk_string_list_append(labels, clawt_incident_recipient_label(
                                           clawt_incident_recipient_nth(i)));

    adw_combo_row_set_model(ADW_COMBO_ROW(to), G_LIST_MODEL(labels));

    for (i = 0; i < clawt_incident_recipient_count(); i++) {
        if (g_strcmp0(clawt_incident_recipient_nick(
                          clawt_incident_recipient_nth(i)), current) == 0)
            adw_combo_row_set_selected(ADW_COMBO_ROW(to), i);
    }

    g_signal_connect(to, "notify::selected", G_CALLBACK(on_incidents_to),
                     self);
    gtk_list_box_append(self->incident_list, to);

    incidents = json_object_get_array_member(root, "incidents");

    if (incidents == NULL || json_array_get_length(incidents) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Nothing has broken",
                               "A run that fails, stalls or cannot start is "
                               "reported to its team's lead and listed here. "
                               "Each team's reports are in its Problems to "
                               "look at room.");
        gtk_list_box_append(self->incident_list, row);
        return;
    }

    for (i = 0; i < json_array_get_length(incidents); i++) {
        JsonObject *incident = json_array_get_object_element(incidents, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;
        g_autofree gchar *status = incident_status(incident);
        g_autofree gchar *subtitle = NULL;
        const gchar *reason = clawt_json_string(incident, "reason", NULL);

        title = g_strdup_printf("%s's run %s",
                                clawt_json_string(incident, "agent", "?"),
                                clawt_json_string(incident, "kind_label",
                                                  "broke"));
        subtitle = (reason != NULL)
                   ? g_strdup_printf("%s\n%s", reason, status)
                   : g_strdup(status);

        clawt_gtk_set_row_text(row, title, subtitle);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);

        adw_action_row_add_suffix(
            ADW_ACTION_ROW(row),
            clawt_gtk_badge(clawt_json_string(incident, "kind", "?"),
                            clawt_gtk_tone_class("bad"),
                            clawt_json_string(incident, "room", "")));

        if (clawt_json_boolean(incident, "retryable", FALSE))
            adw_action_row_add_suffix(
                ADW_ACTION_ROW(row),
                retry_button(self, "incident",
                             clawt_json_string(incident, "id", "")));

        gtk_list_box_append(self->incident_list, row);
    }
}

static void
refresh_tasks_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *tasks;
    guint i;

    refresh_incidents_once(self);

    clawt_gtk_clear_list(self->task_list);

    reply = clawt_window_request(self, "task.list", NULL);

    /*
     * What the lists cover: the open tasks and the last day's, kept
     * across restarts for the retention window -- or, from a daemon with
     * no task store, everything since it started.  The library says
     * which, in the words the other clients use.
     */
    if (self->tasks_since != NULL) {
        JsonObject *root = (reply != NULL) ? clawt_payload_of(reply) : NULL;
        g_autofree gchar *since = clawt_task_list_caption(
            (root != NULL) ? clawt_json_int(root, "since", 0) : 0,
            (root != NULL) ? clawt_json_boolean(root, "kept", FALSE) : FALSE,
            (root != NULL) ? clawt_json_int(root, "retention_days", 0) : 0,
            g_get_real_time() / G_USEC_PER_SEC);

        gtk_label_set_text(GTK_LABEL(self->tasks_since), since);
        gtk_widget_set_visible(self->tasks_since, reply != NULL);
    }

    if (reply == NULL) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Tasks are unavailable",
                              "Check the daemon connection, then reopen Tasks to retry.");
        gtk_list_box_append(self->task_list, row);
        return;
    }

    tasks = json_object_get_array_member(clawt_payload_of(reply), "tasks");

    if (json_array_get_length(tasks) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "No tasks yet",
                              "Ask an agent to delegate work. Its progress and result will appear here.");
        gtk_list_box_append(self->task_list, row);
    }

    for (i = 0; i < json_array_get_length(tasks); i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;

        /*
         * The task's title (given, or the prompt's first line), with who
         * it is between underneath -- the prompt itself is a page long,
         * and Details shows it whole.  A backlog task says it is one.
         */
        title = clawt_json_string(task, "assignee", NULL) != NULL
            ? g_strdup_printf("%s → %s",
                              clawt_json_string(task, "origin", "?"),
                              clawt_json_string(task, "assignee", "?"))
            : g_strdup_printf("%s · on the backlog",
                              clawt_json_string(task, "origin", "?"));

        clawt_gtk_set_row_text(row,
                               clawt_json_string(task, "title",
                                                 clawt_json_string(task,
                                                                   "prompt",
                                                                   "")),
                               title);

        {
            const gchar *state = clawt_json_string(task, "state", "");
            gint value = 0;
            const gchar *tone = "neutral";

            /*
             * Coloured by the same rule the web client uses.  Every task
             * badge here was "dim-label" whatever the state, so a failed
             * task and a pending one were the same grey -- the web client
             * at least tried, and got it wrong; this one did not try.
             *
             * A state this build does not know stays neutral rather than
             * refusing: a client one version behind should still draw the
             * row.
             */
            if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
                tone = clawt_task_state_tone((ClawtTaskState)value);

            adw_action_row_add_suffix(
                ADW_ACTION_ROW(row),
                clawt_gtk_badge(clawt_json_string(task, "state", "?"),
                                clawt_gtk_tone_class(tone),
                                clawt_json_string(task, "reason", "")));

            /*
             * The subtitle carries the freshest thing anybody knows: the
             * assignee's own progress note while the work runs, and the
             * caveat once it has ended without anybody saying so.  A row
             * that reads `completed` and nothing else cannot distinguish
             * a reported result from a turn that simply stopped.
             */
            /* The project it belongs to, as a quiet label on the row. */
            if (clawt_json_string(task, "project", NULL) != NULL) {
                GtkWidget *chip = gtk_label_new(
                    clawt_json_string(task, "project", ""));

                gtk_widget_add_css_class(chip, "dim-label");
                gtk_widget_add_css_class(chip, "caption");
                gtk_widget_set_tooltip_text(chip, "Project");
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), chip);
            }

            /* A held task says what it is waiting for. */
            if (json_object_has_member(task, "waiting_on") &&
                json_array_get_length(json_object_get_array_member(
                    task, "waiting_on")) > 0) {
                g_autofree gchar *waits = clawt_task_waiting_caption(
                    json_object_get_array_member(task, "waiting_on"));

                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), waits);
            } else if (clawt_json_string(task, "progress_note", NULL) != NULL)
                adw_action_row_set_subtitle(
                    ADW_ACTION_ROW(row),
                    clawt_json_string(task, "progress_note", ""));
            else if (clawt_json_boolean(task, "result_inferred", FALSE))
                adw_action_row_set_subtitle(
                    ADW_ACTION_ROW(row),
                    "nobody reported this finished -- it is the last thing "
                    "the assignee wrote");

            /*
             * A consultation says so: its result is an answer, and
             * nobody did any work to produce it.
             */
            if (clawt_json_boolean(task, "consult", FALSE))
                adw_action_row_add_suffix(
                    ADW_ACTION_ROW(row),
                    clawt_gtk_badge(clawt_task_consult_label(), "dim-label",
                                    clawt_message_consult_tooltip()));

            /* The room the work runs in, and the way into it. */
            {
                const gchar *room = clawt_json_string(task, "thread_room",
                                                      NULL);
                g_autofree gchar *summary = clawt_task_thread_summary(room);

                if (summary != NULL) {
                    GtkWidget *open = gtk_button_new_with_label("Open room");

                    gtk_widget_set_tooltip_text(open, summary);
                    g_object_set_data_full(G_OBJECT(open), "room-id",
                                           g_strdup(room), g_free);
                    g_signal_connect(open, "clicked",
                                     G_CALLBACK(on_task_open_room), self);
                    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
                    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
                }
            }

            /* Off the backlog: pick who does it. */
            if (g_strcmp0(state, "unassigned") == 0)
                adw_action_row_add_suffix(
                    ADW_ACTION_ROW(row),
                    task_button(self, "Assign…",
                                clawt_json_string(task, "id", ""),
                                G_CALLBACK(on_task_assign)));

            /* Its thread, history and a box to comment in. */
            adw_action_row_add_suffix(
                ADW_ACTION_ROW(row),
                task_button(self, "Details",
                            clawt_json_string(task, "id", ""),
                            G_CALLBACK(on_task_details)));

            /* Anything not ended, from the library: blocked and in review too. */
            if (clawt_task_state_nick_is_cancellable(state)) {
                GtkWidget *cancel = gtk_button_new_with_label("Cancel");

                g_object_set_data_full(
                    G_OBJECT(cancel), "task-id",
                    g_strdup(clawt_json_string(task, "id", "")), g_free);
                g_signal_connect(cancel, "clicked",
                                 G_CALLBACK(on_task_cancel), self);
                gtk_widget_set_valign(cancel, GTK_ALIGN_CENTER);
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), cancel);
            }

            /*
             * Retry where the daemon would accept it -- the library's
             * rule, so the button and the refusal cannot disagree.
             */
            if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value) &&
                clawt_task_state_is_retryable((ClawtTaskState)value))
                adw_action_row_add_suffix(
                    ADW_ACTION_ROW(row),
                    retry_button(self, "task",
                                 clawt_json_string(task, "id", "")));
        }

        gtk_list_box_append(self->task_list, row);
    }
}

void
clawt_gtk_refresh_tasks(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_TASKS))
        return;

    do {
        refresh_tasks_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_TASKS));
}

GtkWidget *
clawt_gtk_build_task_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *heading = gtk_label_new("Problems to look at");

    gtk_widget_add_css_class(heading, "heading");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_set_margin_top(heading, 12);
    gtk_widget_set_margin_start(heading, 12);
    gtk_box_append(GTK_BOX(column), heading);

    /* Filled on refresh, from task.list's `since`. */
    self->tasks_since = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(self->tasks_since), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->tasks_since), 0.0f);
    gtk_widget_add_css_class(self->tasks_since, "caption");
    gtk_widget_add_css_class(self->tasks_since, "dim-label");
    gtk_widget_set_margin_start(self->tasks_since, 12);
    gtk_widget_set_margin_end(self->tasks_since, 12);
    gtk_widget_set_visible(self->tasks_since, FALSE);
    gtk_box_append(GTK_BOX(column), self->tasks_since);

    self->incident_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->incident_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->incident_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->incident_list), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(self->incident_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->incident_list), 12);
    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->incident_list));

    /* The tasks' own heading, with the way to add one. */
    {
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *label = gtk_label_new("Tasks");
        GtkWidget *add = gtk_button_new_with_label("New task");

        gtk_widget_add_css_class(label, "heading");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(bar), label);

        gtk_widget_set_tooltip_text(add, "Write a task down for an agent, "
                                         "or for the backlog");
        g_signal_connect(add, "clicked", G_CALLBACK(on_new_task), self);
        gtk_box_append(GTK_BOX(bar), add);

        gtk_widget_set_margin_top(bar, 18);
        gtk_widget_set_margin_start(bar, 12);
        gtk_widget_set_margin_end(bar, 12);
        gtk_box_append(GTK_BOX(column), bar);
    }

    self->task_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->task_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->task_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->task_list), GTK_ALIGN_START);
    gtk_widget_set_margin_top(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->task_list), 12);

    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->task_list));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), column);
    /* Constrain long prompts to the viewport so they wrap instead of
     * making their action buttons disappear beyond the right edge. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    return scroll;
}
