/*
 * web-app.c - What every request handler shares
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One ClawtClient, one set of SSE subscribers, and the small readers that
 * turn a daemon reply into something a renderer can use without checking
 * for a missing member on every line.
 */

#include "clawt-web.h"

struct _ClawtWebApp {
    GObject      parent_instance;

    ClawtClient *client;
    GPtrArray   *streams;     /* HtmxSseConnection*, owned */
    gchar       *last_error;
    gchar       *last_refusal;
    gchar       *connection_name;
};

G_DEFINE_FINAL_TYPE(ClawtWebApp, clawt_web_app, G_TYPE_OBJECT)

/* ── Requests ────────────────────────────────────────────────────── */

JsonNode *
clawt_web_app_request(ClawtWebApp  *self,
                      const gchar  *kind,
                      JsonNode     *payload,
                      GError      **error)
{
    JsonNode *reply;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);
    g_return_val_if_fail(kind != NULL, NULL);

    reply = clawt_client_request(self->client, kind, payload, error);

    /*
     * Recorded on both request paths, not only clawt_web_app_call().
     * The agent editor saves through this one -- a key at a time -- and
     * it is the page where a refused render matters most: writing the
     * setting to clawtilla.yaml and not to the agent's own files is
     * exactly the state the call exists to leave behind.
     */
    g_clear_pointer(&self->last_refusal, g_free);

    if (reply != NULL)
        self->last_refusal = clawt_ipc_reply_refusal_text(reply, NULL);

    return reply;
}

JsonNode *
clawt_web_app_call(ClawtWebApp *self,
                   const gchar *kind,
                   JsonNode    *payload)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    reply = clawt_client_request(self->client, kind, payload, &error);

    g_clear_pointer(&self->last_error, g_free);
    g_clear_pointer(&self->last_refusal, g_free);

    if (reply == NULL) {
        self->last_error = g_strdup(error != NULL ? error->message
                                                  : "the daemon did not answer");
        g_debug("web: %s: %s", kind, self->last_error);
    } else {
        /*
         * A reply can succeed and still leave an agent behind: seven
         * daemon handlers re-render the fleet's files, and one agent's
         * `libreclaw:` block being refused stops that agent's files
         * being written while the rest of the call succeeds.  Recorded
         * beside the error rather than reported here, because this
         * function does not know what page is about to be drawn.
         */
        self->last_refusal = clawt_ipc_reply_refusal_text(reply, NULL);

        if (self->last_refusal != NULL)
            g_debug("web: %s: %s", kind, self->last_refusal);
    }

    return reply;
}

const gchar *
clawt_web_app_last_error(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->last_error;
}

const gchar *
clawt_web_app_last_refusal(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->last_refusal;
}

ClawtClient *
clawt_web_app_get_client(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->client;
}

static void on_daemon_event(ClawtClient *client, ClawtEvent *event,
                            gpointer user_data);

const gchar *
clawt_web_app_get_connection_name(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->connection_name;
}

void
clawt_web_app_set_connection_name(ClawtWebApp *self, const gchar *name)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    g_free(self->connection_name);
    self->connection_name = g_strdup(name);
}

