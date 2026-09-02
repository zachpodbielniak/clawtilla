/*
 * clawt-oauth.c - Obtaining a credential without the agent ever holding it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-oauth.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING

/* A device flow that nobody completes should not poll for ever. */
#define DEFAULT_DEVICE_INTERVAL   (5)
#define DEFAULT_DEVICE_LIFETIME   (900)
#define SLOW_DOWN_INCREMENT       (5)

/* ── The session ─────────────────────────────────────────────────── */

/*
 * Named, like every other outbound session here.  libsoup sends no
 * User-Agent by default and a fair number of deployments answer that
 * with a 403 whose body parses into nothing that names the cause.
 */
static SoupSession *
oauth_session(void)
{
    static SoupSession *session = NULL;

    if (session == NULL)
        session = soup_session_new_with_options("user-agent", USER_AGENT,
                                                "timeout", 30, NULL);

    return session;
}

/*
 * A form body from alternating key/value pairs, skipping any pair whose
 * value is NULL.
 *
 * Written out rather than taken from libsoup because most of these
 * fields are optional -- a provider that wants no client secret must
 * receive no `client_secret=` at all, and an empty one is not the same
 * thing: some providers authenticate the empty string and fail.
 */
static gchar *
form_encode(const gchar *first_key, ...)
{
    GString *out = g_string_new(NULL);
    va_list args;
    const gchar *key;

    va_start(args, first_key);

    for (key = first_key; key != NULL; key = va_arg(args, const gchar *)) {
        const gchar *value = va_arg(args, const gchar *);
        g_autofree gchar *escaped = NULL;

        if (value == NULL)
            continue;

        escaped = g_uri_escape_string(value, NULL, FALSE);

        if (out->len > 0)
            g_string_append_c(out, '&');

        g_string_append(out, key);
        g_string_append_c(out, '=');
        g_string_append(out, escaped);
    }

    va_end(args);

    return g_string_free(out, FALSE);
}

static SoupMessage *
form_post(const gchar *url, gchar *body)
{
    SoupMessage *message = soup_message_new("POST", url);
    g_autoptr(GBytes) bytes = NULL;

    if (message == NULL) {
        g_free(body);
        return NULL;
    }

    bytes = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(
        message, "application/x-www-form-urlencoded", bytes);

    /*
     * Without this GitHub answers its token endpoint in
     * application/x-www-form-urlencoded, which parses as JSON into
     * nothing and surfaces as "no access_token in the response" -- a
     * message that points at the provider rather than at the header we
     * failed to send.
     */
    soup_message_headers_replace(soup_message_get_request_headers(message),
                                 "Accept", "application/json");

    return message;
}

/* ── Tokens ──────────────────────────────────────────────────────── */

static void
wipe(gchar *secret)
{
    if (secret == NULL)
        return;

    /*
     * memset through a volatile pointer: a plain memset on memory that
     * is about to be freed is exactly the dead store a compiler is
     * entitled to remove, and at -O2 it does.
     */
    memset((void *volatile)secret, 0, strlen(secret));
}

void
clawt_oauth_token_free(ClawtOauthToken *self)
{
    if (self == NULL)
        return;

    wipe(self->access_token);
    wipe(self->refresh_token);

    g_free(self->access_token);
    g_free(self->refresh_token);
    g_free(self->token_type);
    g_free(self->scopes);

    g_free(self);
}

ClawtOauthToken *
clawt_oauth_token_copy(ClawtOauthToken *self)
{
    ClawtOauthToken *out;

    if (self == NULL)
        return NULL;

    out = g_new0(ClawtOauthToken, 1);
    out->access_token = g_strdup(self->access_token);
    out->refresh_token = g_strdup(self->refresh_token);
    out->token_type = g_strdup(self->token_type);
    out->scopes = g_strdup(self->scopes);
    out->expires_at = self->expires_at;

    return out;
}

G_DEFINE_BOXED_TYPE(ClawtOauthToken, clawt_oauth_token,
                    clawt_oauth_token_copy, clawt_oauth_token_free)

static JsonObject *
parse_object(JsonParser *parser, const gchar *json, gssize length,
             GError **error)
{
    JsonNode *root;

    if (!json_parser_load_from_data(parser, json, length, error))
        return NULL;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the response was not a JSON object");
        return NULL;
    }

    return json_node_get_object(root);
}

