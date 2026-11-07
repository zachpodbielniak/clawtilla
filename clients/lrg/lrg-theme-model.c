/*
 * lrg-theme-model.c - What an appearance looks like, as a libregnum theme
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * All three clients draw from one set of palettes, written in
 * libadwaita's colour vocabulary because that is what GTK reads.
 * libregnum reads neither CSS nor that vocabulary: it has twenty-six
 * colour roles of its own and a YAML theme format.  This file is the
 * translation -- one table saying what each palette colour becomes, and
 * one function turning an appearance into a complete theme document.
 *
 * No libregnum header is included on purpose: see clawt-lrg-model.h.
 */

#include "clawt-lrg-model.h"

#include <math.h>
#include <string.h>

/* ── What each palette colour becomes ─────────────────────────────── */

/*
 * Every name in the shared vocabulary has a row.  A NULL role is a
 * decision too, and the note says why -- libregnum draws no overview, no
 * thumbnails and no secondary sidebar, and a colour mapped onto a role
 * that some other row already fills would be two answers to one
 * question.  tests/test-lrg-models.c walks clawt_appearance_palette_missing("")
 * and fails on any required colour without a row here.
 */
static const ClawtLrgPaletteUse palette_uses[] = {
    { "window_bg_color",  "window",         "the canvas behind everything; the animated background is tinted toward it" },
    { "window_fg_color",  "text",           "body text; muted text, borders and the track are mixed from it and the window colour" },
    { "view_bg_color",    "input",          "text entries and the composer" },
    { "view_fg_color",    NULL,             "same as window_fg_color in every shipped palette; libregnum has one text role" },
    { "sidebar_bg_color", "panel",          "the fleet sidebar and every panel" },
    { "sidebar_fg_color", NULL,             "libregnum's panels take the text role" },
    { "sidebar_backdrop_color", NULL,       "libregnum has no unfocused-window state" },
    { "sidebar_shade_color", NULL,          "shadows come from shade_color" },
    { "sidebar_border_color", "border",     "hairlines between panes" },
    { "secondary_sidebar_bg_color", NULL,   "clawtilla-lrg has no secondary sidebar" },
    { "secondary_sidebar_fg_color", NULL,   "clawtilla-lrg has no secondary sidebar" },
    { "secondary_sidebar_backdrop_color", NULL, "clawtilla-lrg has no secondary sidebar" },
    { "secondary_sidebar_shade_color", NULL, "clawtilla-lrg has no secondary sidebar" },
    { "secondary_sidebar_border_color", NULL, "clawtilla-lrg has no secondary sidebar" },
    { "headerbar_bg_color", NULL,           "the header bar is drawn in the panel material, over the background" },
    { "headerbar_fg_color", NULL,           "the header bar takes the text role" },
    { "headerbar_border_color", NULL,       "the header bar's edge is the border role" },
    { "headerbar_backdrop_color", NULL,     "libregnum has no unfocused-window state" },
    { "headerbar_shade_color", NULL,        "shadows come from shade_color" },
    { "headerbar_darker_shade_color", NULL, "shadows come from shade_color" },
    { "card_bg_color",    "surface",        "cards, chat bubbles and controls; hover and press are mixed toward the text colour" },
    { "card_fg_color",    NULL,             "cards take the text role" },
    { "card_shade_color", NULL,             "shadows come from shade_color" },
    { "dialog_bg_color",  NULL,             "dialogs are popups and take popover_bg_color" },
    { "dialog_fg_color",  NULL,             "dialogs take the text role" },
    { "popover_bg_color", "popup",          "menus, popups, dialogs and tooltips" },
    { "popover_fg_color", NULL,             "popups take the text role" },
    { "popover_shade_color", NULL,          "shadows come from shade_color" },
    { "accent_bg_color",  "accent",         "primary actions, selection, focus, the island's glow; lrg_accent overrides it" },
    { "accent_fg_color",  "text-on-accent", "text on a primary action" },
    { "accent_color",     "link",           "links, and the second colour of the animated background" },
    { "destructive_bg_color", "danger",     "destructive actions and the bad tone" },
    { "destructive_fg_color", NULL,         "text on a danger fill is text-on-accent" },
    { "destructive_color", NULL,            "the danger role covers both fill and text" },
    { "error_bg_color",   NULL,             "errors use the danger role" },
    { "error_fg_color",   NULL,             "errors use the danger role" },
    { "error_color",      NULL,             "errors use the danger role" },
    { "success_bg_color", "success",        "the good tone: completed tasks, running agents" },
    { "success_fg_color", NULL,             "badges draw their text in the tone colour" },
    { "success_color",    NULL,             "the success role covers both fill and text" },
    { "warning_bg_color", "warning",        "the warn tone: stalled work, decisions waiting" },
    { "warning_fg_color", NULL,             "badges draw their text in the tone colour" },
    { "warning_color",    NULL,             "the warning role covers both fill and text" },
    { "shade_color",      "shadow",         "every drop shadow, at its own alpha; the modal veil is mixed from it" },
    { "scrollbar_outline_color", NULL,      "libregnum's scrollbars have no outline" },
    { "thumbnail_bg_color", NULL,           "clawtilla-lrg draws no thumbnails" },
    { "thumbnail_fg_color", NULL,           "clawtilla-lrg draws no thumbnails" },
    { "active_toggle_bg_color", "control-pressed", "a toggle that is on" },
    { "active_toggle_fg_color", NULL,       "toggles take the text role" },
    { "overview_bg_color", NULL,            "clawtilla-lrg has no overview" },
    { "overview_fg_color", NULL,            "clawtilla-lrg has no overview" }
};

