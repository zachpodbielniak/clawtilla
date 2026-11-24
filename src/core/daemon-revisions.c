/*
 * daemon-revisions.c - Version history: what is kept, and putting it back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - Where versions come from: every clawt_config_save() (an observer
 *   the config library calls), a reload that found the file edited by
 *   hand, agent.file_write on a persona file, and every skill the
 *   library writes (::skill-written) -- the daemon's tools, a taught
 *   skill and an accepted proposal alike.  On start the current text of
 *   each is recorded too, so the first change has something to compare
 *   with; an unchanged text is never kept twice.
 * - revision.list / revision.show / revision.diff / revision.restore.
 *   A restore is a save: it goes back through the same path a person's
 *   edit would, and becomes the newest revision itself.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

/* How many versions of one subject are kept. */
#define REVISIONS_KEEP_PER_SUBJECT (200)

static ClawtDaemon *observed = NULL;

/* clawt_config_save()'s observer: one daemon per process. */
static void
on_config_saved(
	ClawtConfig *config,
	const gchar *text,
	gpointer     user_data
){
	ClawtDaemon *self = user_data;
	g_autoptr(GError) error = NULL;

	(void)config;

	if (self != observed)
		return;

	/* Notes first: they do not depend on the history having opened. */
	clawt_daemon_change_notes_saved(self, text);

	if (self->revisions == NULL)
		return;

	/*
	 * The actor whoever started this save pushed, so the history and the
	 * note name the same author; revision_author still wins for the two
	 * writers that set it on purpose (a restore, an import).
	 */
	if (clawt_revisions_record(self->revisions, "config", "config", text,
	                           self->revision_author != NULL
	                               ? self->revision_author
	                               : clawt_change_actor_get(),
	                           self->revision_note, &error) == 0)
		g_warning("revisions: the config was saved but not kept: %s",
		          error->message);
}

void
clawt_daemon_revisions_record_file(
	ClawtDaemon *self,
	const gchar *kind,
	const gchar *subject,
	const gchar *path,
	const gchar *author
){
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->revisions == NULL || path == NULL ||
	    !g_file_get_contents(path, &text, NULL, NULL))
		return;

	if (clawt_revisions_record(self->revisions, kind, subject, text, author,
	                           self->revision_note, &error) == 0)
		g_warning("revisions: %s %s not kept: %s", kind, subject,
		          error->message);
}

/* ::skill-written: the SKILL.md the library just wrote. */
static void
on_skill_written(
	ClawtSkillLibrary *library,
	const gchar       *name,
	const gchar       *text,
	gpointer           user_data
){
	ClawtDaemon *self = user_data;
	g_autoptr(GError) error = NULL;

	(void)library;

	if (self->revisions == NULL)
		return;

	if (clawt_revisions_record(self->revisions, "skill", name, text,
	                           self->revision_author, self->revision_note,
	                           &error) == 0)
		g_warning("revisions: skill %s not kept: %s", name, error->message);
}

void
clawt_daemon_revisions_watch_skills(
	ClawtDaemon *self
){
	g_autoptr(GPtrArray) skills = NULL;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->revisions == NULL || self->skills == NULL ||
	    g_object_get_data(G_OBJECT(self->skills), "clawt-revisions") != NULL)
		return;

	g_object_set_data(G_OBJECT(self->skills), "clawt-revisions",
	                  GINT_TO_POINTER(1));
	g_signal_connect(self->skills, "skill-written",
	                 G_CALLBACK(on_skill_written), self);

	/* The baseline: each skill of the library's own, as it is now. */
	skills = clawt_skill_library_list(self->skills);

	for (i = 0; skills != NULL && i < skills->len; i++) {
		ClawtSkill *skill = g_ptr_array_index(skills, i);
		g_autofree gchar *path = NULL;

		if (clawt_skill_get_directory(skill) == NULL)
			continue;

		path = g_build_filename(clawt_skill_get_directory(skill), "SKILL.md",
		                        NULL);
		clawt_daemon_revisions_record_file(self, "skill",
		                                   clawt_skill_get_name(skill), path,
		                                   "clawtilla");
	}
}

