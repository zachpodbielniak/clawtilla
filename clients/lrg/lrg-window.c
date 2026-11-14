/*
 * lrg-window.c - The shell: sidebar, header, island, sections, pages
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Layout:
 *
 *   canvas
 *   ├── backdrop        animated background (lrg-app.c)
 *   └── root (hbox)
 *       ├── sidebar     the fleet (lrg-sidebar.c), in the chrome material
 *       └── main (vbox)
 *           ├── header  agent, section tabs, island, menu
 *           ├── subtabs a section's pages, when it has more than one
 *           ├── banner  disconnected / paused, until it is not
 *           └── switcher one child per page, sliding between them
 *
 * Sections and pages are walked from the library (clawt_section_*,
 * clawt_section_page_*), the same enumeration GTK and the web client
 * walk, so a page added there appears here or the build says why not.
 */

#include "clawt-lrg.h"

#include <raylib.h>
#include <string.h>

#define N_PAGES (16)

/*
 * How long fleet events are gathered before the lists they invalidate
 * are fetched again.  A busy fleet sends turn.step and agent.* several
 * times a second; one agent.list and one activity.list per event was a
 * request, a parse and a sidebar rebuild each, and a window that could
 * not keep up.  A quarter second is below what reads as lag.
 */
#define REFRESH_COALESCE_MS (250)

typedef enum {
    REFRESH_AGENTS    = 1 << 0,
    REFRESH_ROOMS     = 1 << 1,
    REFRESH_ACTIVITY  = 1 << 2,
    REFRESH_DECISIONS = 1 << 3,
    REFRESH_USAGE     = 1 << 4,
    REFRESH_GROK      = 1 << 5
} RefreshKind;

typedef struct {
    LrgWidget  *header_title;
    LrgWidget  *header_subtitle;
    LrgWidget  *header_avatar_slot;
    LrgWidget  *section_bar;
    LrgWidget  *page_bar;
    LrgWidget  *island_compact;
    LrgWidget  *island_expanded;
    LrgWidget  *banner_label;
    LrgWidget  *banner_action;
    LrgWidget  *section_tabs[N_PAGES];
    LrgWidget  *page_tabs[N_PAGES];
    LrgWidget  *page_widgets[N_PAGES];
    guint       page_badges[N_PAGES];
    gchar      *page_badge_tones[N_PAGES];
    gboolean    built[N_PAGES];
    LrgWidget  *main_column;
    LrgWidget  *alerts_drawer;
    gfloat      column_cap;    /* the last cap set, so it is set on change */
    LrgWidget  *root;          /* the hbox the sidebar lives in when it is beside */
    LrgWidget  *sidebar_toggle;
    gboolean    narrow;        /* the window is below CLAWT_LRG_NARROW_WIDTH */
    gboolean    drawer_open;   /* narrow, and the sidebar is open over the page */
    gboolean    sidebar_over;  /* the sidebar is a canvas child, not in root */
    guint       refresh_due;   /* RefreshKind bits waiting on refresh_source */
    GSource    *refresh_source;
    GSimpleActionGroup *actions;
} Shell;

static Shell shell;

/* ── The page registry ────────────────────────────────────────────── */

const ClawtLrgPage *
clawt_lrg_page_for(ClawtPage page)
{
    /*
     * A switch with no `default:`: -Wswitch names a page the library
     * added that this client has not built, which is the parity failure
     * caught at compile time rather than as a tab that leads nowhere.
     */
    switch (page) {
    case CLAWT_PAGE_CHAT:      return clawt_lrg_chat_page();
    case CLAWT_PAGE_AGENT:     return clawt_lrg_agent_page();
    case CLAWT_PAGE_MAILBOX:   return clawt_lrg_mailbox_page();
    case CLAWT_PAGE_COMPUTER:  return clawt_lrg_computer_page();
    case CLAWT_PAGE_ROUTINES:  return clawt_lrg_routines_page();
    case CLAWT_PAGE_TRIGGERS:  return clawt_lrg_triggers_page();
    case CLAWT_PAGE_TASKS:     return clawt_lrg_tasks_page();
    case CLAWT_PAGE_DECISIONS: return clawt_lrg_decisions_page();
    case CLAWT_PAGE_FLOW:      return clawt_lrg_flow_page();
    case CLAWT_PAGE_ACTIVITY:  return clawt_lrg_activity_page();
    case CLAWT_PAGE_FLEET:     return clawt_lrg_fleet_page();
    case CLAWT_PAGE_GROK:      return clawt_lrg_grok_page();
    case CLAWT_PAGE_SKILLS:    return clawt_lrg_skills_page();
    case CLAWT_PAGE_MEMORY:    return clawt_lrg_memory_page();
    }

    return NULL;
}

/* ── Fleet lookups ────────────────────────────────────────────────── */

JsonObject *
clawt_lrg_find_agent(ClawtLrgApp *app, const gchar *agent_id)
{
    JsonObject *root;
    JsonArray *agents;
    guint i;

    if (agent_id == NULL)
        return NULL;

    root = clawt_ipc_reply_object(app->agents);

    if (root == NULL || !json_object_has_member(root, "agents"))
        return NULL;

    agents = json_object_get_array_member(root, "agents");

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (g_strcmp0(clawt_ipc_payload_string(agent, "id"), agent_id) == 0)
            return agent;
    }

    return NULL;
}