const ClawtLrgPaletteUse *
clawt_lrg_palette_uses(guint *n_uses)
{
    if (n_uses != NULL)
        *n_uses = G_N_ELEMENTS(palette_uses);

    return palette_uses;
}

/* ── libadwaita's stock colours ───────────────────────────────────── */

/*
 * libadwaita 1.9's own values, written out because the "system", "light"
 * and "dark" schemes carry no palette -- GTK asks libadwaita for them and
 * the web client has them in its stylesheet.  Every required colour is
 * defined, so a stock scheme takes the same path through the reader as a
 * palette file and a missing name cannot hide behind a fallback.
 */
static const gchar stock_light[] =
    "@define-color window_bg_color #fafafb;\n"
    "@define-color window_fg_color #2e2e33;\n"
    "@define-color view_bg_color #ffffff;\n"
    "@define-color view_fg_color #2e2e33;\n"
    "@define-color headerbar_bg_color #ffffff;\n"
    "@define-color headerbar_fg_color #2e2e33;\n"
    "@define-color headerbar_border_color #2e2e33;\n"
    "@define-color headerbar_backdrop_color #fafafb;\n"
    "@define-color headerbar_shade_color rgba(0, 0, 6, 0.12);\n"
    "@define-color headerbar_darker_shade_color rgba(0, 0, 6, 0.12);\n"
    "@define-color sidebar_bg_color #ebebed;\n"
    "@define-color sidebar_fg_color #2e2e33;\n"
    "@define-color sidebar_backdrop_color #f2f2f4;\n"
    "@define-color sidebar_shade_color rgba(0, 0, 6, 0.07);\n"
    "@define-color sidebar_border_color rgba(0, 0, 6, 0.07);\n"
    "@define-color secondary_sidebar_bg_color #f3f3f5;\n"
    "@define-color secondary_sidebar_fg_color #2e2e33;\n"
    "@define-color secondary_sidebar_backdrop_color #f6f6fa;\n"
    "@define-color secondary_sidebar_shade_color rgba(0, 0, 6, 0.07);\n"
    "@define-color secondary_sidebar_border_color rgba(0, 0, 6, 0.07);\n"
    "@define-color card_bg_color #ffffff;\n"
    "@define-color card_fg_color #2e2e33;\n"
    "@define-color card_shade_color rgba(0, 0, 6, 0.07);\n"
    "@define-color dialog_bg_color #fafafb;\n"
    "@define-color dialog_fg_color #2e2e33;\n"
    "@define-color popover_bg_color #ffffff;\n"
    "@define-color popover_fg_color #2e2e33;\n"
    "@define-color popover_shade_color rgba(0, 0, 6, 0.07);\n"
    "@define-color accent_bg_color #3584e4;\n"
    "@define-color accent_fg_color #ffffff;\n"
    "@define-color accent_color #0461be;\n"
    "@define-color destructive_bg_color #e01b24;\n"
    "@define-color destructive_fg_color #ffffff;\n"
    "@define-color destructive_color #c30000;\n"
    "@define-color error_bg_color #e01b24;\n"
    "@define-color error_fg_color #ffffff;\n"
    "@define-color error_color #c30000;\n"
    "@define-color success_bg_color #2ec27e;\n"
    "@define-color success_fg_color #ffffff;\n"
    "@define-color success_color #007c3d;\n"
    "@define-color warning_bg_color #e5a50a;\n"
    "@define-color warning_fg_color rgba(0, 0, 0, 0.8);\n"
    "@define-color warning_color #905400;\n"
    "@define-color shade_color rgba(0, 0, 6, 0.07);\n"
    "@define-color scrollbar_outline_color #ffffff;\n"
    "@define-color thumbnail_bg_color #ffffff;\n"
    "@define-color thumbnail_fg_color #2e2e33;\n"
    "@define-color active_toggle_bg_color #ffffff;\n"
    "@define-color active_toggle_fg_color #2e2e33;\n"
    "@define-color overview_bg_color #f3f3f5;\n"
    "@define-color overview_fg_color #2e2e33;\n";

