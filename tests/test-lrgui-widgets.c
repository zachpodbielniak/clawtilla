/*
 * test-lrgui-widgets.c - The real clawtilla-lrg, laid out with no window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Builds the whole client -- shell, every page, every Settings page, the
 * dialogs -- on a libregnum canvas with no window and no GL context, and
 * lays it out.  A canvas lays out without a window (drawing is what needs
 * GL), so this is the client's own code, not a copy of it.
 *
 * The client is pointed at a socket that does not exist.  Every request
 * a page makes on being shown therefore fails, which is the path a page
 * is least likely to have been looked at on: it must not crash, must not
 * leave the page half-built, and must not toast "not connected" over the
 * banner that already says so.
 *
 * Structure is asserted through libregnum's inspector snapshot, the same
 * JSON `clawtilla-lrg --self-test` prints.
 *
 * Built only with LRG_BACKEND=1 (it links libregnum).
 */

#include "clawt-lrg.h"
#include "clawt-test-util.h"

#include <string.h>

static gchar *scratch = NULL;

typedef struct {
    ClawtLrgApp *app;
} Fixture;

static void
drain(ClawtLrgApp *app)
{
    guint i;

    /* every failed request answers on a later iteration */
    for (i = 0; i < 50; i++) {
        while (g_main_context_iteration(app->context, FALSE))
            ;
        g_usleep(1000);
    }
}

static gchar *
snapshot(ClawtLrgApp *app)
{
    g_autoptr(LrgUIInspector) inspector = lrg_ui_inspector_new();

    lrg_widget_set_size(LRG_WIDGET(app->canvas), 1280.0f, 800.0f);
    lrg_widget_set_size(app->backdrop, 1280.0f, 800.0f);
    lrg_widget_set_size(app->root, 1280.0f, 800.0f);
    lrg_widget_layout(LRG_WIDGET(app->canvas));

    lrg_ui_inspector_set_canvas(inspector, app->canvas);
    return lrg_ui_inspector_snapshot_json(inspector, FALSE);
}

/*
 * One app for the whole binary, as in the real client: every page keeps
 * its state in a static (one window per process, clients/lrg/clawt-lrg.h),
 * so building a second app in the same process would test a situation
 * the client never meets.  Each test starts from the Chat page.
 */
static ClawtLrgApp *shared = NULL;

static void
fixture_set_up(Fixture *fixture, gconstpointer user_data)
{
    if (shared == NULL) {
        g_autofree gchar *socket =
            g_build_filename(scratch, "absent.sock", NULL);
        g_autoptr(ClawtConnection) connection =
            clawt_connection_new_local("Test", socket);
        g_autoptr(ClawtClient) client =
            clawt_connection_create_client(connection);

        shared = clawt_lrg_app_new(client, connection);
        clawt_lrg_app_build(shared);
    }

    fixture->app = shared;
    clawt_lrg_show_page(shared, CLAWT_PAGE_CHAT);
    drain(shared);
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer user_data)
{
    /* close whatever a test left open, so the next starts clean */
    while (lrg_canvas_get_popup_count(fixture->app->canvas) > 0)
        lrg_canvas_dismiss_popup(fixture->app->canvas,
                                 lrg_canvas_get_top_popup(fixture->app->canvas));
}

static void
test_every_page_builds_and_lays_out(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    guint n = clawt_page_count();
    guint i;

    for (i = 0; i < n; i++) {
        ClawtPage page = (ClawtPage)i;
        const ClawtLrgPage *impl = clawt_lrg_page_for(page);
        g_autofree gchar *json = NULL;
        LrgWidget *visible;

        /* every page the library has, this client has */
        g_assert_nonnull(impl);
        g_assert_cmpint(impl->page, ==, page);
        g_assert_nonnull(impl->build);

        clawt_lrg_show_page(app, page);
        drain(app);
        json = snapshot(app);

        g_assert_cmpint(app->page, ==, page);

        visible = lrg_switcher_get_visible_child(LRG_SWITCHER(app->switcher));
        g_assert_nonnull(visible);
        g_assert_cmpfloat(lrg_widget_get_width(visible), >, 0.0f);
        g_assert_cmpfloat(lrg_widget_get_height(visible), >, 0.0f);

        /* the page is the switcher's child named for it */
        g_assert_cmpstr(lrg_switcher_get_visible_child_name(
                            LRG_SWITCHER(app->switcher)),
                        ==, clawt_page_nick(page));
        g_assert_nonnull(json);
    }
}

/* Whether any text on screen contains @needle. */
static gboolean
shows(ClawtLrgApp *app, const gchar *needle)
{
    g_autoptr(GPtrArray) texts = NULL;
    guint i;

    g_free(snapshot(app));
    texts = clawt_lrg_visible_texts(app->canvas);

    for (i = 0; i < texts->len; i++) {
        if (strstr(g_ptr_array_index(texts, i), needle) != NULL)
            return TRUE;
    }

    return FALSE;
}

