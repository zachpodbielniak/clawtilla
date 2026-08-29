/*
 * clawt-screen.c - Asking a compositor for a frame, and typing at it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-screen.h"

#include <stdlib.h>
#include <string.h>

void
clawt_screen_frame_info_clear(ClawtScreenFrameInfo *self)
{
    if (self == NULL)
        return;

    g_clear_pointer(&self->path, g_free);
    g_clear_pointer(&self->hash, g_free);
    self->width = 0;
    self->height = 0;
    self->stamp = 0;
}

/*
 * Adds the four words every gdbus call to the extension begins with.
 */
static void
add_gdbus_prefix(GPtrArray *argv, const gchar *method)
{
    g_ptr_array_add(argv, g_strdup("gdbus"));
    g_ptr_array_add(argv, g_strdup("call"));
    g_ptr_array_add(argv, g_strdup("--session"));
    g_ptr_array_add(argv, g_strdup("--dest"));
    g_ptr_array_add(argv, g_strdup(CLAWT_SCREEN_GNOME_BUS_NAME));
    g_ptr_array_add(argv, g_strdup("--object-path"));
    g_ptr_array_add(argv, g_strdup(CLAWT_SCREEN_GNOME_OBJECT_PATH));
    g_ptr_array_add(argv, g_strdup("--method"));
    g_ptr_array_add(argv, g_strdup_printf("%s.%s",
                                          CLAWT_SCREEN_GNOME_INTERFACE,
                                          method));
}

GStrv
clawt_screen_gnome_frame_argv(guint max_width, gboolean include_cursor)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    add_gdbus_prefix(argv, "ScreenshotFrame");
    g_ptr_array_add(argv, g_strdup_printf("%u", max_width));

    /*
     * The pointer is left out by default, and the reason is not
     * aesthetic: `includeCursor` composites the cursor into the image,
     * so two frames of a perfectly still screen differ whenever the
     * mouse has moved a pixel -- and the hash that makes polling cheap
     * would then change on every grab.
     */
    g_ptr_array_add(argv, g_strdup(include_cursor ? "true" : "false"));
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

GStrv
clawt_screen_gnome_monitors_argv(void)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    add_gdbus_prefix(argv, "GetMonitors");
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

