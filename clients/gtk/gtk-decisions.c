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

static void
refresh_decisions_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
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
        guint o;

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

        gtk_widget_set_halign(drop, GTK_ALIGN_START);

        if (gtk_widget_get_first_child(choices) != NULL) {
            pad_decision_row(choices);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(row), choices);
        } else {
            g_object_ref_sink(choices);
            g_object_unref(choices);
        }

        pad_decision_row(actions);
        pad_decision_row(drop);

        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), actions);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), drop);

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
