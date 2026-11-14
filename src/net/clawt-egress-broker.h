/*
 * clawt-egress-broker.h - Provider keys that never enter the sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A gated agent can still send whatever it holds to any public host --
 * that is what the gate lets through -- so an API key in its environment
 * is a key it can give away.  The broker keeps the key in the daemon.
 *
 * For a provider whose CLI lets its endpoint be moved (ANTHROPIC_BASE_URL
 * for Claude Code, OPENAI_BASE_URL for Codex), the agent's environment
 * gets a random token, minted for that one process, in place of the key,
 * and a base URL pointing at 127.0.0.1:3129 inside its box -- which
 * clawtilla-relay bridges to the broker's unix socket.  The broker checks
 * the token, drops every credential header the client sent, adds the real
 * one, and forwards to an upstream the agent cannot choose.  The request
 * is built with clawt_http_message_new(..., CLAWT_HTTP_CARRIES_CREDENTIALS),
 * so the real key never follows a redirect anywhere.
 *
 * A key whose CLI cannot be pointed elsewhere is passed through as before
 * and reported as unbrokered, with the reason, rather than silently
 * treated as protected.  An OAuth login (a Claude subscription) has no
 * key in the environment and is not touched.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * ClawtEgressKey:
 * @name: the environment variable
 * @reason: (nullable): why it is not brokered, or %NULL when it is
 *
 * One credential-looking variable in an agent's environment, and whether
 * the broker holds it.
 */
typedef struct {
	gchar *name;
	gchar *reason;
} ClawtEgressKey;

/**
 * ClawtEgressGrant:
 * @name: the path segment the agent's base URL ends in ("anthropic")
 * @upstream: where requests go, fixed: the agent cannot choose it
 * @headers: (element-type utf8 utf8): the real credential headers the
 *   broker adds, by lower-case name
 *
 * One upstream a token may reach, and the key it reaches it with.
 */
typedef struct {
	gchar      *name;
	gchar      *upstream;
	GHashTable *headers;
} ClawtEgressGrant;

/**
 * clawt_egress_grant_free:
 * @grant: (nullable): a grant
 */
void
clawt_egress_grant_free(
	ClawtEgressGrant *grant
);

/**
 * ClawtEgressBrokerPlan:
 * @environment: (element-type utf8 utf8): the agent's environment with
 *   every brokered key replaced by the token and its base URL pointed at
 *   the broker
 * @keys: (element-type ClawtEgressKey): every credential-looking variable,
 *   brokered or not
 * @grants: (element-type ClawtEgressGrant): what the token may reach
 *
 * What brokering does to one agent's environment.
 */
typedef struct {
	GHashTable *environment;
	GPtrArray  *keys;
	GPtrArray  *grants;
} ClawtEgressBrokerPlan;

/**
 * clawt_egress_broker_plan_free:
 * @plan: (nullable): a plan
 */
void
clawt_egress_broker_plan_free(
	ClawtEgressBrokerPlan *plan
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEgressBrokerPlan,
                              clawt_egress_broker_plan_free)

/**
 * clawt_egress_broker_plan: (skip)
 * @environment: (element-type utf8 utf8) (nullable): the agent's own
 *   variables and resolved credentials, by name
 * @provider: (nullable): the agent's `model.provider`
 * @network: the agent's `runtime.network`
 * @token: (nullable): the token for this process; %NULL plans by name
 *   only, for saying what would be brokered
 *
 * Works out which keys can be brokered and rewrites the environment.
 *
 * Pure: no socket, no file, so a test can assert on the negative -- the
 * real key appears nowhere in the rewritten environment.  Nothing is
 * brokered on an open network: the box's own route reaches the world,
 * and a key that went through the broker there would protect nothing.
 *
 * Returns: (transfer full): the plan
 */
ClawtEgressBrokerPlan *
clawt_egress_broker_plan(
	GHashTable          *environment,
	const gchar         *provider,
	ClawtRuntimeNetwork  network,
	const gchar         *token
);

/**
 * clawt_egress_broker_plan_is_brokered:
 * @plan: a plan
 * @name: an environment variable
 *
 * Returns: %TRUE if @plan replaces @name with the token
 */
gboolean
clawt_egress_broker_plan_is_brokered(
	ClawtEgressBrokerPlan *plan,
	const gchar           *name
);

/**
 * clawt_egress_broker_mint_token:
 * @error: (out) (optional): return location for a #GError
 *
 * A fresh token, from /dev/urandom.
 *
 * Returns: (transfer full) (nullable): the token
 */
