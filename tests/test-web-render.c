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
/* A matching hostile name is escaped, its URL remains an attribute, and
 * an unmatched row never appears. Checking just CSS would prove none of this. */
static void
test_navigation_result(void)
{
    g_autoptr(HtmxDiv) list = htmx_div_new();
    g_autofree gchar *html = NULL;

    g_assert_cmpuint(clawt_web_navigation_result(HTMX_ELEMENT(list), "alice",
        "Alice <script>bad()</script>", "Agent · alice · research",
        "alice", "/a/alice/chat"), ==, 1);
    g_assert_cmpuint(clawt_web_navigation_result(HTMX_ELEMENT(list), "alice",
        "Bob", "Agent · bob", "bob", "/a/bob/chat"), ==, 0);
    html = htmx_element_render(HTMX_ELEMENT(list));
    g_assert_nonnull(strstr(html, "class=\"navigation-result\""));
    g_assert_nonnull(strstr(html, "href=\"/a/alice/chat\""));
    g_assert_nonnull(strstr(html, "&lt;script&gt;"));
    g_assert_null(strstr(html, "<script>"));
    g_assert_null(strstr(html, "/a/bob/chat"));
}

/*
 * The frame, which needs a daemon to draw. Stubbed so the renderers can
 * be exercised without one; nothing here asserts on the frame.
 */
HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtPage view,
                  const ClawtWebLook *look)
{
    (void)app;
    (void)selected;
    (void)view;
    (void)look;

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
 * The 0.51.3 drop-downs: settings that are strings in the schema but take
 * a fixed set of values.  Each must render as a select carrying every
 * value from libclawt's list, mark the current one, and keep a value the
 * list lacks -- else saving an untouched form overwrites it.
 */
static guint
count_options(const gchar *html)
{
    g_auto(GStrv) parts = g_strsplit(html, "<option", -1);

    return g_strv_length(parts) - 1;
}

/* Whether @html shows @label as htmx-glib writes it: a quote as &#39;. */
static gboolean
shows_label(const gchar *html, const gchar *label)
{
    g_auto(GStrv) parts = g_strsplit(label, "'", -1);
    g_autofree gchar *escaped = g_strjoinv("&#39;", parts);

    return strstr(html, escaped) != NULL;
}

static void
test_effort_field_offers_the_default(void)
{
    g_autoptr(HtmxDiv) field = clawt_web_effort_field("effort", NULL);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));
    guint i;

    g_assert_nonnull(strstr(html, "<select"));
    g_assert_cmpuint(count_options(html), ==, clawt_effort_count());
    for (i = 0; i < clawt_effort_count(); i++)
        g_assert_true(shows_label(html, clawt_effort_nth_label(i)));
}

static void
test_schedule_and_weekday_fields(void)
{
    g_autoptr(HtmxDiv) schedule = clawt_web_schedule_field("schedule",
                                                           "weekly");
    g_autoptr(HtmxDiv) weekday = clawt_web_weekday_field("weekday",
                                                         "friday");
    g_autofree gchar *schedule_html =
        htmx_element_render(HTMX_ELEMENT(schedule));
    g_autofree gchar *weekday_html =
        htmx_element_render(HTMX_ELEMENT(weekday));

    g_assert_cmpuint(count_options(schedule_html), ==,
                     clawt_schedule_count());
    g_assert_nonnull(strstr(schedule_html, "Once a week"));
    g_assert_cmpuint(count_options(weekday_html), ==, 7);
    g_assert_nonnull(strstr(weekday_html, "Friday"));
    g_assert_nonnull(strstr(weekday_html, "selected"));
}

static void
test_resolution_field_keeps_a_custom_size(void)
{
    g_autoptr(HtmxDiv) field = clawt_web_resolution_field(
        "Screen size", "resolution", "1234x567");
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_cmpuint(count_options(html), ==, clawt_resolution_count() + 1);
    g_assert_nonnull(strstr(html, "1234x567"));
}

static void
test_review_by_field_lists_agents(void)
{
    static const gchar *const agents[] = { "builder", "checker", NULL };
    g_autoptr(HtmxDiv) project = clawt_web_review_by_field(
        "review_by", agents, TRUE, "checker");
    g_autoptr(HtmxDiv) agent = clawt_web_review_by_field(
        "review_by", agents, FALSE, NULL);
    g_autofree gchar *project_html =
        htmx_element_render(HTMX_ELEMENT(project));
    g_autofree gchar *agent_html = htmx_element_render(HTMX_ELEMENT(agent));

    g_assert_cmpuint(count_options(project_html), ==,
                     clawt_review_by_count() + 2);
    g_assert_nonnull(strstr(project_html, "builder"));
    g_assert_nonnull(strstr(project_html, "selected"));
    /* "not set" means different things on the two forms */
    g_assert_true(shows_label(project_html,
                              clawt_review_by_nth_label(0, TRUE)));
    g_assert_true(shows_label(agent_html,
                              clawt_review_by_nth_label(0, FALSE)));
}


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
 * Every custom property the stylesheet reads, it defines.  A var() of a
 * name nobody declares is not an error to a browser: the declaration
 * silently falls back, so the MCP app card drew with no border and the
 * browser's own text colour for as long as it read --border and --fg,
 * names this stylesheet never had.  A var() with a fallback is left out:
 * those are set by the page's own script (the eyes' --look-x) and the
 * fallback is the deliberate answer until it runs.
 */
static void
test_every_var_is_defined(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *cursor = css;
    guint used = 0;

    while ((cursor = strstr(cursor, "var(--")) != NULL) {
        const gchar *name = cursor + 4;
        const gchar *end = name;
        g_autofree gchar *wanted = NULL;
        g_autofree gchar *definition = NULL;

        while (g_ascii_isalnum(*end) || *end == '-' || *end == '_')
            end++;
        wanted = g_strndup(name, (gsize)(end - name));
        if (*end == ',') {
            cursor = end;
            continue;
        }
        definition = g_strconcat(wanted, ":", NULL);
        if (strstr(css, definition) == NULL)
            g_error("the stylesheet reads %s and never defines it", wanted);
        used++;
        cursor = end;
    }
    g_assert_cmpuint(used, >, 0);
}

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
 * The box that reserves the scrollbar gutter is not the box the
 * completion lists hang off.
 *
 * The composer has to reserve one: the transcript above it is the
 * scroller and reserves a gutter, so an entry that did not would be
 * fifteen pixels wider than the text over it.  But `scrollbar-gutter`
 * only applies to a scroll container, and a scroll container clips --
 * so while that reservation was on `.composer`, every completion list
 * was clipped to the composer's own 74 pixels while being 122 tall.
 * The visible result was a two-pixel sliver at the left edge, which
 * reads as a stylesheet bug rather than as an element outside its box,
 * and the slash list shipped that way from the day it was written.
 *
 * Asserted on the stylesheet because that is where the mistake is
 * expressible: `.composer-pop` is positioned against `.composer`, so
 * giving `.composer` any clipping overflow puts it back.
 */
static void
test_the_composer_does_not_clip_its_completions(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *composer = strstr(css, ".composer{");
    const gchar *end;

    g_assert_nonnull(composer);
    g_assert_nonnull(strstr(css, ".composer-pop{"));

    end = strchr(composer, '}');
    g_assert_nonnull(end);

    {
        gsize length = (gsize)(end - composer);

        /*
         * Any overflow but `visible` on this rule clips, and the
         * default is visible -- so the rule must not name one at all.
         */
        g_assert_null(g_strstr_len(composer, length, "overflow"));

        /* And it is the containing block the lists are placed against. */
        g_assert_nonnull(g_strstr_len(composer, length, "position:relative"));
    }

    /*
     * The reservation still has to exist somewhere, or the entry stops
     * lining up with the transcript and nothing says so.
     */
    {
        const gchar *form = strstr(css, ".composer-form{");
        gsize length;

        g_assert_nonnull(form);
        length = (gsize)(strchr(form, '}') - form);

        g_assert_nonnull(g_strstr_len(form, length, "scrollbar-gutter:stable"));
    }
}

/*
 * The sidebar's head wraps.  The wordmark and its three tabs are wider
 * than the 17rem column; unwrapped, Alerts -- the only link to the
 * Notifications page -- sat under the main pane with just its count
 * showing at the pane's edge, seen in a headless Chromium screenshot of
 * 0.52.2.  Breaks if the head stops wrapping or the sidebar can grow past
 * its column again.
 */
static void
test_the_sidebar_head_wraps(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *head = strstr(css, ".sidebar-head{");
    const gchar *side = strstr(css, ".sidebar{");

    g_assert_nonnull(head);
    g_assert_nonnull(side);
    g_assert_nonnull(g_strstr_len(head, (gssize)(strchr(head, '}') - head),
                                  "flex-wrap:wrap"));
    g_assert_nonnull(g_strstr_len(side, (gssize)(strchr(side, '}') - side),
                                  "min-width:0"));
}

/*
 * The lists keep the entry's column rather than the composer's width,
 * and they do it with the same two variables the entry itself uses.
 * A second copy of the clamp is a second answer that drifts.
 */
static void
test_the_completion_column_is_the_entry_column(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *pop = strstr(css, ".composer-pop-inner{");
    const gchar *inner = strstr(css, ".composer-inner{");
    gsize pop_length;
    gsize inner_length;

    g_assert_nonnull(pop);
    g_assert_nonnull(inner);

    pop_length = (gsize)(strchr(pop, '}') - pop);
    inner_length = (gsize)(strchr(inner, '}') - inner);

    g_assert_nonnull(g_strstr_len(pop, pop_length,
                                  "max-width:var(--chat-measure)"));
    g_assert_nonnull(g_strstr_len(inner, inner_length,
                                  "max-width:var(--chat-measure)"));
    g_assert_nonnull(g_strstr_len(pop, pop_length,
                                  "padding-left:var(--chat-gutter)"));
    g_assert_nonnull(g_strstr_len(inner, inner_length,
                                  "padding-left:var(--chat-gutter)"));
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

/* Renders one inline decision card from a JSON literal. */
static gchar *
inline_card_html(const gchar *json, const gchar *notice)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(HtmxElement) card = NULL;

    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    card = clawt_web_decision_inline(
        json_node_get_object(json_parser_get_root(parser)), notice);

    return htmx_element_render(card);
}

