/*
 * gtk-grok.c - The Grok page: the bots, sending them work, and what came back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three groups, top to bottom: whether a job could be sent at all right
 * now (the daemon's own answer, the same check a send makes), the bots
 * with who may reach each and the two things done to a bot once -- its
 * routine's standing prompt and its MCP key -- and every job, newest
 * first, with its state in the tone libclawt gives it.  Between them, a
 * composer that hands a job to a bot on an agent's behalf.
 *
 * The page's badge is the daemon's count of jobs that need somebody --
 * failed, unanswered, refused, or back with a question -- read from
 * `grok.jobs` whenever `grok.job_updated` arrives, so it goes down as
 * well as up.
 */

#include "clawt-window-private.h"

/* ── Small dialogs ───────────────────────────────────────────────── */

static AdwDialog *
text_dialog(const gchar *title, const gchar *lead, const gchar *text)
{
    AdwDialog *window = adw_dialog_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    GtkWidget *label = gtk_label_new(lead);

    adw_dialog_set_title(window, title);
    adw_dialog_set_content_width(window, 680);
    adw_dialog_set_content_height(window, 520);

    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "dim-label");

    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             text, -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), scroll);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_dialog_set_child(window, toolbar);

    return window;
}

static void
on_routine_prompt(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *bot = g_object_get_data(G_OBJECT(button), "bot");
    g_autoptr(JsonNode) reply = NULL;
    AdwDialog *dialog;

    reply = clawt_window_request(self, "grok.routine_prompt",
                                 clawt_build_payload("bot", bot, NULL));

    if (reply == NULL)
        return;

    dialog = text_dialog(
        "Routine prompt",
        "Paste this into the bot's routine in Grok, once. Every job also "
        "carries its own instructions for reporting back, so this only "
        "sets the bot's habits.",
        clawt_json_string(clawt_payload_of(reply), "text", ""));
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
on_mint_confirmed(AdwAlertDialog *confirm, gchar *response,
                  gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *bot = g_object_get_data(G_OBJECT(confirm), "bot");
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *text = NULL;
    JsonObject *root;

    if (g_strcmp0(response, "mint") != 0)
        return;

    reply = clawt_window_request(self, "grok.mcp_key",
                                 clawt_build_payload("bot", bot, NULL));

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);

    /*
     * Said as shown-once because the daemon said so, not because this
     * client assumes it -- the reading the trigger secret dialog takes.
     */
    text = g_strdup_printf(
        "URL:     %s\nAPI key: %s\n",
        clawt_json_string(root, "mcp_url",
                          "(not reachable yet -- see the top of the page)"),
        clawt_json_string(root, "key", "?"));

    adw_dialog_present(
        text_dialog("Copy this now",
                    json_object_has_member(root, "secret_shown_once") &&
                        json_object_get_boolean_member(root,
                                                       "secret_shown_once")
                        ? "In Grok, ask the bot to add a remote MCP "
                          "connection with this URL and API key. This is "
                          "the only time the key is shown; minting another "
                          "replaces it."
                        : "In Grok, ask the bot to add a remote MCP "
                          "connection with this URL and API key.",
                    text),
        GTK_WIDGET(self));

    clawt_gtk_refresh_grok(self);
}

static void
on_mint_key(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *bot = g_object_get_data(G_OBJECT(button), "bot");
    g_autofree gchar *heading = g_strdup_printf("Mint an MCP key for %s?",
                                                bot);
    AdwAlertDialog *confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        heading,
        "The key is shown once. If the bot already has one, that one stops "
        "working at once and the bot has to be given the new one."));

    adw_alert_dialog_add_response(confirm, "cancel", "Cancel");
    adw_alert_dialog_add_response(confirm, "mint", "Mint");
    adw_alert_dialog_set_default_response(confirm, "cancel");
    adw_alert_dialog_set_close_response(confirm, "cancel");
    g_object_set_data_full(G_OBJECT(confirm), "bot", g_strdup(bot), g_free);
    g_signal_connect(confirm, "response", G_CALLBACK(on_mint_confirmed),
                     self);
    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(self));
}

