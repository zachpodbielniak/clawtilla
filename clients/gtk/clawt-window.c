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

/*
 * The container image, as a list plus a way to type one that is not on
 * it.
 *
 * The catalogue is a suggestion, never a restriction -- any reference
 * podman can pull is valid -- so the last entry is always "Other" and
 * selecting it reveals a free-text row.  The daemon merges the user's
 * own images from defaults.container_images into the list before it gets
 * here, so this does not need to know where an entry came from.
 */
typedef struct {
    ClawtWindow *window;
    GtkWidget   *row;
    GtkWidget   *entry;
    JsonNode    *catalog;
} ImageChooser;

/* Defined below; the inspector and the create dialog both use them. */
static JsonObject  *chooser_provider(ModelChooser *chooser);
static const gchar *chooser_provider_id(ModelChooser *chooser);
static gchar       *chooser_model(ModelChooser *chooser);
static void         model_chooser_build(ModelChooser *chooser,
                                        ClawtWindow  *window,
                                        GtkWidget    *group,
                                        const gchar  *want_provider,
                                        const gchar  *want_model);
static void         model_chooser_build_full(ModelChooser *chooser,
                                             ClawtWindow  *window,
                                             GtkWidget    *group,
                                             const gchar  *want_provider,
                                             const gchar  *want_model,
                                             const gchar  *require);
static void         image_chooser_build(ImageChooser *chooser,
                                        ClawtWindow  *window,
                                        GtkWidget    *group,
                                        const gchar  *want);
static gchar *      image_chooser_value(ImageChooser *chooser);
static const gchar *answer_of(GtkWidget *row);
static void         on_image_changed(GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    user_data);

/*
 * Every view that rebuilds a list from a daemon reply needs one of these.
 *
 * clawt_window_request() iterates the main context while it waits, and the
 * client delivers events from an idle, so an event handler runs in the
 * middle of the rebuild -- and calls the same refresh again.  The inner
 * call emptied the list and refilled it, then the outer call carried on
 * appending from where it was, which showed the tail of the fleet twice.
 */
typedef enum {
    CLAWT_REFRESH_AGENTS,
    CLAWT_REFRESH_SELECTED,
    CLAWT_REFRESH_MAILBOX,
    CLAWT_REFRESH_TASKS,
    CLAWT_N_REFRESH
} ClawtRefreshKind;

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
    ImageChooser       inspector_image;
    gchar             *inspector_computer;   /* the selected agent's type */
    GtkWidget         *vm_cpus_row;
    GtkWidget         *vm_memory_row;
    GtkWidget         *mount_source_row;
    GtkWidget         *mount_target_row;
    GtkWidget         *mount_mode_row;

    /* Mailbox */
    GtkListBox        *mailbox_list;
    GtkLabel          *mailbox_summary;

    /* Computer */
    GtkEntry          *exec_entry;
    GtkTextView       *exec_output;
    GtkLabel          *computer_state;

    /* Tasks */
    GtkListBox        *task_list;

    GtkWidget         *activity_bar;
    GtkSpinner        *activity_spinner;

    GtkWidget         *agent_menu;
    GSimpleActionGroup *agent_actions;

    gboolean           refreshing[CLAWT_N_REFRESH];
    gboolean           refresh_again[CLAWT_N_REFRESH];

    gchar             *selected_agent;
    gboolean           following;
};

G_DEFINE_FINAL_TYPE(ClawtWindow, clawt_window, ADW_TYPE_APPLICATION_WINDOW)

/*
 * Returns %TRUE when the caller owns the rebuild.  A nested call notes
 * that the data changed under it and returns; the owner runs the body
 * again on the way out, so the last word still belongs to the daemon.
 */
static gboolean
refresh_enter(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (self->refreshing[kind]) {
        self->refresh_again[kind] = TRUE;
        return FALSE;
    }

    self->refreshing[kind] = TRUE;
    return TRUE;
}

/* Returns %TRUE when the body must run once more. */
static gboolean
refresh_repeat(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (!self->refresh_again[kind]) {
        self->refreshing[kind] = FALSE;
        return FALSE;
    }

    self->refresh_again[kind] = FALSE;
    return TRUE;
}

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

    /*
     * The context menu decides what to grey out from here rather than
     * asking the daemon again: a right-click has to be instant, and the
     * sidebar was drawn from the same reply a moment ago.
     */
    g_object_set_data_full(G_OBJECT(row), "agent-state", g_strdup(state),
                           g_free);

    return row;
}

/*
 * Selection, not activation, is what drives the view.
 *
 * AdwActionRow clears GtkListBoxRow:activatable unless an
 * activatable-widget is set, so ::row-activated never fires for these
 * rows at all -- clicking an agent moved the highlight and nothing else,
 * leaving the previous agent's transcript on screen under the new
 * agent's name.  ::row-selected also covers arrow-key navigation, which
 * activation never did.
 */
static void
on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id;

    (void)box;

    /* Emptying the list to rebuild it unselects; that is not a choice. */
    if (row == NULL)
        return;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    if (agent_id != NULL)
        select_agent(self, agent_id);
}

static void
clear_list(GtkListBox *list)
{
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));

    /*
     * Rows only, and walked rather than repeatedly taking the first
     * child.  A GtkPopover parented to the list -- the sidebar's context
     * menu is one -- is a child like any other, and gtk_list_box_remove()
     * refuses to take it: the old loop then asked for the same first
     * child forever and hung the window before it ever drew.
     */
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);

        if (GTK_IS_LIST_BOX_ROW(child))
            gtk_list_box_remove(list, child);

        child = next;
    }
}

static void
clear_box(GtkBox *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, child);
}

static void
refresh_agents_once(ClawtWindow *self)
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

        /*
         * Keep the current selection across a refresh, and make the very
         * first refresh pick something.  Both go through
         * gtk_list_box_select_row() rather than select_agent() so the
         * highlight and self->selected_agent can never disagree -- they
         * did, and the sidebar showed no selection at all until the
         * second refresh.
         */
        if (g_strcmp0(clawt_json_string(agent, "id", ""),
                      self->selected_agent) == 0 ||
            (self->selected_agent == NULL && i == 0))
            gtk_list_box_select_row(self->sidebar, GTK_LIST_BOX_ROW(row));
    }
}

