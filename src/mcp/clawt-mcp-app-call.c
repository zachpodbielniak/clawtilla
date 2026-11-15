/*
 * clawt-mcp-app-call.c - An MCP app calling its own server's tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's MCP client for one app-initiated call: start the relay,
 * initialize, list the tools, check the one asked for is visible to
 * apps, call it, stop the relay.  A small state machine over the relay's
 * stdio, every wait asynchronous and every read bounded.
 */

#include "clawtilla.h"
#include "mcp/clawt-mcp-app-call.h"

#include <string.h>

/* How much is read from the relay at a time. */
#define READ_CHUNK (65536)

/* How many tools/list pages are followed before the list is cut. */
#define MAX_LIST_PAGES (20)

/* How long a relay is given to exit after its stdin closes. */
#define REAP_GRACE_SECONDS (3)

typedef enum {
	STAGE_INITIALIZE = 0,
	STAGE_LIST,
	STAGE_CALL
} Stage;

typedef struct {
	GSubprocess   *process;
	GOutputStream *in;
	GInputStream  *out;
	GString       *buffer;
	gchar         *tool;
	JsonNode      *arguments;
	JsonObject    *found;       /* the tool's definition, once listed */
	Stage          stage;
	gint64         waiting_id;
	guint          pages;
	GSource       *timer;
	GCancellable  *cancellable; /* ours, cancelled when the call ends */
	gboolean       done;

	/*
	 * Lines waiting to be written, and the one being written: a stream
	 * takes one outstanding write at a time, and `initialized` and
	 * `tools/list` go out back to back.
	 */
	GString       *outbox;
	gchar         *writing;
} CallJob;

static void
call_job_free(
	gpointer data
){
	CallJob *job = data;

	if (job->timer != NULL) {
		g_source_destroy(job->timer);
		g_source_unref(job->timer);
	}

	g_clear_object(&job->in);
	g_clear_object(&job->out);
	g_clear_object(&job->process);
	g_clear_object(&job->cancellable);
	if (job->buffer != NULL)
		g_string_free(job->buffer, TRUE);
	if (job->outbox != NULL)
		g_string_free(job->outbox, TRUE);
	g_free(job->writing);
	g_free(job->tool);
	g_clear_pointer(&job->arguments, json_node_unref);
	g_clear_pointer(&job->found, json_object_unref);
	g_free(job);
}

/* ── Stopping the relay ──────────────────────────────────────────── */

/*
 * The relay is given its stdin's end, which is how it is told to stop,
 * and a few seconds to do so before it is killed.  Held on its own, so
 * the call's answer does not wait for a relay to exit.
 */
static gboolean
on_reap_timeout(
	gpointer data
){
	g_subprocess_force_exit(G_SUBPROCESS(data));

	return G_SOURCE_REMOVE;
}

static void
on_reaped(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	GSource *timer = data;

	g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	g_source_destroy(timer);
	g_source_unref(timer);
}

static void
stop_relay(
	CallJob      *job,
	GMainContext *context
){
	GSource *timer;

	if (job->process == NULL)
		return;

	if (job->in != NULL)
		g_output_stream_close(job->in, NULL, NULL);

	timer = g_timeout_source_new_seconds(REAP_GRACE_SECONDS);
	g_source_set_callback(timer, on_reap_timeout,
		g_object_ref(job->process), g_object_unref);
	g_source_attach(timer, context);

	/*
	 * The wait takes the thread-default context, and a callback is not
	 * dispatched with its own context pushed -- so it is pushed here,
	 * where the source attaches.
	 */
	g_main_context_push_thread_default(context);
	g_subprocess_wait_async(job->process, NULL, on_reaped, timer);
	g_main_context_pop_thread_default(context);
}

/* The call's end, either way, once. */
static void
finish(
	GTask      *task,
	JsonObject *result,
	GError     *error
){
	CallJob *job = g_task_get_task_data(task);

	if (job->done) {
		if (error != NULL)
			g_error_free(error);
		if (result != NULL)
			json_object_unref(result);
		return;
	}

	job->done = TRUE;
	g_cancellable_cancel(job->cancellable);
	stop_relay(job, g_task_get_context(task));

	/* The timer holds the task; destroying it breaks that cycle. */
	if (job->timer != NULL) {
		g_source_destroy(job->timer);
		g_clear_pointer(&job->timer, g_source_unref);
	}

	if (error != NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, result,
			(GDestroyNotify)json_object_unref);
}

