/*
 * test-markdown.c - Rendering what a model wrote, without obeying it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

static void
test_the_things_a_chat_actually_uses(void)
{
    struct {
        const gchar *markdown;
        const gchar *expected;
    } cases[] = {
        { "**bold**",           "<b>bold</b>" },
        { "*italic*",           "<i>italic</i>" },
        { "_italic_",           "<i>italic</i>" },
        { "`inline code`",      "<tt>inline code</tt>" },
        { "plain",              "plain" },
        { "**bold** and `code`", "<b>bold</b> and <tt>code</tt>" }
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *rendered =
            clawt_markdown_to_pango(cases[i].markdown);

        g_assert_cmpstr(rendered, ==, cases[i].expected);
    }
}

static void
test_blocks_and_lists(void)
{
    g_autofree gchar *heading = clawt_markdown_to_pango("# Title");
    g_autofree gchar *bullets =
        clawt_markdown_to_pango("- one\n- two\n");
    g_autofree gchar *numbered =
        clawt_markdown_to_pango("1. first\n2. second\n");
    g_autofree gchar *fenced =
        clawt_markdown_to_pango("```\nmake test\n```\n");
    g_autofree gchar *quoted = clawt_markdown_to_pango("> quoted\n");

    g_assert_nonnull(strstr(heading, "<b><big>Title</big></b>"));

    g_assert_nonnull(strstr(bullets, "\xe2\x80\xa2 one"));
    g_assert_nonnull(strstr(bullets, "\xe2\x80\xa2 two"));

    /* Numbered lists count, and count from where the author started. */
    g_assert_nonnull(strstr(numbered, "1. first"));
    g_assert_nonnull(strstr(numbered, "2. second"));

    g_assert_nonnull(strstr(fenced, "<tt>"));
    g_assert_nonnull(strstr(fenced, "make test"));

    g_assert_nonnull(strstr(quoted, "quoted"));
    g_assert_nonnull(strstr(quoted, "\xe2\x96\x8f"));
}

static void
test_a_numbered_list_keeps_its_start(void)
{
    g_autofree gchar *rendered =
        clawt_markdown_to_pango("5. five\n6. six\n");

    g_assert_nonnull(strstr(rendered, "5. five"));
    g_assert_nonnull(strstr(rendered, "6. six"));
}

/*
 * The point of the whole file.
 *
 * Markup is emitted for the structure cmark found, and for nothing
 * else. Anything an agent writes is text, whatever it looks like --
 * there is no input that reaches a markup parser, so there is no input
 * that can close a tag, open one, or invent an entity.
 */
static void
test_nothing_an_agent_writes_becomes_markup(void)
{
    static const gchar *hostile[] = {
        "<b>not actually bold</b>",
        "<span foreground=\"red\">red</span>",
        "</b></span><b>",
        "5 &lt; 6 &amp;&amp; 7 &gt; 6",
        "a & b",
        "<img src=x>",
        "<a href=\"file:///etc/passwd\">click</a>",
        "&#x3c;script&#x3e;",
        NULL
    };
    gsize i;

    for (i = 0; hostile[i] != NULL; i++) {
        g_autofree gchar *rendered = clawt_markdown_to_pango(hostile[i]);

        /*
         * Every "<" that survives is one this file wrote. The agent's
         * own angle brackets and ampersands come out escaped, so what
         * a person sees is what the agent typed.
         */
        g_assert_null(strstr(rendered, "<b>not"));
        g_assert_null(strstr(rendered, "<span foreground"));
        g_assert_null(strstr(rendered, "<img"));
        g_assert_null(strstr(rendered, "<a href"));
        g_assert_null(strstr(rendered, "<script"));
    }

    /* And the literal text is still there to read. */
    {
        g_autofree gchar *rendered =
            clawt_markdown_to_pango("<b>not actually bold</b>");

        g_assert_nonnull(strstr(rendered, "&lt;b&gt;"));
        g_assert_nonnull(strstr(rendered, "not actually bold"));
    }

    {
        g_autofree gchar *rendered = clawt_markdown_to_pango("a & b");

        g_assert_nonnull(strstr(rendered, "&amp;"));
    }
}

/*
 * A link shows where it goes and is not clickable.
 *
 * A clickable link in model output is one keystroke between a prompt
 * injection and a browser.
 */
static void
test_links_show_their_target_and_do_not_open(void)
{
    g_autofree gchar *rendered =
        clawt_markdown_to_pango("[the docs](https://example.invalid/x)");

    g_assert_nonnull(strstr(rendered, "the docs"));
    g_assert_nonnull(strstr(rendered, "https://example.invalid/x"));

    /* No anchor: Pango would make it activatable. */
    g_assert_null(strstr(rendered, "<a "));
    g_assert_null(strstr(rendered, "href"));
}