JsonObject *
clawt_lrg_selected_agent(ClawtLrgApp *app)
{
    return clawt_lrg_find_agent(app, app->agent_id);
}

/* ── Header ───────────────────────────────────────────────────────── */

static void
update_header(ClawtLrgApp *app)
{
    JsonObject *agent = clawt_lrg_selected_agent(app);
    const gchar *name = NULL;
    const gchar *description = NULL;

    clawt_lrg_clear(shell.header_avatar_slot);

    if (agent != NULL) {
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        const gchar *state = clawt_ipc_payload_string(agent, "state");

        name = clawt_ipc_payload_string(agent, "name");
        description = clawt_ipc_payload_string(agent, "description");

        clawt_lrg_take(shell.header_avatar_slot,
                       clawt_lrg_avatar(app, (name != NULL) ? name : id, id,
                                        clawt_ipc_payload_boolean(
                                            agent, "has_avatar", FALSE),
                                        clawt_ipc_payload_string(agent,
                                                                 "color"),
                                        40.0f,
                                        g_strcmp0(state, "running") == 0));

        if (name == NULL)
            name = id;
    } else if (app->room_id != NULL) {
        name = app->room_id;
        description = "Room";
    }

    lrg_label_set_text(LRG_LABEL(shell.header_title),
                       (name != NULL) ? name : "clawtilla");
    lrg_label_set_text(LRG_LABEL(shell.header_subtitle),
                       (description != NULL) ? description
                       : (agent == NULL && app->room_id == NULL)
                           ? "Choose an agent or a room" : "");
}

/* ── Sections and pages ───────────────────────────────────────────── */

static void
style_tab(LrgWidget *tab, gboolean active)
{
    if (tab == NULL)
        return;

    if (active) {
        lrg_widget_add_class(tab, "clawt-tab-active");
        lrg_widget_set_state_flags(tab, LRG_WIDGET_STATE_SELECTED);
    } else {
        lrg_widget_remove_class(tab, "clawt-tab-active");
        lrg_widget_unset_state_flags(tab, LRG_WIDGET_STATE_SELECTED);
    }
}

/* A tab's label: its name, plus the badge count when there is one. */
static gchar *
tab_label(const gchar *name, guint count)
{
    if (count == 0)
        return g_strdup(name);

    return g_strdup_printf("%s  %u", name, count);
}

static void
update_tabs(ClawtLrgApp *app)
{
    ClawtSection current = clawt_page_section(app->page);
    guint n_sections = clawt_section_count();
    guint s;

    for (s = 0; s < n_sections; s++) {
        ClawtSection section = clawt_section_nth(s);
        guint n_pages = clawt_section_page_count(section);
        guint total = 0;
        guint p;
        g_autofree gchar *label = NULL;

        /*
         * A section's tab carries the sum of its pages' badges, or a
         * count one level down is invisible until somebody opens the
         * section -- which for Decisions defeats having one.
         */
        for (p = 0; p < n_pages; p++)
            total += shell.page_badges[clawt_section_page_nth(section, p)];

        label = tab_label(clawt_section_label(section), total);
        lrg_button_set_text(LRG_BUTTON(shell.section_tabs[section]), label);
        style_tab(shell.section_tabs[section], section == current);
    }

    /* the inner row: only for a section with more than one page */
    clawt_lrg_clear(shell.page_bar);

    {
        guint n_pages = clawt_section_page_count(current);
        guint p;

        lrg_widget_set_visible(shell.page_bar, n_pages > 1);

        for (p = 0; n_pages > 1 && p < n_pages; p++) {
            ClawtPage page = clawt_section_page_nth(current, p);
            g_autofree gchar *label =
                tab_label(clawt_page_label(page), shell.page_badges[page]);

            lrg_button_set_text(LRG_BUTTON(shell.page_tabs[page]), label);
            style_tab(shell.page_tabs[page], page == app->page);
            clawt_lrg_add(shell.page_bar, shell.page_tabs[page]);
        }
    }
}

void
clawt_lrg_set_page_badge(ClawtLrgApp *app, ClawtPage page, guint count,
                         const gchar *tone)
{
    g_return_if_fail((guint)page < N_PAGES);

    shell.page_badges[page] = count;
    g_free(shell.page_badge_tones[page]);
    shell.page_badge_tones[page] = g_strdup(tone);

    if (shell.section_bar != NULL)
        update_tabs(app);

    clawt_lrg_app_wake(app);
}

void
clawt_lrg_show_page(ClawtLrgApp *app, ClawtPage page)
{
    const ClawtLrgPage *impl = clawt_lrg_page_for(page);

    g_return_if_fail(impl != NULL);

    app->page = page;

    if (!shell.built[page]) {
        shell.page_widgets[page] = impl->build(app);
        lrg_switcher_add_named(LRG_SWITCHER(app->switcher),
                               shell.page_widgets[page],
                               clawt_page_nick(page));
        shell.built[page] = TRUE;
    }

    lrg_switcher_set_visible_child_name(LRG_SWITCHER(app->switcher),
                                        clawt_page_nick(page));
    update_tabs(app);
    update_header(app);

    if (impl->refresh != NULL)
        impl->refresh(app);

    clawt_lrg_app_wake(app);
}

