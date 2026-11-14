/*
 * gtk-prefs-spending.c - Settings: spending
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the fleet has spent, over a period somebody picks, grouped the
 * way they ask: by agent, by model, by what started the work, or by day.
 * The groupings, the periods, the wording of each row and the CSV are
 * all libclawt's, so this page, the web page, the libregnum page and
 * `clawtilla cost --by` cannot tell four different stories.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

/* ── Spending ────────────────────────────────────────────────────── */

/* The reply the rows were drawn from, kept on the list for Export. */
#define SPENDING_REPLY_KEY "clawt-spending-ledger"

static void
on_spending_period_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	ClawtWindow *self = user_data;
	guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

	(void)pspec;

	self->settings_spending_since = clawt_usage_period_since(
		index, g_get_real_time() / G_USEC_PER_SEC);
	clawt_gtk_refresh_settings_spending(self);
}

static void
on_spending_group_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	(void)object;
	(void)pspec;

	clawt_gtk_refresh_settings_spending(user_data);
}

/* The grouping the selector shows, as the library's nick. */
static const gchar *
spending_group(
	ClawtWindow *self
){
	guint index = 0;

	if (self->settings_spending_group != NULL)
		index = adw_combo_row_get_selected(
			ADW_COMBO_ROW(self->settings_spending_group));

	if (index >= clawt_usage_group_count())
		index = 0;

	return clawt_usage_group_nth(index);
}

/* A row with the cost on the right, where a column of them lines up. */
static GtkWidget *
spending_row(
	const gchar *title,
	const gchar *subtitle,
	const gchar *cost,
	gboolean     total
){
	GtkWidget *row = adw_action_row_new();
	GtkWidget *value = gtk_label_new(cost);

	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

	gtk_widget_add_css_class(value, "numeric");
	gtk_widget_add_css_class(value, total ? "heading" : "dim-label");
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

	return row;
}

/*
 * One row per group, then the total -- and, under them, the sentence
 * saying how many turns have no price.  Said in words as well as in
 * each subtitle, because an unpriced turn is exactly the kind of thing
 * a total quietly hides.
 */
void
clawt_gtk_refresh_settings_spending(
	ClawtWindow *self
){
	if (self->settings_spending == NULL)
		return;

	if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SPENDING))
		return;

	do {
		g_autoptr(JsonNode) reply = NULL;
		g_autofree gchar *unpriced = NULL;
		JsonArray *groups = NULL;
		JsonObject *total = NULL;
		JsonObject *root;
		guint i;

		clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_spending));
		g_object_set_data(G_OBJECT(self->settings_spending),
				  SPENDING_REPLY_KEY, NULL);

		reply = clawt_window_request(self, "usage.ledger",
			clawt_usage_ledger_payload(self->settings_spending_since,
						   spending_group(self), NULL));

		if (reply == NULL)
			continue;

		root = clawt_payload_of(reply);
		if (root == NULL)
			continue;

		if (json_object_has_member(root, "groups") &&
		    JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "groups")))
			groups = json_object_get_array_member(root, "groups");
		if (json_object_has_member(root, "total") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "total")))
			total = json_object_get_object_member(root, "total");

		for (i = 0; groups != NULL && i < json_array_get_length(groups); i++) {
			JsonObject *group = json_array_get_object_element(groups, i);
			g_autofree gchar *subtitle = clawt_usage_ledger_row_subtitle(group);
			g_autofree gchar *cost = clawt_usage_ledger_cost_label(group);

			gtk_list_box_append(GTK_LIST_BOX(self->settings_spending),
				spending_row(clawt_json_string(group, "key", "?"),
					     subtitle, cost, FALSE));
		}

		if (groups == NULL || json_array_get_length(groups) == 0)
			gtk_list_box_append(GTK_LIST_BOX(self->settings_spending),
				spending_row("Nothing recorded",
					     "No turns in this period", "", FALSE));

		{
			g_autofree gchar *cost = clawt_usage_ledger_cost_label(total);
			g_autofree gchar *subtitle = g_strdup_printf(
				"%" G_GINT64_FORMAT " turns across the fleet",
				json_object_get_int_member_with_default(total, "turns", 0));

			gtk_list_box_append(GTK_LIST_BOX(self->settings_spending),
				spending_row("Total", subtitle, cost, TRUE));
		}

		unpriced = clawt_usage_ledger_unpriced_line(root);
		if (self->settings_spending_unpriced != NULL) {
			gtk_label_set_text(GTK_LABEL(self->settings_spending_unpriced),
					   unpriced != NULL ? unpriced : "");
			gtk_widget_set_visible(self->settings_spending_unpriced,
					       unpriced != NULL);
		}

		g_object_set_data_full(G_OBJECT(self->settings_spending),
				       SPENDING_REPLY_KEY, json_node_ref(reply),
				       (GDestroyNotify)json_node_unref);
	} while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SPENDING));
}

/* What a save carries: the window, held, and the document to write. */
typedef struct {
	ClawtWindow *window;
	gchar       *csv;
} SpendingExport;