/*
 * The first visible widget that reaches past @right, or NULL.  Hidden
 * widgets are skipped with everything under them, since a switcher's
 * other pages are laid out wherever they were left.
 */
static LrgWidget *
first_past(LrgWidget *widget, gfloat right)
{
    GList *l;

    if (!lrg_widget_get_visible(widget))
        return NULL;

    if (lrg_widget_get_world_x(widget) + lrg_widget_get_width(widget) >
        right + 0.5f)
        return widget;

    if (!LRG_IS_CONTAINER(widget))
        return NULL;

    for (l = lrg_container_get_children(LRG_CONTAINER(widget)); l != NULL;
         l = l->next) {
        LrgWidget *found = first_past(l->data, right);

        if (found != NULL)
            return found;
    }

    return NULL;
}

/* Lays the window out at @width x @height, as a resize to it would. */
static void
resize_to(ClawtLrgApp *app, gfloat width, gfloat height)
{
    lrg_widget_set_size(LRG_WIDGET(app->canvas), width, height);
    lrg_widget_set_size(app->backdrop, width, height);
    lrg_widget_set_size(app->root, width, height);
    clawt_lrg_window_resize(app, width, height);
    lrg_widget_layout(LRG_WIDGET(app->canvas));
    lrg_widget_layout(LRG_WIDGET(app->canvas));
}

/*
 * Every page fits the window: at the size it opens at, and after being
 * shrunk to the smallest the window allows.  The header once took a
 * chief of staff's whole one-line description and ran the island, the
 * alerts and the menu off the right edge; a box never took space back
 * from a child, and a label kept the width it had before a shrink.
 * Shrinking is part of the test because that is where the second
 * happened -- a window that opened small was fine.
 */
static void
test_every_page_fits_the_window(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    guint n = clawt_page_count();
    guint i;

    /*
     * A header with more to say than the window has room for: a room
     * with no agent is titled by its id, which nothing bounds.  With a
     * daemon this is the selected agent's description; disconnected, a
     * long room id is the same one-line label at the same width.
     */
    clawt_lrg_select_room(app, "a-room-whose-name-goes-on-for-much-longer-"
                               "than-any-window-is-wide-because-somebody-"
                               "typed-a-whole-sentence-into-the-id-field-"
                               "and-the-header-must-cut-it-short-not-run-"
                               "the-island-and-the-menu-off-the-edge");
    drain(app);

    for (i = 0; i < n; i++) {
        ClawtPage page = (ClawtPage)i;
        LrgWidget *past;

        clawt_lrg_show_page(app, page);
        drain(app);

        resize_to(app, 1600.0f, 900.0f);
        past = first_past(LRG_WIDGET(app->canvas), 1600.0f);
        if (past != NULL)
            g_error("%s: a %s ends at %.0f in a 1600-wide window",
                    clawt_page_nick(page), G_OBJECT_TYPE_NAME(past),
                    lrg_widget_get_world_x(past) + lrg_widget_get_width(past));

        resize_to(app, (gfloat)CLAWT_LRG_MIN_WIDTH,
                  (gfloat)CLAWT_LRG_MIN_HEIGHT);
        past = first_past(LRG_WIDGET(app->canvas),
                          (gfloat)CLAWT_LRG_MIN_WIDTH);
        if (past != NULL)
            g_error("%s: a %s ends at %.0f after shrinking to %d",
                    clawt_page_nick(page), G_OBJECT_TYPE_NAME(past),
                    lrg_widget_get_world_x(past) + lrg_widget_get_width(past),
                    CLAWT_LRG_MIN_WIDTH);
    }

    /* back to the size, and the selection, the other tests expect */
    clawt_lrg_select_room(app, NULL);
    drain(app);
    resize_to(app, 1280.0f, 800.0f);
}

/*
 * The sidebar folds away and comes back, and a narrow window makes it a
 * drawer over the page.  Wide, the choice is saved in appearance.yaml
 * (the next window opens the same way); narrow, it is not -- a small
 * window is not a decision about the sidebar.  Choosing a room from the
 * drawer closes it.
 */
