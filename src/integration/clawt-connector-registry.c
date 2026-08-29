/*
 * clawt-connector-registry.c - Importing the open MCP registry
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Verified against https://registry.modelcontextprotocol.io/v0.1/servers
 * on 2026-08-29: `GET /v0.1/servers` takes `cursor`, `limit`,
 * `updated_since` (RFC 3339) and `search`; a response is
 * `{"servers": [...], "metadata": {"nextCursor": ..., "count": ...}}`;
 * a server names either a `packages` array (`registryType`,
 * `identifier`, `runtimeHint`, `packageArguments`, `environmentVariables`
 * with `name`/`isRequired`/`isSecret`) or a `remotes` array (`type`,
 * `url`).  This is a *preview* API and the one place in this file that
 * is somebody else's decision rather than clawtilla's: the field names
 * above are what was true on the date at the top of this comment, not a
 * promise about what will be true afterwards -- which is exactly why the
 * catalogue's own overlay mechanism exists, and why this importer only
 * ever *adds* to it rather than replacing anything a person or the
 * built-in table already decided.
 */

#include "clawtilla.h"
#include "integration/clawt-connector-registry.h"

#include <libsoup/soup.h>
#include <string.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING

/* A page that never answers should not hang a refresh forever. */
#define REGISTRY_TIMEOUT_SECONDS (30)
#define REGISTRY_PAGE_LIMIT      (100)

/* ── Reading JSON defensively ─────────────────────────────────────── */

/*
 * json_object_get_array_member() and its object twin are documented as
 * an error to call on a member that is absent or a different shape, so
 * they are never called here without checking first -- these three read
 * a JsonObject the same defensive way clawt-oauth.c's object_string()
 * and object_int() already do for the wire shapes it parses.
 */

static const gchar *
obj_string(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

static gint64
obj_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    if (json_node_get_value_type(node) == G_TYPE_INT64 ||
        json_node_get_value_type(node) == G_TYPE_INT)
        return json_node_get_int(node);

    return fallback;
}

