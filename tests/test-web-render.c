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
#include <math.h>
#include <string.h>

#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"

/*
 * The frame, which needs a daemon to draw. Stubbed so the renderers can
 * be exercised without one; nothing here asserts on the frame.
 */
HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtPage view)
{
    (void)app;
    (void)selected;
    (void)view;

    return HTMX_ELEMENT(htmx_div_new());
}

HtmxElement *
clawt_web_topbar(ClawtWebApp *app, const gchar *agent_id, ClawtPage view)
{
    (void)app;
    (void)agent_id;
    (void)view;

    return HTMX_ELEMENT(htmx_div_new());
}

/*
 * Which conversation is on screen, which the page tells the app so an
 * arrival in it does not accrue an unread count.  It needs the app,
 * which needs a daemon.
 */
void
clawt_web_app_set_viewing(ClawtWebApp *app, const gchar *agent_id)
{
    (void)app;
    (void)agent_id;
}

/*
 * Whether the page should carry a banner about the connection.
 *
 * The real one reads the live client's reconnect state and the daemon's
 * version, neither of which exists here -- so it is a variable the tests
 * set.  NULL for everything except the one that is about the banner:
 * every other assertion in this file is written against a page with no
 * banner on it, which is also the ordinary case.
 */
static const gchar *the_connection_notice = NULL;

gchar *
clawt_web_app_connection_notice(ClawtWebApp *app)
{
    (void)app;

    return g_strdup(the_connection_notice);
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
 * A select whose labels differ from its values posts the *value*.
 *
 * The team control is the first one that needs them to differ: the
 * fleet declares a team by id and calls it something readable, so the
 * page shows "Operations" and `agent.set` has to receive `ops`. Swap
 * the two and every save writes a display name into `agents.team` --
 * accepted, saved, and naming a team that does not exist. Every other
 * select in the client passes NULL for the labels, so this path had no
 * coverage at all until the team select used it.
 */
static void
test_select_posts_the_value_not_the_label(void)
{
    static const gchar *const ids[] = { "", "ops", "research", NULL };
    static const gchar *const names[] = { "No team", "Operations",
                                          "Research", NULL };
    g_autoptr(HtmxDiv) field = clawt_web_select_field(
        "Team", "k:team", ids, names, "ops");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));
    g_auto(GStrv) options = g_strsplit(html, "<option", -1);
    guint i;
    guint marked = 0;

    /* The readable name is what a person reads... */
    g_assert_nonnull(strstr(html, ">Operations<"));

    /* ...and the id is what the form carries. */
    g_assert_nonnull(strstr(html, "value=\"ops\""));
    g_assert_null(strstr(html, "value=\"Operations\""));

    for (i = 1; options[i] != NULL; i++) {
        if (strstr(options[i], "selected") == NULL)
            continue;

        marked++;
        g_assert_nonnull(strstr(options[i], "value=\"ops\""));
    }

    g_assert_cmpuint(marked, ==, 1);
}

/*
 * A team an agent names that the fleet does not declare keeps its own
 * entry, and stays selected.
 *
 * The same rule as the screen-size row, reached from the config rather
 * than from the widget: without it the control opens on "No team" and
 * saving the page -- without anybody touching that row -- takes the
 * agent off a team it was deliberately put on.
 */
static void
test_select_keeps_an_undeclared_team(void)
{
    static const gchar *const ids[] = { "", "ops", NULL };
    static const gchar *const names[] = { "No team", "Operations", NULL };
    g_autoptr(HtmxDiv) field = clawt_web_select_field(
        "Team", "k:team", ids, names, "ghost-team");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));
    g_auto(GStrv) options = g_strsplit(html, "<option", -1);
    guint i;
    guint marked = 0;

    g_assert_nonnull(strstr(html, "ghost-team"));

    for (i = 1; options[i] != NULL; i++) {
        if (strstr(options[i], "selected") == NULL)
            continue;

        marked++;
        g_assert_nonnull(strstr(options[i], "value=\"ghost-team\""));
    }

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
                                                CLAWT_PAGE_MAILBOX);

    g_assert_null(strchr(url, '?'));
    g_assert_null(strchr(url, ' '));
    g_assert_true(g_str_has_suffix(url, "/mailbox"));
}

/*
 * The nicknames and their round trip now belong to the library, and are
 * covered by tests/test-sections.c.  What is this client's own is the
 * row of page tabs it draws under the topbar for whichever section is
 * open.
 */

/*
 * Every page of the open section gets a tab, and only that section's.
 *
 * The topbar above it shows six section tabs; without this row the five
 * pages that are not a section's first would have no link anywhere in
 * the client -- reachable only by typing the URL, which is the failure
 * this whole change is meant to prevent rather than move.
 */
static void
test_the_subnav_lists_the_open_sections_pages(void)
{
    g_autoptr(HtmxElement) nav =
        clawt_web_section_subnav("scribe", CLAWT_PAGE_DECISIONS);
    g_autofree gchar *html = NULL;
    guint i;

    g_assert_nonnull(nav);
    html = htmx_element_render(nav);

    for (i = 0; i < clawt_section_page_count(CLAWT_SECTION_WORK); i++) {
        ClawtPage page = clawt_section_page_nth(CLAWT_SECTION_WORK, i);
        g_autofree gchar *href =
            g_strdup_printf("href=\"/a/scribe/%s\"", clawt_page_nick(page));

        g_assert_nonnull(strstr(html, href));
        g_assert_nonnull(strstr(html, clawt_page_label(page)));
    }

    /* And nothing from another section. */
    g_assert_null(strstr(html, "/a/scribe/skills"));
    g_assert_null(strstr(html, "/a/scribe/mailbox"));
}

