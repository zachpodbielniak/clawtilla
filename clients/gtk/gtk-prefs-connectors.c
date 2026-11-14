/*
 * gtk-prefs-connectors.c - Settings: connectors
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The MCP connectors an agent can be granted, and the flows that
 * authorise one.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Connectors ──────────────────────────────────────────────────── */

/*
 * Authorising takes as long as a person takes.
 *
 * The request iterates the window's own main context while it waits, so
 * the dialog showing the code stays painted and the rest of the window
 * keeps working -- but the default two-minute timeout would give up
 * while somebody was still unlocking their phone.
 */
static JsonNode *
connector_request_slow(ClawtWindow *self, const gchar *kind, JsonNode *payload,
                       gint seconds)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    reply = clawt_client_request_full(self->client, kind,
                                      payload, seconds, &error);

    if (reply == NULL) {
        clawt_window_toast(self, error->message);
        return NULL;
    }

    return reply;
}

/*
 * Shows the code, then waits.
 *
 * The code is the whole of the interaction for a device flow, so it gets
 * the dialog's heading at full size rather than being a line of body
 * text: it is about to be read off this screen and typed into another
 * device, quite possibly across a room.
 */
static void
connector_run_flow(ClawtWindow *self, const gchar *name)
{
    g_autoptr(JsonNode) begun = NULL;
    g_autoptr(JsonNode) done = NULL;
    JsonObject *root;
    AdwAlertDialog *dialog;
    const gchar *method;
    const gchar *flow;
    g_autofree gchar *body = NULL;

    begun = clawt_window_request(self, "connector.begin",
                                 clawt_build_payload("name", name, NULL));

    if (begun == NULL)
        return;

    root = json_node_get_object(begun);
    method = json_object_get_string_member_with_default(root, "method", "");
    flow = json_object_get_string_member_with_default(root, "flow", NULL);

    if (g_strcmp0(method, "device") == 0) {
        const gchar *uri =
            json_object_get_string_member_with_default(root,
                                                       "verification_uri",
                                                       "the provider's page");
        const gchar *code =
            json_object_get_string_member_with_default(root, "user_code", "?");

        body = g_strdup_printf("Enter this code at\n%s", uri);
        dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(code, body));
    } else {
        const gchar *url =
            json_object_get_string_member_with_default(root, "authorize_url",
                                                       NULL);

        body = g_strdup_printf("Open this to approve:\n\n%s",
                               url != NULL ? url : "(no URL)");
        dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Waiting for you",
                                                        body));

        if (url != NULL) {
            g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);

            /*
             * Opened for them, and still shown above: a browser that
             * declines to open -- or opens somewhere they are not
             * looking -- would otherwise leave a dialog saying "approve
             * this" with nothing to approve.
             */
            gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL,
                                    NULL);
        }
    }

    adw_alert_dialog_add_response(dialog, "close", "Close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));

    /*
     * Fifteen minutes, which is how long a device code is usually good
     * for. Giving up sooner would report a failure for a flow that was
     * about to succeed -- and leave the daemon holding a credential
     * nobody had been told about.
     */
    done = connector_request_slow(self, "connector.await",
                                  clawt_build_payload("flow", flow, NULL),
                                  900);

    adw_dialog_close(ADW_DIALOG(dialog));

    if (done != NULL)
        clawt_window_toast(self, "Connected.");

    clawt_gtk_refresh_settings_connectors(self);
}

typedef struct {
    ClawtWindow *window;
    gchar       *name;
    AdwDialog   *dialog;
    GtkWidget   *entry;
} ConnectorAction;

static void
connector_action_free(gpointer data, GClosure *closure)
{
    ConnectorAction *action = data;

    g_free(action->name);
    g_free(action);
}

static void
on_connector_connect(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;

    adw_dialog_close(action->dialog);
    connector_run_flow(action->window, action->name);
}

static void
on_connector_key_entered(AdwAlertDialog *dialog, const gchar *response,
                         gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *key;

    if (g_strcmp0(response, "save") != 0)
        return;

    key = gtk_editable_get_text(GTK_EDITABLE(action->entry));

    if (key == NULL || *key == '\0')
        return;

    reply = clawt_window_request(action->window, "connector.key",
                                 clawt_build_payload("name", action->name,
                                                      "key", key, NULL));

    /*
     * Cleared whether or not it was accepted.  A rejected token is still
     * a live one, and leaving it in a widget puts it in the accessibility
     * tree and in whatever the toolkit last rendered.
     */
    gtk_editable_set_text(GTK_EDITABLE(action->entry), "");

    if (reply != NULL)
        clawt_window_toast(action->window, "Token stored.");

    clawt_gtk_refresh_settings_connectors(action->window);
}

