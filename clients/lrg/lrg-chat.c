/*
 * lrg-chat.c - The Chat page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The transcript, the composer, what a message and a conversation can be
 * made to do, and the slash commands the composer answers: the libregnum
 * half of clients/gtk/gtk-chat.c and gtk-follow.c, plus the chat's share
 * of clawt-window.c's event handling -- unread counts and typing levels
 * are kept here, in one place, for the whole window.
 *
 * Every request is async (clawt_lrg_request()), so where GTK waits inside
 * a function this file continues in a reply callback; the generation
 * counter does the job GTK's nested-wait guards did.  Everything the
 * page decides is in lrg-chat-model.c.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-chat-model.h"

#include <glib/gstdio.h>
#include <graylib.h>
#include <raylib.h>
#include <string.h>
#include <unistd.h>

/*
 * The column's geometry.  A body starts past the avatar by the gutter;
 * the composer stands on the same line through CHAT_BODY_INSET, because
 * the entry's frame is the strongest vertical on the page and it belongs
 * under the text, not in the column deliberately left empty.
 */
#define CHAT_ROW_MARGIN        12
#define CHAT_AVATAR            40
#define CHAT_GUTTER            (CHAT_AVATAR + CHAT_ROW_MARGIN)
#define CHAT_BODY_INSET        clawt_chat_body_inset(CHAT_ROW_MARGIN, \
                                                     CHAT_GUTTER)

/*
 * The three breaks a reader sees, in order: a paragraph inside a message
 * (markdown's own), a new message inside a run, a new run.  A message gap
 * at or below a paragraph's reads as one wall of text.
 */
#define CHAT_PARAGRAPH_SPACING 18
#define CHAT_MESSAGE_SPACING   24
#define CHAT_RUN_SPACING       30

/* How many live steps are kept: a long turn must not grow without end. */
#define CHAT_MAX_STEPS         200

/* How many of a freshly loaded history fly in; the rest just appear. */
#define CHAT_HISTORY_ENTRANCES 8

typedef enum {
    COMPLETION_COMMAND,
    COMPLETION_MENTION
} CompletionKind;

typedef struct {
    CompletionKind  kind;
    gchar          *value;
    gboolean        takes_argument;
    LrgWidget      *widget;    /* borrowed: the row in the list */
} Completion;

typedef struct {
    gchar  *name;
    GBytes *bytes;
} PendingFile;

typedef struct {
    ClawtLrgApp *app;

    /* ── widgets ── */
    LrgWidget   *page;
    LrgWidget   *conversation_bar;
    LrgWidget   *conversation_button;
    LrgWidget   *scroll;
    LrgWidget   *column;
    LrgWidget   *transcript;
    LrgWidget   *live_steps;     /* borrowed: the running turn's block */
    LrgWidget   *unread_rule;    /* borrowed: "New messages" in the text */
    LrgWidget   *jump;           /* the pill over the transcript */
    LrgWidget   *composer;
    LrgWidget   *activity;
    LrgWidget   *activity_label;
    LrgWidget   *completions;
    LrgWidget   *completion_card;
    LrgWidget   *attachments;
    LrgWidget   *editor;
    LrgWidget   *stop_button;
    LrgWidget   *send_button;
    LrgWidget   *char_probe;     /* "0000000000", for a measure in ch */
    GSimpleActionGroup *actions;

    /* ── what is on screen ── */
    gboolean     primed;         /* the first refresh has run */
    gchar       *key;            /* agent id or room id loaded */
    gchar       *room;           /* the room the daemon resolved */
    gchar       *conversation;   /* a peer, when reading one */
    guint        history_generation;
    guint        history_inflight;
    GHashTable  *shown;          /* message ids drawn */
    ClawtLrgRun  run;
    GPtrArray   *steps;          /* the running turn's, for the live block */
    gboolean     can_interrupt;

    /* ── following the live edge ── */
    gboolean     following;
    gboolean     auto_scrolling;
    gboolean     jump_instantly;
    gfloat       last_scroll;
    gint         measure;
    gfloat       measured_in;    /* the width it was resolved against */

    /* ── the composer ── */
    GPtrArray   *pending;        /* PendingFile */
    GHashTable  *drafts;         /* key -> text, this run */
    GPtrArray   *rows;           /* Completion */
    gint         highlight;
    JsonNode    *skill_commands; /* cached while the line is a command */
    guint        skill_generation; /* the selection they were asked for */
    gboolean     skill_asked;
    gboolean     sending;

    /* ── the connection this state describes ── */
    gint64       connected_at;
    ClawtClient *client_seen;
} Chat;

/* The most completion rows drawn at once; the rest are counted, not built. */
#define COMPLETION_MAX_ROWS (12)

static Chat chat;

static void append_local(ClawtLrgApp *app, const gchar *text);
static void load_history(ClawtLrgApp *app);
static void send_text(ClawtLrgApp *app, const gchar *text);
static void entry_changed(ClawtLrgApp *app);
static void conversation_action(ClawtLrgApp *app, const gchar *action);

/* ── Small helpers ────────────────────────────────────────────────── */

static gfloat
px(ClawtLrgApp *app, gfloat value)
{
    return value * (gfloat)app->scale;
}

/* The room selected as a room, or NULL when an agent is. */
static const gchar *
room_entry(ClawtLrgApp *app)
{
    return (app->agent_id == NULL) ? app->room_id : NULL;
}

/* What the selection is called: an export's name, a draft's key. */
static const gchar *
selected_label(ClawtLrgApp *app)
{
    return (app->agent_id != NULL) ? app->agent_id : app->room_id;
}

static void
completion_free(gpointer data)
{
    Completion *row = data;

    g_free(row->value);
    g_free(row);
}

static void
pending_file_free(gpointer data)
{
    PendingFile *file = data;

    g_free(file->name);
    g_clear_pointer(&file->bytes, g_bytes_unref);
    g_free(file);
}

/*
 * The state describes one daemon.  A client switched to another one
 * starts again: whatever that daemon replays happened before this window
 * met it, and a draft keyed on an id may name somebody else's agent.
 */
static void
notice_connection(ClawtLrgApp *app)
{
    if (chat.client_seen == app->client)
        return;

    chat.client_seen = app->client;
    chat.connected_at = g_get_real_time();

    if (chat.drafts != NULL)
        g_hash_table_remove_all(chat.drafts);

    if (chat.shown != NULL)
        g_hash_table_remove_all(chat.shown);

    g_clear_pointer(&chat.key, g_free);
    g_clear_pointer(&chat.room, g_free);
    g_clear_pointer(&chat.conversation, g_free);
    chat.primed = FALSE;
}

/* ── A row that can be clicked ────────────────────────────────────── */

/*
 * The same construction as the sidebar's: a surface holding the content
 * and an invisible catcher over it that turns press-and-release into a
 * click.  Right presses are left alone so they bubble to a context menu.
 */
typedef void (*Clicked)(ClawtLrgApp *app, gpointer user_data);

typedef struct {
    ClawtLrgApp    *app;
    Clicked         func;
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
clickable_new(ClawtLrgApp *app, LrgWidget *content, const gchar *style_class,
              Clicked func, gpointer user_data, GDestroyNotify destroy)
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
    lrg_widget_add_class(LRG_WIDGET(surface),
                         style_class != NULL ? style_class : "clawt-row");
    lrg_widget_remove_class(content, "clawt-row");
    clawt_lrg_take(LRG_WIDGET(surface), content);

    lrg_custom_widget_set_paint_box(catcher, FALSE);
    g_signal_connect_data(catcher, "event", G_CALLBACK(on_catcher_event),
                          clicker,
                          (GClosureNotify)(void (*)(void))clicker_free, 0);
    clawt_lrg_take(LRG_WIDGET(surface), LRG_WIDGET(catcher));

    return LRG_WIDGET(surface);
}

/* A horizontal rule that takes whatever width it is given. */
static LrgWidget *
rule_new(void)
{
    LrgSeparator *line = lrg_separator_new_with_orientation(
        LRG_ORIENTATION_HORIZONTAL);

    lrg_widget_set_fill_weight(LRG_WIDGET(line), 1.0f);
    lrg_widget_set_valign(LRG_WIDGET(line), LRG_ALIGN_CENTER);
    return LRG_WIDGET(line);
}

/* ── Images ───────────────────────────────────────────────────────── */

/*
 * Decoded straight to the size it is drawn at, never larger: a size
 * request is no cap anywhere, and a transcript of screenshots held at
 * full resolution is memory nobody asked to spend.
 */
static GrlTexture *
texture_fit(GrlImage *image, gint max_width, gint max_height)
{
    gint width = grl_image_get_width(image);
    gint height = grl_image_get_height(image);
    gdouble shrink;

    if (width <= 0 || height <= 0)
        return NULL;

    /* scale down only: a small picture at its own size, not blown up */
    shrink = MIN(1.0, MIN((gdouble)max_width / width,
                          (gdouble)max_height / height));

    if (shrink < 1.0)
        grl_image_resize(image, MAX(1, (gint)(width * shrink)),
                         MAX(1, (gint)(height * shrink)));

    return grl_texture_new_from_image(image);
}

static LrgWidget *
image_widget(GrlTexture *texture, gfloat corner)
{
    LrgImage *image = lrg_image_new_with_texture(texture);
    gfloat w = (gfloat)grl_texture_get_width(texture);
    gfloat h = (gfloat)grl_texture_get_height(texture);

    lrg_image_set_scale_mode(image, LRG_IMAGE_SCALE_MODE_FIT);
    lrg_image_set_corner_radius(image, corner);
    lrg_widget_set_min_size(LRG_WIDGET(image), w, h);
    lrg_widget_set_max_size(LRG_WIDGET(image), w, h);
    return LRG_WIDGET(image);
}

/* Bytes to a texture, through a file named after their magic number. */
static GrlTexture *
texture_from_bytes(const guchar *data, gsize length, gint max_width,
                   gint max_height)
{
    const gchar *mime = clawt_avatar_sniff_mime_type(data, length);
    g_autofree gchar *template = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GrlImage) image = NULL;
    GrlTexture *texture = NULL;
    gint fd;

    if (mime == NULL)
        return NULL;

    template = clawt_lrg_image_template("clawtilla-picture", mime);
    if (template == NULL)
        return NULL;
    fd = g_file_open_tmp(template, &path, NULL);

    if (fd < 0)
        return NULL;

    close(fd);

    if (g_file_set_contents(path, (const gchar *)data, (gssize)length, NULL)) {
        image = grl_image_new_from_file(path);

        if (image != NULL)
            texture = texture_fit(image, max_width, max_height);
    }

    g_unlink(path);
    return texture;
}

/*
 * One picture full size, beside the conversation.  A dialog rather than
 * a window, because this canvas is the only window -- but its content is
 * the picture at its own size or shrunk to fit, never blown up.
 */
static void
show_picture(ClawtLrgApp *app, const gchar *title, GrlTexture *texture)
{
    LrgWidget *view = image_widget(texture, px(app, 8.0f));

    g_object_unref(clawt_lrg_dialog(app, title, NULL, view, NULL, FALSE,
                                    NULL, NULL, NULL));
    g_object_unref(view);
}

static void
on_avatar_preview(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    const gchar *name = user_data;
    const gchar *base64 = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "base64");
    g_autofree guchar *raw = NULL;
    g_autoptr(GrlTexture) texture = NULL;
    gsize length = 0;

    /*
     * A picture that has gone since the row was drawn is said out loud:
     * silence reads as the click not having registered.
     */
    if (base64 != NULL) {
        raw = g_base64_decode(base64, &length);
        texture = texture_from_bytes(raw, length, (gint)px(app, 640.0f),
                                     (gint)px(app, 640.0f));
    }

    if (texture == NULL) {
        clawt_lrg_toast(app, "That picture is no longer there.");
        return;
    }

    show_picture(app, name, texture);
}

/*
 * Fetched afresh rather than reusing the sidebar's texture: that one is
 * decoded small for a list, and blowing it up is the "larger" that is
 * not larger.
 */
static void
avatar_preview_open(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *agent_id = user_data;
    JsonObject *agent = clawt_lrg_find_agent(app, agent_id);
    const gchar *name = clawt_ipc_payload_string(agent, "name");

    clawt_lrg_request(app, "agent.avatar",
                      clawt_ipc_payload_new("agent", agent_id, NULL),
                      CLAWT_LRG_QUIET, on_avatar_preview,
                      g_strdup(name != NULL ? name : agent_id), g_free);
}

/* ── Copying, exporting ───────────────────────────────────────────── */

static void
copy_text(ClawtLrgApp *app, const gchar *text, const gchar *what)
{
    g_autofree gchar *said = g_strdup_printf("%s copied.", what);

    clawt_lrg_copy(app, text);
    clawt_lrg_toast(app, said);
}

/*
 * Converts and copies, and says so when the format is not available --
 * markdown handed back under an org label is the worst of the answers.
 */
static void
copy_as(ClawtLrgApp *app, const gchar *markdown, ClawtExportFormat format,
        const gchar *what)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *converted = clawt_export_convert(markdown, format,
                                                       &error);

    if (converted == NULL) {
        clawt_lrg_toast(app, error->message);
        return;
    }

    copy_text(app, converted, what);
}

static void
on_document_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = chat.app;
    g_autofree gchar *contents = user_data;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GError) error = NULL;

    file = lrg_file_dialog_save_finish(LRG_FILE_DIALOG(source), result,
                                       &error);

    if (file == NULL) {
        /* dismissing the dialog is not a failure worth a toast */
        if (error != NULL &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_CANCELLED))
            clawt_lrg_toast(app, error->message);

        return;
    }

    if (!g_file_replace_contents(file, contents, strlen(contents), NULL,
                                 FALSE, G_FILE_CREATE_NONE, NULL, NULL,
                                 &error))
        clawt_lrg_toast(app, error->message);
    else
        clawt_lrg_toast(app, "Saved.");
}

/*
 * The contents travel with the dialog rather than being re-derived when
 * a name is picked: by then the conversation may have moved on, and
 * saving something other than what was asked for is worse than not.
 */
static void
save_document(ClawtLrgApp *app, const gchar *contents, const gchar *basename,
              ClawtExportFormat format)
{
    g_autoptr(LrgFileDialog) dialog = lrg_file_dialog_new();
    g_autofree gchar *suggested = g_strconcat(
        basename, clawt_export_format_extension(format), NULL);

    lrg_file_dialog_set_title(dialog, "Save");
    lrg_file_dialog_set_initial_name(dialog, suggested);
    lrg_file_dialog_save(dialog, LRG_WIDGET(app->canvas), NULL,
                         on_document_saved, g_strdup(contents));
}

/*
 * Not deleted afterwards, unlike the composer's scratch file: this is a
 * conversation being taken away to keep.
 */
static void
open_document_in_editor(ClawtLrgApp *app, const gchar *contents,
                        const gchar *basename, ClawtExportFormat format)
{
    g_autofree gchar *template = g_strdup_printf(
        "%s-XXXXXX%s", basename, clawt_export_format_extension(format));
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    gint fd;

    /* a room id may carry a colon; a filename should not */
    g_strdelimit(template, ":/", '-');
    fd = g_file_open_tmp(template, &path, &error);

    if (fd < 0) {
        clawt_lrg_toast(app, error->message);
        return;
    }

    close(fd);

    if (!g_file_set_contents(path, contents, -1, &error)) {
        clawt_lrg_toast(app, error->message);
        g_unlink(path);
        return;
    }

    clawt_lrg_open_in_editor(app, path);
}

