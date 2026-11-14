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

/* Read the proposed effective value without changing the live YAML mapping.
 * Explicit null removes an override, so validate the schema default in that
 * case, exactly as the routine getter will after applying the update. */
static const gchar *
requested_routine_string(ClawtRoutine *routine, JsonObject *payload,
						 const gchar *key)
{
	const gchar *value;
	g_autofree gchar *schema_key = NULL;
	const ClawtSchemaEntry *entry;

	if (!json_object_has_member(payload, key))
		return clawt_routine_get_string(routine, key);
	value = clawt_ipc_payload_string(payload, key);
	if (value != NULL)
		return value;
	schema_key = g_strconcat("routines.", key, NULL);
	entry = clawt_config_schema_lookup(schema_key);
	return entry != NULL ? entry->default_value : NULL;
}

/*
 * Adds or changes a routine from a payload of its schema fields, saves
 * the config and reloads, so the scheduler sees it at once.
 *
 * One function for the routine.add/update frames and for an agent
 * scheduling its own (clawtilla_routine_add/_update): two copies of
 * "validate, apply, save, reload, announce" would differ the first time
 * one of them learned something.  The whole schedule is validated before
 * any field changes, so a refused edit leaves the routine -- and the
 * file's comments -- as they were.
 */
gboolean
clawt_daemon_routine_save(ClawtDaemon *self, const gchar *id,
                          JsonObject *payload, gboolean adding,
                          GError **error)
{
    ClawtRoutine *routine;
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);
    g_return_val_if_fail(payload != NULL, FALSE);

    if (adding) {
        routine = clawt_config_add_routine(self->config, id, error);

        if (routine == NULL)
            return FALSE;
    } else {
        routine = (id != NULL) ? clawt_config_get_routine(self->config, id)
                               : NULL;

        if (routine == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                                "there is no routine called that");
            return FALSE;
        }
    }

    {
        g_autoptr(GError) invalid = NULL;
        g_autofree gchar *expression = clawt_cron_from_preset(
            requested_routine_string(routine, payload, "schedule"),
            requested_routine_string(routine, payload, "at"),
            requested_routine_string(routine, payload, "weekday"),
            requested_routine_string(routine, payload, "cron"), &invalid);

        if (expression == NULL && invalid != NULL) {
            if (adding)
                clawt_config_remove_routine(self->config, id);

            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT, invalid->message);
            return FALSE;
        }
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
                routine, leaf, clawt_ipc_payload_boolean(payload, leaf, FALSE));
            break;

        case CLAWT_SCHEMA_INT:
            clawt_routine_set_int(routine, leaf,
                                  clawt_ipc_payload_int(payload, leaf, 0));
            break;

        default:
            clawt_routine_set_string(routine, leaf,
                                     clawt_ipc_payload_string(payload, leaf));
            break;
        }
    }

    if (!clawt_config_save(self->config, error)) {
        if (adding)
            clawt_config_remove_routine(self->config, id);

        return FALSE;
    }

    if (!clawt_daemon_reload(self, error))
        return FALSE;

    clawt_event_bus_emit(self->bus, "routine.changed", id);
    return TRUE;
}

/* Removes a routine, saves and reloads; shared like the function above. */
gboolean
clawt_daemon_routine_delete(ClawtDaemon *self, const gchar *id,
                            GError **error)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (id == NULL || !clawt_config_remove_routine(self->config, id)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "there is no routine called that");
        return FALSE;
    }

    if (!clawt_config_save(self->config, error))
        return FALSE;

    if (!clawt_daemon_reload(self, error))
        return FALSE;

    clawt_event_bus_emit(self->bus, "routine.changed", id);
    return TRUE;
}

/*
 * An agent's change to its own routines, already checked by the tools
 * (ownership, lock, limit, frequency); persisted through the same two
 * functions the client frames use.
 */
gboolean
clawt_daemon_routine_for_tools(const gchar *verb, const gchar *id,
                               JsonObject *payload, gpointer user_data,
                               GError **error)
{
    ClawtDaemon *self = user_data;

    if (g_strcmp0(verb, "remove") == 0)
        return clawt_daemon_routine_delete(self, id, error);

    if (payload == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "nothing to save");
        return FALSE;
    }

    return clawt_daemon_routine_save(self, id, payload,
                                     g_strcmp0(verb, "add") == 0, error);
}

/* ── Runs whose computer did not start ───────────────────────────── */

static void
ensure_start_tables(
	ClawtDaemon *self
){
	if (self->routine_starts == NULL)
		self->routine_starts = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)g_hash_table_unref);

	if (self->routine_computer_down == NULL)
		self->routine_computer_down = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, g_free);

	if (self->routine_waking == NULL)
		self->routine_waking = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
}

