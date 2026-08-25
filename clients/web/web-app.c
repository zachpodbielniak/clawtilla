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

    /*
     * What arrived while the reader was somewhere else.
     *
     * Keyed by agent id; `dm_rooms` maps the room an event names to the
     * agent whose conversation it is, from the `dm_room` the daemon
     * reports beside every agent -- so nothing here takes "dm:a:b"
     * apart, which is the daemon's business and has changed before.
     *
     * `viewing` is the agent whose chat was last rendered, and is how
     * this applies the same rule the GTK client does: a conversation on
     * screen never accrues, a conversation elsewhere does.  It is one
     * reader's state on a server that could in principle be answering
     * two browsers, and that is the honest limit of it -- clawtilla-web
     * binds the loopback and the tailnet for one operator, and a second
     * reader would share the counts rather than get their own.
     */
    GHashTable  *unread;
    GHashTable  *dm_rooms;
    gchar       *viewing;

    /*
     * When this app connected, in microseconds.
     *
     * A client subscribes from cursor 0 and the daemon replays its
     * recent events, so the first thing received is everything that has
     * just happened.  Counting those would give a freshly started web
     * client a number for a conversation nobody has touched.  Replayed
     * events keep their original timestamps, so this is the whole test.
     */
    gint64       connected_at;

    /*
     * What has happened, for the alerts page.
     *
     * Same split as the GTK client's panel: two of the daemon's events
     * are notifications -- a download that failed, a message the loop
     * guard refused -- and everything else is the routine stream, kept
     * quietly so the page can also answer "what is the fleet doing".
     * Session-scoped and capped; the daemon's event log is the durable
     * copy and `event.list` reads it for anything older.
     */
    GPtrArray   *alerts;
    guint        next_alert_id;
    gchar       *connection_name;
};

G_DEFINE_FINAL_TYPE(ClawtWebApp, clawt_web_app, G_TYPE_OBJECT)

/* ── Alerts ──────────────────────────────────────────────────────── */

/*
 * The most recent 200, matching the limit `agent.logs` already defaults
 * to so the two surfaces agree on what "recent" means.
 */
#define ALERTS_KEPT 200

static void
alert_free(gpointer data)
{
    ClawtWebAlert *alert = data;

    g_free(alert->text);
    g_free(alert->source);
    g_free(alert->agent);
    g_free(alert);
}

static void
note_alert(ClawtWebApp *self, ClawtWebAlertTier tier, const gchar *source,
           const gchar *agent, const gchar *text)
{
    ClawtWebAlert *alert = g_new0(ClawtWebAlert, 1);

    alert->id = ++self->next_alert_id;
    alert->tier = tier;
    alert->text = g_strdup(text);
    alert->source = g_strdup(source != NULL ? source : "");
    alert->agent = g_strdup(agent != NULL ? agent : "");
    alert->ts = g_get_real_time();
    alert->read = FALSE;

    /* Newest first: this list is read from the top. */
    g_ptr_array_insert(self->alerts, 0, alert);

    while (self->alerts->len > ALERTS_KEPT)
        g_ptr_array_remove_index(self->alerts, self->alerts->len - 1);
}

GPtrArray *
clawt_web_app_alerts(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->alerts;
}

guint
clawt_web_app_alert_count(ClawtWebApp *self)
{
    guint count = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    for (i = 0; i < self->alerts->len; i++) {
        ClawtWebAlert *alert = g_ptr_array_index(self->alerts, i);

        /*
         * Never the routine stream.  A badge that counted it would be
         * permanently non-zero and would stop being read, and widening
         * the filter is a thing the reader chooses.
         */
        if (!alert->read && alert->tier != CLAWT_WEB_ALERT_ROUTINE)
            count++;
    }

    return count;
}

void
clawt_web_app_alerts_mark_read(ClawtWebApp *self)
{
    guint i;

    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    for (i = 0; i < self->alerts->len; i++)
        ((ClawtWebAlert *)g_ptr_array_index(self->alerts, i))->read = TRUE;
}

void
clawt_web_app_alert_dismiss(ClawtWebApp *self, guint id)
{
    guint i;

    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    for (i = 0; i < self->alerts->len; i++) {
        ClawtWebAlert *alert = g_ptr_array_index(self->alerts, i);

        if (alert->id == id) {
            g_ptr_array_remove_index(self->alerts, i);
            return;
        }
    }
}

void
clawt_web_app_alerts_clear(ClawtWebApp *self)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    g_ptr_array_set_size(self->alerts, 0);
}

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

guint
clawt_web_app_unread(ClawtWebApp *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    if (agent_id == NULL)
        return 0;

    return GPOINTER_TO_UINT(g_hash_table_lookup(self->unread, agent_id));
}

guint
clawt_web_app_unread_total(ClawtWebApp *self)
{
    GHashTableIter iter;
    gpointer value;
    guint total = 0;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    g_hash_table_iter_init(&iter, self->unread);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        total += GPOINTER_TO_UINT(value);

    return total;
}

