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
#include "mcp/clawt-mcp-relay.h"

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
 * @name: the connector's integration name, for what the agent is told
 * @accounts: whether the agent holds several accounts on this service,
 *   so this one's tools are renamed for it
 * @account_prefix: (nullable): the account's slug, or %NULL when
 *   @accounts is set and two labels collide -- every call is refused
 * @account_siblings: (array zero-terminated=1) (nullable): every slug on
 *   the service, for a refusal to name
 * @account_conflict: (nullable): why the labels collide
 * @unauthorized: (nullable): told when a remote server answers 401, so
 *   the daemon can offer a sign-in
 * @unauthorized_data: data for @unauthorized
 * @gateway: (nullable): the integration broker answering for this
 *   connector -- "AnythingMCP", "Jentic One" -- so a refusal it sends is
 *   named as its, never as clawtilla's
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
 * Exactly one of @argv and @url is set.  A @url plan may carry no
 * credential at all (@header_value %NULL): a remote server that has not
 * been signed in to yet still lists its tools to some clients, and its
 * 401 is what tells the operator a sign-in is wanted.
 *
 * The account fields are filled by the caller, which is the one that
 * knows every connector the agent holds;
 * clawt_connector_account_prefix() decides them.
 *
 * Deliberately not introspectable.  Its entire purpose is to hold a live
 * credential in this process's memory for as long as a server is
 * running, and handing that to a language binding -- where its lifetime
 * would belong to somebody else's garbage collector -- is the opposite
 * of what the rest of this file is for.
 */
/**
 * ClawtConnectorUnauthorized: (skip)
 * @name: the connector
 * @challenge: (nullable): the 401's `WWW-Authenticate` header
 * @sent_credential: whether the refused request carried a credential
 * @user_data: the plan's @unauthorized_data
 *
 * Told when a remote tool server refuses a request with a 401.
 */
typedef void (*ClawtConnectorUnauthorized)(const gchar *name,
                                           const gchar *challenge,
                                           gboolean     sent_credential,
                                           gpointer     user_data);

typedef struct {
    GStrv   argv;
    gchar  *url;
    GStrv   envp;
    gchar  *header_name;
    gchar  *header_value;
    GStrv   permitted;

    gchar   *name;
    gboolean accounts;
    gchar   *account_prefix;
    GStrv    account_siblings;
    gchar   *account_conflict;

    ClawtConnectorUnauthorized unauthorized;
    gpointer                   unauthorized_data;

    gchar   *gateway;
} ClawtConnectorPlan;

/**
 * clawt_connector_resolve_command:
 * @command: a bare name or path, from a connector's `server_command`
 * @error: (out) (optional): return location for a #GError
 *
 * Where a connector's own tool server actually is, checked in the same
 * order and reported the same way `sibling_binary_path()` resolves
 * clawtilla's own CLI: beside the running binary, then the install
 * location, then `PATH`.
 *
 * An already-absolute or explicitly relative @command (one containing a
 * directory separator) is trusted as written and only checked for
 * existing.  A bare name that resolves nowhere is refused naming all
 * three places that were tried, rather than left to fail later as a
 * bare "No such file or directory" from the subprocess that tried to
 * start it -- which points at nothing an operator can act on.
 *
 * Returns: (transfer full) (nullable): an executable path, or %NULL if
 *   @command is nowhere to be found
 */
gchar *clawt_connector_resolve_command(const gchar *command, GError **error);

/**
 * clawt_connector_plan_new: (skip)
 * @info: the connector from the catalogue
 * @binding: the integration as this agent has it
 * @credential: (nullable): the secret, already loaded from its file;
 *   %NULL only for a server reached by URL that nobody has signed in to
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

/**
 * clawt_connector_relay_run_gated: (skip)
 * @plan: what to run
 * @gate: (scope call) (nullable): asked once per tool call -- the agent's
 *   action rules, from the CLI
 * @gate_data: data for @gate
 *
 * As clawt_connector_relay_run(), asking @gate about every call, over
 * stdio and over HTTP alike.
 *
 * Returns: the exit status for the process
 */
gint clawt_connector_relay_run_gated(ClawtConnectorPlan *plan,
                                     ClawtMcpRelayGate   gate,
                                     gpointer            gate_data);

/**
 * clawt_connector_relay_run_full: (skip)
 * @plan: what to run
 * @gate: (scope call) (nullable): asked once per tool call, naming the
 *   tool that would run -- never the directory in front of it
 * @gate_data: data for @gate
 * @directory: (nullable): counts the server's tools, and presents a big
 *   one as three tools that search, describe and call the rest
 *
 * As clawt_connector_relay_run_gated(), with the directory in the path.
 *
 * Returns: the exit status for the process
 */
gint clawt_connector_relay_run_full(ClawtConnectorPlan *plan,
                                    ClawtMcpRelayGate   gate,
                                    gpointer            gate_data,
                                    ClawtMcpDirectory  *directory);

/**
 * clawt_connector_directory_new:
 * @config: the fleet's configuration
 * @binding: the connector, as this agent has it
 * @agent_id: (nullable): the agent the relay serves
 *
 * The directory for one connector relay, from the connector's
 * `directory`, the fleet's `connectors.directory_min_tools` and
 * `connectors.directory_min_chars`, and whether the agent's CLI searches
 * tools by itself (clawt_mcp_directory_cli_searches_tools()).
 *
 * Always returns one, even for `never`: a directory that may not present
 * itself still counts what goes past, which is how the Connectors page
 * learns a server's size.
 *
 * Returns: (transfer full): the directory
 */
ClawtMcpDirectory *clawt_connector_directory_new(ClawtConfig             *config,
                                                 ClawtIntegrationBinding *binding,
                                                 const gchar             *agent_id);

G_END_DECLS
