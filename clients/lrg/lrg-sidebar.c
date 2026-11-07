/*
 * lrg-sidebar.c - The fleet sidebar
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The fleet as the daemon groups it, the unread counts beside it, the
 * drag that reorders it and the menu a right-click opens -- the libregnum
 * half of clients/gtk/gtk-sidebar.c.  What the rows *say* and the order
 * they come in are decided in lrg-chat-model.c; this file draws that and
 * wires the gestures.
 *
 * It reads app->agents and app->rooms, which the shell keeps current, and
 * asks for team.list itself: the agent listing names a team by id and
 * nothing more.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-chat-model.h"

#include <string.h>

/* The drag payload type: "a:<agent>" or "r:<room>". */
#define ROW_DRAG_TYPE "application/x-clawtilla-fleet-row"

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *root;
    LrgWidget   *subtitle;     /* which daemon, under the brand */
    LrgWidget   *list;         /* the rows, rebuilt on every refresh */
    JsonNode    *teams;        /* team.list, kept for the menus */
    gchar       *teams_text;   /* its serialisation, to skip no-op redraws */
    GHashTable  *collapsed;    /* folded team ids; a view preference */
    GHashTable  *seen;         /* tokens drawn last time, for entrances */
    GPtrArray   *entries;      /* what the list was built from */
    gboolean     teams_asked;
    ClawtPage    page_seen;    /* for the Chat tab's badge */
} Sidebar;

static Sidebar sidebar;

static void rebuild(ClawtLrgApp *app);

/* ── A row that can be clicked ────────────────────────────────────── */

/*
 * libregnum's boxes take no clicks of their own, so a clickable row is a
 * surface holding the content and, over it, an invisible custom widget
 * that turns a press-and-release inside it into a click.  The surface is
 * what carries clawt-row: the pointer is over the catcher, which makes
 * the surface an ancestor on the hover path, so the row's hover look
 * still applies.  A right press is left unhandled on purpose, so it
 * bubbles to the surface's context menu.
 */
typedef void (*RowClicked)(ClawtLrgApp *app, gpointer user_data);

typedef struct {
    ClawtLrgApp    *app;
    RowClicked      func;
    gpointer        user_data;
    GDestroyNotify  destroy;
} Clicker;

static void
clicker_free(gpointer data)
{
    Clicker *clicker = data;

    if (clicker->destroy != NULL)
        clicker->destroy(clicker->user_data);

    g_free(clicker);
}

static gboolean
on_catcher_event(LrgCustomWidget *widget, LrgUIEvent *event,
                 gpointer user_data)
{
    Clicker *clicker = user_data;

    if (lrg_ui_event_get_button(event) != GRL_MOUSE_BUTTON_LEFT)
        return FALSE;

    if (lrg_ui_event_get_event_type(event) == LRG_UI_EVENT_MOUSE_BUTTON_DOWN)
        return TRUE;

    /*
     * A release inside is the click; outside is somebody changing their
     * mind -- and how a drag that started here ends, since the canvas
     * hands the press back far outside every widget when it begins.
     */
    if (lrg_ui_event_get_event_type(event) == LRG_UI_EVENT_MOUSE_BUTTON_UP) {
        if (lrg_widget_contains_point(LRG_WIDGET(widget),
                                      lrg_ui_event_get_x(event),
                                      lrg_ui_event_get_y(event)))
            clicker->func(clicker->app, clicker->user_data);

        clawt_lrg_app_wake(clicker->app);
        return TRUE;
    }

    return FALSE;
}

static LrgWidget *
clickable_new(ClawtLrgApp *app, LrgWidget *content, RowClicked func,
              gpointer user_data, GDestroyNotify destroy)
{
    LrgPanel *surface = lrg_panel_new();
    LrgCustomWidget *catcher = lrg_custom_widget_new();
    Clicker *clicker = g_new0(Clicker, 1);

    clicker->app = app;
    clicker->func = func;
    clicker->user_data = user_data;
    clicker->destroy = destroy;

    lrg_panel_set_fill_children(surface, TRUE);
    lrg_panel_set_elevation(surface, 0);
    lrg_panel_set_border_color(surface, NULL);
    lrg_container_set_padding(LRG_CONTAINER(surface), 0.0f);
    lrg_widget_add_class(LRG_WIDGET(surface), "clawt-row");

    /* the content keeps its layout and loses the class to the surface */
    lrg_widget_remove_class(content, "clawt-row");
    clawt_lrg_take(LRG_WIDGET(surface), content);

    lrg_custom_widget_set_paint_box(catcher, FALSE);
    g_signal_connect_data(catcher, "event", G_CALLBACK(on_catcher_event),
                          clicker,
                          (GClosureNotify)(void (*)(void))clicker_free, 0);
    clawt_lrg_take(LRG_WIDGET(surface), LRG_WIDGET(catcher));

    return LRG_WIDGET(surface);
}

/* ── Unread, as the sidebar draws it ──────────────────────────────── */

/*
 * The filled pill: filled means "for you", where every other mark in the
 * row is a coloured caption about the agent.  No 99+ cap -- at caption
 * size "99+" is wider than the number it would replace.
 */