static const gchar stock_dark[] =
    "@define-color window_bg_color #222226;\n"
    "@define-color window_fg_color #ffffff;\n"
    "@define-color view_bg_color #1d1d20;\n"
    "@define-color view_fg_color #ffffff;\n"
    "@define-color headerbar_bg_color #2e2e32;\n"
    "@define-color headerbar_fg_color #ffffff;\n"
    "@define-color headerbar_border_color #ffffff;\n"
    "@define-color headerbar_backdrop_color #222226;\n"
    "@define-color headerbar_shade_color rgba(0, 0, 6, 0.36);\n"
    "@define-color headerbar_darker_shade_color rgba(0, 0, 12, 0.9);\n"
    "@define-color sidebar_bg_color #2e2e32;\n"
    "@define-color sidebar_fg_color #ffffff;\n"
    "@define-color sidebar_backdrop_color #28282c;\n"
    "@define-color sidebar_shade_color rgba(0, 0, 6, 0.25);\n"
    "@define-color sidebar_border_color rgba(0, 0, 6, 0.36);\n"
    "@define-color secondary_sidebar_bg_color #28282c;\n"
    "@define-color secondary_sidebar_fg_color #ffffff;\n"
    "@define-color secondary_sidebar_backdrop_color #252529;\n"
    "@define-color secondary_sidebar_shade_color rgba(0, 0, 6, 0.25);\n"
    "@define-color secondary_sidebar_border_color rgba(0, 0, 6, 0.36);\n"
    "@define-color card_bg_color #36363a;\n"
    "@define-color card_fg_color #ffffff;\n"
    "@define-color card_shade_color rgba(0, 0, 6, 0.36);\n"
    "@define-color dialog_bg_color #36363a;\n"
    "@define-color dialog_fg_color #ffffff;\n"
    "@define-color popover_bg_color #36363a;\n"
    "@define-color popover_fg_color #ffffff;\n"
    "@define-color popover_shade_color rgba(0, 0, 6, 0.25);\n"
    "@define-color accent_bg_color #3584e4;\n"
    "@define-color accent_fg_color #ffffff;\n"
    "@define-color accent_color #81d0ff;\n"
    "@define-color destructive_bg_color #c01c28;\n"
    "@define-color destructive_fg_color #ffffff;\n"
    "@define-color destructive_color #ff938c;\n"
    "@define-color error_bg_color #c01c28;\n"
    "@define-color error_fg_color #ffffff;\n"
    "@define-color error_color #ff938c;\n"
    "@define-color success_bg_color #26a269;\n"
    "@define-color success_fg_color #ffffff;\n"
    "@define-color success_color #78e9ab;\n"
    "@define-color warning_bg_color #cd9309;\n"
    "@define-color warning_fg_color rgba(0, 0, 0, 0.8);\n"
    "@define-color warning_color #ffc252;\n"
    "@define-color shade_color rgba(0, 0, 6, 0.25);\n"
    "@define-color scrollbar_outline_color rgba(0, 0, 6, 0.5);\n"
    "@define-color thumbnail_bg_color #39393d;\n"
    "@define-color thumbnail_fg_color #ffffff;\n"
    "@define-color active_toggle_bg_color rgba(255, 255, 255, 0.2);\n"
    "@define-color active_toggle_fg_color #ffffff;\n"
    "@define-color overview_bg_color #28282c;\n"
    "@define-color overview_fg_color #ffffff;\n";

const gchar *
clawt_lrg_stock_palette(gboolean dark)
{
    return dark ? stock_dark : stock_light;
}

gboolean
clawt_lrg_appearance_is_dark(ClawtAppearance *appearance,
                             gboolean         system_dark)
{
    const gchar *css;

    g_return_val_if_fail(appearance != NULL, system_dark);

    /*
     * A palette on disk says which way it draws by its own window colour;
     * a built-in scheme is answered by the library.  Asked directly rather
     * than by walking the scheme table, which would make this client the
     * only one offering that enumeration (make parity, layer 3).
     */
    css = clawt_appearance_get_palette_css(appearance);

    if (css != NULL) {
        gdouble bg[4];

        /* unreadable: the library's assumption for a palette, dark */
        if (!clawt_palette_color(css, "window_bg_color", bg))
            return TRUE;

        return (0.2126 * bg[0] + 0.7152 * bg[1] + 0.0722 * bg[2]) < 0.5;
    }

    if (clawt_appearance_get_theme(appearance) == CLAWT_THEME_SYSTEM)
        return system_dark;

    return clawt_appearance_theme_is_dark(clawt_appearance_get_theme(appearance));
}

/* ── Colour arithmetic ────────────────────────────────────────────── */

typedef struct {
    gdouble r;
    gdouble g;
    gdouble b;
    gdouble a;
} Rgba;

