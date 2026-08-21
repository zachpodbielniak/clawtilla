/*
 * clawt-integration.h - How an agent reaches the world
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
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
 * @id: the `integrations.<id>` key
 * @summary: one line about what it does
 * @required_keys: (array zero-terminated=1) (nullable): keys that must be
 *   set when the integration is enabled
 * @credential_keys: (array zero-terminated=1) (nullable): keys holding
 *   secret references
 * @channel: the libreclaw channel it renders to
 *
 * What clawtilla knows about one kind of integration.
 *
 * A table rather than a class hierarchy: every built-in integration is
 * declarative -- validate some keys, resolve some secrets, emit a channel
 * block -- and a class per integration would be five files that each say
 * the same thing differently.  Anything that genuinely needs behaviour
 * arrives as a #ClawtIntegrationProvider plugin instead.
 */
typedef struct {
    const gchar        *id;
    const gchar        *summary;
    const gchar *const *required_keys;
    const gchar *const *credential_keys;
    const gchar        *channel;
} ClawtIntegrationInfo;

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
 * @id: an integration id
 *
 * Returns: (transfer none) (nullable): what clawtilla knows about it
 */
const ClawtIntegrationInfo *clawt_integration_find(const gchar *id);

/**
 * clawt_integration_is_enabled:
 * @agent: an agent's configuration
 * @id: an integration id
 *
 * Returns: %TRUE if this agent has the integration turned on
 */
gboolean clawt_integration_is_enabled(ClawtAgentConfig *agent,
                                      const gchar      *id);

/**
 * clawt_integration_validate:
 * @agent: an agent's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Checks every integration this agent has enabled.
 *
 * Reported as one error naming the first missing key rather than a
 * warning at start time, because an agent whose Matrix block is missing
 * its homeserver does not fail loudly -- it simply never receives
 * anything, which is much harder to notice.
 *
 * Returns: %TRUE if the agent's integrations are usable
 */
gboolean clawt_integration_validate(ClawtAgentConfig  *agent,
                                    GError           **error);

/**
 * clawt_integration_enabled_for:
 * @agent: an agent's configuration
 *
 * Returns: (transfer full) (array zero-terminated=1): the ids this agent
 *   has enabled
 */
GStrv clawt_integration_enabled_for(ClawtAgentConfig *agent);

/**
 * clawt_integration_health_check:
 * @agent: an agent's configuration
 * @id: which integration
 * @timeout_seconds: how long to wait
 * @error: (out) (optional): return location for a #GError
 *
 * Tries to reach whatever the integration talks to.
 *
 * Deliberately a connectivity check rather than a login: a check that
 * needed credentials could not run before they are resolved, and the
 * failure people actually hit is a typo in a hostname or a firewall.
 *
 * Returns: %TRUE if it answered
 */
gboolean clawt_integration_health_check(ClawtAgentConfig  *agent,
                                        const gchar       *id,
                                        guint              timeout_seconds,
                                        GError           **error);

G_END_DECLS
