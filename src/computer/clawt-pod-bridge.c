/*
 * clawt-pod-bridge.c - Talking to podomation's modules
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-pod-bridge.h"

#include <podomation.h>
#include <gmodule.h>

struct _ClawtPodBridge {
    GObject parent_instance;

    GStrv       search_path;
    GHashTable *modules;   /* "name" or "name@uri" -> PodModule* (owned) */
};

G_DEFINE_FINAL_TYPE(ClawtPodBridge, clawt_pod_bridge, G_TYPE_OBJECT)

/*
 * Where the binary that is running lives.
 *
 * Used so an uninstalled clawtilla finds the modules the build just
 * produced.  Returns NULL when /proc is not available, which only costs
 * that one entry in the search path.
 */
static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL)
        return NULL;

    return g_path_get_dirname(exe);
}

/*
 * Builds the list of directories a module is looked for in, best first.
 *
 * There was one directory before, fixed at compile time to the install
 * prefix, and a comment claiming it was the build tree.  It was not, so
 * `container` and `vm_virtmanager` were missing for anyone running
 * straight out of a checkout -- which is everyone, until the first
 * `make install`.
 */
static GStrv
default_search_path(void)
{
    g_autoptr(GPtrArray) dirs = g_ptr_array_new_with_free_func(g_free);
    const gchar *env_path = g_getenv("CLAWT_POD_MODULE_DIR");
    g_autofree gchar *exe_dir = NULL;

    /* An override has to be able to win, so it goes first. */
    if (env_path != NULL) {
        g_auto(GStrv) parts = g_strsplit(env_path, ":", -1);
        gsize i;

        for (i = 0; parts[i] != NULL; i++) {
            if (*parts[i] != '\0')
                g_ptr_array_add(dirs, clawt_expand_path(parts[i]));
        }
    }

    exe_dir = executable_dir();

    if (exe_dir != NULL) {
        g_ptr_array_add(dirs, g_build_filename(exe_dir, "pod-modules", NULL));

        /*
         * Where libreclaw itself drops them.  Worth trying because a
         * checkout that has built libreclaw but not staged the modules
         * still has a complete set sitting right there.
         */
        g_ptr_array_add(dirs, g_build_filename(exe_dir, "modules", NULL));
    }

    g_ptr_array_add(dirs, g_strdup(CLAWT_POD_MODULE_DIR));
    g_ptr_array_add(dirs, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&dirs), FALSE);
}

ClawtPodBridge *
clawt_pod_bridge_new(const gchar *module_dir)
{
    ClawtPodBridge *self = g_object_new(CLAWT_TYPE_POD_BRIDGE, NULL);

    if (module_dir != NULL) {
        /*
         * Named explicitly, so it is the only place looked at.  A caller
         * that says where the modules are is answered literally rather
         * than quietly succeeding from somewhere else -- that is what
         * makes the setting testable and its failure legible.
         */
        self->search_path = g_new0(gchar *, 2);
        self->search_path[0] = clawt_expand_path(module_dir);
    } else {
        self->search_path = default_search_path();
    }

    return self;
}

const gchar * const *
clawt_pod_bridge_get_search_path(ClawtPodBridge *self)
{
    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(self), NULL);

    return (const gchar * const *)self->search_path;
}

/* Returns the first directory that holds the module, or NULL. */
static gchar *
find_module(ClawtPodBridge *self, const gchar *module_name)
{
    gsize i;

    for (i = 0; self->search_path[i] != NULL; i++) {
        g_autofree gchar *path = g_strdup_printf(
            "%s/libpod-module-%s.so", self->search_path[i], module_name);

        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return g_steal_pointer(&path);
    }

    return NULL;
}

/*
 * The cache key.
 *
 * A module instance carries its connection, so two agents pointed at
 * different podman sockets need two instances -- sharing one meant the
 * second agent silently talked to the first one's daemon.
 */
static gchar *
instance_key(const gchar *module_name, const gchar *connection_uri)
{
    if (connection_uri == NULL || *connection_uri == '\0')
        return g_strdup(module_name);

    return g_strdup_printf("%s@%s", module_name, connection_uri);
}

gboolean
clawt_pod_bridge_load_module(ClawtPodBridge  *self,
                             const gchar     *module_name,
                             GError         **error)
{
    return clawt_pod_bridge_load_module_for(self, module_name, NULL, error);
}