static void
on_connector_key(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    AdwAlertDialog *ask;
    GtkWidget *entry;

    adw_dialog_close(action->dialog);

    ask = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Paste a token",
        "A personal access token works as well as an authorization, and "
        "needs no application registered with the provider. It is stored "
        "0600 and never reaches the agent."));

    entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ask, entry);

    adw_alert_dialog_add_response(ask, "cancel", "Cancel");
    adw_alert_dialog_add_response(ask, "save", "Store");
    adw_alert_dialog_set_response_appearance(ask, "save",
                                             ADW_RESPONSE_SUGGESTED);

    action->entry = entry;

    g_signal_connect_data(ask, "response",
                          G_CALLBACK(on_connector_key_entered), action,
                          connector_action_free, 0);

    adw_dialog_present(ADW_DIALOG(ask), GTK_WIDGET(action->window));
}

static void
on_connector_refresh(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    reply = clawt_window_request(action->window, "connector.refresh",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL)
        clawt_window_toast(action->window, "Renewed.");

    clawt_gtk_refresh_settings_connectors(action->window);
}

static void
on_connector_revoke(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    reply = clawt_window_request(action->window, "connector.revoke",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL) {
        JsonObject *root = json_node_get_object(reply);

        /*
         * Says which of the two things happened.  Somebody who believes
         * a token is dead and finds it working months later has been
         * misled by this message, and the remaining step is on a page
         * only they can reach.
         */
        if (json_object_get_boolean_member_with_default(root, "told_provider",
                                                         FALSE))
            clawt_window_toast(action->window,
                               "Revoked, and the provider was told.");
        else
            clawt_window_toast(action->window,
                               "Forgotten here -- withdraw it in the "
                               "provider's settings to finish.");
    }

    clawt_gtk_refresh_settings_connectors(action->window);
}

static void
on_connector_remove(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) revoked = NULL;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    /*
     * The credential first. Removing the integration and leaving the
     * token behind would strand a live credential under a name nothing
     * refers to any more.
     */
    revoked = clawt_window_request(action->window, "connector.revoke",
                                   clawt_build_payload("name", action->name,
                                                        NULL));

    reply = clawt_window_request(action->window, "integration.remove",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL)
        clawt_window_toast(action->window, "Removed.");

    clawt_gtk_refresh_settings_connectors(action->window);
}

typedef struct {
    ClawtWindow *window;
    gchar       *name;
} DirectoryChoice;

static void
directory_choice_free(gpointer data, GClosure *closure)
{
    DirectoryChoice *choice = data;

    g_free(choice->name);
    g_free(choice);
}

/*
 * Saved the moment it changes, through the same integration.update every
 * other connector field goes through.  It reaches an agent the next time
 * its relay starts, which is the next time its CLI starts a session.
 */
static void
on_directory_chosen(AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
    DirectoryChoice *choice = user_data;
    g_autoptr(JsonNode) reply = NULL;
    guint selected = adw_combo_row_get_selected(row);

    if (selected >= clawt_tool_directory_mode_count())
        return;

    reply = clawt_window_request(
        choice->window, "integration.update",
        clawt_build_payload("name", choice->name,
                            "directory",
                            clawt_tool_directory_mode_nth_nick(selected),
                            NULL));

    if (reply != NULL)
        clawt_gtk_refresh_settings_connectors(choice->window);
}

/*
 * "Present as a directory: Auto / Always / Never", walked from the
 * library's enumeration so a mode added there is offered here at once.
 */
static GtkWidget *
build_directory_choice(ClawtWindow *self, AdwDialog *dialog,
                       const gchar *name, const gchar *current)
{
    GtkWidget *list = gtk_list_box_new();
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *labels = gtk_string_list_new(NULL);
    DirectoryChoice *choice = g_new0(DirectoryChoice, 1);
    guint selected = 0;
    guint i;

    for (i = 0; i < clawt_tool_directory_mode_count(); i++) {
        gtk_string_list_append(labels, clawt_tool_directory_mode_nth_label(i));

        if (g_strcmp0(current, clawt_tool_directory_mode_nth_nick(i)) == 0)
            selected = i;
    }

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  "Present as a directory");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(row),
        "Three tools that search, describe and run the rest, instead of "
        "every tool in every prompt");
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(labels));
    g_object_unref(labels);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);

    choice->window = self;
    choice->name = g_strdup(name);
    g_signal_connect_data(row, "notify::selected",
                          G_CALLBACK(on_directory_chosen), choice,
                          directory_choice_free, 0);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    (void)dialog;

    return list;
}

