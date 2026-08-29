/*
 * clawt-summariser.c - Turning finished work into memories
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "memory/clawt-summariser.h"

#include <string.h>

struct _ClawtSummariser {
    GObject parent_instance;

    ClawtConfig     *config;
    AiProvider      *provider;
    AiToolExecutor  *executor;

    guint budget_bytes;
    guint max_turns;

    /*
     * The context an asynchronous answer must arrive on.  Named rather
     * than taken from whatever is thread-default when the call is made:
     * an embedded daemon runs its own loop on its own context, and a
     * GTask that took the global default would call back onto a loop
     * nobody iterates.
     */
    GMainContext *main_context;

    /*
     * Set for the length of one summary, and one is all there may be.
     *
     * The tool callback writes into @collected rather than into a store,
     * because the model call happens on a worker thread and the store is
     * the daemon's: the memories are written back on the daemon's own
     * context, in _finish(), where every other write to it happens.
     */
    gboolean          running;
    GPtrArray        *collected;   /* ClawtMemory*, owned */
    const gchar      *source;
    gchar            *range;
};

G_DEFINE_FINAL_TYPE(ClawtSummariser, clawt_summariser, G_TYPE_OBJECT)

/*
 * Enough transcript to be worth summarising and little enough that a
 * runaway conversation cannot turn one finished task into the largest
 * request the fleet has ever sent.
 */
#define DEFAULT_BUDGET_BYTES 16384

/*
 * Two turns: one for the tool calls, one for the model to stop.
 *
 * The designer needs many because it is a conversation; this is one
 * pass over text that is already written.  A larger number here buys
 * nothing and is billed to whoever turned summarising on.
 */
#define DEFAULT_MAX_TURNS 2

/* ── The one tool ────────────────────────────────────────────────── */

static gchar *
tool_remember(AiToolUse *use, GCancellable *cancellable, GError **error,
              gpointer user_data)
{
    ClawtSummariser *self = user_data;
    g_autoptr(ClawtMemory) memory = NULL;
    const gchar *content = ai_tool_use_get_input_string(use, "content");
    const gchar *category = ai_tool_use_get_input_string(use, "category");
    const gchar *importance = ai_tool_use_get_input_string(use, "importance");
    const gchar *tags = ai_tool_use_get_input_string(use, "tags");

    (void)cancellable;

    if (content == NULL || *content == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a memory with no content is not a memory");
        return NULL;
    }

    if (self->collected == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "there is no summary in progress");
        return NULL;
    }

    memory = clawt_memory_new(content);
    memory->summary = g_strdup(ai_tool_use_get_input_string(use, "summary"));

    if (category != NULL && *category != '\0') {
        g_free(memory->category);
        memory->category = g_strdup(category);
    }

    if (importance != NULL && *importance != '\0') {
        g_free(memory->importance);
        memory->importance = g_strdup(importance);
    }

    /*
     * The source and the range are stamped here rather than asked for.
     *
     * A memory that turns out to be wrong is only fixable if the
     * conversation that produced it can be found again, and a model
     * asked to record its own provenance will sometimes record the
     * wrong one -- it is the one fact in the memory that nobody but
     * clawtilla knows for certain.
     */
    memory->source = g_strdup(self->source);
    memory->tags = (tags != NULL && *tags != '\0')
                   ? g_strconcat(tags, ",", self->range, NULL)
                   : g_strdup(self->range);

    g_ptr_array_add(self->collected, g_steal_pointer(&memory));

    return g_strdup("Recorded.");
}

static void
register_tools(ClawtSummariser *self)
{
    static const ClawtParamInfo remember_params[] = {
        { "content", "string",
          "The fact, in full, written so it is useful to somebody who "
          "was not there.", TRUE },
        { "summary", "string",
          "One line, for a listing.", FALSE },
        { "category", "string",
          "One of the shared categories: general, decision, preference, "
          "fact, person, project, gotcha.", FALSE },
        { "importance", "string",
          "low, normal, high or critical.", FALSE },
        { "tags", "string",
          "Comma-separated, for narrowing a search later.", FALSE }
    };

    g_autoptr(AiTool) tool = NULL;
    gsize i;

    tool = ai_tool_new("remember",
                       "Record one thing worth knowing next time. Call it "
                       "once per fact. Call it not at all if the work "
                       "established nothing worth keeping -- an invented "
                       "memory costs more than a missing one.");

    for (i = 0; i < G_N_ELEMENTS(remember_params); i++)
        ai_tool_add_parameter(tool, remember_params[i].name,
                              remember_params[i].type_name,
                              remember_params[i].description,
                              remember_params[i].required);

    ai_tool_executor_register_callback(self->executor, tool, tool_remember,
                                       self, NULL);
}

