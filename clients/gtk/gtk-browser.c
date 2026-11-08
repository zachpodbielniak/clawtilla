/*
 * gtk-browser.c - The Computer page's Browser view
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/web/web-browser.c and the Browser view in
 * clients/lrg/lrg-computer.c: the line saying what the agent's browser
 * is on (clawt_browser_session_line()), a picture of it, an address to
 * open, and Stop.  A click on the picture is a click in the page.
 *
 * Every request here is asynchronous.  A frame is a screenshot of a live
 * page and a start waits seconds for an X server, and the synchronous
 * request iterates the main context while it waits -- which is a window
 * that stutters every three seconds for as long as this view is open.
 * One read is in flight at a time, and a generation guards a reply that
 * arrives after the view moved to another agent.
 *
 * The picture is drawn at its own size in a scrolling box, so a click's
 * widget coordinates are the page's CSS pixels with no scaling to undo.
 */

#include "clawt-window-private.h"

#define BROWSER_POLL_SECONDS (3)

static void request_frame(ClawtWindow *self);

static void
on_frame_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = clawt_client_request_finish(CLAWT_CLIENT(source),
                                                            result, &error);
    guint generation = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(source),
                                                          "clawt-browser-generation"));
    JsonObject *payload;
    g_autofree gchar *line = NULL;
    const gchar *encoded;

    self->browser_in_flight = FALSE;

    /* About an agent the view has since left: drop it. */
    if (generation != self->browser_generation || self->browser_line == NULL)
        goto done;

    if (reply == NULL) {
        gtk_label_set_text(self->browser_line, error != NULL ? error->message
                                                            : "The browser did not answer.");
        goto done;
    }

    payload = clawt_payload_of(reply);
    line = clawt_browser_session_line(payload);
    gtk_label_set_text(self->browser_line, line);

    encoded = payload != NULL
        ? json_object_get_string_member_with_default(payload, "png_base64", NULL) : NULL;
    if (encoded != NULL) {
        gsize length = 0;
        guchar *raw = g_base64_decode(encoded, &length);
        g_autoptr(GBytes) bytes = g_bytes_new_take(raw, length);
        g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes(bytes, NULL);

        gtk_picture_set_paintable(self->browser_picture, GDK_PAINTABLE(texture));
    } else {
        gtk_picture_set_paintable(self->browser_picture, NULL);
    }

done:
    if (self->browser_again) {
        self->browser_again = FALSE;
        request_frame(self);
    }
}

static void
request_frame(ClawtWindow *self)
{
    if (self->client == NULL || self->browser_agent == NULL)
        return;
    if (self->browser_in_flight) {
        self->browser_again = TRUE;
        return;
    }
    self->browser_in_flight = TRUE;
    g_object_set_data(G_OBJECT(self->client), "clawt-browser-generation",
                      GUINT_TO_POINTER(self->browser_generation));
    clawt_client_request_async(self->client, "browser.frame",
                               clawt_build_payload("agent", self->browser_agent, NULL),
                               NULL, on_frame_reply, self);
}

static gboolean
on_browser_tick(gpointer user_data)
{
    request_frame(user_data);
    return G_SOURCE_CONTINUE;
}

void
clawt_gtk_browser_set_showing(ClawtWindow *self, gboolean showing)
{
    if (showing && self->browser_tick == 0) {
        self->browser_tick = g_timeout_add_seconds(BROWSER_POLL_SECONDS, on_browser_tick,
                                                   self);
        request_frame(self);
    } else if (!showing && self->browser_tick != 0) {
        g_source_remove(self->browser_tick);
        self->browser_tick = 0;
    }
}

/* An action's answer: say what failed, then read the page again. */
static void
on_action_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = clawt_client_request_finish(CLAWT_CLIENT(source),
                                                            result, &error);

    if (reply == NULL && error != NULL)
        clawt_window_toast(self, error->message);
    request_frame(self);
}

