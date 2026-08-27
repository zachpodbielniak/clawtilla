/*
 * clawt-markdown.c - Rendering what a model wrote, without obeying it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two vocabularies, one grammar.  The GTK client wants Pango markup and
 * the web client wants HTML, and the temptation is a renderer each --
 * which is how the chat transcript and the Flow tab came to draw the
 * same message two different ways, and how tables came to exist in one
 * client and not the other.
 *
 * So there is one walk over the document, and a RenderOps per output.
 * The walk decides *what* is happening; the ops decide how it is
 * written.  Adding a construct means adding a vfunc, and a backend that
 * has not implemented it does not compile -- which is the only kind of
 * "both clients got it" that does not rely on somebody remembering.
 */

#include "clawtilla.h"
#include "chat/clawt-markdown.h"

#include <cmark.h>
#include <string.h>

/*
 * Dimming rather than a colour.
 *
 * A hardcoded colour is right in one theme and unreadable in the other,
 * and the client is theme-aware. Alpha works either way round.
 */
#define DIM_OPEN  "<span alpha=\"60%\">"
#define DIM_CLOSE "</span>"

/*
 * How wide a table may draw before it is laid out as records instead.
 *
 * Measured rather than chosen.  The shipped column is AdwClamp's 600px;
 * a body starts CHAT_ROW_MARGIN + CHAT_GUTTER in and stops one margin
 * short of the far edge, which is 68px in all; and a monospace glyph at
 * the default font advances 9px.  So 59 characters reach the edge and
 * 58 keeps one back.
 *
 * A person who widened the measure gets records where a grid would have
 * fitted.  That is the safe direction: a record layout reads correctly
 * at any width, and a grid one character too wide wraps and stops being
 * a grid at all.
 *
 * Pango only.  A browser can scroll a table sideways inside its own box,
 * so the HTML vocabulary draws a real <table> at any width and has no
 * threshold to be wrong about.
 */
#define TABLE_MAX_WIDTH (58)

/* The rule under a table's header, and between two records. */
#define TABLE_RULE_UNIT "\xe2\x94\x80"

typedef enum {
    CELL_ALIGN_LEFT,
    CELL_ALIGN_CENTER,
    CELL_ALIGN_RIGHT
} CellAlign;

typedef struct {
    gboolean ordered;
    gint     next;      /* the number the next item gets */
} ListLevel;

typedef struct _Render Render;

/*
 * One output vocabulary.
 *
 * Every callback is required.  A NULL here would be a construct that
 * renders in one client and silently vanishes in the other, which is
 * exactly the failure this indirection exists to make impossible --
 * so there is no "optional" entry and no NULL check at any call site.
 */
typedef struct {
    /* Inline. */
    void (*text)    (Render *self, const gchar *literal);
    void (*emph)    (Render *self, gboolean enter);
    void (*strong)  (Render *self, gboolean enter);
    void (*code)    (Render *self, const gchar *literal);
    void (*link)    (Render *self, gboolean enter, const gchar *url);
    void (*newline) (Render *self);

    /* Block. */
    void (*paragraph)  (Render *self, gboolean enter, gboolean in_item);
    void (*heading)    (Render *self, gboolean enter, gint level);
    void (*quote)      (Render *self, gboolean enter);
    void (*list)       (Render *self, gboolean enter, gboolean ordered,
                        gint start);
    void (*item)       (Render *self, gboolean enter, gboolean first);
    void (*code_block) (Render *self, const gchar *literal);
    void (*html_block) (Render *self, const gchar *literal);
    void (*rule)       (Render *self);
    void (*table)      (Render *self, GPtrArray *rows, GArray *aligns);
} RenderOps;

struct _Render {
    const RenderOps *ops;
    GString  *out;

    /*
     * Pango's block state.  It has no block elements, so a block is a
     * newline plus whatever indentation and markers the context owes --
     * which has to be tracked.  The HTML vocabulary carries none of
     * this: <ul>, <blockquote> and <p> say it themselves.
     */
    GArray   *lists;    /* ListLevel, innermost last */
    gint      quote;    /* how deep in block quotes */

    /*
     * The opening and closing tags for a run of code.
     *
     * <tt> by default, which Pango resolves through fontconfig's generic
     * "monospace" alias -- and that alias is not reachable from GTK CSS,
     * so a person who chose a code font in the client would see it in the
     * exec console and not in a chat message.  Naming the family in a
     * <span> is the only way the choice reaches this text.
     *
     * The web client has no such problem: a browser reads the family off
     * the stylesheet like everything else, so the HTML vocabulary emits
     * <code> and leaves the font to CSS.
     */
    const gchar *code_open;
    const gchar *code_close;

    /*
     * Whether a blank line is owed before the next block.
     *
     * Set when a block ends and paid when the next one starts, rather
     * than written eagerly -- otherwise the message ends in blank lines
     * and every nested block pays twice.
     */
    gboolean  blank;

    /*
     * Whether this render is one table cell.
     *
     * A cell is inline content: GFM parses it that way, and a block
     * construct inside one would put a newline into a row and take a
     * Pango grid apart.  cmark has no inline-only parse, so the walk
     * skips the block callbacks here and lets the text through.
     */
    gboolean  inlines_only;

    /*
     * How wide what has been emitted draws, in monospace columns.
     *
     * A cell's markup length has nothing to do with the space it takes
     * on screen, so padding computed from it would put a grid out by
     * however many tags the cell happened to contain.  Counted by the
     * Pango vocabulary alone, and only its table reads it -- which is
     * why every visible character it emits goes through pango_text().
     */
    gsize     visible;
};