static const gchar *
object_string(JsonObject *object, const gchar *key)
{
    JsonNode *node;
    const gchar *value;

    if (!json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    value = json_node_get_string(node);

    /*
     * An empty member is an absent one.  A provider that answers a
     * renewal with `"refresh_token": ""` produced a non-NULL empty
     * string, which every `!= NULL` test downstream read as a value --
     * so the blank was stored over a working refresh token and the
     * guard written to prevent exactly that could not see it.  The
     * access token was the only field that checked, and it checked
     * here rather than everywhere it is read; so does this.
     */
    return (value != NULL && *value != '\0') ? value : NULL;
}

static gint64
object_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node;

    if (!json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    /*
     * Providers disagree about whether these are numbers or strings --
     * "expires_in": 3600 and "expires_in": "3600" are both out there --
     * and a client that reads only one silently treats the other as
     * absent, producing a token that never expires or one that expires
     * immediately.
     */
    if (json_node_get_value_type(node) == G_TYPE_STRING) {
        const gchar *text = json_node_get_string(node);

        return (text != NULL) ? g_ascii_strtoll(text, NULL, 10) : fallback;
    }

    return json_node_get_int(node);
}

static ClawtOauthToken *
token_from_object(JsonObject *object, gint64 now, GError **error)
{
    ClawtOauthToken *out;
    const gchar *access = object_string(object, "access_token");
    gint64 expires_in;

    if (access == NULL || *access == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the response carried no access token");
        return NULL;
    }

    out = g_new0(ClawtOauthToken, 1);
    out->access_token = g_strdup(access);
    out->refresh_token = g_strdup(object_string(object, "refresh_token"));
    out->token_type = g_strdup(object_string(object, "token_type"));
    out->scopes = g_strdup(object_string(object, "scope"));

    expires_in = object_int(object, "expires_in", 0);

    /*
     * Absolute, computed here.  Storing the duration as it arrived makes
     * an hour-long token look an hour fresh on every daemon restart, so
     * one that expired overnight reads as valid each morning and every
     * call made with it fails somewhere else entirely.
     */
    out->expires_at = (expires_in > 0) ? now + expires_in : 0;

    return out;
}

ClawtOauthToken *
clawt_oauth_token_parse(const gchar *json, gssize length, gint64 now,
                        GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object;

    g_return_val_if_fail(json != NULL, NULL);

    object = parse_object(parser, json, length, error);

    if (object == NULL)
        return NULL;

    return token_from_object(object, now, error);
}

gboolean
clawt_oauth_token_is_expired(ClawtOauthToken *self, gint64 now, gint64 skew)
{
    g_return_val_if_fail(self != NULL, TRUE);

    if (self->expires_at == 0)
        return FALSE;

    return now + skew >= self->expires_at;
}

gboolean
clawt_oauth_token_save(ClawtOauthToken *self, const gchar *path,
                       GError **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *text = NULL;
    gsize length = 0;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "access_token");
    json_builder_add_string_value(builder, self->access_token);

    if (self->refresh_token != NULL) {
        json_builder_set_member_name(builder, "refresh_token");
        json_builder_add_string_value(builder, self->refresh_token);
    }

    if (self->token_type != NULL) {
        json_builder_set_member_name(builder, "token_type");
        json_builder_add_string_value(builder, self->token_type);
    }

    if (self->scopes != NULL) {
        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(builder, self->scopes);
    }

    /*
     * Written as an absolute time under its own name.  Calling it
     * `expires_in` here would invite the next reader to add it to the
     * time they loaded the file, which is the bug this field exists to
     * avoid.
     */
    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(builder, self->expires_at);

    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, &length);

    if (!clawt_write_file_atomic(path, text, (gssize)length, 0600, FALSE,
                                 error)) {
        wipe(text);
        return FALSE;
    }

    wipe(text);

    return TRUE;
}

ClawtOauthToken *
clawt_oauth_token_load(const gchar *path, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    JsonObject *object;
    ClawtOauthToken *out;
    const gchar *access;

    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_get_contents(path, &text, NULL, error))
        return NULL;

    object = parse_object(parser, text, -1, error);

    if (object == NULL) {
        wipe(text);
        return NULL;
    }

    access = object_string(object, "access_token");

    if (access == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the stored credential has no access token");
        wipe(text);
        return NULL;
    }

    out = g_new0(ClawtOauthToken, 1);
    out->access_token = g_strdup(access);
    out->refresh_token = g_strdup(object_string(object, "refresh_token"));
    out->token_type = g_strdup(object_string(object, "token_type"));
    out->scopes = g_strdup(object_string(object, "scope"));
    out->expires_at = object_int(object, "expires_at", 0);

    wipe(text);

    return out;
}

/* ── Device codes ────────────────────────────────────────────────── */

