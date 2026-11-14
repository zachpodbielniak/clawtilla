/*
 * lrg-settings-orchestration.c - Settings: orchestration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The acknowledgement check (orchestration.ack_check_model), chosen
 * from libclawt's walk of the model catalogue so this page offers the
 * GTK and web pages' list.  The combo is rebuilt on every read, with
 * the daemon's value selected: a combo built once at page creation
 * would show Off until the reply landed, and a person changing it in
 * that window would be answering the wrong question.
 */

#include "clawt-lrg.h"

typedef struct {
	LrgWidget   *slot;        /* holds the combo, rebuilt per read */
	guint        generation;
} Orchestration;

static Orchestration orchestration;

static void refresh(ClawtLrgApp *app);

static void
on_saved(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	const gchar *value = user_data;

	(void)reply;

	if (error == NULL)
		clawt_lrg_toast(app, (value == NULL || *value == '\0')
			? "Acknowledgement check off."
			: "Acknowledgement check saved.");

	/* Read back, so a refusal leaves the combo showing what is set. */
	refresh(app);
}

static void
on_model_changed(
	LrgComboBox *combo,
	gpointer     user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *value = clawt_lrg_combo_id(LRG_WIDGET(combo));

	if (value == NULL)
		return;

	clawt_lrg_request(app, "orchestration.set",
			  clawt_ipc_payload_new("ack_check_model", value, NULL),
			  CLAWT_LRG_REQUEST_NONE, on_saved, g_strdup(value),
			  g_free);
}

static void
on_read(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	const gchar *current = NULL;
	g_autoptr(GPtrArray) ids = g_ptr_array_new();
	g_autoptr(GPtrArray) labels = g_ptr_array_new();
	LrgWidget *combo;
	guint i;

	(void)error;

	if (GPOINTER_TO_UINT(user_data) != orchestration.generation ||
	    orchestration.slot == NULL)
		return;

	if (root != NULL)
		current = clawt_ipc_payload_string(root, "ack_check_model");

	for (i = 0; i < clawt_ack_check_model_count(); i++) {
		g_ptr_array_add(ids, (gpointer)clawt_ack_check_model_nth_value(i));
		g_ptr_array_add(labels, (gpointer)clawt_ack_check_model_nth_label(i));
	}
	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(labels, NULL);

	/*
	 * A model written into the file by hand is kept as a choice by
	 * clawt_lrg_combo(), so it is not shown as Off -- which saving the
	 * page would then make true.
	 */
	combo = clawt_lrg_combo((const gchar *const *)ids->pdata,
				(const gchar *const *)labels->pdata,
				current != NULL ? current : "");

	/* connected after the selection, so building is not a change */
	g_signal_connect(combo, "changed", G_CALLBACK(on_model_changed), app);

	clawt_lrg_clear(orchestration.slot);
	clawt_lrg_take(orchestration.slot, combo);
	clawt_lrg_app_wake(app);
}

static void
refresh(
	ClawtLrgApp *app
){
	if (orchestration.slot == NULL)
		return;

	orchestration.generation++;
	clawt_lrg_request(app, "orchestration.get", NULL, CLAWT_LRG_QUIET,
			  on_read, GUINT_TO_POINTER(orchestration.generation),
			  NULL);
}

static LrgWidget *
build(
	ClawtLrgApp *app
){
	LrgWidget *page = clawt_lrg_page_new("Orchestration",
					     "How agents talk to each other");
	LrgWidget *box = clawt_lrg_vbox(10.0f);
	LrgWidget *form = clawt_lrg_form_new();

	(void)app;

	clawt_lrg_take(box, clawt_lrg_paragraph(
		"When one agent answers another with only an acknowledgement -- "
		"\"thanks, noted\" -- waking the agent that asked costs it a whole "
		"turn. With a model chosen here, a short answer from a turn that "
		"did nothing else is put to that model first. Only a plain ACK "
		"skips the wake; anything else, a failure, or no verdict within 8 "
		"seconds wakes the sender as before. A skipped answer stays in the "
		"room marked \"acknowledged (no wake)\", and the sender is told "
		"about it on its next turn. Off by default.", "muted"));

	orchestration.slot = clawt_lrg_vbox(0.0f);
	clawt_lrg_form_add(form, "Acknowledgement check", orchestration.slot,
			   "A small, fast model reached over HTTP; its key is "
			   "configured as for AI assist.");
	clawt_lrg_take(box, form);

	clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

	return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "orchestration", "Orchestration", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_orchestration_page(void)
{
    return &page_impl;
}
