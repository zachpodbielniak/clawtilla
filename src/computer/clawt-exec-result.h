/*
 * clawt-exec-result.h - What running a command produced
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

G_BEGIN_DECLS

#define CLAWT_TYPE_EXEC_RESULT (clawt_exec_result_get_type())

GType clawt_exec_result_get_type(void) G_GNUC_CONST;

/**
 * clawt_exec_result_new:
 * @exit_status: the command's exit status, or -1 if it was signalled
 * @stdout_text: (nullable): what it wrote to stdout
 * @stderr_text: (nullable): what it wrote to stderr
 *
 * %NULL for either stream becomes an empty string, so a caller printing
 * both does not have to check each one.
 *
 * Returns: (transfer full): a new #ClawtExecResult
 */
ClawtExecResult *clawt_exec_result_new(gint         exit_status,
                                       const gchar *stdout_text,
                                       const gchar *stderr_text);

/**
 * clawt_exec_result_copy:
 * @self: a #ClawtExecResult
 *
 * Returns: (transfer full): a copy
 */
ClawtExecResult *clawt_exec_result_copy(ClawtExecResult *self);
void             clawt_exec_result_free(ClawtExecResult *self);

/**
 * clawt_exec_result_get_exit_status:
 * @self: a #ClawtExecResult
 *
 * The accessors below read what the command produced.
 *
 * stdout and stderr are (transfer none) and never %NULL -- an empty
 * string rather than %NULL, so a caller printing both does not have to
 * check each one.
 *
 * Returns: the command's exit status, or -1 if it was killed by a signal
 */
gint         clawt_exec_result_get_exit_status(ClawtExecResult *self);
const gchar *clawt_exec_result_get_stdout(ClawtExecResult *self);
const gchar *clawt_exec_result_get_stderr(ClawtExecResult *self);
gboolean     clawt_exec_result_succeeded(ClawtExecResult *self);

/**
 * clawt_exec_result_get_truncated:
 * @self: a #ClawtExecResult
 *
 * Whether output was cut short by the size limit.
 *
 * Reported rather than hidden: an agent reasoning about a truncated
 * directory listing as if it were complete reaches confident wrong
 * conclusions.
 *
 * Returns: %TRUE if output was truncated
 */
gboolean clawt_exec_result_get_truncated(ClawtExecResult *self);

void clawt_exec_result_set_truncated(ClawtExecResult *self,
                                     gboolean         truncated);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtExecResult, clawt_exec_result_free)

G_END_DECLS
