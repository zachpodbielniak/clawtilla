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

    return g_test_run();
}
