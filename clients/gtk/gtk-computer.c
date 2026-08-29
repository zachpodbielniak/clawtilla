/*
 * gtk-computer.c - The computer page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The machine an agent has, in four halves: a command run inside it, the
 * screen while somebody is watching, the paths from this machine that
 * appear inside, and the shared drop-box.
 *
 * The four are walked out of clawt_computer_view_count() rather than
 * listed here, because the web client draws the same four and a list
 * spelled out twice is a list that drifts -- which here would be a tab
 * that exists in a browser and not in this window, with nothing to say
 * so.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

/* ── Shell ───────────────────────────────────────────────────────── */

static void
on_exec(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    GtkTextBuffer *buffer;
    GtkTextIter end;
    const gchar *command;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    command = gtk_editable_get_text(GTK_EDITABLE(self->exec_entry));

    if (command == NULL || *command == '\0')
        return;

    buffer = gtk_text_view_get_buffer(self->exec_output);
    gtk_text_buffer_get_end_iter(buffer, &end);

    {
        g_autofree gchar *echo = g_strdup_printf("$ %s\n", command);

        gtk_text_buffer_insert(buffer, &end, echo, -1);
    }

    reply = clawt_window_request(
        self, "computer.exec",
        clawt_build_payload("agent", self->selected_agent, "command",
                            command, NULL));

    if (reply == NULL)
        return;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stdout", ""), -1);

    /*
     * stderr is shown too.  A console that swallowed it would leave a
     * failing command looking like one that produced nothing.
     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stderr", ""), -1);

    gtk_editable_set_text(GTK_EDITABLE(self->exec_entry), "");
}

/* ── Screen ──────────────────────────────────────────────────────── */

/*
 * Subscribes, or lets go, so that the daemon only grabs while this page
 * is the one somebody is looking at.
 *
 * A watch held while the window sat on the chat page would cost the
 * agent a frame a second for nothing, which is the whole thing
 * #ClawtObserver exists to prevent -- and it would be invisible from
 * here, because the only symptom is the agent being slower.
 */
static void
screen_set_watching(ClawtWindow *self, gboolean want)
{
    g_autoptr(JsonNode) reply = NULL;

    if (self->screen_watching == want)
        return;

    if (self->screen_agent == NULL && want)
        return;

    if (want) {
        reply = clawt_window_request(
            self, "computer.observe",
            clawt_build_payload("agent", self->screen_agent,
                                "watcher", CLAWT_GTK_WATCHER_NAME, NULL));

        /*
         * Recorded whether or not the request succeeded, because a
         * client that only remembered a successful subscribe would try
         * again on every refresh against an agent that has no screen --
         * the shape this codebase has already been bitten by once, at
         * the event subscription.
         */
        self->screen_watching = TRUE;
        return;
    }

    reply = clawt_window_request(
        self, "computer.observe_stop",
        clawt_build_payload("agent", self->screen_agent,
                            "watcher", CLAWT_GTK_WATCHER_NAME, NULL));
    self->screen_watching = FALSE;
}

/*
 * The picture, decoded at the size it is drawn.
 *
 * gdk_pixbuf_new_from_stream_at_scale() rather than a full decode and a
 * widget that shrinks it, because GTK has no maximum size: a size
 * request is a floor, and GtkPicture takes its natural size from its
 * paintable. A 4K frame decoded into a preview panel is the trap
 * CLAUDE.md already records, and it arrives once a second here rather
 * than once per avatar.
 */
static void
screen_set_frame(ClawtWindow *self, JsonObject *frame)
{
    const gchar *base64 = clawt_json_string(frame, "base64", NULL);
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GInputStream) stream = NULL;
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    g_autoptr(GBytes) pixels = NULL;
    g_autoptr(GdkTexture) texture = NULL;
    g_autofree guchar *raw = NULL;
    gsize raw_length = 0;

    if (base64 == NULL) {
        gtk_picture_set_paintable(self->screen_picture, NULL);
        return;
    }

    raw = g_base64_decode(base64, &raw_length);

    if (raw == NULL || raw_length == 0) {
        gtk_picture_set_paintable(self->screen_picture, NULL);
        return;
    }

    bytes = g_bytes_new_take(g_steal_pointer(&raw), raw_length);
    stream = g_memory_input_stream_new_from_bytes(bytes);

    pixbuf = gdk_pixbuf_new_from_stream_at_scale(
        stream, CLAWT_SCREEN_DECODE_WIDTH, -1, TRUE, NULL, NULL);

    if (pixbuf == NULL) {
        gtk_picture_set_paintable(self->screen_picture, NULL);
        return;
    }

    pixels = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                         gdk_pixbuf_get_byte_length(pixbuf));
    texture = gdk_memory_texture_new(
        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8
                                         : GDK_MEMORY_R8G8B8,
        pixels, (gsize)gdk_pixbuf_get_rowstride(pixbuf));

    gtk_picture_set_paintable(self->screen_picture,
                              GDK_PAINTABLE(texture));
}

