/*
 * lrg-settings-spending.c - Settings: Spending
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the fleet has spent, over a period somebody picks.  Mirrors
 * clients/gtk/gtk-prefs-spending.c: a period, a grouping (agent, model,
 * origin, day), one row per group with its cost, the total, the
 * unpriced turns said in words, and Export CSV through a save dialog.
 * The periods and what each means in seconds are the model's
 * (clawt_lrg_settings_period_*), so the arithmetic is tested rather
 * than trusted; the groupings, row wording and CSV are libclawt's.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

#include <string.h>

typedef struct {
	LrgWidget   *list;
	LrgWidget   *period;
	LrgWidget   *group;
	LrgWidget   *unpriced;
	ClawtLrgApp *app;
	JsonNode    *shown;       /* the reply the rows were drawn from */
	guint        generation;
} Spending;

static Spending spending;

static void refresh(ClawtLrgApp *app);

static void
on_choice_changed(
	LrgComboBox *combo,
	gpointer     user_data
){
	refresh(user_data);
}

/* A row with the cost on the right, where a column of them lines up. */
static LrgWidget *
cost_row(
	const gchar *title,
	const gchar *subtitle,
	const gchar *cost,
	gboolean     total
){
	LrgWidget *row = clawt_lrg_row(title, subtitle);
	LrgWidget *value = clawt_lrg_label(cost, total ? "heading" : "muted");

	clawt_lrg_row_add_suffix(row, value);
	return row;
}

static void
on_ledger(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *groups;
	JsonObject *total = NULL;
	g_autofree gchar *unpriced = NULL;
	guint i;

	if (GPOINTER_TO_UINT(user_data) != spending.generation || root == NULL)
		return;

	clawt_lrg_clear(spending.list);
	groups = clawt_lrg_settings_member_array(root, "groups");

	if (json_object_has_member(root, "total") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "total")))
		total = json_object_get_object_member(root, "total");

	for (i = 0; groups != NULL && i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		const gchar *key = clawt_ipc_payload_string(group, "key");
		g_autofree gchar *subtitle = clawt_usage_ledger_row_subtitle(group);
		g_autofree gchar *cost = clawt_usage_ledger_cost_label(group);
		LrgWidget *row;

		row = cost_row((key != NULL) ? key : "?", subtitle, cost, FALSE);
		clawt_lrg_appear(app, row, i);
		clawt_lrg_take(spending.list, row);
	}

	if (groups == NULL || json_array_get_length(groups) == 0)
		clawt_lrg_take(spending.list, cost_row("Nothing recorded",
						       "No turns in this period",
						       "", FALSE));

	{
		g_autofree gchar *subtitle =
			clawt_lrg_settings_spend_total_subtitle(total);
		g_autofree gchar *cost = clawt_usage_ledger_cost_label(total);
		LrgWidget *row = cost_row("Total", subtitle, cost, TRUE);

		lrg_widget_add_class(row, "clawt-card-selected");
		clawt_lrg_take(spending.list, row);
	}

	/* the turns a total quietly leaves out, said in words */
	unpriced = clawt_usage_ledger_unpriced_line(root);
	clawt_lrg_paragraph_set_text(spending.unpriced,
				     unpriced != NULL ? unpriced : "");
	lrg_widget_set_visible(spending.unpriced, unpriced != NULL);

	g_clear_pointer(&spending.shown, json_node_unref);
	spending.shown = json_node_ref(reply);

	clawt_lrg_app_wake(app);
}

static void
refresh(
	ClawtLrgApp *app
){
	const gchar *chosen;
	const gchar *by;
	guint index = 0;
	g_autoptr(GDateTime) now = g_date_time_new_now_local();

	if (spending.list == NULL)
		return;

	chosen = clawt_lrg_combo_id(spending.period);
	if (chosen != NULL)
		index = (guint)g_ascii_strtoull(chosen, NULL, 10);

	/* the combo's ids are the library's nicks, so this is one already */
	by = clawt_lrg_combo_id(spending.group);

	spending.generation++;
	clawt_lrg_request(app, "usage.ledger",
			  clawt_usage_ledger_payload(
				  clawt_lrg_settings_period_since(index, now),
				  by, NULL),
			  CLAWT_LRG_QUIET, on_ledger,
			  GUINT_TO_POINTER(spending.generation), NULL);
}

