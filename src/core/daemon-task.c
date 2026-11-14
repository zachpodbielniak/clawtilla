/*
 * daemon-task.c - The client surface: task.*
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
 * Whether the operator's Retry can do anything for @incident right now.
 *
 * Decided here, once, so three clients do not each guess from the state
 * names which rows get the button: a resolved incident, a run still
 * going, and an agent that never started (its work is still queued) all
 * say no.
 */
static gboolean
incident_retryable(
	ClawtDaemon   *self,
	ClawtIncident *incident
){
	ClawtTask *task;

	if (clawt_incident_is_resolved(incident) ||
	    clawt_incident_get_kind(incident) == CLAWT_INCIDENT_UNSTARTABLE)
		return FALSE;

	task = clawt_task_manager_get(self->tasks,
	                              clawt_incident_get_task_id(incident));

	if (task == NULL)
		return clawt_incident_get_routine_id(incident) != NULL;

	return clawt_task_state_is_retryable(clawt_task_get_state(task));
}

static void
add_incident_object(
	ClawtDaemon   *self,
	JsonBuilder   *builder,
	ClawtIncident *incident
){
	const gchar *const *lines = clawt_incident_get_log_lines(incident);
	gsize i;

	json_builder_begin_object(builder);

	clawt_daemon_add_string_member(builder, "id",
	                               clawt_incident_get_id(incident));
	clawt_daemon_add_string_member(
		builder, "kind",
		clawt_enum_to_nick(CLAWT_TYPE_INCIDENT_KIND,
		                   clawt_incident_get_kind(incident)));
	clawt_daemon_add_string_member(
		builder, "kind_label",
		clawt_incident_kind_label(clawt_incident_get_kind(incident)));
	clawt_daemon_add_string_member(builder, "agent",
	                               clawt_incident_get_agent(incident));
	clawt_daemon_add_string_member(builder, "team",
	                               clawt_incident_get_team(incident));
	clawt_daemon_add_string_member(builder, "room",
	                               clawt_incident_get_room(incident));
	clawt_daemon_add_string_member(builder, "run_room",
	                               clawt_incident_get_run_room(incident));
	clawt_daemon_add_string_member(builder, "task",
	                               clawt_incident_get_task_id(incident));
	clawt_daemon_add_string_member(builder, "routine",
	                               clawt_incident_get_routine_id(incident));
	clawt_daemon_add_string_member(builder, "reason",
	                               clawt_incident_get_reason(incident));
	clawt_daemon_add_string_member(builder, "recipient",
	                               clawt_incident_get_recipient(incident));
	clawt_daemon_add_string_member(builder, "note",
	                               clawt_incident_get_note(incident));
	clawt_daemon_add_string_member(builder, "retried_by",
	                               clawt_incident_get_retried_by(incident));

	json_builder_set_member_name(builder, "occurrences");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_occurrences(incident));
	json_builder_set_member_name(builder, "retries");
	json_builder_add_int_value(builder, clawt_incident_get_retries(incident));

	/* Microseconds, like every timestamp but a task's. */
	json_builder_set_member_name(builder, "opened_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_opened_at(incident));
	json_builder_set_member_name(builder, "updated_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_updated_at(incident));
	json_builder_set_member_name(builder, "delivered_at");
	json_builder_add_int_value(builder,
	                           clawt_incident_get_delivered_at(incident));

	json_builder_set_member_name(builder, "queued");
	json_builder_add_boolean_value(builder,
	                               clawt_incident_is_queued(incident));
	json_builder_set_member_name(builder, "resolved");
	json_builder_add_boolean_value(builder,
	                               clawt_incident_is_resolved(incident));
	json_builder_set_member_name(builder, "retryable");
	json_builder_add_boolean_value(builder,
	                               incident_retryable(self, incident));

	json_builder_set_member_name(builder, "log_lines");
	json_builder_begin_array(builder);

	for (i = 0; lines != NULL && lines[i] != NULL; i++)
		json_builder_add_string_value(builder, lines[i]);

	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

JsonNode *
clawt_daemon_handle_task(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── tasks ── */
    if (g_strcmp0(kind, "task.list") == 0) {
        g_autoptr(GPtrArray) tasks = NULL;
        guint i;

        tasks = clawt_task_manager_list(
            self->tasks, clawt_ipc_payload_string(payload, "agent"),
            clawt_ipc_payload_boolean(payload, "all", TRUE));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "tasks");
        json_builder_begin_array(builder);

        for (i = 0; i < tasks->len; i++)
            clawt_daemon_add_task_object(builder, g_ptr_array_index(tasks, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.show") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        ClawtTask *task = (task_id != NULL)
                          ? clawt_task_manager_get(self->tasks, task_id)
                          : NULL;

        if (task == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such task");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        clawt_daemon_add_task_object(builder, task);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.cancel") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        guint cancelled;

        if (task_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task?");

        /*
         * "user", because the clients are the operator's hands.  The
         * delegating agent is then told its task was cancelled over its
         * head -- which is the notice it is waiting on, where its own
         * cancel would be a notice about what it just did.
         */
        cancelled = clawt_task_manager_cancel(self->tasks, task_id,
                                              "cancelled from a client",
                                              "user");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cancelled");
        json_builder_add_int_value(builder, cancelled);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── incidents ── */
    if (g_strcmp0(kind, "task.retry") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        const gchar *incident_id = clawt_ipc_payload_string(payload,
                                                            "incident");
        g_autoptr(GError) error = NULL;
        g_autofree gchar *now = NULL;
        gboolean rerun = FALSE;

        if (task_id == NULL && incident_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task or incident?");

        if (self->incidents == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "retrying is not available");

        /*
         * "user", because the clients are the operator's hands -- and
         * the operator's retry is not counted against the agents'
         * limit, since a person pressing the button has looked.
         */
        now = clawt_incident_desk_retry(
            self->incidents, task_id, incident_id, "user",
            clawt_ipc_payload_string(payload, "reason"), &rerun, &error);

        if (now == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "task", now);
        json_builder_set_member_name(builder, "rerun");
        json_builder_add_boolean_value(builder, rerun);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "incident.list") == 0) {
        gboolean all = clawt_ipc_payload_boolean(payload, "all", FALSE);
        g_autoptr(GPtrArray) incidents = NULL;
        guint i;

        json_builder_begin_object(builder);

        /*
         * The settings travel with the list, so the page that shows the
         * incidents can say who they went to without a second request.
         */
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(
            builder, clawt_config_get_boolean(
                         self->config, "orchestration.incidents.enabled"));
        clawt_daemon_add_string_member(
            builder, "to",
            clawt_incident_recipient_nick((ClawtIncidentRecipient)
                clawt_config_get_enum(self->config,
                                      "orchestration.incidents.to")));

        json_builder_set_member_name(builder, "incidents");
        json_builder_begin_array(builder);

        if (self->incidents != NULL)
            incidents = clawt_incident_desk_list(self->incidents);

        for (i = 0; incidents != NULL && i < incidents->len; i++) {
            ClawtIncident *incident = g_ptr_array_index(incidents, i);

            if (!all && clawt_incident_is_resolved(incident))
                continue;

            add_incident_object(self, builder, incident);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "incident.configure") == 0) {
        const gchar *to = clawt_ipc_payload_string(payload, "to");
        g_autoptr(GError) error = NULL;

        /*
         * Refused where it arrives rather than saved and read back as
         * the default: an unknown recipient would otherwise become
         * `lead` with nothing to say so.
         */
        if (to != NULL) {
            gint value = 0;

            if (!clawt_enum_from_nick(CLAWT_TYPE_INCIDENT_RECIPIENT, to,
                                      &value))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT,
                    "orchestration.incidents.to is lead, chief or none");

            clawt_config_set_string(self->config,
                                    "orchestration.incidents.to",
                                    clawt_incident_recipient_nick(
                                        (ClawtIncidentRecipient)value));
        }

        if (json_object_has_member(payload, "enabled"))
            clawt_config_set_boolean(
                self->config, "orchestration.incidents.enabled",
                clawt_ipc_payload_boolean(payload, "enabled", TRUE));

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "incident.configured", NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(
            builder, clawt_config_get_boolean(
                         self->config, "orchestration.incidents.enabled"));
        clawt_daemon_add_string_member(
            builder, "to",
            clawt_incident_recipient_nick((ClawtIncidentRecipient)
                clawt_config_get_enum(self->config,
                                      "orchestration.incidents.to")));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
