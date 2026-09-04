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
     * What the daemon said it was, asked once at connect.
     *
     * `control.status` has reported the version since the frame existed
     * and had one caller in the tree -- the CLI.  Neither graphical
     * client sent it, so a client talking to an older or newer daemon
     * found out by a frame kind being refused, in whichever feature the
     * person happened to open.
     */
    gchar       *daemon_version;
    gchar       *daemon_update;   /* a newer version, or NULL */

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

    /*
     * What the last probe found for each saved connection, by name.
     *
     * Not filled on a page render: probing every saved connection while
     * a page is being built would multiply that page's latency by the
     * number of daemons somebody has saved, and stall on each one that
     * is asleep for as long as its route takes to give up. It is filled
     * only when a reader asks, by the Check button on the connections
     * page, and an entry nobody has asked about is simply absent.
     */
    GHashTable  *connection_status;
    gchar       *viewing;
    gchar       *viewing_room;

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

    /*
     * How many decisions are open, and whether that number is still
     * true.
     *
     * Not maintained by counting `decision.asked` against
     * `decision.settled`: a replay that fell off the ring would leave
     * the tally permanently wrong with nothing to say so, and a count
     * that is merely too high reads as an inbox nobody has got to.
     * Instead any `decision.` event marks it unknown, and the next
     * render asks -- so the daemon is the only thing that ever decides
     * the number, and it is asked once per change rather than once per
     * page.
     *
     * Asked from the getter, during a render, and never from the event
     * handler: a request issued from there would run while a page
     * render is blocked inside its own request on the same context.
     */
    guint        open_decisions;
    gboolean     decisions_known;
    guint        next_alert_id;

    /*
     * Which daemon this process serves.
     *
     * The whole connection rather than its name, because the notice in
     * the banner has to say different things for a local daemon and one
     * on another machine -- and a name alone cannot tell them apart.
     */
    ClawtConnection *connection;

    /*
     * Whether a daemon has ever been on the other end.  Always true
     * today, since this binary refuses to start without one; kept
     * because clawt_daemon_link_state() needs it and a client that
     * learns to start disconnected would otherwise word it wrongly with
     * nothing to notice.
     */
    gboolean     reached_once;
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
note_alert(ClawtWebApp *self, ClawtAlertTier tier, const gchar *source,
           const gchar *agent, const gchar *text)
{
    ClawtWebAlert *alert = g_new0(ClawtWebAlert, 1);

    alert->id = ++self->next_alert_id;
    alert->tier = tier;
    alert->text = g_strdup(text);
    alert->source = g_strdup(source != NULL ? source : "");
    alert->agent = g_strdup(agent != NULL ? agent : "");
    alert->ts = g_get_real_time();

    /*
     * The same rule the GTK client applies, from the same function.
     *
     * This client has no panel that stays open, so the case it catches
     * here is the routine tier: those are never counted, and recording
     * one unread leaves a flag no badge reads and that a later widening
     * of the filter would start believing.
     */
    alert->read = clawt_alert_arrives_read(FALSE, alert->tier);

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
        if (!alert->read && alert->tier != CLAWT_ALERT_ROUTINE)
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
clawt_web_app_unread(ClawtWebApp *self, const gchar *room_id)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    if (room_id == NULL)
        return 0;

    return GPOINTER_TO_UINT(g_hash_table_lookup(self->unread, room_id));
}