static void
refresh_agents(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_AGENTS))
        return;

    do {
        refresh_agents_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_AGENTS));
}

/* ── Chat ────────────────────────────────────────────────────────── */

/*
 * Shows or hides the activity line.
 *
 * The spinner is stopped when hidden rather than left running: a
 * GtkSpinner that is not visible still drives a frame clock, and there
 * is one per window for the life of the process.
 */
static void
set_activity(ClawtWindow *self, const gchar *text)
{
    if (self->activity_bar == NULL)
        return;

    if (text == NULL) {
        gtk_spinner_stop(self->activity_spinner);
        gtk_widget_set_visible(self->activity_bar, FALSE);
        gtk_label_set_text(self->streaming, "");
        return;
    }

    gtk_label_set_text(self->streaming, text);
    gtk_widget_set_visible(self->activity_bar, TRUE);
    gtk_spinner_start(self->activity_spinner);
}

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
    set_activity(self, NULL);

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

    /*
     * A stopped agent accepts the message -- that is what a durable
     * mailbox is for -- but it will not answer it, and a spinner that
     * spins forever is a worse lie than no spinner at all.  The daemon
     * reports the target's state so this does not have to be inferred
     * from a sidebar that may be a refresh behind.
     */
    {
        const gchar *state = clawt_json_string(clawt_payload_of(reply),
                                               "target_state", NULL);

        if (state != NULL && g_strcmp0(state, "running") != 0) {
            g_autofree gchar *warning = g_strdup_printf(
                "%s is %s -- held in its mailbox until it starts",
                self->selected_agent, state);

            set_activity(self, NULL);
            clawt_window_toast(self, warning);
        } else {
            set_activity(self, "delivered -- waiting for a reply");
        }
    }

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

/* Shared by the inspector's buttons and the sidebar's context menu. */
static void
agent_action(ClawtWindow *self, const gchar *kind)
{
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || kind == NULL)
        return;

    reply = clawt_window_request(
        self, kind, clawt_build_payload("agent", self->selected_agent, NULL));

    (void)reply;

    /*
     * Refreshed whether or not it worked.  A start that fails still moves
     * the agent to ERROR, and skipping the refresh on failure left the
     * sidebar showing "stopped" next to a toast explaining why it could
     * not start -- two contradictory answers on screen at once.
     */
    refresh_agents(self);
    refresh_selected(self);
}

static void
on_agent_action(GtkButton *button, gpointer user_data)
{
    agent_action(user_data, g_object_get_data(G_OBJECT(button), "kind"));
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

    if (self->vm_cpus_row != NULL) {
        const gchar *cpus = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_cpus_row));
        const gchar *memory = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_memory_row));

        if (cpus != NULL && *cpus != '\0')
            ok &= apply_setting(self, "computer.vm.cpus", cpus);

        if (memory != NULL && *memory != '\0')
            ok &= apply_setting(self, "computer.vm.memory_mb", memory);
    }

    if (self->inspector_image.row != NULL) {
        g_autofree gchar *image = image_chooser_value(&self->inspector_image);

        if (image != NULL)
            ok &= apply_setting(self, "computer.container.image", image);
    }

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
    GtkWidget *check = g_object_get_data(G_OBJECT(dialog), "remove-computer");
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *agent_id = NULL;
    gboolean with_computer;

    if (g_strcmp0(response, "delete") != 0)
        return;

    agent_id = g_strdup(self->selected_agent);
    with_computer = check != NULL &&
                    gtk_check_button_get_active(GTK_CHECK_BUTTON(check));

    reply = clawt_window_request(
        self, "agent.remove",
        clawt_build_payload("agent", agent_id,
                            "remove_computer", with_computer ? "true" : NULL,
                            NULL));

    if (reply == NULL)
        return;

    g_clear_pointer(&self->selected_agent, g_free);
    clear_box(self->inspector);
    clear_box(self->transcript);

    {
        const gchar *computer = clawt_json_string(clawt_payload_of(reply),
                                                   "computer", NULL);
        g_autofree gchar *message = NULL;

        /*
         * The computer's fate is reported rather than assumed: a
         * teardown that failed is not fatal to the removal, and a toast
         * saying "is gone" over a container still running would be a
         * lie.
         */
        if (computer != NULL && g_strcmp0(computer, "removed") == 0)
            message = g_strdup_printf("%s and its computer are gone.",
                                      agent_id);
        else if (computer != NULL)
            message = g_strdup_printf("%s is gone; its computer was not "
                                      "removed: %s", agent_id, computer);
        else
            message = g_strdup_printf("%s is gone.", agent_id);

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

    /*
     * The container or VM is a separate decision.
     *
     * Removing the agent used to leave it running under a name derived
     * from an agent that no longer existed, so the only way to find it
     * again was to remember what it had been called. Off by default,
     * because a container may hold work that was never anywhere else.
     */
    if (g_strcmp0(self->inspector_computer, "container") == 0 ||
        g_strcmp0(self->inspector_computer, "vm") == 0) {
        GtkWidget *check;
        g_autofree gchar *label = g_strdup_printf(
            "Also delete its %s, clawt-%s",
            g_strcmp0(self->inspector_computer, "vm") == 0
                ? "virtual machine" : "container",
            self->selected_agent);

        check = gtk_check_button_new_with_label(label);
        gtk_widget_set_margin_top(check, 12);
        adw_alert_dialog_set_extra_child(second, check);

        /*
         * Kept on the dialog rather than in the window: the dialog is
         * what the response handler is given, and a second delete
         * started before the first finished would otherwise read the
         * wrong checkbox.
         */
        g_object_set_data(G_OBJECT(second), "remove-computer", check);
    }

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
delete_agent(ClawtWindow *self)
{
    AdwAlertDialog *first;
    g_autofree gchar *heading = NULL;

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

static void
on_delete_agent(GtkButton *button, gpointer user_data)
{
    (void)button;

    delete_agent(user_data);
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

/*
 * Shares a host folder with the agent's computer.
 *
 * The mount list has always been read and applied -- bind mounts for a
 * container, virtiofs devices for a VM -- and no client could write one,
 * so the only way to share a folder was to edit the YAML by hand.
 */
static void
on_add_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    static const gchar *const modes[] = { "ro", "rw" };
    const gchar *source;
    const gchar *target;
    guint mode;

    (void)button;

    if (self->selected_agent == NULL || self->mount_source_row == NULL)
        return;

    source = gtk_editable_get_text(GTK_EDITABLE(self->mount_source_row));
    target = gtk_editable_get_text(GTK_EDITABLE(self->mount_target_row));

    if (source == NULL || *source == '\0' || target == NULL ||
        *target == '\0') {
        clawt_window_toast(self, "A share needs a folder and a path inside.");
        return;
    }

    mode = adw_combo_row_get_selected(ADW_COMBO_ROW(self->mount_mode_row));

    reply = clawt_window_request(
        self, "agent.mount.add",
        clawt_build_payload("agent", self->selected_agent,
                            "source", source, "target", target,
                            "mode", modes[MIN(mode, 1)], NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Shared. It appears when the agent restarts.");
    refresh_selected(self);
}

static void
on_remove_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "target");
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || target == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.mount.remove",
        clawt_build_payload("agent", self->selected_agent, "target", target,
                            NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Removed. Takes effect on the next restart.");
    refresh_selected(self);
}

/*
 * The shared folders group, for a computer that can have them.
 *
 * A host agent is left out: its computer is the machine itself, so there
 * is nothing to mount into -- what limits it there is confinement, not
 * a mount list.
 */
static void
build_mounts(ClawtWindow *self, const gchar *computer_type)
{
    static const gchar *const modes[] = { "ro", "rw", NULL };
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    GtkWidget *add;
    JsonArray *mounts;
    guint i;

    self->mount_source_row = NULL;
    self->mount_target_row = NULL;
    self->mount_mode_row = NULL;

    if (g_strcmp0(computer_type, "container") != 0 &&
        g_strcmp0(computer_type, "vm") != 0)
        return;

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Shared folders");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        g_strcmp0(computer_type, "vm") == 0
            ? "Passed into the VM over virtiofs. Changes apply when it "
              "next starts."
            : "Bind-mounted into the container, SELinux-relabelled so "
              "they are actually readable. Changes apply when it next "
              "starts.");

    reply = clawt_window_request(
        self, "agent.mount.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    mounts = (reply != NULL)
             ? json_object_get_array_member(clawt_payload_of(reply), "mounts")
             : NULL;

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *target = clawt_json_string(mount, "target", "?");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *remove;
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s  (%s)", clawt_json_string(mount, "source", "-"),
            clawt_json_string(mount, "mode", "ro"));

        set_row_text(row, target, subtitle);

        remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Stop sharing this folder");
        g_object_set_data_full(G_OBJECT(remove), "target", g_strdup(target),
                               g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_mount),
                         self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    self->mount_source_row = entry_row("Folder on this machine", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_source_row);

    self->mount_target_row = entry_row("Where the agent sees it", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_target_row);

    self->mount_mode_row = combo_row("Access", modes, "ro");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_mode_row);

    add = gtk_button_new_with_label("Share this folder");
    gtk_widget_set_margin_top(add, 6);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_mount), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add);

    gtk_box_append(self->inspector, group);
}