/* Input that is not markdown at all still has to come out readable. */
static void
test_plain_and_empty_input(void)
{
    g_autofree gchar *empty = clawt_markdown_to_pango("");
    g_autofree gchar *null_input = clawt_markdown_to_pango(NULL);
    g_autofree gchar *lines =
        clawt_markdown_to_pango("first line\nsecond line");

    g_assert_cmpstr(empty, ==, "");
    g_assert_cmpstr(null_input, ==, "");

    /* A single newline is the line break the writer typed. */
    g_assert_cmpstr(lines, ==, "first line\nsecond line");
}

/*
 * Balanced tags, whatever the input.
 *
 * Pango refuses to parse unbalanced markup, and a GtkLabel handed
 * something it cannot parse renders nothing at all -- a message that
 * silently disappears is worse than one that renders plainly.
 */
static void
test_the_markup_is_always_balanced(void)
{
    static const gchar *inputs[] = {
        "**unclosed bold",
        "`unclosed code",
        "*a **b* c**",
        "> quote with `code` and **bold**\n> more",
        "- item with **bold**\n  - nested `code`\n",
        "# heading with *emphasis*",
        "```\nunclosed fence",
        "[link",
        "***",
        "\n\n\n",
        "| a | b |\n|---|---|\n| **x** | `y` |\n",
        "| a |\n|---|\n| *unclosed\n",
        "| h |\n|---|\n",
        "|",
        "|---|\n",
        NULL
    };
    gsize i;

    for (i = 0; inputs[i] != NULL; i++) {
        g_autofree gchar *rendered = clawt_markdown_to_pango(inputs[i]);
        const gchar *p = rendered;
        gint depth = 0;

        /*
         * Counted rather than parsed: every "<" this file emits opens a
         * tag and every ">" closes one, and the agent's own brackets
         * are escaped before they get here. So the counts must match
         * and must never go negative.
         */
        for (; *p != '\0'; p++) {
            if (*p == '<')
                depth++;
            else if (*p == '>')
                depth--;

            g_assert_cmpint(depth, >=, 0);
            g_assert_cmpint(depth, <=, 1);
        }

        g_assert_cmpint(depth, ==, 0);
    }
}

/* ── Tables ──────────────────────────────────────────────────────── */

/*
 * A table draws as a grid, in the code font.
 *
 * Asserted whole rather than by fragments.  Alignment is the entire
 * reason a table is a table, so a test that only looked for the cells
 * would pass on output where every column had drifted -- which is what
 * the raw pipes already did.
 *
 * The code font is not decoration either: a proportional font gives
 * every glyph its own advance, so the padding would line nothing up.
 */
static void
test_a_table_draws_a_grid(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "| Team  | Lead |\n"
        "|-------|------|\n"
        "| forge | oxpecker |\n");

    g_assert_cmpstr(rendered, ==,
        "<tt><b>Team</b>   <b>Lead</b></tt>\n"
        "<tt><span alpha=\"60%\">"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "  "
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "</span></tt>\n"
        "<tt>forge  oxpecker</tt>");
}

/* The delimiter row's colons decide which way a column leans. */
static void
test_a_column_leans_where_its_delimiter_says(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "| l | c | r |\n"
        "|:---|:---:|---:|\n"
        "| xxxx | xxxx | xxxx |\n");

    /* Four-wide columns, so a one-character heading has three to place. */
    g_assert_nonnull(strstr(rendered, "<b>l</b>   "));       /* all trailing */
    g_assert_nonnull(strstr(rendered, "  <b>c</b>  "));      /* split */
    g_assert_nonnull(strstr(rendered, "   <b>r</b></tt>"));  /* all leading */
}

/*
 * A column is padded to what its cells *draw*, not to what they weigh.
 *
 * A cell's markup has nothing to do with the space it takes on screen,
 * and neither has its length in bytes or in characters: `**a**` is one
 * column and eleven bytes of markup, and a CJK glyph is one character
 * and two columns.  Each of those three answers is a different grid,
 * and two of them are crooked.
 */
static void
test_a_column_is_padded_to_what_it_draws(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "| n | who |\n"
        "|---|-----|\n"
        "| 1 | \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e |\n"
        "| 22 | **a** |\n");

    /* The second column is six wide: three CJK glyphs at two each. */
    g_assert_nonnull(strstr(rendered,
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80</span>"));

    /* And the first is two, from "22" rather than from "1". */
    g_assert_nonnull(strstr(rendered, "<tt>1   \xe6\x97\xa5"));
    g_assert_nonnull(strstr(rendered, "<tt>22  <b>a</b></tt>"));
}

/*
 * A table wider than the column becomes records instead.
 *
 * This is the case a chat actually produces -- an agent summarising a
 * fleet writes a sentence per cell -- and a grid that wraps is worse
 * than no grid at all: the wrap lands in the middle of a row and every
 * column after it is somewhere else.  One `Header: value` line per cell
 * carries the same information at any width.
 */
static void
test_a_wide_table_becomes_records(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "| Team | Agents |\n"
        "|---|---|\n"
        "| forge | oxpecker reviews every merge request and refuses "
        "a warning |\n"
        "| qa | oryx holds the reference result on Fedora |\n");

    /* No grid: the code font is what a grid is drawn in. */
    g_assert_null(strstr(rendered, "<tt>"));

    g_assert_nonnull(strstr(rendered, "<b>Team</b>: forge"));
    g_assert_nonnull(strstr(rendered, "<b>Agents</b>: oxpecker reviews"));
    g_assert_nonnull(strstr(rendered, "<b>Team</b>: qa"));

    /* One rule between the two records, and none before the first. */
    g_assert_true(g_str_has_prefix(rendered, "<b>Team</b>"));
}