static void
on_section_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = g_object_get_data(G_OBJECT(button), "clawt-app");
    ClawtSection section = (ClawtSection)GPOINTER_TO_UINT(user_data);

    clawt_lrg_show_page(app, clawt_section_default_page(section));
}

static void
on_page_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = g_object_get_data(G_OBJECT(button), "clawt-app");

    clawt_lrg_show_page(app, (ClawtPage)GPOINTER_TO_UINT(user_data));
}

/* ── Selection ────────────────────────────────────────────────────── */

/* ── The sidebar: beside, hidden, or a drawer over the page ───────── */

/*
 * Puts the sidebar where the current state says.  Wide: in the row,
 * shown unless the person hid it (`sidebar_collapsed`).  Narrow: never
 * in the row -- beside the page it left a column too thin to read --
 * and, while the drawer is open, a child of the canvas on top of the
 * page instead.  Moving it rather than drawing a second one keeps one
 * sidebar, with one selection and one scroll position.
 */
static void
place_sidebar(ClawtLrgApp *app)
{
    LrgWidget *sidebar = app->sidebar;
    gboolean over = shell.narrow && shell.drawer_open;
    gboolean shown = shell.narrow
        ? shell.drawer_open
        : !clawt_appearance_get_sidebar_collapsed(app->appearance);

    if (sidebar == NULL || shell.root == NULL)
        return;

    if (over != shell.sidebar_over) {
        g_object_ref(sidebar);
        lrg_container_remove_child(
            LRG_CONTAINER(over ? shell.root : LRG_WIDGET(app->canvas)), sidebar);
        if (over)
            lrg_container_add_child(LRG_CONTAINER(app->canvas), sidebar);
        else
            lrg_container_insert_child_at(LRG_CONTAINER(shell.root), sidebar, 0);
        g_object_unref(sidebar);
        shell.sidebar_over = over;
    }

    if (over)
        lrg_widget_add_class(sidebar, "clawt-drawer");
    else
        lrg_widget_remove_class(sidebar, "clawt-drawer");

    lrg_widget_set_visible(sidebar, shown);

    if (shell.sidebar_toggle != NULL)
        lrg_button_set_text(LRG_BUTTON(shell.sidebar_toggle),
                            shown ? "\xe2\x80\xb9" : "\xe2\x80\xba");

    /* the column's width depends on it: recompute on the next resize */
    shell.column_cap = -1.0f;
    clawt_lrg_app_wake(app);
}

gboolean
clawt_lrg_window_sidebar_shown(ClawtLrgApp *app)
{
    return app->sidebar != NULL && lrg_widget_get_visible(app->sidebar);
}

void
clawt_lrg_window_toggle_sidebar(ClawtLrgApp *app)
{
    g_autoptr(GError) error = NULL;

    /*
     * Narrow, the toggle opens and closes the drawer and remembers
     * nothing: the window being small is not a decision about the
     * sidebar.  Wide, it is the person's choice, and it is saved, so
     * the next window opens the way this one was left -- and GTK, which
     * reads the same key, does too.
     */
    if (shell.narrow) {
        shell.drawer_open = !shell.drawer_open;
    } else {
        clawt_appearance_set_sidebar_collapsed(
            app->appearance,
            !clawt_appearance_get_sidebar_collapsed(app->appearance));
        if (!clawt_appearance_save(app->appearance, NULL, &error))
            g_warning("could not save the sidebar's state: %s", error->message);
    }

    place_sidebar(app);
}

/* An agent or a room was chosen from the drawer: it has done its job. */
static void
close_drawer(ClawtLrgApp *app)
{
    if (shell.narrow && shell.drawer_open) {
        shell.drawer_open = FALSE;
        place_sidebar(app);
    }
}

void
clawt_lrg_window_resize(ClawtLrgApp *app, gfloat width, gfloat height)
{
    gfloat used = 0.0f;
    gfloat w;

    if (shell.main_column == NULL)
        return;

    /* crossing the narrow edge either way closes a drawer */
    {
        gboolean narrow = width < CLAWT_LRG_NARROW_WIDTH * (gfloat)app->scale;

        if (narrow != shell.narrow) {
            shell.narrow = narrow;
            shell.drawer_open = FALSE;
            place_sidebar(app);
        }
    }

    /* over the page, the drawer is the window's height at its own width */
    if (shell.sidebar_over) {
        lrg_widget_set_position(app->sidebar, 0.0f, 0.0f);
        lrg_widget_set_size(app->sidebar, 340.0f * (gfloat)app->scale, height);
    }

    /*
     * The column gets what is left and no more.  A box allocates its
     * children their natural width, and one natural width too many -- a
     * label that cannot wrap, a long id -- pushed the page, the header
     * and its menu past the right edge with nothing to say they had
     * gone.  Capping it here stops that at the root, whatever page it
     * came from; the content then wraps or clips inside the column.
     */
    /* a drawer is over the page, not beside it, and takes no width */
    if (app->sidebar != NULL && lrg_widget_get_visible(app->sidebar) &&
        !shell.sidebar_over)
        lrg_widget_get_min_size(app->sidebar, &w, NULL), used += w;

    if (shell.alerts_drawer != NULL &&
        lrg_widget_get_visible(shell.alerts_drawer))
        lrg_widget_get_min_size(shell.alerts_drawer, &w, NULL), used += w;

    /*
     * Only when it changes.  lrg_widget_set_max_size() queues a layout
     * every time it is called, value or no value, and this runs every
     * frame -- so setting it unconditionally re-laid out the whole window,
     * every markdown label in the transcript included, sixty times a
     * second, and the client ran at a few frames a second on one pegged
     * core.  The height is not part of the cap.
     */
    w = MAX(width - used, 200.0f);

    if (w == shell.column_cap)
        return;

    shell.column_cap = w;
    lrg_widget_set_max_size(shell.main_column, w, -1.0f);
}

