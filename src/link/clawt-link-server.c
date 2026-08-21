/*
 * clawt-link-server.c - Accepts agents dialling in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "link/clawt-link-server.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <errno.h>

/* Declared in clawt-link.c; internal to the link layer. */
void clawt_link_accept_identity(ClawtLink *self, const gchar *agent_id);

/*
 * How often keepalives go out, and how long silence is tolerated.
 *
 * An agent that has stopped answering is not merely quiet: its process may
 * be gone while the socket stays open, in which case messages routed to it
 * would vanish. The ping is what turns that into a detectable event.
 */
#define KEEPALIVE_INTERVAL_SECONDS 30
#define KEEPALIVE_DEADLINE_SECONDS 120

enum {
    SIGNAL_LINK_ADDED,
    SIGNAL_LINK_REMOVED,
    SIGNAL_MESSAGE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtLinkServer {
    GObject parent_instance;

    gchar          *socket_path;
    GSocketService *service;

    GHashTable     *links;      /* agent_id -> ClawtLink (owned) */
    GPtrArray      *pending;    /* ClawtLink*, not yet identified */

    ClawtLinkAuthFunc auth_func;
    gpointer          auth_data;
    GDestroyNotify    auth_destroy;

    guint keepalive_source_id;
};

G_DEFINE_FINAL_TYPE(ClawtLinkServer, clawt_link_server, G_TYPE_OBJECT)

static void
on_link_message(ClawtLink   *link,
                const gchar *room_id,
                const gchar *body,
                const gchar *thread_id,
                gpointer     user_data)
{
    ClawtLinkServer *self = user_data;

    g_signal_emit(self, signals[SIGNAL_MESSAGE], 0,
                  clawt_link_get_agent_id(link), room_id, body, thread_id);
}

static void
on_link_closed(ClawtLink *link, gpointer user_data)
{
    ClawtLinkServer *self = user_data;
    const gchar *agent_id = clawt_link_get_agent_id(link);

    g_ptr_array_remove(self->pending, link);

    if (agent_id == NULL)
        return;

    /*
     * Only drop it from the table if it is still the current link for that
     * agent.  An agent that reconnected before its old connection finished
     * closing would otherwise have the new link removed by the old one's
     * teardown, and end up unreachable while appearing connected.
     */
    if (g_hash_table_lookup(self->links, agent_id) == link) {
        g_object_ref(link);
        g_hash_table_remove(self->links, agent_id);
        g_signal_emit(self, signals[SIGNAL_LINK_REMOVED], 0, agent_id);
        g_object_unref(link);
    }
}

static void
on_link_hello(ClawtLink   *link,
              const gchar *agent_id,
              const gchar *token,
              gpointer     user_data)
{
    ClawtLinkServer *self = user_data;
    g_autoptr(GError) error = NULL;
    ClawtLink *existing;

    if (self->auth_func != NULL &&
        !self->auth_func(agent_id, token, self->auth_data)) {
        g_info("link server: refused a connection claiming to be '%s'",
               agent_id);
        clawt_link_send_error(link, 403, "not authorised for that agent");
        clawt_link_close(link, "not authorised");
        return;
    }

    /*
     * A reconnect replaces the old link.  Keeping both would mean messages
     * going to whichever the table happened to hold, and the stale one
     * never noticing it had been superseded.
     */
    existing = g_hash_table_lookup(self->links, agent_id);
    if (existing != NULL && existing != link) {
        g_info("link server: '%s' reconnected; closing the previous link",
               agent_id);
        clawt_link_close(existing, "replaced by a newer connection");
    }

    clawt_link_accept_identity(link, agent_id);

    g_ptr_array_remove(self->pending, link);
    g_hash_table_insert(self->links, g_strdup(agent_id), g_object_ref(link));

    if (!clawt_link_send_welcome(link, &error)) {
        g_warning("link server: could not welcome '%s': %s",
                  agent_id, error->message);
        clawt_link_close(link, NULL);
        return;
    }

    g_info("link server: agent '%s' connected", agent_id);
    g_signal_emit(self, signals[SIGNAL_LINK_ADDED], 0, agent_id);
}

static gboolean
on_incoming(GSocketService    *service,
            GSocketConnection *connection,
            GObject           *source,
            gpointer           user_data)
{
    ClawtLinkServer *self = user_data;
    ClawtLink *link;

    (void)service;
    (void)source;

    link = clawt_link_new(connection);

    g_signal_connect(link, "hello", G_CALLBACK(on_link_hello), self);
    g_signal_connect(link, "message", G_CALLBACK(on_link_message), self);
    g_signal_connect(link, "closed", G_CALLBACK(on_link_closed), self);

    /*
     * Held in `pending` until it says who it is.  Without this the link is
     * unreferenced the moment this function returns and is finalized before
     * its first frame arrives.
     */
    g_ptr_array_add(self->pending, link);

    clawt_link_start(link);

    return TRUE;
}

/*
 * Pings every link, and drops those that have gone quiet for too long.
 *
 * A socket can stay open long after the process behind it has stopped
 * responding, and messages routed to such an agent would simply vanish.
 */
static gboolean
on_keepalive(gpointer user_data)
{
    ClawtLinkServer *self = user_data;
    g_autoptr(GList) agents = g_hash_table_get_keys(self->links);
    GList *l;

    for (l = agents; l != NULL; l = l->next) {
        ClawtLink *link = g_hash_table_lookup(self->links, l->data);

        if (link == NULL)
            continue;

        if (clawt_link_seconds_since_seen(link) > KEEPALIVE_DEADLINE_SECONDS) {
            g_info("link server: '%s' stopped answering; closing its link",
                   (const gchar *)l->data);
            clawt_link_close(link, "no response to keepalives");
            continue;
        }

        clawt_link_ping(link);
    }

    return G_SOURCE_CONTINUE;
}

ClawtLinkServer *
clawt_link_server_new(const gchar *socket_path)
{
    ClawtLinkServer *self;

    g_return_val_if_fail(socket_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_LINK_SERVER, NULL);
    self->socket_path = clawt_expand_path(socket_path);

    return self;
}

void
clawt_link_server_set_auth_func(ClawtLinkServer   *self,
                                ClawtLinkAuthFunc  func,
                                gpointer           user_data,
                                GDestroyNotify     destroy)
{
    g_return_if_fail(CLAWT_IS_LINK_SERVER(self));

    if (self->auth_destroy != NULL && self->auth_data != NULL)
        self->auth_destroy(self->auth_data);

    self->auth_func = func;
    self->auth_data = user_data;
    self->auth_destroy = destroy;
}

/*
 * Removes a socket left behind by a daemon that did not shut down cleanly.
 *
 * Only after checking that nothing is listening on it.  Unlinking a live
 * socket would leave the running daemon's agents connected to a path that
 * no longer exists, and silently steal every new connection from it -- two
 * daemons quietly fighting over one fleet.
 */
static gboolean
clear_stale_socket(const gchar *path, GError **error)
{
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) probe = NULL;
    g_autoptr(GError) probe_error = NULL;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return TRUE;

