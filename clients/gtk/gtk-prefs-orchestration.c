/*
 * gtk-prefs-orchestration.c - Settings: orchestration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * How agents talk to each other, as far as a person tunes it from here:
 * today the acknowledgement check (orchestration.ack_check_model).  The
 * choices are libclawt's walk of the model catalogue, so this page, the
 * web page and the libregnum page offer the same list.
 */

#include "clawt-window-private.h"

#include <string.h>

/* The stored value behind each row of the combo, in the same order. */
#define ACK_VALUES_KEY "clawt-ack-values"

static void
on_ack_model_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	ClawtWindow *self = user_data;
	GPtrArray *values;
	guint index;
	g_autoptr(JsonNode) reply = NULL;

	(void)pspec;

	/* A refresh selecting the current value is not the person choosing it. */
	if (self->settings_ack_loading)
		return;

	values = g_object_get_data(object, ACK_VALUES_KEY);
	index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

	if (values == NULL || index >= values->len)
		return;

	reply = clawt_window_request(self, "orchestration.set",
		clawt_build_payload("ack_check_model",
			(const gchar *)g_ptr_array_index(values, index), NULL));

	if (reply != NULL)
		clawt_window_toast(self, index == 0
			? "Acknowledgement check off."
			: "Acknowledgement check saved.");
}

void
clawt_gtk_refresh_settings_orchestration(
	ClawtWindow *self
){
	if (self->settings_ack_model == NULL)
		return;

	if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_ORCHESTRATION))
		return;

	do {
		g_autoptr(JsonNode) reply = NULL;
		g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
		GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
		const gchar *current = "";
		JsonObject *root;
		guint selected = 0;
		guint i;

		reply = clawt_window_request(self, "orchestration.get", NULL);
		root = reply != NULL ? clawt_payload_of(reply) : NULL;

		/* The settings dialog may have closed while the request waited. */
		if (self->settings_ack_model == NULL) {
			g_ptr_array_unref(values);
			continue;
		}

		if (root != NULL)
			current = clawt_json_string(root, "ack_check_model", "");

		for (i = 0; i < clawt_ack_check_model_count(); i++) {
			const gchar *value = clawt_ack_check_model_nth_value(i);

			gtk_string_list_append(labels, clawt_ack_check_model_nth_label(i));
			g_ptr_array_add(values, g_strdup(value));

			if (g_strcmp0(value, current) == 0)
				selected = i;
		}

		/*
		 * A model somebody wrote into the file by hand is kept as a
		 * choice rather than shown as Off, which would be a lie about
		 * what is configured -- and saving the page would make it true.
		 */
		if (current != NULL && *current != '\0' && selected == 0) {
			g_autofree gchar *label = clawt_ack_check_model_label(current);

			gtk_string_list_append(labels, label);
			g_ptr_array_add(values, g_strdup(current));
			selected = values->len - 1;
		}

		self->settings_ack_loading = TRUE;
		g_object_set_data_full(G_OBJECT(self->settings_ack_model),
				       ACK_VALUES_KEY, values,
				       (GDestroyNotify)g_ptr_array_unref);
		adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_ack_model),
					G_LIST_MODEL(labels));
		adw_combo_row_set_selected(ADW_COMBO_ROW(self->settings_ack_model),
					   selected);
		self->settings_ack_loading = FALSE;
	} while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_ORCHESTRATION));
}

GtkWidget *
clawt_gtk_build_orchestration_page(
	ClawtWindow *self
){
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *group = adw_preferences_group_new();

	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Orchestration");
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
					   "mail-send-receive-symbolic");

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
					"Acknowledgement check");
	adw_preferences_group_set_description(
		ADW_PREFERENCES_GROUP(group),
		"When one agent answers another with only an acknowledgement -- "
		"\"thanks, noted\" -- waking the agent that asked costs it a whole "
		"turn. With a model chosen here, a short answer from a turn that "
		"did nothing else is put to that model first: only a plain ACK "
		"skips the wake, and anything else, a failure, or no verdict "
		"within 8 seconds wakes the sender as before. A skipped answer "
		"stays in the room marked \"acknowledged (no wake)\", and the "
		"sender is told about it on its next turn. Off by default.");

	self->settings_ack_model = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->settings_ack_model),
				      "Model");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(self->settings_ack_model),
		"A small, fast model reached over HTTP; its key is configured "
		"as for AI assist");
	g_signal_connect(self->settings_ack_model, "notify::selected",
			 G_CALLBACK(on_ack_model_changed), self);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_ack_model);

	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
				 ADW_PREFERENCES_GROUP(group));

	return page;
}
