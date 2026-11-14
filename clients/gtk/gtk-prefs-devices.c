/*
 * gtk-prefs-devices.c - Settings: paired devices
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Which devices hold a token of their own for this daemon, when each was
 * last seen, and a Revoke button for the one that went missing.  And the
 * other half of pairing: "Pair a new device" asks the daemon for a code
 * and shows it, large, with the time it has left -- the screen somebody
 * holds their phone up to.
 *
 * Mirrors the Devices page of the web client and of clawtilla-lrg; the
 * wording of a row (clawt_device_seen_text()) and of the countdown
 * (clawt_device_countdown_text()) comes from libclawt so the three say
 * the same thing.
 */

#include "clawt-window-private.h"

/* ── Pairing a new device ────────────────────────────────────────── */

/*
 * The dialog showing the code, while it is up.  One at a time: the daemon
 * keeps one code, so a second dialog would be showing a code the first
 * one's request had already replaced.
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *code_label;
    GtkWidget   *countdown;
    GSource     *timer;
    gint64       deadline;     /* monotonic seconds, this machine's clock */
    gboolean     paired;
} PairDialog;

static PairDialog *pair_dialog;

static void
pair_dialog_stop_timer(PairDialog *pair)
{
    if (pair->timer == NULL)
        return;

    g_source_destroy(pair->timer);
    g_clear_pointer(&pair->timer, g_source_unref);
}

static gboolean
on_pair_tick(gpointer user_data)
{
    PairDialog *pair = user_data;
    gint64 left = pair->deadline - g_get_monotonic_time() / G_USEC_PER_SEC;
    g_autofree gchar *text = clawt_device_countdown_text(left);
    g_autofree gchar *line = NULL;

    line = (left > 0) ? g_strdup_printf("Good for %s, once.", text)
                      : g_strdup("This code has expired. Close this and "
                                 "ask for another.");
    gtk_label_set_text(GTK_LABEL(pair->countdown), line);

    if (left > 0)
        return G_SOURCE_CONTINUE;

    gtk_widget_add_css_class(pair->code_label, "dim-label");
    g_clear_pointer(&pair->timer, g_source_unref);

    return G_SOURCE_REMOVE;
}

static void
on_pair_dialog_closed(AdwDialog *dialog, gpointer user_data)
{
    PairDialog *pair = user_data;

    (void)dialog;

    pair_dialog_stop_timer(pair);

    /*
     * A code nobody used is withdrawn rather than left valid for the
     * rest of its five minutes -- closing the window is the person
     * saying they are not going to type it anywhere.
     */
    if (!pair->paired && pair->window->client != NULL) {
        g_autoptr(JsonNode) reply = clawt_window_request(
            pair->window, "device.pair_cancel", NULL);
    }

    if (pair_dialog == pair)
        pair_dialog = NULL;

    g_free(pair);
}