static void   render_markdown(Render *render, const gchar *markdown);
static gchar *render_cell(const gchar *source, const Render *parent,
                          gsize *out_width);

/* ── Shared plumbing ─────────────────────────────────────────────── */

/*
 * How many monospace columns a string occupies.
 *
 * Not its length in either bytes or characters: a combining mark adds
 * nothing and an emoji or a CJK glyph takes two cells, and a table
 * padded by character count is visibly crooked for exactly the message
 * that put one in.
 */
static gsize
display_width(const gchar *text)
{
    const gchar *p;
    gsize width = 0;

    if (text == NULL)
        return 0;

    for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        if (g_unichar_iszerowidth(c))
            continue;

        width += g_unichar_iswide(c) ? 2 : 1;
    }

    return width;
}

/* Markup this file wrote, verbatim. */
static void
emit(Render *render, const gchar *markup)
{
    g_string_append(render->out, markup);
}

/*
 * Text somebody else wrote.
 *
 * The one door a literal goes through, in either vocabulary.
 * g_markup_escape_text() covers both: & < > " ' are the five that
 * matter to Pango's parser and to a browser alike.
 */
static void
emit_escaped(Render *render, const gchar *text)
{
    g_autofree gchar *escaped = NULL;

    if (text == NULL)
        return;

    escaped = g_markup_escape_text(text, -1);
    g_string_append(render->out, escaped);
}

/*
 * Markup a Render has already produced.
 *
 * A table cell is rendered on its own so its width can be measured, and
 * what comes back is markup rather than a literal -- every piece of text
 * inside it went through the vocabulary's text callback in that cell's
 * own render, so the escaping rule holds and escaping it again would
 * show the tags.
 */
static void
emit_rendered(Render *render, const gchar *markup)
{
    g_string_append(render->out, markup);
}

/* Returns: (transfer full): @unit repeated @count times */
static gchar *
repeat_utf8(const gchar *unit, gsize count)
{
    GString *out = g_string_new(NULL);

    while (count-- > 0)
        g_string_append(out, unit);

    return g_string_free(out, FALSE);
}

/* ── The Pango vocabulary ────────────────────────────────────────── */

static void
pango_text(Render *render, const gchar *text)
{
    if (text == NULL)
        return;

    emit_escaped(render, text);
    render->visible += display_width(text);
}

static void
pango_spaces(Render *render, gsize count)
{
    g_autofree gchar *pad = g_strnfill(count, ' ');

    pango_text(render, pad);
}

/*
 * Starts a block on its own line, indented for any list it is inside
 * and marked for any quote.
 */
static void
pango_begin_block_full(Render *render, gboolean spaced)
{
    gint i;

    if (render->out->len > 0 &&
        render->out->str[render->out->len - 1] != '\n')
        g_string_append_c(render->out, '\n');

    /*
     * The blank line between blocks. Paid here so a run of list items
     * can decline it and stay tight, which is what a list looks like
     * everywhere else.
     */
    if (spaced && render->blank && render->out->len > 0)
        g_string_append_c(render->out, '\n');

    render->blank = FALSE;

    for (i = 0; i < render->quote; i++) {
        emit(render, DIM_OPEN);
        pango_text(render, "\xe2\x96\x8f ");
        emit(render, DIM_CLOSE);
    }

    /*
     * Two spaces per enclosing list, minus the one the item marker
     * itself occupies -- so a nested bullet lines up under its parent's
     * text rather than under its parent's bullet.
     */
    for (i = 1; i < (gint)render->lists->len; i++)
        pango_text(render, "    ");
}

static void
pango_begin_block(Render *render)
{
    pango_begin_block_full(render, TRUE);
}

static void
pango_end_block(Render *render)
{
    g_string_append_c(render->out, '\n');
    render->blank = TRUE;
}

static void
pango_emph(Render *render, gboolean enter)
{
    emit(render, enter ? "<i>" : "</i>");
}

static void
pango_strong(Render *render, gboolean enter)
{
    emit(render, enter ? "<b>" : "</b>");
}

static void
pango_code(Render *render, const gchar *literal)
{
    emit(render, render->code_open);
    pango_text(render, literal);
    emit(render, render->code_close);
}

static void
pango_link(Render *render, gboolean enter, const gchar *url)
{
    if (enter) {
        emit(render, "<u>");
        return;
    }

    emit(render, "</u>");

    /*
     * The target beside the text, not behind it.  A clickable link in
     * model output is one keystroke between a prompt injection and a
     * browser, and a person who can see where it goes is a person who
     * can decide.
     */
    if (url != NULL && url[0] != '\0') {
        emit(render, DIM_OPEN);
        pango_text(render, " (");
        pango_text(render, url);
        pango_text(render, ")");
        emit(render, DIM_CLOSE);
    }
}

static void
pango_newline(Render *render)
{
    g_string_append_c(render->out, '\n');
}

static void
pango_paragraph(Render *render, gboolean enter, gboolean in_item)
{
    if (!enter) {
        pango_end_block(render);
        return;
    }

    /*
     * A paragraph directly inside a list item continues the line the
     * marker is on; anywhere else it starts its own.
     */
    if (in_item)
        render->blank = FALSE;
    else
        pango_begin_block(render);
}

static void
pango_heading(Render *render, gboolean enter, gint level)
{
    (void)level;

    if (enter) {
        pango_begin_block(render);
        emit(render, "<b><big>");
    } else {
        emit(render, "</big></b>");
        pango_end_block(render);
    }
}

static void
pango_quote(Render *render, gboolean enter)
{
    render->quote += enter ? 1 : -1;
}

