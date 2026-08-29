/*
 * gtk-chat.c - The chat page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The transcript, the composer, what a message and a conversation can
 * be made to do, and the slash commands the entry answers.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include <string.h>

static void         on_send(GtkWidget *widget, gpointer user_data);

/* ── Chat ────────────────────────────────────────────────────────── */

/*
 * Shows or hides the activity line.
 *
 * The spinner is stopped when hidden rather than left running: a
 * GtkSpinner that is not visible still drives a frame clock, and there
 * is one per window for the life of the process.
 */
void
clawt_gtk_set_activity(ClawtWindow *self, const gchar *text)
{
    if (self->activity_bar == NULL)
        return;

    if (text == NULL) {
        gtk_spinner_stop(self->activity_spinner);
        gtk_widget_set_visible(self->activity_bar, FALSE);
        gtk_label_set_text(self->streaming, "");
        return;
    }

    gtk_label_set_text(self->streaming, text);
    gtk_widget_set_visible(self->activity_bar, TRUE);
    gtk_spinner_start(self->activity_spinner);
}

/*
 * Shows or hides Stop, and says why it is not offered.
 *
 * Visible only while there is a turn to stop.  A button that is present
 * whether or not it can do anything is one people stop reading, and the
 * question it answers -- "is something running that I can end?" -- is
 * exactly the one its presence already answers.
 *
 * @caps is the agent's own capability list rather than a guess from the
 * runtime type: an embedded agent takes its turn inside the daemon,
 * where the only process to signal is the daemon itself, so it declares
 * no `interrupt` and must not be offered a button that would refuse.
 */
void
clawt_gtk_sync_stop_turn(ClawtWindow *self, gboolean busy)
{
    if (self->stop_turn == NULL)
        return;

    gtk_widget_set_visible(self->stop_turn,
                           busy && self->selected_can_interrupt);
}

/*
 * Kills what the agent is doing, and leaves the agent running.
 *
 * Deliberately not `agent.stop`: that takes the agent down and needs a
 * start afterwards, along with its container or VM.  This ends the work
 * and leaves the conversation where it was, which is what somebody
 * watching an agent go the wrong way actually wants.
 */
static gchar *
interrupt_selected(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL)
        return NULL;

    reply = clawt_window_request(
        self, "agent.interrupt",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return NULL;

    /*
     * Hidden straight away rather than waiting for the next typing
     * frame.  The turn was just taken out from under the code that
     * lowers the indicator, so the frame may never come -- and a Stop
     * button still sitting there after a successful stop reads as the
     * press having failed.
     */
    clawt_gtk_sync_stop_turn(self, FALSE);
    clawt_gtk_set_activity(self, NULL);

    /*
     * The count is the answer.  "Stopped" over an agent that was between
     * turns claims something happened that did not, and the next thing
     * it says would then read as it ignoring the button.
     */
    {
        JsonObject *payload = clawt_payload_of(reply);
        gint64 killed = (payload != NULL &&
                         json_object_has_member(payload, "killed"))
                        ? json_object_get_int_member(payload, "killed") : 0;

        if (killed > 0)
            return g_strdup_printf("Stopped %s: %" G_GINT64_FORMAT
                                   " process(es) ended.",
                                   self->selected_agent, killed);

        return g_strdup_printf("%s was between turns -- nothing was running "
                               "to stop.", self->selected_agent);
    }
}

static void
on_stop_turn(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *said = NULL;

    (void)widget;

    said = interrupt_selected(self);

    if (said != NULL)
        clawt_window_toast(self, said);
}

/*
 * Sets a label from markdown, falling back to plain text.
 *
 * Pango refuses unbalanced markup and a GtkLabel handed something it
 * cannot parse renders *nothing* -- a message that silently disappears
 * is far worse than one that renders without its bold. So the markup is
 * checked before it is used, and anything that fails goes up as the
 * text it came from.
 */

/* ── Context menus ───────────────────────────────────────────────── */

/*
 * Attaches a right-click menu to a widget.
 *
 * GtkPopoverMenu wants a GMenuModel and an action group, which is a lot
 * of ceremony for six entries whose targets change per widget. A
 * GtkListBox in a plain popover is less machinery and behaves the same
 * way to the person using it -- and a popover is fine here, unlike one
 * parented to a GtkEntry, because it hangs off a container.
 */
typedef struct {
    const gchar *label;
    const gchar *action;
} MenuEntry;

typedef void (*MenuChosen)(ClawtWindow *self, const gchar *action,
                           gpointer target);

typedef struct {
    ClawtWindow *window;
    GtkWidget   *popover;
    MenuChosen   chosen;
    gpointer     target;      /* borrowed; owned by the widget it hangs off */
} ContextMenu;

static void
context_menu_free(gpointer data)
{
    ContextMenu *menu = data;

    /*
     * The popover is usually already gone by the time this runs.
     *
     * It is a child of the widget this is attached to, and GTK
     * unparents a widget's children while destroying it -- which
     * happens before object data is released. So this ran on a pointer
     * to a finalized widget and asserted once per message, every time a
     * transcript was cleared: fourteen messages, fourteen criticals.
     *
     * The weak pointer is what makes "already gone" tell the truth
     * instead of dangling. The unparent is kept for the other way this
     * can be reached -- the data being replaced on a widget that is
     * still alive.
     */
    if (menu->popover != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(menu->popover),
                                     (gpointer *)&menu->popover);

        if (gtk_widget_get_parent(menu->popover) != NULL)
            gtk_widget_unparent(menu->popover);
    }

    g_free(menu);
}

static void
on_context_chosen(GtkButton *button, gpointer user_data)
{
    ContextMenu *menu = user_data;
    const gchar *action = g_object_get_data(G_OBJECT(button), "action");

    gtk_popover_popdown(GTK_POPOVER(menu->popover));

    if (action != NULL)
        menu->chosen(menu->window, action, menu->target);
}

static void
on_context_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                   gdouble y, gpointer user_data)
{
    ContextMenu *menu = user_data;
    GdkRectangle at;

    (void)gesture;
    (void)n_press;

    at.x = (gint)x;
    at.y = (gint)y;
    at.width = 1;
    at.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(menu->popover), &at);
    gtk_popover_popup(GTK_POPOVER(menu->popover));
}

/*
 * Takes the popover off its owner while the owner is still a widget.
 *
 * A popover given a parent by hand is a child that parent knows nothing
 * about, so nothing else will remove it.
 */
static void
on_menu_owner_destroyed(GtkWidget *widget, gpointer user_data)
{
    ContextMenu *menu = user_data;

    (void)widget;

    if (menu->popover != NULL &&
        gtk_widget_get_parent(menu->popover) != NULL)
        gtk_widget_unparent(menu->popover);
}

static void
add_context_menu(ClawtWindow *self, GtkWidget *widget,
                 const MenuEntry *entries, gsize n_entries,
                 MenuChosen chosen, gpointer target)
{
    ContextMenu *menu = g_new0(ContextMenu, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkGesture *gesture;
    gsize i;

    menu->window = self;
    menu->chosen = chosen;
    menu->target = target;

    /*
     * Buttons rather than a GtkListBox.
     *
     * A list box selects a row when it takes focus, and a popover takes
     * focus the moment it opens -- so right-clicking a message ran the
     * first entry immediately and copied it without being asked. A
     * button does nothing until it is clicked, which is the entire
     * behaviour wanted here.
     */
    for (i = 0; i < n_entries; i++) {
        GtkWidget *item;

        /* A NULL label is a separator in the table. */
        if (entries[i].label == NULL) {
            item = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(item, 3);
            gtk_widget_set_margin_bottom(item, 3);
            gtk_box_append(GTK_BOX(box), item);
            continue;
        }

        item = gtk_button_new_with_label(entries[i].label);
        gtk_widget_add_css_class(item, "flat");
        gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
        gtk_widget_set_halign(item, GTK_ALIGN_FILL);

        /* Left-aligned, like every other menu on the desktop. */
        gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(item))),
                             0.0f);

        g_object_set_data_full(G_OBJECT(item), "action",
                               g_strdup(entries[i].action), g_free);
        g_signal_connect(item, "clicked", G_CALLBACK(on_context_chosen), menu);
        gtk_box_append(GTK_BOX(box), item);
    }

    menu->popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(menu->popover), FALSE);
    gtk_popover_set_child(GTK_POPOVER(menu->popover), box);
    gtk_widget_set_parent(menu->popover, widget);
    g_object_add_weak_pointer(G_OBJECT(menu->popover),
                              (gpointer *)&menu->popover);

    gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_context_pressed), menu);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

    /*
     * Unparented from ::destroy, not from the object data's notify.
     *
     * qdata is cleared in finalize, and gtk_widget_finalize() checks for
     * leftover children *before* chaining up to it -- so the notify ran
     * too late and every chip in a cleared transcript warned "Finalizing
     * GtkButton, but it still has children left: GtkPopover". ::destroy
     * is emitted from dispose, which is early enough for the child to be
     * gone before anything counts them.
     */
    g_signal_connect(widget, "destroy", G_CALLBACK(on_menu_owner_destroyed),
                     menu);

    g_object_set_data_full(G_OBJECT(widget), "context-menu", menu,
                           context_menu_free);
}

/* ── What the menus do ───────────────────────────────────────────── */

static void
copy_text(ClawtWindow *self, const gchar *text, const gchar *what)
{
    g_autofree gchar *message = NULL;

    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)),
                           text != NULL ? text : "");

    message = g_strdup_printf("%s copied.", what);
    clawt_window_toast(self, message);
}

/*
 * Converts and copies, saying so when the format is not available.
 *
 * Silently handing back markdown under an org label would be the worst
 * of the three possible answers.
 */
static void
copy_as(ClawtWindow *self, const gchar *markdown, ClawtExportFormat format,
        const gchar *what)
{
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;

    converted = clawt_export_convert(markdown, format, &error);

    if (converted == NULL) {
        clawt_window_toast(self, error->message);
        return;
    }

    copy_text(self, converted, what);
}

/*
 * The whole conversation as a markdown document.
 *
 * Read back from the daemon rather than scraped off the widgets: the
 * transcript on screen is the last two hundred messages, and an export
 * that quietly stopped there would be a export of the window rather
 * than of the conversation.
 */
static gchar *
conversation_markdown(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) messages = NULL;
    JsonArray *array;
    guint i;

    if (self->selected_agent == NULL)
        return NULL;

    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", self->selected_agent, "as", "user",
                            "limit", "5000", NULL));

    if (reply == NULL)
        return NULL;

    array = json_object_get_array_member(clawt_payload_of(reply), "messages");
    messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonObject *one = json_array_get_object_element(array, i);
        ClawtMessage *message = clawt_message_new(
            self->selected_room, clawt_json_string(one, "sender", "?"),
            clawt_json_string(one, "body", ""));

        clawt_message_set_timestamp(message, clawt_json_int(one, "ts", 0));
        g_ptr_array_add(messages, message);
    }

    return clawt_export_transcript(self->selected_agent, messages,
                                   CLAWT_EXPORT_MARKDOWN, NULL);
}

static ClawtExportFormat
format_from_action(const gchar *action)
{
    if (g_str_has_suffix(action, "org"))
        return CLAWT_EXPORT_ORG;

    if (g_str_has_suffix(action, "text"))
        return CLAWT_EXPORT_PLAIN;

    return CLAWT_EXPORT_MARKDOWN;
}

static void
on_conversation_saved(GObject *source, GAsyncResult *result, gpointer data)
{
    ClawtWindow *self = g_object_get_data(source, "window");
    g_autoptr(GFile) file = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *contents = data;

    file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result,
                                       &error);

    if (file == NULL) {
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR,
                             GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    if (!g_file_replace_contents(file, contents, strlen(contents), NULL,
                                 FALSE, G_FILE_CREATE_NONE, NULL, NULL,
                                 &error))
        clawt_window_toast(self, error->message);
    else
        clawt_window_toast(self, "Saved.");

    g_object_unref(self);
}

static void
save_document(ClawtWindow *self, const gchar *contents, const gchar *basename,
              ClawtExportFormat format)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    g_autofree gchar *suggested = g_strconcat(
        basename, clawt_export_format_extension(format), NULL);

    gtk_file_dialog_set_title(dialog, "Save");
    gtk_file_dialog_set_initial_name(dialog, suggested);

    /*
     * The contents travel with the dialog rather than being re-derived
     * in the callback: by the time somebody has picked a filename the
     * conversation may have moved on, and saving something other than
     * what they asked for is worse than not saving.
     */
    g_object_set_data_full(G_OBJECT(dialog), "window", g_object_ref(self),
                           g_object_unref);
    gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_conversation_saved,
                         g_strdup(contents));
    g_object_unref(dialog);
}

static void
open_document_in_editor(ClawtWindow *self, const gchar *contents,
                        const gchar *basename, ClawtExportFormat format)
{
    g_autofree gchar *template = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    gint fd;

    template = g_strdup_printf("%s-XXXXXX%s", basename,
                               clawt_export_format_extension(format));
    fd = g_file_open_tmp(template, &path, &error);

    if (fd < 0) {
        clawt_window_toast(self, error->message);
        return;
    }

    close(fd);

    if (!g_file_set_contents(path, contents, -1, &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    /*
     * Not deleted afterwards, unlike the message composer's scratch
     * file: this is somebody taking a conversation away to keep, and
     * removing it the moment their editor exits would throw away what
     * they went to get.
     */
    clawt_gtk_open_path_in_editor(self, path, basename);
}

static void
on_conversation_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    g_autofree gchar *markdown = NULL;
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;
    ClawtExportFormat format = format_from_action(action);

    (void)target;

    markdown = conversation_markdown(self);

    if (markdown == NULL) {
        clawt_window_toast(self, "there is no conversation to export");
        return;
    }

    converted = clawt_export_convert(markdown, format, &error);

    if (converted == NULL) {
        clawt_window_toast(self, error->message);
        return;
    }

    if (g_str_has_prefix(action, "copy"))
        copy_text(self, converted, "Conversation");
    else if (g_str_has_prefix(action, "edit"))
        open_document_in_editor(self, converted, self->selected_agent, format);
    else if (g_str_has_prefix(action, "save"))
        save_document(self, converted, self->selected_agent, format);
}

static void
on_message_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    const gchar *body = target;

    if (g_strcmp0(action, "copy-markdown") == 0) {
        copy_text(self, body, "Message");
        return;
    }

    copy_as(self, body, format_from_action(action), "Message");
}