void
clawt_gtk_refresh_screen(ClawtWindow *self)
{
    g_autoptr(JsonNode) status = NULL;
    g_autoptr(JsonNode) frame = NULL;
    JsonObject *root;
    gboolean held;
    gboolean observable;
    gint64 stamp;

    if (self->screen_picture == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SCREEN))
        return;

    do {
        if (self->screen_agent == NULL) {
            gtk_label_set_text(self->screen_status, "No agent selected.");
            gtk_picture_set_paintable(self->screen_picture, NULL);
            continue;
        }

        /*
         * Cleared before each pass. This loop runs again when an event
         * arrives mid-refresh, and an autoptr assigned twice leaks the
         * first reply -- once a second, for as long as the tab is open.
         */
        g_clear_pointer(&status, json_node_unref);
        g_clear_pointer(&frame, json_node_unref);

        /*
         * The lease is pushed forward on every redraw rather than only
         * when the tab is opened. A watch is a lease so that a browser
         * that goes away stops costing the agent frames, and a window
         * left open on the Screen tab for longer than the lease would
         * otherwise be dropped by exactly the same rule.
         */
        if (self->screen_watching) {
            g_autoptr(JsonNode) renewed = clawt_window_request(
                self, "computer.observe",
                clawt_build_payload("agent", self->screen_agent,
                                    "watcher", CLAWT_GTK_WATCHER_NAME,
                                    NULL));

            (void)renewed;
        }

        status = clawt_window_request(
            self, "computer.screen",
            clawt_build_payload("agent", self->screen_agent, NULL));

        if (status == NULL) {
            gtk_label_set_text(self->screen_status,
                               "The screen cannot be read.");
            continue;
        }

        root = clawt_payload_of(status);
        observable = clawt_json_boolean(root, "observable", FALSE);
        held = clawt_json_boolean(root, "held", FALSE);
        stamp = clawt_json_int(root, "stamp", 0);

        self->screen_held = held;

        g_free(self->screen_viewer);
        self->screen_viewer = g_strdup(clawt_json_string(root, "viewer",
                                                         NULL));

        gtk_widget_set_visible(self->screen_take, !held);
        gtk_widget_set_visible(self->screen_release, held);
        gtk_widget_set_visible(self->screen_viewer_button,
                               self->screen_viewer != NULL);
        gtk_widget_set_sensitive(GTK_WIDGET(self->screen_input), held);
        gtk_widget_set_sensitive(GTK_WIDGET(self->screen_click), held);

        if (!observable) {
            gtk_label_set_text(
                self->screen_status,
                clawt_json_string(
                    root, "error",
                    "No screen to watch yet: the agent is not running, or "
                    "it has no desktop."));
            gtk_picture_set_paintable(self->screen_picture, NULL);
            continue;
        }

        frame = clawt_window_request(
            self, "computer.frame",
            clawt_build_payload("agent", self->screen_agent, NULL));

        if (frame != NULL)
            screen_set_frame(self, clawt_payload_of(frame));

        {
            g_autoptr(GString) said = g_string_new(NULL);

            /*
             * A stale frame is labelled with its age rather than shown
             * as current. The threshold comes from the library, so this
             * window and a browser agree about when a picture stopped
             * being news.
             */
            if (stamp <= 0)
                g_string_append(said, "No frame yet.");
            else if (clawt_json_boolean(root, "stale", FALSE)) {
                g_autofree gchar *ago =
                    clawt_time_ago_label(stamp, g_get_real_time());

                g_string_append_printf(said, "Stale \xe2\x80\x94 %s.", ago);
            } else
                g_string_append(said, "Live.");

            if (held)
                g_string_append_printf(
                    said, " Held by %s.",
                    clawt_json_string(root, "holder", "somebody"));

            if (clawt_json_string(root, "request", NULL) != NULL)
                g_string_append_printf(
                    said, " It has asked for hands: %s",
                    clawt_json_string(root, "request", ""));

            if (clawt_json_string(root, "error", NULL) != NULL)
                g_string_append_printf(
                    said, " %s", clawt_json_string(root, "error", ""));

            gtk_label_set_text(self->screen_status, said->str);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SCREEN));
}

