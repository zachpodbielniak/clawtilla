/*
 * clawt-higgsfield-mcp.c - Higgsfield generation through a connector's sign-in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Higgsfield bills two ways.  Its REST API (ai-glib's AiHiggsfieldClient,
 * a key pair from the console) spends an API balance of its own; its MCP
 * server, signed in to with the account, spends the account's app
 * credits -- the ones the `hf` CLI and the website spend.  On the day
 * this was written an account had app credits and an empty API balance,
 * so the REST road answered every submit with 403 not_enough_credits.
 * This is the second road, chosen with `office.asset_api: higgsfield-mcp`:
 * the same AiMediaGenerator the daemon drives, on Higgsfield's MCP tools.
 *
 * The tool shapes are recorded in docs/higgsfield.org; the arguments are
 * not the names a reader would guess (job_status takes `jobId`).
 */

#include "clawtilla.h"
#include "clawt-http.h"

#include <string.h>

struct _ClawtHiggsfieldMcp {
    GObject             parent_instance;
    ClawtMcpHttpClient *client;
    gchar              *connector;
    gdouble             estimate_credits;
};

static void clawt_higgsfield_mcp_generator_init(AiMediaGeneratorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(ClawtHiggsfieldMcp, clawt_higgsfield_mcp, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(AI_TYPE_MEDIA_GENERATOR,
                                                    clawt_higgsfield_mcp_generator_init))

static void
clawt_higgsfield_mcp_finalize(GObject *object)
{
    ClawtHiggsfieldMcp *self = CLAWT_HIGGSFIELD_MCP(object);

    g_clear_object(&self->client);
    g_free(self->connector);
    G_OBJECT_CLASS(clawt_higgsfield_mcp_parent_class)->finalize(object);
}

static void
clawt_higgsfield_mcp_class_init(ClawtHiggsfieldMcpClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_higgsfield_mcp_finalize;
}

static void
clawt_higgsfield_mcp_init(ClawtHiggsfieldMcp *self)
{
    self->estimate_credits = -1.0;
}

ClawtHiggsfieldMcp *
clawt_higgsfield_mcp_new(ClawtMcpHttpClient *client)
{
    ClawtHiggsfieldMcp *self;

    g_return_val_if_fail(CLAWT_IS_MCP_HTTP_CLIENT(client), NULL);

    self = g_object_new(CLAWT_TYPE_HIGGSFIELD_MCP, NULL);
    self->client = g_object_ref(client);
    return self;
}

const gchar *
clawt_higgsfield_mcp_get_connector(ClawtHiggsfieldMcp *self)
{
    g_return_val_if_fail(CLAWT_IS_HIGGSFIELD_MCP(self), NULL);

    return self->connector;
}

void
clawt_higgsfield_mcp_set_connector(ClawtHiggsfieldMcp *self, const gchar *name)
{
    g_return_if_fail(CLAWT_IS_HIGGSFIELD_MCP(self));

    g_free(self->connector);
    self->connector = g_strdup(name);
}

gdouble
clawt_higgsfield_mcp_get_estimate_credits(ClawtHiggsfieldMcp *self)
{
    g_return_val_if_fail(CLAWT_IS_HIGGSFIELD_MCP(self), -1.0);

    return self->estimate_credits;
}

/* ── What is sent ───────────────────────────────────────────────── */

/* The request's image, which ai-glib keeps only as a parameter. */
static gchar *
request_image(AiMediaRequest *request)
{
    g_autofree gchar *json = ai_media_request_to_json(request);
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;

    if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
        return NULL;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    object = json_node_get_object(root);
    return g_strdup(json_object_get_string_member_with_default(object, "image_url", NULL));
}

JsonObject *
clawt_higgsfield_mcp_params_for(AiMediaRequest *request)
{
    JsonObject *params = json_object_new();
    const gchar *model = ai_media_request_get_model(request);
    g_autofree gchar *image = request_image(request);

    json_object_set_string_member(params, "model",
                                  model != NULL && *model != '\0'
                                  ? model : CLAWT_HIGGSFIELD_MCP_DEFAULT_MODEL);
    json_object_set_string_member(params, "prompt", ai_media_request_get_prompt(request));
    json_object_set_string_member(params, "aspect_ratio", "1:1");
    json_object_set_int_member(params, "count", 1);

    /*
     * A reference image is a media id from media_upload, never a URL: the
     * server says so and refuses one.  The role is the one image models
     * take; the server coerces it when a model names it differently.
     */
    if (image != NULL && *image != '\0') {
        JsonArray *medias = json_array_new();
        JsonObject *medium = json_object_new();

        json_object_set_string_member(medium, "value", image);
        json_object_set_string_member(medium, "role", "image");
        json_array_add_object_element(medias, medium);
        json_object_set_array_member(params, "medias", medias);
    }
    return params;
}

static JsonObject *
wrap_params(JsonObject *params)
{
    JsonObject *arguments = json_object_new();

    json_object_set_object_member(arguments, "params", params);
    return arguments;
}

/* structuredContent, or NULL */
static JsonObject *
structured(JsonObject *result)
{
    JsonNode *node = result != NULL ? json_object_get_member(result, "structuredContent") : NULL;

    return node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

/* ── Estimate ───────────────────────────────────────────────────── */

static void
on_estimated(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    ClawtHiggsfieldMcp *self = g_task_get_source_object(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = clawt_mcp_http_client_call_tool_finish(
        CLAWT_MCP_HTTP_CLIENT(source), result, &error);
    JsonObject *sc = structured(outcome);
    JsonObject *cost = sc != NULL && json_object_has_member(sc, "cost") &&
                       JSON_NODE_HOLDS_OBJECT(json_object_get_member(sc, "cost"))
        ? json_object_get_object_member(sc, "cost") : NULL;

    if (outcome == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    /* a quote that names no price is no estimate: the card says unknown */
    self->estimate_credits = cost != NULL && json_object_has_member(cost, "credits")
        ? json_object_get_double_member(cost, "credits") : -1.0;
    g_task_return_int(task, AI_MEDIA_COST_UNKNOWN);
}

static void
hm_estimate_async(AiMediaGenerator    *generator,
                  AiMediaRequest      *request,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    ClawtHiggsfieldMcp *self = CLAWT_HIGGSFIELD_MCP(generator);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_autoptr(JsonObject) arguments = wrap_params(clawt_higgsfield_mcp_params_for(request));

    self->estimate_credits = -1.0;
    clawt_mcp_http_client_call_tool_async(self->client, "estimate_image_cost", arguments,
                                          cancellable, on_estimated, task);
}

static gboolean
hm_estimate_finish(AiMediaGenerator  *generator,
                   GAsyncResult      *result,
                   gint64            *micro_usd,
                   GError           **error)
{
    g_autoptr(GError) inner = NULL;
    gssize value = g_task_propagate_int(G_TASK(result), &inner);

    (void)generator;
    if (inner != NULL) {
        g_propagate_error(error, g_steal_pointer(&inner));
        return FALSE;
    }
    if (micro_usd != NULL)
        *micro_usd = (gint64)value;
    return TRUE;
}

/* ── Submit ─────────────────────────────────────────────────────── */

static void
on_submitted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = clawt_mcp_http_client_call_tool_finish(
        CLAWT_MCP_HTTP_CLIENT(source), result, &error);
    JsonObject *sc = structured(outcome);
    JsonArray *results = sc != NULL && json_object_has_member(sc, "results") &&
                         JSON_NODE_HOLDS_ARRAY(json_object_get_member(sc, "results"))
        ? json_object_get_array_member(sc, "results") : NULL;
    JsonObject *first = results != NULL && json_array_get_length(results) > 0
        ? json_array_get_object_element(results, 0) : NULL;
    const gchar *id = first != NULL
        ? json_object_get_string_member_with_default(first, "id", NULL) : NULL;

    if (outcome == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    if (id == NULL) {
        g_autofree gchar *text = clawt_mcp_http_result_text(outcome);

        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "generate_image named no job: %s", text);
        return;
    }
    g_task_return_pointer(task, g_strdup(id), g_free);
}

static void
hm_submit_async(AiMediaGenerator    *generator,
                AiMediaRequest      *request,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    ClawtHiggsfieldMcp *self = CLAWT_HIGGSFIELD_MCP(generator);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_autoptr(JsonObject) arguments = wrap_params(clawt_higgsfield_mcp_params_for(request));

    clawt_mcp_http_client_call_tool_async(self->client, "generate_image", arguments,
                                          cancellable, on_submitted, task);
}

static gchar *
hm_submit_finish(AiMediaGenerator *generator, GAsyncResult *result, GError **error)
{
    (void)generator;
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Poll ───────────────────────────────────────────────────────── */

/* Higgsfield's job status as ai-glib's state; unknown words are still running. */
static AiMediaJobState
state_for(const gchar *status)
{
    if (g_strcmp0(status, "completed") == 0 || g_strcmp0(status, "succeeded") == 0)
        return AI_MEDIA_JOB_STATE_DONE;
    if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "nsfw") == 0 ||
        g_strcmp0(status, "ip_detected") == 0)
        return AI_MEDIA_JOB_STATE_FAILED;
    if (g_strcmp0(status, "canceled") == 0 || g_strcmp0(status, "cancelled") == 0)
        return AI_MEDIA_JOB_STATE_CANCELLED;
    if (g_strcmp0(status, "queued") == 0 || g_strcmp0(status, "pending") == 0)
        return AI_MEDIA_JOB_STATE_QUEUED;
    return AI_MEDIA_JOB_STATE_RUNNING;
}

typedef struct {
    gchar *job_id;
} Poll;

static void
poll_free(Poll *poll)
{
    g_free(poll->job_id);
    g_free(poll);
}

static void
on_polled(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    Poll *poll = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = clawt_mcp_http_client_call_tool_finish(
        CLAWT_MCP_HTTP_CLIENT(source), result, &error);
    JsonObject *sc = structured(outcome);
    JsonObject *generation = sc != NULL && json_object_has_member(sc, "generation") &&
                             JSON_NODE_HOLDS_OBJECT(json_object_get_member(sc, "generation"))
        ? json_object_get_object_member(sc, "generation") : NULL;
    const gchar *status;
    AiMediaJob *job;

    if (outcome == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    if (generation == NULL) {
        g_autofree gchar *text = clawt_mcp_http_result_text(outcome);

        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "job_status said nothing about job %s: %s", poll->job_id, text);
        return;
    }
    status = json_object_get_string_member_with_default(generation, "status", NULL);
    job = ai_media_job_new(poll->job_id, state_for(status));
    ai_media_job_set_provider_status(job, status);
    if (ai_media_job_get_state(job) == AI_MEDIA_JOB_STATE_DONE) {
        JsonObject *results = json_object_has_member(generation, "results") &&
                              JSON_NODE_HOLDS_OBJECT(json_object_get_member(generation, "results"))
            ? json_object_get_object_member(generation, "results") : NULL;
        const gchar *raw = results != NULL
            ? json_object_get_string_member_with_default(results, "rawUrl", NULL) : NULL;

        if (raw != NULL)
            ai_media_job_add_result_url(job, raw);
    } else if (ai_media_job_get_state(job) == AI_MEDIA_JOB_STATE_FAILED) {
        ai_media_job_set_error_message(job, status);
    }
    g_task_return_pointer(task, job, g_object_unref);
}

static void
hm_poll_async(AiMediaGenerator    *generator,
              const gchar         *job_id,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
    ClawtHiggsfieldMcp *self = CLAWT_HIGGSFIELD_MCP(generator);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    Poll *poll = g_new0(Poll, 1);
    g_autoptr(JsonObject) arguments = json_object_new();

    poll->job_id = g_strdup(job_id);
    g_task_set_task_data(task, poll, (GDestroyNotify)poll_free);
    /* camel case: `job_id` is refused as a missing jobId */
    json_object_set_string_member(arguments, "jobId", job_id);
    /* the server waits up to ~25 s for a finished state, which saves polls */
    json_object_set_boolean_member(arguments, "sync", TRUE);
    clawt_mcp_http_client_call_tool_async(self->client, "job_status", arguments, cancellable,
                                          on_polled, task);
}

static AiMediaJob *
hm_poll_finish(AiMediaGenerator *generator, GAsyncResult *result, GError **error)
{
    (void)generator;
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Download ───────────────────────────────────────────────────── */

typedef struct {
    SoupMessage *message;
} Transfer;

static void
transfer_free(Transfer *t)
{
    g_clear_object(&t->message);
    g_free(t);
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    Transfer *t = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body = clawt_mcp_http_send_bounded_finish(result, &error);
    guint status;

    (void)source;
    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    status = soup_message_get_status(t->message);
    if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the result could not be fetched (HTTP %u)", status);
        return;
    }
    g_task_return_pointer(task, g_steal_pointer(&body), (GDestroyNotify)g_bytes_unref);
}

static void
hm_download_async(AiMediaGenerator    *generator,
                  const gchar         *url,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    ClawtHiggsfieldMcp *self = CLAWT_HIGGSFIELD_MCP(generator);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    Transfer *t = g_new0(Transfer, 1);

    g_task_set_task_data(task, t, (GDestroyNotify)transfer_free);

    /* a CDN link: https only, and it carries nothing of ours */
    if (url == NULL || !g_str_has_prefix(url, "https://")) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                                "the result is not an https URL");
        g_object_unref(task);
        return;
    }
    t->message = clawt_http_message_new("GET", url, CLAWT_HTTP_NO_CREDENTIALS);
    if (t->message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "the result's URL cannot be parsed");
        g_object_unref(task);
        return;
    }
    clawt_mcp_http_send_bounded_async(clawt_mcp_http_client_get_session(self->client),
                                      t->message, CLAWT_HIGGSFIELD_MCP_MAX_RESULT_BYTES,
                                      cancellable, on_downloaded, task);
}

