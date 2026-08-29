/*
 * clawt-operator-profile.h - What the fleet knows about the person it works for
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

#include "memory/clawt-memory-scopes.h"

G_BEGIN_DECLS

/**
 * CLAWT_OPERATOR_CATEGORY:
 *
 * The category fleet-scope memories about the operator are filed under.
 *
 * A named constant rather than the literal in five places, because the
 * profile is assembled by matching on it and a listing that spelled it
 * differently would find nothing and look like an empty profile.
 */
#define CLAWT_OPERATOR_CATEGORY "operator"

#define CLAWT_TYPE_OPERATOR_PROFILE (clawt_operator_profile_get_type())

G_DECLARE_FINAL_TYPE(ClawtOperatorProfile, clawt_operator_profile, CLAWT,
                     OPERATOR_PROFILE, GObject)

/**
 * clawt_operator_profile_new:
 * @state_dir: the daemon's state directory
 * @scopes: (nullable) (transfer none): the fleet's shared memory
 *   databases, or %NULL to use the file alone
 *
 * A model of the person the fleet works for.
 *
 * Two halves, on purpose.  The file at `<state_dir>/OPERATOR.org` is
 * what a person writes and edits; fleet-scope memories in the
 * %CLAWT_OPERATOR_CATEGORY category are what agents record as they learn
 * it.  Both are read into a new agent's `USER.org`, so an agent created
 * on day ninety starts knowing what the fleet already worked out instead
 * of learning it again -- and the person can read, correct or delete
 * every line of it, because a model of somebody that they cannot read is
 * not something to build.
 *
 * Returns: (transfer full): a new #ClawtOperatorProfile
 */
ClawtOperatorProfile *clawt_operator_profile_new(const gchar       *state_dir,
                                                 ClawtMemoryScopes *scopes);

/**
 * clawt_operator_profile_path:
 * @self: a #ClawtOperatorProfile
 *
 * Where the editable half lives.
 *
 * Returns: (transfer none): the path
 */
const gchar *clawt_operator_profile_path(ClawtOperatorProfile *self);

/**
 * clawt_operator_profile_read_text:
 * @self: a #ClawtOperatorProfile
 *
 * The file's contents, or %NULL when there is no file.
 *
 * A missing file is not created by a read: an empty profile and a
 * profile somebody deliberately emptied are different, and only one of
 * them should leave a file behind.
 *
 * Returns: (transfer full) (nullable): the text
 */
gchar *clawt_operator_profile_read_text(ClawtOperatorProfile *self);

/**
 * clawt_operator_profile_write_text:
 * @self: a #ClawtOperatorProfile
 * @text: what the profile should say
 * @error: (out) (optional): return location for a #GError
 *
 * Replaces the editable half.
 *
 * 0600, like every other file in the state directory: this is a
 * description of a person.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_operator_profile_write_text(ClawtOperatorProfile  *self,
                                           const gchar           *text,
                                           GError               **error);

/**
 * clawt_operator_profile_render:
 * @self: a #ClawtOperatorProfile
 * @limit: how many learned memories to include, 0 for a sensible default
 *
 * The whole profile as org text: the file, then what agents recorded.
 *
 * Returns %NULL when there is nothing at all -- no file and no memories
 * -- so a caller can tell "nothing known yet" from "known to be empty"
 * without inspecting both halves itself.
 *
 * Returns: (transfer full) (nullable): the org text, or %NULL
 */
gchar *clawt_operator_profile_render(ClawtOperatorProfile *self,
                                     guint                 limit);

/**
 * clawt_operator_profile_learned:
 * @self: a #ClawtOperatorProfile
 * @limit: how many at most, 0 for a sensible default
 *
 * The fleet-scope memories filed under %CLAWT_OPERATOR_CATEGORY.
 *
 * Empty, never %NULL, when the fleet database does not exist -- reading
 * the profile must not create it.
 *
 * Returns: (transfer full) (element-type ClawtMemory): the memories
 */
GPtrArray *clawt_operator_profile_learned(ClawtOperatorProfile *self,
                                          guint                 limit);

G_END_DECLS
