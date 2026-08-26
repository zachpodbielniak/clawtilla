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
 * clawt_appearance_palette_missing:
 * @css: (nullable): a palette stylesheet
 *
 * Which of libadwaita's colours @css does not define.
 *
 * A palette re-colours libadwaita by redefining its named colours, so
 * every name it leaves out keeps the stock GNOME value -- one widget in
 * the wrong grey in a window that is otherwise right, with nothing
 * anywhere to say why.  That is how the alerts panel came to be drawn in
 * `--secondary-sidebar-bg-color` while every palette in the tree had
 * never heard of it.
 *
 * Both dialects count: `@define-color window_bg_color ...` and
 * `--window-bg-color: ...` are the same statement, and a palette written
 * in either is complete.
 *
 * Returns: (transfer full) (array zero-terminated=1): the colours not
 *   named, empty when the palette is complete
 */
gchar **clawt_appearance_palette_missing(const gchar *css);

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
 * ClawtMeasureUnit:
 * @CLAWT_MEASURE_DEFAULT: follow the shipped column
 * @CLAWT_MEASURE_PERCENT: a share of the width the window has to give
 * @CLAWT_MEASURE_COLUMNS: a character count, whatever the font
 * @CLAWT_MEASURE_PIXELS: an absolute width
 *
 * What the measure's number means.
 *
 * It was pixels and only pixels, which is the one unit that is wrong on
 * every screen but the one it was typed on: a column that fits a laptop
 * leaves two thirds of a 4K panel empty, and the reader who wants the
 * conversation to use the display has no way to say so once rather than
 * once per machine.
 *
 * %CLAWT_MEASURE_PERCENT is the answer to that and is what the client
 * ships -- the column grows with the window, so there is nothing to
 * revisit when the window changes.  %CLAWT_MEASURE_COLUMNS is the
 * answer to the other half: typography's own unit is characters a line,
 * and a reader who wants 80 of them should not have to convert that to
 * pixels and then convert it again after changing their font.
 * %CLAWT_MEASURE_PIXELS stays because it is what an existing appearance
 * file says, and because somebody matching another window wants exactly
 * it.
 *
 * The unit travels *with* the number, in one self-describing spelling
 * (`90%`, `80ch`, `640px`), so a bare integer in a file written before
 * this still reads as the pixels it always was.  A separate `unit:` key
 * would have made an old file's 640 mean 640 percent the moment the new
 * key defaulted to anything but pixels.
 */
typedef enum {
    CLAWT_MEASURE_DEFAULT = 0,
    CLAWT_MEASURE_PERCENT,
    CLAWT_MEASURE_COLUMNS,
    CLAWT_MEASURE_PIXELS
} ClawtMeasureUnit;

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
 * CLAWT_APPEARANCE_MIN_PERCENT:
 * CLAWT_APPEARANCE_MAX_PERCENT:
 *
 * The bounds on a share of the available width.
 *
 * 100 is permitted and means it: a reader who wants the text edge to
 * edge may have it.  The floor is where a column stops being one.
 */
#define CLAWT_APPEARANCE_MIN_PERCENT 20
#define CLAWT_APPEARANCE_MAX_PERCENT 100

/**
 * CLAWT_APPEARANCE_MIN_COLUMNS:
 * CLAWT_APPEARANCE_MAX_COLUMNS:
 *
 * The bounds on a character count.  Continuous prose reads comfortably
 * at 45 to 90; the range is wider than that on purpose, because a
 * transcript is not only prose and somebody reading diffs wants more.
 */
#define CLAWT_APPEARANCE_MIN_COLUMNS 20
#define CLAWT_APPEARANCE_MAX_COLUMNS 240

/**
 * CLAWT_APPEARANCE_DEFAULT_PERCENT:
 *
 * The shipped column: nine tenths of whatever the window has to give.
 *
 * One number, read by both clients through
 * clawt_measure_resolve_px() and clawt_measure_to_css(), rather than a
 * pixel constant in the GTK client and a rem in the web sheet -- which
 * is what those two were, and they were not even the same column.
 *
 * It is deliberately not written into anybody's appearance file.  A
 * value stored there would freeze, so a later change to the shipped
 * column would reach nobody who had ever opened the dialog; that is the
 * same rule every other field here follows, one layer up.
 */
#define CLAWT_APPEARANCE_DEFAULT_PERCENT 90

/**
 * clawt_measure_unit_count:
 *
 * How many units a reader may choose between.
 *
 * Both clients build their control by walking this rather than naming
 * the units, for the reason the colour schemes already record: two
 * hand-written lists is how a value came to be selectable in one client
 * and not the other, with nothing to say so.
 *
 * Returns: the number of units
 */
