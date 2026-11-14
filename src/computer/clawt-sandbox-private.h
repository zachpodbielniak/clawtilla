/*
 * clawt-sandbox-private.h - Taking one protection out of a sandbox, for a test
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately absent from <clawtilla.h> and from PUBLIC_HEADERS: nothing
 * here is API, and nothing in the daemon calls it.  It exists for
 * tests/test-sandbox-probe.c, which runs a hostile probe inside a real box
 * and then removes one protection at a time to prove that each probe
 * check can fail.  A check never shown to fail is a check that may not be
 * able to -- see docs/security.org, "The sandbox self-check".
 *
 * There is no configuration key, environment variable or IPC frame that
 * reaches this.  Only code already running inside the daemon's process
 * can call it, and that code can spawn whatever it likes anyway.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "computer/clawt-sandbox.h"

G_BEGIN_DECLS

/*
 * One flag per protection the bwrap builder applies, each naming the
 * weaker thing it is replaced with.  "Removed" has to mean "replaced with
 * what a careless builder would have written": dropping `--tmpfs /tmp`
 * outright leaves /tmp absent, which is no weaker, so the mutation binds
 * the host's /tmp instead.
 */
typedef enum {
	CLAWT_SANDBOX_MUTATE_NONE            = 0,
	CLAWT_SANDBOX_MUTATE_SHARE_TMP       = 1 << 0,  /* --bind /tmp, not --tmpfs */
	CLAWT_SANDBOX_MUTATE_SHARE_HOME      = 1 << 1,  /* --bind $HOME, not a tmpfs or nothing */
	CLAWT_SANDBOX_MUTATE_WIDEN_ROOT      = 1 << 2,  /* the root's parent is bound, not the root */
	CLAWT_SANDBOX_MUTATE_SHARE_NET       = 1 << 3,  /* no --unshare-net */
	CLAWT_SANDBOX_MUTATE_SHARE_PID       = 1 << 4,  /* no --unshare-pid */
	CLAWT_SANDBOX_MUTATE_SHARE_SESSION   = 1 << 5,  /* no --new-session */
	CLAWT_SANDBOX_MUTATE_RO_WRITABLE     = 1 << 6,  /* a `mode: ro` mount bound --bind */
	CLAWT_SANDBOX_MUTATE_SKIP_DENIALS    = 1 << 7,  /* deny_paths emit nothing */
	CLAWT_SANDBOX_MUTATE_AGENT_ENV_WINS  = 1 << 8,  /* TMPDIR, HOME, XDG_* from env: kept */
	CLAWT_SANDBOX_MUTATE_SKIP_LINK_CHECK = 1 << 9,  /* clawt_sandbox_check_links() says yes */
	CLAWT_SANDBOX_MUTATE_SHARE_ABSTRACT  = 1 << 10  /* no Landlock abstract-socket scope */
} ClawtSandboxMutation;

/**
 * clawt_sandbox_test_set_mutations:
 * @self: a #ClawtSandbox
 * @mutations: a mask of #ClawtSandboxMutation, or 0 for the real box
 *
 * Test only.  Replaces the protections named in @mutations with their
 * weaker form the next time the box is built, so a test can show which
 * probe checks each protection is the one guarding.
 */
void clawt_sandbox_test_set_mutations(ClawtSandbox *self,
                                      guint         mutations);

G_END_DECLS
