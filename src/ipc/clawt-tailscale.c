/*
 * clawt-tailscale.c - Finding the address the tailnet reaches us on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "ipc/clawt-tailscale.h"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

/*
 * Tailscale's IPv4 range: 100.64.0.0/10, the CGNAT space.  A /10 is not a
 * whole-octet boundary, so the second octet is a range rather than a
 * value -- 100.64.x.x through 100.127.x.x.  Getting that wrong in the
 * generous direction would accept 100.128.0.1, which is somebody else's
 * address on the open internet.
 */
#define TAILNET_V4_FIRST_OCTET  (100)
#define TAILNET_V4_SECOND_LOW   (64)
#define TAILNET_V4_SECOND_HIGH  (127)

/* And its IPv6 range, fd7a:115c:a1e0::/48. */
static const guint8 tailnet_v6_prefix[6] = {
    0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0
};

gboolean
clawt_tailscale_is_tailnet_address(const gchar *address)
{
    struct in_addr v4;
    struct in6_addr v6;

    if (address == NULL || *address == '\0')
        return FALSE;

    if (inet_pton(AF_INET, address, &v4) == 1) {
        guint32 host = g_ntohl(v4.s_addr);
        guint8 first = (guint8)((host >> 24) & 0xff);
        guint8 second = (guint8)((host >> 16) & 0xff);

        return first == TAILNET_V4_FIRST_OCTET &&
               second >= TAILNET_V4_SECOND_LOW &&
               second <= TAILNET_V4_SECOND_HIGH;
    }

    if (inet_pton(AF_INET6, address, &v6) == 1)
        return memcmp(&v6, tailnet_v6_prefix, sizeof(tailnet_v6_prefix)) == 0;

    return FALSE;
}

gboolean
clawt_tailscale_is_tailnet_interface(const gchar *name)
{
    if (name == NULL || *name == '\0')
        return FALSE;

    return g_str_has_prefix(name, "tailscale") ||
           g_str_has_prefix(name, "utun");
}

/*
 * Walks the interface list once, and takes the first address that is both
 * on an interface Tailscale would have made and inside a range Tailscale
 * owns.  Both tests, because either alone is wrong: a utun is any tunnel
 * on a BSD, and a container can hold a 100.64/10 address for reasons of
 * its own.
 *
 * IPv4 is preferred over IPv6 -- not on merit, but because it is what a
 * person reads off `tailscale status` and types into the other machine,
 * and an address they do not recognise looks like the wrong one.
 */
gchar *
clawt_tailscale_find_address(void)
{
    struct ifaddrs *addresses = NULL;
    struct ifaddrs *entry;
    gchar *v6_match = NULL;

    if (getifaddrs(&addresses) != 0)
        return NULL;

    for (entry = addresses; entry != NULL; entry = entry->ifa_next) {
        gchar text[INET6_ADDRSTRLEN];

        if (entry->ifa_addr == NULL)
            continue;

        if (!clawt_tailscale_is_tailnet_interface(entry->ifa_name))
            continue;

        if (entry->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)entry->ifa_addr;

            if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) == NULL)
                continue;

            if (clawt_tailscale_is_tailnet_address(text)) {
                freeifaddrs(addresses);
                g_free(v6_match);
                return g_strdup(text);
            }
        } else if (entry->ifa_addr->sa_family == AF_INET6 &&
                   v6_match == NULL) {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)entry->ifa_addr;

            if (inet_ntop(AF_INET6, &in6->sin6_addr, text,
                          sizeof(text)) == NULL)
                continue;

            if (clawt_tailscale_is_tailnet_address(text))
                v6_match = g_strdup(text);
        }
    }

    freeifaddrs(addresses);

    return v6_match;
}

/* ── This machine's tailnet name ──────────────────────────────────── */

#define TAILSCALED_SOCKET "/var/run/tailscale/tailscaled.sock"
#define LOCALAPI_TIMEOUT_SECONDS (5)