/*
 * The inline view of an open decision: the question, its options as
 * buttons posting `decision.answer` from the chat, and the default with
 * its deadline -- every agent-written string escaped, and the form
 * guarded against a double tap before the daemon has to refuse one.
 */
static void
test_an_open_inline_decision_offers_its_options(void)
{
    g_autofree gchar *html = inline_card_html(
        "{\"id\": \"d1\", \"agent\": \"chief\","
        " \"question\": \"Ship <now>?\", \"options\": [\"yes\", \"<b>no</b>\"],"
        " \"default\": \"no\", \"default_reason\": null,"
        " \"reversible_until\": 0, \"created_at\": 1, \"state\": 0,"
        " \"room\": \"talk-1\"}", NULL);

    g_assert_nonnull(strstr(html, "class=\"decision-inline\""));
    g_assert_nonnull(strstr(html, "/f/decision/answer"));
    g_assert_nonnull(strstr(html, "value=\"chat\""));
    g_assert_nonnull(strstr(html, "hx-sync=\"this:drop\""));
    g_assert_nonnull(strstr(html, "hx-disabled-elt"));
    g_assert_nonnull(strstr(html, "value=\"yes\""));
    g_assert_nonnull(strstr(html, "If nobody answers"));
    g_assert_nonnull(strstr(html, "chief asks"));

    g_assert_nonnull(strstr(html, "Ship &lt;now&gt;?"));
    /*
     * As text it is escaped; inside the quoted `value` attribute a `<`
     * is not markup, which is how the inbox's option buttons carry it.
     */
    g_assert_nonnull(strstr(html, ">&lt;b&gt;no&lt;/b&gt;"));
    g_assert_null(strstr(html, "><b>"));
}

/*
 * Settled -- here by the inbox -- the same card offers nothing to press
 * and says how it ended and where; a refused press writes the refusal
 * into it.
 */
static void
test_a_settled_inline_decision_says_how(void)
{
    g_autofree gchar *html = inline_card_html(
        "{\"id\": \"d1\", \"agent\": \"chief\", \"question\": \"Ship?\","
        " \"options\": [\"yes\", \"no\"], \"default\": \"no\","
        " \"answer\": \"yes\", \"state\": 1, \"settled_via\": \"inbox\","
        " \"room\": \"talk-1\"}",
        "Already settled: You answered yes in the inbox.");

    g_assert_null(strstr(html, "<form"));
    g_assert_null(strstr(html, "<button"));
    g_assert_nonnull(strstr(html, "decision-inline-settled"));
    g_assert_nonnull(strstr(html, "in the inbox"));
    g_assert_nonnull(strstr(html, "Already settled"));
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

/*
 * A write has to come from this page.
 *
 * The banner says there is no login: reaching a listed address is the
 * whole of the permission, and that is a deliberate decision about the
 * *network*.  A cross-site form quietly turns it into a decision about
 * every page the operator's browser happens to load -- an ordinary
 * urlencoded POST is a "simple request", so it needs no preflight and
 * the attacker never has to read the reply, because the effect is the
 * side effect.  `POST /a/<id>/exec` runs a command inside an agent's
 * computer; `POST /a/<id>/remove` deletes the agent.
 *
 * Tested as a function over header values rather than through a server,
 * because the cases that matter are the ones a browser sends and a test
 * client cannot be made to send.
 */
static void
test_a_cross_site_write_is_refused(void)
{
    /* The browser's own account, when it gives one. */
    g_assert_true(clawt_web_write_is_cross_site(HTMX_METHOD_POST,
                                                "cross-site", NULL, NULL,
                                                "127.0.0.1:8790"));
    g_assert_true(clawt_web_write_is_cross_site(HTMX_METHOD_POST,
                                                "same-site", NULL, NULL,
                                                "127.0.0.1:8790"));
    g_assert_false(clawt_web_write_is_cross_site(HTMX_METHOD_POST,
                                                 "same-origin", NULL, NULL,
                                                 "127.0.0.1:8790"));

    /*
     * "none" is a request the person started themselves -- a typed
     * address or a bookmark -- which no page can cause.
     */
    g_assert_false(clawt_web_write_is_cross_site(HTMX_METHOD_POST, "none",
                                                 NULL, NULL,
                                                 "127.0.0.1:8790"));
}

/* Origin, then Referer, for the browser that sends no Sec-Fetch-Site. */
static void
test_the_origin_is_compared_with_the_host(void)
{
    g_assert_true(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, "https://evil.example", NULL,
        "127.0.0.1:8790"));

    g_assert_false(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, "http://127.0.0.1:8790", NULL,
        "127.0.0.1:8790"));

    g_assert_true(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, NULL, "https://evil.example/page",
        "127.0.0.1:8790"));

    g_assert_false(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, NULL, "http://127.0.0.1:8790/a/x",
        "127.0.0.1:8790"));

    /*
     * A different port on the same host is a different origin, which is
     * the case a split on the last colon gets wrong and an IPv6 address
     * gets wrong twice.
     */
    g_assert_true(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, "http://127.0.0.1:9999", NULL,
        "127.0.0.1:8790"));

    g_assert_false(clawt_web_write_is_cross_site(
        HTMX_METHOD_POST, NULL, "http://[::1]:8790", NULL, "[::1]:8790"));
}

/*
 * A read is never refused, and neither is a client that is not a
 * browser.  Every link anybody has sent anybody else is a GET, and curl
 * cannot be talked into forging the three headers a browser attaches on
 * its own.
 */
static void
test_reads_and_non_browsers_are_left_alone(void)
{
    g_assert_false(clawt_web_write_is_cross_site(
        HTMX_METHOD_GET, "cross-site", "https://evil.example", NULL,
        "127.0.0.1:8790"));

    g_assert_false(clawt_web_write_is_cross_site(HTMX_METHOD_POST, NULL,
                                                 NULL, NULL,
                                                 "127.0.0.1:8790"));
}

/*
 * A write whose destination cannot be established is refused rather than
 * guessed at.  HTTP/1.1 requires a Host, so there is no honest request
 * this turns away.
 */
static void
test_a_write_with_no_host_is_refused(void)
{
    g_assert_true(clawt_web_write_is_cross_site(HTMX_METHOD_POST, NULL,
                                                "http://127.0.0.1:8790",
                                                NULL, NULL));
}

/*
 * A schema BOOLEAN arrives from the daemon as a real JSON boolean, and a
 * reader that only understands "true" reports every one of them as off.
 *
 * The routine and trigger editors both did, with clawt_web_member() --
 * the string reader, which returns its fallback for a non-string node.
 * Every switch rendered unticked, and because the switch posts a
 * `__present` marker whether or not it is ticked, saving the form wrote
 * an explicit false back.  Opening a live routine to fix a typo turned
 * it off; the re-rendered page agreed, so it reported itself to nobody.
 */
static void
test_a_json_boolean_reads_as_on(void)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) node = NULL;
    JsonObject *object;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "catch_up");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_end_object(builder);

    node = json_builder_get_root(builder);
    object = json_node_get_object(node);

    /* This is the shape routine.list and trigger.list actually send. */
    g_assert_true(clawt_web_schema_flag(object, "enabled"));
    g_assert_false(clawt_web_schema_flag(object, "catch_up"));

    /* And the string form agent.show sends, which must keep working. */
    g_assert_false(clawt_web_schema_flag(object, "absent"));
    g_assert_false(clawt_web_schema_flag(NULL, "enabled"));
}

static void
test_a_stringified_boolean_still_reads_as_on(void)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) node = NULL;
    JsonObject *object;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_string_value(builder, "true");
    json_builder_set_member_name(builder, "isolate");
    json_builder_add_string_value(builder, "false");
    json_builder_end_object(builder);

    node = json_builder_get_root(builder);
    object = json_node_get_object(node);

    g_assert_true(clawt_web_schema_flag(object, "enabled"));
    g_assert_false(clawt_web_schema_flag(object, "isolate"));
}

/*
 * And the switch that reads it renders ticked, which is the half a
 * browser sees.  Asserted on the rendered attribute rather than on the
 * boolean, because the boolean was right in one of the two clients the
 * whole time and the page was still wrong.
 */
static void
test_an_enabled_routine_renders_a_ticked_switch(void)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) node = NULL;
    JsonObject *object;
    g_autoptr(HtmxDiv) field = NULL;
    g_autofree gchar *html = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    node = json_builder_get_root(builder);
    object = json_node_get_object(node);

    field = clawt_web_switch_field("enabled", "enabled", NULL,
                                   clawt_web_schema_flag(object, "enabled"));
    html = htmx_element_render(HTMX_ELEMENT(field));

    g_assert_nonnull(strstr(html, "checked=\"checked\""));
}

/* ── Looking at a face properly ────────────────────────────────── */

/*
 * The viewer is emitted once per document, outside the transcript.
 *
 * Both halves matter and both are invisible when wrong.  A run header
 * is drawn once per run, so an overlay beside each face would repeat
 * one element id down the length of a conversation -- and
 * `getElementById` resolves that in favour of whichever came first, so
 * every face after the top of the page would open the top of the
 * page's picture.  And htmx swaps `#transcript` whole on every fleet
 * event, so an overlay inside it would vanish mid-look.
 */
static void
test_the_picture_viewer_is_one_element_outside_the_transcript(void)
{
    g_autofree gchar *html =
        clawt_web_page(NULL, "alpha", CLAWT_PAGE_CHAT, NULL, NULL);
    const gchar *first = strstr(html, "id=\"avatar-zoom\"");

    g_assert_nonnull(first);
    g_assert_null(strstr(first + 1, "id=\"avatar-zoom\""));

    /*
     * And it carries no `src`.  The overlay costs no request until
     * somebody opens one, which is what keeps a page that must work on
     * a tailnet with no route out honest -- and the script fills it
     * from the button's own attribute at click time.
     */
    g_assert_null(strstr(html, "id=\"avatar-zoom\"><img src"));
}

/*
 * Escape closes it, and so does clicking the backdrop.
 *
 * The GTK window this mirrors takes Escape through a
 * GtkEventControllerKey, and an overlay with no way out but a control
 * somebody has to find is a modal dialog by accident.
 */
