/*
 * clawt-wait.c - One condition an agent asked to be told about
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "wait/clawt-wait.h"
#include "chat/clawt-preamble.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

/* The base gap for each kind, before any backoff. */
#define WAIT_CHEAP_SECONDS (15)
#define WAIT_COSTLY_SECONDS (60)

/* How much of a quoted command output a notice carries. */
#define WAIT_QUOTE_BYTES (1500)

static void
wait_clear(
	gpointer data
){
	ClawtWait *self = data;

	g_free(self->id);
	g_free(self->agent_id);
	g_free(self->room_id);
	g_free(self->label);
	g_free(self->note);
	g_free(self->url);
	g_free(self->contains);
	g_free(self->path);
	g_free(self->task_id);
	g_free(self->command);
	g_free(self->last_result);
	g_free(self->quoted);
}

ClawtWait *
clawt_wait_new(
	ClawtWaitKind kind
){
	ClawtWait *self = g_atomic_rc_box_new0(ClawtWait);

	self->kind = kind;
	self->state = CLAWT_WAIT_PENDING;

	return self;
}

ClawtWait *
clawt_wait_ref(
	ClawtWait *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return g_atomic_rc_box_acquire(self);
}

void
clawt_wait_unref(
	ClawtWait *self
){
	if (self != NULL)
		g_atomic_rc_box_release_full(self, wait_clear);
}

/* ── Names ───────────────────────────────────────────────────────── */

/*
 * Both tables are walked by the nick functions, never copied: the tool's
 * parameter text, the stored form and the IPC reply all spell a kind the
 * same way because they all ask here.
 */
static const gchar *const kind_nicks[] = {
	"url", "file", "task", "computer", "command"
};

static const gchar *const state_nicks[] = {
	"pending", "satisfied", "expired", "cancelled", "failed"
};

const gchar *
clawt_wait_kind_nick(
	ClawtWaitKind kind
){
	if ((guint)kind >= G_N_ELEMENTS(kind_nicks))
		return "unknown";

	return kind_nicks[kind];
}