static void
on_screen_take(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (self->screen_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "computer.takeover",
        clawt_build_payload("agent", self->screen_agent,
                            "holder", "the GTK client", NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self,
                       "You have the screen. The agent's clicks are refused "
                       "until you give it back.");
    clawt_gtk_refresh_screen(self);
}

static void
on_screen_release(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (self->screen_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "computer.release",
        clawt_build_payload("agent", self->screen_agent, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "The agent has the screen back.");
    clawt_gtk_refresh_screen(self);
}

/*
 * Opens the VM's own VNC server in a real viewer.
 *
 * Beside the preview rather than instead of it: a frame a second is
 * right for watching and hopeless for using. `remote-viewer` is spawned
 * by name and its absence is said plainly -- a button that silently does
 * nothing is worse than one that explains what is missing.
 */
static void
on_screen_viewer(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *binary = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[3];

    (void)button;

    if (self->screen_viewer == NULL)
        return;

    binary = g_find_program_in_path("remote-viewer");

    if (binary == NULL) {
        clawt_window_toast(self,
                           "remote-viewer is not installed (Fedora: "
                           "virt-viewer). The address is on the Screen tab.");
        return;
    }

    argv[0] = binary;
    argv[1] = self->screen_viewer;
    argv[2] = NULL;

    if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                       G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
        clawt_window_toast(self, error->message);
}

/*
 * One event, from whichever field was filled in.
 *
 * Typing first, then a key, then a click: three ways to act in one form
 * has to pick one, and typing is what somebody reaches for most.
 */
static void
on_screen_input(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *text;
    const gchar *click;

    (void)widget;

    if (self->screen_agent == NULL || !self->screen_held)
        return;

    text = gtk_editable_get_text(GTK_EDITABLE(self->screen_input));
    click = gtk_editable_get_text(GTK_EDITABLE(self->screen_click));

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, self->screen_agent);

    if (text != NULL && *text != '\0') {
        /*
         * A leading `@` means a key name rather than a string, because
         * a second entry for keys would be a second control somebody
         * has to notice -- and "ctrl+l" typed into a browser's address
         * bar is a perfectly ordinary thing to want to type.
         */
        if (text[0] == '@' && text[1] != '\0') {
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(builder, "key");
            json_builder_set_member_name(builder, "text");
            json_builder_add_string_value(builder, text + 1);
        } else {
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(builder, "text");
            json_builder_set_member_name(builder, "text");
            json_builder_add_string_value(builder, text);
        }
    } else if (click != NULL && strchr(click, ',') != NULL) {
        g_auto(GStrv) parts = g_strsplit(click, ",", 2);

        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder, "click");
        json_builder_set_member_name(builder, "x");
        json_builder_add_int_value(builder,
                                   g_ascii_strtoll(g_strstrip(parts[0]),
                                                   NULL, 10));
        json_builder_set_member_name(builder, "y");
        json_builder_add_int_value(builder,
                                   g_ascii_strtoll(g_strstrip(parts[1]),
                                                   NULL, 10));
    } else {
        json_builder_end_object(builder);
        clawt_window_toast(self,
                           "Nothing to send: type something, or give a "
                           "coordinate as x,y.");
        return;
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(self, "computer.input",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    gtk_editable_set_text(GTK_EDITABLE(self->screen_input), "");
    clawt_gtk_refresh_screen(self);
}

/* ── Mounts ──────────────────────────────────────────────────────── */

void
clawt_gtk_refresh_computer_mounts(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *mounts;
    guint i;

    if (self->computer_mount_list == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_COMPUTER_MOUNTS))
        return;

    do {
        clawt_gtk_clear_list(self->computer_mount_list);

        if (self->selected_agent == NULL)
            continue;

        g_clear_pointer(&reply, json_node_unref);

        reply = clawt_window_request(
            self, "agent.mount.list",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply == NULL)
            continue;

        mounts = json_object_get_array_member(clawt_payload_of(reply),
                                              "mounts");

        for (i = 0; mounts != NULL && i < json_array_get_length(mounts);
             i++) {
            JsonObject *mount = json_array_get_object_element(mounts, i);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *pair = NULL;

            /*
             * Both spellings, host first. An agent's own read and write
             * run on the host and only computer_exec enters the guest,
             * so a path given without saying which side it is on gets
             * looked for on the wrong one.
             */
            pair = g_strdup_printf("%s  =  %s",
                                   clawt_json_string(mount, "source", "?"),
                                   clawt_json_string(mount, "target", "?"));

            clawt_gtk_set_row_text(row, pair,
                                   clawt_json_string(mount, "mode", "rw"));
            gtk_list_box_append(self->computer_mount_list, row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_COMPUTER_MOUNTS));
}

/* ── Exchange ────────────────────────────────────────────────────── */

void
clawt_gtk_refresh_exchange(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *files;
    guint i;

    if (self->exchange_list == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_EXCHANGE))
        return;

    do {
        clawt_gtk_clear_list(self->exchange_list);

        g_clear_pointer(&reply, json_node_unref);

        reply = clawt_window_request(self, "exchange.list", NULL);

        if (reply == NULL)
            continue;

        root = clawt_payload_of(reply);
        files = json_object_has_member(root, "entries")
                ? json_object_get_array_member(root, "entries")
                : (json_object_has_member(root, "files")
                   ? json_object_get_array_member(root, "files") : NULL);

        for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
            JsonNode *node = json_array_get_element(files, i);
            GtkWidget *row = adw_action_row_new();
            const gchar *name;

            if (JSON_NODE_HOLDS_OBJECT(node))
                name = clawt_json_string(json_node_get_object(node), "path",
                                         "?");
            else
                name = json_node_get_string(node);

            clawt_gtk_set_row_text(row, name != NULL ? name : "?", NULL);
            gtk_list_box_append(self->exchange_list, row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_EXCHANGE));
}

/* ── The page ────────────────────────────────────────────────────── */

/*
 * Whether the Screen tab should be there at all.
 *
 * Asked of the daemon's own answer -- `computer_screen`, which it fills
 * in from clawt_computer_type_has_screen() -- rather than from a list of
 * types here, which would offer the tab on a backend that has none or
 * fail to offer it on one added later.
 */
static void
sync_screen_tab(ClawtWindow *self, JsonObject *agent)
{
    gboolean has_screen = clawt_json_boolean(agent, "computer_screen",
                                             FALSE);

    if (self->computer_screen_page == NULL)
        return;

    adw_view_stack_page_set_visible(self->computer_screen_page, has_screen);
}

static void
on_computer_view_changed(GObject *object, GParamSpec *spec,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name;

    (void)spec;

    name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(object));

    /*
     * The subscription follows the tab, so nothing is grabbed while
     * somebody is reading the mounts. Leaving the watch running would
     * cost the agent a frame a second for a picture behind another tab.
     */
    screen_set_watching(
        self,
        clawt_computer_view_from_nick(name) == CLAWT_COMPUTER_VIEW_SCREEN);

    if (clawt_computer_view_from_nick(name) == CLAWT_COMPUTER_VIEW_SCREEN)
        clawt_gtk_refresh_screen(self);
    else if (clawt_computer_view_from_nick(name) ==
             CLAWT_COMPUTER_VIEW_MOUNTS)
        clawt_gtk_refresh_computer_mounts(self);
    else if (clawt_computer_view_from_nick(name) ==
             CLAWT_COMPUTER_VIEW_EXCHANGE)
        clawt_gtk_refresh_exchange(self);
}