void
clawt_lrg_select_agent(ClawtLrgApp *app, const gchar *agent_id)
{
    JsonObject *agent;

    if (g_strcmp0(app->agent_id, agent_id) == 0 && agent_id != NULL) {
        clawt_lrg_show_page(app, app->page);
        return;
    }

    g_free(app->agent_id);
    app->agent_id = g_strdup(agent_id);
    close_drawer(app);

    /* an agent's own conversation is its direct room */
    g_clear_pointer(&app->room_id, g_free);
    agent = clawt_lrg_selected_agent(app);

    if (agent != NULL)
        app->room_id = g_strdup(clawt_ipc_payload_string(agent, "dm_room"));

    app->generation++;
    clawt_lrg_sidebar_refresh(app);
    clawt_lrg_show_page(app, app->page);
}

void
clawt_lrg_select_room(ClawtLrgApp *app, const gchar *room_id)
{
    g_free(app->room_id);
    app->room_id = g_strdup(room_id);
    g_clear_pointer(&app->agent_id, g_free);
    close_drawer(app);

    app->generation++;
    clawt_lrg_sidebar_refresh(app);
    clawt_lrg_show_page(app, CLAWT_PAGE_CHAT);
}

/* ── Fleet ────────────────────────────────────────────────────────── */

static void
on_agents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    if (reply == NULL)
        return;

    g_clear_pointer(&app->agents, json_node_unref);
    app->agents = json_node_copy(reply);

    /* first load: open on the first agent rather than an empty page */
    if (app->agent_id == NULL && app->room_id == NULL) {
        JsonObject *root = clawt_ipc_reply_object(reply);
        JsonArray *agents = (root != NULL &&
                             json_object_has_member(root, "agents"))
            ? json_object_get_array_member(root, "agents") : NULL;

        if (agents != NULL && json_array_get_length(agents) > 0) {
            clawt_lrg_select_agent(app, clawt_ipc_payload_string(
                json_array_get_object_element(agents, 0), "id"));
            return;
        }
    }

    clawt_lrg_sidebar_refresh(app);
    update_header(app);
}

static void
on_rooms(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    if (reply == NULL)
        return;

    g_clear_pointer(&app->rooms, json_node_unref);
    app->rooms = json_node_copy(reply);
    clawt_lrg_sidebar_refresh(app);
}

static void
on_activity(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    if (reply != NULL) {
        clawt_lrg_island_fold_activity(&app->island, reply);
        clawt_lrg_update_island(app);
    }
}

static void
on_usage(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonObject *total = (root != NULL && json_object_has_member(root, "total"))
        ? json_object_get_object_member(root, "total") : NULL;

    app->island.spend_micros =
        (total != NULL) ? clawt_ipc_payload_int(total, "cost_micros", -1) : -1;
    clawt_lrg_update_island(app);
}

static void
on_decisions(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *decisions = (root != NULL &&
                            json_object_has_member(root, "decisions"))
        ? json_object_get_array_member(root, "decisions") : NULL;

    app->decisions_open = (decisions != NULL)
        ? json_array_get_length(decisions) : 0;
    app->island.decisions = app->decisions_open;
    clawt_lrg_set_page_badge(app, CLAWT_PAGE_DECISIONS, app->decisions_open,
                             "warn");
    clawt_lrg_update_island(app);
}

/*
 * The Grok page's badge: the daemon's count of jobs that need somebody,
 * read whole rather than tallied from events, so it goes down as well as
 * up and a missed event cannot leave it wrong.
 */
static void
on_grok_jobs(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    (void)error;
    (void)user_data;

    if (root == NULL)
        return;

    clawt_lrg_set_page_badge(app, CLAWT_PAGE_GROK,
                             (guint)clawt_ipc_payload_int(root, "attention",
                                                          0),
                             "bad");
}

/* `usage.summary` since local midnight, in unix seconds as it wants. */
static JsonNode *
usage_today_payload(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autoptr(GDateTime) midnight = g_date_time_new_local(
        g_date_time_get_year(now), g_date_time_get_month(now),
        g_date_time_get_day_of_month(now), 0, 0, 0.0);
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, g_date_time_to_unix(midnight));
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

void
clawt_lrg_refresh_fleet(ClawtLrgApp *app)
{
    clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET, on_agents,
                      NULL, NULL);
    clawt_lrg_request(app, "room.list", NULL, CLAWT_LRG_QUIET, on_rooms,
                      NULL, NULL);
    clawt_lrg_request(app, "activity.list", NULL, CLAWT_LRG_QUIET,
                      on_activity, NULL, NULL);
    clawt_lrg_request(app, "decision.list", NULL, CLAWT_LRG_QUIET,
                      on_decisions, NULL, NULL);
    clawt_lrg_request(app, "grok.jobs", NULL, CLAWT_LRG_QUIET,
                      on_grok_jobs, NULL, NULL);
    clawt_lrg_request(app, "usage.summary",
                      usage_today_payload(),
                      CLAWT_LRG_QUIET, on_usage, NULL, NULL);
    clawt_lrg_hold_refresh(app);
}

