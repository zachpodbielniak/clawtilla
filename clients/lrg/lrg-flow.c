/*
 * lrg-flow.c - The Flow page: what the agents are saying to each other
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-flow.c -- the conversation list (room.list,
 * most recent first, the operator's own chats only on request) and the
 * open conversation (room.history), drawn the way the chat draws a
 * transcript: runs by sender, day dividers, stamps, the operator's turns
 * as bubbles, a task chip that leads to the task board and a hop count
 * from the second hop on.
 *
 * Above them, the same traffic as a picture: an LrgNodeGraph with an
 * agent per node, round a ring in fleet order and in its own colour, and
 * a wire per direction of traffic labelled with its count.  A wire lit
 * recently glows in its sender's colour and fades over ten minutes; a
 * message arriving makes its wire pulse.  The graph is a view of the same
 * model the list is -- clicking a wire opens that conversation below --
 * and every number on it comes from clawt-lrg-fleet-model.h, tested
 * without a window.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-fleet-model.h"

#include <string.h>

#define ROOM_KEY        "clawt-flow-room"
#define BODY_KEY        "clawt-flow-body"
#define HISTORY_LIMIT   "200"
#define GUTTER          (44.0f)
#define AVATAR          (32.0f)
#define PULSE_MS        (2600)
#define FOLLOW_DELAY_MS (90)
#define FRAME_DELAY_MS  (150)

static struct {
    ClawtLrgApp   *app;
    LrgWidget     *root;
    LrgWidget     *subtitle;
    LrgWidget     *include_user;

    /* the list */
    LrgListView   *list;
    GListStore    *items;
    LrgWidget     *list_empty;
    JsonNode      *rooms;          /* the last room.list this page read */
    gboolean       filling;

    /* the open conversation */
    LrgWidget     *pane;           /* title + transcript */
    LrgWidget     *pane_empty;
    LrgWidget     *flow_title;
    LrgWidget     *flow_count;
    LrgScrollBox  *scroll;
    LrgWidget     *transcript;
    gchar         *flow_room;      /* what is drawn */
    gchar         *wanted;         /* what was asked for last */
    gchar         *wanted_label;
    GHashTable    *flow_shown;     /* message ids already drawn */
    gchar         *flow_run_sender;
    gchar         *flow_run_day;
    gboolean       follow_pending;

    /* the graph */
    LrgNodeGraph  *graph;
    ClawtLrgFlowGraph *model;
    gchar         *graph_signature;
    GHashTable    *fetched;        /* room id -> message count read */
    gboolean       building;
    gboolean       pulsing;
    LrgGraphLink  *pulse_link;     /* weak: the wire pulsing now */
    guint          pulse_generation;
    gboolean       frame_pending;
} state;

static void refresh_flow(ClawtLrgApp *app);
static void rebuild_graph(ClawtLrgApp *app);

/* ── Small helpers ────────────────────────────────────────────────── */

static gboolean
include_user(void)
{
    return clawt_lrg_switch_active(state.include_user);
}

/* The agent's colour as the daemon spells it, in graylib's terms. */
static gboolean
hex_color(const gchar *hex, GrlColor *out)
{
    gdouble rgba[4];
    g_autofree gchar *css = NULL;

    if (hex == NULL || *hex == '\0')
        return FALSE;

    css = g_strdup_printf("@define-color c %s;", hex);

    if (!clawt_palette_color(css, "c", rgba))
        return FALSE;

    out->r = (guint8)(rgba[0] * 255.0);
    out->g = (guint8)(rgba[1] * 255.0);
    out->b = (guint8)(rgba[2] * 255.0);
    out->a = 255;

    return TRUE;
}

/* A one-shot timer on the app's own context, never the ambient one. */
static void
after(ClawtLrgApp *app, guint ms, GSourceFunc func, gpointer data,
      GDestroyNotify destroy)
{
    GSource *source = g_timeout_source_new(ms);

    g_source_set_callback(source, func, data, destroy);
    clawt_lrg_app_attach(app, source);
    g_source_unref(source);
}

/* ── Following the live edge ──────────────────────────────────────── */

static gboolean
at_bottom(void)
{
    gfloat max_y = 0.0f;
    gfloat page = lrg_widget_get_height(LRG_WIDGET(state.scroll));

    lrg_scroll_box_get_max_scroll(state.scroll, NULL, &max_y);

    /* the shared predicate, so "at the bottom" means one thing everywhere */
    return clawt_transcript_is_at_bottom(
        lrg_scroll_box_get_scroll_y(state.scroll), max_y + page, page);
}

/*
 * After a layout, not now: the rows just added have no height until the
 * next frame lays them out, so the range read now is the old one.
 */
static gboolean
on_follow(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    gfloat max_y = 0.0f;

    state.follow_pending = FALSE;
    lrg_scroll_box_get_max_scroll(state.scroll, NULL, &max_y);
    lrg_scroll_box_scroll_to(state.scroll, 0.0f, max_y, TRUE);
    clawt_lrg_app_wake(app);

    return G_SOURCE_REMOVE;
}

static void
follow_queue(ClawtLrgApp *app)
{
    if (state.follow_pending)
        return;

    state.follow_pending = TRUE;
    after(app, FOLLOW_DELAY_MS, on_follow, app, NULL);
    clawt_lrg_app_wake(app);
}

/* ── A message, drawn as the chat draws one ───────────────────────── */