/* ── Attachment previews in the transcript ───────────────────────── */

/*
 * The marker body_with_attachments() writes.
 *
 * The transcript is rebuilt from what the daemon stored, so the only
 * way back to "this message had a picture on it" is to recognise the
 * line we wrote. Matched on the prefix rather than the whole sentence,
 * so rewording the guidance does not silently turn previews off.
 */
#define ATTACHMENT_MARKER CLAWT_ATTACHMENT_MARKER

/*
 * Brings an agent-sent file to this machine, once.
 *
 * An attachment the *operator* sent is a path on this host, because the
 * client put it there.  One an *agent* sent is a `clawt:<id>` naming a
 * copy the daemon took at send time -- and the daemon may be on another
 * machine, which is the whole reason the bytes travel rather than the
 * path.  Cached under the user's cache directory so a transcript
 * redrawn on every fleet event does not re-fetch every picture in it.
 *
 * Returns: (transfer full) (nullable): a local path, or %NULL
 */
static gchar *
fetch_attachment(ClawtWindow *self, const gchar *id)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *encoded;
    guchar *bytes;
    gsize length = 0;

    if (id == NULL || *id == '\0')
        return NULL;

    dir = g_build_filename(g_get_user_cache_dir(), "clawtilla",
                           "attachments", NULL);
    path = g_build_filename(dir, id, NULL);

    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&path);

    reply = clawt_window_request(self, "attachment.get",
                                 clawt_build_payload("id", id, NULL));

    if (reply == NULL)
        return NULL;

    encoded = clawt_json_string(clawt_payload_of(reply), "base64", NULL);

    if (encoded == NULL)
        return NULL;

    if (g_mkdir_with_parents(dir, 0700) != 0)
        return NULL;

    bytes = g_base64_decode(encoded, &length);

    if (!g_file_set_contents(path, (const gchar *)bytes, (gssize)length,
                             NULL)) {
        g_free(bytes);
        return NULL;
    }

    g_free(bytes);

    return g_steal_pointer(&path);
}

static gboolean
looks_like_an_image(const gchar *path)
{
    static const gchar *extensions[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", NULL
    };
    g_autofree gchar *lowered = g_ascii_strdown(path, -1);
    gsize i;

    for (i = 0; extensions[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, extensions[i]))
            return TRUE;
    }

    return FALSE;
}

static gboolean
on_preview_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;

    if (keyval != GDK_KEY_Escape)
        return GDK_EVENT_PROPAGATE;

    gtk_window_destroy(GTK_WINDOW(user_data));
    return GDK_EVENT_STOP;
}

/*
 * Hands a file to the desktop -- xdg-open's job.
 *
 * GtkFileLauncher rather than a spawn, so this goes through the
 * portal when there is one and through the mime handler when there is
 * not, which is what a person means by "open it".
 */
static void
open_with_desktop(ClawtWindow *self, const gchar *path)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);

    gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
}

static void
on_attachment_saved(GObject *source, GAsyncResult *result, gpointer data)
{
    ClawtWindow *self = g_object_get_data(source, "window");
    g_autoptr(GFile) target = NULL;
    g_autoptr(GFile) from = g_file_new_for_path(data);
    g_autoptr(GError) error = NULL;

    g_free(data);

    target = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result,
                                         &error);

    if (target == NULL) {
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR,
                             GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    if (!g_file_copy(from, target, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                     &error))
        clawt_window_toast(self, error->message);
    else
        clawt_window_toast(self, "Saved.");

    g_object_unref(self);
}

static void
on_attachment_delete_confirmed(AdwAlertDialog *dialog, const gchar *response,
                               gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *path = g_object_get_data(G_OBJECT(dialog), "path");
    g_autofree gchar *name = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "delete") != 0)
        return;

    name = g_path_get_basename(path);

    /*
     * Through the daemon, which owns the exchange directory and checks
     * the name, rather than unlinking from here -- a client that
     * deletes by path is a client that can be asked to delete any path.
     */
    reply = clawt_window_request(
        self, "attachment.remove",
        clawt_build_payload("agent", self->selected_agent, "name", name,
                            NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Deleted. The message still names it.");
    clawt_gtk_load_history(self);
}

static void
on_attachment_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    const gchar *path = target;

    if (g_strcmp0(action, "open") == 0) {
        open_with_desktop(self, path);
        return;
    }

    if (g_strcmp0(action, "copy-path") == 0) {
        copy_text(self, path, "Path");
        return;
    }

    if (g_strcmp0(action, "save") == 0) {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        g_autofree gchar *name = g_path_get_basename(path);

        gtk_file_dialog_set_title(dialog, "Save a copy");
        gtk_file_dialog_set_initial_name(dialog, name);
        g_object_set_data_full(G_OBJECT(dialog), "window",
                               g_object_ref(self), g_object_unref);
        gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL,
                             on_attachment_saved, g_strdup(path));
        g_object_unref(dialog);
        return;
    }

    if (g_strcmp0(action, "delete") == 0) {
        AdwAlertDialog *dialog;
        g_autofree gchar *name = g_path_get_basename(path);
        g_autofree gchar *body = g_strdup_printf(
            "\xe2\x80\x9c%s\xe2\x80\x9d will be deleted from %s's exchange "
            "directory. This cannot be undone, and the message will still "
            "name the file.", name, self->selected_agent);

        dialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Delete this file?", body));
        adw_alert_dialog_add_responses(dialog, "cancel", "Cancel",
                                       "delete", "Delete", NULL);
        adw_alert_dialog_set_response_appearance(dialog, "delete",
                                                 ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_default_response(dialog, "cancel");
        g_object_set_data_full(G_OBJECT(dialog), "path", g_strdup(path),
                               g_free);
        g_signal_connect(dialog, "response",
                         G_CALLBACK(on_attachment_delete_confirmed), self);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
    }
}

static const MenuEntry attachment_menu[] = {
    { "Open",              "open" },
    { "Save a copy\xe2\x80\xa6", "save" },
    { "Copy path",         "copy-path" },
    { NULL,                NULL },
    { "Delete permanently", "delete" }
};

/*
 * Opens one attachment full size.
 *
 * A window rather than a dialog: this is something to look at beside
 * the conversation, and a modal one would stop you reading the message
 * it came with.
 */
static void
on_preview_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *path = g_object_get_data(G_OBJECT(button), "path");
    GtkWidget *window;
    GtkWidget *scroll;
    GtkWidget *picture;
    GtkEventController *keys;
    g_autofree gchar *title = NULL;

    if (path == NULL)
        return;

    title = g_path_get_basename(path);

    window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(self));
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);

    picture = gtk_picture_new_for_filename(path);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);

    /*
     * SCALE_DOWN, not CONTAIN: a big screenshot shrinks to fit, and a
     * small image is shown at its own size rather than blown up to fill
     * the window, which is what CONTAIN does and it looks like a
     * mistake.
     */
    gtk_picture_set_content_fit(GTK_PICTURE(picture),
                                GTK_CONTENT_FIT_SCALE_DOWN);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), picture);
    gtk_window_set_child(GTK_WINDOW(window), scroll);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_preview_key), window);
    gtk_widget_add_controller(window, keys);

    gtk_window_present(GTK_WINDOW(window));
}

/*
 * A file that is not an image: a name, a size, and the same menu.
 *
 * Clicking opens it with the desktop's handler, which is what makes a
 * PDF in a conversation something you can read rather than a path you
 * have to go and find.
 */
static void
on_file_chip_clicked(GtkButton *button, gpointer user_data)
{
    open_with_desktop(user_data, g_object_get_data(G_OBJECT(button), "path"));
}

static void
append_file_chip(ClawtWindow *self, GtkWidget *row, const gchar *path,
                 gboolean from_user)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = NULL;
    g_autofree gchar *name = g_path_get_basename(path);
    g_autofree gchar *label = NULL;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *icon;

    info = g_file_query_info(file,
                             G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                             G_FILE_ATTRIBUTE_STANDARD_ICON,
                             G_FILE_QUERY_INFO_NONE, NULL, NULL);

    if (info != NULL) {
        g_autofree gchar *size = g_format_size(
            (guint64)g_file_info_get_size(info));

        label = g_strdup_printf("%s  \xc2\xb7  %s", name, size);
    } else {
        label = g_strdup(name);
    }

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /*
     * The icon the desktop would use for this type, so a PDF looks like
     * a PDF rather than like every other attachment.
     */
    icon = (info != NULL && g_file_info_get_icon(info) != NULL)
           ? gtk_image_new_from_gicon(g_file_info_get_icon(info))
           : gtk_image_new_from_icon_name("text-x-generic-symbolic");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));

    button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_add_css_class(button, "card");
    gtk_widget_set_halign(button,
                          from_user ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(button, path);
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_file_chip_clicked),
                     self);

    add_context_menu(self, button, attachment_menu,
                     G_N_ELEMENTS(attachment_menu), on_attachment_action,
                     g_object_get_data(G_OBJECT(button), "path"));

    gtk_box_append(GTK_BOX(row), button);
}

/*
 * A thumbnail under any message that carried a picture.
 *
 * So the conversation still shows what was sent when you scroll back to
 * it -- a path in a body is a thing nobody remembers the content of ten
 * minutes later.
 */
static void
append_attachment_previews(ClawtWindow *self, GtkWidget *row,
                           const gchar *body, gboolean from_user)
{
    g_auto(GStrv) lines = NULL;
    gboolean in_block = FALSE;
    gsize i;

    if (body == NULL || strstr(body, ATTACHMENT_MARKER) == NULL)
        return;

    lines = g_strsplit(body, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        g_autofree gchar *candidate = NULL;
        const gchar *start;
        GtkWidget *button;
        GtkWidget *picture;

        if (strstr(lines[i], ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block)
            continue;

        /*
         * The block is its list items and their indented continuation
         * lines. Anything at the left margin ends it, so a path an
         * agent quotes further down the message does not sprout a
         * preview of its own.
         *
         * Leaving on "no slash on this line" was wrong and cost the
         * first preview: every entry starts with a name line, which has
         * no slash, and that ended the block before the path beneath it
         * was ever looked at.
         */
        if (lines[i][0] != '\0' && lines[i][0] != ' ' &&
            lines[i][0] != '-' && lines[i][0] != '\t') {
            in_block = FALSE;
            continue;
        }

        /*
         * An agent's attachment is `clawt:<id>`, not a path: the file
         * lives in the daemon's keeping and may be on another machine.
         * Fetched to a local cache and then treated exactly like one the
         * operator attached, so there is one preview path rather than
         * two to disagree.
         */
        start = strstr(lines[i], "clawt:");

        if (start != NULL) {
            g_autofree gchar *id = g_strdup(start + strlen("clawt:"));

            g_strchomp(id);
            candidate = fetch_attachment(self, id);

            if (candidate == NULL)
                continue;
        } else {
            start = strchr(lines[i], '/');

            if (start == NULL)
                continue;

            candidate = g_strdup(start);
            g_strchomp(candidate);
        }

        /* The container path is in brackets; the host one is not. */
        if (g_str_has_suffix(candidate, ")"))
            continue;

        if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
            continue;

        /*
         * Anything that is not an image gets a chip instead of a
         * thumbnail: a name, a size and the same menu. A PDF in a
         * conversation was previously a path and nothing else.
         */
        if (!looks_like_an_image(candidate)) {
            append_file_chip(self, row, candidate, from_user);
            continue;
        }

        /*
         * Scaled on load rather than shrunk in the widget.
         *
         * A size request is a *minimum*: asking for 160 high and
         * handing GtkPicture a 2000-pixel screenshot gets a 2000-pixel
         * screenshot. Decoding straight to thumbnail size also means a
         * transcript full of images does not hold every one of them in
         * memory at full resolution.
         */
        /*
         * Scaled during the decode, not by the widget.
         *
         * GTK has no maximum-size property: a size request is a
         * *minimum*, and a GtkPicture takes its natural size from the
         * paintable, so handing it a full screenshot gives a full-size
         * thumbnail however the widget is configured. Decoding straight
         * to thumbnail size settles it, and a transcript full of images
         * then does not hold every one at full resolution either.
         */
        {
            g_autoptr(GdkPixbuf) scaled = NULL;
            g_autoptr(GdkTexture) texture = NULL;
            g_autoptr(GBytes) pixels = NULL;
            g_autoptr(GError) error = NULL;

            /*
             * Big enough to actually see, the way a chat client shows
             * one. A hundred and sixty pixels was a postage stamp.
             */
            scaled = gdk_pixbuf_new_from_file_at_scale(candidate, 460, 320,
                                                        TRUE, &error);

            if (scaled == NULL) {
                /* Not an image after all, or one we cannot decode. */
                g_info("no preview for %s: %s", candidate, error->message);
                continue;
            }

            /*
             * A memory texture from the pixbuf's own pixels.
             * gdk_texture_new_for_pixbuf() would say this in one line
             * and is deprecated.
             */
            pixels = g_bytes_new(gdk_pixbuf_get_pixels(scaled),
                                 gdk_pixbuf_get_byte_length(scaled));
            texture = gdk_memory_texture_new(
                gdk_pixbuf_get_width(scaled),
                gdk_pixbuf_get_height(scaled),
                gdk_pixbuf_get_has_alpha(scaled)
                    ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                pixels, (gsize)gdk_pixbuf_get_rowstride(scaled));

            picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
        }

        /*
         * can-shrink off.
         *
         * It defaults on, which makes a GtkPicture's *minimum* width
         * zero -- so anything in the ancestry doing a height-for-width
         * pass is free to squeeze it to nothing, and it did: the
         * thumbnail rendered a few dozen pixels wide. The texture is
         * already exactly the size it should be drawn at, so shrinking
         * it is never the right answer.
         */
        gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
        gtk_widget_set_valign(picture, GTK_ALIGN_START);

        gtk_widget_set_halign(picture,
                              from_user ? GTK_ALIGN_END : GTK_ALIGN_START);

        button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), picture);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button,
                              from_user ? GTK_ALIGN_END : GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(button, "Click to see it full size");
        g_object_set_data_full(G_OBJECT(button), "path",
                               g_strdup(candidate), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_preview_clicked),
                         self);

        add_context_menu(self, button, attachment_menu,
                         G_N_ELEMENTS(attachment_menu),
                         on_attachment_action,
                         g_object_get_data(G_OBJECT(button), "path"));

        gtk_box_append(GTK_BOX(row), button);
    }
}