static void
on_conversation_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                        gpointer user_data)
{
    const gchar *action = user_data;
    g_autoptr(GPtrArray) messages = NULL;
    g_autofree gchar *markdown = NULL;
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) failure = NULL;
    ClawtExportFormat format = clawt_lrg_export_format(action);
    const gchar *label = selected_label(app);

    if (reply == NULL)
        return;

    /*
     * Read back from the daemon rather than scraped off the widgets: the
     * transcript on screen is the last two hundred messages, and an
     * export that stopped there would be of the window, not the talk.
     */
    messages = clawt_lrg_chat_export_messages(reply, chat.room);
    markdown = clawt_export_transcript(label != NULL ? label : "chat",
                                       messages, CLAWT_EXPORT_MARKDOWN,
                                       &failure);

    if (markdown == NULL) {
        clawt_lrg_toast(app, failure != NULL ? failure->message
                                              : "there is no conversation "
                                                "to export");
        return;
    }

    converted = clawt_export_convert(markdown, format, &failure);

    if (converted == NULL) {
        clawt_lrg_toast(app, failure->message);
        return;
    }

    if (g_str_has_prefix(action, "copy"))
        copy_text(app, converted, "Conversation");
    else if (g_str_has_prefix(action, "edit"))
        open_document_in_editor(app, converted, label != NULL ? label : "chat",
                                format);
    else if (g_str_has_prefix(action, "save"))
        save_document(app, converted, label != NULL ? label : "chat", format);
}

static void
conversation_action(ClawtLrgApp *app, const gchar *action)
{
    JsonNode *payload = clawt_lrg_history_payload(room_entry(app), NULL,
                                                  app->agent_id, "5000");

    if (payload == NULL) {
        clawt_lrg_toast(app, "there is no conversation to export");
        return;
    }

    clawt_lrg_request(app, "room.history", payload, CLAWT_LRG_GUARDED,
                      on_conversation_history, g_strdup(action), g_free);
}

/* ── Context menus ────────────────────────────────────────────────── */

static void
on_message_menu(GSimpleAction *action, GVariant *parameter,
                gpointer user_data)
{
    const gchar *body = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    if (g_strcmp0(name, "copy-markdown") == 0)
        copy_text(chat.app, body, "Message");
    else
        copy_as(chat.app, body, clawt_lrg_export_format(name), "Message");
}

/*
 * Right-clicking a message copies what was actually said, not what the
 * label renders: the body travels with the row's actions.
 */
static void
attach_message_menu(LrgWidget *row, const gchar *body)
{
    static const gchar *const names[] = {
        "copy-markdown", "copy-text", "copy-org"
    };
    g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
    g_autoptr(GMenu) menu = g_menu_new();
    gchar *kept = g_strdup(body);
    gsize i;

    g_object_set_data_full(G_OBJECT(row), "clawt-body", kept, g_free);

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_message_menu),
                         kept);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    g_menu_append(menu, "Copy", "msg.copy-markdown");
    g_menu_append(menu, "Copy as text", "msg.copy-text");
    g_menu_append(menu, "Copy as org", "msg.copy-org");

    lrg_widget_insert_action_group(row, "msg", G_ACTION_GROUP(group));
    lrg_widget_set_context_menu(row, G_MENU_MODEL(menu));
}

/* ── Attachments in the transcript ────────────────────────────────── */

static void
on_attachment_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = chat.app;
    g_autofree gchar *from_path = user_data;
    g_autoptr(GFile) target = NULL;
    g_autoptr(GFile) from = g_file_new_for_path(from_path);
    g_autoptr(GError) error = NULL;

    target = lrg_file_dialog_save_finish(LRG_FILE_DIALOG(source), result,
                                         &error);

    if (target == NULL) {
        if (error != NULL &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_CANCELLED))
            clawt_lrg_toast(app, error->message);

        return;
    }

    if (!g_file_copy(from, target, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                     &error))
        clawt_lrg_toast(app, error->message);
    else
        clawt_lrg_toast(app, "Saved.");
}

static void
on_attachment_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                      gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Deleted. The message still names it.");
    load_history(app);
}

static void
on_attachment_delete_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *path = user_data;
    g_autofree gchar *name = g_path_get_basename(path);

    /*
     * Through the daemon, which owns the exchange directory and checks
     * the name: a client that deletes by path is a client that can be
     * asked to delete any path.
     */
    clawt_lrg_request(app, "attachment.remove",
                      clawt_ipc_payload_new("agent", app->agent_id,
                                            "name", name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_attachment_removed, NULL,
                      NULL);
}

static void
open_with_desktop(ClawtLrgApp *app, const gchar *path)
{
    g_autofree gchar *uri = g_filename_to_uri(path, NULL, NULL);

    if (uri != NULL)
        clawt_lrg_open_uri(app, uri);
}

static void
on_attachment_menu(GSimpleAction *action, GVariant *parameter,
                   gpointer user_data)
{
    ClawtLrgApp *app = chat.app;
    const gchar *path = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    if (g_strcmp0(name, "open") == 0) {
        open_with_desktop(app, path);
    } else if (g_strcmp0(name, "copy-path") == 0) {
        copy_text(app, path, "Path");
    } else if (g_strcmp0(name, "save") == 0) {
        g_autoptr(LrgFileDialog) dialog = lrg_file_dialog_new();
        g_autofree gchar *base = g_path_get_basename(path);

        lrg_file_dialog_set_title(dialog, "Save a copy");
        lrg_file_dialog_set_initial_name(dialog, base);
        lrg_file_dialog_save(dialog, LRG_WIDGET(app->canvas), NULL,
                             on_attachment_saved, g_strdup(path));
    } else if (g_strcmp0(name, "delete") == 0) {
        g_autofree gchar *base = g_path_get_basename(path);
        g_autofree gchar *body = g_strdup_printf(
            "\342\200\234%s\342\200\235 will be deleted from %s's exchange "
            "directory. This cannot be undone, and the message will still "
            "name the file.", base,
            app->agent_id != NULL ? app->agent_id : "the agent");

        clawt_lrg_confirm(app, "Delete this file?", body, "Delete", TRUE,
                          on_attachment_delete_confirmed, g_strdup(path),
                          g_free);
    }
}

static void
attach_attachment_menu(LrgWidget *widget, const gchar *path)
{
    static const gchar *const names[] = { "open", "save", "copy-path",
                                          "delete" };
    g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) main_section = g_menu_new();
    g_autoptr(GMenu) danger = g_menu_new();
    gchar *kept = g_strdup(path);
    gsize i;

    g_object_set_data_full(G_OBJECT(widget), "clawt-path", kept, g_free);

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_attachment_menu),
                         kept);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    g_menu_append(main_section, "Open", "file.open");
    g_menu_append(main_section, "Save a copy\342\200\246", "file.save");
    g_menu_append(main_section, "Copy path", "file.copy-path");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(main_section));
    g_menu_append(danger, "Delete permanently", "file.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(danger));

    lrg_widget_insert_action_group(widget, "file", G_ACTION_GROUP(group));
    lrg_widget_set_context_menu(widget, G_MENU_MODEL(menu));
}

static void
on_picture_clicked(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *path = user_data;
    g_autoptr(GrlImage) image = grl_image_new_from_file(path);
    g_autoptr(GrlTexture) texture = NULL;
    g_autofree gchar *title = g_path_get_basename(path);

    if (image != NULL)
        texture = texture_fit(image, (gint)px(app, 900.0f),
                              (gint)px(app, 640.0f));

    if (texture == NULL) {
        clawt_lrg_toast(app, "That picture cannot be opened here.");
        return;
    }

    show_picture(app, title, texture);
}

static void
on_file_chip_clicked(ClawtLrgApp *app, gpointer user_data)
{
    open_with_desktop(app, user_data);
}

/*
 * A file that is not a picture: a name, a size and the same menu, and a
 * click opens it -- a PDF in a conversation is something to read, not a
 * path to go and find.
 */
static void
append_file_chip(ClawtLrgApp *app, LrgWidget *into, const gchar *path,
                 gboolean from_user)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL,
        NULL);
    g_autofree gchar *name = g_path_get_basename(path);
    g_autofree gchar *label = NULL;
    LrgWidget *content = clawt_lrg_hbox(8.0f);
    LrgWidget *chip;

    if (info != NULL) {
        g_autofree gchar *size = g_format_size(
            (guint64)g_file_info_get_size(info));

        label = g_strdup_printf("%s  \302\267  %s", name, size);
    } else {
        label = g_strdup(name);
    }

    lrg_container_set_padding(LRG_CONTAINER(content), 10.0f);
    clawt_lrg_take(content, clawt_lrg_badge("FILE", "info"));
    clawt_lrg_take(content, clawt_lrg_label(label, NULL));

    chip = clickable_new(app, content, "clawt-card", on_file_chip_clicked,
                         g_strdup(path), g_free);
    lrg_widget_set_halign(chip, from_user ? LRG_ALIGN_END : LRG_ALIGN_START);
    attach_attachment_menu(chip, path);
    clawt_lrg_take(into, chip);
}

/* One attachment, wherever it now is on this machine. */
static void
append_attachment(ClawtLrgApp *app, LrgWidget *into, const gchar *path,
                  gboolean from_user)
{
    g_autoptr(GrlImage) image = NULL;
    g_autoptr(GrlTexture) texture = NULL;
    LrgWidget *picture;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return;

    if (clawt_lrg_looks_like_image(path))
        image = grl_image_new_from_file(path);

    /* big enough to see, as a chat client shows one; an SVG gets a chip */
    if (image != NULL)
        texture = texture_fit(image, (gint)px(app, 460.0f),
                              (gint)px(app, 320.0f));

    if (texture == NULL) {
        append_file_chip(app, into, path, from_user);
        return;
    }

    picture = clickable_new(app, image_widget(texture, px(app, 12.0f)),
                            "clawt-card", on_picture_clicked, g_strdup(path),
                            g_free);
    lrg_widget_set_halign(picture,
                          from_user ? LRG_ALIGN_END : LRG_ALIGN_START);
    lrg_widget_set_appear(picture, LRG_APPEAR_ZOOM, 0.05f);
    attach_attachment_menu(picture, path);
    clawt_lrg_take(into, picture);
}

typedef struct {
    LrgWidget *into;      /* held */
    gboolean   from_user;
    gchar     *id;
} AttachmentFetch;

static void
attachment_fetch_free(gpointer data)
{
    AttachmentFetch *fetch = data;

    g_clear_object(&fetch->into);
    g_free(fetch->id);
    g_free(fetch);
}

static gchar *
attachment_cache_path(const gchar *id)
{
    g_autofree gchar *safe = g_strdup(id);

    /* an id is the daemon's, and never a path of its own */
    g_strdelimit(safe, "/\\", '_');
    return g_build_filename(g_get_user_cache_dir(), "clawtilla",
                            "attachments", safe, NULL);
}

static void
on_agent_attachment(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                    gpointer user_data)
{
    AttachmentFetch *fetch = user_data;
    const gchar *encoded = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "base64");
    g_autofree gchar *path = attachment_cache_path(fetch->id);
    g_autofree gchar *dir = g_path_get_dirname(path);
    g_autofree guchar *bytes = NULL;
    gsize length = 0;

    if (encoded == NULL || g_mkdir_with_parents(dir, 0700) != 0)
        return;

    bytes = g_base64_decode(encoded, &length);

    if (!g_file_set_contents(path, (const gchar *)bytes, (gssize)length, NULL))
        return;

    append_attachment(app, fetch->into, path, fetch->from_user);
    clawt_lrg_app_wake(app);
}

/*
 * An agent's file is `clawt:<id>`: a copy the daemon took at send time,
 * on a machine that may not be this one, which is why the bytes travel.
 * Cached so a transcript redrawn on every switch does not fetch every
 * picture in it again.
 */
static void
fetch_agent_attachment(ClawtLrgApp *app, LrgWidget *into, const gchar *id,
                       gboolean from_user)
{
    g_autofree gchar *path = attachment_cache_path(id);
    AttachmentFetch *fetch;

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        append_attachment(app, into, path, from_user);
        return;
    }

    fetch = g_new0(AttachmentFetch, 1);
    fetch->into = g_object_ref(into);
    fetch->from_user = from_user;
    fetch->id = g_strdup(id);

    clawt_lrg_request(app, "attachment.get",
                      clawt_ipc_payload_new("id", id, NULL), CLAWT_LRG_QUIET,
                      on_agent_attachment, fetch, attachment_fetch_free);
}

/*
 * A thumbnail under any message that carried a picture, so scrolling
 * back still shows what was sent rather than a path nobody remembers.
 */
static void
attachment_previews_append(ClawtLrgApp *app, LrgWidget *row,
                           const gchar *body, gboolean from_user)
{
    g_autoptr(GPtrArray) refs = clawt_lrg_attachment_refs(body);
    guint i;

    for (i = 0; i < refs->len; i++) {
        ClawtLrgAttachmentRef *ref = g_ptr_array_index(refs, i);

        if (ref->daemon_copy)
            fetch_agent_attachment(app, row, ref->value, from_user);
        else
            append_attachment(app, row, ref->value, from_user);
    }
}

/* ── The transcript ───────────────────────────────────────────────── */

static gint
run_spacing(ClawtLrgApp *app)
{
    gint set = clawt_appearance_get_run_spacing(app->appearance);

    return set > 0 ? set : CHAT_RUN_SPACING;
}

/*
 * A new message inside a run: derived from the run gap so a reader who
 * changes that keeps the order, and never at or below a paragraph.
 */
static gint
message_spacing(ClawtLrgApp *app)
{
    gint run = run_spacing(app);
    gint gap = run - (CHAT_RUN_SPACING - CHAT_MESSAGE_SPACING);

    if (gap <= CHAT_PARAGRAPH_SPACING)
        gap = CHAT_PARAGRAPH_SPACING + 1;

    return MIN(gap, run);
}

/* "Today", "Yesterday" or "Wednesday 25 August", between rules. */
static LrgWidget *
day_divider_new(ClawtLrgApp *app, GDateTime *when)
{
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    g_autofree gchar *text = clawt_chat_day_label(when, NULL);
    LrgInsets margin = { px(app, CHAT_ROW_MARGIN), px(app, 24.0f),
                         px(app, CHAT_ROW_MARGIN), 0.0f };

    clawt_lrg_take(row, rule_new());
    clawt_lrg_take(row, clawt_lrg_label(text, "caption"));
    clawt_lrg_take(row, rule_new());
    lrg_widget_set_margin(row, &margin);
    return row;
}

static void
on_task_chip(LrgButton *button, gpointer user_data)
{
    /* the task board is where the rest of the story is */
    clawt_lrg_show_page(user_data, CLAWT_PAGE_TASKS);
}

/*
 * The task and the hop count belong to a message, not a run: a delegated
 * reply in your own chat is exactly the one you want to know was
 * delegated, and a hop count climbing is what tells a loop from a talk.
 */
static void
append_message_chips(ClawtLrgApp *app, LrgWidget *into, const gchar *task,
                     gint64 depth)
{
    g_autofree gchar *chip = clawt_lrg_task_chip_text(task);
    g_autofree gchar *hops = clawt_lrg_hop_text(depth);

    if (chip != NULL) {
        LrgWidget *button = clawt_lrg_button(chip, "flat",
                                             G_CALLBACK(on_task_chip), app);

        lrg_widget_add_class(button, "caption");
        lrg_widget_set_valign(button, LRG_ALIGN_CENTER);
        clawt_lrg_take(into, button);
    }

    if (hops != NULL)
        clawt_lrg_take(into, clawt_lrg_badge(hops, "warn"));
}

/*
 * The face at the head of a run, from the sender's own listing -- in a
 * room the senders are several -- and a button only when there is a
 * picture to enlarge: initials behind a button offer to enlarge a letter.
 */
