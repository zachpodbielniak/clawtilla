/*
 * lrg-ui.c - The widget kit every page of clawtilla-lrg draws with
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Small constructors over libregnum's widgets, so every page builds the
 * same card, the same badge and the same confirmation the same way --
 * and so the look is decided by style classes the theme document in
 * lrg-theme-model.c paints, never by colours a page picks itself.
 *
 * Reference counting: every constructor here returns a new reference
 * (transfer full), like libregnum's own.  clawt_lrg_take() adds a child
 * and drops that reference in one call, which is what almost every
 * caller wants; clawt_lrg_add() keeps it, for a widget the page also
 * holds on to.
 */

#include "clawt-lrg.h"

#include <raylib.h>
#include <string.h>

/* ── Trees ────────────────────────────────────────────────────────── */

void
clawt_lrg_add(LrgWidget *parent, LrgWidget *child)
{
    g_return_if_fail(LRG_IS_CONTAINER(parent));
    g_return_if_fail(LRG_IS_WIDGET(child));

    lrg_container_add_child(LRG_CONTAINER(parent), child);
}

void
clawt_lrg_take(LrgWidget *parent, LrgWidget *child)
{
    if (child == NULL)
        return;

    clawt_lrg_add(parent, child);
    g_object_unref(child);
}

void
clawt_lrg_clear(LrgWidget *container)
{
    g_return_if_fail(LRG_IS_CONTAINER(container));

    lrg_container_remove_all(LRG_CONTAINER(container));
}

LrgWidget *
clawt_lrg_vbox(gfloat spacing)
{
    LrgVBox *box = lrg_vbox_new();

    lrg_container_set_spacing(LRG_CONTAINER(box), spacing);
    return LRG_WIDGET(box);
}

LrgWidget *
clawt_lrg_hbox(gfloat spacing)
{
    LrgHBox *box = lrg_hbox_new();

    lrg_container_set_spacing(LRG_CONTAINER(box), spacing);
    return LRG_WIDGET(box);
}

void
clawt_lrg_appear(ClawtLrgApp *app, LrgWidget *widget, guint index)
{
    g_return_if_fail(LRG_IS_WIDGET(widget));

    /*
     * Staggered, and capped: a list of two hundred rows that each waited
     * its turn would take seconds to finish arriving.  Reduced motion is
     * honoured by libregnum itself (motion "none" makes every appear
     * instant), so nothing here has to ask.
     */
    lrg_widget_set_appear(widget, LRG_APPEAR_FLY_UP,
                          0.03f * (gfloat)MIN(index, 12u));
    (void)app;
}

/* ── Text ─────────────────────────────────────────────────────────── */

LrgWidget *
clawt_lrg_label(const gchar *text, const gchar *style_class)
{
    LrgLabel *label = lrg_label_new((text != NULL) ? text : "");

    if (style_class != NULL)
        lrg_widget_add_class(LRG_WIDGET(label), style_class);

    return LRG_WIDGET(label);
}

LrgWidget *
clawt_lrg_paragraph(const gchar *text, const gchar *style_class)
{
    g_autofree gchar *escaped = clawt_lrg_markdown_escape(text);
    LrgMarkdownLabel *label = lrg_markdown_label_new(escaped);

    /*
     * LrgLabel draws one line.  A wrapping paragraph is a markdown label
     * whose every punctuation mark is escaped, so text a person or a
     * model wrote is shown as written: an underscore in a filename does
     * not start italics, and nothing in it can become a link.
     */
    if (style_class != NULL)
        lrg_widget_add_class(LRG_WIDGET(label), style_class);

    return LRG_WIDGET(label);
}

void
clawt_lrg_paragraph_set_text(LrgWidget *paragraph, const gchar *text)
{
    g_autofree gchar *escaped = clawt_lrg_markdown_escape(text);

    g_return_if_fail(LRG_IS_MARKDOWN_LABEL(paragraph));
    lrg_markdown_label_set_markdown(LRG_MARKDOWN_LABEL(paragraph), escaped);
}

static void
on_link_activated(LrgMarkdownLabel *label, const gchar *url, gpointer user_data)
{
    clawt_lrg_open_uri(user_data, url);
}

