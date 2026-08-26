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
    g_assert_nonnull(strstr(css, ".msg-body{white-space:pre-wrap;"
                                 "word-wrap:break-word;"
                                 "margin-left:var(--chat-gutter)}"));
    g_assert_nonnull(strstr(css, ".attachments{display:flex;flex-wrap:wrap;"
                                 "gap:8px;margin-top:8px;"
                                 "margin-left:var(--chat-gutter)}"));
    g_assert_nonnull(composer);
    g_assert_nonnull(strstr(composer, "padding-left:var(--chat-gutter)"));
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
    g_test_add_func("/web/every-view-has-a-slug-and-a-title",
                    test_every_view_has_a_slug_and_a_title);
    g_test_add_func("/web/an-unknown-view-falls-back-to-chat",
                    test_an_unknown_view_falls_back_to_chat);

    g_test_add_func("/web/the-palette-is-defined-outside-a-media-query",
                    test_the_palette_is_defined_outside_a_media_query);
    g_test_add_func("/web/dark-is-reachable-by-preference-and-by-choice",
                    test_dark_is_reachable_by_preference_and_by_choice);
    g_test_add_func("/web/composer-stands-on-the-message-column",
                    test_the_composer_stands_on_the_message_column);
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

    g_test_add_func("/web/the-two-clients-stay-level",
                    test_the_two_clients_stay_level);

    return g_test_run();
}