static gboolean
obj_bool(JsonObject *object, const gchar *key, gboolean fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

static JsonArray *
obj_array(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

static JsonObject *
obj_object(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static GStrv
strv_from_array(JsonArray *array)
{
    GPtrArray *out;
    guint i;
    guint length;

    if (array == NULL)
        return NULL;

    length = json_array_get_length(array);
    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);

        if (element != NULL && JSON_NODE_HOLDS_VALUE(element) &&
            json_node_get_value_type(element) == G_TYPE_STRING)
            g_ptr_array_add(out, g_strdup(json_node_get_string(element)));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

/* ── One server entry, translated ────────────────────────────────── */

/*
 * The registry's own `packageArguments` entries are either a bare string
 * or an object carrying a "value" -- the Argument schema allows both a
 * positional and a named shape, and only the value matters for a bare
 * argv this file can build without also modelling flags it does not
 * need.
 */
static void
append_package_arguments(GPtrArray *args, JsonArray *extra)
{
    guint i;
    guint length = (extra != NULL) ? json_array_get_length(extra) : 0;

    for (i = 0; i < length; i++) {
        JsonNode *node = json_array_get_element(extra, i);
        const gchar *value = NULL;

        if (node == NULL)
            continue;

        if (JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_STRING)
            value = json_node_get_string(node);
        else if (JSON_NODE_HOLDS_OBJECT(node))
            value = obj_string(json_node_get_object(node), "value");

        if (value != NULL)
            g_ptr_array_add(args, g_strdup(value));
    }
}

/*
 * Translates one package's own idea of how to run it into an argv.
 *
 * Only the three runtime hints worth guessing at are handled by name;
 * anything else falls back to the identifier as a bare command, which is
 * right for a `cargo` or `go` package naming an installed program and
 * wrong for an `npm` or `pypi` one naming a package rather than a
 * program.  Both are labelled the same way in the registry, and there is
 * no more here to go on -- see this file's header comment.
 */
static gchar *
package_command_and_args(JsonObject *package, GStrv *out_args)
{
    const gchar *runtime_hint = obj_string(package, "runtimeHint");
    const gchar *identifier = obj_string(package, "identifier");
    GPtrArray *args;
    gchar *command;

    *out_args = NULL;

    if (identifier == NULL || *identifier == '\0')
        return NULL;

    args = g_ptr_array_new_with_free_func(g_free);

    if (g_strcmp0(runtime_hint, "npx") == 0) {
        command = g_strdup("npx");
        g_ptr_array_add(args, g_strdup("-y"));
        g_ptr_array_add(args, g_strdup(identifier));
    } else if (g_strcmp0(runtime_hint, "uvx") == 0) {
        command = g_strdup("uvx");
        g_ptr_array_add(args, g_strdup(identifier));
    } else if (g_strcmp0(runtime_hint, "docker") == 0) {
        command = g_strdup("docker");
        g_ptr_array_add(args, g_strdup("run"));
        g_ptr_array_add(args, g_strdup("--rm"));
        g_ptr_array_add(args, g_strdup("-i"));
        g_ptr_array_add(args, g_strdup(identifier));
    } else {
        command = g_strdup(identifier);
    }

    append_package_arguments(args, obj_array(package, "packageArguments"));
    g_ptr_array_add(args, NULL);
    *out_args = (GStrv)g_ptr_array_free(args, FALSE);

    return command;
}

/*
 * The first environment variable worth brokering a credential for.
 *
 * A variable flagged `isSecret` wins outright.  Failing that, the first
 * merely `isRequired` one is kept as a fallback -- a package that needs
 * something but never marked it secret is still asking for a value that
 * has to come from somewhere other than the agent's own config, and
 * refusing to guess would leave every such entry looking like it needs
 * nothing at all.
 */
static const gchar *
first_credential_env(JsonObject *package)
{
    JsonArray *vars = obj_array(package, "environmentVariables");
    guint i;
    guint length = (vars != NULL) ? json_array_get_length(vars) : 0;
    const gchar *first_required = NULL;

    for (i = 0; i < length; i++) {
        JsonNode *node = json_array_get_element(vars, i);
        JsonObject *env;
        const gchar *name;

        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
            continue;

        env = json_node_get_object(node);
        name = obj_string(env, "name");

        if (name == NULL || *name == '\0')
            continue;

        if (obj_bool(env, "isSecret", FALSE))
            return name;

        if (first_required == NULL && obj_bool(env, "isRequired", FALSE))
            first_required = name;
    }

    return first_required;
}

static ClawtConnectorInfo *
entry_from_server_object(JsonObject *server, GError **error)
{
    const gchar *name = obj_string(server, "name");
    const gchar *title;
    const gchar *description;
    ClawtConnectorInfo *out;
    JsonArray *packages;
    JsonArray *remotes;
    JsonObject *first_package = NULL;
    JsonObject *first_remote = NULL;

    if (name == NULL || *name == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "a registry entry needs a name");
        return NULL;
    }

    packages = obj_array(server, "packages");

    if (packages != NULL && json_array_get_length(packages) > 0) {
        JsonNode *node = json_array_get_element(packages, 0);

        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
            first_package = json_node_get_object(node);
    }

    remotes = obj_array(server, "remotes");

    if (remotes != NULL && json_array_get_length(remotes) > 0) {
        JsonNode *node = json_array_get_element(remotes, 0);

        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
            first_remote = json_node_get_object(node);
    }

    if (first_package == NULL && first_remote == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "'%s' names neither a package nor a remote", name);
        return NULL;
    }

    out = g_new0(ClawtConnectorInfo, 1);

    /*
     * Prefixed so an imported id can never collide with one somebody --
     * or the built-in table -- chose by hand.  The registry's own names
     * are reverse-DNS strings ("io.github.owner/repo"); a short curated
     * id like "github" is never shaped like one.
     */
    out->id = g_strconcat("registry:", name, NULL);

    title = obj_string(server, "title");
    out->name = g_strdup(title != NULL ? title : name);

    description = obj_string(server, "description");
    out->summary = g_strdup(description);
    out->category = g_strdup("MCP Registry");
    out->placement = CLAWT_CREDENTIAL_PLACEMENT_ENV;

    if (first_package != NULL) {
        GStrv args = NULL;
        const gchar *credential = first_credential_env(first_package);

        out->server_command = package_command_and_args(first_package, &args);
        out->server_args = (const gchar *const *)args;

        if (credential != NULL) {
            out->auth = CLAWT_CONNECTOR_AUTH_API_KEY;
            out->credential_name = g_strdup(credential);
        }
    } else {
        out->server_url = g_strdup(obj_string(first_remote, "url"));
    }

    if (out->server_command == NULL && out->server_url == NULL) {
        clawt_connector_info_free(out);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "'%s' has a package with no usable identifier", name);
        return NULL;
    }

    return out;
}

