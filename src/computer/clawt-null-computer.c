/*
 * clawt-null-computer.c - An agent with no computer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-null-computer.h"

struct _ClawtNullComputer {
    ClawtComputer parent_instance;
};

G_DEFINE_FINAL_TYPE(ClawtNullComputer, clawt_null_computer,
                    CLAWT_TYPE_COMPUTER)

ClawtComputer *
clawt_null_computer_new(const gchar *agent_id)
{
    ClawtComputer *self = g_object_new(CLAWT_TYPE_NULL_COMPUTER, NULL);

    clawt_computer_bind_agent(self, agent_id);

    return self;
}

static ClawtExecResult *
null_exec(ClawtComputer        *self,
          const gchar * const  *argv,
          const gchar          *working_dir,
          guint                 timeout_seconds,
          GCancellable         *cancellable,
          GError              **error)
{
    (void)self; (void)argv; (void)working_dir;
    (void)timeout_seconds; (void)cancellable;

    /*
     * An explicit refusal rather than a silent failure.  An agent that gets
     * an empty result assumes the command produced nothing and carries on;
     * one that gets this sentence stops asking.
     */
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                        "you have no computer, so there is nowhere to run "
                        "commands");
    return NULL;
}

static gchar *
null_describe(ClawtComputer *self)
{
    (void)self;

    return g_strdup("You have no computer. You cannot run commands, read "
                    "files or reach a filesystem.");
}

/*
 * Nothing was ever created, so nothing is left over.
 *
 * Spelled out rather than left to a default, because the default is now a
 * refusal -- a backend that cannot destroy what it made should say so,
 * and this one genuinely made nothing.
 */
static gboolean
null_teardown(ClawtComputer *computer, GError **error)
{
    return TRUE;
}

static ClawtComputerType
null_get_computer_type(ClawtComputer *self)
{
    (void)self;
    return CLAWT_COMPUTER_NONE;
}

static void
clawt_null_computer_class_init(ClawtNullComputerClass *klass)
{
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    computer_class->teardown = null_teardown;
    computer_class->exec = null_exec;
    computer_class->describe = null_describe;
    computer_class->get_computer_type = null_get_computer_type;
}

static void
clawt_null_computer_init(ClawtNullComputer *self)
{
    (void)self;
}
