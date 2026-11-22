/*
 * clawt-ephemeral-name.h - Names for ephemeral agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An ephemeral agent is a helper that lives for one piece of work.  It
 * still needs a name the fleet can address and a person can tell apart
 * from the last five, so it gets one the way a container does -- an
 * adjective and a noun -- only louder: `turbo-narwhal`, `feral-kazoo`,
 * `quantum-pickle`.  A name is an address, not a secret, so the
 * randomness is GLib's.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * ClawtNameTakenFunc:
 * @name: a candidate
 * @user_data: as given
 *
 * Returns: %TRUE when @name is already in use
 */
typedef gboolean (*ClawtNameTakenFunc)(const gchar *name, gpointer user_data);

/**
 * clawt_ephemeral_name_generate:
 * @rand: (nullable): the random source, or %NULL for GLib's global one;
 *   a test passes a seeded #GRand to get the same names every run
 * @taken: (nullable) (scope call): says whether a candidate is in use
 * @user_data: passed to @taken
 *
 * An unused `adjective-noun` id.  After enough collisions to suggest the
 * fleet has used most of the pairs, a two-digit suffix is added rather
 * than looping for ever.
 *
 * Returns: (transfer full): the name, always a valid agent id
 */
gchar *clawt_ephemeral_name_generate(GRand              *rand,
                                     ClawtNameTakenFunc  taken,
                                     gpointer            user_data);

/**
 * clawt_ephemeral_name_pair_count:
 *
 * Returns: how many distinct `adjective-noun` names there are
 */
guint clawt_ephemeral_name_pair_count(void);

G_END_DECLS
