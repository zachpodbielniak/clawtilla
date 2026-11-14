/*
 * daemon-device.c - Paired client devices: device.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's side of per-device tokens.  ClawtDeviceStore holds them,
 * ClawtIpcServer checks them at `control.hello` and redeems codes at
 * `device.redeem`; this file is the rest: issuing a code, listing and
 * revoking devices, and the idle sweep.
 *
 * Who may do what.  Issuing a code is the owner's alone -- a device that
 * could mint codes could pair a replacement for itself and outlive its
 * own revocation.  Listing and revoking are open to any authenticated
 * client: a listing carries no secret, and revoking can only take access
 * away, so the laptop that is still in your bag can cut off the phone
 * that is not.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* Told to every client, so an open Devices list redraws. */
static void
publish_device_event(ClawtDaemon *self, const gchar *kind, const gchar *id)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;

    event = clawt_event_new(kind, NULL);
    clawt_event_set_detail(event, "device", id);
    clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_devices_configure(ClawtDaemon *self)
{
    gint64 days;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->devices == NULL)
        return;

    days = clawt_config_get_int(self->config, "daemon.device_idle_days");

    clawt_device_store_set_idle_seconds(self->devices,
                                        (days > 0) ? days * 86400 : 0);
}

void
clawt_daemon_devices_start(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtDeviceStore) store = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->state_dir == NULL || self->ipc_server == NULL)
        return;

    path = g_build_filename(self->state_dir, "devices.yaml", NULL);
    store = clawt_device_store_new(path);

    /*
     * A file that will not parse leaves pairing off rather than starting
     * from empty: the next pairing would write an empty list over every
     * device the damaged file still describes, and the owner token keeps
     * working either way.
     */
    if (!clawt_device_store_load(store, &error)) {
        g_warning("devices: %s -- paired devices cannot connect, and no "
                  "new one can be paired, until it is fixed or removed",
                  error->message);
        return;
    }

    self->devices = g_steal_pointer(&store);
    clawt_daemon_devices_configure(self);
    clawt_ipc_server_set_device_store(self->ipc_server, self->devices);
}

void
clawt_daemon_devices_sweep(ClawtDaemon *self)
{
    g_autoptr(GPtrArray) expired = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->devices == NULL || self->ipc_server == NULL)
        return;

    /*
     * Reapplied here as well as on reload, so a sweep never judges
     * idleness against a window the configuration no longer says.
     */
    clawt_daemon_devices_configure(self);

    expired = clawt_ipc_server_sweep_devices(self->ipc_server,
                                             now_seconds());

    for (i = 0; i < expired->len; i++)
        publish_device_event(self, "device.expired",
                             g_ptr_array_index(expired, i));
}

/*
 * A refusal that names what would have been allowed.
 *
 * "Permission denied" alone sends somebody looking for a file mode; this
 * says which credential the thing needs and where that credential is.
 */
static JsonNode *
owner_only(JsonNode *request, const gchar *device, const gchar *what)
{
    g_autofree gchar *message = g_strdup_printf(
        "%s needs the owner credential -- this connection is the paired "
        "device %s. Run it on the daemon's machine (`clawtilla device "
        "pair`), or from a client connected with the daemon's own token",
        what, device);

    return clawt_ipc_error_new(request, CLAWT_ERROR_PERMISSION_DENIED,
                               message);
}

static JsonNode *
unavailable(JsonNode *request)
{
    return clawt_ipc_error_new(
        request, CLAWT_ERROR_NOT_SUPPORTED,
        "paired devices are unavailable on this daemon: devices.yaml in "
        "the state directory could not be read (the daemon log says why)");
}

static JsonNode *
handle_pair(ClawtDaemon *self, JsonNode *request, JsonObject *payload,
            const gchar *device)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *display = NULL;
    g_auto(GStrv) addresses = NULL;
    gint64 now = now_seconds();
    gint64 expires_at = 0;
    gsize i;

    if (device != NULL)
        return owner_only(request, device, "Pairing a new device");

    code = clawt_device_store_begin_pairing(
        self->devices, clawt_ipc_payload_string(payload, "name"), now,
        &expires_at, &error);

    if (code == NULL)
        return clawt_ipc_error_new(request, error->code, error->message);

    display = clawt_device_code_display(code);
    addresses = clawt_ipc_server_list_bound(self->ipc_server);

    /*
     * The code goes back to the owner who asked for it, and only to them.
     * It is not a bearer token: it is single use, five minutes long and
     * guess-limited, and it is useless to anybody who cannot also reach a
     * listener -- which is why the listeners are named beside it.
     */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_string_value(builder, code);
    json_builder_set_member_name(builder, "display");
    json_builder_add_string_value(builder, display);

    /*
     * Both ends of the clock.  A client counts down from `expires_in`
     * against its own clock, because the daemon's `expires_at` is only
     * right on a machine whose clock agrees with it.
     */
    json_builder_set_member_name(builder, "expires_in");
    json_builder_add_int_value(builder, expires_at - now);
    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(builder, expires_at);
    json_builder_set_member_name(builder, "addresses");
    json_builder_begin_array(builder);

    for (i = 0; addresses != NULL && addresses[i] != NULL; i++)
        json_builder_add_string_value(builder, addresses[i]);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    memset(code, 0, strlen(code));

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

