/*
 * clawt-web.h - Shared declarations for clawtilla-web
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla-web is a thin client like every other one: a ClawtClient over
 * the daemon's socket, and a fold over one event stream.  What is
 * different is that the fold happens on the server and the result is
 * HTML, so the thing at the far end is a browser rather than a widget
 * tree.
 *
 * The rule this file exists to keep is parity.  Every feature the GTK
 * client has must exist here, and every feature added here must exist
 * there -- checked by tools/clawt-client-parity.sh rather than
 * remembered, because two clients drifting apart is invisible until
 * somebody reaches for the half that was not built.
 */

#pragma once

#include <clawtilla.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_WEB_WATCHER_NAME:
 *
 * What this client calls itself when it subscribes to a screen.
 *
 * One name for the whole process rather than one per browser, and
 * deliberately: two browsers pointed at the same clawtilla-web share a
 * watch, which is what they would get anyway -- the daemon grabs once
 * per interval whoever is looking. Per-browser names would mean two
 * leases to expire and nothing gained.
 */
#define CLAWT_WEB_WATCHER_NAME "clawtilla-web"

/* ── The application ─────────────────────────────────────────────── */

#define CLAWT_TYPE_WEB_APP (clawt_web_app_get_type())

G_DECLARE_FINAL_TYPE(ClawtWebApp, clawt_web_app, CLAWT, WEB_APP, GObject)

/**
 * clawt_web_app_new:
 * @client: (transfer none): a connected #ClawtClient
 *
 * Returns: (transfer full): the application state every handler shares
 */
ClawtWebApp *clawt_web_app_new(ClawtClient *client);

/**
 * clawt_web_app_get_client:
 * @self: a #ClawtWebApp
 *
 * Returns: (transfer none): the daemon connection
 */
ClawtClient *clawt_web_app_get_client(ClawtWebApp *self);

/**
 * clawt_web_app_request:
 * @self: a #ClawtWebApp
 * @kind: an IPC frame kind
 * @payload: (nullable) (transfer full): the request payload
 * @error: (out) (optional): return location for a #GError
 *
 * One request to the daemon.
 *
 * Returns: (transfer full) (nullable): the reply
 */
JsonNode *clawt_web_app_request(ClawtWebApp  *self,
                                const gchar  *kind,
                                JsonNode     *payload,
                                GError      **error);

/**
 * clawt_web_app_call:
 * @self: a #ClawtWebApp
 * @kind: an IPC frame kind
 * @payload: (nullable) (transfer full): the request payload
 *
 * A request whose failure is reported rather than handled, for the many
 * read paths where an empty section is the right thing to draw.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL
 */
JsonNode *clawt_web_app_call(ClawtWebApp *self,
                             const gchar *kind,
                             JsonNode    *payload);

/**
 * ClawtWebAlert:
 * @id: identifies the row for dismissal
 * @tier: how loudly to draw it, from clawt_alert_tier_for_event()
 * @text: what happened
 * @source: the event kind, so a row says where to look
 * @agent: the event's subject
 * @ts: when, in microseconds
 * @read: whether the page has been opened since it arrived
 *
 * One thing that happened.
 */
typedef struct {
    guint              id;
    ClawtAlertTier     tier;
    gchar             *text;
    gchar             *source;
    gchar             *agent;
    gint64             ts;
    gboolean           read;
} ClawtWebAlert;

/**
 * clawt_web_app_alerts:
 * @self: a #ClawtWebApp
 *
 * Everything that has happened since this client started, newest first.
 *
 * Returns: (transfer none) (element-type ClawtWebAlert): the entries
 */
/**
 * clawt_web_app_connection_notice:
 * @self: a #ClawtWebApp
 *
 * What the page's banner should say about the daemon, or %NULL.
 *
 * Two persistent conditions and nothing else: this process has lost the
 * daemon and is trying to get it back, or the daemon is a different
 * version from this client.  Both were invisible -- auto-reconnect is
 * right, and silence while it happens is indistinguishable from a fleet
 * that has gone quiet; and `control.status` reported the version from
 * the day the frame existed with no graphical client ever sending it, so
 * a mismatch surfaced as an unrelated feature refusing to work.
 *
 * Reconnecting wins, because while the connection is down the version is
 * whatever it was before it went and telling somebody to update a daemon
 * they cannot reach is advice about the wrong problem.  The GTK banner
 * applies the same precedence.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL
 */