static LrgWidget *
sender_avatar(ClawtLrgApp *app, const gchar *sender, const gchar *name)
{
    JsonObject *agent = clawt_lrg_find_agent(app, sender);
    gboolean has_avatar = clawt_ipc_payload_boolean(agent, "has_avatar",
                                                    FALSE);
    LrgWidget *face = clawt_lrg_avatar(
        app, name, sender, has_avatar,
        clawt_ipc_payload_string(agent, "color"), px(app, CHAT_AVATAR),
        clawt_lrg_typing_busy(app->typing, sender, FALSE));

    if (!has_avatar)
        return face;

    face = clickable_new(app, face, "clawt-tab", avatar_preview_open,
                         g_strdup(sender), g_free);
    lrg_widget_set_valign(face, LRG_ALIGN_CENTER);
    return face;
}

/* The operator's turn: a bubble in the accent, on the right. */
static LrgWidget *
own_bubble_new(ClawtLrgApp *app, const gchar *body)
{
    LrgPanel *bubble = lrg_panel_new();
    LrgWidget *text = clawt_lrg_markdown(app, body);

    /*
     * Only the operator's turns are bubbles: an agent's runs to dozens of
     * lines with headings and code, and a container that long reads as a
     * panel.  The asymmetry is what says who is speaking at a glance.
     */
    lrg_panel_set_fill_children(bubble, TRUE);
    lrg_panel_set_elevation(bubble, 0);
    lrg_container_set_padding(LRG_CONTAINER(bubble), px(app, 12.0f));
    lrg_widget_add_class(LRG_WIDGET(bubble), "clawt-bubble-mine");
    lrg_widget_set_halign(LRG_WIDGET(bubble), LRG_ALIGN_END);

    if (app->theme != NULL)
        lrg_markdown_label_set_text_color(
            LRG_MARKDOWN_LABEL(text),
            lrg_theme_get_color(app->theme, LRG_THEME_COLOR_TEXT_ON_ACCENT));

    clawt_lrg_take(LRG_WIDGET(bubble), text);
    return LRG_WIDGET(bubble);
}

/*
 * One message, drawn into the transcript.  @ts is the message's own
 * time in seconds, or 0 for one that arrived without one -- which gets
 * no stamp rather than the moment it happened to be drawn.
 */
static void
append_message(ClawtLrgApp *app, const gchar *sender, const gchar *body,
               gint64 ts, const gchar *task, gint64 depth, gboolean animate)
{
    gboolean from_user = g_strcmp0(sender, "user") == 0;
    g_autoptr(GDateTime) when = (ts > 0) ? g_date_time_new_from_unix_local(ts)
                                         : g_date_time_new_now_local();
    g_autofree gchar *stamp = (ts > 0) ? clawt_chat_time_label(when) : NULL;
    JsonObject *agent = clawt_lrg_find_agent(app, sender);
    const gchar *name = clawt_ipc_payload_string(agent, "name");
    LrgWidget *row = clawt_lrg_vbox(px(app, 4.0f));
    LrgInsets margin = { px(app, CHAT_ROW_MARGIN), 0.0f,
                         px(app, CHAT_ROW_MARGIN), 0.0f };
    gboolean new_day = FALSE;
    gboolean run_start;

    if (name == NULL)
        name = sender;

    run_start = clawt_lrg_run_advance(&chat.run, sender, when, &new_day);

    if (new_day)
        clawt_lrg_take(chat.transcript, day_divider_new(app, when));

    if (from_user) {
        if (run_start) {
            /*
             * The time once, above the first bubble; no name and no face,
             * since it is always the same person and the side says so.
             */
            LrgWidget *line = clawt_lrg_hbox(6.0f);

            lrg_widget_set_halign(line, LRG_ALIGN_END);
            append_message_chips(app, line, task, depth);

            if (stamp != NULL)
                clawt_lrg_take(line, clawt_lrg_label(stamp, "caption"));

            clawt_lrg_take(row, line);
        }

        {
            LrgWidget *bubble = own_bubble_new(app, body);

            /* about seven tenths of the column: wide enough to read as a
             * bubble, narrow enough to be one */
            if (chat.measure > 0)
                lrg_widget_set_max_size(bubble, (gfloat)chat.measure * 0.72f,
                                        -1.0f);

            if (animate)
                lrg_widget_set_appear(bubble, LRG_APPEAR_POP, 0.0f);

            clawt_lrg_take(row, bubble);
        }
    } else {
        LrgWidget *line = clawt_lrg_hbox(0.0f);
        LrgWidget *gutter = clawt_lrg_hbox(0.0f);
        LrgPanel *card = lrg_panel_new();
        LrgWidget *text = clawt_lrg_markdown(app, body);

        if (run_start) {
            LrgWidget *header = clawt_lrg_hbox(px(app, 10.0f));
            LrgWidget *who = clawt_lrg_label(name, "heading");

            lrg_widget_set_valign(who, LRG_ALIGN_CENTER);
            clawt_lrg_take(header, sender_avatar(app, sender, name));
            clawt_lrg_take(header, who);

            if (stamp != NULL) {
                LrgWidget *at = clawt_lrg_label(stamp, "caption");

                lrg_widget_set_valign(at, LRG_ALIGN_CENTER);
                clawt_lrg_take(header, at);
            }

            append_message_chips(app, header, task, depth);
            clawt_lrg_take(row, header);
        }

        /*
         * A real slot the width of the avatar and its gap, so every body
         * in the run starts on one line.  A continuation puts its time
         * there: the half of a message boundary that space cannot carry
         * -- six pixels more than a paragraph is perceptible and not
         * nameable; a time is unambiguous.
         */
        lrg_widget_set_min_size(gutter, px(app, CHAT_GUTTER), 0.0f);
        lrg_widget_set_max_size(gutter, px(app, CHAT_GUTTER), -1.0f);

        if (!run_start && stamp != NULL) {
            LrgWidget *at = clawt_lrg_label(stamp, "caption");

            lrg_label_set_alignment(LRG_LABEL(at), LRG_TEXT_ALIGN_RIGHT);
            lrg_widget_set_fill_weight(at, 1.0f);
            lrg_widget_set_valign(at, LRG_ALIGN_START);
            lrg_widget_add_class(at, "muted");
            {
                LrgInsets pad = { 0.0f, px(app, 12.0f),
                                  px(app, CHAT_GUTTER - CHAT_AVATAR), 0.0f };

                lrg_widget_set_margin(at, &pad);
            }
            clawt_lrg_take(gutter, at);
        }

        /*
         * The body on a soft surface card, filling the column: a wrapping
         * body at its natural width is a ribbon with an empty band beside
         * it, which is what GTK's FILL alignment was fixing.
         */
        lrg_panel_set_fill_children(card, TRUE);
        lrg_panel_set_elevation(card, 0);
        lrg_container_set_padding(LRG_CONTAINER(card), px(app, 12.0f));
        lrg_widget_add_class(LRG_WIDGET(card), "clawt-bubble");
        lrg_widget_set_fill_weight(LRG_WIDGET(card), 1.0f);
        clawt_lrg_take(LRG_WIDGET(card), text);

        if (animate)
            lrg_widget_set_appear(LRG_WIDGET(card), LRG_APPEAR_FLY_UP, 0.0f);

        clawt_lrg_take(line, gutter);
        clawt_lrg_take(line, LRG_WIDGET(card));
        clawt_lrg_take(row, line);
    }

    attachment_previews_append(app, row, body, from_user);
    attach_message_menu(row, body);

    /*
     * Turns further apart than the paragraphs inside one, and a new run
     * further still; after a day divider, close, so the date belongs to
     * the block beneath it.
     */
    margin.top = px(app, !run_start ? (gfloat)message_spacing(app)
                                    : (new_day ? 6.0f
                                               : (gfloat)run_spacing(app)));
    lrg_widget_set_margin(row, &margin);
    clawt_lrg_take(chat.transcript, row);
}

/* A reply that came from the client, not from the agent. */
static void
append_local(ClawtLrgApp *app, const gchar *text)
{
    if (chat.transcript == NULL)
        return;

    /*
     * Following again: the operator just asked to see this, and output
     * appended below the fold looks like a command that did nothing.
     */
    chat.following = TRUE;
    chat.auto_scrolling = FALSE;
    append_message(app, "clawtilla", text, 0, NULL, 0, TRUE);
    clawt_lrg_app_wake(app);
}

/* ── Turn steps ───────────────────────────────────────────────────── */

static LrgWidget *
step_label(const gchar *text, const gchar *tone, gboolean monospace)
{
    LrgWidget *label = clawt_lrg_paragraph(text, monospace ? "mono"
                                                           : "caption");

    /* only a failure is coloured, or colour stops meaning anything */
    if (g_strcmp0(tone, "bad") == 0)
        lrg_widget_add_class(label, clawt_lrg_tone_class("bad"));
    else
        lrg_widget_add_class(label, "muted");

    return label;
}

/*
 * One run of tool calls, collapsed to its summary and expandable to the
 * command lines in it: the prose between the calls is what somebody is
 * reading, and the commands are what they want when the answer looks
 * wrong -- selectable there, not hidden in a tooltip.
 */
static LrgWidget *
step_run_new(ClawtLrgApp *app, GPtrArray *steps, guint from, guint to,
             guint failed)
{
    g_autofree gchar *summary = clawt_turn_step_run_summary(steps, from, to);
    LrgExpander *expander = lrg_expander_new(summary);
    LrgWidget *inner = clawt_lrg_vbox(2.0f);
    guint i;

    lrg_container_set_padding(LRG_CONTAINER(inner), px(app, 8.0f));
    lrg_widget_add_class(inner, "clawt-terminal");

    for (i = from; i < to; i++) {
        ClawtTurnStep *step = g_ptr_array_index(steps, i);
        g_autofree gchar *line = NULL;

        /* calls only: an outcome marker with no name reads as a bug */
        if (!clawt_turn_step_is_call(step))
            continue;

        line = clawt_turn_step_summary(step);
        clawt_lrg_take(inner, step_label(line, clawt_turn_step_tone(step),
                                         TRUE));
    }

    lrg_expander_set_child(expander, inner);
    g_object_unref(inner);

    if (failed > 0)
        lrg_widget_add_class(lrg_expander_get_header(expander),
                             clawt_lrg_tone_class("bad"));

    return LRG_WIDGET(expander);
}

/*
 * Steps into a box: runs collapsed, prose as written.  One renderer for
 * the live block and for history, because two renderers of one kind of
 * content drift.
 */
static LrgWidget *
steps_box_new(ClawtLrgApp *app, GPtrArray *steps, guint from, guint end)
{
    LrgWidget *box = clawt_lrg_vbox(2.0f);
    LrgInsets margin = { px(app, CHAT_BODY_INSET), px(app, 6.0f),
                         px(app, CHAT_ROW_MARGIN), px(app, 6.0f) };
    guint i;

    lrg_widget_set_margin(box, &margin);

    for (i = from; i < end; ) {
        ClawtTurnStep *step = g_ptr_array_index(steps, i);

        if (clawt_turn_step_joins_run(step)) {
            guint run_start = i;
            guint failed = 0;

            i = clawt_turn_step_run_extent(steps, i, end, NULL, &failed);
            clawt_lrg_take(box, step_run_new(app, steps, run_start, i,
                                             failed));
            continue;
        }

        {
            g_autofree gchar *line = clawt_lrg_step_line(step);

            clawt_lrg_take(box, step_label(line, clawt_turn_step_tone(step),
                                           FALSE));
        }

        i++;
    }

    return box;
}

static void
steps_append_sealed(ClawtLrgApp *app, GPtrArray *steps, guint from,
                    guint end)
{
    if (steps == NULL || end <= from || chat.transcript == NULL)
        return;

    clawt_lrg_take(chat.transcript, steps_box_new(app, steps, from, end));
}

/*
 * The live block, redrawn whole: a run's line changes as it grows ("Ran
 * 5 commands" becoming 6), which is an edit, and redrawing a bounded list
 * is less state than tracking which row that was.  Always last, so a
 * message that arrived mid-turn sits above the work still going on.
 */
static void
steps_rebuild(ClawtLrgApp *app)
{
    if (chat.transcript == NULL)
        return;

    if (chat.live_steps != NULL) {
        lrg_container_remove_child(LRG_CONTAINER(chat.transcript),
                                   chat.live_steps);
        chat.live_steps = NULL;
    }

    if (chat.steps == NULL || chat.steps->len == 0)
        return;

    chat.live_steps = steps_box_new(app, chat.steps, 0, chat.steps->len);
    lrg_widget_add_class(chat.live_steps, "clawt-busy");
    clawt_lrg_take(chat.transcript, chat.live_steps);
}

static void
steps_add_live(ClawtLrgApp *app, ClawtTurnStep *step)
{
    if (chat.steps == NULL)
        chat.steps = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_turn_step_free);

    while (chat.steps->len >= CHAT_MAX_STEPS)
        g_ptr_array_remove_index(chat.steps, 0);

    g_ptr_array_add(chat.steps, clawt_turn_step_copy(step));
    steps_rebuild(app);
}

/*
 * The turn ended: its steps stay where they are, no longer ours.  They
 * used to vanish when the answer arrived, which read as text being
 * deleted from a conversation somebody was reading -- because it was.
 */
static void
steps_seal(void)
{
    if (chat.live_steps != NULL)
        lrg_widget_remove_class(chat.live_steps, "clawt-busy");

    chat.live_steps = NULL;

    if (chat.steps != NULL)
        g_ptr_array_set_size(chat.steps, 0);
}

/* ── Following the live edge ──────────────────────────────────────── */

/*
 * The one place `following` changes to TRUE, and so the one place what
 * the chat draws about not following is cleared: the rule in the text
 * and the pill over it go together, on every path that comes back.
 */
static void
set_following(gboolean following)
{
    chat.following = following;

    if (!following)
        return;

    if (chat.unread_rule != NULL) {
        lrg_container_remove_child(LRG_CONTAINER(chat.transcript),
                                   chat.unread_rule);
        chat.unread_rule = NULL;
    }

    if (chat.jump != NULL)
        lrg_widget_set_visible(chat.jump, FALSE);
}

/*
 *   ------------------  New messages  ------------------
 *
 * Stores nothing: it is exactly as durable as `following`, which is what
 * lets it promise "these arrived while you were up there" rather than a
 * read position it could not keep across a restart.
 */
static LrgWidget *
unread_rule_new(ClawtLrgApp *app)
{
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgInsets margin = { px(app, CHAT_ROW_MARGIN), px(app, 18.0f),
                         px(app, CHAT_ROW_MARGIN), 0.0f };

    clawt_lrg_take(row, rule_new());
    clawt_lrg_take(row, clawt_lrg_badge("New messages", "info"));
    clawt_lrg_take(row, rule_new());
    lrg_widget_set_margin(row, &margin);
    lrg_widget_set_appear(row, LRG_APPEAR_FADE, 0.0f);
    return row;
}

/*
 * Something arrived that the reader is not looking at.  Only from the
 * event path, so a history replay or the client's own output can never
 * trip it; at most one rule, because the second arrival belongs under
 * the first's.
 */
static void
note_arrival(ClawtLrgApp *app)
{
    if (chat.following || chat.unread_rule != NULL || chat.transcript == NULL)
        return;

    chat.unread_rule = unread_rule_new(app);
    clawt_lrg_take(chat.transcript, chat.unread_rule);

    if (chat.jump != NULL)
        lrg_widget_set_visible(chat.jump, TRUE);
}

static void
scroll_to_end(gboolean animate)
{
    gfloat max_y = 0.0f;

    lrg_scroll_box_get_max_scroll(LRG_SCROLL_BOX(chat.scroll), NULL, &max_y);
    chat.auto_scrolling = animate;
    lrg_scroll_box_scroll_to(LRG_SCROLL_BOX(chat.scroll), 0.0f, max_y,
                             animate);
    chat.last_scroll = lrg_scroll_box_get_scroll_y(LRG_SCROLL_BOX(chat.scroll));
}

static void
on_jump_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    set_following(TRUE);
    scroll_to_end(TRUE);
    clawt_lrg_app_wake(app);
}

