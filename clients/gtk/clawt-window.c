/*
 * clawt-window.c - The clawtilla window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Agents are presented as chats, because that is what they are: a sidebar
 * of who is here, a transcript in the middle, and everything else about
 * an agent behind a tab rather than a settings dialog somewhere else.
 */

#include "clawt-gtk.h"

#include <string.h>

/*
 * The provider and model rows, and the free-text row that appears when a
 * model is not in the catalogue.
 *
 * Shared by the create dialog and the agent inspector: the rules for
 * which models a provider runs belong in one place, or the two views
 * drift and one of them starts offering combinations that cannot work.
 */
typedef struct {
    ClawtWindow *window;
    GtkWidget   *provider_row;
    GtkWidget   *model_row;
    GtkWidget   *model_entry;
    JsonNode    *catalog;
} ModelChooser;

/* Defined below; the inspector and the create dialog both use them. */
static JsonObject  *chooser_provider(ModelChooser *chooser);
static const gchar *chooser_provider_id(ModelChooser *chooser);
static gchar       *chooser_model(ModelChooser *chooser);
static void         model_chooser_build(ModelChooser *chooser,
                                        ClawtWindow  *window,
                                        GtkWidget    *group,
                                        const gchar  *want_provider,
                                        const gchar  *want_model);

struct _ClawtWindow {
    AdwApplicationWindow parent_instance;

    ClawtClient       *client;

    AdwToastOverlay   *toasts;
    AdwOverlaySplitView *split;
    GtkListBox        *sidebar;
    AdwViewStack      *pages;

    /* Chat */
    GtkBox            *transcript;
    GtkScrolledWindow *transcript_scroll;
    GtkEntry          *entry;
    GtkLabel          *streaming;

    /* Inspector */
    GtkBox            *inspector;
    GtkWidget         *name_row;
    GtkWidget         *description_row;
    GtkWidget         *effort_row;
    GtkWidget         *computer_row;
    GtkWidget         *restart_row;
    GtkWidget         *autostart_row;
    GtkWidget         *chief_row;
    ModelChooser       inspector_models;

    /* Mailbox */
    GtkListBox        *mailbox_list;
    GtkLabel          *mailbox_summary;

    /* Computer */
    GtkEntry          *exec_entry;
    GtkTextView       *exec_output;
    GtkLabel          *computer_state;

    /* Tasks */
    GtkListBox        *task_list;

    gchar             *selected_agent;
    gboolean           following;
};

G_DEFINE_FINAL_TYPE(ClawtWindow, clawt_window, ADW_TYPE_APPLICATION_WINDOW)

static void refresh_agents(ClawtWindow *self);
static void refresh_selected(ClawtWindow *self);
static void select_agent(ClawtWindow *self, const gchar *agent_id);

/* ── Talking to the daemon ───────────────────────────────────────── */

void
clawt_window_toast(ClawtWindow *self, const gchar *text)
{
    g_return_if_fail(CLAWT_IS_WINDOW(self));

    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(text));
}

JsonNode *
clawt_window_request(ClawtWindow *self, const gchar *kind, JsonNode *payload)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    g_return_val_if_fail(CLAWT_IS_WINDOW(self), NULL);

    reply = clawt_client_request(self->client, kind, payload, &error);

    if (reply == NULL) {
        /*
         * Every failure is surfaced.  A request that quietly did nothing
         * is the worst outcome here: the interface would look like it had
         * worked and the fleet would disagree.
         */
        clawt_window_toast(self, error->message);
        return NULL;
    }

    return reply;
}

/* ── Sidebar ─────────────────────────────────────────────────────── */

/*
 * A coloured dot for the agent's state.
 *
 * Colour alone is never the only signal -- the state is also in the
 * tooltip and in the inspector -- because a dot is exactly the kind of
 * cue that disappears for a colourblind reader.
 */
static GtkWidget *
state_dot(const gchar *state)
{
    GtkWidget *dot = gtk_image_new();
    const gchar *icon = "media-record-symbolic";
    const gchar *css = NULL;

    if (g_strcmp0(state, "running") == 0)
        css = "success";
    else if (g_strcmp0(state, "error") == 0 || g_strcmp0(state, "shadow") == 0)
        css = "error";
    else if (g_strcmp0(state, "degraded") == 0 ||
             g_strcmp0(state, "starting") == 0 ||
             g_strcmp0(state, "stopping") == 0)
        css = "warning";
    else
        css = "dim-label";

    gtk_image_set_from_icon_name(GTK_IMAGE(dot), icon);
    gtk_widget_add_css_class(dot, css);
    gtk_widget_set_tooltip_text(dot, state);

    return dot;
}

/*
 * Sets a row's title and subtitle as plain text.
 *
 * AdwPreferencesRow:use-markup defaults to TRUE, so a row's title and
 * subtitle are parsed as Pango markup unless you say otherwise.  Every
 * string here came from a person or a model -- an agent's description, a
 * message body, a task prompt -- and two things follow from that.  Text
 * containing a bare "<" or "&" silently fails to render at all, which is
 * ordinary English rather than an edge case; and text containing real
 * markup renders as live styled content, including clickable links.
 * Neither is acceptable for something an agent wrote.
 */
static void
set_row_text(GtkWidget *row, const gchar *title, const gchar *subtitle)
{
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  title != NULL ? title : "");

    if (subtitle != NULL)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
}

static GtkWidget *
badge(const gchar *text, const gchar *css_class, const gchar *tooltip)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, css_class);
    gtk_widget_set_tooltip_text(label, tooltip);

    return label;
}

