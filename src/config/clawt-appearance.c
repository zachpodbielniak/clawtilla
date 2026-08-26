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

    /*
     * CLAWT_MEASURE_DEFAULT means defer to the shipped column, like
     * every other field here.  Naming the current default instead would
     * freeze it.
     *
     * The unit is stored beside the amount rather than being implied,
     * because the two together are the setting: 90 is nine tenths of
     * the window, ninety characters or ninety pixels, and there is no
     * order of the three in which the wrong reading is harmless.
     */
    ClawtMeasureUnit measure_unit;
    gint       measure;
    gint       run_spacing;

    /*
     * Set only when the chosen scheme came from a file, because a
     * palette on disk has no ClawtTheme value.  The built-in schemes go
     * on `theme` as they always did, so nothing about how a choice is
     * stored or read changed.
     */
    gchar     *palette;
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
    copy->measure_unit = self->measure_unit;
    copy->measure = self->measure;
    copy->run_spacing = self->run_spacing;
    copy->palette = g_strdup(self->palette);

    return copy;
}

void
clawt_appearance_free(ClawtAppearance *self)
{
    if (self == NULL)
        return;

    g_free(self->font);
    g_free(self->monospace_font);
    g_free(self->palette);
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

/* ── The measure's units ─────────────────────────────────────────── */

/*
 * One table, walked by both clients rather than copied into either.
 *
 * The colour schemes already record what the alternative costs: two
 * hand-written lists is how a palette came to be selectable in the GTK
 * combo and absent from the web select, with `make parity` reporting OK
 * throughout because a choice like this sends no IPC frame and answers
 * no slash command.
 *
 * `suffix` is what a stored measure ends in and is the whole reason the
 * value is self-describing: `90%` cannot be misread as pixels by a
 * build that has never heard of percentages, because it will not parse
 * at all and falls back to the shipped column.  A *missing* suffix is
 * pixels, handled in the parser rather than by giving pixels an empty
 * suffix here -- an empty suffix in the table would match before any
 * real one on a build whose loop happened to reach it first, so `90%`
 * would parse as 90 pixels.
 */
typedef struct {
    ClawtMeasureUnit unit;
    const gchar     *nick;
    const gchar     *label;
    const gchar     *suffix;
    gint             min;
    gint             max;
    gint             preset;
    gint             step;
} MeasureUnitInfo;

static const MeasureUnitInfo measure_units[] = {
    { CLAWT_MEASURE_DEFAULT, "default", "Follow the shipped column", "",
      0, 0, 0, 1 },
    { CLAWT_MEASURE_PERCENT, "percent", "Share of the window", "%",
      CLAWT_APPEARANCE_MIN_PERCENT, CLAWT_APPEARANCE_MAX_PERCENT,
      CLAWT_APPEARANCE_DEFAULT_PERCENT, 5 },
    { CLAWT_MEASURE_COLUMNS, "columns", "Characters a line", "ch",
      CLAWT_APPEARANCE_MIN_COLUMNS, CLAWT_APPEARANCE_MAX_COLUMNS,
      80, 5 },
    { CLAWT_MEASURE_PIXELS, "pixels", "Pixels", "px",
      CLAWT_APPEARANCE_MIN_MEASURE, CLAWT_APPEARANCE_MAX_MEASURE,
      CLAWT_CHAT_CLAMP_WIDTH, 20 }
};

static const MeasureUnitInfo *
measure_unit_info(ClawtMeasureUnit unit)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(measure_units); i++) {
        if (measure_units[i].unit == unit)
            return &measure_units[i];
    }

    return &measure_units[0];
}

guint
clawt_measure_unit_count(void)
{
    return G_N_ELEMENTS(measure_units);
}

ClawtMeasureUnit
clawt_measure_unit_nth(guint n)
{
    if (n >= G_N_ELEMENTS(measure_units))
        return CLAWT_MEASURE_DEFAULT;

    return measure_units[n].unit;
}

const gchar *
clawt_measure_unit_nick(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->nick;
}

const gchar *
clawt_measure_unit_label(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->label;
}