/*
 * The column's width: the appearance's measure, resolved against what
 * the transcript has to sit in and the width of a zero in its font --
 * `ch`, which is what the web client renders a character count with.
 */
static void
update_measure(ClawtLrgApp *app)
{
    gfloat probe_w = 0.0f;
    gfloat available = lrg_widget_get_width(chat.scroll);
    gint measure;

    lrg_widget_measure(chat.char_probe, &probe_w, NULL);

    measure = clawt_measure_resolve_px(
        clawt_appearance_get_measure_unit(app->appearance),
        clawt_appearance_get_measure_amount(app->appearance),
        (gint)available, probe_w / 10.0, (gint)px(app, CHAT_BODY_INSET));

    if (measure == chat.measure && available == chat.measured_in)
        return;

    chat.measured_in = available;

    /*
     * Floor and ceiling both: a centred column would otherwise take its
     * content's natural width and change size with every message.  The
     * floor stops at what there is, since the resolver deliberately has
     * no upper bound.
     */
    chat.measure = measure;
    lrg_widget_set_min_size(chat.column, MIN((gfloat)measure, available),
                            0.0f);
    lrg_widget_set_max_size(chat.column, (gfloat)measure, -1.0f);
    lrg_widget_set_min_size(chat.composer, MIN((gfloat)measure, available),
                            0.0f);
    lrg_widget_set_max_size(chat.composer, (gfloat)measure, -1.0f);
    lrg_widget_queue_layout(chat.page);
}

/*
 * Ticks with the canvas: the follower GTK hung off its adjustment.
 *
 * Content grew with the view held still -- a message, a step, the window
 * resized and every body re-wrapped -- and the reader was at the bottom:
 * follow it down.  The view moved upward while nothing of ours was
 * moving it: the reader scrolled away, and is left there.  Back at the
 * bottom by any route: following again, which clears the rule and pill.
 */
static gboolean
on_follow_step(LrgCustomWidget *widget, gfloat dt, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgScrollBox *scroll = LRG_SCROLL_BOX(chat.scroll);
    gfloat max_y = 0.0f;
    gfloat page_h;
    gfloat y;
    gboolean at_bottom;

    update_measure(app);

    lrg_scroll_box_get_max_scroll(scroll, NULL, &max_y);
    page_h = lrg_widget_get_height(chat.scroll);
    y = lrg_scroll_box_get_scroll_y(scroll);
    at_bottom = clawt_transcript_is_at_bottom(y, max_y + page_h, page_h);

    if (chat.auto_scrolling) {
        if (y < chat.last_scroll - 0.5f) {
            /* our scroll is always downward; upward is a person */
            chat.auto_scrolling = FALSE;
            chat.following = FALSE;
        } else if (max_y - y < 1.0f) {
            chat.auto_scrolling = FALSE;
        }
    } else if (at_bottom) {
        if (!chat.following)
            set_following(TRUE);
    } else if (y != chat.last_scroll) {
        chat.following = FALSE;
    } else if (chat.following) {
        scroll_to_end(!chat.jump_instantly);
        chat.jump_instantly = FALSE;
        chat.last_scroll = y;
        return TRUE;
    }

    chat.last_scroll = y;
    return chat.auto_scrolling;
}

/* ── Unread and typing, for the whole window ──────────────────────── */

/*
 * The sidebar redraws its rows and the tab's number from app->unread and
 * app->typing; this is where those change.
 */
static void
fleet_marks_changed(ClawtLrgApp *app)
{
    clawt_lrg_sidebar_refresh(app);
}

static void
set_activity(const gchar *text)
{
    if (chat.activity == NULL)
        return;

    lrg_widget_set_visible(chat.activity, text != NULL);

    if (text != NULL)
        lrg_label_set_text(LRG_LABEL(chat.activity_label), text);
}

/*
 * Stop is shown only while there is a turn to stop, and only for an
 * agent that declares `interrupt`: an embedded agent takes its turn
 * inside the daemon and must not be offered a button that would refuse.
 */
static void
sync_stop(gboolean busy)
{
    if (chat.stop_button != NULL)
        lrg_widget_set_visible(chat.stop_button, busy && chat.can_interrupt);
}

/* The selected agent's activity, from the typing set and the listing. */
static void
sync_selected_activity(ClawtLrgApp *app)
{
    JsonObject *agent = clawt_lrg_selected_agent(app);
    gboolean busy;
    g_autofree gchar *line = NULL;

    if (agent == NULL) {
        sync_stop(FALSE);
        set_activity(NULL);
        return;
    }

    chat.can_interrupt = strstr(clawt_ipc_payload_string(agent, "caps") != NULL
                                ? clawt_ipc_payload_string(agent, "caps")
                                : "", "interrupt") != NULL;

    /*
     * Asked of the listing as well as the frames: selecting an agent
     * already mid-turn, or connecting to a daemon that has been working,
     * produces no frame at all.
     */
    busy = g_strcmp0(clawt_ipc_payload_string(agent, "state"),
                     "running") == 0 &&
           clawt_lrg_typing_busy(app->typing, app->agent_id,
                                 clawt_ipc_payload_boolean(agent, "busy",
                                                           FALSE));

    if (busy)
        line = clawt_lrg_typing_activity(
            TRUE, (chat.room != NULL &&
                   clawt_lrg_typing_in_room(app->typing, app->agent_id,
                                            chat.room))
                  ? NULL : clawt_ipc_payload_string(agent, "peer"));

    set_activity(line);
    sync_stop(busy);
}

/* ── History ──────────────────────────────────────────────────────── */

typedef struct {
    guint      generation;
    JsonNode  *messages;      /* the history reply, held for the steps */
} HistoryLoad;

static void
history_load_free(gpointer data)
{
    HistoryLoad *load = data;

    /*
     * Counted down here, in the destroy, because a guarded reply that is
     * dropped never reaches its callback -- and an in-flight count that
     * only went down on success would wedge the stuck-view retry for good.
     */
    if (chat.history_inflight > 0)
        chat.history_inflight--;

    g_clear_pointer(&load->messages, json_node_unref);
    g_free(load);
}

/*
 * Emptying the transcript: the rule and the live block are borrowed
 * pointers into it and go first, or the next edge removes a widget that
 * is already gone.  Following again, so a fresh history draws no rule.
 */
static void
reset_transcript(void)
{
    chat.unread_rule = NULL;
    chat.live_steps = NULL;

    if (chat.steps != NULL)
        g_ptr_array_set_size(chat.steps, 0);

    clawt_lrg_run_reset(&chat.run);

    if (chat.transcript != NULL)
        clawt_lrg_clear(chat.transcript);

    set_following(TRUE);
}

static void
draw_history(ClawtLrgApp *app, JsonNode *history, GPtrArray *steps)
{
    JsonObject *root = clawt_ipc_reply_object(history);
    JsonArray *messages = (root != NULL &&
                           json_object_has_member(root, "messages"))
        ? json_object_get_array_member(root, "messages") : NULL;
    guint n = (messages != NULL) ? json_array_get_length(messages) : 0;
    guint s_at = 0;
    guint i;

    reset_transcript();
    g_hash_table_remove_all(chat.shown);

    if (n == 0) {
        g_autofree gchar *body = g_strdup_printf(
            "Nothing has been said here yet. Whatever you write to %s "
            "starts it.", selected_label(app) != NULL ? selected_label(app)
                                                       : "this room");

        clawt_lrg_take(chat.transcript,
                       clawt_lrg_empty_state("A fresh conversation", body));
    }

    /*
     * Messages and steps merged on time, so each stretch of tool calls
     * sits between the message that prompted it and the answer it made;
     * every step drawn here is sealed, being history by definition.
     */
    for (i = 0; i < n; i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_ipc_payload_string(message, "sender");
        const gchar *id = clawt_ipc_payload_string(message, "id");
        gint64 ts = clawt_ipc_payload_int(message, "ts", 0);
        guint run = clawt_lrg_steps_before(steps, s_at, ts);

        steps_append_sealed(app, steps, s_at, run);
        s_at = run;

        if (id != NULL)
            g_hash_table_add(chat.shown, g_strdup(id));

        append_message(app, sender != NULL ? sender : "?",
                       clawt_ipc_payload_string(message, "body") != NULL
                           ? clawt_ipc_payload_string(message, "body") : "",
                       ts, clawt_ipc_payload_string(message, "task"),
                       clawt_ipc_payload_int(message, "depth", 0),
                       i + CHAT_HISTORY_ENTRANCES >= n);
    }

    if (steps != NULL)
        steps_append_sealed(app, steps, s_at, steps->len);

    /*
     * Everything just drawn is read.  A message that arrived during the
     * wait was counted against the old room; it is on screen now.
     */
    if (clawt_lrg_unread_clear(app->unread, chat.room))
        fleet_marks_changed(app);

    set_following(TRUE);
    chat.jump_instantly = TRUE;
    sync_selected_activity(app);
    clawt_lrg_app_wake(app);
}

static void
on_steps(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    HistoryLoad *load = user_data;
    g_autoptr(GPtrArray) steps = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_turn_step_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    guint i;

    if (load->generation != chat.history_generation)
        return;

    if (root != NULL && json_object_has_member(root, "steps")) {
        JsonArray *array = json_object_get_array_member(root, "steps");

        for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
            JsonObject *entry = json_array_get_object_element(array, i);
            ClawtTurnStep *step;

            if (entry == NULL)
                continue;

            step = clawt_turn_step_new_from_object(
                entry, clawt_ipc_payload_string(entry, "agent"));

            if (step != NULL)
                g_ptr_array_add(steps, step);
        }
    }

    /* a failed step listing still draws the conversation, without them */
    draw_history(app, load->messages, steps);
}

static void
on_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    HistoryLoad *load = user_data;
    HistoryLoad *next;

    /* a newer load owns the view; this one must not touch it */
    if (load->generation != chat.history_generation)
        return;

    g_clear_pointer(&chat.room, g_free);

    /*
     * A failed load leaves the pane blank and the room unset rather than
     * the old conversation under a new heading; unset is recoverable --
     * the next message for it, or a click on it again, loads it.
     */
    if (reply == NULL) {
        reset_transcript();
        return;
    }

    /* the room the daemon resolved is what later messages are matched on */
    chat.room = g_strdup(clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "room"));

    next = g_new0(HistoryLoad, 1);
    next->generation = load->generation;
    next->messages = json_node_ref(reply);
    chat.history_inflight++;

    clawt_lrg_request(app, "room.steps",
                      clawt_ipc_payload_new("room", chat.room, NULL),
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_steps, next,
                      history_load_free);
}

static void
load_history(ClawtLrgApp *app)
{
    JsonNode *payload;
    HistoryLoad *load;

    payload = clawt_lrg_history_payload(room_entry(app), chat.conversation,
                                        app->agent_id, NULL);

    /*
     * No conversation at all -- nothing selected, or the agent removed:
     * a blank pane is the honest rendering, and there is nothing to wait
     * for.
     */
    if (payload == NULL) {
        chat.history_generation++;
        g_clear_pointer(&chat.room, g_free);
        reset_transcript();
        g_hash_table_remove_all(chat.shown);

        if (chat.transcript != NULL)
            clawt_lrg_take(chat.transcript, clawt_lrg_empty_state(
                "Pick somebody to talk to",
                "Choose an agent or a room from the sidebar. Its "
                "conversation opens here."));

        set_activity(NULL);
        sync_stop(FALSE);
        return;
    }

    load = g_new0(HistoryLoad, 1);
    load->generation = ++chat.history_generation;
    chat.history_inflight++;

    clawt_lrg_request(app, "room.history", payload, CLAWT_LRG_GUARDED,
                      on_history, load, history_load_free);
}

/* ── The conversation picker ──────────────────────────────────────── */

static void
fill_conversation_bar(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) peers = NULL;
    g_autofree gchar *label = NULL;
    GAction *action;
    gboolean own;

    if (chat.conversation_bar == NULL)
        return;

    /*
     * A peer conversation is read-only: typing there would post as the
     * operator into a room the operator is not in, and what the composer
     * sends is a message to the agent, landing in the main chat while
     * somebody looked at another.  Disabled, never hidden -- a composer
     * that vanishes reads as a lost connection.  A group is typed into.
     */
    own = chat.conversation == NULL || room_entry(app) != NULL;
    lrg_widget_set_enabled(chat.editor, own);
    lrg_text_editor_set_read_only(LRG_TEXT_EDITOR(chat.editor), !own);
    lrg_text_editor_set_placeholder(
        LRG_TEXT_EDITOR(chat.editor),
        own ? "Message  \342\200\224  / for commands, Ctrl+G to write it in "
              "$EDITOR"
            : "A conversation between two agents: read-only here");

    if (app->agent_id == NULL) {
        lrg_widget_set_visible(chat.conversation_bar, FALSE);
        return;
    }

    /*
     * Filled from the room listing per agent, because a direct room is
     * created the first time two agents speak; one that has never
     * delegated has one conversation and no switcher at all.
     */
    peers = clawt_lrg_conversation_peers(app->rooms, app->agent_id);
    lrg_widget_set_visible(chat.conversation_bar,
                           peers->len > 0 || chat.conversation != NULL);

    label = clawt_lrg_conversation_label(chat.conversation);
    lrg_button_set_text(LRG_BUTTON(chat.conversation_button), label);

    action = g_action_map_lookup_action(G_ACTION_MAP(chat.actions),
                                        "conversation");

    if (action != NULL)
        g_simple_action_set_state(
            G_SIMPLE_ACTION(action),
            g_variant_new_string(chat.conversation != NULL
                                 ? chat.conversation : ""));
}

static void
on_conversation_chosen(GSimpleAction *action, GVariant *parameter,
                       gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *peer = g_variant_get_string(parameter, NULL);

    g_simple_action_set_state(action, g_variant_new_string(peer));
    g_clear_pointer(&chat.conversation, g_free);

    if (*peer != '\0')
        chat.conversation = g_strdup(peer);

    load_history(app);
    fill_conversation_bar(app);
}

static void
conversation_picker(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GPtrArray) peers = clawt_lrg_conversation_peers(app->rooms,
                                                              app->agent_id);
    guint i;

    /*
     * The operator's own first and always: it is the one the client
     * opens on, and an agent with peers still needs the way back.
     */
    {
        g_autoptr(GMenuItem) item = g_menu_item_new("Chat", NULL);

        g_menu_item_set_action_and_target_value(item, "chat.conversation",
                                                g_variant_new_string(""));
        g_menu_append_item(menu, item);
    }

    for (i = 0; i < peers->len; i++) {
        g_autofree gchar *label = clawt_lrg_conversation_label(
            g_ptr_array_index(peers, i));
        g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);

        g_menu_item_set_action_and_target_value(
            item, "chat.conversation",
            g_variant_new_string(g_ptr_array_index(peers, i)));
        g_menu_append_item(menu, item);
    }

    clawt_lrg_popup_menu(app, LRG_WIDGET(button), G_MENU_MODEL(menu));
}

/* ── Drafts ───────────────────────────────────────────────────────── */

/*
 * The composer's contents in the client's own store, so a half-typed
 * message survives the client closing; keyed with the connection's name,
 * because two fleets can each hold an agent called `chief`.  Errors are
 * dropped: a draft is a convenience, and a dialog about a file nobody
 * has heard of while clicking between agents is worse than losing it.
 */
static void
chat_persist_draft(ClawtLrgApp *app, const gchar *id, const gchar *text)
{
    g_autofree gchar *key = NULL;

    if (id == NULL)
        return;

    key = clawt_draft_key(clawt_connection_get_name(app->connection), id);
    clawt_draft_store_set(NULL, key, text, NULL);

    if (text != NULL && *text != '\0')
        g_hash_table_insert(chat.drafts, g_strdup(id), g_strdup(text));
    else
        g_hash_table_remove(chat.drafts, id);
}

