/*
 * clawt-connector-relay.h - Using a credential without holding it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The reason the broker is worth building at all.
 *
 * An `mcp` integration's environment is resolved into the agent's
 * `.mcp.json`, which is fine for a key somebody chose to give it and not
 * fine for an OAuth grant on their Google account: the agent can read
 * that file, and anything it reads can end up quoted into a message or
 * an exported transcript.  So a connector's `.mcp.json` entry names
 * `clawtilla connector relay <name>` and carries no secret at all.  The
 * relay reads the credential from a 0600 file, starts the real server
 * with it, and copies MCP back and forth in between.
 *
 * What that is worth depends on where the agent runs, and it is worth
 * saying plainly rather than overstating.  For a container or VM agent
 * the boundary is real: the token file and the relay are on the host and
 * the agent cannot reach either.  For an unconfined host agent it
 * prevents the credential being leaked by accident -- which is the
 * failure that actually happens -- but not one that goes looking, since
 * it runs as the same user.  Confinement is what makes it airtight;
 * this makes it airtight *there* and much harder to spill everywhere
 * else.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "integration/clawt-connector.h"
#include "integration/clawt-integration.h"

G_BEGIN_DECLS

/**
 * ClawtConnectorPlan: (skip)
 * @argv: (array zero-terminated=1) (nullable): a stdio server to start
 * @url: (nullable): an HTTP server to forward to
 * @envp: (array zero-terminated=1) (nullable): what to add to the
 *   server's environment, which is where the credential goes
 * @header_name: (nullable): the header the credential goes in instead
 * @header_value: (nullable): its value, already formatted
 * @permitted: (array zero-terminated=1) (nullable): the only tools the
 *   agent may use, or %NULL for all of them
 *
 * Everything needed to reach one connector's tools, worked out before
 * anything is started.
 *
 * Separated from the running so it can be asserted on without a token, a
 * provider or a subprocess -- and the assertion that matters is a
 * negative one: that the credential appears in @envp or @header_value
 * and nowhere in @argv.  An argument vector is visible in the process
 * table to every process on the machine, so a credential that reached it
 * would be readable by exactly the thing this file exists to keep it
 * from.  That is not visible by reading the code that builds the argv;
 * it is visible by looking for the value in the result.
 *
 * Exactly one of @argv and @url is set.
 *
 * Deliberately not introspectable.  Its entire purpose is to hold a live
 * credential in this process's memory for as long as a server is
 * running, and handing that to a language binding -- where its lifetime
 * would belong to somebody else's garbage collector -- is the opposite
 * of what the rest of this file is for.
 */
typedef struct {
    GStrv   argv;
    gchar  *url;
    GStrv   envp;
    gchar  *header_name;
    gchar  *header_value;
    GStrv   permitted;
} ClawtConnectorPlan;

/**
 * clawt_connector_plan_new: (skip)
 * @info: the connector from the catalogue
 * @binding: the integration as this agent has it
 * @credential: the secret, already loaded from its file
 * @error: (out) (optional): return location for a #GError
 *
 * Works out how to reach @info's tools with @credential.
 *
 * The server comes from the integration when it names one and from the
 * catalogue otherwise, so a connector for a service with a well-known
 * server is a provider and a client id and nothing else.
 *
 * Returns: (transfer full) (nullable): the plan, or %NULL on error
 */
ClawtConnectorPlan *
clawt_connector_plan_new(const ClawtConnectorInfo *info,
                         ClawtIntegrationBinding  *binding,
                         const gchar              *credential,
                         GError                  **error);

/**
 * clawt_connector_plan_free: (skip)
 * @self: (transfer full) (nullable): a plan
 *
 * Frees a plan, wiping the credential wherever it was placed.
 */
void clawt_connector_plan_free(ClawtConnectorPlan *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtConnectorPlan, clawt_connector_plan_free)

/**
 * clawt_connector_relay_run: (skip)
 * @plan: what to reach and how
 *
 * Relays MCP between this process's stdio and @plan's server until
 * either end closes.
 *
 * Returns: the exit status for the process
 */
gint clawt_connector_relay_run(ClawtConnectorPlan *plan);

G_END_DECLS