static void
spending_export_free(
	SpendingExport *export
){
	g_clear_object(&export->window);
	g_free(export->csv);
	g_free(export);
}

static void
on_spending_csv_saved(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	SpendingExport *export = data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;

	file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);

	if (file == NULL) {
		/* dismissing the dialog is not a failure worth a toast */
		if (error != NULL &&
		    !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			clawt_window_toast(export->window, error->message);
	} else if (!g_file_replace_contents(file, export->csv, strlen(export->csv),
					    NULL, FALSE, G_FILE_CREATE_NONE, NULL,
					    NULL, &error)) {
		clawt_window_toast(export->window, error->message);
	} else {
		clawt_window_toast(export->window, "Spending exported.");
	}

	spending_export_free(export);
}

/*
 * Saves what is on screen, not a fresh read.  The CSV is made from the
 * reply the rows were drawn from, so the file and the page agree even
 * if a turn lands while the dialog is open.
 */
static void
on_spending_export(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	JsonNode *reply = NULL;
	JsonObject *root;
	GtkFileDialog *dialog;
	SpendingExport *export;
	g_autofree gchar *name = NULL;

	(void)button;

	if (self->settings_spending != NULL)
		reply = g_object_get_data(G_OBJECT(self->settings_spending),
					  SPENDING_REPLY_KEY);
	root = (reply != NULL) ? clawt_payload_of(reply) : NULL;

	if (root == NULL) {
		clawt_window_toast(self, "Nothing to export yet.");
		return;
	}

	export = g_new0(SpendingExport, 1);
	export->window = g_object_ref(self);
	export->csv = clawt_usage_ledger_csv(root);
	name = clawt_usage_ledger_csv_name(root, g_get_real_time() / G_USEC_PER_SEC);

	dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Export spending");
	gtk_file_dialog_set_initial_name(dialog, name);
	gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_spending_csv_saved,
			     export);
	g_object_unref(dialog);
}

GtkWidget *
clawt_gtk_build_spending_page(
	ClawtWindow *self
){
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *group = adw_preferences_group_new();
	GtkStringList *periods = gtk_string_list_new(NULL);
	GtkStringList *groupings = gtk_string_list_new(NULL);
	GtkWidget *export;
	guint i;

	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Spending");
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
					   "emblem-documents-symbolic");

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
					"What the fleet has cost");
	adw_preferences_group_set_description(
		ADW_PREFERENCES_GROUP(group),
		"The figure each provider reported for each turn. Token counts "
		"cover new input and output only -- cached context is billed but "
		"is not reported as tokens, so the cost is larger than the tokens "
		"beside it suggest. Origin says what started the work: you, "
		"another agent, a routine or a trigger.");

	/* Export sits on the heading, beside what it exports */
	export = gtk_button_new_with_label("Export CSV");
	gtk_widget_add_css_class(export, "flat");
	gtk_widget_set_valign(export, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(export,
		"Save the table below as a spreadsheet-safe CSV file");
	g_signal_connect(export, "clicked", G_CALLBACK(on_spending_export), self);
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
						export);

	/* the library's periods, so this list cannot drift from the others */
	for (i = 0; i < clawt_usage_period_count(); i++)
		gtk_string_list_append(periods, clawt_usage_period_label(i));

	self->settings_spending_since = clawt_usage_period_since(
		0, g_get_real_time() / G_USEC_PER_SEC);
	self->settings_spending_period = adw_combo_row_new();
	adw_preferences_row_set_title(
		ADW_PREFERENCES_ROW(self->settings_spending_period), "Period");
	adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_spending_period),
				G_LIST_MODEL(periods));
	g_signal_connect(self->settings_spending_period, "notify::selected",
			 G_CALLBACK(on_spending_period_changed), self);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_spending_period);

	for (i = 0; i < clawt_usage_group_count(); i++)
		gtk_string_list_append(groupings, clawt_usage_group_label(i));

	self->settings_spending_group = adw_combo_row_new();
	adw_preferences_row_set_title(
		ADW_PREFERENCES_ROW(self->settings_spending_group), "Group by");
	adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_spending_group),
				G_LIST_MODEL(groupings));
	g_signal_connect(self->settings_spending_group, "notify::selected",
			 G_CALLBACK(on_spending_group_changed), self);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_spending_group);

	self->settings_spending = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_spending),
					GTK_SELECTION_NONE);
	gtk_widget_add_css_class(self->settings_spending, "boxed-list");
	gtk_widget_set_margin_top(self->settings_spending, 12);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_spending);

	/* hidden until a period holds a turn with no price */
	self->settings_spending_unpriced = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(self->settings_spending_unpriced), TRUE);
	gtk_label_set_xalign(GTK_LABEL(self->settings_spending_unpriced), 0.0f);
	gtk_widget_add_css_class(self->settings_spending_unpriced, "warning");
	gtk_widget_set_margin_top(self->settings_spending_unpriced, 8);
	gtk_widget_set_visible(self->settings_spending_unpriced, FALSE);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_spending_unpriced);

	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
				 ADW_PREFERENCES_GROUP(group));

	return page;
}
