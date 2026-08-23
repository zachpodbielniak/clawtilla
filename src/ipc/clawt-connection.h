/*
 * clawt-connection.h - Saved ways of reaching a daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A client that can reach a daemon on another machine needs somewhere to
 * remember which machine, on what port, with which token -- otherwise
 * every connection is an address typed in again, and a bearer token typed
 * in again after it.
 *
 * These live in the *client's* config, not in clawtilla.yaml, and that is
 * the whole reason this is a separate file rather than a schema section.
 * The point of a profile is to reach a daemon that is somewhere else; a
 * laptop connecting to a workstation may have no fleet of its own, no
 * state directory and no clawtilla.yaml at all.  Reading the daemon's
 * config to find out how to reach a different daemon is backwards.
 *
 * The file holds bearer tokens, so it is written 0600 and its directory
 * is created 0700.  There is no way to write a secret into clawtilla.yaml
 * on purpose; here a token *is* the thing being remembered, and pushing
 * it into a second file the person also has to manage would mean most
 * people keeping it in their shell history instead.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "ipc/clawt-client.h"

G_BEGIN_DECLS

/**
 * CLAWT_DEFAULT_TCP_PORT:
 *
 * The port a daemon listens on when it listens on the network at all.
 *
 * Named here as well as in the config schema because the client has to
 * offer it as a default without reading the daemon's config -- which is
 * on the machine it is trying to reach.
 */
#define CLAWT_DEFAULT_TCP_PORT (8792)

#define CLAWT_TYPE_CONNECTION (clawt_connection_get_type())

typedef struct _ClawtConnection ClawtConnection;

GType clawt_connection_get_type(void) G_GNUC_CONST;

/**
 * clawt_connection_new_local:
 * @name: what to call it in the client
 * @socket_path: (nullable): the daemon's socket, or %NULL for the default
 *
 * A connection to a daemon on this machine.
 *
 * Returns: (transfer full): a new #ClawtConnection
 */
ClawtConnection *clawt_connection_new_local(const gchar *name,
                                            const gchar *socket_path);

/**
 * clawt_connection_new_remote:
 * @name: what to call it in the client
 * @host: the daemon's address
 * @port: its port
 * @token: (nullable): the bearer token that daemon expects
 *
 * A connection to a daemon on another machine.
 *
 * Returns: (transfer full): a new #ClawtConnection
 */
ClawtConnection *clawt_connection_new_remote(const gchar *name,
                                             const gchar *host,
                                             guint16      port,
                                             const gchar *token);

ClawtConnection *clawt_connection_copy(ClawtConnection *self);
void             clawt_connection_free(ClawtConnection *self);

const gchar *clawt_connection_get_name(ClawtConnection *self);
gboolean     clawt_connection_is_local(ClawtConnection *self);
const gchar *clawt_connection_get_socket_path(ClawtConnection *self);
const gchar *clawt_connection_get_host(ClawtConnection *self);
guint16      clawt_connection_get_port(ClawtConnection *self);
const gchar *clawt_connection_get_token(ClawtConnection *self);
gboolean     clawt_connection_get_tls(ClawtConnection *self);
gboolean     clawt_connection_get_accept_unknown_certificate(
    ClawtConnection *self);

void clawt_connection_set_name(ClawtConnection *self, const gchar *name);
void clawt_connection_set_tls(ClawtConnection *self,
                              gboolean         enabled,
                              gboolean         accept_unknown_certificate);

/**
 * clawt_connection_describe:
 * @self: a #ClawtConnection
 *
 * Where this connection goes, for showing beside its name.
 *
 * Never includes the token: this ends up in a subtitle in the client and
 * in `clawtilla remote list`, and a bearer token printed for convenience
 * is a bearer token in somebody's scrollback.
 *
 * Returns: (transfer full): a short description
 */
gchar *clawt_connection_describe(ClawtConnection *self);

/**
 * clawt_connection_create_client:
 * @self: a #ClawtConnection
 *
 * Builds the client this connection describes, without connecting it.
 *
 * Returns: (transfer full): a new #ClawtClient
 */
ClawtClient *clawt_connection_create_client(ClawtConnection *self);

/**
 * clawt_connection_list_default_path:
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/connections.yaml`
 */
gchar *clawt_connection_list_default_path(void);

/**
 * clawt_connection_list_parse:
 * @text: the contents of a connections file
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a set of profiles.
 *
 * Separated from the file so a round trip can be asserted on without
 * writing anything: the failure this guards against is a token or a port
 * that survives being saved and does not survive being loaded, which on a
 * file only touched when someone edits their connections would otherwise
 * be found by that person, months later.
 *
 * Returns: (transfer full) (element-type ClawtConnection) (nullable): the
 *   profiles, or %NULL on error
 */
GPtrArray *clawt_connection_list_parse(const gchar *text, GError **error);

/**
 * clawt_connection_list_to_data:
 * @connections: (element-type ClawtConnection): the profiles
 *
 * Renders profiles back to YAML.
 *
 * Returns: (transfer full): the file contents
 */
gchar *clawt_connection_list_to_data(GPtrArray *connections);

/**
 * clawt_connection_list_load:
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * A missing file is an empty list rather than an error: not having saved
 * a remote host yet is the ordinary state, not a fault.
 *
 * Returns: (transfer full) (element-type ClawtConnection) (nullable): the
 *   profiles, or %NULL if the file exists and could not be read
 */
GPtrArray *clawt_connection_list_load(const gchar *path, GError **error);

/**
 * clawt_connection_list_save:
 * @path: (nullable): the file, or %NULL for the default
 * @connections: (element-type ClawtConnection): the profiles
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the profiles, 0600, into a directory created 0700.
 *
 * Returns: %TRUE if written
 */
gboolean clawt_connection_list_save(const gchar *path,
                                    GPtrArray   *connections,
                                    GError     **error);

/**
 * clawt_connection_list_find:
 * @connections: (element-type ClawtConnection): the profiles
 * @name: the name to look for
 *
 * Returns: (transfer none) (nullable): the profile called @name
 */
ClawtConnection *clawt_connection_list_find(GPtrArray   *connections,
                                            const gchar *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtConnection, clawt_connection_free)

G_END_DECLS