static void
pango_list(Render *render, gboolean enter, gboolean ordered, gint start)
{
    if (enter) {
        ListLevel level;

        level.ordered = ordered;
        level.next = start < 1 ? 1 : start;

        g_array_append_val(render->lists, level);
        return;
    }

    if (render->lists->len > 0)
        g_array_remove_index(render->lists, render->lists->len - 1);
}

static void
pango_item(Render *render, gboolean enter, gboolean first)
{
    ListLevel *level;

    if (!enter)
        return;

    pango_begin_block_full(render, first);

    if (render->lists->len == 0) {
        pango_text(render, "\xe2\x80\xa2 ");
        return;
    }

    level = &g_array_index(render->lists, ListLevel, render->lists->len - 1);

    if (level->ordered) {
        g_autofree gchar *marker = g_strdup_printf("%d. ", level->next);

        pango_text(render, marker);
        level->next++;
    } else {
        pango_text(render, "\xe2\x80\xa2 ");
    }
}

/*
 * A fenced or indented code block.
 *
 * Every line is prefixed, so the monospace run is unbroken and a quote
 * or list marker still lands at the start of each line.
 */
static void
pango_code_block(Render *render, const gchar *literal)
{
    g_auto(GStrv) lines = NULL;
    gsize i;

    if (literal == NULL)
        return;

    lines = g_strsplit(literal, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        /* A fenced block ends with a newline, so the last split is empty. */
        if (lines[i + 1] == NULL && lines[i][0] == '\0')
            break;

        pango_begin_block_full(render, i == 0);
        emit(render, render->code_open);
        emit(render, DIM_OPEN);
        pango_text(render, "  ");
        emit(render, DIM_CLOSE);
        pango_text(render, lines[i]);
        emit(render, render->code_close);
    }

    pango_end_block(render);
}

static void
pango_html_block(Render *render, const gchar *literal)
{
    pango_begin_block(render);
    pango_text(render, literal);
    pango_end_block(render);
}

static void
pango_rule(Render *render)
{
    pango_begin_block(render);
    emit(render, DIM_OPEN);
    pango_text(render, TABLE_RULE_UNIT TABLE_RULE_UNIT TABLE_RULE_UNIT
                       TABLE_RULE_UNIT TABLE_RULE_UNIT TABLE_RULE_UNIT
                       TABLE_RULE_UNIT TABLE_RULE_UNIT);
    emit(render, DIM_CLOSE);
    pango_end_block(render);
}

/* The dim rule under a table's header, one run per column. */
static void
pango_column_rule(Render *render, GArray *columns)
{
    guint c;

    pango_begin_block_full(render, FALSE);
    emit(render, render->code_open);
    emit(render, DIM_OPEN);

    for (c = 0; c < columns->len; c++) {
        g_autofree gchar *bar =
            repeat_utf8(TABLE_RULE_UNIT, g_array_index(columns, gsize, c));

        if (c > 0)
            pango_spaces(render, 2);

        pango_text(render, bar);
    }

    emit(render, DIM_CLOSE);
    emit(render, render->code_close);
}

/*
 * The grid: every cell padded to its column, in the code font.
 *
 * The code font is not decoration.  A proportional font gives every
 * glyph its own advance, so spaces line nothing up and the padding is
 * wasted -- a grid only exists because every cell is the same width.
 */
static void
pango_table_grid(Render *render, GPtrArray *rows, GArray *aligns,
                 GPtrArray *markup, GArray *widths, GArray *columns)
{
    guint cols = aligns->len;
    guint r;
    guint c;

    for (r = 0; r < rows->len; r++) {
        pango_begin_block_full(render, r == 0);
        emit(render, render->code_open);

        for (c = 0; c < cols; c++) {
            gsize width = g_array_index(widths, gsize, r * cols + c);
            gsize pad = g_array_index(columns, gsize, c) - width;
            gsize lead = 0;
            CellAlign align = g_array_index(aligns, CellAlign, c);

            if (align == CELL_ALIGN_RIGHT)
                lead = pad;
            else if (align == CELL_ALIGN_CENTER)
                lead = pad / 2;

            if (c > 0)
                pango_spaces(render, 2);

            pango_spaces(render, lead);

            if (r == 0)
                emit(render, "<b>");

            emit_rendered(render, g_ptr_array_index(markup, r * cols + c));

            if (r == 0)
                emit(render, "</b>");

            /* Nothing follows the last column, so nothing pads it. */
            if (c + 1 < cols)
                pango_spaces(render, pad - lead);
        }

        /*
         * A row whose last cells are empty has been padded towards
         * columns that drew nothing, so the line ends in a run of
         * spaces it does not need.  Invisible, but it is width, and a
         * line that measures wider than it looks is what decides
         * whether the label wraps.
         */
        while (render->out->len > 0 &&
               render->out->str[render->out->len - 1] == ' ')
            g_string_truncate(render->out, render->out->len - 1);

        emit(render, render->code_close);

        if (r == 0)
            pango_column_rule(render, columns);
    }

    pango_end_block(render);
}

/*
 * The fallback: one `Header: value` line per cell, records separated by
 * a rule.
 *
 * A table too wide for the column is the case a chat actually produces
 * -- an agent summarising a fleet writes a sentence per cell -- and a
 * grid that wraps is worse than no grid, because the wrap lands in the
 * middle of a row and every column after it is somewhere else.  This
 * carries the same information at any width, and each value wraps as
 * prose, which is what it is.
 */
