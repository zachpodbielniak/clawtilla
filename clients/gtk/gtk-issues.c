/*
 * gtk-issues.c - The Activity page's Issues: what the sentinel found in
 * the event log
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Activity is what is running now; Issues is what keeps going wrong --
 * a tool that fails again and again, a queue that keeps somebody
 * waiting, a day of spend, turns the watchdog had to stop.  The daemon
 * never reads its own log for this: "Scan now" asks it to start
 * `clawtilla sentinel` (sentinel.scan), and the list is the YAML report
 * that process wrote (sentinel.report), redrawn when `sentinel.finished`
 * arrives.  The words for a kind and for the report's age are libclawt's,
 * so the web client and clawtilla-lrg say the same thing.
 *
 * Read when the page is shown and when a scan finishes, never on the
 * Activity page's ten-second tick: the report changes only when a scan
 * does.
 */

#include "clawt-window-private.h"

static void
on_scan_clicked(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	g_autoptr(JsonNode) reply = NULL;

	(void)button;

	reply = clawt_window_request(self, "sentinel.scan", NULL);

	if (reply == NULL)
		return;

	clawt_window_toast(self, clawt_json_boolean(clawt_payload_of(reply),
			"started", FALSE)
		? "Scanning the event log. The list updates when it finishes."
		: "A scan is already running.");
	clawt_gtk_refresh_issues(self);
}

void
clawt_gtk_refresh_issues(
	ClawtWindow *self
){
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *payload;
	JsonObject *report;
	JsonObject *header;
	JsonArray *issues;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	g_autofree gchar *age = NULL;
	const gchar *error;
	guint i;

	if (self->issues_list == NULL)
		return;

	reply = clawt_window_request(self, "sentinel.report", NULL);

	/* The request iterates the loop; the page may have gone. */
	if (self->issues_list == NULL)
		return;

	clawt_gtk_clear_list(self->issues_list);

	payload = (reply != NULL) ? clawt_payload_of(reply) : NULL;
	report = (payload != NULL && json_object_has_member(payload, "report"))
		? json_object_get_object_member(payload, "report") : NULL;
	header = (report != NULL && json_object_has_member(report, "sentinel"))
		? json_object_get_object_member(report, "sentinel") : NULL;
	issues = (report != NULL && json_object_has_member(report, "issues"))
		? json_object_get_array_member(report, "issues") : NULL;
	error = (payload != NULL) ? clawt_json_string(payload, "error", NULL)
		: NULL;

	age = clawt_sentinel_age_text(clawt_json_int(header, "scanned_at", 0),
		now);

	if (self->issues_status != NULL) {
		g_autofree gchar *line = g_strdup_printf("%s%s", age,
			clawt_json_boolean(payload, "running", FALSE)
				? " \302\267 scanning now\342\200\246" : "");

		gtk_label_set_text(GTK_LABEL(self->issues_status), line);
	}

	if (error != NULL) {
		GtkWidget *row = adw_action_row_new();

		clawt_gtk_set_row_text(row, "The last scan failed", error);
		gtk_list_box_append(self->issues_list, row);
	}

	if (report == NULL) {
		GtkWidget *row = adw_action_row_new();

		clawt_gtk_set_row_text(row, "Not scanned yet",
			"Scan now reads the event log -- without changing it -- and "
			"groups failing tools, slow queues, spending and stalled turns "
			"into issues.");
		gtk_list_box_append(self->issues_list, row);
		return;
	}

	if (issues == NULL || json_array_get_length(issues) == 0) {
		GtkWidget *row = adw_action_row_new();
		g_autofree gchar *subtitle = g_strdup_printf("%" G_GINT64_FORMAT
			" events in %" G_GINT64_FORMAT " day files, nothing recurring.",
			clawt_json_int(header, "events", 0),
			clawt_json_int(header, "files", 0));

		clawt_gtk_set_row_text(row, "No issues", subtitle);
		gtk_list_box_append(self->issues_list, row);
		return;
	}

	for (i = 0; i < json_array_get_length(issues); i++) {
		JsonObject *issue = json_array_get_object_element(issues, i);
		const gchar *sample = clawt_json_string(issue, "sample", "");
		g_autofree gchar *ago = clawt_time_ago_label(
			clawt_json_int(issue, "last_at", 0) * G_USEC_PER_SEC,
			now * G_USEC_PER_SEC);
		g_autofree gchar *subtitle = g_strdup_printf("last %s%s%s", ago,
			*sample != '\0' ? " \302\267 " : "", sample);
		GtkWidget *row = adw_action_row_new();
		GtkWidget *badge = gtk_label_new(clawt_sentinel_kind_label(
			clawt_json_string(issue, "kind", NULL)));
		gboolean bad = g_strcmp0(clawt_json_string(issue, "severity", ""),
			"bad") == 0;

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
			clawt_json_string(issue, "title", "?"));
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

		gtk_widget_add_css_class(badge, "caption");
		gtk_widget_add_css_class(badge, "clawt-badge");
		gtk_widget_add_css_class(badge, bad ? "error" : "warning");
		gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
		adw_action_row_add_prefix(ADW_ACTION_ROW(row), badge);

		gtk_list_box_append(self->issues_list, row);
	}
}

void
clawt_gtk_build_issues(
	ClawtWindow *self,
	GtkBox      *column
){
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *title = gtk_label_new("Issues");
	GtkWidget *scan = gtk_button_new_with_label("Scan now");

	gtk_widget_set_margin_top(head, 18);
	gtk_widget_set_margin_start(head, 12);
	gtk_widget_set_margin_end(head, 12);

	gtk_widget_add_css_class(title, "heading");
	gtk_widget_set_halign(title, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(titles), title);

	self->issues_status = gtk_label_new("Not scanned yet");
	gtk_widget_add_css_class(self->issues_status, "dim-label");
	gtk_widget_add_css_class(self->issues_status, "caption");
	gtk_widget_set_halign(self->issues_status, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(titles), self->issues_status);
	gtk_widget_set_hexpand(titles, TRUE);
	gtk_box_append(GTK_BOX(head), titles);

	gtk_widget_set_valign(scan, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(scan, "Read the event log for recurring "
		"trouble: failing tools, slow queues, spending, stalled turns. "
		"The log itself is never changed.");
	g_signal_connect(scan, "clicked", G_CALLBACK(on_scan_clicked), self);
	gtk_box_append(GTK_BOX(head), scan);

	gtk_box_append(column, head);

	self->issues_list = GTK_LIST_BOX(gtk_list_box_new());
	gtk_list_box_set_selection_mode(self->issues_list, GTK_SELECTION_NONE);
	gtk_widget_add_css_class(GTK_WIDGET(self->issues_list), "boxed-list");
	gtk_widget_set_valign(GTK_WIDGET(self->issues_list), GTK_ALIGN_START);
	gtk_widget_set_margin_start(GTK_WIDGET(self->issues_list), 12);
	gtk_widget_set_margin_end(GTK_WIDGET(self->issues_list), 12);
	gtk_widget_set_margin_bottom(GTK_WIDGET(self->issues_list), 12);
	gtk_box_append(column, GTK_WIDGET(self->issues_list));
}
