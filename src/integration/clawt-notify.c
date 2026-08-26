/*
 * clawt-notify.c - How the fleet reaches the person running it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-notify.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <string.h>

#define USER_AGENT \
    "clawtilla/" G_STRINGIFY(CLAWT_VERSION_MAJOR) "." \
    G_STRINGIFY(CLAWT_VERSION_MINOR) "." G_STRINGIFY(CLAWT_VERSION_MICRO)

/*
 * A lock screen shows about this much and a phone banner rather less.
 * Anything past it is in the body, which every backend here keeps.
 */
#define SUMMARY_CHARS (140)

/* ── The notification ────────────────────────────────────────────── */

ClawtNotification *
clawt_notification_new(ClawtNotifyEvents  events,
                       const gchar       *agent_id,
                       const gchar       *agent_name,
                       const gchar       *title,
                       const gchar       *body)
{
    ClawtNotification *self = g_new0(ClawtNotification, 1);

    self->events = events;
    self->agent_id = g_strdup(agent_id);
    self->agent_name = g_strdup(agent_name);
    self->title = g_strdup(title);
    self->body = g_strdup(body);

    return self;
}

ClawtNotification *
clawt_notification_copy(ClawtNotification *self)
{
    ClawtNotification *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_notification_new(self->events, self->agent_id,
                                  self->agent_name, self->title, self->body);
    copy->room_id = g_strdup(self->room_id);

    return copy;
}

void
clawt_notification_free(ClawtNotification *self)
{
    if (self == NULL)
        return;

    g_free(self->agent_id);
    g_free(self->agent_name);
    g_free(self->title);
    g_free(self->body);
    g_free(self->room_id);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtNotification, clawt_notification,
                    clawt_notification_copy, clawt_notification_free)

gchar *
clawt_notify_summarize(const gchar *text, gsize max_chars)
{
    g_autofree gchar *flattened = NULL;
    GString *out;
    const gchar *p;
    gboolean space = FALSE;
    gsize kept = 0;

    if (text == NULL)
        return g_strdup("");

    if (max_chars == 0)
        max_chars = SUMMARY_CHARS;

    /*
     * Code fences go first and whole.  A model's answer often opens with
     * one, and the first hundred characters of a shell script tell a
     * person nothing about why they are being interrupted.
     */
    {
        GString *stripped = g_string_new(NULL);
        const gchar *cursor = text;

        while (*cursor != '\0') {
            const gchar *fence = strstr(cursor, "```");
            const gchar *close;

            if (fence == NULL) {
                g_string_append(stripped, cursor);
                break;
            }

            g_string_append_len(stripped, cursor, fence - cursor);
            close = strstr(fence + 3, "```");

            if (close == NULL)
                break;

            g_string_append_c(stripped, ' ');
            cursor = close + 3;
        }

        flattened = g_string_free(stripped, FALSE);
    }

    out = g_string_new(NULL);

    for (p = flattened; *p != '\0' && kept < max_chars; ) {
        gunichar c = g_utf8_get_char(p);

        if (g_unichar_isspace(c)) {
            space = TRUE;
            p = g_utf8_next_char(p);
            continue;
        }

        if (space && out->len > 0) {
            g_string_append_c(out, ' ');
            kept++;
        }

        space = FALSE;

        {
            const gchar *next = g_utf8_next_char(p);

            g_string_append_len(out, p, next - p);
            kept++;
            p = next;
        }
    }

    /* Only when something was actually left out. */
    if (*p != '\0')
        g_string_append(out, "\342\200\246");

    return g_string_free(out, FALSE);
}

ClawtNotifyEvents
clawt_notify_events_from_strv(const gchar *const *names, GError **error)
{
    ClawtNotifyEvents events = CLAWT_NOTIFY_EVENTS_NONE;
    guint i;

    for (i = 0; names != NULL && names[i] != NULL; i++) {
        guint value = 0;

        if (*names[i] == '\0')
            continue;

        if (!clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, names[i],
                                   &value)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "'%s' is not something to be notified about: use "
                        "question, done, error or routine", names[i]);
            return CLAWT_NOTIFY_EVENTS_NONE;
        }

        events |= (ClawtNotifyEvents)value;
    }

    return events;
}

