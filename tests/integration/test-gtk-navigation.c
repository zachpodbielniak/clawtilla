/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Integration only: needs a display and the fixture documented in ui-usability.org.
 * Include the picker to exercise the actual filtering and activation callbacks;
 * the rest of the real client is linked by the dedicated Makefile target. */
#include "../../clients/gtk/gtk-navigation.c"

/* A finite drain lets closed dialogs finish disposing without a timing sleep. */
static void
drain_events(void)
{
	guint i;

	for (i = 0; i < 100 && g_main_context_pending(NULL); i++)
		g_main_context_iteration(NULL, FALSE);
}

/* Reopen through the registered action, exactly as the button does. */
static Navigation *
open_picker(ClawtWindow *window)
{
	AdwDialog *dialog;

	drain_events();
	g_action_group_activate_action(G_ACTION_GROUP(window), "navigate", NULL);
	dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
	g_assert_nonnull(dialog);
	return (Navigation *)g_object_get_data(G_OBJECT(dialog), "navigation");
}

/* Check real selection, draft ownership, collapsed teams, room transitions,
 * and the no-results path; rendered markup cannot establish these outcomes. */
static void
test_navigation(void)
{
	g_autoptr(AdwApplication) app = NULL;
	g_autoptr(ClawtConnection) connection = NULL;
	g_autoptr(ClawtClient) client = NULL;
	g_autoptr(GError) error = NULL;
	ClawtWindow *window;
	Navigation *nav;
	GtkListBoxRow *selected;
	GtkWidget *row;
	guint visible = 0;
	const gchar *socket = g_getenv("CLAWT_UI_TEST_SOCKET");

	g_assert_nonnull(socket);
	app = adw_application_new("org.clawtilla.NavigationTest", G_APPLICATION_NON_UNIQUE);
	g_assert_true(g_application_register(G_APPLICATION(app), NULL, &error));
	g_assert_no_error(error);
	connection = clawt_connection_new_local("UI test", socket);
	client = clawt_connection_create_client(connection);
	g_assert_true(clawt_client_connect(client, &error));
	g_assert_no_error(error);
	window = clawt_window_new(app, client, connection);
	gtk_window_present(GTK_WINDOW(window));
	g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);

	clawt_gtk_select_agent(window, "alice");
	clawt_gtk_entry_set_text(window, "Draft survives navigation");
	if (window->collapsed_teams == NULL)
		window->collapsed_teams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_add(window->collapsed_teams, g_strdup("engineering"));
	clawt_gtk_refresh_agents(window);

	nav = open_picker(window);
	gtk_editable_set_text(GTK_EDITABLE(nav->entry), "bob");
	g_signal_emit_by_name(nav->entry, "activate");
	g_assert_cmpstr(window->selected_agent, ==, "bob");
	g_assert_false(g_hash_table_contains(window->collapsed_teams, "engineering"));
	selected = gtk_list_box_get_selected_row(window->sidebar);
	g_assert_nonnull(selected);
	g_assert_cmpstr(g_object_get_data(G_OBJECT(selected), "agent-id"), ==, "bob");

	nav = open_picker(window);
	gtk_editable_set_text(GTK_EDITABLE(nav->entry), "alice");
	g_signal_emit_by_name(nav->entry, "activate");
	{
		g_autofree gchar *draft = clawt_gtk_entry_text(window);
		g_assert_cmpstr(draft, ==, "Draft survives navigation");
	}
	nav = open_picker(window);
	gtk_editable_set_text(GTK_EDITABLE(nav->entry), "standup");
	g_signal_emit_by_name(nav->entry, "activate");
	g_assert_null(window->selected_agent);
	g_assert_cmpstr(window->selected_room_entry, ==, "standup");
	g_assert_cmpint(clawt_gtk_current_page(window), ==, CLAWT_PAGE_CHAT);

	nav = open_picker(window);
	gtk_editable_set_text(GTK_EDITABLE(nav->entry), "no-such-destination");
	for (row = gtk_widget_get_first_child(GTK_WIDGET(nav->list)); row != NULL;
	     row = gtk_widget_get_next_sibling(row)) {
		if (GTK_IS_LIST_BOX_ROW(row) && gtk_widget_get_child_visible(row))
			visible++;
	}
	g_assert_cmpuint(visible, ==, 0);
	g_signal_emit_by_name(nav->entry, "activate");
	g_assert_cmpstr(window->selected_room_entry, ==, "standup");
	g_signal_emit_by_name(nav->entry, "stop-search");
	drain_events();
	g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));

	clawt_gtk_select_agent(window, "alice");
	nav = open_picker(window);
	gtk_editable_set_text(GTK_EDITABLE(nav->entry), "tasks");
	g_signal_emit_by_name(nav->entry, "activate");
	g_assert_cmpint(clawt_gtk_current_page(window), ==, CLAWT_PAGE_TASKS);
	drain_events();

	/* Exercise breakpoint allocation after the interaction checks. These
	 * explicit allocations do not resize the native surface, so they must
	 * not be interleaved with dialogs and frame-clock snapshots. */
	gtk_widget_allocate(GTK_WIDGET(window), 600, 760, -1, NULL);
	g_assert_true(adw_overlay_split_view_get_collapsed(window->split));
	g_assert_true(adw_overlay_split_view_get_collapsed(window->alerts_split));
	gtk_widget_allocate(GTK_WIDGET(window), 1000, 760, -1, NULL);
	g_assert_false(adw_overlay_split_view_get_collapsed(window->split));
	g_assert_true(adw_overlay_split_view_get_collapsed(window->alerts_split));
	gtk_widget_allocate(GTK_WIDGET(window), 1280, 720, -1, NULL);
	g_assert_false(adw_overlay_split_view_get_collapsed(window->split));
	g_assert_false(adw_overlay_split_view_get_collapsed(window->alerts_split));
	gtk_window_destroy(GTK_WINDOW(window));
	drain_events();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	adw_init();
	g_test_add_func("/gtk/navigation", test_navigation);
	return g_test_run();
}
