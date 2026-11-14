/*
 * daemon-sentinel.c - The client surface for the sentinel: sentinel.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The sentinel reads the event log and writes a report; the daemon does
 * neither.  `sentinel.scan` starts `clawtilla sentinel` as a child -- out
 * of process, so a scan of a week of logs never holds the loop every
 * client is waiting on -- and answers at once; when the child exits the
 * daemon publishes `sentinel.finished`, and `sentinel.report` reads the
 * YAML file the child wrote.  Nothing here opens an event log file.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* Where the report goes: beside the log, never in it. */
static gchar *
report_path(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "sentinel.yaml", NULL);
}

static gchar *
events_dir(
	ClawtDaemon *self
){
	return g_build_filename(self->state_dir, "events", NULL);
}

static void
publish_finished(
	ClawtDaemon *self,
	gboolean     ok
){
	g_autoptr(ClawtEvent) event = NULL;

	if (self->bus == NULL)
		return;

	event = clawt_event_new("sentinel.finished", NULL);
	clawt_event_set_detail(event, "ok", ok ? "true" : "false");

	if (!ok && self->sentinel_error != NULL)
		clawt_event_set_detail(event, "error", self->sentinel_error);

	clawt_event_bus_publish(self->bus, event);
}

static void
on_sentinel_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(ClawtDaemon) self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	GSubprocess *child = G_SUBPROCESS(source);
	gboolean ok;

	ok = g_subprocess_communicate_utf8_finish(child, result, &out, &err,
		&error);

	/* A daemon stopping mid-scan drops the child; nobody to tell. */
	if (self->sentinel_child != child)
		return;

	g_clear_object(&self->sentinel_child);
	g_clear_pointer(&self->sentinel_error, g_free);

	if (!ok) {
		self->sentinel_error = g_strdup(error->message);
	} else if (!g_subprocess_get_successful(child)) {
		/*
		 * The child's own last line, which is the sentence it wrote for
		 * a person; a bare exit status points at nothing.
		 */
		g_autofree gchar *said = g_strstrip(g_strdup(err != NULL ? err : ""));

		self->sentinel_error = (*said != '\0')
			? clawt_clip_line(said, 400)
			: g_strdup("the sentinel exited without saying why");
		ok = FALSE;
	}

	if (!ok)
		g_warning("sentinel: %s", self->sentinel_error);

	publish_finished(self, ok);
}

void
clawt_daemon_sentinel_set_command(
	ClawtDaemon *self,
	const gchar *command
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	g_free(self->sentinel_command);
	self->sentinel_command = g_strdup(command);
}

void
clawt_daemon_sentinel_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	/*
	 * A scan still running is left to finish on its own: it only reads,
	 * and the report it writes is atomic.  Clearing the pointer is what
	 * tells the completion it has nobody to report to.
	 */
	g_clear_object(&self->sentinel_child);
	g_clear_pointer(&self->sentinel_error, g_free);
	g_clear_pointer(&self->sentinel_command, g_free);
}

/* Starts a scan, or says one is already running. */
static JsonNode *
start_scan(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *command = NULL;
	g_autofree gchar *events = events_dir(self);
	g_autofree gchar *output = report_path(self);
	GPtrArray *argv;
	gboolean started = FALSE;

	if (self->sentinel_child == NULL) {
		command = (self->sentinel_command != NULL)
			? g_strdup(self->sentinel_command)
			: clawt_connector_resolve_command("clawtilla", &error);

		if (command == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
				error->message);

		argv = g_ptr_array_new();
		g_ptr_array_add(argv, command);

		if (self->config_path != NULL) {
			g_ptr_array_add(argv, (gpointer)"-c");
			g_ptr_array_add(argv, self->config_path);
		}

		g_ptr_array_add(argv, (gpointer)"sentinel");
		g_ptr_array_add(argv, (gpointer)"--events");
		g_ptr_array_add(argv, events);
		g_ptr_array_add(argv, (gpointer)"--output");
		g_ptr_array_add(argv, output);
		g_ptr_array_add(argv, NULL);

		launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
			G_SUBPROCESS_FLAGS_STDERR_PIPE);
		self->sentinel_child = g_subprocess_launcher_spawnv(launcher,
			(const gchar * const *)argv->pdata, &error);
		g_ptr_array_free(argv, TRUE);

		if (self->sentinel_child == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_RUNTIME_SPAWN,
				error->message);

		/*
		 * The wait is attached to the daemon's own context by pushing it:
		 * an async call takes the thread-default, and for an embedded
		 * daemon that is a loop nobody runs.
		 */
		g_main_context_push_thread_default(self->main_context);
		g_subprocess_communicate_utf8_async(self->sentinel_child, NULL, NULL,
			on_sentinel_done, g_object_ref(self));
		g_main_context_pop_thread_default(self->main_context);

		started = TRUE;
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "started");
	json_builder_add_boolean_value(builder, started);
	json_builder_set_member_name(builder, "running");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/* What the last scan wrote, and whether another is running. */
static JsonNode *
read_report(
	ClawtDaemon *self,
	JsonNode    *request
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autofree gchar *path = report_path(self);
	g_autofree gchar *text = NULL;
	g_autofree gchar *unreadable = NULL;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		report = clawt_sentinel_report_from_yaml(text, &error);

		if (report == NULL)
			unreadable = g_strdup_printf("%s could not be read: %s", path,
				error->message);
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "path");
	json_builder_add_string_value(builder, path);
	json_builder_set_member_name(builder, "running");
	json_builder_add_boolean_value(builder, self->sentinel_child != NULL);

	if (self->sentinel_error != NULL || unreadable != NULL) {
		json_builder_set_member_name(builder, "error");
		json_builder_add_string_value(builder,
			unreadable != NULL ? unreadable : self->sentinel_error);
	}

	/*
	 * Absent when nothing has been scanned yet, which a client says in
	 * words; an empty report is a scan that found nothing, which is a
	 * different and better answer.
	 */
	if (report != NULL) {
		json_builder_set_member_name(builder, "report");
		json_builder_add_value(builder, g_steal_pointer(&report));
	}

	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_sentinel(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	(void)payload;

	*handled = TRUE;

	if (g_strcmp0(kind, "sentinel.scan") == 0)
		return start_scan(self, request);

	if (g_strcmp0(kind, "sentinel.report") == 0)
		return read_report(self, request);

	*handled = FALSE;
	return NULL;
}
