/*
 * test-lrg-models.c - clawtilla-lrg's shell decisions, without libregnum
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The theme document, the island's sentence and the plain-text escaper
 * are what the libregnum client decides for itself; everything a page
 * decides is in its own tests/test-lrg-models-<area>.c.  The model files
 * include no libregnum header, so these run in every `make test`, with
 * or without LRG_BACKEND.  Whether libregnum *accepts* the document is
 * tests/test-lrg-theme.c, which needs the library.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "../clients/lrg/lrg-theme-model.c"
#include "../clients/lrg/lrg-island-model.c"
#include "../clients/lrg/lrg-text-model.c"

/* ── The palette table ────────────────────────────────────────────── */

static void
test_every_required_colour_has_a_row(void)
{
    g_auto(GStrv) required = clawt_appearance_palette_missing("");
    const ClawtLrgPaletteUse *uses;
    guint n_uses = 0;
    guint i;

    uses = clawt_lrg_palette_uses(&n_uses);

    /*
     * A colour added to the shared vocabulary later must be a decision
     * somebody makes in lrg-theme-model.c, not a colour this client
     * silently never draws.
     */
    for (i = 0; required[i] != NULL; i++) {
        guint j;
        gboolean found = FALSE;

        for (j = 0; j < n_uses; j++) {
            if (g_strcmp0(uses[j].adw_name, required[i]) == 0)
                found = TRUE;
        }

        if (!found)
            g_error("%s has no row in palette_uses[]", required[i]);
    }

    /* and nothing in the table is a name the vocabulary does not have */
    for (i = 0; i < n_uses; i++) {
        g_assert_true(g_strv_contains((const gchar *const *)required,
                                      uses[i].adw_name));
        g_assert_nonnull(uses[i].note);
    }
}

static void
test_no_role_is_fed_twice(void)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    const ClawtLrgPaletteUse *uses;
    guint n_uses = 0;
    guint i;

    uses = clawt_lrg_palette_uses(&n_uses);

    /* two palette colours on one role would be two answers to one question */
    for (i = 0; i < n_uses; i++) {
        if (uses[i].role == NULL)
            continue;

        g_assert_false(g_hash_table_contains(seen, uses[i].role));
        g_hash_table_add(seen, (gpointer)uses[i].role);
    }
}

static void
test_stock_palettes_define_every_required_colour(void)
{
    gboolean dark;

    for (dark = FALSE; dark <= TRUE; dark++) {
        g_auto(GStrv) missing =
            clawt_appearance_palette_missing(clawt_lrg_stock_palette(dark));
        g_auto(GStrv) required = clawt_appearance_palette_missing("");
        guint i;

        g_assert_cmpuint(g_strv_length(missing), ==, 0);

        /* and every one of them is readable, not merely present */
        for (i = 0; required[i] != NULL; i++) {
            gdouble rgba[4];

            if (!clawt_palette_color(clawt_lrg_stock_palette(dark),
                                     required[i], rgba))
                g_error("stock %s %s is unreadable", dark ? "dark" : "light",
                        required[i]);
        }
    }
}

/* ── Darkness and scale ───────────────────────────────────────────── */

static void
test_darkness_follows_the_scheme(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    /* system defers to the desktop, both ways */
    g_assert_true(clawt_lrg_appearance_is_dark(appearance, TRUE));
    g_assert_false(clawt_lrg_appearance_is_dark(appearance, FALSE));

    clawt_appearance_set_theme(appearance, CLAWT_THEME_LIGHT);
    g_assert_false(clawt_lrg_appearance_is_dark(appearance, TRUE));

    clawt_appearance_set_theme(appearance, CLAWT_THEME_DARK);
    g_assert_true(clawt_lrg_appearance_is_dark(appearance, FALSE));

    clawt_appearance_set_theme(appearance, CLAWT_THEME_CATPPUCCIN_MOCHA);
    g_assert_true(clawt_lrg_appearance_is_dark(appearance, FALSE));
}

static void
test_scale_prefers_the_setting(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();

    g_assert_cmpfloat(clawt_lrg_effective_scale(appearance, 2.0), ==, 2.0);
    g_assert_cmpfloat(clawt_lrg_effective_scale(appearance, 0.0), ==, 1.0);
    g_assert_cmpfloat(clawt_lrg_effective_scale(appearance, 9.0), ==,
                      CLAWT_LRG_SCALE_MAX);

    clawt_appearance_set_lrg_scale(appearance, 1.5);
    g_assert_cmpfloat(clawt_lrg_effective_scale(appearance, 2.0), ==, 1.5);
    g_assert_cmpfloat(clawt_lrg_effective_scale(NULL, 1.25), ==, 1.25);
}

