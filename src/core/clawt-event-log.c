/*
 * clawt-event-log.c - The durable record of what happened
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-event-log.h"

#include <glib/gstdio.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

struct _ClawtEventLog {
    GObject parent_instance;

    gchar         *dir;
    gint           retention_days;
    ClawtEventBus *bus;      /* unowned */
    gulong         handler;
    gchar         *open_day; /* which file the handle below belongs to */
    FILE          *handle;
};

G_DEFINE_FINAL_TYPE(ClawtEventLog, clawt_event_log, G_TYPE_OBJECT)

ClawtEventLog *
clawt_event_log_new(const gchar *dir, gint retention_days)
{
    ClawtEventLog *self = g_object_new(CLAWT_TYPE_EVENT_LOG, NULL);

    self->dir = clawt_expand_path(dir);
    self->retention_days = retention_days;

    return self;
}

/*
 * One file per day, named by UTC date.
 *
 * Per-day rather than per-room: a room's history is already in its
 * transcript, and what the log is actually for is answering "what
 * happened around the time this went wrong", which is a question about
 * time and not about rooms.
 */
static gchar *
day_string(gint64 timestamp_us)
{
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(
        timestamp_us / G_USEC_PER_SEC);

    if (when == NULL)
        return g_strdup("unknown");

    return g_date_time_format(when, "%Y-%m-%d");
}

static FILE *
open_for(ClawtEventLog *self, gint64 timestamp_us, GError **error)
{
    g_autofree gchar *day = day_string(timestamp_us);
    g_autofree gchar *path = NULL;
    gint fd;

    if (self->handle != NULL && g_strcmp0(self->open_day, day) == 0)
        return self->handle;

    if (self->handle != NULL) {
        fclose(self->handle);
        self->handle = NULL;
    }

    if (!clawt_ensure_dir(self->dir, 0700, error))
        return NULL;

    {
        g_autofree gchar *filename = g_strdup_printf("%s.ndjson", day);

        path = g_build_filename(self->dir, filename, NULL);
    }

    /*
     * Opened with an explicit 0600 rather than through fopen, whose mode
     * is at the mercy of umask.  The log holds message bodies.
     */
    fd = g_open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_WRITE,
                    "could not open %s: %s", path, g_strerror(errno));
        return NULL;
    }

    self->handle = fdopen(fd, "a");
    if (self->handle == NULL) {
        close(fd);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_WRITE,
                    "could not open %s: %s", path, g_strerror(errno));
        return NULL;
    }

    g_free(self->open_day);
    self->open_day = g_strdup(day);

    return self->handle;
}

gboolean
clawt_event_log_append(ClawtEventLog *self, ClawtEvent *event, GError **error)
{
    g_autoptr(JsonNode) node = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autofree gchar *line = NULL;
    FILE *handle;

    g_return_val_if_fail(CLAWT_IS_EVENT_LOG(self), FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    handle = open_for(self, clawt_event_get_timestamp(event), error);
    if (handle == NULL)
        return FALSE;

    node = clawt_event_to_json(event);
    generator = json_generator_new();
    json_generator_set_root(generator, node);
    line = json_generator_to_data(generator, NULL);

    if (fprintf(handle, "%s\n", line) < 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_WRITE,
                    "could not write to the event log: %s",
                    g_strerror(errno));
        return FALSE;
    }

    /*
     * Flushed per event.  The log is read after a crash more often than
     * during ordinary running, and a buffered tail lost at exactly that
     * moment is the part somebody needed.
     */
    fflush(handle);

    return TRUE;
}

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtEventLog *self = user_data;
    g_autoptr(GError) error = NULL;

    (void)bus;

    if (!clawt_event_log_append(self, event, &error)) {
        /*
         * Warned once per failure rather than raised: losing the log is
         * bad, and stopping the daemon because it cannot write a log line
         * is worse.
         */
        g_warning("event log: %s", error->message);
    }
}

void
clawt_event_log_attach(ClawtEventLog *self, ClawtEventBus *bus)
{
    g_return_if_fail(CLAWT_IS_EVENT_LOG(self));
    g_return_if_fail(CLAWT_IS_EVENT_BUS(bus));

    if (self->bus != NULL && self->handler != 0)
        g_signal_handler_disconnect(self->bus, self->handler);

    self->bus = bus;
    self->handler = g_signal_connect(bus, "event",
                                     G_CALLBACK(on_bus_event), self);
}

static ClawtEvent *
event_from_json(JsonNode *node)
{
    JsonObject *object;
    ClawtEvent *event;

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    object = json_node_get_object(node);

    if (!json_object_has_member(object, "kind"))
        return NULL;

    event = clawt_event_new(
        json_object_get_string_member(object, "kind"),
        json_object_has_member(object, "subject")
            ? json_object_get_string_member(object, "subject") : NULL);

    if (json_object_has_member(object, "ts"))
        clawt_event_set_timestamp(event,
                                  json_object_get_int_member(object, "ts"));

    if (json_object_has_member(object, "cursor"))
        clawt_event_set_cursor(
            event, (guint64)json_object_get_int_member(object, "cursor"));

    if (json_object_has_member(object, "detail")) {
        JsonObject *detail = json_object_get_object_member(object, "detail");
        g_autoptr(GList) members = json_object_get_members(detail);
        GList *l;

        for (l = members; l != NULL; l = l->next)
            clawt_event_set_detail(
                event, l->data,
                json_object_get_string_member(detail, l->data));
    }

    return event;
}

