/*
 * daemon-rebuild.c - An agent asking for its stuck computer to be rebuilt
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `clawtilla computer rebuild` is the operator's, and is refused while
 * the agent runs.  An agent whose machine is past repair -- a package
 * database it broke, a guest that stopped answering -- could only say so
 * in prose and hope somebody read it.  clawtilla_request_rebuild files
 * the request as a decision instead, shaped like clawtilla_request_hands:
 * it asks and it is never a handover.  The decision says, before anybody
 * presses a button, what a rebuild destroys; there is no automatic
 * replacement at all, because a machine kept on purpose
 * (`computer.container.keep`, a VM's disk) holds state that only a
 * person can decide is worth less than a working machine.
 *
 * Approval runs in this order, and the order is the point:
 *
 *   1. the agent's turns end -- recorded as interrupted, never rerun,
 *      since the person who approved knows why they stopped -- and the
 *      agent is stopped, so nothing is working on the machine;
 *   2. the machine is rebuilt through clawt_daemon_computer_rebuild(),
 *      the operator's own path, with the operator's own refusals;
 *   3. the agent is started again if it had been running, and told what
 *      happened in a notice that waits in its mailbox until it is.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"
#include "task/clawt-incident.h"

#include <string.h>

/* An approval waiting for its idle. */
typedef struct {
	gchar *decision_id;
	gchar *agent_id;
	gchar *loss;
} RebuildJob;

static void
rebuild_job_free(
	gpointer data
){
	RebuildJob *job = data;

	g_free(job->decision_id);
	g_free(job->agent_id);
	g_free(job->loss);
	g_free(job);
}

/* ── What a rebuild destroys ─────────────────────────────────────── */

gchar *
clawt_daemon_rebuild_loss(
	ClawtDaemon *self,
	const gchar *agent_id
){
	ClawtAgentConfig *config;
	ClawtComputerType type;
	g_autoptr(GString) loss = g_string_new(NULL);
	const gchar *shared;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	config = agent_id != NULL ? clawt_config_get_agent(self->config, agent_id)
	                          : NULL;
	if (config == NULL)
		return NULL;

	type = (ClawtComputerType)clawt_agent_config_get_enum(config,
		"computer.type");

	/*
	 * Said per backend, because "everything in it" is a different
	 * amount of work on each -- and the keep flags are named, since
	 * they are the reason a machine holds anything worth losing.
	 */
	switch (type) {
	case CLAWT_COMPUTER_CONTAINER:
		if (clawt_agent_config_get_boolean(config, "computer.container.keep"))
			g_string_append(loss,
				"the container and everything installed or written inside "
				"it since it was built. computer.container.keep is on, so it "
				"has been kept across every stop and may hold work that "
				"exists nowhere else.");
		else
			g_string_append(loss,
				"the container and whatever was written inside it since "
				"its last start. computer.container.keep is off, so a stop "
				"already removes it -- a rebuild loses no more than a "
				"restart would.");
		break;
	case CLAWT_COMPUTER_VM:
		g_string_append(loss,
			"the VM's disk and everything on it: whatever was installed, "
			"configured or written inside the guest since it was built. "
			"The disk is kept across every stop, so it may hold work that "
			"exists nowhere else.");
		break;
	case CLAWT_COMPUTER_DISTROBOX:
		if (clawt_agent_config_get_boolean(config, "computer.distrobox.keep"))
			g_string_append(loss,
				"the distrobox and every package installed into it. "
				"computer.distrobox.keep is on, so it has been kept across "
				"every stop. The home directory it shares with this machine "
				"is not touched.");
		else
			g_string_append(loss,
				"the distrobox and the packages installed since its last "
				"start; computer.distrobox.keep is off, so a stop already "
				"removes it. The home directory it shares with this machine "
				"is not touched.");
		break;
	case CLAWT_COMPUTER_NONE:
	case CLAWT_COMPUTER_HOST:
	default:
		return NULL;
	}

	g_string_append(loss, " Folders mounted in from this machine are not "
	                      "touched.");

	/* A shared machine is everybody's on it. */
	shared = clawt_daemon_shared_computer_id(self, agent_id);
	if (shared != NULL) {
		g_autoptr(GPtrArray) others = clawt_daemon_shared_users(self, shared,
			FALSE, agent_id);
		guint i;

		if (others->len > 0) {
			g_string_append_printf(loss, " The machine is '%s', shared with ",
				shared);
			for (i = 0; i < others->len; i++)
				g_string_append_printf(loss, "%s%s", i > 0 ? ", " : "",
					(const gchar *)g_ptr_array_index(others, i));
			g_string_append(loss, ": they lose it too, and it is refused "
			                      "while any of them is running.");
		}
	}

	return g_string_free(g_steal_pointer(&loss), FALSE);
}

