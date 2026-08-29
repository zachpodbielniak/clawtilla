/*
 * gtk-prefs-appearance.c - Appearance
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The stylesheet this client applies, and the settings page that edits
 * it.  The two are one file because the page is a live preview: every
 * control here edits the window's ClawtAppearance and applies it at
 * once, so the code that turns one into CSS is the code the page is
 * about.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Appearance ──────────────────────────────────────────────────── */

/*
 * The chosen code font, and the provider carrying the rest.
 *
 * File-scope because both are genuinely per-display rather than per
 * window: a GtkCssProvider is added to the GdkDisplay, and a second
 * window must not add a second copy of the same sheet. The alternative
 * -- threading an appearance pointer down to set_label_markdown() --
 * would put a parameter on a function whose whole job is one label, for
 * a value that cannot differ between two labels.
 */
static GtkCssProvider *appearance_provider = NULL;
static GtkCssProvider *structure_provider = NULL;
static GtkCssProvider *user_provider = NULL;
static gchar          *appearance_code_font = NULL;

/*
 * Structure this client draws that libadwaita has no widget for.
 *
 * Its own provider at PRIORITY_APPLICATION, below the generated
 * appearance sheet at PRIORITY_APPLICATION + 1.  The order the two are
 * added no longer decides anything, which is what the concatenation was
 * avoiding -- an explicit priority says it instead, and it says it
 * without re-parsing this constant every time somebody changes a font.
 *
 * Splitting is safe for the named colours below even though the palette
 * that defines them now lives in a different provider: a @define-color
 * is visible across the whole cascade, not only within the sheet that
 * wrote it.  Measured, because the failure mode is silent -- an
 * unresolved reference falls back to libadwaita's own value with no
 * parse error and no warning.
 *
 * Every colour is a libadwaita named colour, never a hex value.  That is
 * what makes a palette a palette swap rather than a second pass over
 * every rule here -- the Catppuccin sheet redefines `accent_bg_color`
 * and this follows it for free.
 */
static const gchar CLAWT_STRUCTURE_CSS[] =
    /*
     * The unread pill.  Filled, because everything else in that row is a
     * coloured caption: filled means for you, text means about the
     * agent.
     */
    ".clawt-unread-badge {\n"
    "  background-color: @accent_bg_color;\n"
    "  color: @accent_fg_color;\n"
    "  border-radius: 9px;\n"
    "  min-height: 18px;\n"
    "  min-width: 6px;\n"
    "  padding: 0 6px;\n"
    "  font-weight: bold;\n"
    "}\n"
    /*
     * ...and the name in bold beside it.  Colour is never the only
     * signal in this client; the state dot already holds that rule.
     */
    ".clawt-unread .title {\n"
    "  font-weight: bold;\n"
    "}\n"
    /*
     * The operator's own turns.
     *
     * 12px is libadwaita's card radius, so a bubble matches every other
     * rounded surface in the application rather than inventing one.
     * Named colours throughout, which is what makes a palette a palette
     * swap rather than a second pass over this block.
     */
    ".clawt-bubble {\n"
    "  background-color: @accent_bg_color;\n"
    "  color: @accent_fg_color;\n"
    "  border-radius: 12px;\n"
    "  padding: 8px 12px;\n"
    "}\n"
    /*
     * A run of bubbles reads as one utterance because the second and
     * later ones drop the corner nearest the one above.
     */
    ".clawt-bubble-cont {\n"
    "  border-top-right-radius: 4px;\n"
    "}\n"
    /*
     * The message times are a column, so every one of them has to be the
     * same width.
     *
     * Proportional digits make that impossible: measured against real
     * GTK 4.22 at this stylesheet's caption size, an `HH:MM` string
     * ranges from 22px of ink for 11:11 to 34px for 00:00 -- a twelve
     * pixel swing inside a rail meant to read as one straight edge.
     * Tabular figures give every one of them the same advance, and the
     * four ink right edges collapse from 34/31/22/31 to a single number.
     *
     * Purely visual, so it is a rule here rather than a call in C.
     * Where the label *sits* is not: that has to stay tied to the
     * avatar's diameter, and a value that must move in lockstep with
     * another belongs where that other one lives.
     */
    ".clawt-message-time {\n"
    "  font-feature-settings: \"tnum\" 1;\n"
    "}\n"
    /*
     * Links and inline code inside a bubble.  Without this they render
     * in the accent colour on the accent colour, which is invisible
     * rather than merely low contrast.
     */
    ".clawt-bubble .body {\n"
    "  color: @accent_fg_color;\n"
    "}\n";

