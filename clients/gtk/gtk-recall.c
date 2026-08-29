/*
 * gtk-recall.c - The memory page: fleet-wide recall and the operator model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things a person needs that are not about one agent.
 *
 * Recall searches what was actually said, across every conversation in
 * the fleet and every session -- which is a different question from
 * "what does this agent remember", and the answer to it lives in a
 * different database.  The operator profile is what the fleet believes
 * about the person running it, and it is here because a model of
 * somebody that they cannot read is not something to build: they can
 * read every line, edit the half they wrote, and see the date and the
 * agent behind the half they did not.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/*
 * A row that says why a list is empty, rather than an empty list.
 *
 * "Nothing matched" and "nothing has ever been said" are different
 * situations with different next steps, and a bare empty box says
 * neither -- which is how an empty store and a failed FTS5 parse came to
 * look identical in the first place.
 */
static GtkWidget *
placeholder_row(const gchar *title, const gchar *subtitle)
{
    GtkWidget *row = adw_action_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_sensitive(row, FALSE);

    return row;
}

/*
 * A heading between the sections of this page.
 */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(label, 6);

    return label;
}

/* ── Recall ──────────────────────────────────────────────────────── */

static void
refresh_recall_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    const gchar *query;
    JsonArray *hits;
    guint i;

    if (self->recall_list == NULL || self->recall_entry == NULL)
        return;

    clawt_gtk_clear_list(self->recall_list);

    query = gtk_editable_get_text(GTK_EDITABLE(self->recall_entry));

    if (query == NULL || *query == '\0') {
        gtk_list_box_append(self->recall_list,
                            placeholder_row(
                                "Search every conversation",
                                "What was said, across all sessions and "
                                "every room -- not what an agent chose to "
                                "remember."));
        return;
    }

    reply = clawt_window_request(self, "memory.recall",
                                 clawt_build_payload("query", query, NULL));

    if (reply == NULL)
        return;

    hits = json_object_get_array_member(clawt_payload_of(reply), "hits");

    if (hits == NULL || json_array_get_length(hits) == 0) {
        /*
         * The reason, not only the fact.  A query is quoted as an FTS5
         * phrase by the daemon, so a stray quote is searched for rather
         * than being a syntax error -- but an empty index looks exactly
         * like a query that matched nothing, and the two need different
         * next steps.
         */
        gint64 indexed = clawt_json_int(clawt_payload_of(reply), "indexed", 0);

        gtk_list_box_append(
            self->recall_list,
            placeholder_row(
                indexed > 0 ? "Nothing matched"
                            : "Nothing has been said yet",
                indexed > 0
                ? "Every word is searched as a phrase, so quotes and "
                  "brackets are looked for rather than interpreted."
                : "The transcript index fills as the fleet talks."));
        return;
    }

    for (i = 0; i < json_array_get_length(hits); i++) {
        JsonObject *hit = json_array_get_object_element(hits, i);
        GtkWidget *row = adw_action_row_new();
        g_autoptr(GDateTime) when = NULL;
        g_autofree gchar *subtitle = NULL;
        const gchar *from = clawt_json_string(hit, "from_name", NULL);

        if (from == NULL)
            from = clawt_json_string(hit, "from", "?");

        when = g_date_time_new_from_unix_local(
            clawt_json_int(hit, "at", 0));

        /*
         * The clock and the day, because a recall result is a record
         * being matched against something else -- a task, a journal
         * entry, an event line.  The relative times belong to the lists
         * that are about recency.
         */
        subtitle = g_strdup_printf("%s in %s", from,
                                   clawt_json_string(hit, "room", "?"));

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(hit, "body", ""));
        adw_preferences_row_set_title_selectable(ADW_PREFERENCES_ROW(row),
                                                 TRUE);
        gtk_label_set_wrap(GTK_LABEL(gtk_widget_get_first_child(row)), TRUE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

        if (when != NULL) {
            g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
            g_autofree gchar *stamp = clawt_chat_time_label(when);
            g_autofree gchar *label = g_strdup_printf("%s %s", day, stamp);
            GtkWidget *time_label = gtk_label_new(label);

            gtk_widget_add_css_class(time_label, "dim-label");
            gtk_widget_add_css_class(time_label, "numeric");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), time_label);
        }

        gtk_list_box_append(self->recall_list, row);
    }
}

void
clawt_gtk_refresh_recall(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_RECALL))
        return;

    do {
        refresh_recall_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_RECALL));
}

static void
on_recall_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;

    clawt_gtk_refresh_recall(user_data);
}

/* ── The operator profile ────────────────────────────────────────── */

static void
on_operator_save(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkTextBuffer *buffer;
    GtkTextIter start;
    GtkTextIter end;
    g_autofree gchar *text = NULL;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    if (self->operator_view == NULL)
        return;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->operator_view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    reply = clawt_window_request(self, "operator.set",
                                 clawt_build_payload("text", text, NULL));

    if (reply == NULL)
        return;

    /*
     * A toast rather than a banner: this answers a question somebody is
     * holding right now -- "did that save" -- rather than describing a
     * condition the window stays in.
     */
    clawt_window_toast(self, "Saved. Every agent's USER.org was rewritten.");
    clawt_gtk_refresh_operator(self);
}

