/*
 * clawt-routine-runner.c - Running the standing work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-routine-runner.h"

#include <yaml-glib.h>

#include <string.h>

/* Cron's resolution, so anything finer is work to reach the same answer. */
#define TICK_SECONDS (60)

/*
 * The longest jitter that is still a jitter.
 *
 * An hour, so a typo of 86400 delays a routine rather than pushing it
 * past its own next slot -- at which point the run that eventually fires
 * is for a time nobody can point at, and the routine looks skipped.
 * Clamped rather than refused because the value is spreading load, not
 * naming a moment.
 */
#define MAX_JITTER_SECONDS (3600)

typedef struct {
    gint64         last_run;     /* Unix time */
    ClawtRunState  state;
    gchar         *detail;
    gchar         *task_id;

    /*
     * Armed when a due routine is waiting out its jitter.
     *
     * Held here rather than in a list of its own because the tick runs
     * once a minute and a jitter longer than that would otherwise find
     * the same routine due again and arm a second timer -- and then a
     * third -- so `jitter_seconds: 300` would fire five runs instead of
     * delaying one.
     */
    GSource       *jitter;

    /*
     * Runs in a row whose computer did not start, and what turned the
     * routine off when that reached `max_start_failures`.
     *
     * Here and not in clawtilla.yaml for the reason last_run is: it is
     * state.  `off_after` is non-zero only while the routine is off for
     * this reason, so the list can say why it is off and a client can
     * offer to turn it back on.
     */
    guint          start_failures;
    guint          off_after;
    gchar         *off_error;

    /*
     * When the routine was last edited, and what it looked like then.
     *
     * The fingerprint is compared on every config load, so an edit made
     * by hand in clawtilla.yaml counts as much as one made from a
     * client -- a rule that only saw routine.update would miss the edit
     * most likely to be the fix.  `edited_at_us` is what keeps a run
     * that began under the old shape from counting against the new one.
     */
    gint64         edited_at_us;
    gchar         *fingerprint;
} RunRecord;

/*
 * Cancels a pending jitter, wherever the reason came from.
 *
 * One spelling because there are four: the delay elapsing, a run
 * starting some other way, the runner stopping, and the record going
 * away.  A source left attached on any of them fires against a runner
 * that has moved on.
 */
static void
clear_jitter(RunRecord *record)
{
    if (record == NULL || record->jitter == NULL)
        return;

    g_source_destroy(record->jitter);
    g_clear_pointer(&record->jitter, g_source_unref);
}

static void
run_record_free(RunRecord *self)
{
    if (self == NULL)
        return;

    clear_jitter(self);

    g_free(self->detail);
    g_free(self->task_id);
    g_free(self->off_error);
    g_free(self->fingerprint);
    g_free(self);
}

struct _ClawtRoutineRunner {
    GObject parent_instance;

    ClawtConfig *config;         /* owned */
    gchar       *state_path;

    /*
     * What has run, kept beside the config rather than in it.
     *
     * A last-run time is state, not configuration: writing it into
     * clawtilla.yaml would rewrite somebody's file every time a routine
     * fired, and a file that rewrites itself is one people stop keeping
     * in git.
     */
    GHashTable *records;         /* routine id -> RunRecord* */

    /*
     * The last schedule failure announced for each routine, so the same
     * one is not announced again.
     *
     * A malformed schedule is re-derived on every tick -- next_run() and
     * the due check both ask for the cron -- and each failure used to
     * take a g_warning() with it.  One routine whose `at:` could not be
     * read produced roughly a line a minute for as long as the daemon
     * was up: 1,368 a day, on a daemon nobody restarts, and it was the
     * dominant line in the unit's log while somebody was diagnosing an
     * unrelated fault.
     *
     * Keyed by routine and holding the message rather than a flag,
     * because an edit that changes *how* a schedule is wrong is news
     * and must not be swallowed by "already said something about this
     * one".
     */
    GHashTable *announced;       /* routine id -> gchar* message */

    ClawtRoutineRunFunc run_func;
    gpointer            run_data;

    GSource *tick;
};

G_DEFINE_FINAL_TYPE(ClawtRoutineRunner, clawt_routine_runner, G_TYPE_OBJECT)

/* ── Remembering ─────────────────────────────────────────────────── */