/* ── Opening a workspace file ────────────────────────────────────── */

/*
 * One attempt at running an editor, so a failed one can be retried
 * differently.
 *
 * Holds a reference to the window: this outlives the click that started
 * it, and an editor the user leaves open for an hour would otherwise be
 * writing its result into freed memory when they finally quit it.
 */
typedef struct {
    ClawtWindow *window;
    gchar       *path;
    gchar       *name;
    gboolean     in_terminal;
    gint64       started;
} EditorLaunch;

static void editor_launch(EditorLaunch *launch);

static void
editor_launch_free(EditorLaunch *launch)
{
    g_clear_object(&launch->window);
    g_free(launch->path);
    g_free(launch->name);
    g_free(launch);
}

/*
 * $CLAWT_EDITOR first, so this can be pointed somewhere else without
 * changing what every other program on the machine does.
 */
static const gchar *
editor_command(void)
{
    const gchar *editor = g_getenv("CLAWT_EDITOR");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("VISUAL");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("EDITOR");

    return (editor != NULL && editor[0] != '\0') ? editor : NULL;
}

/*
 * A terminal to run a terminal-only editor inside.
 *
 * There is no way to ask an editor whether it needs one, so this is
 * only reached after running it bare has already failed -- which is
 * exactly what a curses editor does when it is started without a
 * terminal, and quickly. The flag differs per terminal: some take the
 * command straight after the binary, some want a separator first.
 */
static gboolean
terminal_prefix(GPtrArray *argv)
{
    static const struct {
        const gchar *binary;
        const gchar *flag;
    } terminals[] = {
        { "xdg-terminal-exec", NULL },
        { "gst",               "-e" },
        { "foot",              NULL },
        { "kitty",             NULL },
        { "wezterm",           "start" },
        { "alacritty",         "-e" },
        { "gnome-terminal",    "--" },
        { "xterm",             "-e" }
    };
    const gchar *configured = g_getenv("TERMINAL");
    gsize i;

    if (configured != NULL && configured[0] != '\0') {
        g_ptr_array_add(argv, g_strdup(configured));
        g_ptr_array_add(argv, g_strdup("-e"));
        return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(terminals); i++) {
        g_autofree gchar *found = g_find_program_in_path(terminals[i].binary);

        if (found == NULL)
            continue;

        g_ptr_array_add(argv, g_steal_pointer(&found));

        if (terminals[i].flag != NULL)
            g_ptr_array_add(argv, g_strdup(terminals[i].flag));

        return TRUE;
    }

    return FALSE;
}

static void
on_editor_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    EditorLaunch *launch = user_data;
    g_autoptr(GError) error = NULL;

    if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        editor_launch_free(launch);
        return;
    }

    /*
     * A terminal editor started without a terminal fails immediately,
     * and there is no way to know in advance which kind $EDITOR is --
     * emacsclient opens a window here and vi does not. So the bare run
     * is the test, and this is the second attempt.
     *
     * Only when it failed straight away, though: an editor the user
     * spent ten minutes in and then quit with a non-zero status has
     * already done its job, and popping a terminal open at that point
     * would be baffling.
     */
    if (!launch->in_terminal &&
        g_get_monotonic_time() - launch->started < 2 * G_USEC_PER_SEC) {
        launch->in_terminal = TRUE;
        editor_launch(launch);
        return;
    }

    {
        g_autofree gchar *message =
            g_strdup_printf("could not open %s: %s", launch->name,
                            error->message);

        clawt_window_toast(launch->window, message);
    }

    editor_launch_free(launch);
}

