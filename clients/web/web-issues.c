/*
 * web-issues.c - The Activity page's Issues: what the sentinel found in
 * the event log
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web half of gtk-issues.c and the libregnum client's section.  The
 * card is drawn with the page, not with the activity fragment the page
 * polls: the report changes only when a scan does.  Scan now posts to
 * /issues/scan, which asks the daemon to start `clawtilla sentinel` and
 * comes back here; the daemon never reads its own log for this.
 */

#include "clawt-web.h"
#include "web-pages.h"

void
clawt_web_add_issues_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(HtmxDiv) card = clawt_web_card(
		"Issues",
		"What keeps going wrong, from the event log: failing tools, slow "
		"queues, spending and stalled turns. Scan now reads the log "
		"without changing it.");
	HtmxElement *body = clawt_web_card_body(card);
	g_autoptr(HtmxForm) form = clawt_web_form("/issues/scan");
	g_autoptr(HtmxButton) scan = clawt_web_button("Scan now", "default");
	JsonObject *root;
	JsonObject *report;
	JsonObject *header;
	JsonArray *issues;
	const gchar *error;
	g_autofree gchar *age = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	guint i;

	reply = clawt_web_app_call(app, "sentinel.report", NULL);
	root = clawt_web_root(reply);
	report = clawt_web_member_object(root, "report");
	header = clawt_web_member_object(report, "sentinel");
	issues = clawt_web_member_array(report, "issues");
	error = clawt_web_member(root, "error", NULL);

	age = clawt_sentinel_age_text(clawt_web_member_int(header, "scanned_at",
		0), now);

	{
		g_autofree gchar *line = g_strdup_printf("%s%s", age,
			clawt_web_member_bool(root, "running", FALSE)
				? " \302\267 scanning now\342\200\246" : "");

		clawt_web_add(body, clawt_web_text(line, "small muted"));
	}

	if (agent_id != NULL)
		clawt_web_add(form, clawt_web_hidden("agent", agent_id));
	htmx_element_set_attribute(HTMX_ELEMENT(scan), "type", "submit");
	htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(scan));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

	if (reply == NULL) {
		g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

		clawt_web_add(body, clawt_web_notice(why, "bad"));
	} else if (error != NULL) {
		g_autofree gchar *line = g_strdup_printf("The last scan failed: %s",
			error);

		clawt_web_add(body, clawt_web_notice(line, "bad"));
	}

	if (reply != NULL && report == NULL)
		clawt_web_add(body, clawt_web_empty("Not scanned yet",
			"Scan now groups what the event log says keeps going wrong."));
	else if (report != NULL &&
		(issues == NULL || json_array_get_length(issues) == 0)) {
		g_autofree gchar *line = g_strdup_printf("%" G_GINT64_FORMAT
			" events in %" G_GINT64_FORMAT " day files, nothing recurring.",
			clawt_web_member_int(header, "events", 0),
			clawt_web_member_int(header, "files", 0));

		clawt_web_add(body, clawt_web_empty("No issues", line));
	}

	for (i = 0; issues != NULL && i < json_array_get_length(issues); i++) {
		JsonObject *issue = json_array_get_object_element(issues, i);
		const gchar *sample = clawt_web_member(issue, "sample", "");
		g_autoptr(HtmxDiv) row = htmx_div_new();
		g_autoptr(HtmxDiv) head = htmx_div_new();
		g_autoptr(HtmxSpan) label = htmx_span_new();
		g_autofree gchar *ago = clawt_time_ago_label(
			clawt_web_member_int(issue, "last_at", 0) * G_USEC_PER_SEC,
			now * G_USEC_PER_SEC);
		g_autofree gchar *line = g_strdup_printf("last %s%s%s", ago,
			*sample != '\0' ? " \302\267 " : "", sample);

		htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
		htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
		clawt_web_add(head, clawt_web_badge(
			clawt_sentinel_kind_label(clawt_web_member(issue, "kind", NULL)),
			g_strcmp0(clawt_web_member(issue, "severity", ""), "bad") == 0
				? "bad" : "warn"));
		htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
		htmx_node_set_text_content(HTMX_NODE(label),
			clawt_web_member(issue, "title", "?"));
		htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
		clawt_web_add(row, clawt_web_text(line, "small muted"));
		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	}

	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* Starts a scan and comes back to the Activity page saying so. */
static HtmxResponse *
on_issues_scan(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = g_strdup(clawt_web_form_value(request,
		"agent"));
	g_autoptr(JsonNode) reply = NULL;

	(void)params;

	reply = clawt_web_app_call(app, "sentinel.scan", NULL);

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id,
			CLAWT_PAGE_ACTIVITY, clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_ACTIVITY,
		clawt_web_member_bool(clawt_web_root(reply), "started", FALSE)
			? "Scanning the event log. Reload in a moment for the result."
			: "A scan is already running.");
}

void
clawt_web_register_issues(
	HtmxRouter  *router,
	ClawtWebApp *app
){
	htmx_router_post(router, "/issues/scan", on_issues_scan, app);
}
