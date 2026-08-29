/*
 * clawt-connector.h - The catalogue of services an agent can be given
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A connector is one service -- GitHub, a GitLab instance, a Google
 * account -- described well enough that clawtilla can obtain a credential
 * for it and hand its tools to an agent without the agent ever holding
 * the credential.
 *
 * The catalogue is deliberately two things at once.  A table compiled
 * into the library covers the services people actually reach for, so the
 * common case needs no file and can be tested; a directory of YAML files
 * adds to it and overrides it, so a provider that moves an endpoint is
 * fixed by editing a file rather than by waiting for a release.  These
 * are somebody else's URLs and somebody else's scopes: a copy of them in
 * C goes stale silently, and silently in the wrong direction, which is
 * the same failure this codebase already learned from a pinned
 * dependency range.
 *
 * What a catalogue entry does *not* carry is a client id.  clawtilla has
 * no OAuth application of its own to lend, and borrowing another
 * project's would mean every clawtilla in the world identifying itself as
 * something it is not.  Registering an application takes a few minutes
 * once per provider, and @client_id_help says where.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * ClawtConnectorInfo:
 * @id: the `provider:` value, stable and lowercase
 * @name: what to call it on screen
 * @summary: one line about what connecting it gets you
 * @category: how the catalogue groups it in a list
 * @auth: how a credential is obtained
 * @auth_url: (nullable): device-code or authorization endpoint
 * @token_url: (nullable): where a code is exchanged and a token renewed
 * @revoke_url: (nullable): where a token is handed back
 * @scopes: (nullable): the default scopes to request, space separated
 * @client_id_help: (nullable): where to register an application
 * @docs_url: (nullable): the provider's own documentation
 * @default_instance: (nullable): the flagship host of a self-hostable service
 * @server_command: (nullable): a stdio MCP server that fronts it
 * @server_args: (array zero-terminated=1) (nullable): its arguments
 * @server_url: (nullable): an HTTP or SSE MCP server that fronts it
 * @instance_var: (nullable): the environment variable a stdio server reads
 *   to learn which instance to reach
 * @placement: where the relay puts the credential
 * @credential_name: (nullable): the variable or header it goes in
 * @credential_format: (nullable): a printf format with one %s for the value
 * @known_tools: (array zero-terminated=1) (nullable): the tool names this
 *   server is known to offer
 * @identity_keys: (array zero-terminated=1) (nullable): keys that must
 *   differ between two agents sharing one instance of this connector
 * @identity_note: (nullable): what goes wrong when they do not
 *
 * One service in the catalogue.
 *
 * A table rather than a class, for the same reason #ClawtIntegrationInfo
 * is one: every entry is pure description, and a class per provider would
 * be a hundred files that each say the same six things differently.
 *
 * @default_instance is what makes a self-hosted service work.  When it is
 * set, the connector is one somebody may run themselves, and @auth_url,
 * @token_url, @revoke_url and @server_url are read as *paths* joined onto
 * whichever instance the configuration names -- falling back to this one.
 * When it is %NULL they are absolute URLs.  One rule, applied to every
 * field, because a mixture of absolute and relative in the same entry is
 * how a Forgejo connector ends up quietly authenticating against
 * somebody else's server.
 *
 * @server_command and @server_url are hints, not requirements.  The
 * catalogue's job is the credential; a connector with neither is
 * perfectly useful and takes its server from the integration's own
 * `command` or `url`, exactly as an `mcp` integration does.
 *
 * @known_tools is a different kind of hint: what the entry's author
 * believes the server offers, so an integration's own `tools:` narrowing
 * can be checked against it.  A name that does not appear there is
 * almost always a typo, and the relay warns about it rather than
 * silently narrowing an agent down to nothing.  Left %NULL, which is the
 * ordinary case for a connector whose server nobody here has looked
 * inside of, disables the check rather than treating every name as
 * unknown.
 *
 * @instance_var is the other half of @default_instance.  For an HTTP
 * server the resolved instance *is* the URL the relay dials, so nothing
 * more is needed; a stdio server is a separate program and has to be
 * told, and every one of them takes it from the environment.  Without
 * it a connector pointed at a second instance starts a server that
 * quietly talks to the first -- which is the same failure
 * @default_instance exists to prevent, one layer along.
 *
 * @identity_keys is the connector's own version of the field
 * #ClawtIntegrationInfo carries: the `connector` integration type sets
 * none, because sharing one account across a fleet is usually the whole
 * point of connecting it.  For a service that records *who* did
 * something it is not, and the entry says so here rather than the type
 * having to choose one answer for every provider.  @identity_note is
 * appended to the warning, because "give each its own" without saying
 * what breaks reads as pedantry.
 */