/*
 * Plain buttons in a box rather than a list.
 *
 * A GtkListBox selects a row when it takes focus and a popover takes
 * focus as it opens, so a menu built from ::row-selected runs its first
 * entry before anybody has chosen anything -- and the first entry here
 * would be Revoke.
 */
static void
on_connector_activated(GtkListBox *list, GtkListBoxRow *row,
                       gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(row), "clawt-connector");
    const gchar *provider = g_object_get_data(G_OBJECT(row),
                                              "clawt-connector-provider");
    AdwDialog *dialog;
    GtkWidget *toolbar;
    GtkWidget *box;
    GtkWidget *button;
    ConnectorAction *action;
    struct {
        const gchar *label;
        const gchar *css;
        GCallback    handler;
    } actions[] = {
        { "Authorize again", NULL,      G_CALLBACK(on_connector_connect) },
        { "Paste a token",   NULL,      G_CALLBACK(on_connector_key) },
        { "Renew now",       NULL,      G_CALLBACK(on_connector_refresh) },
        { "Revoke",          "destructive-action",
          G_CALLBACK(on_connector_revoke) },
        { "Remove",          "destructive-action",
          G_CALLBACK(on_connector_remove) }
    };
    gsize i;

    if (name == NULL)
        return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, name);
    adw_dialog_set_content_width(dialog, 380);

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    if (provider != NULL) {
        GtkWidget *label = gtk_label_new(provider);

        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(box), label);
    }

    gtk_box_append(GTK_BOX(box),
                   build_directory_choice(self, dialog, name,
                                          g_object_get_data(
                                              G_OBJECT(row),
                                              "clawt-connector-directory")));

    for (i = 0; i < G_N_ELEMENTS(actions); i++) {
        action = g_new0(ConnectorAction, 1);
        action->window = self;
        action->name = g_strdup(name);
        action->dialog = dialog;

        button = gtk_button_new_with_label(actions[i].label);

        if (actions[i].css != NULL)
            gtk_widget_add_css_class(button, actions[i].css);

        g_signal_connect_data(button, "clicked", actions[i].handler, action,
                              connector_action_free, 0);
        gtk_box_append(GTK_BOX(box), button);
    }

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_dialog_set_child(dialog, toolbar);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/*
 * The registry is imported by id, not by name -- an entry the daemon
 * added since the last time this page opened is invisible until the
 * catalogue is asked for again, which "Add a connector" does on its own
 * every time it opens.  So this button's only job is to say what
 * happened, in a fleet's own words: how many, or why not (most often
 * that connectors.registry_enabled is still off).
 */
static void
on_registry_refresh(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "connector.registry_refresh", NULL);

    if (reply != NULL) {
        gint64 imported = json_object_get_int_member_with_default(
            json_node_get_object(reply), "imported", 0);
        g_autofree gchar *message = g_strdup_printf(
            "Imported %" G_GINT64_FORMAT " connector%s from the registry.",
            imported, imported == 1 ? "" : "s");

        clawt_window_toast(self, message);
    }

    (void)button;
}

/* Fills the threshold rows without that counting as an edit. */
static void
sync_directory_thresholds(ClawtWindow *self, gint64 min_tools,
                          gint64 min_chars)
{
    if (self->settings_directory_min_tools == NULL)
        return;

    self->settings_directory_syncing = TRUE;
    adw_spin_row_set_value(ADW_SPIN_ROW(self->settings_directory_min_tools),
                           (gdouble)min_tools);
    adw_spin_row_set_value(ADW_SPIN_ROW(self->settings_directory_min_chars),
                           (gdouble)min_chars);
    self->settings_directory_syncing = FALSE;
}

/*
 * connectors.directory_min_tools and _min_chars, written through
 * connector.settings, which checks both before saving either.
 */