/* ── Sending ─────────────────────────────────────────────────────── */

static void
on_send(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->grok_instructions);
    GtkTextIter start;
    GtkTextIter end;
    g_autofree gchar *instructions = NULL;
    GStrv bots = g_object_get_data(G_OBJECT(self->grok_bot_row), "ids");
    GStrv agents = g_object_get_data(G_OBJECT(self->grok_agent_row), "ids");
    guint bot = adw_combo_row_get_selected(ADW_COMBO_ROW(self->grok_bot_row));
    guint agent =
        adw_combo_row_get_selected(ADW_COMBO_ROW(self->grok_agent_row));
    const gchar *context =
        gtk_editable_get_text(GTK_EDITABLE(self->grok_context_row));

    (void)button;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    instructions = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (bots == NULL || agents == NULL || bot == GTK_INVALID_LIST_POSITION ||
        agent == GTK_INVALID_LIST_POSITION ||
        bot >= g_strv_length(bots) || agent >= g_strv_length(agents)) {
        clawt_window_toast(self, "Choose a bot and an agent first");
        return;
    }

    if (instructions == NULL || *g_strstrip(instructions) == '\0') {
        clawt_window_toast(self, "Say what the bot should do");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "bot");
    json_builder_add_string_value(builder, bots[bot]);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agents[agent]);
    json_builder_set_member_name(builder, "instructions");
    json_builder_add_string_value(builder, instructions);

    if (context != NULL && *context != '\0') {
        json_builder_set_member_name(builder, "context");
        json_builder_add_string_value(builder, context);
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(self, "grok.send",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    {
        JsonObject *job = json_object_get_object_member(
            clawt_payload_of(reply), "job");
        g_autofree gchar *said = g_strdup_printf(
            "Sent to %s as %s; the answer goes to %s",
            clawt_json_string(job, "bot", "?"),
            clawt_json_string(job, "id", "?"), agents[agent]);

        clawt_window_toast(self, said);
    }

    gtk_text_buffer_set_text(buffer, "", -1);
    gtk_editable_set_text(GTK_EDITABLE(self->grok_context_row), "");
    clawt_gtk_refresh_grok(self);
}

/* ── Rows ────────────────────────────────────────────────────────── */

static GtkWidget *
bot_row(ClawtWindow *self, JsonObject *bot)
{
    GtkWidget *row = adw_action_row_new();
    const gchar *name = clawt_json_string(bot, "name", "?");
    const gchar *description = clawt_json_string(bot, "description", "");
    JsonArray *agents = json_object_get_array_member(bot, "agents");
    g_autoptr(GString) subtitle = g_string_new(description);
    GtkWidget *prompt = gtk_button_new_with_label("Routine prompt");
    GtkWidget *mint = gtk_button_new_with_label("Mint MCP key");
    guint a;

    if (subtitle->len > 0)
        g_string_append(subtitle, "\n");

    g_string_append(subtitle, "Reachable by ");

    for (a = 0; agents != NULL && a < json_array_get_length(agents); a++) {
        if (a > 0)
            g_string_append(subtitle, ", ");
        g_string_append(subtitle, json_array_get_string_element(agents, a));
    }

    clawt_gtk_set_row_text(row, name, subtitle->str);

    if (!clawt_json_boolean(bot, "key_configured", FALSE))
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
            clawt_gtk_badge("no key", clawt_gtk_tone_class("bad"),
                            "The integration names no routine key"));

    adw_action_row_add_suffix(ADW_ACTION_ROW(row),
        clawt_gtk_badge(clawt_json_boolean(bot, "mcp_key", FALSE)
                            ? "MCP" : "HTTP only",
                        clawt_gtk_tone_class(
                            clawt_json_boolean(bot, "mcp_key", FALSE)
                                ? "good" : "neutral"),
                        "How the bot can report back"));

    gtk_widget_set_valign(prompt, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(prompt, "flat");
    g_object_set_data_full(G_OBJECT(prompt), "bot", g_strdup(name), g_free);
    g_signal_connect(prompt, "clicked", G_CALLBACK(on_routine_prompt), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), prompt);

    gtk_widget_set_valign(mint, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(mint, "flat");
    g_object_set_data_full(G_OBJECT(mint), "bot", g_strdup(name), g_free);
    g_signal_connect(mint, "clicked", G_CALLBACK(on_mint_key), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), mint);

    return row;
}