gboolean
clawt_wait_kind_from_nick(
	const gchar   *nick,
	ClawtWaitKind *out_kind
){
	guint i;

	g_return_val_if_fail(out_kind != NULL, FALSE);

	if (nick == NULL)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(kind_nicks); i++) {
		if (g_ascii_strcasecmp(nick, kind_nicks[i]) == 0) {
			*out_kind = (ClawtWaitKind)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
clawt_wait_state_nick(
	ClawtWaitState state
){
	if ((guint)state >= G_N_ELEMENTS(state_nicks))
		return "unknown";

	return state_nicks[state];
}

static gboolean
state_from_nick(
	const gchar    *nick,
	ClawtWaitState *out_state
){
	guint i;

	for (i = 0; nick != NULL && i < G_N_ELEMENTS(state_nicks); i++) {
		if (g_strcmp0(nick, state_nicks[i]) == 0) {
			*out_state = (ClawtWaitState)i;
			return TRUE;
		}
	}

	return FALSE;
}

/* ── Decisions ───────────────────────────────────────────────────── */

gint64
clawt_wait_clamp_hours(
	gint64    asked,
	gboolean *out_clamped
){
	if (out_clamped != NULL)
		*out_clamped = FALSE;

	if (asked <= 0)
		return CLAWT_WAIT_DEFAULT_HOURS;

	if (asked > CLAWT_WAIT_MAX_HOURS) {
		if (out_clamped != NULL)
			*out_clamped = TRUE;

		return CLAWT_WAIT_MAX_HOURS;
	}

	return asked;
}

guint
clawt_wait_interval_seconds(
	ClawtWaitKind kind,
	guint         failures
){
	guint base = WAIT_COSTLY_SECONDS;
	guint gap;
	guint i;

	/*
	 * Every kind named, no default: a kind added later must say what a
	 * look at it costs rather than inheriting the cheap answer.
	 */
	switch (kind) {
	case CLAWT_WAIT_FILE:
	case CLAWT_WAIT_TASK:
		base = WAIT_CHEAP_SECONDS;
		break;

	case CLAWT_WAIT_URL:
		base = CLAWT_WAIT_URL_MIN_SECONDS;
		break;

	case CLAWT_WAIT_COMPUTER:
	case CLAWT_WAIT_COMMAND:
		base = WAIT_COSTLY_SECONDS;
		break;
	}

	gap = base;

	/* Doubling by loop rather than shift, so a large count cannot overflow. */
	for (i = 0; i < failures && gap < CLAWT_WAIT_MAX_BACKOFF_SECONDS; i++)
		gap *= 2;

	if (gap > CLAWT_WAIT_MAX_BACKOFF_SECONDS)
		gap = CLAWT_WAIT_MAX_BACKOFF_SECONDS;

	if (kind == CLAWT_WAIT_URL && gap < CLAWT_WAIT_URL_MIN_SECONDS)
		gap = CLAWT_WAIT_URL_MIN_SECONDS;

	return gap;
}

/*
 * Case-insensitive containment, by casefolding both sides.
 *
 * A page that says "Shipped" satisfies a wait for "shipped": the agent
 * wrote the text from memory of what the page says, and a wait that
 * failed on capitalisation would run for a week and expire.
 */
static gboolean
contains_folded(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *valid = NULL;
	g_autofree gchar *folded_haystack = NULL;
	g_autofree gchar *folded_needle = NULL;

	if (needle == NULL || *needle == '\0')
		return TRUE;

	if (haystack == NULL)
		return FALSE;

	/* A page is not promised to be UTF-8; casefolding requires it. */
	valid = g_utf8_make_valid(haystack, -1);
	folded_haystack = g_utf8_casefold(valid, -1);
	folded_needle = g_utf8_casefold(needle, -1);

	return strstr(folded_haystack, folded_needle) != NULL;
}

ClawtWaitVerdict
clawt_wait_decide_url(
	ClawtWait   *self,
	guint        status,
	const gchar *body
){
	gboolean status_ok;

	g_return_val_if_fail(self != NULL, CLAWT_WAIT_UNANSWERED);

	if (status == 0)
		return CLAWT_WAIT_UNANSWERED;

	if (self->status > 0)
		status_ok = (status == (guint)self->status);
	else
		status_ok = (status >= 200 && status < 300);

	/*
	 * Overloaded or broken, unless that is the very answer wanted.  A
	 * wait for "the 503 page goes away" asks for a 200 and is backed
	 * off while it gets 503s, which is the polite thing; a wait for
	 * "it starts returning 503" asks for 503 and gets it.
	 */
	if (!status_ok && (status == 429 || status >= 500))
		return CLAWT_WAIT_UNANSWERED;

	if (!status_ok)
		return CLAWT_WAIT_NOT_YET;

	return contains_folded(body, self->contains)
		? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;
}

ClawtWaitVerdict
clawt_wait_decide_file(
	ClawtWait *self,
	gboolean   exists,
	gint64     size,
	gint64     mtime
){
	g_return_val_if_fail(self != NULL, CLAWT_WAIT_UNANSWERED);

	if (!self->on_change)
		return exists ? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;

	if (!exists)
		return CLAWT_WAIT_NOT_YET;

	if (!self->baseline_exists)
		return CLAWT_WAIT_MET;

	return (size != self->baseline_size || mtime != self->baseline_mtime)
		? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;
}

ClawtWaitVerdict
clawt_wait_decide_task(
	ClawtWait *self,
	gboolean   known,
	gboolean   finished
){
	g_return_val_if_fail(self != NULL, CLAWT_WAIT_UNANSWERED);

	if (!known)
		return CLAWT_WAIT_IMPOSSIBLE;

	return finished ? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;
}

ClawtWaitVerdict
clawt_wait_decide_exec(
	ClawtWait *self,
	gboolean   ran,
	gint       exit_status
){
	g_return_val_if_fail(self != NULL, CLAWT_WAIT_UNANSWERED);

	if (self->kind == CLAWT_WAIT_COMPUTER)
		return (ran && exit_status == 0) ? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;

	if (!ran)
		return CLAWT_WAIT_UNANSWERED;

	return (exit_status == self->exit_status)
		? CLAWT_WAIT_MET : CLAWT_WAIT_NOT_YET;
}

/* ── Files ───────────────────────────────────────────────────────── */

gchar *
clawt_wait_path_refusal(
	const gchar *path
){
	g_auto(GStrv) parts = NULL;
	guint i;

	if (path == NULL || *path == '\0')
		return g_strdup("A path is needed, relative to your workspace.");

	if (g_path_is_absolute(path) || path[0] == '~')
		return g_strdup_printf(
			"%s is not inside your workspace. Give the path relative to "
			"it -- build/out.log, not /home/.../build/out.log.", path);

	parts = g_strsplit(path, "/", -1);

	for (i = 0; parts[i] != NULL; i++) {
		if (g_strcmp0(parts[i], "..") == 0)
			return g_strdup_printf(
				"%s climbs out of your workspace with '..'. Only files "
				"inside it can be watched.", path);
	}

	return NULL;
}

gboolean
clawt_wait_stat_in_workspace(
	const gchar  *workspace,
	const gchar  *path,
	gboolean     *out_exists,
	gint64       *out_size,
	gint64       *out_mtime,
	GError      **error
){
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *resolved = NULL;
	GStatBuf info;

	g_return_val_if_fail(out_exists != NULL, FALSE);
	g_return_val_if_fail(out_size != NULL, FALSE);
	g_return_val_if_fail(out_mtime != NULL, FALSE);

	*out_exists = FALSE;
	*out_size = 0;
	*out_mtime = 0;

	refusal = clawt_wait_path_refusal(path);

	if (refusal != NULL) {
		g_set_error_literal(error, CLAWT_ERROR,
		                    CLAWT_ERROR_PERMISSION_DENIED, refusal);
		return FALSE;
	}

	if (workspace == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		                    "there is no workspace to look in");
		return FALSE;
	}

	root = realpath(workspace, NULL);

	if (root == NULL) {
		gint saved = errno;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "the workspace %s cannot be read: %s", workspace,
		            g_strerror(saved));
		return FALSE;
	}

	joined = g_build_filename(root, path, NULL);
	resolved = realpath(joined, NULL);

	if (resolved == NULL) {
		gint saved = errno;

		/*
		 * Not there -- or a link to somewhere not there, which from
		 * inside the workspace is the same answer and reveals nothing.
		 */
		if (saved == ENOENT || saved == ENOTDIR)
			return TRUE;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "%s cannot be examined: %s", path, g_strerror(saved));
		return FALSE;
	}

	/*
	 * Inside means the root itself or below it with a separator -- a
	 * bare prefix test would let /agents/kudu-evil pass for
	 * /agents/kudu.
	 */
	if (g_strcmp0(resolved, root) != 0) {
		g_autofree gchar *prefix = g_strconcat(root, "/", NULL);

		if (!g_str_has_prefix(resolved, prefix)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
			            "%s leads out of the workspace, so it cannot be "
			            "watched", path);
			return FALSE;
		}
	}

	if (g_stat(resolved, &info) != 0) {
		gint saved = errno;

		if (saved == ENOENT)
			return TRUE;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "%s cannot be examined: %s", path, g_strerror(saved));
		return FALSE;
	}

	*out_exists = TRUE;
	*out_size = (gint64)info.st_size;
	*out_mtime = (gint64)info.st_mtim.tv_sec * G_USEC_PER_SEC +
	             (gint64)(info.st_mtim.tv_nsec / 1000);

	return TRUE;
}

