/*
 * clawt-connector-accounts.h - Several accounts on one service
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two GitHub connectors -- work and personal -- granted to one agent
 * give it two copies of every GitHub tool.  Each connector is its own
 * relay and its own `.mcp.json` entry, so the agent's CLI keeps them
 * apart by server name; but the tool *names* inside are identical, and
 * that is what a model reads, what an action rule matches and what a
 * decision card names.  An agent asked to "open an issue" picks
 * whichever `create_issue` it happens to see first, which is the shape
 * "a test with one key cannot see a shared-scalar bug" warns about.
 *
 * So when an agent holds more than one account on the same service, the
 * relay renames each account's tools with that account's label --
 * `work_create_issue`, `personal_create_issue` -- and refuses a call to
 * the bare name instead of guessing.  The label is the connector's
 * `account`, which must be unique per service; two labels that still
 * fold to the same prefix make every call on both refused, never routed
 * to whichever was registered first.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "integration/clawt-integration.h"

G_BEGIN_DECLS

/**
 * clawt_connector_account_slug:
 * @name: the connector's integration name
 * @account: (nullable): its `account` label
 *
 * The prefix an account's tools carry: @account, or @name when it has no
 * label, folded to lowercase letters, digits and underscores.
 *
 * Folded because it becomes part of a tool name, which MCP clients
 * restrict; compared folded because "Work" and "work" are the same word
 * to a person and two different prefixes to nothing.
 *
 * Returns: (transfer full): the slug, never empty
 */
gchar *
clawt_connector_account_slug(
	const gchar *name,
	const gchar *account
);

/**
 * clawt_connector_account_prefix:
 * @bindings: (element-type ClawtIntegrationBinding): everything one
 *   agent is bound to
 * @binding: one connector among them
 * @out_siblings: (out) (transfer full) (optional) (nullable)
 *   (array zero-terminated=1): every slug on the same service, this one
 *   included, when there is more than one
 * @error: (out) (optional): return location for a #GError
 *
 * Whether @binding's tools need an account prefix, and what it is.
 *
 * %NULL with no error means the agent holds one account on this service
 * and nothing is renamed.  %NULL with @error set means two of its
 * accounts fold to the same prefix -- the relay refuses every call on
 * the service rather than route one account's call to the other.
 *
 * "The same service" is the same `provider`, whatever the instance: two
 * GitLabs are two servers with identical tool names.
 *
 * Returns: (transfer full) (nullable): the slug, or %NULL
 */
gchar *
clawt_connector_account_prefix(
	GPtrArray                *bindings,
	ClawtIntegrationBinding  *binding,
	GStrv                    *out_siblings,
	GError                  **error
);

/**
 * clawt_connector_check_new_account:
 * @integrations: (element-type ClawtIntegrationConfig): every integration
 *   instance in the fleet, the new one included
 * @candidate: the connector being added
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a second account on a service that has no label, or a label
 * another account on that service already folds to.
 *
 * Asked when an account is *added*, never when a configuration loads: a
 * file written before this rule must keep working, and does, because an
 * unlabelled account's prefix falls back to its integration name.
 *
 * Returns: %TRUE if @candidate may be added
 */
gboolean
clawt_connector_check_new_account(
	GPtrArray               *integrations,
	ClawtIntegrationConfig  *candidate,
	GError                 **error
);

/**
 * clawt_connector_accounts_inbound:
 * @line: one JSON-RPC message from the tool server
 * @prefix: the account's slug
 *
 * Renames the tools in a `tools/list` result to `<prefix>_<tool>`, and
 * says in each description which account it acts as.
 *
 * Returns: (transfer full): the message to pass on, which is @line
 *   unchanged when it is not a tool list
 */
gchar *
clawt_connector_accounts_inbound(
	const gchar *line,
	const gchar *prefix
);

/**
 * clawt_connector_accounts_outbound:
 * @line: one JSON-RPC message from the agent's MCP client
 * @prefix: (nullable): the account's slug, or %NULL when two accounts
 *   collide and every call must be refused
 * @siblings: (array zero-terminated=1) (nullable): every slug on the
 *   service, for the refusal to name
 * @conflict: (nullable): why the accounts collide, when @prefix is %NULL
 * @refusal: (out) (transfer full) (optional) (nullable): a reply to send
 *   the agent instead, when the call is refused and has an id
 *
 * Strips @prefix from a `tools/call`, or refuses it.
 *
 * A call to the bare name is refused rather than forwarded, because a
 * forwarded one would run as this account when the agent may have meant
 * the other -- the guess this exists to take away.
 *
 * Returns: (transfer full) (nullable): the message to forward, or %NULL
 *   when it is refused
 */
gchar *
clawt_connector_accounts_outbound(
	const gchar  *line,
	const gchar  *prefix,
	GStrv         siblings,
	const gchar  *conflict,
	gchar       **refusal
);

G_END_DECLS
