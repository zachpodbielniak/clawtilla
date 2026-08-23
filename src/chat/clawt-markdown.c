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
} Render;

/* Every literal goes through here. There is no other path to the output. */
static void
put_text(Render *render, const gchar *text)
{
    g_autofree gchar *escaped = NULL;

    if (text == NULL)
        return;

    escaped = g_markup_escape_text(text, -1);
    g_string_append(render->out, escaped);
}

static void
put_markup(Render *render, const gchar *markup)
{
    g_string_append(render->out, markup);
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

    for (i = 0; i < render->quote; i++)
        put_markup(render, DIM_OPEN "\xe2\x96\x8f " DIM_CLOSE);

    /*
     * Two spaces per enclosing list, minus the one the item marker
     * itself occupies -- so a nested bullet lines up under its parent's
     * text rather than under its parent's bullet.
     */
    for (i = 1; i < (gint)render->lists->len; i++)
        g_string_append(render->out, "    ");
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
        put_markup(render, DIM_OPEN "  " DIM_CLOSE);
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
        put_markup(render, "\xe2\x80\xa2 ");
        return;
    }

    level = &g_array_index(render->lists, ListLevel, render->lists->len - 1);

    if (level->ordered) {
        g_string_append_printf(render->out, "%d. ", level->next);
        level->next++;
    } else {
        put_markup(render, "\xe2\x80\xa2 ");
    }
}

static void
enter_node(Render *render, cmark_node *node)
{
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
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
        begin_block(render);
        put_markup(render, "<b><big>");
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        render->quote++;
        break;

    case CMARK_NODE_LIST: {
        ListLevel level;

        level.ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
        level.next = cmark_node_get_list_start(node);

        if (level.next < 1)
            level.next = 1;

        g_array_append_val(render->lists, level);
        break;
    }

    case CMARK_NODE_ITEM:
        begin_block_full(render, cmark_node_previous(node) == NULL);
        put_list_marker(render);
        break;

    case CMARK_NODE_CODE_BLOCK:
        put_code_block(render, cmark_node_get_literal(node));
        break;

    case CMARK_NODE_THEMATIC_BREAK:
        begin_block(render);
        put_markup(render, DIM_OPEN
                   "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                   "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                   DIM_CLOSE);
        end_block(render);
        break;

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
         */
        g_string_append_c(render->out, '\n');
        break;

    case CMARK_NODE_LINEBREAK:
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
        end_block(render);
        break;

    case CMARK_NODE_HEADING:
        put_markup(render, "</big></b>");
        end_block(render);
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        render->quote--;
        break;

    case CMARK_NODE_LIST:
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
            put_markup(render, " " DIM_OPEN "(");
            put_text(render, url);
            put_markup(render, ")" DIM_CLOSE);
        }

        break;
    }

    default:
        break;
    }
}

gchar *
clawt_markdown_to_pango_full(const gchar *markdown, const gchar *code_font)
{
    cmark_node *document;
    cmark_iter *iter;
    cmark_event_type event;
    g_autofree gchar *code_open = NULL;
    Render render;

    if (markdown == NULL || markdown[0] == '\0')
        return g_strdup("");

    render.out = g_string_new(NULL);
    render.lists = g_array_new(FALSE, FALSE, sizeof(ListLevel));
    render.quote = 0;
    render.blank = FALSE;

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

    /*
     * CMARK_OPT_SAFE would strip raw HTML for us, but it replaces it
     * with a comment rather than showing it -- and a person reading an
     * agent's message should see what the agent actually wrote. It is
     * escaped instead.
     */
    document = cmark_parse_document(markdown, strlen(markdown),
                                    CMARK_OPT_DEFAULT);

    if (document == NULL) {
        g_array_unref(render.lists);
        return g_markup_escape_text(markdown, -1);
    }

    iter = cmark_iter_new(document);

    while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node *node = cmark_iter_get_node(iter);

        if (event == CMARK_EVENT_ENTER)
            enter_node(&render, node);
        else if (event == CMARK_EVENT_EXIT)
            exit_node(&render, node);
    }

    cmark_iter_free(iter);
    cmark_node_free(document);
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