void
clawt_device_code_free(ClawtDeviceCode *self)
{
    if (self == NULL)
        return;

    wipe(self->device_code);

    g_free(self->device_code);
    g_free(self->user_code);
    g_free(self->verification_uri);
    g_free(self->verification_uri_complete);

    g_free(self);
}

ClawtDeviceCode *
clawt_device_code_copy(ClawtDeviceCode *self)
{
    ClawtDeviceCode *out;

    if (self == NULL)
        return NULL;

    out = g_new0(ClawtDeviceCode, 1);
    out->device_code = g_strdup(self->device_code);
    out->user_code = g_strdup(self->user_code);
    out->verification_uri = g_strdup(self->verification_uri);
    out->verification_uri_complete =
        g_strdup(self->verification_uri_complete);
    out->interval = self->interval;
    out->expires_at = self->expires_at;

    return out;
}

G_DEFINE_BOXED_TYPE(ClawtDeviceCode, clawt_device_code,
                    clawt_device_code_copy, clawt_device_code_free)

ClawtDeviceCode *
clawt_oauth_parse_device_code(const gchar *json, gssize length, gint64 now,
                              GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object;
    ClawtDeviceCode *out;
    const gchar *device;
    const gchar *user;
    const gchar *uri;
    gint64 lifetime;

    g_return_val_if_fail(json != NULL, NULL);

    object = parse_object(parser, json, length, error);

    if (object == NULL)
        return NULL;

    device = object_string(object, "device_code");
    user = object_string(object, "user_code");
    uri = object_string(object, "verification_uri");

    /*
     * Microsoft spells it verification_uri; some providers still send
     * the draft's verification_url.  Accepting both costs one line and
     * saves a flow that otherwise fails with a code and nowhere to
     * enter it.
     */
    if (uri == NULL)
        uri = object_string(object, "verification_url");

    if (device == NULL || user == NULL || uri == NULL) {
        const gchar *message = object_string(object, "error_description");

        if (message == NULL)
            message = object_string(object, "error");

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the provider did not start a device flow: %s",
                    message != NULL ? message : "no device code in the reply");
        return NULL;
    }

    out = g_new0(ClawtDeviceCode, 1);
    out->device_code = g_strdup(device);
    out->user_code = g_strdup(user);
    out->verification_uri = g_strdup(uri);
    out->verification_uri_complete =
        g_strdup(object_string(object, "verification_uri_complete"));

    out->interval = (gint)object_int(object, "interval",
                                     DEFAULT_DEVICE_INTERVAL);

    if (out->interval < 1)
        out->interval = DEFAULT_DEVICE_INTERVAL;

    lifetime = object_int(object, "expires_in", DEFAULT_DEVICE_LIFETIME);
    out->expires_at = now + (lifetime > 0 ? lifetime
                                          : DEFAULT_DEVICE_LIFETIME);

    return out;
}

ClawtOauthPollResult
clawt_oauth_read_poll(const gchar *json, gssize length, gint64 now,
                      ClawtOauthToken **out_token, gchar **out_message)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    JsonObject *object;
    const gchar *code;
    const gchar *description;

    if (out_token != NULL)
        *out_token = NULL;

    if (out_message != NULL)
        *out_message = NULL;

    if (json == NULL) {
        if (out_message != NULL)
            *out_message = g_strdup("the provider sent nothing");
        return CLAWT_OAUTH_POLL_FAILED;
    }

    object = parse_object(parser, json, length, &error);

    if (object == NULL) {
        if (out_message != NULL)
            *out_message = g_strdup(error->message);
        return CLAWT_OAUTH_POLL_FAILED;
    }

    code = object_string(object, "error");

    /*
     * No `error` member means this is the grant.  Checked before the
     * error branches rather than after, because a provider that returns
     * both -- and some return an empty `error` alongside a real token --
     * has granted it.
     */
    if (code == NULL || *code == '\0') {
        g_autoptr(GError) token_error = NULL;
        ClawtOauthToken *token = token_from_object(object, now, &token_error);

        if (token == NULL) {
            if (out_message != NULL)
                *out_message = g_strdup(token_error->message);
            return CLAWT_OAUTH_POLL_FAILED;
        }

        if (out_token != NULL)
            *out_token = token;
        else
            clawt_oauth_token_free(token);

        return CLAWT_OAUTH_POLL_GRANTED;
    }

    if (g_strcmp0(code, "authorization_pending") == 0)
        return CLAWT_OAUTH_POLL_PENDING;

    if (g_strcmp0(code, "slow_down") == 0)
        return CLAWT_OAUTH_POLL_SLOW_DOWN;

    description = object_string(object, "error_description");

    if (out_message != NULL)
        *out_message = g_strdup(description != NULL ? description : code);

    if (g_strcmp0(code, "access_denied") == 0)
        return CLAWT_OAUTH_POLL_DENIED;

    if (g_strcmp0(code, "expired_token") == 0)
        return CLAWT_OAUTH_POLL_EXPIRED;

    return CLAWT_OAUTH_POLL_FAILED;
}