guint
clawt_web_app_open_decisions(ClawtWebApp *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *items;

    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), 0);

    if (self->decisions_known)
        return self->open_decisions;

    {
        ClawtWebPayload *query = clawt_web_payload_new();

        clawt_web_payload_set_bool(query, "open", TRUE);
        reply = clawt_web_app_call(self, "decision.list",
                                   clawt_web_payload_take(query));
    }

    /*
     * A daemon that did not answer leaves the count unknown rather than
     * zero, so the next render asks again.  Storing zero would draw a
     * clear inbox for as long as the connection is down, which is the
     * one direction this number must never be wrong in.
     */
    if (reply == NULL)
        return 0;

    items = clawt_web_member_array(clawt_web_root(reply), "decisions");

    self->open_decisions = (items != NULL) ? json_array_get_length(items) : 0;
    self->decisions_known = TRUE;

    return self->open_decisions;
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

        if (!json_object_has_member(agent, "dm_room"))
            continue;

        room = json_object_get_member(agent, "dm_room");

        if (json_node_get_value_type(room) != G_TYPE_STRING)
            continue;

        g_hash_table_add(live, (gpointer)json_node_get_string(room));

        g_hash_table_insert(self->dm_rooms,
                            g_strdup(json_node_get_string(room)),
                            g_strdup(id));
    }

    /*
     * And forget the count for a conversation that is no longer there.
     * Without this a removed agent's number stays in the total on the
     * Chat tab for ever, pointing at a row nobody can open to clear it.
     *
     * `live` holds rooms rather than agent ids, because that is what the
     * counts are keyed on: a group has no agent, and every row has a
     * room.
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

    /* And no longer a room; the two are exclusive. */
    if (agent_id != NULL)
        g_clear_pointer(&self->viewing_room, g_free);

    /*
     * Opening a conversation is the only thing that clears its count --
     * the same single rule the GTK client follows.  A counter that
     * decays on its own is a counter you stop trusting.
     */
    if (agent_id != NULL) {
        /*
         * By the room, because that is what the count is against -- a
         * group has no agent to key on.
         */
        g_autofree gchar *room = clawt_room_manager_direct_id("user",
                                                              agent_id);

        g_hash_table_remove(self->unread, room);
    }
}

void
clawt_web_app_set_viewing_room(ClawtWebApp *self, const gchar *room_id)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    g_free(self->viewing_room);
    self->viewing_room = g_strdup(room_id);

    /*
     * The two selections are exclusive: a room is an entry in its own
     * right rather than one of a selected agent's conversations, and a
     * stale agent would keep the sidebar highlighting a row nobody is
     * looking at.
     */
    if (room_id != NULL) {
        g_clear_pointer(&self->viewing, g_free);
        g_hash_table_remove(self->unread, room_id);
    }
}

const gchar *
clawt_web_app_get_viewing_room(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    return self->viewing_room;
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

    /*
     * Derived rather than stored beside the connection.  Two answers to
     * "which daemon is this" is how a settings page came to tick a row
     * that was not the one being served.
     */
    return (self->connection != NULL)
           ? clawt_connection_get_name(self->connection) : NULL;
}

void
clawt_web_app_set_connection(ClawtWebApp *self, ClawtConnection *connection)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));

    g_clear_pointer(&self->connection, clawt_connection_free);
    self->connection = (connection != NULL)
                       ? clawt_connection_copy(connection) : NULL;

    /* Asked, not assumed: connect happens before this app exists. */
    self->reached_once = clawt_client_is_connected(self->client);
}

/*
 * Asks the daemon what it is, once.
 *
 * Cheap enough to do inline at connect: the daemon answers `control.status`
 * from what it already holds and never leaves the machine to do it.
 */
static void
note_daemon_version(ClawtWebApp *self)
{
    g_autoptr(JsonNode) reply = NULL;

    g_clear_pointer(&self->daemon_version, g_free);
    g_clear_pointer(&self->daemon_update, g_free);

    if (self->client == NULL)
        return;

    reply = clawt_web_app_call(self, "control.status", NULL);

    if (reply != NULL) {
        JsonObject *root = clawt_web_root(reply);
        JsonObject *update = clawt_web_member_object(root, "update");

        self->daemon_version =
            g_strdup(clawt_web_member(root, "version", NULL));

        /*
         * Only when the daemon says one is available.  Reading `latest`
         * on its own would draw a banner for the version we are already
         * on -- the comparison belongs to the daemon, which is the only
         * place it is written once.
         */
        if (update != NULL &&
            clawt_web_member_bool(update, "available", FALSE))
            self->daemon_update =
                g_strdup(clawt_web_member(update, "latest", NULL));
    }
}