/*
 * The chat column's geometry, in one place.
 *
 * Every row in the transcript is inset from the clamp by CHAT_ROW_MARGIN
 * on both sides, and an agent's body is indented past its avatar by a
 * further CHAT_GUTTER.  CHAT_BODY_INSET is where a body therefore
 * starts, and the composer below uses it so that the entry's frame and
 * the text above it stand on the same line.
 *
 * They are constants rather than three literals because the composer is
 * the only thing here that has to agree with a number it does not draw:
 * a literal 56 would be a number nobody could trace back to the two it
 * came from, and it would go stale the first time either changed.
 */
#define CHAT_ROW_MARGIN  12
#define CHAT_AVATAR      32
#define CHAT_GUTTER      (CHAT_AVATAR + CHAT_ROW_MARGIN)

/*
 * The three breaks a reader sees, in one unit and in the right order.
 *
 * Measured against real GTK4 at the default font: a line is 18px, and a
 * markdown paragraph break is one blank line, so 18px of clear space.
 * A new message inside a run used to get 6, which put it at a third of
 * a paragraph -- so three consecutive turns read as one message with
 * unusually tight paragraphs, and the grouping said the opposite of
 * what it meant.
 *
 *   paragraph inside a message   18   (markdown's, not ours)
 *   a new message inside a run   24
 *   a new run                    30
 *
 * Even 6px steps, and 6 is a third of a line and the number the file
 * already used.  The window was 19 to 29: below 19 a message reads as a
 * paragraph, at 30 it reads as a new run and the grouping carries no
 * information at all.
 *
 * Space alone is not enough to carry it -- 6px between two of these
 * steps is perceptible but not *nameable* -- so a continuation row also
 * puts its time in the gutter, which is otherwise empty on those rows.
 * That is the signal that says "new message" rather than "new
 * paragraph", and it costs nothing: the gutter is 44px and a caption
 * time measures 31 to 34.
 */
#define CHAT_PARAGRAPH_SPACING 18
#define CHAT_MESSAGE_SPACING   24
#define CHAT_RUN_SPACING       30

/*
 * The two chat measurements a person may set, resolved against what the
 * client ships.
 *
 * Zero on the appearance means defer, exactly as an unset font does --
 * so the shipped value lives here and in clawt-chat-layout.h rather
 * than being written into anybody's appearance file, where it would
 * freeze the moment they opened the dialog.
 *
 * AdwClamp takes a property rather than a stylesheet, which is why this
 * reads the value instead of letting CSS do it. The web client reads
 * the same field and renders it as a custom property; the number is
 * shared and the mechanism is not, like the palette.
 */
/*
 * The rendered width of one character, for a measure counted in them.
 *
 * Taken from the transcript's own Pango context rather than from a
 * constant, so a reader who asks for 80 characters and then changes
 * their font size still gets 80 -- which is the whole reason a
 * character count is worth offering beside a pixel width.
 *
 * It measures the digit zero, because that is what CSS defines `ch` as
 * and `ch` is what the web client renders a character count with.  The
 * obvious alternative, pango_font_metrics_get_approximate_char_width(),
 * is an *average* over the font, and measured here against real GTK4 in
 * Adwaita Sans the two differ by 29%: 7.156px against 9.253px, so "80
 * characters a line" would have been 572px in this client and 742px in
 * the browser.  One setting, two clients, two different columns, and
 * nothing anywhere to say so -- which is the drift `make parity`
 * exists for, at a layer no check of it can see.
 *
 * The unrounded logical extent, not pango_layout_get_pixel_size():
 * that rounds 9.253 to 10, which is 8% per character and about sixty
 * pixels across a column.
 *
 * Zero when there is no context yet; clawt_measure_resolve_px() floors
 * it, so a clamp built before realisation holds something sensible.
 */
static gdouble
chat_char_width(ClawtWindow *self)
{
    PangoContext *context;
    g_autoptr(PangoLayout) layout = NULL;
    PangoRectangle logical;

    if (self == NULL || self->transcript == NULL)
        return 0.0;

    context = gtk_widget_get_pango_context(GTK_WIDGET(self->transcript));

    if (context == NULL)
        return 0.0;

    layout = pango_layout_new(context);
    pango_layout_set_text(layout, "0", -1);
    pango_layout_get_extents(layout, NULL, &logical);

    return logical.width / (gdouble)PANGO_SCALE;
}

/*
 * How wide a scrolled window's viewport is, from its own adjustment.
 *
 * Zero for a page the stack has never shown, which
 * clawt_measure_resolve_px() answers with the reference column -- and
 * the adjustment notifies the moment that page is allocated, so the
 * real number arrives before anybody looks at it.
 */
static gint
viewport_width(GtkWidget *scroll)
{
    GtkAdjustment *adjustment;

    if (scroll == NULL)
        return 0;

    adjustment = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(scroll));

    return adjustment != NULL
           ? (gint)gtk_adjustment_get_page_size(adjustment) : 0;
}

static gint
chat_measure_for(ClawtWindow *self, gint available)
{
    ClawtMeasureUnit unit = CLAWT_MEASURE_DEFAULT;
    gint amount = 0;

    if (self != NULL && self->appearance != NULL) {
        unit = clawt_appearance_get_measure_unit(self->appearance);
        amount = clawt_appearance_get_measure_amount(self->appearance);
    }

    /*
     * The inset is the transcript's, not the composer's, even though
     * both clamps take this number: a character count is about the
     * words, and the words start past the avatar gutter.  The composer
     * is already inset by the same amount for the same reason, so one
     * answer keeps the two columns on one line.
     */
    return clawt_measure_resolve_px(
        unit, amount, available, chat_char_width(self),
        clawt_chat_body_inset(CHAT_ROW_MARGIN, CHAT_GUTTER)
            + CHAT_ROW_MARGIN);
}

gint
clawt_gtk_chat_measure(ClawtWindow *self)
{
    return chat_measure_for(self,
                            viewport_width(self != NULL ? self->chat_scroll
                                                        : NULL));
}

static void
apply_measure(GtkWidget *clamp, gint measure)
{
    /*
     * Setting the same value again is skipped: a clamp's maximum-size
     * is a layout input, and writing it from inside an allocation that
     * did not need it is work every frame for no change on screen.
     */
    if (clamp == NULL)
        return;

    if (adw_clamp_get_maximum_size(ADW_CLAMP(clamp)) == measure)
        return;

    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), measure);
}

/*
 * Pushes the resolved measure onto every clamp that carries it.
 *
 * Called from the settings page and from each scrolled window's own
 * adjustment, because a share of the window has to be recomputed when
 * the window changes and AdwClamp takes a property rather than a
 * stylesheet -- so nothing recomputes it on its own.
 *
 * Flow resolves against its own viewport rather than the chat's.  They
 * are pages of one stack and so are almost always the same width, but
 * an unshown page is allocated nothing at all -- so sharing a number
 * would mean whichever page somebody opened first decided the column
 * for the other.
 */
void
clawt_gtk_push_chat_measure(ClawtWindow *self)
{
    gint measure;

    if (self == NULL)
        return;

    measure = clawt_gtk_chat_measure(self);
    apply_measure(self->transcript_clamp, measure);
    apply_measure(self->composer_clamp, measure);

    if (self->flow_clamp != NULL)
        apply_measure(self->flow_clamp,
                      chat_measure_for(
                          self,
                          viewport_width(GTK_WIDGET(self->flow_scroll))));

    if (self->decision_clamp != NULL)
        apply_measure(self->decision_clamp,
                      chat_measure_for(
                          self,
                          viewport_width(
                              GTK_WIDGET(self->decision_scroll))));
}

static void
on_viewport_width(GObject *adjustment, GParamSpec *spec, gpointer user_data)
{
    (void)adjustment;
    (void)spec;

    clawt_gtk_push_chat_measure(user_data);
}

/*
 * Follows a scrolled window's width for as long as it exists.
 *
 * The adjustment is fetched once and connected to; GtkScrolledWindow
 * only replaces it if somebody calls set_hadjustment, which nothing
 * here does.
 */
void
clawt_gtk_follow_viewport_width(ClawtWindow *self, GtkWidget *scroll)
{
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(scroll));

    if (adjustment == NULL)
        return;

    g_signal_connect_object(adjustment, "notify::page-size",
                            G_CALLBACK(on_viewport_width), self, 0);
}

static gint
chat_run_spacing(ClawtWindow *self)
{
    gint set = (self != NULL && self->appearance != NULL)
               ? clawt_appearance_get_run_spacing(self->appearance) : 0;

    return set > 0 ? set : CHAT_RUN_SPACING;
}

/*
 * A new message inside a run: a third of a line below the run gap, and
 * never at or below a paragraph.
 *
 * Derived rather than a second setting, so a reader who changes the run
 * gap keeps the ordering.  Setting it to 20 with a fixed message gap of
 * 24 would put a new message *further* apart than a new run, which is
 * the same inversion this fixes, arrived at from the other side.
 */
static gint
chat_message_spacing(ClawtWindow *self)
{
    gint run = chat_run_spacing(self);
    gint step = CHAT_RUN_SPACING - CHAT_MESSAGE_SPACING;
    gint gap = run - step;

    if (gap <= CHAT_PARAGRAPH_SPACING)
        gap = CHAT_PARAGRAPH_SPACING + 1;

    return MIN(gap, run);
}
#define CHAT_BODY_INSET  clawt_chat_body_inset(CHAT_ROW_MARGIN, CHAT_GUTTER)

/*
 * "Today", "Yesterday", or "Wednesday 25 August".
 *
 * A date change is a bigger break than a speaker change, so it gets more
 * room than the gap it sits among: 24 above, and the run header below it
 * drops to 6 so the divider belongs to the block it labels.
 */
static GtkWidget *
day_divider(GDateTime *when)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(row, "clawt-day-divider");
    GtkWidget *right = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *label;
    g_autofree gchar *text = clawt_chat_day_label(when, NULL);

    label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");

    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_hexpand(right, TRUE);

    gtk_box_append(GTK_BOX(row), left);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), right);

    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_top(row, 24);

    return row;
}

/*
 * The task and the hop count, which belong to a message rather than to a
 * run.
 *
 * The Flow tab has had both since it was written and the chat has had
 * neither, which is backwards: a delegated reply arriving in your own
 * chat is exactly the one you want to know was delegated, and a hop
 * count climbing towards max_hops is the only thing on screen that
 * distinguishes a loop from a conversation.  The web client already
 * drew both in its chat, so this also closes an asymmetry between the
 * two clients that `make parity` cannot see.
 */
static void
append_message_chips(ClawtWindow *self, GtkWidget *into, const gchar *task,
                     gint64 depth)
{
    if (task != NULL) {
        g_autofree gchar *chip = g_strdup_printf("task %.8s", task);
        GtkWidget *button = gtk_button_new_with_label(chip);

        gtk_widget_add_css_class(button, "flat");
        gtk_widget_add_css_class(button, "caption");
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(button, task);
        g_signal_connect(button, "clicked", G_CALLBACK(clawt_gtk_on_flow_task_clicked),
                         self);
        gtk_box_append(GTK_BOX(into), button);
    }

    /*
     * From the second hop on.  Every ordinary message is one hop, and a
     * "hop 1" on all of them would make the number stop being read.
     */
    if (depth > 1) {
        g_autofree gchar *hops = g_strdup_printf("hop %" G_GINT64_FORMAT,
                                                 depth);
        GtkWidget *chip = gtk_label_new(hops);

        gtk_widget_add_css_class(chip, "caption");
        gtk_widget_add_css_class(chip, "dim-label");
        gtk_widget_set_tooltip_text(
            chip,
            "How far this is from the request that started it. "
            "A count that keeps climbing is a loop; "
            "orchestration.max_hops is where it stops.");
        gtk_box_append(GTK_BOX(into), chip);
    }
}