static void
load_state(ClawtRoutineRunner *self)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    YamlNode *root;
    YamlMapping *mapping;
    GList *members;
    GList *l;

    g_hash_table_remove_all(self->records);

    if (self->state_path == NULL ||
        !g_file_test(self->state_path, G_FILE_TEST_EXISTS))
        return;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_file(parser, self->state_path, &error)) {
        /*
         * A warning and an empty slate.  This file is a convenience, and
         * refusing to schedule anything because it is corrupt would turn
         * a lost timestamp into a stopped fleet.
         */
        g_warning("routines: %s could not be read (%s); every routine will "
                  "look as though it has never run", self->state_path,
                  error->message);
        return;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return;

    mapping = yaml_node_get_mapping(root);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *id = l->data;
        YamlNode *entry = yaml_mapping_get_member(mapping, id);
        YamlMapping *fields;
        YamlNode *node;
        RunRecord *record;

        if (entry == NULL ||
            yaml_node_get_node_type(entry) != YAML_NODE_MAPPING)
            continue;

        fields = yaml_node_get_mapping(entry);
        record = g_new0(RunRecord, 1);

        node = yaml_mapping_get_member(fields, "last_run");
        if (node != NULL)
            record->last_run = yaml_node_get_int(node);

        node = yaml_mapping_get_member(fields, "state");
        if (node != NULL) {
            gint value = 0;

            if (clawt_enum_from_nick(CLAWT_TYPE_RUN_STATE,
                                     yaml_node_get_string(node), &value))
                record->state = (ClawtRunState)value;
        }

        node = yaml_mapping_get_member(fields, "detail");
        if (node != NULL)
            record->detail = g_strdup(yaml_node_get_string(node));

        node = yaml_mapping_get_member(fields, "task");
        if (node != NULL)
            record->task_id = g_strdup(yaml_node_get_string(node));

        /* What the start-failure guard keeps; all absent in an old file. */
        node = yaml_mapping_get_member(fields, "start_failures");
        if (node != NULL)
            record->start_failures = (guint)MAX(yaml_node_get_int(node), 0);

        node = yaml_mapping_get_member(fields, "turned_off_after");
        if (node != NULL)
            record->off_after = (guint)MAX(yaml_node_get_int(node), 0);

        node = yaml_mapping_get_member(fields, "turned_off_error");
        if (node != NULL)
            record->off_error = g_strdup(yaml_node_get_string(node));

        node = yaml_mapping_get_member(fields, "edited_at");
        if (node != NULL)
            record->edited_at_us = yaml_node_get_int(node);

        node = yaml_mapping_get_member(fields, "fingerprint");
        if (node != NULL)
            record->fingerprint = g_strdup(yaml_node_get_string(node));

        g_hash_table_insert(self->records, g_strdup(id), record);
    }

    g_list_free(members);
}

static void
save_state(ClawtRoutineRunner *self)
{
    g_autoptr(YamlNode) root = NULL;
    g_autoptr(YamlGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (self->state_path == NULL)
        return;

    root = yaml_node_new_mapping(NULL);

    g_hash_table_iter_init(&iter, self->records);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        RunRecord *record = value;
        g_autoptr(YamlNode) entry = yaml_node_new_mapping(NULL);
        YamlMapping *fields = yaml_node_get_mapping(entry);
        g_autoptr(YamlNode) last = yaml_node_new_int(record->last_run);
        g_autoptr(YamlNode) state = yaml_node_new_string(
            clawt_enum_to_nick(CLAWT_TYPE_RUN_STATE, (gint)record->state));

        yaml_mapping_set_member(fields, "last_run", last);
        yaml_mapping_set_member(fields, "state", state);

        if (record->detail != NULL) {
            g_autoptr(YamlNode) detail = yaml_node_new_string(record->detail);

            yaml_mapping_set_member(fields, "detail", detail);
        }

        if (record->task_id != NULL) {
            g_autoptr(YamlNode) task = yaml_node_new_string(record->task_id);

            yaml_mapping_set_member(fields, "task", task);
        }

        /* Only what is set, so a routine that never failed stays short. */
        if (record->start_failures > 0) {
            g_autoptr(YamlNode) failures =
                yaml_node_new_int((gint64)record->start_failures);

            yaml_mapping_set_member(fields, "start_failures", failures);
        }

        if (record->off_after > 0) {
            g_autoptr(YamlNode) after =
                yaml_node_new_int((gint64)record->off_after);

            yaml_mapping_set_member(fields, "turned_off_after", after);
        }

        if (record->off_error != NULL) {
            g_autoptr(YamlNode) off_error =
                yaml_node_new_string(record->off_error);

            yaml_mapping_set_member(fields, "turned_off_error", off_error);
        }

        if (record->edited_at_us > 0) {
            g_autoptr(YamlNode) edited =
                yaml_node_new_int(record->edited_at_us);

            yaml_mapping_set_member(fields, "edited_at", edited);
        }

        if (record->fingerprint != NULL) {
            g_autoptr(YamlNode) fingerprint =
                yaml_node_new_string(record->fingerprint);

            yaml_mapping_set_member(fields, "fingerprint", fingerprint);
        }

        yaml_mapping_set_member(yaml_node_get_mapping(root), key, entry);
    }

    generator = yaml_generator_new();
    yaml_generator_set_root(generator, root);
    text = yaml_generator_to_data(generator, NULL, &error);

    if (text == NULL) {
        g_warning("routines: could not render %s: %s",
                  self->state_path,
                  error != NULL ? error->message : "unknown");
        return;
    }

    if (!clawt_write_file_atomic(self->state_path, text, -1, 0600, FALSE,
                                 &error))
        g_warning("routines: could not write %s: %s", self->state_path,
                  error->message);
}