static GtkWidget *
agent_row(JsonObject *agent)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gint64 depth = json_object_has_member(agent, "mailbox_depth")
                   ? json_object_get_int_member(agent, "mailbox_depth") : 0;

    set_row_text(row,
                 clawt_json_string(agent, "name",
                                   clawt_json_string(agent, "id", "?")),
                 clawt_json_string(agent, "description", ""));

    adw_action_row_add_prefix(ADW_ACTION_ROW(row), state_dot(state));

    /*
     * A queue badge, because a stopped agent with mail waiting looks
     * identical to an idle one otherwise -- and the difference is the
     * whole point of having durable mailboxes.
     */
    if (depth > 0) {
        g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, depth);

        gtk_box_append(GTK_BOX(box),
                       badge(text, "accent", "messages waiting"));
    }

    /*
     * A HOST badge is not decoration: an agent that can run commands on
     * the real machine should be visibly different from one that cannot,
     * at a glance, in the list.
     */
    if (strstr(caps, "host-control") != NULL)
        gtk_box_append(GTK_BOX(box),
                       badge("HOST", "error",
                             "this agent can run commands on this machine"));

    if (json_object_has_member(agent, "chief_of_staff") &&
        json_object_get_boolean_member(agent, "chief_of_staff"))
        gtk_box_append(GTK_BOX(box),
                       badge("CHIEF", "accent",
                             "hands work to the other agents"));

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);

    g_object_set_data_full(G_OBJECT(row), "agent-id",
                           g_strdup(clawt_json_string(agent, "id", "")),
                           g_free);

    return row;
}

static void
on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id;

    (void)box;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    if (agent_id != NULL)
        select_agent(self, agent_id);
}

static void
clear_list(GtkListBox *list)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL)
        gtk_list_box_remove(list, child);
}

static void
clear_box(GtkBox *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, child);
}

static void
refresh_agents(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    guint i;

    reply = clawt_window_request(self, "agent.list", NULL);

    if (reply == NULL)
        return;

    clear_list(self->sidebar);

    agents = json_object_get_array_member(clawt_payload_of(reply), "agents");

    if (json_array_get_length(agents) == 0) {
        GtkWidget *row = adw_action_row_new();

        set_row_text(row, "No agents yet", "Use the + button to add one");
        gtk_list_box_append(self->sidebar, row);
        return;
    }

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        GtkWidget *row = agent_row(agent);

        gtk_list_box_append(self->sidebar, row);

        /* Keep the current selection across a refresh. */
        if (g_strcmp0(clawt_json_string(agent, "id", ""),
                      self->selected_agent) == 0)
            gtk_list_box_select_row(self->sidebar, GTK_LIST_BOX_ROW(row));
    }

    if (self->selected_agent == NULL) {
        JsonObject *first = json_array_get_object_element(agents, 0);

        select_agent(self, clawt_json_string(first, "id", NULL));
    }
}

/* ── Chat ────────────────────────────────────────────────────────── */

static void
append_message(ClawtWindow *self, const gchar *sender, const gchar *body,
               gboolean from_user)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *who = gtk_label_new(sender);
    GtkWidget *text = gtk_label_new(body);

    gtk_widget_add_css_class(who, "caption");
    gtk_widget_add_css_class(who, "dim-label");
    gtk_widget_set_halign(who, from_user ? GTK_ALIGN_END : GTK_ALIGN_START);

    /*
     * The body is a label, not markup.  Model output arrives from
     * somewhere the user does not control, and treating it as Pango markup
     * would let a message rewrite the interface around it.
     */
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_widget_set_halign(text, from_user ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_widget_add_css_class(text, from_user ? "accent" : "body");

    gtk_box_append(GTK_BOX(row), who);
    gtk_box_append(GTK_BOX(row), text);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_top(row, 6);

    gtk_box_append(self->transcript, row);
}

/*
 * Schedules a scroll that cannot outlive the window.
 *
 * A plain g_idle_add(self) runs after the window has been destroyed if
 * the user closes it in the same turn a message arrives, and the
 * callback then reads freed memory.  Holding a reference for the life of
 * the idle costs nothing and removes the race.
 */
static gboolean scroll_to_bottom(gpointer user_data);

static void
queue_scroll(ClawtWindow *self)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_to_bottom,
                    g_object_ref(self), g_object_unref);
}

/*
 * Scrolls to the bottom, but only when the reader was already there.
 *
 * Yanking somebody down mid-read because a message arrived is the single
 * most annoying thing a chat window can do.
 */
static gboolean
scroll_to_bottom(gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkAdjustment *adjustment;

    if (!self->following || self->transcript_scroll == NULL)
        return G_SOURCE_REMOVE;

    adjustment = gtk_scrolled_window_get_vadjustment(self->transcript_scroll);
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                             gtk_adjustment_get_page_size(adjustment));

    return G_SOURCE_REMOVE;
}

static void
on_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    ClawtWindow *self = user_data;
    gdouble value = gtk_adjustment_get_value(adjustment);
    gdouble bottom = gtk_adjustment_get_upper(adjustment) -
                     gtk_adjustment_get_page_size(adjustment);

    /* A small tolerance, or a pixel of rounding stops the follow. */
    self->following = (bottom - value) < 32.0;
}

static void
load_history(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    clear_box(self->transcript);

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", self->selected_agent, "as", "user",
                            NULL));

    if (reply == NULL)
        return;

    messages = json_object_get_array_member(clawt_payload_of(reply),
                                            "messages");

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_json_string(message, "sender", "?");

        append_message(self, sender, clawt_json_string(message, "body", ""),
                       g_strcmp0(sender, "user") == 0);
    }

    self->following = TRUE;
    queue_scroll(self);
}

static void
on_send(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *body;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    body = gtk_editable_get_text(GTK_EDITABLE(self->entry));

    if (body == NULL || *body == '\0')
        return;

    reply = clawt_window_request(
        self, "msg.send",
        clawt_build_payload("target", self->selected_agent, "body", body,
                            "from", "user", NULL));

    if (reply == NULL)
        return;

    append_message(self, "you", body, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(self->entry), "");

    self->following = TRUE;
    queue_scroll(self);
}

/* ── Inspector ───────────────────────────────────────────────────── */

static GtkWidget *
info_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_action_row_new();

    set_row_text(row, title, value != NULL ? value : "-");

    return row;
}