guint clawt_measure_unit_count(void);

/**
 * clawt_measure_unit_nth:
 * @n: an index below clawt_measure_unit_count()
 *
 * Returns: the unit at @n, in the order a client should offer them
 */
ClawtMeasureUnit clawt_measure_unit_nth(guint n);

/**
 * clawt_measure_unit_nick:
 * @unit: a #ClawtMeasureUnit
 *
 * Returns: (transfer none): the stable spelling, never %NULL
 */
const gchar *clawt_measure_unit_nick(ClawtMeasureUnit unit);

/**
 * clawt_measure_unit_label:
 * @unit: a #ClawtMeasureUnit
 *
 * Returns: (transfer none): what to call it on screen, never %NULL
 */
const gchar *clawt_measure_unit_label(ClawtMeasureUnit unit);

/**
 * clawt_measure_unit_from_nick:
 * @nick: (nullable): a spelling from a form or a file
 *
 * An unrecognised nick is %CLAWT_MEASURE_DEFAULT rather than an error,
 * so a unit written by a newer build degrades to the shipped column.
 *
 * Returns: the unit
 */
ClawtMeasureUnit clawt_measure_unit_from_nick(const gchar *nick);

/**
 * clawt_measure_unit_min:
 * @unit: a #ClawtMeasureUnit
 *
 * Returns: the smallest amount @unit accepts, or 0 for
 *   %CLAWT_MEASURE_DEFAULT, which carries no amount
 */
gint clawt_measure_unit_min(ClawtMeasureUnit unit);

/**
 * clawt_measure_unit_max:
 * @unit: a #ClawtMeasureUnit
 *
 * Returns: the largest amount @unit accepts, or 0 for
 *   %CLAWT_MEASURE_DEFAULT
 */
gint clawt_measure_unit_max(ClawtMeasureUnit unit);

/**
 * clawt_measure_unit_preset:
 * @unit: a #ClawtMeasureUnit
 *
 * A sensible amount to start from when somebody picks @unit.
 *
 * Switching from pixels to characters wants a *character* count, not
 * the old pixel number clamped into range -- 640 pixels arriving as 240
 * characters is a column nobody chose and would have to be undone
 * before the control could be used.  So the amount is seeded from here
 * on a unit change rather than carried across.
 *
 * The percent preset is %CLAWT_APPEARANCE_DEFAULT_PERCENT, so picking
 * "share of the window" by hand lands on the shipped column exactly.
 *
 * Returns: the amount, or 0 for %CLAWT_MEASURE_DEFAULT
 */
gint clawt_measure_unit_preset(ClawtMeasureUnit unit);

/**
 * clawt_measure_unit_step:
 * @unit: a #ClawtMeasureUnit
 *
 * How far one step of a control moves the amount.  A pixel width wants
 * a coarser step than a percentage, and a client deciding that for
 * itself is one more thing the two could disagree about.
 *
 * Returns: the step, at least 1
 */
gint clawt_measure_unit_step(ClawtMeasureUnit unit);

/**
 * clawt_measure_parse:
 * @text: (nullable): a spelling such as `90%`, `80ch`, `640px` or `""`
 * @unit: (out): the unit read
 * @amount: (out): the amount read, clamped into @unit's range
 *
 * The one reader of a measure, used by the appearance file and by the
 * web client's cookie.  Two readers of a self-describing value is how
 * one of them comes to accept a spelling the other silently drops.
 *
 * A bare integer is pixels, because that is what a bare integer has
 * always meant in this file.  Anything unrecognised, empty or
 * non-positive is %CLAWT_MEASURE_DEFAULT with an amount of 0 -- a
 * measure nobody can read is the shipped column, never a refusal, since
 * this is a preference and not a fleet setting.
 *
 * Returns: %TRUE when @text named a unit and an amount this build
 *   understands, %FALSE when it fell back to the default
 */
gboolean clawt_measure_parse(const gchar      *text,
                             ClawtMeasureUnit *unit,
                             gint             *amount);

/**
 * clawt_measure_to_string:
 * @unit: a #ClawtMeasureUnit
 * @amount: the amount
 *
 * The inverse of clawt_measure_parse().
 *
 * Returns: (transfer full): the spelling, or an empty string for
 *   %CLAWT_MEASURE_DEFAULT
 */
gchar *clawt_measure_to_string(ClawtMeasureUnit unit, gint amount);