    address = g_unix_socket_address_new(path);
    client = g_socket_client_new();
    probe = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address),
                                    NULL, &probe_error);

    if (probe != NULL) {
        g_io_stream_close(G_IO_STREAM(probe), NULL, NULL);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "another clawtilla daemon is already listening on %s",
                    path);
        return FALSE;
    }

    if (g_unlink(path) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not remove the stale socket %s: %s",
                    path, g_strerror(errno));
        return FALSE;
    }

    g_info("link server: removed a stale socket at %s", path);
    return TRUE;
}

gboolean
clawt_link_server_start(ClawtLinkServer *self, GError **error)
{
    g_autoptr(GSocketAddress) address = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(CLAWT_IS_LINK_SERVER(self), FALSE);

    if (self->service != NULL)
        return TRUE;

    if (!clawt_check_socket_path(self->socket_path, error))
        return FALSE;

    dir = g_path_get_dirname(self->socket_path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    if (!clear_stale_socket(self->socket_path, error))
        return FALSE;

    self->service = g_socket_service_new();
    address = g_unix_socket_address_new(self->socket_path);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                       address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, NULL, error)) {
        g_clear_object(&self->service);
        g_prefix_error(error, "listening on %s: ", self->socket_path);
        return FALSE;
    }

    /*
     * 0600 on the socket itself.  Everything an agent may do -- read its
     * mail, message its peers, run commands on its computer -- goes through
     * here, so the file permissions are the outer gate and the token the
     * inner one.
     */
    if (g_chmod(self->socket_path, 0600) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not restrict %s to the owner: %s",
                    self->socket_path, g_strerror(errno));
        clawt_link_server_stop(self);
        return FALSE;
    }

    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming), self);
    g_socket_service_start(self->service);

    self->keepalive_source_id =
        g_timeout_add_seconds(KEEPALIVE_INTERVAL_SECONDS, on_keepalive, self);

    g_info("link server: listening on %s", self->socket_path);

    return TRUE;
}