static void
editor_launch(EditorLaunch *launch)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *editor = editor_command();
    gint i;

    if (launch->in_terminal && !terminal_prefix(argv)) {
        clawt_window_toast(launch->window,
                           "no terminal found to run $EDITOR in");
        editor_launch_free(launch);
        return;
    }

    /*
     * $EDITOR is a command line, not a program name: "emacsclient -nw"
     * and "code --wait" are both ordinary settings, so it is parsed
     * rather than exec'd whole.
     */
    if (editor == NULL || !g_shell_parse_argv(editor, NULL, &parts, &error)) {
        g_autofree gchar *message =
            g_strdup_printf("$EDITOR (%s) could not be parsed: %s",
                            editor != NULL ? editor : "unset",
                            error != NULL ? error->message : "it is not set");

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, g_strdup(parts[i]));

    g_ptr_array_add(argv, g_strdup(launch->path));
    g_ptr_array_add(argv, NULL);

    process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error);

    if (process == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("could not run %s: %s", editor, error->message);

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    launch->started = g_get_monotonic_time();
    g_subprocess_wait_check_async(process, NULL, on_editor_finished, launch);
}

static void
on_open_file(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    EditorLaunch *launch;
    const gchar *path = g_object_get_data(G_OBJECT(button), "path");
    const gchar *name = g_object_get_data(G_OBJECT(button), "name");

    if (editor_command() == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("no editor set; try `clawtilla agent edit %s %s`",
                            self->selected_agent, name);

        clawt_window_toast(self, message);
        return;
    }

    launch = g_new0(EditorLaunch, 1);
    launch->window = g_object_ref(self);
    launch->path = g_strdup(path);
    launch->name = g_strdup(name);

    editor_launch(launch);
}

/*
 * The agent's workspace files, each openable in $EDITOR.
 *
 * These are the files the agent reads as its system prompt, plus the
 * .mcp.json that decides which MCP servers it can call -- so this is
 * where an agent is actually configured, as opposed to merely wired up.
 */
static void
build_files(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    JsonArray *files;
    guint i;

    reply = clawt_window_request(
        self, "agent.files",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    files = json_object_get_array_member(clawt_payload_of(reply), "files");

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Files");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Opened in $EDITOR. The ones marked \"prompt\" are read into every "
        "turn; .mcp.json is the list of MCP servers this agent may call, "
        "and clawtilla only ever rewrites its own entry in it.");

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *name = clawt_json_string(file, "name", "?");
        const gchar *path = clawt_json_string(file, "path", "");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *open;
        g_autofree gchar *subtitle = NULL;

        subtitle = json_object_get_boolean_member(file, "identity")
                   ? g_strdup_printf("prompt \xc2\xb7 %s",
                                     clawt_json_string(file, "title", ""))
                   : g_strdup(clawt_json_string(file, "title", ""));

        set_row_text(row, name, subtitle);
        gtk_widget_set_tooltip_text(row, path);

        /*
         * An explicit button rather than an activatable row: libadwaita
         * clears GtkListBoxRow:activatable on an AdwActionRow unless it
         * has an activatable-widget, so ::row-activated would never
         * fire and clicking would appear to do nothing.
         */
        open = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(open, "flat");
        gtk_widget_set_tooltip_text(open, "Open in $EDITOR");
        g_object_set_data_full(G_OBJECT(open), "path", g_strdup(path),
                               g_free);
        g_object_set_data_full(G_OBJECT(open), "name", g_strdup(name),
                               g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_file), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    gtk_box_append(self->inspector, group);
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

    /*
     * The image chooser is rebuilt with the inspector, so its catalogue
     * and its widget pointers are cleared together -- a stale row
     * pointer here is one the save handler would write through after
     * the widget had been destroyed.
     */
    g_clear_pointer(&self->inspector_image.catalog, json_node_unref);
    self->inspector_image.row = NULL;
    self->inspector_image.entry = NULL;

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

    /*
     * The image, for a container agent.  Built only when there is one:
     * an agent that is not a container has nothing to show, and the
     * inspector is rebuilt whenever the selection changes, so switching
     * the computer type and saving brings the row in on the next
     * refresh.
     */
    g_free(self->inspector_computer);
    self->inspector_computer =
        g_strdup(clawt_json_string(agent, "computer", "none"));

    if (g_strcmp0(self->inspector_computer, "container") == 0)
        image_chooser_build(&self->inspector_image, self, group,
                            clawt_json_string(agent, "image", NULL));
    else
        self->inspector_image.row = NULL;

    /*
     * How big the VM is.  Only shown for one: cpus and memory_mb are
     * read by no other backend, and a row that quietly does nothing is
     * worse than no row.
     */
    self->vm_cpus_row = NULL;
    self->vm_memory_row = NULL;

    if (g_strcmp0(self->inspector_computer, "vm") == 0) {
        self->vm_cpus_row = entry_row(
            "Cores", clawt_json_string(agent, "vm_cpus", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_cpus_row);

        self->vm_memory_row = entry_row(
            "Memory (MB)", clawt_json_string(agent, "vm_memory_mb", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_memory_row);
    }

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

    build_mounts(self, self->inspector_computer);
    build_files(self);

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
refresh_mailbox_once(ClawtWindow *self)
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

static void
refresh_mailbox(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_MAILBOX))
        return;

    do {
        refresh_mailbox_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_MAILBOX));
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
refresh_tasks_once(ClawtWindow *self)
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

static void
refresh_tasks(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_TASKS))
        return;

    do {
        refresh_tasks_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_TASKS));
}

/* ── Selection ───────────────────────────────────────────────────── */

static void
refresh_selected_once(ClawtWindow *self)
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
refresh_selected(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_SELECTED))
        return;

    do {
        refresh_selected_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_SELECTED));
}