/* ── Island and banner ────────────────────────────────────────────── */

void
clawt_lrg_update_island(ClawtLrgApp *app)
{
    g_autofree gchar *text = NULL;
    guint unread = 0;
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, app->unread);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        unread += GPOINTER_TO_UINT(value);

    app->island.unread = unread;
    app->island.connected = clawt_client_is_connected(app->client);
    app->island.held = app->held;

    text = clawt_lrg_island_text(&app->island);
    lrg_label_set_text(LRG_LABEL(shell.island_compact), text);

    if (clawt_lrg_island_alert(&app->island))
        lrg_widget_add_class(app->island_widget, "clawt-island-alert");
    else
        lrg_widget_remove_class(app->island_widget, "clawt-island-alert");

    clawt_lrg_app_wake(app);
}

static void
on_banner_action(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (app->held && clawt_client_is_connected(app->client))
        clawt_lrg_hold_resume(app);
    else
        clawt_lrg_open_settings(app, "connections");
}

void
clawt_lrg_update_banner(ClawtLrgApp *app)
{
    gboolean connected = clawt_client_is_connected(app->client);
    g_autofree gchar *where = clawt_connection_describe(app->connection);

    if (!connected) {
        g_autofree gchar *text = g_strdup_printf(
            clawt_client_is_reconnecting(app->client)
                ? "Not connected to %s — retrying"
                : "Not connected to %s",
            where);

        clawt_lrg_paragraph_set_text(shell.banner_label, text);
        lrg_button_set_text(LRG_BUTTON(shell.banner_action), "Connections…");
        lrg_widget_add_class(app->banner, "clawt-banner-bad");
        lrg_widget_set_visible(app->banner, TRUE);
    } else if (app->held) {
        clawt_lrg_paragraph_set_text(shell.banner_label,
                           "The fleet is paused: nothing is delivered and no "
                           "routine runs until it is resumed");
        lrg_button_set_text(LRG_BUTTON(shell.banner_action), "Resume");
        lrg_widget_remove_class(app->banner, "clawt-banner-bad");
        lrg_widget_set_visible(app->banner, TRUE);
    } else {
        /*
         * Connected and running: the one thing left to say is about the
         * daemon itself -- a version that differs from this client's, or
         * an update it found.  The sentence and its precedence are the
         * library's (clawt_connection_notice_text()), shared with GTK and
         * the web client, which also hold it in their banners.
         */
        g_autofree gchar *notice = clawt_connection_notice_text(
            CLAWT_DAEMON_LINK_UP, app->connection, app->daemon_version,
            app->daemon_update);

        if (notice != NULL) {
            clawt_lrg_paragraph_set_text(shell.banner_label, notice);
            lrg_button_set_text(LRG_BUTTON(shell.banner_action),
                                "Connections…");
            lrg_widget_remove_class(app->banner, "clawt-banner-bad");
        }

        lrg_widget_set_visible(app->banner, notice != NULL);
    }

    clawt_lrg_app_wake(app);
}

void
clawt_lrg_window_connection_changed(ClawtLrgApp *app)
{
    clawt_lrg_update_banner(app);
    clawt_lrg_update_island(app);

    if (clawt_client_is_connected(app->client)) {
        clawt_lrg_refresh_fleet(app);
        clawt_lrg_show_page(app, app->page);
    }
}

/* ── Events the shell itself cares about ──────────────────────────── */

static gboolean
on_refresh_due(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    guint due = shell.refresh_due;

    shell.refresh_due = 0;

    if (due & REFRESH_AGENTS)
        clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET,
                          on_agents, NULL, NULL);
    if (due & REFRESH_ROOMS)
        clawt_lrg_request(app, "room.list", NULL, CLAWT_LRG_QUIET, on_rooms,
                          NULL, NULL);
    if (due & REFRESH_ACTIVITY)
        clawt_lrg_request(app, "activity.list", NULL, CLAWT_LRG_QUIET,
                          on_activity, NULL, NULL);
    /* both ends of a decision, or the count could only go up */
    if (due & REFRESH_DECISIONS)
        clawt_lrg_request(app, "decision.list", NULL, CLAWT_LRG_QUIET,
                          on_decisions, NULL, NULL);
    if (due & REFRESH_USAGE)
        clawt_lrg_request(app, "usage.summary", usage_today_payload(),
                          CLAWT_LRG_QUIET, on_usage, NULL, NULL);
    if (due & REFRESH_GROK)
        clawt_lrg_request(app, "grok.jobs", NULL, CLAWT_LRG_QUIET,
                          on_grok_jobs, NULL, NULL);

    return G_SOURCE_REMOVE;
}

/*
 * Asks for @kinds to be fetched again within REFRESH_COALESCE_MS, once
 * however many events asked.  The source is the app's (clawt_lrg_app_attach),
 * so it dies with the app; the shell keeps its own reference only to ask
 * whether it is still pending.
 */