static void
pango_table_records(Render *render, GPtrArray *rows, guint cols,
                    GPtrArray *markup)
{
    gboolean separator = FALSE;
    gsize drawn = 0;
    guint r;
    guint c;

    for (r = 1; r < rows->len; r++) {
        for (c = 0; c < cols; c++) {
            const gchar *value = g_ptr_array_index(markup, r * cols + c);
            const gchar *label = g_ptr_array_index(markup, c);

            if (value[0] == '\0')
                continue;

            /*
             * Held back until there is something to separate: a rule
             * drawn for a row whose cells are all empty is a divider
             * with nothing under it.
             */
            if (separator) {
                g_autofree gchar *bar = repeat_utf8(TABLE_RULE_UNIT, 8);

                pango_begin_block_full(render, FALSE);
                emit(render, DIM_OPEN);
                pango_text(render, bar);
                emit(render, DIM_CLOSE);
                separator = FALSE;
                drawn++;
            }

            pango_begin_block_full(render, drawn == 0);
            drawn++;

            if (label[0] != '\0') {
                emit(render, "<b>");
                emit_rendered(render, label);
                emit(render, "</b>");
                pango_text(render, ": ");
            }

            emit_rendered(render, value);
        }

        if (drawn > 0)
            separator = TRUE;
    }

    if (drawn > 0)
        pango_end_block(render);
}

static void
pango_table(Render *render, GPtrArray *rows, GArray *aligns)
{
    g_autoptr(GPtrArray) markup = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GArray) widths = g_array_new(FALSE, FALSE, sizeof(gsize));
    g_autoptr(GArray) columns = g_array_new(FALSE, TRUE, sizeof(gsize));
    guint cols = aligns->len;
    gsize total = 0;
    guint r;
    guint c;

    g_array_set_size(columns, cols);

    for (r = 0; r < rows->len; r++) {
        GPtrArray *row = g_ptr_array_index(rows, r);

        for (c = 0; c < cols; c++) {
            /*
             * A row with fewer cells than the header is padded and one
             * with more is truncated, which is what GFM does -- and a
             * ragged table is what a model writes when it forgets a
             * pipe, so refusing it would lose the whole table over one
             * line.
             */
            const gchar *source =
                c < row->len ? g_ptr_array_index(row, c) : "";
            gsize width = 0;

            g_ptr_array_add(markup, render_cell(source, render, &width));
            g_array_append_val(widths, width);

            if (width > g_array_index(columns, gsize, c))
                g_array_index(columns, gsize, c) = width;
        }
    }

    for (c = 0; c < cols; c++)
        total += g_array_index(columns, gsize, c);

    total += 2 * (cols - 1);

    /*
     * A table with no body rows takes the grid whatever it measures.
     * The records layout draws one line per body cell, so for a header
     * alone it draws nothing at all -- and a table that disappears is
     * worse than one that wraps.
     */
    if (total <= TABLE_MAX_WIDTH || rows->len == 1)
        pango_table_grid(render, rows, aligns, markup, widths, columns);
    else
        pango_table_records(render, rows, cols, markup);
}

static const RenderOps pango_ops = {
    pango_text,
    pango_emph,
    pango_strong,
    pango_code,
    pango_link,
    pango_newline,
    pango_paragraph,
    pango_heading,
    pango_quote,
    pango_list,
    pango_item,
    pango_code_block,
    pango_html_block,
    pango_rule,
    pango_table
};

/* ── The HTML vocabulary ─────────────────────────────────────────── */

/*
 * Everything below emits markup this file wrote and text somebody else
 * wrote, and never mixes the two: a literal reaches the page only
 * through html_text(), which escapes.  There is no path by which an
 * agent's angle bracket becomes a tag, which is what lets the web
 * client set this as HTML content at all.
 *
 * Nothing here emits an <a href>, an <img src> or a style attribute --
 * for links, the same reason the Pango vocabulary does not, and more
 * sharply in a browser: a clickable link in model output is one
 * keystroke between a prompt injection and somewhere else.  The target
 * is shown beside the text instead.  That also means no URL from a
 * model is ever parsed as one, so `javascript:` and `data:` are text
 * like everything else rather than a scheme to be filtered.
 */

static void
html_text(Render *render, const gchar *literal)
{
    emit_escaped(render, literal);
}

/* Escaped, with the line breaks somebody typed kept as line breaks. */
static void
html_text_lines(Render *render, const gchar *literal)
{
    g_auto(GStrv) lines = NULL;
    gsize i;

    if (literal == NULL)
        return;

    lines = g_strsplit(literal, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        if (lines[i + 1] == NULL && lines[i][0] == '\0')
            break;

        if (i > 0)
            emit(render, "<br>");

        emit_escaped(render, lines[i]);
    }
}

static void
html_emph(Render *render, gboolean enter)
{
    emit(render, enter ? "<em>" : "</em>");
}

static void
html_strong(Render *render, gboolean enter)
{
    emit(render, enter ? "<strong>" : "</strong>");
}

static void
html_code(Render *render, const gchar *literal)
{
    emit(render, "<code>");
    emit_escaped(render, literal);
    emit(render, "</code>");
}

static void
html_link(Render *render, gboolean enter, const gchar *url)
{
    if (enter) {
        emit(render, "<span class=\"md-link\">");
        return;
    }

    emit(render, "</span>");

    if (url != NULL && url[0] != '\0') {
        emit(render, "<span class=\"md-url\"> (");
        emit_escaped(render, url);
        emit(render, ")</span>");
    }
}