/* Memory first -- within a run it is authoritative -- then the file. */
static gchar *
stored_draft(ClawtLrgApp *app, const gchar *id)
{
    const gchar *held;
    g_autofree gchar *key = NULL;

    if (id == NULL)
        return NULL;

    held = g_hash_table_lookup(chat.drafts, id);

    if (held != NULL)
        return g_strdup(held);

    key = clawt_draft_key(clawt_connection_get_name(app->connection), id);
    return clawt_draft_store_get(NULL, key);
}

/* ── The composer ─────────────────────────────────────────────────── */

static gchar *
editor_text(void)
{
    return clawt_lrg_editor_text(chat.editor);
}

static void
editor_focus_end(ClawtLrgApp *app)
{
    LrgTextEditor *editor = LRG_TEXT_EDITOR(chat.editor);
    guint lines = lrg_text_editor_get_line_count(editor);
    const gchar *last = (lines > 0)
        ? lrg_text_editor_get_line(editor, lines - 1) : NULL;

    lrg_text_editor_set_cursor(editor, lines > 0 ? lines - 1 : 0,
                               last != NULL ? (guint)g_utf8_strlen(last, -1)
                                            : 0);
    lrg_canvas_set_focused_widget(app->canvas, chat.editor);
}

static void
editor_set_text(ClawtLrgApp *app, const gchar *text)
{
    lrg_text_editor_set_text(LRG_TEXT_EDITOR(chat.editor),
                             text != NULL ? text : "");
    editor_focus_end(app);
}

/*
 * The text up to the caret, not the whole buffer: somebody going back to
 * correct an earlier `@` is typing there, and a completion taken from the
 * end would offer names for a mention they are not writing.
 */
static gchar *
editor_text_to_cursor(void)
{
    LrgTextEditor *editor = LRG_TEXT_EDITOR(chat.editor);
    GString *out = g_string_new(NULL);
    guint line = 0;
    guint column = 0;
    guint i;

    lrg_text_editor_get_cursor(editor, &line, &column);

    for (i = 0; i < line; i++) {
        g_string_append(out, lrg_text_editor_get_line(editor, i));
        g_string_append_c(out, '\n');
    }

    {
        const gchar *current = lrg_text_editor_get_line(editor, line);

        if (current != NULL) {
            const gchar *end = g_utf8_offset_to_pointer(
                current, MIN((glong)column, g_utf8_strlen(current, -1)));

            g_string_append_len(out, current, end - current);
        }
    }

    return g_string_free(out, FALSE);
}

/* ── Pending attachments ──────────────────────────────────────────── */

static void refresh_attachment_strip(ClawtLrgApp *app);

static void
on_drop_pending(LrgButton *button, gpointer user_data)
{
    PendingFile *file = g_object_get_data(G_OBJECT(button), "clawt-pending");

    g_ptr_array_remove(chat.pending, file);
    refresh_attachment_strip(user_data);
}

/* A row of chips above the composer, one per queued file. */
static void
refresh_attachment_strip(ClawtLrgApp *app)
{
    guint i;

    clawt_lrg_clear(chat.attachments);
    lrg_widget_set_visible(chat.attachments, chat.pending->len > 0);

    for (i = 0; i < chat.pending->len; i++) {
        PendingFile *file = g_ptr_array_index(chat.pending, i);
        LrgWidget *chip = clawt_lrg_hbox(6.0f);
        LrgWidget *card;
        LrgWidget *drop;
        g_autofree gchar *text = g_strdup_printf(
            "%s (%" G_GSIZE_FORMAT " KB)", file->name,
            (g_bytes_get_size(file->bytes) + 1023) / 1024);

        clawt_lrg_take(chip, clawt_lrg_label(text, "caption"));
        drop = clawt_lrg_button("\303\227", "flat",
                                G_CALLBACK(on_drop_pending), app);
        g_object_set_data(G_OBJECT(drop), "clawt-pending", file);
        clawt_lrg_take(chip, drop);

        card = clawt_lrg_card(chip);
        lrg_container_set_padding(LRG_CONTAINER(card), px(app, 6.0f));
        lrg_widget_set_appear(card, LRG_APPEAR_POP, 0.0f);
        clawt_lrg_take(chat.attachments, card);
    }

    clawt_lrg_app_wake(app);
}

static void
queue_attachment(ClawtLrgApp *app, const gchar *name, GBytes *bytes)
{
    PendingFile *file;

    if (bytes == NULL || g_bytes_get_size(bytes) == 0)
        return;

    /*
     * Bounded: this crosses the socket base64-encoded, and a 400 MB video
     * would block the daemon's main context while it decodes.
     */
    if (clawt_lrg_attachment_too_big(g_bytes_get_size(bytes))) {
        clawt_lrg_toast(app, "that file is over 32 MB; put it in a shared "
                             "folder instead");
        return;
    }

    file = g_new0(PendingFile, 1);
    file->name = g_strdup(name);
    file->bytes = g_bytes_ref(bytes);
    g_ptr_array_add(chat.pending, file);
    refresh_attachment_strip(app);
}

static void
on_files_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    files = lrg_file_dialog_open_multiple_finish(LRG_FILE_DIALOG(source),
                                                 result, &error);

    if (files == NULL) {
        if (error != NULL &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(error, lrg_file_dialog_error_quark(),
                             LRG_FILE_DIALOG_ERROR_CANCELLED))
            clawt_lrg_toast(app, error->message);

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
            clawt_lrg_toast(app, read_error->message);
            continue;
        }

        {
            g_autoptr(GBytes) bytes = g_bytes_new(contents, length);

            queue_attachment(app, name, bytes);
        }
    }
}

static void
pick_files(ClawtLrgApp *app)
{
    g_autoptr(LrgFileDialog) dialog = lrg_file_dialog_new();

    lrg_file_dialog_set_title(dialog, "Send with this message");
    lrg_file_dialog_open_multiple(dialog, LRG_WIDGET(app->canvas), NULL,
                                  on_files_chosen, app);
}

static void
on_attach_clicked(LrgButton *button, gpointer user_data)
{
    pick_files(user_data);
}

/* ── Pasting a picture ────────────────────────────────────────────── */

static void
queue_pasted_png(ClawtLrgApp *app, GBytes *png)
{
    g_autofree gchar *name = g_strdup_printf(
        "pasted-%" G_GINT64_FORMAT ".png", g_get_real_time() / G_USEC_PER_SEC);

    /*
     * PNG, because a pasted screenshot is pixels with no file behind it,
     * and every model that reads images reads PNG.
     */
    queue_attachment(app, name, png);
}

static void
on_pasted_png(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GBytes) out = NULL;

    if (!g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out,
                                         NULL, NULL) || out == NULL ||
        g_bytes_get_size(out) == 0) {
        lrg_text_editor_paste_clipboard(LRG_TEXT_EDITOR(chat.editor));
        return;
    }

    queue_pasted_png(app, out);
}

static void
on_clipboard_types(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *types = NULL;
    g_autoptr(GSubprocess) fetch = NULL;

    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                              &types, NULL, NULL) ||
        types == NULL || strstr(types, "image/png") == NULL) {
        /* no picture: the text paste the key was going to do anyway */
        lrg_text_editor_paste_clipboard(LRG_TEXT_EDITOR(chat.editor));
        clawt_lrg_app_wake(app);
        return;
    }

    fetch = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                             G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                             "wl-paste", "--no-newline", "--type", "image/png",
                             NULL);

    if (fetch == NULL) {
        lrg_text_editor_paste_clipboard(LRG_TEXT_EDITOR(chat.editor));
        return;
    }

    g_subprocess_communicate_async(fetch, NULL, NULL, on_pasted_png, app);
}

/*
 * Ctrl+V takes a picture when the clipboard holds one, and only then --
 * an image copied from a browser usually carries a URL too, and getting
 * both is worse than either.  Wayland's clipboard is asked through
 * wl-paste (raylib's GLFW cannot read it there); X11's through raylib.
 *
 * Returns: %TRUE when the key was taken here (the paste follows later)
 */
static gboolean
paste_image(ClawtLrgApp *app)
{
    if (g_getenv("WAYLAND_DISPLAY") != NULL) {
        g_autoptr(GSubprocess) probe = g_subprocess_new(
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
            NULL, "wl-paste", "--list-types", NULL);

        if (probe == NULL)
            return FALSE;

        g_subprocess_communicate_utf8_async(probe, NULL, NULL,
                                            on_clipboard_types, app);
        return TRUE;
    }

    {
        Image image = GetClipboardImage();
        unsigned char *png;
        gint size = 0;

        if (image.data == NULL)
            return FALSE;

        png = ExportImageToMemory(image, ".png", &size);
        UnloadImage(image);

        if (png == NULL || size <= 0)
            return FALSE;

        {
            g_autoptr(GBytes) bytes = g_bytes_new(png, (gsize)size);

            MemFree(png);
            queue_pasted_png(app, bytes);
        }

        return TRUE;
    }
}

/* ── Composing in $EDITOR ─────────────────────────────────────────── */

static void
on_compose_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = chat.app;
    g_autofree gchar *path = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *written = NULL;

    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result,
                                        &error)) {
        clawt_lrg_toast(app, error->message);
        g_unlink(path);
        return;
    }

    /*
     * The trailing newline every editor adds is stripped, or it arrives
     * as an empty line at the end of every message composed this way.
     */
    if (g_file_get_contents(path, &written, NULL, NULL)) {
        g_strchomp(written);
        editor_set_text(app, written);
    }

    g_unlink(path);
    clawt_lrg_app_wake(app);
}

/*
 * Ctrl+G: what is typed goes to $EDITOR and whatever comes out comes
 * back -- seeded, so it extends a draft rather than replacing it.  .md,
 * so an editor that picks a mode by extension picks one for prose.
 */
static void
compose_in_editor(ClawtLrgApp *app)
{
    const gchar *editor = g_getenv("VISUAL");
    g_autofree gchar *path = NULL;
    g_autofree gchar *current = editor_text();
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    gint fd;
    guint i;

    if (editor == NULL || *editor == '\0')
        editor = g_getenv("EDITOR");

    if (editor == NULL || *editor == '\0') {
        clawt_lrg_toast(app, "no editor found; set $EDITOR or $VISUAL");
        return;
    }

    fd = g_file_open_tmp("clawtilla-message-XXXXXX.md", &path, &error);

    if (fd < 0) {
        clawt_lrg_toast(app, error->message);
        return;
    }

    close(fd);

    if (!g_file_set_contents(path, current != NULL ? current : "", -1,
                             &error) ||
        !g_shell_parse_argv(editor, NULL, &parts, &error)) {
        clawt_lrg_toast(app, error->message);
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
        clawt_lrg_toast(app, error->message);
        g_unlink(path);
        return;
    }

    g_subprocess_wait_check_async(process, NULL, on_compose_finished,
                                  g_steal_pointer(&path));
}

/* ── Completion: "/" and "@" ──────────────────────────────────────── */

static gboolean
completion_showing(void)
{
    return chat.completion_card != NULL &&
           lrg_widget_get_visible(chat.completion_card) &&
           chat.rows->len > 0;
}

static void
completion_hide(void)
{
    if (chat.completion_card != NULL)
        lrg_widget_set_visible(chat.completion_card, FALSE);

    chat.highlight = -1;
}

/*
 * The highlight on @index and off everything else -- every row, since a
 * list showing two highlighted names is worse than none: it is the thing
 * Return acts on.
 */
static void
completion_highlight_set(gint index)
{
    guint i;

    chat.highlight = index;

    for (i = 0; i < chat.rows->len; i++) {
        Completion *row = g_ptr_array_index(chat.rows, i);

        if ((gint)i == index)
            lrg_widget_add_class(row->widget, "clawt-row-selected");
        else
            lrg_widget_remove_class(row->widget, "clawt-row-selected");
    }
}

/*
 * Moves the highlight, wrapping; from nothing, Down takes the first and
 * Up the last, which reaches the bottom of a long roster in one key.
 */
static gboolean
completion_move(gint delta)
{
    gint count = (gint)chat.rows->len;
    gint next;

    if (count == 0)
        return FALSE;

    if (chat.highlight < 0)
        next = (delta > 0) ? 0 : count - 1;
    else
        next = ((chat.highlight + delta) % count + count) % count;

    completion_highlight_set(next);
    return TRUE;
}

/*
 * Replaces the half-typed `@name` at the caret with the chosen one: only
 * the prefix, so the `@` and the rest of the line stay as written.  The
 * member is copied first, because the edit refills the list and frees
 * the row it came from.
 */
static void
insert_mention(ClawtLrgApp *app, const gchar *member)
{
    LrgTextEditor *editor = LRG_TEXT_EDITOR(chat.editor);
    g_autofree gchar *filled = g_strconcat(member, " ", NULL);
    g_autofree gchar *to_cursor = editor_text_to_cursor();
    g_autofree gchar *prefix = clawt_mention_prefix_at(to_cursor,
                                                       strlen(to_cursor));
    guint line = 0;
    guint column = 0;
    guint length;

    if (prefix == NULL)
        return;

    length = (guint)g_utf8_strlen(prefix, -1);
    lrg_text_editor_get_cursor(editor, &line, &column);
    lrg_text_editor_select_range(editor, line, column - MIN(column, length),
                                 line, column);
    lrg_text_editor_insert_text(editor, filled);
    lrg_canvas_set_focused_widget(app->canvas, chat.editor);
}

static void
completion_apply(ClawtLrgApp *app, guint index)
{
    Completion *row;
    g_autofree gchar *value = NULL;
    CompletionKind kind;
    gboolean takes_argument;

    if (index >= chat.rows->len)
        return;

    row = g_ptr_array_index(chat.rows, index);
    value = g_strdup(row->value);
    kind = row->kind;
    takes_argument = row->takes_argument;
    completion_hide();

    if (kind == COMPLETION_MENTION) {
        insert_mention(app, value);
        return;
    }

    /* a trailing space when it takes an argument: the next key is that */
    {
        g_autofree gchar *filled = g_strconcat(value,
                                               takes_argument ? " " : "",
                                               NULL);

        editor_set_text(app, filled);
    }
}

/*
 * Tab passes TRUE: it is unambiguously the completion key.  Return
 * passes FALSE, because with nothing highlighted Return has always sent
 * the message, and taking that over would break every /command typed.
 */
static gboolean
completion_take(ClawtLrgApp *app, gboolean fallback_first)
{
    gint index = chat.highlight;

    if (index < 0) {
        if (!fallback_first || chat.rows->len == 0)
            return FALSE;

        index = 0;
    }

    if ((guint)index >= chat.rows->len)
        return FALSE;

    completion_apply(app, (guint)index);
    return TRUE;
}

static void
on_completion_clicked(ClawtLrgApp *app, gpointer user_data)
{
    Completion *clicked = user_data;
    guint i;

    for (i = 0; i < chat.rows->len; i++) {
        if (g_ptr_array_index(chat.rows, i) == clicked) {
            completion_apply(app, i);
            return;
        }
    }
}

static void
completion_add(ClawtLrgApp *app, CompletionKind kind, const gchar *value,
               const gchar *title, const gchar *subtitle,
               gboolean takes_argument)
{
    Completion *row = g_new0(Completion, 1);
    LrgWidget *content = clawt_lrg_vbox(2.0f);

    /*
     * One line each, the description ellipsized -- not clawt_lrg_row(),
     * whose subtitle is a wrapping markdown paragraph.  A list of sixty
     * skills, rebuilt per keystroke, each paragraph re-measured as every
     * later row was added, took 2.6 s a key: the window looked hung the
     * moment "/" opened it.  A completion is read at a glance; the whole
     * description is on the Skills page.
     */
    clawt_lrg_take(content, clawt_lrg_label(title, NULL));
    if (subtitle != NULL && *subtitle != '\0') {
        LrgWidget *line = clawt_lrg_label(subtitle, "muted");

        lrg_label_set_ellipsize(LRG_LABEL(line), TRUE);
        lrg_widget_set_fill_weight(line, 1.0f);
        clawt_lrg_take(content, line);
    }

    row->kind = kind;
    row->value = g_strdup(value);
    row->takes_argument = takes_argument;
    lrg_container_set_padding(LRG_CONTAINER(content), px(app, 6.0f));
    row->widget = clickable_new(app, content, NULL, on_completion_clicked,
                                row, NULL);
    g_ptr_array_add(chat.rows, row);
    clawt_lrg_take(chat.completions, row->widget);
}

