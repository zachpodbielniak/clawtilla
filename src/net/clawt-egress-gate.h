/*
 * clawt-egress-gate.h - The one way out of a gated agent's sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `runtime.network: gated` puts a confined agent in a network namespace
 * of its own, with nothing in it but loopback.  The only route out is
 * this: an HTTP proxy (CONNECT for TLS, an absolute http:// URL for plain
 * HTTP) that the daemon serves on a unix socket per agent, bound into the
 * box and bridged to 127.0.0.1:3128 inside it by clawtilla-relay.
 *
 * The gate resolves names itself and connects only to addresses
 * clawt_egress_decide() passed, so neither a name pointed at 127.0.0.1
 * nor a redirect gets anywhere: a CONNECT is a tunnel to one checked
 * address, and the address it actually connected to is checked again.
 * When one family cannot route -- an AAAA answer on a host with no IPv6
 * -- the next address is tried rather than the connection stalling.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "net/clawt-net-policy.h"

G_BEGIN_DECLS

/**
 * CLAWT_EGRESS_PROXY_PORT:
 *
 * The loopback port inside a gated agent's network namespace that
 * clawtilla-relay bridges to the gate.  Fixed, because the namespace is
 * the agent's own and nothing else listens there.
 */
#define CLAWT_EGRESS_PROXY_PORT 3128

/**
 * CLAWT_EGRESS_BROKER_PORT:
 *
 * The loopback port inside the namespace that clawtilla-relay bridges to
 * the provider-key broker.
 */
#define CLAWT_EGRESS_BROKER_PORT 3129

/**
 * CLAWT_EGRESS_MAX_HEAD:
 *
 * The most bytes the gate reads looking for the end of a request head.
 * Bounded while reading, not checked after: a client that never sends
 * the blank line must not grow a buffer for ever.
 */
#define CLAWT_EGRESS_MAX_HEAD 16384

/**
 * ClawtEgressRequest:
 * @is_connect: %TRUE for `CONNECT host:port`, %FALSE for a plain HTTP
 *   request with an absolute http:// URL
 * @host: the name or address literal asked for, without brackets
 * @port: the port asked for
 * @forward_head: (nullable): for plain HTTP, the head to send upstream:
 *   the request line in origin form, the client's headers without the
 *   proxy and hop-by-hop ones, and `Connection: close`
 *
 * One request to the gate, parsed.
 */
typedef struct {
	gboolean  is_connect;
	gchar    *host;
	guint16   port;
	gchar    *forward_head;
} ClawtEgressRequest;

/**
 * clawt_egress_request_free:
 * @request: (nullable): a request
 */
void
clawt_egress_request_free(
	ClawtEgressRequest *request
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEgressRequest, clawt_egress_request_free)

/**
 * clawt_egress_find_head_end:
 * @data: (array length=length): bytes read so far
 * @length: how many
 *
 * Where the request head ends.
 *
 * Returns: the offset just past the blank line, or -1 when it has not
 *   arrived yet
 */
gssize
clawt_egress_find_head_end(
	const guint8 *data,
	gsize         length
);

/**
 * clawt_egress_parse_request: (skip)
 * @head: (array length=length): the request head, through the blank line
 * @length: its length
 * @error: (out) (optional): return location for a #GError
 *
 * Parses what a client sent the gate.
 *
 * `CONNECT host:port` and `CONNECT [v6]:port` open a tunnel.  A plain
 * request with an absolute http:// URL is forwarded with its request line
 * rewritten to origin form and `Connection: close`, so a second request
 * on the same connection cannot ride the first one's checked address to
 * a different host.  An https:// URL (which must be a CONNECT) and an
 * origin-form request (which is not a proxy request at all) are refused
 * with a sentence saying so, as is a host that is neither a name nor an
 * address literal.
 *
 * Returns: (transfer full) (nullable): the request, or %NULL with @error
 *   set
 */
ClawtEgressRequest *
clawt_egress_parse_request(
	const gchar  *head,
	gsize         length,
	GError      **error
);

/**
 * ClawtEgressRefusal:
 * @at: when, in microseconds since the epoch
 * @host: the name or literal the agent asked for
 * @port: the port
 * @address: (nullable): the address that was refused, when there was one
 * @reason: why
 *
 * One connection the gate refused.
 */
