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
    tasks.list = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(clawt_lrg_page_body(page), tasks.list);
    g_object_unref(tasks.list);

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_list(app, CLAWT_LRG_REQUEST_NONE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    if (kind != NULL && g_str_has_prefix(kind, "task.") &&
        app->page == CLAWT_PAGE_TASKS)
        refresh_list(app, CLAWT_LRG_QUIET);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_TASKS, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_tasks_page(void)
{
    return &page_impl;
}