static Rgba
rgba_mix(Rgba from, Rgba to, gdouble t)
{
    Rgba out;

    out.r = from.r + (to.r - from.r) * t;
    out.g = from.g + (to.g - from.g) * t;
    out.b = from.b + (to.b - from.b) * t;
    out.a = from.a + (to.a - from.a) * t;

    return out;
}

static Rgba
rgba_with_alpha(Rgba color, gdouble alpha)
{
    color.a = alpha;
    return color;
}

/*
 * Composited over the window colour: libregnum's roles are opaque except
 * where alpha is the point (shadow, veil), and a palette colour such as
 * libadwaita's dark card -- white at 8% in the original -- only means
 * something over what it was drawn on.
 */
static Rgba
rgba_over(Rgba color, Rgba under)
{
    Rgba out;

    out.r = color.r * color.a + under.r * (1.0 - color.a);
    out.g = color.g * color.a + under.g * (1.0 - color.a);
    out.b = color.b * color.a + under.b * (1.0 - color.a);
    out.a = 1.0;

    return out;
}

static guint
channel(gdouble value)
{
    return (guint)CLAMP(lround(value * 255.0), 0, 255);
}

/* Quoted `'#rrggbbaa'`: an unquoted '#' starts a YAML comment. */
static void
append_color(GString *out, Rgba color)
{
    g_string_append_printf(out, "'#%02x%02x%02x%02x'",
                           channel(color.r), channel(color.g),
                           channel(color.b), channel(color.a));
}

static gboolean
hex_to_rgba(const gchar *hex, Rgba *out)
{
    gdouble values[4];
    g_autofree gchar *css = NULL;

    if (hex == NULL)
        return FALSE;

    /* Through the one reader, so the accent is parsed like a palette. */
    css = g_strdup_printf("@define-color x %s;", hex);

    if (!clawt_palette_color(css, "x", values))
        return FALSE;

    out->r = values[0];
    out->g = values[1];
    out->b = values[2];
    out->a = values[3];

    return TRUE;
}

typedef struct {
    const gchar *primary;   /* the palette in use */
    const gchar *fallback;  /* libadwaita's stock colours for its brightness */
} PaletteSource;

/*
 * A colour from the palette, else from the stock palette of the same
 * brightness.  A partial palette on disk is supported -- GTK falls back
 * to libadwaita's value for every name it leaves out -- and this is the
 * same rule, so the two clients agree about what such a file looks like.
 */
static Rgba
palette_get(const PaletteSource *source, const gchar *name)
{
    gdouble values[4] = { 0.0, 0.0, 0.0, 1.0 };
    Rgba out;

    if (!clawt_palette_color(source->primary, name, values))
        clawt_palette_color(source->fallback, name, values);

    out.r = values[0];
    out.g = values[1];
    out.b = values[2];
    out.a = values[3];

    return out;
}

/* ── The theme document ───────────────────────────────────────────── */

gdouble
clawt_lrg_effective_scale(ClawtAppearance *appearance, gdouble monitor_scale)
{
    gdouble setting = 0.0;

    if (appearance != NULL)
        setting = clawt_appearance_get_lrg_scale(appearance);

    if (setting > 0.0)
        return setting;

    if (!(monitor_scale > 0.0))
        return 1.0;

    return CLAMP(monitor_scale, CLAWT_LRG_SCALE_MIN, CLAWT_LRG_SCALE_MAX);
}

static const gchar *
motion_nick(ClawtLrgMotion motion)
{
    /*
     * libregnum's spellings.  No `default:`: -Wswitch names a value
     * somebody added to the enum without deciding what it animates like.
     */
    switch (motion) {
    case CLAWT_LRG_MOTION_DEFAULT:
    case CLAWT_LRG_MOTION_SMOOTH:
        return "smooth";

    case CLAWT_LRG_MOTION_NONE:
        return "none";

    case CLAWT_LRG_MOTION_SNAPPY:
        return "snappy";

    case CLAWT_LRG_MOTION_BOUNCY:
        return "bouncy";

    case CLAWT_LRG_MOTION_WOBBLY:
        return "wobbly";
    }

    return "smooth";
}

/* NULL for "no material": the chrome is then opaque panel colour. */
static const gchar *
material_nick(ClawtLrgMaterial material)
{
    switch (material) {
    case CLAWT_LRG_MATERIAL_DEFAULT:
    case CLAWT_LRG_MATERIAL_FROSTED:
        return "frosted";

    case CLAWT_LRG_MATERIAL_GLASS:
        return "glass";

    case CLAWT_LRG_MATERIAL_FLAT:
        return NULL;
    }

    return "frosted";
}

/* NULL for "nothing behind the interface but the window colour". */
static const gchar *
background_effect(ClawtLrgBackground background)
{
    switch (background) {
    case CLAWT_LRG_BACKGROUND_DEFAULT:
    case CLAWT_LRG_BACKGROUND_AURORA:
        return "aurora";

    case CLAWT_LRG_BACKGROUND_MESH:
        return "mesh-gradient";

    case CLAWT_LRG_BACKGROUND_PLASMA:
        return "plasma";

    case CLAWT_LRG_BACKGROUND_NONE:
        return NULL;
    }

    return "aurora";
}