gboolean
clawt_connector_registry_parse_page(JsonNode   *root,
                                    GPtrArray  *out_entries,
                                    gchar     **out_next_cursor,
                                    GError    **error)
{
    JsonObject *object;
    JsonArray *servers;
    guint i;
    guint length;

    g_return_val_if_fail(out_entries != NULL, FALSE);

    if (out_next_cursor != NULL)
        *out_next_cursor = NULL;

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the registry's response was not a JSON object");
        return FALSE;
    }

    object = json_node_get_object(root);
    servers = obj_array(object, "servers");

    if (servers == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the registry's response carried no `servers` "
                            "list");
        return FALSE;
    }

    length = json_array_get_length(servers);

    for (i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(servers, i);
        g_autoptr(GError) entry_error = NULL;
        ClawtConnectorInfo *entry;

        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            g_warning("mcp registry: entry %u is not an object; skipped", i);
            continue;
        }

        entry = entry_from_server_object(json_node_get_object(element),
                                         &entry_error);

        if (entry == NULL) {
            g_warning("mcp registry: entry %u skipped: %s", i,
                      entry_error != NULL ? entry_error->message
                      : "unknown reason");
            continue;
        }

        g_ptr_array_add(out_entries, entry);
    }

    if (out_next_cursor != NULL) {
        JsonObject *metadata = obj_object(object, "metadata");
        const gchar *cursor = obj_string(metadata, "nextCursor");

        if (cursor != NULL && *cursor != '\0')
            *out_next_cursor = g_strdup(cursor);
    }

    return TRUE;
}

/* ── The cache on disk ────────────────────────────────────────────── */

static JsonNode *
entry_to_json_node(const ClawtConnectorInfo *info)
{
    JsonObject *object = json_object_new();
    JsonNode *node;

    json_object_set_string_member(object, "id", info->id);

    if (info->name != NULL)
        json_object_set_string_member(object, "name", info->name);

    if (info->summary != NULL)
        json_object_set_string_member(object, "summary", info->summary);

    if (info->category != NULL)
        json_object_set_string_member(object, "category", info->category);

    json_object_set_string_member(
        object, "auth",
        clawt_enum_to_nick(CLAWT_TYPE_CONNECTOR_AUTH, (gint)info->auth));

    if (info->auth_url != NULL)
        json_object_set_string_member(object, "auth_url", info->auth_url);

    if (info->token_url != NULL)
        json_object_set_string_member(object, "token_url", info->token_url);

    if (info->revoke_url != NULL)
        json_object_set_string_member(object, "revoke_url", info->revoke_url);

    if (info->scopes != NULL)
        json_object_set_string_member(object, "scopes", info->scopes);

    if (info->client_id_help != NULL)
        json_object_set_string_member(object, "client_id_help",
                                      info->client_id_help);

    if (info->docs_url != NULL)
        json_object_set_string_member(object, "docs_url", info->docs_url);

    if (info->default_instance != NULL)
        json_object_set_string_member(object, "default_instance",
                                      info->default_instance);

    if (info->server_command != NULL)
        json_object_set_string_member(object, "server_command",
                                      info->server_command);

    if (info->server_args != NULL) {
        JsonArray *args = json_array_new();
        gsize i;

        for (i = 0; info->server_args[i] != NULL; i++)
            json_array_add_string_element(args, info->server_args[i]);

        json_object_set_array_member(object, "server_args", args);
    }

    if (info->server_url != NULL)
        json_object_set_string_member(object, "server_url", info->server_url);

    if (info->instance_var != NULL)
        json_object_set_string_member(object, "instance_var",
                                      info->instance_var);

    json_object_set_string_member(
        object, "placement",
        clawt_enum_to_nick(CLAWT_TYPE_CREDENTIAL_PLACEMENT,
                          (gint)info->placement));

    if (info->credential_name != NULL)
        json_object_set_string_member(object, "credential_name",
                                      info->credential_name);

    if (info->credential_format != NULL)
        json_object_set_string_member(object, "credential_format",
                                      info->credential_format);

    if (info->known_tools != NULL) {
        JsonArray *tools = json_array_new();
        gsize i;

        for (i = 0; info->known_tools[i] != NULL; i++)
            json_array_add_string_element(tools, info->known_tools[i]);

        json_object_set_array_member(object, "known_tools", tools);
    }

    if (info->identity_keys != NULL) {
        JsonArray *keys = json_array_new();
        gsize i;

        for (i = 0; info->identity_keys[i] != NULL; i++)
            json_array_add_string_element(keys, info->identity_keys[i]);

        json_object_set_array_member(object, "identity_keys", keys);
    }

    if (info->identity_note != NULL)
        json_object_set_string_member(object, "identity_note",
                                      info->identity_note);

    node = json_node_alloc();
    json_node_take_object(node, object);

    return node;
}

