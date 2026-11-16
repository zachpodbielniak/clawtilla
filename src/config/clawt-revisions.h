/*
 * clawt-revisions.h - Every version of what shapes the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The fleet's config, each agent's persona files and every skill change
 * all the time, by hand, from a client, and by agents themselves -- and
 * until now a change was a one-way door: what an agent was told last
 * week was gone the moment somebody saved.  This keeps each version
 * (`<state_dir>/revisions.db`, since 0.41.0): a row per save whose text
 * differs from the last one for the same subject, so it can be listed,
 * read, compared and put back.  Putting one back is a save like any
 * other, and is itself a revision -- the history only grows.
 *
 * Subjects are spelled by kind:
 *
 * - `config`: the subject is `config` (the fleet's config file);
 * - `persona`: `<agent>/<file>`, e.g. `scout/SOUL.org`;
 * - `skill`: the skill's name, and the text is its SKILL.md.
 *
 * The config holds secrets only by reference; its text is passed through
 * clawt_redact_secrets() anyway before it is kept, so a literal that
 * slipped in is not copied into a second file.
 */

#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_REVISIONS (clawt_revisions_get_type())

G_DECLARE_FINAL_TYPE(ClawtRevisions, clawt_revisions, CLAWT, REVISIONS,
                     GObject)

/**
 * CLAWT_REVISIONS_MAX_BYTES:
 *
 * The largest text kept as a revision; anything larger is refused rather
 * than cut, since a cut file put back would be a broken file.
 */
#define CLAWT_REVISIONS_MAX_BYTES (2 * 1024 * 1024)

/**
 * clawt_revisions_new:
 * @path: the database file, created with its directory when missing
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store
 */
ClawtRevisions *clawt_revisions_new(const gchar *path, GError **error);

/**
 * clawt_revisions_record:
 * @self: a #ClawtRevisions
 * @kind: `config`, `persona` or `skill`
 * @subject: what it is a version of (see the header)
 * @text: the whole text
 * @author: (nullable): who saved it: `user`, an agent id, `clawtilla`
 * @note: (nullable): why, in a few words (`restored from 12`)
 * @error: (out) (optional): return location for a #GError
 *
 * Keeps @text as the newest revision of @subject, unless it is the same
 * as the newest already kept -- a save that changed nothing is not a
 * version.
 *
 * Returns: the new revision's id, the existing one's when nothing
 *   changed, or 0 with @error set
 */
gint64 clawt_revisions_record(ClawtRevisions  *self,
                              const gchar     *kind,
                              const gchar     *subject,
                              const gchar     *text,
                              const gchar     *author,
                              const gchar     *note,
                              GError         **error);

/**
 * clawt_revisions_list:
 * @self: a #ClawtRevisions
 * @kind: (nullable): only this kind
 * @subject: (nullable): only this subject
 * @limit: at most this many, newest first; 0 for 100
 *
 * Returns: (transfer full): an array of objects -- `id`, `kind`,
 *   `subject`, `at`, `author`, `note`, `bytes` -- without the text
 */
JsonNode *clawt_revisions_list(ClawtRevisions *self,
                               const gchar    *kind,
                               const gchar    *subject,
                               guint           limit);

/**
 * clawt_revisions_get:
 * @self: a #ClawtRevisions
 * @id: a revision id
 * @out_kind: (out) (transfer full) (optional): its kind
 * @out_subject: (out) (transfer full) (optional): its subject
 *
 * Returns: (transfer full) (nullable): its text, or %NULL when there is
 *   no such revision
 */
gchar *clawt_revisions_get(ClawtRevisions  *self,
                           gint64           id,
                           gchar          **out_kind,
                           gchar          **out_subject);

/**
 * clawt_revisions_prune:
 * @self: a #ClawtRevisions
 * @keep: how many revisions to keep per subject, newest first
 *
 * Returns: how many were removed
 */
guint clawt_revisions_prune(ClawtRevisions *self, guint keep);

/**
 * clawt_revisions_diff:
 * @before: the older text
 * @after: the newer text
 * @label_before: what to call it in the header
 * @label_after: what to call the other
 *
 * A unified diff (three lines of context) of two texts, line by line.
 * For texts too large to compare line by line in reasonable time it says
 * so in one line rather than diffing them.
 *
 * Returns: (transfer full): the diff; empty when they are the same
 */
gchar *clawt_revisions_diff(const gchar *before,
                            const gchar *after,
                            const gchar *label_before,
                            const gchar *label_after);

/**
 * clawt_revisions_newest_id:
 * @self: a #ClawtRevisions
 * @kind: a kind
 * @subject: a subject
 *
 * Returns: the newest revision's id, or 0 when none is kept
 */
gint64 clawt_revisions_newest_id(ClawtRevisions *self,
                                 const gchar    *kind,
                                 const gchar    *subject);

/**
 * clawt_revisions_start_test_run:
 * @self: a #ClawtRevisions
 * @skill: the skill under test
 * @test: the test's name (its file, without `.yaml`)
 * @revision: the skill's revision it ran against
 * @agent: who ran it
 * @task_id: the task it ran as
 *
 * Records a skill test as running (since 0.41.0).
 *
 * Returns: the run's id, or 0
 */
gint64 clawt_revisions_start_test_run(ClawtRevisions *self,
                                      const gchar    *skill,
                                      const gchar    *test,
                                      gint64          revision,
                                      const gchar    *agent,
                                      const gchar    *task_id);

/**
 * clawt_revisions_test_run_for_task:
 * @self: a #ClawtRevisions
 * @task_id: (nullable): a task id
 * @out_skill: (out) (transfer full) (optional): the skill
 * @out_test: (out) (transfer full) (optional): the test
 *
 * Returns: whether @task_id is a skill test still running
 */
gboolean clawt_revisions_test_run_for_task(ClawtRevisions  *self,
                                           const gchar     *task_id,
                                           gchar          **out_skill,
                                           gchar          **out_test);

/**
 * clawt_revisions_finish_test_run:
 * @self: a #ClawtRevisions
 * @task_id: the task the test ran as
 * @state: `passed`, `failed` or `error`
 * @detail: (nullable): why, in a sentence
 */
void clawt_revisions_finish_test_run(ClawtRevisions *self,
                                     const gchar    *task_id,
                                     const gchar    *state,
                                     const gchar    *detail);

/**
 * clawt_revisions_last_test_run:
 * @self: a #ClawtRevisions
 * @skill: the skill
 * @test: the test
 *
 * Returns: (transfer full) (nullable): the newest run -- `at`, `state`,
 *   `detail`, `revision`, `agent`, `task` -- or %NULL for none
 */
JsonNode *clawt_revisions_last_test_run(ClawtRevisions *self,
                                        const gchar    *skill,
                                        const gchar    *test);

G_END_DECLS