static void
refresh_soon(ClawtLrgApp *app, guint kinds)
{
    shell.refresh_due |= kinds;

    if (shell.refresh_source != NULL &&
        !g_source_is_destroyed(shell.refresh_source))
        return;

    g_clear_pointer(&shell.refresh_source, g_source_unref);
    shell.refresh_source = g_timeout_source_new(REFRESH_COALESCE_MS);
    g_source_set_callback(shell.refresh_source, on_refresh_due, app, NULL);
    clawt_lrg_app_attach(app, shell.refresh_source);
}

void
clawt_lrg_window_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (kind == NULL)
        return;

    /* every event is offered to the alerts drawer, which keeps what it should */
    clawt_lrg_alerts_event(app, event);

    if (g_str_has_prefix(kind, "agent.") || g_str_has_prefix(kind, "team.") ||
        g_strcmp0(kind, "fleet.reordered") == 0) {
        if (g_strcmp0(kind, "agent.changed") == 0)
            clawt_lrg_avatar_invalidate(clawt_event_get_subject(event));

        /* typing frames are a level; the sidebar row reads the set */
        if (g_strcmp0(kind, "agent.typing") != 0)
            refresh_soon(app, REFRESH_AGENTS);
    }

    if (g_str_has_prefix(kind, "room."))
        refresh_soon(app, REFRESH_ROOMS);

    if (g_strcmp0(kind, "turn.step") == 0 ||
        g_strcmp0(kind, "agent.typing") == 0 ||
        g_strcmp0(kind, "message") == 0)
        refresh_soon(app, REFRESH_ACTIVITY);

    if (g_str_has_prefix(kind, "decision."))
        refresh_soon(app, REFRESH_DECISIONS);

    /* both ends of a job publish it, so the badge counts down too */
    if (g_str_has_prefix(kind, "grok."))
        refresh_soon(app, REFRESH_GROK);

    if (g_str_has_prefix(kind, "control.") || g_str_has_prefix(kind, "hold"))
        clawt_lrg_hold_refresh(app);

    if (g_strcmp0(kind, "message") == 0)
        refresh_soon(app, REFRESH_USAGE);
}

/* ── Window actions ───────────────────────────────────────────────── */

static void
on_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    if (g_strcmp0(name, "navigate") == 0)
        clawt_lrg_navigation_open(app);
    else if (g_strcmp0(name, "settings") == 0)
        clawt_lrg_open_settings(app, NULL);
    else if (g_strcmp0(name, "quit") == 0)
        app->quit = TRUE;
    else if (g_strcmp0(name, "pause-fleet") == 0)
        clawt_lrg_hold_pause(app);
    else if (g_strcmp0(name, "resume-fleet") == 0)
        clawt_lrg_hold_resume(app);
    else if (g_strcmp0(name, "agent-create") == 0)
        clawt_lrg_new_agent_dialog(app);
    else if (g_strcmp0(name, "agent-import") == 0)
        clawt_lrg_import_agent_dialog(app);
    else if (g_strcmp0(name, "room-create") == 0)
        clawt_lrg_new_room_dialog(app);
    else if (g_strcmp0(name, "connections") == 0)
        clawt_lrg_open_settings(app, "connections");
    else if (g_strcmp0(name, "toggle-sidebar") == 0)
        clawt_lrg_window_toggle_sidebar(app);
}

static GSimpleActionGroup *
build_actions(ClawtLrgApp *app)
{
    static const gchar *const names[] = {
        "navigate", "settings", "quit", "pause-fleet", "resume-fleet",
        "agent-create", "agent-import", "room-create", "connections",
        "toggle-sidebar"
    };
    GSimpleActionGroup *group = g_simple_action_group_new();
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_action), app);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    return group;
}

static GMenuModel *
build_app_menu(void)
{
    GMenu *menu = g_menu_new();
    g_autoptr(GMenu) fleet = g_menu_new();
    g_autoptr(GMenu) app_section = g_menu_new();

    g_menu_append(fleet, "New agent…", "win.agent-create");
    g_menu_append(fleet, "Import an agent…", "win.agent-import");
    g_menu_append(fleet, "New room…", "win.room-create");
    g_menu_append(fleet, "Pause the fleet", "win.pause-fleet");
    g_menu_append(fleet, "Resume the fleet", "win.resume-fleet");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fleet));

    g_menu_append(app_section, "Go to…\tCtrl+K", "win.navigate");
    g_menu_append(app_section, "Show or hide the sidebar\tCtrl+B",
                  "win.toggle-sidebar");
    g_menu_append(app_section, "Connections…", "win.connections");
    g_menu_append(app_section, "Settings…\tCtrl+,", "win.settings");
    g_menu_append(app_section, "Quit\tCtrl+Q", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app_section));

    return G_MENU_MODEL(menu);
}

static void
on_menu_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GMenuModel) model = build_app_menu();

    clawt_lrg_popup_menu(app, LRG_WIDGET(app->canvas), model);
}

static void
on_island_toggled(LrgIsland *island, gboolean expanded, gpointer user_data)
{
    clawt_lrg_app_wake(user_data);
}

/* ── Building ─────────────────────────────────────────────────────── */

static void
on_sidebar_toggle(LrgButton *button, gpointer user_data)
{
    clawt_lrg_window_toggle_sidebar(user_data);
}