/**
 * clawt_measure_to_css:
 * @unit: a #ClawtMeasureUnit
 * @amount: the amount
 * @inset: (nullable): a CSS length expression for the leading inset a
 *   body takes inside the column, such as `var(--chat-gutter)`
 *
 * The measure as a CSS `max-width` value.
 *
 * `ch` is why this is worth having rather than emitting pixels: the
 * browser resolves a character count against the element's own font, so
 * a reader who then changes their font size keeps the column they asked
 * for.  @inset is added to it because a character count is about the
 * *text*, and a body starts a gutter in from the column's edge -- 80
 * columns that included the avatar gutter would be 74 characters of
 * words, and quietly wrong by a different amount at every font size.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL for
 *   %CLAWT_MEASURE_DEFAULT, which emits no declaration at all
 */
gchar *clawt_measure_to_css(ClawtMeasureUnit  unit,
                            gint              amount,
                            const gchar      *inset);

/**
 * clawt_measure_resolve_px:
 * @unit: a #ClawtMeasureUnit
 * @amount: the amount
 * @available: the width the column has to sit in, or 0 if not laid out
 *   yet
 * @char_width: the rendered advance of the digit zero, for
 *   %CLAWT_MEASURE_COLUMNS
 * @inset: the leading inset a body takes inside the column, in pixels
 *
 * The measure in pixels, for a toolkit that takes a number rather than
 * a stylesheet -- which is what AdwClamp does, and the reason this
 * exists beside clawt_measure_to_css() instead of one of the two
 * covering both clients.
 *
 * A percentage of nothing is not zero: with no allocation yet the
 * answer is the reference column, so a clamp built before its first
 * size-allocate holds something sensible rather than collapsing to a
 * sliver for one frame.
 *
 * @char_width is the digit zero on purpose, because that is what CSS
 * defines `ch` as and `ch` is what the web client renders a character
 * count with.  An *average* character width is the obvious alternative
 * and would have made one setting mean two different columns: measured
 * against real GTK4 in Adwaita Sans, Pango's average is 7.156px and its
 * zero is 9.253px, so "80 characters a line" would have come out 29%
 * narrower in the GTK client than in the browser, with nothing anywhere
 * to say so.
 *
 * The result is floored at %CLAWT_APPEARANCE_MIN_MEASURE but never
 * capped above @available, and there is deliberately no upper bound: a
 * reader who asked for nine tenths of a very wide display asked for
 * exactly that, and clamping it back would be a control that reports a
 * setting it did not apply.
 *
 * Returns: the column width in pixels, always positive
 */
gint clawt_measure_resolve_px(ClawtMeasureUnit unit,
                              gint             amount,
                              gint             available,
                              gdouble          char_width,
                              gint             inset);

/**
 * CLAWT_APPEARANCE_MAX_RUN_SPACING:
 *
 * The largest gap between runs, in pixels.  No lower bound beyond zero,
 * which means "unset" -- a person who wants runs touching may have them.
 */
#define CLAWT_APPEARANCE_MAX_RUN_SPACING 96

/**
 * clawt_appearance_get_measure_unit:
 * @self: a #ClawtAppearance
 *
 * What the transcript column is expressed in.
 *
 * The measure was a constant in C, so the one thing about the reading
 * experience most likely to be wrong for a given person and screen was
 * the one thing they could not change.  Whatever number is chosen is
 * right for one reader on one display: a measure the reader can adjust
 * is strictly better than a measure that is correct for whoever picked
 * it.
 *
 * %CLAWT_MEASURE_DEFAULT means defer, exactly as an unset font does,
 * and for the same reason -- a value naming the current default would
 * freeze it, so a later change to the shipped measure would not reach
 * anyone who had ever opened this dialog.
 *
 * Returns: the unit
 */
ClawtMeasureUnit clawt_appearance_get_measure_unit(ClawtAppearance *self);

/**
 * clawt_appearance_get_measure_amount:
 * @self: a #ClawtAppearance
 *
 * The number the unit applies to, or 0 when the unit is
 * %CLAWT_MEASURE_DEFAULT.
 *
 * Meaningless on its own: 90 is nine tenths of the window or ninety
 * pixels depending on the unit beside it, which is why the two are
 * never stored apart and never written apart.
 *
 * Returns: the amount
 */
gint clawt_appearance_get_measure_amount(ClawtAppearance *self);

/**
 * clawt_appearance_set_measure:
 * @self: a #ClawtAppearance
 * @unit: a #ClawtMeasureUnit
 * @amount: the amount, clamped into @unit's range
 *
 * An @amount at or below zero is %CLAWT_MEASURE_DEFAULT whatever @unit
 * said, so "clear it" needs no separate call and a spin button that can
 * reach 0 means what a reader expects it to.
 */
void clawt_appearance_set_measure(ClawtAppearance *self,
                                  ClawtMeasureUnit unit,
                                  gint             amount);

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