static void
completion_begin(void)
{
    clawt_lrg_clear(chat.completions);
    g_ptr_array_set_size(chat.rows, 0);
    chat.highlight = -1;
}

static void
completion_reveal(ClawtLrgApp *app)
{
    gboolean any = chat.rows->len > 0;

    if (any && !lrg_widget_get_visible(chat.completion_card))
        lrg_widget_play_appear(chat.completion_card);

    lrg_widget_set_visible(chat.completion_card, any);
    clawt_lrg_app_wake(app);
}

/*
 * The room's members as the person types "@", in the same list as the
 * commands -- "/" and "@" cannot both start the word being typed, and a
 * second list would be a second set of rules about when one is showing.
 * A conversation with one agent has nobody else to name.
 */
static gboolean
mention_rows_fill(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) roster = NULL;
    g_autoptr(GPtrArray) candidates = NULL;
    g_autofree gchar *to_cursor = NULL;
    g_autofree gchar *prefix = NULL;
    gboolean offer_all = FALSE;
    guint i;

    if (room_entry(app) == NULL)
        return FALSE;

    roster = clawt_lrg_room_roster(app->rooms, room_entry(app));
    to_cursor = editor_text_to_cursor();

    /* NULL is "not writing a mention"; "" is "the @ was just typed" */
    prefix = clawt_mention_prefix_at(to_cursor, strlen(to_cursor));

    if (prefix == NULL)
        return FALSE;

    candidates = clawt_lrg_mention_rows(prefix, roster, &offer_all);

    if (candidates->len == 0)
        return FALSE;

    completion_begin();

    for (i = 0; i < candidates->len; i++) {
        const gchar *member = g_ptr_array_index(candidates, i);
        g_autofree gchar *label = g_strconcat("@", member, NULL);

        completion_add(app, COMPLETION_MENTION, member, label, NULL, FALSE);
    }

    if (offer_all)
        completion_add(app, COMPLETION_MENTION, CLAWT_MENTION_ALL,
                       "@" CLAWT_MENTION_ALL,
                       "Everybody in this room. One turn each.", FALSE);

    return TRUE;
}

static void
on_skill_commands(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    g_clear_pointer(&chat.skill_commands, json_node_unref);

    if (reply != NULL)
        chat.skill_commands = json_node_ref(reply);

    /* still typing a command: draw the agent's own beside the built-ins */
    entry_changed(app);
}

/*
 * The agent's own commands, cached for as long as the line stays a
 * command: asked once rather than per key, and dropped when the line
 * stops being a command or the selection changes, so a skill enabled
 * while the window is open is offered the next time a slash is typed.
 */
static void skill_commands_forget(void);

static JsonNode *
skill_commands_cached(ClawtLrgApp *app)
{
    /*
     * Another agent selected since they were asked: they are its
     * commands, not this one's.  And a reply the selection guard dropped
     * left `skill_asked` set with nothing cached, so without this the
     * list stayed empty until the line stopped being a command.
     */
    if (chat.skill_generation != app->generation) {
        skill_commands_forget();
        chat.skill_generation = app->generation;
    }

    if (chat.skill_commands != NULL || chat.skill_asked ||
        app->agent_id == NULL)
        return chat.skill_commands;

    chat.skill_asked = TRUE;
    clawt_lrg_request(app, "skill.commands",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_skill_commands,
                      NULL, NULL);
    return NULL;
}

static void
skill_commands_forget(void)
{
    g_clear_pointer(&chat.skill_commands, json_node_unref);
    chat.skill_asked = FALSE;
}

/*
 * The matching commands as the person types "/": discoverability rather
 * than completion -- the list is there to be read, and anybody who knows
 * the command just keeps typing.
 */
static void
entry_changed(ClawtLrgApp *app)
{
    g_autofree gchar *text = NULL;
    const ClawtLrgSlashCommand *commands;
    JsonNode *skills;
    guint n = 0;
    guint more;
    guint i;

    if (chat.editor == NULL)
        return;

    text = editor_text();

    if (!clawt_lrg_slash_completing(text)) {
        if (!mention_rows_fill(app))
            completion_begin();

        completion_reveal(app);
        skill_commands_forget();
        return;
    }

    completion_begin();
    commands = clawt_lrg_slash_commands(&n);
    more = 0;

    for (i = 0; i < n; i++) {
        g_autofree gchar *label = NULL;

        if (!g_str_has_prefix(commands[i].name, text))
            continue;

        if (chat.rows->len >= COMPLETION_MAX_ROWS) {
            more++;
            continue;
        }
        label = clawt_lrg_slash_label(commands[i].name, commands[i].argument);
        completion_add(app, COMPLETION_COMMAND, commands[i].name, label,
                       commands[i].summary, commands[i].argument != NULL);
    }

    /*
     * ...and the agent's own, in the same list: a skill's /name is the
     * same kind of thing as a built-in, and two lists would need two
     * dismiss rules and two keyboard behaviours.
     */
    skills = skill_commands_cached(app);

    if (skills != NULL) {
        JsonObject *root = clawt_ipc_reply_object(skills);
        JsonArray *array = (root != NULL &&
                            json_object_has_member(root, "commands"))
            ? json_object_get_array_member(root, "commands") : NULL;

        for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
            JsonObject *command = json_array_get_object_element(array, i);
            const gchar *hint = clawt_ipc_payload_string(command,
                                                         "argument_hint");
            const gchar *bare = clawt_ipc_payload_string(command, "name");
            g_autofree gchar *name = g_strconcat("/", bare != NULL ? bare : "",
                                                 NULL);
            g_autofree gchar *label = NULL;

            if (bare == NULL || !g_str_has_prefix(name, text))
                continue;

            if (chat.rows->len >= COMPLETION_MAX_ROWS) {
                more++;
                continue;
            }
            label = clawt_lrg_slash_label(name, hint);
            completion_add(app, COMPLETION_COMMAND, name, label,
                           clawt_ipc_payload_string(command, "description"),
                           hint != NULL);
        }
    }

    /*
     * Said, not silently dropped: a list that stops at twelve with no
     * sign would read as "that is all of them".
     */
    if (more > 0) {
        g_autofree gchar *said = g_strdup_printf(
            "and %u more \xe2\x80\x94 keep typing to narrow it", more);

        clawt_lrg_take(chat.completions, clawt_lrg_label(said, "caption"));
    }

    completion_reveal(app);
}

guint
clawt_lrg_chat_debug_type(ClawtLrgApp *app, const gchar *text, JsonNode *skills)
{
    if (chat.editor == NULL)
        return 0;

    g_clear_pointer(&chat.skill_commands, json_node_unref);
    if (skills != NULL)
        chat.skill_commands = json_node_ref(skills);
    chat.skill_asked = TRUE;
    chat.skill_generation = app->generation;

    lrg_text_editor_set_text(LRG_TEXT_EDITOR(chat.editor), text);
    entry_changed(app);
    return chat.rows->len;
}

static void
on_editor_changed(LrgTextEditor *editor, gpointer user_data)
{
    entry_changed(user_data);
}

/* ── Keys the composer takes before the editor does ───────────────── */

/*
 * LrgTextEditor consumes Up, Down, Tab and Escape for itself, and the
 * canvas offers a key to the focused widget before anything else -- so a
 * completion list could never be driven from the keyboard.  The editor
 * is a final type with no key signal, so its class's handle_event is
 * wrapped once: the wrapper acts only on an editor marked as this
 * composer and hands every other editor, and every key it does not want,
 * to the original.  (A key-filter signal on LrgTextEditor would make
 * this unnecessary.)
 */
static gboolean (*editor_class_handle_event)(LrgWidget        *widget,
                                             const LrgUIEvent *event);

#define COMPOSER_MARK "clawt-lrg-chat-composer"

static gboolean
composer_key(ClawtLrgApp *app, GrlKey key, LrgInputModifiers mods)
{
    gboolean shift = (mods & LRG_INPUT_MODIFIER_SHIFT) != 0;
    gboolean ctrl = (mods & LRG_INPUT_MODIFIER_CTRL) != 0;

    /*
     * The list first, and only while it is showing, so every key does
     * exactly what it did before the moment it closes.
     */
    if (completion_showing()) {
        if (key == GRL_KEY_DOWN)
            return completion_move(1);

        if (key == GRL_KEY_UP)
            return completion_move(-1);

        if (key == GRL_KEY_TAB)
            return completion_take(app, TRUE);

        if (key == GRL_KEY_ESCAPE) {
            completion_hide();
            return TRUE;
        }

        if ((key == GRL_KEY_ENTER || key == GRL_KEY_KP_ENTER) && !shift &&
            completion_take(app, FALSE))
            return TRUE;
    }

    /*
     * Enter sends; Shift+Enter is a newline.  A chat box where Enter
     * inserts a newline is one nobody can send from.
     */
    if (key == GRL_KEY_ENTER || key == GRL_KEY_KP_ENTER) {
        if (shift)
            return FALSE;

        {
            g_autofree gchar *text = editor_text();

            send_text(app, text);
        }

        return TRUE;
    }

    if (!ctrl)
        return FALSE;

    /*
     * Ctrl+G writes the message in $EDITOR; with Shift it takes the
     * whole conversation there as org.  Same gesture; the difference is
     * how much of it was meant.
     */
    if (key == GRL_KEY_G) {
        if (shift)
            conversation_action(app, "edit-org");
        else
            compose_in_editor(app);

        return TRUE;
    }

    /* Ctrl+V is taken only when there is a picture to take */
    if (key == GRL_KEY_V && !shift)
        return paste_image(app);

    return FALSE;
}

static gboolean
composer_handle_event(LrgWidget *widget, const LrgUIEvent *event)
{
    if (g_object_get_data(G_OBJECT(widget), COMPOSER_MARK) != NULL &&
        lrg_ui_event_get_event_type(event) == LRG_UI_EVENT_KEY_DOWN &&
        composer_key(chat.app, lrg_ui_event_get_key(event),
                     lrg_ui_event_get_modifiers(event))) {
        clawt_lrg_app_wake(chat.app);
        return TRUE;
    }

    return editor_class_handle_event(widget, event);
}

static void
wrap_editor_keys(void)
{
    LrgWidgetClass *klass = g_type_class_ref(LRG_TYPE_TEXT_EDITOR);

    /* once per process; the class reference is kept for its life */
    if (klass->handle_event == composer_handle_event)
        return;

    editor_class_handle_event = klass->handle_event;
    klass->handle_event = composer_handle_event;
}

/* ── Slash commands ───────────────────────────────────────────────── */

typedef struct {
    gchar *agent_id;
    gchar *command;
    gchar *argument;
} CommandJob;

static CommandJob *
command_job_new(const gchar *agent_id, const gchar *command,
                const gchar *argument)
{
    CommandJob *job = g_new0(CommandJob, 1);

    job->agent_id = g_strdup(agent_id);
    job->command = g_strdup(command);
    job->argument = g_strdup(argument);
    return job;
}

static void
command_job_free(gpointer data)
{
    CommandJob *job = data;

    g_free(job->agent_id);
    g_free(job->command);
    g_free(job->argument);
    g_free(job);
}

static void
on_agents_listed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    text = clawt_lrg_chat_agents_text(reply);
    append_local(app, text);
}

static void
on_lifecycle_requested(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                       gpointer user_data)
{
    CommandJob *job = user_data;

    if (reply != NULL) {
        g_autofree gchar *said = g_strdup_printf("%s: %s requested.",
                                                 job->agent_id,
                                                 job->command + 1);

        append_local(app, said);
    }

    clawt_lrg_refresh_fleet(app);
}

static void
on_interrupted(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    CommandJob *job = user_data;
    g_autofree gchar *said = NULL;

    if (reply == NULL)
        return;

    /*
     * Lowered at once: the turn was taken out from under the code that
     * lowers the indicator, so the frame may never come, and a Stop still
     * showing after a stop reads as the press having failed.
     */
    clawt_lrg_typing_forget(app->typing, job->agent_id);
    set_activity(NULL);
    sync_stop(FALSE);
    fleet_marks_changed(app);

    said = clawt_lrg_chat_interrupt_text(job->agent_id, reply);

    if (g_strcmp0(job->command, "/interrupt") == 0)
        append_local(app, said);
    else
        clawt_lrg_toast(app, said);

    clawt_lrg_refresh_fleet(app);
}

static void
on_reset(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    g_autofree gchar *said = NULL;

    if (reply != NULL) {
        said = clawt_lrg_chat_reset_text(reply);
        append_local(app, said);
    }

    clawt_lrg_refresh_fleet(app);
}

static void
on_retry_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    const gchar *last;

    if (reply == NULL)
        return;

    last = clawt_lrg_chat_last_user_message(reply);

    if (last == NULL) {
        append_local(app, "You have not said anything to resend.");
        return;
    }

    /*
     * Put in the box rather than sent: retry usually means "that did not
     * go how I wanted", and changing a word first is the point.
     */
    editor_set_text(app, last);
}

static void
on_files_listed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                gpointer user_data)
{
    CommandJob *job = user_data;
    const gchar *path;

    if (reply == NULL)
        return;

    if (g_strcmp0(job->command, "/files") == 0 || job->argument == NULL) {
        g_autofree gchar *text = clawt_lrg_chat_files_text(reply);

        append_local(app, text);
        return;
    }

    /* the daemon's path, never one built here */
    path = clawt_lrg_chat_file_path(reply, job->argument);

    if (path != NULL) {
        clawt_lrg_open_in_editor(app, path);
        return;
    }

    {
        g_autofree gchar *said = g_strdup_printf(
            "%s has no file called '%s'. /files lists them.", job->agent_id,
            job->argument);

        append_local(app, said);
    }
}

static void
on_memories(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    text = clawt_lrg_chat_memory_text(reply);
    append_local(app, text);
}

static void
on_skill_expanded(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    CommandJob *job = user_data;
    const gchar *prompt = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "prompt");

    /*
     * Not a built-in and not a skill: said, never sent -- a mistyped
     * command reaching the model as prose is how a person learns to
     * distrust the feature.
     */
    if (reply == NULL || prompt == NULL) {
        g_autofree gchar *said = clawt_lrg_slash_unknown_text(job->command);

        append_local(app, said);
        return;
    }

    /*
     * Expanded by the daemon, so this client and the others send
     * identical text, and then sent down the ordinary path: one send,
     * with its attachments, drafts and stopped-agent handling.
     */
    send_text(app, prompt);
}

/*
 * Recall is not about one agent, so it goes to the memory page, and the
 * query with it (clawt_lrg_recall_search()).
 */