/* ── Writing ─────────────────────────────────────────────────────── */

static void flush_outbox(GTask *task);

static void
on_written(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	g_autoptr(GTask) task = data;
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;

	g_clear_pointer(&job->writing, g_free);

	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
			NULL, &error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			finish(task, NULL, g_error_new(CLAWT_ERROR, CLAWT_ERROR_FAILED,
				"could not write to the connector's relay: %s",
				error->message));
		return;
	}

	if (!job->done)
		flush_outbox(task);
}

/* Starts writing whatever is waiting, unless a write is under way. */
static void
flush_outbox(
	GTask *task
){
	CallJob *job = g_task_get_task_data(task);

	if (job->writing != NULL || job->outbox->len == 0)
		return;

	job->writing = g_string_free(job->outbox, FALSE);
	job->outbox = g_string_new(NULL);

	/* Pushed where the write is started: callbacks are not. */
	g_main_context_push_thread_default(g_task_get_context(task));
	g_output_stream_write_all_async(job->in, job->writing,
		strlen(job->writing), G_PRIORITY_DEFAULT, job->cancellable,
		on_written, g_object_ref(task));
	g_main_context_pop_thread_default(g_task_get_context(task));
}

/* One message to the relay, as one line, in order. */
static void
send_message(
	GTask      *task,
	JsonObject *message
){
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;

	json_node_set_object(node, message);
	text = json_to_string(node, FALSE);
	g_string_append(job->outbox, text);
	g_string_append_c(job->outbox, '\n');

	flush_outbox(task);
}

static JsonObject *
request_new(
	gint64       id,
	const gchar *method
){
	JsonObject *message = json_object_new();

	json_object_set_string_member(message, "jsonrpc", "2.0");
	json_object_set_int_member(message, "id", id);
	json_object_set_string_member(message, "method", method);

	return message;
}

static void
send_initialize(
	GTask *task
){
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(JsonObject) message = request_new(1, "initialize");
	JsonObject *params = json_object_new();
	JsonObject *info = json_object_new();

	json_object_set_string_member(params, "protocolVersion", "2025-06-18");
	json_object_set_object_member(params, "capabilities", json_object_new());
	json_object_set_string_member(info, "name", "clawtilla-app-host");
	json_object_set_string_member(info, "version", CLAWT_VERSION_STRING);
	json_object_set_object_member(params, "clientInfo", info);
	json_object_set_object_member(message, "params", params);

	job->stage = STAGE_INITIALIZE;
	job->waiting_id = 1;
	send_message(task, message);
}

static void
send_list(
	GTask       *task,
	const gchar *cursor
){
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(JsonObject) message = NULL;

	job->waiting_id++;
	message = request_new(job->waiting_id, "tools/list");

	if (cursor != NULL) {
		JsonObject *params = json_object_new();

		json_object_set_string_member(params, "cursor", cursor);
		json_object_set_object_member(message, "params", params);
	}

	job->stage = STAGE_LIST;
	job->pages++;
	send_message(task, message);
}

static void
send_call(
	GTask *task
){
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(JsonObject) message = NULL;
	JsonObject *params = json_object_new();

	job->waiting_id++;
	message = request_new(job->waiting_id, "tools/call");
	json_object_set_string_member(params, "name", job->tool);

	if (job->arguments != NULL)
		json_object_set_member(params, "arguments",
			json_node_copy(job->arguments));
	else
		json_object_set_object_member(params, "arguments", json_object_new());

	json_object_set_object_member(message, "params", params);

	job->stage = STAGE_CALL;
	send_message(task, message);
}

/* ── Reading ─────────────────────────────────────────────────────── */

/* A tools/list page: the asked-for tool, if it is on it; FALSE at the end. */
static gboolean
take_page(
	CallJob     *job,
	JsonObject  *result,
	gchar      **cursor
){
	JsonArray *tools;
	guint i;

	*cursor = NULL;

	if (json_object_has_member(result, "tools") &&
		JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "tools"))) {
		tools = json_object_get_array_member(result, "tools");

		for (i = 0; i < json_array_get_length(tools); i++) {
			JsonNode *element = json_array_get_element(tools, i);
			JsonObject *tool = JSON_NODE_HOLDS_OBJECT(element)
				? json_node_get_object(element) : NULL;

			if (tool != NULL && job->found == NULL &&
				g_strcmp0(json_object_get_string_member_with_default(tool,
					"name", NULL), job->tool) == 0)
				job->found = json_object_ref(tool);
		}
	}

	if (job->found == NULL &&
		json_object_has_member(result, "nextCursor") &&
		JSON_NODE_HOLDS_VALUE(json_object_get_member(result, "nextCursor")) &&
		json_node_get_value_type(json_object_get_member(result,
			"nextCursor")) == G_TYPE_STRING)
		*cursor = g_strdup(json_object_get_string_member(result,
			"nextCursor"));

	return *cursor != NULL;
}

