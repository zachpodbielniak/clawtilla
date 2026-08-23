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
    switch (theme) {
    case CLAWT_THEME_LIGHT:
        return "light";
    case CLAWT_THEME_DARK:
        return "dark";
    case CLAWT_THEME_SYSTEM:
    default:
        return "system";
    }
}

/*
 * An unrecognised theme is "system", not an error.
 *
 * A newer build may write one this one has never heard of, and refusing
 * to load the file would take the fonts down with it -- the same
 * forward-compatibility rule shadow agents exist for.
 */
static ClawtTheme
theme_from_nick(const gchar *nick)
{
    if (g_strcmp0(nick, "light") == 0)
        return CLAWT_THEME_LIGHT;

    if (g_strcmp0(nick, "dark") == 0)
        return CLAWT_THEME_DARK;

    return CLAWT_THEME_SYSTEM;
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