/* ── Quiet hours ─────────────────────────────────────────────────── */

static gboolean
parse_time_of_day(const gchar *text, gint *out_minutes)
{
    gchar *end = NULL;
    gint64 hours;
    gint64 minutes;

    hours = g_ascii_strtoll(text, &end, 10);

    if (end == text || *end != ':' || hours < 0 || hours > 23)
        return FALSE;

    text = end + 1;
    minutes = g_ascii_strtoll(text, &end, 10);

    if (end == text || minutes < 0 || minutes > 59)
        return FALSE;

    while (*end == ' ')
        end++;

    if (*end != '\0')
        return FALSE;

    *out_minutes = (gint)(hours * 60 + minutes);

    return TRUE;
}

gboolean
clawt_notify_parse_quiet_hours(const gchar *text, gint *out_start,
                               gint *out_end)
{
    g_auto(GStrv) parts = NULL;

    g_return_val_if_fail(out_start != NULL, FALSE);
    g_return_val_if_fail(out_end != NULL, FALSE);

    if (text == NULL || *text == '\0')
        return FALSE;

    parts = g_strsplit(text, "-", 2);

    if (parts[0] == NULL || parts[1] == NULL)
        return FALSE;

    return parse_time_of_day(g_strstrip(parts[0]), out_start) &&
           parse_time_of_day(g_strstrip(parts[1]), out_end);
}

gboolean
clawt_notify_in_quiet_hours(gint start, gint end, gint minute_of_day)
{
    /*
     * A range that does not wrap is the simple one, and the rarer one:
     * people sleep across midnight, so 23:00-07:00 is the ordinary
     * spelling and it is the case that has to be right.
     */
    if (start <= end)
        return minute_of_day >= start && minute_of_day < end;

    return minute_of_day >= start || minute_of_day < end;
}

/* ── Priorities ──────────────────────────────────────────────────── */

const gchar *
clawt_notify_priority_for_ntfy(const gchar *priority)
{
    if (g_strcmp0(priority, "low") == 0)
        return "low";

    if (g_strcmp0(priority, "high") == 0)
        return "high";

    if (g_strcmp0(priority, "urgent") == 0)
        return "urgent";

    return "default";
}

gint
clawt_notify_priority_for_gotify(const gchar *priority)
{
    if (g_strcmp0(priority, "low") == 0)
        return 1;

    if (g_strcmp0(priority, "high") == 0)
        return 7;

    /*
     * gotify treats 8 and above as demanding attention -- that is what
     * turns off the auto-dismiss on its Android client.  Urgent has to
     * clear it or it means nothing.
     */
    if (g_strcmp0(priority, "urgent") == 0)
        return 9;

    return 5;
}

guchar
clawt_notify_priority_for_desktop(const gchar *priority)
{
    if (g_strcmp0(priority, "low") == 0)
        return 0;

    if (g_strcmp0(priority, "high") == 0 ||
        g_strcmp0(priority, "urgent") == 0)
        return 2;

    return 1;
}

/* ── The command backend ─────────────────────────────────────────── */

static gchar *
substitute(const gchar *text, ClawtNotification *notification,
           gboolean *used)
{
    GString *out = g_string_new(NULL);
    const gchar *p = text;

    while (*p != '\0') {
        const gchar *open = strstr(p, "{{");
        const gchar *close;
        g_autofree gchar *key = NULL;

        if (open == NULL) {
            g_string_append(out, p);
            break;
        }

        close = strstr(open, "}}");

        if (close == NULL) {
            g_string_append(out, p);
            break;
        }

        g_string_append_len(out, p, open - p);
        key = g_strndup(open + 2, close - open - 2);

        if (g_strcmp0(key, "title") == 0) {
            g_string_append(out, notification->title != NULL
                                 ? notification->title : "");
            *used = TRUE;
        } else if (g_strcmp0(key, "body") == 0) {
            g_string_append(out, notification->body != NULL
                                 ? notification->body : "");
            *used = TRUE;
        } else if (g_strcmp0(key, "agent") == 0) {
            g_string_append(out, notification->agent_id != NULL
                                 ? notification->agent_id : "");
            *used = TRUE;
        } else {
            /* Left alone: it is not ours, so it is somebody's literal. */
            g_string_append_len(out, open, (close + 2) - open);
        }

        p = close + 2;
    }

    return g_string_free(out, FALSE);
}