void
clawt_daemon_revisions_setup(
	ClawtDaemon *self
){
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	/*
	 * The observer first, and whether or not the history opens: the
	 * change notes ride the same observer and must not depend on a
	 * database they never read.
	 */
	clawt_daemon_change_notes_rebase(self);
	observed = self;
	clawt_config_set_save_observer(on_config_saved, self);

	path = g_build_filename(self->state_dir, "revisions.db", NULL);
	self->revisions = clawt_revisions_new(path, &error);

	if (self->revisions == NULL) {
		g_warning("revisions: %s: %s; no history is kept until it opens",
		          path, error->message);
		return;
	}

	/*
	 * The config's baseline here; each agent's persona files are recorded
	 * as its workspace is rendered (after scaffolding, which may not have
	 * happened yet), by clawt_daemon_revisions_record_persona().
	 */
	clawt_daemon_revisions_record_file(self, "config", "config",
	                                   self->config_path, "clawtilla");

	clawt_daemon_revisions_watch_skills(self);
	clawt_revisions_prune(self->revisions, REVISIONS_KEEP_PER_SUBJECT);
}

void
clawt_daemon_revisions_record_persona(
	ClawtDaemon      *self,
	ClawtAgentConfig *agent,
	const gchar      *only,
	const gchar      *author
){
	const ClawtWorkspaceFile *files;
	guint n = 0;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->revisions == NULL || agent == NULL)
		return;

	files = clawt_workspace_files(&n);

	for (i = 0; i < n; i++) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *subject = NULL;

		/* the loaders are generated from the org files; not history */
		if (files[i].generated)
			continue;
		if (only != NULL && g_strcmp0(only, files[i].name) != 0)
			continue;

		path = clawt_workspace_file_path(agent, files[i].name);
		subject = g_strdup_printf("%s/%s", clawt_agent_config_get_id(agent),
		                          files[i].name);
		clawt_daemon_revisions_record_file(self, "persona", subject, path,
		                                   author);
	}
}

void
clawt_daemon_revisions_teardown(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (observed == self) {
		clawt_config_set_save_observer(NULL, NULL);
		observed = NULL;
	}

	g_clear_object(&self->revisions);
	clawt_daemon_change_notes_teardown(self);
}

/* ── The current text of a subject ───────────────────────────────── */

/*
 * Where @kind/@subject lives now, for a diff against the present or a
 * restore.  NULL for a subject that no longer has a home: an agent or a
 * skill since removed.
 */
static gchar *
current_path(
	ClawtDaemon *self,
	const gchar *kind,
	const gchar *subject
){
	if (g_strcmp0(kind, "config") == 0)
		return g_strdup(self->config_path);

	if (g_strcmp0(kind, "persona") == 0) {
		g_auto(GStrv) parts = g_strsplit(subject, "/", 2);
		ClawtAgentConfig *agent;

		if (g_strv_length(parts) != 2)
			return NULL;

		agent = clawt_config_get_agent(self->config, parts[0]);
		return (agent != NULL) ? clawt_workspace_file_path(agent, parts[1])
		                       : NULL;
	}

	if (g_strcmp0(kind, "skill") == 0 && self->skills != NULL) {
		ClawtSkill *skill = clawt_skill_library_lookup(self->skills, subject);

		return (skill != NULL && clawt_skill_get_directory(skill) != NULL)
			? g_build_filename(clawt_skill_get_directory(skill), "SKILL.md",
			                   NULL)
			: NULL;
	}

	return NULL;
}

/* Puts a revision's text back, through the path an edit would take. */
static gboolean
restore(
	ClawtDaemon  *self,
	gint64        id,
	GError      **error
){
	g_autofree gchar *kind = NULL;
	g_autofree gchar *subject = NULL;
	g_autofree gchar *text = clawt_revisions_get(self->revisions, id, &kind,
	                                             &subject);
	g_autofree gchar *path = NULL;
	g_autofree gchar *note = g_strdup_printf("restored from revision %"
	                                         G_GINT64_FORMAT, id);
	gboolean done = FALSE;

	if (text == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "there is no revision %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	path = current_path(self, kind, subject);

	if (path == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "%s %s no longer exists to be restored into", kind,
		            subject);
		return FALSE;
	}

	self->revision_author = "user";
	self->revision_note = note;

	if (g_strcmp0(kind, "config") == 0) {
		/*
		 * Parsed before it is written: a revision that no longer loads
		 * (a key this build refuses) must not replace a config that does.
		 */
		g_autoptr(ClawtConfig) check = clawt_config_load_from_string(text,
		                                                             error);

		if (check != NULL &&
		    clawt_write_file_atomic(path, text, -1, 0600, TRUE, error)) {
			clawt_daemon_revisions_record_file(self, "config", "config",
			                                   path, "user");
			done = clawt_daemon_reload(self, error);
		}
	} else if (g_strcmp0(kind, "persona") == 0) {
		if (clawt_write_file_atomic(path, text, -1, 0600, FALSE, error)) {
			clawt_daemon_revisions_record_file(self, "persona", subject,
			                                   path, "user");
			done = TRUE;
		}
	} else {
		g_autoptr(ClawtSkill) parsed = clawt_skill_parse(text, -1, subject,
		                                                 error);

		done = parsed != NULL &&
		       clawt_skill_library_replace(self->skills, subject,
		           clawt_skill_get_description(parsed),
		           clawt_skill_get_body(parsed), error) != NULL;
	}

	self->revision_author = NULL;
	self->revision_note = NULL;

	return done;
}