/*
 * Applies fonts and colour scheme.
 *
 * Called on startup and on every change in the settings dialog, so the
 * dialog is a live preview rather than something you close and hope
 * about.
 */
void
clawt_gtk_apply_appearance(ClawtAppearance *appearance)
{
    GdkDisplay *display = gdk_display_get_default();
    g_autofree gchar *css = NULL;
    AdwStyleManager *style = adw_style_manager_get_default();

    if (appearance == NULL || display == NULL)
        return;

    /*
     * The scheme first, and asked for rather than switched on, so a
     * palette added later needs no case here.  A palette still sets the
     * scheme: its colours only name some of libadwaita's, and the rest
     * come from whichever scheme is underneath -- so Mocha over a light
     * libadwaita is not a lighter Mocha, it is two palettes arguing.
     */
    {
        ClawtTheme theme = clawt_appearance_get_theme(appearance);

        if (theme == CLAWT_THEME_SYSTEM)
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_DEFAULT);
        else if (clawt_appearance_theme_is_dark(theme))
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_FORCE_DARK);
        else
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_FORCE_LIGHT);
    }

    g_free(appearance_code_font);
    appearance_code_font =
        g_strdup(clawt_appearance_get_monospace_font(appearance));

    css = clawt_appearance_to_css(appearance);

    /*
     * The structure sheet is a constant, so it is loaded once and left
     * alone.  It used to be re-parsed on every settings change purely
     * because it was glued to the front of the generated one.
     */
    if (structure_provider == NULL) {
        structure_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(structure_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_string(structure_provider,
                                          CLAWT_STRUCTURE_CSS);
    }

    /*
     * One provider, reloaded, rather than a new one each time.  Adding a
     * provider per change leaves every previous sheet on the display at
     * the same priority, so the fonts stop changing after the first edit
     * -- the oldest rule keeps winning ties.
     */
    if (appearance_provider == NULL) {
        appearance_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(appearance_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

    gtk_css_provider_load_from_string(appearance_provider, css);

    /*
     * And whatever the person running this wrote, above everything the
     * client has an opinion about.
     *
     * PRIORITY_USER is what makes it worth having: a bare `.clawt-thing`
     * there beats an `#id.clawt-thing` in either sheet above, so a rule
     * can be overridden without having to out-specify code somebody else
     * wrote.  Absent file, nothing loaded and nothing said -- not having
     * one is the normal case, not a misconfiguration.
     */
    if (user_provider == NULL) {
        g_autofree gchar *path =
            g_build_filename(g_get_user_config_dir(), "clawtilla",
                             "style.css", NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            user_provider = gtk_css_provider_new();
            gtk_style_context_add_provider_for_display(
                display, GTK_STYLE_PROVIDER(user_provider),
                GTK_STYLE_PROVIDER_PRIORITY_USER);
            gtk_css_provider_load_from_path(user_provider, path);
        }
    }
}

void
clawt_gtk_set_label_markdown(GtkLabel *label, const gchar *body)
{
    g_autofree gchar *markup =
        clawt_markdown_to_pango_full(body, appearance_code_font);
    g_autoptr(GError) error = NULL;

    if (pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, &error)) {
        gtk_label_set_markup(label, markup);
        return;
    }

    g_warning("markdown produced markup Pango rejected (%s); "
              "showing it plainly", error->message);
    gtk_label_set_text(label, body != NULL ? body : "");
}

/* ── The Appearance page ─────────────────────────────────────────── */

/*
 * Saved on every change rather than behind an Apply button.
 *
 * The page is a live preview -- the font changes under the dialog as you
 * pick it -- and a preview you can see but which is not yet saved is the
 * worst of both: it looks applied, and closing the window loses it.
 */
static void
appearance_changed(ClawtWindow *self)
{
    g_autoptr(GError) error = NULL;

    clawt_gtk_apply_appearance(self->appearance);

    /*
     * The measure reaches a property rather than the stylesheet, so it
     * has to be pushed.  Without this the setting would be written to
     * the file, reflected in the CSS, and invisible on screen until the
     * chat page was rebuilt -- a control that appears to do nothing,
     * which is worse than one that is missing.
     *
     * Both clamps, from one resolver: a column widened while the box
     * you type into stayed put would restore exactly the misalignment
     * the composer inset exists to fix.
     */
    clawt_gtk_push_chat_measure(self);

    if (!clawt_appearance_save(self->appearance, NULL, &error))
        clawt_window_toast(self, error->message);
}

static void
on_theme_selected(GObject *row, GParamSpec *spec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));

    (void)spec;

    /*
     * An index into the library's own list rather than into a copy of
     * it. The copy here named four schemes while the web client's named
     * three, so the palette added to clawt-appearance.c reached one
     * client and not the other -- and nothing said so, because a colour
     * scheme sends no IPC frame and is no slash command.
     */
    clawt_appearance_set_scheme(
        self->appearance,
        clawt_appearance_scheme_nth_nick(
            MIN(selected, clawt_appearance_scheme_count() - 1)));
    appearance_changed(self);
}