static LrgWidget *
unread_pill(guint count)
{
    g_autofree gchar *text = g_strdup_printf("%u", count);
    LrgPanel *pill = lrg_panel_new();
    LrgWidget *label = clawt_lrg_label(text, "caption");
    LrgInsets padding = { 8.0f, 2.0f, 8.0f, 2.0f };

    lrg_panel_set_fill_children(pill, TRUE);
    lrg_panel_set_elevation(pill, 0);
    lrg_container_set_padding(LRG_CONTAINER(pill), 0.0f);
    lrg_widget_add_class(LRG_WIDGET(pill), "clawt-unread");
    lrg_widget_add_class(label, "clawt-unread");
    lrg_widget_set_margin(label, &padding);
    lrg_widget_set_valign(LRG_WIDGET(pill), LRG_ALIGN_CENTER);
    lrg_widget_set_name(LRG_WIDGET(pill), "unread-badge");
    clawt_lrg_take(LRG_WIDGET(pill), label);

    /* a new count arrives with a pop, so it is seen */
    lrg_widget_set_appear(LRG_WIDGET(pill), LRG_APPEAR_POP, 0.0f);

    return LRG_WIDGET(pill);
}

/*
 * The Chat tab's number, and the island's.  Kept in step with what the
 * sidebar can still draw: a count against a room with no row could only
 * ever climb.
 */
static void
update_unread_total(ClawtLrgApp *app)
{
    g_autoptr(GHashTable) rows = NULL;
    guint unread_total;

    if (app->agents != NULL || app->rooms != NULL) {
        rows = clawt_lrg_chat_row_rooms(app->agents, app->rooms);
        clawt_lrg_unread_prune(app->unread, rows);
    }

    unread_total = clawt_lrg_unread_badge(app->unread,
                                          app->page == CLAWT_PAGE_CHAT);
    sidebar.page_seen = app->page;
    clawt_lrg_set_page_badge(app, CLAWT_PAGE_CHAT, unread_total, "info");

    /*
     * The sidebar is built before the header that holds the island, and
     * the island's update writes into it unconditionally.
     */
    if (app->island_widget != NULL)
        clawt_lrg_update_island(app);
}

/*
 * Ticks with the canvas.  The badge is hidden while the chat is showing
 * and nothing tells the sidebar when the page changes, so it notices.
 */
static gboolean
on_ticker_step(LrgCustomWidget *widget, gfloat dt, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (app->page != sidebar.page_seen)
        update_unread_total(app);

    return FALSE;
}

/* ── Lifecycle verbs ──────────────────────────────────────────────── */

typedef struct {
    gchar *agent_id;
    gchar *verb;
} AgentJob;

static AgentJob *
agent_job_new(const gchar *agent_id, const gchar *verb)
{
    AgentJob *job = g_new0(AgentJob, 1);

    job->agent_id = g_strdup(agent_id);
    job->verb = g_strdup(verb);
    return job;
}

static void
agent_job_free(gpointer data)
{
    AgentJob *job = data;

    g_free(job->agent_id);
    g_free(job->verb);
    g_free(job);
}

/*
 * Refreshed whether or not it worked: a start that fails still moves the
 * agent to ERROR, and a sidebar saying "stopped" beside a toast saying
 * why it could not start is two answers on screen at once.
 */
static void
on_lifecycle_reply(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    AgentJob *job = user_data;

    /* a stopped agent's turns all ended, however the frames went */
    if (reply != NULL && g_strcmp0(job->verb, "start") != 0)
        clawt_lrg_typing_forget(app->typing, job->agent_id);

    clawt_lrg_refresh_fleet(app);

    if (g_strcmp0(job->agent_id, app->agent_id) == 0)
        clawt_lrg_show_page(app, app->page);
}

static void
on_interrupt_reply(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    AgentJob *job = user_data;
    g_autofree gchar *said = NULL;

    if (reply == NULL)
        return;

    /* the turn was taken out from under the code that lowers the flag */
    clawt_lrg_typing_forget(app->typing, job->agent_id);
    said = clawt_lrg_chat_interrupt_text(job->agent_id, reply);
    clawt_lrg_toast(app, said);
    clawt_lrg_refresh_fleet(app);
}

/* ── Delete, asked twice ──────────────────────────────────────────── */

typedef struct {
    gchar     *agent_id;
    LrgWidget *remove_computer;   /* nullable: none to remove */
    LrgWidget *remove_files;
} DeleteAsk;

static void
delete_ask_free(gpointer data)
{
    DeleteAsk *ask = data;

    g_free(ask->agent_id);
    g_clear_object(&ask->remove_computer);
    g_clear_object(&ask->remove_files);
    g_free(ask);
}

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    const gchar *agent_id = user_data;
    g_autofree gchar *said = NULL;

    if (reply == NULL)
        return;

    said = clawt_lrg_removed_text(agent_id, reply);
    clawt_lrg_toast(app, said);
    g_hash_table_remove(app->typing, agent_id);

    /* nothing on screen may go on describing an agent that is gone */
    if (g_strcmp0(app->agent_id, agent_id) == 0)
        clawt_lrg_select_agent(app, NULL);

    clawt_lrg_refresh_fleet(app);
}