const gchar *
clawt_lrg_tone_class(const gchar *tone)
{
    if (g_strcmp0(tone, "good") == 0)
        return "clawt-tone-good";

    if (g_strcmp0(tone, "warn") == 0)
        return "clawt-tone-warn";

    if (g_strcmp0(tone, "bad") == 0)
        return "clawt-tone-bad";

    if (g_strcmp0(tone, "info") == 0)
        return "clawt-tone-info";

    return "clawt-tone-neutral";
}

static void
append_role(GString *out, const gchar *role, Rgba color)
{
    g_string_append_printf(out, "  %s: ", role);
    append_color(out, color);
    g_string_append_c(out, '\n');
}

/* A style rule's opening line plus its selector. */
static void
rule_begin(GString *out, const gchar *widget, const gchar *style_class,
           const gchar *part, const gchar *state)
{
    g_string_append(out, "  -");

    if (widget != NULL)
        g_string_append_printf(out, " widget: %s\n   ", widget);

    if (style_class != NULL)
        g_string_append_printf(out, " class: %s\n   ", style_class);

    g_string_append_printf(out, " part: %s\n", (part != NULL) ? part : "box");

    if (state != NULL)
        g_string_append_printf(out, "    state: %s\n", state);
}

static void
rule_color(GString *out, const gchar *key, Rgba color)
{
    g_string_append_printf(out, "    %s: ", key);
    append_color(out, color);
    g_string_append_c(out, '\n');
}

static void
rule_number(GString *out, const gchar *key, gdouble value)
{
    gchar text[G_ASCII_DTOSTR_BUF_SIZE];

    g_ascii_formatd(text, sizeof(text), "%.2f", value);
    g_string_append_printf(out, "    %s: %s\n", key, text);
}

/*
 * A material and the glass it is made of.  The tint is the surface's own
 * colour at a strength that keeps text on it readable over a busy
 * background: the more see-through the material, the stronger the tint.
 */
static void
rule_material(GString *out, const gchar *material, Rgba tint, gdouble scale)
{
    gchar blur[G_ASCII_DTOSTR_BUF_SIZE];

    if (material == NULL)
        return;

    g_ascii_formatd(blur, sizeof(blur), "%.1f", 22.0 * scale);
    g_string_append_printf(out, "    material: %s\n", material);
    g_string_append_printf(out, "    glass: {blur: %s, saturation: 1.25, "
                                "rim: %s, tint: ",
                           blur,
                           (g_strcmp0(material, "glass") == 0) ? "0.9"
                                                               : "0.45");
    append_color(out, tint);
    g_string_append(out, "}\n");
}