static void
html_newline(Render *render)
{
    /*
     * A break, not a collapsed space.  CommonMark folds a soft break to
     * whitespace; the GTK client keeps the line break the writer typed,
     * and two clients disagreeing about where a message's lines are is
     * the kind of difference nobody reports and everybody notices.
     */
    emit(render, "<br>\n");
}

static void
html_paragraph(Render *render, gboolean enter, gboolean in_item)
{
    /*
     * No <p> directly inside a list item, which keeps a list tight --
     * and keeps it looking like the GTK one, where a paragraph in an
     * item continues the line the marker is on.
     */
    if (in_item)
        return;

    emit(render, enter ? "<p>" : "</p>\n");
}

static void
html_heading(Render *render, gboolean enter, gint level)
{
    gchar tag[8];

    if (level < 1)
        level = 1;
    else if (level > 6)
        level = 6;

    /*
     * Two calls rather than a ternary format.  -Wformat=2 refuses a
     * non-literal format string, and it is right to: this one is ours
     * today and is one edit from being somebody else's.
     */
    if (enter)
        g_snprintf(tag, sizeof(tag), "<h%d>", level);
    else
        g_snprintf(tag, sizeof(tag), "</h%d>", level);

    emit(render, tag);

    if (!enter)
        emit(render, "\n");
}

static void
html_quote(Render *render, gboolean enter)
{
    emit(render, enter ? "<blockquote>\n" : "</blockquote>\n");
}

static void
html_list(Render *render, gboolean enter, gboolean ordered, gint start)
{
    if (!enter) {
        emit(render, ordered ? "</ol>\n" : "</ul>\n");
        return;
    }

    if (!ordered) {
        emit(render, "<ul>\n");
        return;
    }

    if (start > 1) {
        g_autofree gchar *open = g_strdup_printf("<ol start=\"%d\">\n", start);

        emit(render, open);
    } else {
        emit(render, "<ol>\n");
    }
}

static void
html_item(Render *render, gboolean enter, gboolean first)
{
    (void)first;

    emit(render, enter ? "<li>" : "</li>\n");
}

static void
html_code_block(Render *render, const gchar *literal)
{
    emit(render, "<pre><code>");
    emit_escaped(render, literal);
    emit(render, "</code></pre>\n");
}

static void
html_html_block(Render *render, const gchar *literal)
{
    emit(render, "<p>");
    html_text_lines(render, literal);
    emit(render, "</p>\n");
}

static void
html_rule(Render *render)
{
    emit(render, "<hr>\n");
}

static const gchar *
html_align_attr(CellAlign align)
{
    if (align == CELL_ALIGN_CENTER)
        return " class=\"md-c\"";

    if (align == CELL_ALIGN_RIGHT)
        return " class=\"md-r\"";

    return "";
}

/*
 * A real table, at any width.
 *
 * The Pango vocabulary has to choose between a grid and a record layout
 * because a GtkLabel is one paragraph of text and a grid that wraps is
 * ruined.  A browser has neither problem: the wrapper scrolls sideways
 * inside the message, so the table stays a table however wide it is and
 * the column beside it does not move.
 */
static void
html_table(Render *render, GPtrArray *rows, GArray *aligns)
{
    guint cols = aligns->len;
    guint r;
    guint c;

    emit(render, "<div class=\"md-table\"><table>\n");

    for (r = 0; r < rows->len; r++) {
        GPtrArray *row = g_ptr_array_index(rows, r);

        if (r == 0)
            emit(render, "<thead>\n");
        else if (r == 1)
            emit(render, "<tbody>\n");

        emit(render, "<tr>");

        for (c = 0; c < cols; c++) {
            /* Padded when short and truncated when long, as GFM does. */
            const gchar *source =
                c < row->len ? g_ptr_array_index(row, c) : "";
            g_autofree gchar *cell = NULL;
            gsize width = 0;

            cell = render_cell(source, render, &width);

            emit(render, r == 0 ? "<th" : "<td");
            emit(render, html_align_attr(g_array_index(aligns, CellAlign, c)));
            emit(render, ">");
            emit_rendered(render, cell);
            emit(render, r == 0 ? "</th>" : "</td>");
        }

        emit(render, "</tr>\n");

        if (r == 0)
            emit(render, "</thead>\n");
    }

    if (rows->len > 1)
        emit(render, "</tbody>\n");

    emit(render, "</table></div>\n");
}

static const RenderOps html_ops = {
    html_text,
    html_emph,
    html_strong,
    html_code,
    html_link,
    html_newline,
    html_paragraph,
    html_heading,
    html_quote,
    html_list,
    html_item,
    html_code_block,
    html_html_block,
    html_rule,
    html_table
};

/* ── The walk ────────────────────────────────────────────────────── */

/*
 * A block's literal with its line breaks folded to spaces.
 *
 * Only reached inside a table cell, and only because cmark decides what
 * is a block from the text rather than from the context: a cell whose
 * content starts with `<` is an HTML *block*, and one indented four
 * spaces is a code block, however inline they look sitting between two
 * pipes.  Both literals end in a newline, and a newline in a cell puts
 * one row on two lines -- which in a Pango grid moves every column
 * after it and in a table cell is simply wrong.
 *
 * Returns: (transfer full): the literal, on one line
 */
static gchar *
flatten_literal(cmark_node *node)
{
    const gchar *literal = cmark_node_get_literal(node);
    gchar *flat = g_strdup(literal != NULL ? literal : "");

    g_strdelimit(flat, "\n", ' ');

    return g_strchomp(flat);
}