/*
 * The column and the run gap.
 *
 * Both are 0-means-defer, like every other field on the appearance, so
 * the spin buttons run from 0 rather than from their real minimum --
 * and the library clamps anything between 0 and the floor up to it, so
 * a person dragging down from 400 lands on the minimum rather than
 * silently on "follow the shipped value".
 */
static void
on_reading_size_changed(GtkSpinButton *spin, gpointer user_data)
{
    ClawtWindow *self = user_data;
    gboolean is_gap =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(spin), "run-gap"));
    gint value = (gint)gtk_spin_button_get_value(spin);

    if (is_gap) {
        clawt_appearance_set_run_spacing(self->appearance, value);
    } else {
        GtkWidget *combo = g_object_get_data(G_OBJECT(spin), "unit-combo");

        /*
         * The amount is meaningless without the unit beside it, so it is
         * read off the combo rather than remembered here -- 90 is nine
         * tenths of the window, ninety characters or ninety pixels, and
         * a second copy of which one it is now is a second thing to get
         * out of step.
         */
        clawt_appearance_set_measure(
            self->appearance,
            clawt_measure_unit_nth(
                adw_combo_row_get_selected(ADW_COMBO_ROW(combo))),
            value);
    }

    appearance_changed(self);
}

/*
 * Picking what the column's number means.
 *
 * The amount is re-seeded from the unit's own preset rather than
 * carried across, because the old number is in the old unit: 640 pixels
 * arriving as 240 characters is a column nobody asked for, and one that
 * has to be undone before the control is usable again.
 */
static void
on_measure_unit_selected(GObject *row, GParamSpec *spec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *spin = g_object_get_data(row, "amount-spin");
    ClawtMeasureUnit unit = clawt_measure_unit_nth(
        adw_combo_row_get_selected(ADW_COMBO_ROW(row)));

    (void)spec;

    if (unit == CLAWT_MEASURE_DEFAULT) {
        /*
         * Insensitive rather than hidden: a row that vanishes takes the
         * reader's place on the page with it, and coming back to find
         * the column control gone reads as the dialog having broken.
         */
        gtk_widget_set_sensitive(spin, FALSE);
        clawt_appearance_set_measure(self->appearance,
                                     CLAWT_MEASURE_DEFAULT, 0);
        appearance_changed(self);
        return;
    }

    gtk_widget_set_sensitive(spin, TRUE);

    /*
     * Blocked across the re-ranging, live for the value.
     *
     * gtk_spin_button_set_range() clamps whatever is showing into the
     * new range and emits ::value-changed doing it -- measured against
     * real GTK4, switching from 80 characters to pixels wrote `320`
     * (the pixel floor) and then `600` (the preset).  The first is an
     * intermediate nobody chose, and it reached the file and the column
     * on screen before the second overwrote it, so the transcript
     * visibly snapped to the minimum on its way.
     */
    g_signal_handlers_block_by_func(spin, on_reading_size_changed, self);
    gtk_spin_button_set_increments(GTK_SPIN_BUTTON(spin),
                                   clawt_measure_unit_step(unit),
                                   clawt_measure_unit_step(unit) * 5);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(spin),
                              clawt_measure_unit_min(unit),
                              clawt_measure_unit_max(unit));
    g_signal_handlers_unblock_by_func(spin, on_reading_size_changed, self);

    /*
     * Which fires ::value-changed and writes the pair through
     * on_reading_size_changed(), so there is one writer of the measure
     * rather than this function having its own.
     */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                              clawt_measure_unit_preset(unit));
}

