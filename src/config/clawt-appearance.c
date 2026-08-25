/*
 * clawt-appearance.c - How the client looks, on this machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-appearance.h"

#include <yaml-glib.h>

/*
 * A size nobody would type on purpose, either way round.  A file edited
 * by hand -- which this one invites, being three lines of YAML -- can
 * carry a 2 or a 2000, and a client that honoured either is a client
 * whose settings dialog has become unreadable and cannot be reopened to
 * fix itself.
 */
#define MIN_FONT_POINTS (6.0)
#define MAX_FONT_POINTS (48.0)

struct _ClawtAppearance {
    ClawtTheme theme;
    gchar     *font;
    gdouble    font_size;
    gchar     *monospace_font;
    gdouble    monospace_size;
};

G_DEFINE_BOXED_TYPE(ClawtAppearance, clawt_appearance, clawt_appearance_copy,
                    clawt_appearance_free)

ClawtAppearance *
clawt_appearance_new(void)
{
    /*
     * Zeroed is the same thing as "defer to the system" throughout:
     * CLAWT_THEME_SYSTEM is 0, an unset family is NULL and an unset size
     * is 0.  That is on purpose -- it means a field added later defaults
     * to deferring rather than to whatever the author had in mind.
     */
    return g_new0(ClawtAppearance, 1);
}

ClawtAppearance *
clawt_appearance_copy(ClawtAppearance *self)
{
    ClawtAppearance *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtAppearance, 1);
    copy->theme = self->theme;
    copy->font = g_strdup(self->font);
    copy->font_size = self->font_size;
    copy->monospace_font = g_strdup(self->monospace_font);
    copy->monospace_size = self->monospace_size;

    return copy;
}

void
clawt_appearance_free(ClawtAppearance *self)
{
    if (self == NULL)
        return;

    g_free(self->font);
    g_free(self->monospace_font);
    g_free(self);
}

ClawtTheme
clawt_appearance_get_theme(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_THEME_SYSTEM);

    return self->theme;
}

void
clawt_appearance_set_theme(ClawtAppearance *self, ClawtTheme theme)
{
    g_return_if_fail(self != NULL);

    self->theme = theme;
}

const gchar *
clawt_appearance_get_font(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->font;
}

/*
 * An empty family is stored as NULL rather than "".
 *
 * They mean the same thing to a person clearing the field, and keeping
 * both would put `font: ''` in the file and a `font-family: ;` in the
 * CSS -- a rule that is not merely useless but invalid, and GTK drops the
 * whole block it appears in.
 */
static void
set_family(gchar **slot, const gchar *family)
{
    g_free(*slot);
    *slot = (family != NULL && *family != '\0') ? g_strdup(family) : NULL;

    if (*slot != NULL)
        g_strstrip(*slot);

    if (*slot != NULL && **slot == '\0')
        g_clear_pointer(slot, g_free);
}

void
clawt_appearance_set_font(ClawtAppearance *self, const gchar *family)
{
    g_return_if_fail(self != NULL);

    set_family(&self->font, family);
}

gdouble
clawt_appearance_get_font_size(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, 0.0);

    return self->font_size;
}

/*
 * Out of range is clamped rather than refused.
 *
 * This is reached from a file somebody may have edited, and from a spin
 * button; refusing would mean either a dialog that will not close or a
 * silent revert to the system size, which reads as the setting not
 * working. Zero stays zero -- that is "unset", not "very small".
 */
static gdouble
clamp_points(gdouble points)
{
    if (points <= 0.0)
        return 0.0;

    return CLAMP(points, MIN_FONT_POINTS, MAX_FONT_POINTS);
}

void
clawt_appearance_set_font_size(ClawtAppearance *self, gdouble points)
{
    g_return_if_fail(self != NULL);

    self->font_size = clamp_points(points);
}

const gchar *
clawt_appearance_get_monospace_font(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->monospace_font;
}

void
clawt_appearance_set_monospace_font(ClawtAppearance *self,
                                    const gchar *family)
{
    g_return_if_fail(self != NULL);

    set_family(&self->monospace_font, family);
}

gdouble
clawt_appearance_get_monospace_size(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, 0.0);

    return self->monospace_size;
}

void
clawt_appearance_set_monospace_size(ClawtAppearance *self, gdouble points)
{
    g_return_if_fail(self != NULL);

    self->monospace_size = clamp_points(points);
}

/* ── Themes ──────────────────────────────────────────────────────── */

