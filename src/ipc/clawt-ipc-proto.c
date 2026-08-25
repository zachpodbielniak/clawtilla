/*
 * clawt-ipc-proto.c - The frames clients and the daemon exchange
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-ipc-proto.h"

#include <string.h>

static JsonNode *
frame_new(const gchar *kind, const gchar *id)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *object = json_object_new();

    json_object_set_int_member(object, "v", CLAWT_IPC_VERSION);
    json_object_set_string_member(object, "kind", kind);

    if (id != NULL)
        json_object_set_string_member(object, "id", id);

    json_node_take_object(node, object);

    return node;
}

JsonNode *
clawt_ipc_request_new(const gchar *kind, const gchar *id)
{
    g_return_val_if_fail(kind != NULL, NULL);

    return frame_new(kind, id);
}

JsonNode *
clawt_ipc_response_new(JsonNode *request, JsonNode *payload)
{
    JsonNode *frame;
    const gchar *kind = "response";

    if (request != NULL && clawt_ipc_frame_get_kind(request) != NULL)
        kind = clawt_ipc_frame_get_kind(request);

    frame = frame_new(kind, request != NULL
                            ? clawt_ipc_frame_get_id(request) : NULL);

    json_object_set_boolean_member(json_node_get_object(frame), "ok", TRUE);

    if (payload != NULL)
        clawt_ipc_frame_set_payload(frame, payload);

    return frame;
}

JsonNode *
clawt_ipc_error_new(JsonNode *request, gint code, const gchar *message)
{
    JsonNode *frame;
    JsonObject *object;
    g_autofree gchar *safe = NULL;

    frame = frame_new(request != NULL &&
                      clawt_ipc_frame_get_kind(request) != NULL
                          ? clawt_ipc_frame_get_kind(request) : "error",
                      request != NULL ? clawt_ipc_frame_get_id(request)
                                      : NULL);

    object = json_node_get_object(frame);
    json_object_set_boolean_member(object, "ok", FALSE);
    json_object_set_int_member(object, "code", code);

    /*
     * Redacted on the way out.  Error messages quote what failed, and what
     * failed is sometimes a command line with a token in it.
     */
    safe = clawt_redact_secrets(message != NULL ? message : "failed");
    json_object_set_string_member(object, "error", safe);

    return frame;
}

JsonNode *
clawt_ipc_event_new(ClawtEvent *event)
{
    JsonNode *frame;

    g_return_val_if_fail(event != NULL, NULL);

    frame = frame_new("event", NULL);
    clawt_ipc_frame_set_payload(frame, clawt_event_to_json(event));

    return frame;
}