static void
on_font_size_changed(GtkSpinButton *spin, gpointer user_data)
{
    ClawtWindow *self = user_data;
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(spin), "monospace"));
    gdouble points = gtk_spin_button_get_value(spin);

    if (monospace)
        clawt_appearance_set_monospace_size(self->appearance, points);
    else
        clawt_appearance_set_font_size(self->appearance, points);

    appearance_changed(self);
}

/*
 * The label under a font row: the chosen family, or what the desktop is
 * using when nothing is chosen.
 *
 * Naming the system font rather than saying "Default" matters, because
 * the two states look identical on screen and only one of them keeps
 * following the desktop when it changes.
 */
static void
update_font_row(ClawtWindow *self, GtkWidget *row, gboolean monospace)
{
    const gchar *chosen =
        monospace ? clawt_appearance_get_monospace_font(self->appearance)
                  : clawt_appearance_get_font(self->appearance);

    if (chosen != NULL) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), chosen);
        return;
    }

    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(row),
        monospace ? "Whatever the desktop uses for monospace"
                  : "Whatever the desktop uses");
}

static void
on_font_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    g_autoptr(PangoFontDescription) description = NULL;
    g_autoptr(GError) error = NULL;

    description = gtk_font_dialog_choose_font_finish(
        GTK_FONT_DIALOG(source), result, &error);

    /* Dismissing the chooser is not a failure worth a toast. */
    if (description == NULL)
        return;

    if (monospace)
        clawt_appearance_set_monospace_font(
            self->appearance, pango_font_description_get_family(description));
    else
        clawt_appearance_set_font(
            self->appearance, pango_font_description_get_family(description));

    /*
     * The size comes with the family from a font chooser, and ignoring
     * it would mean picking "Cantarell 14" and getting Cantarell at
     * whatever size was already set -- which reads as the size control
     * being broken.
     */
    if (pango_font_description_get_size(description) > 0) {
        gdouble points =
            (gdouble)pango_font_description_get_size(description) / PANGO_SCALE;
        GtkWidget *spin = g_object_get_data(G_OBJECT(row), "spin");

        if (monospace)
            clawt_appearance_set_monospace_size(self->appearance, points);
        else
            clawt_appearance_set_font_size(self->appearance, points);

        /*
         * Set on the widget too, so the spin button agrees with what was
         * just chosen.  ::value-changed then fires and saves, which is
         * why this happens before the explicit save below rather than
         * after it.
         */
        if (spin != NULL)
            gtk_spin_button_set_value(
                GTK_SPIN_BUTTON(spin),
                monospace
                    ? clawt_appearance_get_monospace_size(self->appearance)
                    : clawt_appearance_get_font_size(self->appearance));
    }

    update_font_row(self, row, monospace);
    appearance_changed(self);
}

static void
on_choose_font(GtkButton *button, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    GtkFontDialog *chooser = gtk_font_dialog_new();
    g_autoptr(PangoFontDescription) current = NULL;
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    const gchar *family =
        monospace ? clawt_appearance_get_monospace_font(self->appearance)
                  : clawt_appearance_get_font(self->appearance);

    (void)button;

    gtk_font_dialog_set_title(chooser, monospace ? "Code font"
                                                 : "Interface font");

    if (family != NULL)
        current = pango_font_description_from_string(family);

    gtk_font_dialog_choose_font(chooser, GTK_WINDOW(self), current, NULL,
                                on_font_chosen, row);
    g_object_unref(chooser);
}

/*
 * Back to the desktop's own font.
 *
 * Worth its own button: clearing a font chooser is not something a font
 * chooser offers, so without this a person who tried a font could never
 * get back to following their desktop -- only to naming whatever it
 * happens to use today, which is a different and worse thing.
 */
