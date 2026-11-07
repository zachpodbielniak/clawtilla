/*
 * lrg-mailbox.c - The Mailbox page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-mailbox.c: the selected agent's delivery
 * backlog, the follow-ups held while it is mid-turn, and its dead
 * letters, with Ack, Requeue and Purge.
 *
 * Rows rather than an LrgTable: every line carries its own buttons and a
 * body that wraps, and LrgTable's cells are one line of text or a widget
 * per column from a GListModel of GObjects -- three conversions to draw
 * what a row already draws.  What each line *says* is decided in
 * lrg-agent-model.c (clawt_lrg_mailbox_rows()).
 */

#include "clawt-lrg.h"
#include "clawt-lrg-agent-model.h"

typedef struct {
    LrgWidget *page;
    LrgWidget *summary;
    LrgWidget *list;
    LrgWidget *purge;

    /*
     * The two halves of one listing arrive separately; the list is drawn
     * when both have, so a refresh never shows the waiting items without
     * the dead letters under them and then jumps.
     */
    JsonNode  *waiting;
    JsonNode  *dead;
    guint      pending;
    guint      ticket;
} MailboxPage;

static MailboxPage mb;

static void refresh(ClawtLrgApp *app);

/* ── Actions ──────────────────────────────────────────────────────── */

static void
on_item_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    /* Re-read whichever way it went: a failed ack may still have moved. */
    refresh(app);
}

