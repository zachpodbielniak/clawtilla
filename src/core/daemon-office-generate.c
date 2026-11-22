/*
 * daemon-office-generate.c - A character made by the daemon itself
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The custodial team makes characters through a connector, with a model
 * deciding what to ask for.  This is the other road: with
 * `office.asset_api` naming a service whose REST API ai-glib speaks
 * (Higgsfield, AiHiggsfieldClient), the daemon makes a character with
 * no model in the loop -- one portrait, from the agent's own picture
 * when it has one, imported as a one-frame character.
 *
 * Money is asked for first, always: the service's estimate is put to
 * the operator as a decision, and nothing is submitted until a person
 * answers "approve" -- the same rule a rebuild follows, and for the same
 * reason: a charge that happened because nobody answered is the one
 * thing this must not do.  Nothing here waits on the network on the
 * daemon's loop: the estimate, the upload, the job and the download are
 * all asynchronous, on the daemon's own context.
 */

#include "clawtilla.h"
#include "clawt-daemon-private.h"
#include "office/clawt-office-pack.h"

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define GENERATE_PREFIX "office-generate-"

/* ── The key ────────────────────────────────────────────────────── */

static gchar *
key_path(ClawtDaemon *self, const gchar *service)
{
    g_autofree gchar *secrets = clawt_config_get_path_value(self->config, "secrets.dir");
    g_autofree gchar *file = g_strdup_printf("%s.key", service);

    return g_build_filename(secrets, "office", file, NULL);
}

gchar *
clawt_daemon_office_store_api_key(ClawtDaemon  *self,
                                  const gchar  *service,
                                  const gchar  *key,
                                  GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *dir = NULL;

    if (g_strcmp0(service, "higgsfield") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the office can make characters itself through higgsfield only "
                    "(office.asset_api)");
        return NULL;
    }
    if (key == NULL || strchr(key, ':') == NULL || key[0] == ':' ||
        key[strlen(key) - 1] == ':') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a Higgsfield key is KEY_ID:KEY_SECRET, from console.higgsfield.ai");
        return NULL;
    }
    path = key_path(self, service);
    dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents_full(path, key, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  error))
        return NULL;
    return g_steal_pointer(&path);
}

/*
 * The generator: the test's when it set one, else a Higgsfield client
 * with the stored key.  The key is read here, never kept, and never in
 * a reply or a log.
 */
static AiMediaGenerator *
generator_for(ClawtDaemon *self, GError **error)
{
    const gchar *service = clawt_config_get_string(self->config, "office.asset_api");
    g_autofree gchar *path = NULL;
    g_autofree gchar *key = NULL;
    g_auto(GStrv) parts = NULL;

    if (self->office_generator_factory != NULL)
        return self->office_generator_factory(self);
    if (g_strcmp0(service, "higgsfield") != 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no service is set to make characters directly "
                            "(office.asset_api: higgsfield)");
        return NULL;
    }
    path = key_path(self, service);
    if (!g_file_get_contents(path, &key, NULL, NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no Higgsfield key is stored: `clawtilla office api-key higgsfield`");
        return NULL;
    }
    g_strstrip(key);
    parts = g_strsplit(key, ":", 2);
    if (parts[0] == NULL || parts[1] == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "the stored Higgsfield key is not KEY_ID:KEY_SECRET; store it again");
        return NULL;
    }
    return AI_MEDIA_GENERATOR(ai_higgsfield_client_new_with_credentials(parts[0], parts[1]));
}

/* What is asked for: the agent, as the sidebar knows it, as a person. */
static AiMediaRequest *
request_for(ClawtDaemon *self, ClawtAgentConfig *agent)
{
    const gchar *id = clawt_agent_config_get_id(agent);
    const gchar *name = clawt_agent_config_get_string(agent, "name");
    const gchar *description = clawt_agent_config_get_string(agent, "description");
    const gchar *color = clawt_avatar_color(id, name, clawt_agent_config_get_string(agent,
                                                                                    "color"));
    g_autofree gchar *prompt = g_strdup_printf(
        "A full-body character for a cosy top-down office game: %s%s%s.  Standing, facing "
        "the viewer, friendly, simple clean shapes, a plain light background.  Their main "
        "colour is %s.",
        name != NULL ? name : id,
        description != NULL && *description != '\0' ? ", who " : "",
        description != NULL ? description : "", color);
    const gchar *model = clawt_config_get_string(self->config, "office.asset_model");

    return ai_media_request_new(model != NULL && *model != '\0'
                                ? model : AI_HIGGSFIELD_MODEL_SOUL_V2, prompt);
}