ClawtMeasureUnit
clawt_measure_unit_from_nick(const gchar *nick)
{
    guint i;

    if (nick == NULL)
        return CLAWT_MEASURE_DEFAULT;

    for (i = 0; i < G_N_ELEMENTS(measure_units); i++) {
        if (g_ascii_strcasecmp(nick, measure_units[i].nick) == 0)
            return measure_units[i].unit;
    }

    return CLAWT_MEASURE_DEFAULT;
}

gint
clawt_measure_unit_min(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->min;
}

gint
clawt_measure_unit_max(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->max;
}

gint
clawt_measure_unit_preset(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->preset;
}

gint
clawt_measure_unit_step(ClawtMeasureUnit unit)
{
    return measure_unit_info(unit)->step;
}

gboolean
clawt_measure_parse(const gchar *text, ClawtMeasureUnit *unit, gint *amount)
{
    g_autofree gchar *trimmed = NULL;
    const gchar *suffix;
    gchar *end = NULL;
    gint64 value;
    guint i;

    g_return_val_if_fail(unit != NULL, FALSE);
    g_return_val_if_fail(amount != NULL, FALSE);

    *unit = CLAWT_MEASURE_DEFAULT;
    *amount = 0;

    if (text == NULL)
        return FALSE;

    trimmed = g_strstrip(g_strdup(text));

    if (trimmed[0] == '\0')
        return FALSE;

    value = g_ascii_strtoll(trimmed, &end, 10);

    /*
     * `end` is where the digits stopped, so it is the suffix -- and it
     * is compared against the table rather than switched on, so a unit
     * added to the table is parseable without an edit here.  A suffix
     * nothing matches is the shipped column, not an error: this is a
     * preference file somebody types into, and refusing to start over a
     * typo in it would be far worse than ignoring the line.
     */
    if (end == trimmed || value <= 0)
        return FALSE;

    suffix = g_strchug(end);

    /*
     * A bare number is pixels, which is what a bare number has always
     * meant in the appearance file.  Anything else would read an
     * existing `measure: 640` as 640 of whatever the new default unit
     * was -- so a column somebody set once would silently become the
     * whole window, with nothing anywhere to say so.
     */
    if (suffix[0] == '\0') {
        *unit = CLAWT_MEASURE_PIXELS;
        *amount = (gint)CLAMP(value, (gint64)CLAWT_APPEARANCE_MIN_MEASURE,
                              (gint64)CLAWT_APPEARANCE_MAX_MEASURE);
        return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(measure_units); i++) {
        if (measure_units[i].unit == CLAWT_MEASURE_DEFAULT)
            continue;

        if (g_ascii_strcasecmp(suffix, measure_units[i].suffix) != 0)
            continue;

        *unit = measure_units[i].unit;
        *amount = (gint)CLAMP(value, (gint64)measure_units[i].min,
                              (gint64)measure_units[i].max);
        return TRUE;
    }

    return FALSE;
}

gchar *
clawt_measure_to_string(ClawtMeasureUnit unit, gint amount)
{
    const MeasureUnitInfo *info = measure_unit_info(unit);

    if (unit == CLAWT_MEASURE_DEFAULT || amount <= 0)
        return g_strdup("");

    return g_strdup_printf("%d%s", CLAMP(amount, info->min, info->max),
                           info->suffix);
}

gchar *
clawt_measure_to_css(ClawtMeasureUnit unit, gint amount, const gchar *inset)
{
    const MeasureUnitInfo *info = measure_unit_info(unit);
    gint bounded;

    if (unit == CLAWT_MEASURE_DEFAULT || amount <= 0)
        return NULL;

    bounded = CLAMP(amount, info->min, info->max);

    /*
     * A character count is about the words, and a body starts a gutter
     * in from the column's edge -- so 80 columns without the correction
     * is 80 less the gutter's worth of characters, which is a different
     * number at every font size.  calc() is what keeps `ch` doing the
     * work it is worth having: the browser resolves it against the
     * element's own font, so changing the font size later keeps the
     * count the reader asked for.
     */
    if (unit == CLAWT_MEASURE_COLUMNS && inset != NULL)
        return g_strdup_printf("calc(%dch + %s)", bounded, inset);

    return g_strdup_printf("%d%s", bounded, info->suffix);
}