static void
test_the_picture_viewer_can_be_dismissed(void)
{
    g_autofree gchar *html =
        clawt_web_page(NULL, "alpha", CLAWT_PAGE_CHAT, NULL, NULL);

    g_assert_nonnull(strstr(html, "function unzoom()"));
    g_assert_nonnull(strstr(html, "e.key==='Escape'"));
    g_assert_nonnull(strstr(html, "z.contains(e.target)"));

    /*
     * ...and closing clears the `src`, so a picture the agent has
     * replaced since is re-asked for rather than served out of a node
     * still holding the old one.
     */
    g_assert_nonnull(strstr(html, "i.removeAttribute('src')"));
}

/*
 * The transcript's face is half again the sidebar's step, and the
 * cursor only offers to enlarge where there is something to enlarge.
 *
 * A face in a transcript is looked at; the sidebar's is a locator in a
 * list, and stays small.  The zoom affordance is on `.avatar-zoom`
 * rather than on `.msg-avatar` because an agent with no picture draws
 * its initials, and offering to enlarge a letter is an affordance that
 * answers nothing on most of the fleet.
 */
static void
test_the_transcript_face_is_larger_and_only_zooms_with_a_picture(void)
{
    const gchar *css = clawt_web_stylesheet();

    g_assert_nonnull(strstr(css, ".msg-avatar{"));
    g_assert_nonnull(strstr(css, "width:42px;height:42px"));

    /* The sidebar's did not move with it. */
    g_assert_nonnull(strstr(css, ".agent-face{"));
    g_assert_nonnull(strstr(css, "width:24px;height:24px"));

    g_assert_nonnull(strstr(css, ".avatar-zoom{"));
    g_assert_nonnull(strstr(css, "cursor:zoom-in"));
    g_assert_nonnull(strstr(css, "#avatar-zoom.on{display:flex}"));

    /*
     * Hidden until it is opened.  A `display:none` that was never
     * written would leave a full-screen black square over the page from
     * the moment it loaded, which is the kind of failure that is
     * obvious in a browser and invisible in a diff.
     */
    g_assert_nonnull(strstr(css, "#avatar-zoom{display:none"));
}

/* ── Generated faces (0.53.0) ───────────────────────────────────── */

/*
 * The order every client keeps: a picture, else the face the daemon
 * resolved (an inline SVG the stylesheet can blink and the script can
 * turn), else -- for initials, or a shape this build does not know --
 * the letter.
 */
static void
test_the_avatar_draws_a_face(void)
{
    g_autoptr(HtmxElement) picture = NULL;
    g_autoptr(HtmxElement) face = NULL;
    g_autoptr(HtmxElement) letter = NULL;
    g_autoptr(HtmxElement) unknown = NULL;
    g_autofree gchar *picture_html = NULL;
    g_autofree gchar *face_html = NULL;
    g_autofree gchar *letter_html = NULL;
    g_autofree gchar *unknown_html = NULL;

    picture = clawt_web_avatar("Scout", "scout", TRUE, NULL, "agent-face");
    picture_html = htmx_element_render(picture);
    g_assert_nonnull(strstr(picture_html, "<img"));
    g_assert_null(strstr(picture_html, "<svg"));

    clawt_web_face_note("scout", "cloud");
    face = clawt_web_avatar("Scout", "scout", FALSE, "#22b573", "agent-face");
    face_html = htmx_element_render(face);
    g_assert_nonnull(strstr(face_html, "<svg"));
    g_assert_nonnull(strstr(face_html, "web-face"));
    g_assert_nonnull(strstr(face_html, "agent-face"));
    g_assert_nonnull(strstr(face_html, "data-face=\"cloud\""));
    g_assert_nonnull(strstr(face_html, "fill=\"#22b573\""));
    g_assert_nonnull(strstr(face_html, "--blink-gap:"));

    clawt_web_face_note("scout", "initials");
    letter = clawt_web_avatar("Scout", "scout", FALSE, NULL, "agent-face");
    letter_html = htmx_element_render(letter);
    g_assert_null(strstr(letter_html, "<svg"));
    g_assert_nonnull(strstr(letter_html, ">S<"));

    clawt_web_face_note("scout", "hexagon");
    unknown = clawt_web_avatar("Scout", "scout", FALSE, NULL, "agent-face");
    unknown_html = htmx_element_render(unknown);
    g_assert_null(strstr(unknown_html, "<svg"));

    clawt_web_face_note("scout", NULL);
}

/*
 * A colour typed into YAML reaches the face's markup only when it is a
 * hex colour; anything else is the derived colour.  A face is the one
 * place a configured value is written into an attribute unescaped (it
 * is SVG markup), so this is the check that keeps it out.
 */
static void
test_a_face_never_carries_a_hostile_colour(void)
{
    g_autoptr(HtmxElement) face = clawt_web_avatar(
        "Scout", "scout", FALSE, "red\"/><script>alert(1)</script>",
        "agent-face");
    g_autofree gchar *html = htmx_element_render(face);
    g_autofree gchar *derived = g_strdup_printf(
        "fill=\"%s\"", clawt_avatar_color("scout", "Scout", NULL));

    g_assert_null(strstr(html, "<script"));
    g_assert_nonnull(strstr(html, derived));
}

/*
 * The stylesheet blinks every face on its own period and moves the eyes
 * by exactly the library's reach -- a number written here by hand, so
 * this pins it to CLAWT_FACE_LOOK_REACH: a reach changed in one place
 * would put the web client's eyes somewhere the others' are not.  And
 * a browser that asks for reduced motion gets still faces.
 */
static void
test_the_stylesheet_moves_faces_like_the_library(void)
{
    const gchar *css = clawt_web_stylesheet();
    g_autofree gchar *across = g_strdup_printf(
        "var(--look-x,0)*%gpx", CLAWT_FACE_LOOK_REACH);
    g_autofree gchar *down = g_strdup_printf(
        "var(--look-y,0)*%gpx", CLAWT_FACE_LOOK_REACH * 0.7);

    g_assert_nonnull(strstr(css, across));
    g_assert_nonnull(strstr(css, down));
    g_assert_nonnull(strstr(css, "@keyframes face-blink"));
    g_assert_nonnull(strstr(css, "var(--blink-gap,4000ms)"));
    g_assert_nonnull(strstr(css, "var(--blink-delay,0ms)"));
    g_assert_nonnull(strstr(css, "@media (prefers-reduced-motion:reduce){"
                                 ".face .face-eye{animation:none}"));
}

/*
 * Still faces are an explicit "0" in clawt_faces, as descriptions are;
 * the look sheet then stops the blink and tells the script to leave the
 * eyes alone.  No cookie, or anything else, is faces that move.
 */
static void
test_still_faces_are_an_explicit_choice(void)
{
    ClawtWebLook still = { 0 };
    ClawtWebLook moving = { 0 };
    g_autofree gchar *still_css = NULL;
    g_autofree gchar *moving_css = NULL;
    g_autoptr(ClawtWebLook) none = clawt_web_look_from_request(NULL);

    g_assert_false(none->still_faces);

    still.still_faces = TRUE;
    still_css = clawt_web_look_css(&still);
    moving_css = clawt_web_look_css(&moving);

    g_assert_nonnull(strstr(still_css, "--face-motion:0"));
    g_assert_nonnull(strstr(still_css, "animation:none"));
    g_assert_null(strstr(moving_css, "--face-motion"));

    {
        g_autofree gchar *value = cookie_value(
            "clawt_agent_desc=0; clawt_faces=0", "clawt_faces");

        g_assert_cmpstr(value, ==, "0");
    }
}

/* How many times @needle occurs in @haystack. */
static guint
count_of(const gchar *haystack, const gchar *needle)
{
    guint n = 0;
    const gchar *at = haystack;

    while ((at = strstr(at, needle)) != NULL) {
        n++;
        at += strlen(needle);
    }

    return n;
}

/*
 * Whether @html has an <input> in @group with @value that is checked,
 * whatever order its attributes were written in.
 */
static gboolean
radio_checked(const gchar *html, const gchar *group, const gchar *value)
{
    g_autofree gchar *want_name = g_strdup_printf("name=\"%s\"", group);
    g_autofree gchar *want_value = g_strdup_printf("value=\"%s\"", value);
    const gchar *at = html;

    while ((at = strstr(at, "<input")) != NULL) {
        const gchar *end = strchr(at, '>');
        g_autofree gchar *tag = g_strndup(at, end != NULL ? (gsize)(end - at)
                                                          : strlen(at));

        if (strstr(tag, want_name) != NULL && strstr(tag, want_value) != NULL &&
            strstr(tag, "checked") != NULL)
            return TRUE;
        at += 6;
    }

    return FALSE;
}

/*
 * The picker is a form without any script: one face radio per shape
 * plus automatic and initials, one colour radio per swatch plus
 * automatic, the starting choice checked -- and a colour that is not a
 * swatch kept as one more, rather than silently dropped back to
 * automatic on the next save.
 */
static void
test_the_face_picker_is_a_form(void)
{
    g_autoptr(HtmxElement) picker = clawt_web_face_picker("scout", "Star",
                                                          "#e5484d");
    g_autoptr(HtmxElement) custom = clawt_web_face_picker("scout", NULL,
                                                          "#123456");
    g_autoptr(HtmxElement) hostile = clawt_web_face_picker(
        "scout", "\"><script>", "red\"><script>");
    g_autofree gchar *html = htmx_element_render(picker);
    g_autofree gchar *custom_html = htmx_element_render(custom);
    g_autofree gchar *hostile_html = htmx_element_render(hostile);

    g_assert_cmpuint(count_of(html, "name=\"face\""), ==,
                     clawt_face_shape_count() + 2);
    g_assert_cmpuint(count_of(html, "name=\"color\""), ==,
                     CLAWT_FACE_PALETTE_SIZE + 1);
    g_assert_cmpuint(count_of(html, "checked=\"checked\""), ==, 2);
    g_assert_true(radio_checked(html, "face", "star"));
    g_assert_true(radio_checked(html, "color", "#e5484d"));
    g_assert_nonnull(strstr(html, "face-shuffle"));
    g_assert_nonnull(strstr(html, "face-preview"));

    /* a non-swatch colour is offered, and chosen */
    g_assert_cmpuint(count_of(custom_html, "name=\"color\""), ==,
                     CLAWT_FACE_PALETTE_SIZE + 2);
    g_assert_true(radio_checked(custom_html, "color", "#123456"));
    /* and no face named is automatic, checked */
    g_assert_true(radio_checked(custom_html, "face", ""));

    /* nothing typed reaches the markup unescaped */
    g_assert_null(strstr(hostile_html, "<script"));
}