static void
test_sidebar_toggles_and_drawers(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    g_autoptr(ClawtAppearance) saved = NULL;
    gfloat wide_column;

    /* wide: beside the page, then hidden, and saved */
    resize_to(app, 1600.0f, 900.0f);
    g_assert_true(clawt_lrg_window_sidebar_shown(app));
    g_assert_true(lrg_widget_get_parent(app->sidebar) != LRG_CONTAINER(app->canvas));
    wide_column = lrg_widget_get_width(app->switcher);

    clawt_lrg_window_toggle_sidebar(app);
    resize_to(app, 1600.0f, 900.0f);
    g_assert_false(clawt_lrg_window_sidebar_shown(app));
    /* its width went to the page */
    g_assert_cmpfloat(lrg_widget_get_width(app->switcher), >, wide_column);
    saved = clawt_appearance_load(NULL, NULL);
    g_assert_nonnull(saved);
    g_assert_true(clawt_appearance_get_sidebar_collapsed(saved));

    clawt_lrg_window_toggle_sidebar(app);
    resize_to(app, 1600.0f, 900.0f);
    g_assert_true(clawt_lrg_window_sidebar_shown(app));

    /* narrow: hidden whatever was saved, and the toggle opens a drawer */
    resize_to(app, 700.0f, 600.0f);
    g_assert_false(clawt_lrg_window_sidebar_shown(app));
    clawt_lrg_window_toggle_sidebar(app);
    resize_to(app, 700.0f, 600.0f);
    g_assert_true(clawt_lrg_window_sidebar_shown(app));
    g_assert_true(lrg_widget_get_parent(app->sidebar) == LRG_CONTAINER(app->canvas));
    g_assert_cmpfloat(lrg_widget_get_height(app->sidebar), ==, 600.0f);
    /* over the page, not beside it: the page keeps the window's width */
    g_assert_cmpfloat(lrg_widget_get_x(app->switcher), <, 10.0f);

    /* choosing from the drawer closes it */
    clawt_lrg_select_room(app, "standup");
    resize_to(app, 700.0f, 600.0f);
    g_assert_false(clawt_lrg_window_sidebar_shown(app));
    g_assert_true(lrg_widget_get_parent(app->sidebar) != LRG_CONTAINER(app->canvas));

    /* and wide again, it is beside the page as the person left it */
    clawt_lrg_select_room(app, NULL);
    resize_to(app, 1280.0f, 800.0f);
    g_assert_true(clawt_lrg_window_sidebar_shown(app));
}

static void
test_section_tabs_walk_the_library(Fixture *fixture, gconstpointer data)
{
    guint n = clawt_section_count();
    guint i;

    /* every section's name is drawn, read from the library's table */
    for (i = 0; i < n; i++)
        g_assert_true(shows(fixture->app, clawt_section_nth_label(i)));

    /* and only the visible page's text: Settings' pages are not on screen */
    g_assert_false(shows(fixture->app, "Cloud images"));
}

static void
test_disconnected_says_so_once(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    g_autofree gchar *json = NULL;

    clawt_lrg_update_island(app);
    clawt_lrg_update_banner(app);
    json = snapshot(app);
    g_assert_nonnull(json);

    /* the island and the banner say it; nothing else needs to */
    g_assert_true(shows(app, "Not connected"));
    g_assert_true(lrg_widget_get_visible(app->banner));
    g_assert_true(lrg_widget_has_class(app->island_widget,
                                       "clawt-island-alert"));

    /*
     * Every page's requests failed with NOT_CONNECTED while it was shown;
     * none of them may have reached the toast queue.
     */
    g_assert_null(app->last_toast);
}

static void
test_every_settings_page_builds(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    const ClawtLrgSettingsPage *const *pages = clawt_lrg_settings_pages();
    guint n = 0;
    guint i;

    g_assert_nonnull(pages);

    for (i = 0; pages[i] != NULL; i++) {
        g_assert_nonnull(pages[i]->id);
        g_assert_nonnull(pages[i]->title);
        g_assert_nonnull(pages[i]->build);

        clawt_lrg_open_settings(app, pages[i]->id);
        drain(app);
        g_free(snapshot(app));
        n++;
    }

    /* GTK's seven pages, and Connections */
    g_assert_cmpuint(n, ==, 8);
    g_assert_cmpstr(pages[n - 1]->id, ==, "connections");
}

static void
test_dialogs_open(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;

    clawt_lrg_new_agent_dialog(app);
    drain(app);
    g_free(snapshot(app));

    clawt_lrg_import_agent_dialog(app);
    drain(app);
    g_free(snapshot(app));

    clawt_lrg_new_room_dialog(app);
    drain(app);
    g_free(snapshot(app));

    clawt_lrg_navigation_open(app);
    drain(app);
    g_free(snapshot(app));

    g_assert_cmpuint(lrg_canvas_get_popup_count(app->canvas), >, 0);
}