static RunRecord *
record_for(ClawtRoutineRunner *self, const gchar *routine_id)
{
    RunRecord *record = g_hash_table_lookup(self->records, routine_id);

    if (record != NULL)
        return record;

    record = g_new0(RunRecord, 1);
    g_hash_table_insert(self->records, g_strdup(routine_id), record);

    return record;
}

/* ── Edits ───────────────────────────────────────────────────────── */

/*
 * What a routine looks like, as one string.
 *
 * Walked from the schema rather than from a list here: a field added to
 * the schema and left out of a list would be a field whose edit does not
 * reset the count, which is the drift AGENTS.md has recorded three times
 * for key tables.  Each leaf is read through its typed getter, so a
 * value written as `"3"` and as `3` is the same routine.  `enabled` is
 * included on purpose: turning a routine back on is an edit, and it is
 * the one that has to clear a "turned off" note.
 */
static gchar *
routine_fingerprint(
	ClawtRoutine *routine
){
	g_autoptr(GChecksum) checksum = NULL;
	const ClawtSchemaEntry *entries;
	gsize n_entries = 0;
	gsize i;

	checksum = g_checksum_new(G_CHECKSUM_SHA256);
	entries = clawt_config_schema_get(&n_entries);

	for (i = 0; i < n_entries; i++) {
		const gchar *leaf;
		g_autofree gchar *line = NULL;

		if (!g_str_has_prefix(entries[i].key, "routines."))
			continue;

		leaf = entries[i].key + strlen("routines.");

		if (strchr(leaf, '.') != NULL)
			continue;

		switch (entries[i].type) {
		case CLAWT_SCHEMA_SECTION:
		case CLAWT_SCHEMA_MAPPING:
		case CLAWT_SCHEMA_LIST_OF:
			continue;

		case CLAWT_SCHEMA_BOOLEAN:
			line = g_strdup_printf("%s=%d\n", leaf,
			                       (gint)clawt_routine_get_boolean(routine, leaf));
			break;

		case CLAWT_SCHEMA_INT:
			line = g_strdup_printf("%s=%" G_GINT64_FORMAT "\n", leaf,
			                       clawt_routine_get_int(routine, leaf));
			break;

		default: {
			const gchar *value = clawt_routine_get_string(routine, leaf);

			line = g_strdup_printf("%s=%s\n", leaf,
			                       (value != NULL) ? value : "");
			break;
		}
		}

		g_checksum_update(checksum, (const guchar *)line, (gssize)strlen(line));
	}

	return g_strdup(g_checksum_get_string(checksum));
}

/*
 * Notices every routine that changed since the runner last looked.
 *
 * An edit sets the count of failed starts back to zero and stamps when
 * it happened, so a run that began before it cannot count afterwards.
 * An edit that leaves the routine on also clears a "turned off" note --
 * that is what turning it back on is.  One that leaves it off keeps the
 * note, which is why the runner's own turning-off does not erase the
 * reason it gave.
 *
 * A routine seen for the first time is remembered and not treated as
 * edited: a state file from before this existed has no fingerprints,
 * and the upgrade is not an edit.
 */
static void
sync_edits(
	ClawtRoutineRunner *self
){
	GPtrArray *routines;
	gboolean changed = FALSE;
	guint i;

	if (self->config == NULL)
		return;

	routines = clawt_config_get_routines(self->config);

	for (i = 0; routines != NULL && i < routines->len; i++) {
		ClawtRoutine *routine = g_ptr_array_index(routines, i);
		const gchar *id = clawt_routine_get_id(routine);
		g_autofree gchar *fingerprint = NULL;
		RunRecord *record;

		if (id == NULL)
			continue;

		fingerprint = routine_fingerprint(routine);
		record = record_for(self, id);

		if (record->fingerprint == NULL) {
			record->fingerprint = g_steal_pointer(&fingerprint);
			changed = TRUE;
			continue;
		}

		if (g_strcmp0(record->fingerprint, fingerprint) == 0)
			continue;

		g_free(record->fingerprint);
		record->fingerprint = g_steal_pointer(&fingerprint);
		record->edited_at_us = g_get_real_time();
		record->start_failures = 0;

		if (clawt_routine_get_boolean(routine, "enabled")) {
			record->off_after = 0;
			g_clear_pointer(&record->off_error, g_free);
		}

		changed = TRUE;
	}

	if (changed)
		save_state(self);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clawt_routine_runner_finalize(GObject *object)
{
    ClawtRoutineRunner *self = CLAWT_ROUTINE_RUNNER(object);

    clawt_routine_runner_stop(self);

    g_clear_object(&self->config);
    g_clear_pointer(&self->state_path, g_free);
    g_clear_pointer(&self->records, g_hash_table_unref);
    g_clear_pointer(&self->announced, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_routine_runner_parent_class)->finalize(object);
}

static void
clawt_routine_runner_class_init(ClawtRoutineRunnerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_routine_runner_finalize;
}

static void
clawt_routine_runner_init(ClawtRoutineRunner *self)
{
    self->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)run_record_free);
    self->announced = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);
}