static GBytes *
hm_download_finish(AiMediaGenerator *generator, GAsyncResult *result, GError **error)
{
    (void)generator;
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Cancel ─────────────────────────────────────────────────────── */

static void
hm_cancel_async(AiMediaGenerator    *generator,
                const gchar         *job_id,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    g_autoptr(GTask) task = g_task_new(generator, cancellable, callback, user_data);

    (void)job_id;
    /* the MCP server offers no cancel for an image job: say so, never pretend */
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Higgsfield's MCP tools cannot cancel an image job");
}

static gboolean
hm_cancel_finish(AiMediaGenerator *generator, GAsyncResult *result, GError **error)
{
    (void)generator;
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
clawt_higgsfield_mcp_generator_init(AiMediaGeneratorInterface *iface)
{
    iface->submit_async = hm_submit_async;
    iface->submit_finish = hm_submit_finish;
    iface->poll_async = hm_poll_async;
    iface->poll_finish = hm_poll_finish;
    iface->download_async = hm_download_async;
    iface->download_finish = hm_download_finish;
    iface->estimate_async = hm_estimate_async;
    iface->estimate_finish = hm_estimate_finish;
    iface->cancel_async = hm_cancel_async;
    iface->cancel_finish = hm_cancel_finish;
}

/* ── Upload ─────────────────────────────────────────────────────── */

typedef struct {
    GBytes      *data;
    gchar       *filename;
    gchar       *mime;
    gchar       *media_id;
    SoupMessage *message;
} Upload;

static void
upload_free(Upload *u)
{
    g_bytes_unref(u->data);
    g_free(u->filename);
    g_free(u->mime);
    g_free(u->media_id);
    g_clear_object(&u->message);
    g_free(u);
}

static void
on_confirmed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    Upload *u = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = clawt_mcp_http_client_call_tool_finish(
        CLAWT_MCP_HTTP_CLIENT(source), result, &error);

    if (outcome == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    g_task_return_pointer(task, g_strdup(u->media_id), g_free);
}

static void
on_put(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    ClawtHiggsfieldMcp *self = g_task_get_source_object(task);
    Upload *u = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body = clawt_mcp_http_send_bounded_finish(result, &error);
    g_autoptr(JsonObject) arguments = NULL;

    (void)source;
    if (body == NULL || !SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(u->message))) {
        if (error == NULL)
            g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "the upload was refused (HTTP %u)",
                        soup_message_get_status(u->message));
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }
    arguments = json_object_new();
    json_object_set_string_member(arguments, "type", "image");
    json_object_set_string_member(arguments, "media_id", u->media_id);
    clawt_mcp_http_client_call_tool_async(self->client, "media_confirm", arguments,
                                          g_task_get_cancellable(task), on_confirmed, task);
}

