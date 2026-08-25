/*
 * test-appearance.c - How the client looks, on this machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The interesting case throughout is *unset*, because "follow the
 * desktop" and "use the font the desktop currently uses" look identical
 * on screen and behave differently for ever afterwards: one keeps
 * following, the other has quietly frozen. So most of what is asserted
 * here is that a default emits nothing at all.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * Whether Pango would accept this markup.
 *
 * Checked with GMarkup rather than with Pango, because libclawt must not
 * link GTK or pango -- and Pango markup is XML once it has a single root
 * element, so a parse here catches exactly what Pango would reject: an
 * unbalanced tag, a stray "<", an unquoted attribute.  A GtkLabel handed
 * markup it cannot parse renders *nothing*, so this is the difference
 * between a wrong font and a message that silently disappears.
 */
static gboolean
markup_is_well_formed(const gchar *markup)
{
    static const GMarkupParser parser = { NULL, NULL, NULL, NULL, NULL };
    g_autoptr(GMarkupParseContext) context = NULL;
    g_autofree gchar *wrapped = g_strdup_printf("<x>%s</x>", markup);
    gboolean ok;

    context = g_markup_parse_context_new(&parser, 0, NULL, NULL);

    ok = g_markup_parse_context_parse(context, wrapped, -1, NULL) &&
         g_markup_parse_context_end_parse(context, NULL);

    return ok;
}

/*
 * A fresh appearance must produce an empty stylesheet.
 *
 * Not a tidiness point.  A rule naming the current default would freeze
 * it, so a person who later changed their desktop font would find this
 * one app ignoring it -- and would have no reason to look here, having
 * never opened the dialog.
 */
static void
test_defaults_defer_to_the_desktop(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *css = clawt_appearance_to_css(appearance);

    g_assert_cmpint(clawt_appearance_get_theme(appearance), ==,
                    CLAWT_THEME_SYSTEM);
    g_assert_null(clawt_appearance_get_font(appearance));
    g_assert_null(clawt_appearance_get_monospace_font(appearance));
    g_assert_cmpfloat(clawt_appearance_get_font_size(appearance), ==, 0.0);
    g_assert_cmpfloat(clawt_appearance_get_monospace_size(appearance), ==,
                      0.0);

    g_assert_cmpstr(css, ==, "");
}

static void
test_a_font_reaches_the_stylesheet(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *css = NULL;

    clawt_appearance_set_font(appearance, "Cantarell");
    clawt_appearance_set_font_size(appearance, 12);
    clawt_appearance_set_monospace_font(appearance, "JetBrains Mono");
    clawt_appearance_set_monospace_size(appearance, 10.5);

    css = clawt_appearance_to_css(appearance);

    g_assert_nonnull(strstr(css, "\"Cantarell\""));
    g_assert_nonnull(strstr(css, "12pt"));
    g_assert_nonnull(strstr(css, "\"JetBrains Mono\""));
    g_assert_nonnull(strstr(css, "10.5pt"));

    /* The console renders through the monospace class. */
    g_assert_nonnull(strstr(css, "monospace"));
}

/*
 * Half-set is the ordinary state -- a person who wants a bigger font at
 * the desktop's own family, or the reverse -- and each half has to be
 * independent of the other.
 */
static void
test_each_half_is_independent(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *size_only = NULL;
    g_autofree gchar *family_only = NULL;

    clawt_appearance_set_font_size(appearance, 14);
    size_only = clawt_appearance_to_css(appearance);

    g_assert_nonnull(strstr(size_only, "14pt"));
    g_assert_null(strstr(size_only, "font-family"));

    clawt_appearance_set_font_size(appearance, 0);
    clawt_appearance_set_font(appearance, "Inter");
    family_only = clawt_appearance_to_css(appearance);

    g_assert_nonnull(strstr(family_only, "font-family"));
    g_assert_null(strstr(family_only, "pt;"));
}