gchar *
clawt_web_app_connection_notice(ClawtWebApp *self)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    /*
     * The sentence and its precedence are in libclawt, so this client
     * and the GTK one cannot come to disagree about whether a connection
     * was lost or never made.  This used to be a second copy of the
     * wording, and it had only two of the three states.
     */
    return clawt_connection_notice_text(
        clawt_daemon_link_state(self->client, self->reached_once),
        self->connection, self->daemon_version, self->daemon_update);
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

    clawt_web_app_set_connection(self, connection);

    /*
     * The decision inbox belongs to the daemon, so the count from the
     * previous one says nothing about this one.  Marked unknown rather
     * than zeroed: zero is a claim, and the first render will ask.
     */
    self->decisions_known = FALSE;

    /*
     * The version belongs to the daemon, not to this process, so it is
     * asked again.  A banner kept from the machine somebody just
     * switched away from is worse than no banner.
     */
    note_daemon_version(self);

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
        g_autofree gchar *viewing_room = NULL;

        /*
         * The rule is clawt_unread_should_count(), in libclawt, because
         * the GTK client applies the same one.  It compares rooms, and
         * this client tracks the agent whose chat was last rendered --
         * so the room being read is that agent's, which is the reverse
         * of the same lookup.
         */
        if (self->viewing != NULL)
            viewing_room = clawt_room_manager_direct_id("user",
                                                        self->viewing);

        /*
         * Counted against the room rather than the agent whose room it
         * is.  It used to give up when the room resolved to no agent --
         * which is every group room, so a group could never light a
         * badge, and a chat you have to remember to open is a chat you
         * forget.
         */
        if (clawt_unread_should_count(subject, viewing_room, from,
                                      clawt_event_get_timestamp(event),
                                      self->connected_at))
            g_hash_table_insert(
                self->unread, g_strdup(subject),
                GUINT_TO_POINTER(clawt_web_app_unread(self, subject) + 1));
    }

    /*
     * One was filed, answered or dismissed -- by this browser, another
     * one, the CLI, or the venture bridge answering on the operator's
     * behalf.  Whichever it was, the count on the Work tab is no longer
     * known to be right.
     *
     * Marked rather than adjusted, and nothing is asked from in here:
     * see the field's own note.
     */
    if (kind != NULL && g_str_has_prefix(kind, "decision."))
        self->decisions_known = FALSE;

    /*
     * Two of the daemon's kinds are notifications; the rest is the
     * routine stream, kept quietly.  `image.progress` is excluded -- a
     * download emits one per percent and would fill the list with one
     * file -- and so is `agent.typing`, which is a spinner rather than
     * an event.
     */
    {
        /*
         * Which tier an event belongs in is clawt_alert_tier_for_event(),
         * in libclawt, because the GTK panel classifies the same stream
         * -- and two implementations of that rule would differ exactly
         * once, on the kind somebody adds next.  Only the wording is
         * decided here.
         */
        ClawtAlertTier tier = clawt_alert_tier_for_event(event);
        g_autofree gchar *line = NULL;

        if (tier == CLAWT_ALERT_ERROR &&
            g_strcmp0(kind, "message.refused") == 0) {
            const gchar *from = clawt_event_get_detail(event, "from");
            const gchar *to = clawt_event_get_detail(event, "to");
            const gchar *reason = clawt_event_get_detail(event, "reason");

            line = g_strdup_printf("%s to %s was stopped: %s",
                                   from != NULL ? from : "?",
                                   to != NULL ? to : "?",
                                   reason != NULL ? reason : "a limit");
            note_alert(self, tier, kind, from, line);
        } else if (tier == CLAWT_ALERT_ERROR) {
            line = g_strdup_printf("%s could not be downloaded: %s",
                                   subject != NULL ? subject : "an image",
                                   clawt_event_get_detail(event, "error"));
            note_alert(self, tier, kind, subject, line);
        } else if (tier == CLAWT_ALERT_NOTICE ||
                   tier == CLAWT_ALERT_ROUTINE) {
            if (g_strcmp0(kind, "agent.state") == 0) {
                const gchar *state = clawt_event_get_detail(event, "state");

                line = g_strdup_printf("%s is %s",
                                       subject != NULL ? subject : "?",
                                       state != NULL ? state : "?");
            } else {
                line = g_strdup_printf("%s · %s", kind != NULL ? kind : "?",
                                       subject != NULL ? subject
                                                       : "the fleet");
            }

            note_alert(self, tier, kind, subject, line);
        }
    }

    for (i = 0; i < self->streams->len; i++) {
        HtmxSseConnection *connection = g_ptr_array_index(self->streams, i);

        if (!htmx_sse_connection_is_connected(connection))
            continue;

        /*
         * A running turn's steps go out under their own undotted name
         * and stay off the umbrella, for two separate reasons.
         *
         * Undotted because a dot in an hx-trigger is a class selector,
         * so `sse:turn.step` would listen for nothing -- the same trap
         * that made the umbrella necessary in the first place.
         *
         * Off the umbrella because `sse:fleet` is what the whole
         * transcript re-fetches on, and a turn produces tens of steps:
         * every open browser would re-fetch an entire conversation per
         * tool call, for the whole of every turn.  The small region
         * that draws the steps listens for `step` instead, so the
         * expensive one is left alone.
         */
        if (clawt_event_is_ephemeral(event)) {
            htmx_sse_connection_send_event(connection, "step",
                                           subject != NULL ? subject : "",
                                           NULL);
            continue;
        }

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

/*
 * Nudges every open page to re-render.
 *
 * The banner is drawn from the app's state on each render, so making it
 * appear and disappear is the same one round trip a fleet event already
 * costs -- no second rendering of the same fact from two places.
 */
static void
tell_every_page(ClawtWebApp *self, const gchar *why)
{
    guint i;

    for (i = 0; i < self->streams->len; i++) {
        HtmxSseConnection *stream = g_ptr_array_index(self->streams, i);

        if (htmx_sse_connection_is_connected(stream))
            htmx_sse_connection_send_event(stream, "fleet", why, NULL);
    }
}

static void
on_connection_changed(ClawtClient *client, gpointer user_data)
{
    ClawtWebApp *self = user_data;

    (void)client;

    tell_every_page(self, "connection.changed");
}

/*
 * The daemon could not replay from where this client had got to, so
 * anything a browser is showing may be stale in ways no event will
 * correct.  Every page re-fetches; the unread counts go, because they
 * were counted from a stream with a hole in it and a count nobody can
 * trust is worse than none.
 */
static void
on_resync(ClawtClient *client, gpointer user_data)
{
    ClawtWebApp *self = user_data;

    (void)client;

    g_hash_table_remove_all(self->unread);

    /*
     * And the decision count is asked again rather than kept.
     *
     * A resync means the replay had a hole, so a `decision.settled`
     * this client needed may be in it -- and a cached count that missed
     * one is wrong in the direction that reads as work waiting.
     */
    self->decisions_known = FALSE;

    tell_every_page(self, "resync");
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

    /*
     * A daemon that goes away mid-session is otherwise indistinguishable
     * from a fleet that has gone quiet -- and here the reader is a
     * browser, so there is not even a process exiting to notice.  Both
     * signals simply push every open page to re-render, which is how the
     * banner gets drawn and undrawn.
     */
    g_signal_connect(client, "disconnected",
                     G_CALLBACK(on_connection_changed), self);
    g_signal_connect(client, "connected",
                     G_CALLBACK(on_connection_changed), self);
    g_signal_connect(client, "resync", G_CALLBACK(on_resync), self);

    note_daemon_version(self);

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
    g_clear_pointer(&self->viewing_room, g_free);
    g_clear_pointer(&self->alerts, g_ptr_array_unref);
    g_clear_pointer(&self->connection, clawt_connection_free);
    g_clear_pointer(&self->daemon_version, g_free);
    g_clear_pointer(&self->daemon_update, g_free);

    g_clear_pointer(&self->connection_status, g_hash_table_unref);

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
    self->connection_status = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)clawt_connection_status_free);

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

void
clawt_web_app_note_connection_status(ClawtWebApp           *self,
                                     const gchar           *name,
                                     ClawtConnectionStatus *status)
{
    g_return_if_fail(CLAWT_IS_WEB_APP(self));
    g_return_if_fail(name != NULL);

    if (status == NULL)
        g_hash_table_remove(self->connection_status, name);
    else
        g_hash_table_replace(self->connection_status, g_strdup(name),
                             status);
}

ClawtConnectionStatus *
clawt_web_app_connection_status(ClawtWebApp *self, const gchar *name)
{
    g_return_val_if_fail(CLAWT_IS_WEB_APP(self), NULL);

    if (name == NULL)
        return NULL;

    return g_hash_table_lookup(self->connection_status, name);
}