static void
enter_node(Render *render, cmark_node *node)
{
    const RenderOps *ops = render->ops;

    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
        if (!render->inlines_only)
            ops->paragraph(render, TRUE,
                           cmark_node_parent(node) != NULL &&
                           cmark_node_get_type(cmark_node_parent(node))
                               == CMARK_NODE_ITEM);
        break;

    case CMARK_NODE_HEADING:
        if (!render->inlines_only)
            ops->heading(render, TRUE, cmark_node_get_heading_level(node));
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        if (!render->inlines_only)
            ops->quote(render, TRUE);
        break;

    case CMARK_NODE_LIST:
        if (!render->inlines_only)
            ops->list(render, TRUE,
                      cmark_node_get_list_type(node) == CMARK_ORDERED_LIST,
                      cmark_node_get_list_start(node));
        break;

    case CMARK_NODE_ITEM:
        if (!render->inlines_only)
            ops->item(render, TRUE, cmark_node_previous(node) == NULL);
        break;

    case CMARK_NODE_CODE_BLOCK:
        /*
         * A fenced block cannot occur inside a table cell, but a cell
         * indented by four spaces parses as one -- and a block there
         * would put a newline in a row.  Inline code says the same
         * thing without leaving the line.
         */
        if (render->inlines_only) {
            g_autofree gchar *literal = flatten_literal(node);

            ops->code(render, literal);
            break;
        }

        ops->code_block(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_THEMATIC_BREAK:
        if (!render->inlines_only)
            ops->rule(render);
        break;

    case CMARK_NODE_TEXT:
        ops->text(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_EMPH:
        ops->emph(render, TRUE);
        break;

    case CMARK_NODE_STRONG:
        ops->strong(render, TRUE);
        break;

    case CMARK_NODE_CODE:
        ops->code(render, cmark_node_get_literal(node));
        break;

    /*
     * HTML in the source is text, always.  cmark hands it over
     * unparsed, and the whole point of this file is that nothing an
     * agent writes reaches a markup parser -- which for the web client
     * is the difference between showing somebody a <script> tag and
     * running it.
     */
    case CMARK_NODE_HTML_BLOCK:
        if (render->inlines_only) {
            g_autofree gchar *literal = flatten_literal(node);

            ops->text(render, literal);
        } else {
            ops->html_block(render, cmark_node_get_literal(node));
        }
        break;

    case CMARK_NODE_HTML_INLINE:
        ops->text(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_SOFTBREAK:
    case CMARK_NODE_LINEBREAK:
        /*
         * The line break the writer typed, rather than the space
         * CommonMark folds it to -- a person or a model writing a chat
         * message means the break.  Inside a cell it has to be a space:
         * a newline there would put a row on two lines.
         */
        if (render->inlines_only)
            ops->text(render, " ");
        else
            ops->newline(render);
        break;

    case CMARK_NODE_LINK:
    case CMARK_NODE_IMAGE:
        ops->link(render, TRUE, NULL);
        break;

    default:
        break;
    }
}

static void
exit_node(Render *render, cmark_node *node)
{
    const RenderOps *ops = render->ops;

    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
        if (!render->inlines_only)
            ops->paragraph(render, FALSE,
                           cmark_node_parent(node) != NULL &&
                           cmark_node_get_type(cmark_node_parent(node))
                               == CMARK_NODE_ITEM);
        break;

    case CMARK_NODE_HEADING:
        if (!render->inlines_only)
            ops->heading(render, FALSE, cmark_node_get_heading_level(node));
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        if (!render->inlines_only)
            ops->quote(render, FALSE);
        break;

    case CMARK_NODE_LIST:
        if (!render->inlines_only)
            ops->list(render, FALSE,
                      cmark_node_get_list_type(node) == CMARK_ORDERED_LIST,
                      cmark_node_get_list_start(node));
        break;

    case CMARK_NODE_ITEM:
        if (!render->inlines_only)
            ops->item(render, FALSE, cmark_node_previous(node) == NULL);
        break;

    case CMARK_NODE_EMPH:
        ops->emph(render, FALSE);
        break;

    case CMARK_NODE_STRONG:
        ops->strong(render, FALSE);
        break;

    case CMARK_NODE_LINK:
    case CMARK_NODE_IMAGE:
        ops->link(render, FALSE, cmark_node_get_url(node));
        break;

    default:
        break;
    }
}

/* One cmark parse, walked into @render. */
static void
render_markdown(Render *render, const gchar *markdown)
{
    cmark_node *document;
    cmark_iter *iter;
    cmark_event_type event;

    if (markdown == NULL || markdown[0] == '\0')
        return;

    /*
     * CMARK_OPT_SAFE would strip raw HTML for us, but it replaces it
     * with a comment rather than showing it -- and a person reading an
     * agent's message should see what the agent actually wrote. It is
     * escaped instead.
     */
    document = cmark_parse_document(markdown, strlen(markdown),
                                    CMARK_OPT_DEFAULT);

    if (document == NULL) {
        render->ops->text(render, markdown);
        return;
    }

    iter = cmark_iter_new(document);

    while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node *node = cmark_iter_get_node(iter);

        if (event == CMARK_EVENT_ENTER)
            enter_node(render, node);
        else if (event == CMARK_EVENT_EXIT)
            exit_node(render, node);
    }

    cmark_iter_free(iter);
    cmark_node_free(document);
}

/*
 * Renders one cell, reporting how wide it draws.
 *
 * The width is the Pango table's; the HTML one ignores it, because a
 * browser measures its own columns.
 *
 * Returns: (transfer full): the cell's markup, in the parent's own
 *   vocabulary
 */
