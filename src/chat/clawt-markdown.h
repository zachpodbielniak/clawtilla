/*
 * clawt-markdown.h - Rendering what a model wrote, without obeying it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_markdown_to_pango:
 * @markdown: (nullable): what the agent said
 *
 * Turns markdown into Pango markup for display.
 *
 * The important word is *turns*. Model output is never handed to a
 * markup parser: this walks the document cmark parsed and emits markup
 * only for the structure cmark identified, escaping every piece of
 * literal text on the way out. An agent that writes `<b>` or `&` gets
 * those characters on screen, not bold and not an entity -- and a
 * message crafted to close a tag cannot reach the parser at all,
 * because the parser never sees its text.
 *
 * Handles what a chat actually uses: bold, italic, inline code, fenced
 * code, headings, bullet and numbered lists, block quotes and rules.
 * Links are rendered with their target beside them rather than as
 * anchors -- a clickable link in model output is one keystroke between
 * a prompt injection and a browser.
 *
 * Always returns valid markup, including for input that is not markdown
 * at all.
 *
 * Returns: (transfer full): Pango markup
 */
gchar *clawt_markdown_to_pango(const gchar *markdown);

G_END_DECLS
