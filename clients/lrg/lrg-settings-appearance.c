/*
 * lrg-settings-appearance.c - Settings: Appearance
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything GTK's appearance page has (clients/gtk/gtk-prefs-
 * appearance.c) -- the colour scheme with the palettes found on disk,
 * both fonts and their sizes, the column measure and the gap between
 * runs, descriptions in the agent list -- and the settings only this
 * client reads: motion, material, background, accent and scale.
 *
 * A live preview, as GTK's is: every control edits one ClawtAppearance
 * and hands it to clawt_lrg_app_apply_appearance() at once, so the
 * window restyles under the dialog as somebody drags.  It is saved too,
 * because a preview that looks applied and is lost on close is the
 * worst of both -- but on a short timer rather than per event, since a
 * colour wheel or a spin box being scrubbed changes sixty times a
 * second and each would otherwise be a write of appearance.yaml.
 *
 * Every list of values here is walked from the library -- schemes,
 * measure units, motions, materials, backgrounds -- never written out:
 * a copy of an option's values in a client is how a palette once
 * reached one client and not the other while `make parity` said OK.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

/* How long after the last change appearance.yaml is written. */
#define SAVE_DELAY_MS (400)

typedef struct {
    ClawtLrgApp     *app;
    ClawtAppearance *edited;          /* what the controls describe */
    gboolean         syncing;         /* a control is being set by us */
    GSource         *save_source;

    LrgWidget       *scheme_slot;
    LrgWidget       *palette_note;
    LrgWidget       *font_line[2];    /* [0] interface, [1] code */
    LrgWidget       *font_entry[2];
    LrgWidget       *font_size[2];
    LrgWidget       *measure_unit;
    LrgWidget       *measure_amount;
    LrgWidget       *scale_line;
    LrgWidget       *scale;
    LrgWidget       *accent;
    LrgWidget       *accent_palette;
} Appearance;

static Appearance looks;

/* ── Applying and saving ──────────────────────────────────────────── */

static gboolean
on_save_due(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GError) error = NULL;

    g_clear_pointer(&looks.save_source, g_source_unref);

    if (!clawt_appearance_save(looks.edited, NULL, &error))
        clawt_lrg_toast(app, error->message);

    return G_SOURCE_REMOVE;
}

/*
 * Applies now, saves shortly.  The timer is attached to the app's own
 * context, by name: g_timeout_add() takes the ambient one, which is the
 * mistake AGENTS.md lists six APIs for.
 */
static void
changed(void)
{
    ClawtLrgApp *app = looks.app;

    if (looks.syncing)
        return;

    clawt_lrg_app_apply_appearance(app, looks.edited);

    if (looks.save_source != NULL) {
        g_source_destroy(looks.save_source);
        g_clear_pointer(&looks.save_source, g_source_unref);
    }

    looks.save_source = g_timeout_source_new(SAVE_DELAY_MS);
    g_source_set_callback(looks.save_source, on_save_due, app, NULL);
    clawt_lrg_app_attach(app, looks.save_source);
}

/* ── Colour scheme ────────────────────────────────────────────────── */

static void
update_palette_note(void)
{
    g_autofree gchar *note = clawt_lrg_settings_palette_note(looks.edited);
    g_autofree gchar *escaped = clawt_lrg_markdown_escape(note);

    lrg_markdown_label_set_markdown(LRG_MARKDOWN_LABEL(looks.palette_note),
                                    escaped);
    lrg_widget_set_visible(looks.palette_note, note != NULL);
}

static void
on_scheme_changed(LrgComboBox *combo, gpointer user_data)
{
    const gchar *nick = clawt_lrg_combo_id(LRG_WIDGET(combo));

    /*
     * A nick from the library's own list, never an index into a copy of
     * it: the copy here once named four schemes while the web client's
     * named three.
     */
    clawt_appearance_set_scheme(looks.edited, nick);
    update_palette_note();
    changed();
}