static LrgWidget *
build_header(ClawtLrgApp *app)
{
    LrgWidget *header = clawt_lrg_hbox(14.0f);
    LrgWidget *titles = clawt_lrg_vbox(0.0f);
    LrgWidget *tabs = LRG_WIDGET(lrg_wrap_box_new());
    LrgIsland *island = lrg_island_new();
    guint n = clawt_section_count();
    guint s;

    lrg_widget_add_class(header, "clawt-chrome");
    lrg_container_set_padding(LRG_CONTAINER(header), 12.0f);

    /*
     * The sidebar's toggle, at the edge it hides: "‹" folds it away,
     * "›" brings it back (or, in a narrow window, opens it over the page).
     */
    shell.sidebar_toggle = clawt_lrg_button("\xe2\x80\xb9", "flat",
                                            G_CALLBACK(on_sidebar_toggle), app);
    lrg_widget_set_valign(shell.sidebar_toggle, LRG_ALIGN_CENTER);
    clawt_lrg_take(header, shell.sidebar_toggle);

    shell.header_avatar_slot = clawt_lrg_hbox(0.0f);
    lrg_widget_set_valign(shell.header_avatar_slot, LRG_ALIGN_CENTER);
    clawt_lrg_take(header, shell.header_avatar_slot);

    shell.header_title = clawt_lrg_label("clawtilla", "heading");
    shell.header_subtitle = clawt_lrg_label("", "muted");
    /*
     * Cut with "…" rather than drawn on.  The subtitle is the agent's
     * whole description on one line, and a chief of staff's runs past
     * a 1300-pixel window; before libregnum 0.8.6 the header's box gave
     * it its full width anyway, and the island, the alerts and the menu
     * went past the right edge.  The titles are the flexible child, so
     * a header short of room takes it from them, and these say what to
     * draw with less.
     */
    lrg_label_set_ellipsize(LRG_LABEL(shell.header_title), TRUE);
    lrg_label_set_ellipsize(LRG_LABEL(shell.header_subtitle), TRUE);
    clawt_lrg_take(titles, shell.header_title);
    clawt_lrg_take(titles, shell.header_subtitle);
    lrg_widget_set_valign(titles, LRG_ALIGN_CENTER);
    lrg_widget_set_fill_weight(titles, 1.0f);
    clawt_lrg_take(header, titles);

    /* section tabs, walked from the library */
    for (s = 0; s < n; s++) {
        ClawtSection section = clawt_section_nth(s);
        LrgWidget *tab = clawt_lrg_button(clawt_section_nth_label(s), "flat",
                                          G_CALLBACK(on_section_clicked),
                                          GUINT_TO_POINTER(section));

        lrg_widget_add_class(tab, "clawt-tab");
        g_object_set_data(G_OBJECT(tab), "clawt-app", app);
        shell.section_tabs[section] = tab;
        clawt_lrg_add(tabs, tab);
    }

    /*
     * The tabs get a row of their own under the header rather than a
     * place in it: in the header they set its minimum width, and a
     * window narrower than avatar + title + six tabs + island pushed the
     * island off the right edge, where nothing said it had gone.
     */
    shell.section_bar = tabs;

    /*
     * Wrapped, not a row: six tabs beside a 340-pixel sidebar need more
     * than a 900-pixel window has, and a row ran its last tabs off the
     * right edge.  Centred lines, filling the strip so there is a width
     * to wrap at.
     */
    lrg_container_set_spacing(LRG_CONTAINER(tabs), 4.0f);
    lrg_wrap_box_set_line_spacing(LRG_WRAP_BOX(tabs), 4.0f);
    lrg_wrap_box_set_justify(LRG_WRAP_BOX(tabs), LRG_WRAP_JUSTIFY_CENTER);
    lrg_widget_set_halign(tabs, LRG_ALIGN_FILL);

    /* the island: one line compact, the activity summary expanded */
    shell.island_compact = clawt_lrg_label("Connecting…", NULL);
    lrg_widget_add_class(shell.island_compact, "clawt-island");
    shell.island_expanded = clawt_lrg_vbox(6.0f);
    lrg_container_set_padding(LRG_CONTAINER(shell.island_expanded), 6.0f);
    lrg_island_set_compact_child(island, shell.island_compact);
    lrg_island_set_expanded_child(island, shell.island_expanded);

    /* the island holds them now; the shell keeps borrowed pointers */
    g_object_unref(shell.island_compact);
    g_object_unref(shell.island_expanded);
    lrg_island_set_toggle_on_click(island, TRUE);
    lrg_widget_add_class(LRG_WIDGET(island), "clawt-island");
    lrg_widget_set_valign(LRG_WIDGET(island), LRG_ALIGN_CENTER);
    g_signal_connect(island, "toggled", G_CALLBACK(on_island_toggled), app);
    app->island_widget = LRG_WIDGET(island);
    clawt_lrg_take(header, LRG_WIDGET(island));

    clawt_lrg_take(header, clawt_lrg_alerts_bell(app));
    clawt_lrg_take(header, clawt_lrg_button("☰", "flat",
                                            G_CALLBACK(on_menu_clicked), app));

    return header;
}