/* One complete line from the relay. */
static void
take_line(
	GTask       *task,
	const gchar *line
){
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *message;
	JsonNode *root;

	if (!json_parser_load_from_data(parser, line, -1, NULL) ||
		(root = json_parser_get_root(parser)) == NULL ||
		!JSON_NODE_HOLDS_OBJECT(root) ||
		(message = json_node_get_object(root)) == NULL)
		return;

	/* Only the answer to what was asked: notifications pass by. */
	if (!json_object_has_member(message, "id") ||
		json_object_has_member(message, "method") ||
		!JSON_NODE_HOLDS_VALUE(json_object_get_member(message, "id")) ||
		json_node_get_value_type(json_object_get_member(message, "id")) !=
			G_TYPE_INT64 ||
		json_object_get_int_member(message, "id") != job->waiting_id)
		return;

	if (json_object_has_member(message, "error")) {
		JsonObject *error = JSON_NODE_HOLDS_OBJECT(
			json_object_get_member(message, "error"))
			? json_object_get_object_member(message, "error") : NULL;

		finish(task, NULL, g_error_new(CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"the server refused: %s", error != NULL
				? json_object_get_string_member_with_default(error, "message",
					"no reason given")
				: "no reason given"));
		return;
	}

	if (!json_object_has_member(message, "result") ||
		!JSON_NODE_HOLDS_OBJECT(json_object_get_member(message, "result"))) {
		finish(task, NULL, g_error_new_literal(CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"the relay answered with no result"));
		return;
	}

	switch (job->stage) {
	case STAGE_INITIALIZE: {
		g_autoptr(JsonObject) initialized = json_object_new();

		json_object_set_string_member(initialized, "jsonrpc", "2.0");
		json_object_set_string_member(initialized, "method",
			"notifications/initialized");
		send_message(task, initialized);
		send_list(task, NULL);
		return;
	}

	case STAGE_LIST: {
		g_autofree gchar *cursor = NULL;

		if (take_page(job, json_object_get_object_member(message, "result"),
				&cursor) && job->pages < MAX_LIST_PAGES) {
			send_list(task, cursor);
			return;
		}

		/*
		 * Refused here before anything is sent: a tool the server does
		 * not offer, and one it does not let its apps call.  The relay
		 * refuses the second as well; this says so in a sentence and
		 * never puts the call on the wire.
		 */
		if (job->found == NULL) {
			finish(task, NULL, g_error_new(CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
				"the server offers no tool called '%s' that this agent may "
				"use", job->tool));
			return;
		}

		if (!clawt_mcp_app_tool_app_visible(job->found)) {
			finish(task, NULL, g_error_new(CLAWT_ERROR,
				CLAWT_ERROR_PERMISSION_DENIED,
				"'%s' is not one of the tools the server lets its apps call",
				job->tool));
			return;
		}

		send_call(task);
		return;
	}

	case STAGE_CALL:
		finish(task, json_object_ref(json_object_get_object_member(message,
			"result")), NULL);
		return;
	}
}

static void read_more(GTask *task);

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	g_autoptr(GTask) task = data;
	CallJob *job = g_task_get_task_data(task);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *chunk;
	gsize length = 0;
	gchar *newline;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
		&error);

	if (job->done)
		return;

	if (bytes == NULL) {
		finish(task, NULL, g_error_new(CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"could not read the connector's relay: %s", error->message));
		return;
	}

	chunk = g_bytes_get_data(bytes, &length);

	if (length == 0) {
		finish(task, NULL, g_error_new_literal(CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"the connector's relay ended before it answered; the daemon's "
			"log has what it said"));
		return;
	}

	g_string_append_len(job->buffer, chunk, (gssize)length);

	/* Every whole line, in order; the partial tail stays. */
	while (!job->done &&
		(newline = memchr(job->buffer->str, '\n', job->buffer->len)) != NULL) {
		gsize line_length = (gsize)(newline - job->buffer->str);
		g_autofree gchar *line = g_strndup(job->buffer->str, line_length);

		g_string_erase(job->buffer, 0, (gssize)(line_length + 1));
		take_line(task, line);
	}

	if (job->done)
		return;

	/* Bounded as it accumulates, not measured after. */
	if (job->buffer->len > CLAWT_MCP_APP_CALL_MAX_BYTES) {
		finish(task, NULL, g_error_new(CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"the answer is larger than an app call may carry (%d bytes)",
			CLAWT_MCP_APP_CALL_MAX_BYTES));
		return;
	}

	read_more(task);
}