GPtrArray *
clawt_event_log_read(ClawtEventLog *self, const gchar *subject, guint limit)
{
    g_autoptr(GPtrArray) files = NULL;
    g_autoptr(GDir) dir = NULL;
    GPtrArray *out;
    const gchar *name;
    guint i;

    g_return_val_if_fail(CLAWT_IS_EVENT_LOG(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_event_free);

    dir = g_dir_open(self->dir, 0, NULL);
    if (dir == NULL)
        return out;

    files = g_ptr_array_new_with_free_func(g_free);

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(name, ".ndjson"))
            g_ptr_array_add(files, g_strdup(name));
    }

    /* Filenames are ISO dates, so sorting them sorts by time. */
    g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

    for (i = 0; i < files->len; i++) {
        g_autofree gchar *path = g_build_filename(
            self->dir, g_ptr_array_index(files, i), NULL);
        g_autofree gchar *contents = NULL;
        g_auto(GStrv) lines = NULL;
        gsize j;

        if (!g_file_get_contents(path, &contents, NULL, NULL))
            continue;

        lines = g_strsplit(contents, "\n", -1);

        for (j = 0; lines[j] != NULL; j++) {
            g_autoptr(JsonParser) parser = NULL;
            ClawtEvent *event;

            if (lines[j][0] == '\0')
                continue;

            parser = json_parser_new();

            /*
             * A malformed line is skipped, not fatal.  The log is
             * append-only and flushed per line, so a crash mid-write
             * leaves exactly one truncated line -- refusing to read the
             * file over it would throw away the history somebody is
             * trying to read.
             */
            if (!json_parser_load_from_data(parser, lines[j], -1, NULL))
                continue;

            event = event_from_json(json_parser_get_root(parser));
            if (event == NULL)
                continue;

            if (subject != NULL &&
                g_strcmp0(clawt_event_get_subject(event), subject) != 0) {
                clawt_event_free(event);
                continue;
            }

            g_ptr_array_add(out, event);
        }
    }

    if (limit > 0 && out->len > limit)
        g_ptr_array_remove_range(out, 0, out->len - limit);

    return out;
}

guint
clawt_event_log_sweep(ClawtEventLog *self)
{
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GDateTime) now = NULL;
    const gchar *name;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_EVENT_LOG(self), 0);

    if (self->retention_days <= 0)
        return 0;

    dir = g_dir_open(self->dir, 0, NULL);
    if (dir == NULL)
        return 0;

    now = g_date_time_new_now_utc();

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autoptr(GDateTime) file_day = NULL;
        g_autofree gchar *path = NULL;
        gint year = 0;
        gint month = 0;
        gint day = 0;

        if (!g_str_has_suffix(name, ".ndjson"))
            continue;

        if (sscanf(name, "%4d-%2d-%2d", &year, &month, &day) != 3)
            continue;

        file_day = g_date_time_new_utc(year, month, day, 0, 0, 0);
        if (file_day == NULL)
            continue;

        if (g_date_time_difference(now, file_day) <=
            (GTimeSpan)self->retention_days * G_TIME_SPAN_DAY)
            continue;

        path = g_build_filename(self->dir, name, NULL);

        /*
         * The file currently open is never swept.  Deleting it out from
         * under the handle would send every subsequent line to a file
         * nothing can find.
         */
        if (self->open_day != NULL && g_str_has_prefix(name, self->open_day))
            continue;

        if (g_unlink(path) == 0)
            removed++;
    }

    return removed;
}

static void
clawt_event_log_dispose(GObject *object)
{
    ClawtEventLog *self = CLAWT_EVENT_LOG(object);

    if (self->bus != NULL && self->handler != 0) {
        g_signal_handler_disconnect(self->bus, self->handler);
        self->handler = 0;
        self->bus = NULL;
    }

    if (self->handle != NULL) {
        fclose(self->handle);
        self->handle = NULL;
    }

    G_OBJECT_CLASS(clawt_event_log_parent_class)->dispose(object);
}

static void
clawt_event_log_finalize(GObject *object)
{
    ClawtEventLog *self = CLAWT_EVENT_LOG(object);

    g_free(self->dir);
    g_free(self->open_day);

    G_OBJECT_CLASS(clawt_event_log_parent_class)->finalize(object);
}

static void
clawt_event_log_class_init(ClawtEventLogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_event_log_dispose;
    object_class->finalize = clawt_event_log_finalize;
}

static void
clawt_event_log_init(ClawtEventLog *self)
{
    self->retention_days = 0;
}