/* ── Construction ────────────────────────────────────────────────── */

ClawtSummariser *
clawt_summariser_new(ClawtConfig *config)
{
    ClawtSummariser *self;

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    self = g_object_new(CLAWT_TYPE_SUMMARISER, NULL);
    self->config = g_object_ref(config);

    register_tools(self);

    return self;
}

void
clawt_summariser_set_provider(ClawtSummariser *self, AiProvider *provider)
{
    g_return_if_fail(CLAWT_IS_SUMMARISER(self));

    g_clear_object(&self->provider);

    if (provider != NULL)
        self->provider = g_object_ref(provider);
}

gboolean
clawt_summariser_use_configured_provider(ClawtSummariser *self,
                                         GError **error)
{
    g_autoptr(AiConfig) ai_config = NULL;
    const gchar *provider_name;
    const gchar *model;
    GObject *provider;

    g_return_val_if_fail(CLAWT_IS_SUMMARISER(self), FALSE);

    if (!clawt_config_get_boolean(self->config, "ai_assist.enabled")) {
        /*
         * The same provider the designer uses, so the same switch turns
         * both off.  Named rather than silently skipped: a fleet with
         * `memories.summarise: true` and no assist provider would
         * otherwise summarise nothing for ever and say nothing about it.
         */
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "summarising needs an AI provider; set "
                            "ai_assist.enabled: true");
        return FALSE;
    }

    provider_name = clawt_config_get_string(self->config,
                                            "ai_assist.provider");
    model = clawt_config_get_string(self->config, "ai_assist.model");

    if (provider_name == NULL || *provider_name == '\0')
        provider_name = "claude-code";

    ai_config = ai_config_new();

    if (model != NULL && *model != '\0')
        ai_config_set_default_model(ai_config, model);

    provider = ai_provider_factory_new_from_string(provider_name, ai_config,
                                                   error);

    if (provider == NULL) {
        g_prefix_error(error, "AI provider '%s': ", provider_name);
        return FALSE;
    }

    clawt_summariser_set_provider(self, AI_PROVIDER(provider));
    g_object_unref(provider);

    return TRUE;
}

void
clawt_summariser_set_budget_bytes(ClawtSummariser *self, guint bytes)
{
    g_return_if_fail(CLAWT_IS_SUMMARISER(self));

    self->budget_bytes = (bytes > 0) ? bytes : DEFAULT_BUDGET_BYTES;
}

guint
clawt_summariser_get_budget_bytes(ClawtSummariser *self)
{
    g_return_val_if_fail(CLAWT_IS_SUMMARISER(self), 0);

    return self->budget_bytes;
}

/* ── Running one summary ─────────────────────────────────────────── */

/*
 * Runs the model and leaves what it wrote in @self->collected.
 *
 * No store is touched here, because this runs on a worker thread on the
 * asynchronous path and the store belongs to the daemon's own context.
 */
