/*
 * clawt-appearance.h - How the client looks, on this machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These live in the *client's* config rather than in clawtilla.yaml, for
 * the same reason connection profiles do -- and the reason is sharper
 * here, because the client can switch daemons while it is running.  Fonts
 * that came from the daemon's config would change when you connected to
 * a workstation and change back when you went home, which is nobody's
 * idea of a preference.
 *
 * They are also per-machine by nature: the size that is right on a
 * laptop is not the size that is right on a 4K panel, and the same
 * clawtilla.yaml is often the same fleet seen from both.
 *
 * Every value has an "unset" that means *defer to the system* rather
 * than a hardcoded fallback -- an empty font name, a zero size,
 * CLAWT_THEME_SYSTEM.  A client that shipped its own idea of a font
 * would override the one the person chose for their desktop, on a
 * machine where they had never opened this dialog.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtTheme:
 * @CLAWT_THEME_SYSTEM: follow the desktop's light/dark preference
 * @CLAWT_THEME_LIGHT: always light
 * @CLAWT_THEME_DARK: always dark
 * @CLAWT_THEME_CATPPUCCIN_MOCHA: the Catppuccin Mocha palette, dark
 *
 * Which colour scheme the client asks libadwaita for.
 *
 * A named palette is one more answer to that same question rather than a
 * second setting beside it.  The alternative -- a `palette` string next
 * to the enum -- immediately raises what `theme: light` plus
 * `palette: mocha` is supposed to mean, and there is no good answer:
 * every palette is drawn for one scheme and is wrong over the other.
 * Keeping one axis means that state cannot be written down.
 *
 * The enum does not grow a case per palette anywhere it is used: the
 * colours live in a table in clawt-appearance.c and everything else asks
 * clawt_appearance_theme_has_palette() and
 * clawt_appearance_theme_is_dark().  Another palette is one enum value
 * and one row.
 */
typedef enum {
    CLAWT_THEME_SYSTEM = 0,
    CLAWT_THEME_LIGHT,
    CLAWT_THEME_DARK,
    CLAWT_THEME_CATPPUCCIN_MOCHA
} ClawtTheme;

#define CLAWT_TYPE_APPEARANCE (clawt_appearance_get_type())

typedef struct _ClawtAppearance ClawtAppearance;

GType clawt_appearance_get_type(void) G_GNUC_CONST;

/**
 * clawt_appearance_new:
 *
 * Appearance that defers to the system in every respect.
 *
 * Returns: (transfer full): a new #ClawtAppearance
 */
ClawtAppearance *clawt_appearance_new(void);

ClawtAppearance *clawt_appearance_copy(ClawtAppearance *self);
void             clawt_appearance_free(ClawtAppearance *self);

ClawtTheme   clawt_appearance_get_theme(ClawtAppearance *self);
void         clawt_appearance_set_theme(ClawtAppearance *self,
                                        ClawtTheme       theme);

/**
 * clawt_appearance_theme_has_palette:
 * @theme: a #ClawtTheme
 *
 * Whether @theme brings colours of its own, as opposed to only asking
 * libadwaita for a light or dark scheme.
 *
 * Returns: %TRUE for a named palette
 */
/**
 * clawt_appearance_theme_count:
 *
 * How many colour schemes the library offers.
 *
 * A client builds its own control from this rather than from a list of
 * its own.  Two copies is how the GTK combo came to name four schemes
 * while the web select named three, with nothing to say so: a colour
 * scheme sends no IPC frame and is no slash command, so `make parity`
 * looks straight past it.
 *
 * Returns: the number of schemes
 */
guint        clawt_appearance_theme_count(void);

/**
 * clawt_appearance_theme_nth:
 * @n: an index below clawt_appearance_theme_count()
 *
 * Returns: the scheme at @n, in the order a client should offer them
 */
ClawtTheme   clawt_appearance_theme_nth(guint n);

/**
 * clawt_appearance_theme_nick:
 * @theme: a #ClawtTheme
 *
 * The stable spelling, as written to disk and to a cookie.
 *
 * Returns: (transfer none): the nick, never %NULL
 */
const gchar *clawt_appearance_theme_nick(ClawtTheme theme);

/**
 * clawt_appearance_theme_label:
 * @theme: a #ClawtTheme
 *
 * What to call it on screen.
 *
 * Returns: (transfer none): the label, never %NULL
 */
const gchar *clawt_appearance_theme_label(ClawtTheme theme);

/**
 * clawt_appearance_theme_from_nick:
 * @nick: (nullable): a spelling from disk, a cookie or a form
 *
 * An unrecognised nick is %CLAWT_THEME_SYSTEM rather than an error, so a
 * palette written by a newer build degrades to the desktop's own scheme
 * instead of failing to load.
 *
 * Returns: the scheme
 */