GStrv
clawt_notify_expand_argv(const gchar        *command,
                         const gchar *const *args,
                         ClawtNotification  *notification)
{
    g_autoptr(GPtrArray) argv = NULL;
    gboolean used = FALSE;
    guint i;

    g_return_val_if_fail(command != NULL, NULL);
    g_return_val_if_fail(notification != NULL, NULL);

    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(command));

    for (i = 0; args != NULL && args[i] != NULL; i++)
        g_ptr_array_add(argv, substitute(args[i], notification, &used));

    /*
     * A program with no placeholders anywhere gets the text appended,
     * which is what makes `command: receipt-print` work with nothing
     * else written down.
     */
    if (!used) {
        g_ptr_array_add(argv, g_strdup(notification->title != NULL
                                       ? notification->title : ""));

        if (notification->body != NULL && *notification->body != '\0')
            g_ptr_array_add(argv, g_strdup(notification->body));
    }

    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&argv), FALSE);
}

/* ── Sending ─────────────────────────────────────────────────────── */

static SoupSession *
notify_session(void)
{
    static SoupSession *session = NULL;

    if (session == NULL)
        session = soup_session_new_with_options("user-agent", USER_AGENT,
                                                "timeout", 15, NULL);

    return session;
}

static void
on_http_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    SoupMessage *message = g_task_get_task_data(task);
    guint status;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    status = soup_message_get_status(message);

    if (status < 200 || status >= 300) {
        gsize size = 0;
        const gchar *data = g_bytes_get_data(body, &size);
        g_autofree gchar *text = g_strndup(data != NULL ? data : "",
                                           MIN(size, 200));

        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                                "the server answered %u: %s", status, text);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

static void
send_http(GTask *task, SoupMessage *message)
{
    g_task_set_task_data(task, message, g_object_unref);

    soup_session_send_and_read_async(notify_session(), message,
                                     G_PRIORITY_DEFAULT,
                                     g_task_get_cancellable(task),
                                     on_http_sent, task);
}

static void
send_ntfy(GTask *task, ClawtIntegrationBinding *binding,
          ClawtNotification *notification, const gchar *token)
{
    const gchar *url = clawt_integration_binding_get_string(binding, "url");
    const gchar *priority =
        clawt_integration_binding_get_string(binding, "priority");
    SoupMessage *message;
    SoupMessageHeaders *headers;
    g_autoptr(GBytes) bytes = NULL;
    const gchar *body;

    if (url == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "ntfy needs a url, such as "
                                "https://ntfy.sh/your-topic");
        g_object_unref(task);
        return;
    }

    message = soup_message_new("POST", url);

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "%s is not a URL this can post to", url);
        g_object_unref(task);
        return;
    }

    headers = soup_message_get_request_headers(message);

    /*
     * ntfy takes the title out of a header and the message out of the
     * body, so a body that is empty would arrive as a notification with
     * a title and nothing in it -- which several clients render as
     * nothing at all.
     */
    if (notification->title != NULL)
        soup_message_headers_replace(headers, "Title", notification->title);

    soup_message_headers_replace(headers, "Priority",
                                 clawt_notify_priority_for_ntfy(priority));
    soup_message_headers_replace(headers, "Tags", "robot");

    if (token != NULL && *token != '\0') {
        g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);

        soup_message_headers_replace(headers, "Authorization", bearer);
        memset(bearer, 0, strlen(bearer));
    }

    body = (notification->body != NULL && *notification->body != '\0')
        ? notification->body
        : (notification->title != NULL ? notification->title : " ");

    bytes = g_bytes_new(body, strlen(body));
    soup_message_set_request_body_from_bytes(message, "text/plain", bytes);

    send_http(task, message);
}

