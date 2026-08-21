/*
 * clawt-computer-provider.h - Adding a kind of computer
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
#include "computer/clawt-computer.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_COMPUTER_PROVIDER (clawt_computer_provider_get_type())

G_DECLARE_INTERFACE(ClawtComputerProvider, clawt_computer_provider, CLAWT,
                    COMPUTER_PROVIDER, GObject)

/**
 * ClawtComputerProviderInterface:
 * @get_type_name: the `computer.type` value this provider answers to
 * @create: build a computer for an agent
 *
 * Implemented by a plugin that adds a computer backend -- firecracker, a
 * remote host over ssh, someone else's cloud.
 */
struct _ClawtComputerProviderInterface {
    GTypeInterface parent_iface;

    const gchar   *(*get_type_name) (ClawtComputerProvider *self);
    ClawtComputer *(*create)        (ClawtComputerProvider  *self,
                                     ClawtAgentConfig       *config,
                                     GError                **error);
};

const gchar *clawt_computer_provider_get_type_name(
    ClawtComputerProvider *self);

/**
 * clawt_computer_provider_create:
 * @self: a #ClawtComputerProvider
 * @config: the agent's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the computer, or %NULL
 */
ClawtComputer *clawt_computer_provider_create(ClawtComputerProvider  *self,
                                              ClawtAgentConfig       *config,
                                              GError                **error);

G_END_DECLS