static gchar *
render_cell(const gchar *source, const Render *parent, gsize *out_width)
{
    Render cell;

    cell.ops = parent->ops;
    cell.out = g_string_new(NULL);
    cell.lists = g_array_new(FALSE, FALSE, sizeof(ListLevel));
    cell.quote = 0;
    cell.code_open = parent->code_open;
    cell.code_close = parent->code_close;
    cell.blank = FALSE;
    cell.inlines_only = TRUE;
    cell.visible = 0;

    render_markdown(&cell, source);

    g_array_unref(cell.lists);
    *out_width = cell.visible;

    return g_string_free(cell.out, FALSE);
}

/* ── Tables ──────────────────────────────────────────────────────── */

/*
 * cmark is the reference CommonMark parser, and CommonMark has no
 * tables -- they are GitHub's extension, and reach C only through
 * cmark-gfm, which Fedora does not package and which exports the same
 * `cmark_*` symbols as cmark.  Linking both into one process, where
 * libreclaw already brings cmark, is a collision the dynamic linker
 * resolves silently and in whichever direction it likes.
 *
 * So the source is split before it is parsed: a table block is found
 * here and handed to the vocabulary's table callback, and everything
 * either side of it goes to cmark as it always did.  Each cell is then
 * a cmark parse of its own, which is what keeps the rule this file
 * exists for -- a cell's text reaches the output through the text
 * callback like every other literal.
 */

/*
 * Splits one row into its cells.
 *
 * Returns: (transfer full) (nullable) (element-type utf8): the cells,
 *   or %NULL when the line is not a row at all, which is what ends a
 *   table
 */
static GPtrArray *
split_row(const gchar *line)
{
    GPtrArray *cells;
    GString *cell;
    const gchar *p;
    const gchar *end;
    gboolean piped = FALSE;

    cells = g_ptr_array_new_with_free_func(g_free);
    cell = g_string_new(NULL);

    p = line;

    if (*p == '|') {
        piped = TRUE;
        p++;
    }

    for (; *p != '\0'; p++) {
        /*
         * GFM replaces an escaped pipe before the cell is parsed as
         * inlines, rather than leaving the backslash for the inline
         * parser: an escape does not work inside a code span, so
         * `a\|b` in a cell has to reach cmark as `a|b`.
         */
        if (*p == '\\' && *(p + 1) == '|') {
            g_string_append_c(cell, '|');
            p++;
            continue;
        }

        if (*p == '|') {
            piped = TRUE;
            g_ptr_array_add(cells, g_strstrip(g_string_free(cell, FALSE)));
            cell = g_string_new(NULL);
            continue;
        }

        g_string_append_c(cell, *p);
    }

    /* The trailing pipe is optional, so its empty last cell is not one. */
    end = line + strlen(line);

    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    if (!(end > line && end[-1] == '|' && cell->len == 0))
        g_ptr_array_add(cells, g_strstrip(g_string_free(cell, FALSE)));
    else
        g_string_free(cell, TRUE);

    if (!piped || cells->len == 0) {
        g_ptr_array_unref(cells);
        return NULL;
    }

    return cells;
}

/* Whether @text is `---`, `:--`, `--:` or `:-:`, and which way it leans. */
static gboolean
cell_is_delimiter(const gchar *text, CellAlign *out_align)
{
    const gchar *p = text;
    gboolean left = FALSE;
    gboolean right = FALSE;
    gsize dashes = 0;

    if (*p == ':') {
        left = TRUE;
        p++;
    }

    while (*p == '-') {
        dashes++;
        p++;
    }

    if (*p == ':') {
        right = TRUE;
        p++;
    }

    if (*p != '\0' || dashes == 0)
        return FALSE;

    if (left && right)
        *out_align = CELL_ALIGN_CENTER;
    else if (right)
        *out_align = CELL_ALIGN_RIGHT;
    else
        *out_align = CELL_ALIGN_LEFT;

    return TRUE;
}

/*
 * Returns: (transfer full) (nullable) (element-type CellAlign): one
 *   alignment per column, or %NULL when @line is not a delimiter row
 */
static GArray *
parse_delimiter(const gchar *line)
{
    g_autoptr(GPtrArray) cells = split_row(line);
    GArray *aligns;
    guint i;

    if (cells == NULL)
        return NULL;

    aligns = g_array_new(FALSE, FALSE, sizeof(CellAlign));

    for (i = 0; i < cells->len; i++) {
        CellAlign align = CELL_ALIGN_LEFT;

        if (!cell_is_delimiter(g_ptr_array_index(cells, i), &align)) {
            g_array_unref(aligns);
            return NULL;
        }

        g_array_append_val(aligns, align);
    }

    return aligns;
}

/*
 * Whether @lines[@i] is a header row and @lines[@i + 1] its delimiter.
 *
 * Column zero only.  Up to three spaces would be GFM's rule, but a
 * table written under a list item is indented by two, and treating that
 * one as top level would cut the list in half around it.  An indented
 * table stays as it renders today, which is no worse than it was.
 */
static gboolean
table_starts_at(gchar **lines, gsize i, GPtrArray **out_header,
                GArray **out_aligns)
{
    GPtrArray *header;
    GArray *aligns;

    *out_header = NULL;
    *out_aligns = NULL;

    if (lines[i] == NULL || lines[i + 1] == NULL)
        return FALSE;

    if (lines[i][0] == ' ' || lines[i][0] == '\t' ||
        lines[i + 1][0] == ' ' || lines[i + 1][0] == '\t')
        return FALSE;

    header = split_row(lines[i]);

    if (header == NULL)
        return FALSE;

    aligns = parse_delimiter(lines[i + 1]);

    /*
     * GFM does not recognise a table whose delimiter row has a
     * different number of cells from its header, and neither do we:
     * guessing which one meant what would draw a grid nobody wrote.
     */
    if (aligns == NULL || aligns->len != header->len) {
        g_ptr_array_unref(header);

        if (aligns != NULL)
            g_array_unref(aligns);

        return FALSE;
    }

    *out_header = header;
    *out_aligns = aligns;

    return TRUE;
}