/*
 * The Face form writes only what changed: a colour picked leaves a face
 * set by hand in the YAML alone, an unchanged form writes nothing, and
 * a field the form did not carry is never written as "".
 */
static void
test_the_face_form_writes_only_changes(void)
{
    g_autoptr(GPtrArray) colour_only =
        clawt_web_face_changes("star", "star", "", "#22b573");
    g_autoptr(GPtrArray) nothing =
        clawt_web_face_changes("star", "star", "#22b573", "#22b573");
    g_autoptr(GPtrArray) both =
        clawt_web_face_changes(NULL, "cloud", NULL, "#e5484d");
    g_autoptr(GPtrArray) to_automatic =
        clawt_web_face_changes("cloud", "", "#e5484d", NULL);

    g_assert_cmpuint(colour_only->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(colour_only, 0), ==, "color");
    g_assert_cmpuint(nothing->len, ==, 0);
    g_assert_cmpuint(both->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(both, 0), ==, "face");
    g_assert_cmpuint(to_automatic->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(to_automatic, 0), ==, "face");
}

/*
 * The page script turns faces' eyes over a row, a face or a picker, and
 * runs Shuffle -- both delegated, since htmx swaps the sidebar and the
 * page under them.
 */
static void
test_the_page_script_moves_faces(void)
{
    g_autoptr(HtmxDiv) body = htmx_div_new();
    g_autofree gchar *html = clawt_web_page(NULL, "alpha", CLAWT_PAGE_CHAT,
                                            HTMX_ELEMENT(body), NULL);

    g_assert_nonnull(strstr(html, "addEventListener('pointermove'"));
    g_assert_nonnull(strstr(html, "'.agent-row,.web-face,.face-picker'"));
    g_assert_nonnull(strstr(html, "--face-motion"));
    g_assert_nonnull(strstr(html, "prefers-reduced-motion"));
    g_assert_nonnull(strstr(html, ".face-shuffle"));
}

/* ── Descriptions in the agent list ────────────────────────────── */

/*
 * A browser that has said nothing gets descriptions.
 *
 * The same rule the GTK client's appearance file follows, and it has to
 * be, or the two clients would answer one preference differently -- the
 * cookie's absence is the common case, since it is only written when
 * somebody turns the setting off.
 */
static void
test_a_browser_with_no_cookie_shows_descriptions(void)
{
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(NULL);

    g_assert_false(look->hide_descriptions);
}

/*
 * Only an explicit "0" turns them off.
 *
 * A cookie is edited as easily as a config file, and a value nobody can
 * read has to leave the list as it has always been rather than silently
 * emptying every row -- which would read as the descriptions being
 * gone rather than as a mangled preference.
 */
static void
test_only_an_explicit_zero_hides_descriptions(void)
{
    g_assert_true(descriptions_hidden("0"));

    g_assert_false(descriptions_hidden("1"));
    g_assert_false(descriptions_hidden("maybe"));
    g_assert_false(descriptions_hidden(""));
    g_assert_false(descriptions_hidden(NULL));

    /*
     * And the header parser reaches it whole.  "clawt_agent_desc" is a
     * prefix of nothing here, but the reverse has already been a bug
     * in this file -- looking for "clawt_font" and finding
     * "clawt_font_size" -- so the pairing is asserted rather than
     * assumed.
     */
    {
        g_autofree gchar *value = cookie_value(
            "clawt_theme=dark; clawt_agent_desc=0; clawt_run_gap=18",
            "clawt_agent_desc");

        g_assert_cmpstr(value, ==, "0");
        g_assert_true(descriptions_hidden(value));
    }
}

/*
 * The line the sidebar draws clips rather than widening the column.
 *
 * A description is a sentence, and a sentence sets its element's
 * natural width -- so without the clamp and the `min-width:0` beside it
 * the sidebar scrolls sideways instead of ellipsising, with nothing
 * logged and nothing to say why.  The same trap a wrapping GtkLabel
 * sets for a GtkScrolledWindow left at POLICY_AUTOMATIC, which this
 * codebase has already been bitten by.
 */
static void
test_the_sidebar_description_clips(void)
{
    const gchar *css = clawt_web_stylesheet();
    const gchar *rule = strstr(css, ".agent-desc{");

    g_assert_nonnull(rule);
    g_assert_nonnull(strstr(rule, "min-width:0"));
    g_assert_nonnull(strstr(rule, "overflow:hidden"));
    g_assert_nonnull(strstr(rule, "line-clamp:2"));
}

/* A JSON object for a fixture, or a failed test. */
static JsonNode *
json_fixture(const gchar *text)
{
    g_autoptr(GError) error = NULL;
    JsonNode *node = json_from_string(text, &error);

    g_assert_no_error(error);
    g_assert_nonnull(node);
    return node;
}

/*
 * The "On fallback" badge: the library's words, the sentence as the
 * title -- quoted safely, since it carries an agent's name -- and nothing at
 * all while the agent is on its own backend.
 */
static void
test_the_failover_badge(void)
{
    g_autoptr(JsonNode) away = json_fixture(
        "{\"id\":\"scout\",\"failover\":{\"state\":\"fallback\","
        "\"primary\":{\"provider\":\"claude-code\",\"model\":\"opus\"},"
        "\"active\":{\"provider\":\"codex-cli\",\"model\":\"gpt-5.5\","
        "\"index\":0},\"reason\":\"usage-limit\"},"
        "\"failover_message\":\"Scout\\\" onmouseover=\\\"x() is away\"}");
    g_autoptr(JsonNode) home = json_fixture("{\"id\":\"scout\"}");
    g_autoptr(HtmxSpan) badge =
        clawt_web_failover_badge(json_node_get_object(away));
    g_autofree gchar *html = NULL;

    g_assert_nonnull(badge);
    html = htmx_element_render(HTMX_ELEMENT(badge));
    g_assert_nonnull(strstr(html, "On fallback: codex-cli gpt-5.5"));
    g_assert_nonnull(strstr(html, "badge-warn"));
    g_assert_nonnull(strstr(html, "title=\""));
    g_assert_null(strstr(html, "\" onmouseover=\""));

    g_assert_null(clawt_web_failover_badge(json_node_get_object(home)));
    g_assert_null(clawt_web_failover_badge(NULL));
}

/*
 * The text between @from and the next @to in @html, for asserting on
 * one control of a form rather than on the whole page.
 */
static gchar *
slice(const gchar *html, const gchar *from, const gchar *to)
{
    const gchar *start = strstr(html, from);
    const gchar *end;

    g_assert_nonnull(start);
    end = strstr(start, to);
    g_assert_nonnull(end);

    return g_strndup(start, (gsize)(end - start));
}

/*
 * The fallback editor: a row per entry in order, each with the entry's
 * provider selected, its model in a text box that suggests the
 * provider's catalogue, and only logins for that CLI -- plus the one it
 * names -- in its account picker.  Providers no agent runs on are not
 * offered.  The buttons say which edit they are.
 */
static void
test_the_fallback_editor(void)
{
    g_autoptr(JsonNode) catalog = json_fixture(
        "{\"providers\":["
        "{\"id\":\"claude-code\",\"label\":\"Claude Code\",\"agent\":true,"
        " \"models\":[{\"id\":\"opus\",\"label\":\"Opus\"},"
        "             {\"id\":\"sonnet\",\"label\":\"Sonnet\"}]},"
        "{\"id\":\"codex-cli\",\"label\":\"Codex\",\"agent\":true,"
        " \"models\":[{\"id\":\"gpt-5.5\",\"label\":\"GPT-5.5\"}]},"
        "{\"id\":\"openai\",\"label\":\"OpenAI\",\"agent\":false,"
        " \"models\":[{\"id\":\"gpt-4o\",\"label\":\"GPT-4o\"}]}]}");
    g_autoptr(JsonNode) accounts = json_fixture(
        "{\"accounts\":["
        "{\"id\":\"backup\",\"provider\":\"codex-cli\"},"
        "{\"id\":\"work\",\"provider\":\"claude-code\","
        " \"description\":\"<i>the office</i>\"}]}");
    g_autoptr(HtmxDiv) card = clawt_web_fallback_card(
        "scout", "codex-cli:gpt-5.5@backup, claude-code:sonnet",
        "claude-code", json_node_get_object(catalog),
        json_node_get_object(accounts));
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(card));
    g_autofree gchar *first_account = NULL;
    g_autofree gchar *first_provider = NULL;

    g_assert_nonnull(strstr(html, "action=\"/a/scout/fallbacks\""));
    g_assert_nonnull(strstr(html, "name=\"fb_count\""));
    g_assert_nonnull(strstr(html, "name=\"fb_model_0\""));
    g_assert_nonnull(strstr(html, "value=\"gpt-5.5\""));
    g_assert_nonnull(strstr(html, "name=\"fb_model_1\""));
    g_assert_nonnull(strstr(html, "list=\"fallback-models-"));
    g_assert_nonnull(strstr(html, "<datalist"));

    /* the edits, named by their buttons */
    g_assert_nonnull(strstr(html, "value=\"up:1\""));
    g_assert_nonnull(strstr(html, "value=\"down:0\""));
    g_assert_nonnull(strstr(html, "value=\"remove:1\""));
    g_assert_nonnull(strstr(html, "value=\"add\""));
    g_assert_nonnull(strstr(html, "value=\"save\""));

    /* the first entry runs codex-cli, so only codex logins are offered */
    first_provider = slice(html, "name=\"fb_provider_0\"", "</select>");
    g_assert_nonnull(strstr(first_provider,
                            "selected=\"selected\" value=\"codex-cli\""));
    g_assert_null(strstr(first_provider, "value=\"openai\""));

    first_account = slice(html, "name=\"fb_account_0\"", "</select>");
    g_assert_nonnull(strstr(first_account,
                            "selected=\"selected\" value=\"backup\""));
    g_assert_null(strstr(first_account, "value=\"work\""));

    /* a description is somebody's text */
    g_assert_null(strstr(html, "<i>"));
}

