/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "clawt-window-private.h"

/* A snapshot owns its row targets: a fleet refresh can replace the sidebar
 * while this dialog is open. Never keep pointers into its widgets or JSON. */
typedef struct {
	ClawtWindow *window;
	AdwDialog *dialog;
	GtkWidget *entry;
	GtkListBox *list;
} Navigation;

/* Filter locally so typing never blocks on an IPC request. */
static gboolean
navigation_filter(GtkListBoxRow *row, gpointer user_data)
{
	Navigation *nav = (Navigation *)user_data;
	const gchar *text = (const gchar *)g_object_get_data(G_OBJECT(row), "search");

	return clawt_navigation_matches(
		gtk_editable_get_text(GTK_EDITABLE(nav->entry)), text);
}

/* Invalidation also lets GtkListBox display its no-results placeholder. */
static void
navigation_changed(GtkEditable *entry, gpointer user_data)
{
	Navigation *nav = (Navigation *)user_data;

	(void)entry;
	gtk_list_box_invalidate_filter(nav->list);
}

/* Copy the target before closing; the dialog owns the row and callback data. */
static void
navigation_activate(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
	Navigation *nav = (Navigation *)user_data;
	ClawtWindow *self = nav->window;
	g_autofree gchar *target = g_strdup(g_object_get_data(G_OBJECT(row), "target"));
	gint kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"));
	const gchar *team = (const gchar *)g_object_get_data(G_OBJECT(row), "team");

	(void)list;
	if (kind != 2 && self->collapsed_teams != NULL)
		g_hash_table_remove(self->collapsed_teams, team != NULL ? team : "");
	adw_dialog_close(nav->dialog);
	if (kind == 0) {
		clawt_gtk_select_agent(self, target);
	} else if (kind == 1) {
		clawt_gtk_select_room(self, target);
	} else {
		clawt_gtk_show_page(self, clawt_page_from_nick(target));
	}
	if (kind != 2) {
		clawt_gtk_show_page(self, CLAWT_PAGE_CHAT);
		clawt_gtk_refresh_agents(self);
	}
}

/* Return opens the first visible result, without requiring a pointer. */
static void
navigation_enter(GtkSearchEntry *entry, gpointer user_data)
{
	Navigation *nav = (Navigation *)user_data;
	GtkWidget *row;

	(void)entry;
	for (row = gtk_widget_get_first_child(GTK_WIDGET(nav->list)); row != NULL;
	     row = gtk_widget_get_next_sibling(row)) {
		if (GTK_IS_LIST_BOX_ROW(row) && gtk_widget_get_child_visible(row)) {
			navigation_activate(nav->list, GTK_LIST_BOX_ROW(row), nav);
			return;
		}
	}
}

/* Text is plain, including names supplied by the daemon. */
static void
navigation_add(Navigation *nav, const gchar *name, const gchar *detail,
               const gchar *target, gint kind, const gchar *team)
{
	GtkWidget *row = adw_action_row_new();
	GtkWidget *icon = gtk_image_new_from_icon_name(
		kind == 0 ? "avatar-default-symbolic" :
		kind == 1 ? "system-users-symbolic" : "go-next-symbolic");

	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), detail);
	adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
	adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
	adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	g_object_set_data_full(G_OBJECT(row), "target", g_strdup(target), g_free);
	g_object_set_data_full(G_OBJECT(row), "team", g_strdup(team), g_free);
	g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(kind));
	g_object_set_data_full(G_OBJECT(row), "search",
		g_strdup_printf("%s %s %s", name, detail, target), g_free);
	gtk_list_box_append(nav->list, row);
}