void
clawt_gtk_refresh_computer(ClawtWindow *self, JsonObject *agent)
{
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean has_computer = strstr(caps, "computer") != NULL;
    const gchar *now = clawt_json_string(agent, "id", NULL);

    gtk_widget_set_sensitive(GTK_WIDGET(self->exec_entry), has_computer);

    gtk_label_set_text(self->computer_state,
                       has_computer
                           ? clawt_json_string(agent, "computer", "none")
                           : "This agent has no computer.");

    sync_screen_tab(self, agent);

    /*
     * A different agent means the watch on the old one has to go, and it
     * has to go *before* the id is replaced -- unsubscribing after would
     * name whichever agent was selected next, leaving the first one
     * being grabbed for ever with nobody watching.
     */
    if (g_strcmp0(now, self->screen_agent) != 0) {
        gboolean was_watching = self->screen_watching;

        screen_set_watching(self, FALSE);
        g_free(self->screen_agent);
        self->screen_agent = g_strdup(now);
        g_clear_pointer(&self->screen_viewer, g_free);
        gtk_picture_set_paintable(self->screen_picture, NULL);

        if (was_watching)
            screen_set_watching(self, TRUE);
    }

    /*
     * The state row is the only thing the daemon's own status adds over
     * the listing, and it is the one that says *why* -- "created but not
     * started" reads very differently from "running".
     */
    if (has_computer && now != NULL) {
        g_autoptr(JsonNode) status = clawt_window_request(
            self, "computer.status",
            clawt_build_payload("agent", now, NULL));

        if (status != NULL) {
            g_autofree gchar *said = g_strdup_printf(
                "%s \xe2\x80\x94 %s",
                clawt_json_string(agent, "computer", "none"),
                clawt_json_string(clawt_payload_of(status), "state",
                                  "unknown"));

            gtk_label_set_text(self->computer_state, said);
        }
    }
}