static GtkWidget *
job_row(JsonObject *job, gint64 now)
{
    GtkWidget *row = adw_expander_row_new();
    gint state = CLAWT_GROK_JOB_FAILED;
    g_autofree gchar *title = NULL;
    g_autofree gchar *subtitle = NULL;
    g_autofree gchar *age = clawt_time_ago_label(
        clawt_json_int(job, "created_at", 0), now);
    const gchar *result = clawt_json_string(job, "result", NULL);
    const gchar *note = clawt_json_string(job, "note", NULL);
    const gchar *body = result != NULL && *result != '\0' ? result : note;

    /* The nick resolved to the enum, never compared as a spelling here. */
    clawt_enum_from_nick(CLAWT_TYPE_GROK_JOB_STATE,
                         clawt_json_string(job, "state", "failed"), &state);

    title = g_strdup_printf("%s \xc2\xb7 %s", clawt_json_string(job, "bot", "?"),
                            clawt_json_string(job, "instructions", ""));
    subtitle = g_strdup_printf("%s for %s, %s%s%s",
                               clawt_json_string(job, "id", "?"),
                               clawt_json_string(job, "agent", "?"), age,
                               clawt_json_string(job, "run_uuid", NULL) != NULL
                                   ? " \xc2\xb7 Grok run " : "",
                               clawt_json_string(job, "run_uuid", ""));

    /* Plain text: a title is somebody's request, not markup. */
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), subtitle);
    adw_expander_row_set_title_lines(ADW_EXPANDER_ROW(row), 1);

    adw_expander_row_add_prefix(ADW_EXPANDER_ROW(row),
        clawt_gtk_badge(clawt_grok_job_state_label((ClawtGrokJobState)state),
                        clawt_gtk_tone_class(clawt_grok_job_state_tone(
                            (ClawtGrokJobState)state)),
                        NULL));

    if (body != NULL && *body != '\0') {
        GtkWidget *label = gtk_label_new(body);

        /*
         * The bot's own text, drawn as text: it was written outside the
         * fleet, and a label that parsed it as markup would let it draw
         * whatever it liked here.
         */
        gtk_label_set_use_markup(GTK_LABEL(label), FALSE);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_top(label, 8);
        gtk_widget_set_margin_bottom(label, 8);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), label);
    } else {
        adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(row), FALSE);
    }

    return row;
}

/* ── Refreshing ──────────────────────────────────────────────────── */

static void
fill_combo(GtkWidget *combo, GPtrArray *ids, GPtrArray *labels)
{
    g_autoptr(GtkStringList) model = gtk_string_list_new(NULL);
    const gchar *previous = NULL;
    GStrv old = g_object_get_data(G_OBJECT(combo), "ids");
    guint old_selected = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));
    guint i;
    guint keep = 0;

    /* Keep the choice somebody made across a refresh, by id. */
    if (old != NULL && old_selected != GTK_INVALID_LIST_POSITION &&
        old_selected < g_strv_length(old))
        previous = old[old_selected];

    for (i = 0; i < labels->len; i++) {
        gtk_string_list_append(model, g_ptr_array_index(labels, i));

        if (previous != NULL &&
            g_strcmp0(previous, g_ptr_array_index(ids, i)) == 0)
            keep = i;
    }

    adw_combo_row_set_model(ADW_COMBO_ROW(combo), G_LIST_MODEL(model));

    if (labels->len > 0)
        adw_combo_row_set_selected(ADW_COMBO_ROW(combo), keep);

    g_ptr_array_add(ids, NULL);
    g_object_set_data_full(G_OBJECT(combo), "ids",
                           g_strdupv((GStrv)ids->pdata),
                           (GDestroyNotify)g_strfreev);
    g_ptr_array_remove_index(ids, ids->len - 1);
}