static ClawtConnectorInfo *
entry_from_cache_object(JsonObject *object)
{
    ClawtConnectorInfo *out;
    const gchar *id = obj_string(object, "id");
    const gchar *nick;
    gint value = 0;

    if (id == NULL || *id == '\0')
        return NULL;

    out = g_new0(ClawtConnectorInfo, 1);
    out->id = g_strdup(id);
    out->name = g_strdup(obj_string(object, "name"));
    out->summary = g_strdup(obj_string(object, "summary"));
    out->category = g_strdup(obj_string(object, "category"));
    out->auth_url = g_strdup(obj_string(object, "auth_url"));
    out->token_url = g_strdup(obj_string(object, "token_url"));
    out->revoke_url = g_strdup(obj_string(object, "revoke_url"));
    out->scopes = g_strdup(obj_string(object, "scopes"));
    out->client_id_help = g_strdup(obj_string(object, "client_id_help"));
    out->docs_url = g_strdup(obj_string(object, "docs_url"));
    out->default_instance = g_strdup(obj_string(object, "default_instance"));
    out->server_command = g_strdup(obj_string(object, "server_command"));
    out->server_args = (const gchar *const *)
        strv_from_array(obj_array(object, "server_args"));
    out->server_url = g_strdup(obj_string(object, "server_url"));
    out->instance_var = g_strdup(obj_string(object, "instance_var"));
    out->credential_name = g_strdup(obj_string(object, "credential_name"));
    out->credential_format = g_strdup(obj_string(object, "credential_format"));
    out->known_tools = (const gchar *const *)
        strv_from_array(obj_array(object, "known_tools"));
    out->identity_keys = (const gchar *const *)
        strv_from_array(obj_array(object, "identity_keys"));
    out->identity_note = g_strdup(obj_string(object, "identity_note"));

    if (out->name == NULL)
        out->name = g_strdup(id);

    if (out->category == NULL)
        out->category = g_strdup("MCP Registry");

    nick = obj_string(object, "auth");

    if (nick != NULL &&
        clawt_enum_from_nick(CLAWT_TYPE_CONNECTOR_AUTH, nick, &value))
        out->auth = (ClawtConnectorAuth)value;

    value = 0;
    nick = obj_string(object, "placement");

    if (nick != NULL &&
        clawt_enum_from_nick(CLAWT_TYPE_CREDENTIAL_PLACEMENT, nick, &value))
        out->placement = (ClawtCredentialPlacement)value;

    return out;
}

gchar *
clawt_connector_registry_cache_path(const gchar *state_dir)
{
    g_return_val_if_fail(state_dir != NULL, NULL);

    return g_build_filename(state_dir, "connector-registry-cache.json", NULL);
}

GPtrArray *
clawt_connector_registry_cache_load(const gchar *path, gint64 *out_fetched_at)
{
    GPtrArray *out =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    JsonObject *root;
    JsonArray *connectors;
    guint i;
    guint length;

    if (out_fetched_at != NULL)
        *out_fetched_at = 0;

    g_return_val_if_fail(path != NULL, out);

    /*
     * A cache that does not exist yet, or does not parse, is empty
     * rather than an error: nothing has been imported, which is the
     * ordinary state whenever `connectors.registry_enabled` is off.
     */
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return out;

    if (!json_parser_load_from_data(parser, text, -1, NULL))
        return out;

    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        return out;

    root = json_node_get_object(json_parser_get_root(parser));

    if (out_fetched_at != NULL)
        *out_fetched_at = obj_int(root, "fetched_at", 0);

    connectors = obj_array(root, "connectors");

    if (connectors == NULL)
        return out;

    length = json_array_get_length(connectors);

    for (i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(connectors, i);
        ClawtConnectorInfo *entry;

        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element))
            continue;

        entry = entry_from_cache_object(json_node_get_object(element));

        if (entry != NULL)
            g_ptr_array_add(out, entry);
    }

    return out;
}

