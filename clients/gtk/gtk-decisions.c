/*
 * gtk-decisions.c - The decisions page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Choices agents are waiting on somebody for.  Its own page rather than
 * a section of the alerts panel: an alert is something that happened
 * and a decision is something that needs you.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

/*
 * The operator answers, and the daemon routes it back to whoever asked.
 */
static void
on_decision_answer(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    GtkWidget *entry = g_object_get_data(G_OBJECT(button), "entry");
    const gchar *chosen = g_object_get_data(G_OBJECT(button), "option");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *answer = chosen;

    /*
     * A typed answer wins over a clicked option, because somebody who
     * typed something meant it -- and "neither, do X" is the most
     * valuable answer there is.
     */
    if (entry != NULL) {
        const gchar *typed = gtk_editable_get_text(GTK_EDITABLE(entry));

        if (typed != NULL && *typed != '\0')
            answer = typed;
    }

    if (id == NULL || answer == NULL || *answer == '\0')
        return;

    reply = clawt_window_request(self, "decision.answer",
                                 clawt_build_payload("decision", id,
                                                     "answer", answer,
                                                     NULL));

    if (reply != NULL)
        clawt_gtk_refresh_decisions(self);
}

/*
 * Gone from the inbox for good, whatever state it is in.  Dismiss keeps
 * the record and says the operator passed; this is for an entry nobody
 * wants to see again -- a stale ask, one raised by mistake, one a daemon
 * from before 0.3.0 left open past its deadline.
 */
static void
on_decision_delete(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    g_autoptr(JsonNode) reply = NULL;

    if (id == NULL)
        return;

    reply = clawt_window_request(self, "decision.delete",
                                 clawt_build_payload("decision", id, NULL));

    if (reply != NULL)
        clawt_gtk_refresh_decisions(self);
}

static void
on_decision_dismiss(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    g_autoptr(JsonNode) reply = NULL;

    if (id == NULL)
        return;

    reply = clawt_window_request(self, "decision.dismiss",
                                 clawt_build_payload("decision", id, NULL));

    if (reply != NULL)
        clawt_gtk_refresh_decisions(self);
}

/*
 * The inset every row inside a decision shares.
 *
 * One function because four of them take it and a fifth added later
 * would otherwise be the one that sits flush against the card edge --
 * which is exactly how the options row and the answer box came to have
 * different margins from each other.
 */
/*
 * One offered option, as a button whose label wraps.
 *
 * gtk_button_new_with_label() makes a label that does not wrap, so the
 * button becomes as wide as the sentence and pushes the whole card past
 * the window -- which, with the page's horizontal scroll, is how the
 * decisions view became unreadable.
 *
 * Left-aligned because an option is read rather than scanned: a centred
 * label broken over three lines gives the eye no edge to come back to.
 */
static GtkWidget *
decision_option_button(const gchar *option)
{
    GtkWidget *text = gtk_label_new(option);
    GtkWidget *button = gtk_button_new();

    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0);

    gtk_button_set_child(GTK_BUTTON(button), text);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);

    return button;
}

static void
pad_decision_row(GtkWidget *widget)
{
    gtk_widget_set_margin_top(widget, 6);
    gtk_widget_set_margin_bottom(widget, 6);
    gtk_widget_set_margin_start(widget, 12);
    gtk_widget_set_margin_end(widget, 12);
}

/* ── Skill proposals ─────────────────────────────────────────────── */

/*
 * Sends one of the three answers a proposal takes.
 *
 * Built with a JsonBuilder rather than clawt_build_payload(), because the
 * note and the edited text are optional and a NULL in the middle of that
 * function's argument list would end it early and silently drop the rest.
 */