void
clawt_daemon_routine_start_watch(
	ClawtDaemon *self,
	const gchar *routine_id,
	const gchar *agent_id,
	gint64       run_at_us
){
	ClawtAgent *agent;
	ClawtAgentState state;
	GHashTable *pending;
	gint64 *when;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (routine_id == NULL || agent_id == NULL || self->routines == NULL)
		return;

	agent = clawt_agent_manager_get(self->agents, agent_id);

	if (agent == NULL)
		return;

	/*
	 * Already up is a computer that started.  Said now rather than
	 * waited for: there is no start coming to settle it.
	 */
	state = clawt_agent_get_state(agent);

	if (state == CLAWT_AGENT_STATE_RUNNING ||
	    state == CLAWT_AGENT_STATE_DEGRADED) {
		clawt_routine_runner_note_start(self->routines, routine_id,
		                                run_at_us, NULL);
		return;
	}

	ensure_start_tables(self);

	/*
	 * One entry per routine, holding its latest run.  Several runs
	 * queued behind one start are one start, and counting each of them
	 * would turn a routine off on the first refusal of a computer that
	 * had been down for an afternoon.
	 */
	pending = g_hash_table_lookup(self->routine_starts, agent_id);

	if (pending == NULL) {
		pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                g_free);
		g_hash_table_insert(self->routine_starts, g_strdup(agent_id),
		                    pending);
	}

	when = g_new(gint64, 1);
	*when = run_at_us;
	g_hash_table_replace(pending, g_strdup(routine_id), when);

	/*
	 * An agent whose computer refused to start sits in ERROR until
	 * somebody starts it, so without this the run would wait in its
	 * mailbox for good and the routine would report a run that never
	 * happened, every tick.  Each run is another attempt -- which is
	 * also what lets a computer that has been fixed come back on its
	 * own.  In the background, because a VM can take minutes and this
	 * is the scheduler's tick.
	 *
	 * Only that ERROR.  A stopped agent was stopped by somebody, and one
	 * in ERROR for any other reason is not this guard's to restart; the
	 * run waits for whoever starts it next, as it always has, and that
	 * start settles it.
	 */
	if (state == CLAWT_AGENT_STATE_ERROR &&
	    g_hash_table_contains(self->routine_computer_down, agent_id) &&
	    !g_hash_table_contains(self->routine_waking, agent_id)) {
		g_hash_table_add(self->routine_waking, g_strdup(agent_id));
		clawt_daemon_start_agent_background(self, agent_id);
	}
}

typedef struct {
	ClawtDaemon *daemon;    /* ref */
	gchar       *routine_id;
	gchar       *agent_id;
	guint        failures;
	gchar       *error;
} TurnOff;

static void
turn_off_free(
	gpointer data
){
	TurnOff *job = data;

	g_object_unref(job->daemon);
	g_free(job->routine_id);
	g_free(job->agent_id);
	g_free(job->error);
	g_free(job);
}

/*
 * Turns the routine off and says so.
 *
 * Through clawt_daemon_routine_save(), the function every client's edit
 * goes through, so the change is saved, reloaded, announced as
 * `routine.changed` and visible as `enabled: false` everywhere -- and
 * turned back on the same way.  A routine that was off by some private
 * flag would read as on in every editor while never running, which is a
 * wrong answer that looks like a working configuration.
 *
 * On an idle rather than where the failure was noticed: that is inside
 * an agent start, and a save reloads the fleet, which reconciles the
 * very agent whose start is still on the stack.
 */
static gboolean
on_turn_off(
	gpointer user_data
){
	TurnOff *job = user_data;
	ClawtDaemon *self = job->daemon;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtEvent) event = NULL;
	g_autofree gchar *alert = NULL;

	json_object_set_string_member(payload, "id", job->routine_id);
	json_object_set_boolean_member(payload, "enabled", FALSE);

	if (!clawt_daemon_routine_save(self, job->routine_id, payload, FALSE,
	                               &error)) {
		g_warning("routine '%s' reached routines.max_start_failures but could "
		          "not be turned off: %s", job->routine_id, error->message);
		return G_SOURCE_REMOVE;
	}

	alert = clawt_routine_turned_off_alert(job->routine_id, job->failures,
	                                       job->error);
	/* A message, not a warning: it is reported, not a fault in the daemon. */
	g_message("%s", alert);

	/*
	 * An alert, because it arrived on its own: nobody was looking at
	 * the routine when its computer refused for the third time.
	 */
	event = clawt_event_new("routine.turned_off", job->routine_id);
	clawt_event_set_detail(event, "agent", job->agent_id);
	clawt_event_set_detail_int(event, "failures", (gint64)job->failures);

	if (job->error != NULL)
		clawt_event_set_detail(event, "error", job->error);

	clawt_event_bus_publish(self->bus, event);

	/*
	 * And to a notifier, as an error as well as a routine event: the
	 * default subscription is questions and errors, and a routine that
	 * has stopped running is the second.
	 */
	if (self->notifier != NULL) {
		ClawtAgentConfig *config = clawt_config_get_agent(self->config,
		                                                  job->agent_id);
		g_autoptr(ClawtNotification) notification = NULL;
		g_autofree gchar *title = g_strdup_printf("routine %s turned off",
		                                          job->routine_id);

		notification = clawt_notification_new(
			(ClawtNotifyEvents)(CLAWT_NOTIFY_EVENTS_ERROR |
			                    CLAWT_NOTIFY_EVENTS_ROUTINE),
			job->agent_id,
			(config != NULL) ? clawt_agent_config_get_string(config, "name")
			                 : NULL,
			title, alert);
		clawt_notifier_notify(self->notifier, notification);
	}

	return G_SOURCE_REMOVE;
}

