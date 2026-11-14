/*
 * lrg-settings-devices.c - Settings: paired devices
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors the Devices page of clients/gtk/gtk-prefs-devices.c and the
 * web client's /settings/devices: the paired devices with when each was
 * last seen and a Revoke button, and "Pair a new device", which shows
 * the code large with the time it has left.  The row and countdown
 * wording comes from libclawt (clawt_device_seen_text(),
 * clawt_device_countdown_text()) so the three clients agree.
 *
 * The countdown ticks on a one-second GSource attached through
 * clawt_lrg_app_attach(), so it is destroyed with the app; the frame
 * loop iterates that context while it idles.
 */

#include "clawt-lrg.h"

typedef struct {
    LrgWidget *list;
    LrgWidget *pair_button;
    guint      generation;
} Devices;

static Devices devices;

/*
 * The dialog showing a code, while it is up.  Reference counted between
 * the dialog (its destroy notify) and the tick, so whichever lets go
 * last frees it.
 */
typedef struct {
    ClawtLrgApp *app;
    LrgDialog   *dialog;      /* weak */
    LrgWidget   *countdown;   /* the dialog's */
    gint64       deadline;    /* monotonic seconds */
    gboolean     paired;
    gboolean     closed;
} PairShow;

static PairShow *showing;

static void refresh(ClawtLrgApp *app);

/* ── Pairing a new device ─────────────────────────────────────────── */

static void
pair_show_clear(gpointer data)
{
    PairShow *show = data;

    g_clear_weak_pointer(&show->dialog);
}

static void
pair_show_release(gpointer data)
{
    g_rc_box_release_full(data, pair_show_clear);
}

static gboolean
on_pair_tick(gpointer user_data)
{
    PairShow *show = user_data;
    gint64 left;
    g_autofree gchar *text = NULL;
    g_autofree gchar *line = NULL;

    if (show->closed || show->paired || show->dialog == NULL)
        return G_SOURCE_REMOVE;

    left = show->deadline - g_get_monotonic_time() / G_USEC_PER_SEC;
    text = clawt_device_countdown_text(left);
    line = (left > 0) ? g_strdup_printf("Good for %s, once.", text)
                      : g_strdup("This code has expired. Close this and "
                                 "ask for another.");
    clawt_lrg_paragraph_set_text(show->countdown, line);
    clawt_lrg_app_wake(show->app);

    return (left > 0) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/*
 * The dialog let go: a code nobody used is withdrawn rather than left
 * valid for the rest of its five minutes.
 */
static void
pair_show_closed(gpointer data)
{
    PairShow *show = data;

    show->closed = TRUE;

    if (!show->paired)
        clawt_lrg_request(show->app, "device.pair_cancel", NULL,
                          CLAWT_LRG_QUIET, NULL, NULL, NULL);

    if (showing == show)
        showing = NULL;

    pair_show_release(show);
}

static gboolean
on_pair_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                 gpointer user_data)
{
    return TRUE;
}

static void
on_paired_code(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    JsonObject *payload = clawt_ipc_reply_object(reply);
    JsonArray *addresses;
    g_autofree gchar *display = NULL;
    g_autofree gchar *body = NULL;
    GString *where;
    LrgWidget *box;
    PairShow *show;
    GSource *tick;
    guint i;

    if (reply == NULL)
        return;

    addresses = (payload != NULL &&
                 json_object_has_member(payload, "addresses"))
        ? json_object_get_array_member(payload, "addresses") : NULL;
    where = g_string_new(NULL);

    for (i = 0; addresses != NULL && i < json_array_get_length(addresses);
         i++)
        g_string_append_printf(where, "%s%s", (i > 0) ? ", " : "",
                               json_array_get_string_element(addresses, i));

    /* the address beside the code: the other device needs both */
    body = (where->len > 0)
        ? g_strdup_printf("On the other device choose Pair with a code in "
                          "Connections and enter this daemon's address "
                          "(%s) and the code.", where->str)
        : g_strdup("This daemon is not listening on the network, so no "
                   "other device can reach it yet. Turn on "
                   "daemon.tailscale or daemon.tcp_enabled first.");
    g_string_free(where, TRUE);

    display = clawt_device_code_display(
        clawt_ipc_payload_string(payload, "code") != NULL
            ? clawt_ipc_payload_string(payload, "code") : "");

    show = g_rc_box_new0(PairShow);
    show->app = app;
    show->deadline = g_get_monotonic_time() / G_USEC_PER_SEC +
                     clawt_ipc_payload_int(payload, "expires_in", 0);

    box = clawt_lrg_vbox(10.0f);
    clawt_lrg_take(box, clawt_lrg_label(display, "heading"));
    show->countdown = clawt_lrg_paragraph("", "muted");
    clawt_lrg_add(box, show->countdown);
    g_object_unref(show->countdown);

    {
        LrgDialog *dialog = clawt_lrg_dialog(
            app, "Pair a new device", body, box, "Done", FALSE,
            on_pair_response, show, pair_show_closed);

        g_set_weak_pointer(&show->dialog, dialog);
        g_object_unref(dialog);
    }

    g_object_unref(box);
    showing = show;

    /* the tick holds its own reference: it can outlive the dialog */
    tick = g_timeout_source_new_seconds(1);
    g_source_set_callback(tick, on_pair_tick, g_rc_box_acquire(show),
                          pair_show_release);
    clawt_lrg_app_attach(app, tick);
    g_source_unref(tick);

    on_pair_tick(show);
}