static void
on_agent_action(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *kind = g_object_get_data(G_OBJECT(button), "kind");
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, kind, clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply != NULL)
        refresh_agents(self);

    refresh_selected(self);
}

static GtkWidget *
action_button(ClawtWindow *self, const gchar *label, const gchar *kind,
              gboolean sensitive, const gchar *why_not)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "kind", g_strdup(kind), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_agent_action), self);

    gtk_widget_set_sensitive(button, sensitive);

    /*
     * A disabled control says why.  A greyed-out button with no
     * explanation is the interface equivalent of a shrug.
     */
    if (!sensitive && why_not != NULL)
        gtk_widget_set_tooltip_text(button, why_not);

    return button;
}

/*
 * Sends one setting, and reports it if the daemon refuses.
 *
 * Returns: %TRUE if it was accepted
 */
static gboolean
apply_setting(ClawtWindow *self, const gchar *key, const gchar *value)
{
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, "agent.set",
        clawt_build_payload("agent", self->selected_agent, "key", key,
                            "value", value, NULL));

    return reply != NULL;
}

static void
on_save_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *model = NULL;
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm" };
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max" };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always" };
    gboolean ok = TRUE;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    ok &= apply_setting(self, "name",
                        gtk_editable_get_text(GTK_EDITABLE(self->name_row)));
    ok &= apply_setting(self, "description",
                        gtk_editable_get_text(
                            GTK_EDITABLE(self->description_row)));

    if (chooser_provider_id(&self->inspector_models) != NULL)
        ok &= apply_setting(self, "model.provider",
                            chooser_provider_id(&self->inspector_models));

    model = chooser_model(&self->inspector_models);

    if (model != NULL)
        ok &= apply_setting(self, "model.model", model);

    ok &= apply_setting(self, "model.effort",
                        efforts[MIN(adw_combo_row_get_selected(
                                        ADW_COMBO_ROW(self->effort_row)), 4)]);

    ok &= apply_setting(self, "computer.type",
                        computers[MIN(adw_combo_row_get_selected(
                                          ADW_COMBO_ROW(self->computer_row)),
                                      3)]);

    ok &= apply_setting(self, "runtime.restart",
                        restarts[MIN(adw_combo_row_get_selected(
                                         ADW_COMBO_ROW(self->restart_row)),
                                     2)]);

    ok &= apply_setting(self, "runtime.autostart",
                        adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->autostart_row))
                            ? "true" : "false");

    ok &= apply_setting(self, "chief_of_staff",
                        adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->chief_row))
                            ? "true" : "false");

    if (!ok)
        return;

    /*
     * Said plainly, because most of these only take effect on the next
     * start -- the model and the computer especially.  A reload does not
     * restart running agents on purpose, and an interface that implied
     * otherwise would have people wondering why nothing changed.
     */
    clawt_window_toast(self,
                       "Saved. Restart the agent for the model or computer "
                       "to take effect.");
    refresh_agents(self);
}

/* ── Deleting ────────────────────────────────────────────────────── */

static void
on_delete_confirmed_twice(AdwAlertDialog *dialog, gchar *response,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *agent_id = NULL;

    (void)dialog;

    if (g_strcmp0(response, "delete") != 0)
        return;

    agent_id = g_strdup(self->selected_agent);

    reply = clawt_window_request(
        self, "agent.remove", clawt_build_payload("agent", agent_id, NULL));

    if (reply == NULL)
        return;

    g_clear_pointer(&self->selected_agent, g_free);
    clear_box(self->inspector);
    clear_box(self->transcript);

    {
        g_autofree gchar *message = g_strdup_printf("%s is gone.", agent_id);

        clawt_window_toast(self, message);
    }

    refresh_agents(self);
}

static void
on_delete_confirmed_once(AdwAlertDialog *dialog, gchar *response,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *second;

    (void)dialog;

    if (g_strcmp0(response, "delete") != 0)
        return;

    /*
     * A second, deliberately louder confirmation.  Removing an agent
     * takes its running process and its place in every room with it, and
     * a single click between "browsing" and "gone" is too few.
     */
    second = ADW_ALERT_DIALOG(
        adw_alert_dialog_new("ARE YOU REALLY SURE!?", NULL));

    adw_alert_dialog_set_body(
        second,
        "This removes the agent from the fleet and stops it if it is "
        "running.\n\n"
        "Its mailbox and transcripts stay on disk -- removing an agent is "
        "reversible, deleting its history is not.");

    adw_alert_dialog_add_response(second, "cancel", "Keep it");
    adw_alert_dialog_add_response(second, "delete", "Delete it for good");
    adw_alert_dialog_set_response_appearance(second, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(second, "cancel");
    adw_alert_dialog_set_close_response(second, "cancel");

    g_signal_connect(second, "response",
                     G_CALLBACK(on_delete_confirmed_twice), self);

    adw_dialog_present(ADW_DIALOG(second), GTK_WIDGET(self));
}

static void
on_delete_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *first;
    g_autofree gchar *heading = NULL;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    heading = g_strdup_printf("Delete %s?", self->selected_agent);

    first = ADW_ALERT_DIALOG(adw_alert_dialog_new("Are you sure?", NULL));

    adw_alert_dialog_set_body(first, heading);
    adw_alert_dialog_add_response(first, "cancel", "Cancel");
    adw_alert_dialog_add_response(first, "delete", "Delete");
    adw_alert_dialog_set_response_appearance(first, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(first, "cancel");
    adw_alert_dialog_set_close_response(first, "cancel");

    g_signal_connect(first, "response",
                     G_CALLBACK(on_delete_confirmed_once), self);

    adw_dialog_present(ADW_DIALOG(first), GTK_WIDGET(self));
}

/* ── The inspector ───────────────────────────────────────────────── */

static guint
index_of(const gchar *const *values, const gchar *wanted)
{
    guint i;

    for (i = 0; values[i] != NULL; i++) {
        if (g_strcmp0(values[i], wanted) == 0)
            return i;
    }

    return 0;
}

static GtkWidget *
combo_row(const gchar *title, const gchar *const *values,
          const gchar *selected)
{
    GtkWidget *row = adw_combo_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row),
                            G_LIST_MODEL(gtk_string_list_new(values)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                               index_of(values, selected));

    return row;
}