static void
test_appearance_applies_live(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    g_autoptr(ClawtAppearance) appearance = clawt_appearance_new();
    /*
     * Held, not merely remembered: a freed theme's address is the first
     * the next allocation reuses, so comparing a bare pointer can say
     * "unchanged" about a window that was re-themed.
     */
    g_autoptr(LrgTheme) before = g_object_ref(app->theme);
    guint i;

    /* every scheme and material re-themes the running window */
    for (i = 0; i < clawt_appearance_scheme_count(); i++) {
        clawt_appearance_set_scheme(appearance,
                                    clawt_appearance_scheme_nth_nick(i));
        clawt_appearance_set_lrg_material(
            appearance, clawt_lrg_material_nth(i % clawt_lrg_material_count()));
        clawt_lrg_app_apply_appearance(app, appearance);
        g_assert_true(lrg_theme_get_default() == app->theme);
        g_assert_true(app->theme != before);
        g_assert_true(g_str_has_suffix(lrg_theme_get_name(app->theme),
                                       clawt_appearance_scheme_nth_nick(i)));
        g_free(snapshot(app));
    }

}

/*
 * Typing "/" with a fleet's worth of skills must not stall the window.
 *
 * The list was rebuilt from nothing on every keystroke, one wrapping
 * paragraph per row, and each row added to the box measured every row
 * before it -- with sixty skills and their descriptions that was seconds
 * per key, which reads as the window hanging the moment the list opens.
 */
static JsonNode *
many_skills(guint n)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "commands");
    json_builder_begin_array(builder);
    for (i = 0; i < n; i++) {
        g_autofree gchar *name = g_strdup_printf("skill-%02u", i);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
            "Use this skill when the work at hand calls for a long, careful, "
            "multi-step procedure that a model would otherwise improvise, "
            "including reading, planning, writing, testing and reporting back "
            "with the evidence for each step it took along the way.");
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}

static void
test_typing_a_slash_stays_quick(Fixture *fixture, gconstpointer data)
{
    ClawtLrgApp *app = fixture->app;
    g_autoptr(JsonNode) skills = many_skills(60);
    const gchar *typed[] = { "/", "/s", "/sk", "/ski", "/skil", "/skill", "/skill-", "/" };
    gint64 start;
    gint64 worst = 0;
    guint rows = 0;
    guint i;

    for (i = 0; i < G_N_ELEMENTS(typed); i++) {
        g_autofree gchar *json = NULL;
        gint64 took;

        start = g_get_monotonic_time();
        rows = clawt_lrg_chat_debug_type(app, typed[i], skills);
        json = snapshot(app);
        took = g_get_monotonic_time() - start;
        worst = MAX(worst, took);
    }

    g_test_message("typing a slash: worst keystroke %.1f ms, %u rows",
                   (gdouble)worst / 1000.0, rows);
    g_assert_cmpuint(rows, >, 0);
    g_assert_cmpint(worst, <, 100 * 1000);

    clawt_lrg_chat_debug_type(app, "", NULL);
}

int
main(int argc, char *argv[])
{
    gint result;

    /* before g_test_init(): the XDG getters cache on first use */
    scratch = g_dir_make_tmp("clawt-lrgui-widgets-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", scratch, TRUE);
    g_setenv("XDG_DATA_HOME", scratch, TRUE);
    g_setenv("XDG_CACHE_HOME", scratch, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add("/lrgui/widgets/every-page", Fixture, NULL, fixture_set_up,
               test_every_page_builds_and_lays_out, fixture_tear_down);
    g_test_add("/lrgui/widgets/every-page-fits", Fixture, NULL, fixture_set_up,
               test_every_page_fits_the_window, fixture_tear_down);
    g_test_add("/lrgui/widgets/sidebar-toggle", Fixture, NULL, fixture_set_up,
               test_sidebar_toggles_and_drawers, fixture_tear_down);
    g_test_add("/lrgui/widgets/section-tabs", Fixture, NULL, fixture_set_up,
               test_section_tabs_walk_the_library, fixture_tear_down);
    g_test_add("/lrgui/widgets/disconnected", Fixture, NULL, fixture_set_up,
               test_disconnected_says_so_once, fixture_tear_down);
    g_test_add("/lrgui/widgets/settings-pages", Fixture, NULL, fixture_set_up,
               test_every_settings_page_builds, fixture_tear_down);
    g_test_add("/lrgui/widgets/dialogs", Fixture, NULL, fixture_set_up,
               test_dialogs_open, fixture_tear_down);
    g_test_add("/lrgui/widgets/appearance-live", Fixture, NULL, fixture_set_up,
               test_appearance_applies_live, fixture_tear_down);

    g_test_add("/lrgui/widgets/typing-a-slash-stays-quick", Fixture, NULL,
               fixture_set_up, test_typing_a_slash_stays_quick, fixture_tear_down);

    result = g_test_run();

    g_clear_pointer(&shared, clawt_lrg_app_free);
    clawt_test_remove_tree(scratch);
    g_free(scratch);

    return result;
}