static void
refresh_operator_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;
    JsonArray *learned;
    GtkTextBuffer *buffer;
    const gchar *text;
    guint i;

    if (self->operator_view == NULL || self->operator_learned == NULL)
        return;

    reply = clawt_window_request(self, "operator.get", NULL);

    if (reply == NULL)
        return;

    payload = clawt_payload_of(reply);
    text = clawt_json_string(payload, "text", "");

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->operator_view));
    gtk_text_buffer_set_text(buffer, text, -1);

    if (self->operator_banner != NULL) {
        /*
         * A banner, because this is a condition the page is in until
         * somebody changes a setting -- not an event.  Saying it here
         * rather than leaving the editor to look broken: text typed into
         * a profile nobody reads is worse than an empty page.
         */
        gboolean enabled = clawt_json_boolean(payload, "enabled", FALSE);

        adw_banner_set_title(
            ADW_BANNER(self->operator_banner),
            "memories.operator_profile is off, so no agent is told any of "
            "this.");
        gtk_widget_set_visible(self->operator_banner, !enabled);
    }

    clawt_gtk_clear_list(self->operator_learned);

    learned = json_object_has_member(payload, "learned")
              ? json_object_get_array_member(payload, "learned") : NULL;

    if (learned == NULL || json_array_get_length(learned) == 0) {
        gtk_list_box_append(
            self->operator_learned,
            placeholder_row(
                "Nothing recorded yet",
                "Agents add to this by writing fleet-scope memories in "
                "the 'operator' category."));
        return;
    }

    for (i = 0; i < json_array_get_length(learned); i++) {
        JsonObject *memory = json_array_get_object_element(learned, i);
        GtkWidget *row = adw_action_row_new();
        g_autoptr(GDateTime) when = NULL;
        g_autofree gchar *subtitle = NULL;

        when = g_date_time_new_from_unix_local(
            clawt_json_int(memory, "created_at", 0));

        {
            g_autofree gchar *day = (when != NULL)
                ? g_date_time_format(when, "%Y-%m-%d") : NULL;

            /*
             * The date matters here more than anywhere else on the page:
             * an inference the fleet drew six months ago and one it drew
             * this morning are not the same claim about a person.
             */
            subtitle = g_strdup_printf(
                "%s%s%s", clawt_json_string(memory, "source",
                                            "unattributed"),
                day != NULL ? ", " : "", day != NULL ? day : "");
        }

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(memory, "content",
                                                        ""));
        gtk_label_set_wrap(GTK_LABEL(gtk_widget_get_first_child(row)), TRUE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

        gtk_list_box_append(self->operator_learned, row);
    }
}

void
clawt_gtk_refresh_operator(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_OPERATOR))
        return;

    do {
        refresh_operator_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_OPERATOR));
}

/* ── The page ────────────────────────────────────────────────────── */

GtkWidget *
clawt_gtk_build_recall_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *clamp = adw_clamp_new();
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *search_row;
    GtkWidget *save;

    gtk_widget_set_margin_top(column, 12);
    gtk_widget_set_margin_bottom(column, 12);
    gtk_widget_set_margin_start(column, 12);
    gtk_widget_set_margin_end(column, 12);

    /* ── Recall ── */
    search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    self->recall_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(self->recall_entry, TRUE);
    gtk_search_entry_set_placeholder_text(
        GTK_SEARCH_ENTRY(self->recall_entry),
        "Search everything the fleet has said");
    g_signal_connect(self->recall_entry, "activate",
                     G_CALLBACK(on_recall_activate), self);
    gtk_box_append(GTK_BOX(search_row), self->recall_entry);
    gtk_box_append(GTK_BOX(column), search_row);

    self->recall_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->recall_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->recall_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->recall_list), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->recall_list));

    /* ── The operator profile ── */
    self->operator_banner = adw_banner_new("");
    gtk_widget_set_visible(self->operator_banner, FALSE);
    gtk_box_append(GTK_BOX(column), self->operator_banner);

    gtk_box_append(GTK_BOX(column),
                   section_label("About you"));

    self->operator_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->operator_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->operator_view), TRUE);
    gtk_widget_set_size_request(self->operator_view, -1, 160);
    gtk_widget_add_css_class(self->operator_view, "card");
    gtk_box_append(GTK_BOX(column), self->operator_view);

    save = gtk_button_new_with_label("Save profile");
    gtk_widget_set_halign(save, GTK_ALIGN_END);
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(on_operator_save), self);
    gtk_box_append(GTK_BOX(column), save);

    gtk_box_append(GTK_BOX(column),
                   section_label("What the fleet recorded"));

    self->operator_learned = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->operator_learned,
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->operator_learned),
                             "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->operator_learned),
                          GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->operator_learned));

    adw_clamp_set_child(ADW_CLAMP(clamp), column);
    gtk_widget_set_valign(clamp, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    /*
     * No horizontal scroll.  A wrapping GtkLabel still reports the
     * unwrapped string as its natural width, and a recall result is a
     * whole message -- so at GTK_POLICY_AUTOMATIC one long line would
     * make the page wider than the window and nothing would look
     * broken, it would just be cut off.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    return scroll;
}