static void
select_agent(ClawtWindow *self, const gchar *agent_id)
{
    if (agent_id == NULL || *agent_id == '\0')
        return;

    /*
     * Re-selecting what is already shown has to be a no-op, and not by
     * luck: refresh_agents() rebuilds the sidebar on every daemon event
     * and restores the selection, which emits ::row-selected again.
     * Reloading there would drop the transcript each time an event
     * arrived, and load_history() iterates the main context, so it would
     * re-enter this function while the first call was still running.
     */
    if (g_strcmp0(agent_id, self->selected_agent) == 0)
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

/* ── The sidebar's context menu ───────────────────────────────────── */

/*
 * Start, stop and restart without leaving the conversation.
 *
 * These live on the Agent tab too, but reaching for them meant leaving
 * the chat, acting, and coming back -- three navigations for the thing
 * you do most often to an agent.
 */
static void
on_menu_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    (void)parameter;

    if (g_strcmp0(name, "delete") == 0) {
        delete_agent(self);
        return;
    }

    {
        g_autofree gchar *kind = g_strconcat("agent.", name, NULL);

        agent_action(self, kind);
    }
}

static void
enable_agent_action(ClawtWindow *self, const gchar *name, gboolean enabled)
{
    GAction *action = g_action_map_lookup_action(
        G_ACTION_MAP(self->agent_actions), name);

    if (action != NULL)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/*
 * A menu that offers what would fail is worse than one that greys it
 * out, so the same rules the inspector's buttons use apply here.  A
 * shadow agent cannot run at all: its configuration did not parse, and
 * every lifecycle verb would be refused by the daemon.
 */
static void
set_agent_action_states(ClawtWindow *self, const gchar *state)
{
    gboolean shadow = g_strcmp0(state, "shadow") == 0;

    enable_agent_action(self, "start",
                        !shadow && g_strcmp0(state, "running") != 0);
    enable_agent_action(self, "stop",
                        !shadow && g_strcmp0(state, "stopped") != 0);
    enable_agent_action(self, "restart", !shadow);
    enable_agent_action(self, "delete", TRUE);
}

static void
popup_agent_menu(ClawtWindow *self, gdouble x, gdouble y)
{
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(self->sidebar, (gint)y);
    g_autofree gchar *state = NULL;
    const gchar *agent_id;
    GdkRectangle rect;

    if (row == NULL || self->agent_menu == NULL)
        return;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    /* The "No agents yet" placeholder is a row like any other. */
    if (agent_id == NULL)
        return;

    /*
     * Copied off the row before anything else, because selecting it
     * loads the transcript, which iterates the main context, which lets
     * a refresh rebuild the sidebar and free this row underneath us.
     */
    state = g_strdup(g_object_get_data(G_OBJECT(row), "agent-state"));

    /* Act on what was right-clicked, not on what happened to be selected. */
    gtk_list_box_select_row(self->sidebar, row);

    set_agent_action_states(self, state);

    rect.x = (gint)x;
    rect.y = (gint)y;
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(self->agent_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(self->agent_menu));
}

static void
on_sidebar_secondary_click(GtkGestureClick *gesture, gint n_press,
                           gdouble x, gdouble y, gpointer user_data)
{
    (void)gesture;
    (void)n_press;

    popup_agent_menu(user_data, x, y);
}

/* Touch has no second button; a long press is the same gesture there. */
static void
on_sidebar_long_press(GtkGestureLongPress *gesture, gdouble x, gdouble y,
                      gpointer user_data)
{
    (void)gesture;

    popup_agent_menu(user_data, x, y);
}

static void
build_agent_menu(ClawtWindow *self)
{
    static const gchar *const names[] = { "start", "stop", "restart",
                                          "delete" };
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) lifecycle = g_menu_new();
    g_autoptr(GMenu) danger = g_menu_new();
    GtkGesture *click;
    GtkGesture *press;
    guint i;

    self->agent_actions = g_simple_action_group_new();

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), self);
        g_action_map_add_action(G_ACTION_MAP(self->agent_actions),
                                G_ACTION(action));
    }

    g_menu_append(lifecycle, "Start", "agent.start");
    g_menu_append(lifecycle, "Stop", "agent.stop");
    g_menu_append(lifecycle, "Restart", "agent.restart");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(lifecycle));

    /* Its own section, so Delete is never the neighbour of Restart. */
    g_menu_append(danger, "Delete\342\200\246", "agent.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(danger));

    gtk_widget_insert_action_group(GTK_WIDGET(self), "agent",
                                   G_ACTION_GROUP(self->agent_actions));

    self->agent_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(self->agent_menu), FALSE);
    gtk_widget_set_halign(self->agent_menu, GTK_ALIGN_START);
    gtk_widget_set_parent(self->agent_menu, GTK_WIDGET(self->sidebar));

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_sidebar_secondary_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->sidebar),
                              GTK_EVENT_CONTROLLER(click));

    press = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(press), TRUE);
    g_signal_connect(press, "pressed",
                     G_CALLBACK(on_sidebar_long_press), self);
    gtk_widget_add_controller(GTK_WIDGET(self->sidebar),
                              GTK_EVENT_CONTROLLER(press));
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

            /*
             * The reply is the end of the turn.  libreclaw drops the
             * typing indicator too, but the message overtakes it often
             * enough that relying on the indicator alone leaves a
             * spinner running under an answer that has already arrived.
             */
            if (g_strcmp0(from, self->selected_agent) == 0)
                set_activity(self, NULL);

            queue_scroll(self);
        }

        refresh_agents(self);
        return;
    }

    if (g_strcmp0(kind, "agent.typing") == 0) {
        const gchar *typing = clawt_event_get_detail(event, "typing");

        if (g_strcmp0(clawt_event_get_subject(event),
                      self->selected_agent) == 0)
            set_activity(self,
                         g_strcmp0(typing, "true") == 0 ? "thinking" : NULL);

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
    GtkWidget    *name_entry;
    GtkWidget    *description_entry;
    GtkWidget    *computer_row;
    GtkWidget    *describe_entry;   /* purpose: the one required answer */
    GtkWidget    *boundaries_entry;
    GtkWidget    *needs_entry;
    GtkWidget    *personality_entry;
    GtkWidget    *projects_entry;
    GtkWidget    *notes_entry;
    ModelChooser  models;           /* the model the agent will run */
    ModelChooser  designer;         /* the model that drafts it */
    ImageChooser  image;
} NewAgentDialog;

