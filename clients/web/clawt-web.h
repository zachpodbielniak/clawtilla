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
 * clawt_web_app_last_error:
 * @self: a #ClawtWebApp
 *
 * Why the most recent clawt_web_app_call() returned %NULL.
 *
 * Returns: (nullable): the message, owned by @self
 */
const gchar *clawt_web_app_last_error(ClawtWebApp *self);

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
