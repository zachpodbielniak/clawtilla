/*
 * gtk-flow.c - The flow page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Which agents have been talking to each other, and about what.  It
 * draws through the chat's row builder rather than one of its own,
 * because two builders for one kind of content is how the two drifted
 * into different renderings of the same messages.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

/* ── The flow page ───────────────────────────────────────────────── */

/*
 * "4 minutes ago" rather than a timestamp.
 *
 * A conversation list is read to find the recent one, and working out
 * which of two wall-clock times is nearer to now is work the reader
 * should not be doing.
 */
static gchar *
relative_time(gint64 ts)
{
    gint64 delta = (g_get_real_time() / G_USEC_PER_SEC) - ts;

    if (ts <= 0)
        return g_strdup("");

    if (delta < 60)
        return g_strdup("just now");

    if (delta < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", delta / 60);

    if (delta < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", delta / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", delta / 86400);
}

/*
 * One line of a message, trimmed, for a list subtitle.
 *
 * Cut on a character boundary rather than a byte one: an agent's reply
 * is as likely to contain an em dash as not, and half a UTF-8 sequence
 * renders as a replacement glyph for the rest of the row.
 */
static gchar *
one_line(const gchar *body, glong limit)
{
    g_autofree gchar *flat = NULL;
    const gchar *newline;

    if (body == NULL)
        return g_strdup("");

    newline = strchr(body, '\n');
    flat = (newline != NULL) ? g_strndup(body, (gsize)(newline - body))
                             : g_strdup(body);
    g_strstrip(flat);

    if (!g_utf8_validate(flat, -1, NULL))
        return g_strdup("");

    if (g_utf8_strlen(flat, -1) <= limit)
        return g_steal_pointer(&flat);

    {
        const gchar *end = g_utf8_offset_to_pointer(flat, limit);
        g_autofree gchar *cut = g_strndup(flat, (gsize)(end - flat));

        return g_strconcat(cut, "\xe2\x80\xa6", NULL);
    }
}

/*
 * "alpha and beta", from the room's member list.
 *
 * Built from the members rather than by taking the id apart, because
 * how a direct room is named is the daemon's business -- a client that
 * parses "dm:a:b" is a client that breaks when that changes.
 */
static gchar *
room_label(JsonArray *members)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (out->len > 0)
            g_string_append(out, i + 1 == json_array_get_length(members)
                                 ? " and " : ", ");

        g_string_append(out, g_strcmp0(member, "user") == 0 ? "you" : member);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gboolean
room_involves_user(JsonArray *members)
{
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        if (g_strcmp0(json_array_get_string_element(members, i),
                      "user") == 0)
            return TRUE;
    }

    return FALSE;
}

void
clawt_gtk_on_flow_task_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    /*
     * The task board is where the rest of the story is -- who asked for
     * it, what state it is in, what it returned.
     */
    adw_view_stack_set_visible_child_name(self->pages, "tasks");
}

/*
 * Loads one conversation into the right-hand pane.
 */
static void
show_flow_room(ClawtWindow *self, const gchar *room_id, const gchar *label)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    /*
     * The request first, and only then the clear.
     *
     * clawt_window_request() iterates the main context while it waits,
     * so an event arriving mid-flight re-enters this function: the inner
     * call emptied the box and filled it, the outer one carried on
     * appending from where it was, and the conversation appeared twice.
     * Emptying after the answer is back means a nested call finishes
     * completely and the outer one then replaces its work rather than
     * adding to it.
     */
    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", room_id, "limit", "200", NULL));

    if (reply == NULL)
        return;

    clawt_gtk_clear_box(self->flow_transcript);

    g_free(self->flow_room);
    self->flow_room = g_strdup(room_id);

    gtk_label_set_text(GTK_LABEL(self->flow_title),
                       label != NULL ? label : room_id);

    messages = json_object_get_array_member(clawt_payload_of(reply),
                                            "messages");

    {
        g_autofree gchar *count = g_strdup_printf(
            "%u message%s", json_array_get_length(messages),
            json_array_get_length(messages) == 1 ? "" : "s");

        gtk_label_set_text(GTK_LABEL(self->flow_subtitle), count);
    }

    /*
     * Drawn through the chat's own row builder.
     *
     * This used to be a second builder, and the two had drifted into
     * visibly different renderings of the same messages: one with runs,
     * avatars, day dividers and a measure, the other a flat list of
     * captions.  A reader moving between a conversation and the flow of
     * one saw two conventions for one kind of content.
     *
     * Its avatars are derived from each sender's name rather than from
     * one agent's configured image, because a room here has several
     * participants -- which is what the NULLs in the view say.
     */
    g_clear_pointer(&self->flow_run_sender, g_free);
    g_clear_pointer(&self->flow_run_day, g_free);

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_json_string(message, "sender", "?");
        TranscriptView view = { self->flow_transcript,
                                &self->flow_run_sender,
                                &self->flow_run_day, NULL, NULL };

        clawt_gtk_append_message_to(self, &view, sender,
                                    clawt_json_string(message, "body", ""),
                                    g_strcmp0(sender, "user") == 0,
                                    clawt_json_int(message, "ts", 0),
                                    clawt_json_string(message, "task", NULL),
                                    clawt_json_int(message, "depth", 0));
    }

    gtk_stack_set_visible_child_name(GTK_STACK(self->flow_stack), "room");
}

