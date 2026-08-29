/*
 * daemon-routine.c - The client surface: routine.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_routine(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── routines ── */
    if (g_strcmp0(kind, "routine.list") == 0) {
        GPtrArray *routines = clawt_config_get_routines(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "routines");
        json_builder_begin_array(builder);

        for (i = 0; routines != NULL && i < routines->len; i++) {
            ClawtRoutine *routine = g_ptr_array_index(routines, i);
            const gchar *id = clawt_routine_get_id(routine);
            g_autofree gchar *expression = NULL;
            g_autoptr(GDateTime) next = NULL;
            g_autoptr(GError) cron_error = NULL;
            const ClawtSchemaEntry *entries;
            ClawtRunState state = CLAWT_RUN_NEVER;
            const gchar *detail = NULL;
            gint64 last;
            gsize n_entries = 0;
            gsize k;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, id);

            /*
             * The fields come from the schema rather than a list here,
             * for the reason the integration ones now do: a list in the
             * daemon and a list in the schema drift, and the drift is
             * silent.
             */
            entries = clawt_config_schema_get(&n_entries);

            for (k = 0; k < n_entries; k++) {
                const gchar *leaf;

                if (!g_str_has_prefix(entries[k].key, "routines."))
                    continue;

                leaf = entries[k].key + strlen("routines.");

                if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
                    continue;

                switch (entries[k].type) {
                case CLAWT_SCHEMA_BOOLEAN:
                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_boolean_value(
                        builder, clawt_routine_get_boolean(routine, leaf));
                    break;

                case CLAWT_SCHEMA_INT:
                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_int_value(
                        builder, clawt_routine_get_int(routine, leaf));
                    break;

                default: {
                    const gchar *value =
                        clawt_routine_get_string(routine, leaf);

                    if (value == NULL)
                        break;

                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_string_value(builder, value);
                    break;
                }
                }
            }

            /*
             * What it actually means, worked out here.  A client that
             * had to turn "weekdays at 09:00" into an expression itself
             * would be a second implementation of the schedule.
             */
            expression = clawt_routine_get_cron(routine, &cron_error);

            if (expression != NULL) {
                json_builder_set_member_name(builder, "expression");
                json_builder_add_string_value(builder, expression);
            } else if (cron_error != NULL) {
                json_builder_set_member_name(builder, "problem");
                json_builder_add_string_value(builder, cron_error->message);
            }

            next = (self->routines != NULL)
                ? clawt_routine_runner_next_run(self->routines, id) : NULL;

            if (next != NULL) {
                g_autofree gchar *formatted =
                    g_date_time_format_iso8601(next);

                json_builder_set_member_name(builder, "next_run");
                json_builder_add_string_value(builder, formatted);
            }

            last = (self->routines != NULL)
                ? clawt_routine_runner_last_run(self->routines, id, &state,
                                                &detail) : 0;

            json_builder_set_member_name(builder, "last_run");
            json_builder_add_int_value(builder, last);
            json_builder_set_member_name(builder, "last_state");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_RUN_STATE,
                                            (gint)state));

            if (detail != NULL) {
                json_builder_set_member_name(builder, "last_detail");
                json_builder_add_string_value(builder, detail);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "routine.add") == 0 ||
        g_strcmp0(kind, "routine.update") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        gboolean adding = g_strcmp0(kind, "routine.add") == 0;
        ClawtRoutine *routine;
        const ClawtSchemaEntry *entries;
        gsize n_entries = 0;
        gsize i;

        if (adding) {
            routine = clawt_config_add_routine(self->config, id, &error);

            if (routine == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else {
            routine = (id != NULL)
                ? clawt_config_get_routine(self->config, id) : NULL;

            if (routine == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no routine called "
                                           "that");
        }

        entries = clawt_config_schema_get(&n_entries);

        for (i = 0; i < n_entries; i++) {
            const gchar *leaf;

            if (!g_str_has_prefix(entries[i].key, "routines."))
                continue;

            leaf = entries[i].key + strlen("routines.");

            if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0 ||
                !json_object_has_member(payload, leaf))
                continue;

            switch (entries[i].type) {
            case CLAWT_SCHEMA_BOOLEAN:
                clawt_routine_set_boolean(
                    routine, leaf,
                    clawt_ipc_payload_boolean(payload, leaf, FALSE));
                break;

            case CLAWT_SCHEMA_INT:
                clawt_routine_set_int(routine, leaf,
                                      clawt_ipc_payload_int(payload, leaf, 0));
                break;

            default:
                clawt_routine_set_string(
                    routine, leaf, clawt_ipc_payload_string(payload, leaf));
                break;
            }
        }

        /*
         * The schedule is checked here, while somebody is still looking
         * at what they typed -- rather than at the next tick, in a
         * warning nobody is watching for.
         */
        {
            g_autofree gchar *expression = NULL;
            g_autoptr(GError) cron_error = NULL;

            expression = clawt_routine_get_cron(routine, &cron_error);

            if (expression == NULL && cron_error != NULL) {
                if (adding)
                    clawt_config_remove_routine(self->config, id);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           cron_error->message);
            }
        }

        if (!clawt_config_save(self->config, &error)) {
            if (adding)
                clawt_config_remove_routine(self->config, id);

            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "routine.changed", id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "routine.remove") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");

        if (id == NULL || !clawt_config_remove_routine(self->config, id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no routine called that");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "routine.changed", id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "routine.run") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        const gchar *task_id;

        if (self->routines == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no scheduler");

        task_id = clawt_routine_runner_run_now(self->routines, id, &error);

        if (task_id == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        json_builder_add_string_value(builder, task_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
