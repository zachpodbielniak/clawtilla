/*
 * clawt-secret-ref-private.h - Making a command unkillable, for a test
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately absent from <clawtilla.h> and from PUBLIC_HEADERS: nothing
 * here is API, and nothing in the daemon calls it.  It exists for
 * tests/test-secret-ref.c, which needs a timed-out command that is not
 * reaped within the reap grace -- what a process in uninterruptible sleep
 * does -- without a hung NFS or FUSE mount to put one in.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_secret_ref_test_set_spare_on_timeout: (skip)
 * @spare: %TRUE to leave a timed-out command and its process group alive
 *
 * Test only.  While set, a command backend that reaches its deadline is
 * not sent SIGKILL; everything else -- closing stdout, the reap grace,
 * handing the pid to the default context's child watch -- runs as usual.
 */
void clawt_secret_ref_test_set_spare_on_timeout(gboolean spare);

G_END_DECLS
