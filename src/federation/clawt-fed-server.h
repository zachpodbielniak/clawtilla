/*
 * clawt-fed-server.h - The HTTP door other fleets knock on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A SoupServer on a unix socket by default -- cloudflared reaches it
 * directly (`service: unix:...`) and nothing on the network can -- or on
 * a loopback, tailnet or LAN address.  It speaks plain HTTP: TLS belongs
 * to whatever stands in front of it, and authenticity to the envelope.
 *
 * It knows nothing about peers or grants.  It bounds the body, reads the
 * stamp out of the headers, and hands both to its handler, which
 * verifies and answers; it then puts the answer's stamp back into
 * headers.  The signature is checked before any JSON is parsed: the
 * handler gets the body as text.
 *
 * Routes, from the pure clawt_fed_route():
 *
 *   GET  /fed/v1/hello          who this fleet is (signed, no auth)
 *   POST /fed/v1/invite/accept  redeem an invite
 *   POST /fed/v1/envelope       anything else, signed
 *   GET  /fed/v1/events         the live event stream (WebSocket)
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <libsoup/soup.h>
#include "federation/clawt-fed-envelope.h"

G_BEGIN_DECLS

/**
 * ClawtFedRoute:
 * @CLAWT_FED_ROUTE_NONE: nothing here
 * @CLAWT_FED_ROUTE_HELLO: `GET /fed/v1/hello`
 * @CLAWT_FED_ROUTE_INVITE: `POST /fed/v1/invite/accept`
 * @CLAWT_FED_ROUTE_ENVELOPE: `POST /fed/v1/envelope`
 * @CLAWT_FED_ROUTE_EVENTS: `GET /fed/v1/events`
 * @CLAWT_FED_ROUTE_WRONG_METHOD: a known path, the wrong method
 *
 * Where a request goes.
 */
typedef enum {
    CLAWT_FED_ROUTE_NONE = 0,
    CLAWT_FED_ROUTE_HELLO,
    CLAWT_FED_ROUTE_INVITE,
    CLAWT_FED_ROUTE_ENVELOPE,
    CLAWT_FED_ROUTE_EVENTS,
    CLAWT_FED_ROUTE_WRONG_METHOD
} ClawtFedRoute;

/**
 * clawt_fed_route:
 * @method: the request method
 * @path: the request path, without the query
 *
 * Pure, so the routing table is tested without a socket.  A path under a
 * base (`/base/fed/v1/envelope`, behind a proxy that mounts it there) is
 * found by its suffix.
 *
 * Returns: the route
 */
ClawtFedRoute clawt_fed_route(const gchar *method,
                              const gchar *path);

/**
 * clawt_fed_route_binding:
 * @route: a route
 *
 * Returns: (transfer none): the binding a stamp on this route signs:
 *   `POST /fed/v1/envelope`, ...
 */
const gchar *clawt_fed_route_binding(ClawtFedRoute route);

/**
 * ClawtFedAnswer:
 * @status: the HTTP status
 * @stamp: (nullable): the answer's stamp, set into headers
 * @body: (nullable): the answer, JSON text
 *
 * What a handler fills in.
 */
typedef struct {
    guint          status;
    ClawtFedStamp *stamp;
    gchar         *body;
} ClawtFedAnswer;

#define CLAWT_TYPE_FED_SERVER (clawt_fed_server_get_type())
G_DECLARE_FINAL_TYPE(ClawtFedServer, clawt_fed_server, CLAWT, FED_SERVER, GObject)

/**
 * ClawtFedServerFunc:
 * @server: the server
 * @route: which route
 * @stamp: (nullable): the request's stamp; %NULL when it carried none
 * @body: the request body, text (empty for none)
 * @remote: (nullable): who asked, for the audit and rate limits: the
 *   `CF-Connecting-IP` a local tunnel reports, else the socket's address
 * @answer: (out caller-allocates): what to answer; the server owns the
 *   members afterwards
 * @user_data: closure
 *
 * Answers one request.  Runs on the daemon's main context and must not
 * wait on the network.
 */
typedef void (*ClawtFedServerFunc)(ClawtFedServer *server,
                                   ClawtFedRoute   route,
                                   ClawtFedStamp  *stamp,
                                   const gchar    *body,
                                   const gchar    *remote,
                                   ClawtFedAnswer *answer,
                                   gpointer        user_data);

/**
 * ClawtFedSocketFunc:
 * @server: the server
 * @connection: (transfer none): a WebSocket another fleet opened
 * @stamp: the stamp its upgrade request carried
 * @user_data: closure
 *
 * Takes an events stream.  Returns %FALSE to refuse it (the upgrade then
 * fails before any frame flows).
 *
 * Returns: %TRUE if taken
 */
typedef gboolean (*ClawtFedSocketFunc)(ClawtFedServer           *server,
                                       SoupWebsocketConnection  *connection,
                                       ClawtFedStamp            *stamp,
                                       gpointer                  user_data);

/**
 * clawt_fed_server_new:
 * @max_body_bytes: the largest request body accepted
 * @handler: (scope forever): answers requests
 * @socket_handler: (scope forever) (nullable): takes event streams
 * @user_data: closure for both
 *
 * Returns: (transfer full): a server listening nowhere yet
 */
ClawtFedServer *clawt_fed_server_new(gsize              max_body_bytes,
                                     ClawtFedServerFunc handler,
                                     ClawtFedSocketFunc socket_handler,
                                     gpointer           user_data);

/**
 * clawt_fed_server_listen:
 * @self: a #ClawtFedServer
 * @where: `unix:PATH` or `HOST:PORT` (`[v6]:PORT`)
 * @error: (out) (optional): return location for a #GError
 *
 * Binds one more address.  A unix socket's stale file is replaced (the
 * daemon's state lock says nobody else owns it) and the new one is 0600.
 *
 * Returns: %TRUE if bound
 */
gboolean clawt_fed_server_listen(ClawtFedServer  *self,
                                 const gchar     *where,
                                 GError         **error);

/**
 * clawt_fed_server_list_bound:
 * @self: a #ClawtFedServer
 *
 * Returns: (transfer full) (array zero-terminated=1): what is bound, in
 *   the spelling clawt_fed_server_listen() took
 */
gchar **clawt_fed_server_list_bound(ClawtFedServer *self);

/**
 * clawt_fed_server_stop:
 * @self: a #ClawtFedServer
 *
 * Stops listening and removes the unix socket files it made.
 */
void clawt_fed_server_stop(ClawtFedServer *self);

G_END_DECLS