/* Build once per opening; all declared rooms are included even in folded teams. */
static void
on_navigate(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	ClawtWindow *self = (ClawtWindow *)user_data;
	Navigation *nav;
	GtkWidget *toolbar;
	GtkWidget *body;
	GtkWidget *scroll;
	GtkWidget *empty;
	g_autoptr(JsonNode) agents_reply = NULL;
	g_autoptr(JsonNode) rooms_reply = NULL;
	JsonArray *agents = NULL;
	JsonArray *rooms = NULL;
	guint i;

	(void)action;
	(void)parameter;
	if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)) != NULL)
		return;
	nav = g_new0(Navigation, 1);
	nav->window = self;
	nav->dialog = adw_dialog_new();
	adw_dialog_set_title(nav->dialog, "Go to…");
	adw_dialog_set_content_width(nav->dialog, 520);
	adw_dialog_set_content_height(nav->dialog, 540);
	g_object_set_data_full(G_OBJECT(nav->dialog), "navigation", nav, g_free);
	toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(body, 18);
	gtk_widget_set_margin_end(body, 18);
	gtk_widget_set_margin_bottom(body, 18);
	nav->entry = gtk_search_entry_new();
	g_object_set(nav->entry, "placeholder-text", "Find agents, rooms and pages", NULL);
	gtk_widget_set_tooltip_text(nav->entry, "Search by name, ID, team or page. Enter opens the first result.");
	gtk_box_append(GTK_BOX(body), nav->entry);
	nav->list = GTK_LIST_BOX(gtk_list_box_new());
	gtk_list_box_set_selection_mode(nav->list, GTK_SELECTION_NONE);
	gtk_widget_set_valign(GTK_WIDGET(nav->list), GTK_ALIGN_START);
	gtk_widget_add_css_class(GTK_WIDGET(nav->list), "boxed-list");
	empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "system-search-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No matches");
	adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Try a name, ID, team or page, or clear the search.");
	gtk_widget_add_css_class(empty, "compact");
	gtk_list_box_set_placeholder(nav->list, empty);
	gtk_list_box_set_filter_func(nav->list, navigation_filter, nav, NULL);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(nav->list));
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(body), scroll);
	gtk_box_append(GTK_BOX(body), gtk_label_new("Enter to open · Tab to browse · Esc to close"));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), body);
	adw_dialog_set_child(nav->dialog, toolbar);

	/* Quiet failures remain visible in the dialog instead of producing a
     * toast behind it; page navigation remains available while disconnected. */
	agents_reply = clawt_client_request(self->client, "agent.list", NULL, NULL);
	rooms_reply = clawt_client_request(self->client, "room.list", NULL, NULL);
	if (agents_reply != NULL)
		agents = json_object_get_array_member(clawt_payload_of(agents_reply), "agents");
	if (rooms_reply != NULL)
		rooms = json_object_get_array_member(clawt_payload_of(rooms_reply), "rooms");
	if (agents_reply == NULL || rooms_reply == NULL) {
		GtkWidget *notice = gtk_label_new("Some destinations are unavailable. Close and reopen to retry.");
		gtk_label_set_wrap(GTK_LABEL(notice), TRUE);
		gtk_widget_add_css_class(notice, "dim-label");
		gtk_box_prepend(GTK_BOX(body), notice);
	}
	for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
		JsonObject *agent = json_array_get_object_element(agents, i);
		const gchar *id = clawt_json_string(agent, "id", "");
		g_autofree gchar *detail = g_strdup_printf("Agent · %s · %s", id,
			clawt_json_string(agent, "team", ""));
		navigation_add(nav, clawt_json_string(agent, "name", id), detail, id, 0,
			clawt_json_string(agent, "team", ""));
	}
	for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
		JsonObject *room = json_array_get_object_element(rooms, i);
		const gchar *id = clawt_json_string(room, "id", "");
		g_autofree gchar *detail = g_strdup_printf("Room · %s · %s", id,
			clawt_json_string(room, "team", ""));
		if (clawt_json_boolean(room, "declared", FALSE))
			navigation_add(nav, clawt_json_string(room, "name", id), detail, id, 1,
				clawt_json_string(room, "team", ""));
	}
	for (i = 0; i < clawt_section_count(); i++) {
		ClawtSection section = clawt_section_nth(i);
		guint j;
		for (j = 0; j < clawt_section_page_count(section); j++) {
			ClawtPage page = clawt_section_page_nth(section, j);
			if (self->selected_agent == NULL && page != CLAWT_PAGE_MEMORY)
				continue;
			navigation_add(nav, clawt_page_label(page), clawt_section_label(section), clawt_page_nick(page), 2, NULL);
		}
	}
	g_signal_connect(nav->entry, "changed", G_CALLBACK(navigation_changed), nav);
	g_signal_connect_swapped(nav->entry, "stop-search", G_CALLBACK(adw_dialog_close), nav->dialog);
	g_signal_connect(nav->entry, "activate", G_CALLBACK(navigation_enter), nav);
	g_signal_connect(nav->list, "row-activated", G_CALLBACK(navigation_activate), nav);
	adw_dialog_set_focus(nav->dialog, nav->entry);
	adw_dialog_present(nav->dialog, GTK_WIDGET(self));
}

/* Register once per window; the visible button advertises the accelerator. */
void
clawt_gtk_build_navigation(ClawtWindow *self, GtkWidget *parent)
{
	g_autoptr(GSimpleAction) action = g_simple_action_new("navigate", NULL);
	GtkWidget *button = gtk_button_new_with_label("Go to…");
	const gchar *accels[] = { "<Control>k", NULL };

	g_signal_connect(action, "activate", G_CALLBACK(on_navigate), self);
	g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(action));
	gtk_application_set_accels_for_action(gtk_window_get_application(GTK_WINDOW(self)), "win.navigate", accels);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.navigate");
	gtk_widget_set_tooltip_text(button, "Find agents, rooms and pages (Ctrl+K)");
	gtk_widget_add_css_class(button, "clawt-navigation-button");
	gtk_widget_set_margin_start(button, 12);
	gtk_widget_set_margin_end(button, 12);
	gtk_widget_set_margin_top(button, 6);
	gtk_widget_set_margin_bottom(button, 6);
	gtk_box_append(GTK_BOX(parent), button);
}
