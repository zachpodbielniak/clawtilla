/*
 * clawt-markdown.c - Rendering what a model wrote, without obeying it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
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

typedef struct {
    GString  *out;
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
     * construct inside one would put a newline into a row and take the
     * grid apart.  cmark has no inline-only parse, so the block cases
     * suppress their own structure here and let their text through.
     */
    gboolean  inlines_only;

    /*
     * How wide what has been emitted draws, in monospace columns.
     *
     * A cell's markup length has nothing to do with the space it takes
     * on screen, so padding computed from it would put a grid out by
     * however many tags the cell happened to contain.  Counted here
     * because put_text() is the only door visible characters go
     * through -- which is why the markers and the rules below hand
     * their literals to it rather than writing them as markup.
     */
    gsize     visible;
} Render;

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

/* Every literal goes through here. There is no other path to the output. */
static void
put_text(Render *render, const gchar *text)
{
    g_autofree gchar *escaped = NULL;

    if (text == NULL)
        return;

    escaped = g_markup_escape_text(text, -1);
    g_string_append(render->out, escaped);
    render->visible += display_width(text);
}

static void
put_markup(Render *render, const gchar *markup)
{
    g_string_append(render->out, markup);
}

/*
 * Markup a Render has already produced.
 *
 * A table cell is rendered on its own so its width can be measured, and
 * what comes back is markup rather than a literal -- every piece of text
 * inside it went through put_text() in that cell's own render, so the
 * escaping rule holds and escaping it again would show the tags.
 */
static void
put_rendered(Render *render, const gchar *markup)
{
    g_string_append(render->out, markup);
}

static void
put_spaces(Render *render, gsize count)
{
    g_autofree gchar *pad = g_strnfill(count, ' ');

    put_text(render, pad);
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

/*
 * Starts a block on its own line, indented for any list it is inside
 * and marked for any quote.
 */
static void
begin_block_full(Render *render, gboolean spaced)
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
        put_markup(render, DIM_OPEN);
        put_text(render, "\xe2\x96\x8f ");
        put_markup(render, DIM_CLOSE);
    }

    /*
     * Two spaces per enclosing list, minus the one the item marker
     * itself occupies -- so a nested bullet lines up under its parent's
     * text rather than under its parent's bullet.
     */
    for (i = 1; i < (gint)render->lists->len; i++)
        put_text(render, "    ");
}

static void
begin_block(Render *render)
{
    begin_block_full(render, TRUE);
}

static void
end_block(Render *render)
{
    g_string_append_c(render->out, '\n');
    render->blank = TRUE;
}

/*
 * A fenced or indented code block.
 *
 * Every line is prefixed, so the monospace run is unbroken and a quote
 * or list marker still lands at the start of each line.
 */
static void
put_code_block(Render *render, const gchar *literal)
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

        begin_block_full(render, i == 0);
        put_markup(render, render->code_open);
        put_markup(render, DIM_OPEN);
        put_text(render, "  ");
        put_markup(render, DIM_CLOSE);
        put_text(render, lines[i]);
        put_markup(render, render->code_close);
    }

    end_block(render);
}

static void
put_list_marker(Render *render)
{
    ListLevel *level;

    if (render->lists->len == 0) {
        put_text(render, "\xe2\x80\xa2 ");
        return;
    }

    level = &g_array_index(render->lists, ListLevel, render->lists->len - 1);

    if (level->ordered) {
        g_autofree gchar *marker = g_strdup_printf("%d. ", level->next);

        put_text(render, marker);
        level->next++;
    } else {
        put_text(render, "\xe2\x80\xa2 ");
    }
}

