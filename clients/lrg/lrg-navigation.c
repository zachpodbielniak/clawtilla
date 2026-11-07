/*
 * lrg-navigation.c - Go to… (Ctrl+K), mirrors clients/gtk/gtk-navigation.c
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A command palette: type, and the agents, declared rooms and pages that
 * match are listed; Up and Down move the highlight, Enter opens it,
 * Escape closes.  The destinations and the matching rule are the
 * model's (clawt_lrg_nav_targets(), clawt_navigation_matches()), so a
 * search here finds what the same words find in the other clients.
 *
 * The destinations are a snapshot taken when the palette opens -- it
 * owns copies of every name and id -- because a fleet refresh can
 * replace app->agents while it is open.  Filtering is local, so typing
 * never waits on the daemon.
 *
 * Up and Down: LrgTextInput leaves both to focus navigation, which
 * would walk the focus out of the entry and into the result buttons,
 * taking the typing with it.  While the palette is open two canvas
 * shortcuts send them to actions installed on the entry itself, so
 * they move the highlight instead and only while the entry has focus.
 * The shortcuts are removed when the palette closes, by any route.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

typedef struct {
    ClawtLrgApp        *app;
    gboolean            open;
    guint               generation;  /* bumped per opening, for replies */
    LrgDialog          *dialog;      /* weak */
    LrgWidget          *content;     /* strong */
    LrgWidget          *entry;
    LrgWidget          *scroll;
    LrgWidget          *results;
    LrgWidget          *notice;
    JsonNode           *agents;
    JsonNode           *rooms;
    GPtrArray          *targets;     /* ClawtLrgNavTarget */
    GArray             *visible;     /* guint indices into targets */
    GPtrArray          *rows;        /* LrgWidget, parallel to visible */
    guint               cursor;
    GSimpleActionGroup *actions;
} Navigation;

static Navigation nav;

/* ── Drawing the results ──────────────────────────────────────────── */

static void
highlight(void)
{
    guint i;

    for (i = 0; i < nav.rows->len; i++) {
        LrgWidget *row = g_ptr_array_index(nav.rows, i);

        if (i == nav.cursor) {
            lrg_widget_add_class(row, "clawt-row-selected");
            lrg_scroll_box_scroll_to_widget(LRG_SCROLL_BOX(nav.scroll), row);
        } else {
            lrg_widget_remove_class(row, "clawt-row-selected");
        }
    }

    clawt_lrg_app_wake(nav.app);
}

static void activate_visible(guint position);

static void
on_result_clicked(LrgButton *button, gpointer user_data)
{
    activate_visible(GPOINTER_TO_UINT(user_data));
}

/* A glyph per kind, standing in for GTK's three symbolic icons. */
static const gchar *
kind_glyph(ClawtLrgNavKind kind)
{
    switch (kind) {
    case CLAWT_LRG_NAV_AGENT:
        return "\342\227\217";   /* ● */

    case CLAWT_LRG_NAV_ROOM:
        return "#";

    case CLAWT_LRG_NAV_PAGE:
        return "\342\206\222";   /* → */
    }

    return "";
}

/*
 * Re-filters and redraws.  Text is plain throughout, including the
 * names the daemon supplied: a label here parses nothing.
 */
static void
navigation_search_changed(void)
{
    const gchar *query = clawt_lrg_entry_text(nav.entry);
    guint i;

    g_clear_pointer(&nav.visible, g_array_unref);
    nav.visible = clawt_lrg_nav_filter(nav.targets, query);
    g_ptr_array_set_size(nav.rows, 0);
    nav.cursor = 0;
    clawt_lrg_clear(nav.results);

    if (nav.visible->len == 0) {
        clawt_lrg_take(nav.results,
                       clawt_lrg_empty_state(
                           "No matches",
                           "Try a name, ID, team or page, or clear the "
                           "search."));
        clawt_lrg_app_wake(nav.app);
        return;
    }

    for (i = 0; i < nav.visible->len; i++) {
        ClawtLrgNavTarget *target = g_ptr_array_index(
            nav.targets, g_array_index(nav.visible, guint, i));
        LrgWidget *row = clawt_lrg_hbox(10.0f);
        LrgWidget *texts = clawt_lrg_vbox(0.0f);
        LrgWidget *glyph = clawt_lrg_label(kind_glyph(target->kind),
                                           "muted");
        LrgWidget *name = clawt_lrg_button(target->name, "flat",
                                           G_CALLBACK(on_result_clicked),
                                           GUINT_TO_POINTER(i));

        lrg_widget_add_class(row, "clawt-row");
        lrg_container_set_padding(LRG_CONTAINER(row), 6.0f);
        lrg_widget_set_valign(glyph, LRG_ALIGN_CENTER);
        lrg_widget_set_min_size(glyph, 18.0f, 0.0f);
        lrg_widget_set_fill_weight(texts, 1.0f);
        lrg_widget_set_halign(name, LRG_ALIGN_START);

        clawt_lrg_take(texts, name);
        clawt_lrg_take(texts, clawt_lrg_label(target->detail, "caption"));
        clawt_lrg_take(row, glyph);
        clawt_lrg_take(row, texts);

        g_ptr_array_add(nav.rows, row);
        clawt_lrg_take(nav.results, row);
    }

    highlight();
}

