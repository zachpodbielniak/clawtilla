/*
 * clawt-plugin-manager.c - Finding, loading and running plugins
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "plugin/clawt-plugin-manager.h"
#include "interfaces/clawt-computer-provider.h"
#include "interfaces/clawt-event-handler.h"
#include "interfaces/clawt-integration-provider.h"
#include "interfaces/clawt-tool-provider.h"

#include <string.h>

typedef GType (*ClawtPluginRegisterFunc)(void);

struct _ClawtPluginManager {
    GObject parent_instance;

    ClawtConfig   *config;
    GHashTable    *plugins;    /* id -> ClawtPlugin (owned) */
    GPtrArray     *order;      /* ids, load order */
    GHashTable    *services;   /* name -> GObject, unowned values */
    ClawtEventBus *bus;
    gulong         bus_handler;
};

G_DEFINE_FINAL_TYPE(ClawtPluginManager, clawt_plugin_manager, G_TYPE_OBJECT)

ClawtPluginManager *
clawt_plugin_manager_new(ClawtConfig *config)
{
    ClawtPluginManager *self = g_object_new(CLAWT_TYPE_PLUGIN_MANAGER, NULL);

    if (config != NULL)
        self->config = g_object_ref(config);

    return self;
}

void
clawt_plugin_manager_add_service(ClawtPluginManager *self, const gchar *name,
                                 GObject *service)
{
    g_return_if_fail(CLAWT_IS_PLUGIN_MANAGER(self));
    g_return_if_fail(name != NULL);

    g_hash_table_replace(self->services, g_strdup(name), service);
}

/*
 * The plugin's id comes from its filename: libclawt-plugin-<id>.so.
 *
 * Taking it from the file rather than from the plugin itself is what makes
 * `plugins.<id>` and `plugins.disabled` work for a plugin the core has
 * never heard of -- the id exists before the module is opened, so a
 * disabled plugin is never loaded at all.
 */
static gchar *
id_from_filename(const gchar *path)
{
    g_autofree gchar *base = g_path_get_basename(path);
    const gchar *start;
    const gchar *end;

    if (!g_str_has_prefix(base, "libclawt-plugin-"))
        return NULL;

    start = base + strlen("libclawt-plugin-");
    end = g_strrstr(start, ".so");

    if (end == NULL || end == start)
        return NULL;

    return g_strndup(start, (gsize)(end - start));
}

static gboolean
is_disabled(ClawtPluginManager *self, const gchar *plugin_id)
{
    g_auto(GStrv) disabled = NULL;
    g_autofree gchar *key = NULL;
    gsize i;

    if (self->config == NULL)
        return FALSE;

    disabled = clawt_config_get_string_list(self->config, "plugins.disabled");

    for (i = 0; disabled != NULL && disabled[i] != NULL; i++) {
        if (g_strcmp0(disabled[i], plugin_id) == 0)
            return TRUE;
    }

    /*
     * `plugins.<id>.enabled: false` also disables, by a generic keyed
     * lookup rather than a list of known plugin names.  A third-party
     * plugin the core has never heard of has to be switchable off the same
     * way the bundled ones are.
     */
    key = g_strdup_printf("plugins.%s.enabled", plugin_id);

    if (clawt_config_has_key(self->config, key))
        return !clawt_config_get_boolean(self->config, key);

    return FALSE;
}

static GHashTable *
settings_for(ClawtPluginManager *self, const gchar *plugin_id)
{
    GHashTable *settings = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_free);
    static const gchar *const common[] = { "endpoint", "token_file", "path",
                                           "mode", NULL };
    gsize i;

    if (self->config == NULL)
        return settings;

    for (i = 0; common[i] != NULL; i++) {
        g_autofree gchar *key = g_strdup_printf("plugins.%s.%s", plugin_id,
                                                common[i]);
        const gchar *value = clawt_config_get_string(self->config, key);

        if (value != NULL)
            g_hash_table_insert(settings, g_strdup(common[i]),
                                g_strdup(value));
    }

    return settings;
}