/*
 * A family is arbitrary text from a font chooser.  A stray quote would
 * end the CSS string early and take the rest of the stylesheet with it,
 * so it is dropped rather than escaped: no real font has one, and losing
 * a rule beats emitting a sheet GTK will refuse.
 */
static void
test_a_quote_cannot_escape_the_stylesheet(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *css = NULL;
    const gchar *p;
    guint quotes = 0;
    guint open_braces = 0;
    guint close_braces = 0;
    guint semicolons = 0;

    clawt_appearance_set_font(appearance,
                              "Evil\"; } * { font-size: 99pt; } x {\"");
    css = clawt_appearance_to_css(appearance);

    for (p = css; *p != '\0'; p++) {
        switch (*p) {
        case '"': quotes++; break;
        case '{': open_braces++; break;
        case '}': close_braces++; break;
        case ';': semicolons++; break;
        default: break;
        }
    }

    /*
     * Structure, not content.  "99pt" survives as text inside the quoted
     * family and is harmless there -- it names a font nobody has.  What
     * must not survive is the punctuation that would turn it into a
     * second rule: one block, one declaration, one pair of quotes.
     */
    g_assert_cmpuint(quotes, ==, 2);
    g_assert_cmpuint(open_braces, ==, 1);
    g_assert_cmpuint(close_braces, ==, 1);
    g_assert_cmpuint(semicolons, ==, 1);
}

/*
 * Reached from a file somebody may have edited by hand, which this one
 * invites -- it is five lines of YAML.  A 2 or a 2000 there would make
 * the settings dialog unreadable and therefore unable to fix itself.
 */
static void
test_an_absurd_size_is_clamped(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    clawt_appearance_set_font_size(appearance, 2000);
    g_assert_cmpfloat(clawt_appearance_get_font_size(appearance), <=, 48.0);

    clawt_appearance_set_font_size(appearance, 2);
    g_assert_cmpfloat(clawt_appearance_get_font_size(appearance), >=, 6.0);

    /* But 0 is "unset", not "very small", and survives. */
    clawt_appearance_set_font_size(appearance, 0);
    g_assert_cmpfloat(clawt_appearance_get_font_size(appearance), ==, 0.0);

    clawt_appearance_set_monospace_size(appearance, -5);
    g_assert_cmpfloat(clawt_appearance_get_monospace_size(appearance), ==,
                      0.0);
}

/*
 * Clearing a font must return to *unset* rather than to an empty string:
 * the second would put `font-family: ;` in the sheet, which is invalid,
 * and GTK drops the whole block it appears in -- so clearing the family
 * would silently lose the size beside it.
 */
static void
test_clearing_a_font_is_unset_not_empty(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *css = NULL;

    clawt_appearance_set_font(appearance, "Inter");
    clawt_appearance_set_font(appearance, "");
    g_assert_null(clawt_appearance_get_font(appearance));

    clawt_appearance_set_font(appearance, "   ");
    g_assert_null(clawt_appearance_get_font(appearance));

    clawt_appearance_set_font(appearance, NULL);
    g_assert_null(clawt_appearance_get_font(appearance));

    clawt_appearance_set_font_size(appearance, 13);
    css = clawt_appearance_to_css(appearance);

    g_assert_null(strstr(css, "font-family"));
    g_assert_nonnull(strstr(css, "13pt"));
}

static void
test_settings_survive_the_round_trip(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *text = NULL;
    g_autoptr(ClawtAppearance) back = NULL;
    g_autoptr(GError) error = NULL;

    clawt_appearance_set_theme(appearance, CLAWT_THEME_DARK);
    clawt_appearance_set_font(appearance, "DejaVu Sans");
    clawt_appearance_set_font_size(appearance, 11.5);
    clawt_appearance_set_monospace_font(appearance, "Fira Code");
    clawt_appearance_set_monospace_size(appearance, 9);

    text = clawt_appearance_to_data(appearance);
    back = clawt_appearance_parse(text, &error);

    g_assert_no_error(error);
    g_assert_cmpint(clawt_appearance_get_theme(back), ==, CLAWT_THEME_DARK);
    g_assert_cmpstr(clawt_appearance_get_font(back), ==, "DejaVu Sans");
    g_assert_cmpfloat(clawt_appearance_get_font_size(back), ==, 11.5);
    g_assert_cmpstr(clawt_appearance_get_monospace_font(back), ==,
                    "Fira Code");
    g_assert_cmpfloat(clawt_appearance_get_monospace_size(back), ==, 9.0);
}