/*
 * What is not a table stays what it was.
 *
 * The delimiter row must have as many cells as the header, which is
 * GFM's rule and the thing keeping a paragraph that happens to contain
 * a pipe from being drawn as a grid nobody wrote.  A setext heading is
 * the sharp case: its underline is a row of dashes.
 */
static void
test_what_is_not_a_table_is_left_alone(void)
{
    g_autofree gchar *mismatched =
        clawt_markdown_to_pango("| a | b |\n|---|\n| 1 | 2 |\n");
    g_autofree gchar *heading = clawt_markdown_to_pango("Title\n-----\n");
    g_autofree gchar *prose =
        clawt_markdown_to_pango("either a | b, but not both\n");

    g_assert_null(strstr(mismatched, "<tt>"));
    g_assert_nonnull(strstr(mismatched, "| a | b |"));

    g_assert_nonnull(strstr(heading, "<b><big>Title</big></b>"));

    g_assert_null(strstr(prose, "<tt>"));
    g_assert_nonnull(strstr(prose, "either a | b"));
}

/*
 * A table inside a fence is source, and stays source.
 *
 * Somebody showing you the markdown for a table is the one reader
 * certain to notice it being drawn instead.
 */
static void
test_a_table_in_a_fence_is_still_source(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "```markdown\n"
        "| a | b |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "```\n");

    g_assert_nonnull(strstr(rendered, "|---|---|"));
    g_assert_null(strstr(rendered, "<b>a</b>"));
}

/*
 * An escaped pipe is a pipe, and it is unescaped before the cell is
 * parsed.
 *
 * GFM does it in that order because a backslash escape does not work
 * inside a code span, so `a \| b` in a cell has to reach cmark as
 * `a | b` -- leaving the backslash for the inline parser would show it.
 */
static void
test_an_escaped_pipe_stays_in_its_cell(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "| cmd |\n"
        "|---|\n"
        "| `a \\| b` |\n");

    g_assert_nonnull(strstr(rendered, "a | b"));
    g_assert_null(strstr(rendered, "\\|"));
}

/* A table sits between the blocks around it without swallowing them. */
static void
test_a_table_keeps_its_neighbours(void)
{
    g_autofree gchar *rendered = clawt_markdown_to_pango(
        "Here is the fleet:\n"
        "\n"
        "| a | b |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "\n"
        "- and a list after it\n");

    g_assert_true(g_str_has_prefix(rendered, "Here is the fleet:\n\n<tt>"));
    g_assert_nonnull(strstr(rendered, "\xe2\x80\xa2 and a list after it"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/markdown/inline", test_the_things_a_chat_actually_uses);
    g_test_add_func("/markdown/blocks", test_blocks_and_lists);
    g_test_add_func("/markdown/ordered-start",
                    test_a_numbered_list_keeps_its_start);
    g_test_add_func("/markdown/nothing-becomes-markup",
                    test_nothing_an_agent_writes_becomes_markup);
    g_test_add_func("/markdown/links-are-not-clickable",
                    test_links_show_their_target_and_do_not_open);
    g_test_add_func("/markdown/plain-and-empty", test_plain_and_empty_input);
    g_test_add_func("/markdown/always-balanced",
                    test_the_markup_is_always_balanced);

    g_test_add_func("/markdown/table-draws-a-grid", test_a_table_draws_a_grid);
    g_test_add_func("/markdown/table-alignment",
                    test_a_column_leans_where_its_delimiter_says);
    g_test_add_func("/markdown/table-padded-to-what-it-draws",
                    test_a_column_is_padded_to_what_it_draws);
    g_test_add_func("/markdown/table-too-wide-becomes-records",
                    test_a_wide_table_becomes_records);
    g_test_add_func("/markdown/table-needs-a-matching-delimiter",
                    test_what_is_not_a_table_is_left_alone);
    g_test_add_func("/markdown/table-in-a-fence-is-source",
                    test_a_table_in_a_fence_is_still_source);
    g_test_add_func("/markdown/table-escaped-pipe",
                    test_an_escaped_pipe_stays_in_its_cell);
    g_test_add_func("/markdown/table-keeps-its-neighbours",
                    test_a_table_keeps_its_neighbours);

    return g_test_run();
}