static GtkWidget *
entry_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_entry_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), value != NULL ? value : "");

    return row;
}

static GtkWidget *
switch_row(const gchar *title, const gchar *subtitle, gboolean active)
{
    GtkWidget *row = adw_switch_row_new();

    set_row_text(row, title, subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);

    return row;
}

static void
build_inspector(ClawtWindow *self, JsonObject *agent, JsonObject *payload)
{
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm", NULL };
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max", NULL };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always", NULL };
    GtkWidget *group;
    GtkWidget *actions;
    GtkWidget *save;
    GtkWidget *danger;
    GtkWidget *delete_button;
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean is_shadow = g_strcmp0(state, "shadow") == 0;

    clear_box(self->inspector);
    g_clear_pointer(&self->inspector_models.catalog, json_node_unref);

    /* ── What it is, and what it is doing ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Agent");

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Id", clawt_json_string(agent, "id",
                                                               "?")));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("State", state));

    if (clawt_json_string(agent, "detail", NULL) != NULL)
        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(group),
            info_row("Why", clawt_json_string(agent, "detail", NULL)));

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Can do", caps));

    gtk_box_append(self->inspector, group);

    /* ── Editable ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Settings");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The model and the computer take effect when the agent next "
        "starts.");

    self->name_row = entry_row("Name", clawt_json_string(agent, "name", ""));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->name_row);

    self->description_row = entry_row(
        "Description", clawt_json_string(agent, "description", ""));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->description_row), "");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->description_row);

    model_chooser_build(&self->inspector_models, self, group,
                        clawt_json_string(agent, "provider", NULL),
                        clawt_json_string(agent, "model", NULL));

    self->effort_row = combo_row("Effort", efforts,
                                 clawt_json_string(agent, "effort",
                                                   "medium"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->effort_row);

    self->computer_row = combo_row("Computer", computers,
                                   clawt_json_string(agent, "computer",
                                                     "none"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->computer_row);

    self->restart_row = combo_row("Restart", restarts,
                                  clawt_json_string(agent, "restart",
                                                    "on-failure"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->restart_row);

    self->autostart_row = switch_row(
        "Start with the daemon", NULL,
        json_object_has_member(agent, "autostart")
            ? json_object_get_boolean_member(agent, "autostart") : TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->autostart_row);

    self->chief_row = switch_row(
        "Chief of staff", "Hands work to the other agents",
        json_object_has_member(agent, "chief_of_staff") &&
        json_object_get_boolean_member(agent, "chief_of_staff"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->chief_row);

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_agent), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    gtk_box_append(self->inspector, group);

    /* Credentials: named, never valued. */
    if (json_object_has_member(agent, "credentials")) {
        JsonObject *credentials = json_object_get_object_member(agent,
                                                                "credentials");
        g_autoptr(GList) names = json_object_get_members(credentials);
        GtkWidget *creds = adw_preferences_group_new();
        GList *l;

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(creds),
                                        "Credentials");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(creds),
            "Shown by reference. clawtilla never sends the values to a "
            "client, so nothing secret can end up in a screenshot.");

        for (l = names; l != NULL; l = l->next)
            adw_preferences_group_add(
                ADW_PREFERENCES_GROUP(creds),
                info_row(l->data,
                         json_object_get_string_member(credentials,
                                                       l->data)));

        if (names != NULL)
            gtk_box_append(self->inspector, creds);
    }

    if (clawt_json_string(payload, "computer_detail", NULL) != NULL) {
        GtkWidget *detail = adw_preferences_group_new();
        GtkWidget *label = gtk_label_new(
            clawt_json_string(payload, "computer_detail", ""));

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(detail),
                                        "Computer");
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_top(label, 6);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(detail), label);

        gtk_box_append(self->inspector, detail);
    }

    /* ── Running it ── */
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(GTK_WIDGET(actions), 12);
    gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);

    /*
     * Every control's sensitivity comes from what the agent can actually
     * do, so nothing here offers something that would fail.
     */
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Start", "agent.start",
                                 !is_shadow &&
                                 g_strcmp0(state, "running") != 0,
                                 is_shadow
                                     ? "this agent's configuration could "
                                       "not be understood"
                                     : "already running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Stop", "agent.stop",
                                 g_strcmp0(state, "stopped") != 0 &&
                                 !is_shadow, "not running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Restart", "agent.restart",
                                 !is_shadow,
                                 "this agent cannot start"));

    gtk_box_append(self->inspector, actions);

    /* ── Removing it ── */
    danger = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(danger),
                                    "Danger zone");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(danger),
        "Removing an agent stops it and takes it out of every room. Its "
        "mailbox and transcripts stay on disk.");

    delete_button = gtk_button_new_with_label("Delete this agent");
    gtk_widget_add_css_class(delete_button, "destructive-action");
    gtk_widget_set_margin_top(delete_button, 6);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_agent),
                     self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), delete_button);

    gtk_box_append(self->inspector, danger);
}

/* ── Mailbox ─────────────────────────────────────────────────────── */

static void
on_mailbox_ack(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *item_id = g_object_get_data(G_OBJECT(button), "item-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, "mailbox.ack",
        clawt_build_payload("agent", self->selected_agent, "item", item_id,
                            NULL));

    if (reply != NULL)
        refresh_selected(self);
}