static gboolean
distil(ClawtSummariser *self, const gchar *source, const gchar *transcript,
       gint64 from_at, gint64 to_at, GCancellable *cancellable,
       GError **error)
{
    g_autofree gchar *system_prompt = NULL;
    g_autofree gchar *trimmed = NULL;
    g_autofree gchar *reply = NULL;
    g_autoptr(AiMessage) message = NULL;
    GList *messages = NULL;

    /*
     * Cut to the most recent budget rather than the first: what a piece
     * of work concluded is at the end of it, and a summary taken from
     * the opening of a long conversation records the plan rather than
     * the outcome.  On a character boundary, so a sliced multi-byte
     * sequence does not reach the model as a replacement character in
     * the middle of a word.
     */
    trimmed = clawt_utf8_truncate(transcript, self->budget_bytes, TRUE);

    self->source = source;

    /*
     * The range, as a tag, so a memory can be traced back to the part of
     * the conversation it came from.  Seconds, which is what
     * #ClawtMessage carries.
     */
    g_free(self->range);
    self->range = g_strdup_printf("transcript:%" G_GINT64_FORMAT
                                  "-%" G_GINT64_FORMAT, from_at, to_at);

    system_prompt = g_strdup_printf(
        "You are reading a finished piece of work and recording what is "
        "worth knowing next time.\n"
        "\n"
        "Call `remember` once per fact. Record a decision and why it was "
        "taken, a preference the operator expressed, a fact that cost "
        "effort to establish, a footgun that was hit. Do not record what "
        "is already written down somewhere that can be read again, and do "
        "not record the narrative of the work itself.\n"
        "\n"
        "%s\n"
        "\n"
        "If nothing here is worth keeping, call nothing and say so. That "
        "is a correct answer and the common one.\n"
        "\n"
        "This work is identified as '%s'.",
        clawt_memory_provenance_rule(),
        source != NULL ? source : "unknown");

    message = ai_message_new_user(trimmed);
    messages = g_list_append(NULL, message);

    reply = ai_tool_executor_run(self->executor, self->provider, messages,
                                 system_prompt, self->max_turns, cancellable,
                                 error);

    g_list_free(messages);
    self->source = NULL;

    /*
     * A model that wrote nothing is not a failure.  There is often
     * nothing in a piece of work worth remembering, and a summariser
     * that treated that as an error would fill a fleet's log with
     * reports of it working correctly.
     */
    return reply != NULL;
}

/*
 * Writes what distil() collected, and empties the collection.
 *
 * Always on the thread that owns the store: the synchronous call\'s
 * caller, or the daemon\'s context in _finish().
 */
static guint
commit(ClawtSummariser *self, ClawtMemoryStore *into)
{
    guint written = 0;
    guint i;

    for (i = 0; self->collected != NULL && i < self->collected->len; i++) {
        g_autoptr(GError) local = NULL;
        g_autofree gchar *id = NULL;

        id = clawt_memory_store_add(into,
                                    g_ptr_array_index(self->collected, i),
                                    &local);

        if (id == NULL)
            g_warning("summariser: %s", local->message);
        else
            written++;
    }

    if (self->collected != NULL)
        g_ptr_array_set_size(self->collected, 0);

    return written;
}

/*
 * Claims the summariser for one summary.
 *
 * One at a time, and said so rather than allowed: the collection, the
 * source and the range are per-summary state on the instance, and two
 * overlapping calls would file each other\'s memories under each other\'s
 * transcripts.  The daemon holds one summariser for the whole fleet, so
 * two tasks finishing in the same second is the ordinary case rather
 * than an exotic one.
 */
static gboolean
claim(ClawtSummariser *self, ClawtMemoryStore *into, GError **error)
{
    if (self->provider == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this summariser has no provider");
        return FALSE;
    }

    if (into == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "there is nowhere to write a summary");
        return FALSE;
    }

    if (self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "a summary is already running; this one is "
                            "skipped rather than queued");
        return FALSE;
    }

    self->running = TRUE;
    g_ptr_array_set_size(self->collected, 0);

    return TRUE;
}

guint
clawt_summariser_summarise(ClawtSummariser *self, ClawtMemoryStore *into,
                           const gchar *source, const gchar *transcript,
                           gint64 from_at, gint64 to_at,
                           GCancellable *cancellable, GError **error)
{
    guint written;

    g_return_val_if_fail(CLAWT_IS_SUMMARISER(self), 0);
    g_return_val_if_fail(transcript != NULL, 0);

    if (!claim(self, into, error))
        return 0;

    if (!distil(self, source, transcript, from_at, to_at, cancellable,
                error)) {
        self->running = FALSE;
        g_ptr_array_set_size(self->collected, 0);
        return 0;
    }

    written = commit(self, into);
    self->running = FALSE;

    return written;
}

/* ── The asynchronous form, which is the one the daemon uses ─────── */

typedef struct {
    ClawtMemoryStore *into;      /* owned */
    gchar            *source;
    gchar            *transcript;
    gint64            from_at;
    gint64            to_at;
} SummaryJob;

static void
summary_job_free(gpointer data)
{
    SummaryJob *job = data;

    g_clear_object(&job->into);
    g_free(job->source);
    g_free(job->transcript);
    g_free(job);
}