static GtkWidget *
build_shell_view(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *run;
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->exec_output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(self->exec_output, FALSE);
    gtk_text_view_set_monospace(self->exec_output, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->exec_output));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);

    self->exec_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->exec_entry, "Command");
    gtk_widget_set_hexpand(GTK_WIDGET(self->exec_entry), TRUE);
    g_signal_connect(self->exec_entry, "activate", G_CALLBACK(on_exec), self);

    run = gtk_button_new_with_label("Run");
    g_signal_connect(run, "clicked", G_CALLBACK(on_exec), self);

    gtk_box_append(GTK_BOX(entry_box), GTK_WIDGET(self->exec_entry));
    gtk_box_append(GTK_BOX(entry_box), run);
    gtk_widget_set_margin_start(entry_box, 12);
    gtk_widget_set_margin_end(entry_box, 12);
    gtk_widget_set_margin_bottom(entry_box, 12);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), entry_box);

    return box;
}

static GtkWidget *
build_screen_view(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *send;

    self->screen_status = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_label_set_wrap(self->screen_status, TRUE);
    gtk_label_set_xalign(self->screen_status, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(self->screen_status), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->screen_status), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(self->screen_status), 12);

    self->screen_picture = GTK_PICTURE(gtk_picture_new());

    /*
     * SCALE_DOWN, never CONTAIN: a frame smaller than the panel must not
     * be blown up into a blurry one, and the coordinates a person reads
     * off it stay the picture's own.
     */
    gtk_picture_set_content_fit(self->screen_picture,
                                GTK_CONTENT_FIT_SCALE_DOWN);
    gtk_widget_set_vexpand(GTK_WIDGET(self->screen_picture), TRUE);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->screen_picture));

    /*
     * Never horizontally. A GtkScrolledWindow left at AUTOMATIC gives
     * its child the child's natural width, and the whole page then
     * scrolls sideways -- the trap every boxed-list page in this client
     * has already been bitten by.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);

    self->screen_take = gtk_button_new_with_label("Take the screen");
    g_signal_connect(self->screen_take, "clicked",
                     G_CALLBACK(on_screen_take), self);

    self->screen_release = gtk_button_new_with_label("Give the screen back");
    gtk_widget_add_css_class(self->screen_release, "suggested-action");
    g_signal_connect(self->screen_release, "clicked",
                     G_CALLBACK(on_screen_release), self);

    self->screen_viewer_button = gtk_button_new_with_label("Open in a viewer");
    g_signal_connect(self->screen_viewer_button, "clicked",
                     G_CALLBACK(on_screen_viewer), self);

    gtk_box_append(GTK_BOX(buttons), self->screen_take);
    gtk_box_append(GTK_BOX(buttons), self->screen_release);
    gtk_box_append(GTK_BOX(buttons), self->screen_viewer_button);
    gtk_widget_set_margin_start(buttons, 12);
    gtk_widget_set_margin_end(buttons, 12);

    self->screen_input = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->screen_input,
                                   "Type, or @key for a key or combo");
    gtk_widget_set_hexpand(GTK_WIDGET(self->screen_input), TRUE);
    g_signal_connect(self->screen_input, "activate",
                     G_CALLBACK(on_screen_input), self);

    self->screen_click = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->screen_click, "Click at x,y");

    send = gtk_button_new_with_label("Send");
    g_signal_connect(send, "clicked", G_CALLBACK(on_screen_input), self);

    gtk_box_append(GTK_BOX(input_box), GTK_WIDGET(self->screen_input));
    gtk_box_append(GTK_BOX(input_box), GTK_WIDGET(self->screen_click));
    gtk_box_append(GTK_BOX(input_box), send);
    gtk_widget_set_margin_start(input_box, 12);
    gtk_widget_set_margin_end(input_box, 12);
    gtk_widget_set_margin_bottom(input_box, 12);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->screen_status));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_box_append(GTK_BOX(box), input_box);

    return box;
}

/*
 * A list in a scrolled window, aligned to the top.
 *
 * GTK_ALIGN_START because a GtkListBox fills its viewport otherwise, so
 * one short row draws a card with several hundred pixels of empty frame
 * under it.
 */
