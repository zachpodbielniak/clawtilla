/*
 * test-lrgui-theme.c - libregnum accepts every theme clawtilla-lrg writes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * tests/test-lrg-models.c checks what the theme document *says*; this
 * checks that libregnum *loads* it.  libregnum's theme loader is strict
 * and atomic -- an unknown key, role or state name refuses the whole
 * document -- and the client then falls back to a preset with a warning,
 * which would look like "the palette does not work" rather than like a
 * bug.  So every combination the appearance settings can produce is
 * loaded here, and every one must also pass WCAG AA after the contrast
 * fix the client applies.
 *
 * Built only with LRG_BACKEND=1 (it links libregnum).
 */

#include "clawt-lrg.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>

static gchar *config_home = NULL;

static void
load_and_check(ClawtAppearance *appearance, gboolean system_dark,
               gdouble scale, const gchar *what)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(LrgTheme) theme = NULL;

    yaml = clawt_lrg_theme_yaml(appearance, system_dark, scale);
    theme = lrg_theme_new_from_data(yaml, -1, &error);

    if (theme == NULL)
        g_error("%s: libregnum refused the theme: %s\n%s", what,
                error->message, yaml);

    /* what the client does before drawing with it (lrg-app.c) */
    lrg_theme_fix_contrast(theme, 4.5f);
    g_assert_cmpuint(lrg_theme_count_contrast_failures(theme, 4.5f), ==, 0);

    g_assert_cmpint(lrg_theme_get_dark(theme), ==,
                    clawt_lrg_appearance_is_dark(appearance, system_dark));
}

static void
test_every_combination_loads(void)
{
    guint scheme;
    guint material;
    guint background;
    guint motion;
    guint loaded = 0;

    for (scheme = 0; scheme < clawt_appearance_scheme_count(); scheme++)
    for (material = 0; material < clawt_lrg_material_count(); material++)
    for (background = 0; background < clawt_lrg_background_count();
         background++)
    for (motion = 0; motion < clawt_lrg_motion_count(); motion++) {
        g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
        g_autofree gchar *what = NULL;

        clawt_appearance_set_scheme(appearance,
                                    clawt_appearance_scheme_nth_nick(scheme));
        clawt_appearance_set_lrg_material(appearance,
                                          clawt_lrg_material_nth(material));
        clawt_appearance_set_lrg_background(
            appearance, clawt_lrg_background_nth(background));
        clawt_appearance_set_lrg_motion(appearance,
                                        clawt_lrg_motion_nth(motion));

        what = g_strdup_printf("%s/%s/%s/%s",
                               clawt_appearance_scheme_nth_nick(scheme),
                               clawt_lrg_material_nick(
                                   clawt_lrg_material_nth(material)),
                               clawt_lrg_background_nick(
                                   clawt_lrg_background_nth(background)),
                               clawt_lrg_motion_nick(
                                   clawt_lrg_motion_nth(motion)));

        load_and_check(appearance, (motion % 2) == 0, 1.0, what);
        loaded++;
    }

    g_assert_cmpuint(loaded, >, 0);
}

static void
test_accent_scale_and_fonts_load(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    clawt_appearance_set_lrg_accent(appearance, "#ffee00");
    clawt_appearance_set_font_size(appearance, 18.0);
    load_and_check(appearance, TRUE, CLAWT_LRG_SCALE_MAX, "accent at max scale");
    load_and_check(appearance, FALSE, CLAWT_LRG_SCALE_MIN,
                   "accent at min scale, light");
}

static void
test_a_partial_disk_palette_falls_back(void)
{
    g_autofree gchar *dir = clawt_appearance_palette_dir();
    g_autofree gchar *path = g_build_filename(dir, "half.css", NULL);
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *yaml = NULL;

    /*
     * A palette that names three colours and leaves forty-eight out: GTK
     * draws libadwaita's value for each one missing, and so must this.
     * Hot pink windows are deliberate -- a window colour that came out
     * stock would mean the file was not read at all.
     */
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path,
        "/* clawtilla-palette: label=\"Half\" dark=1 */\n"
        "@define-color window_bg_color #3a0a2a;\n"
        "@define-color window_fg_color #ffe0f0;\n"
        "@define-color accent_bg_color #ff3399;\n", -1, NULL));

    clawt_appearance_reload_palettes();
    clawt_appearance_set_scheme(appearance, "half");
    g_assert_cmpstr(clawt_appearance_get_scheme(appearance), ==, "half");

    yaml = clawt_lrg_theme_yaml(appearance, FALSE, 1.0);
    g_assert_nonnull(strstr(yaml, "  window: '#3a0a2aff'"));
    load_and_check(appearance, FALSE, 1.0, "partial palette");
}

int
main(int argc, char *argv[])
{
    gint result;

    /* before g_test_init(): g_get_user_config_dir() caches on first use */
    config_home = g_dir_make_tmp("clawt-lrgui-theme-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrgui/theme/every-combination-loads",
                    test_every_combination_loads);
    g_test_add_func("/lrgui/theme/accent-scale-fonts",
                    test_accent_scale_and_fonts_load);
    g_test_add_func("/lrgui/theme/partial-palette",
                    test_a_partial_disk_palette_falls_back);

    result = g_test_run();

    clawt_test_remove_tree(config_home);
    g_free(config_home);

    return result;
}
