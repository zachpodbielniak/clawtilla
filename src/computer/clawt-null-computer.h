/*
 * clawt-null-computer.h - An agent with no computer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The default, and a real choice rather than an absence.  An agent that can
 * only talk is a smaller grant than one that can run commands, and plenty
 * of useful agents never need to.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_NULL_COMPUTER (clawt_null_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtNullComputer, clawt_null_computer,
                     CLAWT, NULL_COMPUTER, ClawtComputer)

/**
 * clawt_null_computer_new:
 * @agent_id: whose computer this is
 *
 * A computer that is not one: every operation refuses, politely and with
 * a reason the agent can act on.
 *
 * It exists so the rest of clawtilla never has to check for %NULL before
 * touching an agent's computer, and so an agent that tries anyway is
 * told "you have no computer" rather than crashing something.
 *
 * Returns: (transfer full): a new #ClawtComputer
 */
ClawtComputer *clawt_null_computer_new(const gchar *agent_id);

G_END_DECLS