gboolean
clawt_connector_registry_cache_save(const gchar *path, GPtrArray *entries,
                                    gint64 fetched_at, GError **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *text = NULL;
    gsize length = 0;
    guint i;

    g_return_val_if_fail(path != NULL, FALSE);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "fetched_at");
    json_builder_add_int_value(builder, fetched_at);

    json_builder_set_member_name(builder, "connectors");
    json_builder_begin_array(builder);

    for (i = 0; entries != NULL && i < entries->len; i++)
        json_builder_add_value(builder,
                               entry_to_json_node(g_ptr_array_index(entries,
                                                                    i)));

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    text = json_generator_to_data(generator, &length);

    return clawt_write_file_atomic(path, text, (gssize)length, 0644, FALSE,
                                   error);
}

/* ── Merging into a live catalogue ───────────────────────────────── */

void
clawt_connector_catalog_merge_registry(GPtrArray *catalog,
                                       GPtrArray *registry_entries)
{
    guint i;

    g_return_if_fail(catalog != NULL);

    for (i = 0; registry_entries != NULL && i < registry_entries->len; i++) {
        const ClawtConnectorInfo *entry = g_ptr_array_index(registry_entries,
                                                            i);

        /*
         * The built-in table and a person's own overlay are both a
         * choice somebody made on purpose.  An imported listing is
         * neither, so it fills a gap and never overrides one -- the
         * opposite rule from an overlay file, which replaces a built-in
         * entry wholesale because that override *is* the point of
         * writing one.
         */
        if (clawt_connector_catalog_find(catalog, entry->id) != NULL)
            continue;

        g_ptr_array_add(catalog, clawt_connector_info_copy(entry));
    }
}

/* ── Fetching it, paginated and never on a request's own thread ────── */

typedef struct {
    gchar        *base_url;
    gchar        *cache_path;
    gchar        *updated_since;  /* NULL for a first-ever refresh */
    gchar        *cursor;         /* NULL for the first page of this run */
    gint64        started_at;
    GCancellable *cancellable;
    SoupSession  *session;
    GHashTable   *merged;         /* id -> ClawtConnectorInfo* (owned) */
    GTask        *task;
} RegistryRefresh;

static void registry_fetch_page(RegistryRefresh *self);

static void
registry_refresh_free(RegistryRefresh *self)
{
    if (self == NULL)
        return;

    g_free(self->base_url);
    g_free(self->cache_path);
    g_free(self->updated_since);
    g_free(self->cursor);
    g_clear_object(&self->cancellable);
    g_clear_object(&self->session);

    if (self->merged != NULL)
        g_hash_table_unref(self->merged);

    g_free(self);
}

/*
 * Replaces whatever the merge already has for this id, freeing the
 * value that loses -- the hash table's own value-destroy is left unset
 * because a straight destroy-on-replace would also fire during the
 * final handoff into the array this refresh returns.
 */
static void
merged_put(GHashTable *merged, ClawtConnectorInfo *entry)
{
    ClawtConnectorInfo *old = g_hash_table_lookup(merged, entry->id);

    if (old != NULL && old != entry)
        clawt_connector_info_free(old);

    g_hash_table_replace(merged, g_strdup(entry->id), entry);
}

static void
registry_refresh_done(RegistryRefresh *self, GError *error)
{
    if (error != NULL) {
        g_task_return_error(self->task, error);
    } else {
        g_autoptr(GPtrArray) final =
            g_ptr_array_new_with_free_func((GDestroyNotify)
                                           clawt_connector_info_free);
        g_autoptr(GError) save_error = NULL;
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init(&iter, self->merged);

        while (g_hash_table_iter_next(&iter, &key, &value))
            g_ptr_array_add(final, value);

        if (!clawt_connector_registry_cache_save(self->cache_path, final,
                                                 self->started_at,
                                                 &save_error))
            g_task_return_error(self->task, g_steal_pointer(&save_error));
        else
            g_task_return_int(self->task, (gssize)final->len);
    }

    g_object_unref(self->task);
    registry_refresh_free(self);
}