static void
send_gotify(GTask *task, ClawtIntegrationBinding *binding,
            ClawtNotification *notification, const gchar *token)
{
    const gchar *url = clawt_integration_binding_get_string(binding, "url");
    const gchar *priority =
        clawt_integration_binding_get_string(binding, "priority");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *payload = NULL;
    g_autofree gchar *endpoint = NULL;
    g_autoptr(GBytes) bytes = NULL;
    SoupMessage *message;

    if (url == NULL || token == NULL || *token == '\0') {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "gotify needs a url and an application "
                                "token");
        g_object_unref(task);
        return;
    }

    /*
     * The token goes in a header rather than the query string.  Gotify
     * accepts either, and a URL is the one place a credential reliably
     * ends up in somebody's proxy log.
     */
    {
        gsize length = strlen(url);

        while (length > 0 && url[length - 1] == '/')
            length--;

        endpoint = g_strdup_printf("%.*s/message", (int)length, url);
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "title");
    json_builder_add_string_value(builder, notification->title != NULL
                                           ? notification->title : "");
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, notification->body != NULL
                                           ? notification->body : "");
    json_builder_set_member_name(builder, "priority");
    json_builder_add_int_value(builder,
                               clawt_notify_priority_for_gotify(priority));
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    payload = json_generator_to_data(generator, NULL);

    message = soup_message_new("POST", endpoint);

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "%s is not a URL this can post to", endpoint);
        g_object_unref(task);
        return;
    }

    soup_message_headers_replace(soup_message_get_request_headers(message),
                                 "X-Gotify-Key", token);

    bytes = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(message, "application/json",
                                             bytes);

    send_http(task, message);
}

static void
send_matrix(GTask *task, ClawtIntegrationBinding *binding,
            ClawtNotification *notification, const gchar *token)
{
    const gchar *homeserver =
        clawt_integration_binding_get_string(binding, "homeserver");
    const gchar *room = clawt_integration_binding_get_string(binding, "room");
    g_autofree gchar *base = NULL;
    g_autofree gchar *escaped = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *payload = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autoptr(GBytes) bytes = NULL;
    SoupMessage *message;

    if (homeserver == NULL || room == NULL || token == NULL ||
        *token == '\0') {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "a Matrix notifier needs a homeserver, a "
                                "room and a token");
        g_object_unref(task);
        return;
    }

    base = clawt_matrix_base_url(homeserver);

    if (base == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "'%s' is not a homeserver address",
                                homeserver);
        g_object_unref(task);
        return;
    }

    escaped = g_uri_escape_string(room, NULL, FALSE);

    /*
     * The transaction id is derived from the notification rather than
     * random, so a retry of the same notification is deduplicated by the
     * homeserver instead of arriving twice.
     */
    {
        g_autofree gchar *seed = g_strdup_printf(
            "%s\n%s\n%s", notification->agent_id != NULL
                              ? notification->agent_id : "",
            notification->title != NULL ? notification->title : "",
            notification->body != NULL ? notification->body : "");
        g_autofree gchar *digest =
            g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);

        url = g_strdup_printf(
            "%s/_matrix/client/v3/rooms/%s/send/m.room.message/clawt%.16s",
            base, escaped, digest);
    }

    text = (notification->body != NULL && *notification->body != '\0')
        ? g_strdup_printf("%s\n\n%s", notification->title, notification->body)
        : g_strdup(notification->title != NULL ? notification->title : "");

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "msgtype");
    json_builder_add_string_value(builder, "m.text");
    json_builder_set_member_name(builder, "body");
    json_builder_add_string_value(builder, text);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    payload = json_generator_to_data(generator, NULL);

    message = soup_message_new("PUT", url);

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "that room id does not make a usable URL");
        g_object_unref(task);
        return;
    }

    {
        g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);

        soup_message_headers_replace(soup_message_get_request_headers(message),
                                     "Authorization", bearer);
        memset(bearer, 0, strlen(bearer));
    }

    bytes = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(message, "application/json",
                                             bytes);

    send_http(task, message);
}

static void
on_desktop_notified(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GVariant) reply = NULL;
    g_autoptr(GError) error = NULL;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                          &error);

    if (reply == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_boolean(task, TRUE);
}