/* ── Starting a device flow ──────────────────────────────────────── */

static void
on_device_begin(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    ClawtDeviceCode *code;
    const gchar *text;
    gsize length = 0;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    text = g_bytes_get_data(body, &length);
    code = clawt_oauth_parse_device_code(text, (gssize)length,
                                         g_get_real_time() / G_USEC_PER_SEC,
                                         &error);

    if (code == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_pointer(task, code, (GDestroyNotify)clawt_device_code_free);
}

void
clawt_oauth_device_begin_async(const gchar         *auth_url,
                               const gchar         *client_id,
                               const gchar         *scopes,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_autoptr(SoupMessage) message = NULL;

    g_return_if_fail(auth_url != NULL);
    g_return_if_fail(client_id != NULL);

    message = form_post(auth_url,
                        form_encode("client_id", client_id,
                                    "scope", scopes,
                                    NULL));

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a URL that can be dialled",
                                auth_url);
        g_object_unref(task);
        return;
    }

    soup_session_send_and_read_async(oauth_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_device_begin, task);
}

ClawtDeviceCode *
clawt_oauth_device_begin_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Polling it out ──────────────────────────────────────────────── */

typedef struct {
    gchar        *token_url;
    gchar        *client_id;
    gchar        *client_secret;
    gchar        *device_code;
    gint          interval;
    gint64        expires_at;
    GCancellable *cancellable;
    GSource      *timer;
    GTask        *task;
} DevicePoll;

static void
device_poll_free(DevicePoll *self)
{
    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_source_unref(self->timer);
    }

    wipe(self->device_code);

    g_free(self->token_url);
    g_free(self->client_id);
    g_free(self->client_secret);
    g_free(self->device_code);
    g_clear_object(&self->cancellable);
    g_clear_object(&self->task);

    g_free(self);
}

static void device_poll_once(DevicePoll *poll);

static gboolean
on_poll_timer(gpointer user_data)
{
    DevicePoll *poll = user_data;

    g_clear_pointer(&poll->timer, g_source_unref);

    device_poll_once(poll);

    return G_SOURCE_REMOVE;
}

/*
 * Re-arms rather than running on a repeating source, because the
 * interval changes: a `slow_down` lengthens it for the rest of the flow.
 */
static void
device_poll_later(DevicePoll *poll)
{
    if (poll->timer != NULL) {
        g_source_destroy(poll->timer);
        g_source_unref(poll->timer);
    }

    poll->timer = clawt_timeout_add_seconds((guint)poll->interval,
                                            on_poll_timer, poll);
}

static void
on_poll_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DevicePoll *poll = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *message = NULL;
    ClawtOauthToken *token = NULL;
    const gchar *text;
    gsize length = 0;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(poll->task, g_steal_pointer(&error));
        device_poll_free(poll);
        return;
    }

    text = g_bytes_get_data(body, &length);

    switch (clawt_oauth_read_poll(text, (gssize)length,
                                  g_get_real_time() / G_USEC_PER_SEC,
                                  &token, &message)) {
    case CLAWT_OAUTH_POLL_GRANTED:
        g_task_return_pointer(poll->task, token,
                              (GDestroyNotify)clawt_oauth_token_free);
        device_poll_free(poll);
        return;

    case CLAWT_OAUTH_POLL_SLOW_DOWN:
        /*
         * Permanently.  A provider that has asked once to be polled
         * less often will ask again, and reverting to the old interval
         * after a single slower poll spends the rest of the flow being
         * rate limited -- while the person is standing at the consent
         * screen wondering why nothing happened.
         */
        poll->interval += SLOW_DOWN_INCREMENT;
        /* fall through */

    case CLAWT_OAUTH_POLL_PENDING:
        if (g_get_real_time() / G_USEC_PER_SEC >= poll->expires_at) {
            g_task_return_new_error(poll->task, CLAWT_ERROR,
                                    CLAWT_ERROR_TIMEOUT,
                                    "the code expired before it was entered");
            device_poll_free(poll);
            return;
        }

        device_poll_later(poll);
        return;

    case CLAWT_OAUTH_POLL_DENIED:
        g_task_return_new_error(poll->task, CLAWT_ERROR,
                                CLAWT_ERROR_PERMISSION_DENIED,
                                "the request was refused: %s",
                                message != NULL ? message : "access denied");
        device_poll_free(poll);
        return;

    case CLAWT_OAUTH_POLL_EXPIRED:
        g_task_return_new_error(poll->task, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                                "the code expired before it was entered");
        device_poll_free(poll);
        return;

    case CLAWT_OAUTH_POLL_FAILED:
    default:
        g_task_return_new_error(poll->task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "%s", message != NULL ? message
                                                      : "the provider refused");
        device_poll_free(poll);
        return;
    }
}