static void
on_item_action(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *item = g_object_get_data(G_OBJECT(button), "clawt-item");
    const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");

    if (app->agent_id == NULL || item == NULL)
        return;

    /* Spelled out, never assembled: `make parity` greps for literals. */
    if (g_strcmp0(kind, "requeue") == 0)
        clawt_lrg_request(app, "mailbox.requeue",
                          clawt_ipc_payload_new("agent", app->agent_id,
                                                "item", item, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_item_done, NULL, NULL);
    else
        clawt_lrg_request(app, "mailbox.ack",
                          clawt_ipc_payload_new("agent", app->agent_id,
                                                "item", item, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_item_done, NULL, NULL);
}

static void
on_purged(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    g_autofree gchar *said = NULL;

    if (reply != NULL) {
        said = clawt_lrg_mailbox_purged_text(reply);
        clawt_lrg_toast(app, said);
    }

    refresh(app);
}

/*
 * Every expired item in this agent's mailbox.  Per agent: the daemon
 * resolves a mailbox from the payload and refuses without one, so a
 * fleet-wide sweep is not a thing it offers and the button does not
 * claim one.
 */
static void
on_purge(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (app->agent_id == NULL)
        return;

    clawt_lrg_request(app, "mailbox.purge",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_purged, NULL, NULL);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static LrgWidget *
item_button(ClawtLrgApp *app, const gchar *label, const gchar *kind,
            const gchar *item, const gchar *style)
{
    LrgWidget *button = clawt_lrg_button(label, style,
                                         G_CALLBACK(on_item_action), app);

    g_object_set_data_full(G_OBJECT(button), "clawt-item", g_strdup(item),
                           g_free);
    g_object_set_data(G_OBJECT(button), "clawt-kind", (gpointer)kind);

    return button;
}

static void
draw(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) rows = NULL;
    g_autofree gchar *summary = NULL;
    JsonObject *agent = clawt_lrg_selected_agent(app);
    guint i;

    clawt_lrg_clear(mb.list);

    summary = clawt_lrg_mailbox_summary(mb.waiting);
    lrg_label_set_text(LRG_LABEL(mb.summary), summary);

    rows = clawt_lrg_mailbox_rows(mb.waiting, mb.dead);

    if (rows->len == 0) {
        clawt_lrg_take(mb.list, clawt_lrg_empty_state(
            "Mailbox empty",
            clawt_lrg_mailbox_empty_text(
                clawt_ipc_payload_string(agent, "state"))));
        clawt_lrg_app_wake(app);
        return;
    }

    for (i = 0; i < rows->len; i++) {
        ClawtLrgMailboxRow *row = g_ptr_array_index(rows, i);
        LrgWidget *line = clawt_lrg_row(row->title, NULL);
        LrgWidget *card = clawt_lrg_card(NULL);
        LrgWidget *body = clawt_lrg_vbox(6.0f);

        if (row->badge != NULL)
            clawt_lrg_row_add_prefix(line, clawt_lrg_badge(row->badge,
                                                           row->badge_tone));

        if (row->can_requeue)
            clawt_lrg_row_add_suffix(line, item_button(app, "Requeue",
                                                       "requeue", row->id,
                                                       "primary"));

        if (row->can_ack)
            clawt_lrg_row_add_suffix(line, item_button(app, "Ack", "ack",
                                                       row->id, "flat"));

        /*
         * The body wraps in full.  A message cut to one line is a message
         * nobody can decide whether to ack.
         */
        clawt_lrg_take(body, line);
        clawt_lrg_take(body, clawt_lrg_paragraph(row->body, NULL));
        clawt_lrg_take(card, body);
        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(mb.list, card);
    }

    clawt_lrg_app_wake(app);
}

static void
on_listing(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    guint ticket = GPOINTER_TO_UINT(user_data) >> 1;
    gboolean dead = (GPOINTER_TO_UINT(user_data) & 1) != 0;

    /* A reply to a refresh that a newer one has replaced. */
    if (ticket != mb.ticket)
        return;

    if (dead) {
        g_clear_pointer(&mb.dead, json_node_unref);
        mb.dead = (reply != NULL) ? json_node_copy(reply) : NULL;
    } else {
        g_clear_pointer(&mb.waiting, json_node_unref);
        mb.waiting = (reply != NULL) ? json_node_copy(reply) : NULL;
    }

    if (mb.pending > 0)
        mb.pending--;

    if (mb.pending == 0)
        draw(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (mb.page == NULL)
        return;

    g_clear_pointer(&mb.waiting, json_node_unref);
    g_clear_pointer(&mb.dead, json_node_unref);
    mb.ticket++;

    lrg_widget_set_enabled(mb.purge, app->agent_id != NULL);

    if (app->agent_id == NULL) {
        clawt_lrg_clear(mb.list);
        lrg_label_set_text(LRG_LABEL(mb.summary), "No agent selected.");
        clawt_lrg_take(mb.list, clawt_lrg_empty_state(
            "No agent selected", "Pick an agent to see what is waiting for "
                                 "it."));
        clawt_lrg_app_wake(app);
        return;
    }

    mb.pending = 2;

    /*
     * Guarded: a listing for the agent somebody clicked away from must
     * not be drawn under the one they clicked to.  The low bit says which
     * half this is; the rest is the ticket.
     */
    clawt_lrg_request(app, "mailbox.list",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED, on_listing,
                      GUINT_TO_POINTER(mb.ticket << 1), NULL);
    clawt_lrg_request(app, "mailbox.dead",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_listing,
                      GUINT_TO_POINTER((mb.ticket << 1) | 1u), NULL);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    if (kind == NULL || app->page != CLAWT_PAGE_MAILBOX ||
        app->agent_id == NULL)
        return;

    /*
     * A typing frame is a level re-sent every 25 seconds; it says nothing
     * about the mailbox and would redraw the list under the pointer.
     */
    if (g_strcmp0(kind, "agent.typing") == 0)
        return;

    if (g_str_has_prefix(kind, "agent.") ||
        g_str_has_prefix(kind, "mailbox.") ||
        g_str_has_prefix(kind, "hold.") || g_strcmp0(kind, "message") == 0)
        refresh(app);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Mailbox", "Delivery backlog, follow-ups held mid-turn, and dead "
                   "letters");
    LrgWidget *body = clawt_lrg_page_body(page);

    mb.page = page;

    mb.purge = clawt_lrg_button("Purge expired items", "flat",
                                G_CALLBACK(on_purge), app);
    clawt_lrg_add(clawt_lrg_page_toolbar(page), mb.purge);

    mb.summary = clawt_lrg_label("No agent selected.", "muted");
    clawt_lrg_add(body, mb.summary);

    mb.list = clawt_lrg_vbox(10.0f);
    clawt_lrg_add(body, mb.list);

    /* the page holds them; the struct's pointers are borrowed */
    g_object_unref(mb.purge);
    g_object_unref(mb.summary);
    g_object_unref(mb.list);

    return page;
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_MAILBOX, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_mailbox_page(void)
{
    return &page_impl;
}
