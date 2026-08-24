/*
 * web-events.c - Telling a browser that something moved
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One SSE stream per open page.  What goes down it is a name and a
 * subject, never the change itself: a browser told "the fleet changed"
 * re-fetches the fragments it is showing, which against a daemon on the
 * same machine is one cheap round trip -- while an event carrying the row
 * would mean this client rendering the same row two ways, once from a
 * reply and once from an event, and the two disagreeing the first time
 * either changed.
 */

#include "web-pages.h"

/*
 * A comment every half minute, so an idle stream is not mistaken for a
 * dead one.
 *
 * A quiet fleet can go hours without an event, and anything between the
 * browser and here -- a proxy somebody put in front of it, a NAT holding
 * a mapping open -- times an idle connection out without saying so.  The
 * browser then reconnects, which is fine, but the page has been stale for
 * however long it took to notice.
 */
static gboolean
keepalive(gpointer user_data)
{
    HtmxSseConnection *connection = user_data;

    if (!htmx_sse_connection_is_connected(connection))
        return G_SOURCE_REMOVE;

    htmx_sse_connection_send_comment(connection, "still here");

    return G_SOURCE_CONTINUE;
}

static void
on_closed(HtmxSseConnection *connection, gpointer user_data)
{
    guint source_id = GPOINTER_TO_UINT(user_data);

    (void)connection;

    /*
     * The timer goes with the connection. It is the whole reason
     * HtmxSseConnection grew a ::closed signal: a browser tab closes
     * without saying anything at the HTTP level, so without this every
     * tab somebody shut left a timer running for the life of the
     * process.
     */
    g_source_remove(source_id);
}

static HtmxResponse *
on_events(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(HtmxSseConnection) connection = NULL;
    guint source_id;

    (void)params;

    connection = htmx_sse_connection_new(htmx_request_get_message(request));

    if (connection == NULL) {
        HtmxResponse *failed =
            htmx_response_new_with_content("cannot open an event stream");

        htmx_response_set_status(failed, 500);

        return failed;
    }

    /*
     * Told to come back in two seconds if the stream drops. The default
     * is three, and a client that has just been restarted is exactly
     * when somebody is watching.
     */
    htmx_sse_connection_set_retry(connection, 2000);

    /*
     * One event immediately, so the page knows the stream is live rather
     * than merely open. A browser cannot tell an SSE connection that
     * works from one that will never say anything.
     */
    htmx_sse_connection_send_event(connection, "ready", "connected", NULL);

    source_id = g_timeout_add_seconds(30, keepalive, connection);
    g_signal_connect(connection, "closed", G_CALLBACK(on_closed),
                     GUINT_TO_POINTER(source_id));

    clawt_web_app_add_stream(app, connection);

    return htmx_response_new_streaming();
}

void
clawt_web_register_events(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/events", on_events, app);
}