static void
on_search_changed(LrgTextInput *entry, gpointer user_data)
{
    navigation_search_changed();
}

/* Rebuilds the destinations from whatever listings are in hand. */
static void
rebuild_targets(void)
{
    g_clear_pointer(&nav.targets, g_ptr_array_unref);
    nav.targets = clawt_lrg_nav_targets(nav.agents, nav.rooms,
                                        nav.app->agent_id != NULL);
    navigation_search_changed();
}

/* ── Closing and choosing ─────────────────────────────────────────── */

/* Idempotent: Escape, Cancel and a choice all end up here. */
static void
navigation_finish(void)
{
    if (!nav.open)
        return;

    nav.open = FALSE;
    lrg_canvas_remove_shortcut(nav.app->canvas, "Up");
    lrg_canvas_remove_shortcut(nav.app->canvas, "Down");

    if (nav.dialog != NULL) {
        LrgDialog *dialog = nav.dialog;

        g_object_remove_weak_pointer(G_OBJECT(dialog),
                                     (gpointer *)&nav.dialog);
        nav.dialog = NULL;

        if (lrg_dialog_get_presented(dialog))
            lrg_dialog_close(dialog);
    }

    clawt_lrg_app_wake(nav.app);
}

static void
on_dialog_response(LrgDialog *dialog, gint response, gpointer user_data)
{
    navigation_finish();
}

/*
 * Copies the target before closing: the rows and the snapshot belong to
 * the palette, and selecting an agent rebuilds things that may refresh
 * the fleet underneath it.
 */
static void
activate_visible(guint position)
{
    ClawtLrgNavTarget *target;
    ClawtLrgNavKind kind;
    g_autofree gchar *id = NULL;
    ClawtLrgApp *app = nav.app;

    if (!nav.open || nav.visible == NULL || position >= nav.visible->len)
        return;

    target = g_ptr_array_index(nav.targets,
                               g_array_index(nav.visible, guint, position));
    kind = target->kind;
    id = g_strdup(target->target);

    navigation_finish();

    switch (kind) {
    case CLAWT_LRG_NAV_AGENT:
        clawt_lrg_select_agent(app, id);
        clawt_lrg_show_page(app, CLAWT_PAGE_CHAT);
        break;

    case CLAWT_LRG_NAV_ROOM:
        clawt_lrg_select_room(app, id);
        break;

    case CLAWT_LRG_NAV_PAGE:
        clawt_lrg_show_page(app, clawt_page_from_nick(id));
        break;
    }
}

/* Enter opens the highlighted result, without needing a pointer. */
static void
on_search_submitted(LrgTextInput *entry, gpointer user_data)
{
    activate_visible(nav.cursor);
}

static void
on_move(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    gint delta = GPOINTER_TO_INT(user_data);

    if (!nav.open || nav.visible == NULL)
        return;

    nav.cursor = clawt_lrg_nav_move(nav.cursor, delta, nav.visible->len);
    highlight();
}

/* ── Listings that were not loaded yet ────────────────────────────── */

static void
show_notice(void)
{
    lrg_widget_set_visible(nav.notice, TRUE);
    clawt_lrg_app_wake(nav.app);
}

static void
on_agents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    if (!nav.open || GPOINTER_TO_UINT(user_data) != nav.generation)
        return;

    /*
     * Quiet: a failure is said inside the palette, not as a toast
     * behind it, and pages stay reachable while disconnected.
     */
    if (reply == NULL) {
        show_notice();
        return;
    }

    g_clear_pointer(&nav.agents, json_node_unref);
    nav.agents = json_node_copy(reply);
    rebuild_targets();
}