static void
read_more(
	GTask *task
){
	CallJob *job = g_task_get_task_data(task);

	/* Pushed where the read is started; see stop_relay(). */
	g_main_context_push_thread_default(g_task_get_context(task));
	g_input_stream_read_bytes_async(job->out, READ_CHUNK, G_PRIORITY_DEFAULT,
		job->cancellable, on_read, g_object_ref(task));
	g_main_context_pop_thread_default(g_task_get_context(task));
}

static gboolean
on_timeout(
	gpointer data
){
	GTask *task = data;
	CallJob *job = g_task_get_task_data(task);

	g_clear_pointer(&job->timer, g_source_unref);
	finish(task, NULL, g_error_new_literal(CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
		"the app's call took longer than its limit; an approval it was "
		"waiting for can still be answered, but the app has been told it "
		"failed"));

	return G_SOURCE_REMOVE;
}

void
clawt_mcp_app_call_async(
	const gchar * const  *argv,
	const gchar          *tool,
	JsonNode             *arguments,
	guint                 timeout_seconds,
	GCancellable         *cancellable,
	GAsyncReadyCallback   callback,
	gpointer              user_data
){
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	CallJob *job;

	g_return_if_fail(argv != NULL && argv[0] != NULL);
	g_return_if_fail(tool != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_source_tag(task, clawt_mcp_app_call_async);

	job = g_new0(CallJob, 1);
	job->tool = g_strdup(tool);
	job->arguments = (arguments != NULL) ? json_node_copy(arguments) : NULL;
	job->buffer = g_string_new(NULL);
	job->outbox = g_string_new(NULL);
	job->cancellable = g_cancellable_new();
	g_task_set_task_data(task, job, call_job_free);

	/*
	 * stderr is inherited: the relay's own diagnostics -- a connector
	 * that is not there, a credential that cannot be read -- belong in
	 * the daemon's log, which is where somebody looks after "the relay
	 * ended before it answered".
	 */
	job->process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);

	if (job->process == NULL) {
		job->done = TRUE;
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
			"could not start the connector's relay: %s", error->message);
		return;
	}

	job->in = g_object_ref(g_subprocess_get_stdin_pipe(job->process));
	job->out = g_object_ref(g_subprocess_get_stdout_pipe(job->process));

	/* The limit, on the context the task answers on. */
	job->timer = g_timeout_source_new_seconds(timeout_seconds > 0
		? timeout_seconds : CLAWT_MCP_APP_CALL_TIMEOUT_SECONDS);
	g_source_set_callback(job->timer, on_timeout, g_object_ref(task),
		g_object_unref);
	g_source_attach(job->timer, g_task_get_context(task));

	read_more(task);
	send_initialize(task);
}

JsonObject *
clawt_mcp_app_call_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(G_IS_TASK(result), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

gchar *
clawt_mcp_app_call_note(
	const gchar *connector,
	const gchar *title,
	const gchar *tool,
	const gchar *failure
){
	g_autofree gchar *safe_tool = clawt_preamble_escape_all(
		tool != NULL ? tool : "?");
	g_autofree gchar *safe_title = clawt_preamble_escape_all(
		title != NULL ? title : "an app");
	g_autofree gchar *safe_failure = NULL;
	g_autofree gchar *clipped = NULL;

	if (failure == NULL)
		return g_strdup_printf("App call: %s from %s called %s itself -- "
			"the app, not the model.", safe_title,
			connector != NULL ? connector : "a connector", safe_tool);

	clipped = clawt_clip_line(failure, 200);
	safe_failure = clawt_preamble_escape_all(clipped);

	return g_strdup_printf("App call: %s from %s asked to call %s itself -- "
		"the app, not the model -- and it did not run: %s", safe_title,
		connector != NULL ? connector : "a connector", safe_tool,
		safe_failure);
}