/*
 * A list that cannot be read is said, not edited: an empty form saved
 * back would write the empty list over the one in clawtilla.yaml.
 */
static void
test_the_fallback_editor_will_not_save_over_a_bad_list(void)
{
    g_autoptr(HtmxDiv) card = clawt_web_fallback_card(
        "scout", "codex-cli", "claude-code", NULL, NULL);
    g_autofree gchar *html = htmx_element_render(HTMX_ELEMENT(card));

    g_assert_nonnull(strstr(html, "notice-bad"));
    g_assert_null(strstr(html, "fb_count"));
    g_assert_null(strstr(html, "<form"));
}

/* ── The web app manifest, folded requests and the exchange ─────── */

/* The manifest's fields, and that every icon it names is vendored. */
static void
test_the_manifest_names_what_an_install_needs(void)
{
	g_autofree gchar *json = clawt_web_manifest_json();
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *root;
	JsonArray *icons;
	gboolean any_192 = FALSE;
	gboolean any_512 = FALSE;
	guint i;

	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);
	root = json_node_get_object(json_parser_get_root(parser));
	g_assert_nonnull(root);

	g_assert_cmpstr(json_object_get_string_member(root, "name"), ==,
			"clawtilla");
	g_assert_cmpstr(json_object_get_string_member(root, "start_url"), ==,
			"/");
	g_assert_cmpstr(json_object_get_string_member(root, "scope"), ==, "/");
	g_assert_cmpstr(json_object_get_string_member(root, "display"), ==,
			"standalone");

	/*
	 * Root-relative URLs only: the client answers at the loopback, a
	 * tailnet address and a proxy's name, and one absolute URL in here
	 * would be wrong at two of them.
	 */
	g_assert_null(strstr(json, "http:"));
	g_assert_null(strstr(json, "https:"));

	icons = json_object_get_array_member(root, "icons");
	g_assert_nonnull(icons);

	for (i = 0; i < json_array_get_length(icons); i++) {
		JsonObject *icon = json_array_get_object_element(icons, i);
		const gchar *src = json_object_get_string_member(icon, "src");
		const gchar *sizes = json_object_get_string_member(icon, "sizes");
		g_autofree gchar *path = NULL;
		g_autofree gchar *bytes = NULL;
		gsize length = 0;
		guint32 width;
		guint32 height;
		g_autofree gchar *want = NULL;

		g_assert_true(g_str_has_prefix(src, "/static/"));
		g_assert_cmpstr(json_object_get_string_member(icon, "type"), ==,
				"image/png");

		/*
		 * The file is in data/web, where /static is served from, and
		 * it is a PNG of the size the manifest claims -- read from the
		 * IHDR chunk rather than trusted from the name.
		 */
		path = g_build_filename(CLAWT_TEST_FIXTURES, "..", "..", "data",
					"web", src + strlen("/static/"), NULL);
		g_assert_true(g_file_get_contents(path, &bytes, &length, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(length, >, 24);
		g_assert_cmpint(memcmp(bytes, "\x89PNG\r\n\x1a\n", 8), ==, 0);

		width = ((guint32)(guchar)bytes[16] << 24) |
			((guint32)(guchar)bytes[17] << 16) |
			((guint32)(guchar)bytes[18] << 8) | (guint32)(guchar)bytes[19];
		height = ((guint32)(guchar)bytes[20] << 24) |
			 ((guint32)(guchar)bytes[21] << 16) |
			 ((guint32)(guchar)bytes[22] << 8) | (guint32)(guchar)bytes[23];
		want = g_strdup_printf("%ux%u", width, height);
		g_assert_cmpstr(want, ==, sizes);

		any_192 = any_192 || width == 192;
		any_512 = any_512 || width == 512;
	}

	/* the two sizes an installability check asks for */
	g_assert_true(any_192);
	g_assert_true(any_512);
}

/* Every page links the manifest, credentialed, and nothing off-host. */
static void
test_every_page_links_the_manifest(void)
{
	g_autofree gchar *html = NULL;

	html = clawt_web_page(NULL, "chief", CLAWT_PAGE_CHAT, NULL, NULL);
	g_assert_nonnull(html);
	g_assert_nonnull(strstr(html, "rel=\"manifest\""));
	g_assert_nonnull(strstr(html, "href=\"" CLAWT_WEB_MANIFEST_PATH "\""));
	g_assert_nonnull(strstr(html, "crossorigin=\"use-credentials\""));
	g_assert_nonnull(strstr(html, "rel=\"apple-touch-icon\""));
	g_assert_null(strstr(html, "src=\"http"));
	g_assert_null(strstr(html, "href=\"http"));
}

typedef struct {
	GMainLoop *loop;
	gchar     *content_type;
	gchar     *body;
	guint      status;
} ManifestFetch;

/* Answers every request with exactly what the route returns. */
static void
on_manifest_request(SoupServer *server, SoupServerMessage *message,
		    const char *path, GHashTable *query, gpointer user_data)
{
	g_autoptr(HtmxResponse) response = clawt_web_manifest_response();

	(void)server;
	(void)path;
	(void)query;
	(void)user_data;

	htmx_response_apply(response, message);
}

static void
on_manifest_fetched(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ManifestFetch *fetch = user_data;
	SoupMessage *message;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;

	bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
						  &error);
	g_assert_no_error(error);
	message = soup_session_get_async_result_message(SOUP_SESSION(source),
							result);
	fetch->status = soup_message_get_status(message);
	fetch->content_type = g_strdup(soup_message_headers_get_content_type(
		soup_message_get_response_headers(message), NULL));
	fetch->body = g_strndup(g_bytes_get_data(bytes, NULL),
				g_bytes_get_size(bytes));
	g_main_loop_quit(fetch->loop);
}

/*
 * Served, over a real HTTP exchange on the loopback: the media type a
 * browser's installability check reads off the wire, not the one the
 * handler meant to set.
 */