static void
on_upload_slot(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    ClawtHiggsfieldMcp *self = g_task_get_source_object(task);
    Upload *u = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = clawt_mcp_http_client_call_tool_finish(
        CLAWT_MCP_HTTP_CLIENT(source), result, &error);
    JsonObject *sc = structured(outcome);
    JsonArray *uploads = sc != NULL && json_object_has_member(sc, "uploads") &&
                         JSON_NODE_HOLDS_ARRAY(json_object_get_member(sc, "uploads"))
        ? json_object_get_array_member(sc, "uploads") : NULL;
    JsonObject *slot = uploads != NULL && json_array_get_length(uploads) > 0
        ? json_array_get_object_element(uploads, 0) : NULL;
    const gchar *url = slot != NULL
        ? json_object_get_string_member_with_default(slot, "upload_url", NULL) : NULL;
    const gchar *named = slot != NULL
        ? json_object_get_string_member_with_default(slot, "media_id",
              json_object_get_string_member_with_default(slot, "id", NULL)) : NULL;
    g_autofree gchar *from_url = NULL;
    const gchar *id = named;

    /*
     * The slot names its id, or the id is the object's file name in the
     * presigned URL (".../<user>/<media id>.png?X-Amz-...") -- which is
     * what the live server gave on 2026-11-22.
     */
    if (id == NULL && url != NULL) {
        g_autofree gchar *path = g_strndup(url, strcspn(url, "?"));
        g_autofree gchar *base = g_path_get_basename(path);
        gchar *dot = strrchr(base, '.');

        if (dot != NULL)
            *dot = '\0';
        if (g_uuid_string_is_valid(base))
            id = from_url = g_steal_pointer(&base);
    }

    if (outcome == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }
    if (url == NULL || id == NULL || !g_str_has_prefix(url, "https://")) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "media_upload gave no https upload URL and media id");
        g_object_unref(task);
        return;
    }
    u->media_id = g_strdup(id);

    /* a presigned URL is its own permission: it is sent no credential of ours */
    u->message = clawt_http_message_new("PUT", url, CLAWT_HTTP_NO_CREDENTIALS);
    if (u->message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "media_upload's URL cannot be parsed");
        g_object_unref(task);
        return;
    }
    soup_message_set_request_body_from_bytes(u->message, u->mime, u->data);
    clawt_mcp_http_send_bounded_async(clawt_mcp_http_client_get_session(self->client),
                                      u->message, 64 * 1024, g_task_get_cancellable(task),
                                      on_put, task);
}

void
clawt_higgsfield_mcp_upload_async(ClawtHiggsfieldMcp  *self,
                                  GBytes              *data,
                                  const gchar         *filename,
                                  const gchar         *mime,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    GTask *task;
    Upload *u;
    g_autoptr(JsonObject) arguments = json_object_new();

    g_return_if_fail(CLAWT_IS_HIGGSFIELD_MCP(self));
    g_return_if_fail(data != NULL && filename != NULL && mime != NULL);

    task = g_task_new(self, cancellable, callback, user_data);
    u = g_new0(Upload, 1);
    u->data = g_bytes_ref(data);
    u->filename = g_strdup(filename);
    u->mime = g_strdup(mime);
    g_task_set_task_data(task, u, (GDestroyNotify)upload_free);
    json_object_set_string_member(arguments, "filename", filename);
    json_object_set_string_member(arguments, "content_type", mime);
    clawt_mcp_http_client_call_tool_async(self->client, "media_upload", arguments, cancellable,
                                          on_upload_slot, task);
}

gchar *
clawt_higgsfield_mcp_upload_finish(ClawtHiggsfieldMcp  *self,
                                   GAsyncResult        *result,
                                   GError             **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