/*
 * Catppuccin Mocha, as libadwaita's own named colours.
 *
 * Named colours rather than per-widget rules: libadwaita builds its
 * whole stylesheet out of these, so redefining them re-colours widgets
 * this client has never heard of, including the ones libadwaita adds
 * next release.  A sheet of `.headerbar { background: ... }` rules would
 * cover exactly the widgets somebody remembered.
 *
 * The full set rather than the handful that are obviously wrong without
 * it.  Every name left undefined keeps libadwaita's dark value, and its
 * dark greys are not this palette's -- the gaps show up as a card or a
 * popover that is the wrong grey next to everything around it.  The
 * foreground colours matter for the same reason in reverse: libadwaita's
 * accent_fg_color is white, which on Catppuccin blue is unreadable.
 *
 * Palette (Mocha): base #1e1e2e, mantle #181825, crust #11111b,
 * surface0 #313244, text #cdd6f4, blue #89b4fa, lavender #b4befe,
 * green #a6e3a1, yellow #f9e2af, red #f38ba8.
 *
 * Catppuccin is MIT licensed, so the values are written here rather than
 * vendored -- ten lines of hex in the file that uses them beats a data
 * file and a loader for something that never changes at runtime.
 */
static const gchar catppuccin_mocha_css[] =
    "@define-color window_bg_color #1e1e2e;\n"
    "@define-color window_fg_color #cdd6f4;\n"
    "@define-color view_bg_color #181825;\n"
    "@define-color view_fg_color #cdd6f4;\n"
    "@define-color headerbar_bg_color #181825;\n"
    "@define-color headerbar_fg_color #cdd6f4;\n"
    "@define-color headerbar_border_color #cdd6f4;\n"
    "@define-color headerbar_backdrop_color #11111b;\n"
    "@define-color headerbar_shade_color #11111b;\n"
    "@define-color sidebar_bg_color #181825;\n"
    "@define-color sidebar_fg_color #cdd6f4;\n"
    "@define-color sidebar_backdrop_color #11111b;\n"
    "@define-color sidebar_shade_color #11111b;\n"
    "@define-color card_bg_color #313244;\n"
    "@define-color card_fg_color #cdd6f4;\n"
    "@define-color card_shade_color #11111b;\n"
    "@define-color dialog_bg_color #313244;\n"
    "@define-color dialog_fg_color #cdd6f4;\n"
    "@define-color popover_bg_color #313244;\n"
    "@define-color popover_fg_color #cdd6f4;\n"
    "@define-color accent_bg_color #89b4fa;\n"
    "@define-color accent_fg_color #11111b;\n"
    "@define-color accent_color #b4befe;\n"
    "@define-color destructive_bg_color #f38ba8;\n"
    "@define-color destructive_fg_color #11111b;\n"
    "@define-color destructive_color #f38ba8;\n"
    "@define-color error_bg_color #f38ba8;\n"
    "@define-color error_fg_color #11111b;\n"
    "@define-color error_color #f38ba8;\n"
    "@define-color success_bg_color #a6e3a1;\n"
    "@define-color success_fg_color #11111b;\n"
    "@define-color success_color #a6e3a1;\n"
    "@define-color warning_bg_color #f9e2af;\n"
    "@define-color warning_fg_color #11111b;\n"
    "@define-color warning_color #f9e2af;\n"
    "@define-color shade_color #11111b;\n"
    "@define-color scrollbar_outline_color #11111b;\n";

/*
 * One row per colour scheme the client offers.
 *
 * @css is %NULL for the three that are not palettes: they ask
 * libadwaita for a scheme and take its colours, which is the whole point
 * of them.  @dark is what a palette needs underneath it and is not
 * consulted for CLAWT_THEME_SYSTEM, whose answer is the desktop's.
 */
typedef struct {
    ClawtTheme   theme;
    const gchar *nick;
    const gchar *label;
    gboolean     dark;
    const gchar *css;
} ThemeInfo;

/*
 * @label is here rather than in each client because both of them offer
 * this same list, and they had a hand-written copy each: the GTK combo
 * named four schemes and the web select named three, so the palette
 * added to this file reached one client and not the other.  `make
 * parity` could not see it -- a colour scheme sends no IPC frame and is
 * no slash command -- which is the blind spot that check already has
 * recorded against it.  One list, read by both.
 */
static const ThemeInfo theme_table[] = {
    { CLAWT_THEME_SYSTEM,           "system",           "Follow the system",
      FALSE, NULL },
    { CLAWT_THEME_LIGHT,            "light",            "Light",
      FALSE, NULL },
    { CLAWT_THEME_DARK,             "dark",             "Dark",
      TRUE,  NULL },
    { CLAWT_THEME_CATPPUCCIN_MOCHA, "catppuccin-mocha", "Catppuccin Mocha",
      TRUE,  catppuccin_mocha_css }
};

static const ThemeInfo *
theme_info(ClawtTheme theme)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(theme_table); i++) {
        if (theme_table[i].theme == theme)
            return &theme_table[i];
    }

    return NULL;
}