static void
on_copy_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtLrgApp *app = state.app;
    const gchar *body = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));
    ClawtExportFormat format = CLAWT_EXPORT_MARKDOWN;
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * What was said rather than what the label renders: the body
     * travels with the row, and the conversion is the library's.
     */
    if (g_strcmp0(name, "copy-text") == 0)
        format = CLAWT_EXPORT_PLAIN;
    else if (g_strcmp0(name, "copy-org") == 0)
        format = CLAWT_EXPORT_ORG;

    if (format == CLAWT_EXPORT_MARKDOWN) {
        clawt_lrg_copy(app, body);
        clawt_lrg_toast(app, "Message copied");
        return;
    }

    converted = clawt_export_convert(body, format, &error);

    if (converted == NULL) {
        clawt_lrg_toast(app, error->message);
        return;
    }

    clawt_lrg_copy(app, converted);
    clawt_lrg_toast(app, "Message copied");
}

/* Copy, Copy as text, Copy as org -- the chat's menu, on a right click. */
static void
add_message_menu(LrgWidget *row, const gchar *body)
{
    static const gchar *const names[] = {
        "copy-markdown", "copy-text", "copy-org"
    };
    GSimpleActionGroup *group = g_simple_action_group_new();
    g_autoptr(GMenu) menu = g_menu_new();
    gchar *kept = g_strdup(body);
    gsize i;

    g_object_set_data_full(G_OBJECT(row), BODY_KEY, kept, g_free);

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_copy_action),
                         kept);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    g_menu_append(menu, "Copy", "msg.copy-markdown");
    g_menu_append(menu, "Copy as text", "msg.copy-text");
    g_menu_append(menu, "Copy as org", "msg.copy-org");

    lrg_widget_insert_action_group(row, "msg", G_ACTION_GROUP(group));
    lrg_widget_set_context_menu(row, G_MENU_MODEL(menu));
    g_object_unref(group);
}

static void
on_task_chip_clicked(LrgButton *button, gpointer user_data)
{
    /*
     * The task board is where the rest of the story is -- who asked for
     * it, what state it is in, what it returned.
     */
    clawt_lrg_show_page(user_data, CLAWT_PAGE_TASKS);
}

/* The task and the hop count belong to a message, not to a run. */
static void
add_chips(ClawtLrgApp *app, LrgWidget *into, const gchar *task, gint64 depth)
{
    g_autofree gchar *task_chip = clawt_lrg_flow_task_chip(task);
    g_autofree gchar *hop_chip = clawt_lrg_flow_hop_chip(depth);

    if (task_chip != NULL) {
        LrgWidget *button = clawt_lrg_button(task_chip, "flat",
                                             G_CALLBACK(on_task_chip_clicked),
                                             app);

        lrg_widget_set_valign(button, LRG_ALIGN_CENTER);
        clawt_lrg_take(into, button);
    }

    /*
     * A count that keeps climbing is a loop, and this is the only thing
     * on screen that tells one from a conversation.
     */
    if (hop_chip != NULL)
        clawt_lrg_take(into, clawt_lrg_badge(hop_chip, "warn"));
}