ClawtPlugin *
clawt_plugin_manager_load_file(ClawtPluginManager *self, const gchar *path,
                               GError **error)
{
    g_autofree gchar *plugin_id = NULL;
    g_autoptr(GHashTable) settings = NULL;
    GModule *module;
    ClawtPluginRegisterFunc register_func = NULL;
    gpointer abi_symbol = NULL;
    GType plugin_type;
    ClawtPlugin *plugin;

    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), NULL);
    g_return_val_if_fail(path != NULL, NULL);

    plugin_id = id_from_filename(path);

    if (plugin_id == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD,
                    "%s is not named libclawt-plugin-<id>.so, so its id "
                    "cannot be worked out", path);
        return NULL;
    }

    if (g_hash_table_contains(self->plugins, plugin_id)) {
        /*
         * The first one found wins, so a plugin in the user's directory
         * shadows the system copy rather than fighting it.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a plugin called '%s' is already loaded", plugin_id);
        return NULL;
    }

    module = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);

    if (module == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD,
                    "could not open %s: %s", path, g_module_error());
        return NULL;
    }

    /*
     * The ABI check comes before anything is called.  Loading a plugin
     * built against a different vtable does not fail cleanly: it calls
     * through pointers at the wrong offsets, and the crash lands somewhere
     * unrelated.
     */
    if (!g_module_symbol(module, CLAWT_PLUGIN_ABI_SYMBOL, &abi_symbol) ||
        abi_symbol == NULL) {
        g_module_close(module);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_ABI,
                    "%s does not export %s, so it cannot be checked for "
                    "compatibility", path, CLAWT_PLUGIN_ABI_SYMBOL);
        return NULL;
    }

    if (*(const guint *)abi_symbol != CLAWT_PLUGIN_ABI_VERSION) {
        guint found = *(const guint *)abi_symbol;

        g_module_close(module);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_ABI,
                    "%s was built against plugin ABI %u; this build speaks "
                    "%d -- rebuild the plugin", path, found,
                    CLAWT_PLUGIN_ABI_VERSION);
        return NULL;
    }

    if (!g_module_symbol(module, CLAWT_PLUGIN_REGISTER_SYMBOL,
                         (gpointer *)&register_func) ||
        register_func == NULL) {
        g_module_close(module);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD,
                    "%s does not export %s", path,
                    CLAWT_PLUGIN_REGISTER_SYMBOL);
        return NULL;
    }

    plugin_type = register_func();

    if (plugin_type == G_TYPE_INVALID ||
        !g_type_is_a(plugin_type, CLAWT_TYPE_PLUGIN)) {
        g_module_close(module);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD,
                    "%s registered '%s', which is not a ClawtPlugin", path,
                    plugin_type != G_TYPE_INVALID
                        ? g_type_name(plugin_type) : "nothing");
        return NULL;
    }

    /*
     * Made resident deliberately.  The GTypes the plugin registered live
     * for the life of the process, and unmapping the code behind them
     * turns the next type lookup into a jump into freed memory.
     */
    g_module_make_resident(module);

    plugin = g_object_new(plugin_type, NULL);
    clawt_plugin_set_id(plugin, plugin_id);
    clawt_plugin_set_services(plugin, self->services);

    settings = settings_for(self, plugin_id);

    if (!clawt_plugin_configure(plugin, settings, error)) {
        g_object_unref(plugin);
        return NULL;
    }

    if (!clawt_plugin_activate(plugin, error)) {
        g_object_unref(plugin);
        return NULL;
    }

    g_hash_table_insert(self->plugins, g_strdup(plugin_id), plugin);
    g_ptr_array_add(self->order, g_strdup(plugin_id));

    g_info("plugin: loaded %s (%s %s)", plugin_id,
           clawt_plugin_get_name(plugin), clawt_plugin_get_version(plugin));

    return plugin;
}