/*
 * Spoken to over D-Bus rather than by running notify-send.
 *
 * notify-send is a thin wrapper around this same call, so spawning it
 * buys a process and a PATH dependency and nothing else -- and the
 * daemon may be running as a systemd user service where a program on
 * PATH is a less certain thing than the session bus it is already on.
 */
static void
send_desktop(GTask *task, ClawtIntegrationBinding *binding,
             ClawtNotification *notification)
{
    g_autoptr(GDBusConnection) bus = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *priority =
        clawt_integration_binding_get_string(binding, "priority");
    GVariantBuilder hints;
    GVariantBuilder actions;

    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

    if (bus == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency",
                          g_variant_new_byte(
                              clawt_notify_priority_for_desktop(priority)));
    g_variant_builder_add(&hints, "{sv}", "category",
                          g_variant_new_string("im.received"));

    g_dbus_connection_call(bus,
                           "org.freedesktop.Notifications",
                           "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications",
                           "Notify",
                           g_variant_new("(susssasa{sv}i)",
                                         "clawtilla",
                                         0u,
                                         "dialog-information",
                                         notification->title != NULL
                                             ? notification->title : "",
                                         notification->body != NULL
                                             ? notification->body : "",
                                         &actions,
                                         &hints,
                                         -1),
                           G_VARIANT_TYPE("(u)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           g_task_get_cancellable(task),
                           on_desktop_notified, task);
}

static void
on_command_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GError) error = NULL;

    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result,
                                        &error)) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_boolean(task, TRUE);
}

static void
send_command(GTask *task, ClawtIntegrationBinding *binding,
             ClawtNotification *notification)
{
    const gchar *command =
        clawt_integration_binding_get_string(binding, "command");
    g_auto(GStrv) args = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;

    if (command == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "a command notifier needs a command");
        g_object_unref(task);
        return;
    }

    args = clawt_integration_binding_get_string_list(binding, "args");
    argv = clawt_notify_expand_argv(command, (const gchar *const *)args,
                                    notification);

    /*
     * Output is discarded rather than captured.  This is a fire-and-
     * forget notifier: a receipt printer's chatter is not something the
     * daemon has any use for, and reading it would mean keeping pipes
     * open for a process nobody is waiting on.
     */
    process = g_subprocess_newv((const gchar *const *)argv,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error);

    if (process == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    g_subprocess_wait_check_async(process, g_task_get_cancellable(task),
                                  on_command_done, task);
}

void
clawt_notify_send_async(ClawtIntegrationBinding *binding,
                        ClawtNotification       *notification,
                        const gchar             *token,
                        GCancellable            *cancellable,
                        GAsyncReadyCallback      callback,
                        gpointer                 user_data)
{
    GTask *task;
    const gchar *nick;
    gint backend = CLAWT_NOTIFY_BACKEND_DESKTOP;

    g_return_if_fail(binding != NULL);
    g_return_if_fail(notification != NULL);

    task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, clawt_notify_send_async);

    nick = clawt_integration_binding_get_string(binding, "backend");

    if (nick != NULL &&
        !clawt_enum_from_nick(CLAWT_TYPE_NOTIFY_BACKEND, nick, &backend)) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a notify backend: use desktop, "
                                "ntfy, gotify, matrix or command", nick);
        g_object_unref(task);
        return;
    }

    switch ((ClawtNotifyBackend)backend) {
    case CLAWT_NOTIFY_BACKEND_NTFY:
        send_ntfy(task, binding, notification, token);
        return;

    case CLAWT_NOTIFY_BACKEND_GOTIFY:
        send_gotify(task, binding, notification, token);
        return;

    case CLAWT_NOTIFY_BACKEND_MATRIX:
        send_matrix(task, binding, notification, token);
        return;

    case CLAWT_NOTIFY_BACKEND_COMMAND:
        send_command(task, binding, notification);
        return;

    case CLAWT_NOTIFY_BACKEND_DESKTOP:
    default:
        send_desktop(task, binding, notification);
        return;
    }
}

gboolean
clawt_notify_send_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

/* ── The notifier ────────────────────────────────────────────────── */

struct _ClawtNotifier {
    GObject parent_instance;