gboolean
clawt_ipc_frame_validate(JsonNode *frame, GError **error)
{
    JsonObject *object;
    gint64 version;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "a frame must be a JSON object");
        return FALSE;
    }

    object = json_node_get_object(frame);

    if (!json_object_has_member(object, "kind")) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the frame has no kind");
        return FALSE;
    }

    if (json_node_get_value_type(json_object_get_member(object, "kind")) !=
        G_TYPE_STRING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the frame's kind is not a string");
        return FALSE;
    }

    version = (json_object_has_member(object, "v") &&
               json_node_get_value_type(json_object_get_member(object, "v")) ==
                   G_TYPE_INT64)
              ? json_object_get_int_member(object, "v") : 0;

    if (version != CLAWT_IPC_VERSION) {
        /*
         * Refused with both versions named.  "Protocol error" alone sends
         * people looking for a bug when the answer is that one half of the
         * install was upgraded and the other was not.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "this frame speaks protocol version %" G_GINT64_FORMAT
                    ", but this build speaks %d -- the client and the "
                    "daemon are different versions",
                    version, CLAWT_IPC_VERSION);
        return FALSE;
    }

    return TRUE;
}

const gchar *
clawt_ipc_frame_get_kind(JsonNode *frame)
{
    JsonObject *object;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame))
        return NULL;

    object = json_node_get_object(frame);

    if (!json_object_has_member(object, "kind"))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(object, "kind")) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(object, "kind");
}

const gchar *
clawt_ipc_frame_get_id(JsonNode *frame)
{
    JsonObject *object;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame))
        return NULL;

    object = json_node_get_object(frame);

    if (!json_object_has_member(object, "id"))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(object, "id")) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(object, "id");
}

JsonObject *
clawt_ipc_frame_get_payload(JsonNode *frame)
{
    JsonObject *object;
    JsonNode *payload;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame))
        return NULL;

    object = json_node_get_object(frame);

    if (!json_object_has_member(object, "payload"))
        return NULL;

    payload = json_object_get_member(object, "payload");

    if (!JSON_NODE_HOLDS_OBJECT(payload))
        return NULL;

    return json_node_get_object(payload);
}

void
clawt_ipc_frame_set_payload(JsonNode *frame, JsonNode *payload)
{
    g_return_if_fail(frame != NULL && JSON_NODE_HOLDS_OBJECT(frame));
    g_return_if_fail(payload != NULL);

    json_object_set_member(json_node_get_object(frame), "payload", payload);
}

gboolean
clawt_ipc_frame_is_error(JsonNode *frame)
{
    JsonObject *object;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame))
        return TRUE;

    object = json_node_get_object(frame);

    if (!json_object_has_member(object, "ok"))
        return FALSE;

    return !json_object_get_boolean_member(object, "ok");
}

GError *
clawt_ipc_frame_to_error(JsonNode *frame)
{
    JsonObject *object;
    const gchar *message = "the daemon reported a failure";
    gint code = CLAWT_ERROR_FAILED;

    if (frame == NULL || !JSON_NODE_HOLDS_OBJECT(frame))
        return g_error_new_literal(CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                                   "unreadable reply");

    object = json_node_get_object(frame);

    /*
     * Type-checked like every other read of a peer's frame.  A reply with
     * "code" as a string is malformed, not a reason to hand json-glib
     * something it will complain about.
     */
    if (json_object_has_member(object, "error") &&
        json_node_get_value_type(json_object_get_member(object, "error")) ==
            G_TYPE_STRING)
        message = json_object_get_string_member(object, "error");

    if (json_object_has_member(object, "code") &&
        json_node_get_value_type(json_object_get_member(object, "code")) ==
            G_TYPE_INT64)
        code = (gint)json_object_get_int_member(object, "code");

    return g_error_new_literal(CLAWT_ERROR, code, message);
}

gchar *
clawt_ipc_frame_to_line(JsonNode *frame)
{
    g_autoptr(JsonGenerator) generator = NULL;
    g_autofree gchar *data = NULL;

    g_return_val_if_fail(frame != NULL, NULL);

    generator = json_generator_new();
    json_generator_set_root(generator, frame);
    data = json_generator_to_data(generator, NULL);

    return g_strdup_printf("%s\n", data);
}

JsonNode *
clawt_ipc_frame_from_line(const gchar *line, GError **error)
{
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GError) local = NULL;
    JsonNode *root;

    g_return_val_if_fail(line != NULL, NULL);

    if (strlen(line) > CLAWT_IPC_MAX_FRAME_BYTES) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "frame of %u bytes exceeds the %d byte limit",
                    (guint)strlen(line), CLAWT_IPC_MAX_FRAME_BYTES);
        return NULL;
    }

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, line, -1, &local)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "could not parse the frame: %s", local->message);
        return NULL;
    }

    root = json_parser_get_root(parser);

    if (root == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the frame was empty");
        return NULL;
    }

    return json_node_copy(root);
}

const gchar *
clawt_ipc_payload_string(JsonObject *payload, const gchar *key)
{
    if (payload == NULL || key == NULL ||
        !json_object_has_member(payload, key))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(payload, key)) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(payload, key);
}