static void
device_poll_once(DevicePoll *poll)
{
    g_autoptr(SoupMessage) message = NULL;

    if (g_cancellable_is_cancelled(poll->cancellable)) {
        g_task_return_new_error(poll->task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the connection attempt was cancelled");
        device_poll_free(poll);
        return;
    }

    message = form_post(poll->token_url,
                        form_encode("client_id", poll->client_id,
                                    "client_secret", poll->client_secret,
                                    "device_code", poll->device_code,
                                    "grant_type",
                                    "urn:ietf:params:oauth:grant-type:"
                                    "device_code",
                                    NULL));

    if (message == NULL) {
        g_task_return_new_error(poll->task, CLAWT_ERROR,
                                CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a URL that can be dialled",
                                poll->token_url);
        device_poll_free(poll);
        return;
    }

    soup_session_send_and_read_async(oauth_session(), message,
                                     G_PRIORITY_DEFAULT, poll->cancellable,
                                     on_poll_response, poll);
}

void
clawt_oauth_device_poll_async(const gchar         *token_url,
                              const gchar         *client_id,
                              const gchar         *client_secret,
                              ClawtDeviceCode     *code,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    DevicePoll *poll;

    g_return_if_fail(token_url != NULL);
    g_return_if_fail(client_id != NULL);
    g_return_if_fail(code != NULL);

    poll = g_new0(DevicePoll, 1);
    poll->token_url = g_strdup(token_url);
    poll->client_id = g_strdup(client_id);
    poll->client_secret = g_strdup(client_secret);
    poll->device_code = g_strdup(code->device_code);
    poll->interval = code->interval;
    poll->expires_at = code->expires_at;
    poll->cancellable = (cancellable != NULL) ? g_object_ref(cancellable)
                                              : NULL;
    poll->task = g_task_new(NULL, cancellable, callback, user_data);

    /*
     * The first poll waits out one interval rather than firing at once.
     * The person has not had time to read the code, let alone type it,
     * so an immediate poll is guaranteed to come back pending and
     * spends one of the attempts a rate limiter is counting.
     */
    device_poll_later(poll);
}

ClawtOauthToken *
clawt_oauth_device_poll_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── PKCE ────────────────────────────────────────────────────────── */

/*
 * base64url without padding, which is what RFC 7636 asks for and what
 * every provider validates against.
 */
static gchar *
base64url(const guchar *data, gsize length)
{
    gchar *encoded = g_base64_encode(data, length);
    gchar *p;

    for (p = encoded; *p != '\0'; p++) {
        if (*p == '+')
            *p = '-';
        else if (*p == '/')
            *p = '_';
    }

    p = strchr(encoded, '=');

    if (p != NULL)
        *p = '\0';

    return encoded;
}

/*
 * From the kernel, or not at all.
 *
 * GLib's g_random_* is a Mersenne Twister: given a little of its output
 * the rest is computable, and a computable verifier is not weaker PKCE
 * but no PKCE, since the entire mechanism is that only the client which
 * began the flow can complete it.  So a machine with no usable
 * randomness gets a refusal rather than a downgrade -- the same rule
 * this codebase applies to a missing bwrap.
 */
static gboolean
random_bytes(guchar *out, gsize length)
{
    gsize got = 0;
    FILE *source = fopen("/dev/urandom", "rb");

    if (source == NULL)
        return FALSE;

    while (got < length) {
        gsize n = fread(out + got, 1, length - got, source);

        if (n == 0)
            break;

        got += n;
    }

    fclose(source);

    return got == length;
}

gchar *
clawt_oauth_pkce_verifier(void)
{
    guchar raw[32];

    if (!random_bytes(raw, sizeof(raw))) {
        g_warning("cannot read /dev/urandom; refusing to build a PKCE "
                  "verifier from a predictable source");
        return NULL;
    }

    return base64url(raw, sizeof(raw));
}

gchar *
clawt_oauth_pkce_challenge(const gchar *verifier)
{
    guchar digest[32];
    gsize length = sizeof(digest);
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);

    g_return_val_if_fail(verifier != NULL, NULL);

    g_checksum_update(checksum, (const guchar *)verifier, (gssize)strlen(verifier));
    g_checksum_get_digest(checksum, digest, &length);

    return base64url(digest, length);
}