/* ── The theme document ───────────────────────────────────────────── */

/* The colour a role is given in @yaml, as written: `'#rrggbbaa'`. */
static gchar *
role_value(const gchar *yaml, const gchar *role)
{
    g_autofree gchar *needle = g_strdup_printf("\n  %s: '", role);
    const gchar *at = strstr(yaml, needle);

    if (at == NULL)
        return NULL;

    at += strlen(needle);
    return g_strndup(at, 9);
}

static void
test_document_names_every_role_once(void)
{
    static const gchar *const roles[] = {
        "window", "panel", "surface", "border", "text", "text-muted",
        "text-on-accent", "control", "control-hover", "control-pressed",
        "input", "track", "handle", "popup", "tooltip", "shadow", "veil",
        "success", "warning", "danger", "link", "disabled"
    };
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *yaml = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(roles); i++) {
        g_autofree gchar *needle = g_strdup_printf("\n  %s: '", roles[i]);
        const gchar *first = strstr(yaml, needle);

        g_assert_nonnull(first);
        /* a duplicate key in YAML silently keeps the last one */
        g_assert_null(strstr(first + 1, needle));
    }

    g_assert_nonnull(strstr(yaml, "\ndark: true\n"));
    g_assert_nonnull(strstr(yaml, "\nmotion: smooth\n"));
}

static void
test_mocha_is_drawn_in_mocha(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *window = NULL;
    g_autofree gchar *accent = NULL;

    clawt_appearance_set_theme(appearance, CLAWT_THEME_CATPPUCCIN_MOCHA);
    yaml = clawt_lrg_theme_yaml(appearance, FALSE, 1.0);

    /*
     * The value, not the shape: a built-in palette once fell through to
     * stock libadwaita here because the getter only answered for files.
     */
    window = role_value(yaml, "window");
    g_assert_cmpstr(window, ==, "#1e1e2eff");
    g_assert_nonnull(strstr(yaml, "accent: '#89b4faff'"));

    clawt_appearance_set_lrg_accent(appearance, "#ff7a3d");
    g_free(yaml);
    yaml = clawt_lrg_theme_yaml(appearance, FALSE, 1.0);
    accent = g_strdup(strstr(yaml, "accent: '"));
    g_assert_true(g_str_has_prefix(accent, "accent: '#ff7a3dff'"));
}

static void
test_reduced_motion_stills_everything(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *moving = NULL;
    g_autofree gchar *still = NULL;

    moving = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);
    g_assert_nonnull(strstr(moving, "effect: aurora"));
    g_assert_nonnull(strstr(moving, "effect: shimmer"));

    clawt_appearance_set_lrg_motion(appearance, CLAWT_LRG_MOTION_NONE);
    still = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);

    /* the background holds still and the busy shimmer becomes a border */
    g_assert_nonnull(strstr(still, "\nmotion: none\n"));
    g_assert_nonnull(strstr(still, "effect-intensity: 0.00"));
    g_assert_null(strstr(still, "effect: shimmer"));
}

static void
test_flat_material_and_no_background(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *yaml = NULL;

    clawt_appearance_set_lrg_material(appearance, CLAWT_LRG_MATERIAL_FLAT);
    clawt_appearance_set_lrg_background(appearance,
                                        CLAWT_LRG_BACKGROUND_NONE);
    yaml = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);

    g_assert_null(strstr(yaml, "material:"));
    g_assert_null(strstr(yaml, "effect: aurora"));
    g_assert_null(strstr(yaml, "effect: mesh-gradient"));
}

static void
test_scale_reaches_the_metrics(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *normal = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);
    g_autofree gchar *large = clawt_lrg_theme_yaml(appearance, TRUE, 2.0);

    g_assert_nonnull(strstr(normal, "  control-height: 36.0\n"));
    g_assert_nonnull(strstr(large, "  control-height: 72.0\n"));
}