static void
refresh_mailbox(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *items;
    guint i;

    clear_list(self->mailbox_list);

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "mailbox.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    items = json_object_get_array_member(clawt_payload_of(reply), "items");

    {
        gint64 depth = json_object_get_int_member(clawt_payload_of(reply),
                                                  "depth");
        g_autofree gchar *summary = g_strdup_printf(
            "%" G_GINT64_FORMAT " message(s) waiting", depth);

        gtk_label_set_text(self->mailbox_summary, summary);
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        GtkWidget *row = adw_action_row_new();
        GtkWidget *ack = gtk_button_new_with_label("Ack");
        g_autofree gchar *title = NULL;

        title = g_strdup_printf("from %s",
                                clawt_json_string(item, "from", "?"));

        set_row_text(row, title, clawt_json_string(item, "body", ""));

        if (clawt_json_string(item, "last_error", NULL) != NULL) {
            GtkWidget *warn = badge("failed", "error",
                                    clawt_json_string(item, "last_error",
                                                      ""));

            adw_action_row_add_prefix(ADW_ACTION_ROW(row), warn);
        }

        g_object_set_data_full(G_OBJECT(ack), "item-id",
                               g_strdup(clawt_json_string(item, "id", "")),
                               g_free);
        g_signal_connect(ack, "clicked", G_CALLBACK(on_mailbox_ack), self);
        gtk_widget_set_valign(ack, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), ack);

        gtk_list_box_append(self->mailbox_list, row);
    }
}

/* ── Computer ────────────────────────────────────────────────────── */

static void
on_exec(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    GtkTextBuffer *buffer;
    GtkTextIter end;
    const gchar *command;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    command = gtk_editable_get_text(GTK_EDITABLE(self->exec_entry));

    if (command == NULL || *command == '\0')
        return;

    buffer = gtk_text_view_get_buffer(self->exec_output);
    gtk_text_buffer_get_end_iter(buffer, &end);

    {
        g_autofree gchar *echo = g_strdup_printf("$ %s\n", command);

        gtk_text_buffer_insert(buffer, &end, echo, -1);
    }

    reply = clawt_window_request(
        self, "computer.exec",
        clawt_build_payload("agent", self->selected_agent, "command",
                            command, NULL));

    if (reply == NULL)
        return;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stdout", ""), -1);

    /*
     * stderr is shown too.  A console that swallowed it would leave a
     * failing command looking like one that produced nothing.
     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stderr", ""), -1);

    gtk_editable_set_text(GTK_EDITABLE(self->exec_entry), "");
}

static void
refresh_computer(ClawtWindow *self, JsonObject *agent)
{
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean has_computer = strstr(caps, "computer") != NULL;

    gtk_widget_set_sensitive(GTK_WIDGET(self->exec_entry), has_computer);

    gtk_label_set_text(self->computer_state,
                       has_computer
                           ? clawt_json_string(agent, "computer", "none")
                           : "This agent has no computer.");
}

/* ── Tasks ───────────────────────────────────────────────────────── */

static void
refresh_tasks(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *tasks;
    guint i;

    clear_list(self->task_list);

    reply = clawt_window_request(self, "task.list", NULL);

    if (reply == NULL)
        return;

    tasks = json_object_get_array_member(clawt_payload_of(reply), "tasks");

    for (i = 0; i < json_array_get_length(tasks); i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;

        title = g_strdup_printf("%s → %s",
                                clawt_json_string(task, "origin", "?"),
                                clawt_json_string(task, "assignee", "?"));

        set_row_text(row, title, clawt_json_string(task, "prompt", ""));
        adw_action_row_add_suffix(
            ADW_ACTION_ROW(row),
            badge(clawt_json_string(task, "state", "?"), "dim-label",
                  clawt_json_string(task, "reason", "")));

        gtk_list_box_append(self->task_list, row);
    }
}

/* ── Selection ───────────────────────────────────────────────────── */

static void
refresh_selected(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *agent;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.show",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    agent = json_object_get_object_member(clawt_payload_of(reply), "agent");

    build_inspector(self, agent, clawt_payload_of(reply));
    refresh_computer(self, agent);
    refresh_mailbox(self);
    refresh_tasks(self);
}

static void
select_agent(ClawtWindow *self, const gchar *agent_id)
{
    if (agent_id == NULL)
        return;

    g_free(self->selected_agent);
    self->selected_agent = g_strdup(agent_id);

    adw_window_title_set_title(
        ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(self), "title")),
        agent_id);

    load_history(self);
    refresh_selected(self);

    /* On a narrow window the sidebar is a drawer, so close it. */
    if (adw_overlay_split_view_get_collapsed(self->split))
        adw_overlay_split_view_set_show_sidebar(self->split, FALSE);
}

/* ── Events ──────────────────────────────────────────────────────── */

static void
on_daemon_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *kind = clawt_event_get_kind(event);

    (void)client;

    if (g_strcmp0(kind, "message") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *body = clawt_event_get_detail(event, "body");

        /*
         * Only messages in the conversation on screen are appended; the
         * rest change the sidebar's queue badge, which is what tells you
         * something happened elsewhere.
         */
        if (g_strcmp0(clawt_event_get_subject(event),
                      self->selected_agent) == 0 ||
            g_strcmp0(from, self->selected_agent) == 0) {
            append_message(self, from != NULL ? from : "?",
                           body != NULL ? body : "", FALSE);
            queue_scroll(self);
        }

        refresh_agents(self);
        return;
    }

    if (g_str_has_prefix(kind, "agent.") || g_str_has_prefix(kind, "mailbox.")) {
        refresh_agents(self);
        refresh_mailbox(self);
        return;
    }

    if (g_str_has_prefix(kind, "task."))
        refresh_tasks(self);
}

/* ── New agent ───────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow  *window;
    AdwDialog    *dialog;
    GtkWidget    *id_entry;
    GtkWidget    *computer_row;
    GtkWidget    *describe_entry;
    ModelChooser  models;
} NewAgentDialog;

static void
new_agent_dialog_free(gpointer data)
{
    NewAgentDialog *dialog = data;

    g_clear_pointer(&dialog->models.catalog, json_node_unref);
    g_free(dialog);
}

static JsonObject *
chooser_provider(ModelChooser *chooser)
{
    JsonArray *providers;
    guint selected;

    if (chooser->catalog == NULL)
        return NULL;

    providers = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "providers");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->provider_row));

    if (selected >= json_array_get_length(providers))
        return NULL;

    return json_array_get_object_element(providers, selected);
}

/*
 * Rebuilds the model list for whichever provider is selected.
 *
 * The models a provider runs are not interchangeable -- "sonnet" means
 * nothing to Ollama -- so one flat list would offer combinations that
 * cannot work.
 */