static guint
load_directory(ClawtPluginManager *self, const gchar *dir_path)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;
    guint loaded = 0;

    dir = g_dir_open(dir_path, 0, NULL);

    if (dir == NULL)
        return 0;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *plugin_id = NULL;
        g_autoptr(GError) error = NULL;

        if (!g_str_has_prefix(name, "libclawt-plugin-") ||
            !g_str_has_suffix(name, ".so"))
            continue;

        path = g_build_filename(dir_path, name, NULL);
        plugin_id = id_from_filename(path);

        /*
         * Checked before opening, so a disabled plugin's code never runs
         * at all.  Loading it and then not activating it would still give
         * it a constructor and a chance to misbehave.
         */
        if (plugin_id != NULL && is_disabled(self, plugin_id)) {
            g_info("plugin: %s is disabled by configuration", plugin_id);
            continue;
        }

        if (clawt_plugin_manager_load_file(self, path, &error) != NULL)
            loaded++;
        else
            /*
             * One bad plugin disables itself and nothing else.  A daemon
             * that refuses to start because a third-party plugin is stale
             * would be a poor trade.
             */
            g_warning("plugin: %s was not loaded: %s", name, error->message);
    }

    return loaded;
}

guint
clawt_plugin_manager_load_all(ClawtPluginManager *self)
{
    g_autoptr(GPtrArray) dirs = NULL;
    const gchar *env_path;
    guint loaded = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), 0);

    if (self->config != NULL &&
        clawt_config_has_key(self->config, "plugins.enabled") &&
        !clawt_config_get_boolean(self->config, "plugins.enabled"))
        return 0;

    dirs = g_ptr_array_new_with_free_func(g_free);

    /* Highest priority first: an override has to be able to win. */
    env_path = g_getenv("CLAWT_PLUGIN_PATH");

    if (env_path != NULL) {
        g_auto(GStrv) parts = g_strsplit(env_path, ":", -1);
        gsize j;

        for (j = 0; parts[j] != NULL; j++) {
            if (*parts[j] != '\0')
                g_ptr_array_add(dirs, clawt_expand_path(parts[j]));
        }
    }

    if (self->config != NULL) {
        g_auto(GStrv) configured =
            clawt_config_get_string_list(self->config, "plugins.dirs");
        gsize j;

        for (j = 0; configured != NULL && configured[j] != NULL; j++)
            g_ptr_array_add(dirs, clawt_expand_path(configured[j]));
    }

    g_ptr_array_add(dirs, g_build_filename(g_get_user_config_dir(),
                                           "clawtilla", "plugins", NULL));
    g_ptr_array_add(dirs, g_strdup(CLAWT_PLUGIN_DIR));

    for (i = 0; i < dirs->len; i++)
        loaded += load_directory(self, g_ptr_array_index(dirs, i));

    return loaded;
}

GPtrArray *
clawt_plugin_manager_list(ClawtPluginManager *self)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), NULL);

    out = g_ptr_array_new();

    for (i = 0; i < self->order->len; i++) {
        ClawtPlugin *plugin = g_hash_table_lookup(
            self->plugins, g_ptr_array_index(self->order, i));

        if (plugin != NULL)
            g_ptr_array_add(out, plugin);
    }

    return out;
}

ClawtPlugin *
clawt_plugin_manager_get(ClawtPluginManager *self, const gchar *plugin_id)
{
    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), NULL);

    if (plugin_id == NULL)
        return NULL;

    return g_hash_table_lookup(self->plugins, plugin_id);
}

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtPluginManager *self = user_data;
    g_autoptr(GPtrArray) plugins = NULL;
    guint i;

    (void)bus;

    plugins = clawt_plugin_manager_list(self);

    for (i = 0; i < plugins->len; i++) {
        ClawtPlugin *plugin = g_ptr_array_index(plugins, i);

        if (!CLAWT_IS_EVENT_HANDLER(plugin))
            continue;

        if (!clawt_event_handler_handles(CLAWT_EVENT_HANDLER(plugin),
                                         clawt_event_get_kind(event)))
            continue;

        clawt_event_handler_handle(CLAWT_EVENT_HANDLER(plugin), event);
    }
}