static LrgWidget *
day_divider(GDateTime *when)
{
    g_autofree gchar *text = clawt_chat_day_label(when, NULL);
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgWidget *left = LRG_WIDGET(lrg_separator_new());
    LrgWidget *right = LRG_WIDGET(lrg_separator_new());
    LrgInsets margin = { 0.0f, 22.0f, 0.0f, 4.0f };

    lrg_widget_set_fill_weight(left, 1.0f);
    lrg_widget_set_fill_weight(right, 1.0f);
    lrg_widget_set_valign(left, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(right, LRG_ALIGN_CENTER);
    lrg_widget_set_margin(row, &margin);
    lrg_widget_add_class(row, "day-divider");

    clawt_lrg_take(row, left);
    clawt_lrg_take(row, clawt_lrg_label(text, "caption"));
    clawt_lrg_take(row, right);

    return row;
}

static void
append_message(ClawtLrgApp *app, JsonObject *message, guint index)
{
    const gchar *sender = clawt_ipc_payload_string(message, "sender");
    const gchar *body = clawt_ipc_payload_string(message, "body");
    const gchar *task = clawt_ipc_payload_string(message, "task");
    gint64 ts = clawt_ipc_payload_int(message, "ts", 0);
    gint64 depth = clawt_ipc_payload_int(message, "depth", 0);
    gboolean from_user;
    g_autoptr(GDateTime) when = NULL;
    g_autofree gchar *day = NULL;
    g_autofree gchar *stamp = NULL;
    gboolean new_day = FALSE;
    gboolean run_start;
    LrgWidget *row;
    LrgInsets spacing = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (sender == NULL)
        sender = "?";

    if (body == NULL)
        body = "";

    from_user = g_strcmp0(sender, "user") == 0;

    /*
     * The run needs a day either way, so a message with no time falls
     * back to now for grouping -- but carries no stamp, rather than the
     * moment it happened to be drawn.
     */
    when = (ts > 0) ? g_date_time_new_from_unix_local(ts)
                    : g_date_time_new_now_local();
    day = g_date_time_format(when, "%Y-%m-%d");
    stamp = (ts > 0) ? clawt_chat_time_label(when) : NULL;
    run_start = clawt_chat_run_is_start(state.flow_run_sender,
                                        state.flow_run_day, sender, day,
                                        &new_day);

    if (new_day)
        clawt_lrg_take(state.transcript, day_divider(when));

    g_free(state.flow_run_day);
    state.flow_run_day = g_steal_pointer(&day);
    g_free(state.flow_run_sender);
    state.flow_run_sender = g_strdup(sender);

    row = clawt_lrg_vbox(4.0f);

    if (from_user) {
        /*
         * The operator's turns are bubbles, and only the operator's: an
         * agent's turn runs to dozens of lines, and a container that long
         * stops reading as a message.  The asymmetry says who is
         * speaking at a glance.
         */
        LrgWidget *bubble = clawt_lrg_vbox(0.0f);

        if (run_start) {
            LrgWidget *line = clawt_lrg_hbox(6.0f);

            lrg_widget_set_halign(line, LRG_ALIGN_END);
            add_chips(app, line, task, depth);

            if (stamp != NULL)
                clawt_lrg_take(line, clawt_lrg_label(stamp, "caption"));

            clawt_lrg_take(row, line);
        }

        lrg_widget_add_class(bubble, "clawt-bubble");
        lrg_widget_add_class(bubble, "clawt-bubble-mine");
        lrg_container_set_padding(LRG_CONTAINER(bubble), 10.0f);
        lrg_widget_set_halign(bubble, LRG_ALIGN_END);
        lrg_widget_set_max_size(bubble, 560.0f, -1.0f);
        clawt_lrg_take(bubble, clawt_lrg_markdown(app, body));
        clawt_lrg_take(row, bubble);
    } else {
        LrgWidget *line = clawt_lrg_hbox(0.0f);
        LrgWidget *gutter = clawt_lrg_vbox(0.0f);
        LrgWidget *text = clawt_lrg_markdown(app, body);

        if (run_start) {
            JsonObject *agent = clawt_lrg_find_agent(app, sender);
            const gchar *name = clawt_ipc_payload_string(agent, "name");
            LrgWidget *header = clawt_lrg_hbox(10.0f);

            /*
             * The sender's own face and colour.  GTK derives Flow's
             * avatars from the name alone; this client has the fleet
             * listing to hand, so a picture an agent has is used.
             */
            clawt_lrg_take(header, clawt_lrg_avatar(
                app, name != NULL ? name : sender, sender,
                clawt_ipc_payload_boolean(agent, "has_avatar", FALSE),
                clawt_ipc_payload_string(agent, "color"), AVATAR, FALSE));
            clawt_lrg_take(header, clawt_lrg_label(
                name != NULL ? name : sender, "heading"));

            if (stamp != NULL)
                clawt_lrg_take(header, clawt_lrg_label(stamp, "caption"));

            add_chips(app, header, task, depth);
            clawt_lrg_take(row, header);
        }

        /*
         * A real slot the width of the avatar and its gap, so every body
         * in the run starts at the same edge; a continuation's time sits
         * in it, where nothing else does.
         */
        lrg_widget_set_min_size(gutter, GUTTER, 0.0f);
        lrg_widget_set_max_size(gutter, GUTTER, -1.0f);

        if (!run_start && stamp != NULL) {
            LrgWidget *at = clawt_lrg_label(stamp, "caption");

            lrg_widget_add_class(at, "muted");
            lrg_widget_set_halign(at, LRG_ALIGN_START);
            clawt_lrg_take(gutter, at);
        }

        lrg_widget_set_fill_weight(text, 1.0f);
        clawt_lrg_take(line, gutter);
        clawt_lrg_take(line, text);
        clawt_lrg_take(row, line);
    }

    /* turns further apart than the lines inside one */
    spacing.top = !run_start ? 4.0f : (new_day ? 4.0f : 22.0f);
    lrg_widget_set_margin(row, &spacing);

    add_message_menu(row, body);
    clawt_lrg_appear(app, row, index);
    clawt_lrg_take(state.transcript, row);
}

/* ── The open conversation ────────────────────────────────────────── */

typedef struct {
    gchar *room;
    gchar *label;
} Opening;

static void
opening_free(gpointer data)
{
    Opening *opening = data;

    g_free(opening->room);
    g_free(opening->label);
    g_free(opening);
}

static void
show_pane(gboolean open)
{
    lrg_widget_set_visible(state.pane, open);
    lrg_widget_set_visible(state.pane_empty, !open);
}

/*
 * Opens one conversation, or brings the open one up to date -- the same
 * function for both, because a refresh arrives exactly like a click.
 * The room being opened is redrawn; the room already open is appended
 * to, keyed on the message id.  GTK learned this the slow way: redrawing
 * the open room on every fleet event threw the reader back to the oldest
 * message of whatever they were reading, each time anything anywhere
 * said something.
 */
static void
on_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    Opening *opening = user_data;
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *messages;
    gboolean opening_room;
    gboolean was_at_bottom;
    guint drawn = 0;
    guint i;

    /* somebody opened another conversation while this was in flight */
    if (reply == NULL || g_strcmp0(opening->room, state.wanted) != 0)
        return;

    opening_room = g_strcmp0(opening->room, state.flow_room) != 0;
    was_at_bottom = opening_room || at_bottom();

    if (opening_room) {
        clawt_lrg_clear(state.transcript);
        g_hash_table_remove_all(state.flow_shown);
        g_free(state.flow_room);
        state.flow_room = g_strdup(opening->room);
        g_clear_pointer(&state.flow_run_sender, g_free);
        g_clear_pointer(&state.flow_run_day, g_free);
    }

    lrg_label_set_text(LRG_LABEL(state.flow_title),
                       opening->label != NULL ? opening->label
                                              : opening->room);

    messages = (root != NULL && json_object_has_member(root, "messages"))
        ? json_object_get_array_member(root, "messages") : NULL;

    {
        g_autofree gchar *count = clawt_lrg_flow_count_label(
            messages != NULL ? json_array_get_length(messages) : 0);

        lrg_label_set_text(LRG_LABEL(state.flow_count), count);
    }

    /* the same reply feeds the graph: what was said, said once */
    {
        g_auto(GStrv) members = clawt_lrg_flow_room_members(state.rooms,
                                                            opening->room);

        if (members != NULL &&
            clawt_lrg_flow_graph_fold_history(
                state.model, (const gchar *const *)members, reply) > 0)
            rebuild_graph(app);
    }

    for (i = 0; messages != NULL && i < json_array_get_length(messages);
         i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *id = clawt_ipc_payload_string(message, "id");

        /*
         * A message with no id can only be drawn on the pass that
         * redraws everything: appended on a refresh, it would be
         * appended again on the next one.
         */
        if (id == NULL) {
            if (!opening_room)
                continue;
        } else if (!g_hash_table_add(state.flow_shown, g_strdup(id))) {
            continue;
        }

        append_message(app, message, opening_room ? i : drawn);
        drawn++;
    }

    show_pane(TRUE);

    /*
     * Only when something was drawn, and only if the reader was at the
     * newest message: a refresh that found nothing new must not move the
     * view, and one that did moves it only for somebody already there.
     */
    if (drawn > 0 && was_at_bottom)
        follow_queue(app);

    clawt_lrg_app_wake(app);
}