typedef struct {
	gint64   at;
	gchar   *host;
	guint16  port;
	gchar   *address;
	gchar   *reason;
} ClawtEgressRefusal;

#define CLAWT_TYPE_EGRESS_REFUSAL (clawt_egress_refusal_get_type())

/**
 * clawt_egress_refusal_get_type:
 *
 * Returns: the #GType of #ClawtEgressRefusal
 */
GType clawt_egress_refusal_get_type(void) G_GNUC_CONST;

/**
 * clawt_egress_refusal_copy:
 * @refusal: a refusal
 *
 * Returns: (transfer full): a copy
 */
ClawtEgressRefusal *
clawt_egress_refusal_copy(
	const ClawtEgressRefusal *refusal
);

/**
 * clawt_egress_refusal_free:
 * @refusal: (nullable): a refusal
 */
void
clawt_egress_refusal_free(
	ClawtEgressRefusal *refusal
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEgressRefusal, clawt_egress_refusal_free)

#define CLAWT_TYPE_EGRESS_GATE (clawt_egress_gate_get_type())

G_DECLARE_FINAL_TYPE(ClawtEgressGate, clawt_egress_gate, CLAWT, EGRESS_GATE,
                     GObject)

/**
 * clawt_egress_gate_new:
 * @context: (nullable): the main context the gate's sockets run on;
 *   %NULL for the thread-default at the time of the call
 * @socket_dir: the directory each agent's socket is made in
 *
 * Creates a gate.  Nothing is bound until clawt_egress_gate_open().
 *
 * Returns: (transfer full): a new #ClawtEgressGate
 */
ClawtEgressGate *
clawt_egress_gate_new(
	GMainContext *context,
	const gchar  *socket_dir
);

/**
 * clawt_egress_gate_set_allowlist:
 * @self: a #ClawtEgressGate
 * @allow: (nullable): the operator's `egress.allow`, or %NULL for none
 *
 * Replaces the allowlist.  Applies to the next connection; a tunnel
 * already open was checked against the list it was opened under.
 */
void
clawt_egress_gate_set_allowlist(
	ClawtEgressGate      *self,
	ClawtEgressAllowlist *allow
);

/**
 * clawt_egress_gate_get_allowlist:
 * @self: a #ClawtEgressGate
 *
 * Returns: (transfer none) (nullable): the allowlist in force
 */
ClawtEgressAllowlist *
clawt_egress_gate_get_allowlist(
	ClawtEgressGate *self
);

/**
 * clawt_egress_gate_socket_path:
 * @self: a #ClawtEgressGate
 * @agent_id: an agent
 *
 * Where @agent_id's socket is, or will be.
 *
 * Returns: (transfer full): the path
 */
gchar *
clawt_egress_gate_socket_path(
	ClawtEgressGate *self,
	const gchar     *agent_id
);

/**
 * clawt_egress_gate_open:
 * @self: a #ClawtEgressGate
 * @agent_id: the agent whose sandbox the socket is bound into
 * @error: (out) (optional): return location for a #GError
 *
 * Binds @agent_id's socket, unless it is bound already, and returns its
 * path.  A unix socket, mode 0600, one per agent -- so every connection
 * arrives already knowing whose it is, and a refusal is counted against
 * the agent that made it.  Binding waits on nothing.
 *
 * Returns: (transfer full) (nullable): the socket path, or %NULL with
 *   @error set
 */
gchar *
clawt_egress_gate_open(
	ClawtEgressGate  *self,
	const gchar      *agent_id,
	GError          **error
);

/**
 * clawt_egress_gate_close:
 * @self: a #ClawtEgressGate
 * @agent_id: an agent
 *
 * Stops listening for @agent_id and removes its socket.  Tunnels already
 * open are left to finish.
 */
void
clawt_egress_gate_close(
	ClawtEgressGate *self,
	const gchar     *agent_id
);

/**
 * clawt_egress_gate_stop:
 * @self: a #ClawtEgressGate
 *
 * Closes every socket and cancels every connection in flight.
 */
void
clawt_egress_gate_stop(
	ClawtEgressGate *self
);

/**
 * clawt_egress_gate_record_refusal:
 * @self: a #ClawtEgressGate
 * @agent_id: whose connection it was
 * @host: what was asked for
 * @port: the port
 * @address: (nullable): the refused address
 * @reason: why
 *
 * Records a refusal against @agent_id and emits #ClawtEgressGate::refused.
 * The gate calls this for every refusal it makes; it is public so the
 * bookkeeping can be tested without a socket.
 */