ClawtRoutineRunner *
clawt_routine_runner_new(ClawtConfig *config, const gchar *state_path)
{
    ClawtRoutineRunner *self = g_object_new(CLAWT_TYPE_ROUTINE_RUNNER, NULL);

    self->config = g_object_ref(config);
    self->state_path = g_strdup(state_path);
    load_state(self);

    /* An edit made while the daemon was down is still an edit. */
    sync_edits(self);

    return self;
}

void
clawt_routine_runner_set_run_func(ClawtRoutineRunner  *self,
                                  ClawtRoutineRunFunc  func,
                                  gpointer             user_data)
{
    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    self->run_func = func;
    self->run_data = user_data;
}

void
clawt_routine_runner_set_config(ClawtRoutineRunner *self, ClawtConfig *config)
{
    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    g_set_object(&self->config, config);

    /*
     * Every save reloads, and so does a hand edit picked up by a
     * reload: this is the one place both pass through.
     */
    sync_edits(self);
}

/* ── Deciding ────────────────────────────────────────────────────── */

/*
 * The cron a routine means, or %NULL.
 *
 * A malformed expression is a warning naming the routine, not a
 * refusal to schedule anything else: one routine that cannot be parsed
 * should not take the rest down with it.
 */
/*
 * Says a routine's schedule failure once, and again only if it changes.
 *
 * The failure itself is re-derived on every tick, because the schedule
 * is read from the config each time and nothing caches it.  Announcing
 * it every time turned one unreadable `at:` into roughly a line a
 * minute for as long as the daemon was up -- and a warning nobody can
 * act on differently the second time is not telling the operator
 * anything new.
 *
 * The message is compared rather than a flag being set, so a routine
 * edited from one broken schedule to a differently broken one is
 * announced again.
 */
static void
announce_once(ClawtRoutineRunner *self, const gchar *id, const gchar *message)
{
    /*
     * A NULL runner is the read path -- clawt_routine_runner_next_run(),
     * which routine.list calls once per routine per refresh -- and it
     * says nothing at all.  A client polling a page is not an occasion
     * to write to the daemon's log, and warning there would reproduce
     * the once-a-minute defect this exists to end, with the period set
     * by whoever left a window open.  The reason still reaches that
     * caller: it is in the GError, and the reply carries it as
     * `problem`.
     */
    if (self == NULL)
        return;

    if (g_strcmp0(g_hash_table_lookup(self->announced, id), message) == 0)
        return;

    g_hash_table_insert(self->announced, g_strdup(id), g_strdup(message));

    g_warning("routine '%s' will never run until this is fixed: %s", id,
              message);
}

/*
 * @self may be %NULL: clawt_routine_runner_next_run() is also reached
 * from routine.list, where the caller wants the answer and the daemon
 * has already said whatever there was to say at load.
 */
static ClawtCron *
cron_for(ClawtRoutineRunner *self, ClawtRoutine *routine)
{
    g_autofree gchar *expression = NULL;
    g_autoptr(GError) error = NULL;
    ClawtCron *cron;

    expression = clawt_routine_get_cron(routine, &error);

    if (expression == NULL) {
        if (error != NULL)
            announce_once(self, clawt_routine_get_id(routine),
                          error->message);

        return NULL;
    }

    cron = clawt_cron_parse(expression, &error);

    if (cron == NULL)
        announce_once(self, clawt_routine_get_id(routine), error->message);

    return cron;
}

GDateTime *
clawt_routine_runner_next_run(ClawtRoutineRunner *self,
                              const gchar        *routine_id)
{
    ClawtRoutine *routine;
    g_autoptr(ClawtCron) cron = NULL;
    g_autoptr(GDateTime) now = NULL;

    g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), NULL);

    routine = clawt_config_get_routine(self->config, routine_id);

    if (routine == NULL || !clawt_routine_get_boolean(routine, "enabled"))
        return NULL;

    /*
     * NULL rather than `self`: this is the read path `routine.list`
     * calls, once per routine per refresh, and a client opening a page
     * is not an occasion to say anything to the log.  What the client
     * needs is the reason, and it gets that from `problem` in the reply.
     */
    cron = cron_for(NULL, routine);

    if (cron == NULL)
        return NULL;

    now = g_date_time_new_now_local();

    return clawt_cron_next(cron, now);
}