static void
test_the_manifest_is_served_as_a_manifest(void)
{
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	/*
	 * No proxy resolver: the default one is libproxy, which may go
	 * looking for a PAC file, and this test must reach nothing but the
	 * loopback server it started.
	 */
	g_autoptr(SoupSession) session = soup_session_new_with_options(
		"proxy-resolver", NULL, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *url = NULL;
	ManifestFetch fetch = { NULL, NULL, NULL, 0 };
	GSList *uris;

	soup_server_add_handler(server, CLAWT_WEB_MANIFEST_PATH,
				on_manifest_request, NULL, NULL);
	g_assert_true(soup_server_listen_local(server, 0,
					       SOUP_SERVER_LISTEN_IPV4_ONLY,
					       &error));
	g_assert_no_error(error);

	uris = soup_server_get_uris(server);
	g_assert_nonnull(uris);
	url = g_strdup_printf("http://127.0.0.1:%d" CLAWT_WEB_MANIFEST_PATH,
			      g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	fetch.loop = loop;
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
					 NULL, on_manifest_fetched, &fetch);
	g_main_loop_run(loop);

	g_assert_cmpuint(fetch.status, ==, 200);
	g_assert_cmpstr(fetch.content_type, ==, CLAWT_WEB_MANIFEST_TYPE);
	g_assert_true(json_parser_load_from_data(parser, fetch.body, -1,
						 &error));
	g_assert_no_error(error);

	g_free(fetch.content_type);
	g_free(fetch.body);
	soup_server_disconnect(server);
}

/* A folded request: one escaped line, the body, and both actions. */
static void
test_a_folded_request_renders_closed_and_escaped(void)
{
	g_autoptr(HtmxDiv) body = htmx_div_new();
	g_autoptr(HtmxElement) fold = NULL;
	g_autofree gchar *html = NULL;
	const gchar *text = "<script>alert(1)</script> step one\nstep two";

	htmx_node_set_text_content(HTMX_NODE(body), "the whole body");
	fold = clawt_web_fold_element(HTMX_ELEMENT(g_steal_pointer(&body)),
				      "lead", "chief&co", "m-1", text);
	html = htmx_element_render(fold);

	/* a <details> that is closed until somebody opens it */
	g_assert_true(g_str_has_prefix(html, "<details"));
	g_assert_null(strstr(html, " open"));
	g_assert_nonnull(strstr(html, "id=\"fold-m-1\""));
	g_assert_nonnull(strstr(html, "hx-preserve=\"true\""));
	g_assert_nonnull(strstr(html, "class=\"fold-line\""));

	/* the preview is text, escaped, on one line */
	g_assert_nonnull(strstr(html, "&lt;script&gt;alert(1)&lt;/script&gt; "
				      "step one step two"));
	g_assert_null(strstr(html, "<script>"));
	g_assert_nonnull(strstr(html, "the whole body"));

	/* both actions, the exchange a real link with the peer escaped */
	g_assert_nonnull(strstr(html, ">Show less<"));
	g_assert_nonnull(strstr(html, ">View exchange<"));
	g_assert_nonnull(strstr(html,
		"href=\"/a/lead/exchange?with=chief%26co\""));
	g_assert_nonnull(strstr(html,
		"hx-get=\"/f/a/lead/exchange?with=chief%26co\""));
	g_assert_nonnull(strstr(html, "hx-target=\"#exchange-panel\""));
}

/* The exchange: the two agents only, in order, the request marked. */
static void
test_the_exchange_lists_requests_and_answers(void)
{
	g_autoptr(JsonArray) messages = json_array_new();
	g_autoptr(JsonArray) exchange = NULL;
	g_autoptr(HtmxElement) panel = NULL;
	g_autoptr(HtmxElement) page = NULL;
	g_autofree gchar *panel_html = NULL;
	g_autofree gchar *page_html = NULL;
	const gchar *request_at;
	const gchar *answer_at;
	static const gchar *rows[][2] = {
		{ "chief", "Please <b>build</b> it." },
		{ "user", "operator aside" },
		{ "lead", "an answer to the operator" },
		{ "chief", "And the tests." },
		{ "lead", "Built and tested." },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++) {
		JsonObject *one = json_object_new();

		json_object_set_string_member(one, "sender", rows[i][0]);
		json_object_set_string_member(one, "body", rows[i][1]);
		json_array_add_object_element(messages, one);
	}

	exchange = clawt_chat_exchange(messages, "lead", "chief");
	panel = clawt_web_exchange_element(exchange, "lead", "chief", TRUE);
	panel_html = htmx_element_render(panel);

	g_assert_nonnull(strstr(panel_html, "Between chief and lead"));
	g_assert_nonnull(strstr(panel_html, "class=\"exchange-close\""));
	g_assert_null(strstr(panel_html, "operator aside"));
	g_assert_null(strstr(panel_html, "an answer to the operator"));

	/* markdown rendered, a person's tag escaped rather than obeyed */
	g_assert_null(strstr(panel_html, "<b>build</b>"));

	request_at = strstr(panel_html, "And the tests.");
	answer_at = strstr(panel_html, "Built and tested.");
	g_assert_nonnull(request_at);
	g_assert_nonnull(answer_at);
	g_assert_true(request_at < answer_at);
	g_assert_nonnull(strstr(panel_html, "class=\"exchange-request\""));
	g_assert_nonnull(strstr(panel_html, "class=\"exchange-answer\""));

	/* as a page of its own, a way back instead of a close button */
	page = clawt_web_exchange_element(exchange, "lead", "chief", FALSE);
	page_html = htmx_element_render(page);
	g_assert_null(strstr(page_html, "exchange-close"));
	g_assert_nonnull(strstr(page_html, "href=\"/a/lead/chat?with=chief\""));
}


/* ── Connector budget and MCP apps ───────────────────────────────── */

/* An attribute's value, as a browser would decode it. */
static gchar *
attribute_value(const gchar *html, const gchar *name)
{
    g_autofree gchar *needle = g_strdup_printf(" %s=\"", name);
    const gchar *start = strstr(html, needle);
    const gchar *end;
    g_autoptr(GString) out = g_string_new(NULL);
    const gchar *p;

    if (start == NULL)
        return NULL;

    start += strlen(needle);
    end = strchr(start, '"');
    g_assert_nonnull(end);

    for (p = start; p < end; p++) {
        if (g_str_has_prefix(p, "&quot;")) { g_string_append_c(out, '"'); p += 5; }
        else if (g_str_has_prefix(p, "&lt;")) { g_string_append_c(out, '<'); p += 3; }
        else if (g_str_has_prefix(p, "&gt;")) { g_string_append_c(out, '>'); p += 3; }
        else if (g_str_has_prefix(p, "&#39;")) { g_string_append_c(out, '\''); p += 4; }
        else if (g_str_has_prefix(p, "&#x27;")) { g_string_append_c(out, '\''); p += 5; }
        else if (g_str_has_prefix(p, "&amp;")) { g_string_append_c(out, '&'); p += 4; }
        else g_string_append_c(out, *p);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* A connector.app_get reply, as the daemon builds it. */
static JsonObject *
app_reply(gboolean with_view)
{
    JsonObject *app = json_object_new();
    JsonObject *input = json_object_new();
    JsonObject *result = json_object_new();
    JsonArray *refused = json_array_new();

    json_object_set_string_member(app, "id", "a0123456789abcdef");
    json_object_set_string_member(app, "agent", "oryx");
    json_object_set_string_member(app, "connector", "draw");
    json_object_set_string_member(app, "tool", "create_view");
    json_object_set_string_member(app, "title", "Diagram");
    json_object_set_string_member(app, "sandbox",
                                  CLAWT_MCP_APP_IFRAME_SANDBOX);

    if (with_view) {
        g_autofree gchar *csp = clawt_mcp_app_build_csp(NULL, FALSE, NULL);
        g_autofree gchar *document = clawt_mcp_app_build_document(
            "<!doctype html><p>\"hi\" & <b>bye</b></p></script>", csp);

        json_object_set_string_member(app, "document", document);
        json_object_set_string_member(app, "csp", csp);
    } else {
        json_object_set_string_member(app, "blocked",
                                      "its view is bigger than clawtilla "
                                      "shows");
    }

    json_object_set_string_member(input, "q", "</script><b>x</b>");
    json_object_set_object_member(app, "input", input);
    json_object_set_boolean_member(result, "isError", FALSE);
    json_object_set_object_member(app, "result", result);
    json_array_add_string_element(refused, "http://evil.example");
    json_object_set_array_member(app, "refused", refused);

    return app;
}

/*
 * The frame is the isolation: exactly allow-scripts, so an opaque
 * origin with no reach into this page, and the policy before a byte of
 * the server's.
 */
static void
test_an_opened_app_is_sandboxed(void)
{
    g_autoptr(JsonObject) app = app_reply(TRUE);
    g_autoptr(HtmxElement) frame = clawt_web_app_frame(app);
    g_autofree gchar *html = htmx_element_render(frame);
    g_autofree gchar *sandbox = attribute_value(html, "sandbox");
    g_autofree gchar *srcdoc = attribute_value(html, "srcdoc");

    g_assert_nonnull(strstr(html, "<iframe"));
    g_assert_cmpstr(sandbox, ==, "allow-scripts");
    g_assert_null(strstr(html, "allow-same-origin"));
    g_assert_null(strstr(html, " allow=\""));
    g_assert_nonnull(strstr(html, "referrerpolicy=\"no-referrer\""));
    g_assert_nonnull(srcdoc);
    g_assert_true(g_str_has_prefix(
        srcdoc, "<!DOCTYPE html><meta http-equiv=\"Content-Security-Policy\""));
    g_assert_nonnull(strstr(srcdoc, "default-src 'none'"));
    g_assert_nonnull(strstr(srcdoc, "form-action 'none'"));
    g_assert_nonnull(strstr(srcdoc, "<p>\"hi\" & <b>bye</b></p>"));

    /*
     * The view's content and the tool's input stay inside their
     * attributes: no quote of theirs reaches the page unescaped, which
     * is the only character that could end the attribute early.
     */
    g_assert_null(strstr(html, "\"hi\""));
    g_assert_null(strstr(html, "\"q\""));
    g_assert_nonnull(strstr(html, "data-app-input="));
    g_assert_nonnull(strstr(html, "http://evil.example"));
}

static void
test_a_blocked_app_says_why_and_draws_no_frame(void)
{
    g_autoptr(JsonObject) app = app_reply(FALSE);
    g_autoptr(HtmxElement) frame = clawt_web_app_frame(app);
    g_autofree gchar *html = htmx_element_render(frame);

    g_assert_null(strstr(html, "<iframe"));
    g_assert_null(strstr(html, "srcdoc"));
    g_assert_nonnull(strstr(html, "its view is bigger than clawtilla shows"));
    g_assert_nonnull(strstr(html, "app-blocked"));
}

/* In the transcript an app is a button: nothing is fetched at load. */
static void
test_an_app_in_the_transcript_is_closed_until_opened(void)
{
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new(
        CLAWT_STEP_STATUS, "oryx", "dm:oryx:op", "App: Diagram from draw",
        "create_view", NULL, FALSE);
    g_autoptr(HtmxElement) element = NULL;
    g_autofree gchar *html = NULL;

    clawt_turn_step_set_app(step, "a0123456789abcdef",
                            "http://127.0.0.1:8790/a/oryx/app/"
                            "a0123456789abcdef", "Flowchart");
    element = clawt_web_step_element(step);
    html = htmx_element_render(element);

    g_assert_nonnull(strstr(html, "class=\"turn-step app-card\""));
    g_assert_nonnull(strstr(html,
        "hx-get=\"/a/oryx/app/a0123456789abcdef/frame\""));
    g_assert_nonnull(strstr(html, "hx-preserve=\"true\""));
    g_assert_nonnull(strstr(html, "App: Diagram from draw"));
    g_assert_null(strstr(html, "<iframe"));
    g_assert_null(strstr(html, "srcdoc"));
}

/* An id that is not an app id never becomes part of a URL. */
static void
test_a_bad_app_id_is_not_a_link(void)
{
    g_autoptr(HtmxElement) card = clawt_web_app_card(
        "oryx", "../../settings", "App");
    g_autofree gchar *html = htmx_element_render(card);

    g_assert_null(strstr(html, "hx-get"));
    g_assert_null(strstr(html, "settings"));
}

static void
test_a_trimmed_result_is_a_warning(void)
{
    g_autofree gchar *notice = clawt_mcp_result_notice(
        "github", "list_issues", 2000000, 65536, "r0123456789abcdef");
    g_autoptr(ClawtTurnStep) step = clawt_turn_step_new(
        CLAWT_STEP_STATUS, "oryx", "dm:oryx:op", notice, NULL, NULL, FALSE);
    g_autoptr(ClawtTurnStep) plain = clawt_turn_step_new(
        CLAWT_STEP_STATUS, "oryx", "dm:oryx:op", "Reconnecting", NULL, NULL,
        FALSE);
    g_autoptr(HtmxElement) one = clawt_web_step_element(step);
    g_autoptr(HtmxElement) two = clawt_web_step_element(plain);
    g_autofree gchar *html = htmx_element_render(one);
    g_autofree gchar *other = htmx_element_render(two);

    g_assert_nonnull(strstr(html,
                            "class=\"turn-step warn trimmed-notice\""));
    g_assert_nonnull(strstr(html, "list_issues"));
    g_assert_null(strstr(other, "trimmed-notice"));
}

static void
test_the_connector_forms_show_their_values(void)
{
    g_autoptr(HtmxElement) budget = clawt_web_budget_form("gh", 4096,
                                                          "4 KB");
    g_autoptr(HtmxElement) apps_on = clawt_web_apps_form("gh", TRUE);
    g_autoptr(HtmxElement) apps_off = clawt_web_apps_form("gh", FALSE);
    g_autoptr(HtmxElement) fleet = clawt_web_fleet_budget(65536);
    g_autofree gchar *budget_html = htmx_element_render(budget);
    g_autofree gchar *on_html = htmx_element_render(apps_on);
    g_autofree gchar *off_html = htmx_element_render(apps_off);
    g_autofree gchar *fleet_html = htmx_element_render(fleet);

    g_assert_nonnull(strstr(budget_html,
                            "action=\"/settings/connectors/gh/budget\""));
    g_assert_nonnull(strstr(budget_html, "name=\"max_result_bytes\""));
    g_assert_nonnull(strstr(budget_html, "value=\"4096\""));
    g_assert_nonnull(strstr(budget_html, "4 KB"));
    g_assert_nonnull(strstr(on_html,
                            "action=\"/settings/connectors/gh/apps\""));
    g_assert_nonnull(strstr(on_html, "Show its apps in chat"));
    g_assert_nonnull(strstr(on_html, "checked"));
    g_assert_null(strstr(off_html, "checked"));
    g_assert_nonnull(strstr(fleet_html, "value=\"65536\""));
}

/* Every page carries the host bridge, and it trusts only an app frame. */
static void
test_the_app_host_bridge_checks_the_source(void)
{
    g_autoptr(HtmxBuilder) builder = htmx_builder_new();
    g_autofree gchar *html = NULL;

    open_document(builder, NULL, NULL);
    close_document(builder);
    html = htmx_builder_render(builder);

    g_assert_nonnull(strstr(html, "f[i].contentWindow===src"));
    g_assert_nonnull(strstr(html, "ui/notifications/tool-result"));
    g_assert_nonnull(strstr(html, "not supported by clawtilla"));

    /*
     * A tools/call goes to the frame's own route, from this page and
     * with this page's credentials -- never to an address the view named.
     */
    g_assert_nonnull(strstr(html, "m.method==='tools/call'"));
    g_assert_nonnull(strstr(html, "getAttribute('data-app-call')"));
    g_assert_nonnull(strstr(html, "credentials:'same-origin'"));
}

/* An opened app knows its own call route, and only for a real id. */
static void
test_an_app_frame_names_its_call_route(void)
{
    g_autoptr(JsonObject) app = app_reply(TRUE);
    g_autoptr(HtmxElement) frame = clawt_web_app_frame(app);
    g_autofree gchar *html = htmx_element_render(frame);
    g_autofree gchar *route = attribute_value(html, "data-app-call");

    g_assert_cmpstr(route, ==, "/a/oryx/app/a0123456789abcdef/call");

    json_object_set_string_member(app, "id", "../../settings");
    {
        g_autoptr(HtmxElement) forged = clawt_web_app_frame(app);
        g_autofree gchar *forged_html = htmx_element_render(forged);

        g_assert_null(strstr(forged_html, "data-app-call"));
    }
}

/*
 * The bridge's half that runs in C: what a view's message becomes.  Only
 * tools/call with a name and an id is a call; the two the extension
 * defines and clawtilla decided against are refused by name.
 */
static void
test_the_bridge_reads_a_views_message(void)
{
    g_autofree gchar *tool = NULL;
    g_autofree gchar *refusal = NULL;
    g_autoptr(JsonNode) arguments = NULL;
    g_autoptr(JsonNode) id = NULL;
    struct {
        const gchar *body;
        const gchar *says;
    } refused[] = {
        { "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ui/message\","
          "\"params\":{}}", "write into the conversation" },
        { "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ui/open-link\","
          "\"params\":{\"url\":\"https://evil.example\"}}", "links" },
        { "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/read\"}",
          "not supported" },
        { "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
          "\"params\":{\"name\":\"draw\"}}", "needs an id" },
        { "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
          "\"params\":{}}", "names a tool" },
        { "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
          "\"params\":{\"name\":\"draw\",\"arguments\":[1]}}",
          "arguments are an object" },
        { "not json at all", "not a JSON-RPC" },
        { NULL, NULL }
    };
    guint i;

    g_assert_true(clawt_web_app_bridge_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":\"c1\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"save_view\",\"arguments\":{\"x\":1}}}",
        &tool, &arguments, &id, &refusal));
    g_assert_cmpstr(tool, ==, "save_view");
    g_assert_null(refusal);
    g_assert_cmpstr(json_node_get_string(id), ==, "c1");
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(arguments),
                                               "x"), ==, 1);

    for (i = 0; refused[i].body != NULL; i++) {
        g_autofree gchar *t = NULL;
        g_autofree gchar *r = NULL;
        g_autoptr(JsonNode) a = NULL;
        g_autoptr(JsonNode) n = NULL;

        g_assert_false(clawt_web_app_bridge_parse(refused[i].body, &t, &a, &n,
                                                  &r));
        g_assert_null(t);
        g_assert_nonnull(r);
        g_assert_nonnull(strstr(r, "\"error\""));
        g_assert_nonnull(strstr(r, refused[i].says));
    }

    /* The answer carries the view's own id back, result or error. */
    {
        g_autoptr(JsonObject) result = json_object_new();
        g_autofree gchar *ok = NULL;
        g_autofree gchar *failed = NULL;

        json_object_set_boolean_member(result, "isError", FALSE);
        ok = clawt_web_app_bridge_response(id, result, NULL);
        failed = clawt_web_app_bridge_response(id, NULL, "not visible");

        g_assert_nonnull(strstr(ok, "\"id\":\"c1\""));
        g_assert_nonnull(strstr(ok, "\"result\":{\"isError\":false}"));
        g_assert_nonnull(strstr(failed, "\"id\":\"c1\""));
        g_assert_nonnull(strstr(failed, "not visible"));
        g_assert_null(strstr(failed, "\"result\""));
    }
}