static void
on_flow_row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *room_id;
    const gchar *label;

    (void)list;

    if (row == NULL)
        return;

    room_id = g_object_get_data(G_OBJECT(row), "room");
    label = g_object_get_data(G_OBJECT(row), "label");

    if (room_id != NULL)
        show_flow_room(self, room_id, label);
}

/*
 * Sorts the conversation list, most recently active first.
 */
static gint
compare_by_last(gconstpointer a, gconstpointer b)
{
    JsonObject *left = *(JsonObject **)a;
    JsonObject *right = *(JsonObject **)b;
    gint64 left_ts = clawt_json_int(left, "last_ts", 0);
    gint64 right_ts = clawt_json_int(right, "last_ts", 0);

    if (left_ts == right_ts)
        return 0;

    return (left_ts > right_ts) ? -1 : 1;
}

static void
refresh_flow_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) ordered = NULL;
    gboolean include_user = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(self->flow_include_user));
    JsonArray *rooms;
    guint i;

    clawt_gtk_clear_list(self->flow_list);

    reply = clawt_window_request(self, "room.list", NULL);

    if (reply == NULL)
        return;

    rooms = json_object_get_array_member(clawt_payload_of(reply), "rooms");
    ordered = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);

        /*
         * An empty room is one the daemon made because somebody could
         * have talked, not one where anybody did. A fleet accumulates a
         * direct room per pair and listing them all buries the few that
         * matter.
         */
        if (clawt_json_int(room, "messages", 0) == 0)
            continue;

        if (!include_user &&
            room_involves_user(json_object_get_array_member(room, "members")))
            continue;

        g_ptr_array_add(ordered, room);
    }

    g_ptr_array_sort(ordered, compare_by_last);

    for (i = 0; i < ordered->len; i++) {
        JsonObject *room = g_ptr_array_index(ordered, i);
        const gchar *room_id = clawt_json_string(room, "id", "");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *count;
        g_autofree gchar *label =
            room_label(json_object_get_array_member(room, "members"));
        g_autofree gchar *snippet =
            one_line(clawt_json_string(room, "last_body", ""), 44);
        g_autofree gchar *when =
            relative_time(clawt_json_int(room, "last_ts", 0));
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s \xc2\xb7 %s: %s", when,
            clawt_json_string(room, "last_sender", "?"), snippet);
        g_autofree gchar *clawt_gtk_badge = g_strdup_printf(
            "%" G_GINT64_FORMAT, clawt_json_int(room, "messages", 0));

        clawt_gtk_set_row_text(row, label, subtitle);

        count = gtk_label_new(clawt_gtk_badge);
        gtk_widget_add_css_class(count, "caption");
        gtk_widget_add_css_class(count, "dim-label");
        gtk_widget_set_valign(count, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), count);

        g_object_set_data_full(G_OBJECT(row), "room", g_strdup(room_id),
                               g_free);
        g_object_set_data_full(G_OBJECT(row), "label",
                               g_strdup(label), g_free);

        gtk_list_box_append(self->flow_list, row);

        /*
         * The conversation already open stays open across a refresh,
         * which arrives on every message -- reselecting the first row
         * would drag the reader away from what they were reading every
         * time anything anywhere said something.
         */
        if (g_strcmp0(room_id, self->flow_room) == 0)
            gtk_list_box_select_row(
                self->flow_list,
                GTK_LIST_BOX_ROW(gtk_widget_get_last_child(
                    GTK_WIDGET(self->flow_list))));
    }
}