void
clawt_plugin_manager_attach_bus(ClawtPluginManager *self, ClawtEventBus *bus)
{
    g_return_if_fail(CLAWT_IS_PLUGIN_MANAGER(self));
    g_return_if_fail(CLAWT_IS_EVENT_BUS(bus));

    if (self->bus != NULL && self->bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->bus_handler);

    /*
     * A reference rather than a borrowed pointer.  The handler has to be
     * disconnected at dispose time, and disconnecting from a bus that was
     * finalized first is a use-after-free -- which is exactly what happens
     * when a caller's cleanup order differs from the daemon's.
     */
    g_clear_object(&self->bus);
    self->bus = g_object_ref(bus);
    self->bus_handler = g_signal_connect(bus, "event",
                                         G_CALLBACK(on_bus_event), self);
}

GObject *
clawt_plugin_manager_find_computer_provider(ClawtPluginManager *self,
                                            const gchar *type_name)
{
    g_autoptr(GPtrArray) plugins = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), NULL);

    if (type_name == NULL)
        return NULL;

    plugins = clawt_plugin_manager_list(self);

    for (i = 0; i < plugins->len; i++) {
        ClawtPlugin *plugin = g_ptr_array_index(plugins, i);

        if (!CLAWT_IS_COMPUTER_PROVIDER(plugin))
            continue;

        if (g_strcmp0(clawt_computer_provider_get_type_name(
                          CLAWT_COMPUTER_PROVIDER(plugin)), type_name) == 0)
            return G_OBJECT(plugin);
    }

    return NULL;
}

GPtrArray *
clawt_plugin_manager_tool_providers(ClawtPluginManager *self)
{
    g_autoptr(GPtrArray) plugins = NULL;
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_PLUGIN_MANAGER(self), NULL);

    out = g_ptr_array_new();
    plugins = clawt_plugin_manager_list(self);

    for (i = 0; i < plugins->len; i++) {
        ClawtPlugin *plugin = g_ptr_array_index(plugins, i);

        if (CLAWT_IS_TOOL_PROVIDER(plugin))
            g_ptr_array_add(out, plugin);
    }

    return out;
}

void
clawt_plugin_manager_unload_all(ClawtPluginManager *self)
{
    g_autoptr(GPtrArray) plugins = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_PLUGIN_MANAGER(self));

    plugins = clawt_plugin_manager_list(self);

    for (i = 0; i < plugins->len; i++)
        clawt_plugin_deactivate(g_ptr_array_index(plugins, i));

    g_hash_table_remove_all(self->plugins);
    g_ptr_array_set_size(self->order, 0);
}

static void
clawt_plugin_manager_dispose(GObject *object)
{
    ClawtPluginManager *self = CLAWT_PLUGIN_MANAGER(object);

    if (self->bus != NULL && self->bus_handler != 0) {
        g_signal_handler_disconnect(self->bus, self->bus_handler);
        self->bus_handler = 0;
    }

    g_clear_object(&self->bus);

    clawt_plugin_manager_unload_all(self);
    g_clear_object(&self->config);

    G_OBJECT_CLASS(clawt_plugin_manager_parent_class)->dispose(object);
}

static void
clawt_plugin_manager_finalize(GObject *object)
{
    ClawtPluginManager *self = CLAWT_PLUGIN_MANAGER(object);

    g_clear_pointer(&self->plugins, g_hash_table_unref);
    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_clear_pointer(&self->services, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_plugin_manager_parent_class)->finalize(object);
}

static void
clawt_plugin_manager_class_init(ClawtPluginManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_plugin_manager_dispose;
    object_class->finalize = clawt_plugin_manager_finalize;
}

static void
clawt_plugin_manager_init(ClawtPluginManager *self)
{
    self->plugins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_object_unref);
    self->order = g_ptr_array_new_with_free_func(g_free);
    self->services = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
}