gchar *clawt_web_app_connection_notice(ClawtWebApp *self);

GPtrArray *clawt_web_app_alerts(ClawtWebApp *self);

/**
 * clawt_web_app_alert_count:
 * @self: a #ClawtWebApp
 *
 * How many unread entries are loud enough to be worth a number.
 *
 * Returns: the count, never including routine entries
 */
guint clawt_web_app_alert_count(ClawtWebApp *self);

/**
 * clawt_web_app_alerts_mark_read:
 * @self: a #ClawtWebApp
 *
 * Opening the page marks everything read.  Rows stay until dismissed, so
 * nothing is lost by the count going to zero.
 */
void clawt_web_app_alerts_mark_read(ClawtWebApp *self);

/**
 * clawt_web_app_alert_dismiss:
 * @self: a #ClawtWebApp
 * @id: which entry
 *
 * Removes one entry.
 */
void clawt_web_app_alert_dismiss(ClawtWebApp *self, guint id);

/**
 * clawt_web_app_alerts_clear:
 * @self: a #ClawtWebApp
 *
 * Removes them all.
 */
void clawt_web_app_alerts_clear(ClawtWebApp *self);

/**
 * clawt_web_app_last_error:
 * @self: a #ClawtWebApp
 *
 * Why the most recent clawt_web_app_call() returned %NULL.
 *
 * Valid only until the next call. Every clawt_web_app_call() frees this
 * and writes a new one, so anything that renders a page -- which makes
 * half a dozen calls of its own -- must copy the message *before* it
 * starts rather than pass this pointer along. Getting that wrong does
 * not read as a bug: the banner shows whatever string now lives at that
 * address, which the first time was a static one from an unrelated
 * table.
 *
 * Returns: (nullable): the message, owned by @self and freed by the next
 *   clawt_web_app_call()
 */
const gchar *clawt_web_app_last_error(ClawtWebApp *self);

/**
 * clawt_web_app_last_refusal:
 * @self: a #ClawtWebApp
 *
 * What the last call left unwritten, if anything.
 *
 * Seven daemon handlers re-render the fleet's agent files, and any of
 * them can be refused for one agent while succeeding for the rest -- an
 * operator-typed `libreclaw:` block that redeclares a section clawtilla
 * renders itself is the ordinary cause.  The call succeeds; the agent it
 * names is still running against the config.yaml it already had.
 *
 * Owned by the app and freed by the next call, exactly like
 * clawt_web_app_last_error() -- so copy it before rendering anything.
 *
 * Returns: (nullable): the refusals as text, or %NULL
 */
const gchar *clawt_web_app_last_refusal(ClawtWebApp *self);

/**
 * clawt_web_app_unread:
 * @self: a #ClawtWebApp
 * @agent_id: (nullable): whose conversation
 *
 * How many messages have arrived from @agent_id since its conversation
 * was last opened.
 *
 * The counterpart of the transcript's "New messages" rule, and the two
 * never fire for the same message: a conversation on screen never
 * accrues a count whatever the scroll position -- that case is the
 * rule's -- and a conversation elsewhere accrues one.
 *
 * Session-scoped and in memory.  It knows nothing about what arrived
 * while nobody was connected.
 *
 * Returns: the count, 0 if there is none
 */
guint clawt_web_app_unread(ClawtWebApp *self, const gchar *agent_id);

/**
 * clawt_web_app_unread_total:
 * @self: a #ClawtWebApp
 *
 * Every agent's unread count added up, for the Chat tab.
 *
 * Returns: the total
 */
guint clawt_web_app_unread_total(ClawtWebApp *self);

/**
 * clawt_web_app_open_decisions:
 * @self: a #ClawtWebApp
 *
 * How many decisions are waiting on a person, across the whole fleet.
 *
 * The number the Work tab's badge carries.  Cached and invalidated by
 * any `decision.` event rather than counted from the stream, so a replay
 * that fell off the ring cannot leave it permanently wrong -- and asked
 * from a render rather than from the event handler.
 *
 * A daemon that did not answer gives 0 and leaves the count unknown, so
 * the next render asks again.
 *
 * Returns: the count
 */