/*
 * The defaults have to round-trip too.  That is the file a person gets
 * the first time they change the theme and nothing else, and a written
 * empty font that reloaded as the string "''" would name a font nobody
 * has.
 */
static void
test_the_defaults_round_trip(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *text = clawt_appearance_to_data(appearance);
    g_autoptr(ClawtAppearance) back = clawt_appearance_parse(text, NULL);
    g_autofree gchar *css = NULL;

    g_assert_nonnull(back);
    g_assert_null(clawt_appearance_get_font(back));
    g_assert_null(clawt_appearance_get_monospace_font(back));
    g_assert_cmpint(clawt_appearance_get_theme(back), ==,
                    CLAWT_THEME_SYSTEM);

    css = clawt_appearance_to_css(back);
    g_assert_cmpstr(css, ==, "");
}

/*
 * A named palette reaches the stylesheet as libadwaita's own colour
 * names, so the whole widget tree picks it up without a rule per widget.
 *
 * This is the seam the theme enum did not have: every colour came from
 * libadwaita's light/dark scheme and there was nowhere for a palette to
 * be expressed at all.
 */
static void
test_a_palette_reaches_the_stylesheet(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *css = NULL;

    clawt_appearance_set_theme(appearance, CLAWT_THEME_CATPPUCCIN_MOCHA);
    css = clawt_appearance_to_css(appearance);

    g_assert_nonnull(strstr(css, "@define-color window_bg_color #1e1e2e;"));
    g_assert_nonnull(strstr(css, "@define-color window_fg_color #cdd6f4;"));
    g_assert_nonnull(strstr(css, "@define-color accent_bg_color #89b4fa;"));
    g_assert_nonnull(strstr(css, "@define-color destructive_bg_color #f38ba8;"));
}

/*
 * And the three schemes that are not palettes emit no colour at all.
 *
 * Paired with the test above deliberately: an assertion that something
 * did *not* happen passes trivially in a build where it never happens,
 * so it is only worth having next to one that proves it can.
 */
static void
test_a_scheme_that_is_not_a_palette_emits_no_colour(void)
{
    static const ClawtTheme plain[] = {
        CLAWT_THEME_SYSTEM, CLAWT_THEME_LIGHT, CLAWT_THEME_DARK
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(plain); i++) {
        g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
        g_autofree gchar *css = NULL;

        clawt_appearance_set_theme(appearance, plain[i]);
        clawt_appearance_set_font(appearance, "Inter");
        css = clawt_appearance_to_css(appearance);

        g_assert_null(strstr(css, "@define-color"));
        g_assert_nonnull(strstr(css, "font-family"));
    }
}

/*
 * A palette forces the scheme it was drawn for.  Mocha over a light
 * libadwaita is not a darker Mocha, it is libadwaita's light colours
 * fighting the palette's dark ones on every widget the palette does not
 * name.
 */
static void
test_a_palette_names_the_scheme_it_needs(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    g_assert_false(clawt_appearance_theme_is_dark(CLAWT_THEME_LIGHT));
    g_assert_true(clawt_appearance_theme_is_dark(CLAWT_THEME_DARK));
    g_assert_true(clawt_appearance_theme_is_dark(CLAWT_THEME_CATPPUCCIN_MOCHA));

    /* And a palette is distinguishable from a plain scheme. */
    g_assert_false(clawt_appearance_theme_has_palette(CLAWT_THEME_SYSTEM));
    g_assert_false(clawt_appearance_theme_has_palette(CLAWT_THEME_DARK));
    g_assert_true(clawt_appearance_theme_has_palette(
                      CLAWT_THEME_CATPPUCCIN_MOCHA));

    (void)appearance;
}

