/*
 * lrg-settings.c - The Settings dialog: a page list and the page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * GTK's Settings is an AdwPreferencesDialog: one page per icon in a
 * header switcher.  Here it is one large dialog with the pages listed
 * down the left and the chosen page on the right, in GTK's order --
 * Cloud images, Teams, Spending, Integrations, Connectors, Shared
 * folders, Appearance, Devices -- plus Connections, which GTK keeps in a menu of
 * its own and this client keeps beside the rest so the banner's
 * "Connections…" lands on a page rather than a second kind of window.
 *
 * Built once and kept.  The dialog is re-presented rather than rebuilt,
 * so each page's widgets (and the static state each page file keeps
 * about them) live for the process; what a page *shows* is re-read from
 * the daemon every time it is shown, by its refresh, which is the same
 * moment GTK reads its settings pages -- when somebody opens them.
 */

#include "clawt-lrg.h"

#define N_SETTINGS_PAGES (10)

typedef struct {
    LrgDialog  *dialog;      /* our reference; re-presented, never rebuilt */
    LrgWidget  *switcher;
    LrgWidget  *tabs[N_SETTINGS_PAGES];
    gboolean    built[N_SETTINGS_PAGES];
    guint       current;
} Settings;

static Settings settings;

const ClawtLrgSettingsPage *const *
clawt_lrg_settings_pages(void)
{
    static const ClawtLrgSettingsPage *pages[N_SETTINGS_PAGES + 1];

    /*
     * Filled on first use rather than as a static initialiser: each page
     * is a function in its own file, and calling them here keeps the
     * order in one place -- GTK's, then Connections.
     */
    if (pages[0] == NULL) {
        pages[0] = clawt_lrg_settings_images_page();
        pages[1] = clawt_lrg_settings_teams_page();
        pages[2] = clawt_lrg_settings_spending_page();
        pages[3] = clawt_lrg_settings_orchestration_page();
        pages[4] = clawt_lrg_settings_integrations_page();
        pages[5] = clawt_lrg_settings_connectors_page();
        pages[6] = clawt_lrg_settings_folders_page();
        pages[7] = clawt_lrg_settings_appearance_page();
        pages[8] = clawt_lrg_settings_devices_page();
        pages[9] = clawt_lrg_settings_connections_page();
        pages[10] = NULL;
    }

    return (const ClawtLrgSettingsPage *const *)pages;
}

/* Which page has @id; the first when there is none or it is NULL. */
static guint
page_index(const gchar *page_id)
{
    const ClawtLrgSettingsPage *const *pages = clawt_lrg_settings_pages();
    guint i;

    for (i = 0; page_id != NULL && pages[i] != NULL; i++) {
        if (g_strcmp0(pages[i]->id, page_id) == 0)
            return i;
    }

    return 0;
}

static void
show_page(ClawtLrgApp *app, guint index)
{
    const ClawtLrgSettingsPage *page = clawt_lrg_settings_pages()[index];
    guint i;

    g_return_if_fail(page != NULL);

    /*
     * Built the first time it is shown.  The cloud image page asks for
     * the catalogue and the spending page for a summary; building all
     * eight at once would pay for every one of those on the way to the
     * page somebody actually wanted.
     */
    if (!settings.built[index]) {
        LrgWidget *widget = page->build(app);

        lrg_switcher_add_named(LRG_SWITCHER(settings.switcher), widget,
                               page->id);
        g_object_unref(widget);
        settings.built[index] = TRUE;
    }

    settings.current = index;
    lrg_switcher_set_visible_child_name(LRG_SWITCHER(settings.switcher),
                                        page->id);

    for (i = 0; i < N_SETTINGS_PAGES; i++) {
        if (i == index) {
            lrg_widget_add_class(settings.tabs[i], "clawt-tab-active");
            lrg_widget_set_state_flags(settings.tabs[i],
                                       LRG_WIDGET_STATE_SELECTED);
        } else {
            lrg_widget_remove_class(settings.tabs[i], "clawt-tab-active");
            lrg_widget_unset_state_flags(settings.tabs[i],
                                         LRG_WIDGET_STATE_SELECTED);
        }
    }

    if (page->refresh != NULL)
        page->refresh(app);

    clawt_lrg_app_wake(app);
}

