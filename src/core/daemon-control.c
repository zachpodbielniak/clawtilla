/*
 * daemon-control.c - The client surface: control.*, git and plugins
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

/*
 * Told when the check finds a version we are not on.
 *
 * Once per version, not once per check: the timer keeps firing for as
 * long as the daemon is up, and a buzz every interval about the same
 * release is how somebody turns the notifier off -- and then it is not
 * there for the two events it exists for.
 */
static void
on_update_found(ClawtUpdateCheck *check,
                const gchar      *version,
                gpointer          user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtNotification) notification = NULL;

    (void)check;

    g_message("clawtilla %s is available (running %s)", version,
              CLAWT_VERSION_STRING);

    if (self->notifier == NULL)
        return;

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_UPDATE, NULL, NULL,
        "A newer clawtilla is available",
        version);

    clawt_notifier_notify(self->notifier, notification);
}

void
clawt_daemon_updates_start(ClawtDaemon *self)
{
    g_autofree gchar *url = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (!clawt_config_get_boolean(self->config, "daemon.update_check"))
        return;

    url = g_strdup(clawt_config_get_string(self->config,
                                           "daemon.update_url"));

    if (url == NULL || *url == '\0') {
        g_warning("daemon.update_check is on but daemon.update_url is "
                  "empty; nothing will be checked");
        return;
    }

    self->updates = clawt_update_check_new(
        CLAWT_VERSION_STRING, url,
        (gint)clawt_config_get_int(self->config,
                                   "daemon.update_interval_hours"));

    g_signal_connect(self->updates, "found", G_CALLBACK(on_update_found),
                     self);

    clawt_update_check_start(self->updates);
}

JsonNode *
clawt_daemon_handle_control(
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

    /* ── control ── */
    if (g_strcmp0(kind, "control.status") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "version");
        json_builder_add_string_value(builder, CLAWT_VERSION_STRING);
        json_builder_set_member_name(builder, "config");
        json_builder_add_string_value(builder, self->config_path);
        json_builder_set_member_name(builder, "agents");
        json_builder_add_int_value(
            builder, clawt_agent_manager_list(self->agents)->len);
        json_builder_set_member_name(builder, "connected");
        json_builder_add_int_value(
            builder, clawt_link_server_count_links(self->link_server));
        json_builder_set_member_name(builder, "clients");
        json_builder_add_int_value(
            builder, clawt_ipc_server_count_clients(self->ipc_server));
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(
            builder, (gint64)clawt_event_bus_get_cursor(self->bus));

        /*
         * Which build this actually is.
         *
         * "0.1.0" is the number three releases can share while somebody
         * is asking whether they are on the fix -- and answering that
         * without it meant rebuilding to find out.  Compiled in beside
         * the version it qualifies.
         */
        json_builder_set_member_name(builder, "commit");
        json_builder_add_string_value(builder, CLAWT_GIT_SHA);

        /*
         * And whether a newer one exists.  Reported by the daemon rather
         * than checked by each client: three clients comparing two
         * version strings is three chances to decide 0.10.0 is older
         * than 0.9.0, and a wrong answer there looks exactly like a
         * right one.
         *
         * Absent when daemon.update_check is off, which is the default.
         * A client seeing no `update` member knows nothing is being
         * checked -- which is a different thing from a check that ran
         * and found nothing, and has to read differently.
         */
        if (self->updates != NULL)
            clawt_update_check_describe(self->updates, builder);

        /*
         * And whether the fleet is being held.
         *
         * Always, including "not held", because this is the answer to
         * "is it safe to restart" -- a client that has to infer it from
         * an absent member cannot tell a daemon that is not held from
         * one too old to say.
         */
        clawt_daemon_hold_describe(self, builder);

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "control.reload") == 0) {
        g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

        if (!clawt_daemon_reload_internal(self, refusals, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * A response and not an error: the configuration was reloaded and
         * every agent clawtilla could render was.  But an agent it
         * refused is still running against the config.yaml it had, and
         * the only person who can fix that is the one who just asked for
         * the reload -- so they are handed the names and the reasons
         * rather than a bare success and a warning in the journal.
         *
         * The array is always present, so a client can tell "nothing was
         * refused" from "this daemon does not report refusals".
         */
        json_builder_begin_object(builder);
        clawt_daemon_add_render_refusals(builder, refusals);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "control.shutdown") == 0) {
        JsonNode *reply = clawt_ipc_response_new(request, NULL);
        GSource *quit = g_idle_source_new();

        /*
         * Answered first, then queued.  Quitting the loop inside this
         * call would close the socket before the reply reached the client,
         * which looks to them like the daemon crashed.
         *
         * Attached to the daemon's own context by name.  g_idle_add()
         * takes the global default, which is the loop clawtillad runs
         * and not the one an embedded daemon does -- so the standalone
         * daemon quit, and an embedded one answered "ok" and carried on
         * for ever, with nobody iterating the context the quit was on.
         */
        g_source_set_callback(quit, clawt_daemon_quit_idle, self, NULL);
        g_source_attach(quit, self->main_context);
        g_source_unref(quit);

        return reply;
    }

    if (g_strcmp0(kind, "state.git_init") == 0) {
        g_autofree gchar *ignore_path = NULL;
        gboolean created = FALSE;

        if (self->state_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "there is no state directory yet");

        if (!clawt_daemon_prepare_state_git(self->state_dir, TRUE, &created,
                                            &ignore_path, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder, self->state_dir);
        json_builder_set_member_name(builder, "created");
        json_builder_add_boolean_value(builder, created);
        json_builder_set_member_name(builder, "gitignore");
        json_builder_add_string_value(builder, ignore_path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "plugin.list") == 0) {
        g_autoptr(GPtrArray) plugins = NULL;
        guint i;

        plugins = clawt_plugin_manager_list(self->plugins);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "plugins");
        json_builder_begin_array(builder);

        for (i = 0; i < plugins->len; i++) {
            ClawtPlugin *plugin = g_ptr_array_index(plugins, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_id(plugin));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_name(plugin));
            json_builder_set_member_name(builder, "version");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_version(plugin));
            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(
                builder, clawt_plugin_get_description(plugin));
            json_builder_set_member_name(builder, "active");
            json_builder_add_boolean_value(builder,
                                           clawt_plugin_is_active(plugin));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