static void
open_room(ClawtLrgApp *app, const gchar *room_id, const gchar *label)
{
    Opening *opening;

    if (room_id == NULL)
        return;

    g_free(state.wanted);
    state.wanted = g_strdup(room_id);
    g_free(state.wanted_label);
    state.wanted_label = g_strdup(label);

    opening = g_new0(Opening, 1);
    opening->room = g_strdup(room_id);
    opening->label = g_strdup(label);

    clawt_lrg_request(app, "room.history",
                      clawt_ipc_payload_new("room", room_id,
                                            "limit", HISTORY_LIMIT, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_history, opening,
                      opening_free);
}

/* ── The conversation list ────────────────────────────────────────── */

static LrgWidget *
create_row(LrgListView *view, gpointer user_data)
{
    LrgWidget *slot = clawt_lrg_vbox(0.0f);

    return slot;
}

/* Rows are recycled, so a bind draws the whole row afresh. */
static void
bind_row(LrgListView *view, LrgWidget *slot, GObject *item, guint position,
         gpointer user_data)
{
    ClawtLrgFlowRoom *room = g_object_get_data(item, ROOM_KEY);
    LrgWidget *row;

    clawt_lrg_clear(slot);

    if (room == NULL)
        return;

    row = clawt_lrg_row(room->label, room->subtitle);
    clawt_lrg_row_add_suffix(row, clawt_lrg_label(room->count, "caption"));

    if (g_strcmp0(room->id, state.flow_room) == 0)
        lrg_widget_add_class(row, "clawt-row-selected");

    clawt_lrg_take(slot, row);
}

static void
on_list_selected(LrgListView *view, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    guint position = lrg_list_view_get_selected(view);
    g_autoptr(GObject) item = NULL;
    ClawtLrgFlowRoom *room;

    /* a refill re-selects the open row; that is not a click */
    if (state.filling || position == LRG_LIST_VIEW_INVALID_POSITION)
        return;

    item = g_list_model_get_item(G_LIST_MODEL(state.items), position);
    room = (item != NULL) ? g_object_get_data(item, ROOM_KEY) : NULL;

    if (room != NULL)
        open_room(app, room->id, room->label);
}

/*
 * Reads each listed room's history once, and again only when the room
 * has more messages than were read -- so the graph starts from what was
 * already said rather than only what is said from now on, without
 * fetching every transcript on every event.
 */
static void
on_graph_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    const gchar *room_id = user_data;
    g_auto(GStrv) members = NULL;

    if (reply == NULL)
        return;

    members = clawt_lrg_flow_room_members(state.rooms, room_id);

    if (members != NULL &&
        clawt_lrg_flow_graph_fold_history(
            state.model, (const gchar *const *)members, reply) > 0)
        rebuild_graph(app);
}

static void
fetch_graph_histories(ClawtLrgApp *app, GPtrArray *rows)
{
    JsonArray *rooms = NULL;
    JsonObject *root = clawt_ipc_reply_object(state.rooms);
    guint i;

    if (root != NULL && json_object_has_member(root, "rooms"))
        rooms = json_object_get_array_member(root, "rooms");

    /* every room with traffic, the operator's too: the switch filters */
    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_ipc_payload_string(room, "id");
        gint64 messages = clawt_ipc_payload_int(room, "messages", 0);
        gint64 read;

        if (id == NULL || messages <= 0)
            continue;

        read = GPOINTER_TO_SIZE(g_hash_table_lookup(state.fetched, id));

        if (messages <= read)
            continue;

        g_hash_table_insert(state.fetched, g_strdup(id),
                            GSIZE_TO_POINTER((gsize)messages));
        clawt_lrg_request(app, "room.history",
                          clawt_ipc_payload_new("room", id,
                                                "limit", HISTORY_LIMIT, NULL),
                          CLAWT_LRG_QUIET, on_graph_history, g_strdup(id),
                          g_free);
    }

    (void)rows;
}

