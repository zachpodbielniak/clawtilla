/*
 * clawt-exec-result.c - What running a command produced
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-exec-result.h"

struct _ClawtExecResult {
    gint      ref_count;

    gint      exit_status;
    gchar    *stdout_text;
    gchar    *stderr_text;
    gboolean  truncated;
};

static ClawtExecResult *
clawt_exec_result_ref(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

G_DEFINE_BOXED_TYPE(ClawtExecResult, clawt_exec_result,
                    clawt_exec_result_ref, clawt_exec_result_free)

ClawtExecResult *
clawt_exec_result_new(gint         exit_status,
                      const gchar *stdout_text,
                      const gchar *stderr_text)
{
    ClawtExecResult *self = g_new0(ClawtExecResult, 1);

    self->ref_count = 1;
    self->exit_status = exit_status;
    self->stdout_text = g_strdup(stdout_text != NULL ? stdout_text : "");
    self->stderr_text = g_strdup(stderr_text != NULL ? stderr_text : "");

    return self;
}

ClawtExecResult *
clawt_exec_result_copy(ClawtExecResult *self)
{
    ClawtExecResult *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_exec_result_new(self->exit_status, self->stdout_text,
                                 self->stderr_text);
    copy->truncated = self->truncated;

    return copy;
}

void
clawt_exec_result_free(ClawtExecResult *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->stdout_text);
    g_free(self->stderr_text);
    g_free(self);
}

gint
clawt_exec_result_get_exit_status(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, -1);
    return self->exit_status;
}

const gchar *
clawt_exec_result_get_stdout(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->stdout_text;
}

const gchar *
clawt_exec_result_get_stderr(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->stderr_text;
}

gboolean
clawt_exec_result_succeeded(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, FALSE);
    return self->exit_status == 0;
}

gboolean
clawt_exec_result_get_truncated(ClawtExecResult *self)
{
    g_return_val_if_fail(self != NULL, FALSE);
    return self->truncated;
}

void
clawt_exec_result_set_truncated(ClawtExecResult *self, gboolean truncated)
{
    g_return_if_fail(self != NULL);
    self->truncated = truncated;
}