/*
 * The tab for the page being read says so.
 *
 * Without it every tab in the row looks the same, and the row stops
 * being able to answer "where am I" -- which is most of what it is for
 * when the section tab above only says "Work".
 */
static void
test_the_subnav_marks_the_page_being_read(void)
{
    g_autoptr(HtmxElement) nav =
        clawt_web_section_subnav("scribe", CLAWT_PAGE_FLOW);
    g_autofree gchar *html = htmx_element_render(nav);
    g_auto(GStrv) tags = NULL;
    guint marked = 0;
    guint i;

    g_assert_nonnull(nav);

    /*
     * Split into tags rather than matched as one string.  The attribute
     * order is htmx-glib's to choose, so an assertion spelling
     * `href="..." aria-current` tests that library's rendering and not
     * this row -- and would pass or fail for reasons nothing here
     * controls.
     *
     * Counted as well as located: two marked tabs and none marked are
     * different bugs, and looking only for the right one catches the
     * second.
     */
    tags = g_strsplit(html, "<a ", -1);

    for (i = 0; tags[i] != NULL; i++) {
        if (strstr(tags[i], "aria-current") == NULL)
            continue;

        marked++;
        g_assert_nonnull(strstr(tags[i], "/a/scribe/flow"));
    }

    g_assert_cmpuint(marked, ==, 1);
}

/*
 * And no row at all for a section that is one page.
 *
 * Chat and Computer are each a single page, so a row would hold one tab
 * -- a control that does nothing, costing a strip of the window to say
 * so on the two pages somebody spends the most time on.
 */
static void
test_a_single_page_section_draws_no_subnav(void)
{
    g_assert_cmpuint(clawt_section_page_count(CLAWT_SECTION_CHAT), ==, 1);

    g_assert_null(clawt_web_section_subnav("scribe", CLAWT_PAGE_CHAT));
    g_assert_null(clawt_web_section_subnav("scribe", CLAWT_PAGE_COMPUTER));

    /* Nor with no agent selected, where the links would have no target. */
    g_assert_null(clawt_web_section_subnav(NULL, CLAWT_PAGE_DECISIONS));
}

/*
 * The id is escaped here too.
 *
 * The topbar's links go through clawt_web_agent_url() and are covered
 * above; this row builds its own and would have been the one place a
 * "?" in an agent id still took the rest of the path into the query
 * string.
 */