gchar *
clawt_oauth_authorize_url(const gchar *auth_url,
                          const gchar *client_id,
                          const gchar *redirect_uri,
                          const gchar *scopes,
                          const gchar *state,
                          const gchar *challenge)
{
    g_autofree gchar *query = NULL;

    g_return_val_if_fail(auth_url != NULL, NULL);
    g_return_val_if_fail(client_id != NULL, NULL);
    g_return_val_if_fail(redirect_uri != NULL, NULL);

    query = form_encode("response_type", "code",
                        "client_id", client_id,
                        "redirect_uri", redirect_uri,
                        "scope", scopes,
                        "state", state,
                        "code_challenge", challenge,
                        "code_challenge_method",
                        challenge != NULL ? "S256" : NULL,
                        NULL);

    /*
     * An authorization endpoint may already carry a query of its own --
     * a tenant id, an audience -- so this appends rather than assuming
     * it can start one.
     */
    return g_strconcat(auth_url, strchr(auth_url, '?') != NULL ? "&" : "?",
                       query, NULL);
}

/* ── The redirect ────────────────────────────────────────────────── */

gboolean
clawt_oauth_parse_redirect(const gchar *target, gchar **out_code,
                           gchar **out_state, gchar **out_error)
{
    const gchar *question;
    g_auto(GStrv) pairs = NULL;
    guint i;
    gboolean found = FALSE;

    if (out_code != NULL)
        *out_code = NULL;

    if (out_state != NULL)
        *out_state = NULL;

    if (out_error != NULL)
        *out_error = NULL;

    if (target == NULL)
        return FALSE;

    question = strchr(target, '?');

    if (question == NULL)
        return FALSE;

    pairs = g_strsplit(question + 1, "&", -1);

    for (i = 0; pairs[i] != NULL; i++) {
        gchar *equals = strchr(pairs[i], '=');
        g_autofree gchar *value = NULL;

        if (equals == NULL)
            continue;

        *equals = '\0';

        /*
         * A `+` in a query means a space, which g_uri_unescape_string()
         * does not know: it only undoes percent escapes.  A state or a
         * code containing one would otherwise fail to match by exactly
         * that character.
         */
        value = g_strdup(equals + 1);
        g_strdelimit(value, "+", ' ');

        if (g_strcmp0(pairs[i], "code") == 0 && out_code != NULL) {
            *out_code = g_uri_unescape_string(value, NULL);
            found = TRUE;
        } else if (g_strcmp0(pairs[i], "state") == 0 && out_state != NULL) {
            *out_state = g_uri_unescape_string(value, NULL);
        } else if (g_strcmp0(pairs[i], "error") == 0) {
            if (out_error != NULL)
                *out_error = g_uri_unescape_string(value, NULL);

            found = TRUE;
        }
    }

    return found;
}

/*
 * Reference counted, and it has to be.
 *
 * Two things can end this wait -- a redirect arriving, and the deadline
 * passing -- and a read of a request line may be in flight when the
 * deadline does.  Freeing on the timeout alone leaves that read's
 * callback holding a pointer to released memory, which is the exact
 * shape of bug the link server and the IPC server each had once: an
 * async callback must own a reference to whatever it will touch.
 *
 * The listener holds one reference; every outstanding read holds
 * another.
 */
typedef struct {
    GSocketService *service;
    gchar          *expected_state;
    GSource        *timer;
    GTask          *task;
    gboolean        settled;
    gint            refs;
} RedirectWait;

static RedirectWait *
redirect_wait_ref(RedirectWait *self)
{
    g_atomic_int_inc(&self->refs);

    return self;
}

static void
redirect_wait_unref(RedirectWait *self)
{
    if (!g_atomic_int_dec_and_test(&self->refs))
        return;

    g_free(self->expected_state);
    g_clear_object(&self->task);

    g_free(self);
}

/*
 * Stops listening and drops the listener's reference.  Outstanding reads
 * keep theirs until their callbacks have run, so this is safe to call
 * from either of the two paths that can finish the wait.
 */