void
clawt_daemon_routine_starts_settle(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *computer_error
){
	g_autoptr(GHashTable) pending = NULL;
	g_autofree gchar *stolen_key = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (agent_id == NULL)
		return;

	ensure_start_tables(self);
	g_hash_table_remove(self->routine_waking, agent_id);

	/* Remembered for every agent, so the first routine run can act on it. */
	if (computer_error != NULL)
		g_hash_table_replace(self->routine_computer_down, g_strdup(agent_id),
		                     g_strdup(computer_error));
	else
		g_hash_table_remove(self->routine_computer_down, agent_id);

	/*
	 * Taken out before it is walked: a note can turn a routine off, and
	 * nothing here may be reached again for this agent while the table
	 * is being read.
	 */
	if (!g_hash_table_steal_extended(self->routine_starts, agent_id,
	                                 (gpointer *)&stolen_key,
	                                 (gpointer *)&pending))
		return;

	if (self->routines == NULL)
		return;

	g_hash_table_iter_init(&iter, pending);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const gchar *routine_id = key;
		TurnOff *job;
		GSource *source;
		guint failures = 0;

		if (!clawt_routine_runner_note_start(self->routines, routine_id,
		                                     *(gint64 *)value,
		                                     computer_error))
			continue;

		clawt_routine_runner_get_turned_off(self->routines, routine_id,
		                                    &failures, NULL);

		job = g_new0(TurnOff, 1);
		job->daemon = g_object_ref(self);
		job->routine_id = g_strdup(routine_id);
		job->agent_id = g_strdup(agent_id);
		job->failures = failures;
		job->error = g_strdup(computer_error);

		source = g_idle_source_new();
		g_source_set_callback(source, on_turn_off, job, turn_off_free);
		g_source_attach(source, self->main_context);
		g_source_unref(source);
	}
}

void
clawt_daemon_routine_wake_done(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->routine_waking != NULL && agent_id != NULL)
		g_hash_table_remove(self->routine_waking, agent_id);
}

void
clawt_daemon_routine_starts_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	g_clear_pointer(&self->routine_starts, g_hash_table_unref);
	g_clear_pointer(&self->routine_computer_down, g_hash_table_unref);
	g_clear_pointer(&self->routine_waking, g_hash_table_unref);
}

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

            /*
             * The start-failure guard: the count so far, and -- while the
             * routine is off for it -- how many failures and which error
             * turned it off.  Numbers and the raw error rather than a
             * sentence, so each client words it with
             * clawt_routine_turned_off_note() and none of them parses it.
             */
            if (self->routines != NULL) {
                guint off_after = 0;
                const gchar *off_error = NULL;

                json_builder_set_member_name(builder, "start_failures");
                json_builder_add_int_value(
                    builder, (gint64)clawt_routine_runner_get_start_failures(
                        self->routines, id));

                if (clawt_routine_runner_get_turned_off(self->routines, id,
                                                        &off_after,
                                                        &off_error)) {
                    json_builder_set_member_name(builder, "turned_off_after");
                    json_builder_add_int_value(builder, (gint64)off_after);

                    if (off_error != NULL) {
                        json_builder_set_member_name(builder,
                                                     "turned_off_error");
                        json_builder_add_string_value(builder, off_error);
                    }
                }
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

        if (!clawt_daemon_routine_save(self, id, payload,
                                       g_strcmp0(kind, "routine.add") == 0,
                                       &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "routine.remove") == 0) {
        if (!clawt_daemon_routine_delete(
                self, clawt_ipc_payload_string(payload, "id"), &error))
            return clawt_ipc_error_new(request, error->code, error->message);

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