static void
on_clear_font(GtkButton *button, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    GtkWidget *spin = g_object_get_data(G_OBJECT(row), "spin");

    (void)button;

    if (monospace) {
        clawt_appearance_set_monospace_font(self->appearance, NULL);
        clawt_appearance_set_monospace_size(self->appearance, 0);
    } else {
        clawt_appearance_set_font(self->appearance, NULL);
        clawt_appearance_set_font_size(self->appearance, 0);
    }

    if (spin != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 0);

    update_font_row(self, row, monospace);
    appearance_changed(self);
}

/*
 * One font row: a family with a chooser, and a size beside it.
 *
 * The size is a separate control from the chooser's own because 0 has to
 * be reachable, and 0 means "the desktop's size" -- which no font chooser
 * has a way to express.
 */
static GtkWidget *
build_font_group(ClawtWindow *self, const gchar *title,
                 const gchar *description, gboolean monospace)
{
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *row = adw_action_row_new();
    GtkWidget *size_row = adw_action_row_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *choose = gtk_button_new_with_label("Choose\342\200\246");
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    /*
     * Half-point steps, one decimal shown.
     *
     * Whole points looked tidier and could not express a size people
     * actually run: Emacs states a pixel size, and 18px lands on 13.6pt
     * -- which a whole-number control silently rounds to 14, so the file
     * and the dialog disagree about what is set.
     */
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 48, 0.5);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          description);

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Font");

    g_object_set_data(G_OBJECT(row), "window", self);
    g_object_set_data(G_OBJECT(row), "monospace",
                      GINT_TO_POINTER(monospace));
    g_object_set_data(G_OBJECT(row), "spin", spin);

    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(clear, "flat");
    gtk_widget_set_tooltip_text(clear, "Follow the desktop again");

    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_font), row);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_font), row);

    gtk_box_append(GTK_BOX(buttons), choose);
    gtk_box_append(GTK_BOX(buttons), clear);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), buttons);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(size_row), "Size");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(size_row),
                                "0 follows the desktop");

    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 1);
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(spin),
        monospace ? clawt_appearance_get_monospace_size(self->appearance)
                  : clawt_appearance_get_font_size(self->appearance));
    g_object_set_data(G_OBJECT(spin), "monospace",
                      GINT_TO_POINTER(monospace));
    g_signal_connect(spin, "value-changed",
                     G_CALLBACK(on_font_size_changed), self);

    adw_action_row_add_suffix(ADW_ACTION_ROW(size_row), spin);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), size_row);

    update_font_row(self, row, monospace);

    return group;
}

/*
 * How wide the conversation runs, and how far apart the turns sit.
 *
 * The measure was a constant in C for the whole life of the client, so
 * the single thing about reading most likely to be wrong for a given
 * person and screen was the one thing they could not change.  Whatever
 * number is chosen is right for one reader on one display.
 */