gboolean
clawt_pod_bridge_load_module_for(ClawtPodBridge  *self,
                                 const gchar     *module_name,
                                 const gchar     *connection_uri,
                                 GError         **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *key = NULL;
    GModule *module;
    GType (*register_func)(void);
    gpointer symbol = NULL;
    PodModule *instance;
    GType module_type;

    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(self), FALSE);
    g_return_val_if_fail(module_name != NULL, FALSE);

    key = instance_key(module_name, connection_uri);

    if (g_hash_table_contains(self->modules, key))
        return TRUE;

    path = find_module(self, module_name);

    if (path == NULL) {
        /*
         * Names every directory tried.  The old message named one and
         * read as if that were the only place it could be, which sent
         * people off to create a system directory when staging the
         * build tree or setting CLAWT_POD_MODULE_DIR was the answer.
         */
        g_autofree gchar *tried = g_strjoinv(", ", self->search_path);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this computer backend needs podomation's '%s' module, "
                    "and there is no libpod-module-%s.so in any of: %s. "
                    "Build it with `make -C deps/libreclaw pod-modules`, or "
                    "point daemon.pod_module_dir at an existing set.",
                    module_name, module_name, tried);
        return FALSE;
    }

    module = g_module_open(path, G_MODULE_BIND_LAZY);
    if (module == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "could not load %s: %s", path, g_module_error());
        return FALSE;
    }

    if (!g_module_symbol(module, "pod_module_register", &symbol) ||
        symbol == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s does not export pod_module_register", path);
        g_module_close(module);
        return FALSE;
    }

    register_func = symbol;
    module_type = register_func();

    if (!g_type_is_a(module_type, POD_TYPE_MODULE)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s did not register a podomation module", path);
        g_module_close(module);
        return FALSE;
    }

    /*
     * Resident, because the GTypes it registered outlive any point at which
     * we might unload it -- and unloading a module whose types are still
     * referenced crashes at the next type lookup.
     */
    g_module_make_resident(module);

    {
        /*
         * The class has to be referenced before its properties can be
         * looked up -- g_type_class_peek() returns NULL until somebody
         * does, and the first instance of this type is created below.
         *
         * Checked rather than passed blind: g_object_new() warns loudly
         * about a property a module does not have.
         *
         * The two modules clawtilla uses do not agree on the name.
         * `container` calls it connection-uri and `vm_virtmanager` calls
         * it uri, and looking for only the first meant every VM was
         * created against vm_virtmanager's default of qemu:///system --
         * which an unprivileged user cannot reach, whatever
         * computer.vm.uri said.
         */
        static const gchar *const uri_properties[] = {
            "connection-uri", "uri", NULL
        };
        GObjectClass *klass = g_type_class_ref(module_type);
        const gchar *uri_property = NULL;
        gsize i;

        if (connection_uri != NULL && *connection_uri != '\0') {
            for (i = 0; uri_properties[i] != NULL; i++) {
                if (g_object_class_find_property(klass,
                                                 uri_properties[i]) != NULL) {
                    uri_property = uri_properties[i];
                    break;
                }
            }
        }

        instance = uri_property != NULL
                   ? g_object_new(module_type,
                                  uri_property, connection_uri, NULL)
                   : g_object_new(module_type, NULL);

        g_type_class_unref(klass);
    }

    /*
     * A module that is an event source opens its connection when it is
     * started, not when it is constructed.  vm_virtmanager is one, so
     * without this it holds a URI and no connection, and every action on
     * it fails with "not connected to libvirt" -- including the
     * define_xml that creates the domain, which is why a VM agent could
     * not be provisioned at all.
     *
     * Failure to start is failure to load: a module that cannot reach its
     * backend answers every call with the same unhelpful refusal, and
     * saying so here names the URI that could not be reached.
     */
    if (POD_IS_EVENT_SOURCE(instance)) {
        g_autoptr(GError) start_error = NULL;

        if (!pod_event_source_start(POD_EVENT_SOURCE(instance),
                                    g_main_context_get_thread_default(),
                                    &start_error)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                        "the podomation '%s' module could not reach %s: %s",
                        module_name,
                        connection_uri != NULL ? connection_uri
                                               : "its default backend",
                        start_error->message);
            g_object_unref(instance);
            return FALSE;
        }
    }

    g_hash_table_insert(self->modules, g_steal_pointer(&key), instance);

    return TRUE;
}

gboolean
clawt_pod_bridge_has_module(ClawtPodBridge *self, const gchar *module_name)
{
    return clawt_pod_bridge_has_module_for(self, module_name, NULL);
}

gboolean
clawt_pod_bridge_has_module_for(ClawtPodBridge *self,
                                const gchar    *module_name,
                                const gchar    *connection_uri)
{
    g_autofree gchar *key = NULL;

    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(self), FALSE);

    key = instance_key(module_name, connection_uri);

    return g_hash_table_contains(self->modules, key);
}

/*
 * podomation speaks GVariant dictionaries.  Converting at this boundary
 * keeps every caller in ordinary C types rather than spreading GVariant
 * building across the computer backends.
 */