static void
summarise_worker(GTask *task, gpointer source_object, gpointer task_data,
                 GCancellable *cancellable)
{
    ClawtSummariser *self = source_object;
    SummaryJob *job = task_data;
    GError *error = NULL;

    if (!distil(self, job->source, job->transcript, job->from_at, job->to_at,
                cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

void
clawt_summariser_set_main_context(ClawtSummariser *self,
                                  GMainContext    *context)
{
    g_return_if_fail(CLAWT_IS_SUMMARISER(self));

    self->main_context = context;
}

void
clawt_summariser_summarise_async(ClawtSummariser *self,
                                 ClawtMemoryStore *into, const gchar *source,
                                 const gchar *transcript, gint64 from_at,
                                 gint64 to_at, GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error = NULL;
    SummaryJob *job;

    g_return_if_fail(CLAWT_IS_SUMMARISER(self));
    g_return_if_fail(transcript != NULL);

    /*
     * The context is pushed around g_task_new(), which is where the task
     * captures the one its callback will be dispatched on.  A daemon
     * embedded in another program runs its own loop on its own context,
     * and a task that took the global default would call back onto a
     * loop nobody iterates -- the summary would run and its memories
     * would never be written.
     */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);

    task = g_task_new(self, cancellable, callback, user_data);

    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);

    g_task_set_source_tag(task, clawt_summariser_summarise_async);

    if (!claim(self, into, &error)) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    job = g_new0(SummaryJob, 1);
    job->into = g_object_ref(into);
    job->source = g_strdup(source);
    job->transcript = g_strdup(transcript);
    job->from_at = from_at;
    job->to_at = to_at;

    g_task_set_task_data(task, job, summary_job_free);
    g_task_run_in_thread(task, summarise_worker);
}

guint
clawt_summariser_summarise_finish(ClawtSummariser *self, GAsyncResult *result,
                                  GError **error)
{
    SummaryJob *job;
    guint written;

    g_return_val_if_fail(CLAWT_IS_SUMMARISER(self), 0);
    g_return_val_if_fail(g_task_is_valid(result, self), 0);

    job = g_task_get_task_data(G_TASK(result));

    if (!g_task_propagate_boolean(G_TASK(result), error)) {
        /*
         * The claim is released here and only here on this path.  A
         * summariser left claimed by a failed model call would refuse
         * every summary afterwards, and nothing would say why.
         */
        if (self->running) {
            self->running = FALSE;
            g_ptr_array_set_size(self->collected, 0);
        }

        return 0;
    }

    /*
     * Written from here rather than from the worker: this runs on the
     * context the task was created on, which is the daemon's, and that
     * is the thread every other write to this store happens on.
     */
    written = commit(self, job->into);
    self->running = FALSE;

    return written;
}

gchar *
clawt_summariser_nudge_text(guint nudge_turns)
{
    if (nudge_turns == 0)
        return NULL;

    return g_strdup_printf(
        "About every %u turns, before you finish, ask whether this turn "
        "established anything you would be worse off not knowing next "
        "time -- a decision and why, a preference, a fact that cost you "
        "effort, a footgun. If it did, record it with "
        "clawtilla_memory_add. Do not record what you can read again.",
        nudge_turns);
}

static void
clawt_summariser_dispose(GObject *object)
{
    ClawtSummariser *self = CLAWT_SUMMARISER(object);

    g_clear_object(&self->executor);
    g_clear_object(&self->provider);
    g_clear_object(&self->config);
    g_clear_pointer(&self->collected, g_ptr_array_unref);
    g_clear_pointer(&self->range, g_free);

    G_OBJECT_CLASS(clawt_summariser_parent_class)->dispose(object);
}

static void
clawt_summariser_class_init(ClawtSummariserClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_summariser_dispose;
}

static void
clawt_summariser_init(ClawtSummariser *self)
{
    /*
     * _new_empty(), never _new().  The latter silently grants `bash`,
     * `read`, `write` and `edit`, and unregister() cannot take a
     * built-in back -- so a summariser built with it could run commands
     * on the machine, unattended, after every finished task.
     */
    self->executor = ai_tool_executor_new_empty();
    self->budget_bytes = DEFAULT_BUDGET_BYTES;
    self->max_turns = DEFAULT_MAX_TURNS;
    self->collected = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_memory_free);
}