/* ── Filing the request ──────────────────────────────────────────── */

/* The open rebuild request @agent_id already has, if any. */
static gchar *
open_request_for(
	ClawtDaemon *self,
	const gchar *agent_id
){
	g_autoptr(GPtrArray) open = NULL;
	guint i;

	if (self->decisions == NULL)
		return NULL;

	open = clawt_decision_store_list(self->decisions, TRUE);
	for (i = 0; open != NULL && i < open->len; i++) {
		ClawtDecision *decision = g_ptr_array_index(open, i);

		if (g_str_has_prefix(clawt_decision_get_id(decision),
				CLAWT_REBUILD_DECISION_PREFIX) &&
			g_strcmp0(clawt_decision_get_agent(decision), agent_id) == 0)
			return g_strdup(clawt_decision_get_id(decision));
	}

	return NULL;
}

gchar *
clawt_daemon_rebuild_request(
	ClawtDaemon  *self,
	const gchar  *agent_id,
	const gchar  *reason,
	GError      **error
){
	static const gchar * const options[] = { "approve", "decline", NULL };
	g_autofree gchar *loss = NULL;
	g_autofree gchar *already = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *fenced = NULL;
	g_autofree gchar *question = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *stored = NULL;
	g_autoptr(ClawtDecision) decision = NULL;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	if (self->decisions == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
			"This daemon keeps no decision inbox to ask in.");
		return NULL;
	}

	if (reason == NULL || *reason == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			"Say why the machine needs rebuilding.");
		return NULL;
	}

	loss = clawt_daemon_rebuild_loss(self, agent_id);
	if (loss == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
			"You have no machine of your own to rebuild: a host agent runs "
			"on this one, and an agent with no computer has nothing to "
			"replace.");
		return NULL;
	}

	/*
	 * One at a time.  A second request is the same question asked
	 * again, and an inbox with two copies of it is one where a person
	 * approves the first and then wonders what the second wants.
	 */
	already = open_request_for(self, agent_id);
	if (already != NULL)
		return g_strdup_printf(
			"You already asked (decision %s), and it is still waiting for "
			"your operator. Finish your turn and say you are waiting; you "
			"will be told the answer.", already);

	/*
	 * The agent's words are quoted inside the daemon's question, so
	 * they are escaped -- a reason ending in a forged `[clawtilla]` line
	 * must not read as the daemon -- and fenced with a fence they
	 * cannot close.  The loss comes last and after the marker, which
	 * is where the clients find it to draw on its own.
	 */
	escaped = clawt_preamble_escape(reason);
	fenced = clawt_incident_fence(NULL, escaped);
	question = g_strdup_printf(
		"%s asks for its computer to be rebuilt: destroyed, and built "
		"fresh from the configuration.\n\nIts reason, in its own words:\n"
		"%s\nApprove stops %s's current turn, rebuilds the machine and "
		"then tells %s. Decline leaves the machine as it is."
		CLAWT_REBUILD_LOSS_MARKER "%s",
		agent_id, fenced, agent_id, agent_id, loss);

	uuid = g_uuid_string_random();
	id = g_strconcat(CLAWT_REBUILD_DECISION_PREFIX, uuid, NULL);
	decision = clawt_decision_new(id, agent_id, question);
	clawt_decision_set_options(decision, options);

	/*
	 * The default is to leave the machine alone, and it never takes
	 * effect by itself: no deadline is set, so nothing settles this but
	 * a person.  A rebuild that happened because nobody answered is the
	 * automatic replacement this whole design refuses.
	 */
	clawt_decision_set_default(decision, "decline",
		"nobody approved it, so the machine is left as it is");

	stored = clawt_decision_store_post(self->decisions, decision, error);
	if (stored == NULL)
		return NULL;

	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "decision.asked", agent_id);

	return g_strdup_printf(
		"Asked, as decision %s. Your operator sees why, and what a "
		"rebuild would destroy: %s\n\nNothing has been rebuilt. Finish "
		"your turn and say you are waiting -- if they approve, your turn "
		"is stopped, the machine is rebuilt and you are told; if they "
		"decline, you are told that too.", stored, loss);
}

