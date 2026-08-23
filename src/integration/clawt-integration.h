/*
 * clawt-integration.h - How an agent reaches the world
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An integration is configured in one of two places and behaves the same
 * either way:
 *
 *   - inside an agent, under `integrations:`, for something only that
 *     agent will ever use;
 *   - at the top level, as a named instance with a scope, for something
 *     handed to several agents or to the whole fleet.
 *
 * A #ClawtIntegrationBinding is one integration as one agent actually has
 * it, whichever of those it came from.  Everything downstream -- the
 * rendered libreclaw config, the agent's .mcp.json, the paragraph written
 * into its TOOLS.org, the health check -- reads bindings and never looks
 * at where the values were written, so a global instance and an inline
 * block cannot drift into behaving differently.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * ClawtIntegrationInfo:
 * @id: the `type:` value, and the `integrations.<id>` key inline
 * @kind: which direction it runs in
 * @summary: one line about what it does
 * @required_keys: (array zero-terminated=1) (nullable): keys that must be
 *   set when the integration is enabled
 * @credential_keys: (array zero-terminated=1) (nullable): keys holding
 *   secret references
 * @identity_keys: (array zero-terminated=1) (nullable): keys that must
 *   differ between two agents sharing one instance
 * @channel: (nullable): the libreclaw channel it renders to
 * @one_per_agent: %TRUE if an agent may have at most one of these
 * @one_per_fleet: %TRUE if at most one agent in the fleet may have it
 *
 * What clawtilla knows about one kind of integration.
 *
 * A table rather than a class hierarchy: every built-in integration is
 * declarative -- validate some keys, resolve some secrets, emit a channel
 * block or an MCP server entry -- and a class per integration would be
 * six files that each say the same thing differently.  Anything that
 * genuinely needs behaviour arrives as a #ClawtIntegrationProvider plugin
 * instead.
 *
 * @identity_keys is the field that makes sharing safe.  A Matrix account
 * is one login: two agents on the same `user_id` both receive every
 * message and both answer as the same person, which looks like a bug in
 * the fleet rather than a mistake in a config file.  Naming the keys that
 * must differ lets that be caught when the file is read.
 */
typedef struct {
    const gchar          *id;
    ClawtIntegrationKind  kind;
    const gchar          *summary;
    const gchar *const   *required_keys;
    const gchar *const   *credential_keys;
    const gchar *const   *identity_keys;
    const gchar          *channel;
    gboolean              one_per_agent;
    gboolean              one_per_fleet;
} ClawtIntegrationInfo;

/**
 * ClawtIntegrationBinding:
 *
 * One integration, as one agent has it.
 *
 * Holds no values of its own: it points at whichever configuration the
 * values live in and reads through to it, so a change to the config is
 * seen by a binding that was made before it.
 */
typedef struct _ClawtIntegrationBinding ClawtIntegrationBinding;

#define CLAWT_TYPE_INTEGRATION_BINDING (clawt_integration_binding_get_type())

GType clawt_integration_binding_get_type(void) G_GNUC_CONST;

ClawtIntegrationBinding *
clawt_integration_binding_ref(ClawtIntegrationBinding *self);

void clawt_integration_binding_unref(ClawtIntegrationBinding *self);

/**
 * clawt_integration_binding_for_instance:
 * @instance: a shared integration
 * @info: what clawtilla knows about its type
 * @agent_id: (nullable): whose values to read, or %NULL for the instance's own
 *
 * A binding onto one instance, without resolving a whole agent.
 *
 * Wanted where there is no agent to resolve for -- testing a notifier
 * that covers nobody yet, or reading an instance's settings in a dialog.
 *
 * Returns: (transfer full): the binding
 */
ClawtIntegrationBinding *
clawt_integration_binding_for_instance(ClawtIntegrationConfig     *instance,
                                       const ClawtIntegrationInfo *info,
                                       const gchar                *agent_id);

/**
 * clawt_integration_binding_get_name:
 * @self: a #ClawtIntegrationBinding
 *
 * What this integration is called for this agent.
 *
 * The instance name for a shared integration; the type id for an inline
 * one, since an inline block has no name of its own.
 *
 * Returns: (transfer none): the name
 */