gint64
clawt_routine_runner_last_run(ClawtRoutineRunner  *self,
                              const gchar         *routine_id,
                              ClawtRunState       *out_state,
                              const gchar        **out_detail)
{
    RunRecord *record;

    g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), 0);

    record = g_hash_table_lookup(self->records, routine_id);

    if (record == NULL) {
        if (out_state != NULL)
            *out_state = CLAWT_RUN_NEVER;

        if (out_detail != NULL)
            *out_detail = NULL;

        return 0;
    }

    if (out_state != NULL)
        *out_state = record->state;

    if (out_detail != NULL)
        *out_detail = record->detail;

    return record->last_run;
}

/* ── Running ─────────────────────────────────────────────────────── */

/*
 * What the agent is actually asked.
 *
 * The instructions, plus where to run and a plain statement that
 * nobody is watching.  That last part earns its place: an agent that
 * asks a clarifying question at three in the morning has not done the
 * work, and it has no way to know from the prompt alone that this run
 * is different from a person typing.
 */
static gchar *
build_prompt(ClawtRoutine *routine)
{
    GString *out = g_string_new(NULL);
    const gchar *instructions =
        clawt_routine_get_string(routine, "instructions");
    const gchar *directory = clawt_routine_get_string(routine, "directory");

    /*
     * A proactive routine nobody wrote instructions for still has a job:
     * the same one ChatGPT's dots and Grok Bot do unasked, done here
     * without acting on anything.
     */
    if ((instructions == NULL || *instructions == '\0') &&
        clawt_routine_get_boolean(routine, "proactive"))
        instructions = "Go through what you are connected to -- your integrations, "
                       "your connectors, your mailbox, your tasks -- and look for "
                       "anything that needs attention: something overdue, something "
                       "that failed, something somebody asked for and nobody answered, "
                       "a conflict, a deadline. File each finding as a decision for the "
                       "operator with clawtilla_ask_decision, saying what you saw and "
                       "what you would do. Do not act on any of it.";

    g_string_append(out, instructions != NULL ? instructions : "");
    g_string_append(out, "\n\n---\n");
    g_string_append_printf(out,
        "This is the scheduled routine '%s', started by clawtilla rather "
        "than by a person. Nobody is waiting on this conversation, so "
        "asking a question here reaches nobody -- do the work with what "
        "you have, and if something is genuinely blocking, say so with "
        "clawtilla_message_user and stop.\n",
        clawt_routine_get_id(routine));

    if (clawt_policy_routine_is_read_only(routine))
        g_string_append(out,
            "\nThis run is read-only. You have only tools that look; anything that "
            "would send, write, delete, run a program or touch a screen is refused. "
            "Report what you find with clawtilla_ask_decision -- one decision per "
            "finding, with what you would do as the default -- and change nothing.\n");

    if (directory != NULL && *directory != '\0') {
        if (clawt_routine_get_boolean(routine, "worktree"))
            g_string_append_printf(out,
                "\nWork in a fresh git worktree of %s, created for this "
                "run. Do not touch whatever is checked out in %s itself.\n",
                directory, directory);
        else
            g_string_append_printf(out, "\nWork in %s.\n", directory);
    }

    return g_string_free(out, FALSE);
}

static const gchar *
start_run(ClawtRoutineRunner *self, ClawtRoutine *routine, GError **error)
{
    g_autofree gchar *prompt = NULL;
    RunRecord *record;
    const gchar *task_id;
    const gchar *agent = clawt_routine_get_string(routine, "agent");
    const gchar *id = clawt_routine_get_id(routine);

    record = record_for(self, id);

    /*
     * Any run at all cancels a jitter that was still waiting.  Here
     * rather than in run_now(), because it is true of every path: the
     * routine has run, so the delayed copy of the same run is a second
     * one nobody asked for.
     */
    clear_jitter(record);

    record->last_run = g_get_real_time() / G_USEC_PER_SEC;
    g_clear_pointer(&record->detail, g_free);
    g_clear_pointer(&record->task_id, g_free);

    if (self->run_func == NULL) {
        record->state = CLAWT_RUN_FAILED;
        record->detail = g_strdup("nothing is wired up to run routines");
        save_state(self);

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "nothing is wired up to run routines");
        return NULL;
    }

    if (agent == NULL || *agent == '\0') {
        record->state = CLAWT_RUN_FAILED;
        record->detail = g_strdup("no agent is set");
        save_state(self);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "routine '%s' has no agent", id);
        return NULL;
    }

    prompt = build_prompt(routine);
    task_id = self->run_func(id, agent, prompt, self->run_data, error);

    if (task_id == NULL) {
        record->state = CLAWT_RUN_FAILED;
        record->detail = g_strdup((error != NULL && *error != NULL)
                                  ? (*error)->message : "it did not start");
        save_state(self);

        return NULL;
    }

    record->state = CLAWT_RUN_OK;
    record->task_id = g_strdup(task_id);
    save_state(self);

    g_message("routine '%s' started as task %s", id, task_id);

    return record->task_id;
}

