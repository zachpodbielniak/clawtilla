/*
 * clawt-computer-provider.c - Adding a kind of computer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "interfaces/clawt-computer-provider.h"

G_DEFINE_INTERFACE(ClawtComputerProvider, clawt_computer_provider,
                   G_TYPE_OBJECT)

static void
clawt_computer_provider_default_init(ClawtComputerProviderInterface *iface)
{
    (void)iface;
}

const gchar *
clawt_computer_provider_get_type_name(ClawtComputerProvider *self)
{
    ClawtComputerProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_COMPUTER_PROVIDER(self), NULL);

    iface = CLAWT_COMPUTER_PROVIDER_GET_IFACE(self);

    return (iface->get_type_name != NULL) ? iface->get_type_name(self) : NULL;
}

ClawtComputer *
clawt_computer_provider_create(ClawtComputerProvider *self,
                               ClawtAgentConfig *config, GError **error)
{
    ClawtComputerProviderInterface *iface;

    g_return_val_if_fail(CLAWT_IS_COMPUTER_PROVIDER(self), NULL);

    iface = CLAWT_COMPUTER_PROVIDER_GET_IFACE(self);

    if (iface->create == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the '%s' provider claims a computer type but cannot "
                    "build one",
                    clawt_computer_provider_get_type_name(self));
        return NULL;
    }

    return iface->create(self, config, error);
}
