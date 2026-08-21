/*
 * clawt-secret-ref.h - A reference to a secret, never the secret itself
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Credentials in clawtilla.yaml are always references:
 *
 *   api_key: {file: ~/.clawtilla/secrets/anthropic}
 *   api_key: {env: ANTHROPIC_API_KEY}
 *   api_key: {command: "pass show clawtilla/anthropic"}
 *
 * There is deliberately no inline form.  A literal secret in the config is
 * the thing this exists to avoid: the file gets copied into a git repo, or
 * pasted into a bug report, or read by the GTK client and echoed back over
 * IPC, and the secret goes with it.
 *
 * Resolved values live only in memory and in per-agent credential files at
 * mode 0600.  They are never put into an IPC response, a log line or a
 * transcript.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SECRET_REF (clawt_secret_ref_get_type())

GType clawt_secret_ref_get_type(void) G_GNUC_CONST;

/**
 * clawt_secret_ref_new:
 * @backend: how to resolve it
 * @locator: the path, variable name or command line
 *
 * Returns: (transfer full): a new #ClawtSecretRef
 */
ClawtSecretRef *
clawt_secret_ref_new(ClawtSecretBackend  backend,
                     const gchar        *locator);

/**
 * clawt_secret_ref_parse:
 * @spec: a one-key mapping such as `{env: FOO}`, or a bare string
 * @default_backend: backend assumed for a bare string
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a secret reference as it appears in the config.
 *
 * Returns: (transfer full) (nullable): the reference, or %NULL on error
 */
ClawtSecretRef *
clawt_secret_ref_parse(gpointer             spec,
                       ClawtSecretBackend   default_backend,
                       GError             **error);

ClawtSecretRef *clawt_secret_ref_copy(ClawtSecretRef *self);
void            clawt_secret_ref_free(ClawtSecretRef *self);

ClawtSecretBackend clawt_secret_ref_get_backend(ClawtSecretRef *self);
const gchar       *clawt_secret_ref_get_locator(ClawtSecretRef *self);

/**
 * clawt_secret_ref_resolve:
 * @self: a #ClawtSecretRef
 * @base_dir: (nullable): directory a relative file locator resolves against
 * @command_timeout_seconds: how long a command backend may take
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves the reference to its value.
 *
 * The timeout is not optional in spirit: a password manager that is locked
 * blocks rather than failing, and without a bound so does daemon startup.
 *
 * Returns: (transfer full) (nullable): the secret, or %NULL on error
 */
gchar *
clawt_secret_ref_resolve(ClawtSecretRef  *self,
                         const gchar     *base_dir,
                         guint            command_timeout_seconds,
                         GError         **error);

/**
 * clawt_secret_ref_describe:
 * @self: a #ClawtSecretRef
 *
 * A human-readable description that does NOT contain the secret -- safe for
 * logs, IPC responses and error messages.
 *
 * Returns: (transfer full): e.g. "env:ANTHROPIC_API_KEY"
 */
gchar *
clawt_secret_ref_describe(ClawtSecretRef *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtSecretRef, clawt_secret_ref_free)

G_END_DECLS