gint
clawt_measure_resolve_px(ClawtMeasureUnit unit,
                         gint             amount,
                         gint             available,
                         gdouble          char_width,
                         gint             inset)
{
    const MeasureUnitInfo *info = measure_unit_info(unit);
    gint share = CLAWT_APPEARANCE_DEFAULT_PERCENT;
    gint px;

    if (amount > 0)
        amount = CLAMP(amount, info->min, info->max);
    else
        unit = CLAWT_MEASURE_DEFAULT;

    switch (unit) {
    case CLAWT_MEASURE_PIXELS:
        px = amount;
        break;

    case CLAWT_MEASURE_COLUMNS:
        px = (gint)(amount * MAX(char_width, 1.0) + 0.5) + MAX(inset, 0);
        break;

    case CLAWT_MEASURE_PERCENT:
        share = amount;
        /* fall through */

    case CLAWT_MEASURE_DEFAULT:
    default:
        /*
         * A percentage of an unknown width is the reference column
         * rather than nothing.  A clamp is built before its widget has
         * ever been allocated, and answering 0 there would collapse the
         * transcript for the frame between construction and the first
         * size-allocate -- visible, and indistinguishable from the
         * setting being broken.
         */
        px = available > 0 ? (available * share) / 100
                           : CLAWT_CHAT_CLAMP_WIDTH;
        break;
    }

    /*
     * Floored, never capped.  A reader who asked for nine tenths of a
     * very wide display asked for exactly that, and clamping it back to
     * a typographic maximum would be a control that reports a setting
     * it did not apply -- which is the failure this file records about
     * a combo box that cannot say "something else", arrived at from the
     * other side.
     */
    if (available > 0 && px > available)
        px = available;

    if (px < CLAWT_APPEARANCE_MIN_MEASURE) {
        px = (available > 0)
             ? MIN(CLAWT_APPEARANCE_MIN_MEASURE, available)
             : CLAWT_APPEARANCE_MIN_MEASURE;
    }

    return MAX(px, 1);
}

/*
 * Out-of-range is clamped rather than refused, for the same reason a
 * font size is: this file is edited by hand, and a value that made the
 * transcript unusable would leave no obvious way to fix it from inside
 * the app.
 */
ClawtMeasureUnit
clawt_appearance_get_measure_unit(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_MEASURE_DEFAULT);

    return self->measure_unit;
}

gint
clawt_appearance_get_measure_amount(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->measure;
}

void
clawt_appearance_set_measure(ClawtAppearance *self,
                             ClawtMeasureUnit unit,
                             gint             amount)
{
    const MeasureUnitInfo *info;

    g_return_if_fail(self != NULL);

    if (amount <= 0 || unit == CLAWT_MEASURE_DEFAULT) {
        self->measure_unit = CLAWT_MEASURE_DEFAULT;
        self->measure = 0;
        return;
    }

    info = measure_unit_info(unit);
    self->measure_unit = info->unit;
    self->measure = CLAMP(amount, info->min, info->max);
}

gint
clawt_appearance_get_run_spacing(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->run_spacing;
}