void
clawt_gtk_refresh_flow(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_FLOW))
        return;

    do {
        refresh_flow_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_FLOW));
}

static void
on_flow_filter_toggled(GtkCheckButton *button, gpointer user_data)
{
    (void)button;

    clawt_gtk_refresh_flow(user_data);
}

GtkWidget *
clawt_gtk_build_flow_page(ClawtWindow *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *list_scroll = gtk_scrolled_window_new();
    GtkWidget *empty = adw_status_page_new();

    /* ── who has been talking ── */
    self->flow_include_user = gtk_check_button_new_with_label(
        "Include your own chats");
    gtk_widget_set_margin_start(self->flow_include_user, 12);
    gtk_widget_set_margin_end(self->flow_include_user, 12);
    gtk_widget_set_margin_top(self->flow_include_user, 12);
    gtk_widget_set_margin_bottom(self->flow_include_user, 6);
    gtk_widget_set_tooltip_text(
        self->flow_include_user,
        "Off by default: this page is for what the agents did without you");
    g_signal_connect(self->flow_include_user, "toggled",
                     G_CALLBACK(on_flow_filter_toggled), self);

    self->flow_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->flow_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->flow_list), "navigation-sidebar");

    /*
     * ::row-selected, not ::row-activated: libadwaita clears
     * GtkListBoxRow:activatable on an AdwActionRow with no
     * activatable-widget, so the activate signal never arrives. Selection
     * also covers arrow-key navigation, which activation does not.
     */
    g_signal_connect(self->flow_list, "row-selected",
                     G_CALLBACK(on_flow_row_selected), self);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll),
                                  GTK_WIDGET(self->flow_list));
    gtk_widget_set_vexpand(list_scroll, TRUE);

    gtk_box_append(GTK_BOX(left), self->flow_include_user);
    gtk_box_append(GTK_BOX(left), list_scroll);
    gtk_widget_set_size_request(left, 280, -1);

    /* ── and what they said ── */
    self->flow_title = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->flow_title, "title-4");
    gtk_label_set_xalign(GTK_LABEL(self->flow_title), 0.0f);

    self->flow_subtitle = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->flow_subtitle, "caption");
    gtk_widget_add_css_class(self->flow_subtitle, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(self->flow_subtitle), 0.0f);

    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 6);
    gtk_box_append(GTK_BOX(header), self->flow_title);
    gtk_box_append(GTK_BOX(header), self->flow_subtitle);

    self->flow_transcript = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    self->flow_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());

    /*
     * The same measure the chat has, and for the same reason: without a
     * clamp a body label's natural width is whatever the window is, and
     * the line runs past the point where the eye can find the start of
     * the next one.  Inside the scrolled window rather than around it,
     * so the scrollbar stays at the window edge.
     */
    {
        GtkWidget *clamp = adw_clamp_new();

        /*
         * "The same measure the chat has" was true of the comment above
         * and not of the code: this clamp was left at libadwaita's
         * default, so a reader who widened the conversation found Flow
         * still at 600.  It takes the resolved measure now, from the
         * one resolver.
         */
        self->flow_clamp = clamp;
        adw_clamp_set_maximum_size(ADW_CLAMP(clamp), clawt_gtk_chat_measure(self));
        adw_clamp_set_child(ADW_CLAMP(clamp),
                            GTK_WIDGET(self->flow_transcript));
        gtk_scrolled_window_set_child(self->flow_scroll, clamp);
        clawt_gtk_follow_viewport_width(self, GTK_WIDGET(self->flow_scroll));
    }

    gtk_widget_set_margin_bottom(GTK_WIDGET(self->flow_transcript), 18);
    gtk_widget_set_vexpand(GTK_WIDGET(self->flow_scroll), TRUE);

    gtk_box_append(GTK_BOX(right), header);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(self->flow_scroll));

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty),
                                  "system-users-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Nothing yet");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(empty),
        "When one agent messages another it appears here, newest first. "
        "Pick a conversation to read it.");

    self->flow_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->flow_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(self->flow_stack), right, "room");

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), self->flow_stack);
    gtk_paned_set_position(GTK_PANED(paned), 300);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);

    return paned;
}