static void
on_provider_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ModelChooser *chooser = user_data;
    JsonObject *provider = chooser_provider(chooser);
    g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
    JsonArray *models;
    guint i;

    (void)object;
    (void)pspec;

    if (provider == NULL)
        return;

    models = json_object_get_array_member(provider, "models");

    for (i = 0; i < json_array_get_length(models); i++) {
        JsonObject *model = json_array_get_object_element(models, i);
        const gchar *note = clawt_json_string(model, "note", NULL);
        g_autofree gchar *label = NULL;

        label = (note != NULL)
                ? g_strdup_printf("%s - %s",
                                  clawt_json_string(model, "label", "?"), note)
                : g_strdup(clawt_json_string(model, "label", "?"));

        gtk_string_list_append(names, label);
    }

    /*
     * The catalogue is curated and goes stale, so every provider also
     * takes a name typed by hand.  Offering it as the last entry keeps
     * the common case one click.
     */
    gtk_string_list_append(names, "Something else…");

    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->model_row),
                            G_LIST_MODEL(g_steal_pointer(&names)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row), 0);

    if (clawt_json_string(provider, "note", NULL) != NULL) {
        adw_preferences_row_set_use_markup(
            ADW_PREFERENCES_ROW(chooser->provider_row), FALSE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->provider_row),
                                    clawt_json_string(provider, "note", ""));
    }
}

/* The free-text row appears only when "Something else…" is chosen. */
static void
on_model_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ModelChooser *chooser = user_data;
    JsonObject *provider = chooser_provider(chooser);
    JsonArray *models;
    guint selected;

    (void)object;
    (void)pspec;

    if (provider == NULL)
        return;

    models = json_object_get_array_member(provider, "models");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->model_row));

    gtk_widget_set_visible(chooser->model_entry,
                           selected >= json_array_get_length(models));
}

/*
 * Returns: (transfer full) (nullable): the chosen model id
 */
static gchar *
chooser_model(ModelChooser *chooser)
{
    JsonObject *provider = chooser_provider(chooser);
    JsonArray *models;
    guint selected;

    if (provider == NULL)
        return NULL;

    models = json_object_get_array_member(provider, "models");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->model_row));

    if (selected < json_array_get_length(models))
        return g_strdup(clawt_json_string(
            json_array_get_object_element(models, selected), "id", NULL));

    {
        const gchar *typed = gtk_editable_get_text(
            GTK_EDITABLE(chooser->model_entry));

        return (typed != NULL && *typed != '\0') ? g_strdup(typed) : NULL;
    }
}

/*
 * Returns: (transfer none) (nullable): the chosen provider id
 */
static const gchar *
chooser_provider_id(ModelChooser *chooser)
{
    JsonObject *provider = chooser_provider(chooser);

    return (provider != NULL) ? clawt_json_string(provider, "id", NULL)
                              : NULL;
}

/*
 * Adds the provider and model rows to a group, populated from the daemon
 * so every view agrees on what exists.
 */
static void
model_chooser_build(ModelChooser *chooser, ClawtWindow *window,
                    GtkWidget *group, const gchar *want_provider,
                    const gchar *want_model)
{
    g_autoptr(GtkStringList) provider_names = gtk_string_list_new(NULL);
    JsonArray *providers = NULL;
    guint chosen = 0;
    guint i;

    chooser->window = window;

    chooser->provider_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->provider_row),
                                  "Provider");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->provider_row);

    chooser->model_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->model_row),
                                  "Model");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->model_row);

    chooser->model_entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->model_entry),
                                  "Model name");
    gtk_widget_set_visible(chooser->model_entry, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->model_entry);

    chooser->catalog = clawt_window_request(window, "model.list", NULL);

    if (chooser->catalog != NULL) {
        providers = json_object_get_array_member(
            json_node_get_object(chooser->catalog), "providers");

        for (i = 0; i < json_array_get_length(providers); i++) {
            JsonObject *provider = json_array_get_object_element(providers, i);

            gtk_string_list_append(provider_names,
                                   clawt_json_string(provider, "label", "?"));

            if (want_provider != NULL &&
                g_strcmp0(clawt_json_string(provider, "id", ""),
                          want_provider) == 0)
                chosen = i;
        }
    }

    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->provider_row),
                            G_LIST_MODEL(g_steal_pointer(&provider_names)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->provider_row), chosen);

    g_signal_connect(chooser->provider_row, "notify::selected",
                     G_CALLBACK(on_provider_changed), chooser);
    g_signal_connect(chooser->model_row, "notify::selected",
                     G_CALLBACK(on_model_changed), chooser);

    on_provider_changed(NULL, NULL, chooser);

    /* Select the agent's current model, or offer it as typed text. */
    if (want_model != NULL && providers != NULL) {
        JsonObject *provider = chooser_provider(chooser);
        JsonArray *models = (provider != NULL)
                            ? json_object_get_array_member(provider, "models")
                            : NULL;
        gboolean found = FALSE;

        for (i = 0; models != NULL && i < json_array_get_length(models); i++) {
            if (g_strcmp0(clawt_json_string(
                              json_array_get_object_element(models, i),
                              "id", ""), want_model) != 0)
                continue;

            adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row), i);
            found = TRUE;
            break;
        }

        if (!found && models != NULL) {
            adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row),
                                       json_array_get_length(models));
            gtk_editable_set_text(GTK_EDITABLE(chooser->model_entry),
                                  want_model);
        }
    }
}