const gchar *
clawt_integration_binding_get_name(ClawtIntegrationBinding *self);

/**
 * clawt_integration_binding_get_info:
 * @self: a #ClawtIntegrationBinding
 *
 * Returns: (transfer none): what clawtilla knows about this kind
 */
const ClawtIntegrationInfo *
clawt_integration_binding_get_info(ClawtIntegrationBinding *self);

/**
 * clawt_integration_binding_get_agent_id:
 * @self: a #ClawtIntegrationBinding
 *
 * Returns: (transfer none): the agent this binding is for
 */
const gchar *
clawt_integration_binding_get_agent_id(ClawtIntegrationBinding *self);

/**
 * clawt_integration_binding_is_shared:
 * @self: a #ClawtIntegrationBinding
 *
 * Whether it came from a named instance rather than the agent's own block.
 *
 * Returns: %TRUE for a shared instance
 */
gboolean
clawt_integration_binding_is_shared(ClawtIntegrationBinding *self);

const gchar *clawt_integration_binding_get_string(ClawtIntegrationBinding *self,
                                                  const gchar             *key);

gboolean clawt_integration_binding_get_boolean(ClawtIntegrationBinding *self,
                                               const gchar             *key);

gint64 clawt_integration_binding_get_int(ClawtIntegrationBinding *self,
                                         const gchar             *key);

/**
 * clawt_integration_binding_get_string_list:
 * @self: a #ClawtIntegrationBinding
 * @key: a key relative to the integration
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the values
 */
GStrv clawt_integration_binding_get_string_list(ClawtIntegrationBinding *self,
                                                const gchar             *key);

/**
 * clawt_integration_binding_get_mapping:
 * @self: a #ClawtIntegrationBinding
 * @key: a key relative to the integration
 *
 * Returns: (transfer full) (element-type utf8 utf8): the entries
 */
GHashTable *clawt_integration_binding_get_mapping(ClawtIntegrationBinding *self,
                                                  const gchar             *key);

/**
 * clawt_integration_binding_get_secret:
 * @self: a #ClawtIntegrationBinding
 * @key: a key relative to the integration
 *
 * Returns: (transfer full) (nullable): the reference
 */
ClawtSecretRef *
clawt_integration_binding_get_secret(ClawtIntegrationBinding *self,
                                     const gchar             *key);

/**
 * clawt_integration_binding_resolve_env:
 * @self: a #ClawtIntegrationBinding
 * @key: the mapping key, in practice "env"
 * @secrets_dir: (nullable): where a `{file: ...}` reference is relative to
 * @error: (out) (optional): return location for a #GError
 *
 * A mapping whose values may be literals or secret references, fetched.
 *
 * Returns: (transfer full) (nullable) (element-type utf8 utf8): the entries
 */
GHashTable *
clawt_integration_binding_resolve_env(ClawtIntegrationBinding  *self,
                                      const gchar              *key,
                                      const gchar              *secrets_dir,
                                      GError                  **error);

gboolean clawt_integration_binding_has_key(ClawtIntegrationBinding *self,
                                           const gchar             *key);

/**
 * clawt_integration_binding_validate:
 * @self: a #ClawtIntegrationBinding
 * @error: (out) (optional): return location for a #GError
 *
 * Checks that this integration has everything it needs for this agent.
 *
 * Returns: %TRUE if it is usable
 */
gboolean clawt_integration_binding_validate(ClawtIntegrationBinding  *self,
                                            GError                  **error);

/**
 * clawt_integration_list:
 * @n_integrations: (out): how many there are
 *
 * Returns: (array length=n_integrations) (transfer none): the built-in
 *   integrations
 */
const ClawtIntegrationInfo *clawt_integration_list(gsize *n_integrations);

/**
 * clawt_integration_find:
 * @id: an integration type id
 *
 * Returns: (transfer none) (nullable): what clawtilla knows about it
 */
const ClawtIntegrationInfo *clawt_integration_find(const gchar *id);