static void
on_room_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    g_autoptr(GPtrArray) rows = NULL;
    guint reselect = LRG_LIST_VIEW_INVALID_POSITION;
    guint i;

    if (reply == NULL)
        return;

    g_clear_pointer(&state.rooms, json_node_unref);
    state.rooms = json_node_copy(reply);

    rows = clawt_lrg_flow_rooms(reply, include_user(), g_get_real_time());

    /*
     * Items own their row; the store is refilled wholesale, so nothing
     * a recycled row was bound to outlives the item it came from.
     */
    state.filling = TRUE;
    g_list_store_remove_all(state.items);

    for (i = 0; i < rows->len; i++) {
        ClawtLrgFlowRoom *room = g_ptr_array_index(rows, i);
        g_autoptr(GObject) item = g_object_new(G_TYPE_OBJECT, NULL);

        if (g_strcmp0(room->id, state.flow_room) == 0)
            reselect = i;

        g_ptr_array_index(rows, i) = NULL;
        g_object_set_data_full(item, ROOM_KEY, room,
                               (GDestroyNotify)clawt_lrg_flow_room_free);
        g_list_store_append(state.items, item);
    }

    /*
     * The conversation already open stays open across a refresh, which
     * arrives on every message -- reselecting the first row would drag
     * the reader away from what they were reading each time anything
     * anywhere said something.
     */
    if (reselect != LRG_LIST_VIEW_INVALID_POSITION)
        lrg_list_view_select(state.list, reselect);

    state.filling = FALSE;

    lrg_widget_set_visible(state.list_empty, rows->len == 0);
    lrg_widget_set_visible(LRG_WIDGET(state.list), rows->len > 0);

    {
        g_autofree gchar *subtitle = (rows->len == 0)
            ? g_strdup("When one agent messages another it appears here")
            : g_strdup_printf("%u conversation%s, newest first", rows->len,
                              rows->len == 1 ? "" : "s");

        lrg_label_set_text(LRG_LABEL(state.subtitle), subtitle);
    }

    /* the open one is brought up to date by the same path as a click */
    if (state.flow_room != NULL && g_strcmp0(state.wanted,
                                             state.flow_room) == 0)
        open_room(app, state.flow_room, state.wanted_label);

    fetch_graph_histories(app, rows);
    rebuild_graph(app);
    clawt_lrg_app_wake(app);
}

static void
refresh_flow(ClawtLrgApp *app)
{
    clawt_lrg_flow_graph_set_agents(state.model, app->agents);
    clawt_lrg_request(app, "room.list", NULL, CLAWT_LRG_QUIET, on_room_list,
                      NULL, NULL);
}

static void
on_include_user_toggled(LrgCheckbox *toggle, gpointer user_data)
{
    refresh_flow(user_data);
}

/* ── The graph ────────────────────────────────────────────────────── */

/*
 * Only links this page makes.  The graph is an editor, and a wire
 * dragged by hand would claim a conversation that never happened.
 */
static gboolean
on_can_connect(LrgNodeGraph *graph, LrgGraphPort *output, LrgGraphPort *input,
               gpointer user_data)
{
    return state.building;
}

/* A wire coloured by its sender, as bright as it is recent. */
static void
color_link(LrgGraphLink *link, const ClawtLrgFlowEdge *edge,
           const gchar *sender_color, gint64 now_us)
{
    GrlColor color = { 0x8f, 0x96, 0xa8, 0xff };
    gdouble heat = clawt_lrg_flow_edge_heat(edge, now_us);

    if (!hex_color(sender_color, &color)) {
        /* no colour of its own: the link keeps the theme's blend */
        if (heat >= 1.0) {
            lrg_graph_link_set_color(link, NULL);
            return;
        }
    }

    color.a = (guint8)(70.0 + 185.0 * heat);
    lrg_graph_link_set_color(link, &color);
}

static gboolean
on_frame(gpointer user_data)
{
    state.frame_pending = FALSE;
    lrg_node_graph_frame_all(state.graph, TRUE);
    clawt_lrg_app_wake(user_data);

    return G_SOURCE_REMOVE;
}

static void
recolor_links(void)
{
    gint64 now = g_get_real_time();
    guint n = lrg_node_graph_get_n_links(state.graph);
    guint i;

    for (i = 0; i < n; i++) {
        LrgGraphLink *link = lrg_node_graph_get_nth_link(state.graph, i);
        const gchar *from = g_object_get_data(G_OBJECT(link), "from");
        const gchar *to = g_object_get_data(G_OBJECT(link), "to");
        const ClawtLrgFlowEdge *edge =
            clawt_lrg_flow_graph_edge(state.model, from, to);
        JsonObject *agent;

        if (edge == NULL)
            continue;

        agent = clawt_lrg_find_agent(state.app, from);
        color_link(link, edge, clawt_ipc_payload_string(agent, "color"), now);
    }
}