static void
on_create_manually(GtkButton *button, gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    JsonObject *provider = chooser_provider(&dialog->models);
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *model = NULL;
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm" };
    const gchar *agent_id;
    guint selected;

    (void)button;

    agent_id = gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry));

    if (agent_id == NULL || *agent_id == '\0') {
        clawt_window_toast(self, "An agent needs an id.");
        return;
    }

    model = chooser_model(&dialog->models);
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));

    reply = clawt_window_request(
        self, "agent.create",
        clawt_build_payload(
            "id", agent_id,
            "provider", provider != NULL
                        ? clawt_json_string(provider, "id", NULL) : NULL,
            "model", model,
            "computer", computers[MIN(selected, 3)],
            NULL));

    /*
     * The dialog stays open on failure, with the toast explaining why, so
     * whatever was typed is still there to correct.
     */
    if (reply == NULL)
        return;

    clawt_window_toast(self, "Agent created.");
    refresh_agents(self);

    /*
     * adw_dialog_close, not gtk_window_close.  An AdwDialog is a widget
     * inside the window rather than a window of its own, so asking for
     * its GtkWindow ancestor finds the main window -- which either does
     * nothing useful or closes the application.
     */
    adw_dialog_close(dialog->dialog);
}

static void
on_preview_response(AdwAlertDialog *dialog, gchar *response,
                    gpointer user_data)
{
    NewAgentDialog *new_agent = user_data;
    ClawtWindow *self = new_agent->window;
    const gchar *description;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = NULL;

    if (g_strcmp0(response, "create") != 0)
        return;

    description = g_object_get_data(G_OBJECT(dialog), "description");

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder, description);
    json_builder_set_member_name(builder, "commit");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "design.agent",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Agent created.");
    refresh_agents(self);

    /*
     * The outer dialog closes here too.  Only the manual path used to,
     * so designing an agent left the New agent dialog sitting open over
     * a fleet that already contained it.
     */
    adw_dialog_close(new_agent->dialog);
}

static void
on_design_with_ai(GtkButton *button, gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *description;

    description = gtk_editable_get_text(GTK_EDITABLE(dialog->describe_entry));

    if (description == NULL || *description == '\0') {
        clawt_window_toast(self, "Say what the agent should do first.");
        return;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);

    reply = clawt_window_request(
        self, "design.agent",
        clawt_build_payload("description", description, "commit", NULL,
                            NULL));

    gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);

    if (reply == NULL)
        return;

    /*
     * The design is shown before anything is written.  A model's proposal
     * becoming an agent without a person reading it first is exactly the
     * kind of convenience nobody asks for twice.
     */
    {
        AdwAlertDialog *preview = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Proposed agent", NULL));

        adw_alert_dialog_set_body(preview,
                                  clawt_json_string(clawt_payload_of(reply),
                                                    "yaml", ""));
        adw_alert_dialog_add_response(preview, "cancel", "Cancel");
        adw_alert_dialog_add_response(preview, "create", "Create");
        adw_alert_dialog_set_response_appearance(preview, "create",
                                                 ADW_RESPONSE_SUGGESTED);

        g_object_set_data_full(G_OBJECT(preview), "description",
                               g_strdup(description), g_free);
        g_signal_connect(preview, "response",
                         G_CALLBACK(on_preview_response), dialog);

        adw_dialog_present(ADW_DIALOG(preview), GTK_WIDGET(self));
    }
}

static void
on_new_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    NewAgentDialog *dialog = g_new0(NewAgentDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *manual = adw_preferences_group_new();
    GtkWidget *ai = adw_preferences_group_new();
    GtkWidget *create;
    GtkWidget *design;
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm", NULL };

    (void)button;

    dialog->window = self;
    dialog->dialog = window;

    adw_dialog_set_title(window, "New agent");
    adw_dialog_set_content_width(window, 520);

    /* ── By hand ── */
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(manual),
                                    "By hand");

    dialog->id_entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->id_entry),
                                  "Id");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->id_entry);

    /*
     * Provider first, then model: the model list depends on it, and
     * asking for a model before knowing the provider is how you end up
     * offering "sonnet" for Ollama.
     */
    model_chooser_build(&dialog->models, self, manual, NULL, NULL);

    dialog->computer_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->computer_row),
                                  "Computer");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->computer_row),
                            G_LIST_MODEL(gtk_string_list_new(computers)));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->computer_row);

    create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect(create, "clicked", G_CALLBACK(on_create_manually),
                     dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual), create);

    /* ── By description ── */
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(ai),
                                    "By description");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(ai),
        "Describe what you want and clawtilla drafts the configuration. "
        "You see it before anything is created.");

    dialog->describe_entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->describe_entry),
                                  "What should it do?");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(ai),
                              dialog->describe_entry);

    design = gtk_button_new_with_label("Design it");
    gtk_widget_set_margin_top(design, 12);
    g_signal_connect(design, "clicked", G_CALLBACK(on_design_with_ai),
                     dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(ai), design);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(manual));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(ai));

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           new_agent_dialog_free);
    adw_dialog_set_child(window, page);
    adw_dialog_present(window, GTK_WIDGET(self));
}

/* ── Construction ────────────────────────────────────────────────── */

static GtkWidget *
build_chat_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *send;

    self->transcript = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->transcript));
    gtk_widget_set_vexpand(scroll, TRUE);
    self->transcript_scroll = GTK_SCROLLED_WINDOW(scroll);

    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(on_scrolled), self);

    self->streaming = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(self->streaming), "dim-label");
    gtk_label_set_wrap(self->streaming, TRUE);
    gtk_label_set_xalign(self->streaming, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(self->streaming), 12);

    self->entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->entry, "Message");
    gtk_widget_set_hexpand(GTK_WIDGET(self->entry), TRUE);
    g_signal_connect(self->entry, "activate", G_CALLBACK(on_send), self);

    send = gtk_button_new_from_icon_name("document-send-symbolic");
    g_signal_connect(send, "clicked", G_CALLBACK(on_send), self);

    gtk_box_append(GTK_BOX(entry_box), GTK_WIDGET(self->entry));
    gtk_box_append(GTK_BOX(entry_box), send);
    gtk_widget_set_margin_start(entry_box, 12);
    gtk_widget_set_margin_end(entry_box, 12);
    gtk_widget_set_margin_top(entry_box, 6);
    gtk_widget_set_margin_bottom(entry_box, 12);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->streaming));
    gtk_box_append(GTK_BOX(box), entry_box);

    return box;
}