guint
clawt_appearance_theme_count(void)
{
    return (guint)G_N_ELEMENTS(theme_table);
}

ClawtTheme
clawt_appearance_theme_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(theme_table), CLAWT_THEME_SYSTEM);

    return theme_table[n].theme;
}

const gchar *
clawt_appearance_theme_nick(ClawtTheme theme)
{
    const ThemeInfo *info = theme_info(theme);

    return (info != NULL) ? info->nick : "system";
}

const gchar *
clawt_appearance_theme_label(ClawtTheme theme)
{
    const ThemeInfo *info = theme_info(theme);

    return (info != NULL) ? info->label : "Follow the system";
}

ClawtTheme
clawt_appearance_theme_from_nick(const gchar *nick)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(theme_table); i++) {
        if (g_strcmp0(nick, theme_table[i].nick) == 0)
            return theme_table[i].theme;
    }

    return CLAWT_THEME_SYSTEM;
}

gboolean
clawt_appearance_theme_has_palette(ClawtTheme theme)
{
    const ThemeInfo *info = theme_info(theme);

    return info != NULL && info->css != NULL;
}

gboolean
clawt_appearance_theme_is_dark(ClawtTheme theme)
{
    const ThemeInfo *info = theme_info(theme);

    return info != NULL && info->dark;
}

/* ── CSS ─────────────────────────────────────────────────────────── */

/*
 * A family as CSS.
 *
 * Quoted, because a font family is arbitrary text -- "DejaVu Sans Mono"
 * has spaces and plenty have digits or punctuation.
 *
 * Then reduced to what a font name can actually contain.  A quote would
 * close the string early and hand the rest of the family to the CSS
 * parser as syntax; the braces and semicolons after it are what that
 * syntax would be made of.  Dropping all of them means the worst a
 * strange name can do is name a font nobody has, which shows up as the
 * default font rather than as a stylesheet doing something else.
 *
 * Not escaping instead: CSS string escapes are their own small language,
 * and there is nothing to preserve here.  No font on any system has a
 * brace in its name.
 */
static void
append_family(GString *out, const gchar *family)
{
    const gchar *p;

    g_string_append_c(out, '"');

    for (p = family; *p != '\0'; p++) {
        switch (*p) {
        case '"':
        case '\'':
        case '\\':
        case '{':
        case '}':
        case ';':
        case '<':
        case '>':
        case '\n':
        case '\r':
            break;
        default:
            g_string_append_c(out, *p);
            break;
        }
    }

    g_string_append_c(out, '"');
}

gchar *
clawt_appearance_to_css(ClawtAppearance *self)
{
    GString *out;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_string_new(NULL);

    /*
     * The palette first, so a colour is defined before anything could
     * refer to it, and so a person reading the sheet sees what the theme
     * is before how big it is.
     *
     * Conditional, like everything else here: the three schemes that are
     * not palettes emit nothing, which is what keeps a fresh appearance
     * producing an empty stylesheet.  A sheet that named the current
     * default would freeze it.
     */
    if (clawt_appearance_theme_has_palette(self->theme))
        g_string_append(out, theme_info(self->theme)->css);

    /*
     * Nothing at all when nothing is set, rather than a sheet full of
     * `inherit`.  An empty provider still overrides nothing, but an
     * explicit rule naming the current default would freeze it -- so a
     * person who later changed their desktop font would find this app
     * alone ignoring it.
     */
    if (self->font != NULL || self->font_size > 0.0) {
        g_string_append(out, "window, popover, dialog {\n");

        if (self->font != NULL) {
            g_string_append(out, "  font-family: ");
            append_family(out, self->font);
            g_string_append(out, ";\n");
        }

        if (self->font_size > 0.0)
            g_string_append_printf(out, "  font-size: %gpt;\n",
                                   self->font_size);

        g_string_append(out, "}\n");
    }

    /*
     * The console and anything tagged monospace.  GtkTextView's
     * :monospace property renders through the .monospace class, which is
     * what makes one rule cover both the exec output and any code view
     * added later.
     */
    if (self->monospace_font != NULL || self->monospace_size > 0.0) {
        g_string_append(out, ".monospace, textview.monospace {\n");

        if (self->monospace_font != NULL) {
            g_string_append(out, "  font-family: ");
            append_family(out, self->monospace_font);
            g_string_append(out, ";\n");
        }

        if (self->monospace_size > 0.0)
            g_string_append_printf(out, "  font-size: %gpt;\n",
                                   self->monospace_size);

        g_string_append(out, "}\n");
    }

    return g_string_free(out, FALSE);
}

/* ── The file ────────────────────────────────────────────────────── */

gchar *
clawt_appearance_default_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "clawtilla",
                            "appearance.yaml", NULL);
}

static const gchar *
theme_to_nick(ClawtTheme theme)
{
    return clawt_appearance_theme_nick(theme);
}