static gchar *
request_rebuild_for_tools(
	const gchar  *agent_id,
	const gchar  *reason,
	gpointer      user_data,
	GError      **error
){
	return clawt_daemon_rebuild_request(CLAWT_DAEMON(user_data), agent_id,
		reason, error);
}

/* ── Answering it ────────────────────────────────────────────────── */

/* A notice to the agent, waiting in its mailbox if it is not running. */
static void
tell_agent(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *body
){
	g_autoptr(GError) error = NULL;

	if (self->router == NULL)
		return;

	if (clawt_mailbox_router_send_to(self->router, CLAWT_SYSTEM_SENDER,
			agent_id, body, NULL, 0, &error) < 0)
		g_warning("rebuild: could not tell %s: %s", agent_id,
			error != NULL ? error->message : "unknown");
}

/*
 * One approved rebuild, in the order the header gives.  Each step is
 * logged, because a rebuild that went wrong is reconstructed from the
 * log and the agent's notice, and both have to say where it stopped.
 */
static void
run_rebuild(
	ClawtDaemon *self,
	RebuildJob  *job
){
	ClawtAgent *agent = clawt_agent_manager_get(self->agents, job->agent_id);
	g_autofree gchar *note = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	gboolean was_running;
	gboolean rebuilt;

	if (agent == NULL) {
		g_message("rebuild %s: %s no longer exists", job->decision_id,
			job->agent_id);
		return;
	}

	was_running = clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED;

	/* 1. Its turns end, unrerun, and it stops working on the machine. */
	clawt_daemon_turns_abandon(self, job->agent_id,
		"it was stopped so its computer could be rebuilt, as it asked");
	if (was_running)
		clawt_daemon_stop_agent(self, job->agent_id, TRUE);

	/* 2. The rebuild, through the operator's own path. */
	g_message("rebuild %s: rebuilding %s's computer", job->decision_id,
		job->agent_id);
	rebuilt = self->rebuild_func != NULL
		? self->rebuild_func(self, job->agent_id, &note,
		                     self->rebuild_func_data, &error)
		: clawt_daemon_computer_rebuild(self, job->agent_id, &note, &error);

	/* 3. Back up if it was up, and told either way. */
	if (rebuilt)
		body = g_strdup_printf(
			"[clawtilla] Your operator approved the rebuild you asked for, "
			"and it is done: your computer was destroyed and built fresh "
			"from the configuration. What went with it: %s\n\nAnything you "
			"had installed or left inside it has to be set up again. Your "
			"turn was stopped for the rebuild and will not be resumed, so "
			"pick up from what your conversation says was last done.%s%s",
			job->loss != NULL ? job->loss : "the old machine.",
			note != NULL ? "\n\nNote from the teardown: " : "",
			note != NULL ? note : "");
	else
		body = g_strdup_printf(
			"[clawtilla] Your operator approved the rebuild you asked for, "
			"and it failed: %s\n\nYour computer may be gone or half-built. "
			"Do not ask again straight away; tell your operator what you "
			"see.", error != NULL ? error->message : "no reason given");

	if (!rebuilt)
		g_warning("rebuild %s: %s's computer was not rebuilt: %s",
			job->decision_id, job->agent_id,
			error != NULL ? error->message : "no reason given");

	/*
	 * Told before it starts, so the notice is waiting in the mailbox
	 * and is the first thing the new process is handed.
	 */
	tell_agent(self, job->agent_id, body);

	if (was_running)
		clawt_daemon_start_agent_background(self, job->agent_id);
}

static gboolean
on_rebuild_idle(
	gpointer user_data
){
	ClawtDaemon *self = user_data;

	g_clear_pointer(&self->rebuild_idle, g_source_unref);
	clawt_daemon_rebuild_flush(self);

	return G_SOURCE_REMOVE;
}

void
clawt_daemon_rebuild_flush(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->rebuild_idle != NULL) {
		g_source_destroy(self->rebuild_idle);
		g_clear_pointer(&self->rebuild_idle, g_source_unref);
	}

	/* Taken first: a rebuild can settle more decisions while it runs. */
	while (self->rebuild_pending != NULL && self->rebuild_pending->len > 0) {
		RebuildJob *job = g_ptr_array_steal_index(self->rebuild_pending, 0);

		run_rebuild(self, job);
		rebuild_job_free(job);
	}
}