gint64
clawt_ipc_payload_int(JsonObject *payload, const gchar *key, gint64 fallback)
{
    if (payload == NULL || key == NULL ||
        !json_object_has_member(payload, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(payload, key)) !=
        G_TYPE_INT64)
        return fallback;

    return json_object_get_int_member(payload, key);
}

GStrv
clawt_ipc_payload_strv(JsonObject *payload, const gchar *key)
{
    JsonArray *array;
    GPtrArray *out;
    guint i;

    if (payload == NULL || key == NULL ||
        !json_object_has_member(payload, key))
        return NULL;

    if (json_node_get_node_type(
            json_object_get_member(payload, key)) != JSON_NODE_ARRAY)
        return NULL;

    array = json_object_get_array_member(payload, key);
    out = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonNode *element = json_array_get_element(array, i);

        if (json_node_get_value_type(element) != G_TYPE_STRING)
            continue;

        g_ptr_array_add(out, g_strdup(json_node_get_string(element)));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

gboolean
clawt_ipc_payload_boolean(JsonObject *payload, const gchar *key,
                          gboolean fallback)
{
    if (payload == NULL || key == NULL ||
        !json_object_has_member(payload, key))
        return fallback;

    /*
     * "true" and "false" are accepted as well as real booleans.
     *
     * Clients build payloads from string pairs -- both bundled ones have
     * a build_payload(key, value, ...) helper that only ever emits
     * strings -- so a flag sent as "true" was silently read as the
     * fallback. That is the worst possible failure for a boolean: the
     * request succeeds and does the other thing.
     */
    {
        JsonNode *node = json_object_get_member(payload, key);
        GType type = json_node_get_value_type(node);

        if (type == G_TYPE_BOOLEAN)
            return json_object_get_boolean_member(payload, key);

        if (type == G_TYPE_STRING) {
            const gchar *text = json_node_get_string(node);

            if (g_ascii_strcasecmp(text, "true") == 0 ||
                g_ascii_strcasecmp(text, "yes") == 0 ||
                g_strcmp0(text, "1") == 0)
                return TRUE;

            if (g_ascii_strcasecmp(text, "false") == 0 ||
                g_ascii_strcasecmp(text, "no") == 0 ||
                g_strcmp0(text, "0") == 0)
                return FALSE;
        }
    }

    return fallback;
}

gchar *
clawt_ipc_reply_refusal_text(JsonNode *payload, guint *n_refused)
{
    JsonObject *object;
    JsonArray *refused;
    g_autoptr(GString) text = NULL;
    guint length;
    guint i;

    if (n_refused != NULL)
        *n_refused = 0;

    if (payload == NULL || !JSON_NODE_HOLDS_OBJECT(payload))
        return NULL;

    object = json_node_get_object(payload);

    if (!json_object_has_member(object, "refused"))
        return NULL;

    refused = json_object_get_array_member(object, "refused");
    length = (refused != NULL) ? json_array_get_length(refused) : 0;

    if (length == 0)
        return NULL;

    if (n_refused != NULL)
        *n_refused = length;

    text = g_string_new(NULL);

    for (i = 0; i < length; i++) {
        JsonObject *entry = json_array_get_object_element(refused, i);
        const gchar *agent = clawt_ipc_payload_string(entry, "agent");
        const gchar *message = clawt_ipc_payload_string(entry, "message");

        g_string_append_printf(text, "%s: %s\n",
                               (agent != NULL) ? agent : "?",
                               (message != NULL) ? message
                                                 : "its config was refused");
    }

    /*
     * Said in full rather than left to the reader.  "1 agent refused" does
     * not tell somebody that the agent is still running, which is the part
     * that decides whether they have to do anything about it now.
     */
    if (length == 1)
        g_string_append(text, "1 agent is still running against the "
                              "config.yaml it already had.");
    else
        g_string_append_printf(text, "%u agents are still running against "
                                     "the config.yaml they already had.",
                               length);

    return g_strdup(text->str);
}