typedef struct {
    const gchar              *id;
    const gchar              *name;
    const gchar              *summary;
    const gchar              *category;
    ClawtConnectorAuth        auth;
    const gchar              *auth_url;
    const gchar              *token_url;
    const gchar              *revoke_url;
    const gchar              *scopes;
    const gchar              *client_id_help;
    const gchar              *docs_url;
    const gchar              *default_instance;
    const gchar              *server_command;
    const gchar *const       *server_args;
    const gchar              *server_url;
    const gchar              *instance_var;
    ClawtCredentialPlacement  placement;
    const gchar              *credential_name;
    const gchar              *credential_format;
    const gchar *const       *known_tools;
    const gchar *const       *identity_keys;
    const gchar              *identity_note;
} ClawtConnectorInfo;

/**
 * clawt_connector_catalog_load:
 * @overlay_dir: (nullable): a directory of `.yaml` files to read
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the catalogue: the built-in entries, then whatever @overlay_dir
 * adds or replaces.
 *
 * Files are read in filename order and an entry with an existing @id
 * replaces it wholesale rather than merging into it.  Merging would mean
 * a half-overridden entry -- a new token endpoint against an old
 * authorization endpoint -- which is a state nobody asked for and which
 * fails in a way that points at neither file.
 *
 * A file that cannot be parsed is a warning naming the file, not a
 * failure: a typo in one connector must not take away the others, and
 * this is read on the path that starts the daemon.
 *
 * Returns: (transfer full) (element-type ClawtConnectorInfo): every
 *   connector, sorted by category then name
 */
GPtrArray *clawt_connector_catalog_load(const gchar  *overlay_dir,
                                        GError      **error);

/**
 * clawt_connector_catalog_builtin:
 * @n_connectors: (out): how many there are
 *
 * The compiled-in entries alone, without reading anything.
 *
 * Returns: (array length=n_connectors) (transfer none): the entries
 */
const ClawtConnectorInfo *clawt_connector_catalog_builtin(gsize *n_connectors);

/**
 * clawt_connector_catalog_find:
 * @catalog: (element-type ClawtConnectorInfo): a loaded catalogue
 * @id: the provider id to look for
 *
 * Returns: (transfer none) (nullable): the entry, or %NULL
 */
const ClawtConnectorInfo *clawt_connector_catalog_find(GPtrArray   *catalog,
                                                       const gchar *id);

/**
 * clawt_connector_default_overlay_dir:
 *
 * Where a person's own connector files live.
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/connectors.d`
 */
gchar *clawt_connector_default_overlay_dir(void);

/**
 * clawt_connector_resolve_url:
 * @info: a connector
 * @endpoint: (nullable): one of its URL fields
 * @instance: (nullable): the instance from the configuration
 *
 * Turns one of @info's endpoint fields into a URL that can be dialled.
 *
 * For a connector with no @default_instance this hands back @endpoint
 * unchanged.  For a self-hostable one it joins @endpoint onto @instance,
 * or onto @default_instance when the configuration named none -- so
 * pointing an agent at a Forgejo of your own is one config line and not a
 * copy of the whole entry.
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL if @endpoint was %NULL
 */
gchar *clawt_connector_resolve_url(const ClawtConnectorInfo *info,
                                   const gchar              *endpoint,
                                   const gchar              *instance);

/**
 * clawt_connector_format_credential:
 * @info: a connector
 * @value: the secret
 *
 * Applies @info's @credential_format to @value.
 *
 * Split out because the difference between `Bearer xyz` and a bare `xyz`
 * is invisible in a config file and produces a 401 that names neither.
 *
 * Returns: (transfer full): the value as the service expects to receive it
 */
gchar *clawt_connector_format_credential(const ClawtConnectorInfo *info,
                                         const gchar              *value);

/**
 * clawt_connector_token_path:
 * @secrets_dir: where the daemon keeps credentials
 * @name: the integration instance's name
 *
 * Where one connector's credential is kept.
 *
 * Keyed by the instance name rather than by the provider, because two
 * accounts on one service is the ordinary case and sharing a file
 * between them would mean connecting the second silently signed the
 * first out.
 *
 * Returns: (transfer full): the path
 */
gchar *clawt_connector_token_path(const gchar *secrets_dir,
                                  const gchar *name);

/**
 * clawt_connector_info_copy:
 * @src: an entry, built-in or loaded
 *
 * A deep copy that owns its own strings, so a caller holding a pointer
 * into the built-in table -- which is `static const` and shares its
 * strings with every other reader of it -- can put a copy somewhere it
 * will later free, such as onto a catalogue built by
 * clawt_connector_catalog_merge_registry().
 *
 * Returns: (transfer full): the copy
 */
ClawtConnectorInfo *clawt_connector_info_copy(const ClawtConnectorInfo *src);

/**
 * clawt_connector_info_free:
 * @info: (transfer full): an entry from a loaded catalogue
 *
 * Frees an entry.  Only entries built by the loader own their strings;
 * this is the catalogue's element free function and is not for the
 * built-in table.
 */
void clawt_connector_info_free(ClawtConnectorInfo *info);

G_END_DECLS
