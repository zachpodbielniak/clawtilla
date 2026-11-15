/*
 * gtk-steps.c - The Screen view's saved steps: a desktop task replayed
 * with no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Under the screen: what the agent's last turn did to it, step by step,
 * with a name field and Save when the daemon says the turn can be
 * replayed exactly (`steps.last`, `steps.save`) and its reason when it
 * cannot; then every saved list with Replay and Delete (`steps.list`,
 * `steps.replay`, `steps.delete`), each saying how it is checked --
 * picture and title, or title only for a screen with a clock -- with one
 * button switching it (`steps.set_check`; the daemon names the other
 * check, so no client spells one).  Replay works only while you hold the
 * screen, which is also what keeps the agent off it -- the button says
 * so rather than failing.  The step lines are libclawt's
 * (clawt_saved_step_describe(), through the daemon), so the web client
 * and clawtilla-lrg read the same.
 *
 * Read when the computer page is drawn and on `steps.changed` and
 * `steps.replayed`, never on the screen's own frame timer.
 */

#include "clawt-window-private.h"

/* "3 steps: Click at 5, 6 · Type “hello” · Press Return" */
static gchar *
steps_line(
	JsonArray *steps
){
	g_autoptr(GString) line = g_string_new(NULL);
	guint n = (steps != NULL) ? json_array_get_length(steps) : 0;
	guint i;

	g_string_append_printf(line, "%u step%s", n, n == 1 ? "" : "s");

	for (i = 0; i < n && i < 4; i++)
		g_string_append_printf(line, "%s%s", i == 0 ? ": " : " \302\267 ",
			json_array_get_string_element(steps, i));

	if (n > 4)
		g_string_append(line, " \342\200\246");

	return g_string_free(g_steal_pointer(&line), FALSE);
}

static void
on_save_clicked(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	g_autoptr(JsonNode) reply = NULL;
	const gchar *name;

	(void)button;

	if (self->selected_agent == NULL || self->steps_name == NULL)
		return;

	name = gtk_editable_get_text(GTK_EDITABLE(self->steps_name));

	reply = clawt_window_request(self, "steps.save",
		clawt_build_payload("agent", self->selected_agent, "name", name,
			NULL));

	if (reply != NULL) {
		clawt_window_toast(self, "Saved. Take the screen and press Replay to "
			"run them again with no model.");
		gtk_editable_set_text(GTK_EDITABLE(self->steps_name), "");
	}

	clawt_gtk_refresh_saved_steps(self);
}

static void
on_list_action(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");
	const gchar *name = g_object_get_data(G_OBJECT(button), "clawt-name");
	const gchar *check = g_object_get_data(G_OBJECT(button), "clawt-check");
	g_autoptr(JsonNode) reply = NULL;

	if (self->selected_agent == NULL)
		return;

	reply = clawt_window_request(self, kind,
		clawt_build_payload("agent", self->selected_agent, "name", name,
			"check", check, NULL));

	if (reply != NULL && g_strcmp0(kind, "steps.replay") == 0)
		clawt_window_toast(self, "Checking the screen against the saved "
			"picture, then replaying.");

	clawt_gtk_refresh_saved_steps(self);
}

static GtkWidget *
list_button(
	ClawtWindow *self,
	const gchar *label,
	const gchar *kind,
	const gchar *name,
	const gchar *tooltip
){
	GtkWidget *button = gtk_button_new_with_label(label);

	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_tooltip_text(button, tooltip);
	g_object_set_data_full(G_OBJECT(button), "clawt-kind", g_strdup(kind),
		g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-name", g_strdup(name),
		g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_list_action), self);

	return button;
}