/* ── Words ───────────────────────────────────────────────────────── */

gchar *
clawt_wait_describe(
	ClawtWait *self
){
	GString *out;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new(NULL);

	switch (self->kind) {
	case CLAWT_WAIT_URL:
		g_string_append(out, self->url != NULL ? self->url : "(no URL)");

		if (self->status > 0)
			g_string_append_printf(out, " answers %d", self->status);
		else
			g_string_append(out, " answers 2xx");

		if (self->contains != NULL && *self->contains != '\0')
			g_string_append_printf(out, " and contains \"%s\"",
			                       self->contains);
		break;

	case CLAWT_WAIT_FILE:
		g_string_append_printf(out, "%s %s",
		                       self->path != NULL ? self->path : "(no path)",
		                       self->on_change ? "changes" : "exists");
		break;

	case CLAWT_WAIT_TASK:
		g_string_append_printf(out, "task %s settles",
		                       self->task_id != NULL ? self->task_id : "?");
		break;

	case CLAWT_WAIT_COMPUTER:
		g_string_append(out, "the computer answers");
		break;

	case CLAWT_WAIT_COMMAND:
		g_string_append_printf(out, "`%s` exits %d",
		                       self->command != NULL ? self->command : "?",
		                       self->exit_status);
		break;
	}

	return g_string_free(out, FALSE);
}

/*
 * How long something ran, for a notice: "3h", "2d 4h", "12m".
 */