static gboolean
on_delete_confirmed_twice(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                          gpointer user_data)
{
    DeleteAsk *ask = user_data;
    gboolean with_computer = ask->remove_computer != NULL &&
                             clawt_lrg_switch_active(ask->remove_computer);
    gboolean purge = clawt_lrg_switch_active(ask->remove_files);

    clawt_lrg_request(app, "agent.remove",
                      clawt_ipc_payload_new(
                          "agent", ask->agent_id,
                          "remove_computer", with_computer ? "true" : NULL,
                          "remove_files", purge ? "true" : NULL, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed,
                      g_strdup(ask->agent_id), g_free);
    return TRUE;
}

static void
on_delete_confirmed_once(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *agent_id = user_data;
    JsonObject *agent = clawt_lrg_find_agent(app, agent_id);
    const gchar *computer = clawt_ipc_payload_string(agent, "computer");
    DeleteAsk *ask = g_new0(DeleteAsk, 1);
    LrgWidget *content = clawt_lrg_vbox(10.0f);

    ask->agent_id = g_strdup(agent_id);

    /*
     * The container or VM is a separate decision, off by default: it may
     * hold work that was never anywhere else.  Removing the agent used to
     * leave it running under a name derived from an agent that no longer
     * existed.
     */
    if (g_strcmp0(computer, "container") == 0 ||
        g_strcmp0(computer, "vm") == 0) {
        g_autofree gchar *label = g_strdup_printf(
            "Also delete its %s, clawt-%s",
            g_strcmp0(computer, "vm") == 0 ? "virtual machine" : "container",
            agent_id);

        ask->remove_computer = clawt_lrg_switch(label, FALSE);
        clawt_lrg_add(content, ask->remove_computer);
    }

    /* and the files, the half with no undo, worded without euphemism */
    ask->remove_files = clawt_lrg_switch(
        "Also delete everything it owns: its persona, notes, mailbox, "
        "memories, transcripts and credentials", FALSE);
    clawt_lrg_add(content, ask->remove_files);

    g_object_unref(clawt_lrg_dialog(app, "ARE YOU REALLY SURE!?",
                     "This removes the agent from the fleet and stops it if "
                     "it is running.\n\nIts mailbox and transcripts stay on "
                     "disk -- removing an agent is reversible, deleting its "
                     "history is not.",
                     content, "Delete it for good", TRUE,
                     on_delete_confirmed_twice, ask, delete_ask_free));
    g_object_unref(content);
}

static void
ask_delete(ClawtLrgApp *app, const gchar *agent_id)
{
    g_autofree gchar *body = g_strdup_printf("Delete %s?", agent_id);

    /*
     * Twice, the second louder: removing an agent takes its process and
     * its place in every room, and one click between browsing and gone
     * is too few.
     */
    clawt_lrg_confirm(app, "Are you sure?", body, "Delete", TRUE,
                      on_delete_confirmed_once, g_strdup(agent_id), g_free);
}

/* ── The machine's own verbs ──────────────────────────────────────── */

static void
on_power_reply(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    AgentJob *job = user_data;

    if (reply != NULL) {
        g_autofree gchar *said = clawt_lrg_power_text(job->agent_id, reply);

        clawt_lrg_toast(app, said);
    }

    /* a stop that failed may still have left the machine somewhere new */
    clawt_lrg_refresh_fleet(app);

    if (g_strcmp0(job->agent_id, app->agent_id) == 0)
        clawt_lrg_show_page(app, app->page);
}

/*
 * `remove` as a JSON boolean: the daemon refuses a stop that destroys the
 * machine unless told to go ahead, and the string "true" would arrive as
 * the boolean only through the lenient reader.  Sent because the asking
 * has already happened by the time this runs.
 */
static JsonNode *
power_payload(const gchar *agent_id)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent_id);
    json_builder_set_member_name(builder, "remove");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