void
clawt_egress_gate_record_refusal(
	ClawtEgressGate *self,
	const gchar     *agent_id,
	const gchar     *host,
	guint16          port,
	const gchar     *address,
	const gchar     *reason
);

/**
 * clawt_egress_gate_refused_today:
 * @self: a #ClawtEgressGate
 * @agent_id: an agent
 *
 * How many connections @agent_id has had refused since local midnight.
 *
 * Returns: the count
 */
guint
clawt_egress_gate_refused_today(
	ClawtEgressGate *self,
	const gchar     *agent_id
);

/**
 * clawt_egress_gate_recent_refusals:
 * @self: a #ClawtEgressGate
 * @agent_id: an agent
 *
 * The last few refusals for @agent_id, newest first.
 *
 * Returns: (transfer full) (element-type ClawtEgressRefusal): the
 *   refusals, possibly none
 */
GPtrArray *
clawt_egress_gate_recent_refusals(
	ClawtEgressGate *self,
	const gchar     *agent_id
);

/* ── What the clients say ─────────────────────────────────────── */

/**
 * clawt_egress_network_label:
 *
 * What the clients call `runtime.network`.
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_egress_network_label(void);

/**
 * clawt_egress_network_choice_label:
 * @nick: a #ClawtRuntimeNetwork nickname
 *
 * The name of one choice, as all three clients draw it.
 *
 * Returns: (transfer none): the label; @nick itself when unknown
 */
const gchar *
clawt_egress_network_choice_label(
	const gchar *nick
);

/**
 * clawt_egress_network_choice_explanation:
 * @nick: a #ClawtRuntimeNetwork nickname
 *
 * One line under a choice saying what it does.
 *
 * Returns: (transfer none) (nullable): the explanation
 */
const gchar *
clawt_egress_network_choice_explanation(
	const gchar *nick
);

/**
 * clawt_egress_network_notice:
 * @network: (nullable): the agent's `runtime.network` nickname
 * @confine: (nullable): the agent's `runtime.confine` nickname
 *
 * Why the network choice cannot be changed or cannot be given, or %NULL.
 *
 * Without the sandbox a process can ignore every proxy variable it is
 * handed, so anything but open needs `runtime.confine: bwrap`.  Worded
 * here because it is the refusal agent.set and the validator make, and a
 * client that worded its own would drift from it.
 *
 * Returns: (transfer none) (nullable): the notice
 */
const gchar *
clawt_egress_network_notice(
	const gchar *network,
	const gchar *confine
);

/**
 * clawt_egress_status_line:
 * @network: (nullable): the agent's `runtime.network` nickname
 * @refused_today: refusals since local midnight
 *
 * The one-line summary an agent view shows: "egress: gated (3 refused
 * today)", "egress: open".
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_egress_status_line(
	const gchar *network,
	guint        refused_today
);

/**
 * clawt_egress_refusal_line:
 * @host: (nullable): what was asked for
 * @port: the port
 * @address: (nullable): the refused address
 * @reason: (nullable): why
 *
 * One refusal as a line of text.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_egress_refusal_line(
	const gchar *host,
	guint        port,
	const gchar *address,
	const gchar *reason
);

/**
 * clawt_egress_url_wait_refusal:
 * @network: (nullable): the agent's `runtime.network` nickname
 *
 * Why a URL wait (`clawtilla_wait_until` with kind url) is refused for an
 * agent with this network, or %NULL when it may have one.
 *
 * The daemon makes a URL wait's request itself, from the machine's own
 * network position, so for a gated or closed agent it would be a fetch on
 * the agent's behalf around the gate -- the one route the gate exists to
 * be.  Refused for both, where the wait is set and again at each check.
 *
 * Returns: (transfer none) (nullable): the refusal
 */
const gchar *
clawt_egress_url_wait_refusal(
	const gchar *network
);

/**
 * clawt_egress_allowlist_label:
 *
 * Returns: (transfer none): what the clients call `egress.allow`
 */
const gchar *
clawt_egress_allowlist_label(void);

/**
 * clawt_egress_allowlist_explanation:
 *
 * Returns: (transfer none): the sentence under it
 */
const gchar *
clawt_egress_allowlist_explanation(void);

G_END_DECLS
