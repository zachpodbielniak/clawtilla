/*
 * test-web-render.c - The web client's rendering, and the two clients' parity
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web client serves agent-written text to a browser, which makes
 * escaping a security property rather than a cosmetic one.  It is
 * asserted here on the *rendered output* rather than on the code that
 * produces it, because "we call the escaping function" is a claim about
 * the source and "no < reached the page" is a claim about what a browser
 * would receive.
 *
 * The sources are included rather than linked: the web client is a
 * binary, not a library, and its renderers are the part worth testing.
 * The two functions it calls that live in other translation units are
 * stubbed below.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"

/*
 * The frame, which needs a daemon to draw. Stubbed so the renderers can
 * be exercised without one; nothing here asserts on the frame.
 */
HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtWebView view)
{
    (void)app;
    (void)selected;
    (void)view;

    return HTMX_ELEMENT(htmx_div_new());
}

HtmxElement *
clawt_web_topbar(ClawtWebApp *app, const gchar *agent_id, ClawtWebView view)
{
    (void)app;
    (void)agent_id;
    (void)view;

    return HTMX_ELEMENT(htmx_div_new());
}

/* ── Escaping ────────────────────────────────────────────────────── */

/*
 * An agent's name, description and every message body were written by a
 * person or a model, and this client serves them back over HTTP. An
 * unescaped "<" is not a rendering glitch -- it is script injection into
 * whoever opened the page.
 */
static void
test_text_is_escaped(void)
{
    const gchar *hostile = "<script>alert('x')</script>";
    g_autoptr(HtmxP) para = clawt_web_text(hostile, NULL);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(para));

    g_assert_null(strstr(html, "<script>"));
    g_assert_nonnull(strstr(html, "&lt;script&gt;"));
}

static void
test_row_values_are_escaped(void)
{
    g_autoptr(HtmxDiv) row = clawt_web_row("Name", "<img onerror=x>");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(row));

    g_assert_null(strstr(html, "<img"));
    g_assert_nonnull(strstr(html, "&lt;img"));
}

/*
 * An attribute is its own escaping problem: a value that closes the
 * quote can add an attribute of its own, which is how an onerror gets
 * onto an element that never had one.
 */
static void
test_field_values_are_escaped(void)
{
    g_autoptr(HtmxDiv) field = clawt_web_field(
        "Id", "id", "\" onfocus=\"alert(1)", NULL);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_null(strstr(html, "onfocus=\"alert"));
}

static void
test_badges_are_escaped(void)
{
    g_autoptr(HtmxSpan) badge = clawt_web_badge("<b>bold</b>", "good");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(badge));

    g_assert_null(strstr(html, "<b>"));
    g_assert_nonnull(strstr(html, "badge-good"));
}

/* ── A control that can represent what is set ────────────────────── */

/*
 * A value the list does not have is added to it, never dropped. A select
 * that cannot show the current value opens on its first entry, and
 * saving the form -- without anybody touching that control -- writes the
 * first entry back over whatever was there.
 */
static void
test_select_keeps_a_value_it_does_not_offer(void)
{
    static const gchar *const values[] = { "none", "host", NULL };
    g_autoptr(HtmxDiv) field = clawt_web_select_field(
        "Computer", "computer", values, NULL, "firecracker");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_nonnull(strstr(html, "firecracker"));
    g_assert_nonnull(strstr(html, "selected"));
}

static void
test_select_marks_the_current_value(void)
{
    static const gchar *const values[] = { "none", "host", "vm", NULL };
    g_autoptr(HtmxDiv) field = clawt_web_select_field(
        "Computer", "computer", values, NULL, "host");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));
    g_auto(GStrv) options = g_strsplit(html, "<option", -1);
    guint i;
    guint marked = 0;

    /*
     * Split into options rather than scanned forward from the value,
     * because the marker is emitted before it: htmx-glib writes
     * `<option selected="selected" value="host">`. Asserting on the
     * order would be asserting on something nobody promised.
     */
    for (i = 1; options[i] != NULL; i++) {
        if (strstr(options[i], "selected") == NULL)
            continue;

        marked++;
        g_assert_nonnull(strstr(options[i], "value=\"host\""));
    }

    /* Exactly one, or the browser picks for itself. */
    g_assert_cmpuint(marked, ==, 1);
}