/*
 * An agent row's menu: a `<details class="row-menu">` that always exists
 * now that Mute is in it.  "Mark as read" carries the count -- the words
 * every client uses -- and posts to the agent's /read route; with
 * nothing unread it is still drawn, disabled, as GTK and lrg grey it.
 * The mute item flips: "Mute notifications" posting to /mute, "Unmute"
 * to /unmute.  The agent id is escaped in every URL.  "Mark all as read"
 * appears only with a total.  Regressing it offers to mark nothing, a
 * menu the right-click handler cannot find, or a mute nobody can undo.
 */
static void
test_the_row_menu_offers_read_and_mute(void)
{
    g_autoptr(HtmxElement) menu = clawt_web_agent_row_menu("alpha", 3,
                                                           FALSE);
    g_autoptr(HtmxElement) none = clawt_web_agent_row_menu("alpha", 0,
                                                           TRUE);
    g_autoptr(HtmxElement) hostile = clawt_web_agent_row_menu("a b", 1,
                                                              FALSE);
    g_autoptr(HtmxElement) all = clawt_web_mark_all_read_button(5);
    g_autoptr(HtmxElement) no_all = clawt_web_mark_all_read_button(0);
    g_autofree gchar *html = NULL;
    g_autofree gchar *none_html = NULL;
    g_autofree gchar *escaped = NULL;
    g_autofree gchar *all_html = NULL;

    g_assert_nonnull(menu);
    html = htmx_element_render(menu);
    g_assert_nonnull(strstr(html, "<details"));
    g_assert_nonnull(strstr(html, "row-menu"));
    g_assert_nonnull(strstr(html, "Mark as read (3)"));
    g_assert_nonnull(strstr(html, "/a/alpha/read"));
    g_assert_nonnull(strstr(html, "Mute notifications"));
    g_assert_nonnull(strstr(html, "/a/alpha/mute"));
    g_assert_null(strstr(html, "disabled"));

    /* nothing unread: the item stays, disabled; a muted agent unmutes */
    g_assert_nonnull(none);
    none_html = htmx_element_render(none);
    g_assert_nonnull(strstr(none_html, "Mark as read"));
    g_assert_nonnull(strstr(none_html, "disabled"));
    g_assert_nonnull(strstr(none_html, "Unmute"));
    g_assert_nonnull(strstr(none_html, "/a/alpha/unmute"));
    g_assert_null(strstr(none_html, "Mute notifications"));

    g_assert_null(clawt_web_agent_row_menu(NULL, 1, FALSE));

    g_assert_nonnull(hostile);
    escaped = htmx_element_render(hostile);
    g_assert_nonnull(strstr(escaped, "/a/a%20b/read"));
    g_assert_nonnull(strstr(escaped, "/a/a%20b/mute"));

    g_assert_nonnull(all);
    all_html = htmx_element_render(all);
    g_assert_nonnull(strstr(all_html, "Mark all as read"));
    g_assert_nonnull(strstr(all_html, "/read-all"));
    g_assert_null(no_all);
}

/*
 * The Notifications page's four sections from a notice.list reply, in
 * order -- Chats, Decisions waiting, Grok jobs needing attention, Alerts
 * -- each row followed by its row menu with the verbs it offers, an empty
 * section left out, and a narrowed page showing one agent's rows only.
 * Regressing it is a page that drops a source the bell counts, or a row
 * whose menu posts to nothing.
 */
