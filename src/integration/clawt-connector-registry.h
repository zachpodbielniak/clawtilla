/*
 * clawt-connector-registry.h - Importing the open MCP registry
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The built-in table and a person's own overlay are both curated: every
 * entry in either was written by somebody who checked it.  The registry
 * at https://registry.modelcontextprotocol.io is neither -- it is
 * whatever anybody has published -- so it is imported into the same
 * #ClawtConnectorInfo shape but held to a lower standard of trust: it
 * only ever *fills a gap*, never replaces an entry the built-in table or
 * an overlay file already answers for, and each imported id is prefixed
 * so it can never collide with one somebody chose by hand.
 *
 * Everything that touches the network lives behind
 * clawt_connector_registry_refresh_async().  Parsing a page, and reading
 * or writing the cache, do not: they are pure functions over a
 * #JsonNode or a file, which is what lets the importer be tested against
 * a fixture and never against the registry itself.  `make test` opens no
 * network socket, and this file is the reason that claim still holds
 * once a registry importer exists at all.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "integration/clawt-connector.h"

G_BEGIN_DECLS

/**
 * clawt_connector_registry_parse_page:
 * @root: one page of the registry's own `GET /v0.1/servers` response
 * @out_entries: (element-type ClawtConnectorInfo) (transfer none): entries
 *   parsed from this page are appended here
 * @out_next_cursor: (out) (optional) (nullable) (transfer full): the
 *   registry's own pagination cursor for the next page, or %NULL when
 *   this was the last one
 * @error: (out) (optional): set only when @root itself is not a usable
 *   page
 *
 * Turns one page into catalogue entries.
 *
 * A page that is not a JSON object, or that carries no `servers` array,
 * is the one failure that takes the whole page down -- there is nothing
 * to salvage from a response that is not shaped like a response.
 * Anything wrong with a single server inside an otherwise-usable page --
 * no name, no way to reach it at all -- is skipped with a warning naming
 * the entry, exactly as a single broken entry in an overlay file is: the
 * registry is bigger than clawtilla's opinion of it, and one bad listing
 * must not cost every other one on the page.
 *
 * Returns: %TRUE if @root was a usable page, even if it added nothing
 */
gboolean clawt_connector_registry_parse_page(JsonNode    *root,
                                             GPtrArray   *out_entries,
                                             gchar      **out_next_cursor,
                                             GError     **error);

/**
 * clawt_connector_registry_cache_path:
 * @state_dir: the daemon's own state directory
 *
 * Returns: (transfer full): where the imported registry is cached
 */
gchar *clawt_connector_registry_cache_path(const gchar *state_dir);

/**
 * clawt_connector_registry_cache_load:
 * @path: from clawt_connector_registry_cache_path()
 * @out_fetched_at: (out) (optional): when the cache was last refreshed,
 *   Unix seconds, or 0 if there is no cache yet
 *
 * A cache that does not exist yet is empty, not an error: nothing has
 * been imported, which is the ordinary state whenever
 * `connectors.registry_enabled` is off -- and it defaults to off.
 *
 * Returns: (transfer full) (element-type ClawtConnectorInfo): the
 *   entries last saved, possibly empty
 */
GPtrArray *clawt_connector_registry_cache_load(const gchar *path,
                                               gint64      *out_fetched_at);

/**
 * clawt_connector_registry_cache_save:
 * @path: from clawt_connector_registry_cache_path()
 * @entries: (element-type ClawtConnectorInfo): what to persist
 * @fetched_at: Unix seconds this refresh completed, becomes the next
 *   refresh's `updated_since`
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean clawt_connector_registry_cache_save(const gchar *path,
                                             GPtrArray   *entries,
                                             gint64       fetched_at,
                                             GError     **error);

/**
 * clawt_connector_catalog_merge_registry:
 * @catalog: (element-type ClawtConnectorInfo): a loaded catalogue,
 *   extended in place
 * @registry_entries: (element-type ClawtConnectorInfo) (nullable):
 *   imported entries, most often from clawt_connector_registry_cache_load()
 *
 * Adds a registry entry only where nothing in @catalog already answers
 * for its id.
 *
 * The built-in table and a person's own overlay are both a choice
 * somebody made on purpose; an imported listing is neither, so it fills
 * a gap and never overrides one -- unlike an overlay file, which
 * replaces a built-in entry wholesale by design. Two different kinds of
 * trust get two different merge rules.
 */
void clawt_connector_catalog_merge_registry(GPtrArray *catalog,
                                            GPtrArray *registry_entries);

/**
 * clawt_connector_registry_updated_since_for:
 * @fetched_at: Unix seconds a previous refresh completed at, or 0 for
 *   none
 *
 * The `updated_since` query value one refresh passes to the next.
 *
 * The registry's own API takes an RFC 3339 timestamp, not the Unix
 * seconds the cache stores it as, and %NULL for @fetched_at of 0 rather
 * than a formatted epoch -- a first-ever refresh has nothing to be
 * incremental *since*, and asking for updates since 1970 is a full walk
 * with extra steps that happens to still work, which is not the same as
 * being the request that was meant.
 *
 * Returns: (transfer full) (nullable): the timestamp, or %NULL
 */
gchar *clawt_connector_registry_updated_since_for(gint64 fetched_at);

/**
 * clawt_connector_registry_refresh_async:
 * @base_url: the registry's base URL, e.g. `connectors.registry_url`
 * @cache_path: from clawt_connector_registry_cache_path()
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the refresh finishes
 * @user_data: passed to @callback
 *
 * Imports the registry into @cache_path, incrementally.
 *
 * Every page already on disk is kept; a page is only fetched with
 * `updated_since` set to the previous refresh's own completion time, so
 * a registry with ten thousand servers costs one full walk and every
 * refresh after that costs only what changed.  Follows the registry's
 * own pagination cursor to the end before writing anything back, so a
 * refresh that is cancelled or fails partway leaves the previous cache
 * exactly as it was rather than half-updated.
 *
 * This is the only function in this file that reaches the network, and
 * nothing in clawtilla calls it from an IPC handler or from daemon
 * start -- see `connector.registry_refresh` in daemon-connector.c and
 * the reasoning recorded against `model.list` in CLAUDE.md, which made
 * the same mistake once already.
 */
void clawt_connector_registry_refresh_async(const gchar         *base_url,
                                            const gchar         *cache_path,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);

/**
 * clawt_connector_registry_refresh_finish:
 * @result: the #GAsyncResult passed to the callback
 * @out_imported: (out) (optional): how many entries the cache holds
 *   after this refresh
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean clawt_connector_registry_refresh_finish(GAsyncResult  *result,
                                                 guint         *out_imported,
                                                 GError       **error);

G_END_DECLS