/*
 * An unticked checkbox posts nothing at all, which a form cannot tell
 * from a field that was not on the page. The companion hidden input is
 * what makes "off" expressible.
 */
static void
test_a_switch_can_say_off(void)
{
    g_autoptr(HtmxDiv) field = clawt_web_switch_field(
        "Autostart", "runtime.autostart", NULL, FALSE);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_nonnull(strstr(html, "runtime.autostart__present"));
    g_assert_null(strstr(html, "checked"));
}

static void
test_a_switch_that_is_on_says_so(void)
{
    g_autoptr(HtmxDiv) field = clawt_web_switch_field(
        "Autostart", "runtime.autostart", NULL, TRUE);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_nonnull(strstr(html, "checked"));
    g_assert_nonnull(strstr(html, "runtime.autostart__present"));
}

/* ── Text handling ───────────────────────────────────────────────── */

/*
 * Truncation is by character, not by byte. Cutting a UTF-8 string in the
 * middle of a character does not produce a shorter string, it produces
 * something that is not a string -- and the browser draws a replacement
 * glyph where a word was.
 */
static void
test_one_line_does_not_split_a_character(void)
{
    g_autofree gchar *cut = clawt_web_one_line("ααααααααααααα", 4);

    g_assert_true(g_utf8_validate(cut, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(cut, -1), ==, 5);   /* four plus the ellipsis */
}

static void
test_one_line_folds_newlines(void)
{
    g_autofree gchar *flat = clawt_web_one_line("one\ntwo\tthree", 100);

    g_assert_null(strchr(flat, '\n'));
    g_assert_null(strchr(flat, '\t'));
    g_assert_cmpstr(flat, ==, "one two three");
}

static void
test_one_line_leaves_short_text_alone(void)
{
    g_autofree gchar *same = clawt_web_one_line("short", 100);

    g_assert_cmpstr(same, ==, "short");
}

static void
test_relative_time_of_zero_is_empty(void)
{
    g_autofree gchar *never = clawt_web_relative_time(0);

    g_assert_cmpstr(never, ==, "");
}

static void
test_relative_time_reads_as_ago(void)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree gchar *recent = clawt_web_relative_time(now - 120);

    g_assert_cmpstr(recent, ==, "2m ago");
}

/* ── URLs ────────────────────────────────────────────────────────── */

/*
 * An agent id comes from a config file somebody edits, so it is not
 * necessarily a word. Left raw, a "?" in one takes the rest of the path
 * into the query string and the link reaches a different agent.
 */
static void
test_agent_urls_escape_the_id(void)
{
    g_autofree gchar *url = clawt_web_agent_url("odd id?x=1",
                                                CLAWT_WEB_VIEW_MAILBOX);

    g_assert_null(strchr(url, '?'));
    g_assert_null(strchr(url, ' '));
    g_assert_true(g_str_has_suffix(url, "/mailbox"));
}

static void
test_every_view_has_a_slug_and_a_title(void)
{
    guint i;

    for (i = 0; i < CLAWT_WEB_N_VIEWS; i++) {
        const gchar *slug = clawt_web_view_slug((ClawtWebView)i);

        g_assert_nonnull(slug);
        g_assert_cmpuint(strlen(slug), >, 0);
        g_assert_nonnull(clawt_web_view_title((ClawtWebView)i));

        /* A slug has to survive the round trip, or a tab leads elsewhere. */
        g_assert_cmpint(clawt_web_view_from_slug(slug), ==, (gint)i);
    }
}

static void
test_an_unknown_view_falls_back_to_chat(void)
{
    g_assert_cmpint(clawt_web_view_from_slug("nonsense"), ==,
                    CLAWT_WEB_VIEW_CHAT);
    g_assert_cmpint(clawt_web_view_from_slug(NULL), ==,
                    CLAWT_WEB_VIEW_CHAT);
}

/* ── The stylesheet ──────────────────────────────────────────────── */

/*
 * Every colour is defined on bare :root as well as in the theme blocks.
 * A colour whose only definition is inside a media query has no value at
 * all for a reader whose browser does not match it -- and the page then
 * draws with whatever the browser's default happens to be.
 */