void
clawt_web_app_note_fleet(ClawtWebApp *self, JsonArray *agents)
{
    g_autoptr(GHashTable) live = NULL;
    GHashTableIter iter;
    gpointer key;
    guint i;

    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    if (agents == NULL)
        return;

    live = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_remove_all(self->dm_rooms);

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        JsonNode *room;
        const gchar *id;

        if (agent == NULL || !json_object_has_member(agent, "id"))
            continue;

        id = json_object_get_string_member(agent, "id");
        g_hash_table_add(live, (gpointer)id);

        if (!json_object_has_member(agent, "dm_room"))
            continue;

        room = json_object_get_member(agent, "dm_room");

        if (json_node_get_value_type(room) != G_TYPE_STRING)
            continue;

        g_hash_table_insert(self->dm_rooms,
                            g_strdup(json_node_get_string(room)),
                            g_strdup(id));
    }

    /*
     * And forget the count for an agent that is no longer in the fleet.
     * Without this a removed agent's number stays in the total on the
     * Chat tab for ever, pointing at a row nobody can open to clear it.
     */
    g_hash_table_iter_init(&iter, self->unread);

    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(live, key))
            g_hash_table_iter_remove(&iter);
    }
}

void
clawt_web_app_set_viewing(ClawtWebApp *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    g_free(self->viewing);
    self->viewing = g_strdup(agent_id);

    /*
     * Opening a conversation is the only thing that clears its count --
     * the same single rule the GTK client follows.  A counter that
     * decays on its own is a counter you stop trusting.
     */
    if (agent_id != NULL)
        g_hash_table_remove(self->unread, agent_id);
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

    /*
     * A message in somebody's conversation with an agent, in a
     * conversation that is not the one on screen.
     *
     * Nothing here asks the daemon anything: a request from an event
     * handler would run while a page render is blocked inside its own
     * request on the same context, which is the re-entrancy this client
     * has already been bitten by.  The room map is filled in from the
     * fleet listing the sidebar fetches anyway.
     */
    if (g_strcmp0(kind, "message") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *agent_id = (subject != NULL)
            ? g_hash_table_lookup(self->dm_rooms, subject) : NULL;

        if (agent_id != NULL && g_strcmp0(from, "user") != 0 &&
            g_strcmp0(agent_id, self->viewing) != 0 &&
            clawt_event_get_timestamp(event) >= self->connected_at)
            g_hash_table_insert(
                self->unread, g_strdup(agent_id),
                GUINT_TO_POINTER(clawt_web_app_unread(self, agent_id) + 1));
    }

    /*
     * Two of the daemon's kinds are notifications; the rest is the
     * routine stream, kept quietly.  `image.progress` is excluded -- a
     * download emits one per percent and would fill the list with one
     * file -- and so is `agent.typing`, which is a spinner rather than
     * an event.
     */
    if (g_strcmp0(kind, "message.refused") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *to = clawt_event_get_detail(event, "to");
        const gchar *reason = clawt_event_get_detail(event, "reason");
        g_autofree gchar *line = g_strdup_printf(
            "%s to %s was stopped: %s", from != NULL ? from : "?",
            to != NULL ? to : "?", reason != NULL ? reason : "a limit");

        note_alert(self, CLAWT_WEB_ALERT_ERROR, kind, from, line);
    } else if (g_strcmp0(kind, "image.finished") == 0 &&
               clawt_event_get_detail(event, "error") != NULL) {
        g_autofree gchar *line = g_strdup_printf(
            "%s could not be downloaded: %s",
            subject != NULL ? subject : "an image",
            clawt_event_get_detail(event, "error"));

        note_alert(self, CLAWT_WEB_ALERT_ERROR, kind, subject, line);
    } else if (g_strcmp0(kind, "image.progress") != 0 &&
               g_strcmp0(kind, "agent.typing") != 0) {
        ClawtWebAlertTier tier = CLAWT_WEB_ALERT_ROUTINE;
        g_autofree gchar *line = NULL;

        if (g_strcmp0(kind, "agent.state") == 0) {
            const gchar *state = clawt_event_get_detail(event, "state");

            line = g_strdup_printf("%s is %s", subject != NULL ? subject : "?",
                                   state != NULL ? state : "?");

            if (g_strcmp0(state, "error") == 0 ||
                g_strcmp0(state, "degraded") == 0)
                tier = CLAWT_WEB_ALERT_NOTICE;
        } else {
            line = g_strdup_printf("%s · %s", kind != NULL ? kind : "?",
                                   subject != NULL ? subject : "the fleet");
        }

        note_alert(self, tier, kind, subject, line);
    }

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
    self->connected_at = g_get_real_time();

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
    g_clear_pointer(&self->unread, g_hash_table_unref);
    g_clear_pointer(&self->dm_rooms, g_hash_table_unref);
    g_clear_pointer(&self->viewing, g_free);
    g_clear_pointer(&self->alerts, g_ptr_array_unref);
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
    self->unread = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    self->dm_rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_free);
    self->alerts = g_ptr_array_new_with_free_func(alert_free);
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