gchar *
clawt_tailscale_dns_name_from_status(const gchar  *body,
                                     gssize        length,
                                     GError      **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;
    JsonNode *self_node;
    JsonObject *self_object;
    JsonNode *name;
    g_autofree gchar *dns = NULL;

    if (body == NULL ||
        !json_parser_load_from_data(parser, body, length, error))
        return NULL;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) ||
        (object = json_node_get_object(root)) == NULL ||
        (self_node = json_object_get_member(object, "Self")) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(self_node) ||
        (self_object = json_node_get_object(self_node)) == NULL ||
        (name = json_object_get_member(self_object, "DNSName")) == NULL ||
        !JSON_NODE_HOLDS_VALUE(name) ||
        json_node_get_value_type(name) != G_TYPE_STRING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "tailscaled's status names no DNSName for this "
                            "machine");
        return NULL;
    }

    dns = g_strdup(json_node_get_string(name));

    /* A fully qualified name ends in a dot, which no URL wants. */
    while (g_str_has_suffix(dns, "."))
        dns[strlen(dns) - 1] = '\0';

    if (*dns == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "this machine has no MagicDNS name; set "
                            "grok.callback_host");
        return NULL;
    }

    return g_steal_pointer(&dns);
}

static void
on_status_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    SoupMessage *message = g_task_get_task_data(task);
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    gchar *name;

    body = clawt_http_send_and_read_finish(SOUP_SESSION(source), result,
                                           &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message))) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                "tailscaled answered %u; the daemon's user "
                                "may not read its status -- set "
                                "grok.callback_host",
                                soup_message_get_status(message));
        g_object_unref(task);
        return;
    }

    {
        gsize length = 0;
        const gchar *data = g_bytes_get_data(body, &length);

        name = clawt_tailscale_dns_name_from_status(data, (gssize)length,
                                                    &error);
    }

    if (name == NULL)
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_pointer(task, name, g_free);

    g_object_unref(task);
}

void
clawt_tailscale_dns_name_async(GMainContext        *context,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(SoupSession) session = NULL;
    SoupMessage *message;
    GTask *task;

    /*
     * Pushed before the session is made: libsoup binds a session to the
     * context that is thread-default when it is created, and attaches
     * its sources there from inside its own callbacks -- a session built
     * under the process default and read from the daemon's context sends
     * and never hears the answer.
     */
    if (context != NULL)
        g_main_context_push_thread_default(context);

    task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, clawt_tailscale_dns_name_async);

    if (!g_file_test(TAILSCALED_SOCKET, G_FILE_TEST_EXISTS)) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                                "tailscaled is not running here (%s is "
                                "missing)", TAILSCALED_SOCKET);
        g_object_unref(task);

        if (context != NULL)
            g_main_context_pop_thread_default(context);
        return;
    }

    /*
     * LocalAPI over its unix socket, never `tailscale status` in a
     * subprocess: the same rule as the address, which comes from
     * getifaddrs() rather than `tailscale ip`.  The host part of the URL
     * is what tailscaled expects to see and names nothing on the network.
     */
    address = g_unix_socket_address_new(TAILSCALED_SOCKET);
    session = soup_session_new_with_options("remote-connectable", address,
                                            "timeout",
                                            LOCALAPI_TIMEOUT_SECONDS, NULL);
    message = clawt_http_message_new(
        "GET", "http://local-tailscaled.sock/localapi/v0/status",
        CLAWT_HTTP_NO_CREDENTIALS);
    g_task_set_task_data(task, message, g_object_unref);

    /* Held by the task: a session freed mid-request abandons it. */
    g_object_set_data_full(G_OBJECT(task), "clawt-session",
                           g_object_ref(session), g_object_unref);

    soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
                                     cancellable, on_status_read, task);

    if (context != NULL)
        g_main_context_pop_thread_default(context);
}

gchar *
clawt_tailscale_dns_name_finish(GAsyncResult  *result,
                                GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