static void
test_the_subnav_escapes_the_agent_id(void)
{
    g_autoptr(HtmxElement) nav =
        clawt_web_section_subnav("odd id?x=1", CLAWT_PAGE_TASKS);
    g_autofree gchar *html = htmx_element_render(nav);

    g_assert_nonnull(nav);
    g_assert_null(strstr(html, "?x=1"));
    g_assert_nonnull(strstr(html, "/a/odd%20id%3Fx%3D1/tasks"));
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

/*
 * The composer stands on the message column, not in the gutter.
 *
 * Both are the same 40rem clamp and centred alike, and they still did
 * not line up: only the transcript spends anything on the avatar, so a
 * body starts a gutter in while the entry started at the clamp -- the
 * strongest vertical line on the page inside the one column
 * deliberately kept empty.  The GTK client had the identical fault and
 * `make parity` could not see either, because a margin sends no frame
 * and answers no command.
 *
 * Asserted through the token rather than on a number, because the point
 * is that the four rules cannot disagree.  A literal here would pass
 * for a sheet that had drifted back to three spellings.
 */
static void
test_the_composer_stands_on_the_message_column(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *composer = strstr(css, ".composer-inner{");

    /* One gutter, declared once. */
    g_assert_nonnull(strstr(css, "--chat-gutter:"));

    /* The body, its attachments and the composer all read it. */
    g_assert_nonnull(strstr(css, ".msg-body{word-wrap:break-word;"
                                 "overflow-wrap:anywhere;"
                                 "margin-left:var(--chat-gutter)}"));

    /*
     * And not pre-wrap.  A body was plain text with real newlines in it
     * until clawt_markdown_to_html() started rendering it; pre-wrap over
     * block markup shows the newline between every pair of tags as a
     * blank line, so the markup's own formatting becomes visible gaps
     * down the transcript.  Asserted as an absence because that is the
     * failure -- the rule would still be there, and still look right.
     */
    g_assert_null(strstr(css, ".msg-body{white-space:pre-wrap"));

    /*
     * Every block the renderer can emit is styled.  A construct with no
     * rule does not fail to render, it renders in the browser's default
     * -- a serif blockquote with no rule beside it, a table with no
     * borders -- which reads as the page being broken rather than as a
     * missing line of CSS.
     */
    {
        static const gchar *blocks[] = {
            ".msg-body p{", ".msg-body h1{", ".msg-body ul,.msg-body ol{",
            ".msg-body li{", ".msg-body blockquote{", ".msg-body hr{",
            ".msg-body pre{", ".msg-body code{", ".msg-body table{",
            ".msg-body th,.msg-body td{", ".msg-body .md-table{",
            ".msg-body .md-link{", ".msg-body .md-url{",
            ".msg-body .md-c{", ".msg-body .md-r{",
            NULL
        };
        gsize i;

        for (i = 0; blocks[i] != NULL; i++)
            if (strstr(css, blocks[i]) == NULL)
                g_error("a rendered message can contain %s and the "
                        "stylesheet does not mention it", blocks[i]);
    }
    g_assert_nonnull(strstr(css, ".attachments{display:flex;flex-wrap:wrap;"
                                 "gap:8px;margin-top:8px;"
                                 "margin-left:var(--chat-gutter)}"));
    g_assert_nonnull(composer);
    g_assert_nonnull(strstr(composer, "padding-left:var(--chat-gutter)"));
}

/*
 * A decision's options stack, and read from the left.
 *
 * An option is a sentence rather than a verb -- "Re-provision
 * clawt-oryx from a proper Fedora cloud image, so the exchange mounts
 * and the default-user config land too" is a real one.  In a wrapping
 * button row each of those is centred and broken across lines with no
 * left edge for the eye to come back to, which is most of why the
 * decisions page was unreadable in both clients.
 */
static void
test_decision_options_stack(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *rule = strstr(css, ".decision-options{");
    const gchar *inner = strstr(css, ".decision-options .btn{");
    g_autofree gchar *block = NULL;
    g_autofree gchar *inner_block = NULL;
    const gchar *close;

    g_assert_nonnull(rule);
    g_assert_nonnull(inner);

    /* Cut at each rule's own brace: strstr() from here would otherwise
     * find a declaration anywhere later in the sheet and report the rule
     * as correct however it is written. */
    close = strchr(rule, '}');
    g_assert_nonnull(close);
    block = g_strndup(rule, (gsize)(close - rule));

    close = strchr(inner, '}');
    g_assert_nonnull(close);
    inner_block = g_strndup(inner, (gsize)(close - inner));

    g_assert_nonnull(strstr(block, "flex-direction:column"));

    /*
     * And the label wraps inside the button rather than making it as
     * wide as the sentence, which is the same failure the GTK client
     * had -- there as a button whose label did not wrap, here as a
     * `white-space` that would keep it on one line.
     */
    g_assert_nonnull(strstr(inner_block, "text-align:left"));
    g_assert_nonnull(strstr(inner_block, "white-space:normal"));
}

/*
 * Stop does not reflow the composer row when it arrives.
 *
 * It is drawn only while a turn is running, so it appears and
 * disappears under a cursor that may be aiming at Send.  Without
 * `flex:none` a flex row shrinks its items to fit, so Send moves as
 * Stop arrives -- and the click meant for one lands on the other, which
 * here means stopping an agent somebody was talking to.
 */
static void
test_stop_does_not_move_send(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *rule = strstr(css, ".composer-inner .stop-turn{");
    g_autofree gchar *block = NULL;
    const gchar *close;

    g_assert_nonnull(rule);

    /*
     * Cut at this rule's own closing brace before looking inside it.
     *
     * strstr() from the rule searches the rest of the sheet, so an
     * assertion phrased that way passes on any declaration that appears
     * anywhere later -- and `flex:none` does. The test then reports the
     * rule as correct however it is written, which is the failure a CSS
     * test is most likely to have and least likely to show.
     */
    close = strchr(rule, '}');
    g_assert_nonnull(close);
    block = g_strndup(rule, (gsize)(close - rule));

    g_assert_nonnull(strstr(block, "flex:none"));
    g_assert_nonnull(strstr(block, "white-space:nowrap"));
}

/*
 * And the narrow regime hides the avatar, so the composer goes back to
 * the clamp with the bodies.
 *
 * The override has to come *after* the rule it overrides: the two
 * selectors are identical in specificity, so source order is the only
 * thing deciding.  Grouped with the sheet's other narrow overrides --
 * which sit a hundred lines earlier -- it would lose every time and do
 * nothing, while reading exactly like a fix.  That is the same trap the
 * palette blocks are ordered around, so it is asserted rather than
 * trusted.
 */
static void
test_the_narrow_composer_override_can_win(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *composer = strstr(css, ".composer-inner{");
    const gchar *override = strstr(css, "@media (max-width:26rem){"
                                        ".composer-inner{padding-left:0}}");

    g_assert_nonnull(composer);
    g_assert_nonnull(override);
    g_assert_true(override > composer);

    /* The avatar it defers to is hidden at the same width. */
    g_assert_nonnull(strstr(css, ".msg-avatar{display:none}"));
}

/*
 * The column and the run gap are tokens, not literals.
 *
 * That is step three of #19's configuration layer: the shipped design
 * and a reader's override used to be two mechanisms with only one of
 * them existing.  Asserted through the token rather than on a number,
 * because the point is that the sheet and the override cannot disagree.
 */
static void
test_the_reading_measurements_are_tokens(void)
{
    const gchar *css = clawt_web_stylesheet();

    g_assert_nonnull(strstr(css, "--chat-measure:"));
    g_assert_nonnull(strstr(css, "--chat-run-gap:"));

    g_assert_nonnull(strstr(css, ".transcript-inner{max-width:"
                                 "var(--chat-measure)"));
    g_assert_nonnull(strstr(css, ".msg.run-start{margin-top:"
                                 "var(--chat-run-gap)}"));

    /*
     * The composer follows the same measure.  A column widened while
     * the box you type into stayed put would restore exactly the
     * misalignment the gutter inset exists to fix.
     */
    {
        const gchar *composer = strstr(css, ".composer-inner{");

        g_assert_nonnull(composer);
        g_assert_nonnull(strstr(composer, "max-width:var(--chat-measure)"));
    }
}

/*
 * A new message is separated by more than a paragraph and less than a
 * new run.
 *
 * Measured at the default font: a line is 18px and a markdown paragraph
 * break is one blank line, so a within-run gap of 6 put a new *message*
 * at a third of what separates two paragraphs of one message -- three
 * turns reading as one message with tight paragraphs.  18 / 24 / 30 are
 * even 6px steps; the window was 19 to 29, because at 30 a message
 * reads as a new run and the grouping carries no information.
 *
 * Asserted as an ordering rather than on the number alone, so raising
 * the run gap without raising this one fails here rather than silently
 * re-inverting it.
 */
static void
test_a_new_message_sits_between_a_paragraph_and_a_run(void)
{
    const gchar *css = clawt_web_stylesheet();

    g_assert_nonnull(strstr(css, "--chat-run-gap:36px"));
    g_assert_nonnull(strstr(css,
        "--chat-msg-gap:calc(var(--chat-run-gap) - 7px)"));
    g_assert_nonnull(strstr(css,
        ".msg.run-cont{margin-top:var(--chat-msg-gap)"));

    /*
     * The second signal, because six pixels is perceptible and not
     * nameable: a continuation row names its own time, in the column
     * the avatar already reserves.
     */
    g_assert_nonnull(strstr(css, ".msg-time{position:absolute"));
    g_assert_nonnull(strstr(css, "width:var(--chat-gutter)"));
}

/* ── Appearance ──────────────────────────────────────────────────── */

/*
 * An unset field emits no rule at all.
 *
 * Naming the browser's current font instead would look identical on
 * screen and diverge for ever afterwards: one keeps following, the other
 * has quietly frozen. The GTK client's appearance page was fixed for
 * exactly this, and the web one inherits the rule.
 */
static void
test_an_unset_look_emits_nothing(void)
{
    ClawtWebLook look = { 0 };
    g_autofree gchar *css = clawt_web_look_css(&look);

    g_assert_cmpstr(css, ==, "");
}

static void
test_a_null_look_emits_nothing(void)
{
    g_autofree gchar *css = clawt_web_look_css(NULL);

    g_assert_cmpstr(css, ==, "");
}

static void
test_a_set_look_emits_its_tokens(void)
{
    ClawtWebLook look = { 0 };

    look.font = (gchar *)"Cantarell";
    look.font_size = 16;
    look.mono = (gchar *)"JetBrains Mono";
    look.mono_size = 13;
    g_autofree gchar *css = clawt_web_look_css(&look);

    g_assert_nonnull(strstr(css, "--sans:\"Cantarell\""));
    g_assert_nonnull(strstr(css, "--mono:\"JetBrains Mono\""));
    g_assert_nonnull(strstr(css, "--font-size:16px"));
    g_assert_nonnull(strstr(css, "--mono-size:13px"));
}

/*
 * Half set is half emitted. A size without a family must not drag a
 * family rule along with it, or choosing one size silently freezes the
 * font too.
 */
static void
test_only_what_is_set_is_emitted(void)
{
    ClawtWebLook look = { 0 };

    look.font_size = 18;
    g_autofree gchar *css = clawt_web_look_css(&look);

    g_assert_nonnull(strstr(css, "--font-size:18px"));
    g_assert_null(strstr(css, "--sans:"));
    g_assert_null(strstr(css, "--mono:"));
    g_assert_null(strstr(css, "--mono-size:"));
}

/*
 * A family comes out of a cookie, which is a string somebody can set to
 * anything. It is sanitised by an allowlist rather than escaped: CSS
 * string escapes are their own small language and there is nothing in a
 * font name to preserve.
 *
 * The first attempt was a denylist and was already wrong -- it stopped a
 * quote closing the string and let a comment-opener through, which
 * swallows the rest of the sheet.
 */
static void
test_a_hostile_family_cannot_escape_the_declaration(void)
{
    ClawtWebLook look = { 0 };

    look.font = (gchar *)"X\"}body{display:none}/*";
    g_autofree gchar *css = clawt_web_look_css(&look);

    g_assert_null(strstr(css, "display:none"));

    /*
     * Counted rather than searched for. The block legitimately ends with
     * a brace, so "contains no }" is asserting on the wrong thing --
     * what matters is that the *injected* text added none, which is what
     * a count of exactly two says.
     */
    {
        const gchar *p;
        guint quotes = 0;
        guint braces = 0;

        for (p = css; *p != '\0'; p++) {
            if (*p == '"')
                quotes++;
            if (*p == '{' || *p == '}')
                braces++;
        }

        g_assert_cmpuint(quotes, ==, 2);
        g_assert_cmpuint(braces, ==, 2);
    }
}

/*
 * A family that sanitises down to nothing is treated as unset rather
 * than emitted empty. An empty family is invalid CSS, and a browser
 * drops the whole block it appears in -- so one bad field would take the
 * sizes with it.
 */
static void
test_a_family_of_only_punctuation_is_unset(void)
{
    ClawtWebLook look = { 0 };

    look.font = (gchar *)"{}<>;";
    look.font_size = 15;
    g_autofree gchar *css = clawt_web_look_css(&look);

    g_assert_null(strstr(css, "--sans:"));
    g_assert_nonnull(strstr(css, "--font-size:15px"));
}

/*
 * A size outside what a person could want is ignored rather than
 * emitted. A cookie saying 0 or 4000 is not a preference.
 */
static void
test_an_absurd_size_is_ignored(void)
{
    ClawtWebLook small = { 0 };
    ClawtWebLook huge = { 0 };
    g_autofree gchar *small_css = NULL;
    g_autofree gchar *huge_css = NULL;

    small.font_size = 2;
    huge.font_size = 4000;

    small_css = clawt_web_look_css(&small);
    huge_css = clawt_web_look_css(&huge);

    g_assert_cmpstr(small_css, ==, "");
    g_assert_cmpstr(huge_css, ==, "");
}

/*
 * A request with no message behind it -- which is what
 * htmx_request_new_for_path() makes -- has no cookies, so every field
 * defers. Zeroed meaning "defer" is what makes a field added later
 * default to deferring too.
 */
static void
test_a_request_without_cookies_defers_everything(void)
{
    g_autoptr(HtmxRequest) request =
        htmx_request_new_for_path(HTMX_METHOD_GET, "/a/x/chat");
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    g_autofree gchar *css = clawt_web_look_css(look);

    g_assert_null(look->theme);
    g_assert_null(look->font);
    g_assert_cmpint(look->font_size, ==, 0);
    g_assert_cmpstr(css, ==, "");
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

/* ── Contrast ────────────────────────────────────────────────────── */

/*
 * WCAG's relative luminance, and the ratio built from it.
 *
 * Written out rather than eyeballed because a colour is not readable
 * or unreadable by how it looks in the one scheme the person choosing
 * it happened to be in.  Two of the four palettes here are dark; the
 * value that was under the line was in the light one, and it had been
 * measured by nobody.
 */
static gdouble
channel_luminance(gint byte)
{
    gdouble v = byte / 255.0;

    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static gdouble
relative_luminance(const gchar *hex)
{
    gint r, g, b;

    g_assert_cmpuint(strlen(hex), ==, 7);
    g_assert_cmpint(hex[0], ==, '#');

    r = (gint)g_ascii_strtoull(&(gchar[3]){hex[1], hex[2], 0}[0], NULL, 16);
    g = (gint)g_ascii_strtoull(&(gchar[3]){hex[3], hex[4], 0}[0], NULL, 16);
    b = (gint)g_ascii_strtoull(&(gchar[3]){hex[5], hex[6], 0}[0], NULL, 16);

    return 0.2126 * channel_luminance(r) +
           0.7152 * channel_luminance(g) +
           0.0722 * channel_luminance(b);
}

static gdouble
contrast_ratio(const gchar *fg, const gchar *bg)
{
    gdouble a = relative_luminance(fg);
    gdouble b = relative_luminance(bg);
    gdouble hi = MAX(a, b);
    gdouble lo = MIN(a, b);

    return (hi + 0.05) / (lo + 0.05);
}

/*
 * The `--name:#rrggbb` pairs inside one palette block.
 *
 * Bounded at the block's own closing brace, and that is the whole
 * difficulty.  An unbounded scan reads on into the palettes below, and
 * a later insert replaces an earlier one in a hash table -- so every
 * scheme reports the *last* one's colours under its own name.  This
 * test passed that way first time round: four palettes, all of them
 * Catppuccin, nothing under the line anywhere.
 *
 * A palette block has no nested braces, so its first `}` is its own.
 * The `"}` this looked for at first is a sequence in the *source*,
 * where the sheet is a run of adjacent C string literals; by the time
 * clawt_web_stylesheet() returns it the quotes are gone and there is
 * no such pair to find.  A test written against the shape of the code
 * rather than the shape of its output.
 *
 * Returns: (transfer full) (element-type utf8 utf8)
 */
static GHashTable *
palette_after(const gchar *css, const gchar *selector)
{
    GHashTable *tokens = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    const gchar *p = strstr(css, selector);
    const gchar *end;

    g_assert_nonnull(p);

    p += strlen(selector);
    end = strchr(p, '}');
    g_assert_nonnull(end);

    while ((p = strstr(p, "--")) != NULL && p < end) {
        const gchar *colon = strchr(p, ':');

        if (colon == NULL || colon > end)
            break;

        if (colon[1] == '#' && strlen(colon) >= 8) {
            g_hash_table_insert(tokens,
                                g_strndup(p + 2, (gsize)(colon - p - 2)),
                                g_strndup(colon + 1, 7));
        }

        p = colon + 1;
    }

    return tokens;
}

/*
 * Every text colour this client can draw clears WCAG AA.
 *
 * 4.5:1 is AA for normal text, which every one of these is: the tokens
 * below are body copy, secondary annotations and badge labels, none of
 * them at the 24px that would let 3:1 apply.
 *
 * The check is worth more than the fix that prompted it.  --muted was
 * 4.32:1 on the canvas in the light palette -- legible, plainly wrong
 * only when measured, and used by every timestamp, day divider and
 * link target in the client.  Nothing said so, and nothing would have
 * said so about the next palette either.
 */
static void
test_every_text_colour_clears_aa(void)
{
    static const gchar *palettes[] = {
        ":root{",
        "@media (prefers-color-scheme:dark){:root:not([data-theme=\"light\"]){",
        ":root[data-theme=\"dark\"]{",
        ":root[data-theme=\"catppuccin-mocha\"]{",
        NULL
    };
    /*
     * Text on ground.  Each foreground is checked against every ground
     * it can land on rather than against one -- --muted was over the
     * line on white and under it on surface-2, and a check that had
     * picked the first would have passed.
     */
    static const gchar *grounds[] = { "canvas", "surface", "surface-2", NULL };
    static const gchar *inks[] = { "ink", "ink-2", "muted", NULL };
    static const gchar *pairs[][2] = {
        { "good-fg", "good-bg" },
        { "warn-fg", "warn-bg" },
        { "bad-fg", "bad-bg" },
        { "info-fg", "info-bg" },
        { "neutral-fg", "neutral-bg" },
        { NULL, NULL }
    };
    const gchar *css = clawt_web_stylesheet();
    g_autofree gchar *light_canvas = NULL;
    gsize p;

    for (p = 0; palettes[p] != NULL; p++) {
        g_autoptr(GHashTable) tokens = palette_after(css, palettes[p]);
        gsize i;
        gsize j;

        /*
         * A palette that parsed to nothing would pass every assertion
         * below it.  The count is what makes a renamed selector a
         * failure rather than a silently empty check.
         */
        g_assert_cmpuint(g_hash_table_size(tokens), >=, 16);

        /*
         * And the light palette must not have come back holding a dark
         * one's colours.  That is not a hypothetical: an unbounded scan
         * gives every block the last one's values, and the check then
         * passes while measuring one palette four times.  Comparing the
         * ground each block reports is the cheapest thing that can tell
         * the difference.
         */
        if (p == 0)
            light_canvas = g_strdup(g_hash_table_lookup(tokens, "canvas"));
        else
            g_assert_cmpstr(g_hash_table_lookup(tokens, "canvas"), !=,
                            light_canvas);

        for (i = 0; inks[i] != NULL; i++) {
            const gchar *fg = g_hash_table_lookup(tokens, inks[i]);

            g_assert_nonnull(fg);

            for (j = 0; grounds[j] != NULL; j++) {
                const gchar *bg = g_hash_table_lookup(tokens, grounds[j]);
                gdouble ratio;

                g_assert_nonnull(bg);
                ratio = contrast_ratio(fg, bg);

                if (ratio < 4.5)
                    g_error("%s --%s (%s) on --%s (%s) is %.2f:1, "
                            "under AA's 4.5 for text",
                            palettes[p], inks[i], fg, grounds[j], bg, ratio);
            }
        }

        for (i = 0; pairs[i][0] != NULL; i++) {
            const gchar *fg = g_hash_table_lookup(tokens, pairs[i][0]);
            const gchar *bg = g_hash_table_lookup(tokens, pairs[i][1]);
            const gchar *canvas = g_hash_table_lookup(tokens, "canvas");
            gdouble on_pill;
            gdouble on_page;

            g_assert_nonnull(fg);
            g_assert_nonnull(bg);
            g_assert_nonnull(canvas);

            on_pill = contrast_ratio(fg, bg);
            on_page = contrast_ratio(fg, canvas);

            /* A semantic colour is drawn on its own tint and bare. */
            if (on_pill < 4.5 || on_page < 4.5)
                g_error("%s --%s (%s) is %.2f:1 on --%s and %.2f:1 on the "
                        "canvas, under AA's 4.5 for text",
                        palettes[p], pairs[i][0], fg, on_pill, pairs[i][1],
                        on_page);
        }
    }
}

/*
 * The app box is measured against the viewport that is actually there.
 *
 * 100vh on a phone is the height with the URL bar retracted, so the
 * bottom row of this grid -- the composer -- starts below the fold and
 * reaching it means fighting the browser chrome. Both declarations are
 * required: a browser that does not know dvh keeps the first.
 */
static void
test_the_app_is_measured_in_dynamic_viewport_height(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *app = strstr(css, ".app{");
    const gchar *end;

    g_assert_nonnull(app);
    end = strchr(app, '}');
    g_assert_nonnull(end);

    g_assert_nonnull(g_strstr_len(app, (gsize)(end - app), "height:100dvh"));

    /* And the fallback is still there, in front of it. */
    g_assert_nonnull(g_strstr_len(app, (gsize)(end - app), "height:100vh"));
    g_assert_true(g_strstr_len(app, (gsize)(end - app), "height:100vh") <
                  g_strstr_len(app, (gsize)(end - app), "height:100dvh"));
}

/*
 * No auto-fit track has a floor it cannot go below.
 *
 * In minmax() a bare length is a hard floor, so the track refuses to go
 * under it and overflows its container rather than shrinking. Written as
 * a sweep over every minmax() in the sheet rather than as two named
 * assertions, because the next one somebody adds is the one that will be
 * wrong.
 *
 * Two spellings satisfy it and the test accepts both, because the rule
 * is that the floor can shrink rather than that it is written a
 * particular way. `min(20rem,100%)` keeps a preferred width and gives it
 * up when there is no room, which is what an auto-fit column wants;
 * plain `0` is the stronger form and is what a row containing a
 * scrolling box wants, since 1fr's own minimum is min-content and a long
 * transcript would otherwise push the track past the viewport.
 *
 * Asserting `min(` alone would have failed the second the moment it was
 * used -- and it did. A test that names the fix rather than the property
 * refuses the next correct answer.
 */
static void
test_no_auto_fit_track_has_a_hard_floor(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *at = css;
    guint seen = 0;

    while ((at = strstr(at, "minmax(")) != NULL) {
        at += strlen("minmax(");
        seen++;

        if (g_str_has_prefix(at, "min(") || g_str_has_prefix(at, "0,"))
            continue;

        g_error("minmax() with a floor that cannot shrink: %.40s", at);
    }

    /*
     * And the sweep found something to check. An empty loop would pass
     * in a sheet with no grids at all, which is the shape of a test that
     * survives the feature being deleted.
     */
    g_assert_cmpuint(seen, >=, 2);
}

/*
 * At phone width the sidebar is a drawer that starts closed.
 *
 * It used to be a 14rem band above every page: on a ~660px viewport that
 * is a third of the screen permanently spent on navigation, with the
 * content beginning below it on every view.
 *
 * The toggle is asserted to be a sibling selector rather than anything
 * inside .sidebar, because the sidebar is swapped outerHTML on every
 * fleet event -- state kept inside it would be discarded several times a
 * minute on a live fleet.
 */
static void
test_the_narrow_sidebar_is_a_drawer(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *narrow = strstr(css, "@media (max-width:56rem){");
    const gchar *end;
    gsize length;

    g_assert_nonnull(narrow);

    /*
     * Bounded by the next media query rather than by a closing brace:
     * the block contains braces of its own, so the first `}` is a rule's
     * and not the block's.
     */
    end = strstr(narrow + 1, "@media");
    g_assert_nonnull(end);
    length = (gsize)(end - narrow);

    g_assert_nonnull(g_strstr_len(narrow, length, ".sidebar{display:none"));
    g_assert_nonnull(g_strstr_len(narrow, length,
                                  ".nav-toggle:checked~.sidebar"));

    /* And the band it replaces is gone. */
    g_assert_null(g_strstr_len(narrow, length, "max-height:14rem"));
}


/*
 * The page really emits the drawer's checkbox.
 *
 * The whole drawer is a <label for="nav-open"> in the topbar and a
 * checkbox with that id in front of the sidebar; the stylesheet does the
 * rest with `.nav-toggle:checked~.sidebar`. Every part of that was
 * asserted against the *stylesheet* and none of it against the page, and
 * the page was where it was broken: the checkbox was built with a
 * g_autoptr *and* handed to clawt_web_add(), which takes the reference,
 * so it was freed while still in the tree and rendered as nothing.
 *
 * From outside that is indistinguishable from a working drawer until
 * somebody presses the button -- the hamburger was drawn, the CSS was
 * correct, and the toggle pointed at an element that was not there.
 *
 * So this asserts on the emitted HTML. A stylesheet test cannot see a
 * missing element, which is the whole reason this one exists.
 */
static void
test_the_page_emits_the_drawer_toggle(void)
{
    g_autoptr(HtmxDiv) body = htmx_div_new();
    g_autofree gchar *html = NULL;
    const gchar *toggle;
    const gchar *sidebar;

    /*
     * Borrowed, not handed over: clawt_web_page() is (transfer none),
     * so the g_object_ref() this used to wrap the body in was a
     * reference nobody ever dropped -- the one leak in an otherwise
     * clean binary, and it sat in the test rather than the page.
     */
    html = clawt_web_page(NULL, "alpha", CLAWT_PAGE_CHAT,
                          HTMX_ELEMENT(body), NULL);

    g_assert_nonnull(html);

    /* The control the label names, by the id the label names. */
    toggle = strstr(html, "id=\"nav-open\"");
    g_assert_nonnull(toggle);
    g_assert_nonnull(strstr(html, "type=\"checkbox\""));
    g_assert_nonnull(strstr(html, "nav-toggle"));

    /*
     * And in front of the sidebar, not inside it. `~` is a following
     * sibling combinator, so a checkbox emitted after the sidebar -- or
     * within it -- would render identically and toggle nothing. The
     * sidebar is stubbed to a bare div here, so this compares against
     * the class the real one carries via the frame's own child order.
     */
    sidebar = strstr(html, "class=\"app\"");
    g_assert_nonnull(sidebar);
    g_assert_true(toggle > sidebar);
}



/* ── The connection banner ───────────────────────────────────────── */

/*
 * A page whose daemon is missing or mismatched says so, above the page
 * and inside the content column.
 *
 * Inside the content rather than above the whole frame, so it does not
 * push the agent list down: that list is navigation, and losing a
 * connection is not a reason to move it.  The GTK client puts its
 * AdwBanner in the same place relative to its own header, which is what
 * makes the two look like one product.
 *
 * And escaped, like everything else on this page.  The sentence is built
 * from a version string the *daemon* reported and a connection name a
 * person typed, so neither is ours.
 */
static void
test_the_connection_banner_is_drawn_when_there_is_something_to_say(void)
{
    g_autofree gchar *quiet = NULL;
    g_autofree gchar *noisy = NULL;

    the_connection_notice = NULL;
    quiet = clawt_web_page(NULL, "chief", CLAWT_PAGE_CHAT, NULL, NULL);

    g_assert_nonnull(quiet);

    /*
     * The rendered attribute, not the class name.  The stylesheet is
     * included in this page too, so the bare name is present whether or
     * not anything drew it -- which is the same trap `make parity` had
     * at its own layer, where a CSS rule satisfied the check for an
     * element nothing rendered.
     */
    g_assert_null(strstr(quiet, "class=\"clawt-connection-banner\""));

    the_connection_notice = "This daemon is <older> & this client is not.";
    noisy = clawt_web_page(NULL, "chief", CLAWT_PAGE_CHAT, NULL, NULL);
    the_connection_notice = NULL;

    g_assert_nonnull(noisy);
    g_assert_nonnull(strstr(noisy, "class=\"clawt-connection-banner\""));
    g_assert_nonnull(strstr(noisy, "&lt;older&gt; &amp; this client"));
    g_assert_null(strstr(noisy, "<older>"));
}

/*
 * Every tone the library can hand a badge has a rule in the stylesheet.
 *
 * clawt_task_state_tone() answers with a tone and tests/test-task.c pins
 * that it is one of five; nothing there says the five are *painted*.
 * clawt_web_badge() builds the class by concatenation -- "badge-" plus
 * whatever it was given -- so a tone with no rule is a class the sheet
 * has never heard of, and the badge draws as unstyled text. That is the
 * same failure the task badge already had once: visibly wrong, silently
 * produced, and indistinguishable from a design decision.
 *
 * Driven from CLAWT_TYPE_TASK_STATE rather than from a list of tones, so
 * a state added later reaches this check with no edit here.
 */
static void
test_every_task_tone_is_painted(void)
{
    g_autoptr(GEnumClass) states = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    const gchar *css = clawt_web_stylesheet();
    guint i;

    g_assert_cmpuint(states->n_values, >, 0);

    for (i = 0; i < states->n_values; i++) {
        const gchar *tone = clawt_task_state_tone(states->values[i].value);
        g_autofree gchar *rule = g_strdup_printf(".badge-%s{", tone);

        if (strstr(css, rule) == NULL)
            g_error("state '%s' has tone '%s', and the stylesheet has no "
                    "%s rule for it", states->values[i].value_nick, tone,
                    rule);
    }
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/web/connection-banner-when-there-is-something-to-say",
                    test_the_connection_banner_is_drawn_when_there_is_something_to_say);
    g_test_add_func("/web/text-is-escaped", test_text_is_escaped);
    g_test_add_func("/web/row-values-are-escaped", test_row_values_are_escaped);
    g_test_add_func("/web/field-values-are-escaped",
                    test_field_values_are_escaped);
    g_test_add_func("/web/badges-are-escaped", test_badges_are_escaped);

    g_test_add_func("/web/select-keeps-a-value-it-does-not-offer",
                    test_select_keeps_a_value_it_does_not_offer);
    g_test_add_func("/web/select-posts-the-value-not-the-label",
                    test_select_posts_the_value_not_the_label);
    g_test_add_func("/web/select-keeps-an-undeclared-team",
                    test_select_keeps_an_undeclared_team);
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
    g_test_add_func("/web/the-subnav-lists-the-open-sections-pages",
                    test_the_subnav_lists_the_open_sections_pages);
    g_test_add_func("/web/the-subnav-marks-the-page-being-read",
                    test_the_subnav_marks_the_page_being_read);
    g_test_add_func("/web/a-single-page-section-draws-no-subnav",
                    test_a_single_page_section_draws_no_subnav);
    g_test_add_func("/web/the-subnav-escapes-the-agent-id",
                    test_the_subnav_escapes_the_agent_id);

    g_test_add_func("/web/the-app-is-measured-in-dvh",
                    test_the_app_is_measured_in_dynamic_viewport_height);
    g_test_add_func("/web/no-auto-fit-track-has-a-hard-floor",
                    test_no_auto_fit_track_has_a_hard_floor);
    g_test_add_func("/web/the-page-emits-the-drawer-toggle",
                    test_the_page_emits_the_drawer_toggle);
    g_test_add_func("/web/the-narrow-sidebar-is-a-drawer",
                    test_the_narrow_sidebar_is_a_drawer);
    g_test_add_func("/web/the-palette-is-defined-outside-a-media-query",
                    test_the_palette_is_defined_outside_a_media_query);
    g_test_add_func("/web/dark-is-reachable-by-preference-and-by-choice",
                    test_dark_is_reachable_by_preference_and_by_choice);
    g_test_add_func("/web/composer-stands-on-the-message-column",
                    test_the_composer_stands_on_the_message_column);
    g_test_add_func("/web/decision-options-stack",
                    test_decision_options_stack);
    g_test_add_func("/web/stop-does-not-move-send",
                    test_stop_does_not_move_send);
    g_test_add_func("/web/narrow-composer-override-can-win",
                    test_the_narrow_composer_override_can_win);
    g_test_add_func("/web/reading-measurements-are-tokens",
                    test_the_reading_measurements_are_tokens);
    g_test_add_func("/web/message-sits-between-paragraph-and-run",
                    test_a_new_message_sits_between_a_paragraph_and_a_run);

    g_test_add_func("/web/an-unset-look-emits-nothing",
                    test_an_unset_look_emits_nothing);
    g_test_add_func("/web/a-null-look-emits-nothing",
                    test_a_null_look_emits_nothing);
    g_test_add_func("/web/a-set-look-emits-its-tokens",
                    test_a_set_look_emits_its_tokens);
    g_test_add_func("/web/only-what-is-set-is-emitted",
                    test_only_what_is_set_is_emitted);
    g_test_add_func("/web/a-hostile-family-cannot-escape-the-declaration",
                    test_a_hostile_family_cannot_escape_the_declaration);
    g_test_add_func("/web/a-family-of-only-punctuation-is-unset",
                    test_a_family_of_only_punctuation_is_unset);
    g_test_add_func("/web/an-absurd-size-is-ignored",
                    test_an_absurd_size_is_ignored);
    g_test_add_func("/web/a-request-without-cookies-defers-everything",
                    test_a_request_without_cookies_defers_everything);

    g_test_add_func("/web/every-text-colour-clears-aa",
                    test_every_text_colour_clears_aa);

    g_test_add_func("/web/every-task-tone-is-painted",
                    test_every_task_tone_is_painted);
    g_test_add_func("/web/the-two-clients-stay-level",
                    test_the_two_clients_stay_level);

    return g_test_run();
}