static void
refresh_grok_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) bots_reply = NULL;
    g_autoptr(JsonNode) jobs_reply = NULL;
    g_autoptr(GPtrArray) bot_ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) bot_labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) agent_ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) agent_labels =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen_agents =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gint64 now = g_get_real_time();
    JsonArray *bots;
    JsonArray *jobs;
    JsonObject *root;
    guint i;

    clawt_gtk_clear_list(self->grok_bot_list);
    clawt_gtk_clear_list(self->grok_job_list);

    bots_reply = clawt_window_request(self, "grok.bots", NULL);

    if (bots_reply == NULL)
        return;

    root = clawt_payload_of(bots_reply);

    if (clawt_json_boolean(root, "reachable", FALSE)) {
        g_autofree gchar *where = g_strdup_printf(
            "Bots report back to %s over the tailnet.",
            clawt_json_string(root, "callback_host", "?"));

        gtk_label_set_text(GTK_LABEL(self->grok_status), where);
        gtk_widget_remove_css_class(self->grok_status, "error");
    } else {
        g_autofree gchar *why = g_strdup_printf(
            "No job can be sent right now: %s.",
            clawt_json_string(root, "unreachable_reason", "unknown"));

        gtk_label_set_text(GTK_LABEL(self->grok_status), why);
        gtk_widget_add_css_class(self->grok_status, "error");
    }

    bots = json_object_get_array_member(root, "bots");

    for (i = 0; bots != NULL && i < json_array_get_length(bots); i++) {
        JsonObject *bot = json_array_get_object_element(bots, i);
        JsonArray *agents = json_object_get_array_member(bot, "agents");
        guint a;

        gtk_list_box_append(self->grok_bot_list, bot_row(self, bot));
        g_ptr_array_add(bot_ids,
                        g_strdup(clawt_json_string(bot, "name", "?")));
        g_ptr_array_add(bot_labels,
                        g_strdup(clawt_json_string(bot, "name", "?")));

        for (a = 0; agents != NULL && a < json_array_get_length(agents);
             a++) {
            const gchar *id = json_array_get_string_element(agents, a);

            if (g_hash_table_contains(seen_agents, id))
                continue;

            g_hash_table_add(seen_agents, g_strdup(id));
            g_ptr_array_add(agent_ids, g_strdup(id));
            g_ptr_array_add(agent_labels, g_strdup(id));
        }
    }

    if (bot_ids->len == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "No Grok bot is shared with any agent",
            "Add a grokbot integration in Settings, named for the bot, with "
            "its routine's webhook address and key.");
        gtk_list_box_append(self->grok_bot_list, row);
    }

    fill_combo(self->grok_bot_row, bot_ids, bot_labels);
    fill_combo(self->grok_agent_row, agent_ids, agent_labels);
    gtk_widget_set_sensitive(self->grok_send, bot_ids->len > 0);

    jobs_reply = clawt_window_request(self, "grok.jobs",
                                      NULL);

    if (jobs_reply == NULL)
        return;

    jobs = json_object_get_array_member(clawt_payload_of(jobs_reply), "jobs");

    for (i = 0; jobs != NULL && i < json_array_get_length(jobs); i++)
        gtk_list_box_append(self->grok_job_list,
                            job_row(json_array_get_object_element(jobs, i),
                                    now));

    if (jobs == NULL || json_array_get_length(jobs) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "No work has been handed to a Grok bot",
                               "Agents send it with clawtilla_grok_dispatch; "
                               "you can send it from here.");
        gtk_list_box_append(self->grok_job_list, row);
    }

    {
        guint attention = (guint)clawt_json_int(
            clawt_payload_of(jobs_reply), "attention", 0);

        clawt_gtk_set_page_badge(self, CLAWT_PAGE_GROK, attention,
                                 attention > 0);
    }
}