LrgWidget *
clawt_lrg_markdown(ClawtLrgApp *app, const gchar *body)
{
    LrgMarkdownLabel *label =
        lrg_markdown_label_new((body != NULL) ? body : "");

    g_signal_connect(label, "link-activated", G_CALLBACK(on_link_activated),
                     app);

    return LRG_WIDGET(label);
}

LrgWidget *
clawt_lrg_section(const gchar *title)
{
    LrgWidget *label = clawt_lrg_label(title, "heading");
    LrgInsets margin = { 4.0f, 14.0f, 4.0f, 4.0f };

    lrg_widget_set_margin(label, &margin);
    return label;
}

/* ── Controls ─────────────────────────────────────────────────────── */

LrgWidget *
clawt_lrg_button(const gchar *label, const gchar *style_class,
                 GCallback on_clicked, gpointer user_data)
{
    LrgButton *button = lrg_button_new(label);

    if (style_class != NULL)
        lrg_widget_add_class(LRG_WIDGET(button), style_class);

    if (on_clicked != NULL)
        g_signal_connect(button, "clicked", on_clicked, user_data);

    return LRG_WIDGET(button);
}

LrgWidget *
clawt_lrg_entry(const gchar *text, const gchar *placeholder)
{
    LrgTextInput *entry = lrg_text_input_new_with_placeholder(
        (placeholder != NULL) ? placeholder : "");

    if (text != NULL)
        lrg_text_input_set_text(entry, text);

    return LRG_WIDGET(entry);
}

const gchar *
clawt_lrg_entry_text(LrgWidget *entry)
{
    g_return_val_if_fail(LRG_IS_TEXT_INPUT(entry), "");

    return lrg_text_input_get_text(LRG_TEXT_INPUT(entry));
}

LrgWidget *
clawt_lrg_editor(const gchar *text, const gchar *language)
{
    LrgTextEditor *editor = lrg_text_editor_new();

    lrg_text_editor_set_wrap(editor, TRUE);
    lrg_text_editor_set_show_line_numbers(editor, language != NULL);

    if (language != NULL)
        lrg_text_editor_set_language(editor, language);

    if (text != NULL)
        lrg_text_editor_set_text(editor, text);

    lrg_widget_set_min_size(LRG_WIDGET(editor), 0.0f, 120.0f);
    return LRG_WIDGET(editor);
}

gchar *
clawt_lrg_editor_text(LrgWidget *editor)
{
    g_return_val_if_fail(LRG_IS_TEXT_EDITOR(editor), g_strdup(""));

    return lrg_text_editor_get_text(LRG_TEXT_EDITOR(editor));
}

/*
 * A combo keeps the ids beside it: LrgComboBox holds display strings
 * only, and a combo read back by its label breaks the day two choices
 * share one.
 */
#define COMBO_IDS_KEY "clawt-lrg-combo-ids"

LrgWidget *
clawt_lrg_combo(const gchar *const *ids, const gchar *const *labels,
                const gchar *selected)
{
    LrgComboBox *combo = lrg_combo_box_new();
    GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
    gint chosen = -1;
    guint i;

    for (i = 0; ids != NULL && ids[i] != NULL; i++) {
        const gchar *label = (labels != NULL && labels[i] != NULL)
            ? labels[i] : ids[i];

        lrg_combo_box_append(combo, label);
        g_ptr_array_add(kept, g_strdup(ids[i]));

        if (g_strcmp0(ids[i], selected) == 0)
            chosen = (gint)i;
    }

    /*
     * A current value the list does not have is added rather than
     * replaced (AGENTS.md: "A control that cannot represent the current
     * value will silently replace it").  It shows as itself, so saving
     * the form unchanged writes back what was there.
     */
    if (chosen < 0 && selected != NULL && *selected != '\0') {
        lrg_combo_box_append(combo, selected);
        g_ptr_array_add(kept, g_strdup(selected));
        chosen = (gint)kept->len - 1;
    }

    g_object_set_data_full(G_OBJECT(combo), COMBO_IDS_KEY, kept,
                           (GDestroyNotify)g_ptr_array_unref);

    if (chosen >= 0)
        lrg_combo_box_set_selected(combo, chosen);

    return LRG_WIDGET(combo);
}

