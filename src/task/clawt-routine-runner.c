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

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clawt_routine_runner_finalize(GObject *object)
{
    ClawtRoutineRunner *self = CLAWT_ROUTINE_RUNNER(object);

    clawt_routine_runner_stop(self);

    g_clear_object(&self->config);
    g_clear_pointer(&self->state_path, g_free);
    g_clear_pointer(&self->records, g_hash_table_unref);

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
}

ClawtRoutineRunner *
clawt_routine_runner_new(ClawtConfig *config, const gchar *state_path)
{
    ClawtRoutineRunner *self = g_object_new(CLAWT_TYPE_ROUTINE_RUNNER, NULL);

    self->config = g_object_ref(config);
    self->state_path = g_strdup(state_path);
    load_state(self);

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
}

/* ── Deciding ────────────────────────────────────────────────────── */

/*
 * The cron a routine means, or %NULL.
 *
 * A malformed expression is a warning naming the routine, not a
 * refusal to schedule anything else: one routine that cannot be parsed
 * should not take the rest down with it.
 */
static ClawtCron *
cron_for(ClawtRoutine *routine)
{
    g_autofree gchar *expression = NULL;
    g_autoptr(GError) error = NULL;
    ClawtCron *cron;

    expression = clawt_routine_get_cron(routine, &error);

    if (expression == NULL) {
        if (error != NULL)
            g_warning("routine '%s': %s", clawt_routine_get_id(routine),
                      error->message);

        return NULL;
    }

    cron = clawt_cron_parse(expression, &error);

    if (cron == NULL)
        g_warning("routine '%s': %s", clawt_routine_get_id(routine),
                  error->message);

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

    cron = cron_for(routine);

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

    g_string_append(out, instructions != NULL ? instructions : "");
    g_string_append(out, "\n\n---\n");
    g_string_append_printf(out,
        "This is the scheduled routine '%s', started by clawtilla rather "
        "than by a person. Nobody is waiting on this conversation, so "
        "asking a question here reaches nobody -- do the work with what "
        "you have, and if something is genuinely blocking, say so with "
        "clawtilla_message_user and stop.\n",
        clawt_routine_get_id(routine));

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

        cron = cron_for(routine);

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

        cron = cron_for(routine);

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