static void
new_agent_dialog_free(gpointer data)
{
    NewAgentDialog *dialog = data;

    g_clear_pointer(&dialog->models.catalog, json_node_unref);
    g_clear_pointer(&dialog->designer.catalog, json_node_unref);
    g_clear_pointer(&dialog->image.catalog, json_node_unref);
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

/* The reference currently chosen, from the list or from the entry. */
static gchar *
image_chooser_value(ImageChooser *chooser)
{
    JsonArray *images;
    guint selected;

    if (chooser->catalog == NULL)
        return NULL;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    if (selected < json_array_get_length(images))
        return g_strdup(clawt_json_string(
            json_array_get_object_element(images, selected), "reference",
            NULL));

    {
        const gchar *typed =
            gtk_editable_get_text(GTK_EDITABLE(chooser->entry));

        return (typed != NULL && *typed != '\0') ? g_strdup(typed) : NULL;
    }
}

static void
on_image_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ImageChooser *chooser = user_data;
    JsonArray *images;
    guint selected;

    (void)object;
    (void)pspec;

    if (chooser->catalog == NULL)
        return;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    /* "Other" is the one entry past the end of the catalogue. */
    gtk_widget_set_visible(chooser->entry,
                           selected >= json_array_get_length(images));

    if (selected < json_array_get_length(images)) {
        JsonObject *image = json_array_get_object_element(images, selected);
        const gchar *note = clawt_json_string(image, "note", NULL);
        const gchar *reference = clawt_json_string(image, "reference", "");

        /*
         * The subtitle carries the full reference, because a label like
         * "Fedora 44" does not say which registry it comes from -- and
         * that is exactly what goes wrong when two machines disagree
         * about an unqualified name.
         */
        if (note != NULL) {
            g_autofree gchar *subtitle =
                g_strdup_printf("%s -- %s", reference, note);

            adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                        subtitle);
        } else {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                        reference);
        }
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                    "any reference podman can pull");
    }
}

/*
 * Adds the image rows to a group, selecting @want if it is on the list
 * and putting it in the entry if it is not.
 */
static void
image_chooser_build(ImageChooser *chooser, ClawtWindow *window,
                    GtkWidget *group, const gchar *want)
{
    GtkStringList *labels = gtk_string_list_new(NULL);
    JsonArray *images = NULL;
    const gchar *fallback = NULL;
    gboolean matched = FALSE;
    guint chosen = 0;
    guint i;

    chooser->window = window;

    chooser->row = adw_combo_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->row),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->row), "Image");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->row);

    chooser->entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->entry),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->entry),
                                  "Image reference");
    gtk_widget_set_visible(chooser->entry, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->entry);

    chooser->catalog = clawt_window_request(window, "image.list", NULL);

    if (chooser->catalog != NULL) {
        JsonObject *root = json_node_get_object(chooser->catalog);

        images = json_object_get_array_member(root, "images");
        fallback = clawt_json_string(root, "default", NULL);

        for (i = 0; i < json_array_get_length(images); i++) {
            JsonObject *image = json_array_get_object_element(images, i);
            const gchar *reference = clawt_json_string(image, "reference", "");
            /*
             * The label alone, not the group.  AdwComboRow takes a flat
             * GListModel and sizes its popup to the row, so a
             * "Enterprise Linux / CentOS Stream 10" prefix truncates to
             * the part every entry in the group shares -- which is the
             * half that carries no information. The catalogue's labels
             * are already distinct, the order groups them, and the full
             * reference is on the subtitle once one is picked.
             */
            gtk_string_list_append(labels,
                                   clawt_json_string(image, "label",
                                                     reference));

            if (want != NULL && g_strcmp0(reference, want) == 0) {
                chosen = i;
                matched = TRUE;
            }
        }
    }

    gtk_string_list_append(labels, "Other\342\200\246");
    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->row),
                            G_LIST_MODEL(labels));

    /*
     * A reference that is not on the list is not an error: it goes into
     * the entry, selected as "Other", so an agent configured by hand
     * keeps what it was given instead of being silently retargeted at
     * whatever happened to be first.
     */
    if (want != NULL && *want != '\0' && !matched) {
        gtk_editable_set_text(GTK_EDITABLE(chooser->entry), want);
        chosen = (images != NULL) ? json_array_get_length(images) : 0;
    } else if (want == NULL && fallback != NULL && images != NULL) {
        for (i = 0; i < json_array_get_length(images); i++) {
            if (g_strcmp0(clawt_json_string(
                    json_array_get_object_element(images, i),
                    "reference", ""), fallback) == 0) {
                chosen = i;
                break;
            }
        }
    }

    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->row), chosen);

    g_signal_connect(chooser->row, "notify::selected",
                     G_CALLBACK(on_image_changed), chooser);
    on_image_changed(NULL, NULL, chooser);
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
    model_chooser_build_full(chooser, window, group, want_provider,
                             want_model, "agent");
}

/*
 * @require names the flag a provider must carry to be offered, or is
 * %NULL to offer all of them.
 *
 * Two views ask this for two different jobs and want two different
 * lists. "agent" is what libreclaw can drive: its provider table is
 * command-line only, and it rewrites anything it does not recognise to
 * claude-code, so offering the HTTP providers as an agent's backend let
 * a person pick OpenAI and silently get Claude Code with "gpt-4o" in
 * the model field. "tools" is what can be given tool definitions, which
 * the designer is built entirely out of and ai-glib's CLI clients drop;
 * offering those let a person fill in the whole form, press Design, and
 * only then be told the provider cannot do this.
 *
 * The list is filtered rather than the entries greyed out because every
 * remaining index is used to look a provider back up, and a hole in the
 * middle of that array is a bug waiting for somebody to add a provider.
 *
 * @want_provider survives the filter whatever it is. An agent already
 * configured for a provider this view would not offer must still show
 * the one it has -- dropping it would leave the combo pointing at the
 * first entry, and saving the page would then change the agent's
 * provider to something nobody chose.
 */