const gchar *
clawt_lrg_combo_id(LrgWidget *combo)
{
    GPtrArray *kept;
    gint selected;

    g_return_val_if_fail(LRG_IS_COMBO_BOX(combo), NULL);

    kept = g_object_get_data(G_OBJECT(combo), COMBO_IDS_KEY);
    selected = lrg_combo_box_get_selected(LRG_COMBO_BOX(combo));

    if (kept == NULL || selected < 0 || (guint)selected >= kept->len)
        return NULL;

    return g_ptr_array_index(kept, (guint)selected);
}

LrgWidget *
clawt_lrg_switch(const gchar *label, gboolean active)
{
    LrgCheckbox *toggle =
        lrg_checkbox_new_with_style(label, LRG_CHECKBOX_STYLE_SWITCH);

    lrg_checkbox_set_checked(toggle, active);
    return LRG_WIDGET(toggle);
}

gboolean
clawt_lrg_switch_active(LrgWidget *toggle)
{
    g_return_val_if_fail(LRG_IS_CHECKBOX(toggle), FALSE);

    return lrg_checkbox_get_checked(LRG_CHECKBOX(toggle));
}

/* ── Surfaces ─────────────────────────────────────────────────────── */

LrgWidget *
clawt_lrg_card(LrgWidget *child)
{
    LrgPanel *card = lrg_panel_new();

    lrg_widget_add_class(LRG_WIDGET(card), "clawt-card");
    lrg_container_set_padding(LRG_CONTAINER(card), 16.0f);
    lrg_panel_set_fill_children(card, TRUE);

    if (child != NULL)
        clawt_lrg_take(LRG_WIDGET(card), child);

    return LRG_WIDGET(card);
}

LrgWidget *
clawt_lrg_badge(const gchar *text, const gchar *tone)
{
    LrgPanel *badge = lrg_panel_new();
    LrgWidget *label = clawt_lrg_label(text, "caption");
    LrgInsets padding = { 9.0f, 2.0f, 9.0f, 2.0f };

    lrg_widget_add_class(LRG_WIDGET(badge), clawt_lrg_tone_class(tone));
    lrg_widget_add_class(label, clawt_lrg_tone_class(tone));
    lrg_container_set_padding(LRG_CONTAINER(badge), 0.0f);

    /*
     * Sized from the label and its margin.  A panel that does not fill
     * its children measures as an empty box, so the pill came out the
     * default size and the word was drawn half outside it.
     */
    lrg_panel_set_fill_children(badge, TRUE);
    lrg_widget_set_margin(label, &padding);
    lrg_widget_set_halign(LRG_WIDGET(badge), LRG_ALIGN_START);
    lrg_widget_set_valign(LRG_WIDGET(badge), LRG_ALIGN_CENTER);
    clawt_lrg_take(LRG_WIDGET(badge), label);

    return LRG_WIDGET(badge);
}