/* The loss the operator was shown, read back out of the question. */
static gchar *
loss_from(
	ClawtDecision *decision
){
	const gchar *question = clawt_decision_get_question(decision);
	const gchar *at = question != NULL
		? g_strrstr(question, CLAWT_REBUILD_LOSS_MARKER) : NULL;

	return at != NULL ? g_strdup(at + strlen(CLAWT_REBUILD_LOSS_MARKER))
	                  : NULL;
}

gboolean
clawt_daemon_rebuild_settle(
	ClawtDaemon   *self,
	ClawtDecision *decision,
	const gchar   *how
){
	const gchar *agent_id;
	const gchar *answer;
	g_autofree gchar *body = NULL;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	if (decision == NULL ||
		!g_str_has_prefix(clawt_decision_get_id(decision),
			CLAWT_REBUILD_DECISION_PREFIX))
		return FALSE;

	agent_id = clawt_decision_get_agent(decision);
	answer = clawt_decision_get_answer(decision);

	/*
	 * Only the exact word approves.  Anything typed into the answer box
	 * instead -- "not now", "rebuild tomorrow" -- is a person saying
	 * something other than yes, and destroying a machine on a guess at
	 * what they meant is the one reading that cannot be taken back.
	 */
	if (g_strcmp0(how, "answered") == 0 &&
		g_ascii_strcasecmp(answer != NULL ? answer : "", "approve") == 0) {
		RebuildJob *job = g_new0(RebuildJob, 1);

		job->decision_id = g_strdup(clawt_decision_get_id(decision));
		job->agent_id = g_strdup(agent_id);
		job->loss = loss_from(decision);

		if (self->rebuild_pending == NULL)
			self->rebuild_pending = g_ptr_array_new_with_free_func(
				rebuild_job_free);
		g_ptr_array_add(self->rebuild_pending, job);

		/*
		 * In an idle, so the answer reaches the client that sent it
		 * before the stop and the rebuild hold the loop -- the same
		 * reason control.shutdown quits from one.
		 */
		if (self->rebuild_idle == NULL) {
			self->rebuild_idle = g_idle_source_new();
			g_source_set_callback(self->rebuild_idle, on_rebuild_idle, self,
				NULL);
			g_source_attach(self->rebuild_idle, self->main_context);
		}

		return TRUE;
	}

	if (g_strcmp0(how, "answered") == 0) {
		g_autofree gchar *said = clawt_preamble_escape(answer);

		body = g_strdup_printf(
			"[clawtilla] Your operator declined the rebuild you asked for "
			"(their answer: %s). Your computer is as it was. Carry on with "
			"it, or tell them what you need instead.%s",
			said != NULL ? said : "decline",
			g_ascii_strcasecmp(answer != NULL ? answer : "", "decline") != 0
				? " Their words are their instructions." : "");
	} else {
		body = g_strdup(
			"[clawtilla] Your operator withdrew the rebuild you asked for "
			"without approving it. Your computer is as it was.");
	}

	tell_agent(self, agent_id, body);
	return TRUE;
}

/* ── Setup ───────────────────────────────────────────────────────── */

void
clawt_daemon_rebuild_setup(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->mcp_tools != NULL && self->decisions != NULL)
		clawt_mcp_tools_set_request_rebuild_func(self->mcp_tools,
			request_rebuild_for_tools, self, NULL);
}

void
clawt_daemon_rebuild_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->mcp_tools != NULL)
		clawt_mcp_tools_set_request_rebuild_func(self->mcp_tools, NULL,
			NULL, NULL);

	if (self->rebuild_idle != NULL) {
		g_source_destroy(self->rebuild_idle);
		g_clear_pointer(&self->rebuild_idle, g_source_unref);
	}

	/*
	 * An approval that never ran is still on disk as answered.  Left
	 * here it would be forgotten, so the agent is told it did not
	 * happen rather than left waiting on a rebuild nobody will run.
	 */
	if (self->rebuild_pending != NULL) {
		guint i;

		for (i = 0; i < self->rebuild_pending->len; i++) {
			RebuildJob *job = g_ptr_array_index(self->rebuild_pending, i);

			tell_agent(self, job->agent_id,
				"[clawtilla] Your operator approved the rebuild you asked "
				"for, but the daemon stopped before it ran, so nothing was "
				"rebuilt. Ask again if it is still needed.");
		}
		g_clear_pointer(&self->rebuild_pending, g_ptr_array_unref);
	}
}