static void
run_slash_command(ClawtLrgApp *app, const gchar *text)
{
    g_autofree gchar *name = NULL;
    g_autofree gchar *rest = NULL;
    const gchar *agent = app->agent_id;

    clawt_lrg_slash_split(text, &name, &rest);

    if (clawt_lrg_slash_find(name) == NULL) {
        /* perhaps one of the agent's own skills */
        if (agent == NULL) {
            g_autofree gchar *said = clawt_lrg_slash_unknown_text(name);

            append_local(app, said);
            return;
        }

        {
            g_autoptr(JsonBuilder) builder = json_builder_new();

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, agent);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, name + 1);

            if (rest != NULL) {
                json_builder_set_member_name(builder, "arguments");
                json_builder_add_string_value(builder, rest);
            }

            json_builder_end_object(builder);
            clawt_lrg_request(app, "skill.expand",
                              json_builder_get_root(builder), CLAWT_LRG_QUIET,
                              on_skill_expanded,
                              command_job_new(agent, name, rest),
                              command_job_free);
        }

        return;
    }

    if (g_strcmp0(name, "/help") == 0) {
        g_autofree gchar *help = clawt_lrg_slash_help();

        append_local(app, help);
        return;
    }

    if (g_strcmp0(name, "/clear") == 0) {
        reset_transcript();
        g_hash_table_remove_all(chat.shown);
        append_local(app, "Cleared on screen. The transcript on disk is "
                          "untouched -- reopen this agent to see it again.");
        return;
    }

    if (g_strcmp0(name, "/compose") == 0) {
        editor_set_text(app, "");
        compose_in_editor(app);
        return;
    }

    if (g_strcmp0(name, "/attach") == 0) {
        pick_files(app);
        return;
    }

    if (g_strcmp0(name, "/flow") == 0) {
        clawt_lrg_show_page(app, CLAWT_PAGE_FLOW);
        return;
    }

    if (g_strcmp0(name, "/tasks") == 0) {
        clawt_lrg_show_page(app, CLAWT_PAGE_TASKS);
        return;
    }

    if (g_strcmp0(name, "/new") == 0) {
        clawt_lrg_new_agent_dialog(app);
        return;
    }

    if (g_strcmp0(name, "/agents") == 0) {
        clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_REQUEST_NONE,
                          on_agents_listed, NULL, NULL);
        return;
    }

    if (g_strcmp0(name, "/recall") == 0) {
        clawt_lrg_show_page(app, CLAWT_PAGE_MEMORY);

        clawt_lrg_recall_search(app, rest != NULL ? rest : "");

        return;
    }

    if (clawt_lrg_slash_needs_agent(name) && agent == NULL) {
        append_local(app, clawt_lrg_slash_needs_agent_text(
                              room_entry(app) != NULL));
        return;
    }

    if (g_strcmp0(name, "/start") == 0) {
        clawt_lrg_request(app, "agent.start",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_requested,
                          command_job_new(agent, name, NULL),
                          command_job_free);
        return;
    }

    if (g_strcmp0(name, "/stop") == 0) {
        clawt_lrg_request(app, "agent.stop",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_requested,
                          command_job_new(agent, name, NULL),
                          command_job_free);
        return;
    }

    if (g_strcmp0(name, "/restart") == 0) {
        clawt_lrg_request(app, "agent.restart",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_requested,
                          command_job_new(agent, name, NULL),
                          command_job_free);
        return;
    }

    if (g_strcmp0(name, "/interrupt") == 0) {
        clawt_lrg_request(app, "agent.interrupt",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_interrupted,
                          command_job_new(agent, name, NULL),
                          command_job_free);
        return;
    }

    if (g_strcmp0(name, "/reset") == 0) {
        clawt_lrg_request(app, "agent.reset",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_reset, NULL, NULL);
        return;
    }

    if (g_strcmp0(name, "/retry") == 0) {
        clawt_lrg_request(app, "room.history",
                          clawt_ipc_payload_new("room", agent, "as", "user",
                                                NULL),
                          CLAWT_LRG_GUARDED, on_retry_history, NULL, NULL);
        return;
    }

    if (g_strcmp0(name, "/export") == 0 || g_strcmp0(name, "/copy") == 0) {
        g_autofree gchar *what = g_strdup_printf(
            "%s-%s", g_strcmp0(name, "/copy") == 0 ? "copy" : "save",
            rest != NULL ? rest : "markdown");

        conversation_action(app, what);
        return;
    }

    if (g_strcmp0(name, "/files") == 0 || g_strcmp0(name, "/edit") == 0) {
        clawt_lrg_request(app, "agent.files",
                          clawt_ipc_payload_new("agent", agent, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_files_listed,
                          command_job_new(agent, name, rest),
                          command_job_free);
        return;
    }

    if (g_strcmp0(name, "/memory") == 0) {
        if (rest != NULL)
            clawt_lrg_request(app, "memory.search",
                              clawt_ipc_payload_new("agent", agent,
                                                    "query", rest, NULL),
                              CLAWT_LRG_REQUEST_NONE, on_memories, NULL,
                              NULL);
        else
            clawt_lrg_request(app, "memory.list",
                              clawt_ipc_payload_new("agent", agent, NULL),
                              CLAWT_LRG_REQUEST_NONE, on_memories, NULL,
                              NULL);
        return;
    }
}

/* ── Sending ──────────────────────────────────────────────────────── */

typedef struct {
    gchar     *target;     /* room id or agent id */
    gchar     *draft_key;
    gchar     *typed;      /* what the composer held, to clear it after */
    gchar     *body;       /* what goes, before the attachment block */
    GPtrArray *files;      /* ClawtLrgSentFile, in queue order */
    guint      waiting;    /* attachment.put replies outstanding */
} SendJob;

static void
send_job_free(SendJob *job)
{
    g_free(job->target);
    g_free(job->draft_key);
    g_free(job->typed);
    g_free(job->body);
    g_clear_pointer(&job->files, g_ptr_array_unref);
    g_free(job);
}

static void
on_sent(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    SendJob *job = user_data;
    g_autofree gchar *note = NULL;
    g_autofree gchar *now = NULL;
    gboolean waiting = FALSE;

    chat.sending = FALSE;

    if (reply == NULL)
        return;

    /*
     * Not drawn here: the daemon publishes an event for every message it
     * routes, this one included, and that is the version carrying the
     * room the transcript is filtered on.  The composer is emptied only
     * if it still holds what was sent -- somebody may have gone on typing
     * while the reply was on its way.
     */
    now = editor_text();

    if (g_strcmp0(now, job->typed) == 0 &&
        g_strcmp0(chat.key, job->draft_key) == 0)
        lrg_text_editor_set_text(LRG_TEXT_EDITOR(chat.editor), "");

    chat_persist_draft(app, job->draft_key, NULL);

    note = clawt_lrg_chat_send_note(job->target, reply, &waiting);

    if (note != NULL)
        clawt_lrg_toast(app, note);

    if (g_strcmp0(chat.key, job->draft_key) == 0)
        set_activity(waiting ? "delivered -- waiting for a reply" : NULL);

    set_following(TRUE);
    chat.auto_scrolling = FALSE;
}

static void
send_now(ClawtLrgApp *app, SendJob *job)
{
    g_autofree gchar *full = clawt_lrg_body_with_attachments(job->body,
                                                             job->files);

    /*
     * The room when one is selected, the agent otherwise: msg.send
     * resolves either, a room id naming the room and an agent id the
     * direct conversation with it.
     */
    clawt_lrg_request(app, "msg.send",
                      clawt_ipc_payload_new("target", job->target,
                                            "body", full, "from", "user",
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_sent, job,
                      (GDestroyNotify)send_job_free);
}

typedef struct {
    SendJob *job;
    guint    index;
} PutJob;

static void
on_put(ClawtLrgApp *app, JsonNode *reply, const GError *error,
       gpointer user_data)
{
    PutJob *put = user_data;
    ClawtLrgSentFile *file = g_ptr_array_index(put->job->files, put->index);
    JsonObject *root = clawt_ipc_reply_object(reply);

    if (root != NULL) {
        file->saved = TRUE;
        file->host_path = g_strdup(clawt_ipc_payload_string(root,
                                                            "host_path"));
        file->path = g_strdup(clawt_ipc_payload_string(root, "path"));
    }

    /* the message goes once every file has an answer, saved or not */
    if (--put->job->waiting == 0)
        send_now(app, put->job);
}

/*
 * Hands the queued files to the daemon, which writes them where the
 * agent can reach them and says what path the agent should use -- not
 * the host path, for an agent in a container.
 */
static void
send_with_attachments(ClawtLrgApp *app, SendJob *job)
{
    guint i;

    job->files = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_sent_file_free);

    if (chat.pending->len == 0) {
        send_now(app, job);
        return;
    }

    job->waiting = chat.pending->len;

    for (i = 0; i < chat.pending->len; i++) {
        PendingFile *pending = g_ptr_array_index(chat.pending, i);
        ClawtLrgSentFile *file = g_new0(ClawtLrgSentFile, 1);
        PutJob *put = g_new0(PutJob, 1);
        gsize length = 0;
        const guchar *data = g_bytes_get_data(pending->bytes, &length);
        g_autofree gchar *encoded = g_base64_encode(data, length);

        file->name = g_strdup(pending->name);
        g_ptr_array_add(job->files, file);
        put->job = job;
        put->index = i;

        clawt_lrg_request(app, "attachment.put",
                          clawt_ipc_payload_new("agent", app->agent_id,
                                                "name", pending->name,
                                                "data", encoded, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_put, put, g_free);
    }

    g_ptr_array_set_size(chat.pending, 0);
    refresh_attachment_strip(app);
}

static void
send_text(ClawtLrgApp *app, const gchar *text)
{
    const gchar *target = (room_entry(app) != NULL) ? room_entry(app)
                                                    : app->agent_id;
    SendJob *job;

    /* a room is somewhere to send to, exactly as an agent is */
    if (target == NULL || text == NULL || *text == '\0')
        return;

    if (chat.conversation != NULL && room_entry(app) == NULL)
        return;

    completion_hide();

    /* a command never reaches the agent as prose */
    if (text[0] == '/') {
        g_autofree gchar *command = g_strdup(text);

        lrg_text_editor_set_text(LRG_TEXT_EDITOR(chat.editor), "");
        run_slash_command(app, command);
        return;
    }

    if (chat.sending)
        return;

    chat.sending = TRUE;
    job = g_new0(SendJob, 1);
    job->target = g_strdup(target);
    job->draft_key = g_strdup(chat.key);
    job->typed = editor_text();
    job->body = g_strdup(text);
    send_with_attachments(app, job);
}

static void
on_send_clicked(LrgButton *button, gpointer user_data)
{
    g_autofree gchar *text = editor_text();

    send_text(user_data, text);
}

/*
 * Ends what the agent is doing and leaves it running -- deliberately not
 * agent.stop, which takes the agent and its machine down when somebody
 * only wanted a runaway turn to end.
 */
static void
on_stop_turn_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (app->agent_id == NULL)
        return;

    clawt_lrg_request(app, "agent.interrupt",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_interrupted,
                      command_job_new(app->agent_id, "stop-button", NULL),
                      command_job_free);
}

/* ── Page actions ─────────────────────────────────────────────────── */

static void
on_page_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    conversation_action(user_data, g_action_get_name(G_ACTION(action)));
}

static void
build_actions(ClawtLrgApp *app, LrgWidget *owner)
{
    static const gchar *const names[] = {
        "copy-text", "copy-markdown", "copy-org",
        "edit-text", "edit-markdown", "edit-org",
        "save-text", "save-markdown", "save-org"
    };
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) copy = g_menu_new();
    g_autoptr(GMenu) edit = g_menu_new();
    g_autoptr(GMenu) save = g_menu_new();
    gsize i;

    chat.actions = g_simple_action_group_new();

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_page_action), app);
        g_action_map_add_action(G_ACTION_MAP(chat.actions), G_ACTION(action));
    }

    {
        g_autoptr(GSimpleAction) action = g_simple_action_new_stateful(
            "conversation", G_VARIANT_TYPE_STRING, g_variant_new_string(""));

        g_signal_connect(action, "activate",
                         G_CALLBACK(on_conversation_chosen), app);
        g_action_map_add_action(G_ACTION_MAP(chat.actions), G_ACTION(action));
    }

    lrg_widget_insert_action_group(chat.page, "chat",
                                   G_ACTION_GROUP(chat.actions));

    /*
     * Right-clicking the conversation rather than one message: on the
     * scroller, so the empty space under the last message counts -- that
     * is where somebody actually right-clicks.
     */
    g_menu_append(copy, "Copy conversation as text", "chat.copy-text");
    g_menu_append(copy, "Copy conversation as markdown", "chat.copy-markdown");
    g_menu_append(copy, "Copy conversation as org", "chat.copy-org");
    g_menu_append(edit, "Open in $EDITOR as text", "chat.edit-text");
    g_menu_append(edit, "Open in $EDITOR as markdown", "chat.edit-markdown");
    g_menu_append(edit, "Open in $EDITOR as org", "chat.edit-org");
    g_menu_append(save, "Save as text\342\200\246", "chat.save-text");
    g_menu_append(save, "Save as markdown\342\200\246", "chat.save-markdown");
    g_menu_append(save, "Save as org\342\200\246", "chat.save-org");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(copy));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(save));

    lrg_widget_set_context_menu(owner, G_MENU_MODEL(menu));
}

/* ── Building ─────────────────────────────────────────────────────── */