/* The nick is what is on disk, so it has to survive the round trip. */
static void
test_a_palette_survives_the_round_trip(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *text = NULL;
    g_autoptr(ClawtAppearance) back = NULL;

    clawt_appearance_set_theme(appearance, CLAWT_THEME_CATPPUCCIN_MOCHA);
    text = clawt_appearance_to_data(appearance);

    g_assert_nonnull(strstr(text, "catppuccin-mocha"));

    back = clawt_appearance_parse(text, NULL);
    g_assert_nonnull(back);
    g_assert_cmpint(clawt_appearance_get_theme(back), ==,
                    CLAWT_THEME_CATPPUCCIN_MOCHA);
}

/*
 * A newer build may write a theme this one has never heard of, and
 * refusing the file would take the fonts down with it -- the same
 * forward-compatibility rule shadow agents exist for.
 */
static void
test_an_unknown_theme_falls_back(void)
{
    g_autoptr(ClawtAppearance) appearance =
        clawt_appearance_parse("theme: 'solarized-midnight'\n"
                               "font: 'Inter'\n", NULL);

    g_assert_nonnull(appearance);
    g_assert_cmpint(clawt_appearance_get_theme(appearance), ==,
                    CLAWT_THEME_SYSTEM);

    /* And the rest of the file still applied. */
    g_assert_cmpstr(clawt_appearance_get_font(appearance), ==, "Inter");
}

static void
test_an_empty_file_is_the_defaults(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtAppearance) empty = clawt_appearance_parse("", &error);
    g_autoptr(ClawtAppearance) comments =
        clawt_appearance_parse("# nothing\n", NULL);

    g_assert_no_error(error);
    g_assert_nonnull(empty);
    g_assert_cmpint(clawt_appearance_get_theme(empty), ==,
                    CLAWT_THEME_SYSTEM);

    g_assert_nonnull(comments);
    g_assert_null(clawt_appearance_get_font(comments));
}

static void
test_a_missing_file_is_not_an_error(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-appear-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "absent.yaml", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtAppearance) appearance =
        clawt_appearance_load(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(appearance);
    g_assert_cmpint(clawt_appearance_get_theme(appearance), ==,
                    CLAWT_THEME_SYSTEM);

    g_rmdir(dir);
}

static void
test_saving_and_loading_a_file(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-appear-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "sub", "look.yaml", NULL);
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autoptr(ClawtAppearance) back = NULL;
    g_autoptr(GError) error = NULL;

    clawt_appearance_set_theme(appearance, CLAWT_THEME_LIGHT);
    clawt_appearance_set_monospace_font(appearance, "Iosevka");

    g_assert_true(clawt_appearance_save(appearance, path, &error));
    g_assert_no_error(error);

    back = clawt_appearance_load(path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(clawt_appearance_get_theme(back), ==, CLAWT_THEME_LIGHT);
    g_assert_cmpstr(clawt_appearance_get_monospace_font(back), ==, "Iosevka");

    g_unlink(path);
}

/*
 * Pango's <tt> is fontconfig's generic monospace alias and nothing in GTK
 * CSS redirects it, so a chosen code font reaches a chat message only if
 * the renderer names the family.  Without this, the setting worked in the
 * exec console and silently did nothing where people actually read code.
 */
static void
test_markdown_uses_the_chosen_code_font(void)
{
    g_autofree gchar *with = clawt_markdown_to_pango_full(
        "here is `code` inline", "Fira Code");
    g_autofree gchar *without =
        clawt_markdown_to_pango_full("here is `code` inline", NULL);
    g_autofree gchar *fenced =
        clawt_markdown_to_pango_full("```\\nls -la\\n```\\n", "Fira Code");

    g_assert_nonnull(strstr(with, "Fira Code"));
    g_assert_null(strstr(with, "<tt>"));

    /* Unset still uses the generic alias, which is the right default. */
    g_assert_nonnull(strstr(without, "<tt>"));
    g_assert_null(strstr(without, "font_family"));

    g_assert_nonnull(strstr(fenced, "Fira Code"));

    /* And all three are still markup Pango will accept. */
    g_assert_true(markup_is_well_formed(with));
    g_assert_true(markup_is_well_formed(without));
    g_assert_true(markup_is_well_formed(fenced));
}