static void
model_chooser_build_full(ModelChooser *chooser, ClawtWindow *window,
                         GtkWidget *group, const gchar *want_provider,
                         const gchar *want_model, const gchar *require)
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

    /*
     * refresh: the provider's own list where we can get it.  The
     * hardcoded table goes stale, and a person picking a model should
     * see what the provider actually runs today. The daemon falls back
     * to the table when there is no key or no network, so this never
     * produces an empty chooser.
     */
    chooser->catalog = clawt_window_request(
        window, "model.list", clawt_build_payload("refresh", "true", NULL));

    if (chooser->catalog != NULL && require != NULL) {
        JsonArray *all = json_object_get_array_member(
            json_node_get_object(chooser->catalog), "providers");
        g_autoptr(JsonArray) kept = json_array_new();

        for (i = 0; i < json_array_get_length(all); i++) {
            JsonObject *provider = json_array_get_object_element(all, i);

            /*
             * A missing member means an older daemon that does not
             * report this flag -- keep the provider rather than drop
             * it. Filtering on absence emptied the whole list and left
             * two blank rows, which is a worse failure than offering one
             * provider that will refuse.
             */
            if (!json_object_has_member(provider, require) ||
                json_object_get_boolean_member(provider, require) ||
                (want_provider != NULL &&
                 g_strcmp0(clawt_json_string(provider, "id", ""),
                           want_provider) == 0))
                json_array_add_object_element(kept,
                                              json_object_ref(provider));
        }

        json_object_set_array_member(json_node_get_object(chooser->catalog),
                                     "providers",
                                     json_array_ref(kept));
    }

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
    g_autofree gchar *image = NULL;
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

    /*
     * The image is only sent for a container.  Setting it on a host or
     * vm agent would write a key that backend never reads, and it then
     * looks like a setting that is being ignored.
     */
    if (g_strcmp0(computers[MIN(selected, 3)], "container") == 0)
        image = image_chooser_value(&dialog->image);

    reply = clawt_window_request(
        self, "agent.create",
        clawt_build_payload(
            "id", agent_id,
            "name", answer_of(dialog->name_entry),
            "description", answer_of(dialog->description_entry),
            "provider", provider != NULL
                        ? clawt_json_string(provider, "id", NULL) : NULL,
            "model", model,
            "computer", computers[MIN(selected, 3)],
            "image", image,
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
    const gchar *draft = g_object_get_data(G_OBJECT(dialog), "draft");
    g_autoptr(JsonNode) reply = NULL;

    if (draft == NULL)
        return;

    /*
     * Cancelling drops the draft rather than leaving it on the daemon.
     * It holds a whole designer, and a person who says no is done with
     * it.
     */
    if (g_strcmp0(response, "create") != 0) {
        g_autoptr(JsonNode) discarded = clawt_window_request(
            self, "design.discard", clawt_build_payload("draft", draft, NULL));

        (void)discarded;
        return;
    }

    /*
     * Commits the draft that was reviewed.  Re-running the design would
     * be a fresh conversation producing something else, which makes the
     * preview a demonstration rather than a decision.
     */
    reply = clawt_window_request(self, "design.commit",
                                 clawt_build_payload("draft", draft, NULL));

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

/* An answer, or NULL when the row was left empty. */
static const gchar *
answer_of(GtkWidget *row)
{
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));

    return (text != NULL && *text != '\0') ? text : NULL;
}

static void
on_design_with_ai(GtkButton *button, gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *designer_model = NULL;
    const gchar *purpose;

    purpose = answer_of(dialog->describe_entry);

    if (purpose == NULL) {
        clawt_window_toast(self, "Say what the agent should do first.");
        return;
    }

    /*
     * Disabled while the model works.  A design takes tens of seconds
     * and a second click starts a second one, which is billed and
     * discarded.
     */
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    gtk_button_set_label(GTK_BUTTON(button), "Designing...");

    designer_model = chooser_model(&dialog->designer);

    reply = clawt_window_request(
        self, "design.agent",
        clawt_build_payload(
            "purpose", purpose,
            "boundaries", answer_of(dialog->boundaries_entry),
            "needs", answer_of(dialog->needs_entry),
            "personality", answer_of(dialog->personality_entry),
            "projects", answer_of(dialog->projects_entry),
            "notes", answer_of(dialog->notes_entry),
            "provider", chooser_provider_id(&dialog->designer),
            "model", designer_model,
            NULL));

    gtk_button_set_label(GTK_BUTTON(button), "Design it");
    gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);

    if (reply == NULL)
        return;

    /*
     * The design is shown before anything is written.  A model's
     * proposal becoming an agent without a person reading it first is
     * exactly the kind of convenience nobody asks for twice.
     */
    {
        JsonObject *result = clawt_payload_of(reply);
        JsonArray *files = json_object_has_member(result, "files")
                           ? json_object_get_array_member(result, "files")
                           : NULL;
        g_autoptr(GString) body = g_string_new(
            clawt_json_string(result, "yaml", ""));
        AdwAlertDialog *preview;
        guint i;

        /*
         * The org files are listed rather than shown in full: they run
         * to hundreds of lines each, and a dialog nobody can read is not
         * a review.  They are on disk after Create, and
         * `clawtilla agent edit` opens them.
         */
        if (files != NULL && json_array_get_length(files) > 0) {
            g_string_append(body, "\n\nIt also wrote:\n");

            for (i = 0; i < json_array_get_length(files); i++) {
                JsonObject *file = json_array_get_object_element(files, i);
                const gchar *content = clawt_json_string(file, "content", "");
                gsize lines = 0;
                const gchar *p;

                for (p = content; *p != '\0'; p++) {
                    if (*p == '\n')
                        lines++;
                }

                g_string_append_printf(body, "  %s  (%" G_GSIZE_FORMAT
                                             " lines)\n",
                                       clawt_json_string(file, "name", "?"),
                                       lines);
            }

            g_string_append(body,
                            "\nEdit them after with: clawtilla agent edit ");
            g_string_append(body, clawt_json_string(result, "id", ""));
        }

        preview = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Proposed agent", NULL));

        adw_alert_dialog_set_body(preview, body->str);
        adw_alert_dialog_add_response(preview, "cancel", "Cancel");
        adw_alert_dialog_add_response(preview, "create", "Create");
        adw_alert_dialog_set_response_appearance(preview, "create",
                                                 ADW_RESPONSE_SUGGESTED);

        g_object_set_data_full(
            G_OBJECT(preview), "draft",
            g_strdup(clawt_json_string(result, "draft", NULL)), g_free);
        g_signal_connect(preview, "response",
                         G_CALLBACK(on_preview_response), dialog);

        adw_dialog_present(ADW_DIALOG(preview), GTK_WIDGET(self));
    }
}