static void
on_tab_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = g_object_get_data(G_OBJECT(button), "clawt-app");

    show_page(app, GPOINTER_TO_UINT(user_data));
}

static void
build_dialog(ClawtLrgApp *app)
{
    const ClawtLrgSettingsPage *const *pages = clawt_lrg_settings_pages();
    LrgWidget *content = clawt_lrg_hbox(18.0f);
    LrgWidget *nav = clawt_lrg_vbox(4.0f);
    LrgSwitcher *switcher = lrg_switcher_new();
    gfloat scale = (gfloat)app->scale;
    gfloat width = 1120.0f * scale;
    gfloat height = 740.0f * scale;
    guint i;

    /*
     * As large as the window allows, up to a size where a paragraph
     * still reads as one: a settings page is mostly sentences, and the
     * integration editor alone has a dozen rows.
     */
    if (app->canvas != NULL) {
        gfloat canvas_w = lrg_widget_get_width(LRG_WIDGET(app->canvas));
        gfloat canvas_h = lrg_widget_get_height(LRG_WIDGET(app->canvas));

        if (canvas_w > 0.0f)
            width = MIN(width, canvas_w - 80.0f);

        if (canvas_h > 0.0f)
            height = MIN(height, canvas_h - 160.0f);
    }

    lrg_widget_add_class(nav, "clawt-chrome");
    lrg_container_set_padding(LRG_CONTAINER(nav), 10.0f);
    lrg_widget_set_min_size(nav, 200.0f * scale, 0.0f);
    lrg_widget_set_max_size(nav, 200.0f * scale, -1.0f);

    for (i = 0; pages[i] != NULL && i < N_SETTINGS_PAGES; i++) {
        LrgWidget *tab = clawt_lrg_button(pages[i]->title, "flat",
                                          G_CALLBACK(on_tab_clicked),
                                          GUINT_TO_POINTER(i));

        lrg_widget_add_class(tab, "clawt-tab");
        lrg_widget_set_halign(tab, LRG_ALIGN_FILL);
        g_object_set_data(G_OBJECT(tab), "clawt-app", app);
        settings.tabs[i] = tab;
        clawt_lrg_add(nav, tab);
        clawt_lrg_appear(app, tab, i);
    }

    clawt_lrg_take(content, nav);

    lrg_switcher_set_transition(switcher, LRG_SWITCHER_TRANSITION_CROSSFADE);
    lrg_widget_set_fill_weight(LRG_WIDGET(switcher), 1.0f);
    settings.switcher = LRG_WIDGET(switcher);
    clawt_lrg_take(content, LRG_WIDGET(switcher));

    lrg_widget_set_min_size(content, MAX(width, 640.0f),
                            MAX(height, 420.0f));
    lrg_widget_set_max_size(content, MAX(width, 640.0f),
                            MAX(height, 420.0f));

    settings.dialog = lrg_dialog_new("Settings", NULL);
    lrg_dialog_set_content(settings.dialog, content);
    lrg_dialog_add_button(settings.dialog, "Close",
                          LRG_DIALOG_RESPONSE_CANCEL, FALSE);
    g_object_unref(content);
}

void
clawt_lrg_open_settings(ClawtLrgApp *app, const gchar *page_id)
{
    g_return_if_fail(app != NULL);

    if (settings.dialog == NULL)
        build_dialog(app);

    /*
     * Already open: go to the page asked for.  The banner's
     * "Connections…" can be pressed with Settings showing, and a second
     * dialog over the first would be two copies of every page.
     */
    if (!lrg_dialog_get_presented(settings.dialog)) {
        lrg_widget_set_appear(LRG_WIDGET(settings.dialog), LRG_APPEAR_POP,
                              0.0f);
        lrg_dialog_present(settings.dialog, app->canvas);
    }

    show_page(app, page_index(page_id));
}