static void
on_directory_threshold_changed(AdwSpinRow *row, GParamSpec *pspec,
                               gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = NULL;

    if (self->settings_directory_syncing)
        return;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "directory_min_tools");
    json_builder_add_int_value(
        builder, (gint64)adw_spin_row_get_value(
                     ADW_SPIN_ROW(self->settings_directory_min_tools)));
    json_builder_set_member_name(builder, "directory_min_chars");
    json_builder_add_int_value(
        builder, (gint64)adw_spin_row_get_value(
                     ADW_SPIN_ROW(self->settings_directory_min_chars)));
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "connector.settings",
                                 json_builder_get_root(builder));

    if (reply != NULL)
        clawt_gtk_refresh_settings_connectors(self);

    (void)row;
}

void
clawt_gtk_refresh_settings_connectors(ClawtWindow *self)
{
    if (self->settings_connectors == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_CONNECTORS))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *connectors;
        gint64 min_tools;
        gint64 min_chars;
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_connectors));

        reply = clawt_window_request(self, "connector.list", NULL);

        if (reply == NULL)
            continue;

        connectors = json_object_get_array_member(json_node_get_object(reply),
                                                  "connectors");
        min_tools = json_object_get_int_member_with_default(
            json_node_get_object(reply), "directory_min_tools", 40);
        min_chars = json_object_get_int_member_with_default(
            json_node_get_object(reply), "directory_min_chars", 100000);
        sync_directory_thresholds(self, min_tools, min_chars);

        if (json_array_get_length(connectors) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "No connected accounts");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "Connect one to give agents a service's tools without "
                "giving them the credential");
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_connectors), row);
            continue;
        }

        for (i = 0; i < json_array_get_length(connectors); i++) {
            JsonObject *entry =
                json_array_get_object_element(connectors, i);
            const gchar *name =
                json_object_get_string_member_with_default(entry, "name", "?");
            const gchar *provider =
                json_object_get_string_member_with_default(entry, "provider",
                                                           "");
            const gchar *account =
                json_object_get_string_member_with_default(entry, "account",
                                                           NULL);
            gboolean connected =
                json_object_get_boolean_member_with_default(entry,
                                                            "connected",
                                                            FALSE);
            gint64 expires =
                json_object_get_int_member_with_default(entry, "expires_at",
                                                        0);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            /*
             * Seeded with the unauthorised icon so -Wmaybe-uninitialized
             * has nothing to say, while the switch below keeps its
             * missing `default:` -- which is what makes -Wswitch name a
             * ClawtConnectorState nobody classified.
             */
            const gchar *icon = "channel-insecure-symbolic";

            const gchar *directory =
                json_object_get_string_member_with_default(entry, "directory",
                                                           "auto");
            g_autofree gchar *tools_line = clawt_mcp_directory_status_label(
                directory,
                json_object_get_int_member_with_default(entry, "tool_count",
                                                        -1),
                json_object_get_int_member_with_default(entry, "tool_chars",
                                                        0),
                min_tools, min_chars);

            /*
             * How many tools, and whether agents see them as a directory,
             * on the row itself: the reason somebody opens this page
             * after an agent's context filled up with one server.
             */
            if (account != NULL && *account != '\0')
                subtitle = g_strdup_printf("%s -- %s\n%s", provider, account,
                                           tools_line);
            else
                subtitle = g_strdup_printf("%s\n%s", provider, tools_line);

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            /*
             * Three states, not two.  An expired credential that can
             * renew itself is working normally, and showing it as broken
             * would light up the list every hour and teach somebody to
             * ignore the one that genuinely is.
             *
             * Through the library, because the web client draws this too
             * and had never once drawn it right -- it read a member the
             * daemon does not send and reported every connector as
             * unauthorised.  A rule two clients apply separately is a
             * rule they disagree about.
             */
            switch (clawt_connector_state(
                        connected, expires,
                        json_object_get_boolean_member_with_default(
                            entry, "renewable", FALSE), 0)) {
            case CLAWT_CONNECTOR_UNAUTHORISED:
                icon = "channel-insecure-symbolic";
                break;

            case CLAWT_CONNECTOR_EXPIRED:
                icon = "dialog-warning-symbolic";
                break;

            case CLAWT_CONNECTOR_READY:
                icon = "emblem-ok-symbolic";
                break;
            }

            adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                      gtk_image_new_from_icon_name(icon));

            g_object_set_data_full(G_OBJECT(row), "clawt-connector",
                                   g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(row), "clawt-connector-provider",
                                   g_strdup(provider), g_free);
            g_object_set_data_full(G_OBJECT(row), "clawt-connector-directory",
                                   g_strdup(directory), g_free);

            /*
             * This used to pass the row as its own activatable widget,
             * which made it activatable and crashed the client on the
             * first click. See row_opens_something().
             */
            clawt_gtk_row_opens_something(row);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_connectors), row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_CONNECTORS));
}

