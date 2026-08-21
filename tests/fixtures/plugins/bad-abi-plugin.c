/*
 * bad-abi-plugin.c - A plugin from the wrong era
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Built against an ABI version this daemon does not speak, so the manager
 * must refuse it before calling anything in it.
 */

#include <clawtilla.h>

G_MODULE_EXPORT const guint clawt_plugin_abi_version = 9999;

/*
 * Deliberately reachable but never called.  If the manager ever gets this
 * far the abort makes it obvious, rather than leaving a subtly broken
 * plugin loaded.
 */
G_MODULE_EXPORT GType
clawt_plugin_register(void)
{
    g_error("the ABI check let a stale plugin through");

    return G_TYPE_INVALID;
}