ClawtTheme   clawt_appearance_theme_from_nick(const gchar *nick);

gboolean     clawt_appearance_theme_has_palette(ClawtTheme theme);

/**
 * clawt_appearance_theme_is_dark:
 * @theme: a #ClawtTheme
 *
 * Which scheme @theme needs underneath it.  A palette is drawn for one
 * of the two, and over the other one libadwaita's own colours fight it
 * on every widget the palette does not name.
 *
 * Meaningless for %CLAWT_THEME_SYSTEM, which has no answer -- the
 * desktop's preference is the answer, and the caller already has it.
 *
 * Returns: %TRUE if @theme needs the dark scheme
 */
gboolean     clawt_appearance_theme_is_dark(ClawtTheme theme);

/**
 * clawt_appearance_get_font:
 * @self: a #ClawtAppearance
 *
 * Returns: (nullable): the interface font family, or %NULL for the
 *   system's
 */
const gchar *clawt_appearance_get_font(ClawtAppearance *self);
void         clawt_appearance_set_font(ClawtAppearance *self,
                                       const gchar     *family);

/**
 * clawt_appearance_get_font_size:
 * @self: a #ClawtAppearance
 *
 * Returns: the interface font size in points, or 0 for the system's
 */
gdouble clawt_appearance_get_font_size(ClawtAppearance *self);
void    clawt_appearance_set_font_size(ClawtAppearance *self, gdouble points);

/**
 * clawt_appearance_get_monospace_font:
 * @self: a #ClawtAppearance
 *
 * The font for code blocks, inline code and the exec console.
 *
 * Returns: (nullable): the family, or %NULL for the system's monospace
 */
const gchar *clawt_appearance_get_monospace_font(ClawtAppearance *self);
void         clawt_appearance_set_monospace_font(ClawtAppearance *self,
                                                 const gchar     *family);

gdouble clawt_appearance_get_monospace_size(ClawtAppearance *self);
void    clawt_appearance_set_monospace_size(ClawtAppearance *self,
                                            gdouble          points);

/**
 * clawt_appearance_palette_dir:
 *
 * Where palettes live: `$XDG_CONFIG_HOME/clawtilla/palettes`.
 *
 * A palette used to be a fourth value of an enum, which meant adding
 * one was a code change and a rebuild, and adjusting one you mostly
 * liked was not possible at all.  Every other part of the appearance
 * had become configuration; colour was still the exception, and it is
 * the part people most want to touch.
 *
 * Returns: (transfer full): the directory, which need not exist
 */
gchar *clawt_appearance_palette_dir(void);

/**
 * clawt_appearance_reload_palettes:
 *
 * Rescans the palette directory.
 *
 * Discovery is cached, because both clients walk the scheme list every
 * time they build a settings page and a readdir per keystroke would be
 * absurd.  Called once at startup; call it again after writing a
 * palette if you want it to appear without a restart.
 *
 * Returns: how many palettes were found
 */
guint clawt_appearance_reload_palettes(void);

/**
 * clawt_appearance_scheme_count:
 *
 * How many colour schemes there are: the built-in modes, plus every
 * palette found on disk.
 *
 * This is the list both clients build their control from.  It is keyed
 * by *nick* rather than by #ClawtTheme because a palette read from a
 * file has no enum value -- and the nick is what was already stored in
 * the appearance file and in the web client's cookie, so nothing about
 * how a choice is remembered had to change.
 *
 * Returns: the number of schemes
 */
guint clawt_appearance_scheme_count(void);

/**
 * clawt_appearance_scheme_nth_nick:
 * @n: an index below clawt_appearance_scheme_count()
 *
 * Returns: (transfer none): the nick, stable for the process
 */
const gchar *clawt_appearance_scheme_nth_nick(guint n);

/**
 * clawt_appearance_scheme_nth_label:
 * @n: an index below clawt_appearance_scheme_count()
 *
 * Returns: (transfer none): what to show a person
 */
const gchar *clawt_appearance_scheme_nth_label(guint n);

/**
 * clawt_appearance_get_scheme:
 * @self: a #ClawtAppearance
 *
 * The chosen scheme's nick, whether it is built in or from a file.
 *
 * Returns: (transfer none): the nick, never %NULL
 */
const gchar *clawt_appearance_get_scheme(ClawtAppearance *self);

/**
 * clawt_appearance_set_scheme:
 * @self: a #ClawtAppearance
 * @nick: (nullable): a nick from the scheme list
 *
 * A nick this build does not know becomes "system" rather than an
 * error -- the same answer a config naming a palette that has since
 * been deleted gets.  Somebody who removes a palette file should find
 * the client following their desktop again, not refusing to start.
 */