static void
on_registry_page(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RegistryRefresh *self = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GPtrArray) page_entries = NULL;
    g_autofree gchar *next_cursor = NULL;
    const gchar *text;
    gsize length = 0;
    guint i;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        registry_refresh_done(self, g_steal_pointer(&error));
        return;
    }

    text = g_bytes_get_data(body, &length);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, text, (gssize)length, &error)) {
        registry_refresh_done(self, g_steal_pointer(&error));
        return;
    }

    /* No free func: ownership of each entry moves into self->merged
     * below, and the array itself is discarded once that is done. */
    page_entries = g_ptr_array_new();

    if (!clawt_connector_registry_parse_page(json_parser_get_root(parser),
                                             page_entries, &next_cursor,
                                             &error)) {
        registry_refresh_done(self, g_steal_pointer(&error));
        return;
    }

    for (i = 0; i < page_entries->len; i++)
        merged_put(self->merged, g_ptr_array_index(page_entries, i));

    g_free(self->cursor);
    self->cursor = g_steal_pointer(&next_cursor);

    if (self->cursor != NULL) {
        registry_fetch_page(self);
        return;
    }

    registry_refresh_done(self, NULL);
}

static void
registry_fetch_page(RegistryRefresh *self)
{
    g_autoptr(SoupMessage) message = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *query_str = NULL;
    GString *query = g_string_new(NULL);

    g_string_append_printf(query, "?limit=%d", REGISTRY_PAGE_LIMIT);

    if (self->cursor != NULL) {
        g_autofree gchar *escaped = g_uri_escape_string(self->cursor, NULL,
                                                        FALSE);

        g_string_append_printf(query, "&cursor=%s", escaped);
    }

    if (self->updated_since != NULL) {
        g_autofree gchar *escaped = g_uri_escape_string(self->updated_since,
                                                        NULL, FALSE);

        g_string_append_printf(query, "&updated_since=%s", escaped);
    }

    query_str = g_string_free(query, FALSE);
    url = g_strconcat(self->base_url, "/v0.1/servers", query_str, NULL);

    message = soup_message_new("GET", url);

    if (message == NULL) {
        GError *error = NULL;

        g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "'%s' is not a URL that can be dialled", url);
        registry_refresh_done(self, error);
        return;
    }

    soup_session_send_and_read_async(self->session, message,
                                     G_PRIORITY_DEFAULT, self->cancellable,
                                     on_registry_page, self);
}

void
clawt_connector_registry_refresh_async(const gchar         *base_url,
                                       const gchar         *cache_path,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    RegistryRefresh *self;
    g_autoptr(GPtrArray) existing = NULL;
    gint64 fetched_at = 0;
    guint i;

    g_return_if_fail(base_url != NULL);
    g_return_if_fail(cache_path != NULL);

    self = g_new0(RegistryRefresh, 1);
    self->base_url = g_strdup(base_url);
    self->cache_path = g_strdup(cache_path);
    self->cancellable = (cancellable != NULL) ? g_object_ref(cancellable)
                                              : NULL;
    self->session = soup_session_new_with_options(
        "user-agent", USER_AGENT, "timeout", REGISTRY_TIMEOUT_SECONDS, NULL);
    self->merged = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    self->started_at = g_get_real_time() / G_USEC_PER_SEC;
    self->task = g_task_new(NULL, cancellable, callback, user_data);

    existing = clawt_connector_registry_cache_load(cache_path, &fetched_at);

    for (i = 0; i < existing->len; i++)
        merged_put(self->merged,
                  clawt_connector_info_copy(g_ptr_array_index(existing, i)));

    /*
     * A first-ever refresh asks for everything; every one after that
     * asks only for what changed since the last one completed, so a
     * registry with ten thousand servers costs one full walk and every
     * refresh after costs only the difference.
     */
    self->updated_since = clawt_connector_registry_updated_since_for(fetched_at);

    registry_fetch_page(self);
}

gchar *
clawt_connector_registry_updated_since_for(gint64 fetched_at)
{
    g_autoptr(GDateTime) at = NULL;

    if (fetched_at <= 0)
        return NULL;

    at = g_date_time_new_from_unix_utc(fetched_at);

    return g_date_time_format_iso8601(at);
}

gboolean
clawt_connector_registry_refresh_finish(GAsyncResult  *result,
                                        guint         *out_imported,
                                        GError       **error)
{
    gssize count;

    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

    count = g_task_propagate_int(G_TASK(result), error);

    if (count < 0)
        return FALSE;

    if (out_imported != NULL)
        *out_imported = (guint)count;

    return TRUE;
}