static gchar *
duration_words(
	gint64 microseconds
){
	gint64 minutes = microseconds / G_USEC_PER_SEC / 60;

	if (minutes < 1)
		return g_strdup("under a minute");

	if (minutes < 60)
		return g_strdup_printf("%" G_GINT64_FORMAT "m", minutes);

	if (minutes < 24 * 60)
		return g_strdup_printf("%" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
		                       minutes / 60, minutes % 60);

	return g_strdup_printf("%" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
	                       minutes / (24 * 60), (minutes / 60) % 24);
}

gchar *
clawt_wait_compose_notice(
	ClawtWait *self,
	gint64     now
){
	g_autoptr(GString) body = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *condition = NULL;
	g_autofree gchar *escaped_condition = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *note = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *waited = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	/*
	 * Everything quoted is escaped, because none of it is the daemon's:
	 * the label and the note are what the agent wrote, the condition
	 * repeats a URL and a command it chose, and what the probe saw is a
	 * page or a program's output.  The daemon's own sentences around
	 * them keep their marker.
	 */
	label = clawt_preamble_escape(self->label != NULL ? self->label : "");
	condition = clawt_wait_describe(self);
	escaped_condition = clawt_preamble_escape(condition);
	seen = clawt_preamble_escape(self->last_result);
	note = clawt_preamble_escape(self->note);

	if (self->quoted != NULL && *self->quoted != '\0') {
		g_autofree gchar *clipped =
			clawt_utf8_truncate(self->quoted, WAIT_QUOTE_BYTES, TRUE);

		quoted = clawt_preamble_escape(clipped);
	}

	waited = duration_words(now - self->created_at);
	body = g_string_new(NULL);

	/*
	 * Every end named, no default: -Wswitch is what catches a state
	 * added later and reported as nothing.
	 */
	switch (self->state) {
	case CLAWT_WAIT_PENDING:
		g_string_append_printf(body,
			"[clawtilla] Your wait %s is still pending.", self->id);
		return g_string_free(g_steal_pointer(&body), FALSE);

	case CLAWT_WAIT_SATISFIED:
		g_string_append_printf(body,
			"[clawtilla] The wait you set is over: %s\n\n"
			"Condition met after %s: %s\n",
			label, waited, escaped_condition);
		break;

	case CLAWT_WAIT_EXPIRED:
		g_string_append_printf(body,
			"[clawtilla] Your wait expired before its condition held: "
			"%s\n\nclawtilla stopped checking after %s. The condition was: "
			"%s\n",
			label, waited, escaped_condition);
		break;

	case CLAWT_WAIT_CANCELLED:
		g_string_append_printf(body,
			"[clawtilla] Your operator cancelled your wait: %s\n\n"
			"Nothing is checking %s any more.\n",
			label, escaped_condition);
		break;

	case CLAWT_WAIT_FAILED:
		g_string_append_printf(body,
			"[clawtilla] Your wait ended without its condition holding: "
			"%s\n\nThe condition was %s, and it can no longer be checked.\n",
			label, escaped_condition);
		break;
	}

	if (seen != NULL && *seen != '\0')
		g_string_append_printf(body, "Last check saw: %s\n", seen);

	if (quoted != NULL)
		g_string_append_printf(body, "Output:\n%s\n", quoted);

	if (note != NULL && *note != '\0')
		g_string_append_printf(body,
			"\nWhen you set it, you asked to be reminded:\n%s\n", note);

	g_string_append_printf(body,
		"\n[clawtilla] This was wait %s. If your operator asked to be told, "
		"tell them now with clawtilla_message_user. This notice closes "
		"the exchange -- nothing you write at the end of this turn is sent "
		"anywhere.", self->id);

	return g_string_free(g_steal_pointer(&body), FALSE);
}

/* ── The stored form ─────────────────────────────────────────────── */

static void
add_string(
	JsonBuilder *builder,
	const gchar *name,
	const gchar *value
){
	if (value == NULL)
		return;

	json_builder_set_member_name(builder, name);
	json_builder_add_string_value(builder, value);
}

static void
add_int(
	JsonBuilder *builder,
	const gchar *name,
	gint64       value
){
	json_builder_set_member_name(builder, name);
	json_builder_add_int_value(builder, value);
}