static void
resolve_proposal(ClawtWindow *self, const gchar *id, const gchar *action,
                 const gchar *note, const gchar *description,
                 const gchar *body)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "proposal");
    json_builder_add_string_value(builder, id);
    json_builder_set_member_name(builder, "action");
    json_builder_add_string_value(builder, action);

    if (note != NULL && *note != '\0') {
        json_builder_set_member_name(builder, "note");
        json_builder_add_string_value(builder, note);
    }
    if (description != NULL) {
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, description);
    }
    if (body != NULL) {
        json_builder_set_member_name(builder, "body");
        json_builder_add_string_value(builder, body);
    }
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "skill.proposal_resolve",
                                 json_builder_get_root(builder));

    if (reply != NULL) {
        clawt_gtk_refresh_decisions(self);
        clawt_gtk_refresh_skills(self);
    }
}

/* Approve or Reject, with whatever the person typed as the note. */
static void
on_proposal_answer(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "proposal-id");
    const gchar *action = g_object_get_data(G_OBJECT(button), "action");
    GtkWidget *entry = g_object_get_data(G_OBJECT(button), "entry");

    if (id == NULL || action == NULL)
        return;

    resolve_proposal(self, id, action,
                     entry != NULL ? gtk_editable_get_text(GTK_EDITABLE(entry))
                                   : NULL,
                     NULL, NULL);
}

static void
on_proposal_edit_response(AdwAlertDialog *dialog, const gchar *response,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(dialog), "proposal-id");
    GtkWidget *description = g_object_get_data(G_OBJECT(dialog),
                                               "description");
    GtkTextView *body = g_object_get_data(G_OBJECT(dialog), "body");
    GtkTextIter start;
    GtkTextIter end;
    g_autofree gchar *text = NULL;

    if (g_strcmp0(response, "save") != 0 || id == NULL)
        return;

    gtk_text_buffer_get_bounds(gtk_text_view_get_buffer(body), &start, &end);
    text = gtk_text_buffer_get_text(gtk_text_view_get_buffer(body), &start,
                                    &end, FALSE);

    resolve_proposal(self, id, "edit", NULL,
                     gtk_editable_get_text(GTK_EDITABLE(description)), text);
}

/*
 * Edit: the agent's text in an editor, saved as the person's version.
 *
 * Prefilled with what the daemon sent, which is the escaped text -- so a
 * forged notice line in the body arrives already rewritten, and saving
 * cannot approve the forgery back in without somebody seeing it.
 */
static void
on_proposal_edit(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "proposal-id");
    const gchar *description = g_object_get_data(G_OBJECT(button),
                                                 "description");
    const gchar *body = g_object_get_data(G_OBJECT(button), "body");
    AdwDialog *dialog;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *entry = adw_entry_row_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();

    dialog = adw_alert_dialog_new(
        "Edit the proposed skill",
        "Saving writes your version and enables it. The agent is told the "
        "skill is not exactly what it wrote.");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), "Description");
    gtk_editable_set_text(GTK_EDITABLE(entry),
                          description != NULL ? description : "");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry);

    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             body != NULL ? body : "", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),
                                               320);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroll, "card");

    gtk_box_append(GTK_BOX(box), group);
    gtk_box_append(GTK_BOX(box), scroll);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "cancel", "Cancel",
                                   "save", "Save and approve", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "save", ADW_RESPONSE_SUGGESTED);

    g_object_set_data_full(G_OBJECT(dialog), "proposal-id", g_strdup(id),
                           g_free);
    g_object_set_data(G_OBJECT(dialog), "description", entry);
    g_object_set_data(G_OBJECT(dialog), "body", view);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_proposal_edit_response), self);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

/* A plain-text label that wraps; never markup, since the agent wrote it. */
static GtkWidget *
proposal_label(const gchar *text, gboolean monospace)
{
    GtkWidget *label = gtk_label_new(NULL);

    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_label_set_use_markup(GTK_LABEL(label), FALSE);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);

    if (monospace)
        gtk_widget_add_css_class(label, "monospace");

    return label;
}

static GtkWidget *
proposal_button(ClawtWindow *self, const gchar *label, const gchar *id,
                const gchar *action, GCallback callback, const gchar *style)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "proposal-id", g_strdup(id),
                           g_free);
    g_object_set_data_full(G_OBJECT(button), "action", g_strdup(action),
                           g_free);
    g_signal_connect(button, "clicked", callback, self);

    if (style != NULL)
        gtk_widget_add_css_class(button, style);

    return button;
}