/*
 * Shows the image rows only for a container.
 *
 * Sensitivity would be the softer choice, but a greyed-out Image row on
 * a host agent still reads as "this agent has an image, you just cannot
 * change it", which is not true.
 */
static void
on_computer_type_changed(GObject *object, GParamSpec *pspec,
                         gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    guint selected;
    gboolean is_container;

    (void)object;
    (void)pspec;

    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));
    is_container = (selected == 2);   /* none, host, container, vm */

    gtk_widget_set_visible(dialog->image.row, is_container);

    if (!is_container)
        gtk_widget_set_visible(dialog->image.entry, FALSE);
    else
        on_image_changed(NULL, NULL, &dialog->image);
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
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(dialog->id_entry),
                                        FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->id_entry),
                                  "Id");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->id_entry);

    /*
     * Name and description, which only the inspector offered before --
     * so every agent was created with its id as its name and no
     * description, and the description is what other agents read when
     * deciding who to delegate to.
     */
    dialog->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->name_entry),
                                  "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->name_entry);

    dialog->description_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->description_entry), FALSE);
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->description_entry),
        "What it is for");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->description_entry);

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

    /*
     * The image, shown only while "container" is the chosen computer.
     * The other backends do not read it, and a row that quietly does
     * nothing is worse than no row.
     */
    image_chooser_build(&dialog->image, self, manual, NULL);
    g_signal_connect(dialog->computer_row, "notify::selected",
                     G_CALLBACK(on_computer_type_changed), dialog);
    on_computer_type_changed(NULL, NULL, dialog);

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
        "Answer what you can and clawtilla drafts the configuration and "
        "the agent's org files. Only the first is required; a blank "
        "answer becomes a heading to fill in rather than an invention. "
        "You see everything before anything is created.");

    /*
     * Named questions rather than one box.
     *
     * "Describe what you want" asked the person to write a paragraph
     * that happened to contain everything the model needed, and a
     * paragraph that leaves out the boundaries produces an agent with
     * none. Each question asks for one thing once.
     */
    {
        static const struct {
            const gchar *title;
            gsize        offset;
        } questions[] = {
            { "What should it do?",
              G_STRUCT_OFFSET(NewAgentDialog, describe_entry) },
            { "What should it never do?",
              G_STRUCT_OFFSET(NewAgentDialog, boundaries_entry) },
            { "What does it need to work on?",
              G_STRUCT_OFFSET(NewAgentDialog, needs_entry) },
            { "How should it come across?",
              G_STRUCT_OFFSET(NewAgentDialog, personality_entry) },
            { "What is it working on, and where?",
              G_STRUCT_OFFSET(NewAgentDialog, projects_entry) },
            { "Anything else it should know?",
              G_STRUCT_OFFSET(NewAgentDialog, notes_entry) },
            { NULL, 0 }
        };
        gsize i;

        for (i = 0; questions[i].title != NULL; i++) {
            GtkWidget *row = adw_entry_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                                FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          questions[i].title);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(ai), row);

            G_STRUCT_MEMBER(GtkWidget *, dialog, questions[i].offset) = row;
        }
    }

    /*
     * Which model does the designing -- separate from the model the
     * agent will run on. There is no reason for them to be the same, and
     * a person will often want their best model to draft an agent that
     * then runs on a cheap one.
     */
    model_chooser_build_full(&dialog->designer, self, ai, NULL, NULL,
                             "tools");
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->designer.provider_row),
        "Designed by");
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->designer.model_row),
        "Designer's model");

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
    /*
     * A header bar, for the close button.
     *
     * An AdwDialog with a bare child has no visible way out: Escape
     * works, but a modal whose only exit is a keystroke nobody
     * advertised is one people report as stuck -- and they were right
     * to.
     */
    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *cancel = gtk_button_new_with_label("Cancel");

        g_signal_connect_swapped(cancel, "clicked",
                                 G_CALLBACK(adw_dialog_close), window);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);

        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

        adw_dialog_set_child(window, toolbar);
    }

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

    /*
     * The activity line.  A chat window that shows nothing between the
     * question and the answer is indistinguishable from a broken one,
     * and an agent turn can easily run for minutes.
     */
    self->streaming = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(self->streaming), "dim-label");
    gtk_label_set_wrap(self->streaming, TRUE);
    gtk_label_set_xalign(self->streaming, 0.0f);
    gtk_label_set_ellipsize(self->streaming, PANGO_ELLIPSIZE_END);

    self->activity_spinner = GTK_SPINNER(gtk_spinner_new());
    self->activity_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->activity_bar),
                   GTK_WIDGET(self->activity_spinner));
    gtk_box_append(GTK_BOX(self->activity_bar), GTK_WIDGET(self->streaming));
    gtk_widget_set_margin_start(self->activity_bar, 12);
    gtk_widget_set_margin_end(self->activity_bar, 12);
    gtk_widget_set_visible(self->activity_bar, FALSE);

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
    gtk_box_append(GTK_BOX(box), self->activity_bar);
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
    g_signal_connect(self->sidebar, "row-selected",
                     G_CALLBACK(on_row_selected), self);
    build_agent_menu(self);

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

    /*
     * A popover parented to a widget outlives that widget's own dispose
     * and GTK complains about it at teardown; unparenting is the
     * documented way to hand it back.
     */
    g_clear_pointer(&self->agent_menu, gtk_widget_unparent);
    g_clear_object(&self->agent_actions);

    G_OBJECT_CLASS(clawt_window_parent_class)->dispose(object);
}

static void
clawt_window_finalize(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    g_free(self->selected_agent);
    g_free(self->inspector_computer);

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