gboolean
clawt_web_app_switch(ClawtWebApp *self, ClawtConnection *connection,
                     GError **error)
{
    g_autoptr(ClawtClient) fresh = NULL;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), FALSE);
    g_return_val_if_fail(connection != NULL, FALSE);

    fresh = clawt_connection_create_client(connection);
    clawt_client_set_auto_reconnect(fresh, TRUE);

    /*
     * Connected before the old one is let go. A daemon on another
     * machine that is not running is the ordinary case, and dropping a
     * working connection first would leave every open page talking to
     * nothing because of a typo in a port.
     */
    if (!clawt_client_connect(fresh, error))
        return FALSE;

    if (!clawt_client_subscribe(fresh, 0, NULL, error))
        return FALSE;

    if (self->client != NULL) {
        g_signal_handlers_disconnect_by_data(self->client, self);
        clawt_client_disconnect(self->client);
        g_object_unref(self->client);
    }

    self->client = g_steal_pointer(&fresh);
    g_signal_connect(self->client, "event", G_CALLBACK(on_daemon_event),
                     self);

    clawt_web_app_set_connection_name(self,
                                      clawt_connection_get_name(connection));

    /*
     * Every open page is told, so it re-fetches. Agent ids, room ids and
     * message ids are all per-daemon, so a page still showing the
     * previous fleet is showing another machine's.
     */
    {
        guint i;

        for (i = 0; i < self->streams->len; i++) {
            HtmxSseConnection *stream = g_ptr_array_index(self->streams, i);

            if (htmx_sse_connection_is_connected(stream))
                htmx_sse_connection_send_event(stream, "fleet",
                                               "connection.changed", NULL);
        }
    }

    return TRUE;
}

/* ── Event streams ───────────────────────────────────────────────── */

static void
on_stream_closed(HtmxSseConnection *connection, gpointer user_data)
{
    ClawtWebApp *self = user_data;

    g_ptr_array_remove_fast(self->streams, connection);
}

void
clawt_web_app_add_stream(ClawtWebApp *self, HtmxSseConnection *connection)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));
    g_return_if_fail(HTMX_IS_SSE_CONNECTION(connection));

    g_ptr_array_add(self->streams, g_object_ref(connection));

    /*
     * Removed on close rather than at the next failed write.  A browser
     * tab closes without saying so at the HTTP level, so the only signal
     * is the connection ending -- and an entry kept past that is a slot
     * we would go on formatting events into for ever.
     */
    g_signal_connect(connection, "closed",
                     G_CALLBACK(on_stream_closed), self);
}

guint
clawt_web_app_stream_count(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    return self->streams->len;
}

/*
 * Tell every listening browser that something moved.
 *
 * The event carries a name and nothing else.  A browser told "the fleet
 * changed" re-fetches the fragments it is showing, which is one round
 * trip against a daemon on the same machine -- while an event carrying
 * the change itself would mean this client rendering the same row two
 * ways, once from a reply and once from an event, and the two disagreeing
 * the first time either changed.
 */
static void
on_daemon_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ClawtWebApp *self = user_data;
    const gchar *kind = clawt_event_get_kind(event);
    const gchar *subject = clawt_event_get_subject(event);
    guint i;

    (void)client;

    for (i = 0; i < self->streams->len; i++) {
        HtmxSseConnection *connection = g_ptr_array_index(self->streams, i);

        if (!htmx_sse_connection_is_connected(connection))
            continue;

        htmx_sse_connection_send_event(connection, kind,
                                       subject != NULL ? subject : "", NULL);

        /*
         * A second event under one name, so a page can listen for
         * "anything at all" without naming every kind the daemon might
         * grow later.  A client that had to enumerate them would quietly
         * stop updating for whichever kind was added last.
         */
        htmx_sse_connection_send_event(connection, "fleet",
                                       kind != NULL ? kind : "", NULL);
    }
}

/* ── Construction ────────────────────────────────────────────────── */

ClawtWebApp *
clawt_web_app_new(ClawtClient *client)
{
    ClawtWebApp *self;

    g_return_val_if_fail(CLAWT_IS_CLIENT(client), NULL);

    self = g_object_new(CLAWT_TYPE_WEB_APP, NULL);
    self->client = g_object_ref(client);

    g_signal_connect(client, "event", G_CALLBACK(on_daemon_event), self);

    return self;
}

static void
clawt_web_app_finalize(GObject *object)
{
    ClawtWebApp *self = CLAWT_WEB_APP(object);

    if (self->client != NULL)
        g_signal_handlers_disconnect_by_data(self->client, self);

    g_clear_object(&self->client);
    g_clear_pointer(&self->streams, g_ptr_array_unref);
    g_clear_pointer(&self->last_error, g_free);
    g_clear_pointer(&self->last_refusal, g_free);
    g_clear_pointer(&self->connection_name, g_free);

    G_OBJECT_CLASS(clawt_web_app_parent_class)->finalize(object);
}

