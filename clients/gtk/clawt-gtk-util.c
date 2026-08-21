/*
 * clawt-gtk-util.c - Small shared helpers for the GTK client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawt-gtk.h"

#include <stdarg.h>

const gchar *
clawt_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    if (object == NULL || key == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_STRING)
        return fallback;

    return json_object_get_string_member(object, key);
}

JsonObject *
clawt_payload_of(JsonNode *reply)
{
    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return NULL;

    return json_node_get_object(reply);
}

JsonNode *
clawt_build_payload(const gchar *first_key, ...)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *key = first_key;
    va_list args;

    json_builder_begin_object(builder);

    va_start(args, first_key);

    while (key != NULL) {
        const gchar *value = va_arg(args, const gchar *);

        /*
         * A NULL value drops the member rather than sending null.  The
         * daemon treats absent and null differently in places, and the
         * caller almost always means "I do not have one".
         */
        if (value != NULL) {
            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(builder, value);
        }

        key = va_arg(args, const gchar *);
    }

    va_end(args);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}
