/*
 * clawt-host-computer.h - The real machine clawtilla runs on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The most useful backend and the most dangerous: the agent's commands run
 * as you, on your files, with your network.  Everything it may touch is
 * decided by #ClawtSandbox, and what each confinement mode actually
 * prevents is spelled out there and in docs/security.org.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-sandbox.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_HOST_COMPUTER (clawt_host_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtHostComputer, clawt_host_computer,
                     CLAWT, HOST_COMPUTER, ClawtComputer)

/**
 * clawt_host_computer_new:
 * @agent_id: the agent this belongs to
 * @sandbox: (transfer none): what it may touch
 *
 * Returns: (transfer full): a new #ClawtHostComputer
 */
ClawtComputer *clawt_host_computer_new(const gchar  *agent_id,
                                       ClawtSandbox *sandbox);

/**
 * clawt_host_computer_get_sandbox:
 * @self: a #ClawtHostComputer
 *
 * Returns: (transfer none): the confinement in force
 */
ClawtSandbox *clawt_host_computer_get_sandbox(ClawtHostComputer *self);

/**
 * clawt_host_computer_set_nice:
 * @self: a #ClawtHostComputer
 * @nice_level: scheduling niceness for the agent's commands
 *
 * A runaway build started by an agent should not make the desktop
 * unusable.
 */
/**
 * clawt_host_computer_set_environment:
 * @self: a #ClawtHostComputer
 * @env: (element-type utf8 utf8) (nullable): variables for the command
 *
 * Sets what a command run on this computer gets beyond the allowlist.
 *
 * The daemon's own environment is never inherited: a command an agent
 * runs would otherwise see every secret resolved for every other agent,
 * and the operator's SSH_AUTH_SOCK with them.
 */
void clawt_host_computer_set_environment(ClawtHostComputer *self,
                                         GHashTable        *env);

void clawt_host_computer_set_nice(ClawtHostComputer *self,
                                  gint               nice_level);

/**
 * clawt_host_computer_set_desktop:
 * @self: a #ClawtHostComputer
 * @desktop: (nullable) (transfer none): the desktop this agent was
 *   granted, or %NULL for none
 *
 * Gives the computer the screen it can be watched through.
 *
 * A host agent's desktop is configured beside its computer rather than
 * inside it, and until this existed nothing joined the two -- so the
 * computer had no way to answer #ClawtObservable and a Screen tab on a
 * host agent would have been permanently empty. Set by the factory,
 * which is the one place that reads both halves of the config.
 */
void clawt_host_computer_set_desktop(ClawtHostComputer *self,
                                     ClawtDesktop      *desktop);

G_END_DECLS