/*
 * An unrecognised theme is "system", not an error.
 *
 * A newer build may write one this one has never heard of, and refusing
 * to load the file would take the fonts down with it -- the same
 * forward-compatibility rule shadow agents exist for.  It is also what
 * makes a palette safe to add: an older build reading `catppuccin-mocha`
 * gets the desktop's own scheme rather than a parse error.
 */
static ClawtTheme
theme_from_nick(const gchar *nick)
{
    return clawt_appearance_theme_from_nick(nick);
}

static const gchar *
member_string(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node;

    if (mapping == NULL || key == NULL)
        return NULL;

    node = yaml_mapping_get_member(mapping, key);

    if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
        return NULL;

    return yaml_node_get_string(node);
}

ClawtAppearance *
clawt_appearance_parse(const gchar *text, GError **error)
{
    g_autoptr(YamlParser) parser = NULL;
    ClawtAppearance *self;
    YamlNode *root;
    YamlMapping *mapping;
    const gchar *value;

    g_return_val_if_fail(text != NULL, NULL);

    self = clawt_appearance_new();

    if (*text == '\0')
        return self;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, text, -1, error)) {
        clawt_appearance_free(self);
        return NULL;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return self;

    mapping = yaml_node_get_mapping(root);

    self->theme = theme_from_nick(member_string(mapping, "theme"));

    clawt_appearance_set_font(self, member_string(mapping, "font"));
    clawt_appearance_set_monospace_font(
        self, member_string(mapping, "monospace_font"));

    value = member_string(mapping, "font_size");

    if (value != NULL)
        clawt_appearance_set_font_size(self, g_ascii_strtod(value, NULL));

    value = member_string(mapping, "monospace_size");

    if (value != NULL)
        clawt_appearance_set_monospace_size(self,
                                            g_ascii_strtod(value, NULL));

    return self;
}

/*
 * Single-quoted, whose only escape is a doubled quote -- so a family with
 * a backslash in it survives without an unescape step to get wrong.  Same
 * reasoning as the connections file.
 */
static void
append_quoted(GString *out, const gchar *key, const gchar *value)
{
    const gchar *p;

    g_string_append_printf(out, "%s: '", key);

    for (p = value; *p != '\0'; p++) {
        if (*p == '\'')
            g_string_append(out, "''");
        else
            g_string_append_c(out, *p);
    }

    g_string_append(out, "'\n");
}

gchar *
clawt_appearance_to_data(ClawtAppearance *self)
{
    GString *out;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_string_new(
        "# clawtilla appearance\n"
        "#\n"
        "# How the clients look on this machine. Written by the Appearance\n"
        "# page in clawtilla-gtk's settings; edit it by hand if you prefer.\n"
        "#\n"
        "# This is deliberately not part of clawtilla.yaml. The client can\n"
        "# switch between daemons while it runs, and fonts that came from a\n"
        "# daemon's config would change when you connected to another\n"
        "# machine. Sizes are per-screen too.\n"
        "#\n"
        "# An empty font or a size of 0 means: use whatever the desktop\n"
        "# says. That is the default, and it is not the same as naming the\n"
        "# font the desktop currently uses -- a named one would stop\n"
        "# following it.\n"
        "\n");

    append_quoted(out, "theme", theme_to_nick(self->theme));

    if (self->font != NULL)
        append_quoted(out, "font", self->font);
    else
        g_string_append(out, "font: ''\n");

    g_string_append_printf(out, "font_size: %g\n", self->font_size);

    if (self->monospace_font != NULL)
        append_quoted(out, "monospace_font", self->monospace_font);
    else
        g_string_append(out, "monospace_font: ''\n");

    g_string_append_printf(out, "monospace_size: %g\n",
                           self->monospace_size);

    return g_string_free(out, FALSE);
}

ClawtAppearance *
clawt_appearance_load(const gchar *path, GError **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *text = NULL;

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_appearance_default_path();

    if (!g_file_test(resolved, G_FILE_TEST_EXISTS))
        return clawt_appearance_new();

    if (!g_file_get_contents(resolved, &text, NULL, error)) {
        g_prefix_error(error, "%s: ", resolved);
        return NULL;
    }

    return clawt_appearance_parse(text, error);
}

gboolean
clawt_appearance_save(ClawtAppearance *self, const gchar *path,
                      GError **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *text = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_appearance_default_path();
    dir = g_path_get_dirname(resolved);

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    text = clawt_appearance_to_data(self);

    /*
     * 0644, unlike the connections file beside it.  There is no secret
     * here, and a font choice that could not be read by a script the
     * person wrote themselves would be an odd thing to lock down.
     */
    return clawt_write_file_atomic(resolved, text, -1, 0644, FALSE, error);
}