JsonNode *
clawt_wait_to_json(
	ClawtWait *self
){
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	add_string(builder, "id", self->id);
	add_string(builder, "agent", self->agent_id);
	add_string(builder, "room", self->room_id);
	add_string(builder, "label", self->label);
	add_string(builder, "note", self->note);
	add_string(builder, "kind", clawt_wait_kind_nick(self->kind));
	add_string(builder, "state", clawt_wait_state_nick(self->state));

	add_string(builder, "url", self->url);
	add_int(builder, "status", self->status);
	add_string(builder, "contains", self->contains);

	add_string(builder, "path", self->path);
	json_builder_set_member_name(builder, "on_change");
	json_builder_add_boolean_value(builder, self->on_change);
	json_builder_set_member_name(builder, "baseline_exists");
	json_builder_add_boolean_value(builder, self->baseline_exists);
	add_int(builder, "baseline_size", self->baseline_size);
	add_int(builder, "baseline_mtime", self->baseline_mtime);

	add_string(builder, "task", self->task_id);

	add_string(builder, "command", self->command);
	add_int(builder, "exit_status", self->exit_status);

	add_int(builder, "created_at", self->created_at);
	add_int(builder, "expires_at", self->expires_at);
	add_int(builder, "last_check_at", self->last_check_at);
	add_int(builder, "next_check_at", self->next_check_at);
	add_int(builder, "ended_at", self->ended_at);
	add_int(builder, "checks", (gint64)self->checks);
	add_int(builder, "failures", (gint64)self->failures);
	add_string(builder, "last_result", self->last_result);
	add_string(builder, "quoted", self->quoted);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static gchar *
dup_member(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return g_strdup(json_node_get_string(node));
}

static gint64
int_member(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
	    json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;

	return json_node_get_int(node);
}

static gboolean
boolean_member(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
	    json_node_get_value_type(node) != G_TYPE_BOOLEAN)
		return FALSE;

	return json_node_get_boolean(node);
}

ClawtWait *
clawt_wait_from_json(
	JsonNode  *node,
	GError   **error
){
	g_autoptr(ClawtWait) wait = NULL;
	g_autofree gchar *kind_nick = NULL;
	g_autofree gchar *state_nick = NULL;
	ClawtWaitKind kind;
	ClawtWaitState state = CLAWT_WAIT_PENDING;
	JsonObject *object;

	/* A type check is not a pointer check: see AGENTS.md, "The wire". */
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) ||
	    json_node_get_object(node) == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a wait record is not a JSON object");
		return NULL;
	}

	object = json_node_get_object(node);
	kind_nick = dup_member(object, "kind");

	if (!clawt_wait_kind_from_nick(kind_nick, &kind)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "a wait record has an unknown kind '%s'",
		            kind_nick != NULL ? kind_nick : "(none)");
		return NULL;
	}

	state_nick = dup_member(object, "state");

	if (state_nick != NULL && !state_from_nick(state_nick, &state)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "a wait record has an unknown state '%s'", state_nick);
		return NULL;
	}

	wait = clawt_wait_new(kind);
	wait->state = state;
	wait->id = dup_member(object, "id");
	wait->agent_id = dup_member(object, "agent");
	wait->room_id = dup_member(object, "room");
	wait->label = dup_member(object, "label");
	wait->note = dup_member(object, "note");

	wait->url = dup_member(object, "url");
	wait->status = (gint)int_member(object, "status");
	wait->contains = dup_member(object, "contains");

	wait->path = dup_member(object, "path");
	wait->on_change = boolean_member(object, "on_change");
	wait->baseline_exists = boolean_member(object, "baseline_exists");
	wait->baseline_size = int_member(object, "baseline_size");
	wait->baseline_mtime = int_member(object, "baseline_mtime");

	wait->task_id = dup_member(object, "task");

	wait->command = dup_member(object, "command");
	wait->exit_status = (gint)int_member(object, "exit_status");

	wait->created_at = int_member(object, "created_at");
	wait->expires_at = int_member(object, "expires_at");
	wait->last_check_at = int_member(object, "last_check_at");
	wait->next_check_at = int_member(object, "next_check_at");
	wait->ended_at = int_member(object, "ended_at");
	wait->checks = (guint)int_member(object, "checks");
	wait->failures = (guint)int_member(object, "failures");
	wait->last_result = dup_member(object, "last_result");
	wait->quoted = dup_member(object, "quoted");

	if (wait->id == NULL || wait->agent_id == NULL || wait->room_id == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a wait record has no id, agent or room");
		return NULL;
	}

	return g_steal_pointer(&wait);
}