void
clawt_gtk_append_message_to(ClawtWindow *self, const TranscriptView *view,
                            const gchar *sender, const gchar *body, gboolean from_user,
                            gint64 ts, const gchar *task, gint64 depth)
{
    static const MenuEntry message_menu[] = {
        { "Copy",             "copy-markdown" },
        { "Copy as text",     "copy-text" },
        { "Copy as org",      "copy-org" }
    };
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *text = gtk_label_new(NULL);
    gtk_widget_add_css_class(row, "clawt-chat-run");
    g_autoptr(GDateTime) when = (ts > 0)
        ? g_date_time_new_from_unix_local(ts)
        : g_date_time_new_now_local();
    g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
    /*
     * The stamp comes from the *message's* time, so a message that
     * arrived without one carries no time rather than the moment it
     * happened to be drawn.  @when falls back to now because the run
     * grouping needs a day either way, and inheriting that fallback here
     * would put a plausible wrong time on a record.  A NULL renders as
     * an empty label, which is what the continuation row already tests
     * for.
     */
    g_autofree gchar *stamp = (ts > 0) ? clawt_chat_time_label(when) : NULL;
    gboolean new_day;
    gboolean run_start = clawt_chat_run_is_start(*view->run_sender,
                                                 *view->run_day, sender, day,
                                                 &new_day);

    if (new_day)
        gtk_box_append(view->into, day_divider(when));

    g_free(*view->run_day);
    *view->run_day = g_steal_pointer(&day);
    g_free(*view->run_sender);
    *view->run_sender = g_strdup(sender);

    /*
     * Rendered from markdown, never *as* markup.
     *
     * clawt_markdown_to_pango() emits markup only for the structure
     * cmark identified and escapes every literal on the way out, so an
     * agent writing "<span foreground=...>" gets those characters on
     * screen rather than a message that repaints the interface around
     * it.
     */
    clawt_gtk_set_label_markdown(GTK_LABEL(text), body);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_widget_set_halign(text, GTK_ALIGN_START);

    /*
     * Full contrast for both speakers.
     *
     * `accent` on the operator's body measured 5.82:1 against the
     * background where the agent's `body` measured 12.22:1 -- so the
     * operator's own words were rendered at less than half the contrast
     * of everything they were reading, all day.  It clears WCAG AA, so
     * this is not an accessibility failure; it is simply the wrong text
     * to make harder to read.
     */
    gtk_widget_add_css_class(text, "body");
    gtk_widget_add_css_class(text, "clawt-chat-body");

    if (from_user) {
        /*
         * The operator's turns are bubbles, and only the operator's.
         *
         * An agent's turn runs to dozens of lines with headings, lists
         * and code blocks.  A container that long stops reading as a
         * message and starts reading as a panel, and a bubble wide
         * enough to read as a bubble is too wide to have a measure --
         * the two constraints pull opposite ways and only one side of
         * this conversation is short enough to satisfy both.  So the
         * bubble goes where it works, and the asymmetry is the thing
         * that says who is speaking at a glance.
         *
         * This is also why an earlier attempt at right alignment did
         * nothing: a bare wrapping label is allocated its natural width,
         * so GTK_ALIGN_END moved no body long enough to fill the column.
         * A bubble has a width of its own to be aligned.
         */
        GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_label_set_max_width_chars(GTK_LABEL(text), 59);
        gtk_box_append(GTK_BOX(bubble), text);
        gtk_widget_add_css_class(bubble, "clawt-bubble");
        gtk_widget_set_halign(bubble, GTK_ALIGN_END);

        /*
         * Within a run the second and later bubbles drop their top-right
         * corner, which is what makes a run read as one utterance rather
         * than a stack.
         */
        gtk_widget_add_css_class(bubble,
                                 run_start ? "clawt-bubble-start"
                                           : "clawt-bubble-cont");

        if (run_start) {
            /*
             * The time once, above the first bubble.  No name and no
             * avatar: it is always the same person, the alignment
             * already said so, and a face on every one of your own
             * messages carries no information while costing the side
             * that can least afford it.
             */
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *at = gtk_label_new(stamp);

            gtk_widget_add_css_class(line, "clawt-run-header");

            gtk_widget_add_css_class(at, "caption");
            gtk_widget_add_css_class(at, "dim-label");
            gtk_widget_set_halign(line, GTK_ALIGN_END);
            gtk_widget_set_margin_bottom(line, 2);
            append_message_chips(self, line, task, depth);
            gtk_box_append(GTK_BOX(line), at);
            gtk_box_append(GTK_BOX(row), line);
        }

        gtk_box_append(GTK_BOX(row), bubble);
    } else {
        /*
         * The agent's side: one header per run, and every body in the
         * run indented to the same 44px so the left edge of the text is
         * unbroken down it.
         */
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *gutter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        if (run_start) {
            GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            GtkWidget *who = gtk_label_new(sender);
            GtkWidget *at = gtk_label_new(stamp);
            GtkWidget *avatar = clawt_gtk_build_avatar(
                self->client, sender, view->agent_id, view->has_avatar,
                view->color, CHAT_AVATAR);

            gtk_widget_add_css_class(header, "clawt-run-header");

            /*
             * `heading` rather than `caption-heading`: shrinking the
             * name to caption size makes every turn look like metadata
             * about a message rather than a person saying something.
             */
            gtk_widget_add_css_class(who, "heading");
            gtk_widget_set_margin_start(who, 12);

            gtk_widget_add_css_class(at, "caption");
            gtk_widget_add_css_class(at, "dim-label");
            gtk_widget_set_margin_start(at, 8);

            gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(header), avatar);
            gtk_box_append(GTK_BOX(header), who);
            gtk_box_append(GTK_BOX(header), at);
            append_message_chips(self, header, task, depth);
            gtk_widget_set_margin_bottom(header, 2);
            gtk_box_append(GTK_BOX(row), header);
        }

        /*
         * A real 44px slot rather than a margin on the label.  Avatar
         * plus its 12px gap; a widget is something a narrow layout could
         * collapse, and a margin set from C is not.
         */
        gtk_widget_set_size_request(gutter, CHAT_GUTTER, -1);

        /*
         * A continuation row puts its time here, where the avatar sits
         * on the first row of a run and nothing sits on the rest.
         *
         * This is the half of the message boundary that space cannot
         * carry.  Six pixels more than a paragraph is perceptible and
         * not nameable; a time is unambiguous, it is information rather
         * than decoration, and it costs no width that was not already
         * reserved -- 31 to 34px of caption in a 44px slot, measured.
         *
         * Dimmed and caption-sized so a column of them reads as a
         * margin rather than as a second column of content.
         */
        if (!run_start && stamp != NULL) {
            GtkWidget *at = gtk_label_new(stamp);

            gtk_widget_add_css_class(at, "caption");
            gtk_widget_add_css_class(at, "dim-label");
            gtk_widget_add_css_class(at, "clawt-message-time");
            gtk_widget_set_valign(at, GTK_ALIGN_START);

            /*
             * Right-aligned against the avatar's edge, so the times
             * under one avatar read as a column rather than as ragged
             * text in a margin.
             *
             * Overlaid rather than packed, and that is the load-bearing
             * part.  A GtkBox is measured from its children, so a time
             * placed *in* the gutter sets the gutter's width -- and
             * `gtk_widget_set_size_request()` is a floor, not a cap, so
             * a time wider than CHAT_AVATAR widens the gutter and takes
             * the body column with it.  Measured against real GTK 4.22
             * at the default interface font, an `HH:MM` caption is 35px
             * against the 32 the arithmetic assumes, and the bodies of a
             * run then start at 47 while their own run header starts at
             * 44.  Which is the exact invariant the gutter exists for --
             * "every body in the run indented to the same 44px" is three
             * comments above this one.
             *
             * An overlay child does not contribute to the measurement
             * (`measure-overlay` is FALSE by default), so the time costs
             * no width at all: it is aligned inside a slot the gutter
             * already reserves, and a time too wide for it overhangs
             * into the row margin instead of moving anything.  The web
             * client reached the same place from CSS and says so in the
             * same words -- `position: absolute` "so it costs no width
             * that was not already reserved".
             *
             * CHAT_GUTTER - CHAT_AVATAR is the gap beside the avatar, so
             * the time ends where the avatar ends by arithmetic rather
             * than by a 12 that happens to match.
             *
             * Measured, three variants, ink right edges of 00:00 / 23:14
             * / 11:11 / 18:48 and where the body column lands:
             *
             *   packed, left-aligned    34 31 22 31   body 44
             *   packed, right-aligned   34 34 34 34   body 47  <- moves
             *   overlaid                31 31 31 31   body 44
             */
            {
                GtkWidget *slot = gtk_overlay_new();

                /*
                 * The gutter box itself becomes what the overlay
                 * measures -- it already carries the CHAT_GUTTER size
                 * request, and building a second empty widget for the
                 * job would leave the first one floating and unparented.
                 */
                gtk_overlay_set_child(GTK_OVERLAY(slot), gutter);

                gtk_widget_set_halign(at, GTK_ALIGN_END);
                gtk_widget_set_margin_end(at, CHAT_GUTTER - CHAT_AVATAR);
                gtk_overlay_add_overlay(GTK_OVERLAY(slot), at);

                gutter = slot;
            }
        }

        gtk_box_append(GTK_BOX(line), gutter);
        gtk_box_append(GTK_BOX(line), text);
        gtk_widget_set_hexpand(text, TRUE);

        /*
         * FILL, not START, and this is the whole of #18.
         *
         * hexpand gives the label the column; halign decides what it
         * does with it.  A wrapping GtkLabel at START is allocated its
         * *natural* width and aligns that inside the space, so the body
         * asked for about 66 characters and stopped while the clamp
         * stood ready to give it 82 -- a narrow ribbon with a wide
         * empty band either side, which is exactly what was reported.
         *
         * The issue blamed the 59-character cap a few lines up.  That
         * cap is inside the operator's branch and never touches an
         * agent's body; it bounds a bubble, which needs a width of its
         * own to be right-aligned at all.  Two plausible causes, and
         * the measurable one is this.
         */
        gtk_widget_set_halign(text, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(row), line);
    }

    append_attachment_previews(self, row, body, from_user);

    /*
     * The body travels with the row, so the menu can copy what was
     * actually said rather than what the label happens to render.
     */
    g_object_set_data_full(G_OBJECT(row), "body", g_strdup(body), g_free);
    add_context_menu(self, row, message_menu, G_N_ELEMENTS(message_menu),
                     on_message_action,
                     g_object_get_data(G_OBJECT(row), "body"));
    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);

    /*
     * Turns have to be further apart than the paragraphs inside one.
     *
     * They were not, and that -- not the 6px -- is why the transcript
     * read as a single wall.  A message's own paragraph break is a
     * literal blank line: clawt_markdown_to_pango() separates blocks
     * with \n\n (src/chat/clawt-markdown.c), and a blank line costs a
     * whole line height.  Measured off a rendered window at 1280px, ink
     * to ink: 11px between lines of one paragraph, 27px between two
     * paragraphs of one message, and 21px between two speakers.
     * Proximity was inverted -- one person's paragraphs sat further
     * apart than two different people's turns did.
     *
     * 30 is the smallest step on the HIG's 6px grid that beats that
     * 27px, and it was measured rather than derived: 18 was tried first,
     * because it is the HIG's own step for separating groups, and it
     * still lost.  The run redesign specified 18 again for run-to-run;
     * the measurement has not changed and neither has the paragraph gap,
     * so the measured number stands.
     *
     * Inside a run it is 6, and after a day divider 6 as well -- the
     * divider already carries 24 above it, and a run header adding 30 to
     * that would put the date adrift between two blocks instead of
     * belonging to the one beneath it.
     */
    /*
     * A day divider carries 24 of its own above it, so a run header
     * adding the run gap on top would set the date adrift between two
     * blocks instead of belonging to the one beneath it.
     */
    gtk_widget_set_margin_top(
        row, !run_start ? chat_message_spacing(self)
                        : (new_day ? 6 : chat_run_spacing(self)));

    gtk_box_append(view->into, row);
}

/*
 * The chat transcript, which is the common case.
 */
void
clawt_gtk_append_message(ClawtWindow *self, const gchar *sender, const gchar *body,
                         gboolean from_user, gint64 ts)
{
    TranscriptView view = { self->transcript, &self->run_sender,
                            &self->run_day, self->selected_agent,
                            self->selected_has_avatar,
                            self->selected_color };

    clawt_gtk_append_message_to(self, &view, sender, body, from_user, ts, NULL, 0);
}

/*
 * Schedules a scroll that cannot outlive the window.
 *
 * A plain g_idle_add(self) runs after the window has been destroyed if
 * the user closes it in the same turn a message arrives, and the
 * callback then reads freed memory.  Holding a reference for the life of
 * the idle costs nothing and removes the race.
 */
static gboolean scroll_to_bottom(gpointer user_data);

void
clawt_gtk_queue_scroll(ClawtWindow *self)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_to_bottom,
                    g_object_ref(self), g_object_unref);
}

/*
 * Scrolls to the bottom, but only when the reader was already there.
 *
 * Yanking somebody down mid-read because a message arrived is the single
 * most annoying thing a chat window can do.
 */
static gboolean
scroll_to_bottom(gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkAdjustment *adjustment;

    if (!self->following || self->transcript_scroll == NULL)
        return G_SOURCE_REMOVE;

    adjustment = gtk_scrolled_window_get_vadjustment(self->transcript_scroll);
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                             gtk_adjustment_get_page_size(adjustment));

    return G_SOURCE_REMOVE;
}

/*
 * Notices that the transcript has grown, and asks for a scroll.
 *
 * It asks rather than scrolls, and that distinction is the whole point.
 * These two notifies are emitted by GtkViewport from inside its own
 * size-allocate, at the moment it reconfigures the adjustment for a
 * layout it has already positioned its child for. Writing `value` here
 * moves the number and does not move the picture: the viewport has
 * finished placing the child for this pass, and the allocation the
 * write asks for is folded into the pass that is already running rather
 * than starting another one. Nothing queues a further one, so the
 * displayed offset stays where it was while the adjustment reports the
 * new bottom.
 *
 * That mismatch is stable, not transient -- measured at 68px, exactly
 * one message, and still there ten seconds later. It is also invisible
 * to every correction in this file, because all of them test the
 * adjustment and the adjustment is already right. `scroll_to_bottom()`
 * in particular finds value == bottom and returns without doing
 * anything, so the one write that would have happened outside a layout
 * pass is the one this handler suppresses.
 *
 * Queueing instead puts the write in an idle, after the pass has
 * finished. It is then a real value change, the viewport allocates
 * again, and the newest message is on screen. `queue_scroll()` already
 * holds a reference for the life of the idle and `scroll_to_bottom()`
 * already re-reads `upper` and re-checks `following`, so deferring
 * costs nothing but the hop.
 *
 * page-size as well as upper, because typing grows the composer and
 * shrinks the transcript above it, which moves the bottom without adding
 * anything.
 */