static void
clawt_web_app_class_init(ClawtWebAppClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_web_app_finalize;
}

static void
clawt_web_app_init(ClawtWebApp *self)
{
    self->streams = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ── Reading a reply ─────────────────────────────────────────────── */

JsonObject *
clawt_web_root(JsonNode *reply)
{
    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return NULL;

    return json_node_get_object(reply);
}

const gchar *
clawt_web_member(JsonObject *object, const gchar *key, const gchar *fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (JSON_NODE_HOLDS_NULL(node))
        return fallback;

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;

    return json_object_get_string_member(object, key);
}

gint64
clawt_web_member_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (!JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    /*
     * A count is sometimes rendered as a string -- the VM's cpus, memory
     * and disk all are, so that a client can put them straight into a
     * text field.  Reading only the integer form would report every one
     * of them as the fallback.
     */
    if (json_node_get_value_type(node) == G_TYPE_STRING) {
        const gchar *text = json_node_get_string(node);

        return (text != NULL) ? g_ascii_strtoll(text, NULL, 10) : fallback;
    }

    if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
        return (gint64)json_node_get_double(node);

    if (json_node_get_value_type(node) != G_TYPE_INT64)
        return fallback;

    return json_object_get_int_member(object, key);
}

gboolean
clawt_web_member_bool(JsonObject *object, const gchar *key, gboolean fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_object_get_boolean_member(object, key);
}

JsonArray *
clawt_web_member_array(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (!JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

JsonObject *
clawt_web_member_object(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (!JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

/* ── Building a payload ──────────────────────────────────────────── */

struct _ClawtWebPayload {
    JsonBuilder *builder;
};

ClawtWebPayload *
clawt_web_payload_new(void)
{
    ClawtWebPayload *self = g_new0(ClawtWebPayload, 1);

    self->builder = json_builder_new();
    json_builder_begin_object(self->builder);

    return self;
}

void
clawt_web_payload_set(ClawtWebPayload *self, const gchar *key,
                      const gchar *value)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    /*
     * A member absent and a member set to null are different to the
     * daemon: absent means "do not change this", null would be a value.
     * A form that did not include a field must therefore add nothing.
     */
    if (value == NULL)
        return;

    json_builder_set_member_name(self->builder, key);
    json_builder_add_string_value(self->builder, value);
}

void
clawt_web_payload_set_int(ClawtWebPayload *self, const gchar *key,
                          gint64 value)
{
    g_return_if_fail(self != NULL);

    json_builder_set_member_name(self->builder, key);
    json_builder_add_int_value(self->builder, value);
}

void
clawt_web_payload_set_bool(ClawtWebPayload *self, const gchar *key,
                           gboolean value)
{
    g_return_if_fail(self != NULL);

    json_builder_set_member_name(self->builder, key);
    json_builder_add_boolean_value(self->builder, value);
}

void
clawt_web_payload_set_list(ClawtWebPayload *self, const gchar *key,
                           const gchar *const *values)
{
    guint i;

    g_return_if_fail(self != NULL);
    g_return_if_fail(values != NULL);

    json_builder_set_member_name(self->builder, key);
    json_builder_begin_array(self->builder);

    for (i = 0; values[i] != NULL; i++)
        json_builder_add_string_value(self->builder, values[i]);

    json_builder_end_array(self->builder);
}

JsonNode *
clawt_web_payload_take(ClawtWebPayload *self)
{
    JsonNode *node;

    g_return_val_if_fail(self != NULL, NULL);

    json_builder_end_object(self->builder);
    node = json_builder_get_root(self->builder);

    g_object_unref(self->builder);
    g_free(self);

    return node;
}

void
clawt_web_payload_free(ClawtWebPayload *self)
{
    if (self == NULL)
        return;

    g_object_unref(self->builder);
    g_free(self);
}