static void
on_pair_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (showing != NULL) {
        clawt_lrg_toast(app, "A code is already on screen.");
        return;
    }

    clawt_lrg_request(app, "device.pair", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_paired_code, NULL, NULL);
}

/* ── Revoking ─────────────────────────────────────────────────────── */

static void
on_revoked(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Revoked. Its token no longer works, and anything "
                         "it had open was closed.");
    refresh(app);
}

static void
on_revoke_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "device.revoke",
                      clawt_ipc_payload_new("id", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_revoked, NULL, NULL);
}

static void
on_revoke_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "clawt-device");
    const gchar *name = g_object_get_data(G_OBJECT(button), "clawt-name");
    gboolean current = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "clawt-current"));
    g_autofree gchar *title = g_strdup_printf("Revoke %s?",
                                              name != NULL ? name : id);

    clawt_lrg_confirm(
        app, title,
        current
            ? "This is the device you are using. Its token stops working "
              "and this window is disconnected; pair it again with a new "
              "code to come back."
            : "Its token stops working at once and anything it has "
              "connected is closed. The daemon's own token is not "
              "affected.",
        "Revoke", TRUE, on_revoke_confirmed, g_strdup(id), g_free);
}

/* ── The list ─────────────────────────────────────────────────────── */

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonObject *payload;
    JsonArray *list;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gboolean owner;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != devices.generation || reply == NULL)
        return;

    payload = clawt_ipc_reply_object(reply);
    list = (payload != NULL && json_object_has_member(payload, "devices"))
        ? json_object_get_array_member(payload, "devices") : NULL;
    owner = clawt_ipc_payload_boolean(payload, "owner", FALSE);

    /* pairing is the owner's: offered only where it would work */
    lrg_widget_set_visible(devices.pair_button, owner);

    clawt_lrg_clear(devices.list);

    if (!owner)
        clawt_lrg_take(devices.list, clawt_lrg_row(
            "Connected as a paired device",
            "Only the daemon's owner can pair another. Revoking still "
            "works."));

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(devices.list, clawt_lrg_row(
            "No paired devices",
            "Pair a laptop or phone to give it a token of its own, which "
            "you can revoke without touching anything else."));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *device = json_array_get_object_element(list, i);
        const gchar *id = clawt_ipc_payload_string(device, "id");
        const gchar *name = clawt_ipc_payload_string(device, "name");
        gboolean current = clawt_ipc_payload_boolean(device, "current",
                                                     FALSE);
        g_autofree gchar *seen = clawt_device_seen_text(
            clawt_ipc_payload_int(device, "last_seen", 0),
            clawt_ipc_payload_boolean(device, "connected", FALSE), now);
        g_autoptr(GDateTime) created = g_date_time_new_from_unix_local(
            clawt_ipc_payload_int(device, "created", 0));
        g_autofree gchar *when = (created != NULL)
            ? g_date_time_format(created, "%Y-%m-%d") : g_strdup("?");
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s \302\267 paired %s \302\267 %s", seen, when,
            id != NULL ? id : "?");
        LrgWidget *row = clawt_lrg_row(name != NULL ? name : id, subtitle);
        LrgWidget *revoke = clawt_lrg_button("Revoke", "danger",
                                             G_CALLBACK(on_revoke_clicked),
                                             app);

        if (current)
            clawt_lrg_row_add_prefix(row, clawt_lrg_badge("this device",
                                                          "info"));

        g_object_set_data_full(G_OBJECT(revoke), "clawt-device",
                               g_strdup(id), g_free);
        g_object_set_data_full(G_OBJECT(revoke), "clawt-name",
                               g_strdup(name), g_free);
        g_object_set_data(G_OBJECT(revoke), "clawt-current",
                          GINT_TO_POINTER(current));
        clawt_lrg_row_add_suffix(row, revoke);

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(devices.list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (devices.list == NULL)
        return;

    devices.generation++;
    clawt_lrg_request(app, "device.list", NULL, CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(devices.generation), NULL);
}

void
clawt_lrg_settings_devices_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (kind == NULL || !g_str_has_prefix(kind, "device."))
        return;

    /* the code on screen worked: say so, and stop counting it down */
    if (g_strcmp0(kind, "device.paired") == 0 && showing != NULL &&
        showing->dialog != NULL) {
        const gchar *name = clawt_event_get_detail(event, "name");
        g_autofree gchar *line = g_strdup_printf(
            "Paired %s. It has a token of its own now.",
            (name != NULL && *name != '\0') ? name : "the new device");

        showing->paired = TRUE;
        clawt_lrg_paragraph_set_text(showing->countdown, line);
    }

    refresh(app);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Devices", "Paired devices");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    devices.pair_button = clawt_lrg_button("Pair a new device", "primary",
                                           G_CALLBACK(on_pair_clicked), app);
    clawt_lrg_take(clawt_lrg_page_toolbar(page), devices.pair_button);

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Each holds a token of its own, so a lost laptop or phone is one "
        "Revoke rather than a new token for every client. The daemon's own "
        "token (clawtilla daemon token) is the owner's and is never "
        "affected. A device unseen for daemon.device_idle_days stops "
        "working by itself.", "muted"));

    devices.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, devices.list);
    g_object_unref(devices.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "devices", "Devices", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_devices_page(void)
{
    return &page_impl;
}