    ClawtConfig *config;      /* owned; replaced on reload */

    /*
     * Credentials, resolved once when the configuration loads.
     *
     * A `{command: "pass show ..."}` reference against a locked password
     * manager blocks until its timeout, and this runs on the daemon's
     * main context -- so doing it per notification would make the
     * notifier the slowest thing in the daemon, on the path that exists
     * to be fast.
     */
    GHashTable *tokens;       /* instance name -> gchar*, owned */
};

G_DEFINE_FINAL_TYPE(ClawtNotifier, clawt_notifier, G_TYPE_OBJECT)

static void
wipe_token(gpointer data)
{
    gchar *token = data;

    if (token == NULL)
        return;

    memset(token, 0, strlen(token));
    g_free(token);
}

static void
clawt_notifier_finalize(GObject *object)
{
    ClawtNotifier *self = CLAWT_NOTIFIER(object);

    g_clear_object(&self->config);
    g_clear_pointer(&self->tokens, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_notifier_parent_class)->finalize(object);
}

static void
clawt_notifier_class_init(ClawtNotifierClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_notifier_finalize;
}

static void
clawt_notifier_init(ClawtNotifier *self)
{
    self->tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          wipe_token);
}

ClawtNotifier *
clawt_notifier_new(ClawtConfig *config)
{
    ClawtNotifier *self = g_object_new(CLAWT_TYPE_NOTIFIER, NULL);

    clawt_notifier_reload(self, config);

    return self;
}

void
clawt_notifier_reload(ClawtNotifier *self, ClawtConfig *config)
{
    g_autofree gchar *secrets_dir = NULL;
    GPtrArray *instances;
    guint timeout;
    guint i;

    g_return_if_fail(CLAWT_IS_NOTIFIER(self));
    g_return_if_fail(CLAWT_IS_CONFIG(config));

    g_set_object(&self->config, config);
    g_hash_table_remove_all(self->tokens);

    secrets_dir = clawt_config_get_path_value(config, "secrets.dir");
    timeout = (guint)clawt_config_get_int(config,
                                          "secrets.command_timeout_seconds");
    instances = clawt_config_get_integrations(config);

    for (i = 0; instances != NULL && i < instances->len; i++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(instances, i);
        const ClawtIntegrationInfo *info;
        g_autoptr(ClawtSecretRef) ref = NULL;
        g_autoptr(GError) error = NULL;
        gchar *token;

        info = clawt_integration_find(
            clawt_integration_config_get_type_id(instance));

        if (info == NULL || info->kind != CLAWT_INTEGRATION_KIND_NOTIFY)
            continue;

        ref = clawt_integration_config_get_secret(instance, NULL, "token");

        if (ref == NULL)
            continue;

        token = clawt_secret_ref_resolve(ref, secrets_dir, timeout, &error);

        if (token == NULL) {
            g_autofree gchar *described = clawt_secret_ref_describe(ref);

            /*
             * One notifier disabled, not a failed reload.  Being unable
             * to buzz a phone is not a reason to stop a fleet, and the
             * warning names the reference rather than the value.
             */
            g_warning("notify '%s': could not resolve %s: %s; that notifier "
                      "is off until it can be",
                      clawt_integration_config_get_name(instance), described,
                      error != NULL ? error->message : "unknown reason");
            continue;
        }

        g_hash_table_insert(self->tokens,
                            g_strdup(clawt_integration_config_get_name(instance)),
                            token);
    }
}

static void
on_notification_sent(GObject *source, GAsyncResult *result,
                     gpointer user_data)
{
    g_autofree gchar *name = user_data;
    g_autoptr(GError) error = NULL;

    (void)source;

    /*
     * A failure is a warning and nothing more.  Whatever the
     * notification was about has already happened; failing louder than
     * this would mean the fleet's behaviour depended on whether a phone
     * was reachable.
     */
    if (!clawt_notify_send_finish(result, &error))
        g_warning("notify '%s': %s", name,
                  error != NULL ? error->message : "it was not accepted");
}

/*
 * Whether this instance wants to hear about this, right now.
 */