static void
redirect_wait_settle(RedirectWait *self)
{
    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_clear_pointer(&self->timer, g_source_unref);
    }

    if (self->service != NULL) {
        g_socket_service_stop(self->service);
        g_socket_listener_close(G_SOCKET_LISTENER(self->service));
        g_clear_object(&self->service);
    }

    redirect_wait_unref(self);
}

static const gchar *const PAGE_OK =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html><meta charset=utf-8><title>Connected</title>"
    "<body style=\"font-family:system-ui;margin:4rem;text-align:center\">"
    "<h1>Connected</h1><p>clawtilla has the credential. "
    "You can close this tab.</p>";

static const gchar *const PAGE_NOT_FOUND =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "not the redirect\n";

static void
respond(GOutputStream *out, const gchar *page)
{
    g_output_stream_write_all(out, page, strlen(page), NULL, NULL, NULL);
    g_output_stream_close(out, NULL, NULL);
}

static void
on_request_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RedirectWait *wait = user_data;
    GSocketConnection *connection;
    g_autofree gchar *line = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *state = NULL;
    g_autofree gchar *failure = NULL;
    g_auto(GStrv) parts = NULL;
    GOutputStream *out;

    /* Borrowed: the reader holds it for as long as this callback runs. */
    connection = g_object_get_data(G_OBJECT(source), "clawt-connection");

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, NULL, NULL);

    if (connection == NULL || line == NULL || wait->settled) {
        redirect_wait_unref(wait);
        return;
    }

    out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    parts = g_strsplit(line, " ", 3);

    if (parts[0] == NULL || parts[1] == NULL ||
        !clawt_oauth_parse_redirect(parts[1], &code, &state, &failure)) {
        /*
         * A browser asks for /favicon.ico on its own, and answering
         * that as though it were the redirect would finish the flow
         * with no code at all.  Anything that is not the redirect gets
         * a 404 and the listener stays up.
         */
        respond(out, PAGE_NOT_FOUND);
        redirect_wait_unref(wait);
        return;
    }

    wait->settled = TRUE;
    respond(out, failure != NULL ? PAGE_NOT_FOUND : PAGE_OK);

    if (failure != NULL) {
        g_task_return_new_error(wait->task, CLAWT_ERROR,
                                CLAWT_ERROR_PERMISSION_DENIED,
                                "the request was refused: %s", failure);
        redirect_wait_settle(wait);
        redirect_wait_unref(wait);
        return;
    }

    /*
     * The state check is the whole reason state exists: without it any
     * page the browser visits can hand this listener a code from
     * somebody else's authorization and have clawtilla store it as the
     * person's own account.
     */
    if (g_strcmp0(state, wait->expected_state) != 0) {
        g_task_return_new_error(wait->task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "the reply did not belong to this request");
        redirect_wait_settle(wait);
        redirect_wait_unref(wait);
        return;
    }

    g_task_return_pointer(wait->task, g_steal_pointer(&code), g_free);
    redirect_wait_settle(wait);
    redirect_wait_unref(wait);
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection,
            GObject *source_object, gpointer user_data)
{
    RedirectWait *wait = user_data;
    GDataInputStream *reader;

    if (wait->settled)
        return TRUE;

    reader = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));

    g_object_set_data_full(G_OBJECT(reader), "clawt-connection",
                           g_object_ref(connection), g_object_unref);

    g_data_input_stream_read_line_async(reader, G_PRIORITY_DEFAULT, NULL,
                                        on_request_line,
                                        redirect_wait_ref(wait));

    g_object_unref(reader);

    return TRUE;
}

static gboolean
on_redirect_timeout(gpointer user_data)
{
    RedirectWait *wait = user_data;

    if (wait->settled)
        return G_SOURCE_REMOVE;

    wait->settled = TRUE;

    g_task_return_new_error(wait->task, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                            "nobody completed the authorization in time");
    redirect_wait_settle(wait);

    return G_SOURCE_REMOVE;
}

void
clawt_oauth_await_redirect_async(guint                port,
                                 const gchar         *expected_state,
                                 guint                timeout_seconds,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    RedirectWait *wait;
    g_autoptr(GInetAddress) address = NULL;
    g_autoptr(GSocketAddress) socket_address = NULL;
    g_autoptr(GError) error = NULL;

    wait = g_new0(RedirectWait, 1);
    wait->refs = 1;
    wait->expected_state = g_strdup(expected_state);
    wait->task = g_task_new(NULL, cancellable, callback, user_data);
    wait->service = g_socket_service_new();

    /*
     * Loopback only.  A redirect carries an authorization code in a URL,
     * and a listener bound to every interface offers that code to
     * whoever on the network reaches the port first -- on a laptop in a
     * cafe, that is a real thing rather than a theoretical one.
     */
    address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    socket_address = g_inet_socket_address_new(address, (guint16)port);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(wait->service),
                                       socket_address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_TCP, NULL, NULL,
                                       &error)) {
        g_task_return_new_error(wait->task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "cannot listen on 127.0.0.1:%u for the "
                                "redirect: %s", port, error->message);
        wait->settled = TRUE;
        redirect_wait_settle(wait);
        return;
    }

    g_signal_connect(wait->service, "incoming", G_CALLBACK(on_incoming), wait);
    g_socket_service_start(wait->service);

    wait->timer = clawt_timeout_add_seconds(timeout_seconds,
                                            on_redirect_timeout, wait);
}