static void
on_csv_saved(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autofree gchar *csv = user_data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;

	file = lrg_file_dialog_save_finish(LRG_FILE_DIALOG(source), result, &error);

	if (file == NULL) {
		/* dismissing the dialog is not a failure worth a toast */
		if (error != NULL &&
		    !g_error_matches(error, lrg_file_dialog_error_quark(),
				     LRG_FILE_DIALOG_ERROR_DISMISSED) &&
		    !g_error_matches(error, lrg_file_dialog_error_quark(),
				     LRG_FILE_DIALOG_ERROR_CANCELLED))
			clawt_lrg_toast(spending.app, error->message);
		return;
	}

	if (!g_file_replace_contents(file, csv, strlen(csv), NULL, FALSE,
				     G_FILE_CREATE_NONE, NULL, NULL, &error))
		clawt_lrg_toast(spending.app, error->message);
	else
		clawt_lrg_toast(spending.app, "Spending exported.");
}

/*
 * Saves what is on screen, not a fresh read: the CSV is made from the
 * reply the rows were drawn from and travels with the dialog, so the
 * file and the page agree even if a turn lands while it is open.
 */
static void
on_export_clicked(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	JsonObject *root = (spending.shown != NULL)
		? clawt_ipc_reply_object(spending.shown) : NULL;
	g_autoptr(LrgFileDialog) dialog = NULL;
	g_autofree gchar *name = NULL;

	if (root == NULL) {
		clawt_lrg_toast(app, "Nothing to export yet.");
		return;
	}

	name = clawt_usage_ledger_csv_name(root, g_get_real_time() / G_USEC_PER_SEC);
	dialog = lrg_file_dialog_new();
	lrg_file_dialog_set_title(dialog, "Export spending");
	lrg_file_dialog_set_initial_name(dialog, name);
	lrg_file_dialog_save(dialog, LRG_WIDGET(app->canvas), NULL, on_csv_saved,
			     clawt_usage_ledger_csv(root));
}

static LrgWidget *
build(
	ClawtLrgApp *app
){
	LrgWidget *page = clawt_lrg_page_new("Spending",
					     "What the fleet has cost");
	LrgWidget *box = clawt_lrg_vbox(10.0f);
	LrgWidget *form = clawt_lrg_form_new();
	guint n = clawt_lrg_settings_period_count();
	g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) labels = g_ptr_array_new();
	g_autoptr(GPtrArray) group_ids = g_ptr_array_new();
	g_autoptr(GPtrArray) group_labels = g_ptr_array_new();
	guint i;

	spending.app = app;

	for (i = 0; i < n; i++) {
		g_ptr_array_add(ids, g_strdup_printf("%u", i));
		g_ptr_array_add(labels, (gpointer)clawt_lrg_settings_period_label(i));
	}

	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(labels, NULL);

	/* the library's groupings, by nick, so the request needs no mapping */
	for (i = 0; i < clawt_usage_group_count(); i++) {
		g_ptr_array_add(group_ids, (gpointer)clawt_usage_group_nth(i));
		g_ptr_array_add(group_labels, (gpointer)clawt_usage_group_label(i));
	}

	g_ptr_array_add(group_ids, NULL);
	g_ptr_array_add(group_labels, NULL);

	clawt_lrg_take(clawt_lrg_page_toolbar(page),
		       clawt_lrg_button("Export CSV", "flat",
					G_CALLBACK(on_export_clicked), app));

	clawt_lrg_take(box, clawt_lrg_paragraph(
		"The figure each provider reported for each turn. Token counts "
		"cover new input and output only -- cached context is billed but "
		"is not reported as tokens, so the cost is larger than the tokens "
		"beside it suggest. Origin says what started the work: you, "
		"another agent, a routine or a trigger.", "muted"));

	/* connected after the initial selection, so building is not a change */
	spending.period = clawt_lrg_combo((const gchar *const *)ids->pdata,
					  (const gchar *const *)labels->pdata,
					  "0");
	g_signal_connect(spending.period, "changed",
			 G_CALLBACK(on_choice_changed), app);
	clawt_lrg_form_add(form, "Period", spending.period, NULL);

	spending.group = clawt_lrg_combo((const gchar *const *)group_ids->pdata,
					 (const gchar *const *)group_labels->pdata,
					 clawt_usage_group_nth(0));
	g_signal_connect(spending.group, "changed",
			 G_CALLBACK(on_choice_changed), app);
	clawt_lrg_form_add(form, "Group by", spending.group, NULL);
	clawt_lrg_take(box, form);

	spending.list = clawt_lrg_vbox(6.0f);
	clawt_lrg_add(box, spending.list);
	g_object_unref(spending.list);

	/* hidden until a period holds a turn with no price */
	spending.unpriced = clawt_lrg_paragraph("", "caption");
	lrg_widget_set_visible(spending.unpriced, FALSE);
	clawt_lrg_add(box, spending.unpriced);
	g_object_unref(spending.unpriced);

	clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

	return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "spending", "Spending", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_spending_page(void)
{
    return &page_impl;
}