static gboolean
wants(ClawtIntegrationBinding *binding, ClawtNotifyEvents events,
      gboolean ignore_filters)
{
    g_auto(GStrv) names = NULL;
    g_autoptr(GError) error = NULL;
    ClawtNotifyEvents wanted;
    const gchar *quiet;
    gint start = 0;
    gint end = 0;

    if (ignore_filters)
        return TRUE;

    names = clawt_integration_binding_get_string_list(binding, "events");
    wanted = clawt_notify_events_from_strv((const gchar *const *)names,
                                           &error);

    if (error != NULL) {
        g_warning("notify '%s': %s",
                  clawt_integration_binding_get_name(binding),
                  error->message);
        return FALSE;
    }

    if ((wanted & events) == 0)
        return FALSE;

    quiet = clawt_integration_binding_get_string(binding, "quiet_hours");

    if (quiet == NULL || *quiet == '\0')
        return TRUE;

    if (!clawt_notify_parse_quiet_hours(quiet, &start, &end)) {
        /*
         * A range nobody can parse must not silence anything.  The
         * failure people would never find is a typo in a quiet-hours
         * string turning a notifier off for good.
         */
        g_warning("notify '%s': '%s' is not a time range such as "
                  "23:00-07:00; ignoring it",
                  clawt_integration_binding_get_name(binding), quiet);
        return TRUE;
    }

    {
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        gint minute_of_day = g_date_time_get_hour(now) * 60 +
                             g_date_time_get_minute(now);

        return !clawt_notify_in_quiet_hours(start, end, minute_of_day);
    }
}

/*
 * The line a lock screen shows.
 *
 * The agent's name leads, because the first question a person has when
 * their phone buzzes is which of them wants something.
 */
static gchar *
headline(ClawtIntegrationBinding *binding, ClawtNotification *notification)
{
    const gchar *override =
        clawt_integration_binding_get_string(binding, "title");

    if (override != NULL && *override != '\0')
        return g_strdup_printf("%s: %s", override,
                               notification->title != NULL
                                   ? notification->title : "");

    if (notification->agent_name != NULL &&
        *notification->agent_name != '\0')
        return g_strdup_printf("%s: %s", notification->agent_name,
                               notification->title != NULL
                                   ? notification->title : "");

    return g_strdup(notification->title != NULL ? notification->title : "");
}

static void
deliver(ClawtNotifier *self, ClawtIntegrationBinding *binding,
        ClawtNotification *notification)
{
    g_autoptr(ClawtNotification) shaped =
        clawt_notification_copy(notification);
    const gchar *name = clawt_integration_binding_get_name(binding);

    g_free(shaped->title);
    shaped->title = headline(binding, notification);

    clawt_notify_send_async(binding, shaped,
                            g_hash_table_lookup(self->tokens, name),
                            NULL, on_notification_sent, g_strdup(name));
}

/*
 * Every notify binding for one agent, or for none in particular.
 *
 * An event with no agent -- a routine that failed before it started, a
 * daemon-level problem -- still has to reach somebody, so it goes to
 * every instance whose scope is `all`.
 */
static GPtrArray *
notifiers_for(ClawtNotifier *self, const gchar *agent_id)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_integration_binding_unref);
    ClawtAgentConfig *agent = (agent_id != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;

    if (agent != NULL) {
        g_autoptr(GPtrArray) bindings =
            clawt_integration_resolve_for_agent(self->config, agent);
        guint i;

        for (i = 0; i < bindings->len; i++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

            if (clawt_integration_binding_get_info(binding)->kind ==
                CLAWT_INTEGRATION_KIND_NOTIFY)
                g_ptr_array_add(out,
                                clawt_integration_binding_ref(binding));
        }

        return out;
    }

    {
        GPtrArray *agents = clawt_config_get_agents(self->config);
        guint i;

        /*
         * With no agent to resolve against, borrow the first one there
         * is: a `scope: all` instance covers it, and that is exactly the
         * set we want.  A fleet with no agents at all has nobody to
         * notify about anything.
         */
        if (agents == NULL || agents->len == 0)
            return out;

        {
            ClawtAgentConfig *any = g_ptr_array_index(agents, 0);
            g_autoptr(GPtrArray) bindings =
                clawt_integration_resolve_for_agent(self->config, any);

            for (i = 0; i < bindings->len; i++) {
                ClawtIntegrationBinding *binding =
                    g_ptr_array_index(bindings, i);
                ClawtIntegrationConfig *instance;

                if (clawt_integration_binding_get_info(binding)->kind !=
                    CLAWT_INTEGRATION_KIND_NOTIFY)
                    continue;

                instance = clawt_config_get_integration(
                    self->config,
                    clawt_integration_binding_get_name(binding));

                if (instance == NULL ||
                    clawt_integration_config_get_scope(instance) !=
                        CLAWT_SCOPE_ALL)
                    continue;

                g_ptr_array_add(out,
                                clawt_integration_binding_ref(binding));
            }
        }
    }

    return out;
}