static void
rebuild_graph(ClawtLrgApp *app)
{
    gboolean user = include_user();
    g_autofree gchar *signature = NULL;
    g_autoptr(GPtrArray) nodes = NULL;
    g_autoptr(GPtrArray) edges = NULL;
    guint i;

    signature = clawt_lrg_flow_graph_signature(state.model, user);

    /*
     * Rebuilt only when the picture changed: a rebuild throws away what
     * somebody dragged and where they had panned.  Otherwise the wires
     * are only recoloured, because heat moves while nothing is said.
     */
    if (g_strcmp0(signature, state.graph_signature) == 0) {
        recolor_links();
        clawt_lrg_app_wake(app);
        return;
    }

    g_free(state.graph_signature);
    state.graph_signature = g_steal_pointer(&signature);

    state.building = TRUE;
    state.pulse_link = NULL;
    lrg_node_graph_clear(state.graph);

    nodes = clawt_lrg_flow_graph_nodes(state.model, user);
    edges = clawt_lrg_flow_graph_edges(state.model, user);

    for (i = 0; i < nodes->len; i++) {
        ClawtLrgFlowNode *node = g_ptr_array_index(nodes, i);
        LrgGraphNode *card = lrg_node_graph_add_node(state.graph, node->id,
                                                     node->name, node->x,
                                                     node->y);
        g_autofree gchar *tally = NULL;
        LrgWidget *body;
        GrlColor accent;

        if (card == NULL)
            continue;

        lrg_graph_node_set_min_width(card, 200.0f);

        /* its own colour; without one the category picks a stable one */
        if (hex_color(node->color, &accent))
            lrg_graph_node_set_accent(card, &accent);
        else
            lrg_graph_node_set_category(card, node->is_user ? "operator"
                                                            : node->id);

        tally = g_strdup_printf("said %u \xc2\xb7 heard %u", node->sent,
                                node->received);
        body = clawt_lrg_label(tally, "caption");
        lrg_graph_node_set_body(card, body);
        g_object_unref(body);
    }

    for (i = 0; i < edges->len; i++) {
        ClawtLrgFlowEdge *edge = g_ptr_array_index(edges, i);
        LrgGraphNode *from = lrg_node_graph_get_node(state.graph, edge->from);
        LrgGraphNode *to = lrg_node_graph_get_node(state.graph, edge->to);
        g_autofree gchar *out_name = NULL;
        g_autofree gchar *in_name = NULL;
        LrgGraphPort *out;
        LrgGraphPort *in;
        LrgGraphLink *link;
        g_autoptr(GError) error = NULL;

        if (from == NULL || to == NULL)
            continue;

        /* one port a direction per peer: the count is on the talker's */
        out_name = clawt_lrg_flow_port_label(state.model, edge, TRUE);
        in_name = clawt_lrg_flow_port_label(state.model, edge, FALSE);
        out = lrg_graph_node_add_port(from, LRG_GRAPH_PORT_OUTPUT, out_name,
                                      NULL);
        in = lrg_graph_node_add_port(to, LRG_GRAPH_PORT_INPUT, in_name, NULL);

        if (out == NULL || in == NULL)
            continue;

        link = lrg_node_graph_connect(state.graph, out, in, &error);

        if (link == NULL) {
            g_debug("flow graph: %s", error != NULL ? error->message : "?");
            continue;
        }

        g_object_set_data_full(G_OBJECT(link), "from", g_strdup(edge->from),
                               g_free);
        g_object_set_data_full(G_OBJECT(link), "to", g_strdup(edge->to),
                               g_free);
    }

    state.building = FALSE;
    recolor_links();

    /* framed after the next layout; before it the graph has no size */
    if (!state.frame_pending && nodes->len > 0) {
        state.frame_pending = TRUE;
        after(app, FRAME_DELAY_MS, on_frame, app, NULL);
    }

    clawt_lrg_app_wake(app);
}

static gboolean
on_pulse_over(gpointer user_data)
{
    guint generation = GPOINTER_TO_UINT(user_data);

    /* a later pulse owns the wire now; let it finish */
    if (generation != state.pulse_generation || state.graph == NULL)
        return G_SOURCE_REMOVE;

    if (state.pulse_link != NULL &&
        lrg_node_graph_get_selected_link(state.graph) == state.pulse_link) {
        state.pulsing = TRUE;
        lrg_node_graph_unselect_all(state.graph);
        state.pulsing = FALSE;
    }

    state.pulse_link = NULL;
    clawt_lrg_app_wake(state.app);
    return G_SOURCE_REMOVE;
}

/*
 * Makes the wire a message just travelled pulse: selected, with the
 * graph's selection flow on, for a couple of seconds.  Not when somebody
 * has a wire or a card selected themselves -- their selection is theirs,
 * and a pulse that stole it would also close the conversation it opened.
 */
static void
pulse(ClawtLrgApp *app, const gchar *from, const gchar *to)
{
    guint n = lrg_node_graph_get_n_links(state.graph);
    LrgGraphLink *selected = lrg_node_graph_get_selected_link(state.graph);
    guint i;

    if ((selected != NULL && selected != state.pulse_link) ||
        lrg_node_graph_get_n_selected(state.graph) > 0)
        return;

    for (i = 0; i < n; i++) {
        LrgGraphLink *link = lrg_node_graph_get_nth_link(state.graph, i);

        if (g_strcmp0(g_object_get_data(G_OBJECT(link), "from"), from) != 0 ||
            g_strcmp0(g_object_get_data(G_OBJECT(link), "to"), to) != 0)
            continue;

        state.pulsing = TRUE;
        lrg_node_graph_select_link(state.graph, link);
        state.pulsing = FALSE;
        state.pulse_link = link;
        state.pulse_generation++;
        after(app, PULSE_MS, on_pulse_over,
              GUINT_TO_POINTER(state.pulse_generation), NULL);
        clawt_lrg_app_wake(app);
        return;
    }
}

/* A wire somebody clicked opens the conversation behind it. */
static void
on_graph_selection(LrgNodeGraph *graph, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgGraphLink *link;
    const gchar *from;
    const gchar *to;
    g_autofree gchar *room = NULL;
    g_auto(GStrv) members = NULL;

    if (state.pulsing || state.building)
        return;

    link = lrg_node_graph_get_selected_link(graph);

    if (link == NULL || link == state.pulse_link)
        return;

    from = g_object_get_data(G_OBJECT(link), "from");
    to = g_object_get_data(G_OBJECT(link), "to");
    room = clawt_lrg_flow_room_between(state.rooms, from, to);

    if (room == NULL)
        return;

    members = clawt_lrg_flow_room_members(state.rooms, room);

    {
        JsonArray *array = json_array_new();
        g_autofree gchar *label = NULL;
        guint i;

        for (i = 0; members != NULL && members[i] != NULL; i++)
            json_array_add_string_element(array, members[i]);

        label = clawt_lrg_flow_room_label(array);
        json_array_unref(array);
        open_room(app, room, label);
    }
}

/* A card double-clicked selects that agent, as the sidebar would. */
static void
on_node_activated(LrgNodeGraph *graph, LrgGraphNode *node, gpointer user_data)
{
    const gchar *id = lrg_graph_node_get_id(node);

    if (id != NULL && g_strcmp0(id, "user") != 0)
        clawt_lrg_select_agent(user_data, id);
}