typedef struct {
    ClawtWindow   *window;
    AdwDialog     *dialog;
    GtkWidget     *provider;
    GtkWidget     *name;
    GtkWidget     *account;
    GtkWidget     *client_id;
    GtkWidget     *instance;
    GtkWidget     *scope;
    GtkWidget     *agents;
    GtkStringList *provider_ids;
} AddConnector;

static void
add_connector_free(gpointer data, GClosure *closure)
{
    AddConnector *add = data;

    g_clear_object(&add->provider_ids);
    g_free(add);
}

static void
on_create_connector(GtkButton *button, gpointer user_data)
{
    AddConnector *add = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(add->name));
    const gchar *client_id =
        gtk_editable_get_text(GTK_EDITABLE(add->client_id));
    const gchar *instance = gtk_editable_get_text(GTK_EDITABLE(add->instance));
    const gchar *account = gtk_editable_get_text(GTK_EDITABLE(add->account));
    const gchar *agents = gtk_editable_get_text(GTK_EDITABLE(add->agents));
    const gchar *provider;
    guint selected;
    g_autofree gchar *chosen = g_strdup(name);

    if (name == NULL || *name == '\0') {
        clawt_window_toast(add->window, "It needs a name.");
        return;
    }

    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(add->provider));
    provider = gtk_string_list_get_string(add->provider_ids, selected);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "connector");
    json_builder_set_member_name(builder, "provider");
    json_builder_add_string_value(builder, provider);
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(
        builder, adw_combo_row_get_selected(ADW_COMBO_ROW(add->scope)) == 0
                 ? "all" : "selected");

    if (account != NULL && *account != '\0') {
        json_builder_set_member_name(builder, "account");
        json_builder_add_string_value(builder, account);
    }

    if (client_id != NULL && *client_id != '\0') {
        json_builder_set_member_name(builder, "client_id");
        json_builder_add_string_value(builder, client_id);
    }

    if (instance != NULL && *instance != '\0') {
        json_builder_set_member_name(builder, "instance");
        json_builder_add_string_value(builder, instance);
    }

    if (agents != NULL && *agents != '\0') {
        g_auto(GStrv) ids = g_strsplit(agents, ",", -1);
        gsize i;

        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; ids[i] != NULL; i++)
            json_builder_add_string_value(builder, g_strstrip(ids[i]));

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(add->window, "integration.add",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    adw_dialog_close(add->dialog);
    clawt_gtk_refresh_settings_connectors(add->window);

    /*
     * Straight into the flow only when there is an application to run
     * it with. Without a client id the provider has nothing to identify
     * the request, and starting something that cannot succeed is worse
     * than leaving the person on a list where the next step is visible.
     */
    if (client_id != NULL && *client_id != '\0')
        connector_run_flow(add->window, chosen);
    else
        clawt_window_toast(add->window,
                           "Added. Open it to paste a token, or set a "
                           "client id to authorize.");
}

