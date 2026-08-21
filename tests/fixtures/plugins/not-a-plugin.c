/*
 * not-a-plugin.c - A module that registers the wrong kind of thing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The ABI matches, so the manager gets as far as instantiating -- and has
 * to notice that what it was handed is not a ClawtPlugin at all.  Without
 * that check the daemon would call plugin vfuncs on a plain GObject.
 */

#include <clawtilla.h>

G_MODULE_EXPORT const guint clawt_plugin_abi_version =
    CLAWT_PLUGIN_ABI_VERSION;

G_MODULE_EXPORT GType
clawt_plugin_register(void)
{
    return G_TYPE_OBJECT;
}