gchar *
clawt_lrg_theme_yaml(ClawtAppearance *appearance, gboolean system_dark,
                     gdouble scale)
{
    GString *out;
    PaletteSource source;
    gboolean dark;
    ClawtLrgMotion motion;
    const gchar *material;
    const gchar *effect;
    Rgba window, text, panel, surface, input, popup, accent, on_accent, link;
    Rgba danger, success, warning, shade, border, muted, pressed;
    Rgba black = { 0.0, 0.0, 0.0, 1.0 };

    g_return_val_if_fail(appearance != NULL, NULL);

    if (!(scale > 0.0))
        scale = 1.0;

    dark = clawt_lrg_appearance_is_dark(appearance, system_dark);
    motion = clawt_appearance_get_lrg_motion(appearance);
    material = material_nick(clawt_appearance_get_lrg_material(appearance));
    effect = background_effect(clawt_appearance_get_lrg_background(appearance));

    source.fallback = clawt_lrg_stock_palette(dark);
    source.primary = clawt_appearance_get_scheme_css(appearance);

    if (source.primary == NULL)
        source.primary = source.fallback;

    /* ── read the palette ── */
    window = rgba_over(palette_get(&source, "window_bg_color"), black);
    text = rgba_over(palette_get(&source, "window_fg_color"), window);
    panel = rgba_over(palette_get(&source, "sidebar_bg_color"), window);
    surface = rgba_over(palette_get(&source, "card_bg_color"), window);
    input = rgba_over(palette_get(&source, "view_bg_color"), window);
    popup = rgba_over(palette_get(&source, "popover_bg_color"), window);
    accent = rgba_over(palette_get(&source, "accent_bg_color"), window);
    on_accent = rgba_over(palette_get(&source, "accent_fg_color"), accent);
    link = rgba_over(palette_get(&source, "accent_color"), window);
    danger = rgba_over(palette_get(&source, "destructive_bg_color"), window);
    success = rgba_over(palette_get(&source, "success_bg_color"), window);
    warning = rgba_over(palette_get(&source, "warning_bg_color"), window);
    pressed = rgba_over(palette_get(&source, "active_toggle_bg_color"), surface);
    shade = palette_get(&source, "shade_color");
    border = rgba_over(palette_get(&source, "sidebar_border_color"), panel);

    {
        Rgba accent_override;

        if (hex_to_rgba(clawt_appearance_get_lrg_accent(appearance),
                        &accent_override))
            accent = rgba_over(accent_override, window);
    }

    /*
     * Derived rather than read: libadwaita has no muted-text colour, and
     * its dark border is near-black at 36%, which disappears over an
     * animated background.  Mixed from the text colour so both follow
     * the palette.
     */
    muted = rgba_mix(text, window, 0.42);
    border = rgba_mix(border, rgba_mix(text, window, 0.82), 0.6);

    out = g_string_new(NULL);

    g_string_append(out,
        "# Generated by clawtilla-lrg from appearance.yaml; see\n"
        "# docs/lrg-client.org#appearance.  Not a file to edit.\n");
    g_string_append_printf(out, "preset: %s\n", dark ? "midnight" : "daylight");
    g_string_append_printf(out, "name: clawtilla-%s\n",
                           clawt_appearance_get_scheme(appearance));
    g_string_append_printf(out, "dark: %s\n", dark ? "true" : "false");
    g_string_append_printf(out, "motion: %s\n", motion_nick(motion));
    g_string_append(out, "accent: ");
    append_color(out, accent);
    g_string_append(out, "\n");

    /* ── colour roles ── */
    g_string_append(out, "colors:\n");
    append_role(out, "window", window);
    append_role(out, "panel", panel);
    append_role(out, "surface", surface);
    append_role(out, "border", border);
    append_role(out, "text", text);
    append_role(out, "text-muted", muted);
    append_role(out, "text-on-accent", on_accent);
    append_role(out, "control", rgba_mix(surface, text, 0.04));
    append_role(out, "control-hover", rgba_mix(surface, text, 0.10));
    append_role(out, "control-pressed", pressed);
    append_role(out, "input", input);
    append_role(out, "track", rgba_mix(surface, text, 0.16));
    append_role(out, "handle", dark ? text : rgba_mix(window, text, 0.0));
    append_role(out, "popup", popup);
    append_role(out, "tooltip", popup);
    append_role(out, "shadow", rgba_with_alpha(shade, dark ? 0.62 : 0.22));
    append_role(out, "veil", rgba_with_alpha(rgba_mix(window, black, 0.5),
                                             dark ? 0.62 : 0.42));
    append_role(out, "success", success);
    append_role(out, "warning", warning);
    append_role(out, "danger", danger);
    append_role(out, "link", link);
    append_role(out, "disabled", rgba_mix(text, window, 0.6));

    /* ── metrics, at the interface scale ── */
    {
        static const struct {
            const gchar *nick;
            gdouble      value;
        } metrics[] = {
            { "font-size",         15.0 },
            { "font-size-small",   12.5 },
            { "font-size-large",   18.0 },
            { "font-size-title",   24.0 },
            { "corner-radius",     10.0 },
            { "control-height",    36.0 },
            { "padding-x",         14.0 },
            { "padding-y",          8.0 },
            { "spacing",            8.0 },
            { "row-height",        44.0 },
            { "popup-radius",      16.0 },
            { "shadow-blur",       26.0 },
            { "shadow-offset-y",    8.0 },
            { "scrollbar-size",     8.0 }
        };
        gsize i;
        gdouble font_scale = 1.0;
        gdouble points = clawt_appearance_get_font_size(appearance);

        /* a font size in appearance.yaml scales text, not layout */
        if (points > 0.0)
            font_scale = points / 11.0;

        g_string_append(out, "metrics:\n");

        for (i = 0; i < G_N_ELEMENTS(metrics); i++) {
            gchar value[G_ASCII_DTOSTR_BUF_SIZE];
            gdouble v = metrics[i].value * scale;

            if (g_str_has_prefix(metrics[i].nick, "font-size"))
                v *= font_scale;

            g_ascii_formatd(value, sizeof(value), "%.1f", v);
            g_string_append_printf(out, "  %s: %s\n", metrics[i].nick, value);
        }
    }

    /* ── style rules: the look ── */
    g_string_append(out, "styles:\n");

    /* the animated background, tinted toward the window so text holds */
    rule_begin(out, NULL, "clawt-backdrop", "box", NULL);
    rule_color(out, "background", window);

    if (effect != NULL) {
        g_string_append_printf(out, "    effect: %s\n", effect);
        g_string_append(out, "    effect-colors: [");
        append_color(out, window);
        g_string_append(out, ", ");
        append_color(out, rgba_mix(window, accent, dark ? 0.48 : 0.26));
        g_string_append(out, ", ");
        append_color(out, rgba_mix(window, link, dark ? 0.36 : 0.2));
        g_string_append(out, "]\n");
        rule_number(out, "effect-intensity",
                    motion == CLAWT_LRG_MOTION_NONE ? 0.0 : 0.85);
    }

    /* chrome: sidebar and header bar, in the chosen material */
    rule_begin(out, NULL, "clawt-chrome", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(panel,
                                                  material ? 0.72 : 1.0));
    rule_color(out, "border", rgba_with_alpha(border, 0.6));
    rule_number(out, "border-width", 1.0);
    rule_material(out, material, rgba_with_alpha(panel, dark ? 0.55 : 0.62),
                  scale);

    /* a card: lifted, rounded, glowing faintly in the accent on hover */
    rule_begin(out, NULL, "clawt-card", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(surface, 0.94));
    rule_color(out, "border", rgba_with_alpha(border, 0.7));
    rule_number(out, "border-width", 1.0);
    rule_number(out, "radius", 14.0 * scale);
    rule_color(out, "shadow", rgba_with_alpha(shade, dark ? 0.55 : 0.18));
    rule_number(out, "shadow-blur", 22.0 * scale);
    rule_number(out, "shadow-offset", 6.0 * scale);

    rule_begin(out, NULL, "clawt-card", "box", "hovered");
    rule_color(out, "glow", rgba_with_alpha(accent, dark ? 0.28 : 0.18));
    rule_number(out, "glow-size", 14.0 * scale);
    rule_color(out, "border", rgba_with_alpha(accent, 0.45));

    rule_begin(out, NULL, "clawt-card-selected", "box", NULL);
    rule_color(out, "background", rgba_mix(surface, accent, 0.16));
    rule_color(out, "border", rgba_with_alpha(accent, 0.8));
    rule_color(out, "glow", rgba_with_alpha(accent, 0.35));
    rule_number(out, "glow-size", 18.0 * scale);

    /* chat bubbles: theirs on the surface, the operator's in the accent */
    rule_begin(out, NULL, "clawt-bubble", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(surface, 0.92));
    rule_number(out, "radius", 16.0 * scale);
    rule_color(out, "shadow", rgba_with_alpha(shade, dark ? 0.4 : 0.12));
    rule_number(out, "shadow-blur", 14.0 * scale);
    rule_number(out, "shadow-offset", 3.0 * scale);

    rule_begin(out, NULL, "clawt-bubble-mine", "box", NULL);
    g_string_append(out, "    gradient: {kind: linear, angle: 120, start: ");
    append_color(out, accent);
    g_string_append(out, ", end: ");
    append_color(out, rgba_mix(accent, link, 0.45));
    g_string_append(out, "}\n");
    rule_number(out, "radius", 16.0 * scale);
    rule_color(out, "glow", rgba_with_alpha(accent, dark ? 0.3 : 0.15));
    rule_number(out, "glow-size", 12.0 * scale);

    rule_begin(out, NULL, "clawt-bubble-mine", "label", NULL);
    rule_color(out, "foreground", on_accent);

    /* the composer: glass with a rim light, whatever the chrome is */
    rule_begin(out, NULL, "clawt-composer", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(input, 0.85));
    rule_number(out, "radius", 18.0 * scale);
    rule_color(out, "border", rgba_with_alpha(border, 0.8));
    rule_number(out, "border-width", 1.0);
    rule_material(out, material != NULL ? "glass" : NULL,
                  rgba_with_alpha(input, 0.6), scale);

    rule_begin(out, NULL, "clawt-composer", "box", "focused");
    rule_color(out, "border", rgba_with_alpha(accent, 0.9));
    rule_color(out, "glow", rgba_with_alpha(accent, 0.3));
    rule_number(out, "glow-size", 16.0 * scale);

    /* the island: a dark pill that glows when something needs a person */
    rule_begin(out, NULL, "clawt-island", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(rgba_mix(window, black,
                                                           dark ? 0.55 : 0.82),
                                                  0.92));
    rule_number(out, "radius", 22.0 * scale);
    rule_color(out, "shadow", rgba_with_alpha(black, 0.45));
    rule_number(out, "shadow-blur", 24.0 * scale);
    rule_number(out, "shadow-offset", 6.0 * scale);

    rule_begin(out, NULL, "clawt-island", "label", NULL);
    rule_color(out, "foreground", rgba_mix(text, (Rgba){ 1.0, 1.0, 1.0, 1.0 },
                                           dark ? 0.0 : 1.0));

    rule_begin(out, NULL, "clawt-island-alert", "box", NULL);
    rule_color(out, "glow", rgba_with_alpha(warning, 0.55));
    rule_number(out, "glow-size", 22.0 * scale);
    rule_color(out, "border", rgba_with_alpha(warning, 0.85));
    rule_number(out, "border-width", 1.5);

    /* badges, one class per tone */
    {
        static const gchar *const tones[] = {
            "good", "warn", "bad", "info", "neutral"
        };
        gsize i;

        for (i = 0; i < G_N_ELEMENTS(tones); i++) {
            g_autofree gchar *klass =
                g_strdup_printf("clawt-tone-%s", tones[i]);
            Rgba tone;

            if (g_strcmp0(tones[i], "good") == 0)
                tone = success;
            else if (g_strcmp0(tones[i], "warn") == 0)
                tone = warning;
            else if (g_strcmp0(tones[i], "bad") == 0)
                tone = danger;
            else if (g_strcmp0(tones[i], "info") == 0)
                tone = accent;
            else
                tone = muted;

            rule_begin(out, NULL, klass, "box", NULL);
            rule_color(out, "background", rgba_with_alpha(tone, 0.18));
            rule_color(out, "border", rgba_with_alpha(tone, 0.55));
            rule_number(out, "border-width", 1.0);
            rule_number(out, "radius", 999.0);

            /*
             * Bold-weight colour is not available, so a badge's text is
             * the tone mixed toward the text colour: a pale accent at
             * caption size on a dark row otherwise loses its horizontal
             * strokes (AGENTS.md, "A coloured caption at badge size").
             */
            rule_begin(out, NULL, klass, "label", NULL);
            rule_color(out, "foreground", rgba_mix(tone, text, 0.25));
        }
    }

    /* the unread count: solid accent, pulsing via its own animation */
    rule_begin(out, NULL, "clawt-unread", "box", NULL);
    rule_color(out, "background", accent);
    rule_number(out, "radius", 999.0);
    rule_color(out, "glow", rgba_with_alpha(accent, 0.5));
    rule_number(out, "glow-size", 10.0 * scale);

    rule_begin(out, NULL, "clawt-unread", "label", NULL);
    rule_color(out, "foreground", on_accent);

    /* a turn running: the row shimmers in the accent */
    rule_begin(out, NULL, "clawt-busy", "box", NULL);

    if (motion != CLAWT_LRG_MOTION_NONE) {
        g_string_append(out, "    effect: shimmer\n");
        g_string_append(out, "    effect-colors: [");
        append_color(out, rgba_with_alpha(surface, 0.0));
        g_string_append(out, ", ");
        append_color(out, rgba_with_alpha(accent, 0.22));
        g_string_append(out, "]\n");
        rule_number(out, "effect-intensity", 0.8);
    } else {
        rule_color(out, "border", rgba_with_alpha(accent, 0.6));
        rule_number(out, "border-width", 1.0);
    }

    /* the connection banner and the danger zone */
    rule_begin(out, NULL, "clawt-banner", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(warning, 0.22));
    rule_color(out, "border", rgba_with_alpha(warning, 0.7));
    rule_number(out, "border-width", 1.0);
    rule_number(out, "radius", 12.0 * scale);

    rule_begin(out, NULL, "clawt-banner-bad", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(danger, 0.22));
    rule_color(out, "border", rgba_with_alpha(danger, 0.7));

    /* the active section tab: a filled pill in the accent */
    rule_begin(out, NULL, "clawt-tab", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(surface, 0.0));
    rule_number(out, "radius", 999.0);

    rule_begin(out, NULL, "clawt-tab", "box", "hovered");
    rule_color(out, "background", rgba_with_alpha(text, 0.08));

    rule_begin(out, NULL, "clawt-tab-active", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(accent, 0.22));
    rule_color(out, "border", rgba_with_alpha(accent, 0.6));
    rule_number(out, "border-width", 1.0);
    rule_number(out, "radius", 999.0);

    rule_begin(out, NULL, "clawt-tab-active", "label", NULL);
    rule_color(out, "foreground", rgba_mix(accent, text, dark ? 0.55 : 0.15));

    /* a selected sidebar row: a soft accent wash with a left glow */
    rule_begin(out, NULL, "clawt-row", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(surface, 0.0));
    rule_number(out, "radius", 12.0 * scale);

    rule_begin(out, NULL, "clawt-row", "box", "hovered");
    rule_color(out, "background", rgba_with_alpha(text, 0.06));

    rule_begin(out, NULL, "clawt-row-selected", "box", NULL);
    rule_color(out, "background", rgba_with_alpha(accent, 0.2));
    rule_color(out, "glow", rgba_with_alpha(accent, 0.2));
    rule_number(out, "glow-size", 10.0 * scale);

    /* monospace output: the shell and code blocks */
    rule_begin(out, NULL, "clawt-terminal", "box", NULL);
    rule_color(out, "background", rgba_mix(window, black, dark ? 0.35 : 0.04));
    rule_number(out, "radius", 12.0 * scale);
    rule_color(out, "border", rgba_with_alpha(border, 0.8));
    rule_number(out, "border-width", 1.0);

    return g_string_free(out, FALSE);
}
