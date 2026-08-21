/*
 * clawt-computer.h - What an agent can run commands on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Four backends behind one interface: nothing at all, the real host, a
 * container, or a virtual machine.  Desktop control is not a backend -- it
 * is an add-on that works alongside any of them.
 *
 * The interface is deliberately small.  Everything an agent does with a
 * computer is "run this and tell me what happened", "put this file there",
 * "fetch that one" -- and the backends differ enormously in how they do
 * that while agreeing completely on what it means.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "computer/clawt-exec-result.h"
#include "computer/clawt-mount.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_COMPUTER (clawt_computer_get_type())

G_DECLARE_DERIVABLE_TYPE(ClawtComputer, clawt_computer,
                         CLAWT, COMPUTER, GObject)

/**
 * ClawtComputerClass:
 * @provision: create it if it does not exist
 * @start: make it ready for commands
 * @stop: take it down
 * @teardown: destroy it entirely
 * @exec: run a command and wait for it
 * @put_file: copy a file in
 * @get_file: copy a file out
 * @describe: a sentence for the agent's prompt
 * @get_computer_type: which backend this is
 *
 * The vtable a backend implements.
 */
struct _ClawtComputerClass {
    GObjectClass parent_class;

    gboolean          (*provision)  (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*start)      (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*stop)       (ClawtComputer        *self,
                                     GError              **error);
    gboolean          (*teardown)   (ClawtComputer        *self,
                                     GError              **error);

    ClawtExecResult * (*exec)       (ClawtComputer        *self,
                                     const gchar * const  *argv,
                                     const gchar          *working_dir,
                                     guint                 timeout_seconds,
                                     GCancellable         *cancellable,
                                     GError              **error);

    gboolean          (*put_file)   (ClawtComputer        *self,
                                     const gchar          *local_path,
                                     const gchar          *remote_path,
                                     GError              **error);
    gboolean          (*get_file)   (ClawtComputer        *self,
                                     const gchar          *remote_path,
                                     const gchar          *local_path,
                                     GError              **error);

    gchar *           (*describe)   (ClawtComputer        *self);

    ClawtComputerType (*get_computer_type)(ClawtComputer   *self);

    /*< private >*/
    gpointer _padding[8];
};

/**
 * clawt_computer_provision:
 * @self: a #ClawtComputer
 * @error: (out) (optional): return location for a #GError
 *
 * The lifecycle, in order: provision creates whatever does not exist
 * yet, start makes it usable, stop makes it unusable but keeps it, and
 * teardown destroys it.
 *
 * start() provisions first if it has to, so a caller that only ever
 * calls start() is correct.  teardown() is never called automatically:
 * destroying an agent's container because the daemon restarted would
 * lose whatever was in it.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_computer_provision(ClawtComputer *self, GError **error);
gboolean clawt_computer_start(ClawtComputer *self, GError **error);
gboolean clawt_computer_stop(ClawtComputer *self, GError **error);
gboolean clawt_computer_teardown(ClawtComputer *self, GError **error);

/**
 * clawt_computer_exec:
 * @self: a #ClawtComputer
 * @argv: (array zero-terminated=1): the command
 * @working_dir: (nullable): where to run it
 * @timeout_seconds: give up after this long, or 0 for no limit
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs a command and waits for it.
 *
 * A timeout is strongly advised: an agent that runs an interactive command
 * by mistake would otherwise wait for input that never comes, and the turn
 * never ends.
 *
 * Returns: (transfer full) (nullable): what happened, or %NULL if it could
 *   not be run at all
 */
ClawtExecResult *clawt_computer_exec(ClawtComputer        *self,
                                     const gchar * const  *argv,
                                     const gchar          *working_dir,
                                     guint                 timeout_seconds,
                                     GCancellable         *cancellable,
                                     GError              **error);

gboolean clawt_computer_put_file(ClawtComputer  *self,
                                 const gchar    *local_path,
                                 const gchar    *remote_path,
                                 GError        **error);
gboolean clawt_computer_get_file(ClawtComputer  *self,
                                 const gchar    *remote_path,
                                 const gchar    *local_path,
                                 GError        **error);

/**
 * clawt_computer_describe:
 * @self: a #ClawtComputer
 *
 * A description for the agent's prompt: what it has and what it cannot
 * reach, so it does not discover the limits by trial.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_computer_describe(ClawtComputer *self);

ClawtComputerType  clawt_computer_get_computer_type(ClawtComputer *self);
ClawtComputerState clawt_computer_get_state(ClawtComputer *self);

/**
 * clawt_computer_get_agent_id:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none): the agent this belongs to
 */
const gchar *clawt_computer_get_agent_id(ClawtComputer *self);

/**
 * clawt_computer_add_mount:
 * @self: a #ClawtComputer
 * @mount: (transfer none): a host path to share
 *
 * Adds a mount.  Applied when the computer is provisioned, so calling this
 * afterwards has no effect until it is provisioned again.
 */
void clawt_computer_add_mount(ClawtComputer *self, ClawtMount *mount);

/**
 * clawt_computer_get_mounts:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none) (element-type ClawtMount): the mounts
 */
GPtrArray *clawt_computer_get_mounts(ClawtComputer *self);

/**
 * clawt_computer_get_last_error:
 * @self: a #ClawtComputer
 *
 * Returns: (transfer none) (nullable): why it last failed
 */
const gchar *clawt_computer_get_last_error(ClawtComputer *self);

/*< protected >*/

void clawt_computer_set_state(ClawtComputer      *self,
                              ClawtComputerState  state,
                              const gchar        *detail);

void clawt_computer_bind_agent(ClawtComputer *self,
                               const gchar   *agent_id);

/**
 * clawt_computer_truncate_output:
 * @text: (nullable): output to bound
 * @limit: maximum bytes
 * @out_truncated: (out) (optional): whether anything was removed
 *
 * Bounds command output.
 *
 * Unbounded output is a real failure mode: an agent that runs `find /` gets
 * a reply too large to send and too large to reason about, and the turn is
 * wasted either way.  Truncation is reported rather than hidden, because an
 * agent treating a cut-short listing as complete reaches confident wrong
 * conclusions.
 *
 * Returns: (transfer full): the bounded text
 */
gchar *clawt_computer_truncate_output(const gchar *text,
                                      gsize        limit,
                                      gboolean    *out_truncated);

G_END_DECLS