/*
 * The family is the one piece of that markup which is not a literal, so
 * it is escaped -- the rule being that nothing reaches a markup parser
 * unescaped, whoever wrote it.
 */
static void
test_a_font_name_cannot_break_the_markup(void)
{
    g_autofree gchar *markup = clawt_markdown_to_pango_full(
        "`x`", "Evil\"><span foreground=\"red\">");

    g_assert_true(markup_is_well_formed(markup));
    g_assert_null(strstr(markup, "foreground=\"red\""));
}

/*
 * Every scheme the library declares is offerable by a client.
 *
 * Both clients build their control by walking this list, so a palette
 * added to clawt-appearance.c appears in each without either being
 * touched. That is the point: the GTK combo and the web select each used
 * to name the schemes themselves, and when Catppuccin Mocha was added
 * only one of the two grew a fourth entry. Nothing caught it -- a colour
 * scheme sends no IPC frame and is no slash command, so `make parity`
 * looks straight past it.
 */
static void
test_every_theme_can_be_offered(void)
{
    guint n = clawt_appearance_theme_count();
    guint i;

    g_assert_cmpuint(n, >=, 4);

    for (i = 0; i < n; i++) {
        ClawtTheme theme = clawt_appearance_theme_nth(i);
        const gchar *nick = clawt_appearance_theme_nick(theme);
        const gchar *label = clawt_appearance_theme_label(theme);

        g_assert_nonnull(nick);
        g_assert_cmpstr(nick, !=, "");
        g_assert_nonnull(label);
        g_assert_cmpstr(label, !=, "");

        /* The nick is what a cookie and a config file carry. */
        g_assert_cmpint(clawt_appearance_theme_from_nick(nick), ==, theme);
    }
}

/*
 * A nick nobody knows is the desktop's own scheme, not an error.
 *
 * The web client stores what a form posted, so this is also what keeps
 * an arbitrary string out of the cookie that ends up on data-theme.
 */
static void
test_an_unknown_theme_is_system(void)
{
    g_assert_cmpint(clawt_appearance_theme_from_nick("nonesuch"), ==,
                    CLAWT_THEME_SYSTEM);
    g_assert_cmpint(clawt_appearance_theme_from_nick(""), ==,
                    CLAWT_THEME_SYSTEM);
    g_assert_cmpint(clawt_appearance_theme_from_nick(NULL), ==,
                    CLAWT_THEME_SYSTEM);
}



/*
 * Which of black or white is legible on a configured avatar colour.
 *
 * `agents.color` is a hex string somebody types into a YAML file and
 * both clients paint an avatar with it, so both have to decide what
 * colour the initials go in -- and white on a pale yellow is
 * unreadable.  One answer, or the two would differ for exactly the
 * colours near the boundary.
 */
static void
test_which_ink_is_legible(void)
{
    /* Dark backgrounds take white. */
    g_assert_cmpstr(clawt_color_ink("#000000"), ==, "#ffffff");
    g_assert_cmpstr(clawt_color_ink("#1f6c9f"), ==, "#ffffff");
    g_assert_cmpstr(clawt_color_ink("#9f2f2d"), ==, "#ffffff");

    /* Light ones take black -- including the pale yellow that started it. */
    g_assert_cmpstr(clawt_color_ink("#ffffff"), ==, "#000000");
    g_assert_cmpstr(clawt_color_ink("#fbf3db"), ==, "#000000");
    g_assert_cmpstr(clawt_color_ink("#89b4fa"), ==, "#000000");

    /*
     * Green weighs far more than blue in the sRGB luminance the WCAG
     * contrast formula uses -- 0.7152 against 0.0722 -- so pure green
     * and pure blue land on opposite sides despite both being one
     * saturated channel.  That is the whole reason this is a function
     * rather than a look at the first digit.
     */
    g_assert_cmpstr(clawt_color_ink("#00ff00"), ==, "#000000");
    g_assert_cmpstr(clawt_color_ink("#0000ff"), ==, "#ffffff");

    /* The three-digit form is the same colour as its six-digit twin. */
    g_assert_cmpstr(clawt_color_ink("#fff"), ==, clawt_color_ink("#ffffff"));
    g_assert_cmpstr(clawt_color_ink("#000"), ==, clawt_color_ink("#000000"));
    g_assert_cmpstr(clawt_color_ink("#89f"), ==, clawt_color_ink("#8899ff"));
}