static void
on_transcript_grew(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkAdjustment *adjustment = GTK_ADJUSTMENT(object);
    gdouble bottom;

    (void)pspec;

    if (!self->following)
        return;

    bottom = gtk_adjustment_get_upper(adjustment) -
             gtk_adjustment_get_page_size(adjustment);

    /*
     * Only when it is not already there. This runs on every layout
     * pass, and a queued idle that would find nothing to do is worth
     * not queueing.
     */
    if (gtk_adjustment_get_value(adjustment) < bottom)
        clawt_gtk_queue_scroll(self);
}

/*
 * The only place `following` changes, and the reason the two unread
 * affordances cannot disagree.
 *
 * `following` false means the reader is deliberately somewhere above the
 * live edge, and the client already refuses to move the view for them --
 * see scroll_to_bottom().  That refusal is right; saying nothing about
 * it was not.  Two things say it: a pill floating over the transcript,
 * and a rule drawn in the transcript at the point reading stopped.
 *
 * Both are cleared by the same false -> true edge, so every path that
 * already re-arms following clears them with no new cases: reaching the
 * bottom by hand, sending a message, switching agent, /clear, and the
 * pill's own click.
 */
void
clawt_gtk_set_following(ClawtWindow *self, gboolean following)
{
    self->following = following;

    if (!following)
        return;

    if (self->unread_marker != NULL) {
        gtk_box_remove(self->transcript, self->unread_marker);
        self->unread_marker = NULL;
    }

    if (self->jump_revealer != NULL) {
        gtk_revealer_set_reveal_child(self->jump_revealer, FALSE);

        /*
         * A GtkRevealer keeps its allocation while its child is hidden,
         * so an overlay child that is merely not revealed can still take
         * the clicks meant for the transcript underneath it.  Dropping
         * can-target with the reveal removes that without having to
         * reason about which transition does what.
         */
        gtk_widget_set_can_target(GTK_WIDGET(self->jump_revealer), FALSE);
    }
}

/*
 * The rule drawn where reading stopped.
 *
 *   ---------------------  New messages  ---------------------
 *
 * It stores nothing: no read receipt, no per-agent position, nothing on
 * disk.  It is exactly as durable as `following` itself, which is what
 * lets it promise something the client can always keep -- "these arrived
 * while you were up there" -- rather than "this is where you left off",
 * which would need state across restarts to be true.
 *
 * `accent` on the label rather than a colour, so the palette work
 * redefines it for free; the separators keep the platform colour,
 * because the label is the message and the rules are only the ruling.
 */
static GtkWidget *
unread_marker_new(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *before = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *label = gtk_label_new("New messages");
    GtkWidget *after = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

    gtk_widget_set_hexpand(before, TRUE);
    gtk_widget_set_valign(before, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(after, TRUE);
    gtk_widget_set_valign(after, GTK_ALIGN_CENTER);

    gtk_widget_add_css_class(label, "caption-heading");
    gtk_widget_add_css_class(label, "accent");

    gtk_box_append(GTK_BOX(row), before);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), after);

    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);

    /*
     * Closer to the message below than to the one above, on purpose: the
     * marker labels the block underneath it, so it should belong to it.
     */
    gtk_widget_set_margin_top(row, 18);

    return row;
}

/*
 * Something arrived that the reader is not looking at.
 *
 * Called only from the event path, so replayed history and the client's
 * own local output can never trip it -- neither of those is an arrival.
 * At most one marker is ever alive, because the second arrival in a run
 * belongs under the same rule as the first.
 */
void
clawt_gtk_note_arrival(ClawtWindow *self)
{
    if (self->following || self->unread_marker != NULL)
        return;

    self->unread_marker = unread_marker_new();
    gtk_box_append(self->transcript, self->unread_marker);

    if (self->jump_revealer != NULL) {
        gtk_widget_set_can_target(GTK_WIDGET(self->jump_revealer), TRUE);
        gtk_revealer_set_reveal_child(self->jump_revealer, TRUE);
    }
}

/*
 * Emptying the transcript, rather than clear_box() on its own.
 *
 * The marker is a borrowed pointer into that box, so clearing the box
 * behind its back leaves it dangling and the next false -> true edge
 * removes a widget that is already gone.  Re-arming here is also what
 * keeps load_history()'s replay from drawing a "New messages" rule at
 * the top of a freshly loaded transcript.
 */
void
clawt_gtk_reset_transcript(ClawtWindow *self)
{
    self->unread_marker = NULL;
    g_clear_pointer(&self->run_sender, g_free);
    g_clear_pointer(&self->run_day, g_free);
    clawt_gtk_clear_box(self->transcript);
    clawt_gtk_set_following(self, TRUE);
}

static void
on_jump_to_latest(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    /*
     * Jump, do not animate.  GTK4 has no adjustment animation, and a
     * scroll through several screens of text disorients rather than
     * orients.  set_following() removes the marker, which shrinks the
     * content, so the scroll is queued rather than computed here --
     * on_transcript_grew() lands it on the real bottom afterwards.
     */
    clawt_gtk_set_following(self, TRUE);
    clawt_gtk_queue_scroll(self);
}

static void
on_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    ClawtWindow *self = user_data;

    /*
     * The predicate is clawt_transcript_is_at_bottom(), in libclawt, so
     * the tolerance can be exercised on both sides and at its boundary
     * without a window -- which is the one part of the follow machinery
     * a test could not otherwise reach.
     */
    clawt_gtk_set_following(self, clawt_transcript_is_at_bottom(
                            gtk_adjustment_get_value(adjustment),
                            gtk_adjustment_get_upper(adjustment),
                            gtk_adjustment_get_page_size(adjustment)));
}

/*
 * Whether this message is already on screen, remembering it if not.
 *
 * Answers the replay case: a client that has just connected is sent the
 * recent events as well as loading the history they are already in.
 */
gboolean
clawt_gtk_already_shown(ClawtWindow *self, const gchar *id)
{
    if (id == NULL)
        return FALSE;

    if (g_hash_table_contains(self->shown, id))
        return TRUE;

    g_hash_table_add(self->shown, g_strdup(id));
    return FALSE;
}

/*
 * The message being composed.
 *
 * A GtkTextView rather than a GtkEntry, because Ctrl+G hands the box
 * whatever came back from $EDITOR and that is usually several
 * paragraphs -- an entry is single-line and draws every newline as a
 * control picture, so the one feature that exists to write something
 * long made it unreadable.
 */
gchar *
clawt_gtk_entry_text(ClawtWindow *self)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->entry);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

void
clawt_gtk_entry_set_text(ClawtWindow *self, const gchar *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->entry),
                             text != NULL ? text : "", -1);
}

static void
entry_focus_end(ClawtWindow *self)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->entry);
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_place_cursor(buffer, &end);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry));
}

/* ── Attachments ─────────────────────────────────────────────────── */

void
clawt_gtk_attachment_free(Attachment *attachment)
{
    if (attachment == NULL)
        return;

    g_free(attachment->name);
    g_clear_pointer(&attachment->bytes, g_bytes_unref);
    g_free(attachment);
}

static void refresh_attachment_strip(ClawtWindow *self);

static void
on_drop_attachment(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    Attachment *attachment = g_object_get_data(G_OBJECT(button), "attachment");

    g_ptr_array_remove(self->pending, attachment);
    refresh_attachment_strip(self);
}

/* A row of chips above the entry, one per queued file. */
static void
refresh_attachment_strip(ClawtWindow *self)
{
    GtkWidget *child;
    guint i;

    while ((child = gtk_widget_get_first_child(self->attachments)) != NULL)
        gtk_box_remove(GTK_BOX(self->attachments), child);

    gtk_widget_set_visible(self->attachments, self->pending->len > 0);

    for (i = 0; i < self->pending->len; i++) {
        Attachment *attachment = g_ptr_array_index(self->pending, i);
        GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *label;
        GtkWidget *drop;
        g_autofree gchar *text = g_strdup_printf(
            "%s (%" G_GSIZE_FORMAT " KB)", attachment->name,
            (g_bytes_get_size(attachment->bytes) + 1023) / 1024);

        label = gtk_label_new(text);
        gtk_widget_add_css_class(label, "caption");
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 28);

        drop = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(drop, "flat");
        gtk_widget_add_css_class(drop, "circular");
        gtk_widget_set_tooltip_text(drop, "Do not send this one");
        g_object_set_data(G_OBJECT(drop), "attachment", attachment);
        g_signal_connect(drop, "clicked", G_CALLBACK(on_drop_attachment),
                         self);

        gtk_widget_add_css_class(chip, "card");
        gtk_box_append(GTK_BOX(chip), label);
        gtk_box_append(GTK_BOX(chip), drop);
        gtk_box_append(GTK_BOX(self->attachments), chip);
    }
}

static void
queue_attachment(ClawtWindow *self, const gchar *name, GBytes *bytes)
{
    Attachment *attachment;

    if (bytes == NULL || g_bytes_get_size(bytes) == 0)
        return;

    /*
     * Bounded, because this crosses the IPC socket base64-encoded and a
     * client that queues a 400 MB video would block the daemon's main
     * context for as long as it takes to decode.
     */
    if (g_bytes_get_size(bytes) > 32u * 1024u * 1024u) {
        clawt_window_toast(self, "that file is over 32 MB; put it in a "
                                 "shared folder instead");
        return;
    }

    attachment = g_new0(Attachment, 1);
    attachment->name = g_strdup(name);
    attachment->bytes = g_bytes_ref(bytes);

    g_ptr_array_add(self->pending, attachment);
    refresh_attachment_strip(self);
}

static void
on_texture_pasted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GdkTexture) texture = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *name = NULL;

    texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result,
                                                &error);

    if (texture == NULL) {
        if (error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            clawt_window_toast(self, error->message);

        return;
    }

    /*
     * PNG, because a pasted screenshot is a texture with no file behind
     * it and every model that reads images reads PNG.
     */
    png = gdk_texture_save_to_png_bytes(texture);

    name = g_strdup_printf("pasted-%" G_GINT64_FORMAT ".png",
                           g_get_real_time() / G_USEC_PER_SEC);

    queue_attachment(self, name, png);
    g_object_unref(self);
}

/*
 * Whether the clipboard has an image, and if so take it.
 *
 * Returns %TRUE when the paste was handled here, so the entry does not
 * also paste whatever text representation the source offered -- an
 * image copied from a browser usually carries a URL alongside it, and
 * getting both is worse than getting either.
 */
static gboolean
paste_image(ClawtWindow *self)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);

    if (!gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE))
        return FALSE;

    gdk_clipboard_read_texture_async(clipboard, NULL, on_texture_pasted,
                                     g_object_ref(self));
    return TRUE;
}

static void
on_files_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source),
                                                 result, &error);

    if (files == NULL) {
        /* Dismissing the dialog is not a failure worth a toast. */
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    for (i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) file = g_list_model_get_item(files, i);
        g_autofree gchar *name = g_file_get_basename(file);
        g_autofree gchar *contents = NULL;
        g_autoptr(GError) read_error = NULL;
        gsize length = 0;

        if (!g_file_load_contents(file, NULL, &contents, &length, NULL,
                                  &read_error)) {
            clawt_window_toast(self, read_error->message);
            continue;
        }

        {
            g_autoptr(GBytes) bytes = g_bytes_new(contents, length);

            queue_attachment(self, name, bytes);
        }
    }

    g_object_unref(self);
}

static void
on_attach_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

    (void)button;

    gtk_file_dialog_set_title(dialog, "Send with this message");
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL,
                                  on_files_chosen, g_object_ref(self));
}

/*
 * Hands the queued files to the daemon and describes them in the body.
 *
 * The daemon writes them into the agent's exchange directory and says
 * what path the *agent* should use, which is not the host path when the
 * agent lives in a container. Naming them in the message is what makes
 * them reachable: an agent reads files with its own tools, and one it
 * has not been told about is one it will not open.
 *
 * Returns: (transfer full): the body to actually send
 */
static gchar *
body_with_attachments(ClawtWindow *self, const gchar *body)
{
    g_autoptr(GString) out = NULL;
    guint i;

    if (self->pending->len == 0)
        return g_strdup(body);

    out = g_string_new(body);

    if (out->len > 0)
        g_string_append(out, "\n\n");

    g_string_append(out,
                    "[clawtilla] Files sent with this message. Open them "
                    "with your own read tool at the host path below; it "
                    "runs on the host even when your shell does not.\n");

    for (i = 0; i < self->pending->len; i++) {
        Attachment *attachment = g_ptr_array_index(self->pending, i);
        g_autoptr(JsonNode) reply = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;
        const guchar *data = g_bytes_get_data(attachment->bytes, &length);

        encoded = g_base64_encode(data, length);

        reply = clawt_window_request(
            self, "attachment.put",
            clawt_build_payload("agent", self->selected_agent,
                                "name", attachment->name,
                                "data", encoded, NULL));

        if (reply == NULL) {
            g_string_append_printf(out, "- %s (could not be saved)\n",
                                   attachment->name);
            continue;
        }

        {
            JsonObject *result = clawt_payload_of(reply);
            const gchar *host = clawt_json_string(result, "host_path", "");
            const gchar *guest = clawt_json_string(result, "path", "");

            g_string_append_printf(out, "- %s\n  %s\n",
                                   attachment->name, host);

            /*
             * Both paths, and the host one first, because they are for
             * different tools.
             *
             * An agent's own read tool runs on the host even when its
             * shell runs in a container -- so given only the container
             * path it could stat the file with clawtilla_computer_exec
             * and never open it, which is exactly what happened to the
             * first image anybody sent.
             */
            if (g_strcmp0(host, guest) != 0)
                g_string_append_printf(
                    out, "  (inside your container: %s)\n", guest);
        }
    }

    g_ptr_array_set_size(self->pending, 0);
    refresh_attachment_strip(self);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Composing in $EDITOR ────────────────────────────────────────── */

/*
 * Ctrl+G: hand what is typed to $EDITOR, take back whatever comes out.
 *
 * A one-line GtkEntry is a bad place to write six paragraphs, and the
 * editor is where the person already knows how to write. The file is
 * seeded with the current text so this extends a draft rather than
 * replacing it.
 */
static void
on_compose_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *path = g_object_get_data(G_OBJECT(source), "path") != NULL
        ? g_strdup(g_object_get_data(G_OBJECT(source), "path")) : NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *written = NULL;

    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result,
                                        &error)) {
        clawt_window_toast(self, error->message);
        g_object_unref(self);
        return;
    }

    if (path != NULL && g_file_get_contents(path, &written, NULL, NULL)) {
        /*
         * Trailing newline stripped: every editor adds one, and it would
         * otherwise arrive as an empty line at the end of every message
         * composed this way.
         */
        g_strchomp(written);
        clawt_gtk_entry_set_text(self, written);
    }

    if (path != NULL)
        g_unlink(path);

    entry_focus_end(self);
    g_object_unref(self);
}

