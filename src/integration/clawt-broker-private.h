/*
 * clawt-broker-private.h - Handing the broker's watch an exit, for a test
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately absent from <clawtilla.h> and from PUBLIC_HEADERS: nothing
 * here is API, and nothing in the daemon calls it.  It exists for
 * tests/test-broker.c, which needs the watch's answer to a stopped
 * container -- restart it or say why not -- without a podman that can be
 * made to stop a real container on cue.  What podman says is tested
 * against a canned socket through clawt_broker_find_stopped(); this is
 * the half after it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "integration/clawt-broker.h"

G_BEGIN_DECLS

/**
 * clawt_broker_test_handle_exit: (skip)
 * @self: a #ClawtBroker with its config set
 * @ended: the container the watch found down, as
 *   clawt_broker_find_stopped() would report it
 *
 * Test only.  Runs exactly what the watch runs when a probe finds
 * @ended down: `broker.restart` decides, and the broker either begins a
 * start or is left failed with a detail that says why.
 */
void clawt_broker_test_handle_exit(ClawtBroker           *self,
                                   const ClawtBrokerExit *ended);

G_END_DECLS