/* ── IPC ─────────────────────────────────────────────────────────── */

/*
 * A revision id, as a number or as the string every client's payload
 * builder makes of it.
 */
static gint64
payload_id(
	JsonObject  *payload,
	const gchar *key
){
	const gchar *text = clawt_ipc_payload_string(payload, key);

	if (text != NULL)
		return g_ascii_strtoll(text, NULL, 10);

	return clawt_ipc_payload_int(payload, key, 0);
}

JsonNode *
clawt_daemon_handle_revision(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;

	if (!g_str_has_prefix(kind, "revision.")) {
		*handled = FALSE;
		return NULL;
	}

	*handled = TRUE;

	if (self->revisions == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
		                           "no history is kept: revisions.db could "
		                           "not be opened");

	if (g_strcmp0(kind, "revision.list") == 0) {
		JsonNode *list = clawt_revisions_list(
			self->revisions, clawt_ipc_payload_string(payload, "kind"),
			clawt_ipc_payload_string(payload, "subject"),
			(guint)MAX(payload_id(payload, "limit"), 0));

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "revisions");
		json_builder_add_value(builder, list);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "revision.show") == 0) {
		gint64 id = payload_id(payload, "id");
		g_autofree gchar *what = NULL;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *text = clawt_revisions_get(self->revisions, id,
		                                             &what, &subject);

		if (text == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           "no such revision");

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, id);
		clawt_daemon_add_string_member(builder, "kind", what);
		clawt_daemon_add_string_member(builder, "subject", subject);
		clawt_daemon_add_string_member(builder, "text", text);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	/*
	 * A revision against an older one (`to` and `from`), or against the
	 * subject as it is now (`from` alone): what changed since.
	 */
	if (g_strcmp0(kind, "revision.diff") == 0) {
		gint64 from = payload_id(payload, "from");
		gint64 to = payload_id(payload, "to");
		g_autofree gchar *what = NULL;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *before = clawt_revisions_get(self->revisions, from,
		                                               &what, &subject);
		g_autofree gchar *after = NULL;
		g_autofree gchar *label_before = NULL;
		g_autofree gchar *label_after = NULL;
		g_autofree gchar *diff = NULL;

		if (before == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           "no such revision");

		if (to > 0) {
			g_autofree gchar *to_kind = NULL;
			g_autofree gchar *to_subject = NULL;

			after = clawt_revisions_get(self->revisions, to, &to_kind,
			                            &to_subject);
			if (after == NULL || g_strcmp0(to_kind, what) != 0 ||
			    g_strcmp0(to_subject, subject) != 0)
				return clawt_ipc_error_new(request,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "both revisions must be of the same thing");
			label_after = g_strdup_printf("%s (revision %" G_GINT64_FORMAT
			                              ")", subject, to);
		} else {
			g_autofree gchar *path = current_path(self, what, subject);

			if (path == NULL || !g_file_get_contents(path, &after, NULL,
			                                         NULL))
				after = g_strdup("");
			label_after = g_strdup_printf("%s (now)", subject);
		}

		label_before = g_strdup_printf("%s (revision %" G_GINT64_FORMAT ")",
		                               subject, from);
		diff = clawt_revisions_diff(before, after, label_before,
		                            label_after);

		json_builder_begin_object(builder);
		clawt_daemon_add_string_member(builder, "diff", diff);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "revision.restore") == 0) {
		gint64 id = payload_id(payload, "id");

		if (!restore(self, id, &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "restored");
		json_builder_add_int_value(builder, id);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	*handled = FALSE;
	return NULL;
}
