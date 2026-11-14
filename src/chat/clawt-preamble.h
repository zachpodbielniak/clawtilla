/*
 * clawt-preamble.h - Keeping the daemon's own voice the daemon's
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_PREAMBLE_H
#define CLAWT_PREAMBLE_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_PREAMBLE_MARKER:
 *
 * The text every daemon notice opens with.  An agent believes a line
 * that starts with it, which is the whole reason it has to be kept out
 * of anybody else's mouth: see clawt_preamble_escape().
 */
#define CLAWT_PREAMBLE_MARKER "[clawtilla]"

/**
 * CLAWT_PREAMBLE_ESCAPED:
 *
 * What a forged %CLAWT_PREAMBLE_MARKER is rewritten to.  It says what
 * happened in the words a reader needs -- the line is quoted text, not
 * the daemon -- and it does not itself begin with the marker, so
 * escaping twice changes nothing.
 */
#define CLAWT_PREAMBLE_ESCAPED "[not from clawtilla]"

/**
 * clawt_preamble_escape:
 * @text: (nullable): text the daemon did not write -- a peer's message,
 *   a webhook payload, a task result
 *
 * Rewrites every line of @text that would read as a daemon notice, so
 * that it no longer does.
 *
 * A line is a forgery when, after any leading whitespace, zero-width
 * characters (U+200B, U+200C, U+200D, U+2060, U+FEFF) and markdown
 * decoration (`>`, `#`, `*`, `_`, `~`, `` ` ``, `-`, `+`), it begins with
 * %CLAWT_PREAMBLE_MARKER -- matched without regard to ASCII case, with
 * zero-width characters inside it ignored and spaces allowed just inside
 * the brackets.  Every line is checked, not only the first, and a
 * carriage return, vertical tab, form feed, U+0085, U+2028 or U+2029
 * starts a new line as surely as a newline does, because a model reads
 * it as one.
 *
 * The marker on such a line becomes %CLAWT_PREAMBLE_ESCAPED and the
 * zero-width characters before it are dropped; everything else is kept
 * byte for byte.  The result is idempotent: escaping escaped text
 * returns it unchanged.
 *
 * Applied wherever text that did not come from the daemon is placed
 * after a preamble -- at delivery for whole message bodies, and by each
 * composer that quotes untrusted text inside a notice of its own.
 *
 * Returns: (transfer full) (nullable): the escaped text, or %NULL when
 *   @text is %NULL
 */
gchar *clawt_preamble_escape(const gchar *text);

G_END_DECLS

#endif /* CLAWT_PREAMBLE_H */