gchar *
clawt_oauth_await_redirect_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Code exchange and renewal ───────────────────────────────────── */

static void
on_token_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *message = NULL;
    ClawtOauthToken *token = NULL;
    const gchar *text;
    gsize length = 0;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    text = g_bytes_get_data(body, &length);

    if (clawt_oauth_read_poll(text, (gssize)length,
                              g_get_real_time() / G_USEC_PER_SEC,
                              &token, &message) != CLAWT_OAUTH_POLL_GRANTED) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s",
                                message != NULL ? message
                                                : "the provider refused");
        return;
    }

    g_task_return_pointer(task, token,
                          (GDestroyNotify)clawt_oauth_token_free);
}

void
clawt_oauth_exchange_async(const gchar         *token_url,
                           const gchar         *client_id,
                           const gchar         *client_secret,
                           const gchar         *code,
                           const gchar         *redirect_uri,
                           const gchar         *verifier,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_autoptr(SoupMessage) message = NULL;

    g_return_if_fail(token_url != NULL);
    g_return_if_fail(code != NULL);

    message = form_post(token_url,
                        form_encode("grant_type", "authorization_code",
                                    "client_id", client_id,
                                    "client_secret", client_secret,
                                    "code", code,
                                    "redirect_uri", redirect_uri,
                                    "code_verifier", verifier,
                                    NULL));

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a URL that can be dialled",
                                token_url);
        g_object_unref(task);
        return;
    }

    soup_session_send_and_read_async(oauth_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_token_response, task);
}

ClawtOauthToken *
clawt_oauth_exchange_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

void
clawt_oauth_refresh_async(const gchar         *token_url,
                          const gchar         *client_id,
                          const gchar         *client_secret,
                          const gchar         *refresh_token,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_autoptr(SoupMessage) message = NULL;

    g_return_if_fail(token_url != NULL);
    g_return_if_fail(refresh_token != NULL);

    message = form_post(token_url,
                        form_encode("grant_type", "refresh_token",
                                    "client_id", client_id,
                                    "client_secret", client_secret,
                                    "refresh_token", refresh_token,
                                    NULL));

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a URL that can be dialled",
                                token_url);
        g_object_unref(task);
        return;
    }

    soup_session_send_and_read_async(oauth_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_token_response, task);
}

ClawtOauthToken *
clawt_oauth_refresh_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Revocation ──────────────────────────────────────────────────── */

static void
on_revoke_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    SoupMessage *message = g_task_get_task_data(task);
    guint status;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    status = soup_message_get_status(message);

    /*
     * Judged on the status rather than the body, which is the one place
     * in this file where that is right: RFC 7009 says a successful
     * revocation returns 200 with no content, and providers differ on
     * what they put there when they bother at all.
     *
     * A token that was already invalid is also a success -- the caller
     * asked for it to stop working, and it has.
     */
    if (status == SOUP_STATUS_OK || status == SOUP_STATUS_NO_CONTENT ||
        status == SOUP_STATUS_BAD_REQUEST) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the provider answered %u", status);
}

void
clawt_oauth_revoke_async(const gchar         *revoke_url,
                         const gchar         *client_id,
                         const gchar         *client_secret,
                         const gchar         *token,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    SoupMessage *message;

    g_return_if_fail(revoke_url != NULL);
    g_return_if_fail(token != NULL);

    message = form_post(revoke_url,
                        form_encode("token", token,
                                    "client_id", client_id,
                                    "client_secret", client_secret,
                                    NULL));

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "'%s' is not a URL that can be dialled",
                                revoke_url);
        g_object_unref(task);
        return;
    }

    /* Kept alive for the callback, which reads its status. */
    g_task_set_task_data(task, message, g_object_unref);

    soup_session_send_and_read_async(oauth_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_revoke_response, task);
}

gboolean
clawt_oauth_revoke_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}