static LrgWidget *
build_composer(ClawtLrgApp *app)
{
    LrgWidget *composer = clawt_lrg_vbox(px(app, 6.0f));
    LrgWidget *entry_row = clawt_lrg_hbox(px(app, 8.0f));
    LrgWidget *attach;
    LrgInsets margin = { px(app, CHAT_BODY_INSET), px(app, 6.0f),
                         px(app, CHAT_ROW_MARGIN), px(app, 14.0f) };
    LrgCustomWidget *spinner_dot;

    lrg_widget_set_margin(composer, &margin);

    /*
     * The activity line: a chat that shows nothing between the question
     * and the answer is indistinguishable from a broken one, and a turn
     * can run for minutes.
     */
    chat.activity = clawt_lrg_hbox(px(app, 8.0f));
    lrg_container_set_padding(LRG_CONTAINER(chat.activity), px(app, 6.0f));
    lrg_widget_add_class(chat.activity, "clawt-busy");
    lrg_widget_add_class(chat.activity, "clawt-row");
    spinner_dot = lrg_custom_widget_new();
    lrg_custom_widget_set_preferred_width(spinner_dot, 0.0f);
    lrg_custom_widget_set_paint_box(spinner_dot, FALSE);
    clawt_lrg_take(chat.activity, LRG_WIDGET(spinner_dot));
    chat.activity_label = clawt_lrg_label("", "caption");
    lrg_widget_add_class(chat.activity_label, "muted");
    clawt_lrg_add(chat.activity, chat.activity_label);
    g_object_unref(chat.activity_label);
    lrg_widget_set_visible(chat.activity, FALSE);
    clawt_lrg_add(composer, chat.activity);
    g_object_unref(chat.activity);

    /* the completion list, on a card above the entry */
    chat.completions = clawt_lrg_vbox(2.0f);
    chat.completion_card = clawt_lrg_card(NULL);
    lrg_container_set_padding(LRG_CONTAINER(chat.completion_card),
                              px(app, 6.0f));
    {
        LrgScrollBox *list_scroll = lrg_scroll_box_new();

        lrg_scroll_box_set_hscroll(list_scroll, FALSE);
        lrg_scroll_box_set_vscroll(list_scroll, TRUE);
        lrg_scroll_box_set_child(list_scroll, chat.completions);
        lrg_widget_set_max_size(LRG_WIDGET(list_scroll), -1.0f,
                                px(app, 240.0f));
        clawt_lrg_take(chat.completion_card, LRG_WIDGET(list_scroll));
    }
    g_object_unref(chat.completions);
    lrg_widget_set_appear(chat.completion_card, LRG_APPEAR_FLY_UP, 0.0f);
    lrg_widget_set_visible(chat.completion_card, FALSE);
    clawt_lrg_add(composer, chat.completion_card);
    g_object_unref(chat.completion_card);

    /* the queued files, hidden until there are some */
    chat.attachments = clawt_lrg_hbox(px(app, 6.0f));
    lrg_widget_set_visible(chat.attachments, FALSE);
    clawt_lrg_add(composer, chat.attachments);
    g_object_unref(chat.attachments);

    /* the entry itself, glass with a rim light */
    chat.editor = clawt_lrg_editor(NULL, NULL);
    lrg_widget_add_class(chat.editor, "clawt-composer");
    lrg_text_editor_set_submit_on_enter(LRG_TEXT_EDITOR(chat.editor), FALSE);
    lrg_text_editor_set_highlight_current_line(LRG_TEXT_EDITOR(chat.editor),
                                               FALSE);
    lrg_text_editor_set_placeholder(
        LRG_TEXT_EDITOR(chat.editor),
        "Message  \342\200\224  / for commands, Ctrl+G to write it in "
        "$EDITOR");
    lrg_widget_set_min_size(chat.editor, 0.0f, px(app, 64.0f));
    lrg_widget_set_max_size(chat.editor, -1.0f, px(app, 200.0f));
    lrg_widget_set_fill_weight(chat.editor, 1.0f);
    g_object_set_data(G_OBJECT(chat.editor), COMPOSER_MARK,
                      GINT_TO_POINTER(1));
    g_signal_connect(chat.editor, "changed", G_CALLBACK(on_editor_changed),
                     app);
    wrap_editor_keys();
    clawt_lrg_add(entry_row, chat.editor);
    g_object_unref(chat.editor);

    attach = clawt_lrg_button("Attach", "flat", G_CALLBACK(on_attach_clicked),
                              app);
    lrg_widget_set_valign(attach, LRG_ALIGN_END);
    clawt_lrg_take(entry_row, attach);

    /*
     * Stop, beside Send rather than in the agent's menu: it is about the
     * turn in front of you, and the menu's Stop takes the agent down.
     */
    chat.stop_button = clawt_lrg_button("Stop turn", "danger",
                                        G_CALLBACK(on_stop_turn_clicked),
                                        app);
    lrg_widget_set_valign(chat.stop_button, LRG_ALIGN_END);
    lrg_widget_set_visible(chat.stop_button, FALSE);
    lrg_widget_set_appear(chat.stop_button, LRG_APPEAR_POP, 0.0f);
    clawt_lrg_add(entry_row, chat.stop_button);
    g_object_unref(chat.stop_button);

    chat.send_button = clawt_lrg_button("Send", "primary",
                                        G_CALLBACK(on_send_clicked), app);
    lrg_widget_set_valign(chat.send_button, LRG_ALIGN_END);
    clawt_lrg_add(entry_row, chat.send_button);
    g_object_unref(chat.send_button);

    clawt_lrg_take(composer, entry_row);
    return composer;
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_vbox(0.0f);
    LrgOverlay *stage = lrg_overlay_new();
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgCustomWidget *follower = lrg_custom_widget_new();
    LrgWidget *column = clawt_lrg_vbox(0.0f);
    LrgWidget *bar = clawt_lrg_hbox(px(app, 8.0f));
    LrgWidget *jump_pill;
    LrgInsets bar_margin = { px(app, 18.0f), px(app, 10.0f), px(app, 18.0f),
                             0.0f };
    LrgInsets jump_margin = { 0.0f, 0.0f, 0.0f, px(app, 14.0f) };

    memset(&chat, 0, sizeof(chat));
    chat.app = app;
    chat.page = page;
    chat.shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    chat.drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
    chat.pending = g_ptr_array_new_with_free_func(pending_file_free);
    chat.rows = g_ptr_array_new_with_free_func(completion_free);
    chat.highlight = -1;
    chat.following = TRUE;
    chat.client_seen = app->client;
    chat.connected_at = g_get_real_time();

    /* which conversation is on screen, when there is more than one */
    lrg_widget_set_margin(bar, &bar_margin);
    clawt_lrg_take(bar, clawt_lrg_label("Conversation", "caption"));
    chat.conversation_button = clawt_lrg_button("Chat", "flat",
                                                G_CALLBACK(conversation_picker),
                                                app);
    lrg_widget_add_class(chat.conversation_button, "clawt-tab");
    clawt_lrg_add(bar, chat.conversation_button);
    g_object_unref(chat.conversation_button);
    lrg_widget_set_visible(bar, FALSE);
    chat.conversation_bar = bar;
    clawt_lrg_take(page, bar);

    /*
     * The transcript: the hero.  A column bounded by the reader's measure
     * and centred, so a wider window widens the margins rather than the
     * lines; inside a scroller that stays at the window edge, where the
     * wheel and the bar are reachable.
     */
    chat.transcript = clawt_lrg_vbox(0.0f);
    lrg_widget_set_name(chat.transcript, "transcript-column");
    clawt_lrg_add(column, chat.transcript);
    g_object_unref(chat.transcript);

    {
        LrgWidget *bottom = clawt_lrg_vbox(0.0f);

        /* the last turn does not sit against the composer */
        lrg_widget_set_min_size(bottom, 0.0f, px(app, 18.0f));
        clawt_lrg_take(column, bottom);
    }

    lrg_widget_set_halign(column, LRG_ALIGN_CENTER);
    lrg_container_set_padding(LRG_CONTAINER(column), px(app, 8.0f));
    chat.column = column;

    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_kinetic(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, column);
    g_object_unref(column);
    chat.scroll = LRG_WIDGET(scroll);
    clawt_lrg_take(LRG_WIDGET(stage), LRG_WIDGET(scroll));

    /*
     * The pill that says something arrived while you were reading: over
     * the transcript, centred, clear of the composer.  It appears only
     * when a message has arrived while not following -- a control always
     * there while you read carries no information.
     */
    jump_pill = clawt_lrg_button("\342\206\223  New messages", "primary",
                                 G_CALLBACK(on_jump_clicked), app);
    lrg_widget_set_halign(jump_pill, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(jump_pill, LRG_ALIGN_END);
    lrg_widget_set_margin(jump_pill, &jump_margin);
    lrg_widget_set_appear(jump_pill, LRG_APPEAR_FLY_UP, 0.0f);
    lrg_widget_set_visible(jump_pill, FALSE);
    chat.jump = jump_pill;
    clawt_lrg_take(LRG_WIDGET(stage), jump_pill);

    lrg_widget_set_fill_weight(LRG_WIDGET(stage), 1.0f);
    clawt_lrg_take(page, LRG_WIDGET(stage));

    /*
     * The composer follows the transcript's column: the same width, the
     * same padding and the same centring, so the thing you read and the
     * thing you type into are one column -- a full-width entry under a
     * narrow column of text looks like a rendering fault.
     */
    chat.composer = clawt_lrg_vbox(0.0f);
    lrg_widget_set_halign(chat.composer, LRG_ALIGN_CENTER);
    lrg_container_set_padding(LRG_CONTAINER(chat.composer), px(app, 8.0f));
    clawt_lrg_take(chat.composer, build_composer(app));
    clawt_lrg_add(page, chat.composer);
    g_object_unref(chat.composer);

    /* the measuring stick for a measure counted in characters */
    chat.char_probe = clawt_lrg_label("0000000000", NULL);
    lrg_widget_set_visible(chat.char_probe, FALSE);
    clawt_lrg_add(page, chat.char_probe);
    g_object_unref(chat.char_probe);

    /* the follower: draws nothing, ticks with the canvas */
    lrg_custom_widget_set_paint_box(follower, FALSE);
    lrg_custom_widget_set_preferred_width(follower, 0.0f);
    lrg_custom_widget_set_preferred_height(follower, 0.0f);
    g_signal_connect(follower, "step", G_CALLBACK(on_follow_step), app);
    clawt_lrg_take(page, LRG_WIDGET(follower));

    build_actions(app, LRG_WIDGET(scroll));

    /* the first refresh, straight after this, loads the conversation */
    return page;
}

/* ── Refresh and events ───────────────────────────────────────────── */

static void
refresh(ClawtLrgApp *app)
{
    const gchar *key = selected_label(app);

    if (chat.page == NULL)
        return;

    notice_connection(app);

    if (!chat.primed || g_strcmp0(key, chat.key) != 0) {
        g_autofree gchar *leaving = editor_text();
        g_autofree gchar *held = NULL;

        /* keep what was being written wherever we are leaving */
        if (chat.key != NULL)
            chat_persist_draft(app, chat.key, leaving);

        chat.primed = TRUE;
        g_free(chat.key);
        chat.key = g_strdup(key);

        /*
         * A different agent opens on its operator conversation; landing
         * in a conversation between two others would be the wrong room
         * under every agent it was shown for.
         */
        g_clear_pointer(&chat.conversation, g_free);
        g_clear_pointer(&chat.room, g_free);
        skill_commands_forget();
        completion_hide();
        g_ptr_array_set_size(chat.pending, 0);
        refresh_attachment_strip(app);

        held = stored_draft(app, key);
        lrg_text_editor_set_text(LRG_TEXT_EDITOR(chat.editor),
                                 held != NULL ? held : "");

        /*
         * Opening a conversation is the only thing that clears its count:
         * not scrolling, not time passing -- a counter that decays on its
         * own is one you stop trusting.  The shell has already set the
         * room to the agent's direct one.
         */
        if (clawt_lrg_unread_clear(app->unread, app->room_id))
            fleet_marks_changed(app);

        load_history(app);
        lrg_canvas_set_focused_widget(app->canvas, chat.editor);
    } else if (key != NULL && chat.room == NULL &&
               chat.history_inflight == 0) {
        /*
         * A transcript with no room lost its load, and a click on it
         * again is what a person actually tries against a stuck view.
         */
        load_history(app);
    }

    fill_conversation_bar(app);
    sync_selected_activity(app);
    clawt_lrg_app_wake(app);
}

static void
on_message_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *room = clawt_event_get_subject(event);
    const gchar *from = clawt_event_get_detail(event, "from");
    const gchar *body = clawt_event_get_detail(event, "body");
    const gchar *id = clawt_event_get_detail(event, "id");
    JsonObject *agent = clawt_lrg_selected_agent(app);
    g_autoptr(GHashTable) rows = NULL;

    /*
     * Matched on the room and only the room: matching the sender drew an
     * agent's reply to a peer into the operator's chat with it.
     */
    if (chat.transcript != NULL && chat.room != NULL &&
        g_strcmp0(room, chat.room) == 0 &&
        (id == NULL || !g_hash_table_contains(chat.shown, id))) {
        if (id != NULL)
            g_hash_table_add(chat.shown, g_strdup(id));

        /* the rule goes above the first thing not yet seen */
        note_arrival(app);
        append_message(app, from != NULL ? from : "?",
                       body != NULL ? body : "", 0,
                       clawt_event_get_detail(event, "task"), 0, TRUE);

        /*
         * The reply is the end of the turn: the indicator drops too, but
         * the message overtakes it often enough to leave a spinner under
         * an answer already there.
         */
        if (g_strcmp0(from, app->agent_id) == 0)
            set_activity(NULL);
    } else if (chat.history_inflight == 0 && chat.conversation == NULL &&
               app->agent_id != NULL && room != NULL &&
               g_strcmp0(room, chat.room) != 0 &&
               g_strcmp0(room, clawt_ipc_payload_string(agent,
                                                        "dm_room")) == 0) {
        /*
         * The daemon says this is the selected agent's own conversation
         * and the transcript's room disagrees: the transcript is what is
         * wrong (a failed or superseded load), and reloading resolves it
         * with this message already in the history.
         */
        load_history(app);
    }

    /*
     * Anything not for the room on screen counts -- against the room,
     * and only for rooms the sidebar draws a row for.
     */
    rows = clawt_lrg_chat_row_rooms(app->agents, app->rooms);

    if (clawt_lrg_unread_note(app->unread, room, chat.room, from,
                              clawt_event_get_timestamp(event),
                              chat.connected_at, rows))
        fleet_marks_changed(app);
}

static void
on_typing_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *agent = clawt_event_get_subject(event);
    const gchar *room = clawt_event_get_detail(event, "room");
    gboolean typing = g_strcmp0(clawt_event_get_detail(event, "typing"),
                                "true") == 0;
    gboolean edge;

    /*
     * A level, re-sent every 25 seconds per room: folded into the set,
     * and only the set's edge redraws the sidebar.
     */
    edge = clawt_lrg_typing_note(app->typing, agent, room, typing);

    if (g_strcmp0(agent, app->agent_id) == 0) {
        /* the steps of the turn that ended here are sealed, not removed */
        if (!typing && chat.room != NULL && g_strcmp0(room, chat.room) == 0)
            steps_seal();

        /*
         * Who it is for when it is not for you: "thinking" under a chat
         * while the agent answers a peer is the lie the sidebar told.
         */
        if (typing && (room == NULL || g_strcmp0(room, chat.room) != 0)) {
            g_autofree gchar *line = clawt_lrg_typing_activity(
                TRUE, clawt_event_get_detail(event, "peer"));

            set_activity(line);
            sync_stop(TRUE);
        } else {
            sync_selected_activity(app);
        }
    }

    if (edge)
        fleet_marks_changed(app);
}

static void
on_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (kind == NULL)
        return;

    /*
     * Before the page is built there is no transcript to draw into, but
     * the window's unread counts and typing levels are kept here all the
     * same -- so only those two run.
     */
    if (chat.page == NULL) {
        if (g_strcmp0(kind, "agent.typing") == 0 &&
            clawt_lrg_typing_note(app->typing, clawt_event_get_subject(event),
                                  clawt_event_get_detail(event, "room"),
                                  g_strcmp0(clawt_event_get_detail(event,
                                                                   "typing"),
                                            "true") == 0))
            fleet_marks_changed(app);

        return;
    }

    notice_connection(app);

    if (g_strcmp0(kind, "message") == 0) {
        on_message_event(app, event);
        return;
    }

    if (g_strcmp0(kind, "agent.typing") == 0) {
        on_typing_event(app, event);
        return;
    }

    if (g_strcmp0(kind, "turn.step") == 0) {
        g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_event(event);

        /*
         * Only the conversation on screen: an agent can be mid-turn in
         * three rooms at once, and a peer exchange's tool calls under the
         * operator's chat would read as work nobody asked for.
         */
        if (step != NULL && chat.room != NULL &&
            g_strcmp0(clawt_turn_step_get_room_id(step), chat.room) == 0)
            steps_add_live(app, step);

        return;
    }

    if (g_strcmp0(kind, "message.refused") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *to = clawt_event_get_detail(event, "to");
        const gchar *reason = clawt_event_get_detail(event, "reason");
        g_autofree gchar *said = g_strdup_printf(
            "%s to %s was stopped: %s", from != NULL ? from : "?",
            to != NULL ? to : "?", reason != NULL ? reason : "a limit");

        /*
         * Invisible otherwise: two agents just stop, and the conversation
         * appears to trail off for no reason.  The alerts drawer records
         * every one (lrg-alerts.c), where it can be found an hour later;
         * the transcript says it too when it is about the one on screen.
         */
        if (chat.room != NULL &&
            g_strcmp0(clawt_event_get_subject(event), chat.room) == 0)
            append_local(app, said);

        return;
    }

    /*
     * State describing a process is cleared when the process ends, all
     * of it: a stopped agent's turns all ended, whatever frames arrived.
     */
    if (g_strcmp0(kind, "agent.state") == 0) {
        const gchar *state = clawt_event_get_detail(event, "state");

        if (g_strcmp0(state, "running") != 0 &&
            clawt_lrg_typing_forget(app->typing,
                                    clawt_event_get_subject(event)))
            fleet_marks_changed(app);

        if (g_strcmp0(clawt_event_get_subject(event), app->agent_id) == 0)
            sync_selected_activity(app);

        return;
    }

    /* a skill enabled or removed changes what "/" offers */
    if (g_str_has_prefix(kind, "skill."))
        skill_commands_forget();

    /* a peer conversation may have begun: the picker gains an entry */
    if (g_str_has_prefix(kind, "room."))
        fill_conversation_bar(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_CHAT, build, refresh, on_event
};

const ClawtLrgPage *
clawt_lrg_chat_page(void)
{
    return &page_impl;
}
