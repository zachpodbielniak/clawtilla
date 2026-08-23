/*
 * clawt-matrix.h - Signing in to Matrix, and finding the rooms
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libreclaw's Matrix channel wants a homeserver, a user id and an access
 * token.  The first two a person knows; the third is the one nobody has
 * to hand, and the usual way to get it -- open Element, find Help &
 * About, scroll to the bottom, reveal the token, copy it -- is both
 * tedious and the single most common place a Matrix agent goes wrong.
 *
 * So clawtilla logs in for you.  The password is used once, in the
 * daemon, and is never written anywhere; the token that comes back is
 * written straight to a file that only the daemon can read and referred
 * to from the config by path.  It is never returned to the client that
 * asked for the login -- a secret's value has no business in an IPC
 * response, and here it does not need to be in one.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtMatrixLogin:
 * @user_id: the full user id the server says this is, e.g. `@agent:example.org`
 * @device_id: the device the server created for this session
 * @access_token: the token itself
 *
 * What a successful login gave back.
 *
 * @user_id comes from the server rather than from what was typed: a
 * person signs in as `agent` and the account is `@agent:example.org`, and
 * writing the short form into the config produces a channel that
 * authenticates and matches no mention.
 */
typedef struct {
    gchar *user_id;
    gchar *device_id;
    gchar *access_token;
} ClawtMatrixLogin;

#define CLAWT_TYPE_MATRIX_LOGIN (clawt_matrix_login_get_type())

GType clawt_matrix_login_get_type(void) G_GNUC_CONST;

ClawtMatrixLogin *clawt_matrix_login_copy(ClawtMatrixLogin *self);

/**
 * clawt_matrix_login_free:
 * @self: (transfer full) (nullable): a #ClawtMatrixLogin
 *
 * Frees it, wiping the token first.
 */
void clawt_matrix_login_free(ClawtMatrixLogin *self);

/**
 * ClawtMatrixRoom:
 * @id: the room id, e.g. `!abcdef:example.org`
 * @name: (nullable): its name, if it has one
 * @alias: (nullable): its canonical alias, if it has one
 *
 * One room the account is in.
 */
typedef struct {
    gchar *id;
    gchar *name;
    gchar *alias;
} ClawtMatrixRoom;

#define CLAWT_TYPE_MATRIX_ROOM (clawt_matrix_room_get_type())

GType clawt_matrix_room_get_type(void) G_GNUC_CONST;

ClawtMatrixRoom *clawt_matrix_room_copy(ClawtMatrixRoom *self);
void             clawt_matrix_room_free(ClawtMatrixRoom *self);

/**
 * clawt_matrix_room_describe:
 * @self: a #ClawtMatrixRoom
 *
 * What to show a person choosing rooms: the name if it has one, the
 * alias if not, and the id as a last resort.
 *
 * Returns: (transfer full): a label
 */
gchar *clawt_matrix_room_describe(ClawtMatrixRoom *self);

/**
 * clawt_matrix_base_url:
 * @homeserver: whatever was typed into the homeserver field
 *
 * Normalises a homeserver into a base URL.
 *
 * People type `matrix.example.org`, `https://matrix.example.org/` and
 * `https://matrix.example.org/_matrix` interchangeably, and the first
 * two are the same server.  A missing scheme becomes https, a trailing
 * slash is dropped, and a path is refused rather than guessed at --
 * appending our own path to somebody's is how a request ends up at
 * `/_matrix/_matrix/client/v3/login`.
 *
 * Returns: (transfer full) (nullable): the base URL, or %NULL if it is
 *   not usable as one
 */
gchar *clawt_matrix_base_url(const gchar *homeserver);

/**
 * clawt_matrix_parse_login:
 * @json: a login response body
 * @error: (out) (optional): return location for a #GError
 *
 * Pure, so the shape of a real response can be asserted on without a
 * homeserver -- including the error shape, which is the half that
 * matters: a wrong password is an ordinary outcome and has to reach the
 * person as the server's own words.
 *
 * Returns: (transfer full) (nullable): the login, or %NULL
 */
ClawtMatrixLogin *clawt_matrix_parse_login(const gchar *json, GError **error);

/**
 * clawt_matrix_parse_joined_rooms:
 * @json: a `/joined_rooms` response body
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable) (element-type ClawtMatrixRoom): the
 *   rooms, with ids only
 */
GPtrArray *clawt_matrix_parse_joined_rooms(const gchar *json, GError **error);

/**
 * clawt_matrix_login_async:
 * @homeserver: the homeserver, in any of the forms people type
 * @user: the user id or its local part
 * @password: the password, used once and never stored
 * @device_name: (nullable): what to call this session on the account's device list
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it finishes
 * @user_data: data for @callback
 *
 * Exchanges a password for an access token.
 */
void clawt_matrix_login_async(const gchar         *homeserver,
                              const gchar         *user,
                              const gchar         *password,
                              const gchar         *device_name,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/**
 * clawt_matrix_login_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the login
 */
ClawtMatrixLogin *clawt_matrix_login_finish(GAsyncResult  *result,
                                            GError       **error);

/**
 * clawt_matrix_rooms_async:
 * @homeserver: the homeserver
 * @access_token: a token for the account
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it finishes
 * @user_data: data for @callback
 *
 * Lists the rooms the account is joined to, with their names.
 *
 * Two round trips deep: the id list, then one name lookup per room.  A
 * room with no name is not an error -- a direct chat usually has none --
 * so a failed lookup leaves the id and carries on rather than failing
 * the whole listing for one room.
 */
void clawt_matrix_rooms_async(const gchar         *homeserver,
                              const gchar         *access_token,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/**
 * clawt_matrix_rooms_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable) (element-type ClawtMatrixRoom): the rooms
 */
GPtrArray *clawt_matrix_rooms_finish(GAsyncResult *result, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMatrixLogin, clawt_matrix_login_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMatrixRoom, clawt_matrix_room_free)

G_END_DECLS