static gboolean
on_rebuild_idle(gpointer user_data)
{
    rebuild_graph(user_data);
    return G_SOURCE_REMOVE;
}

/*
 * Something left the graph that this page did not take out -- the
 * Delete key works on any editor.  It is a view, so it is drawn again
 * from the model, from an idle rather than inside the removal.
 */
static void
on_graph_item_removed(LrgNodeGraph *graph, GObject *item, gpointer user_data)
{
    if (state.building)
        return;

    g_clear_pointer(&state.graph_signature, g_free);
    after(user_data, 0, on_rebuild_idle, user_data, NULL);
}

static void
on_fit_clicked(LrgButton *button, gpointer user_data)
{
    lrg_node_graph_frame_all(state.graph, TRUE);
    clawt_lrg_app_wake(user_data);
}

/* ── Building ─────────────────────────────────────────────────────── */

static LrgWidget *
build_graph(ClawtLrgApp *app)
{
    LrgWidget *frame;

    state.graph = lrg_node_graph_new();
    lrg_node_graph_set_look(state.graph, LRG_CANVAS_LOOK_AUTO);
    lrg_node_graph_set_link_shape(state.graph, LRG_CANVAS_LINK_SHAPE_BEZIER);
    lrg_node_graph_set_selection_flow(state.graph, TRUE);
    lrg_node_graph_set_glow(state.graph, 1.6f);
    lrg_node_graph_set_link_width(state.graph, 3.0f);
    lrg_node_graph_set_show_minimap(state.graph, FALSE);
    lrg_widget_set_min_size(LRG_WIDGET(state.graph), 0.0f, 320.0f);

    g_signal_connect(state.graph, "can-connect", G_CALLBACK(on_can_connect),
                     NULL);
    g_signal_connect(state.graph, "selection-changed",
                     G_CALLBACK(on_graph_selection), app);
    g_signal_connect(state.graph, "node-activated",
                     G_CALLBACK(on_node_activated), app);
    g_signal_connect(state.graph, "node-removed",
                     G_CALLBACK(on_graph_item_removed), app);
    g_signal_connect(state.graph, "link-removed",
                     G_CALLBACK(on_graph_item_removed), app);

    frame = clawt_lrg_card(NULL);
    lrg_container_set_padding(LRG_CONTAINER(frame), 0.0f);
    clawt_lrg_take(frame, LRG_WIDGET(state.graph));

    return frame;
}

static LrgWidget *
build_list(ClawtLrgApp *app)
{
    LrgWidget *column = clawt_lrg_vbox(8.0f);

    state.include_user = clawt_lrg_switch("Include your own chats", FALSE);
    g_signal_connect(state.include_user, "toggled",
                     G_CALLBACK(on_include_user_toggled), app);
    clawt_lrg_add(column, state.include_user);
    g_object_unref(state.include_user);
    clawt_lrg_take(column, clawt_lrg_label(
        "Off by default: this page is for what the agents did without you",
        "caption"));

    state.items = g_list_store_new(G_TYPE_OBJECT);
    state.list = lrg_list_view_new();
    lrg_list_view_set_row_height(state.list, 64.0f);
    lrg_list_view_set_factory(state.list, create_row, bind_row, NULL, NULL);
    lrg_list_view_set_model(state.list, G_LIST_MODEL(state.items));
    lrg_widget_set_fill_weight(LRG_WIDGET(state.list), 1.0f);

    /*
     * Selection, not activation: it is what arrow keys move too, and
     * GTK's list learned that an activate signal can quietly never come.
     */
    g_signal_connect(state.list, "selection-changed",
                     G_CALLBACK(on_list_selected), app);
    clawt_lrg_add(column, LRG_WIDGET(state.list));
    g_object_unref(state.list);

    state.list_empty = clawt_lrg_empty_state(
        "Nothing yet",
        "When one agent messages another it appears here, newest first. "
        "Pick a conversation to read it.");
    lrg_widget_set_visible(state.list_empty, FALSE);
    clawt_lrg_add(column, state.list_empty);
    g_object_unref(state.list_empty);

    lrg_widget_set_min_size(column, 280.0f, 0.0f);
    return column;
}