static void
test_the_notifications_page_draws_the_sections(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(HtmxElement) body = NULL;
    g_autoptr(HtmxElement) narrowed = NULL;
    g_autoptr(HtmxElement) empty = NULL;
    g_autofree gchar *html = NULL;
    g_autofree gchar *narrowed_html = NULL;
    JsonObject *reply;
    guint shown = 0;
    const gchar *chats;
    const gchar *decisions;
    const gchar *grok;
    const gchar *alerts;

    g_assert_true(json_parser_load_from_data(
        parser,
        "{\"chats\":[{\"room\":\"dm:scout:user\",\"agent\":\"scout\","
        "\"unread\":2}],"
        "\"decisions\":[{\"id\":\"d1\",\"agent\":\"zed\","
        "\"question\":\"Ship it?\"}],"
        "\"grok\":[{\"id\":\"j1\",\"agent\":\"scout\",\"bot\":\"chief\","
        "\"state\":\"failed\",\"remove_question\":\"Remove it?\"}],"
        "\"notices\":[{\"id\":7,\"agent\":\"zed\",\"tier\":\"error\","
        "\"title\":\"zed timed out\",\"source\":\"turn.timed_out\","
        "\"created_at\":100,\"read\":false,\"muted\":false}]}",
        -1, NULL));
    reply = json_node_get_object(json_parser_get_root(parser));

    body = clawt_web_notices_sections(reply, NULL, 200 * G_USEC_PER_SEC,
                                      &shown);
    g_assert_cmpuint(shown, ==, 4);
    html = htmx_element_render(body);

    chats = strstr(html, "Chats (1)");
    decisions = strstr(html, "Decisions waiting (1)");
    grok = strstr(html, "Grok jobs needing attention (1)");
    alerts = strstr(html, "Alerts (1)");
    g_assert_nonnull(chats);
    g_assert_nonnull(decisions);
    g_assert_nonnull(grok);
    g_assert_nonnull(alerts);
    g_assert_true(chats < decisions && decisions < grok && grok < alerts);

    /* the rows' menus, and what each posts to */
    g_assert_nonnull(strstr(html, "notice-row"));
    g_assert_nonnull(strstr(html, "Mark as read (2)"));
    g_assert_nonnull(strstr(html, "/alerts/chat-read?room=dm%3Ascout%3Auser"));
    g_assert_nonnull(strstr(html, "/alerts/mute?agent=scout"));
    g_assert_nonnull(strstr(html, "/decisions"));
    g_assert_nonnull(strstr(html, "/grok/jobs/j1/remove"));
    g_assert_nonnull(strstr(html, "Remove it?"));
    g_assert_nonnull(strstr(html, "zed timed out"));
    g_assert_nonnull(strstr(html, "/alerts/read?id=7"));
    g_assert_nonnull(strstr(html, "/alerts/dismiss?id=7"));
    g_assert_nonnull(strstr(html, "alert-unread"));

    /* narrowed to zed: the decision and the alert, nothing of scout's */
    narrowed = clawt_web_notices_sections(reply, "zed", 0, &shown);
    g_assert_cmpuint(shown, ==, 2);
    narrowed_html = htmx_element_render(narrowed);
    g_assert_null(strstr(narrowed_html, "Chats"));
    g_assert_null(strstr(narrowed_html, "Grok jobs"));
    g_assert_nonnull(strstr(narrowed_html, "Ship it?"));

    /* no reply at all: nothing drawn, and nothing claimed */
    empty = clawt_web_notices_sections(NULL, NULL, 0, &shown);
    g_assert_cmpuint(shown, ==, 0);
}

/*
 * The linked-fleet chips live in one cookie: a term switches on and off,
 * an owner with a space survives, and what a browser sends back that is
 * not UTF-8 or absurdly long is dropped rather than drawn.
 */
static void
test_fed_filter_cookie(void)
{
    g_auto(GStrv) none = clawt_web_fed_filter_parse(NULL);
    g_auto(GStrv) empty = clawt_web_fed_filter_parse(g_strdup(""));
    g_autofree gchar *one = clawt_web_fed_filter_toggle(NULL, "tier:trusted");
    g_auto(GStrv) parsed = NULL;
    g_autofree gchar *two = NULL;
    g_autofree gchar *back = NULL;
    g_auto(GStrv) again = NULL;
    g_auto(GStrv) bad = NULL;
    g_autofree gchar *long_term = g_strnfill(300, 'x');

    g_assert_null(none);
    g_assert_null(empty);
    g_assert_cmpstr(one, ==, "tier:trusted");

    parsed = clawt_web_fed_filter_parse(g_strdup(one));
    two = clawt_web_fed_filter_toggle(parsed, "owner:Priya Shah");
    again = clawt_web_fed_filter_parse(g_strdup(two));
    g_assert_cmpuint(g_strv_length(again), ==, 2);
    g_assert_cmpstr(again[1], ==, "owner:Priya Shah");

    back = clawt_web_fed_filter_toggle(again, "tier:trusted");
    g_assert_cmpstr(back, ==, "owner:Priya Shah");

    bad = clawt_web_fed_filter_parse(g_strdup_printf("tag:gpu\t\xff\xfe\t%s\t\tonline",
                                                     long_term));
    g_assert_cmpuint(g_strv_length(bad), ==, 2);
    g_assert_cmpstr(bad[0], ==, "tag:gpu");
    g_assert_cmpstr(bad[1], ==, "online");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web/navigation-result", test_navigation_result);
    g_test_add_func("/web/row-menu-read-and-mute",
                    test_the_row_menu_offers_read_and_mute);
    g_test_add_func("/web/notifications-sections",
                    test_the_notifications_page_draws_the_sections);
    g_test_add_func("/web/the-failover-badge", test_the_failover_badge);
    g_test_add_func("/web/the-fallback-editor", test_the_fallback_editor);
    g_test_add_func("/web/the-fallback-editor-will-not-save-over-a-bad-list",
                    test_the_fallback_editor_will_not_save_over_a_bad_list);

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
    g_test_add_func("/web/picture-viewer-is-one-element",
                    test_the_picture_viewer_is_one_element_outside_the_transcript);
    g_test_add_func("/web/picture-viewer-can-be-dismissed",
                    test_the_picture_viewer_can_be_dismissed);
    g_test_add_func("/web/transcript-face-is-larger",
                    test_the_transcript_face_is_larger_and_only_zooms_with_a_picture);
    g_test_add_func("/web/face/avatar-draws-a-face",
                    test_the_avatar_draws_a_face);
    g_test_add_func("/web/face/no-hostile-colour",
                    test_a_face_never_carries_a_hostile_colour);
    g_test_add_func("/web/face/stylesheet-matches-library",
                    test_the_stylesheet_moves_faces_like_the_library);
    g_test_add_func("/web/face/still-is-explicit",
                    test_still_faces_are_an_explicit_choice);
    g_test_add_func("/web/face/picker-is-a-form",
                    test_the_face_picker_is_a_form);
    g_test_add_func("/web/face/page-script", test_the_page_script_moves_faces);
    g_test_add_func("/web/face/form-writes-only-changes",
                    test_the_face_form_writes_only_changes);
    g_test_add_func("/web/no-cookie-shows-descriptions",
                    test_a_browser_with_no_cookie_shows_descriptions);
    g_test_add_func("/web/only-an-explicit-zero-hides-descriptions",
                    test_only_an_explicit_zero_hides_descriptions);
    g_test_add_func("/web/sidebar-description-clips",
                    test_the_sidebar_description_clips);

    g_test_add_func("/web/the-narrow-sidebar-is-a-drawer",
                    test_the_narrow_sidebar_is_a_drawer);
    g_test_add_func("/web/the-palette-is-defined-outside-a-media-query",
                    test_the_palette_is_defined_outside_a_media_query);
    g_test_add_func("/web/style/every-var-is-defined", test_every_var_is_defined);
    g_test_add_func("/web/style/sidebar-head-wraps",
                    test_the_sidebar_head_wraps);
    g_test_add_func("/web/the-composer-does-not-clip-its-completions",
                    test_the_composer_does_not_clip_its_completions);
    g_test_add_func("/web/the-completion-column-is-the-entry-column",
                    test_the_completion_column_is_the_entry_column);
    g_test_add_func("/web/dark-is-reachable-by-preference-and-by-choice",
                    test_dark_is_reachable_by_preference_and_by_choice);
    g_test_add_func("/web/composer-stands-on-the-message-column",
                    test_the_composer_stands_on_the_message_column);
    g_test_add_func("/web/inline-decision-offers-its-options",
                    test_an_open_inline_decision_offers_its_options);
    g_test_add_func("/web/inline-decision-settled-says-how",
                    test_a_settled_inline_decision_says_how);
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

    g_test_add_func("/web/cross-site-write",
                    test_a_cross_site_write_is_refused);
    g_test_add_func("/web/cross-site-origin",
                    test_the_origin_is_compared_with_the_host);
    g_test_add_func("/web/cross-site-reads",
                    test_reads_and_non_browsers_are_left_alone);
    g_test_add_func("/web/cross-site-no-host",
                    test_a_write_with_no_host_is_refused);

    g_test_add_func("/web-render/a-json-boolean-reads-as-on",
                    test_a_json_boolean_reads_as_on);
    g_test_add_func("/web-render/a-stringified-boolean-reads-as-on",
                    test_a_stringified_boolean_still_reads_as_on);
    g_test_add_func("/web-render/an-enabled-routine-renders-ticked",
                    test_an_enabled_routine_renders_a_ticked_switch);
    g_test_add_func("/web-render/manifest-fields",
                    test_the_manifest_names_what_an_install_needs);
    g_test_add_func("/web-render/manifest-linked",
                    test_every_page_links_the_manifest);
    g_test_add_func("/web-render/manifest-served",
                    test_the_manifest_is_served_as_a_manifest);
    g_test_add_func("/web-render/fold-closed-and-escaped",
                    test_a_folded_request_renders_closed_and_escaped);
    g_test_add_func("/web-render/exchange",
                    test_the_exchange_lists_requests_and_answers);
    g_test_add_func("/web-render/app-sandboxed",
                    test_an_opened_app_is_sandboxed);
    g_test_add_func("/web-render/app-blocked",
                    test_a_blocked_app_says_why_and_draws_no_frame);
    g_test_add_func("/web-render/app-closed-until-opened",
                    test_an_app_in_the_transcript_is_closed_until_opened);
    g_test_add_func("/web-render/app-bad-id",
                    test_a_bad_app_id_is_not_a_link);
    g_test_add_func("/web-render/trimmed-notice",
                    test_a_trimmed_result_is_a_warning);
    g_test_add_func("/web-render/connector-budget-and-apps-forms",
                    test_the_connector_forms_show_their_values);
    g_test_add_func("/web-render/effort-field-offers-the-default",
                    test_effort_field_offers_the_default);
    g_test_add_func("/web-render/schedule-and-weekday-fields",
                    test_schedule_and_weekday_fields);
    g_test_add_func("/web-render/resolution-field-keeps-a-custom-size",
                    test_resolution_field_keeps_a_custom_size);
    g_test_add_func("/web-render/review-by-field-lists-agents",
                    test_review_by_field_lists_agents);
    g_test_add_func("/web-render/app-host-bridge",
                    test_the_app_host_bridge_checks_the_source);
    g_test_add_func("/web-render/fed-filter-cookie", test_fed_filter_cookie);
    g_test_add_func("/web-render/app-call-route",
                    test_an_app_frame_names_its_call_route);
    g_test_add_func("/web-render/app-bridge-message",
                    test_the_bridge_reads_a_views_message);

    return g_test_run();
}