static void
on_pair_new_device(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *body = NULL;
    GtkWidget *box;
    JsonObject *payload;
    JsonArray *addresses;
    GString *where;
    PairDialog *pair;
    guint i;

    (void)button;

    if (pair_dialog != NULL) {
        adw_dialog_present(pair_dialog->dialog, GTK_WIDGET(self));
        return;
    }

    reply = clawt_window_request(self, "device.pair", NULL);

    if (reply == NULL)
        return;

    payload = clawt_payload_of(reply);
    addresses = (payload != NULL &&
                 json_object_has_member(payload, "addresses"))
        ? json_object_get_array_member(payload, "addresses") : NULL;

    where = g_string_new(NULL);

    for (i = 0; addresses != NULL && i < json_array_get_length(addresses);
         i++)
        g_string_append_printf(where, "%s%s", (i > 0) ? ", " : "",
                               json_array_get_string_element(addresses, i));

    /*
     * The address beside the code, because the other device needs both
     * and this is the screen somebody is looking at.  A daemon with no
     * network listener can issue a code nobody can reach to redeem, and
     * saying that now beats a "could not connect" on the other device.
     */
    body = (where->len > 0)
        ? g_strdup_printf("On the other device choose Pair with a code in "
                          "Connections and enter this daemon's address "
                          "(%s) and the code.", where->str)
        : g_strdup("This daemon is not listening on the network, so no "
                   "other device can reach it yet. Turn on "
                   "daemon.tailscale or daemon.tcp_enabled first.");
    g_string_free(where, TRUE);

    pair = g_new0(PairDialog, 1);
    pair->window = self;
    pair->deadline = g_get_monotonic_time() / G_USEC_PER_SEC +
                     clawt_json_int(payload, "expires_in", 0);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    {
        g_autofree gchar *display = clawt_device_code_display(
            clawt_json_string(payload, "code", ""));

        pair->code_label = gtk_label_new(display);
        gtk_label_set_selectable(GTK_LABEL(pair->code_label), TRUE);
        gtk_widget_add_css_class(pair->code_label, "title-1");
        gtk_widget_add_css_class(pair->code_label, "monospace");
        gtk_box_append(GTK_BOX(box), pair->code_label);
    }

    pair->countdown = gtk_label_new(NULL);
    gtk_widget_add_css_class(pair->countdown, "dim-label");
    gtk_box_append(GTK_BOX(box), pair->countdown);

    pair->dialog = adw_alert_dialog_new("Pair a new device", body);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(pair->dialog), box);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(pair->dialog), "close",
                                  "Done");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(pair->dialog),
                                        "close");

    g_signal_connect(pair->dialog, "closed",
                     G_CALLBACK(on_pair_dialog_closed), pair);

    /*
     * Attached to the context this window runs on, by name, rather than
     * through g_timeout_add() -- which is the global default whether or
     * not that is where the window lives.
     */
    pair->timer = g_timeout_source_new_seconds(1);
    g_source_set_callback(pair->timer, on_pair_tick, pair, NULL);
    g_source_attach(pair->timer, g_main_context_get_thread_default());
    on_pair_tick(pair);

    pair_dialog = pair;
    adw_dialog_present(pair->dialog, GTK_WIDGET(self));
}

/* ── Revoking ────────────────────────────────────────────────────── */

static void
on_revoke_response(AdwAlertDialog *dialog, const gchar *response,
                   gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(dialog), "clawt-device");
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "revoke") != 0 || id == NULL)
        return;

    reply = clawt_window_request(self, "device.revoke",
                                 clawt_build_payload("id", id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Revoked. Its token no longer works, and "
                             "anything it had open was closed.");
    clawt_gtk_refresh_settings_devices(self);
}