static void
enter_node(Render *render, cmark_node *node)
{
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
        if (render->inlines_only)
            break;

        /*
         * A paragraph directly inside a list item continues the line the
         * marker is on; anywhere else it starts its own.
         */
        if (cmark_node_parent(node) == NULL ||
            cmark_node_get_type(cmark_node_parent(node)) != CMARK_NODE_ITEM)
            begin_block(render);
        else
            render->blank = FALSE;
        break;

    case CMARK_NODE_HEADING:
        if (render->inlines_only)
            break;

        begin_block(render);
        put_markup(render, "<b><big>");
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        if (render->inlines_only)
            break;

        render->quote++;
        break;

    case CMARK_NODE_LIST: {
        ListLevel level;

        if (render->inlines_only)
            break;

        level.ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
        level.next = cmark_node_get_list_start(node);

        if (level.next < 1)
            level.next = 1;

        g_array_append_val(render->lists, level);
        break;
    }

    case CMARK_NODE_ITEM:
        if (render->inlines_only)
            break;

        begin_block_full(render, cmark_node_previous(node) == NULL);
        put_list_marker(render);
        break;

    case CMARK_NODE_CODE_BLOCK:
        if (render->inlines_only) {
            g_autofree gchar *literal =
                g_strdup(cmark_node_get_literal(node) != NULL
                         ? cmark_node_get_literal(node) : "");

            g_strdelimit(literal, "\n", ' ');
            g_strchomp(literal);

            put_markup(render, render->code_open);
            put_text(render, literal);
            put_markup(render, render->code_close);
            break;
        }

        put_code_block(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_THEMATIC_BREAK: {
        if (render->inlines_only)
            break;

        begin_block(render);
        put_markup(render, DIM_OPEN);
        put_text(render, TABLE_RULE_UNIT TABLE_RULE_UNIT TABLE_RULE_UNIT
                         TABLE_RULE_UNIT TABLE_RULE_UNIT TABLE_RULE_UNIT
                         TABLE_RULE_UNIT TABLE_RULE_UNIT);
        put_markup(render, DIM_CLOSE);
        end_block(render);
        break;
    }

    case CMARK_NODE_TEXT:
        put_text(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_EMPH:
        put_markup(render, "<i>");
        break;

    case CMARK_NODE_STRONG:
        put_markup(render, "<b>");
        break;

    case CMARK_NODE_CODE:
        put_markup(render, render->code_open);
        put_text(render, cmark_node_get_literal(node));
        put_markup(render, render->code_close);
        break;

    /*
     * HTML in the source is text, always.  cmark hands it over
     * unparsed, and the whole point of this file is that nothing an
     * agent writes reaches a markup parser.
     */
    case CMARK_NODE_HTML_BLOCK:
        if (render->inlines_only) {
            put_text(render, cmark_node_get_literal(node));
            break;
        }

        begin_block(render);
        put_text(render, cmark_node_get_literal(node));
        end_block(render);
        break;

    case CMARK_NODE_HTML_INLINE:
        put_text(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_SOFTBREAK:
        /*
         * Kept as a newline rather than folded to a space.  CommonMark
         * says a space, but a person or a model writing a chat message
         * means the line break they typed.
         *
         * Inside a cell it has to be a space: a newline there would put
         * a row on two lines and every column after it out of place.
         */
        if (render->inlines_only)
            put_text(render, " ");
        else
            g_string_append_c(render->out, '\n');
        break;

    case CMARK_NODE_LINEBREAK:
        if (render->inlines_only)
            put_text(render, " ");
        else
            g_string_append_c(render->out, '\n');
        break;

    case CMARK_NODE_LINK:
    case CMARK_NODE_IMAGE:
        put_markup(render, "<u>");
        break;

    default:
        break;
    }
}

static void
exit_node(Render *render, cmark_node *node)
{
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
        if (render->inlines_only)
            break;

        end_block(render);
        break;

    case CMARK_NODE_HEADING:
        if (render->inlines_only)
            break;

        put_markup(render, "</big></b>");
        end_block(render);
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        if (render->inlines_only)
            break;

        render->quote--;
        break;

    case CMARK_NODE_LIST:
        if (render->inlines_only)
            break;

        if (render->lists->len > 0)
            g_array_remove_index(render->lists, render->lists->len - 1);
        break;

    case CMARK_NODE_EMPH:
        put_markup(render, "</i>");
        break;

    case CMARK_NODE_STRONG:
        put_markup(render, "</b>");
        break;

    case CMARK_NODE_LINK:
    case CMARK_NODE_IMAGE: {
        const gchar *url = cmark_node_get_url(node);

        put_markup(render, "</u>");

        /*
         * The target beside the text, not behind it.  A clickable link
         * in model output is one keystroke between a prompt injection
         * and a browser, and a person who can see where it goes is a
         * person who can decide.
         */
        if (url != NULL && url[0] != '\0') {
            put_markup(render, DIM_OPEN);
            put_text(render, " (");
            put_text(render, url);
            put_text(render, ")");
            put_markup(render, DIM_CLOSE);
        }

        break;
    }

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
        put_text(render, markdown);
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
 * and drawn here, and everything either side of it goes to cmark as it
 * always did.  Each cell is then a cmark parse of its own, which is
 * what keeps the rule this file exists for -- a cell's text reaches the
 * output through put_text() like every other literal.
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

/*
 * Renders one cell, reporting how wide it draws.
 *
 * Returns: (transfer full): the cell's markup
 */
static gchar *
render_cell(const gchar *source, const Render *parent, gsize *out_width)
{
    Render cell;

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

/* The dim rule under a table's header, one run per column. */
static void
put_column_rule(Render *render, GArray *columns)
{
    guint c;

    begin_block_full(render, FALSE);
    put_markup(render, render->code_open);
    put_markup(render, DIM_OPEN);

    for (c = 0; c < columns->len; c++) {
        g_autofree gchar *bar =
            repeat_utf8(TABLE_RULE_UNIT, g_array_index(columns, gsize, c));

        if (c > 0)
            put_spaces(render, 2);

        put_text(render, bar);
    }

    put_markup(render, DIM_CLOSE);
    put_markup(render, render->code_close);
}

/*
 * The grid: every cell padded to its column, in the code font.
 *
 * The code font is not decoration.  A proportional font gives every
 * glyph its own advance, so spaces line nothing up and the padding is
 * wasted -- a grid only exists because every cell is the same width.
 */
static void
put_table_grid(Render *render, GPtrArray *rows, GArray *aligns,
               GPtrArray *markup, GArray *widths, GArray *columns)
{
    guint cols = aligns->len;
    guint r;
    guint c;

    for (r = 0; r < rows->len; r++) {
        begin_block_full(render, r == 0);
        put_markup(render, render->code_open);

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
                put_spaces(render, 2);

            put_spaces(render, lead);

            if (r == 0)
                put_markup(render, "<b>");

            put_rendered(render, g_ptr_array_index(markup, r * cols + c));

            if (r == 0)
                put_markup(render, "</b>");

            /* Nothing follows the last column, so nothing pads it. */
            if (c + 1 < cols)
                put_spaces(render, pad - lead);
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

        put_markup(render, render->code_close);

        if (r == 0)
            put_column_rule(render, columns);
    }

    end_block(render);
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
put_table_records(Render *render, GPtrArray *rows, guint cols,
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

                begin_block_full(render, FALSE);
                put_markup(render, DIM_OPEN);
                put_text(render, bar);
                put_markup(render, DIM_CLOSE);
                separator = FALSE;
                drawn++;
            }

            begin_block_full(render, drawn == 0);
            drawn++;

            if (label[0] != '\0') {
                put_markup(render, "<b>");
                put_rendered(render, label);
                put_markup(render, "</b>");
                put_text(render, ": ");
            }

            put_rendered(render, value);
        }

        if (drawn > 0)
            separator = TRUE;
    }

    if (drawn > 0)
        end_block(render);
}

static void
render_table(Render *render, GPtrArray *rows, GArray *aligns)
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
        put_table_grid(render, rows, aligns, markup, widths, columns);
    else
        put_table_records(render, rows, cols, markup);
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

            render_table(render, rows, aligns);
            g_array_unref(aligns);

            i = r - 1;
            continue;
        }

        append_line(prose, lines[i]);
    }

    flush_prose(render, prose);
}

gchar *
clawt_markdown_to_pango_full(const gchar *markdown, const gchar *code_font)
{
    g_autofree gchar *code_open = NULL;
    Render render;

    if (markdown == NULL || markdown[0] == '\0')
        return g_strdup("");

    render.out = g_string_new(NULL);
    render.lists = g_array_new(FALSE, FALSE, sizeof(ListLevel));
    render.quote = 0;
    render.blank = FALSE;
    render.inlines_only = FALSE;
    render.visible = 0;

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
    } else {
        render.code_open = "<tt>";
        render.code_close = "</tt>";
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