GStrv
clawt_screen_gnome_input_argv(ClawtInputEvent *event)
{
    GPtrArray *argv;

    g_return_val_if_fail(event != NULL, NULL);

    argv = g_ptr_array_new_with_free_func(g_free);

    switch (event->kind) {
    case CLAWT_INPUT_KEY:
        /*
         * KeyCombo rather than KeyPress, for every key.  KeyPress takes
         * a keyval, which means converting a name to a number on this
         * side with a table that would immediately be a hand-maintained
         * copy of somebody else's -- and KeyCombo accepts a bare key
         * name as a combo of one, so there is nothing to gain by having
         * two paths.
         */
        if (event->text == NULL || *event->text == '\0') {
            g_ptr_array_free(argv, TRUE);
            return NULL;
        }

        add_gdbus_prefix(argv, "KeyCombo");
        g_ptr_array_add(argv, g_strdup(event->text));
        break;

    case CLAWT_INPUT_TEXT:
        if (event->text == NULL || *event->text == '\0') {
            g_ptr_array_free(argv, TRUE);
            return NULL;
        }

        add_gdbus_prefix(argv, "TypeText");
        g_ptr_array_add(argv, g_strdup(event->text));
        break;

    case CLAWT_INPUT_CLICK:
        add_gdbus_prefix(argv, "MouseClick");
        g_ptr_array_add(argv, g_strdup_printf("%d", event->x));
        g_ptr_array_add(argv, g_strdup_printf("%d", event->y));
        g_ptr_array_add(argv, g_strdup_printf("%u",
                                              event->button > 0
                                              ? event->button : 1));
        break;

    case CLAWT_INPUT_MOVE:
        add_gdbus_prefix(argv, "MouseMove");
        g_ptr_array_add(argv, g_strdup_printf("%d", event->x));
        g_ptr_array_add(argv, g_strdup_printf("%d", event->y));
        break;

    case CLAWT_INPUT_SCROLL:
        add_gdbus_prefix(argv, "MouseScroll");
        g_ptr_array_add(argv, g_strdup_printf("%d", event->x));
        g_ptr_array_add(argv, g_strdup_printf("%d", event->y));

        /*
         * The C locale, always.  g_strdup_printf() with %f writes a
         * decimal comma in half the world's locales, and gdbus parses
         * the argument as a GVariant double -- so a scroll from a
         * German desktop arrived as a syntax error nobody could
         * reproduce.
         */
        {
            gchar dx[G_ASCII_DTOSTR_BUF_SIZE];
            gchar dy[G_ASCII_DTOSTR_BUF_SIZE];

            g_ascii_dtostr(dx, sizeof(dx), event->dx);
            g_ascii_dtostr(dy, sizeof(dy), event->dy);
            g_ptr_array_add(argv, g_strdup(dx));
            g_ptr_array_add(argv, g_strdup(dy));
        }
        break;
    }

    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

GStrv
clawt_screen_in_session_argv(const gchar * const *argv)
{
    g_autoptr(GString) script = NULL;
    GPtrArray *out;
    gsize i;

    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

    script = g_string_new(
        "export DBUS_SESSION_BUS_ADDRESS="
        "\"unix:path=/run/user/$(id -u)/bus\"\nexec");

    for (i = 0; argv[i] != NULL; i++) {
        g_autofree gchar *quoted = g_shell_quote(argv[i]);

        g_string_append_c(script, ' ');
        g_string_append(script, quoted);
    }

    out = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(out, g_strdup("sh"));
    g_ptr_array_add(out, g_strdup("-c"));
    g_ptr_array_add(out, g_strdup(script->str));
    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

/*
 * One token of GVariant text.
 *
 * gdbus prints type annotations -- `uint32 1280` -- so a scan for digit
 * runs finds the 32 in "uint32" before it finds the width.  Walking the
 * text properly is what stops that, and it is cheap: the tuple has five
 * members.
 */
typedef enum {
    TOKEN_END = 0,
    TOKEN_STRING,
    TOKEN_NUMBER
} TokenKind;

static TokenKind
next_token(const gchar **cursor, gchar **string_out, gint64 *number_out)
{
    const gchar *p = *cursor;

    while (*p != '\0') {
        if (*p == '\'' || *p == '"') {
            gchar quote = *p;
            GString *value = g_string_new(NULL);

            p++;

            while (*p != '\0' && *p != quote) {
                /*
                 * GVariant text escapes with a backslash, and a path
                 * with a quote in it is not impossible -- it is what a
                 * directory somebody named badly produces.
                 */
                if (*p == '\\' && p[1] != '\0')
                    p++;

                g_string_append_c(value, *p);
                p++;
            }

            if (*p == quote)
                p++;

            *cursor = p;
            *string_out = g_string_free(value, FALSE);

            return TOKEN_STRING;
        }

        if (g_ascii_isdigit(*p) ||
            (*p == '-' && g_ascii_isdigit(p[1]))) {
            gchar *end = NULL;
            gint64 value = g_ascii_strtoll(p, &end, 10);

            /*
             * A number immediately followed by a letter is the tail of a
             * type name (`uint32`), which only reaches here when the
             * annotation started with a digit -- it cannot, but leaving
             * the check out would make this depend on that.
             */
            *cursor = (end != NULL) ? end : p + 1;
            *number_out = value;

            return TOKEN_NUMBER;
        }

        if (g_ascii_isalpha(*p) || *p == '_') {
            /* A type annotation: consume it whole, digits and all. */
            while (*p != '\0' && (g_ascii_isalnum(*p) || *p == '_'))
                p++;

            continue;
        }

        p++;
    }

    *cursor = p;

    return TOKEN_END;
}

gboolean
clawt_screen_parse_gdbus_frame(const gchar           *text,
                               ClawtScreenFrameInfo  *out,
                               GError               **error)
{
    const gchar *cursor = text;
    gchar *strings[2] = { NULL, NULL };
    gint64 numbers[3] = { 0, 0, 0 };
    gsize string_count = 0;
    gsize number_count = 0;

    g_return_val_if_fail(out != NULL, FALSE);

    if (text == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the desktop said nothing at all");
        return FALSE;
    }

    while (TRUE) {
        gchar *string_value = NULL;
        gint64 number_value = 0;
        TokenKind kind = next_token(&cursor, &string_value, &number_value);

        if (kind == TOKEN_END)
            break;

        if (kind == TOKEN_STRING) {
            if (string_count < G_N_ELEMENTS(strings))
                strings[string_count] = string_value;
            else
                g_free(string_value);

            string_count++;
            continue;
        }

        if (number_count < G_N_ELEMENTS(numbers))
            numbers[number_count] = number_value;

        number_count++;
    }

    /*
     * Both strings and all three numbers, or this is not a frame tuple.
     *
     * Strict rather than forgiving, because the thing most likely to
     * arrive here instead is a gdbus error message -- and a lenient
     * parser would take the first quoted fragment of that as a path and
     * go looking for a file named after an exception.
     */
    if (string_count != 2 || number_count != 3) {
        g_free(strings[0]);
        g_free(strings[1]);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the desktop did not answer with a frame: %s", text);
        return FALSE;
    }

    clawt_screen_frame_info_clear(out);
    out->path = strings[0];
    out->hash = strings[1];
    out->width = (guint)numbers[0];
    out->height = (guint)numbers[1];
    out->stamp = numbers[2];

    return TRUE;
}

gchar *
clawt_screen_parse_gdbus_string(const gchar *text)
{
    const gchar *cursor = text;
    gchar *value = NULL;
    gint64 number = 0;

    if (text == NULL)
        return NULL;

    if (next_token(&cursor, &value, &number) != TOKEN_STRING)
        return NULL;

    return value;
}

GStrv
clawt_screen_gnome_record_start_argv(guint max_seconds, guint max_events)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    add_gdbus_prefix(argv, "StartRecording");
    g_ptr_array_add(argv, g_strdup_printf("%u", max_seconds));
    g_ptr_array_add(argv, g_strdup_printf("%u", max_events));
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

/*
 * A token is a string the extension chose, and it still gets quoted.
 *
 * Everything on this argv is re-parsed by a remote shell after
 * clawt_screen_in_session_argv() has wrapped it, so a value that reached
 * there unquoted would be shell syntax rather than an argument -- the
 * same reason every other argument in this file is quoted on the way in.
 */
static GStrv
record_token_argv(const gchar *method, const gchar *token)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    add_gdbus_prefix(argv, method);
    g_ptr_array_add(argv, g_strdup((token != NULL) ? token : ""));
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

GStrv
clawt_screen_gnome_record_drain_argv(const gchar *token)
{
    return record_token_argv("DrainRecording", token);
}

GStrv
clawt_screen_gnome_record_stop_argv(const gchar *token)
{
    return record_token_argv("StopRecording", token);
}

GStrv
clawt_screen_gnome_record_status_argv(void)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    add_gdbus_prefix(argv, "GetRecordingStatus");
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

gboolean
clawt_screen_parse_gdbus_events(const gchar  *text,
                                gchar       **events_out,
                                guint        *dropped_out,
                                GError      **error)
{
    const gchar *cursor = text;
    gchar *events = NULL;
    gint64 dropped = 0;
    gsize string_count = 0;
    gsize number_count = 0;

    g_return_val_if_fail(events_out != NULL, FALSE);

    *events_out = NULL;

    if (dropped_out != NULL)
        *dropped_out = 0;

    if (text == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the desktop said nothing at all");
        return FALSE;
    }

    while (TRUE) {
        gchar *string_value = NULL;
        gint64 number_value = 0;
        TokenKind kind = next_token(&cursor, &string_value, &number_value);

        if (kind == TOKEN_END)
            break;

        if (kind == TOKEN_STRING) {
            if (string_count == 0)
                events = string_value;
            else
                g_free(string_value);

            string_count++;
            continue;
        }

        if (number_count == 0)
            dropped = number_value;

        number_count++;
    }

    /*
     * Exactly one string and one number, or this is not a drain.
     *
     * Strict for the reason the frame parser is: what arrives here
     * instead is a gdbus error, and a lenient reader would take the
     * first quoted fragment of an exception message as a list of
     * events and hand it to a JSON parser a long way from here.
     */
    if (string_count != 1 || number_count != 1) {
        g_free(events);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the desktop did not answer with recorded events: %s",
                    text);
        return FALSE;
    }

    *events_out = events;

    if (dropped_out != NULL)
        *dropped_out = (dropped > 0) ? (guint)dropped : 0;

    return TRUE;
}

const gchar *
clawt_screen_gowl_input_tool(ClawtInputEvent *event, JsonNode **arguments)
{
    g_autoptr(JsonObject) object = NULL;
    const gchar *tool = NULL;

    g_return_val_if_fail(event != NULL, NULL);
    g_return_val_if_fail(arguments != NULL, NULL);

    *arguments = NULL;
    object = json_object_new();

    switch (event->kind) {
    case CLAWT_INPUT_KEY:
        if (event->text == NULL || *event->text == '\0')
            return NULL;

        tool = "send_key";
        json_object_set_string_member(object, "key", event->text);
        break;

    case CLAWT_INPUT_TEXT:
        if (event->text == NULL || *event->text == '\0')
            return NULL;

        tool = "send_text";
        json_object_set_string_member(object, "text", event->text);
        break;

    case CLAWT_INPUT_CLICK:
        /*
         * Two calls' worth of intent in one event, and gowl needs them
         * as two: send_mouse clicks wherever the pointer already is.
         * The move is issued first by the caller, which is why this one
         * carries the button alone.
         */
        tool = "send_mouse";
        json_object_set_string_member(
            object, "button",
            (event->button == 3) ? "right"
                                 : (event->button == 2) ? "middle" : "left");
        json_object_set_string_member(object, "action", "click");
        break;

    case CLAWT_INPUT_MOVE:
        tool = "send_mouse_move";
        json_object_set_int_member(object, "x", event->x);
        json_object_set_int_member(object, "y", event->y);
        break;

    case CLAWT_INPUT_SCROLL:
        tool = "send_scroll";
        json_object_set_string_member(
            object, "direction",
            (event->dy > 0) ? "down"
                            : (event->dy < 0) ? "up"
                                              : (event->dx > 0) ? "right"
                                                                : "left");
        break;
    }

    if (tool == NULL)
        return NULL;

    {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);

        json_node_set_object(node, object);
        *arguments = node;
    }

    return tool;
}