/* ── Asking ─────────────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon *self;
    gchar       *agent;
    gchar       *model;
} Ask;

static void
ask_free(Ask *ask)
{
    g_object_unref(ask->self);
    g_free(ask->agent);
    g_free(ask->model);
    g_free(ask);
}

static void
on_estimate(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Ask *ask = user_data;
    ClawtDaemon *self = ask->self;
    static const gchar *const options[] = { "approve", "decline", NULL };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDecision) decision = NULL;
    g_autofree gchar *uuid = g_uuid_string_random();
    g_autofree gchar *id = g_strconcat(GENERATE_PREFIX, uuid, NULL);
    g_autofree gchar *cost = NULL;
    g_autofree gchar *question = NULL;
    gint64 micro = -1;

    if (!ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(source), result, &micro,
                                            &error) || micro < 0) {
        /* no estimate is not a free generation: the card says so */
        g_message("office: no estimate for %s's character: %s", ask->agent,
                  error != NULL ? error->message : "the service gave none");
        cost = g_strdup("an amount the service would not estimate");
    } else {
        cost = g_strdup_printf("about $%.2f", (gdouble)micro / 1e6);
    }
    question = g_strdup_printf(
        "Make a character for %s with Higgsfield (%s)?  It costs %s, from your "
        "Higgsfield balance.  Approve submits one image from %s's picture and imports it "
        "as its character; decline spends nothing.",
        ask->agent, ask->model, cost, ask->agent);
    decision = clawt_decision_new(id, ask->agent, question);
    clawt_decision_set_options(decision, options);
    /* no deadline: nothing spends money but a person's yes */
    clawt_decision_set_default(decision, "decline", "nobody approved it, so nothing was spent");
    {
        /* post answers the stored id, which is ours to free */
        g_autofree gchar *stored = clawt_decision_store_post(self->decisions, decision,
                                                             &error);

        if (stored == NULL)
            g_warning("office: could not ask about %s's character: %s", ask->agent,
                      error->message);
        else if (self->bus != NULL)
            clawt_event_bus_emit(self->bus, "decision.asked", ask->agent);
    }
    ask_free(ask);
}

gboolean
clawt_daemon_office_generate_ask(ClawtDaemon  *self,
                                 const gchar  *agent_id,
                                 GError      **error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(AiMediaGenerator) generator = NULL;
    g_autoptr(AiMediaRequest) request = NULL;
    Ask *ask;

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "there is no agent '%s'",
                    agent_id != NULL ? agent_id : "");
        return FALSE;
    }
    generator = generator_for(self, error);
    if (generator == NULL)
        return FALSE;
    request = request_for(self, agent);
    ask = g_new0(Ask, 1);
    ask->self = g_object_ref(self);
    ask->agent = g_strdup(agent_id);
    ask->model = g_strdup(ai_media_request_get_model(request));

    /* on the daemon's context: the estimate is a network call */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    ai_media_generator_estimate_async(generator, request, NULL, on_estimate, ask);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    return TRUE;
}