LrgWidget *
clawt_lrg_window_build(ClawtLrgApp *app)
{
    LrgWidget *root = clawt_lrg_hbox(0.0f);
    LrgWidget *main_column = clawt_lrg_vbox(0.0f);
    LrgWidget *banner = clawt_lrg_hbox(12.0f);
    LrgSwitcher *switcher = lrg_switcher_new();
    guint n = clawt_page_count();
    guint i;

    /* a second window in one process (the tests build several) */
    for (i = 0; i < N_PAGES; i++)
        g_free(shell.page_badge_tones[i]);

    g_clear_object(&shell.actions);
    if (shell.refresh_source != NULL)
        g_source_destroy(shell.refresh_source);
    g_clear_pointer(&shell.refresh_source, g_source_unref);
    memset(&shell, 0, sizeof(shell));
    clawt_lrg_alerts_reset();

    shell.actions = build_actions(app);
    lrg_widget_insert_action_group(LRG_WIDGET(app->canvas), "win",
                                   G_ACTION_GROUP(shell.actions));

    /* the fleet */
    app->sidebar = clawt_lrg_sidebar_build(app);
    lrg_widget_set_min_size(app->sidebar, 340.0f * (gfloat)app->scale, 0.0f);
    lrg_widget_set_max_size(app->sidebar, 340.0f * (gfloat)app->scale, -1.0f);
    clawt_lrg_take(root, app->sidebar);
    shell.root = root;

    clawt_lrg_take(main_column, build_header(app));

    /* the section tabs, centred, under the header (see build_header()) */
    {
        LrgWidget *strip = clawt_lrg_hbox(0.0f);

        lrg_container_set_padding(LRG_CONTAINER(strip), 8.0f);
        clawt_lrg_add(strip, shell.section_bar);
        lrg_widget_set_fill_weight(shell.section_bar, 1.0f);
        g_object_unref(shell.section_bar);
        clawt_lrg_take(main_column, strip);
    }

    /* the inner tab row for multi-page sections */
    /* wrapped for the same reason as the section tabs */
    shell.page_bar = LRG_WIDGET(lrg_wrap_box_new());
    lrg_container_set_spacing(LRG_CONTAINER(shell.page_bar), 4.0f);
    lrg_wrap_box_set_line_spacing(LRG_WRAP_BOX(shell.page_bar), 4.0f);
    lrg_wrap_box_set_justify(LRG_WRAP_BOX(shell.page_bar),
                             LRG_WRAP_JUSTIFY_CENTER);
    lrg_container_set_padding(LRG_CONTAINER(shell.page_bar), 4.0f);
    lrg_widget_set_halign(shell.page_bar, LRG_ALIGN_FILL);
    lrg_widget_set_visible(shell.page_bar, FALSE);

    for (i = 0; i < n && i < N_PAGES; i++) {
        LrgWidget *tab = clawt_lrg_button(clawt_page_label((ClawtPage)i),
                                          "flat", G_CALLBACK(on_page_clicked),
                                          GUINT_TO_POINTER(i));

        lrg_widget_add_class(tab, "clawt-tab");
        g_object_set_data(G_OBJECT(tab), "clawt-app", app);
        shell.page_tabs[i] = tab;
    }

    clawt_lrg_take(main_column, shell.page_bar);

    /* the banner: disconnected, or paused */
    lrg_widget_add_class(banner, "clawt-banner");
    lrg_container_set_padding(LRG_CONTAINER(banner), 12.0f);
    {
        LrgInsets margin = { 16.0f, 10.0f, 16.0f, 0.0f };

        lrg_widget_set_margin(banner, &margin);
    }
    /*
     * A paragraph, not a one-line label: the version notice is a whole
     * sentence ("This daemon is clawtilla 0.7.3 and this client is
     * 0.7.4. The daemon is older, so...") and as one line it ran under
     * the Connections button and off the window.  It wraps beside the
     * button instead, being the banner's flexible child.
     */
    shell.banner_label = clawt_lrg_paragraph("", NULL);
    lrg_widget_set_fill_weight(shell.banner_label, 1.0f);
    lrg_widget_set_valign(shell.banner_label, LRG_ALIGN_CENTER);
    shell.banner_action = clawt_lrg_button("", NULL,
                                           G_CALLBACK(on_banner_action), app);
    clawt_lrg_take(banner, shell.banner_label);
    clawt_lrg_take(banner, shell.banner_action);
    lrg_widget_set_visible(banner, FALSE);
    app->banner = banner;
    clawt_lrg_take(main_column, banner);

    /* the pages */
    lrg_switcher_set_transition(switcher, LRG_SWITCHER_TRANSITION_CROSSFADE);
    lrg_widget_set_fill_weight(LRG_WIDGET(switcher), 1.0f);
    app->switcher = LRG_WIDGET(switcher);
    clawt_lrg_take(main_column, LRG_WIDGET(switcher));

    lrg_widget_set_fill_weight(main_column, 1.0f);
    shell.main_column = main_column;
    clawt_lrg_take(root, main_column);

    /* the alerts drawer, on the right, closed until the bell opens it */
    shell.alerts_drawer = build_alerts_panel(app);
    clawt_lrg_take(root, shell.alerts_drawer);

    clawt_lrg_show_page(app, CLAWT_PAGE_CHAT);
    clawt_lrg_update_banner(app);

    /* as the person left it, from appearance.yaml */
    place_sidebar(app);

    return root;
}