/* ── The document ────────────────────────────────────────────────── */

/*
 * Whether @line opens or closes a fenced code block, and with what.
 *
 * Tracked because a table inside a fence is not a table: it is what
 * somebody is showing you the source of, and drawing it would be the
 * one place a reader is certain to notice.
 */
static gboolean
is_fence(const gchar *line, gchar *out_char, gsize *out_len)
{
    const gchar *p = line;
    gsize indent = 0;
    gsize run = 0;

    while (*p == ' ' && indent < 4) {
        indent++;
        p++;
    }

    if (indent >= 4 || (*p != '`' && *p != '~'))
        return FALSE;

    *out_char = *p;

    while (*p == *out_char) {
        run++;
        p++;
    }

    if (run < 3)
        return FALSE;

    *out_len = run;

    return TRUE;
}

static void
flush_prose(Render *render, GString *prose)
{
    if (prose->len == 0)
        return;

    render_markdown(render, prose->str);
    g_string_truncate(prose, 0);
}

static void
append_line(GString *prose, const gchar *line)
{
    if (prose->len > 0)
        g_string_append_c(prose, '\n');

    g_string_append(prose, line);
}

static void
render_document(Render *render, const gchar *markdown)
{
    g_auto(GStrv) lines = NULL;
    g_autoptr(GString) prose = NULL;
    gchar fence = 0;
    gsize fence_len = 0;
    gsize i;

    lines = g_strsplit(markdown, "\n", -1);
    prose = g_string_new(NULL);

    for (i = 0; lines[i] != NULL; i++) {
        GPtrArray *header = NULL;
        GArray *aligns = NULL;
        gchar opener = 0;
        gsize len = 0;

        if (fence != 0) {
            if (is_fence(lines[i], &opener, &len) &&
                opener == fence && len >= fence_len)
                fence = 0;

            append_line(prose, lines[i]);
            continue;
        }

        if (is_fence(lines[i], &opener, &len)) {
            fence = opener;
            fence_len = len;
            append_line(prose, lines[i]);
            continue;
        }

        if (table_starts_at(lines, i, &header, &aligns)) {
            g_autoptr(GPtrArray) rows =
                g_ptr_array_new_with_free_func(
                    (GDestroyNotify)g_ptr_array_unref);
            gsize r;

            flush_prose(render, prose);
            g_ptr_array_add(rows, header);

            for (r = i + 2; lines[r] != NULL; r++) {
                GPtrArray *row;

                if (lines[r][0] == '\0' || lines[r][0] == ' ' ||
                    lines[r][0] == '\t')
                    break;

                row = split_row(lines[r]);

                if (row == NULL)
                    break;

                g_ptr_array_add(rows, row);
            }

            render->ops->table(render, rows, aligns);
            g_array_unref(aligns);

            i = r - 1;
            continue;
        }

        append_line(prose, lines[i]);
    }

    flush_prose(render, prose);
}

/* ── The two entry points ────────────────────────────────────────── */

static void
render_init(Render *render, const RenderOps *ops)
{
    render->ops = ops;
    render->out = g_string_new(NULL);
    render->lists = g_array_new(FALSE, FALSE, sizeof(ListLevel));
    render->quote = 0;
    render->code_open = "<tt>";
    render->code_close = "</tt>";
    render->blank = FALSE;
    render->inlines_only = FALSE;
    render->visible = 0;
}

gchar *
clawt_markdown_to_pango_full(const gchar *markdown, const gchar *code_font)
{
    g_autofree gchar *code_open = NULL;
    Render render;

    if (markdown == NULL || markdown[0] == '\0')
        return g_strdup("");

    render_init(&render, &pango_ops);

    /*
     * The family is escaped as attribute text before it goes anywhere
     * near the markup.  It comes from a font chooser, so it is not
     * hostile -- but it is the one piece of this markup that is not a
     * literal, and the rule here is that nothing reaches the parser
     * unescaped, whoever wrote it.
     */
    if (code_font != NULL && *code_font != '\0') {
        g_autofree gchar *escaped = g_markup_escape_text(code_font, -1);

        code_open = g_strdup_printf("<span font_family=\"%s\">", escaped);
        render.code_open = code_open;
        render.code_close = "</span>";
    }

    render_document(&render, markdown);

    g_array_unref(render.lists);

    /* Trailing blank lines are an artefact of block separation. */
    while (render.out->len > 0 &&
           render.out->str[render.out->len - 1] == '\n')
        g_string_truncate(render.out, render.out->len - 1);

    return g_string_free(render.out, FALSE);
}

gchar *
clawt_markdown_to_pango(const gchar *markdown)
{
    return clawt_markdown_to_pango_full(markdown, NULL);
}

gchar *
clawt_markdown_to_html(const gchar *markdown)
{
    Render render;

    if (markdown == NULL || markdown[0] == '\0')
        return g_strdup("");

    render_init(&render, &html_ops);
    render_document(&render, markdown);
    g_array_unref(render.lists);

    while (render.out->len > 0 &&
           render.out->str[render.out->len - 1] == '\n')
        g_string_truncate(render.out, render.out->len - 1);

    return g_string_free(render.out, FALSE);
}