/* ── Making it ──────────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon      *self;
    AiMediaGenerator *generator;
    AiMediaRequest   *request;
    gchar            *agent;
    gint64            cost;
} Job;

static void
job_free(Job *job)
{
    g_object_unref(job->self);
    g_object_unref(job->generator);
    g_object_unref(job->request);
    g_free(job->agent);
    g_free(job);
}

static void
job_failed(Job *job, const gchar *what, const GError *error)
{
    g_warning("office: %s's character was not made: %s: %s", job->agent, what,
              error != NULL ? error->message : "no reason given");
    job_free(job);
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    ClawtDaemon *self = job->self;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = ai_media_generator_download_finish(AI_MEDIA_GENERATOR(source),
                                                                 result, &error);
    g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
    g_autofree gchar *tmp = NULL;
    g_autofree gchar *provenance = NULL;
    g_autoptr(JsonObject) animations = json_object_new();
    JsonObject *idle = json_object_new();
    gint fd;
    guint width = 0;
    guint height = 0;
    gsize length = 0;
    const guint8 *data;

    if (bytes == NULL) {
        job_failed(job, "the download failed", error);
        return;
    }
    data = g_bytes_get_data(bytes, &length);
    fd = g_file_open_tmp("clawt-character-XXXXXX.png", &tmp, &error);
    if (fd < 0) {
        job_failed(job, "no temporary file", error);
        return;
    }
    close(fd);
    if (!g_file_set_contents(tmp, (const gchar *)data, (gssize)length, &error) ||
        !clawt_office_png_size(tmp, &width, &height)) {
        g_unlink(tmp);
        job_failed(job, "the result is not a PNG", error);
        return;
    }
    provenance = g_strdup_printf(
        "generated by Higgsfield (%s) for clawtilla's office%s; Higgsfield's terms apply "
        "(docs/higgsfield.org)", ai_media_request_get_model(job->request),
        job->cost >= 0 ? "" : ", cost unknown");
    json_object_set_int_member(idle, "row", 0);
    json_object_set_int_member(idle, "frames", 1);
    json_object_set_object_member(animations, "idle", idle);
    if (!clawt_office_character_import_asset(root, job->agent, tmp, "generated/portrait.png",
                                             NULL, provenance, NULL, &error) ||
        !clawt_office_character_set_sheet(root, job->agent, "generated/portrait.png",
                                          width, height, animations, &error)) {
        g_unlink(tmp);
        job_failed(job, "it could not be imported", error);
        return;
    }
    g_unlink(tmp);
    g_message("office: made %s's character", job->agent);
    if (self->bus != NULL)
        clawt_event_bus_emit(self->bus, "office.changed", job->agent);
    job_free(job);
}

static void
on_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiMediaJob) done = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(source),
                                                                result, &error);
    const gchar *const *urls;

    if (done == NULL || ai_media_job_get_state(done) != AI_MEDIA_JOB_STATE_DONE ||
        ai_media_job_get_n_result_urls(done) == 0) {
        job_failed(job, "the job did not finish with a result", error);
        return;
    }
    job->cost = ai_media_job_get_cost_micro_usd(done);
    urls = ai_media_job_get_result_urls(done);
    ai_media_generator_download_async(job->generator, urls[0], NULL, on_downloaded, job);
}

static void
on_submitted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(source), result,
                                                            &error);
    guint first = job->self->office_generate_poll_ms > 0 ? job->self->office_generate_poll_ms
                                                         : 3000;

    if (id == NULL) {
        job_failed(job, "it was not accepted", error);
        return;
    }
    ai_media_generator_wait_async(job->generator, id, first, MAX(first, 20000u),
                                  10 * 60 * 1000, NULL, on_finished, job);
}

static void
submit(Job *job)
{
    ai_media_request_ensure_idempotency_key(job->request);
    ai_media_generator_submit_async(job->generator, job->request, NULL, on_submitted, job);
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *url = ai_higgsfield_client_upload_finish(AI_HIGGSFIELD_CLIENT(source),
                                                               result, &error);

    /* a reference that would not upload costs the likeness, not the character */
    if (url != NULL)
        ai_media_request_set_image_url(job->request, url);
    else
        g_message("office: %s's picture was not uploaded, so the character is made from "
                  "the description alone: %s", job->agent,
                  error != NULL ? error->message : "no reason given");
    submit(job);
}

/* Approved: upload the agent's own picture if it has a PNG one, then submit. */
static void
start(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *avatar = NULL;
    guint w = 0;
    guint h = 0;
    Job *job;
    AiMediaGenerator *generator;

    if (agent == NULL)
        return;
    generator = generator_for(self, &error);
    if (generator == NULL) {
        g_warning("office: %s's character was approved and cannot be made: %s", agent_id,
                  error->message);
        return;
    }
    job = g_new0(Job, 1);
    job->self = g_object_ref(self);
    job->generator = generator;
    job->request = request_for(self, agent);
    job->agent = g_strdup(agent_id);
    job->cost = -1;

    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    avatar = clawt_agent_config_get_path_value(agent, "avatar");
    if (avatar != NULL && AI_IS_HIGGSFIELD_CLIENT(generator) &&
        clawt_office_png_size(avatar, &w, &h)) {
        g_autofree gchar *data = NULL;
        gsize length = 0;

        if (g_file_get_contents(avatar, &data, &length, NULL)) {
            g_autoptr(GBytes) bytes = g_bytes_new_take(g_steal_pointer(&data), length);

            ai_higgsfield_client_upload_async(AI_HIGGSFIELD_CLIENT(generator), bytes,
                                              "image/png", NULL, on_uploaded, job);
            goto out;
        }
    }
    submit(job);
out:
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
}

gboolean
clawt_daemon_office_generate_settle(ClawtDaemon   *self,
                                    ClawtDecision *decision,
                                    const gchar   *how)
{
    const gchar *answer;

    if (decision == NULL ||
        !g_str_has_prefix(clawt_decision_get_id(decision), GENERATE_PREFIX))
        return FALSE;
    answer = clawt_decision_get_answer(decision);

    /* only the exact word spends money -- "maybe later" is not a yes */
    if (g_strcmp0(how, "answered") == 0 &&
        g_ascii_strcasecmp(answer != NULL ? answer : "", "approve") == 0)
        start(self, clawt_decision_get_agent(decision));
    return TRUE;
}