void
clawt_appearance_set_run_spacing(ClawtAppearance *self, gint pixels)
{
    g_return_if_fail(self != NULL);

    if (pixels <= 0) {
        self->run_spacing = 0;
        return;
    }

    self->run_spacing = MIN(pixels, CLAWT_APPEARANCE_MAX_RUN_SPACING);
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
 * That paragraph was written before the list under it was complete, and
 * fourteen of libadwaita 1.9's colours were missing -- which is what the
 * alerts panel found.  It is a second AdwOverlaySplitView inside the
 * first one's content, so libadwaita gives it `.sidebar-pane` *inside*
 * `.content-pane` and paints it from `--secondary-sidebar-bg-color`,
 * which this palette had never heard of.  Measured against real GTK
 * 4.22 and libadwaita 1.9.3: `#28282c`, stock GNOME's neutral grey, in
 * a window where the agent sidebar beside it was `#181825`.  The checked
 * toggle above it was worse -- `--active-toggle-bg-color` is a literal
 * `rgb(255 255 255 / 20%)` with no `@define-color` behind it at all.
 *
 * So required_colors[] below is now the list, and every palette is
 * checked against it.  A comment claiming completeness is not a check,
 * and this one was wrong for as long as it had been there.
 *
 * Four of the names are ours rather than libadwaita's: it reads
 * `--active-toggle-bg-color`, `--active-toggle-fg-color`,
 * `--overview-bg-color` and `--overview-fg-color` as custom properties
 * only, with no `@define-color` counterpart.  They are written here in
 * the palette's own dialect anyway, because append_custom_properties()
 * derives the `--token` form from it -- one list of colours, which is
 * the whole reason that function exists.  The `@define-color` those four
 * also produce is inert, and that is cheaper than a second list.
 *
 * `--banner-color` is deliberately not among them.  libadwaita sets it
 * on the `banner` element itself, so a `:root` value cannot reach it and
 * overriding it would need a rule rather than a colour.  Nothing in
 * either client draws an AdwBanner; when something does, that is a rule
 * to add, not a name missing from here.
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
    "@define-color scrollbar_outline_color #11111b;\n"
    "@define-color sidebar_border_color #11111b;\n"
    /*
     * A sidebar nested one level deeper, which for this client is the
     * alerts panel.  Crust, because that is the step below the mantle
     * the primary sidebar uses -- the same relationship libadwaita's own
     * dark colours have (#28282c under #2e2e32), spelled in Catppuccin.
     * Its backdrop is crust as well: there is nothing below crust to
     * darken to, and inventing one would be a colour from no palette.
     */
    "@define-color secondary_sidebar_bg_color #11111b;\n"
    "@define-color secondary_sidebar_fg_color #cdd6f4;\n"
    "@define-color secondary_sidebar_backdrop_color #11111b;\n"
    "@define-color secondary_sidebar_shade_color #11111b;\n"
    "@define-color secondary_sidebar_border_color #11111b;\n"
    "@define-color headerbar_darker_shade_color #11111b;\n"
    "@define-color popover_shade_color #11111b;\n"
    "@define-color thumbnail_bg_color #313244;\n"
    "@define-color thumbnail_fg_color #cdd6f4;\n"
    /*
     * The four libadwaita reads only as custom properties.  Surface1 for
     * the checked toggle: it has to read as raised above the trough,
     * which is what libadwaita's 20% white does over its own greys.
     */
    "@define-color active_toggle_bg_color #45475a;\n"
    "@define-color active_toggle_fg_color #cdd6f4;\n"
    "@define-color overview_bg_color #1e1e2e;\n"
    "@define-color overview_fg_color #cdd6f4;\n";

/*
 * Every colour a palette has to answer for.
 *
 * Read off libadwaita 1.9.3's own stylesheet rather than remembered:
 * `gresource extract /usr/lib64/libadwaita-1.so.0
 * /org/gnome/Adwaita/styles/gtk.css`, then every `var(--*)` in it.  That
 * is the honest source, and it is why the list is longer than the set
 * anybody would write down from the widgets they had looked at.
 *
 * Two of libadwaita's tokens are deliberately absent.  `--border-color`
 * is derived from `currentColor`, so it follows a palette's foregrounds
 * without being named.  `--banner-color` is set on the `banner` element
 * itself and cannot be reached from `:root` at all -- see the palette
 * comment above.
 *
 * The point of having it as a list is that it can be checked.  A palette
 * that omits a name does not fail: it silently keeps stock GNOME's value
 * for that one colour, which is a single widget in the wrong grey in an
 * otherwise perfect window -- exactly the report that started this, and
 * exactly the kind of thing nobody thinks to look for.  So the built-in
 * palettes are checked by tests/test-appearance.c, and a palette read
 * off disk is checked when it is read, because somebody writing their
 * own is the person most likely to leave one out and least likely to
 * know the list exists.
 *
 * A libadwaita release that grows a token is not caught by any of that.
 * Nothing hermetic can catch it -- the list lives in a library this
 * process may not even have loaded -- so it is stated here rather than
 * pretended about: when a new libadwaita lands, re-run the gresource
 * command above and diff it against this array.
 */
static const gchar *const required_colors[] = {
    "accent_bg_color",
    "accent_color",
    "accent_fg_color",
    "active_toggle_bg_color",
    "active_toggle_fg_color",
    "card_bg_color",
    "card_fg_color",
    "card_shade_color",
    "destructive_bg_color",
    "destructive_color",
    "destructive_fg_color",
    "dialog_bg_color",
    "dialog_fg_color",
    "error_bg_color",
    "error_color",
    "error_fg_color",
    "headerbar_backdrop_color",
    "headerbar_bg_color",
    "headerbar_border_color",
    "headerbar_darker_shade_color",
    "headerbar_fg_color",
    "headerbar_shade_color",
    "overview_bg_color",
    "overview_fg_color",
    "popover_bg_color",
    "popover_fg_color",
    "popover_shade_color",
    "scrollbar_outline_color",
    "secondary_sidebar_backdrop_color",
    "secondary_sidebar_bg_color",
    "secondary_sidebar_border_color",
    "secondary_sidebar_fg_color",
    "secondary_sidebar_shade_color",
    "shade_color",
    "sidebar_backdrop_color",
    "sidebar_bg_color",
    "sidebar_border_color",
    "sidebar_fg_color",
    "sidebar_shade_color",
    "success_bg_color",
    "success_color",
    "success_fg_color",
    "thumbnail_bg_color",
    "thumbnail_fg_color",
    "view_bg_color",
    "view_fg_color",
    "warning_bg_color",
    "warning_color",
    "warning_fg_color",
    "window_bg_color",
    "window_fg_color"
};

/*
 * Whether @css names @color, in either dialect.
 *
 * Both, because both are legitimate: this file writes `@define-color`
 * and lets append_custom_properties() derive the rest, while somebody
 * copying a palette off the web is as likely to have `--window-bg-color`
 * in it.  Reporting the second kind as incomplete would be a warning
 * about a file that works.
 *
 * The name must be followed by a space or a colon, so `shade_color` is
 * not found inside `card_shade_color` -- which would have made half the
 * list unfindable in the wrong direction, reporting complete palettes
 * as complete for the wrong reason.
 */
static gboolean
palette_defines(const gchar *css, const gchar *color)
{
    g_autofree gchar *at_form = NULL;
    g_autofree gchar *dashed = NULL;
    g_autofree gchar *var_form = NULL;
    const gchar *found;
    gchar *dash;

    if (css == NULL || color == NULL)
        return FALSE;

    at_form = g_strconcat("@define-color ", color, " ", NULL);

    if (strstr(css, at_form) != NULL)
        return TRUE;

    dashed = g_strdup(color);

    for (dash = dashed; *dash != '\0'; dash++) {
        if (*dash == '_')
            *dash = '-';
    }

    var_form = g_strconcat("--", dashed, ":", NULL);
    found = strstr(css, var_form);

    return found != NULL;
}

gchar **
clawt_appearance_palette_missing(const gchar *css)
{
    GPtrArray *missing = g_ptr_array_new();
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(required_colors); i++) {
        if (!palette_defines(css, required_colors[i]))
            g_ptr_array_add(missing, g_strdup(required_colors[i]));
    }

    g_ptr_array_add(missing, NULL);

    return (gchar **)g_ptr_array_free(missing, FALSE);
}

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