/**
 * clawt_integration_is_enabled:
 * @agent: an agent's configuration
 * @id: an integration type id
 *
 * Whether this agent has the integration turned on *in its own block*.
 *
 * Deliberately does not consider shared instances: it answers a question
 * about one place in the file.  Use clawt_integration_resolve_for_agent()
 * for what an agent actually has.
 *
 * Returns: %TRUE if this agent's own block has it on
 */
gboolean clawt_integration_is_enabled(ClawtAgentConfig *agent,
                                      const gchar      *id);

/**
 * clawt_integration_resolve_for_agent:
 * @config: the fleet configuration
 * @agent: an agent's configuration
 *
 * Everything this agent actually has: its own inline blocks, plus every
 * shared instance whose scope covers it.
 *
 * Ordered inline first, then instances in file order, and conflicts are
 * *dropped with a warning* rather than merged -- an agent cannot have two
 * Matrix channels, and picking one silently would leave an account that
 * looks configured and receives nothing.
 *
 * Returns: (transfer full) (element-type ClawtIntegrationBinding): the
 *   bindings
 */
GPtrArray *clawt_integration_resolve_for_agent(ClawtConfig      *config,
                                               ClawtAgentConfig *agent);

/**
 * clawt_integration_find_binding:
 * @bindings: (element-type ClawtIntegrationBinding): resolved bindings
 * @type_id: an integration type id
 *
 * Returns: (transfer none) (nullable): the first binding of that type
 */
ClawtIntegrationBinding *
clawt_integration_find_binding(GPtrArray *bindings, const gchar *type_id);

/**
 * clawt_integration_validate:
 * @agent: an agent's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Checks every integration in this agent's own block.
 *
 * Reported as one error naming the first missing key rather than a
 * warning at start time, because an agent whose Matrix block is missing
 * its homeserver does not fail loudly -- it simply never receives
 * anything, which is much harder to notice.
 *
 * Returns: %TRUE if the agent's own integrations are usable
 */
gboolean clawt_integration_validate(ClawtAgentConfig  *agent,
                                    GError           **error);

/**
 * clawt_integration_validate_fleet:
 * @config: the fleet configuration
 * @warnings: (out) (optional) (element-type utf8): everything wrong, in
 *   the order found
 *
 * Checks the shared instances against each other and against the fleet.
 *
 * The failures worth catching here are the ones that only exist between
 * two things: two agents sharing one Matrix login, two agents told to
 * bind the same webhook port, an instance naming an agent that is not
 * there.  None of them is visible from one agent alone.
 *
 * Returns: %TRUE if nothing was found
 */
gboolean clawt_integration_validate_fleet(ClawtConfig *config,
                                          GPtrArray  **warnings);

/**
 * clawt_integration_enabled_for:
 * @agent: an agent's configuration
 *
 * Returns: (transfer full) (array zero-terminated=1): the type ids this
 *   agent has enabled in its own block
 */
GStrv clawt_integration_enabled_for(ClawtAgentConfig *agent);

/**
 * clawt_integration_health_check_async:
 * @binding: a #ClawtIntegrationBinding
 * @timeout_seconds: how long to wait
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the check finishes
 * @user_data: data for @callback
 *
 * Tries to reach whatever the integration talks to.
 *
 * Asynchronous because every caller is an IPC handler, and a handler that
 * waits on the network stalls the daemon's main context for as long as
 * the far end takes -- with a ten-second timeout, that is ten seconds in
 * which no agent's message is routed and no client is answered.
 *
 * Deliberately a connectivity check rather than a login: a check that
 * needed credentials could not run before they are resolved, and the
 * failure people actually hit is a typo in a hostname or a firewall.
 */
void clawt_integration_health_check_async(ClawtIntegrationBinding *binding,
                                          guint                    timeout_seconds,
                                          GCancellable            *cancellable,
                                          GAsyncReadyCallback      callback,
                                          gpointer                 user_data);

/**
 * clawt_integration_health_check_finish:
 * @binding: a #ClawtIntegrationBinding
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if it answered
 */
gboolean clawt_integration_health_check_finish(ClawtIntegrationBinding  *binding,
                                               GAsyncResult             *result,
                                               GError                  **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtIntegrationBinding,
                              clawt_integration_binding_unref)

G_END_DECLS