static GtkWidget *
build_inspector_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->inspector = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_margin_top(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->inspector), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->inspector));

    return scroll;
}

static GtkWidget *
build_mailbox_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->mailbox_summary = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->mailbox_summary), 12);

    self->mailbox_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->mailbox_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->mailbox_list), "boxed-list");
    gtk_widget_set_margin_start(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->mailbox_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->mailbox_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->mailbox_summary));
    gtk_box_append(GTK_BOX(box), scroll);

    return box;
}

static GtkWidget *
build_computer_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *run;
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->computer_state = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->computer_state), 12);

    self->exec_output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(self->exec_output, FALSE);
    gtk_text_view_set_monospace(self->exec_output, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->exec_output));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);

    self->exec_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->exec_entry, "Command");
    gtk_widget_set_hexpand(GTK_WIDGET(self->exec_entry), TRUE);
    g_signal_connect(self->exec_entry, "activate", G_CALLBACK(on_exec), self);

    run = gtk_button_new_with_label("Run");
    g_signal_connect(run, "clicked", G_CALLBACK(on_exec), self);

    gtk_box_append(GTK_BOX(entry_box), GTK_WIDGET(self->exec_entry));
    gtk_box_append(GTK_BOX(entry_box), run);
    gtk_widget_set_margin_start(entry_box, 12);
    gtk_widget_set_margin_end(entry_box, 12);
    gtk_widget_set_margin_bottom(entry_box, 12);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->computer_state));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), entry_box);

    return box;
}

static GtkWidget *
build_task_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->task_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->task_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->task_list), "boxed-list");
    gtk_widget_set_margin_top(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->task_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->task_list));

    return scroll;
}

ClawtWindow *
clawt_window_new(AdwApplication *app, ClawtClient *client)
{
    ClawtWindow *self;
    GtkWidget *sidebar_box;
    GtkWidget *sidebar_scroll;
    GtkWidget *sidebar_header;
    GtkWidget *content;
    GtkWidget *header;
    GtkWidget *switcher;
    GtkWidget *title;
    GtkWidget *new_button;
    GtkWidget *sidebar_button;
    AdwBreakpoint *breakpoint;

    self = g_object_new(CLAWT_TYPE_WINDOW, "application", app, NULL);
    self->client = g_object_ref(client);

    gtk_window_set_title(GTK_WINDOW(self), "clawtilla");
    gtk_window_set_default_size(GTK_WINDOW(self), 1100, 720);

    /* ── Sidebar ── */
    self->sidebar = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->sidebar, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->sidebar), "navigation-sidebar");
    g_signal_connect(self->sidebar, "row-activated",
                     G_CALLBACK(on_row_activated), self);

    sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                  GTK_WIDGET(self->sidebar));
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);

    sidebar_header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebar_header),
                                    adw_window_title_new("Agents", NULL));

    new_button = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(new_button, "Add an agent");
    g_signal_connect(new_button, "clicked", G_CALLBACK(on_new_agent), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), new_button);

    sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_header);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_scroll);

    /* ── Content ── */
    self->pages = ADW_VIEW_STACK(adw_view_stack_new());

    adw_view_stack_add_titled_with_icon(self->pages, build_chat_page(self),
                                        "chat", "Chat",
                                        "user-available-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages,
                                        build_inspector_page(self),
                                        "agent", "Agent",
                                        "emblem-system-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages, build_mailbox_page(self),
                                        "mailbox", "Mailbox",
                                        "mail-unread-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages,
                                        build_computer_page(self),
                                        "computer", "Computer",
                                        "utilities-terminal-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages, build_task_page(self),
                                        "tasks", "Tasks",
                                        "view-list-symbolic");

    header = adw_header_bar_new();
    title = adw_window_title_new("clawtilla", NULL);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    g_object_set_data(G_OBJECT(self), "title", title);

    sidebar_button = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar_button),
                             "sidebar-show-symbolic");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar_button);

    switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), self->pages);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                 ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), switcher);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content), header);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->pages));
    gtk_widget_set_vexpand(GTK_WIDGET(self->pages), TRUE);

    self->split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar(self->split, sidebar_box);
    adw_overlay_split_view_set_content(self->split, content);

    g_object_bind_property(sidebar_button, "active", self->split,
                           "show-sidebar",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebar_button), TRUE);

    /*
     * Below 800px the sidebar becomes a drawer.  Without this the agent
     * list eats a phone-sized window and the conversation has nowhere to
     * go.
     */
    breakpoint = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 800px"));
    adw_breakpoint_add_setters(breakpoint, G_OBJECT(self->split),
                               "collapsed", TRUE, NULL);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self),
                                          breakpoint);

    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->split));

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(self->toasts));

    g_signal_connect(client, "event", G_CALLBACK(on_daemon_event), self);

    refresh_agents(self);

    return self;
}

static void
clawt_window_dispose(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    if (self->client != NULL)
        g_signal_handlers_disconnect_by_data(self->client, self);

    g_clear_object(&self->client);

    G_OBJECT_CLASS(clawt_window_parent_class)->dispose(object);
}

static void
clawt_window_finalize(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    g_free(self->selected_agent);

    G_OBJECT_CLASS(clawt_window_parent_class)->finalize(object);
}

static void
clawt_window_class_init(ClawtWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_window_dispose;
    object_class->finalize = clawt_window_finalize;
}

static void
clawt_window_init(ClawtWindow *self)
{
    self->following = TRUE;
}