guint clawt_web_app_open_decisions(ClawtWebApp *self);

/**
 * clawt_web_app_note_fleet:
 * @self: a #ClawtWebApp
 * @agents: (nullable): the `agents` array from an `agent.list` reply
 *
 * Learns which room is whose conversation, and forgets agents that have
 * gone.
 *
 * Called wherever the fleet is listed rather than from the event
 * handler: a request issued from there would run while a page render is
 * blocked inside its own request on the same context.
 */
void clawt_web_app_note_fleet(ClawtWebApp *self, JsonArray *agents,
                              JsonArray *rooms);

/**
 * clawt_web_app_note_connection_status:
 * @self: the app
 * @name: the saved connection's name
 * @status: (transfer full) (nullable): what a probe found, or %NULL to forget
 *
 * Remembers what one probe learned, so the connections page can draw it
 * on the next render rather than probing again to redraw the same row.
 */
void clawt_web_app_note_connection_status(ClawtWebApp           *self,
                                          const gchar           *name,
                                          ClawtConnectionStatus *status);

/**
 * clawt_web_app_connection_status:
 * @self: the app
 * @name: (nullable): a saved connection's name
 *
 * Returns: (transfer none) (nullable): what the last probe found, or
 *   %NULL if nobody has asked
 */
ClawtConnectionStatus *clawt_web_app_connection_status(ClawtWebApp *self,
                                                       const gchar *name);

/**
 * clawt_web_app_set_viewing:
 * @self: a #ClawtWebApp
 * @agent_id: (nullable): whose conversation is on screen, or %NULL
 *
 * Says which conversation is being read, and clears its count.
 *
 * Opening a conversation is the only thing that clears one.  Not
 * scrolling, not time passing: a counter that decays on its own is a
 * counter you stop trusting.
 */
void clawt_web_app_set_viewing(ClawtWebApp *self, const gchar *agent_id);

/**
 * clawt_web_app_set_viewing_room:
 * @self: a #ClawtWebApp
 * @room_id: (nullable): the group room on screen, or %NULL for none
 *
 * Which sidebar entry is a room rather than an agent.  The two are
 * exclusive -- a room is a first-class entry, not one of a selected
 * agent's conversations -- so setting one clears the other.
 *
 * Opening it also clears its unread count, which is the only thing that
 * does: a counter that decays on its own is a counter you stop
 * trusting.
 */
void clawt_web_app_set_viewing_room(ClawtWebApp *self,
                                    const gchar *room_id);

/**
 * clawt_web_app_get_viewing_room:
 * @self: a #ClawtWebApp
 *
 * Returns: (nullable) (transfer none): the room on screen, or %NULL
 */
const gchar *clawt_web_app_get_viewing_room(ClawtWebApp *self);

/**
 * clawt_web_app_switch:
 * @self: a #ClawtWebApp
 * @connection: (transfer none): the daemon to talk to instead
 * @error: (out) (optional): return location for a #GError
 *
 * Points this server at a different daemon.
 *
 * The new client is connected *before* the old one is dropped: a remote
 * daemon that is not running is the ordinary case here, and releasing
 * the working connection first would leave every open page connected to
 * nothing because of a typo in a port.
 *
 * Unlike the GTK client, where switching affects one window, this
 * affects every browser looking at this server -- there is one
 * connection here, not one per viewer. The page says so before it asks.
 *
 * Returns: %TRUE if the new daemon answered and is now in use
 */
gboolean clawt_web_app_switch(ClawtWebApp      *self,
                              ClawtConnection  *connection,
                              GError          **error);

/**
 * clawt_web_app_get_connection_name:
 * @self: a #ClawtWebApp
 *
 * Returns: (nullable): what the current daemon is called, if it came
 *   from a profile
 */
const gchar *clawt_web_app_get_connection_name(ClawtWebApp *self);

/**
 * clawt_web_app_set_connection:
 * @self: a #ClawtWebApp
 * @connection: (nullable): which daemon this process is serving
 *
 * The whole connection rather than a name, because the connection
 * banner has to say different things for a local daemon and one on
 * another machine, and a name cannot tell them apart.
 */
void clawt_web_app_set_connection(ClawtWebApp     *self,
                                  ClawtConnection *connection);