/*
 * A palette found on disk.
 *
 * The nick is the file's basename without its extension, so the name a
 * person gives the file is the name they see and the name that lands in
 * their appearance file.  Nothing generates these, so nothing has to
 * agree with a table.
 */
typedef struct {
    gchar    *nick;
    gchar    *label;
    gchar    *css;
    gboolean  dark;
} Palette;

static GPtrArray *palettes;   /* Palette*, discovered once */

static void
palette_free(gpointer data)
{
    Palette *palette = data;

    g_free(palette->nick);
    g_free(palette->label);
    g_free(palette->css);
    g_free(palette);
}

/*
 * A palette may declare itself on its first line:
 *
 *   /-* clawtilla-palette: label="Gruvbox Dark" dark=1 *-/
 *
 * Both parts are optional.  Without a label the basename is used, and
 * without `dark` a palette is assumed dark -- which is the safer guess:
 * almost every hand-written palette is, and libadwaita drawing light
 * base styling under dark colours is far uglier than the reverse.
 */
static void
palette_read_header(const gchar *css, gchar **label_out, gboolean *dark_out)
{
    const gchar *at;

    *label_out = NULL;
    *dark_out = TRUE;

    if (css == NULL)
        return;

    at = strstr(css, "clawtilla-palette:");

    /* Only the header, not a mention further down the file. */
    if (at == NULL || (at - css) > 64)
        return;

    {
        const gchar *label = strstr(at, "label=\"");

        if (label != NULL) {
            const gchar *start = label + strlen("label=\"");
            const gchar *end = strchr(start, '"');

            if (end != NULL && end > start)
                *label_out = g_strndup(start, (gsize)(end - start));
        }
    }

    {
        const gchar *dark = strstr(at, "dark=");

        if (dark != NULL)
            *dark_out = (dark[strlen("dark=")] != '0');
    }
}