static void
test_tone_classes(void)
{
    g_assert_cmpstr(clawt_lrg_tone_class("good"), ==, "clawt-tone-good");
    g_assert_cmpstr(clawt_lrg_tone_class("warn"), ==, "clawt-tone-warn");
    g_assert_cmpstr(clawt_lrg_tone_class("bad"), ==, "clawt-tone-bad");
    g_assert_cmpstr(clawt_lrg_tone_class("info"), ==, "clawt-tone-info");
    g_assert_cmpstr(clawt_lrg_tone_class("neutral"), ==,
                    "clawt-tone-neutral");
    g_assert_cmpstr(clawt_lrg_tone_class(NULL), ==, "clawt-tone-neutral");
    g_assert_cmpstr(clawt_lrg_tone_class("purple"), ==, "clawt-tone-neutral");
}

static void
test_every_task_tone_is_painted(void)
{
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    g_autofree gchar *yaml = clawt_lrg_theme_yaml(appearance, TRUE, 1.0);
    GEnumClass *states = g_type_class_ref(CLAWT_TYPE_TASK_STATE);
    guint i;

    /*
     * Walk the enum, not a list of tones: a state added later whose tone
     * the stylesheet does not paint fails here instead of drawing grey.
     */
    for (i = 0; i < states->n_values; i++) {
        const gchar *tone = clawt_task_state_tone(states->values[i].value);
        g_autofree gchar *rule = g_strdup_printf(
            "class: %s\n", clawt_lrg_tone_class(tone));

        g_assert_nonnull(strstr(yaml, rule));
    }

    g_type_class_unref(states);
}

/* ── The island ───────────────────────────────────────────────────── */

static void
test_island_says_the_urgent_thing_first(void)
{
    ClawtLrgIslandState state = { 0 };
    g_autofree gchar *offline = NULL;
    g_autofree gchar *busy = NULL;
    g_autofree gchar *idle = NULL;

    state.spend_micros = -1;
    state.turns = 3;
    state.decisions = 2;

    /* a disconnected window's counts are stale: none are shown */
    offline = clawt_lrg_island_text(&state);
    g_assert_cmpstr(offline, ==, "Not connected");
    g_assert_true(clawt_lrg_island_alert(&state));

    state.connected = TRUE;
    state.held = TRUE;
    state.agents = 2;
    busy = clawt_lrg_island_text(&state);
    g_assert_true(g_str_has_prefix(busy, "Fleet paused · 2 decisions waiting"));
    g_assert_nonnull(strstr(busy, "3 turns across 2 agents"));
    g_assert_true(clawt_lrg_island_alert(&state));

    memset(&state, 0, sizeof(state));
    state.connected = TRUE;
    state.spend_micros = -1;
    idle = clawt_lrg_island_text(&state);
    g_assert_cmpstr(idle, ==, "Fleet idle");
    g_assert_false(clawt_lrg_island_alert(&state));
}

static void
test_island_shows_spend_only_when_known(void)
{
    ClawtLrgIslandState state = { 0 };
    g_autofree gchar *unknown = NULL;
    g_autofree gchar *known = NULL;

    state.connected = TRUE;
    state.spend_micros = -1;
    unknown = clawt_lrg_island_text(&state);
    g_assert_null(strstr(unknown, "today"));

    /* zero is free and known; -1 is unknown, never free */
    state.spend_micros = 0;
    known = clawt_lrg_island_text(&state);
    g_assert_nonnull(strstr(known, "today"));
}

static void
test_island_counts_agents_not_running_turns(void)
{
    ClawtLrgIslandState state = { 0 };
    g_autoptr(JsonParser) parser = json_parser_new();

    /*
     * One agent in three rooms and one in one: four turns, two agents.
     * Summing `running_turns` (3, 3, 3, 1) would say ten.
     */
    g_assert_true(json_parser_load_from_data(parser,
        "{\"turns\": ["
        "{\"agent\": \"alpha\", \"room\": \"a\", \"running_turns\": 3},"
        "{\"agent\": \"alpha\", \"room\": \"b\", \"running_turns\": 3},"
        "{\"agent\": \"alpha\", \"room\": \"c\", \"running_turns\": 3},"
        "{\"agent\": \"beta\",  \"room\": \"d\", \"running_turns\": 1}"
        "]}", -1, NULL));

    clawt_lrg_island_fold_activity(&state, json_parser_get_root(parser));
    g_assert_cmpuint(state.turns, ==, 4);
    g_assert_cmpuint(state.agents, ==, 2);

    /* a reply with no turns clears, rather than keeping the last count */
    clawt_lrg_island_fold_activity(&state, NULL);
    g_assert_cmpuint(state.turns, ==, 0);
    g_assert_cmpuint(state.agents, ==, 0);
}