static void
on_revoke_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "clawt-device");
    const gchar *name = g_object_get_data(G_OBJECT(button), "clawt-name");
    gboolean current = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "clawt-current"));
    g_autofree gchar *heading = g_strdup_printf("Revoke %s?",
                                                name != NULL ? name : id);
    AdwAlertDialog *dialog;

    /*
     * Said differently for the device this window is: revoking it cuts
     * this window off, and that should not be a surprise.
     */
    dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        heading,
        current
            ? "This is the device you are using. Its token stops working "
              "and this window is disconnected; pair it again with a new "
              "code to come back."
            : "Its token stops working at once and anything it has "
              "connected is closed. The daemon's own token is not "
              "affected."));
    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "revoke", "Revoke");
    adw_alert_dialog_set_response_appearance(dialog, "revoke",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_object_set_data_full(G_OBJECT(dialog), "clawt-device", g_strdup(id),
                           g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(on_revoke_response),
                     self);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/* ── The list ────────────────────────────────────────────────────── */

void
clawt_gtk_refresh_settings_devices(ClawtWindow *self)
{
    if (self->settings_devices == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_DEVICES))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonObject *payload;
        JsonArray *devices;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_devices));

        reply = clawt_window_request(self, "device.list", NULL);

        if (reply == NULL)
            continue;

        payload = clawt_payload_of(reply);
        devices = (payload != NULL &&
                   json_object_has_member(payload, "devices"))
            ? json_object_get_array_member(payload, "devices") : NULL;

        /*
         * Pairing is the owner's: offered only where pressing it would
         * work, with the reason on the button rather than a refusal
         * after it.
         */
        if (self->settings_devices_pair != NULL) {
            gboolean owner = clawt_json_boolean(payload, "owner", FALSE);

            gtk_widget_set_sensitive(self->settings_devices_pair, owner);
            gtk_widget_set_tooltip_text(
                self->settings_devices_pair,
                owner ? "Show a six-digit code another device can trade "
                        "for its own token"
                      : "Only the daemon's owner can pair -- this window "
                        "is connected as a paired device");
        }

        if (devices == NULL || json_array_get_length(devices) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "No paired devices");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "Pair a laptop or phone to give it a token of its own, "
                "which you can revoke without touching anything else.");
            gtk_list_box_append(GTK_LIST_BOX(self->settings_devices), row);
            continue;
        }

        for (i = 0; i < json_array_get_length(devices); i++) {
            JsonObject *device = json_array_get_object_element(devices, i);
            const gchar *id = clawt_json_string(device, "id", "");
            const gchar *name = clawt_json_string(device, "name", id);
            gboolean current = clawt_json_boolean(device, "current", FALSE);
            g_autofree gchar *seen = clawt_device_seen_text(
                clawt_json_int(device, "last_seen", 0),
                clawt_json_boolean(device, "connected", FALSE), now);
            g_autoptr(GDateTime) created = g_date_time_new_from_unix_local(
                clawt_json_int(device, "created", 0));
            g_autofree gchar *when = (created != NULL)
                ? g_date_time_format(created, "%e %b %Y") : g_strdup("?");
            g_autofree gchar *subtitle = g_strdup_printf(
                "%s \302\267 paired %s \302\267 %s", seen, g_strstrip(when),
                id);
            GtkWidget *row = adw_action_row_new();
            GtkWidget *revoke = gtk_button_new_with_label("Revoke");

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            if (current) {
                GtkWidget *badge = gtk_label_new("this device");

                gtk_widget_add_css_class(badge, "caption");
                gtk_widget_add_css_class(badge, "accent");
                gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), badge);
            }

            gtk_widget_set_valign(revoke, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(revoke, "destructive-action");
            g_object_set_data_full(G_OBJECT(revoke), "clawt-device",
                                   g_strdup(id), g_free);
            g_object_set_data_full(G_OBJECT(revoke), "clawt-name",
                                   g_strdup(name), g_free);
            g_object_set_data(G_OBJECT(revoke), "clawt-current",
                              GINT_TO_POINTER(current));
            g_signal_connect(revoke, "clicked",
                             G_CALLBACK(on_revoke_clicked), self);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), revoke);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_devices), row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_DEVICES));
}

void
clawt_gtk_devices_on_event(ClawtWindow *self, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    /*
     * The code on screen worked: say so where the person is looking, and
     * stop counting down a code that no longer exists.
     */
    if (g_strcmp0(kind, "device.paired") == 0 && pair_dialog != NULL &&
        pair_dialog->window == self) {
        const gchar *name = clawt_event_get_detail(event, "name");
        g_autofree gchar *line = g_strdup_printf(
            "Paired %s. It has a token of its own now.",
            (name != NULL && *name != '\0') ? name : "the new device");

        pair_dialog->paired = TRUE;
        pair_dialog_stop_timer(pair_dialog);
        gtk_label_set_text(GTK_LABEL(pair_dialog->countdown), line);
        gtk_widget_add_css_class(pair_dialog->code_label, "dim-label");
    }

    clawt_gtk_refresh_settings_devices(self);
}

GtkWidget *
clawt_gtk_build_devices_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *pair;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Devices");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "phone-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Paired devices");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Each holds a token of its own, so a lost laptop or phone is one "
        "Revoke rather than a new token for every client. The daemon's own "
        "token (clawtilla daemon token) is the owner's and is never "
        "affected. A device unseen for daemon.device_idle_days stops "
        "working by itself.");

    pair = gtk_button_new_with_label("Pair a new device");
    gtk_widget_add_css_class(pair, "suggested-action");
    gtk_widget_set_valign(pair, GTK_ALIGN_CENTER);
    g_signal_connect(pair, "clicked", G_CALLBACK(on_pair_new_device), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            pair);
    self->settings_devices_pair = pair;

    self->settings_devices = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_devices),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_devices, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_devices);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
