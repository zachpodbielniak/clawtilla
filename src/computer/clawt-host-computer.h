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
void clawt_host_computer_set_nice(ClawtHostComputer *self,
                                  gint               nice_level);

G_END_DECLS