void clawt_appearance_set_scheme(ClawtAppearance *self, const gchar *nick);

/**
 * clawt_appearance_get_palette_css:
 * @self: a #ClawtAppearance
 *
 * The stylesheet of a palette read from a file, or %NULL when the
 * chosen scheme is one of the built-in ones.
 *
 * Separate from clawt_appearance_to_css() because the two clients need
 * it at different moments: the GTK client folds it into the one sheet
 * it loads, and the web client emits it as its own block before the
 * reader's overrides so those still win on a specificity tie.
 *
 * Returns: (transfer none) (nullable): the sheet
 */
const gchar *clawt_appearance_get_palette_css(ClawtAppearance *self);

/**
 * CLAWT_APPEARANCE_MIN_MEASURE:
 * CLAWT_APPEARANCE_MAX_MEASURE:
 *
 * The bounds a transcript column is clamped to, in pixels.
 *
 * This file invites hand-editing, and a 40 or a 4000 in it would make
 * the conversation unreadable and leave no obvious way back -- the same
 * reason font sizes are clamped rather than refused.  The lower bound is
 * about twenty characters at an ordinary size; the upper is past the
 * point where a wider column stops being a column.
 */
#define CLAWT_APPEARANCE_MIN_MEASURE 320
#define CLAWT_APPEARANCE_MAX_MEASURE 1600

/**
 * CLAWT_APPEARANCE_MAX_RUN_SPACING:
 *
 * The largest gap between runs, in pixels.  No lower bound beyond zero,
 * which means "unset" -- a person who wants runs touching may have them.
 */
#define CLAWT_APPEARANCE_MAX_RUN_SPACING 96

/**
 * clawt_appearance_get_measure:
 * @self: a #ClawtAppearance
 *
 * How wide the transcript column is, in pixels, or 0 to follow the
 * shipped value.
 *
 * The measure was a constant in C, so the one thing about the reading
 * experience most likely to be wrong for a given person and screen was
 * the one thing they could not change.  Whatever number is chosen is
 * right for one reader on one display: a measure the reader can adjust
 * is strictly better than a measure that is correct for whoever picked
 * it.
 *
 * Zero means defer, exactly as an unset font does, and for the same
 * reason -- a value naming the current default would freeze it, so a
 * later change to the shipped measure would not reach anyone who had
 * ever opened this dialog.
 *
 * Returns: the width in pixels, or 0
 */
gint clawt_appearance_get_measure(ClawtAppearance *self);
void clawt_appearance_set_measure(ClawtAppearance *self, gint pixels);

/**
 * clawt_appearance_get_run_spacing:
 * @self: a #ClawtAppearance
 *
 * The gap between one run of messages and the next, in pixels, or 0 to
 * follow the shipped value.
 *
 * Separate from the measure because they answer different complaints.
 * A column too wide is tiring to read; runs too close together stop
 * reading as separate turns, which is the whole point of grouping them.
 *
 * Returns: the gap in pixels, or 0
 */
gint clawt_appearance_get_run_spacing(ClawtAppearance *self);
void clawt_appearance_set_run_spacing(ClawtAppearance *self, gint pixels);

/**
 * clawt_appearance_to_css:
 * @self: a #ClawtAppearance
 *
 * The stylesheet that applies these fonts.
 *
 * Emitted rather than applied so it can be asserted on without a display
 * -- which is the only way to check that an unset value produces *no*
 * rule at all, and therefore that the desktop's own font still wins.
 *
 * Returns: (transfer full): CSS, empty when nothing is set
 */
gchar *clawt_appearance_to_css(ClawtAppearance *self);

/**
 * clawt_appearance_default_path:
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/appearance.yaml`
 */
gchar *clawt_appearance_default_path(void);

/**
 * clawt_appearance_parse:
 * @text: the contents of an appearance file
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the settings, or %NULL on a parse
 *   error
 */
ClawtAppearance *clawt_appearance_parse(const gchar *text, GError **error);

/**
 * clawt_appearance_to_data:
 * @self: a #ClawtAppearance
 *
 * Returns: (transfer full): the file contents
 */
gchar *clawt_appearance_to_data(ClawtAppearance *self);

/**
 * clawt_appearance_load:
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * A missing file is the defaults rather than an error: never having
 * opened the dialog is the ordinary state.
 *
 * Returns: (transfer full) (nullable): the settings, or %NULL if the file
 *   exists and could not be read
 */
ClawtAppearance *clawt_appearance_load(const gchar *path, GError **error);

/**
 * clawt_appearance_save:
 * @self: a #ClawtAppearance
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if written
 */
gboolean clawt_appearance_save(ClawtAppearance *self,
                               const gchar     *path,
                               GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtAppearance, clawt_appearance_free)

G_END_DECLS
