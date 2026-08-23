/*
 * clawt-tailscale.c - Finding the address the tailnet reaches us on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-tailscale.h"

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