/* The scheme combo, walked from clawt_appearance_scheme_*(). */
static void
fill_schemes(void)
{
    guint n = clawt_appearance_scheme_count();
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgWidget *combo;
    guint i;

    for (i = 0; i < n; i++) {
        g_ptr_array_add(ids, (gpointer)clawt_appearance_scheme_nth_nick(i));
        g_ptr_array_add(labels,
                        (gpointer)clawt_appearance_scheme_nth_label(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_clear(looks.scheme_slot);
    combo = clawt_lrg_combo((const gchar *const *)ids->pdata,
                            (const gchar *const *)labels->pdata,
                            clawt_appearance_get_scheme(looks.edited));
    lrg_widget_set_fill_weight(combo, 1.0f);

    /* connected after the selection, so building is not a change */
    g_signal_connect(combo, "changed", G_CALLBACK(on_scheme_changed), NULL);
    clawt_lrg_take(looks.scheme_slot, combo);
}

/*
 * Rescans the palette directory.  Discovery is cached in the library
 * (a readdir per settings page would be absurd), so a palette written
 * while the client runs appears only when somebody asks -- here.
 */
static void
on_reload_palettes(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    guint found = clawt_appearance_reload_palettes();
    g_autofree gchar *dir = clawt_appearance_palette_dir();
    g_autofree gchar *text = g_strdup_printf(
        "%u palette%s in %s.", found, found == 1 ? "" : "s", dir);

    /* a palette deleted from disk falls back to "system" in the setter */
    clawt_appearance_set_scheme(looks.edited,
                                clawt_appearance_get_scheme(looks.edited));
    fill_schemes();
    update_palette_note();
    clawt_lrg_toast(app, text);
    changed();
}

/* ── Fonts ────────────────────────────────────────────────────────── */

static void
update_font_line(guint which)
{
    const gchar *family = (which == 1)
        ? clawt_appearance_get_monospace_font(looks.edited)
        : clawt_appearance_get_font(looks.edited);

    lrg_label_set_text(LRG_LABEL(looks.font_line[which]),
                       clawt_lrg_settings_font_text(family, which == 1));
}

static void
on_font_typed(LrgTextInput *entry, gpointer user_data)
{
    guint which = GPOINTER_TO_UINT(user_data);
    g_autofree gchar *family =
        g_strdup(clawt_lrg_entry_text(LRG_WIDGET(entry)));

    if (looks.syncing)
        return;

    /* empty reaches NULL, never "": an empty family is invalid CSS */
    g_strstrip(family);

    if (which == 1)
        clawt_appearance_set_monospace_font(looks.edited,
                                            (*family != '\0') ? family : NULL);
    else
        clawt_appearance_set_font(looks.edited,
                                  (*family != '\0') ? family : NULL);

    update_font_line(which);
    changed();
}

static void
on_font_size_changed(LrgSpinBox *spin, gpointer user_data)
{
    guint which = GPOINTER_TO_UINT(user_data);
    gdouble points = lrg_spin_box_get_value(spin);

    if (looks.syncing)
        return;

    if (which == 1)
        clawt_appearance_set_monospace_size(looks.edited, points);
    else
        clawt_appearance_set_font_size(looks.edited, points);

    changed();
}

/*
 * Back to the desktop's font.  A font chooser has no way to say
 * "follow the desktop", so without this somebody who tried a font
 * could only ever name whatever the desktop uses today -- which looks
 * the same and stops following it.
 */
static void
on_font_clear(LrgButton *button, gpointer user_data)
{
    guint which = GPOINTER_TO_UINT(user_data);

    if (which == 1) {
        clawt_appearance_set_monospace_font(looks.edited, NULL);
        clawt_appearance_set_monospace_size(looks.edited, 0);
    } else {
        clawt_appearance_set_font(looks.edited, NULL);
        clawt_appearance_set_font_size(looks.edited, 0);
    }

    looks.syncing = TRUE;
    lrg_text_input_set_text(LRG_TEXT_INPUT(looks.font_entry[which]), "");
    lrg_spin_box_set_value(LRG_SPIN_BOX(looks.font_size[which]), 0.0);
    looks.syncing = FALSE;

    update_font_line(which);
    changed();
}

/* A card with a heading, a paragraph and a body to fill. */
static LrgWidget *
group_card(const gchar *title, const gchar *description,
           LrgWidget **out_body)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *body = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(box, clawt_lrg_label(title, "heading"));

    if (description != NULL)
        clawt_lrg_take(box, clawt_lrg_paragraph(description, "muted"));

    clawt_lrg_add(box, body);
    *out_body = body;
    g_object_unref(body);

    return clawt_lrg_card(box);
}

/*
 * One font: a family and a size.  The size is a control of its own
 * because 0 must be reachable, and 0 means the desktop's size -- which
 * no font chooser can express.  Half-point steps, because 18px is
 * 13.6pt and a whole-number control rounds what Emacs users run.
 */
static LrgWidget *
build_font_card(guint which)
{
    gboolean monospace = which == 1;
    LrgWidget *body;
    LrgWidget *card = group_card(
        monospace ? "Code" : "Interface",
        monospace ? "Code blocks and inline code in a conversation, and the "
                    "output of the exec console."
                  : "Everything but code: the agent list, messages, "
                    "dialogs.", &body);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *family_row = clawt_lrg_hbox(8.0f);
    LrgSpinBox *size = lrg_spin_box_new_with_range(0.0, 48.0, 0.5);
    const gchar *family = monospace
        ? clawt_appearance_get_monospace_font(looks.edited)
        : clawt_appearance_get_font(looks.edited);

    looks.font_line[which] = clawt_lrg_label("", "muted");
    clawt_lrg_add(body, looks.font_line[which]);
    g_object_unref(looks.font_line[which]);
    update_font_line(which);

    looks.font_entry[which] = clawt_lrg_entry(
        family, monospace ? "JetBrains Mono" : "Inter");
    lrg_widget_set_fill_weight(looks.font_entry[which], 1.0f);
    g_signal_connect(looks.font_entry[which], "text-changed",
                     G_CALLBACK(on_font_typed), GUINT_TO_POINTER(which));
    clawt_lrg_add(family_row, looks.font_entry[which]);
    g_object_unref(looks.font_entry[which]);
    clawt_lrg_take(family_row,
                   clawt_lrg_button("Follow the desktop", "flat",
                                    G_CALLBACK(on_font_clear),
                                    GUINT_TO_POINTER(which)));
    clawt_lrg_form_add(form, "Font", family_row,
                       "A family name. This client draws with its own "
                       "font and reads the size; the family is kept for "
                       "the other two clients, which share this file.");

    lrg_spin_box_set_digits(size, 1);
    lrg_spin_box_set_value(size, monospace
        ? clawt_appearance_get_monospace_size(looks.edited)
        : clawt_appearance_get_font_size(looks.edited));
    g_signal_connect(size, "value-changed", G_CALLBACK(on_font_size_changed),
                     GUINT_TO_POINTER(which));
    looks.font_size[which] = LRG_WIDGET(size);
    clawt_lrg_form_add(form, "Size", LRG_WIDGET(size),
                       "0 follows the desktop");

    clawt_lrg_take(body, form);
    return card;
}

/* ── Reading ──────────────────────────────────────────────────────── */

static void
on_measure_amount_changed(LrgSpinBox *spin, gpointer user_data)
{
    if (looks.syncing)
        return;

    /*
     * The amount is meaningless without its unit, so the unit is read
     * off the combo rather than remembered here: 90 is nine tenths of
     * the window, ninety characters or ninety pixels.
     */
    clawt_appearance_set_measure(
        looks.edited,
        clawt_measure_unit_from_nick(clawt_lrg_combo_id(looks.measure_unit)),
        (gint)lrg_spin_box_get_value(spin));
    changed();
}

/* The amount's range and step belong to the unit, so they move with it. */
static void
range_amount_for(ClawtMeasureUnit unit)
{
    LrgSpinBox *spin = LRG_SPIN_BOX(looks.measure_amount);

    lrg_widget_set_enabled(looks.measure_amount,
                           unit != CLAWT_MEASURE_DEFAULT);

    if (unit == CLAWT_MEASURE_DEFAULT)
        return;

    lrg_spin_box_set_step(spin, clawt_measure_unit_step(unit));
    lrg_spin_box_set_range(spin, clawt_measure_unit_min(unit),
                           clawt_measure_unit_max(unit));
}

/*
 * Picking what the number means.  Re-seeded from the unit's preset
 * rather than carried across, because the old number is in the old
 * unit -- and re-ranged under `syncing`, because clamping the shown
 * value into the new range emits value-changed with an intermediate
 * nobody chose (GTK wrote the pixel floor to the file that way).
 */
static void
on_measure_unit_changed(LrgComboBox *combo, gpointer user_data)
{
    ClawtMeasureUnit unit =
        clawt_measure_unit_from_nick(clawt_lrg_combo_id(LRG_WIDGET(combo)));

    looks.syncing = TRUE;
    range_amount_for(unit);

    if (unit != CLAWT_MEASURE_DEFAULT)
        lrg_spin_box_set_value(LRG_SPIN_BOX(looks.measure_amount),
                               clawt_measure_unit_preset(unit));

    looks.syncing = FALSE;

    clawt_appearance_set_measure(looks.edited, unit,
                                 (unit == CLAWT_MEASURE_DEFAULT)
                                     ? 0 : clawt_measure_unit_preset(unit));
    changed();
}

static void
on_run_gap_changed(LrgSpinBox *spin, gpointer user_data)
{
    if (looks.syncing)
        return;

    clawt_appearance_set_run_spacing(looks.edited,
                                     (gint)lrg_spin_box_get_value(spin));
    changed();
}

/*
 * How wide the conversation runs, and how far apart the turns sit:
 * whatever number is right is right for one reader on one display.
 */
static LrgWidget *
build_reading_card(void)
{
    LrgWidget *body;
    LrgWidget *card = group_card(
        "Reading",
        "The conversation takes nine tenths of the window unless you say "
        "otherwise. A share follows the window; a character count follows "
        "your font; a pixel width follows neither and is exact.", &body);
    LrgWidget *form = clawt_lrg_form_new();
    ClawtMeasureUnit unit =
        clawt_appearance_get_measure_unit(looks.edited);
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgSpinBox *amount = lrg_spin_box_new_with_range(0.0, 1.0, 1.0);
    LrgSpinBox *gap = lrg_spin_box_new_with_range(
        0.0, CLAWT_APPEARANCE_MAX_RUN_SPACING, 2.0);
    guint u;

    /* walked from the library: a choice like this sends no frame */
    for (u = 0; u < clawt_measure_unit_count(); u++) {
        ClawtMeasureUnit at = clawt_measure_unit_nth(u);

        g_ptr_array_add(ids, (gpointer)clawt_measure_unit_nick(at));
        g_ptr_array_add(labels, (gpointer)clawt_measure_unit_label(at));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    looks.measure_unit = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                         (const gchar *const *)labels->pdata,
                                         clawt_measure_unit_nick(unit));
    g_signal_connect(looks.measure_unit, "changed",
                     G_CALLBACK(on_measure_unit_changed), NULL);
    clawt_lrg_form_add(form, "Column width", looks.measure_unit, NULL);

    /* the range before the value, or 90 percent clamps up to 320 pixels */
    looks.measure_amount = LRG_WIDGET(amount);
    looks.syncing = TRUE;
    range_amount_for(unit);

    if (unit != CLAWT_MEASURE_DEFAULT)
        lrg_spin_box_set_value(amount,
                               clawt_appearance_get_measure_amount(
                                   looks.edited));

    looks.syncing = FALSE;
    g_signal_connect(amount, "value-changed",
                     G_CALLBACK(on_measure_amount_changed), NULL);
    clawt_lrg_form_add(form, "How much", LRG_WIDGET(amount),
                       "percent of the window, characters a line, or "
                       "pixels, whichever the unit above says");

    lrg_spin_box_set_value(gap,
                           clawt_appearance_get_run_spacing(looks.edited));
    g_signal_connect(gap, "value-changed", G_CALLBACK(on_run_gap_changed),
                     NULL);
    clawt_lrg_form_add(form, "Gap between runs", LRG_WIDGET(gap),
                       "0 follows the shipped gap");

    clawt_lrg_take(body, form);
    return card;
}

/* ── Agent list ───────────────────────────────────────────────────── */

static void
on_descriptions_toggled(LrgCheckbox *toggle, gboolean active,
                        gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    clawt_appearance_set_show_descriptions(looks.edited, active);
    changed();

    /*
     * The sidebar is drawn from the last agent.list and nothing about
     * that changed, so without a redraw the switch would save, be right
     * on the next start, and appear to do nothing now.
     */
    clawt_lrg_sidebar_refresh(app);
}

static LrgWidget *
build_agent_list_card(ClawtLrgApp *app)
{
    LrgWidget *body;
    LrgWidget *card = group_card(
        "Agent list",
        "A description is what tells you which agent to ask. A fleet of "
        "them is also the reason the list stops fitting on a screen.",
        &body);
    LrgWidget *toggle = clawt_lrg_switch(
        "Descriptions in the list",
        clawt_appearance_get_show_descriptions(looks.edited));

    g_signal_connect(toggle, "toggled", G_CALLBACK(on_descriptions_toggled),
                     app);
    clawt_lrg_take(body, toggle);
    clawt_lrg_take(body, clawt_lrg_paragraph(
        "Off keeps them on the pointer rather than losing them.", "muted"));

    return card;
}

/* ── Only this client ─────────────────────────────────────────────── */

static void
on_motion_changed(LrgComboBox *combo, gpointer user_data)
{
    ClawtLrgMotion motion = CLAWT_LRG_MOTION_DEFAULT;

    clawt_lrg_motion_from_nick(clawt_lrg_combo_id(LRG_WIDGET(combo)),
                               &motion);
    clawt_appearance_set_lrg_motion(looks.edited, motion);
    changed();
}

static void
on_material_changed(LrgComboBox *combo, gpointer user_data)
{
    ClawtLrgMaterial material = CLAWT_LRG_MATERIAL_DEFAULT;

    clawt_lrg_material_from_nick(clawt_lrg_combo_id(LRG_WIDGET(combo)),
                                 &material);
    clawt_appearance_set_lrg_material(looks.edited, material);
    changed();
}

static void
on_background_changed(LrgComboBox *combo, gpointer user_data)
{
    ClawtLrgBackground background = CLAWT_LRG_BACKGROUND_DEFAULT;

    clawt_lrg_background_from_nick(clawt_lrg_combo_id(LRG_WIDGET(combo)),
                                   &background);
    clawt_appearance_set_lrg_background(looks.edited, background);
    changed();
}

static void
on_accent_changed(LrgColorPicker *picker, gpointer user_data)
{
    g_autofree gchar *hex = NULL;

    if (looks.syncing)
        return;

    hex = lrg_color_picker_get_hex(picker);
    clawt_appearance_set_lrg_accent(looks.edited, hex);

    /* choosing a colour is choosing not to take the palette's */
    looks.syncing = TRUE;
    lrg_checkbox_set_checked(LRG_CHECKBOX(looks.accent_palette), FALSE);
    looks.syncing = FALSE;
    changed();
}

static void
on_accent_palette_toggled(LrgCheckbox *toggle, gboolean active,
                          gpointer user_data)
{
    g_autofree gchar *hex = NULL;

    if (looks.syncing)
        return;

    /* empty is the palette's accent; off takes the picker's colour */
    if (!active)
        hex = lrg_color_picker_get_hex(LRG_COLOR_PICKER(looks.accent));

    clawt_appearance_set_lrg_accent(looks.edited, hex);
    changed();
}

static void
update_scale_line(void)
{
    g_autofree gchar *text = clawt_lrg_settings_scale_text(
        clawt_appearance_get_lrg_scale(looks.edited));

    lrg_label_set_text(LRG_LABEL(looks.scale_line), text);
}

static void
on_scale_changed(LrgSpinBox *spin, gpointer user_data)
{
    gdouble proposed = lrg_spin_box_get_value(spin);
    gdouble stored;

    if (looks.syncing)
        return;

    /*
     * Between 0 and the minimum there is nothing to choose, so stepping
     * down out of the range follows the monitor and stepping up from 0
     * lands on the minimum (clawt_lrg_settings_scale_step()).
     */
    stored = clawt_lrg_settings_scale_step(
        clawt_appearance_get_lrg_scale(looks.edited), proposed);
    clawt_appearance_set_lrg_scale(looks.edited, stored);

    if (stored != proposed) {
        looks.syncing = TRUE;
        lrg_spin_box_set_value(spin, stored);
        looks.syncing = FALSE;
    }

    update_scale_line();
    changed();
}

/* A combo over one of the lrg-only enumerations, by nick. */
static LrgWidget *
lrg_choice(guint n, const gchar *(*nick_at)(guint),
           const gchar *(*label_at)(guint), const gchar *selected,
           GCallback on_changed)
{
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgWidget *combo;
    guint i;

    for (i = 0; i < n; i++) {
        g_ptr_array_add(ids, (gpointer)nick_at(i));
        g_ptr_array_add(labels, (gpointer)label_at(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    combo = clawt_lrg_combo((const gchar *const *)ids->pdata,
                            (const gchar *const *)labels->pdata, selected);
    g_signal_connect(combo, "changed", on_changed, NULL);

    return combo;
}

/*
 * Indexed adapters, so each enumeration is walked by its own _count()
 * and _nth() and named by its own _nick() and _label().
 */
static const gchar *motion_nick_at(guint n)
{ return clawt_lrg_motion_nick(clawt_lrg_motion_nth(n)); }
static const gchar *motion_label_at(guint n)
{ return clawt_lrg_motion_label(clawt_lrg_motion_nth(n)); }
static const gchar *material_nick_at(guint n)
{ return clawt_lrg_material_nick(clawt_lrg_material_nth(n)); }
static const gchar *material_label_at(guint n)
{ return clawt_lrg_material_label(clawt_lrg_material_nth(n)); }
static const gchar *background_nick_at(guint n)
{ return clawt_lrg_background_nick(clawt_lrg_background_nth(n)); }
static const gchar *background_label_at(guint n)
{ return clawt_lrg_background_label(clawt_lrg_background_nth(n)); }

static LrgWidget *
build_lrg_card(void)
{
    LrgWidget *body;
    LrgWidget *card = group_card(
        "Only this client reads these",
        "Motion, material, background, accent and scale are clawtilla-lrg's "
        "own. They live in the same appearance.yaml; GTK follows the "
        "desktop and the web client the browser, and both ignore them.",
        &body);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *accent_row = clawt_lrg_hbox(12.0f);
    LrgColorPicker *picker = lrg_color_picker_new();
    LrgSpinBox *scale = lrg_spin_box_new_with_range(0.0, CLAWT_LRG_SCALE_MAX,
                                                    0.05);
    const gchar *accent = clawt_appearance_get_lrg_accent(looks.edited);
    LrgWidget *scale_row = clawt_lrg_hbox(12.0f);

    lrg_widget_add_class(card, "clawt-card-selected");

    clawt_lrg_form_add(form, "Motion", lrg_choice(
        clawt_lrg_motion_count(), motion_nick_at, motion_label_at,
        clawt_lrg_motion_nick(clawt_appearance_get_lrg_motion(looks.edited)),
        G_CALLBACK(on_motion_changed)),
        "None turns off every entrance, transition and the moving "
        "background, not just the springs.");

    clawt_lrg_form_add(form, "Material", lrg_choice(
        clawt_lrg_material_count(), material_nick_at, material_label_at,
        clawt_lrg_material_nick(
            clawt_appearance_get_lrg_material(looks.edited)),
        G_CALLBACK(on_material_changed)),
        "What the sidebar and header are made of. Flat is the cheapest to "
        "draw.");

    clawt_lrg_form_add(form, "Background", lrg_choice(
        clawt_lrg_background_count(), background_nick_at,
        background_label_at,
        clawt_lrg_background_nick(
            clawt_appearance_get_lrg_background(looks.edited)),
        G_CALLBACK(on_background_changed)),
        "Dimmed toward the window colour so text keeps its contrast, and "
        "still when motion is none.");

    /* the accent: a swatch that opens a picker; empty is the palette's */
    lrg_color_picker_set_popup_mode(picker, TRUE);
    lrg_color_picker_set_show_alpha(picker, FALSE);

    if (accent != NULL)
        lrg_color_picker_set_hex(picker, accent);

    g_signal_connect(picker, "color-changed", G_CALLBACK(on_accent_changed),
                     NULL);
    looks.accent = LRG_WIDGET(picker);
    clawt_lrg_take(accent_row, LRG_WIDGET(picker));

    looks.accent_palette = clawt_lrg_switch("The palette's accent",
                                            accent == NULL);
    g_signal_connect(looks.accent_palette, "toggled",
                     G_CALLBACK(on_accent_palette_toggled), NULL);
    lrg_widget_set_valign(looks.accent_palette, LRG_ALIGN_CENTER);
    clawt_lrg_take(accent_row, looks.accent_palette);
    clawt_lrg_form_add(form, "Accent", accent_row,
                       "Glows, selection and the background's tint.");

    /* scale: 0 follows the monitor */
    lrg_spin_box_set_digits(scale, 2);
    lrg_spin_box_set_value(scale,
                           clawt_appearance_get_lrg_scale(looks.edited));
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_changed),
                     NULL);
    looks.scale = LRG_WIDGET(scale);
    clawt_lrg_take(scale_row, LRG_WIDGET(scale));
    looks.scale_line = clawt_lrg_label("", "muted");
    lrg_widget_set_valign(looks.scale_line, LRG_ALIGN_CENTER);
    clawt_lrg_add(scale_row, looks.scale_line);
    g_object_unref(looks.scale_line);
    update_scale_line();
    clawt_lrg_form_add(form, "Scale", scale_row,
                       "0 follows the monitor. Everything is drawn at this "
                       "size, text and layout alike.");

    clawt_lrg_take(body, form);
    return card;
}

/*
 * A strip of what the theme paints, so a change is seen where it lands
 * and not only on whichever page happens to be behind the dialog.
 */
static LrgWidget *
build_preview_card(ClawtLrgApp *app)
{
    LrgWidget *body;
    LrgWidget *card = group_card("Preview", NULL, &body);
    LrgWidget *badges = clawt_lrg_hbox(8.0f);
    LrgWidget *buttons = clawt_lrg_hbox(8.0f);
    LrgWidget *bubble = clawt_lrg_vbox(4.0f);
    static const gchar *const tones[] = {
        "good", "warn", "bad", "info", "neutral", NULL
    };
    guint i;

    for (i = 0; tones[i] != NULL; i++)
        clawt_lrg_take(badges, clawt_lrg_badge(tones[i], tones[i]));

    clawt_lrg_take(buttons, clawt_lrg_button("Primary", "primary", NULL,
                                             NULL));
    clawt_lrg_take(buttons, clawt_lrg_button("Plain", NULL, NULL, NULL));
    clawt_lrg_take(buttons, clawt_lrg_button("Danger", "danger", NULL,
                                             NULL));

    lrg_widget_add_class(bubble, "clawt-bubble-mine");
    lrg_container_set_padding(LRG_CONTAINER(bubble), 12.0f);
    clawt_lrg_take(bubble, clawt_lrg_markdown(
        app, "A message with `inline code`, **bold** and a [link]"
             "(https://example.org)."));

    clawt_lrg_take(body, badges);
    clawt_lrg_take(body, buttons);
    clawt_lrg_take(body, bubble);

    return card;
}

/* ── The page ─────────────────────────────────────────────────────── */

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Appearance", "Kept on this machine, applied as you change it");
    LrgWidget *body = clawt_lrg_page_body(page);
    LrgWidget *theme_body;
    LrgWidget *scheme_row = clawt_lrg_hbox(8.0f);
    LrgWidget *theme_form = clawt_lrg_form_new();
    guint shown = 0;

    looks.app = app;
    looks.edited = (app->appearance != NULL)
        ? clawt_appearance_copy(app->appearance) : clawt_appearance_new();

    clawt_lrg_take(body, group_card(
        "Theme",
        "Kept on this machine rather than in clawtilla.yaml. The client can "
        "switch between daemons while it runs, and a look that came from a "
        "daemon's config would change when you connected to another one.",
        &theme_body));

    looks.scheme_slot = clawt_lrg_hbox(0.0f);
    lrg_widget_set_fill_weight(looks.scheme_slot, 1.0f);
    clawt_lrg_add(scheme_row, looks.scheme_slot);
    g_object_unref(looks.scheme_slot);
    fill_schemes();
    clawt_lrg_take(scheme_row,
                   clawt_lrg_button("Reload palettes", "flat",
                                    G_CALLBACK(on_reload_palettes), app));

    {
        g_autofree gchar *dir = clawt_appearance_palette_dir();
        g_autofree gchar *hint = g_strdup_printf(
            "A palette is a CSS file of colours in %s, shared with both "
            "other clients.", dir);

        clawt_lrg_form_add(theme_form, "Colour scheme", scheme_row, hint);
    }

    looks.palette_note = clawt_lrg_markdown(app, "");
    lrg_widget_add_class(looks.palette_note, "muted");
    clawt_lrg_add(theme_form, looks.palette_note);
    g_object_unref(looks.palette_note);
    update_palette_note();
    clawt_lrg_take(theme_body, theme_form);

    {
        LrgWidget *cards[] = {
            build_preview_card(app),
            build_lrg_card(),
            build_font_card(0),
            build_font_card(1),
            build_reading_card(),
            build_agent_list_card(app)
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS(cards); i++) {
            clawt_lrg_appear(app, cards[i], ++shown);
            clawt_lrg_take(body, cards[i]);
        }
    }

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    /*
     * Nothing to ask the daemon: appearance.yaml is this machine's.  The
     * palette note is re-read because a palette file may have been
     * edited since the page was built.
     */
    if (looks.palette_note != NULL)
        update_palette_note();
}

static const ClawtLrgSettingsPage page_impl = {
    "appearance", "Appearance", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_appearance_page(void)
{
    return &page_impl;
}