const gchar *
clawt_routine_runner_run_now(ClawtRoutineRunner  *self,
                             const gchar         *routine_id,
                             GError             **error)
{
    ClawtRoutine *routine;

    g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), NULL);

    routine = clawt_config_get_routine(self->config, routine_id);

    if (routine == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no routine called '%s'",
                    routine_id != NULL ? routine_id : "");
        return NULL;
    }

    /*
     * Neither the schedule nor `enabled` is consulted.  Running a
     * disabled routine by hand is the point: it is how somebody tries
     * one before trusting it with a schedule.
     */
    return start_run(self, routine, error);
}

/*
 * What a pending jitter carries to its callback.
 *
 * The id and not the #ClawtRoutine: the config is reloaded whenever
 * anybody saves, which frees every routine handle it owned, so a pointer
 * held across a delay is a pointer to freed memory.  The runner itself
 * is not reffed -- it owns the source, so the source cannot outlive it.
 */
typedef struct {
    ClawtRoutineRunner *runner;
    gchar              *id;
} JitterArm;

static void
jitter_arm_free(gpointer data)
{
    JitterArm *self = data;

    g_free(self->id);
    g_free(self);
}

/*
 * A routine whose jitter has run out.
 *
 * It re-reads the routine rather than holding the pointer it was armed
 * with, because the config can be reloaded while a jitter is waiting --
 * `routine.update` saves and reloads, which frees every #ClawtRoutine
 * the old config owned.  Looking the id up again is what makes a
 * five-minute jitter survive somebody editing the fleet inside it.
 */
static gboolean
on_jitter_elapsed(gpointer user_data)
{
    JitterArm *arm = user_data;
    ClawtRoutineRunner *self = arm->runner;
    ClawtRoutine *routine = clawt_config_get_routine(self->config, arm->id);
    RunRecord *record = g_hash_table_lookup(self->records, arm->id);
    g_autoptr(GError) error = NULL;

    /*
     * Disarmed first.  start_run() can fail and warn, and leaving the
     * source on the record would make the next tick believe a jitter was
     * still pending and never arm another one.
     */
    if (record != NULL)
        g_clear_pointer(&record->jitter, g_source_unref);

    /*
     * Removed while it waited, or turned off.  Neither is an error: a
     * jitter is a delay somebody asked for, not a promise to run
     * whatever happens in the meantime.
     */
    if (routine == NULL || !clawt_routine_get_boolean(routine, "enabled"))
        return G_SOURCE_REMOVE;

    if (start_run(self, routine, &error) == NULL)
        g_warning("routine '%s': %s", arm->id,
                  error != NULL ? error->message : "it did not start");

    return G_SOURCE_REMOVE;
}

/*
 * Holds a due routine back for a random slice of its jitter.
 *
 * g_random_int_range() rather than /dev/urandom: this value spreads load
 * across a rate-limited service, and nothing about the fleet's security
 * depends on somebody being unable to predict when a routine fires.  The
 * mailbox's retry backoff makes the same choice for the same reason.
 *
 * The source is attached through clawt_timeout_add_seconds(), which uses
 * the thread-default context.  g_timeout_add_seconds() would attach to
 * the global default, so in an embedded daemon every jittered routine
 * would be armed and never fire -- worse than not implementing jitter at
 * all, since the routine would simply stop running.
 */
static void
arm_jitter(ClawtRoutineRunner *self, const gchar *id, guint jitter_seconds)
{
    RunRecord *record = record_for(self, id);
    JitterArm *arm = g_new0(JitterArm, 1);
    guint delay = (guint)g_random_int_range(0, (gint)jitter_seconds + 1);

    arm->runner = self;
    arm->id = g_strdup(id);

    record->jitter = clawt_timeout_add_seconds_full(delay, on_jitter_elapsed,
                                                    arm, jitter_arm_free);

    g_debug("routine '%s' is due; holding it %u second%s for jitter", id,
            delay, delay == 1 ? "" : "s");
}