gchar *
clawt_appearance_palette_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "clawtilla", "palettes",
                            NULL);
}

guint
clawt_appearance_reload_palettes(void)
{
    g_autofree gchar *dir_path = clawt_appearance_palette_dir();
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    if (palettes != NULL)
        g_ptr_array_set_size(palettes, 0);
    else
        palettes = g_ptr_array_new_with_free_func(palette_free);

    dir = g_dir_open(dir_path, 0, NULL);

    /* No directory is no palettes, not an error. */
    if (dir == NULL)
        return 0;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *css = NULL;
        g_autofree gchar *label = NULL;
        Palette *palette;
        gboolean dark = TRUE;

        if (!g_str_has_suffix(name, ".css"))
            continue;

        path = g_build_filename(dir_path, name, NULL);

        if (!g_file_get_contents(path, &css, NULL, NULL))
            continue;

        palette = g_new0(Palette, 1);
        palette->nick = g_strndup(name, strlen(name) - strlen(".css"));

        palette_read_header(css, &label, &dark);

        palette->label = (label != NULL) ? g_steal_pointer(&label)
                                         : g_strdup(palette->nick);
        palette->css = g_steal_pointer(&css);
        palette->dark = dark;

        /*
         * Deliberately not checked against required_colors[] here.
         *
         * The first version of this warned about every colour a palette
         * left out, which fired on the two-line example in
         * docs/clients.org -- a palette that overrides the accent and
         * the window background over libadwaita's dark scheme is a
         * documented, supported and entirely reasonable thing to write.
         * "Write the half you need; the other is inert" is the contract,
         * so a partial palette is not a mistake and saying it is trains
         * somebody to ignore the log.
         *
         * Completeness is a rule about the palettes *this repository
         * ships*, where a missing colour is drift rather than intent,
         * and tests/test-appearance.c is where that belongs.  The list
         * is public for an author who does want a complete one; the
         * whole set is in docs/clients.org.
         */
        g_ptr_array_add(palettes, palette);
    }

    return palettes->len;
}

/*
 * Discovery happens on first use, not from a call somebody has to
 * remember.
 *
 * This file already records what an uncalled factory costs: a feature
 * that is correct, tested and reached by nobody.  Every entry point
 * that can see a palette goes through here, so a client that never
 * heard of reload_palettes() still finds them.
 */
static void
ensure_palettes(void)
{
    if (palettes == NULL)
        clawt_appearance_reload_palettes();
}

static const Palette *
palette_by_nick(const gchar *nick)
{
    guint i;

    if (nick == NULL || palettes == NULL)
        return NULL;

    for (i = 0; i < palettes->len; i++) {
        const Palette *palette = g_ptr_array_index(palettes, i);

        if (g_strcmp0(palette->nick, nick) == 0)
            return palette;
    }

    return NULL;
}

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

const gchar *
clawt_appearance_get_palette_css(ClawtAppearance *self)
{
    const Palette *palette;

    g_return_val_if_fail(self != NULL, NULL);

    if (self->palette == NULL)
        return NULL;

    palette = palette_by_nick(self->palette);

    return (palette != NULL) ? palette->css : NULL;
}

guint
clawt_appearance_scheme_count(void)
{
    ensure_palettes();

    return (guint)G_N_ELEMENTS(theme_table) +
           (palettes != NULL ? palettes->len : 0);
}

const gchar *
clawt_appearance_scheme_nth_nick(guint n)
{
    ensure_palettes();

    if (n < G_N_ELEMENTS(theme_table))
        return theme_table[n].nick;

    n -= (guint)G_N_ELEMENTS(theme_table);

    if (palettes == NULL || n >= palettes->len)
        return "system";

    return ((const Palette *)g_ptr_array_index(palettes, n))->nick;
}