void
clawt_gtk_refresh_grok(ClawtWindow *self)
{
    if (self->grok_job_list == NULL ||
        !clawt_gtk_refresh_enter(self, CLAWT_REFRESH_GROK))
        return;

    do {
        refresh_grok_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_GROK));
}

void
clawt_gtk_refresh_grok_badge(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    guint attention;

    /*
     * The page refreshes itself, badge included, when it is the one
     * showing; anywhere else only the number is fetched, so a job settling
     * while somebody reads the chat is still seen.
     */
    if (clawt_gtk_current_page(self) == CLAWT_PAGE_GROK) {
        clawt_gtk_refresh_grok(self);
        return;
    }

    reply = clawt_window_request(self, "grok.jobs",
                                 NULL);

    if (reply == NULL)
        return;

    attention = (guint)clawt_json_int(clawt_payload_of(reply), "attention",
                                      0);
    clawt_gtk_set_page_badge(self, CLAWT_PAGE_GROK, attention,
                             attention > 0);
}

/* ── Building ────────────────────────────────────────────────────── */

static GtkWidget *
boxed_list(GtkListBox **out)
{
    GtkWidget *list = gtk_list_box_new();

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    *out = GTK_LIST_BOX(list);

    return list;
}

static GtkWidget *
heading(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 6);

    return label;
}

GtkWidget *
clawt_gtk_build_grok_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *composer = gtk_list_box_new();
    GtkWidget *instructions_frame = gtk_frame_new(NULL);
    GtkWidget *instructions = gtk_text_view_new();
    GtkWidget *bot_list;
    GtkWidget *job_list;

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    self->grok_status = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(self->grok_status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->grok_status), 0.0f);
    gtk_box_append(GTK_BOX(box), self->grok_status);

    /* The bots. */
    gtk_box_append(GTK_BOX(box), heading("Bots"));
    bot_list = boxed_list(&self->grok_bot_list);
    gtk_box_append(GTK_BOX(box), bot_list);

    /* The composer. */
    gtk_box_append(GTK_BOX(box), heading("Hand a bot some work"));
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(composer),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(composer, "boxed-list");

    self->grok_bot_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->grok_bot_row),
                                  "Bot");
    gtk_list_box_append(GTK_LIST_BOX(composer), self->grok_bot_row);

    self->grok_agent_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->grok_agent_row),
                                  "On behalf of");
    clawt_gtk_set_row_hint(self->grok_agent_row,
                           "The agent the answer is delivered to. Only "
                           "agents the bot is shared with are listed.");
    gtk_list_box_append(GTK_LIST_BOX(composer), self->grok_agent_row);

    self->grok_context_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->grok_context_row),
                                  "Context (optional)");
    gtk_list_box_append(GTK_LIST_BOX(composer), self->grok_context_row);
    gtk_box_append(GTK_BOX(box), composer);

    self->grok_instructions = GTK_TEXT_VIEW(instructions);
    gtk_text_view_set_wrap_mode(self->grok_instructions, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(self->grok_instructions, 8);
    gtk_text_view_set_bottom_margin(self->grok_instructions, 8);
    gtk_text_view_set_left_margin(self->grok_instructions, 8);
    gtk_text_view_set_right_margin(self->grok_instructions, 8);
    gtk_widget_set_size_request(instructions, -1, 96);
    gtk_frame_set_child(GTK_FRAME(instructions_frame), instructions);
    gtk_box_append(GTK_BOX(box), instructions_frame);

    self->grok_send = gtk_button_new_with_label("Send to the bot");
    gtk_widget_add_css_class(self->grok_send, "suggested-action");
    gtk_widget_set_halign(self->grok_send, GTK_ALIGN_START);
    g_signal_connect(self->grok_send, "clicked", G_CALLBACK(on_send), self);
    gtk_box_append(GTK_BOX(box), self->grok_send);

    /* The jobs. */
    gtk_box_append(GTK_BOX(box), heading("Jobs"));
    job_list = boxed_list(&self->grok_job_list);
    gtk_box_append(GTK_BOX(box), job_list);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    /* A wrapping label's natural width is unwrapped: never scroll sideways. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    return scroll;
}
