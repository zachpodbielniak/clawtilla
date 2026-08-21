/*
 * clawt-gtk.h - Shared declarations for the GTK client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The widgets are built in C rather than from .ui files.  Two reasons:
 * everything here is driven by data that arrives over the socket, so the
 * interesting part is the binding rather than the layout; and a compiler
 * catches a mistake in this file, where a typo in a .ui file becomes a
 * warning at runtime that a person has to be looking to notice.
 */

#pragma once

#include <adwaita.h>
#include <clawtilla.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_WINDOW (clawt_window_get_type())

G_DECLARE_FINAL_TYPE(ClawtWindow, clawt_window, CLAWT, WINDOW,
                     AdwApplicationWindow)

/**
 * clawt_window_new:
 * @app: the application
 * @client: (transfer none): the daemon connection
 *
 * Returns: (transfer none): the window
 */
ClawtWindow *clawt_window_new(AdwApplication *app, ClawtClient *client);

/**
 * clawt_window_toast:
 * @self: a #ClawtWindow
 * @text: what to say
 *
 * Shows a transient message.
 *
 * Used for anything the daemon refused.  A dialog for every refusal would
 * mean dismissing one before reading the next; a toast lets the work
 * carry on.
 */
void clawt_window_toast(ClawtWindow *self, const gchar *text);

/**
 * clawt_window_request:
 * @self: a #ClawtWindow
 * @kind: the request kind
 * @payload: (transfer full) (nullable): the request body
 *
 * Sends a request and shows any failure as a toast.
 *
 * Returns: (transfer full) (nullable): the reply payload, or %NULL
 */
JsonNode *clawt_window_request(ClawtWindow *self, const gchar *kind,
                               JsonNode *payload);

/**
 * clawt_json_string:
 * @object: (nullable): a JSON object
 * @key: the member to read
 * @fallback: (nullable): what to return if it is absent or not a string
 *
 * Returns: (transfer none) (nullable): the value, or @fallback
 */
const gchar *clawt_json_string(JsonObject   *object,
                               const gchar  *key,
                               const gchar  *fallback);

/**
 * clawt_payload_of:
 * @reply: a reply frame
 *
 * Returns: (transfer none) (nullable): the payload object
 */
JsonObject *clawt_payload_of(JsonNode *reply);

/**
 * clawt_build_payload:
 * @first_key: first member name, then its value, then more pairs, then %NULL
 *
 * Builds a flat string payload.
 *
 * Returns: (transfer full): the payload
 */
JsonNode *clawt_build_payload(const gchar *first_key, ...) G_GNUC_NULL_TERMINATED;

G_END_DECLS