static void
compose_in_editor(ClawtWindow *self)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    g_autofree gchar *path = NULL;
    const gchar *editor = clawt_gtk_editor_command();
    g_autofree gchar *current = NULL;
    gint fd;
    gint i;

    if (editor == NULL) {
        clawt_window_toast(self, "no editor found; set $EDITOR or $VISUAL");
        return;
    }

    /*
     * .md, so an editor that picks a mode by extension gives you one
     * suited to prose rather than to nothing at all.
     */
    fd = g_file_open_tmp("clawtilla-message-XXXXXX.md", &path, &error);

    if (fd < 0) {
        clawt_window_toast(self, error->message);
        return;
    }

    close(fd);

    current = clawt_gtk_entry_text(self);

    if (!g_file_set_contents(path, current != NULL ? current : "", -1,
                             &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    if (!g_shell_parse_argv(editor, NULL, &parts, &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, g_strdup(parts[i]));

    g_ptr_array_add(argv, g_strdup(path));
    g_ptr_array_add(argv, NULL);

    process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_NONE, &error);

    if (process == NULL) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    g_object_set_data_full(G_OBJECT(process), "path", g_steal_pointer(&path),
                           g_free);
    g_subprocess_wait_check_async(process, NULL, on_compose_finished,
                                  g_object_ref(self));
}

/* ── Slash commands ──────────────────────────────────────────────── */

/*
 * What "/" offers.
 *
 * A chat entry is the one place a person always is, so the things they
 * do most often should be reachable without going and finding a tab.
 * Everything here is a shortcut to something that already exists --
 * none of it is a second way to do anything.
 */
typedef struct {
    const gchar *name;
    const gchar *argument;   /* (nullable) what to type after it */
    const gchar *summary;
} SlashCommand;

static const SlashCommand slash_commands[] = {
    { "/help",    NULL,      "list these commands" },
    { "/start",   NULL,      "start this agent" },
    { "/stop",    NULL,      "stop this agent" },
    { "/restart", NULL,      "restart this agent" },
    { "/interrupt", NULL,    "stop what it is doing now, without stopping it" },
    { "/attach",  NULL,      "pick files to send with the next message" },
    { "/compose", NULL,      "write the message in $EDITOR (same as Ctrl+G)" },
    { "/edit",    "[file]",  "open a workspace file in $EDITOR" },
    { "/files",   NULL,      "list this agent's workspace files" },
    { "/memory",  "<query>", "search what this agent has remembered" },
    { "/agents",  NULL,      "who is in the fleet" },
    { "/flow",    NULL,      "go to the conversations between agents" },
    { "/tasks",   NULL,      "go to the task board" },
    { "/reset",   NULL,      "start the agent's AI session again, from nothing" },
    { "/retry",   NULL,      "send your last message again" },
    { "/export",  "[org]",   "save the conversation: text, markdown or org" },
    { "/copy",    "[org]",   "copy the conversation: text, markdown or org" },
    { "/clear",   NULL,      "clear this transcript on screen only" },
    { "/new",     NULL,      "create an agent" }
};

/* A reply that came from the client, not from the agent. */
static void
append_local(ClawtWindow *self, const gchar *text)
{
    /*
     * Follow again, because the operator asked for this.
     *
     * scroll_to_bottom() and on_transcript_grew() both refuse to move
     * the view while `following` is false, which is right for a message
     * arriving on its own and wrong for output the operator just asked
     * to see.  on_send() re-arms following at its end, but it returns
     * early when the text was a slash command -- so with the view
     * scrolled up, /help, /export, /copy and /clear appended their
     * output below the fold and the queued scroll bailed on the first
     * line.  The command looked like it had done nothing.
     *
     * Re-arming here rather than in on_send() covers every caller: the
     * point is not which path ran, it is that the operator's own action
     * put this on screen and they should be looking at it.
     */
    clawt_gtk_set_following(self, TRUE);

    clawt_gtk_append_message(self, "clawtilla", text, FALSE, 0);
    clawt_gtk_queue_scroll(self);
}

static void
show_command_help(ClawtWindow *self)
{
    g_autoptr(GString) out = g_string_new(NULL);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        g_string_append_printf(out, "%s%s%s\n    %s\n",
                               slash_commands[i].name,
                               slash_commands[i].argument != NULL ? " " : "",
                               slash_commands[i].argument != NULL
                                   ? slash_commands[i].argument : "",
                               slash_commands[i].summary);
    }

    g_string_append(out,
                    "\nCtrl+G writes the message in $EDITOR. Paste an image "
                    "or use /attach to send files.");

    append_local(self, out->str);
}

/*
 * Runs a slash command.
 *
 * Returns %TRUE when the text was a command and has been dealt with, so
 * the caller does not also send it to the agent -- a mistyped command
 * reaching the model as a message is how a person learns to distrust
 * the feature.
 */
static gboolean
run_slash_command(ClawtWindow *self, const gchar *text)
{
    g_auto(GStrv) parts = NULL;
    const gchar *name;
    const gchar *rest;
    gsize i;

    if (text == NULL || text[0] != '/')
        return FALSE;

    parts = g_strsplit(text, " ", 2);
    name = parts[0];
    rest = (parts[1] != NULL) ? g_strstrip(parts[1]) : NULL;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        if (g_strcmp0(slash_commands[i].name, name) == 0)
            break;
    }

    if (i == G_N_ELEMENTS(slash_commands)) {
        g_autofree gchar *message = g_strdup_printf(
            "There is no %s. Type /help for the list.", name);

        append_local(self, message);
        return TRUE;
    }

    if (g_strcmp0(name, "/help") == 0) {
        show_command_help(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/clear") == 0) {
        clawt_gtk_reset_transcript(self);
        g_hash_table_remove_all(self->shown);
        append_local(self, "Cleared on screen. The transcript on disk is "
                           "untouched -- reopen this agent to see it again.");
        return TRUE;
    }

    if (g_strcmp0(name, "/compose") == 0) {
        clawt_gtk_entry_set_text(self, "");
        compose_in_editor(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/attach") == 0) {
        on_attach_clicked(NULL, self);
        return TRUE;
    }

    if (g_strcmp0(name, "/flow") == 0) {
        adw_view_stack_set_visible_child_name(self->pages, "flow");
        return TRUE;
    }

    if (g_strcmp0(name, "/tasks") == 0) {
        adw_view_stack_set_visible_child_name(self->pages, "tasks");
        return TRUE;
    }

    if (g_strcmp0(name, "/new") == 0) {
        clawt_gtk_on_new_agent(NULL, self);
        return TRUE;
    }

    if (self->selected_agent == NULL) {
        append_local(self, "Pick an agent first.");
        return TRUE;
    }

    if (g_strcmp0(name, "/start") == 0 || g_strcmp0(name, "/stop") == 0 ||
        g_strcmp0(name, "/restart") == 0) {
        g_autofree gchar *verb = g_strdup_printf("agent.%s", name + 1);
        g_autoptr(JsonNode) reply = NULL;

        reply = clawt_window_request(
            self, verb,
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply != NULL) {
            g_autofree gchar *message = g_strdup_printf(
                "%s: %s requested.", self->selected_agent, name + 1);

            append_local(self, message);
        }

        clawt_gtk_refresh_agents(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/interrupt") == 0) {
        g_autofree gchar *said = interrupt_selected(self);

        if (said != NULL)
            append_local(self, said);

        clawt_gtk_refresh_agents(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/reset") == 0) {
        g_autoptr(JsonNode) reply = clawt_window_request(
            self, "agent.reset",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply != NULL) {
            JsonObject *result = clawt_payload_of(reply);
            g_autofree gchar *message = g_strdup_printf(
                "Session reset: %" G_GINT64_FORMAT " stored session%s "
                "cleared%s. The next thing you say starts a new one.",
                clawt_json_int(result, "sessions_cleared", 0),
                clawt_json_int(result, "sessions_cleared", 0) == 1 ? "" : "s",
                clawt_json_int(result, "restarted", 0) ? " and the agent "
                                                          "restarted" : "");

            append_local(self, message);
        }

        clawt_gtk_refresh_agents(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/retry") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *messages;
        const gchar *last = NULL;
        guint j;

        reply = clawt_window_request(
            self, "room.history",
            clawt_build_payload("room", self->selected_agent, "as", "user",
                                NULL));

        if (reply == NULL)
            return TRUE;

        messages = json_object_get_array_member(clawt_payload_of(reply),
                                                "messages");

        for (j = 0; j < json_array_get_length(messages); j++) {
            JsonObject *one = json_array_get_object_element(messages, j);

            if (g_strcmp0(clawt_json_string(one, "sender", ""), "user") == 0)
                last = clawt_json_string(one, "body", NULL);
        }

        if (last == NULL) {
            append_local(self, "You have not said anything to resend.");
            return TRUE;
        }

        /*
         * Put in the box rather than sent. Retry usually means "that
         * did not go how I wanted", and the chance to change a word
         * before it goes again is the point.
         */
        clawt_gtk_entry_set_text(self, last);
        entry_focus_end(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/export") == 0 || g_strcmp0(name, "/copy") == 0) {
        g_autofree gchar *what = g_strdup_printf(
            "%s-%s", g_strcmp0(name, "/copy") == 0 ? "copy" : "save",
            (rest != NULL && rest[0] != '\0') ? rest : "markdown");

        on_conversation_action(self, what, NULL);
        return TRUE;
    }

    if (g_strcmp0(name, "/agents") == 0) {
        g_autoptr(JsonNode) reply = clawt_window_request(self, "agent.list",
                                                          NULL);
        g_autoptr(GString) out = g_string_new(NULL);
        JsonArray *agents;
        guint j;

        if (reply == NULL)
            return TRUE;

        agents = json_object_get_array_member(clawt_payload_of(reply),
                                              "agents");

        for (j = 0; j < json_array_get_length(agents); j++) {
            JsonObject *one = json_array_get_object_element(agents, j);

            g_string_append_printf(out, "%-20s %-10s %s\n",
                                   clawt_json_string(one, "id", "?"),
                                   clawt_json_string(one, "state", "?"),
                                   clawt_json_string(one, "description", ""));
        }

        append_local(self, out->str);
        return TRUE;
    }

    if (g_strcmp0(name, "/files") == 0 || g_strcmp0(name, "/edit") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *files;
        guint j;

        reply = clawt_window_request(
            self, "agent.files",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply == NULL)
            return TRUE;

        files = json_object_get_array_member(clawt_payload_of(reply),
                                             "files");

        if (g_strcmp0(name, "/files") == 0 || rest == NULL) {
            g_autoptr(GString) out = g_string_new(NULL);

            for (j = 0; j < json_array_get_length(files); j++) {
                JsonObject *file = json_array_get_object_element(files, j);

                g_string_append_printf(out, "%-18s %s\n",
                                       clawt_json_string(file, "name", "?"),
                                       clawt_json_string(file, "title", ""));
            }

            g_string_append(out, "\n/edit <name> opens one in $EDITOR.");
            append_local(self, out->str);
            return TRUE;
        }

        for (j = 0; j < json_array_get_length(files); j++) {
            JsonObject *file = json_array_get_object_element(files, j);

            if (g_strcmp0(clawt_json_string(file, "name", ""), rest) != 0)
                continue;

            /*
             * The daemon's path, never one built here: it owns where a
             * workspace is, and a client that constructs the path is a
             * client that can be pointed at somebody else's.
             */
            clawt_gtk_open_path_in_editor(self, clawt_json_string(file, "path", ""),
                                          rest);
            return TRUE;
        }

        {
            g_autofree gchar *message = g_strdup_printf(
                "%s has no file called '%s'. /files lists them.",
                self->selected_agent, rest);

            append_local(self, message);
        }

        return TRUE;
    }

    if (g_strcmp0(name, "/memory") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        g_autoptr(GString) out = g_string_new(NULL);
        JsonArray *memories;
        guint j;

        reply = clawt_window_request(
            self, rest != NULL ? "memory.search" : "memory.list",
            rest != NULL
                ? clawt_build_payload("agent", self->selected_agent,
                                      "query", rest, NULL)
                : clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply == NULL)
            return TRUE;

        memories = json_object_get_array_member(clawt_payload_of(reply),
                                                "memories");

        if (json_array_get_length(memories) == 0) {
            append_local(self, "Nothing remembered matches that.");
            return TRUE;
        }

        for (j = 0; j < json_array_get_length(memories); j++) {
            JsonObject *memory = json_array_get_object_element(memories, j);
            const gchar *summary = clawt_json_string(memory, "summary", NULL);

            g_string_append_printf(out, "%s [%s]\n  %s\n",
                                   clawt_json_string(memory, "id", "?"),
                                   clawt_json_string(memory, "category", "?"),
                                   summary != NULL
                                       ? summary
                                       : clawt_json_string(memory, "content",
                                                           ""));
        }

        append_local(self, out->str);
        return TRUE;
    }

    return TRUE;
}

static void
on_command_row_selected(GtkListBox *list, GtkListBoxRow *row,
                        gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name;
    g_autofree gchar *filled = NULL;

    (void)list;

    if (row == NULL)
        return;

    name = g_object_get_data(G_OBJECT(row), "command");

    if (name == NULL)
        return;

    /*
     * A trailing space for a command that takes an argument, so the
     * next keystroke is the argument rather than a correction.
     */
    filled = g_strconcat(name,
                         g_object_get_data(G_OBJECT(row), "takes-argument")
                             != NULL ? " " : "", NULL);

    gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                  FALSE);
    clawt_gtk_entry_set_text(self, filled);
    entry_focus_end(self);
}

/*
 * Shows the matching commands as the person types "/".
 *
 * Discoverability, not completion: the list is there to be read, and
 * clicking one fills it in. Anybody who already knows the command just
 * keeps typing and never looks at it.
 */
static void
on_entry_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *text = clawt_gtk_entry_text(self);
    guint matches = 0;
    gsize i;

    (void)buffer;

    /* GtkTextView has no placeholder of its own. */
    gtk_widget_set_visible(self->placeholder,
                           text == NULL || text[0] == '\0');

    if (text == NULL || text[0] != '/' || strchr(text, ' ') != NULL) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                      FALSE);
        return;
    }

    clawt_gtk_clear_list(self->command_list);

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        GtkWidget *row;
        g_autofree gchar *label = NULL;

        if (!g_str_has_prefix(slash_commands[i].name, text))
            continue;

        label = g_strdup_printf("%s%s%s", slash_commands[i].name,
                                slash_commands[i].argument != NULL ? " " : "",
                                slash_commands[i].argument != NULL
                                    ? slash_commands[i].argument : "");

        row = adw_action_row_new();
        clawt_gtk_set_row_text(row, label, slash_commands[i].summary);
        g_object_set_data_full(G_OBJECT(row), "command",
                               g_strdup(slash_commands[i].name), g_free);

        if (slash_commands[i].argument != NULL)
            g_object_set_data(G_OBJECT(row), "takes-argument",
                              GINT_TO_POINTER(1));

        gtk_list_box_append(self->command_list, row);
        matches++;
    }

    gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                  matches > 0);
}