static GtkWidget *
build_list_view(GtkListBox **out, const gchar *empty)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_list_box_new();

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    gtk_widget_set_margin_start(list, 12);
    gtk_widget_set_margin_end(list, 12);
    gtk_widget_set_margin_top(list, 12);
    gtk_widget_set_margin_bottom(list, 12);

    gtk_list_box_set_placeholder(GTK_LIST_BOX(list),
                                 gtk_label_new(empty));

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);

    *out = GTK_LIST_BOX(list);

    return scroll;
}

GtkWidget *
clawt_gtk_build_computer_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *stack = adw_view_stack_new();
    GtkWidget *switcher = adw_view_switcher_new();
    GtkWidget *mounts;
    GtkWidget *exchange;
    guint i;

    self->computer_state = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->computer_state), 12);

    mounts = build_list_view(&self->computer_mount_list,
                             "No mounts of its own. Every computer still "
                             "gets the agent's workspace and the exchange.");
    exchange = build_list_view(&self->exchange_list,
                               "Nothing in the exchange.");

    /*
     * The four, walked rather than listed. `make parity` compares which
     * enumerations each client walks, so a fifth added to the library
     * appears here and in the browser or in neither.
     */
    for (i = 0; i < clawt_computer_view_count(); i++) {
        ClawtComputerView view = clawt_computer_view_nth(i);
        GtkWidget *child;
        AdwViewStackPage *page;

        switch (view) {
        case CLAWT_COMPUTER_VIEW_SHELL:
            child = build_shell_view(self);
            break;

        case CLAWT_COMPUTER_VIEW_SCREEN:
            child = build_screen_view(self);
            break;

        case CLAWT_COMPUTER_VIEW_MOUNTS:
            child = mounts;
            break;

        case CLAWT_COMPUTER_VIEW_EXCHANGE:
            child = exchange;
            break;

        default:
            continue;
        }

        page = adw_view_stack_add_titled(
            ADW_VIEW_STACK(stack), child,
            clawt_computer_view_nth_nick(i),
            clawt_computer_view_nth_label(i));

        if (view == CLAWT_COMPUTER_VIEW_SCREEN)
            self->computer_screen_page = page;
    }

    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher),
                                ADW_VIEW_STACK(stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                 ADW_VIEW_SWITCHER_POLICY_WIDE);
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(switcher, 6);

    g_signal_connect(stack, "notify::visible-child-name",
                     G_CALLBACK(on_computer_view_changed), self);

    gtk_widget_set_vexpand(stack, TRUE);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->computer_state));
    gtk_box_append(GTK_BOX(box), switcher);
    gtk_box_append(GTK_BOX(box), stack);

    self->computer_stack = ADW_VIEW_STACK(stack);

    return box;
}

void
clawt_gtk_stop_watching_screen(ClawtWindow *self)
{
    /*
     * Called when the window closes or its daemon changes.
     *
     * Without it the daemon goes on grabbing for a client that is not
     * there -- the watch is a count, and a count nobody decrements is a
     * screen captured for ever. The agent is the only one who would
     * notice, by being slower.
     */
    screen_set_watching(self, FALSE);

    g_clear_pointer(&self->screen_agent, g_free);
    g_clear_pointer(&self->screen_viewer, g_free);
}
