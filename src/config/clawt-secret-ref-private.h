/*
 * clawt-secret-ref-private.h - Test seams for the command secret backend
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately absent from <clawtilla.h> and from PUBLIC_HEADERS: nothing
 * here is API, and nothing in the daemon calls it.  It exists for
 * tests/test-secret-ref.c, which needs to see what a resolution does to
 * processes without racing the kernel for it:
 *
 * - a timed-out command that is not reaped within the reap grace -- what a
 *   process in uninterruptible sleep does -- without a hung NFS or FUSE
 *   mount to put one in;
 * - every signal the deadline sends, and whether the command had already
 *   been reaped when it went out, so a group signal to a pgid number the
 *   kernel may have handed on is caught without waiting for pid reuse;
 * - the path taken on a kernel without pidfd_open(2).
 *
 * The seams are plain globals: set them from the test's main thread, with
 * no resolution running.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * ClawtSecretKillRecord: (skip)
 * @target: the pid argument kill(2) was given: the command's pid, or its
 *   negation for the command's process group
 * @reaped: whether the command had already been reaped -- its pid, and so
 *   the pgid number, released to the kernel -- when the signal was sent
 *
 * One SIGKILL the command backend sent, as recorded by
 * clawt_secret_ref_test_record_kills().
 */
typedef struct {
	gint     target;
	gboolean reaped;
} ClawtSecretKillRecord;

/**
 * clawt_secret_ref_test_set_spare_on_timeout: (skip)
 * @spare: %TRUE to leave a timed-out command and its process group alive
 *
 * Test only.  While set, a command backend that reaches its deadline is
 * not sent SIGKILL; everything else -- closing stdout, the reap grace,
 * handing the pid to the default context's child watch -- runs as usual.
 */
void clawt_secret_ref_test_set_spare_on_timeout(gboolean spare);

/**
 * clawt_secret_ref_test_record_kills: (skip)
 * @records: (nullable) (element-type ClawtSecretKillRecord): an array of
 *   #ClawtSecretKillRecord to append to, or %NULL to stop recording
 *
 * Test only.  While set, every kill(2) the command backend sends is
 * appended to @records, after it is sent.  The caller keeps ownership of
 * @records and must clear the seam before freeing it.
 */
void clawt_secret_ref_test_record_kills(GArray *records);

/**
 * clawt_secret_ref_test_set_no_pidfd: (skip)
 * @no_pidfd: %TRUE to behave as on a kernel without pidfd_open(2)
 *
 * Test only.  While set, the command backend does not open a pidfd and
 * waits with a GLib child watch instead, as it does when pidfd_open(2)
 * fails with ENOSYS.
 */
void clawt_secret_ref_test_set_no_pidfd(gboolean no_pidfd);

G_END_DECLS