/*
 * Anything that is not one of the two forms is refused.
 *
 * This is the *only* validation `agents.color` has, and the value is
 * spliced into a stylesheet -- so a refusal here is what stops a config
 * file closing a declaration and writing rules of its own.  The caller
 * falls back to the avatar's derived colour rather than painting
 * something.
 */
static void
test_a_colour_that_is_not_one_is_refused(void)
{
    static const gchar *const rejected[] = {
        "red",                    /* a name, not a colour we parse */
        "#12345",                 /* neither three digits nor six */
        "#1234567",
        "#12",
        "#",
        "",
        "ffffff",                 /* no hash */
        "#gggggg",                /* not hex */
        "#ff00ff;color:red",      /* the injection this exists to stop */
        "#fff}body{display:none",
        NULL
    };
    gsize i;

    for (i = 0; rejected[i] != NULL; i++)
        g_assert_null(clawt_color_ink(rejected[i]));

    g_assert_null(clawt_color_ink(NULL));
}


int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/appearance/every-theme-can-be-offered",
                    test_every_theme_can_be_offered);
    g_test_add_func("/appearance/unknown-theme-is-system",
                    test_an_unknown_theme_is_system);
    g_test_add_func("/appearance/defaults-defer",
                    test_defaults_defer_to_the_desktop);
    g_test_add_func("/appearance/css", test_a_font_reaches_the_stylesheet);
    g_test_add_func("/appearance/halves", test_each_half_is_independent);
    g_test_add_func("/appearance/css-injection",
                    test_a_quote_cannot_escape_the_stylesheet);
    g_test_add_func("/appearance/clamped", test_an_absurd_size_is_clamped);
    g_test_add_func("/appearance/clearing",
                    test_clearing_a_font_is_unset_not_empty);
    g_test_add_func("/appearance/round-trip",
                    test_settings_survive_the_round_trip);
    g_test_add_func("/appearance/defaults-round-trip",
                    test_the_defaults_round_trip);
    g_test_add_func("/appearance/palette-reaches-the-stylesheet",
                    test_a_palette_reaches_the_stylesheet);
    g_test_add_func("/appearance/no-palette-no-colour",
                    test_a_scheme_that_is_not_a_palette_emits_no_colour);
    g_test_add_func("/appearance/palette-names-its-scheme",
                    test_a_palette_names_the_scheme_it_needs);
    g_test_add_func("/appearance/palette-round-trip",
                    test_a_palette_survives_the_round_trip);
    g_test_add_func("/appearance/unknown-theme",
                    test_an_unknown_theme_falls_back);
    g_test_add_func("/appearance/avatar-ink", test_which_ink_is_legible);
    g_test_add_func("/appearance/avatar-ink-refuses",
                    test_a_colour_that_is_not_one_is_refused);
    g_test_add_func("/appearance/empty-file",
                    test_an_empty_file_is_the_defaults);
    g_test_add_func("/appearance/missing-file",
                    test_a_missing_file_is_not_an_error);
    g_test_add_func("/appearance/save-and-load",
                    test_saving_and_loading_a_file);
    g_test_add_func("/appearance/markdown-code-font",
                    test_markdown_uses_the_chosen_code_font);
    g_test_add_func("/appearance/markdown-font-escaping",
                    test_a_font_name_cannot_break_the_markup);

    return g_test_run();
}