static void
on_rooms(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    if (!nav.open || GPOINTER_TO_UINT(user_data) != nav.generation)
        return;

    if (reply == NULL) {
        show_notice();
        return;
    }

    g_clear_pointer(&nav.rooms, json_node_unref);
    nav.rooms = json_node_copy(reply);
    rebuild_targets();
}

/* ── Opening ──────────────────────────────────────────────────────── */

static void
reset_state(void)
{
    g_clear_object(&nav.content);
    g_clear_pointer(&nav.agents, json_node_unref);
    g_clear_pointer(&nav.rooms, json_node_unref);
    g_clear_pointer(&nav.targets, g_ptr_array_unref);
    g_clear_pointer(&nav.visible, g_array_unref);
    g_clear_pointer(&nav.rows, g_ptr_array_unref);
    nav.cursor = 0;
}

static GSimpleActionGroup *
build_actions(void)
{
    GSimpleActionGroup *group = g_simple_action_group_new();
    g_autoptr(GSimpleAction) up = g_simple_action_new("up", NULL);
    g_autoptr(GSimpleAction) down = g_simple_action_new("down", NULL);

    g_signal_connect(up, "activate", G_CALLBACK(on_move),
                     GINT_TO_POINTER(-1));
    g_signal_connect(down, "activate", G_CALLBACK(on_move),
                     GINT_TO_POINTER(1));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(up));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(down));

    return group;
}

void
clawt_lrg_navigation_open(ClawtLrgApp *app)
{
    LrgWidget *content = clawt_lrg_vbox(10.0f);
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgDialog *dialog;

    /* one palette at a time: Ctrl+K again while it is open does nothing */
    if (nav.open)
        return;

    reset_state();
    nav.app = app;
    nav.open = TRUE;
    nav.generation++;
    nav.rows = g_ptr_array_new();

    if (nav.actions == NULL)
        nav.actions = build_actions();

    nav.notice = clawt_lrg_paragraph("Some destinations are unavailable. "
                                     "Close and reopen to retry.", "muted");
    lrg_widget_set_visible(nav.notice, FALSE);
    clawt_lrg_add(content, nav.notice);
    g_object_unref(nav.notice);

    nav.entry = clawt_lrg_entry(NULL, "Find agents, rooms and pages");
    lrg_widget_insert_action_group(nav.entry, "goto",
                                   G_ACTION_GROUP(nav.actions));
    g_signal_connect(nav.entry, "text-changed",
                     G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(nav.entry, "submitted",
                     G_CALLBACK(on_search_submitted), NULL);
    clawt_lrg_add(content, nav.entry);
    g_object_unref(nav.entry);

    nav.results = clawt_lrg_vbox(2.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, nav.results);
    g_object_unref(nav.results);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), 480.0f, 380.0f);
    lrg_widget_set_max_size(LRG_WIDGET(scroll), -1.0f, 380.0f);
    nav.scroll = LRG_WIDGET(scroll);
    clawt_lrg_take(content, LRG_WIDGET(scroll));

    clawt_lrg_take(content, clawt_lrg_label(
                                "Enter to open \302\267 \342\206\221\342\206"
                                "\223 to move \302\267 Esc to close",
                                "caption"));
    nav.content = content;

    /*
     * The shell's listings when it has them, copied; asked for when it
     * does not (the window opened against a daemon still connecting).
     */
    if (app->agents != NULL)
        nav.agents = json_node_copy(app->agents);
    else
        clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET,
                          on_agents, GUINT_TO_POINTER(nav.generation), NULL);

    if (app->rooms != NULL)
        nav.rooms = json_node_copy(app->rooms);
    else
        clawt_lrg_request(app, "room.list", NULL, CLAWT_LRG_QUIET, on_rooms,
                          GUINT_TO_POINTER(nav.generation), NULL);

    rebuild_targets();

    dialog = clawt_lrg_dialog(app, "Go to…", NULL, content, NULL, FALSE,
                              NULL, NULL, NULL);
    nav.dialog = dialog;
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&nav.dialog);
    g_object_unref(dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response),
                     NULL);

    lrg_canvas_add_shortcut(app->canvas, "Up", "goto.up", NULL);
    lrg_canvas_add_shortcut(app->canvas, "Down", "goto.down", NULL);
    lrg_canvas_set_focused_widget(app->canvas, nav.entry);
    clawt_lrg_app_wake(app);
}