static LrgWidget *
build_pane(ClawtLrgApp *app)
{
    LrgWidget *stack = clawt_lrg_vbox(0.0f);
    LrgWidget *header = clawt_lrg_vbox(2.0f);
    LrgInsets inset = { 12.0f, 0.0f, 12.0f, 18.0f };

    state.pane = clawt_lrg_vbox(8.0f);
    lrg_widget_set_fill_weight(state.pane, 1.0f);

    state.flow_title = clawt_lrg_label("", "heading");
    state.flow_count = clawt_lrg_label("", "caption");
    clawt_lrg_add(header, state.flow_title);
    clawt_lrg_add(header, state.flow_count);
    g_object_unref(state.flow_title);
    g_object_unref(state.flow_count);
    lrg_container_set_padding(LRG_CONTAINER(header), 6.0f);
    clawt_lrg_take(state.pane, header);

    state.transcript = clawt_lrg_vbox(0.0f);
    lrg_widget_set_margin(state.transcript, &inset);

    /*
     * A measure, as the chat has: without one a body runs the width of
     * the window and the eye loses the start of the next line.
     */
    lrg_widget_set_max_size(state.transcript, 820.0f, -1.0f);

    state.scroll = lrg_scroll_box_new();
    lrg_scroll_box_set_hscroll(state.scroll, FALSE);
    lrg_scroll_box_set_vscroll(state.scroll, TRUE);
    lrg_scroll_box_set_kinetic(state.scroll, TRUE);
    lrg_scroll_box_set_child(state.scroll, state.transcript);
    g_object_unref(state.transcript);
    lrg_widget_set_fill_weight(LRG_WIDGET(state.scroll), 1.0f);
    clawt_lrg_add(state.pane, LRG_WIDGET(state.scroll));
    g_object_unref(state.scroll);

    state.pane_empty = clawt_lrg_empty_state(
        "Pick a conversation",
        "Choose one on the left, or click a wire in the graph above.");

    lrg_widget_set_fill_weight(stack, 1.0f);
    clawt_lrg_add(stack, state.pane);
    clawt_lrg_add(stack, state.pane_empty);
    g_object_unref(state.pane);
    g_object_unref(state.pane_empty);
    show_pane(FALSE);

    return stack;
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *header = clawt_lrg_hbox(12.0f);
    LrgWidget *titles = clawt_lrg_vbox(2.0f);
    LrgSplitter *vertical = lrg_splitter_new(LRG_ORIENTATION_VERTICAL);
    LrgSplitter *horizontal = lrg_splitter_new(LRG_ORIENTATION_HORIZONTAL);
    LrgWidget *lower;

    state.app = app;
    state.model = clawt_lrg_flow_graph_new();
    state.flow_shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);
    state.fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          NULL);

    /*
     * Not clawt_lrg_page_new(): its body is one scroll box, and this
     * page is three panes that each scroll on their own -- a list, a
     * transcript and a graph that pans.  The heading is drawn the same.
     */
    state.root = clawt_lrg_vbox(14.0f);
    lrg_container_set_padding(LRG_CONTAINER(state.root), 22.0f);

    clawt_lrg_take(titles, clawt_lrg_label("Flow", "title"));
    state.subtitle = clawt_lrg_label("What the agents are saying to each "
                                     "other", "muted");
    clawt_lrg_add(titles, state.subtitle);
    g_object_unref(state.subtitle);
    lrg_widget_set_fill_weight(titles, 1.0f);
    clawt_lrg_take(header, titles);
    clawt_lrg_take(header, clawt_lrg_button("Fit graph", "flat",
                                            G_CALLBACK(on_fit_clicked), app));
    clawt_lrg_take(state.root, header);

    lower = build_list(app);
    lrg_splitter_set_start_child(horizontal, lower);
    g_object_unref(lower);
    lower = build_pane(app);
    lrg_splitter_set_end_child(horizontal, lower);
    g_object_unref(lower);
    lrg_splitter_set_fraction(horizontal, 0.3f, FALSE);

    lower = build_graph(app);
    lrg_splitter_set_start_child(vertical, lower);
    g_object_unref(lower);
    lrg_splitter_set_end_child(vertical, LRG_WIDGET(horizontal));
    g_object_unref(horizontal);
    /*
     * The graph takes the larger share: its cards are 200 wide on a ring
     * at least 480 across, and in a short pane the frame zooms out until
     * the agents' names are too small to draw.
     */
    lrg_splitter_set_fraction(vertical, 0.6f, FALSE);
    lrg_widget_set_fill_weight(LRG_WIDGET(vertical), 1.0f);
    clawt_lrg_take(state.root, LRG_WIDGET(vertical));

    return state.root;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_flow(app);
}

/* ── Events ───────────────────────────────────────────────────────── */

static void
on_message_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *room_id = clawt_event_get_subject(event);
    const gchar *sender = clawt_event_get_detail(event, "from");
    g_auto(GStrv) members = NULL;
    guint i;

    /*
     * Into the graph at once, from the room's members as last listed --
     * whether or not the page is showing, since that costs no request.
     * The id keeps it from being counted again when a history read
     * brings the same message.
     */
    members = clawt_lrg_flow_room_members(state.rooms != NULL ? state.rooms
                                                              : app->rooms,
                                          room_id);

    if (members == NULL || sender == NULL)
        return;

    if (!clawt_lrg_flow_graph_fold(state.model,
                                   clawt_event_get_detail(event, "id"),
                                   sender, (const gchar *const *)members,
                                   clawt_event_get_timestamp(event)))
        return;

    /* the history read for this room is one message further along */
    {
        gsize read = GPOINTER_TO_SIZE(g_hash_table_lookup(state.fetched,
                                                          room_id));

        if (read > 0)
            g_hash_table_insert(state.fetched, g_strdup(room_id),
                                GSIZE_TO_POINTER(read + 1));
    }

    rebuild_graph(app);

    for (i = 0; members[i] != NULL; i++) {
        if (g_strcmp0(members[i], sender) != 0) {
            pulse(app, sender, members[i]);
            break;
        }
    }
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (state.root == NULL || kind == NULL)
        return;

    if (g_strcmp0(kind, "message") == 0)
        on_message_event(app, event);

    /*
     * The list moves for every message, not only the open room's: it is
     * a list of what the fleet has been doing, and a conversation list
     * that does not move when the agents talk is the one thing it must
     * not be.  A refused message is the other half -- without it two
     * agents just stop, and the conversation trails off for no reason.
     * Read now if the page is showing; showing it reads it anyway.
     */
    if (g_strcmp0(kind, "message") == 0 ||
        g_strcmp0(kind, "message.refused") == 0 ||
        g_str_has_prefix(kind, "room.") ||
        g_strcmp0(kind, "agent.changed") == 0) {
        if (app->page == CLAWT_PAGE_FLOW)
            refresh_flow(app);
    }
}

static const ClawtLrgPage page_impl = { CLAWT_PAGE_FLOW, build, refresh,
                                        event };

const ClawtLrgPage *
clawt_lrg_flow_page(void)
{
    return &page_impl;
}