gchar *
clawt_egress_broker_mint_token(
	GError **error
);

/**
 * clawt_egress_broker_copy_headers:
 * @from: the headers the agent sent
 * @to: the headers of the request that goes upstream
 * @credentials: (element-type utf8 utf8): the real credential headers
 *
 * Copies @from into @to without the hop-by-hop headers, Host, and every
 * header that can carry a credential (Authorization, X-Api-Key,
 * X-Goog-Api-Key, Proxy-Authorization, Cookie), then adds @credentials.
 * The token the agent sent is therefore nowhere in @to.
 */
void
clawt_egress_broker_copy_headers(
	SoupMessageHeaders *from,
	SoupMessageHeaders *to,
	GHashTable         *credentials
);

#define CLAWT_TYPE_EGRESS_BROKER (clawt_egress_broker_get_type())

G_DECLARE_FINAL_TYPE(ClawtEgressBroker, clawt_egress_broker, CLAWT,
                     EGRESS_BROKER, GObject)

/**
 * clawt_egress_broker_new:
 * @context: (nullable): the main context the broker runs on; %NULL for
 *   the thread-default at the time of the call
 *
 * Returns: (transfer full): a new broker, not yet listening
 */
ClawtEgressBroker *
clawt_egress_broker_new(
	GMainContext *context
);

/**
 * clawt_egress_broker_listen:
 * @self: a #ClawtEgressBroker
 * @socket_path: the unix socket to serve on
 * @error: (out) (optional): return location for a #GError
 *
 * Serves on @socket_path, mode 0600.  Waits on nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_egress_broker_listen(
	ClawtEgressBroker  *self,
	const gchar        *socket_path,
	GError            **error
);

/**
 * clawt_egress_broker_get_socket_path:
 * @self: a #ClawtEgressBroker
 *
 * Returns: (transfer none) (nullable): the socket, once listening
 */
const gchar *
clawt_egress_broker_get_socket_path(
	ClawtEgressBroker *self
);

/**
 * clawt_egress_broker_register:
 * @self: a #ClawtEgressBroker
 * @agent_id: the agent the token is for
 * @token: the token, as it appears in the agent's environment
 * @grants: (element-type ClawtEgressGrant): what it may reach
 *
 * Accepts @token for @agent_id from now on, and stops accepting any
 * earlier one for the same agent -- a token lives as long as the process
 * it was minted for.  Only a hash of the token is kept.
 */
void
clawt_egress_broker_register(
	ClawtEgressBroker *self,
	const gchar       *agent_id,
	const gchar       *token,
	GPtrArray         *grants
);

/**
 * clawt_egress_broker_revoke:
 * @self: a #ClawtEgressBroker
 * @agent_id: an agent
 *
 * Stops accepting @agent_id's token.
 */
void
clawt_egress_broker_revoke(
	ClawtEgressBroker *self,
	const gchar       *agent_id
);

/**
 * clawt_egress_broker_authorize:
 * @self: a #ClawtEgressBroker
 * @path: the request path, `/<grant>/...`
 * @query: (nullable): the query string, without the `?`
 * @headers: the request's headers, where the token is
 * @out_url: (out) (transfer full): the upstream URL
 * @out_credentials: (out) (transfer full) (element-type utf8 utf8): the
 *   real headers to add
 * @out_agent_id: (out) (optional) (transfer full): whose token it was
 * @out_status: (out) (optional): the HTTP status to refuse with
 * @error: (out) (optional): return location for a #GError
 *
 * The token swap, without the network: finds the token in `X-Api-Key` or
 * `Authorization: Bearer`, checks it against the registered ones and the
 * grant the path names, and says where the request goes and with what.
 *
 * Returns: %TRUE when the request may be forwarded
 */
gboolean
clawt_egress_broker_authorize(
	ClawtEgressBroker   *self,
	const gchar         *path,
	const gchar         *query,
	SoupMessageHeaders  *headers,
	gchar              **out_url,
	GHashTable         **out_credentials,
	gchar              **out_agent_id,
	guint               *out_status,
	GError             **error
);

/**
 * clawt_egress_broker_stop:
 * @self: a #ClawtEgressBroker
 *
 * Stops serving and cancels every request in flight.
 */
void
clawt_egress_broker_stop(
	ClawtEgressBroker *self
);

G_END_DECLS