void
clawt_link_server_stop(ClawtLinkServer *self)
{
    g_autoptr(GList) agents = NULL;
    GList *l;
    guint i;

    g_return_if_fail(CLAWT_IS_LINK_SERVER(self));

    if (self->keepalive_source_id != 0) {
        g_source_remove(self->keepalive_source_id);
        self->keepalive_source_id = 0;
    }

    if (self->service != NULL) {
        g_socket_service_stop(self->service);
        g_socket_listener_close(G_SOCKET_LISTENER(self->service));
        g_clear_object(&self->service);
    }

    /*
     * Told, not just dropped.  An agent that sees a bye knows the daemon
     * went away deliberately and can reconnect calmly, rather than treating
     * it as a crash.
     */
    agents = g_hash_table_get_keys(self->links);
    for (l = agents; l != NULL; l = l->next) {
        ClawtLink *link = g_hash_table_lookup(self->links, l->data);

        if (link != NULL)
            clawt_link_close(link, "the daemon is shutting down");
    }

    for (i = 0; i < self->pending->len; i++)
        clawt_link_close(g_ptr_array_index(self->pending, i), NULL);

    g_hash_table_remove_all(self->links);
    g_ptr_array_set_size(self->pending, 0);

    if (self->socket_path != NULL)
        g_unlink(self->socket_path);
}

ClawtLink *
clawt_link_server_get_link(ClawtLinkServer *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_LINK_SERVER(self), NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return g_hash_table_lookup(self->links, agent_id);
}

const gchar *
clawt_link_server_get_socket_path(ClawtLinkServer *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK_SERVER(self), NULL);
    return self->socket_path;
}

guint
clawt_link_server_count_links(ClawtLinkServer *self)
{
    g_return_val_if_fail(CLAWT_IS_LINK_SERVER(self), 0);
    return g_hash_table_size(self->links);
}

static void
clawt_link_server_dispose(GObject *object)
{
    ClawtLinkServer *self = CLAWT_LINK_SERVER(object);

    clawt_link_server_stop(self);

    if (self->auth_destroy != NULL && self->auth_data != NULL) {
        self->auth_destroy(self->auth_data);
        self->auth_destroy = NULL;
        self->auth_data = NULL;
    }

    g_clear_pointer(&self->links, g_hash_table_unref);
    g_clear_pointer(&self->pending, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_link_server_parent_class)->dispose(object);
}

static void
clawt_link_server_finalize(GObject *object)
{
    ClawtLinkServer *self = CLAWT_LINK_SERVER(object);

    g_clear_pointer(&self->socket_path, g_free);

    G_OBJECT_CLASS(clawt_link_server_parent_class)->finalize(object);
}

static void
clawt_link_server_class_init(ClawtLinkServerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_link_server_dispose;
    object_class->finalize = clawt_link_server_finalize;

    /**
     * ClawtLinkServer::link-added:
     * @self: the server
     * @agent_id: the agent that connected
     */
    signals[SIGNAL_LINK_ADDED] =
        g_signal_new("link-added", CLAWT_TYPE_LINK_SERVER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * ClawtLinkServer::link-removed:
     * @self: the server
     * @agent_id: the agent that went away
     */
    signals[SIGNAL_LINK_REMOVED] =
        g_signal_new("link-removed", CLAWT_TYPE_LINK_SERVER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * ClawtLinkServer::message:
     * @self: the server
     * @agent_id: who sent it
     * @room_id: (nullable): the room
     * @body: the message
     * @thread_id: (nullable): the thread
     */
    signals[SIGNAL_MESSAGE] =
        g_signal_new("message", CLAWT_TYPE_LINK_SERVER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                     G_TYPE_STRING);
}

static void
clawt_link_server_init(ClawtLinkServer *self)
{
    self->links = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, g_object_unref);
    self->pending = g_ptr_array_new_with_free_func(g_object_unref);
}