void
clawt_routine_runner_tick(ClawtRoutineRunner *self)
{
    g_autoptr(GDateTime) now = NULL;
    GPtrArray *routines;
    guint i;

    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    now = g_date_time_new_now_local();
    routines = clawt_config_get_routines(self->config);

    for (i = 0; routines != NULL && i < routines->len; i++) {
        ClawtRoutine *routine = g_ptr_array_index(routines, i);
        g_autoptr(ClawtCron) cron = NULL;
        g_autoptr(GDateTime) since = NULL;
        g_autoptr(GDateTime) due = NULL;
        g_autoptr(GError) error = NULL;
        RunRecord *record;
        gint64 jitter;
        const gchar *id = clawt_routine_get_id(routine);

        if (!clawt_routine_get_boolean(routine, "enabled"))
            continue;

        cron = cron_for(self, routine);

        if (cron == NULL)
            continue;

        record = g_hash_table_lookup(self->records, id);

        /*
         * Already waiting out its jitter.  Without this the tick a
         * minute later would find it due all over again -- `last_run` is
         * stamped by start_run(), which has not happened yet -- and arm
         * a second delayed run, so a five-minute jitter would fire five
         * times rather than once.
         */
        if (record != NULL && record->jitter != NULL)
            continue;

        since = (record != NULL && record->last_run > 0)
            ? g_date_time_new_from_unix_local(record->last_run)
            : g_date_time_add_minutes(now, -1);

        due = clawt_cron_next(cron, since);

        if (due == NULL || g_date_time_compare(due, now) > 0)
            continue;

        jitter = clawt_routine_get_int(routine, "jitter_seconds");

        if (jitter > 0) {
            arm_jitter(self, id, (guint)MIN(jitter, MAX_JITTER_SECONDS));
            continue;
        }

        if (start_run(self, routine, &error) == NULL)
            g_warning("routine '%s': %s", id,
                      error != NULL ? error->message : "it did not start");
    }
}

/*
 * One tick.
 *
 * Due is "the last fire time at or before now is after the last run we
 * recorded", which is what makes a tick that arrives late still fire --
 * a machine that was busy for ninety seconds has not missed a minute.
 */
static gboolean
on_tick(gpointer user_data)
{
    clawt_routine_runner_tick(user_data);

    return G_SOURCE_CONTINUE;
}

void
clawt_routine_runner_start(ClawtRoutineRunner *self, GMainContext *context)
{
    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    if (self->tick != NULL)
        return;

    (void)context;

    /*
     * clawt_timeout_add_seconds() rather than g_timeout_add_seconds():
     * the latter attaches to the *global* default context, so in an
     * embedded daemon the tick would never fire and every routine would
     * silently never run. It uses the thread-default context, which the
     * daemon has already pushed by the time this is called.
     */
    self->tick = clawt_timeout_add_seconds(TICK_SECONDS, on_tick, self);
}

void
clawt_routine_runner_stop(ClawtRoutineRunner *self)
{
    GHashTableIter iter;
    gpointer value;

    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    /*
     * The jitters go too, and before the early return -- a stopped
     * runner that still holds an armed timer starts a routine after
     * somebody asked it not to, which is the shape of "a stop that only
     * sends a signal is not a stop".
     */
    g_hash_table_iter_init(&iter, self->records);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        clear_jitter(value);

    if (self->tick == NULL)
        return;

    g_source_destroy(self->tick);
    g_clear_pointer(&self->tick, g_source_unref);
}

void
clawt_routine_runner_catch_up(ClawtRoutineRunner *self)
{
    g_autoptr(GDateTime) now = NULL;
    GPtrArray *routines;
    guint i;

    g_return_if_fail(CLAWT_IS_ROUTINE_RUNNER(self));

    now = g_date_time_new_now_local();
    routines = clawt_config_get_routines(self->config);

    for (i = 0; routines != NULL && i < routines->len; i++) {
        ClawtRoutine *routine = g_ptr_array_index(routines, i);
        g_autoptr(ClawtCron) cron = NULL;
        g_autoptr(GDateTime) since = NULL;
        g_autoptr(GDateTime) due = NULL;
        g_autoptr(GError) error = NULL;
        RunRecord *record;
        const gchar *id = clawt_routine_get_id(routine);

        if (!clawt_routine_get_boolean(routine, "enabled"))
            continue;

        cron = cron_for(self, routine);

        if (cron == NULL)
            continue;

        record = g_hash_table_lookup(self->records, id);

        /*
         * A routine that has never run has not missed anything.  Without
         * this, adding one would fire it immediately -- which is exactly
         * the surprise somebody setting a 09:00 schedule at four in the
         * afternoon does not want.
         */
        if (record == NULL || record->last_run == 0) {
            record = record_for(self, id);
            record->last_run = g_get_real_time() / G_USEC_PER_SEC;
            record->state = CLAWT_RUN_NEVER;
            continue;
        }

        since = g_date_time_new_from_unix_local(record->last_run);
        due = clawt_cron_next(cron, since);

        if (due == NULL || g_date_time_compare(due, now) > 0)
            continue;

        if (clawt_routine_get_boolean(routine, "catch_up")) {
            /*
             * Once, however many were missed.  A laptop opened after a
             * long weekend should not deliver a stack of good mornings.
             */
            if (start_run(self, routine, &error) == NULL)
                g_warning("routine '%s': %s", id,
                          error != NULL ? error->message
                                        : "it did not start");
            continue;
        }

        record->last_run = g_get_real_time() / G_USEC_PER_SEC;
        record->state = CLAWT_RUN_MISSED;
        g_clear_pointer(&record->detail, g_free);
        record->detail = g_date_time_format(due, "due %Y-%m-%d %H:%M, but "
                                                 "the daemon was not running");
    }

    save_state(self);
}