LrgWidget *
clawt_lrg_empty_state(const gchar *title, const gchar *body)
{
    LrgWidget *box = clawt_lrg_vbox(8.0f);

    lrg_widget_set_halign(box, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(box, LRG_ALIGN_CENTER);
    clawt_lrg_take(box, clawt_lrg_label(title, "heading"));

    if (body != NULL)
        clawt_lrg_take(box, clawt_lrg_paragraph(body, "muted"));

    lrg_widget_set_appear(box, LRG_APPEAR_FADE, 0.05f);
    return box;
}

/* ── Rows ─────────────────────────────────────────────────────────── */

#define ROW_TEXT_KEY   "clawt-lrg-row-text"
#define ROW_PREFIX_KEY "clawt-lrg-row-prefix"
#define ROW_SUFFIX_KEY "clawt-lrg-row-suffix"

LrgWidget *
clawt_lrg_row(const gchar *title, const gchar *subtitle)
{
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgWidget *prefix = clawt_lrg_hbox(8.0f);
    LrgWidget *text = clawt_lrg_vbox(2.0f);
    LrgWidget *suffix = clawt_lrg_hbox(8.0f);

    lrg_widget_add_class(row, "clawt-row");
    lrg_container_set_padding(LRG_CONTAINER(row), 10.0f);
    lrg_widget_set_valign(prefix, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(suffix, LRG_ALIGN_CENTER);
    lrg_widget_set_fill_weight(text, 1.0f);

    clawt_lrg_take(text, clawt_lrg_label(title, NULL));

    /*
     * The subtitle wraps.  A one-line label reports its whole width as
     * its natural size, so one long subtitle -- "will never run: '0 8 *
     * * 1-5' is not a schedule: use manual, hourly, ..." -- widened the
     * row, the page and the window's whole column past the right edge,
     * taking the row's own buttons and the header's menu with it.  The
     * same bug GTK had with wrapping labels, from the other side.
     */
    if (subtitle != NULL && *subtitle != '\0')
        clawt_lrg_take(text, clawt_lrg_paragraph(subtitle, "muted"));

    clawt_lrg_add(row, prefix);
    clawt_lrg_add(row, text);
    clawt_lrg_add(row, suffix);

    g_object_set_data_full(G_OBJECT(row), ROW_PREFIX_KEY, prefix,
                           g_object_unref);
    g_object_set_data_full(G_OBJECT(row), ROW_SUFFIX_KEY, suffix,
                           g_object_unref);
    g_object_set_data(G_OBJECT(row), ROW_TEXT_KEY, text);
    g_object_unref(text);

    return row;
}

LrgWidget *
clawt_lrg_row_subtitle(LrgWidget *row)
{
    LrgWidget *text = g_object_get_data(G_OBJECT(row), ROW_TEXT_KEY);
    GList *children;

    g_return_val_if_fail(text != NULL, NULL);

    /* the title is first; a subtitle, when there is one, is second */
    children = lrg_container_get_children(LRG_CONTAINER(text));
    return (children != NULL && children->next != NULL)
        ? LRG_WIDGET(children->next->data) : NULL;
}

void
clawt_lrg_row_add_prefix(LrgWidget *row, LrgWidget *prefix)
{
    LrgWidget *slot = g_object_get_data(G_OBJECT(row), ROW_PREFIX_KEY);

    g_return_if_fail(slot != NULL);
    clawt_lrg_take(slot, prefix);
}

void
clawt_lrg_row_add_suffix(LrgWidget *row, LrgWidget *suffix)
{
    LrgWidget *slot = g_object_get_data(G_OBJECT(row), ROW_SUFFIX_KEY);

    g_return_if_fail(slot != NULL);
    clawt_lrg_take(slot, suffix);
}

/* ── Pages ────────────────────────────────────────────────────────── */

#define PAGE_SUBTITLE_KEY "clawt-lrg-page-subtitle"
#define PAGE_TOOLBAR_KEY  "clawt-lrg-page-toolbar"
#define PAGE_BODY_KEY     "clawt-lrg-page-body"

LrgWidget *
clawt_lrg_page_new(const gchar *title, const gchar *subtitle)
{
    LrgWidget *page = clawt_lrg_vbox(14.0f);
    LrgWidget *header = clawt_lrg_hbox(12.0f);
    LrgWidget *titles = clawt_lrg_vbox(2.0f);
    LrgWidget *subtitle_label = clawt_lrg_label(subtitle, "muted");
    LrgWidget *toolbar = clawt_lrg_hbox(8.0f);
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgWidget *body = clawt_lrg_vbox(10.0f);

    lrg_container_set_padding(LRG_CONTAINER(page), 22.0f);
    lrg_widget_set_fill_weight(titles, 1.0f);
    lrg_widget_set_valign(toolbar, LRG_ALIGN_CENTER);

    clawt_lrg_take(titles, clawt_lrg_label(title, "title"));
    clawt_lrg_add(titles, subtitle_label);
    lrg_widget_set_visible(subtitle_label, subtitle != NULL);

    clawt_lrg_take(header, titles);
    clawt_lrg_add(header, toolbar);
    clawt_lrg_take(page, header);

    /*
     * Vertical scrolling only.  A horizontally scrolling page is the
     * bug GTK had with wrapping labels; here nothing wraps by natural
     * width, but a page that could scroll sideways would still invite
     * a row to set the page's width.
     */
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_kinetic(scroll, TRUE);
    lrg_widget_set_fill_weight(LRG_WIDGET(scroll), 1.0f);
    lrg_scroll_box_set_child(scroll, body);
    clawt_lrg_take(page, LRG_WIDGET(scroll));

    g_object_set_data_full(G_OBJECT(page), PAGE_SUBTITLE_KEY, subtitle_label,
                           g_object_unref);
    g_object_set_data_full(G_OBJECT(page), PAGE_TOOLBAR_KEY, toolbar,
                           g_object_unref);
    g_object_set_data_full(G_OBJECT(page), PAGE_BODY_KEY, body,
                           g_object_unref);

    return page;
}

LrgWidget *
clawt_lrg_page_toolbar(LrgWidget *page)
{
    return g_object_get_data(G_OBJECT(page), PAGE_TOOLBAR_KEY);
}

LrgWidget *
clawt_lrg_page_body(LrgWidget *page)
{
    return g_object_get_data(G_OBJECT(page), PAGE_BODY_KEY);
}

void
clawt_lrg_page_set_subtitle(LrgWidget *page, const gchar *subtitle)
{
    LrgWidget *label = g_object_get_data(G_OBJECT(page), PAGE_SUBTITLE_KEY);

    g_return_if_fail(label != NULL);

    lrg_label_set_text(LRG_LABEL(label), (subtitle != NULL) ? subtitle : "");
    lrg_widget_set_visible(label, subtitle != NULL && *subtitle != '\0');
}

/* ── Forms ────────────────────────────────────────────────────────── */

LrgWidget *
clawt_lrg_form_new(void)
{
    return clawt_lrg_vbox(12.0f);
}

void
clawt_lrg_form_add(LrgWidget *form, const gchar *label, LrgWidget *control,
                   const gchar *hint)
{
    LrgWidget *field = clawt_lrg_vbox(4.0f);

    if (label != NULL)
        clawt_lrg_take(field, clawt_lrg_label(label, "caption"));

    clawt_lrg_take(field, control);

    if (hint != NULL && *hint != '\0')
        clawt_lrg_take(field, clawt_lrg_paragraph(hint, "muted"));

    clawt_lrg_take(form, field);
}

/* ── Dialogs ──────────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp        *app;
    ClawtLrgDialogFunc  func;
    gpointer            user_data;
    GDestroyNotify      destroy;
} DialogData;

static void
dialog_data_free(gpointer data)
{
    DialogData *d = data;

    if (d->destroy != NULL)
        d->destroy(d->user_data);

    g_free(d);
}

static void
on_dialog_response(LrgDialog *dialog, gint response, gpointer user_data)
{
    DialogData *d = user_data;
    gboolean close = TRUE;

    /*
     * Cancel and close never reach the callback: a form's handler is
     * about what to do with its answer, and every one of them would
     * otherwise open with the same "if cancelled, return" line.
     */
    if (response > 0 && d->func != NULL)
        close = d->func(d->app, dialog, response, d->user_data);

    if (close)
        lrg_dialog_close(dialog);

    clawt_lrg_app_wake(d->app);
}

LrgDialog *
clawt_lrg_dialog(ClawtLrgApp *app, const gchar *title, const gchar *body,
                 LrgWidget *content, const gchar *primary,
                 gboolean destructive, ClawtLrgDialogFunc func,
                 gpointer user_data, GDestroyNotify destroy)
{
    LrgDialog *dialog = lrg_dialog_new(title, body);
    DialogData *d = g_new0(DialogData, 1);
    LrgButton *button;

    d->app = app;
    d->func = func;
    d->user_data = user_data;
    d->destroy = destroy;

    if (content != NULL)
        lrg_dialog_set_content(dialog, content);

    lrg_dialog_add_button(dialog, "Cancel", LRG_DIALOG_RESPONSE_CANCEL, FALSE);

    if (primary != NULL) {
        button = lrg_dialog_add_button(dialog, primary, 1, TRUE);

        if (destructive) {
            lrg_widget_remove_class(LRG_WIDGET(button), "primary");
            lrg_widget_add_class(LRG_WIDGET(button), "danger");
        }
    }

    g_signal_connect_data(dialog, "response", G_CALLBACK(on_dialog_response),
                          d, (GClosureNotify)(void (*)(void))dialog_data_free,
                          0);

    lrg_widget_set_appear(LRG_WIDGET(dialog), LRG_APPEAR_POP, 0.0f);
    lrg_dialog_present(dialog, app->canvas);
    clawt_lrg_app_wake(app);

    return dialog;
}

typedef struct {
    ClawtLrgConfirmFunc func;
    gpointer            user_data;
    GDestroyNotify      destroy;
} ConfirmData;

static void
confirm_data_free(gpointer data)
{
    ConfirmData *c = data;

    if (c->destroy != NULL)
        c->destroy(c->user_data);

    g_free(c);
}

static gboolean
on_confirmed(ClawtLrgApp *app, LrgDialog *dialog, gint response,
             gpointer user_data)
{
    ConfirmData *c = user_data;

    if (c->func != NULL)
        c->func(app, c->user_data);

    return TRUE;
}

void
clawt_lrg_confirm(ClawtLrgApp *app, const gchar *title, const gchar *body,
                  const gchar *verb, gboolean destructive,
                  ClawtLrgConfirmFunc func, gpointer user_data,
                  GDestroyNotify destroy)
{
    ConfirmData *c = g_new0(ConfirmData, 1);

    c->func = func;
    c->user_data = user_data;
    c->destroy = destroy;

    g_object_unref(clawt_lrg_dialog(app, title, body, NULL, verb,
                                    destructive, on_confirmed,
                     c, confirm_data_free));
}

static gboolean
on_text_copied(ClawtLrgApp *app, LrgDialog *dialog, gint response,
               gpointer user_data)
{
    clawt_lrg_copy(app, user_data);
    clawt_lrg_toast(app, "Copied");
    return TRUE;
}

void
clawt_lrg_show_text(ClawtLrgApp *app, const gchar *title, const gchar *body,
                    const gchar *text)
{
    LrgWidget *view = clawt_lrg_editor(text, NULL);

    lrg_text_editor_set_read_only(LRG_TEXT_EDITOR(view), TRUE);
    lrg_widget_add_class(view, "clawt-terminal");
    lrg_widget_set_min_size(view, 520.0f, 160.0f);

    g_object_unref(clawt_lrg_dialog(app, title, body, view, "Copy", FALSE,
                                    on_text_copied,
                     g_strdup(text), g_free));
    g_object_unref(view);
}

void
clawt_lrg_popup_menu(ClawtLrgApp *app, LrgWidget *owner, GMenuModel *model)
{
    LrgMenu *menu = lrg_menu_new_from_model(model);
    Vector2 pointer = GetMousePosition();

    lrg_menu_popup_at_point(menu, owner, pointer.x, pointer.y);
    g_object_unref(menu);
    clawt_lrg_app_wake(app);
}

/* ── The desktop ──────────────────────────────────────────────────── */

gboolean
clawt_lrg_open_in_editor(ClawtLrgApp *app, const gchar *path)
{
    const gchar *editor = g_getenv("VISUAL");
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) child = NULL;
    GPtrArray *args;
    guint i;

    if (editor == NULL || *editor == '\0')
        editor = g_getenv("EDITOR");

    if (editor == NULL || *editor == '\0') {
        clawt_lrg_toast(app, "Set $VISUAL or $EDITOR to edit files");
        return FALSE;
    }

    /*
     * Lexed, not handed to a shell: the path is somebody's filename and
     * must arrive as one argument whatever is in it.  $EDITOR itself may
     * carry flags ("emacsclient -c"), which is what the lexing is for.
     */
    if (!g_shell_parse_argv(editor, NULL, &argv, &error)) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    args = g_ptr_array_new();

    for (i = 0; argv[i] != NULL; i++)
        g_ptr_array_add(args, argv[i]);

    g_ptr_array_add(args, (gpointer)path);
    g_ptr_array_add(args, NULL);

    child = g_subprocess_newv((const gchar *const *)args->pdata,
                              G_SUBPROCESS_FLAGS_NONE, &error);
    g_ptr_array_free(args, TRUE);

    if (child == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    return TRUE;
}

void
clawt_lrg_open_uri(ClawtLrgApp *app, const gchar *uri)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *scheme = NULL;

    if (uri == NULL)
        return;

    scheme = g_uri_parse_scheme(uri);

    /*
     * Model output decides what a link says, so it does not get to decide
     * what a click runs: a `javascript:`, `x-scheme-handler` or custom
     * scheme reaches whatever the desktop maps it to.
     */
    if (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0 &&
        g_strcmp0(scheme, "file") != 0) {
        clawt_lrg_toast(app, "Only http, https and file links open");
        return;
    }

    if (!g_app_info_launch_default_for_uri(uri, NULL, &error))
        clawt_lrg_toast(app, error->message);
}