/*
 * The listing.  Never a token, never a hash: #ClawtDevice does not carry
 * either, so there is nothing here that could put one in the reply.
 */
static JsonNode *
handle_list(ClawtDaemon *self, JsonNode *request, const gchar *device)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GPtrArray) devices = clawt_device_store_list(self->devices);
    gint64 now = now_seconds();
    gint64 pairing = clawt_device_store_pairing_expires_at(self->devices,
                                                           now);
    guint i;

    json_builder_begin_object(builder);

    /*
     * Whether this client may issue a code, so it can offer the button
     * only where pressing it would work.
     */
    json_builder_set_member_name(builder, "owner");
    json_builder_add_boolean_value(builder, device == NULL);
    json_builder_set_member_name(builder, "idle_days");
    json_builder_add_int_value(
        builder, clawt_config_get_int(self->config,
                                      "daemon.device_idle_days"));

    /* Whether a code is outstanding -- and never which. */
    json_builder_set_member_name(builder, "pairing_expires_in");
    json_builder_add_int_value(builder, pairing > 0 ? pairing - now : 0);

    json_builder_set_member_name(builder, "devices");
    json_builder_begin_array(builder);

    for (i = 0; i < devices->len; i++) {
        ClawtDevice *entry = g_ptr_array_index(devices, i);
        const gchar *id = clawt_device_get_id(entry);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, clawt_device_get_name(entry));
        json_builder_set_member_name(builder, "created");
        json_builder_add_int_value(builder, clawt_device_get_created(entry));
        json_builder_set_member_name(builder, "last_seen");
        json_builder_add_int_value(builder,
                                   clawt_device_get_last_seen(entry));
        json_builder_set_member_name(builder, "connected");
        json_builder_add_boolean_value(
            builder,
            clawt_ipc_server_device_is_connected(self->ipc_server, id));

        /* So a client can say "this device" beside its own row. */
        json_builder_set_member_name(builder, "current");
        json_builder_add_boolean_value(builder,
                                       g_strcmp0(device, id) == 0);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

static JsonNode *
handle_revoke(ClawtDaemon *self, JsonNode *request, JsonObject *payload)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = g_strdup(clawt_ipc_payload_string(payload, "id"));
    guint closed;

    if (id == NULL || *id == '\0')
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "which device? `device.revoke` needs "
                                   "an id from `device.list`");

    if (!clawt_device_store_revoke(self->devices, id, &error))
        return clawt_ipc_error_new(request, error->code, error->message);

    /*
     * The token refuses the next hello; this ends the sessions it has
     * already opened.  That may include the connection asking, which
     * then never sees this reply -- the right outcome for a device that
     * revoked itself.
     */
    closed = clawt_ipc_server_close_device(self->ipc_server, id);

    publish_device_event(self, "device.revoked", id);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);
    json_builder_set_member_name(builder, "closed");
    json_builder_add_int_value(builder, closed);
    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_device(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    const gchar *device;

    *handled = TRUE;

    if (g_strcmp0(kind, "device.pair") != 0 &&
        g_strcmp0(kind, "device.pair_cancel") != 0 &&
        g_strcmp0(kind, "device.list") != 0 &&
        g_strcmp0(kind, "device.revoke") != 0) {
        *handled = FALSE;
        return NULL;
    }

    if (self->devices == NULL)
        return unavailable(request);

    device = clawt_ipc_server_get_current_device(self->ipc_server);

    if (g_strcmp0(kind, "device.pair") == 0)
        return handle_pair(self, request, payload, device);

    /*
     * Closing the dialog that showed the code withdraws it, so a code
     * nobody is going to use does not sit valid for the rest of its
     * five minutes.
     */
    if (g_strcmp0(kind, "device.pair_cancel") == 0) {
        if (device != NULL)
            return owner_only(request, device, "Withdrawing a pairing code");

        clawt_device_store_cancel_pairing(self->devices);

        /* An empty reply is an empty object, never a missing one. */
        return clawt_ipc_response_new(
            request, json_node_init_object(json_node_alloc(),
                                           json_object_new()));
    }

    if (g_strcmp0(kind, "device.list") == 0)
        return handle_list(self, request, device);

    return handle_revoke(self, request, payload);
}