void
clawt_notifier_notify(ClawtNotifier *self, ClawtNotification *notification)
{
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_NOTIFIER(self));
    g_return_if_fail(notification != NULL);

    if (self->config == NULL)
        return;

    bindings = notifiers_for(self, notification->agent_id);

    for (i = 0; i < bindings->len; i++) {
        ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

        if (!wants(binding, notification->events, FALSE))
            continue;

        deliver(self, binding, notification);
    }
}

/* ── Testing one ─────────────────────────────────────────────────── */

typedef struct {
    ClawtNotifier *notifier;    /* unowned; the task holds it as its source */
    gchar         *name;
} TestSend;

static void
test_send_free(TestSend *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self);
}

static void
on_test_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GError) error = NULL;

    (void)source;

    if (!clawt_notify_send_finish(result, &error)) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_boolean(task, TRUE);
}

void
clawt_notifier_test_async(ClawtNotifier       *self,
                          const gchar         *name,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    GTask *task;
    ClawtIntegrationConfig *instance;
    const ClawtIntegrationInfo *info;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtNotification) notification = NULL;
    g_autoptr(GPtrArray) candidates = NULL;
    TestSend *test;
    guint i;

    g_return_if_fail(CLAWT_IS_NOTIFIER(self));

    task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, clawt_notifier_test_async);

    instance = (name != NULL && self->config != NULL)
        ? clawt_config_get_integration(self->config, name) : NULL;

    if (instance == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                                "there is no integration called '%s'",
                                name != NULL ? name : "");
        g_object_unref(task);
        return;
    }

    info = clawt_integration_find(
        clawt_integration_config_get_type_id(instance));

    if (info == NULL || info->kind != CLAWT_INTEGRATION_KIND_NOTIFY) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                "'%s' is not a notifier", name);
        g_object_unref(task);
        return;
    }

    /*
     * A binding is needed to read the settings through, and a binding
     * needs an agent.  Any agent it covers will do -- the values being
     * read are the instance's own -- so the first one in scope is used,
     * and a notifier covering nobody yet falls back to the instance's
     * own values with no agent at all.
     */
    {
        GPtrArray *agents = clawt_config_get_agents(self->config);

        candidates = g_ptr_array_new();

        for (i = 0; agents != NULL && i < agents->len; i++) {
            ClawtAgentConfig *agent = g_ptr_array_index(agents, i);

            if (clawt_integration_config_covers(
                    instance, clawt_agent_config_get_id(agent)))
                g_ptr_array_add(candidates, agent);
        }
    }

    binding = clawt_integration_binding_for_instance(
        instance, info,
        candidates->len > 0
            ? clawt_agent_config_get_id(g_ptr_array_index(candidates, 0))
            : NULL);

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, NULL, NULL,
        "clawtilla is working",
        "If you are reading this, this notifier can reach you.");

    test = g_new0(TestSend, 1);
    test->notifier = self;
    test->name = g_strdup(name);
    g_task_set_task_data(task, test, (GDestroyNotify)test_send_free);

    clawt_notify_send_async(binding, notification,
                            g_hash_table_lookup(self->tokens, name),
                            cancellable, on_test_sent, task);
}

gboolean
clawt_notifier_test_finish(ClawtNotifier *self, GAsyncResult *result,
                           GError **error)
{
    g_return_val_if_fail(CLAWT_IS_NOTIFIER(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}