static void
on_add_connector(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AddConnector *add = g_new0(AddConnector, 1);
    g_autoptr(JsonNode) reply = NULL;
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkStringList *labels = gtk_string_list_new(NULL);
    GtkWidget *create;
    GtkStringList *scopes;
    JsonArray *connectors;
    guint i;

    (void)button;

    add->window = self;
    add->dialog = dialog;
    add->provider_ids = gtk_string_list_new(NULL);

    reply = clawt_window_request(self, "connector.catalog", NULL);

    if (reply == NULL) {
        g_object_unref(labels);
        add_connector_free(add, NULL);
        adw_dialog_close(dialog);
        return;
    }

    connectors = json_object_get_array_member(json_node_get_object(reply),
                                              "connectors");

    for (i = 0; i < json_array_get_length(connectors); i++) {
        JsonObject *entry = json_array_get_object_element(connectors, i);
        const gchar *id =
            json_object_get_string_member_with_default(entry, "id", "?");
        const gchar *label =
            json_object_get_string_member_with_default(entry, "name", id);
        g_autofree gchar *shown = NULL;

        /*
         * The auth kind is on the label because it decides what happens
         * next: a device connector needs a client id, an api_key one
         * needs a token pasted, and being told after pressing Add is too
         * late to be useful.
         */
        shown = g_strdup_printf("%s (%s)", label,
                                json_object_get_string_member_with_default(
                                    entry, "auth", ""));

        gtk_string_list_append(labels, shown);
        gtk_string_list_append(add->provider_ids, id);
    }

    adw_dialog_set_title(dialog, "Add a connector");
    adw_dialog_set_content_width(dialog, 460);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "clawtilla holds the credential and hands it to the tool server. "
        "Agents get the tools; they never get the token.");

    add->provider = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->provider),
                                  "Service");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->provider),
                            G_LIST_MODEL(labels));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->provider);

    add->name = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->name), "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->name);

    add->account = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->account),
                                  "Account (work, personal)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->account);

    add->client_id = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->client_id),
                                  "Client ID (leave empty to paste a token)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->client_id);

    add->instance = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->instance),
                                  "Instance (if you host it yourself)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->instance);

    scopes = gtk_string_list_new(NULL);
    gtk_string_list_append(scopes, "Every agent");
    gtk_string_list_append(scopes, "Only the agents I name");

    add->scope = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->scope), "Who gets it");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->scope), G_LIST_MODEL(scopes));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->scope);

    add->agents = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->agents),
                                  "Agents, comma separated");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->agents);

    create = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect_data(create, "clicked",
                          G_CALLBACK(on_create_connector), add,
                          add_connector_free, 0);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

GtkWidget *
clawt_gtk_build_connectors_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;
    GtkWidget *refresh_registry;
    GtkWidget *header_suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Connectors");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "changes-prevent-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Connected accounts");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "An account clawtilla holds the credential for. Agents get its "
        "tools; the token stays here, is renewed here, and can be "
        "withdrawn here.");

    refresh_registry = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_registry,
                                "Import the open MCP registry now");
    gtk_widget_add_css_class(refresh_registry, "flat");
    g_signal_connect(refresh_registry, "clicked",
                     G_CALLBACK(on_registry_refresh), self);
    gtk_box_append(GTK_BOX(header_suffix), refresh_registry);

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Connect an account");
    gtk_widget_add_css_class(add, "flat");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_connector), self);
    gtk_box_append(GTK_BOX(header_suffix), add);

    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            header_suffix);

    self->settings_connectors = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_connectors),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_connectors, "boxed-list");
    g_signal_connect(self->settings_connectors, "row-activated",
                     G_CALLBACK(on_connector_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_connectors);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    /*
     * Servers with many tools: when a connector becomes three tools
     * instead of hundreds.  Per connector the choice is in its dialog;
     * these are the thresholds `auto` measures against.
     */
    {
        GtkWidget *many = adw_preferences_group_new();

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(many),
                                        "Servers with many tools");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(many),
            "Past either of these, a connector set to Auto reaches agents "
            "as a directory -- search, describe and call -- unless the "
            "agent's CLI searches tools itself, as Claude Code does.");

        self->settings_directory_min_tools =
            adw_spin_row_new_with_range(1, 10000, 1);
        adw_preferences_row_set_title(
            ADW_PREFERENCES_ROW(self->settings_directory_min_tools),
            "More tools than");
        adw_spin_row_set_value(
            ADW_SPIN_ROW(self->settings_directory_min_tools), 40);

        self->settings_directory_min_chars =
            adw_spin_row_new_with_range(1000, 10000000, 1000);
        adw_preferences_row_set_title(
            ADW_PREFERENCES_ROW(self->settings_directory_min_chars),
            "More characters of tool definitions than");
        adw_spin_row_set_value(
            ADW_SPIN_ROW(self->settings_directory_min_chars), 100000);

        g_signal_connect(self->settings_directory_min_tools, "notify::value",
                         G_CALLBACK(on_directory_threshold_changed), self);
        g_signal_connect(self->settings_directory_min_chars, "notify::value",
                         G_CALLBACK(on_directory_threshold_changed), self);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(many),
                                  self->settings_directory_min_tools);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(many),
                                  self->settings_directory_min_chars);
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                                 ADW_PREFERENCES_GROUP(many));
    }

    return page;
}