void
clawt_gtk_refresh_saved_steps(
	ClawtWindow *self
){
	g_autoptr(JsonNode) last = NULL;
	g_autoptr(JsonNode) listing = NULL;
	JsonObject *run;
	JsonArray *saved;
	guint i;

	if (self->steps_list == NULL || self->selected_agent == NULL)
		return;

	last = clawt_window_request(self, "steps.last",
		clawt_build_payload("agent", self->selected_agent, NULL));
	listing = clawt_window_request(self, "steps.list",
		clawt_build_payload("agent", self->selected_agent, NULL));

	/* The requests iterate the loop; the page may have gone. */
	if (self->steps_list == NULL)
		return;

	run = (last != NULL) ? clawt_payload_of(last) : NULL;

	if (run == NULL || !json_object_has_member(run, "steps")) {
		gtk_label_set_text(GTK_LABEL(self->steps_last),
			"The agent's last turn did nothing to this screen since the "
			"daemon started.");
		gtk_widget_set_sensitive(self->steps_save, FALSE);
	} else {
		g_autofree gchar *line = steps_line(
			json_object_get_array_member(run, "steps"));
		gboolean saveable = clawt_json_boolean(run, "saveable", FALSE);
		g_autofree gchar *said = saveable
			? g_strdup_printf("Its last turn: %s. Saved, it is checked by: "
				"%s.", line, clawt_json_string(run, "check_label", "?"))
			: g_strdup_printf("Its last turn: %s. Cannot be saved: %s",
				line, clawt_json_string(run, "why_not", "unknown"));

		gtk_label_set_text(GTK_LABEL(self->steps_last), said);
		gtk_widget_set_sensitive(self->steps_save, saveable);
	}

	clawt_gtk_clear_list(GTK_LIST_BOX(self->steps_list));

	saved = (listing != NULL &&
	         json_object_has_member(clawt_payload_of(listing), "saved"))
		? json_object_get_array_member(clawt_payload_of(listing), "saved")
		: NULL;

	for (i = 0; saved != NULL && i < json_array_get_length(saved); i++) {
		JsonObject *entry = json_array_get_object_element(saved, i);
		const gchar *name = clawt_json_string(entry, "name", "?");
		g_autofree gchar *line = steps_line(
			json_object_has_member(entry, "steps")
				? json_object_get_array_member(entry, "steps") : NULL);
		g_autofree gchar *subtitle = g_strdup_printf("%s\n%s", line,
			clawt_json_string(entry, "check_line", ""));
		const gchar *other = clawt_json_string(entry, "other_check", NULL);
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), list_button(self,
			"Replay", "steps.replay", name, "Run these steps again with no "
			"model. Only while you hold the screen, and only when it matches "
			"the way these steps are checked."));
		/*
		 * How it is checked, switched by one button the daemon names:
		 * title only is for a screen with a clock, which no picture can
		 * match twice.
		 */
		if (other != NULL) {
			g_autofree gchar *label = g_strdup_printf("Check: %s",
				clawt_json_string(entry, "other_check_label", other));
			GtkWidget *check = list_button(self, label, "steps.set_check",
				name, "Change what a replay compares before it sends "
				"anything: the picture and the focused window's title, or "
				"the title only.");

			g_object_set_data_full(G_OBJECT(check), "clawt-check",
				g_strdup(other), g_free);
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), check);
		}

		adw_action_row_add_suffix(ADW_ACTION_ROW(row), list_button(self,
			"Delete", "steps.delete", name, "Forget these steps"));
		gtk_list_box_append(GTK_LIST_BOX(self->steps_list), row);
	}
}

GtkWidget *
clawt_gtk_build_saved_steps(
	ClawtWindow *self
){
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *title = gtk_label_new("Saved steps");
	GtkWidget *about = gtk_label_new("What the agent pressed on this screen, "
		"kept so it can be done again with no model. Replay needs you to "
		"hold the screen and refuses a screen that has changed.");
	GtkWidget *save_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	gtk_widget_add_css_class(title, "heading");
	gtk_widget_set_halign(title, GTK_ALIGN_START);
	gtk_label_set_wrap(GTK_LABEL(about), TRUE);
	gtk_label_set_xalign(GTK_LABEL(about), 0.0f);
	gtk_widget_add_css_class(about, "dim-label");

	self->steps_last = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(self->steps_last), TRUE);
	gtk_label_set_xalign(GTK_LABEL(self->steps_last), 0.0f);

	self->steps_name = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(self->steps_name),
		"Name these steps");
	gtk_widget_set_hexpand(self->steps_name, TRUE);
	self->steps_save = gtk_button_new_with_label("Save");
	gtk_widget_set_sensitive(self->steps_save, FALSE);
	g_signal_connect(self->steps_save, "clicked", G_CALLBACK(on_save_clicked),
		self);
	gtk_box_append(GTK_BOX(save_row), self->steps_name);
	gtk_box_append(GTK_BOX(save_row), self->steps_save);

	self->steps_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->steps_list),
		GTK_SELECTION_NONE);
	gtk_widget_add_css_class(self->steps_list, "boxed-list");
	gtk_widget_set_valign(self->steps_list, GTK_ALIGN_START);

	gtk_box_append(GTK_BOX(box), title);
	gtk_box_append(GTK_BOX(box), about);
	gtk_box_append(GTK_BOX(box), self->steps_last);
	gtk_box_append(GTK_BOX(box), save_row);
	gtk_box_append(GTK_BOX(box), self->steps_list);

	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_bottom(box, 12);

	return box;
}