gboolean
clawt_screen_png_size(GBytes *bytes, guint *width, guint *height)
{
    static const guchar signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    const guchar *data;
    gsize length = 0;

    if (width != NULL)
        *width = 0;

    if (height != NULL)
        *height = 0;

    if (bytes == NULL)
        return FALSE;

    data = g_bytes_get_data(bytes, &length);

    /*
     * The signature, the length and type of the first chunk, and the
     * eight bytes of the IHDR that carry the size: 24 in all. Anything
     * shorter is not a PNG, and reading past it would be reading past
     * a buffer somebody else filled.
     */
    if (data == NULL || length < 24)
        return FALSE;

    if (memcmp(data, signature, sizeof(signature)) != 0)
        return FALSE;

    if (memcmp(data + 12, "IHDR", 4) != 0)
        return FALSE;

    if (width != NULL)
        *width = ((guint)data[16] << 24) | ((guint)data[17] << 16) |
                 ((guint)data[18] << 8) | (guint)data[19];

    if (height != NULL)
        *height = ((guint)data[20] << 24) | ((guint)data[21] << 16) |
                  ((guint)data[22] << 8) | (guint)data[23];

    return TRUE;
}

gchar *
clawt_screen_hash_bytes(GBytes *bytes)
{
    gconstpointer data;
    gsize length = 0;

    if (bytes == NULL)
        return NULL;

    data = g_bytes_get_data(bytes, &length);

    if (data == NULL || length == 0)
        return NULL;

    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
}
