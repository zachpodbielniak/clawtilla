/*
 * clawt-computer.c - What an agent can run commands on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-computer.h"

#include <string.h>

enum {
    SIGNAL_STATE_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct {
    gchar              *agent_id;
    GPtrArray          *mounts;
    ClawtComputerState  state;
    gchar              *last_error;
} ClawtComputerPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(ClawtComputer, clawt_computer,
                                    G_TYPE_OBJECT)

#define PRIV(self) \
    ((ClawtComputerPrivate *) \
     clawt_computer_get_instance_private(CLAWT_COMPUTER(self)))

#define CALL_OR_TRUE(self, method, error)                                  \
    do {                                                                   \
        ClawtComputerClass *klass = CLAWT_COMPUTER_GET_CLASS(self);        \
        if (klass->method == NULL)                                         \
            return TRUE;                                                   \
        return klass->method(self, error);                                 \
    } while (0)

gboolean
clawt_computer_provision(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, provision, error);
}

gboolean
clawt_computer_start(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, start, error);
}

gboolean
clawt_computer_stop(ClawtComputer *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    CALL_OR_TRUE(self, stop, error);
}

gboolean
clawt_computer_teardown(ClawtComputer *self, GError **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    /*
     * Refused rather than answered TRUE, unlike every other vfunc here.
     *
     * This one used to go through CALL_OR_TRUE, so a backend that had not
     * implemented it reported the computer as destroyed and destroyed
     * nothing. ClawtVmComputer was in exactly that state: removing a VM
     * agent said "removed", left the libvirt domain defined and the disk
     * on disk, and the only way to find out was to go looking in
     * virt-manager for a VM that should not have been there.
     *
     * A backend that genuinely has nothing to destroy says so with a
     * teardown of its own -- see the null and host computers, which are
     * two lines each. That way the next backend added inherits a loud
     * failure rather than a quiet lie.
     */
    if (klass->teardown == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "a %s computer does not know how to destroy itself, so "
                    "anything it created is still there. Remove it by hand.",
                    clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE,
                                       clawt_computer_get_computer_type(self)));
        return FALSE;
    }

    return klass->teardown(self, error);
}

ClawtExecResult *
clawt_computer_exec(ClawtComputer        *self,
                    const gchar * const  *argv,
                    const gchar          *working_dir,
                    guint                 timeout_seconds,
                    GCancellable         *cancellable,
                    GError              **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->exec == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no computer to run commands on");
        return NULL;
    }

    return klass->exec(self, argv, working_dir, timeout_seconds, cancellable,
                       error);
}

gboolean
clawt_computer_put_file(ClawtComputer  *self,
                        const gchar    *local_path,
                        const gchar    *remote_path,
                        GError        **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->put_file == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this computer cannot receive files");
        return FALSE;
    }

    return klass->put_file(self, local_path, remote_path, error);
}

gboolean
clawt_computer_get_file(ClawtComputer  *self,
                        const gchar    *remote_path,
                        const gchar    *local_path,
                        GError        **error)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), FALSE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    if (klass->get_file == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this computer cannot provide files");
        return FALSE;
    }

    return klass->get_file(self, remote_path, local_path, error);
}

gchar *
clawt_computer_describe(ClawtComputer *self)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    return (klass->describe != NULL) ? klass->describe(self)
                                     : g_strdup("You have no computer.");
}

ClawtComputerType
clawt_computer_get_computer_type(ClawtComputer *self)
{
    ClawtComputerClass *klass;

    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), CLAWT_COMPUTER_NONE);

    klass = CLAWT_COMPUTER_GET_CLASS(self);

    return (klass->get_computer_type != NULL)
           ? klass->get_computer_type(self)
           : CLAWT_COMPUTER_NONE;
}

ClawtComputerState
clawt_computer_get_state(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), CLAWT_COMPUTER_STATE_ABSENT);

    return PRIV(self)->state;
}

const gchar *
clawt_computer_get_agent_id(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->agent_id;
}

const gchar *
clawt_computer_get_last_error(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->last_error;
}

void
clawt_computer_add_mount(ClawtComputer *self, ClawtMount *mount)
{
    g_return_if_fail(CLAWT_IS_COMPUTER(self));
    g_return_if_fail(mount != NULL);

    g_ptr_array_add(PRIV(self)->mounts, clawt_mount_copy(mount));
}

GPtrArray *
clawt_computer_get_mounts(ClawtComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_COMPUTER(self), NULL);

    return PRIV(self)->mounts;
}

void
clawt_computer_set_state(ClawtComputer      *self,
                         ClawtComputerState  state,
                         const gchar        *detail)
{
    ClawtComputerPrivate *priv;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    priv = PRIV(self);

    if (detail != NULL) {
        g_free(priv->last_error);
        priv->last_error = g_strdup(detail);
    }

    if (priv->state == state)
        return;

    priv->state = state;
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state, detail);
}

void
clawt_computer_bind_agent(ClawtComputer *self, const gchar *agent_id)
{
    ClawtComputerPrivate *priv;

    g_return_if_fail(CLAWT_IS_COMPUTER(self));

    priv = PRIV(self);
    g_free(priv->agent_id);
    priv->agent_id = g_strdup(agent_id);
}

gchar *
clawt_computer_truncate_output(const gchar *text,
                               gsize        limit,
                               gboolean    *out_truncated)
{
    gsize length;

    if (out_truncated != NULL)
        *out_truncated = FALSE;

    if (text == NULL)
        return g_strdup("");

    length = strlen(text);

    if (limit == 0 || length <= limit)
        return g_strdup(text);

    if (out_truncated != NULL)
        *out_truncated = TRUE;

    /*
     * The marker matters as much as the cut.  An agent handed a silently
     * shortened directory listing treats it as the whole thing and reaches
     * a confident wrong conclusion; one told it was cut short asks a
     * narrower question instead.
     */
    return g_strdup_printf(
        "%.*s\n\n[... output truncated at %" G_GSIZE_FORMAT " bytes; "
        "%" G_GSIZE_FORMAT " bytes were produced. Narrow the command to see "
        "the rest.]",
        (int)limit, text, limit, length);
}

static void
clawt_computer_finalize(GObject *object)
{
    ClawtComputerPrivate *priv = PRIV(object);

    g_clear_pointer(&priv->agent_id, g_free);
    g_clear_pointer(&priv->last_error, g_free);
    g_clear_pointer(&priv->mounts, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_computer_parent_class)->finalize(object);
}

static void
clawt_computer_class_init(ClawtComputerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_computer_finalize;

    /**
     * ClawtComputer::state-changed:
     * @self: the computer
     * @state: the new state
     * @detail: (nullable): what happened
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed", CLAWT_TYPE_COMPUTER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_INT, G_TYPE_STRING);
}

static void
clawt_computer_init(ClawtComputer *self)
{
    ClawtComputerPrivate *priv = PRIV(self);

    priv->mounts = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    priv->state = CLAWT_COMPUTER_STATE_ABSENT;
}