/*
 * A skill proposal's card: what the agent proposes, and the three answers.
 *
 * The title is the question's first line only -- the agent's id and a
 * validated skill name, nothing it chose freely -- and the proposed text
 * or the diff sits under it as plain monospace text, because a SKILL.md
 * squeezed into a row title is unreadable and a title is markup.  The
 * generic options and answer box are not offered: "edit" needs text and
 * free text is read as a rejection, so the buttons are the honest set.
 */
static void
clawt_gtk_proposal_review(ClawtWindow *self, GtkWidget *row,
                          JsonObject *decision, JsonObject *proposal)
{
    const gchar *id = clawt_json_string(decision, "id", "");
    const gchar *question = clawt_json_string(decision, "question", "");
    const gchar *kind = clawt_json_string(proposal, "kind", "create");
    const gchar *reason = clawt_json_string(proposal, "reason", NULL);
    const gchar *shown = g_strcmp0(kind, "update") == 0
        ? clawt_json_string(proposal, "diff", "")
        : clawt_json_string(proposal, "text", "");
    JsonArray *warnings = json_object_get_array_member(proposal, "warnings");
    g_autofree gchar *headline = NULL;
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *note = gtk_entry_new();
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *edit;
    guint w;

    {
        const gchar *end = strchr(question, '\n');

        headline = (end != NULL) ? g_strndup(question, (gsize)(end - question))
                                 : g_strdup(question);
    }

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), headline);
    adw_expander_row_set_subtitle(
        ADW_EXPANDER_ROW(row),
        "Nothing reaches a prompt until you answer.");

    if (reason != NULL && *reason != '\0') {
        g_autofree gchar *why = g_strdup_printf("Why: %s", reason);

        gtk_box_append(GTK_BOX(details), proposal_label(why, FALSE));
    }

    for (w = 0; warnings != NULL && w < json_array_get_length(warnings); w++) {
        GtkWidget *warning = proposal_label(
            json_array_get_string_element(warnings, w), FALSE);

        gtk_widget_add_css_class(warning, "warning");
        gtk_box_append(GTK_BOX(details), warning);
    }

    if (clawt_json_boolean(proposal, "stale", FALSE)) {
        GtkWidget *stale = proposal_label(
            "The file changed since this was proposed, so Approve is "
            "refused. Edit shows what is there now; or reject it.", FALSE);

        gtk_widget_add_css_class(stale, "error");
        gtk_box_append(GTK_BOX(details), stale);
    }

    gtk_box_append(GTK_BOX(details), proposal_label(shown, TRUE));
    pad_decision_row(details);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), details);

    gtk_entry_set_placeholder_text(GTK_ENTRY(note),
                                   "Why (optional, sent to the agent)");
    gtk_widget_set_hexpand(note, TRUE);
    gtk_box_append(GTK_BOX(actions), note);

    {
        GtkWidget *approve = proposal_button(self, "Approve", id, "approve",
                                             G_CALLBACK(on_proposal_answer),
                                             "suggested-action");
        GtkWidget *reject = proposal_button(self, "Reject", id, "reject",
                                            G_CALLBACK(on_proposal_answer),
                                            "destructive-action");

        g_object_set_data(G_OBJECT(approve), "entry", note);
        g_object_set_data(G_OBJECT(reject), "entry", note);
        gtk_box_append(GTK_BOX(actions), approve);
        gtk_box_append(GTK_BOX(actions), reject);
    }

    edit = proposal_button(self, "Edit", id, "edit",
                           G_CALLBACK(on_proposal_edit), NULL);
    g_object_set_data_full(G_OBJECT(edit), "description",
                           g_strdup(clawt_json_string(proposal, "description",
                                                      "")), g_free);
    g_object_set_data_full(G_OBJECT(edit), "body",
                           g_strdup(clawt_json_string(proposal, "body", "")),
                           g_free);
    gtk_box_append(GTK_BOX(actions), edit);

    pad_decision_row(actions);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), actions);
}

