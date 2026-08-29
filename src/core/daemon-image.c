/*
 * daemon-image.c - The client surface: image.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_image(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /*
     * Cloud images.  A VM needs one, clawtilla ships none, and they are
     * several hundred megabytes -- so they are fetched deliberately,
     * ahead of any agent needing one, with progress to watch.
     */
    if (g_strcmp0(kind, "image.vm_catalog") == 0) {
        const ClawtVmImageSource *catalog;
        gsize n_sources = 0;
        gsize i;

        catalog = clawt_vm_image_catalog(&n_sources);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "sources");
        json_builder_begin_array(builder);

        for (i = 0; i < n_sources; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, catalog[i].id);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, catalog[i].name);
            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, catalog[i].group);
            json_builder_set_member_name(builder, "url");
            json_builder_add_string_value(builder, catalog[i].url);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_list") == 0) {
        g_autoptr(GPtrArray) images = clawt_vm_image_store_list(self->vm_images);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "images");
        json_builder_begin_array(builder);

        for (i = 0; images != NULL && i < images->len; i++) {
            ClawtVmImage *image = g_ptr_array_index(images, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, image->name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, image->path);
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, image->bytes);
            json_builder_set_member_name(builder, "total");
            json_builder_add_int_value(builder, image->total);
            json_builder_set_member_name(builder, "downloading");
            json_builder_add_boolean_value(builder, image->downloading);

            if (image->url != NULL) {
                json_builder_set_member_name(builder, "url");
                json_builder_add_string_value(builder, image->url);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_download") == 0) {
        const gchar *url = clawt_ipc_payload_string(payload, "url");
        g_autoptr(GError) start_error = NULL;
        g_autofree gchar *name = NULL;

        if (url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a url or a catalog id is required");

        /*
         * Returns as soon as the transfer is under way.  A handler runs on
         * the daemon's main context while the client blocks, so waiting
         * here for half a gigabyte would stall every other client for the
         * length of the download.
         */
        name = clawt_vm_image_store_start(self->vm_images, url,
                                          clawt_ipc_payload_string(payload,
                                                                   "name"),
                                          &start_error);

        if (name == NULL)
            return clawt_ipc_error_new(request, start_error->code,
                                       start_error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_cancel") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");

        if (name == NULL || !clawt_vm_image_store_cancel(self->vm_images,
                                                         name))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "nothing by that name is downloading");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cancelled");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_remove") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autoptr(GError) remove_error = NULL;
        g_autofree gchar *image_path = NULL;

        if (name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a name is required");

        /*
         * An agent's overlay records the base it was built on, so
         * deleting the base breaks that VM the next time it starts --
         * with an error from qemu about a missing backing file, a long
         * way from the button that caused it.
         */
        image_path = clawt_vm_image_store_path(self->vm_images, name);

        if (image_path != NULL &&
            !clawt_ipc_payload_boolean(payload, "force", FALSE)) {
            GPtrArray *agents = clawt_agent_manager_list(self->agents);
            g_autoptr(GString) users = g_string_new(NULL);
            guint i;

            for (i = 0; agents != NULL && i < agents->len; i++) {
                ClawtAgent *agent = g_ptr_array_index(agents, i);
                g_autofree gchar *configured = clawt_agent_config_get_path_value(
                    clawt_agent_get_config(agent), "computer.vm.image");

                if (g_strcmp0(configured, image_path) != 0)
                    continue;

                if (users->len > 0)
                    g_string_append(users, ", ");

                g_string_append(users, clawt_agent_get_id(agent));
            }

            if (users->len > 0) {
                g_autofree gchar *refusal = g_strdup_printf(
                    "%s is the disk image for %s. Deleting it breaks that "
                    "VM the next time it starts, because its overlay is "
                    "built on this file. Point the agent at another image "
                    "first, or pass force to delete it anyway.",
                    name, users->str);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           refusal);
            }
        }

        if (!clawt_vm_image_store_remove(self->vm_images, name,
                                         &remove_error))
            return clawt_ipc_error_new(request, remove_error->code,
                                       remove_error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.list") == 0) {
        const ClawtImageInfo *catalog;
        g_auto(GStrv) configured = NULL;
        gsize n_images = 0;
        gsize i;

        catalog = clawt_image_catalog_get(&n_images);
        configured = clawt_config_get_string_list(self->config,
                                                  "defaults.container_images");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "default");
        json_builder_add_string_value(
            builder, clawt_config_get_string(self->config,
                                             "defaults.container_image"));

        /*
         * Nothing here is a restriction: any reference podman can pull
         * is valid.  Said explicitly so a client offers a way to type
         * one that is not listed rather than treating this as a menu.
         */
        json_builder_set_member_name(builder, "open_ended");
        json_builder_add_boolean_value(builder, TRUE);

        json_builder_set_member_name(builder, "images");
        json_builder_begin_array(builder);

        /*
         * The user's own first.  A list where the images they added sit
         * below a dozen they will never pick is one they scroll past.
         */
        for (i = 0; configured != NULL && configured[i] != NULL; i++) {
            const gchar *entry = configured[i];
            const gchar *separator = strstr(entry, " -- ");
            g_autofree gchar *reference = NULL;

            if (*entry == '\0')
                continue;

            reference = (separator != NULL)
                        ? g_strndup(entry, separator - entry)
                        : g_strdup(entry);
            g_strstrip(reference);

            if (*reference == '\0')
                continue;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "reference");
            json_builder_add_string_value(builder, reference);

            /*
             * The last path component as the label.  A registry-and-org
             * prefix is the same on all of a user's own images, so a
             * list showing the whole reference truncates to
             * "registry.exampl..." for every one of them and
             * distinguishes none.  The full reference is still on the
             * row's subtitle once selected.
             */
            json_builder_set_member_name(builder, "label");
            {
                const gchar *slash = strrchr(reference, '/');

                json_builder_add_string_value(
                    builder, (slash != NULL && slash[1] != '\0') ? slash + 1
                                                                 : reference);
            }

            if (separator != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, separator + 4);
            }

            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, "Yours");
            json_builder_end_object(builder);
        }

        for (i = 0; i < n_images; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "reference");
            json_builder_add_string_value(builder, catalog[i].reference);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, catalog[i].label);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, catalog[i].group);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