/* ── Text ─────────────────────────────────────────────────────────── */

static void
test_escape_shows_text_as_written(void)
{
    g_autofree gchar *plain = clawt_lrg_markdown_escape("hello world");
    g_autofree gchar *file = clawt_lrg_markdown_escape("my_file_name.c");
    g_autofree gchar *link = clawt_lrg_markdown_escape("[x](javascript:y)");
    g_autofree gchar *lines = clawt_lrg_markdown_escape("a\r\nb");
    g_autofree gchar *none = clawt_lrg_markdown_escape(NULL);

    g_assert_cmpstr(plain, ==, "hello world");
    /* an underscore in a filename does not start italics */
    g_assert_cmpstr(file, ==, "my\\_file\\_name\\.c");
    /* nothing in somebody's words can become a link */
    g_assert_cmpstr(link, ==, "\\[x\\]\\(javascript\\:y\\)");
    /* a line break is a hard break, and \r is dropped */
    g_assert_cmpstr(lines, ==, "a\\\nb");
    g_assert_cmpstr(none, ==, "");
}

static void
test_guarded_replies_follow_the_selection(void)
{
    /* unguarded: always delivered, however far the selection moved */
    g_assert_true(clawt_lrg_reply_is_current(FALSE, 3, 3));
    g_assert_true(clawt_lrg_reply_is_current(FALSE, 3, 9));

    /* guarded: only while the selection is the one it was sent for */
    g_assert_true(clawt_lrg_reply_is_current(TRUE, 3, 3));
    g_assert_false(clawt_lrg_reply_is_current(TRUE, 3, 4));
    g_assert_false(clawt_lrg_reply_is_current(TRUE, G_MAXUINT, 0));
}

/* graylib picks a decoder by extension: "XXXXXXjpg" has none */
static void
test_image_template_has_a_dot(void)
{
    g_autofree gchar *png = clawt_lrg_image_template("clawtilla-avatar",
                                                     "image/png");
    g_autofree gchar *jpg = clawt_lrg_image_template("clawtilla-frame",
                                                     "image/jpeg");
    g_autofree gchar *unknown = clawt_lrg_image_template("clawtilla-frame",
                                                         "text/plain");

    g_assert_cmpstr(png, ==, "clawtilla-avatar-XXXXXX.png");
    g_assert_cmpstr(jpg, ==, "clawtilla-frame-XXXXXX.jpg");
    g_assert_null(unknown);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-models/theme/every-colour-has-a-row",
                    test_every_required_colour_has_a_row);
    g_test_add_func("/lrg-models/theme/no-role-fed-twice",
                    test_no_role_is_fed_twice);
    g_test_add_func("/lrg-models/theme/stock-palettes-complete",
                    test_stock_palettes_define_every_required_colour);
    g_test_add_func("/lrg-models/theme/darkness", test_darkness_follows_the_scheme);
    g_test_add_func("/lrg-models/theme/scale", test_scale_prefers_the_setting);
    g_test_add_func("/lrg-models/theme/every-role-once",
                    test_document_names_every_role_once);
    g_test_add_func("/lrg-models/theme/mocha-is-mocha",
                    test_mocha_is_drawn_in_mocha);
    g_test_add_func("/lrg-models/theme/reduced-motion",
                    test_reduced_motion_stills_everything);
    g_test_add_func("/lrg-models/theme/flat-and-still",
                    test_flat_material_and_no_background);
    g_test_add_func("/lrg-models/theme/scale-reaches-metrics",
                    test_scale_reaches_the_metrics);
    g_test_add_func("/lrg-models/theme/tone-classes", test_tone_classes);
    g_test_add_func("/lrg-models/theme/every-task-tone-painted",
                    test_every_task_tone_is_painted);
    g_test_add_func("/lrg-models/island/urgent-first",
                    test_island_says_the_urgent_thing_first);
    g_test_add_func("/lrg-models/island/spend-when-known",
                    test_island_shows_spend_only_when_known);
    g_test_add_func("/lrg-models/island/agents-not-running-turns",
                    test_island_counts_agents_not_running_turns);
    g_test_add_func("/lrg-models/text/escape", test_escape_shows_text_as_written);
    g_test_add_func("/lrg-models/text/image-template",
                    test_image_template_has_a_dot);
    g_test_add_func("/lrg-models/requests/guard",
                    test_guarded_replies_follow_the_selection);

    return g_test_run();
}