static void
test_the_palette_is_defined_outside_a_media_query(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *root = strstr(css, ":root{");
    const gchar *first_media = strstr(css, "@media");

    g_assert_nonnull(root);
    g_assert_nonnull(first_media);

    /* The bare :root block comes before any media query. */
    g_assert_true(root < first_media);

    {
        gsize length = (gsize)(first_media - root);

        g_assert_nonnull(g_strstr_len(root, length, "--canvas:"));
        g_assert_nonnull(g_strstr_len(root, length, "--ink:"));
        g_assert_nonnull(g_strstr_len(root, length, "--line:"));
        g_assert_nonnull(g_strstr_len(root, length, "--bad-fg:"));
    }
}

/*
 * The dark palette is reachable both ways: by the system preference for
 * somebody who has chosen nothing, and by the explicit choice for
 * somebody who has. Only one of the two and the toggle appears to do
 * nothing on half the machines it runs on.
 */
static void
test_dark_is_reachable_by_preference_and_by_choice(void)
{
    const gchar *css = clawt_web_stylesheet();

    g_assert_nonnull(strstr(css, "prefers-color-scheme:dark"));
    g_assert_nonnull(strstr(css, ":root[data-theme=\"dark\"]"));

    /* The media block must not win over an explicit "light". */
    g_assert_nonnull(strstr(css, ":root:not([data-theme=\"light\"])"));
}

/* ── Parity ──────────────────────────────────────────────────────── */

/*
 * The two graphical clients answer for the same daemon.
 *
 * Run here as well as from `make docs-check`, because a check that only
 * runs in one target is a check somebody can go a long time without
 * seeing.
 */
static void
test_the_two_clients_stay_level(void)
{
    g_autofree gchar *script = g_build_filename(
        CLAWT_TEST_SRCDIR, "tools", "clawt-client-parity.sh", NULL);
    g_autofree gchar *output = NULL;
    g_autoptr(GError) error = NULL;
    gint status = 0;
    const gchar *argv[] = { "bash", NULL, NULL };

    if (!g_file_test(script, G_FILE_TEST_EXISTS)) {
        g_test_skip("the parity script is not in this tree");
        return;
    }

    argv[1] = script;

    if (!g_spawn_sync(CLAWT_TEST_SRCDIR, (gchar **)argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL, &output, NULL,
                      &status, &error)) {
        g_test_skip(error->message);
        return;
    }

    if (status != 0)
        g_test_fail_printf("clients have drifted apart:\n%s",
                           output != NULL ? output : "");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/web/text-is-escaped", test_text_is_escaped);
    g_test_add_func("/web/row-values-are-escaped", test_row_values_are_escaped);
    g_test_add_func("/web/field-values-are-escaped",
                    test_field_values_are_escaped);
    g_test_add_func("/web/badges-are-escaped", test_badges_are_escaped);

    g_test_add_func("/web/select-keeps-a-value-it-does-not-offer",
                    test_select_keeps_a_value_it_does_not_offer);
    g_test_add_func("/web/select-marks-the-current-value",
                    test_select_marks_the_current_value);
    g_test_add_func("/web/a-switch-can-say-off", test_a_switch_can_say_off);
    g_test_add_func("/web/a-switch-that-is-on-says-so",
                    test_a_switch_that_is_on_says_so);

    g_test_add_func("/web/one-line-does-not-split-a-character",
                    test_one_line_does_not_split_a_character);
    g_test_add_func("/web/one-line-folds-newlines",
                    test_one_line_folds_newlines);
    g_test_add_func("/web/one-line-leaves-short-text-alone",
                    test_one_line_leaves_short_text_alone);
    g_test_add_func("/web/relative-time-of-zero-is-empty",
                    test_relative_time_of_zero_is_empty);
    g_test_add_func("/web/relative-time-reads-as-ago",
                    test_relative_time_reads_as_ago);

    g_test_add_func("/web/agent-urls-escape-the-id",
                    test_agent_urls_escape_the_id);
    g_test_add_func("/web/every-view-has-a-slug-and-a-title",
                    test_every_view_has_a_slug_and_a_title);
    g_test_add_func("/web/an-unknown-view-falls-back-to-chat",
                    test_an_unknown_view_falls_back_to_chat);

    g_test_add_func("/web/the-palette-is-defined-outside-a-media-query",
                    test_the_palette_is_defined_outside_a_media_query);
    g_test_add_func("/web/dark-is-reachable-by-preference-and-by-choice",
                    test_dark_is_reachable_by_preference_and_by_choice);

    g_test_add_func("/web/the-two-clients-stay-level",
                    test_the_two_clients_stay_level);

    return g_test_run();
}
