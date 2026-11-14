/*
 * lrg-steps.c - The Screen view's saved steps: a desktop task replayed
 * with no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The libregnum half of gtk-steps.c and web-steps.c: under the screen,
 * what the agent's last turn pressed (`steps.last`), a name and Save when
 * the daemon says it can be replayed exactly, and each saved list with
 * Replay and Delete (`steps.list`, `steps.replay`, `steps.delete`).
 * Async through clawt_lrg_request(), re-read with the computer page and
 * on `steps.changed` and `steps.replayed`.
 */

#include "clawt-lrg.h"

static struct {
	LrgWidget *last;    /* the last turn's line */
	LrgWidget *name;    /* the name entry */
	LrgWidget *save;
	LrgWidget *list;
} steps;

static gchar *
steps_line(
	JsonArray *list
){
	g_autoptr(GString) line = g_string_new(NULL);
	guint n = (list != NULL) ? json_array_get_length(list) : 0;
	guint i;

	g_string_append_printf(line, "%u step%s", n, n == 1 ? "" : "s");

	for (i = 0; i < n && i < 4; i++)
		g_string_append_printf(line, "%s%s", i == 0 ? ": " : " \302\267 ",
			json_array_get_string_element(list, i));

	if (n > 4)
		g_string_append(line, " \342\200\246");

	return g_string_free(g_steal_pointer(&line), FALSE);
}

static void
on_done(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	if (reply != NULL && user_data != NULL)
		clawt_lrg_toast(app, user_data);

	clawt_lrg_steps_refresh(app);
}

static void
on_save_clicked(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *name;

	if (app->agent_id == NULL || steps.name == NULL)
		return;

	name = lrg_text_input_get_text(LRG_TEXT_INPUT(steps.name));

	clawt_lrg_request(app, "steps.save",
	                  clawt_ipc_payload_new("agent", app->agent_id,
	                                        "name", name, NULL),
	                  CLAWT_LRG_REQUEST_NONE, on_done,
	                  (gpointer)"Saved. Take the screen and press Replay to "
	                  "run them again with no model.", NULL);
}

static void
on_list_clicked(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");
	const gchar *name = g_object_get_data(G_OBJECT(button), "clawt-name");

	if (app->agent_id == NULL)
		return;

	clawt_lrg_request(app, kind,
	                  clawt_ipc_payload_new("agent", app->agent_id,
	                                        "name", name, NULL),
	                  CLAWT_LRG_REQUEST_NONE, on_done,
	                  g_strcmp0(kind, "steps.replay") == 0
	                      ? (gpointer)"Checking the screen against the saved "
	                        "picture, then replaying."
	                      : (gpointer)"Deleted.", NULL);
}

static LrgWidget *
list_button(
	ClawtLrgApp *app,
	const gchar *label,
	const gchar *style,
	const gchar *kind,
	const gchar *name
){
	LrgWidget *button = clawt_lrg_button(label, style,
	                                     G_CALLBACK(on_list_clicked), app);

	g_object_set_data_full(G_OBJECT(button), "clawt-kind", g_strdup(kind),
	                       g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-name", g_strdup(name),
	                       g_free);

	return button;
}

static void
on_last(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *run = clawt_ipc_reply_object(reply);

	if (steps.last == NULL)
		return;

	if (run == NULL || !json_object_has_member(run, "steps")) {
		clawt_lrg_paragraph_set_text(steps.last, "The agent's last turn did "
			"nothing to this screen since the daemon started.");
		lrg_widget_set_enabled(steps.save, FALSE);
	} else {
		g_autofree gchar *line = steps_line(
			json_object_get_array_member(run, "steps"));
		gboolean saveable = clawt_ipc_payload_boolean(run, "saveable", FALSE);
		const gchar *why = clawt_ipc_payload_string(run, "why_not");
		g_autofree gchar *said = g_strdup_printf("Its last turn: %s.%s%s",
			line, saveable ? "" : " Cannot be saved: ",
			saveable ? "" : (why != NULL ? why : "unknown"));

		clawt_lrg_paragraph_set_text(steps.last, said);
		lrg_widget_set_enabled(steps.save, saveable);
	}

	clawt_lrg_app_wake(app);
}

static void
on_list(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *saved = (root != NULL && json_object_has_member(root, "saved"))
		? json_object_get_array_member(root, "saved") : NULL;
	guint i;

	if (steps.list == NULL)
		return;

	clawt_lrg_clear(steps.list);

	for (i = 0; saved != NULL && i < json_array_get_length(saved); i++) {
		JsonObject *entry = json_array_get_object_element(saved, i);
		const gchar *name = clawt_ipc_payload_string(entry, "name");
		g_autofree gchar *line = steps_line(
			json_object_has_member(entry, "steps")
				? json_object_get_array_member(entry, "steps") : NULL);
		LrgWidget *row = clawt_lrg_row(name != NULL ? name : "?", line);

		clawt_lrg_row_add_suffix(row, list_button(app, "Replay", "flat",
		                                          "steps.replay", name));
		clawt_lrg_row_add_suffix(row, list_button(app, "Delete", "danger",
		                                          "steps.delete", name));
		clawt_lrg_take(steps.list, row);
	}

	clawt_lrg_app_wake(app);
}

void
clawt_lrg_steps_refresh(
	ClawtLrgApp *app
){
	if (steps.list == NULL || app->agent_id == NULL)
		return;

	clawt_lrg_request(app, "steps.last",
	                  clawt_ipc_payload_new("agent", app->agent_id, NULL),
	                  CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_last, NULL,
	                  NULL);
	clawt_lrg_request(app, "steps.list",
	                  clawt_ipc_payload_new("agent", app->agent_id, NULL),
	                  CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_list, NULL,
	                  NULL);
}

LrgWidget *
clawt_lrg_steps_build(
	ClawtLrgApp *app
){
	LrgWidget *box = clawt_lrg_vbox(8.0f);
	LrgWidget *save_row = clawt_lrg_hbox(8.0f);

	clawt_lrg_take(box, clawt_lrg_label("Saved steps", "heading"));
	clawt_lrg_take(box, clawt_lrg_paragraph("What the agent pressed on this "
		"screen, kept so it can be done again with no model. Replay needs "
		"you to hold the screen and refuses a screen that has changed.",
		"muted"));

	steps.last = clawt_lrg_paragraph("", NULL);
	clawt_lrg_add(box, steps.last);
	g_object_unref(steps.last);

	steps.name = clawt_lrg_entry(NULL, "Name these steps");
	lrg_widget_set_fill_weight(steps.name, 1.0f);
	clawt_lrg_add(save_row, steps.name);
	g_object_unref(steps.name);
	steps.save = clawt_lrg_button("Save", "primary",
	                              G_CALLBACK(on_save_clicked), app);
	lrg_widget_set_enabled(steps.save, FALSE);
	clawt_lrg_add(save_row, steps.save);
	g_object_unref(steps.save);
	clawt_lrg_take(box, save_row);

	steps.list = clawt_lrg_vbox(6.0f);
	clawt_lrg_add(box, steps.list);
	g_object_unref(steps.list);

	return box;
}