static gboolean
on_entry_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
             GdkModifierType state, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)controller;
    (void)keycode;

    /*
     * Enter sends; Shift+Enter is a newline.  A multi-line box needs
     * both, and a chat window where Enter inserts a newline is a chat
     * window nobody can send a message from.
     */
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if ((state & GDK_SHIFT_MASK) != 0)
            return GDK_EVENT_PROPAGATE;

        on_send(NULL, self);
        return GDK_EVENT_STOP;
    }

    if ((state & GDK_CONTROL_MASK) == 0)
        return GDK_EVENT_PROPAGATE;

    if (keyval == GDK_KEY_g || keyval == GDK_KEY_G) {
        /*
         * Shift takes the whole conversation to $EDITOR as org; plain
         * Ctrl+G takes the message being written. Same gesture, and the
         * difference is how much of it you meant.
         */
        if ((state & GDK_SHIFT_MASK) != 0)
            on_conversation_action(self, "edit-org", NULL);
        else
            compose_in_editor(self);

        return GDK_EVENT_STOP;
    }

    /*
     * Ctrl+V is intercepted only when there is actually an image on the
     * clipboard; ordinary text paste is left to the entry, which
     * already does it correctly.
     */
    if (keyval == GDK_KEY_v || keyval == GDK_KEY_V)
        return paste_image(self) ? GDK_EVENT_STOP : GDK_EVENT_PROPAGATE;

    return GDK_EVENT_PROPAGATE;
}

void
clawt_gtk_load_history(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    clawt_gtk_reset_transcript(self);
    clawt_gtk_set_activity(self, NULL);
    g_clear_pointer(&self->selected_room, g_free);
    g_hash_table_remove_all(self->shown);

    if (self->selected_agent == NULL)
        return;

    /*
     * The operator's own conversation, or one between this agent and a
     * peer. The daemon resolves either from a member and a viewer, so
     * neither client has to know how a direct room is named.
     */
    reply = clawt_window_request(
        self, "room.history",
        (self->selected_conversation != NULL)
        ? clawt_build_payload("room", self->selected_conversation,
                              "as", self->selected_agent, NULL)
        : clawt_build_payload("room", self->selected_agent, "as", "user",
                              NULL));

    if (reply == NULL)
        return;

    /*
     * The daemon says which room it resolved the agent to, and that is
     * what later messages are matched against.
     */
    self->selected_room = g_strdup(
        clawt_json_string(clawt_payload_of(reply), "room", NULL));

    messages = json_object_get_array_member(clawt_payload_of(reply),
                                            "messages");

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_json_string(message, "sender", "?");
        const gchar *id = clawt_json_string(message, "id", NULL);

        if (id != NULL)
            g_hash_table_add(self->shown, g_strdup(id));

        clawt_gtk_append_message(self, sender, clawt_json_string(message, "body", ""),
                                 g_strcmp0(sender, "user") == 0,
                                 clawt_json_int(message, "ts", 0));
    }

    clawt_gtk_set_following(self, TRUE);
    clawt_gtk_queue_scroll(self);
}

static void
on_send(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *full = NULL;
    g_autofree gchar *body = NULL;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    body = clawt_gtk_entry_text(self);

    if (body == NULL || *body == '\0')
        return;

    /*
     * A command never reaches the agent.  A mistyped one arriving as a
     * message is how somebody learns not to trust the feature.
     */
    if (run_slash_command(self, body)) {
        clawt_gtk_entry_set_text(self, "");
        return;
    }

    full = body_with_attachments(self, body);

    reply = clawt_window_request(
        self, "msg.send",
        clawt_build_payload("target", self->selected_agent, "body", full,
                            "from", "user", NULL));

    if (reply == NULL)
        return;

    /*
     * Not drawn here.  The daemon publishes an event for every message
     * it routes, this one included, so letting the send path draw its
     * own would put it on screen twice -- and the event is the version
     * that carries the room, which is what the transcript is filtered
     * on.
     */
    clawt_gtk_entry_set_text(self, "");
    g_hash_table_remove(self->drafts, self->selected_agent);
    clawt_gtk_persist_draft(self, self->selected_agent, NULL);

    /*
     * A stopped agent accepts the message -- that is what a durable
     * mailbox is for -- but it will not answer it, and a spinner that
     * spins forever is a worse lie than no spinner at all.  The daemon
     * reports the target's state so this does not have to be inferred
     * from a sidebar that may be a refresh behind.
     */
    {
        const gchar *state = clawt_json_string(clawt_payload_of(reply),
                                               "target_state", NULL);
        gboolean steered = clawt_json_boolean(clawt_payload_of(reply),
                                           "steered", FALSE);

        /*
         * A steer is deliberately absent from the transcript until the
         * turn it is steering has ended, so this has to say where it
         * went: a message that leaves the composer and appears nowhere
         * reads exactly like a message that was lost.
         */
        if (steered) {
            g_autofree gchar *note = g_strdup_printf(
                "%s is mid-turn -- held, and sent when this turn ends",
                self->selected_agent);

            clawt_window_toast(self, note);
        } else if (state != NULL && g_strcmp0(state, "running") != 0) {
            g_autofree gchar *warning = g_strdup_printf(
                "%s is %s -- held in its mailbox until it starts",
                self->selected_agent, state);

            clawt_gtk_set_activity(self, NULL);
            clawt_window_toast(self, warning);
        } else {
            clawt_gtk_set_activity(self, "delivered -- waiting for a reply");
        }
    }

    clawt_gtk_set_following(self, TRUE);
    clawt_gtk_queue_scroll(self);
}

/*
 * The conversations this agent is in, and which one is on screen.
 *
 * An agent has one with its operator and one with each peer it has
 * talked to. Work handed down a chain is answered back up it, so those
 * peer conversations are where a delegated answer actually is -- and
 * before this there was nowhere to read them: the operator saw an agent
 * go busy and had no way to see what it was saying to whom.
 *
 * Filled from room.list per agent rather than kept, because a direct
 * room is created the first time two agents speak; an agent that has
 * never delegated has one conversation and no switcher at all.
 */
void
clawt_gtk_fill_conversation_menu(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *rooms;
    GAction *action;
    guint peers = 0;
    guint i;

    if (self->conversation_menu == NULL)
        return;

    g_menu_remove_all(self->conversation_menu);

    if (self->selected_agent == NULL) {
        gtk_widget_set_visible(self->conversation_bar, FALSE);
        return;
    }

    /*
     * The operator's own, always first and always present. It is the one
     * the client opens on, and an agent with no peers still needs the
     * button to say which conversation this is when one appears later.
     */
    {
        g_autoptr(GMenuItem) item = g_menu_item_new("Chat", NULL);

        g_menu_item_set_action_and_target_value(item, "win.conversation",
                                                g_variant_new_string(""));
        g_menu_append_item(self->conversation_menu, item);
    }

    reply = clawt_window_request(self, "room.list", NULL);
    rooms = (reply != NULL)
        ? json_object_get_array_member(clawt_payload_of(reply), "rooms")
        : NULL;

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        JsonArray *members = json_object_get_array_member(room, "members");
        g_autofree GStrv ids = NULL;
        const gchar *peer;
        guint m;

        if (members == NULL)
            continue;

        ids = g_new0(gchar *, json_array_get_length(members) + 1);

        for (m = 0; m < json_array_get_length(members); m++)
            ids[m] = (gchar *)json_array_get_string_element(members, m);

        peer = clawt_chat_conversation_peer((const gchar *const *)ids,
                                            self->selected_agent);

        /*
         * The operator's own room is already the first entry, and a room
         * this agent is not in belongs to somebody else.
         */
        if (peer == NULL || g_strcmp0(peer, "user") == 0)
            continue;

        {
            g_autofree gchar *label = g_strdup_printf("with %s", peer);
            g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);

            g_menu_item_set_action_and_target_value(
                item, "win.conversation", g_variant_new_string(peer));
            g_menu_append_item(self->conversation_menu, item);
            peers++;
        }
    }

    /*
     * Hidden entirely when there is nothing to switch to. A control
     * offering one choice is a control that costs a line of the
     * transcript and answers a question nobody asked.
     */
    gtk_widget_set_visible(self->conversation_bar, peers > 0);

    /*
     * A peer conversation is read-only. Typing into it would post as the
     * operator into a room the operator is not a member of -- and what
     * the composer actually sends is a message to the agent, which would
     * land in the main chat while you were looking at a different one.
     *
     * Insensitive rather than hidden: a composer that disappears reads
     * as the client having lost the connection.
     */
    if (self->entry != NULL) {
        gboolean own = self->selected_conversation == NULL;

        gtk_widget_set_sensitive(GTK_WIDGET(self->entry), own);
        gtk_text_view_set_editable(self->entry, own);
    }

    action = g_action_map_lookup_action(G_ACTION_MAP(self), "conversation");

    if (action != NULL)
        g_simple_action_set_state(
            G_SIMPLE_ACTION(action),
            g_variant_new_string(self->selected_conversation != NULL
                                 ? self->selected_conversation : ""));

    {
        g_autofree gchar *label =
            (self->selected_conversation != NULL)
            ? g_strdup_printf("with %s", self->selected_conversation)
            : g_strdup("Chat");

        gtk_menu_button_set_label(GTK_MENU_BUTTON(self->conversation_button),
                                  label);
    }
}

void
clawt_gtk_on_conversation_chosen(GSimpleAction *action, GVariant *parameter,
                                 gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *peer = g_variant_get_string(parameter, NULL);

    g_simple_action_set_state(action, g_variant_new_string(peer));

    g_clear_pointer(&self->selected_conversation, g_free);

    if (*peer != '\0')
        self->selected_conversation = g_strdup(peer);

    clawt_gtk_load_history(self);
    clawt_gtk_fill_conversation_menu(self);
}