send_power(ClawtLrgApp *app, gpointer user_data)
{
    AgentJob *job = user_data;
    AgentJob *copy = agent_job_new(job->agent_id, job->verb);

    /*
     * Spelled out per verb: a kind built with g_strconcat() is invisible
     * to `make parity`.
     */
    if (g_strcmp0(job->verb, "start") == 0)
        clawt_lrg_request(app, "computer.start", power_payload(job->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_power_reply, copy,
                          agent_job_free);
    else if (g_strcmp0(job->verb, "stop") == 0)
        clawt_lrg_request(app, "computer.stop", power_payload(job->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_power_reply, copy,
                          agent_job_free);
    else
        clawt_lrg_request(app, "computer.restart",
                          power_payload(job->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_power_reply, copy,
                          agent_job_free);
}

static void
power_machine(ClawtLrgApp *app, const gchar *agent_id, const gchar *verb)
{
    JsonObject *agent = clawt_lrg_find_agent(app, agent_id);
    AgentJob *job = agent_job_new(agent_id, verb);

    /*
     * A container that does not survive a stop makes Stop mean
     * stop-and-delete, which nobody reads it as -- so it is asked.  The
     * daemon refuses it too without the flag; the fence protects clients
     * that do not know to ask, the question stops the fence being an
     * error message to decode.
     */
    if (g_strcmp0(verb, "start") != 0 &&
        clawt_ipc_payload_boolean(agent, "computer_stop_removes", FALSE)) {
        g_autofree gchar *title = clawt_lrg_power_confirm_title(verb);
        g_autofree gchar *body = clawt_lrg_power_confirm_body(agent_id);

        clawt_lrg_confirm(app, title, body,
                          g_strcmp0(verb, "stop") == 0 ? "Stop" : "Restart",
                          TRUE, send_power, job, agent_job_free);
        return;
    }

    send_power(app, job);
    agent_job_free(job);
}

void
clawt_lrg_agent_action(ClawtLrgApp *app, const gchar *agent_id,
                       const gchar *kind)
{
    g_return_if_fail(app != NULL);

    if (agent_id == NULL || kind == NULL)
        return;

    /*
     * Each kind written out as its own literal, so `make parity` sees
     * every frame this sends; the inspector's buttons and the sidebar's
     * menu both come through here.
     */
    if (g_strcmp0(kind, "agent.start") == 0)
        clawt_lrg_request(app, "agent.start",
                          clawt_ipc_payload_new("agent", agent_id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_reply,
                          agent_job_new(agent_id, "start"), agent_job_free);
    else if (g_strcmp0(kind, "agent.stop") == 0)
        clawt_lrg_request(app, "agent.stop",
                          clawt_ipc_payload_new("agent", agent_id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_reply,
                          agent_job_new(agent_id, "stop"), agent_job_free);
    else if (g_strcmp0(kind, "agent.restart") == 0)
        clawt_lrg_request(app, "agent.restart",
                          clawt_ipc_payload_new("agent", agent_id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_reply,
                          agent_job_new(agent_id, "restart"), agent_job_free);
    else if (g_strcmp0(kind, "agent.interrupt") == 0)
        clawt_lrg_request(app, "agent.interrupt",
                          clawt_ipc_payload_new("agent", agent_id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_interrupt_reply,
                          agent_job_new(agent_id, "interrupt"),
                          agent_job_free);
    else if (g_strcmp0(kind, "agent.remove") == 0 ||
             g_strcmp0(kind, "delete") == 0)
        ask_delete(app, agent_id);
    else if (g_strcmp0(kind, "computer.start") == 0)
        power_machine(app, agent_id, "start");
    else if (g_strcmp0(kind, "computer.stop") == 0)
        power_machine(app, agent_id, "stop");
    else if (g_strcmp0(kind, "computer.restart") == 0)
        power_machine(app, agent_id, "restart");
    else
        g_warning("clawt_lrg_agent_action: no such action %s", kind);
}

/* ── Moving things between teams ──────────────────────────────────── */

/*
 * A team change and then, maybe, a reorder.  The team first because it
 * is the coarser of the two: an agent in the right team at the wrong
 * position is a better failure than the reverse.  The reorder is sent
 * only after the team was accepted, so a refusal leaves the arrangement
 * untouched instead of half applied.
 */
typedef struct {
    gchar    *who;          /* agent or room id */
    gboolean  is_room;
    gchar    *team;
    gchar    *team_label;
    gchar    *then_order;   /* nullable: a fleet.reorder to send after */
} MoveJob;

static void
move_job_free(gpointer data)
{
    MoveJob *job = data;

    g_free(job->who);
    g_free(job->team);
    g_free(job->team_label);
    g_free(job->then_order);
    g_free(job);
}

static void
on_reordered(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    clawt_lrg_refresh_fleet(app);
}

static void
send_reorder(ClawtLrgApp *app, const gchar *order)
{
    /*
     * The whole list rather than "move this one here": the daemon numbers
     * them from what it is given, so one frame describes the arrangement
     * completely and a stale view cannot produce a half-applied reorder.
     */
    clawt_lrg_request(app, "fleet.reorder",
                      clawt_ipc_payload_new("entries", order, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_reordered, NULL, NULL);
}

static void
on_moved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    MoveJob *job = user_data;
    g_autofree gchar *said = NULL;

    if (reply == NULL) {
        clawt_lrg_refresh_fleet(app);
        return;
    }

    said = clawt_lrg_moved_text(job->who, job->team, job->team_label);
    clawt_lrg_toast(app, said);

    if (job->then_order != NULL)
        send_reorder(app, job->then_order);
    else
        clawt_lrg_refresh_fleet(app);
}

static void
move_to_team(ClawtLrgApp *app, const gchar *who, gboolean is_room,
             const gchar *team, const gchar *team_label,
             const gchar *then_order)
{
    MoveJob *job = g_new0(MoveJob, 1);

    job->who = g_strdup(who);
    job->is_room = is_room;
    job->team = g_strdup(team != NULL ? team : "");
    job->team_label = g_strdup(team_label);
    job->then_order = g_strdup(then_order);

    /*
     * A room's team is presentation and nothing else: it changes neither
     * who is in the room nor who a message reaches.
     */
    if (is_room)
        clawt_lrg_request(app, "room.set",
                          clawt_ipc_payload_new("room", who,
                                                "team", job->team, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_moved, job,
                          move_job_free);
    else
        clawt_lrg_request(app, "agent.set",
                          clawt_ipc_payload_new("agent", who, "key", "team",
                                                "value", job->team, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_moved, job,
                          move_job_free);
}

/* ── Drag and drop ────────────────────────────────────────────────── */

/*
 * The token travels, never the widget: a refresh can rebuild every row
 * mid-drag, and an id survives that where a pointer does not.
 */
static LrgDragPayload *
on_drag_prepare(LrgDragSource *source, gfloat x, gfloat y, gpointer user_data)
{
    const gchar *token = user_data;
    LrgDragPayload *payload = lrg_drag_payload_new_string(
        ROW_DRAG_TYPE, token, LRG_DRAG_ACTION_MOVE);

    lrg_drag_payload_set_label(payload, token + 2);
    return payload;
}

static void
on_drag_begin(LrgDragSource *source, LrgDragPayload *payload,
              gpointer user_data)
{
    /* faded, so it is obvious which row is in flight */
    lrg_widget_set_opacity(user_data, 0.4f);
}

static void
on_drag_end(LrgDragSource *source, LrgDragPayload *payload, gboolean dropped,
            LrgDragAction action, gpointer user_data)
{
    lrg_widget_set_opacity(user_data, 1.0f);
}

/*
 * Dropping on a team's heading puts the row on that team -- the team
 * only; a heading names a group, not a position, and the group may be
 * folded with nothing on screen to show where it went.
 */
static gboolean
on_header_drop(LrgDropTarget *target, LrgDragPayload *payload, gfloat x,
               gfloat y, gpointer user_data)
{
    ClawtLrgApp *app = sidebar.app;
    LrgWidget *header = user_data;
    const gchar *token = lrg_drag_payload_get_string(payload);
    const gchar *team = g_object_get_data(G_OBJECT(header), "clawt-team");
    const gchar *label = g_object_get_data(G_OBJECT(header),
                                           "clawt-team-label");
    const gchar *was;

    if (token == NULL || team == NULL || strlen(token) < 3)
        return FALSE;

    /*
     * Already there: accepted, not refused -- the chip sliding back reads
     * as "that did not work", and it did; there was nothing to do.
     */
    was = clawt_lrg_entry_team_of(sidebar.entries, token);

    if (g_strcmp0(was != NULL ? was : "", team) == 0)
        return TRUE;

    move_to_team(app, token + 2, g_str_has_prefix(token, "r:"), team, label,
                 NULL);
    return TRUE;
}

static gboolean
on_row_drop(LrgDropTarget *target, LrgDragPayload *payload, gfloat x,
            gfloat y, gpointer user_data)
{
    ClawtLrgApp *app = sidebar.app;
    LrgWidget *onto = user_data;
    const gchar *token = lrg_drag_payload_get_string(payload);
    const gchar *onto_token = g_object_get_data(G_OBJECT(onto), "clawt-token");
    const gchar *onto_team = g_object_get_data(G_OBJECT(onto), "clawt-team");
    const gchar *from_team;
    g_autofree gchar *order = NULL;
    gboolean after;

    if (token == NULL || onto_token == NULL || strlen(token) < 3)
        return FALSE;

    /*
     * Above or below, by which half was dropped on -- without it a row
     * could never be placed last.
     */
    after = y > lrg_widget_get_world_y(onto) +
                lrg_widget_get_height(onto) / 2.0f;

    order = clawt_lrg_reorder_entries(sidebar.entries, token, onto_token,
                                      after);

    if (order == NULL)
        return FALSE;

    /*
     * Dropped among another team's rows, so it joins that team: the
     * daemon returns the fleet grouped, and a reorder alone would be
     * sorted straight back under its own heading -- a drop that visibly
     * worked and then reverted.
     */
    from_team = clawt_lrg_entry_team_of(sidebar.entries, token);

    if (g_strcmp0(from_team != NULL ? from_team : "",
                  onto_team != NULL ? onto_team : "") != 0) {
        move_to_team(app, token + 2, g_str_has_prefix(token, "r:"),
                     onto_team,
                     clawt_lrg_team_name(sidebar.teams, onto_team), order);
        return TRUE;
    }

    send_reorder(app, order);
    return TRUE;
}

static void
make_draggable(LrgWidget *row, const gchar *token)
{
    static const gchar *const types[] = { ROW_DRAG_TYPE, NULL };
    LrgDragSource *source = lrg_drag_source_new();
    LrgDropTarget *target = lrg_drop_target_new(types, LRG_DRAG_ACTION_MOVE);

    /* both halves on every row: anything picked up, anything dropped on */
    g_object_set_data_full(G_OBJECT(row), "clawt-token", g_strdup(token),
                           g_free);
    g_signal_connect_data(source, "prepare", G_CALLBACK(on_drag_prepare),
                          g_strdup(token),
                          (GClosureNotify)(void (*)(void))g_free, 0);
    g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), row);
    g_signal_connect(source, "drag-end", G_CALLBACK(on_drag_end), row);
    lrg_widget_add_drag_source(row, source);

    g_signal_connect(target, "drop", G_CALLBACK(on_row_drop), row);
    lrg_widget_add_drop_target(row, target);
}

/* ── The context menu ─────────────────────────────────────────────── */

/*
 * One action group per row, holding the row's agent: the menu acts on
 * what was right-clicked, whatever happens to be selected, and a refresh
 * rebuilding the sidebar under an open menu cannot redirect it.
 */
typedef struct {
    ClawtLrgApp *app;
    gchar       *agent_id;
} MenuTarget;

static void
menu_target_free(gpointer data)
{
    MenuTarget *target = data;

    g_free(target->agent_id);
    g_free(target);
}

static void
on_menu_lifecycle(GSimpleAction *action, GVariant *parameter,
                  gpointer user_data)
{
    MenuTarget *target = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    if (g_strcmp0(name, "start") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id, "agent.start");
    else if (g_strcmp0(name, "stop") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id, "agent.stop");
    else if (g_strcmp0(name, "restart") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id,
                               "agent.restart");
    else if (g_strcmp0(name, "delete") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id, "agent.remove");
    else if (g_strcmp0(name, "computer-start") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id,
                               "computer.start");
    else if (g_strcmp0(name, "computer-stop") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id,
                               "computer.stop");
    else if (g_strcmp0(name, "computer-restart") == 0)
        clawt_lrg_agent_action(target->app, target->agent_id,
                               "computer.restart");
}

static void
on_menu_team(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MenuTarget *target = user_data;
    const gchar *team = g_variant_get_string(parameter, NULL);
    g_autoptr(GVariant) state = g_action_get_state(G_ACTION(action));

    /*
     * The item already ticked is an ordinary thing to choose from a radio
     * menu, and writing it back would render every agent and toast about
     * a move that did not happen.
     */
    if (state != NULL && g_strcmp0(g_variant_get_string(state, NULL),
                                   team) == 0)
        return;

    g_simple_action_set_state(action, g_variant_new_string(team));
    move_to_team(target->app, target->agent_id, FALSE, team,
                 clawt_lrg_team_name(sidebar.teams, team), NULL);
}

static void
attach_agent_menu(ClawtLrgApp *app, LrgWidget *row,
                  const ClawtLrgAgentRow *agent)
{
    static const gchar *const verbs[] = {
        "start", "stop", "restart", "delete",
        "computer-start", "computer-stop", "computer-restart"
    };
    g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) lifecycle = g_menu_new();
    g_autoptr(GMenu) grouping = g_menu_new();
    g_autoptr(GMenu) teams = g_menu_new();
    g_autoptr(GMenu) machine = g_menu_new();
    g_autoptr(GMenu) danger = g_menu_new();
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    MenuTarget *target = g_new0(MenuTarget, 1);
    gsize i;

    target->app = app;
    target->agent_id = g_strdup(agent->id);
    g_object_set_data_full(G_OBJECT(row), "clawt-menu-target", target,
                           menu_target_free);

    for (i = 0; i < G_N_ELEMENTS(verbs); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(verbs[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_lifecycle),
                         target);

        /* greyed out rather than offered to fail, by the inspector's rules */
        if (i < 4)
            g_simple_action_set_enabled(
                action, clawt_lrg_agent_verb_allowed(agent->state, verbs[i]));

        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    /* stateful with a string target: drawn as radios, current ticked */
    {
        g_autoptr(GSimpleAction) action = g_simple_action_new_stateful(
            "team", G_VARIANT_TYPE_STRING, g_variant_new_string(agent->team));

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_team),
                         target);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    g_menu_append(lifecycle, "Start", "agent.start");
    g_menu_append(lifecycle, "Stop", "agent.stop");
    g_menu_append(lifecycle, "Restart", "agent.restart");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(lifecycle));

    clawt_lrg_team_choices(sidebar.teams, agent->team, ids, labels);

    for (i = 0; i < ids->len; i++) {
        g_autoptr(GMenuItem) item = g_menu_item_new(
            g_ptr_array_index(labels, i), NULL);

        g_menu_item_set_action_and_target_value(
            item, "agent.team",
            g_variant_new_string(g_ptr_array_index(ids, i)));
        g_menu_append_item(teams, item);
    }

    g_menu_append_submenu(grouping, "Team", G_MENU_MODEL(teams));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(grouping));

    /*
     * The machine's verbs under a submenu that names it, and only when
     * there is a machine: a chat-only agent's menu is exactly what it was
     * without them, and whether a type has one is the daemon's answer
     * (clawt_computer_type_has_machine()), so a backend added later
     * appears here untouched.
     */
    if (agent->machine) {
        g_autoptr(GMenu) power = g_menu_new();
        g_autofree gchar *label = g_strdup_printf("Computer (%s)",
                                                  agent->computer);

        g_menu_append(power, "Start", "agent.computer-start");
        g_menu_append(power, "Stop", "agent.computer-stop");
        g_menu_append(power, "Restart", "agent.computer-restart");
        g_menu_append_submenu(machine, label, G_MENU_MODEL(power));
        g_menu_append_section(menu, NULL, G_MENU_MODEL(machine));
    }

    /* its own section, so Delete is never the neighbour of Restart */
    g_menu_append(danger, "Delete\342\200\246", "agent.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(danger));

    lrg_widget_insert_action_group(row, "agent", G_ACTION_GROUP(group));
    lrg_widget_set_context_menu(row, G_MENU_MODEL(menu));
}

/* ── Rows ─────────────────────────────────────────────────────────── */

static void
on_agent_clicked(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_select_agent(app, user_data);
}

static void
on_room_clicked(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_select_room(app, user_data);
}

static void
on_team_clicked(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *team = user_data;

    /*
     * Folding is a view preference, kept in the client: it belongs to the
     * person at this screen, not to the fleet.
     */
    if (g_hash_table_contains(sidebar.collapsed, team))
        g_hash_table_remove(sidebar.collapsed, team);
    else
        g_hash_table_add(sidebar.collapsed, g_strdup(team));

    rebuild(app);
}

static LrgWidget *
team_header(ClawtLrgApp *app, const ClawtLrgSidebarEntry *entry)
{
    static const gchar *const types[] = { ROW_DRAG_TYPE, NULL };
    LrgWidget *box = clawt_lrg_hbox(8.0f);
    LrgWidget *title;
    LrgWidget *header;
    LrgDropTarget *drop;
    LrgInsets margin = { 0.0f, 10.0f, 0.0f, 0.0f };

    lrg_container_set_padding(LRG_CONTAINER(box), 6.0f);
    clawt_lrg_take(box, clawt_lrg_label(entry->collapsed ? "\342\226\270"
                                                          : "\342\226\276",
                                        "muted"));
    title = clawt_lrg_label(entry->title, "heading");
    lrg_widget_set_fill_weight(title, 1.0f);
    lrg_widget_set_valign(title, LRG_ALIGN_CENTER);
    clawt_lrg_take(box, title);

    /*
     * Running out of total, and how many are working.  A folded team
     * hides exactly the rows that would have shown a turn, so the heading
     * is the only place left to say the team is busy -- hence the
     * shimmer on it as well as on the rows.
     */
    clawt_lrg_take(box, clawt_lrg_badge(entry->subtitle,
                                        entry->running > 0 ? "info"
                                                           : "neutral"));

    header = clickable_new(app, box, on_team_clicked, g_strdup(entry->id),
                           g_free);
    lrg_widget_set_margin(header, &margin);

    if (entry->busy > 0) {
        lrg_widget_add_class(header, "clawt-busy");
        lrg_widget_set_name(header, "clawt-team-busy");
    }

    /*
     * A drop target, so an agent can be dragged onto its team rather than
     * picked from a menu -- and the canvas lights the heading while a
     * drag hovers it, which is what tells anybody that it would take one.
     */
    g_object_set_data_full(G_OBJECT(header), "clawt-team",
                           g_strdup(entry->id), g_free);
    g_object_set_data_full(G_OBJECT(header), "clawt-team-label",
                           g_strdup(entry->title), g_free);
    drop = lrg_drop_target_new(types, LRG_DRAG_ACTION_MOVE);
    g_signal_connect(drop, "drop", G_CALLBACK(on_header_drop), header);
    lrg_widget_add_drop_target(header, drop);

    return header;
}

static LrgWidget *
agent_row(ClawtLrgApp *app, const ClawtLrgSidebarEntry *entry)
{
    g_autoptr(ClawtLrgAgentRow) agent = clawt_lrg_agent_row_new(
        entry->object, app->unread, app->typing,
        clawt_appearance_get_show_descriptions(app->appearance));
    LrgWidget *content = clawt_lrg_row(agent->name, agent->subtitle);
    LrgWidget *row;
    g_autofree gchar *token = clawt_lrg_entry_token(entry);

    lrg_container_set_padding(LRG_CONTAINER(content), 8.0f);

    /*
     * What the sidebar's 312 leaves after the face and the state and
     * unread badges, so a long description wraps under the name rather
     * than pushing the badges off the edge.
     */
    clawt_lrg_row_set_text_width(content, 160.0f * (gfloat)app->scale);

    /*
     * The face, so picking an agent out starts with recognising it rather
     * than reading; it glows in its own colour while its process is up.
     */
    clawt_lrg_row_add_prefix(content,
                             clawt_lrg_avatar(app, agent->name, agent->id,
                                              agent->has_avatar, agent->color,
                                              34.0f * (gfloat)app->scale,
                                              agent->live));

    /*
     * The unread count, and not the mailbox depth: the depth is work
     * waiting for the agent, close to the opposite of word waiting for
     * you -- which is in the subtitle instead.
     */
    if (agent->unread > 0)
        clawt_lrg_row_add_suffix(content, unread_pill(agent->unread));

    /* an agent that can run commands on this machine looks different */
    if (agent->host)
        clawt_lrg_row_add_suffix(content, clawt_lrg_badge("HOST", "bad"));

    /*
     * Which standing, from the library, and never both.  No `default:`,
     * so a standing added later is a -Wswitch warning here rather than an
     * agent silently going unmarked.
     */
    switch (agent->standing) {
    case CLAWT_TEAM_BADGE_CHIEF:
        clawt_lrg_row_add_suffix(content, clawt_lrg_badge("CHIEF", "info"));
        break;
    case CLAWT_TEAM_BADGE_LEAD:
        clawt_lrg_row_add_suffix(content, clawt_lrg_badge("LEAD", "good"));
        break;
    case CLAWT_TEAM_BADGE_NONE:
        break;
    }

    /*
     * The state in words when it is anything but running: the glow is a
     * colour, and a colour is never the only signal.
     */
    if (!agent->live)
        clawt_lrg_row_add_suffix(content,
                                 clawt_lrg_badge(agent->state,
                                                 agent->state_tone));

    row = clickable_new(app, content, on_agent_clicked, g_strdup(agent->id),
                        g_free);
    lrg_widget_set_name(row, "clawt-agent-row");
    g_object_set_data_full(G_OBJECT(row), "clawt-team", g_strdup(agent->team),
                           g_free);

    if (agent->unread > 0)
        lrg_widget_add_class(row, "heading");

    /* a turn running: movement, which nobody has to learn */
    if (agent->busy)
        lrg_widget_add_class(row, "clawt-busy");

    if (app->agent_id != NULL && g_strcmp0(app->agent_id, agent->id) == 0)
        lrg_widget_add_class(row, "clawt-row-selected");

    make_draggable(row, token);
    attach_agent_menu(app, row, agent);

    return row;
}

static LrgWidget *
room_row(ClawtLrgApp *app, const ClawtLrgSidebarEntry *entry)
{
    LrgWidget *content = clawt_lrg_row(entry->title, entry->subtitle);
    LrgWidget *glyph = clawt_lrg_badge("#", "info");

    clawt_lrg_row_set_text_width(content, 200.0f * (gfloat)app->scale);
    LrgWidget *row;
    g_autofree gchar *token = clawt_lrg_entry_token(entry);
    guint unread = clawt_lrg_unread_count(app->unread, entry->id);

    lrg_container_set_padding(LRG_CONTAINER(content), 8.0f);
    lrg_widget_set_min_size(glyph, 34.0f * (gfloat)app->scale, 0.0f);
    clawt_lrg_row_add_prefix(content, glyph);

    if (unread > 0)
        clawt_lrg_row_add_suffix(content, unread_pill(unread));

    row = clickable_new(app, content, on_room_clicked, g_strdup(entry->id),
                        g_free);
    lrg_widget_set_name(row, "room_row");
    g_object_set_data_full(G_OBJECT(row), "clawt-team", g_strdup(entry->team),
                           g_free);

    if (app->agent_id == NULL && g_strcmp0(app->room_id, entry->id) == 0)
        lrg_widget_add_class(row, "clawt-row-selected");

    make_draggable(row, token);
    return row;
}

static void
rebuild(ClawtLrgApp *app)
{
    g_autoptr(GHashTable) now_seen = g_hash_table_new_full(g_str_hash,
                                                           g_str_equal,
                                                           g_free, NULL);
    guint appeared = 0;
    guint i;

    if (sidebar.list == NULL)
        return;

    g_clear_pointer(&sidebar.entries, g_ptr_array_unref);
    sidebar.entries = clawt_lrg_sidebar_entries(app->agents, sidebar.teams,
                                                app->rooms,
                                                sidebar.collapsed);

    clawt_lrg_clear(sidebar.list);

    for (i = 0; i < sidebar.entries->len; i++) {
        ClawtLrgSidebarEntry *entry = g_ptr_array_index(sidebar.entries, i);
        g_autofree gchar *key = g_strdup_printf("%d:%s", (gint)entry->kind,
                                                entry->id);
        LrgWidget *row = NULL;

        switch (entry->kind) {
        case CLAWT_LRG_ENTRY_TEAM:
            row = team_header(app, entry);
            break;
        case CLAWT_LRG_ENTRY_AGENT:
            row = agent_row(app, entry);
            break;
        case CLAWT_LRG_ENTRY_ROOM:
            row = room_row(app, entry);
            break;
        case CLAWT_LRG_ENTRY_EMPTY:
            row = clawt_lrg_row(entry->title, entry->subtitle);
            break;
        }

        /*
         * Only what is new flies in.  The sidebar is rebuilt on every
         * fleet event, and a list that re-enters each time somebody
         * speaks would never hold still long enough to read.
         */
        if (!g_hash_table_contains(sidebar.seen, key))
            clawt_lrg_appear(app, row, appeared++);

        g_hash_table_add(now_seen, g_steal_pointer(&key));
        clawt_lrg_take(sidebar.list, row);
    }

    g_hash_table_unref(sidebar.seen);
    sidebar.seen = g_hash_table_ref(now_seen);

    update_unread_total(app);
    clawt_lrg_app_wake(app);
}

/* ── Teams ────────────────────────────────────────────────────────── */

static void
on_teams(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    g_autofree gchar *text = NULL;

    sidebar.teams_asked = FALSE;

    /*
     * A daemon that answered nothing leaves the last list in place: a
     * menu offering only "No team" reads as a fleet with no teams, which
     * is a different and wrong statement from "we could not ask".
     */
    if (reply == NULL)
        return;

    text = json_to_string(reply, FALSE);

    if (g_strcmp0(text, sidebar.teams_text) == 0)
        return;

    g_free(sidebar.teams_text);
    sidebar.teams_text = g_steal_pointer(&text);
    g_clear_pointer(&sidebar.teams, json_node_unref);
    sidebar.teams = json_node_copy(reply);
    rebuild(app);
}

/* ── The shell's entry points ─────────────────────────────────────── */

static void
on_new_agent(LrgButton *button, gpointer user_data)
{
    clawt_lrg_new_agent_dialog(user_data);
}

static void
on_new_room(LrgButton *button, gpointer user_data)
{
    clawt_lrg_new_room_dialog(user_data);
}

LrgWidget *
clawt_lrg_sidebar_build(ClawtLrgApp *app)
{
    LrgWidget *root = clawt_lrg_vbox(10.0f);
    LrgWidget *brand = clawt_lrg_vbox(0.0f);
    LrgWidget *actions = clawt_lrg_hbox(8.0f);
    LrgWidget *new_agent;
    LrgWidget *new_room;
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgCustomWidget *ticker = lrg_custom_widget_new();
    g_autofree gchar *where = clawt_connection_describe(app->connection);

    memset(&sidebar, 0, sizeof(sidebar));
    sidebar.app = app;
    sidebar.root = root;
    sidebar.collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              NULL);
    sidebar.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    sidebar.page_seen = app->page;

    lrg_widget_add_class(root, "clawt-chrome");
    lrg_container_set_padding(LRG_CONTAINER(root), 14.0f);

    /* the brand: the product, and which daemon this window is talking to */
    clawt_lrg_take(brand, clawt_lrg_label("clawtilla", "title"));
    sidebar.subtitle = clawt_lrg_label(where, "caption");
    lrg_widget_add_class(sidebar.subtitle, "muted");
    clawt_lrg_add(brand, sidebar.subtitle);
    lrg_widget_set_appear(brand, LRG_APPEAR_FADE, 0.0f);
    clawt_lrg_take(root, brand);

    new_agent = clawt_lrg_button("+ New agent", "primary",
                                 G_CALLBACK(on_new_agent), app);
    new_room = clawt_lrg_button("# New room", "flat",
                                G_CALLBACK(on_new_room), app);
    lrg_widget_set_fill_weight(new_agent, 1.0f);
    lrg_widget_set_fill_weight(new_room, 1.0f);
    clawt_lrg_take(actions, new_agent);
    clawt_lrg_take(actions, new_room);
    clawt_lrg_take(root, actions);

    sidebar.list = clawt_lrg_vbox(2.0f);

    /*
     * The rows get the sidebar's width and no more, so a long description
     * wraps under its name instead of pushing the state badge off the
     * edge: a scroll box hands its child the child's natural width.
     * The sidebar is 340 wide with 14 of padding a side.
     */
    lrg_widget_set_max_size(sidebar.list, (340.0f - 28.0f) * (gfloat)app->scale,
                            -1.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_kinetic(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, sidebar.list);
    lrg_widget_set_fill_weight(LRG_WIDGET(scroll), 1.0f);
    g_object_unref(sidebar.list);
    clawt_lrg_take(root, LRG_WIDGET(scroll));

    /* the badge watcher draws nothing and takes no room */
    lrg_custom_widget_set_paint_box(ticker, FALSE);
    lrg_custom_widget_set_preferred_width(ticker, 0.0f);
    lrg_custom_widget_set_preferred_height(ticker, 0.0f);
    g_signal_connect(ticker, "step", G_CALLBACK(on_ticker_step), app);
    clawt_lrg_take(root, LRG_WIDGET(ticker));

    rebuild(app);
    return root;
}

void
clawt_lrg_sidebar_refresh(ClawtLrgApp *app)
{
    g_autofree gchar *where = NULL;

    if (sidebar.root == NULL)
        return;

    where = clawt_connection_describe(app->connection);
    lrg_label_set_text(LRG_LABEL(sidebar.subtitle), where);

    /*
     * The teams' names come from their own listing, asked alongside every
     * fleet refresh (a team renamed reaches this client as a re-listing
     * of the agents); the rows are drawn now from what was known and
     * again only if the answer changed.
     */
    if (!sidebar.teams_asked && clawt_client_is_connected(app->client)) {
        sidebar.teams_asked = TRUE;
        clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET, on_teams,
                          NULL, NULL);
    }

    rebuild(app);
}
