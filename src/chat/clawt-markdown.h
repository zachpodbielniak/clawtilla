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
 * code, headings, bullet and numbered lists, block quotes, rules and
 * GFM tables. Links are rendered with their target beside them rather
 * than as anchors -- a clickable link in model output is one keystroke
 * between a prompt injection and a browser.
 *
 * Tables are the one construct cmark does not parse, so a table block
 * is found in the source and drawn here, cell by cell. One that fits
 * the column is a padded grid in the code font; one too wide becomes a
 * `Header: value` line per cell, since a grid that wraps loses every
 * column after the wrap. An indented table and one inside a block
 * quote are left as they were. clawt-markdown.c says why for each.
 *
 * Always returns valid markup, including for input that is not markdown
 * at all.
 *
 * Returns: (transfer full): Pango markup
 */
gchar *clawt_markdown_to_pango(const gchar *markdown);

/**
 * clawt_markdown_to_pango_full:
 * @markdown: (nullable): what the agent said
 * @code_font: (nullable): the family to render code in, or %NULL for the
 *   system's monospace
 *
 * As clawt_markdown_to_pango(), with the code font named.
 *
 * `<tt>` is Pango's generic monospace alias, resolved through fontconfig
 * -- and nothing in GTK CSS can redirect it. So a person who picked a
 * code font in the client saw it in the exec console and not in a chat
 * message, which is the place they were actually reading code. Naming
 * the family in a `<span>` is the only way the choice reaches this text.
 *
 * @code_font is escaped before it is emitted. It comes from a font
 * chooser rather than from a model, but the rule in this file is that
 * nothing reaches a markup parser unescaped, whoever wrote it.
 *
 * Returns: (transfer full): Pango markup
 */
gchar *clawt_markdown_to_pango_full(const gchar *markdown,
                                    const gchar *code_font);

/**
 * clawt_markdown_to_html:
 * @markdown: (nullable): what the agent said
 *
 * Turns markdown into HTML for the web client.
 *
 * The same walk over the same document as clawt_markdown_to_pango(),
 * emitting the other vocabulary -- one grammar, two outputs, so a
 * construct cannot render in one client and vanish in the other.
 *
 * The escaping rule is the same and matters more: this text is served
 * to a browser, so an agent that writes `<script>` gets those
 * characters on the page. Markup is emitted only for the structure
 * cmark found, and every literal is escaped on the way out.
 *
 * Nothing here emits an `<a href>`, an `<img src>` or a style
 * attribute. A link shows its target beside the text and is not
 * clickable, exactly as in the GTK client -- which also means no URL
 * a model wrote is ever parsed as one, so `javascript:` is text rather
 * than a scheme to be filtered.
 *
 * Block-level output, so the caller's element must not be styled
 * `white-space: pre-wrap`. The classes it can emit are `md-table`,
 * `md-link`, `md-url`, `md-c` and `md-r`; everything else is a plain
 * HTML element.
 *
 * Returns: (transfer full): HTML
 */
gchar *clawt_markdown_to_html(const gchar *markdown);

G_END_DECLS
