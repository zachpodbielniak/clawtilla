/*
 * lrg-issues.c - The Activity page's Issues: what the sentinel found in
 * the event log
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The libregnum half of gtk-issues.c and web-issues.c.  A section under
 * the running turns: the age of the last report, Scan now (sentinel.scan
 * starts `clawtilla sentinel` as the daemon's child), and the issues the
 * report holds (sentinel.report), worst first.  Read when the page is
 * shown and on `sentinel.finished`, never on the activity tick -- the
 * report changes only when a scan does.  Every request is async through
 * clawt_lrg_request(), never from inside a frame.
 */

#include "clawt-lrg.h"

static struct {
	LrgWidget *status;   /* "Scanned 3m ago" */
	LrgWidget *list;     /* the rows */
} issues;

static void
on_report(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonObject *report = (root != NULL &&
	                      json_object_has_member(root, "report"))
		? json_object_get_object_member(root, "report") : NULL;
	JsonObject *header = (report != NULL &&
	                      json_object_has_member(report, "sentinel"))
		? json_object_get_object_member(report, "sentinel") : NULL;
	JsonArray *rows = (report != NULL &&
	                   json_object_has_member(report, "issues"))
		? json_object_get_array_member(report, "issues") : NULL;
	const gchar *failed = (root != NULL)
		? clawt_ipc_payload_string(root, "error") : NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	g_autofree gchar *age = NULL;
	g_autofree gchar *line = NULL;
	guint i;

	if (issues.list == NULL)
		return;

	age = clawt_sentinel_age_text(
		clawt_ipc_payload_int(header, "scanned_at", 0), now);
	line = g_strdup_printf("%s%s", age,
		clawt_ipc_payload_boolean(root, "running", FALSE)
			? " \302\267 scanning now\342\200\246" : "");
	clawt_lrg_paragraph_set_text(issues.status, line);

	clawt_lrg_clear(issues.list);

	if (failed != NULL) {
		g_autofree gchar *said = g_strdup_printf("The last scan failed: %s",
		                                         failed);

		clawt_lrg_take(issues.list, clawt_lrg_paragraph(said, "muted"));
	}

	if (report == NULL)
		clawt_lrg_take(issues.list, clawt_lrg_paragraph(
			"Not scanned yet. Scan now reads the event log -- without "
			"changing it -- and groups failing tools, slow queues, spending "
			"and stalled turns into issues.", "muted"));
	else if (rows == NULL || json_array_get_length(rows) == 0) {
		g_autofree gchar *none = g_strdup_printf("No issues: %"
			G_GINT64_FORMAT " events in %" G_GINT64_FORMAT " day files, "
			"nothing recurring.",
			clawt_ipc_payload_int(header, "events", 0),
			clawt_ipc_payload_int(header, "files", 0));

		clawt_lrg_take(issues.list, clawt_lrg_paragraph(none, "muted"));
	}

	for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
		JsonObject *issue = json_array_get_object_element(rows, i);
		const gchar *title = clawt_ipc_payload_string(issue, "title");
		const gchar *sample = clawt_ipc_payload_string(issue, "sample");
		const gchar *severity = clawt_ipc_payload_string(issue, "severity");
		g_autofree gchar *ago = clawt_time_ago_label(
			clawt_ipc_payload_int(issue, "last_at", 0) * G_USEC_PER_SEC,
			now * G_USEC_PER_SEC);
		g_autofree gchar *subtitle = g_strdup_printf("last %s%s%s", ago,
			(sample != NULL && *sample != '\0') ? " \302\267 " : "",
			sample != NULL ? sample : "");
		LrgWidget *row = clawt_lrg_row(title != NULL ? title : "?",
		                               subtitle);

		clawt_lrg_row_add_prefix(row, clawt_lrg_badge(
			clawt_sentinel_kind_label(
				clawt_ipc_payload_string(issue, "kind")),
			g_strcmp0(severity, "bad") == 0 ? "bad" : "warn"));
		clawt_lrg_appear(app, row, i);
		clawt_lrg_take(issues.list, row);
	}

	clawt_lrg_app_wake(app);
}

void
clawt_lrg_issues_refresh(
	ClawtLrgApp *app
){
	if (issues.list == NULL)
		return;

	clawt_lrg_request(app, "sentinel.report", NULL, CLAWT_LRG_QUIET,
	                  on_report, NULL, NULL);
}

static void
on_scan_started(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);

	if (reply != NULL)
		clawt_lrg_toast(app, clawt_ipc_payload_boolean(root, "started", FALSE)
			? "Scanning the event log. The list updates when it finishes."
			: "A scan is already running.");

	clawt_lrg_issues_refresh(app);
}

static void
on_scan_clicked(
	LrgButton *button,
	gpointer   user_data
){
	clawt_lrg_request(user_data, "sentinel.scan", NULL,
	                  CLAWT_LRG_REQUEST_NONE, on_scan_started, NULL, NULL);
}

void
clawt_lrg_issues_build(
	ClawtLrgApp *app,
	LrgWidget   *body
){
	LrgWidget *head = clawt_lrg_hbox(12.0f);
	LrgWidget *titles = clawt_lrg_vbox(2.0f);

	clawt_lrg_take(titles, clawt_lrg_label("Issues", "heading"));
	issues.status = clawt_lrg_paragraph("Not scanned yet", "muted");
	clawt_lrg_add(titles, issues.status);
	g_object_unref(issues.status);
	clawt_lrg_take(head, titles);
	clawt_lrg_take(head, clawt_lrg_button("Scan now", "flat",
	                                      G_CALLBACK(on_scan_clicked), app));
	clawt_lrg_take(body, head);

	issues.list = clawt_lrg_vbox(8.0f);
	clawt_lrg_add(body, issues.list);
	g_object_unref(issues.list);
}