static GVariant *
params_to_variant(GHashTable *params)
{
    GVariantBuilder builder;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    if (params != NULL) {
        g_hash_table_iter_init(&iter, params);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            if (value == NULL)
                continue;

            g_variant_builder_add(&builder, "{sv}", (const gchar *)key,
                                  g_variant_new_string((const gchar *)value));
        }
    }

    return g_variant_builder_end(&builder);
}

static GHashTable *
variant_to_result(GVariant *variant)
{
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    GVariantIter iter;
    gchar *key;
    GVariant *value;

    if (variant == NULL ||
        !g_variant_is_of_type(variant, G_VARIANT_TYPE("a{sv}")))
        return out;

    g_variant_iter_init(&iter, variant);
    while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
        g_autofree gchar *text = NULL;

        if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
            text = g_variant_dup_string(value, NULL);
        else
            text = g_variant_print(value, FALSE);

        g_hash_table_insert(out, key, g_steal_pointer(&text));
        g_variant_unref(value);
    }

    return out;
}

GHashTable *
clawt_pod_bridge_call(ClawtPodBridge  *self,
                      const gchar     *module_name,
                      const gchar     *action,
                      GHashTable      *params,
                      GError         **error)
{
    return clawt_pod_bridge_call_for(self, module_name, NULL, action, params,
                                     error);
}

GHashTable *
clawt_pod_bridge_call_for(ClawtPodBridge  *self,
                          const gchar     *module_name,
                          const gchar     *connection_uri,
                          const gchar     *action,
                          GHashTable      *params,
                          GError         **error)
{
    PodModule *module;
    g_autofree gchar *key = NULL;
    g_autoptr(GVariant) variant_params = NULL;
    GVariant *result_variant = NULL;
    GHashTable *result;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(self), NULL);
    g_return_val_if_fail(action != NULL, NULL);

    key = instance_key(module_name, connection_uri);
    module = g_hash_table_lookup(self->modules, key);

    /*
     * Load it if nobody has.  This used to refuse, and the refusal was
     * reached by the most ordinary path there is.
     *
     * The container backend loaded its module in exactly one place --
     * container_provision() -- so start, stop, exec and teardown all
     * depended on a side effect of a function that may never have run
     * in this daemon's lifetime.  Removing a *stopped* agent therefore
     * answered "the podomation 'container' module is not loaded" and
     * left the container on the machine under a name nothing refers to
     * any more.  A daemon restart is enough to reach it, and a podman
     * container outlives the daemon by design, so this was the common
     * case rather than an edge one.
     *
     * Done here rather than at each call site because a new call site
     * cannot forget what it does not have to remember -- the same
     * reason the mount tag and the direct-room id each have one
     * spelling.  load_module_for() is idempotent and takes exactly the
     * arguments this function already holds, so the loaded case costs a
     * hash lookup.
     *
     * A failure to load is reported as itself: it already names every
     * directory searched and what to build, which is a great deal more
     * use than the sentence it replaces.
     */
    if (module == NULL) {
        if (!clawt_pod_bridge_load_module_for(self, module_name,
                                              connection_uri, error))
            return NULL;

        module = g_hash_table_lookup(self->modules, key);
    }

    if (module == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the podomation '%s' module is not loaded", module_name);
        return NULL;
    }

    if (!POD_IS_EVENT_HANDLER(module)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the '%s' module handles no actions", module_name);
        return NULL;
    }

    variant_params = g_variant_ref_sink(params_to_variant(params));

    ok = pod_event_handler_handle_event(POD_EVENT_HANDLER(module), action,
                                        NULL, variant_params,
                                        &result_variant);

    result = variant_to_result(result_variant);

    if (result_variant != NULL)
        g_variant_unref(result_variant);

    if (!ok) {
        const gchar *detail = g_hash_table_lookup(result, "error");

        /*
         * podomation reports failure as a boolean plus an "error" key.
         * Turning that into a GError here means the backends above never
         * have to remember to look for it.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "%s %s failed: %s", module_name, action,
                    detail != NULL ? detail : "no reason given");

        g_hash_table_unref(result);
        return NULL;
    }

    return result;
}

const gchar *
clawt_pod_bridge_get_module_dir(ClawtPodBridge *self)
{
    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(self), NULL);

    return (self->search_path != NULL) ? self->search_path[0] : NULL;
}

static void
clawt_pod_bridge_finalize(GObject *object)
{
    ClawtPodBridge *self = CLAWT_POD_BRIDGE(object);

    g_clear_pointer(&self->modules, g_hash_table_unref);
    g_clear_pointer(&self->search_path, g_strfreev);

    G_OBJECT_CLASS(clawt_pod_bridge_parent_class)->finalize(object);
}

static void
clawt_pod_bridge_class_init(ClawtPodBridgeClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_pod_bridge_finalize;
}

static void
clawt_pod_bridge_init(ClawtPodBridge *self)
{
    self->modules = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_object_unref);
}