void
clawt_lrg_copy(ClawtLrgApp *app, const gchar *text)
{
    SetClipboardText((text != NULL) ? text : "");
    (void)app;
}

gchar *
clawt_lrg_ago(gint64 timestamp_us)
{
    return clawt_time_ago_label(timestamp_us, g_get_real_time());
}

/* ── What is on screen, as text ───────────────────────────────────── */

static void
collect_texts(LrgWidget *widget, GPtrArray *out)
{
    guint n;
    guint i;

    if (widget == NULL || !lrg_widget_get_visible(widget))
        return;

    if (LRG_IS_LABEL(widget))
        g_ptr_array_add(out, g_strdup(lrg_label_get_text(LRG_LABEL(widget))));
    else if (LRG_IS_BUTTON(widget))
        g_ptr_array_add(out,
                        g_strdup(lrg_button_get_text(LRG_BUTTON(widget))));
    else if (LRG_IS_TEXT_INPUT(widget))
        g_ptr_array_add(out, g_strdup(lrg_text_input_get_text(
                                 LRG_TEXT_INPUT(widget))));
    else if (LRG_IS_TEXT_EDITOR(widget))
        g_ptr_array_add(out, lrg_text_editor_get_text(
                                 LRG_TEXT_EDITOR(widget)));
    else if (LRG_IS_MARKDOWN_LABEL(widget)) {
        LrgMarkdownLabel *markdown = LRG_MARKDOWN_LABEL(widget);

        for (i = 0; i < lrg_markdown_label_get_n_blocks(markdown); i++)
            g_ptr_array_add(out, g_strdup(
                lrg_markdown_label_get_block_text(markdown, i)));
    } else if (LRG_IS_DIALOG(widget)) {
        g_ptr_array_add(out, g_strdup(lrg_dialog_get_title(LRG_DIALOG(widget))));
        g_ptr_array_add(out, g_strdup(lrg_dialog_get_body(LRG_DIALOG(widget))));
    }

    if (!LRG_IS_CONTAINER(widget))
        return;

    /* a switcher draws one child; the others are not on screen */
    if (LRG_IS_SWITCHER(widget)) {
        collect_texts(lrg_switcher_get_visible_child(LRG_SWITCHER(widget)),
                      out);
        return;
    }

    n = lrg_container_get_child_count(LRG_CONTAINER(widget));

    for (i = 0; i < n; i++)
        collect_texts(lrg_container_get_child(LRG_CONTAINER(widget), i), out);
}

GPtrArray *
clawt_lrg_visible_texts(LrgCanvas *canvas)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    guint i;

    g_return_val_if_fail(LRG_IS_CANVAS(canvas), out);

    collect_texts(LRG_WIDGET(canvas), out);

    /* popups (dialogs, menus) are drawn over the tree, not in it */
    if (lrg_canvas_get_popup_count(canvas) > 0)
        collect_texts(lrg_canvas_get_top_popup(canvas), out);

    /* NULLs from widgets with no text are dropped, not kept as holes */
    for (i = out->len; i > 0; i--) {
        const gchar *text = g_ptr_array_index(out, i - 1);

        if (text == NULL || *text == '\0')
            g_ptr_array_remove_index(out, i - 1);
    }

    return out;
}