/**
 * clawt_web_app_add_stream:
 * @self: a #ClawtWebApp
 * @connection: (transfer none): a new SSE connection
 *
 * Registers a browser to be told when the fleet changes.
 *
 * The connection removes itself when it closes, which is the whole
 * reason HtmxSseConnection::closed had to exist: without it every tab
 * somebody shut left an entry here for the life of the process.
 */
void clawt_web_app_add_stream(ClawtWebApp       *self,
                              HtmxSseConnection *connection);

/**
 * clawt_web_app_stream_count:
 * @self: a #ClawtWebApp
 *
 * Returns: how many browsers are currently listening
 */
guint clawt_web_app_stream_count(ClawtWebApp *self);

/* ── Reading a reply ─────────────────────────────────────────────── */

/**
 * clawt_web_member:
 * @object: (nullable): a #JsonObject
 * @key: a member name
 * @fallback: (nullable): what to return when the member is absent
 *
 * A string member, or @fallback.
 *
 * Every reply from the daemon is read through this rather than through
 * json_object_get_string_member(), which criticals on a member that is
 * absent or is null -- and the daemon leaves a member out whenever it
 * does not apply, which for an optional field is most of the time.
 *
 * Returns: (nullable): the value, borrowed from @object
 */
const gchar *clawt_web_member(JsonObject  *object,
                              const gchar *key,
                              const gchar *fallback);

/**
 * clawt_web_member_int:
 * @object: (nullable): a #JsonObject
 * @key: a member name
 * @fallback: what to return when the member is absent
 *
 * Returns: the value, or @fallback
 */
gint64 clawt_web_member_int(JsonObject  *object,
                            const gchar *key,
                            gint64       fallback);

/**
 * clawt_web_member_bool:
 * @object: (nullable): a #JsonObject
 * @key: a member name
 * @fallback: what to return when the member is absent
 *
 * Returns: the value, or @fallback
 */
gboolean clawt_web_member_bool(JsonObject  *object,
                               const gchar *key,
                               gboolean     fallback);

/**
 * clawt_web_member_array:
 * @object: (nullable): a #JsonObject
 * @key: a member name
 *
 * Returns: (transfer none) (nullable): the array, or %NULL
 */
JsonArray *clawt_web_member_array(JsonObject  *object,
                                  const gchar *key);

/**
 * clawt_web_member_object:
 * @object: (nullable): a #JsonObject
 * @key: a member name
 *
 * Returns: (transfer none) (nullable): the object, or %NULL
 */
JsonObject *clawt_web_member_object(JsonObject  *object,
                                    const gchar *key);

/**
 * clawt_web_root:
 * @reply: (nullable): a reply node
 *
 * Returns: (transfer none) (nullable): the reply's object
 */
JsonObject *clawt_web_root(JsonNode *reply);

/* ── Building a payload ──────────────────────────────────────────── */

/**
 * ClawtWebPayload:
 *
 * A JSON object under construction, for the many handlers that build a
 * two-or-three member payload and send it.
 */
typedef struct _ClawtWebPayload ClawtWebPayload;

ClawtWebPayload *clawt_web_payload_new(void);
void             clawt_web_payload_set(ClawtWebPayload *self,
                                       const gchar     *key,
                                       const gchar     *value);
void             clawt_web_payload_set_int(ClawtWebPayload *self,
                                           const gchar     *key,
                                           gint64           value);
void             clawt_web_payload_set_bool(ClawtWebPayload *self,
                                            const gchar     *key,
                                            gboolean         value);
void             clawt_web_payload_set_list(ClawtWebPayload    *self,
                                            const gchar        *key,
                                            const gchar *const *values);

/**
 * clawt_web_payload_take:
 * @self: (transfer full): a #ClawtWebPayload
 *
 * Finishes the object and releases the builder.
 *
 * Returns: (transfer full): the finished node
 */
JsonNode *clawt_web_payload_take(ClawtWebPayload *self);

/**
 * clawt_web_payload_free:
 * @self: (transfer full) (nullable): a #ClawtWebPayload
 *
 * Discards a payload that was never sent.
 */
void clawt_web_payload_free(ClawtWebPayload *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtWebPayload, clawt_web_payload_free)

G_END_DECLS