/* ── Computers that did not start ────────────────────────────────── */

guint
clawt_routine_max_start_failures(
	ClawtRoutine *routine
){
	const ClawtSchemaEntry *entry;
	gint64 value;

	g_return_val_if_fail(routine != NULL, 0);

	value = clawt_routine_get_int(routine, "max_start_failures");

	if (value >= 0)
		return (guint)MIN(value, (gint64)G_MAXUINT);

	/*
	 * The default, not 0.  Zero is a real, documented setting -- never
	 * turn it off -- so a typo of -3 would otherwise select a different
	 * working behaviour that nothing could describe.
	 */
	entry = clawt_config_schema_lookup("routines.max_start_failures");

	if (entry == NULL || entry->default_value == NULL)
		return 0;

	return (guint)g_ascii_strtoull(entry->default_value, NULL, 10);
}

gboolean
clawt_routine_runner_note_start(
	ClawtRoutineRunner *self,
	const gchar        *routine_id,
	gint64              run_at_us,
	const gchar        *computer_error
){
	ClawtRoutine *routine;
	RunRecord *record;
	guint limit;

	g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), FALSE);
	g_return_val_if_fail(routine_id != NULL, FALSE);

	/* Removed while its run waited: there is nothing to count against. */
	routine = clawt_config_get_routine(self->config, routine_id);

	if (routine == NULL)
		return FALSE;

	record = record_for(self, routine_id);

	/*
	 * A run from before the last edit says nothing about the routine as
	 * it now is.  Both ways: a stale success must not hide a fresh
	 * failure any more than a stale failure may count towards one.
	 */
	if (run_at_us < record->edited_at_us)
		return FALSE;

	if (computer_error == NULL) {
		if (record->start_failures == 0)
			return FALSE;

		record->start_failures = 0;
		save_state(self);
		return FALSE;
	}

	/*
	 * A disabled routine run by hand has nothing to turn off, and one
	 * already turned off -- or waiting for the caller to do it -- must
	 * not raise the alert a second time.
	 */
	if (!clawt_routine_get_boolean(routine, "enabled") || record->off_after > 0)
		return FALSE;

	record->start_failures++;
	limit = clawt_routine_max_start_failures(routine);

	if (limit == 0 || record->start_failures < limit) {
		save_state(self);
		return FALSE;
	}

	record->off_after = record->start_failures;
	g_free(record->off_error);
	record->off_error = g_strdup(computer_error);
	save_state(self);

	return TRUE;
}

guint
clawt_routine_runner_get_start_failures(
	ClawtRoutineRunner *self,
	const gchar        *routine_id
){
	RunRecord *record;

	g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), 0);

	record = (routine_id != NULL)
		? g_hash_table_lookup(self->records, routine_id) : NULL;

	return (record != NULL) ? record->start_failures : 0;
}

gboolean
clawt_routine_runner_get_turned_off(
	ClawtRoutineRunner  *self,
	const gchar         *routine_id,
	guint               *out_failures,
	const gchar        **out_error
){
	RunRecord *record;

	g_return_val_if_fail(CLAWT_IS_ROUTINE_RUNNER(self), FALSE);

	record = (routine_id != NULL)
		? g_hash_table_lookup(self->records, routine_id) : NULL;

	if (out_failures != NULL)
		*out_failures = (record != NULL) ? record->off_after : 0;

	if (out_error != NULL)
		*out_error = (record != NULL) ? record->off_error : NULL;

	return record != NULL && record->off_after > 0;
}

gchar *
clawt_routine_turned_off_note(
	guint        failures,
	const gchar *error
){
	if (error == NULL || *error == '\0')
		return g_strdup_printf("turned off after %u failed start%s", failures,
		                       failures == 1 ? "" : "s");

	return g_strdup_printf("turned off after %u failed start%s: %s", failures,
	                       failures == 1 ? "" : "s", error);
}

gchar *
clawt_routine_turned_off_alert(
	const gchar *routine_id,
	guint        failures,
	const gchar *error
){
	return g_strdup_printf(
		"Routine '%s' was turned off after %u run%s in a row whose computer "
		"did not start: %s. Fix the computer, then turn the routine back on.",
		(routine_id != NULL) ? routine_id : "?", failures,
		failures == 1 ? "" : "s",
		(error != NULL && *error != '\0') ? error : "no reason was given");
}

const gchar *
clawt_routine_start_failures_hint(void)
{
    return "Turn this routine off after this many runs in a row whose "
           "computer did not start, rather than failing on every run. "
           "0 never turns it off.";
}