static void
on_open(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *url = gtk_editable_get_text(GTK_EDITABLE(self->browser_entry));

    (void)widget;
    if (self->client == NULL || self->browser_agent == NULL || url == NULL || *url == '\0')
        return;
    gtk_label_set_text(self->browser_line, "Opening\xe2\x80\xa6");
    clawt_client_request_async(self->client, "browser.open",
                               clawt_build_payload("agent", self->browser_agent,
                                                   "url", url, NULL),
                               NULL, on_action_reply, self);
}

static void
on_stop(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;
    if (self->client == NULL || self->browser_agent == NULL)
        return;
    clawt_client_request_async(self->client, "browser.stop",
                               clawt_build_payload("agent", self->browser_agent, NULL),
                               NULL, on_action_reply, self);
}

static void
on_picture_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                   gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    (void)gesture;
    (void)n_press;
    if (self->client == NULL || self->browser_agent == NULL ||
        gtk_picture_get_paintable(self->browser_picture) == NULL)
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, self->browser_agent);
    json_builder_set_member_name(builder, "x");
    json_builder_add_int_value(builder, (gint64)x);
    json_builder_set_member_name(builder, "y");
    json_builder_add_int_value(builder, (gint64)y);
    json_builder_end_object(builder);
    clawt_client_request_async(self->client, "browser.click",
                               json_builder_get_root(builder), NULL, on_action_reply,
                               self);
}

GtkWidget *
clawt_gtk_build_browser_view(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *open = gtk_button_new_with_label("Open");
    GtkWidget *stop = gtk_button_new_with_label("Stop the browser");
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkGesture *click = gtk_gesture_click_new();

    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 12);

    self->browser_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->browser_entry, "https://example.org");
    gtk_widget_set_hexpand(GTK_WIDGET(self->browser_entry), TRUE);
    g_signal_connect(self->browser_entry, "activate", G_CALLBACK(on_open), self);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open), self);
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_widget_set_tooltip_text(stop, "The open page is lost; the profile and its "
                                      "logins stay.");
    g_signal_connect(stop, "clicked", G_CALLBACK(on_stop), self);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->browser_entry));
    gtk_box_append(GTK_BOX(row), open);
    gtk_box_append(GTK_BOX(row), stop);
    gtk_box_append(GTK_BOX(box), row);

    self->browser_line = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(self->browser_line, TRUE);
    gtk_label_set_xalign(self->browser_line, 0.0f);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->browser_line));

    /* At its own size, so a click is in the page's pixels. */
    self->browser_picture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(self->browser_picture, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(self->browser_picture), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(self->browser_picture), GTK_ALIGN_START);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self->browser_picture), "crosshair");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->browser_picture),
                                "Click to click in the page");
    g_signal_connect(click, "pressed", G_CALLBACK(on_picture_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->browser_picture),
                              GTK_EVENT_CONTROLLER(click));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->browser_picture));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    return box;
}

void
clawt_gtk_browser_set_agent(ClawtWindow *self, JsonObject *agent)
{
    const gchar *id = clawt_json_string(agent, "id", NULL);
    gboolean has_browser = g_strcmp0(clawt_json_string(agent, "browser", "none"),
                                     "none") != 0;

    if (self->computer_browser_page != NULL)
        adw_view_stack_page_set_visible(self->computer_browser_page, has_browser);

    if (g_strcmp0(id, self->browser_agent) != 0) {
        g_free(self->browser_agent);
        self->browser_agent = has_browser ? g_strdup(id) : NULL;
        self->browser_generation++;
        if (self->browser_picture != NULL)
            gtk_picture_set_paintable(self->browser_picture, NULL);
        if (self->browser_line != NULL)
            gtk_label_set_text(self->browser_line, "");
        if (self->browser_tick != 0)
            request_frame(self);
    } else if (!has_browser) {
        g_clear_pointer(&self->browser_agent, g_free);
    }
}