static GtkWidget *
build_reading_group(ClawtWindow *self)
{
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *unit_row = adw_combo_row_new();
    GtkWidget *measure_row = adw_action_row_new();
    GtkWidget *gap_row = adw_action_row_new();
    GtkWidget *measure_spin = gtk_spin_button_new_with_range(1, 2, 1);
    GtkWidget *gap_spin = gtk_spin_button_new_with_range(
        0, CLAWT_APPEARANCE_MAX_RUN_SPACING, 2);
    g_autoptr(GtkStringList) unit_names = gtk_string_list_new(NULL);
    ClawtMeasureUnit unit =
        clawt_appearance_get_measure_unit(self->appearance);
    gint amount = clawt_appearance_get_measure_amount(self->appearance);
    guint selected = 0;
    guint u;

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Reading");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The conversation takes nine tenths of the window unless you say "
        "otherwise. A share follows the window; a character count follows "
        "your font; a pixel width follows neither and is exact.");

    /*
     * Built by walking the library's units rather than naming them
     * here.  A list of an option's values written into a client is how
     * a colour scheme came to be selectable in this client and not the
     * other, and `make parity` reported OK throughout because a choice
     * like this sends no IPC frame and answers no slash command.
     */
    for (u = 0; u < clawt_measure_unit_count(); u++) {
        ClawtMeasureUnit at = clawt_measure_unit_nth(u);

        gtk_string_list_append(unit_names,
                               clawt_measure_unit_label(at));

        if (at == unit)
            selected = u;
    }

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(unit_row),
                                  "Column width");
    adw_combo_row_set_model(ADW_COMBO_ROW(unit_row),
                            G_LIST_MODEL(unit_names));
    adw_combo_row_set_selected(ADW_COMBO_ROW(unit_row), selected);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), unit_row);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(measure_row),
                                  "How much");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(measure_row),
        "percent of the window, characters a line, or pixels");
    gtk_widget_set_valign(measure_spin, GTK_ALIGN_CENTER);

    /*
     * The range belongs to the unit, so it is set before the value and
     * again on every unit change.  A spin button built at the pixel
     * range and then handed a percentage would clamp 90 up to 320 and
     * report it back as the setting -- the shape this file already
     * records for a combo box that cannot say "something else".
     */
    if (unit == CLAWT_MEASURE_DEFAULT) {
        gtk_widget_set_sensitive(measure_spin, FALSE);
    } else {
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(measure_spin),
                                       clawt_measure_unit_step(unit),
                                       clawt_measure_unit_step(unit) * 5);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(measure_spin),
                                  clawt_measure_unit_min(unit),
                                  clawt_measure_unit_max(unit));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(measure_spin), amount);
    }

    g_object_set_data(G_OBJECT(measure_spin), "unit-combo", unit_row);
    g_object_set_data(G_OBJECT(unit_row), "amount-spin", measure_spin);
    g_signal_connect(measure_spin, "value-changed",
                     G_CALLBACK(on_reading_size_changed), self);
    g_signal_connect(unit_row, "notify::selected",
                     G_CALLBACK(on_measure_unit_selected), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(measure_row), measure_spin);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), measure_row);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(gap_row),
                                  "Gap between runs");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(gap_row),
                                "0 follows the shipped gap");
    gtk_widget_set_valign(gap_spin, GTK_ALIGN_CENTER);
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(gap_spin),
        clawt_appearance_get_run_spacing(self->appearance));
    g_object_set_data(G_OBJECT(gap_spin), "run-gap", GINT_TO_POINTER(TRUE));
    g_signal_connect(gap_spin, "value-changed",
                     G_CALLBACK(on_reading_size_changed), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(gap_row), gap_spin);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), gap_row);

    return group;
}

GtkWidget *
clawt_gtk_build_appearance_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *theme_group = adw_preferences_group_new();
    GtkWidget *theme_row = adw_combo_row_new();
    g_autoptr(GtkStringList) theme_names = gtk_string_list_new(NULL);
    guint selected = 0;
    guint t;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Appearance");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "applications-graphics-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(theme_group),
                                    "Theme");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(theme_group),
        "Kept on this machine rather than in clawtilla.yaml. The client "
        "can switch between daemons while it runs, and fonts that came "
        "from a daemon's config would change when you connected to "
        "another one.");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row),
                                  "Colour scheme");
    /*
     * Built from the library's list, so a palette added there appears
     * here without this file being touched -- and cannot appear here and
     * not in the web client, which builds its own select the same way.
     */
    for (t = 0; t < clawt_appearance_scheme_count(); t++) {
        gtk_string_list_append(theme_names,
                               clawt_appearance_scheme_nth_label(t));

        if (g_strcmp0(clawt_appearance_scheme_nth_nick(t),
                      clawt_appearance_get_scheme(self->appearance)) == 0)
            selected = t;
    }

    adw_combo_row_set_model(ADW_COMBO_ROW(theme_row),
                            G_LIST_MODEL(g_object_ref(theme_names)));

    adw_combo_row_set_selected(ADW_COMBO_ROW(theme_row), selected);

    /*
     * Connected after the initial selection is set, or setting it would
     * fire the handler and save the file on every open.
     */
    g_signal_connect(theme_row, "notify::selected",
                     G_CALLBACK(on_theme_selected), self);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(theme_group), theme_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(theme_group));

    adw_preferences_page_add(
        ADW_PREFERENCES_PAGE(page),
        ADW_PREFERENCES_GROUP(build_font_group(
            self, "Interface",
            "Everything but code: the agent list, messages, dialogs.",
            FALSE)));

    adw_preferences_page_add(
        ADW_PREFERENCES_PAGE(page),
        ADW_PREFERENCES_GROUP(build_font_group(
            self, "Code",
            "Code blocks and inline code in a conversation, and the "
            "output of the exec console.",
            TRUE)));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(build_reading_group(self)));

    return page;
}