GtkWidget *
clawt_gtk_build_chat_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *attach;
    GtkWidget *send;

    /*
     * The width the measure is a share of, and the notify that makes a
     * share mean anything.  Without it the column would be nine tenths
     * of whatever the window was when the page was built and would stay
     * there for ever -- a setting that works once and then silently
     * stops tracking, which reads as it having never worked.
     */
    self->chat_scroll = scroll;
    clawt_gtk_follow_viewport_width(self, scroll);

    self->transcript = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

    /*
     * The last turn should not sit against the composer.  18px because
     * this is a container edge meeting a different control, not a gap
     * between two turns.
     */
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->transcript), 18);

    /*
     * A measure limit, so widening the window widens the margins rather
     * than the lines.
     *
     * Nothing capped the column, so a body label's natural width was
     * whatever the window was: a 1280px window measured 877px of text on
     * its longest line, about 137 characters, and this scales with the
     * display -- a wider one is worse rather than equal.  The clamp
     * bounds it instead.
     *
     * Continuous prose reads comfortably at
     * roughly 45 to 90 characters; past that the eye has to cross the
     * full width and then hunt back for the start of the next line,
     * which is the failure measure exists to prevent.
     *
     * The clamp goes inside the scrolled window rather than around it,
     * so the scrollbar and the wheel target stay at the window edge
     * where they are reachable.
     *
     * AdwClamp's own defaults are the right numbers and are deliberately
     * left alone: maximum-size is 600 and tightening-threshold is 400,
     * which is what stops the column snapping in a narrow window.  A
     * default left unset is a number the platform can revise; a
     * hardcoded one is a number somebody has to maintain.
     *
     * What 600 leaves for words is 600 less CHAT_BODY_INSET at the start
     * and CHAT_ROW_MARGIN at the end: 532px, about 82 characters in the
     * default font.  An earlier reading of this said 576 and 90, which
     * forgot that an agent's body is indented past its avatar as well as
     * inset from the clamp -- 44px of it, seven characters' worth, on
     * every line.
     *
     * The body does not fill even that.  A wrapping GtkLabel set
     * GTK_ALIGN_START is allocated its natural width rather than the
     * column's, and measured here that is 437px against the 532px
     * offered -- about 66 characters.  So 82 is what the clamp permits
     * and 66 is what a reader sees; the two differ by the label's
     * alignment, not by anything decided here.
     */
    {
        GtkWidget *clamp = adw_clamp_new();

        adw_clamp_set_maximum_size(ADW_CLAMP(clamp), clawt_gtk_chat_measure(self));
        self->transcript_clamp = clamp;
        adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(self->transcript));
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    }

    /*
     * Right-clicking the conversation itself, rather than one message.
     * Attached to the scrolled window so the empty space below the last
     * message counts too -- that is where somebody actually right
     * clicks.
     */
    {
        static const MenuEntry conversation_menu[] = {
            { "Copy conversation as text",     "copy-text" },
            { "Copy conversation as markdown", "copy-markdown" },
            { "Copy conversation as org",      "copy-org" },
            { NULL, NULL },
            { "Open in $EDITOR as text",       "edit-text" },
            { "Open in $EDITOR as markdown",   "edit-markdown" },
            { "Open in $EDITOR as org",        "edit-org" },
            { NULL, NULL },
            { "Save as text\xe2\x80\xa6",      "save-text" },
            { "Save as markdown\xe2\x80\xa6",  "save-markdown" },
            { "Save as org\xe2\x80\xa6",       "save-org" }
        };

        add_context_menu(self, scroll, conversation_menu,
                         G_N_ELEMENTS(conversation_menu),
                         on_conversation_action, NULL);
    }
    gtk_widget_set_vexpand(scroll, TRUE);
    self->transcript_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_widget_set_name(scroll, "clawt-transcript");

    /*
     * Following is maintained from three places: the reader scrolling
     * (below), and the content growing (either of these two), because
     * "am I at the bottom" changes for both reasons.
     */
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "notify::upper", G_CALLBACK(on_transcript_grew), self);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "notify::page-size",
                     G_CALLBACK(on_transcript_grew), self);

    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(on_scrolled), self);

    /*
     * The activity line.  A chat window that shows nothing between the
     * question and the answer is indistinguishable from a broken one,
     * and an agent turn can easily run for minutes.
     */
    self->streaming = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(self->streaming), "dim-label");
    gtk_label_set_wrap(self->streaming, TRUE);
    gtk_label_set_xalign(self->streaming, 0.0f);
    gtk_label_set_ellipsize(self->streaming, PANGO_ELLIPSIZE_END);

    self->activity_spinner = GTK_SPINNER(gtk_spinner_new());
    self->activity_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->activity_bar),
                   GTK_WIDGET(self->activity_spinner));
    gtk_box_append(GTK_BOX(self->activity_bar), GTK_WIDGET(self->streaming));
    gtk_widget_set_visible(self->activity_bar, FALSE);

    /* The queued files, hidden until there are some. */
    self->attachments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(self->attachments, 6);
    gtk_widget_set_visible(self->attachments, FALSE);

    self->entry = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(self->entry, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(self->entry, 8);
    gtk_text_view_set_bottom_margin(self->entry, 8);
    gtk_text_view_set_left_margin(self->entry, 8);
    gtk_text_view_set_right_margin(self->entry, 8);
    gtk_widget_set_hexpand(GTK_WIDGET(self->entry), TRUE);
    g_signal_connect(gtk_text_view_get_buffer(self->entry), "changed",
                     G_CALLBACK(on_entry_changed), self);

    /*
     * The placeholder is a label under the view rather than a property,
     * because GtkTextView has none.  Hidden as soon as anything is
     * typed, and it must not eat clicks meant for the text.
     */
    self->placeholder = gtk_label_new(
        "Message  \xe2\x80\x94  / for commands, Ctrl+G to write it in $EDITOR");
    gtk_widget_add_css_class(self->placeholder, "dim-label");
    gtk_widget_set_halign(self->placeholder, GTK_ALIGN_START);
    gtk_widget_set_valign(self->placeholder, GTK_ALIGN_START);
    gtk_widget_set_margin_start(self->placeholder, 10);
    gtk_widget_set_margin_top(self->placeholder, 8);
    gtk_widget_set_can_target(self->placeholder, FALSE);

    {
        GtkEventController *keys = gtk_event_controller_key_new();

        /*
         * The capture phase, so Ctrl+V is seen before the entry's own
         * paste handler consumes it.
         */
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), self);
        gtk_widget_add_controller(GTK_WIDGET(self->entry), keys);
    }

    /*
     * The command list, parented to the entry it belongs to.  Note that
     * this makes it a *child* of the entry, so anything that walks the
     * entry's children has to expect it.
     */
    self->command_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->command_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->command_list),
                             "navigation-sidebar");
    g_signal_connect(self->command_list, "row-selected",
                     G_CALLBACK(on_command_row_selected), self);

    {
        GtkWidget *command_scroll = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(command_scroll),
                                      GTK_WIDGET(self->command_list));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(command_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(command_scroll), 220);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(command_scroll), TRUE);

        self->command_revealer = gtk_revealer_new();
        gtk_revealer_set_transition_type(
            GTK_REVEALER(self->command_revealer),
            GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
        gtk_revealer_set_child(GTK_REVEALER(self->command_revealer),
                               command_scroll);
    }

    attach = gtk_button_new_from_icon_name("mail-attachment-symbolic");
    gtk_widget_set_name(attach, "clawt-attach");
    gtk_widget_set_tooltip_text(attach,
                                "Send files with this message. You can also "
                                "paste an image.");
    g_signal_connect(attach, "clicked", G_CALLBACK(on_attach_clicked), self);

    send = gtk_button_new_from_icon_name("document-send-symbolic");
    gtk_widget_set_name(send, "clawt-send");
    g_signal_connect(send, "clicked", G_CALLBACK(on_send), self);

    /*
     * Stop, beside Send rather than in the agent menu, because it is
     * about the turn happening in front of you: the menu's Stop takes
     * the agent down and needs a start afterwards, and reaching for that
     * one to end a runaway turn costs the container or VM as well.
     */
    self->stop_turn = gtk_button_new_from_icon_name(
                          "process-stop-symbolic");
    gtk_widget_set_name(self->stop_turn, "clawt-stop-turn");
    gtk_widget_add_css_class(self->stop_turn, "destructive-action");
    gtk_widget_set_tooltip_text(self->stop_turn,
                                "Stop what this agent is doing now. Kills "
                                "the CLI running the turn and everything it "
                                "started; the agent stays up.");
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->stop_turn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Stop this turn", -1);
    gtk_widget_set_visible(self->stop_turn, FALSE);
    g_signal_connect(self->stop_turn, "clicked", G_CALLBACK(on_stop_turn),
                     self);

    {
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *entry_scroll = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(entry_scroll),
                                      GTK_WIDGET(self->entry));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(entry_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);

        /*
         * Grows with the message and then stops: a pasted essay should
         * not push the transcript off the top of the window.
         */
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(entry_scroll), 200);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(entry_scroll), TRUE);
        gtk_widget_add_css_class(entry_scroll, "card");
        gtk_widget_set_name(entry_scroll, "clawt-entry");
        gtk_widget_set_hexpand(entry_scroll, TRUE);

        gtk_overlay_set_child(GTK_OVERLAY(overlay), entry_scroll);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->placeholder);

        gtk_box_append(GTK_BOX(entry_box), overlay);
    }

    gtk_widget_set_valign(attach, GTK_ALIGN_END);
    gtk_widget_set_valign(send, GTK_ALIGN_END);
    gtk_widget_set_valign(self->stop_turn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(entry_box), attach);
    gtk_box_append(GTK_BOX(entry_box), self->stop_turn);
    gtk_box_append(GTK_BOX(entry_box), send);
    gtk_widget_set_margin_top(entry_box, 6);
    gtk_widget_set_margin_bottom(entry_box, 12);

    /*
     * The pill that says something arrived while you were reading.
     *
     * It floats over the transcript rather than sitting in the column,
     * because it is about the transcript rather than part of it, and it
     * carries the words -- a bare arrow says "go down", which the
     * scrollbar already says.  What was missing was "something is down
     * there", and that needs saying once, not counting: a message here
     * is a whole turn, so "3" could be three lines or three screens.
     *
     * It appears only when a message has arrived while `following` is
     * false, not merely because the reader has scrolled up.  A control
     * that is always there while you read carries no information; one
     * whose appearance is the signal carries exactly the bit that is
     * missing.
     */
    {
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *revealer = gtk_revealer_new();
        GtkWidget *pill = gtk_button_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_box_append(GTK_BOX(content),
                       gtk_image_new_from_icon_name("go-bottom-symbolic"));
        gtk_box_append(GTK_BOX(content), gtk_label_new("New messages"));

        gtk_button_set_child(GTK_BUTTON(pill), content);
        gtk_widget_add_css_class(pill, "osd");
        gtk_widget_add_css_class(pill, "pill");
        gtk_widget_add_css_class(pill, "clawt-jump-pill");
        gtk_widget_set_tooltip_text(pill, "Jump to latest");
        gtk_accessible_update_property(GTK_ACCESSIBLE(pill),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Jump to latest", -1);
        g_signal_connect(pill, "clicked", G_CALLBACK(on_jump_to_latest),
                         self);

        gtk_revealer_set_child(GTK_REVEALER(revealer), pill);
        gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                         GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);

        /*
         * Centred over the transcript, 12px clear of the composer.  Not
         * anchored to the end: the reader this is for is scrolled up and
         * therefore the one most likely to be holding the scrollbar.
         */
        gtk_widget_set_halign(revealer, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(revealer, GTK_ALIGN_END);
        gtk_widget_set_margin_bottom(revealer, 12);
        gtk_widget_set_can_target(revealer, FALSE);

        self->jump_revealer = GTK_REVEALER(revealer);

        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), revealer);

        self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
        adw_toast_overlay_set_child(self->toasts, overlay);
        gtk_widget_set_vexpand(GTK_WIDGET(self->toasts), TRUE);

        gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->toasts));
    }

    /*
     * Which conversation is on screen, above the transcript rather than
     * on the tab: the tab says what kind of thing this page is, and this
     * says which of several the page is showing.
     */
    {
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *button = gtk_menu_button_new();

        self->conversation_menu = g_menu_new();

        gtk_menu_button_set_menu_model(
            GTK_MENU_BUTTON(button),
            G_MENU_MODEL(self->conversation_menu));
        gtk_widget_add_css_class(button, "flat");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(button), "Chat");

        gtk_widget_set_margin_start(bar, 12);
        gtk_widget_set_margin_end(bar, 12);
        gtk_widget_set_margin_top(bar, 6);
        gtk_box_append(GTK_BOX(bar), button);

        self->conversation_bar = bar;
        self->conversation_button = button;

        /* Nothing to switch to until an agent has a peer conversation. */
        gtk_widget_set_visible(bar, FALSE);
        gtk_box_prepend(GTK_BOX(box), bar);
    }

    /*
     * The composer follows the transcript's column.
     *
     * It becomes visible the moment the transcript is clamped: a
     * full-width entry under a narrow column of text looks like a
     * rendering fault rather than a layout.  The thing you read and the
     * thing you type into should be the same column, so the same clamp
     * wraps the whole composer cluster -- the activity line, the
     * slash-command list, the staged attachments and the entry.
     */
    {
        GtkWidget *composer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_name(composer, "clawt-composer");
        GtkWidget *clamp = adw_clamp_new();

        /*
         * The same measure as the transcript, from the same resolver.
         * A column somebody widened while the box they type into stayed
         * put would reproduce the misalignment this whole arrangement
         * exists to fix, and only for the people who changed it.
         */
        adw_clamp_set_maximum_size(ADW_CLAMP(clamp), clawt_gtk_chat_measure(self));
        self->composer_clamp = clamp;

        /*
         * And it stands on the same line as the words above it.
         *
         * The same clamp is not the same column: a row spends
         * CHAT_ROW_MARGIN plus CHAT_GUTTER of its 600 before a body
         * starts, and the composer spent only CHAT_ROW_MARGIN -- so the
         * entry's frame, the strongest vertical in the whole page, stood
         * CHAT_GUTTER left of every line of text and inside the one
         * column deliberately left empty.  Insetting by CHAT_BODY_INSET
         * puts it back under the text; CHAT_ROW_MARGIN on the trailing
         * edge is what a row already ends at, so both ends agree.
         *
         * This aligns the frame, not the text inside it.  GtkText keeps
         * an inset of its own, so the caret sits a little inside the
         * rail -- which is right, because a bordered control reads as a
         * box and the box's edge is the line the eye follows down.
         */
        gtk_widget_set_margin_start(composer, CHAT_BODY_INSET);
        gtk_widget_set_margin_end(composer, CHAT_ROW_MARGIN);

        gtk_box_append(GTK_BOX(composer), self->activity_bar);
        gtk_box_append(GTK_BOX(composer), self->command_revealer);
        gtk_box_append(GTK_BOX(composer), self->attachments);
        gtk_box_append(GTK_BOX(composer), entry_box);

        adw_clamp_set_child(ADW_CLAMP(clamp), composer);
        gtk_box_append(GTK_BOX(box), clamp);
    }

    return box;
}