/* The pending proposals, by id, or NULL when the inbox holds none. */
static JsonNode *
fetch_proposals(ClawtWindow *self, JsonArray *decisions)
{
    guint i;

    for (i = 0; decisions != NULL && i < json_array_get_length(decisions);
         i++) {
        JsonObject *decision = json_array_get_object_element(decisions, i);

        if (clawt_skill_proposal_id_is_valid(
                clawt_json_string(decision, "id", NULL)))
            return clawt_window_request(self, "skill.proposals", NULL);
    }

    return NULL;
}

static JsonObject *
proposal_for(JsonNode *proposals, const gchar *id)
{
    JsonArray *items;
    guint i;

    if (proposals == NULL)
        return NULL;

    items = json_object_get_array_member(clawt_payload_of(proposals),
                                         "proposals");

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *proposal = json_array_get_object_element(items, i);

        if (g_strcmp0(clawt_json_string(proposal, "id", NULL), id) == 0)
            return proposal;
    }

    return NULL;
}

static void
refresh_decisions_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) proposals = NULL;
    JsonArray *items;
    guint i;
    guint open = 0;

    clawt_gtk_clear_list(self->decision_list);

    reply = clawt_window_request(self, "decision.list", NULL);

    if (reply == NULL)
        return;

    open = (guint)json_object_get_int_member(clawt_payload_of(reply),
                                             "open");
    items = json_object_get_array_member(clawt_payload_of(reply),
                                         "decisions");
    proposals = fetch_proposals(self, items);

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *decision = json_array_get_object_element(items, i);
        GtkWidget *row = adw_expander_row_new();
        const gchar *id = clawt_json_string(decision, "id", "");
        const gchar *fallback = clawt_json_string(decision, "default", NULL);
        const gchar *reason =
            clawt_json_string(decision, "default_reason", NULL);
        JsonArray *options = json_object_get_array_member(decision,
                                                          "options");
        GtkWidget *entry = gtk_entry_new();
        GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *answer = gtk_button_new_with_label("Answer");
        GtkWidget *drop = gtk_button_new_with_label("Does not need me");
        GtkWidget *remove = gtk_button_new_with_label("Delete");
        GtkWidget *closing = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gboolean is_rebuild = g_strcmp0(clawt_json_string(decision, "kind", NULL),
                                        "rebuild") == 0;
        const gchar *loses = clawt_json_string(decision, "loses", NULL);
        JsonObject *proposal = clawt_skill_proposal_id_is_valid(id)
                               ? proposal_for(proposals, id) : NULL;
        guint o;

        /*
         * A skill proposal draws its own card; only the two ways of
         * closing it are shared, and both reject the draft.
         */
        if (proposal != NULL) {
            clawt_gtk_proposal_review(self, row, decision, proposal);

            g_object_set_data_full(G_OBJECT(drop), "decision-id",
                                   g_strdup(id), g_free);
            g_signal_connect(drop, "clicked",
                             G_CALLBACK(on_decision_dismiss), self);
            g_object_set_data_full(G_OBJECT(remove), "decision-id",
                                   g_strdup(id), g_free);
            g_signal_connect(remove, "clicked",
                             G_CALLBACK(on_decision_delete), self);
            gtk_widget_add_css_class(remove, "destructive-action");
            gtk_box_append(GTK_BOX(closing), drop);
            gtk_box_append(GTK_BOX(closing), remove);
            gtk_widget_set_halign(closing, GTK_ALIGN_START);
            pad_decision_row(closing);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(row), closing);

            g_object_ref_sink(entry);
            g_object_unref(entry);
            g_object_ref_sink(choices);
            g_object_unref(choices);
            g_object_ref_sink(actions);
            g_object_unref(actions);
            g_object_ref_sink(answer);
            g_object_unref(answer);

            gtk_list_box_append(self->decision_list, row);
            continue;
        }

        /*
         * AdwExpanderRow, not AdwActionRow: the question is what a
         * reader scans and the options are what they act on, so the
         * second should not cost the first its line.  ExpanderRow does
         * not derive from AdwActionRow, so set_subtitle would be a
         * runtime assertion -- the title carries the default instead.
         */
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(decision,
                                                        "question", ""));

        if (fallback != NULL) {
            g_autofree gchar *going = (reason != NULL && *reason != '\0')
                ? g_strdup_printf("Going ahead with: %s \xe2\x80\x94 %s",
                                  fallback, reason)
                : g_strdup_printf("Going ahead with: %s", fallback);

            adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), going);
        }

        if (clawt_json_boolean(decision, "urgent", FALSE))
            adw_expander_row_add_suffix(
                ADW_EXPANDER_ROW(row),
                clawt_gtk_badge("closing soon", "warning",
                                "the default stops being cheap to undo within a day"));

        /*
         * A rebuild request is a yes or a no about destroying a machine,
         * so it says what goes before anybody reads the options -- and
         * offers no free-text answer, because only "approve" rebuilds
         * and anything typed would read as a yes that is not one.
         */
        if (is_rebuild)
            adw_expander_row_add_suffix(
                ADW_EXPANDER_ROW(row),
                clawt_gtk_badge("rebuild", "error",
                                "approving destroys this agent's computer and "
                                "builds a fresh one"));

        if (clawt_json_boolean(decision, "settled_by_default", FALSE))
            adw_expander_row_add_suffix(
                ADW_EXPANDER_ROW(row),
                clawt_gtk_badge("already decided", "error",
                                "the deadline passed; answering changes nothing"));

        gtk_entry_set_placeholder_text(
            GTK_ENTRY(entry),
            "\"neither, do X\" is a perfectly good answer");

        /*
         * One option per line, not a row of buttons.
         *
         * An option is a sentence -- "Re-provision clawt-oryx from a
         * proper Fedora cloud image, so the exchange mounts and the
         * default-user config land too" is a real one -- and two of
         * those side by side is wider than any window.  Stacked, each
         * reads as the thing it is: a choice somebody is being offered.
         */
        for (o = 0; options != NULL && o < json_array_get_length(options);
             o++) {
            const gchar *option = json_array_get_string_element(options, o);
            GtkWidget *pick;

            if (option == NULL || *option == '\0')
                continue;

            pick = decision_option_button(option);

            /* Approving a rebuild destroys something; it looks like it. */
            if (is_rebuild && g_strcmp0(option, "approve") == 0)
                gtk_widget_add_css_class(pick, "destructive-action");

            g_object_set_data_full(G_OBJECT(pick), "decision-id",
                                   g_strdup(id), g_free);
            g_object_set_data_full(G_OBJECT(pick), "option",
                                   g_strdup(option), g_free);
            g_signal_connect(pick, "clicked",
                             G_CALLBACK(on_decision_answer), self);
            gtk_box_append(GTK_BOX(choices), pick);
        }

        g_object_set_data_full(G_OBJECT(answer), "decision-id",
                               g_strdup(id), g_free);
        g_object_set_data(G_OBJECT(answer), "entry", entry);
        g_signal_connect(answer, "clicked", G_CALLBACK(on_decision_answer),
                         self);

        g_object_set_data_full(G_OBJECT(drop), "decision-id",
                               g_strdup(id), g_free);
        g_signal_connect(drop, "clicked", G_CALLBACK(on_decision_dismiss),
                         self);

        g_object_set_data_full(G_OBJECT(remove), "decision-id",
                               g_strdup(id), g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_decision_delete),
                         self);
        gtk_widget_add_css_class(remove, "destructive-action");
        gtk_widget_set_tooltip_text(
            remove, "Remove it from the inbox for good; an open one is "
                    "withdrawn and the agent carries on with its default");

        gtk_widget_add_css_class(answer, "suggested-action");

        /*
         * The answer box and its button on one line, because they are
         * one action.  They used to sit in separate rows with the
         * option buttons between them, so the control that sends what
         * you typed was three widgets away from where you typed it.
         */
        {
            GtkWidget *typed = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

            gtk_widget_set_hexpand(entry, TRUE);
            gtk_box_append(GTK_BOX(typed), entry);
            gtk_box_append(GTK_BOX(typed), answer);
            gtk_box_append(GTK_BOX(actions), typed);
            gtk_widget_set_hexpand(typed, TRUE);
        }

        /* the two ways of closing it, side by side and apart from answering */
        gtk_box_append(GTK_BOX(closing), drop);
        gtk_box_append(GTK_BOX(closing), remove);
        gtk_widget_set_halign(closing, GTK_ALIGN_START);

        if (is_rebuild && loses != NULL) {
            GtkWidget *warning = gtk_label_new(NULL);
            g_autofree gchar *said = g_strdup_printf("What a rebuild destroys: %s",
                                                     loses);

            gtk_label_set_text(GTK_LABEL(warning), said);
            gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
            gtk_label_set_xalign(GTK_LABEL(warning), 0.0f);
            gtk_widget_add_css_class(warning, "warning");
            pad_decision_row(warning);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(row), warning);
        }

        if (gtk_widget_get_first_child(choices) != NULL) {
            pad_decision_row(choices);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(row), choices);
        } else {
            g_object_ref_sink(choices);
            g_object_unref(choices);
        }

        pad_decision_row(actions);
        pad_decision_row(closing);

        if (!is_rebuild) {
            adw_expander_row_add_row(ADW_EXPANDER_ROW(row), actions);
        } else {
            g_object_ref_sink(actions);
            g_object_unref(actions);
        }
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), closing);

        gtk_list_box_append(self->decision_list, row);
    }

    /*
     * The badge is what makes this a surface rather than a page nobody
     * opens: an agent waiting on a person has to be visible without
     * anybody thinking to look.
     */
    clawt_gtk_set_page_badge(self, CLAWT_PAGE_DECISIONS, open, open > 0);
}