const gchar *
clawt_appearance_scheme_nth_label(guint n)
{
    ensure_palettes();

    if (n < G_N_ELEMENTS(theme_table))
        return theme_table[n].label;

    n -= (guint)G_N_ELEMENTS(theme_table);

    if (palettes == NULL || n >= palettes->len)
        return "Follow the system";

    return ((const Palette *)g_ptr_array_index(palettes, n))->label;
}

const gchar *
clawt_appearance_get_scheme(ClawtAppearance *self)
{
    g_return_val_if_fail(self != NULL, "system");

    if (self->palette != NULL)
        return self->palette;

    return clawt_appearance_theme_nick(self->theme);
}

void
clawt_appearance_set_scheme(ClawtAppearance *self, const gchar *nick)
{
    const Palette *palette;
    gsize i;

    g_return_if_fail(self != NULL);

    ensure_palettes();
    g_clear_pointer(&self->palette, g_free);

    for (i = 0; i < G_N_ELEMENTS(theme_table); i++) {
        if (g_strcmp0(theme_table[i].nick, nick) == 0) {
            self->theme = theme_table[i].theme;
            return;
        }
    }

    palette = palette_by_nick(nick);

    if (palette != NULL) {
        self->palette = g_strdup(palette->nick);

        /*
         * The base mode still has to be one of the built-ins, because
         * that is what libadwaita and the browser are told.  A palette
         * that says nothing is treated as dark: almost every
         * hand-written one is, and light base styling under dark
         * colours is far worse than the reverse.
         */
        self->theme = palette->dark ? CLAWT_THEME_DARK : CLAWT_THEME_LIGHT;
        return;
    }

    /*
     * An unknown nick follows the system rather than failing.
     *
     * Three ways to reach it and all of them ordinary: a palette file
     * somebody deleted, a newer build's scheme read by an older one,
     * and a hand-edited typo.  Refusing would take the fonts down with
     * the colour, which is the same forward-compatibility rule shadow
     * agents exist for.
     */
    self->theme = CLAWT_THEME_SYSTEM;
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

/*
 * The same palette again, in the dialect libadwaita's own rules read.
 *
 * `@define-color` and the `--token` custom properties are two
 * namespaces, not two spellings of one.  Measured on 1.8.7: a `.accent`
 * label under `@define-color accent_color rgb(9,9,9)` stays stock blue
 * and only moves when `:root { --accent-color: ... }` says so, while a
 * rule of our own reading `@accent_color` sees the new value and one
 * reading `var(--accent-color)` does not.  Which tokens bridge between
 * them is undocumented and has changed between releases.
 *
 * So both forms are emitted for every token, and the second is derived
 * from the first rather than written out again -- a palette is one list
 * of colours, and two hand-maintained copies of thirty-seven hex values
 * would drift the first time somebody added a colour.
 *
 * The transform is exactly libadwaita's own naming: `accent_color`
 * becomes `--accent-color`.  Anything that is not a `@define-color`
 * line is skipped rather than guessed at.
 */
static void
append_custom_properties(GString *out, const gchar *palette)
{
    g_auto(GStrv) lines = NULL;
    guint i;

    g_return_if_fail(out != NULL);

    if (palette == NULL)
        return;

    lines = g_strsplit(palette, "\n", -1);

    g_string_append(out, ":root {\n");

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *rest = lines[i];
        const gchar *space;
        g_autofree gchar *name = NULL;
        g_autofree gchar *value = NULL;
        gchar *semicolon;
        gchar *dash;

        if (!g_str_has_prefix(rest, "@define-color "))
            continue;

        rest += strlen("@define-color ");
        space = strchr(rest, ' ');

        if (space == NULL)
            continue;

        name = g_strndup(rest, (gsize)(space - rest));
        value = g_strdup(space + 1);
        semicolon = strchr(value, ';');

        if (semicolon == NULL)
            continue;

        *semicolon = '\0';

        for (dash = name; *dash != '\0'; dash++) {
            if (*dash == '_')
                *dash = '-';
        }

        g_string_append_printf(out, "  --%s: %s;\n", name, value);
    }

    g_string_append(out, "}\n");
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
    {
        const gchar *palette = NULL;

        /*
         * A palette from disk wins over the built-in mode it is based
         * on.  Both go through append_custom_properties() for the same
         * reason: libadwaita's own rules read `--token` and not
         * `@define-color`, so a sheet emitting one form styles this
         * client's rules and leaves every libadwaita-drawn accent at
         * stock GNOME.
         */
        if (self->palette != NULL) {
            const Palette *found = palette_by_nick(self->palette);

            if (found != NULL)
                palette = found->css;
        } else if (clawt_appearance_theme_has_palette(self->theme)) {
            palette = theme_info(self->theme)->css;
        }

        if (palette != NULL) {
            g_string_append(out, palette);
            append_custom_properties(out, palette);
        }
    }

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
     * The chat column, as custom properties rather than rules.
     *
     * This is step three of the configuration layer: the shipped clamp
     * and run gap used to be constants in C, so the shipped design and
     * a person's override were two different mechanisms and only one of
     * them existed.  They are tokens now, declared by whichever client
     * is rendering, and overridden here when somebody has set a value.
     *
     * Emitted as `:root` properties because the two clients apply them
     * in different vocabularies -- the web sheet reads them directly,
     * and the GTK client reads the *value* off the appearance and sets
     * its clamp, because AdwClamp takes a property and not a stylesheet.
     * The colours already work this way for the same reason.
     *
     * Still nothing at all when nothing is set.
     */
    {
        /*
         * The unit travels with the number all the way to the
         * stylesheet, so a percentage stays a percentage and a
         * character count stays `ch`.  Resolving either to pixels here
         * would be resolving it against a window this code cannot see,
         * and would freeze the answer at whatever that window happened
         * to be when the sheet was written.
         */
        g_autofree gchar *measure =
            clawt_measure_to_css(self->measure_unit, self->measure,
                                 "var(--chat-gutter)");

        if (measure != NULL || self->run_spacing > 0) {
            g_string_append(out, ":root {\n");

            if (measure != NULL)
                g_string_append_printf(out, "  --chat-measure: %s;\n",
                                       measure);

            if (self->run_spacing > 0)
                g_string_append_printf(out, "  --chat-run-gap: %dpx;\n",
                                       self->run_spacing);

            g_string_append(out, "}\n");
        }
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

    clawt_appearance_set_scheme(self, member_string(mapping, "theme"));

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

    value = member_string(mapping, "measure");

    if (value != NULL) {
        ClawtMeasureUnit unit;
        gint amount;

        /*
         * Through the one reader, so a spelling the web client's cookie
         * accepts is a spelling this file accepts.  A value it cannot
         * place is the shipped column rather than a parse error --
         * refusing to start over a typo in a preference file would cost
         * somebody their whole client for the sake of a column width.
         */
        clawt_measure_parse(value, &unit, &amount);
        clawt_appearance_set_measure(self, unit, amount);
    }

    value = member_string(mapping, "run_spacing");

    if (value != NULL)
        clawt_appearance_set_run_spacing(
            self, (gint)g_ascii_strtoll(value, NULL, 10));

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

    /*
     * The scheme's nick rather than the enum's, so a palette read from
     * a file round-trips.  Built-in schemes are unaffected -- their
     * nick is what was always written here.
     */
    append_quoted(out, "theme", clawt_appearance_get_scheme(self));

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

    g_string_append(out,
        "\n"
        "# How wide the conversation column is, and how far apart one\n"
        "# person's run of messages sits from the next. Empty means use\n"
        "# the shipped value -- which is not the same as writing that\n"
        "# value here, because writing it would freeze it.\n"
        "#\n"
        "# The measure carries its own unit: '90%' is nine tenths of the\n"
        "# width the window has to give, '80ch' is eighty characters a\n"
        "# line whatever the font, and '640px' is exactly that. A bare\n"
        "# number is pixels, which is what it has always meant here.\n");

    {
        g_autofree gchar *measure =
            clawt_measure_to_string(self->measure_unit, self->measure);

        append_quoted(out, "measure", measure);
    }

    g_string_append_printf(out, "run_spacing: %d\n", self->run_spacing);

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