void
clawt_gtk_refresh_decisions(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_DECISIONS))
        return;

    do {
        refresh_decisions_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_DECISIONS));
}

GtkWidget *
clawt_gtk_build_decision_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *clamp = adw_clamp_new();

    self->decision_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->decision_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->decision_list), "boxed-list");
    gtk_widget_set_margin_top(GTK_WIDGET(self->decision_list), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->decision_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->decision_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->decision_list), 12);

    /*
     * The same measure the chat has, from the same resolver, so a reader
     * who widened the conversation gets it here too and there is no
     * second answer to what a readable line is.  A decision's question
     * is prose and is read the same way.
     */
    self->decision_clamp = clamp;
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), clawt_gtk_chat_measure(self));
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(self->decision_list));

    /*
     * Hugging the top rather than filling the viewport.  A GtkListBox
     * left at the default alignment takes the whole height a scrolled
     * window offers it, so one short decision drew a card with the
     * question at the top and eight hundred pixels of empty frame under
     * it -- which reads as a page that failed to load the rest.
     */
    gtk_widget_set_valign(clamp, GTK_ALIGN_START);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    /*
     * And it does not scroll sideways.
     *
     * This is the whole reason the page was unreadable.  A wrapping
     * GtkLabel still reports the *unwrapped* string as its natural
     * width, and a GtkScrolledWindow left at the default
     * GTK_POLICY_AUTOMATIC gives its child exactly that -- so a
     * question of any length made the row as wide as the sentence, ran
     * it off the right edge of the window, and took the default line,
     * the answer box and every option button with it.  Nothing was
     * ellipsised and nothing looked broken; the page had simply become
     * wider than the screen, so all anybody saw was the first screenful
     * of each line.
     *
     * Refusing the horizontal scroll is what makes the wrap that
     * libadwaita already asks for actually happen.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    /*
     * Recorded before the follow is armed.  push_chat_measure() reads
     * this scroller to resolve the column, and the notify it connects
     * to can fire during realisation -- before a later assignment would
     * have run.
     */
    self->decision_scroll = GTK_SCROLLED_WINDOW(scroll);
    clawt_gtk_follow_viewport_width(self, scroll);

    return scroll;
}
