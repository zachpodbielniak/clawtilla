/*
 * example-plugin.c - A worked clawtilla plugin
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A complete, working plugin rather than a skeleton: it implements two of
 * the four interfaces, so copying this file and changing the names gives
 * you something that runs.
 *
 * As a ClawtEventHandler it keeps a tally of what the fleet has been
 * doing.  As a ClawtToolProvider it offers that tally back to the agents
 * as a tool they can call.  Together those show the two halves of the
 * plugin contract: reacting to the daemon, and extending it.
 *
 * Build:
 *   gcc -shared -fPIC $(pkg-config --cflags clawtilla-1.0) \
 *       example-plugin.c -o libclawt-plugin-example.so \
 *       $(pkg-config --libs clawtilla-1.0)
 *
 * Install into any directory on $CLAWT_PLUGIN_PATH, or into
 * ~/.config/clawtilla/plugins/.
 *
 * Configure in clawtilla.yaml:
 *   plugins:
 *     example:
 *       enabled: true
 *       mode: "verbose"
 */

#include <clawtilla.h>

/*
 * The ABI symbol the manager checks before it calls anything in here.
 *
 * Without it the manager refuses to load the plugin, which is the
 * intended behaviour: a plugin built against a different vtable layout
 * does not fail cleanly, it calls through pointers at wrong offsets.
 */
G_MODULE_EXPORT const guint clawt_plugin_abi_version =
    CLAWT_PLUGIN_ABI_VERSION;

#define EXAMPLE_TYPE_PLUGIN (example_plugin_get_type())

G_DECLARE_FINAL_TYPE(ExamplePlugin, example_plugin, EXAMPLE, PLUGIN,
                     ClawtPlugin)

struct _ExamplePlugin {
    ClawtPlugin parent_instance;

    GHashTable *tally;     /* event kind -> count */
    gboolean    verbose;
};

static void example_plugin_event_handler_init(ClawtEventHandlerInterface *iface);
static void example_plugin_tool_provider_init(ClawtToolProviderInterface *iface);

/*
 * The interfaces are declared here rather than registered by hand.  The
 * manager finds them with g_type_is_a(), so a plugin advertises what it
 * can do simply by implementing it.
 */
G_DEFINE_FINAL_TYPE_WITH_CODE(
    ExamplePlugin, example_plugin, CLAWT_TYPE_PLUGIN,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_EVENT_HANDLER,
                          example_plugin_event_handler_init)
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TOOL_PROVIDER,
                          example_plugin_tool_provider_init))

/* ── The plugin vtable ───────────────────────────────────────────── */

static const gchar *
example_plugin_get_name(ClawtPlugin *plugin)
{
    (void)plugin;

    return "Example";
}

static const gchar *
example_plugin_get_version(ClawtPlugin *plugin)
{
    (void)plugin;

    return "1.0.0";
}

static const gchar *
example_plugin_get_description(ClawtPlugin *plugin)
{
    (void)plugin;

    return "Counts what the fleet does and offers the tally as a tool";
}

static gboolean
example_plugin_configure(ClawtPlugin *plugin, GHashTable *settings,
                         GError **error)
{
    ExamplePlugin *self = EXAMPLE_PLUGIN(plugin);
    const gchar *mode;

    (void)error;

    /*
     * Settings arrive as plain strings from `plugins.example.*`.  An
     * unrecognised mode is not an error: refusing to load over a typo in
     * an optional setting would be a poor trade.
     */
    mode = (settings != NULL) ? g_hash_table_lookup(settings, "mode") : NULL;
    self->verbose = (g_strcmp0(mode, "verbose") == 0);

    return TRUE;
}

static gboolean
example_plugin_activate(ClawtPlugin *plugin, GError **error)
{
    GHashTable *services = clawt_plugin_get_services(plugin);

    (void)error;

    /*
     * Components are reached through the locator rather than through a
     * constructor, so adding a component to the daemon does not change
     * every plugin's signature.  A service that is absent is a reason to
     * do less, not to fail: the same plugin may be loaded by a host that
     * does not have one.
     */
    if (services != NULL && g_hash_table_lookup(services, "agents") == NULL)
        g_message("example: no agent manager available; running anyway");

    return TRUE;
}

static void
example_plugin_deactivate(ClawtPlugin *plugin)
{
    ExamplePlugin *self = EXAMPLE_PLUGIN(plugin);

    g_hash_table_remove_all(self->tally);
}

/* ── ClawtEventHandler ───────────────────────────────────────────── */

static gboolean
example_plugin_handles(ClawtEventHandler *handler, const gchar *kind)
{
    (void)handler;
    (void)kind;

    /* Everything: this plugin's whole job is counting. */
    return TRUE;
}

static void
example_plugin_handle(ClawtEventHandler *handler, ClawtEvent *event)
{
    ExamplePlugin *self = EXAMPLE_PLUGIN(handler);
    const gchar *kind = clawt_event_get_kind(event);
    gpointer count;

    count = g_hash_table_lookup(self->tally, kind);
    g_hash_table_replace(self->tally, g_strdup(kind),
                         GUINT_TO_POINTER(GPOINTER_TO_UINT(count) + 1));

    if (self->verbose)
        g_message("example: %s (%s)", kind,
                  clawt_event_get_subject(event) != NULL
                      ? clawt_event_get_subject(event) : "-");
}

static void
example_plugin_event_handler_init(ClawtEventHandlerInterface *iface)
{
    iface->handles = example_plugin_handles;
    iface->handle = example_plugin_handle;
}

/* ── ClawtToolProvider ───────────────────────────────────────────── */

static const ClawtParamInfo tally_params[] = {
    { "kind", "string",
      "Only count events of this kind. Omit for a full breakdown.", FALSE }
};

static GStrv
example_plugin_list_tools(ClawtToolProvider *provider)
{
    GStrv tools = g_new0(gchar *, 2);

    (void)provider;

    tools[0] = g_strdup("example_fleet_tally");

    return tools;
}

static const ClawtParamInfo *
example_plugin_get_params(ClawtToolProvider *provider,
                          const gchar *tool_name, gsize *n_params)
{
    (void)provider;

    if (g_strcmp0(tool_name, "example_fleet_tally") != 0) {
        *n_params = 0;
        return NULL;
    }

    /*
     * The schema is generated from this table, so the tool cannot describe
     * itself one way and accept another.
     */
    *n_params = G_N_ELEMENTS(tally_params);

    return tally_params;
}

static gchar *
example_plugin_call(ClawtToolProvider *provider, const gchar *agent_id,
                    const gchar *tool_name, JsonObject *arguments,
                    GError **error)
{
    ExamplePlugin *self = EXAMPLE_PLUGIN(provider);
    g_autoptr(GString) out = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    const gchar *wanted = NULL;

    (void)agent_id;

    if (g_strcmp0(tool_name, "example_fleet_tally") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "this plugin has no tool called '%s'", tool_name);
        return NULL;
    }

    if (arguments != NULL && json_object_has_member(arguments, "kind"))
        wanted = json_object_get_string_member(arguments, "kind");

    if (g_hash_table_size(self->tally) == 0)
        return g_strdup("Nothing has happened yet.");

    out = g_string_new(NULL);
    g_hash_table_iter_init(&iter, self->tally);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (wanted != NULL && g_strcmp0(key, wanted) != 0)
            continue;

        g_string_append_printf(out, "%s: %u\n", (const gchar *)key,
                               GPOINTER_TO_UINT(value));
    }

    if (out->len == 0)
        return g_strdup_printf("Nothing of kind '%s' has happened.", wanted);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static void
example_plugin_tool_provider_init(ClawtToolProviderInterface *iface)
{
    iface->list_tools = example_plugin_list_tools;
    iface->get_params = example_plugin_get_params;
    iface->call = example_plugin_call;
}

/* ── Boilerplate ─────────────────────────────────────────────────── */

static void
example_plugin_finalize(GObject *object)
{
    ExamplePlugin *self = EXAMPLE_PLUGIN(object);

    g_clear_pointer(&self->tally, g_hash_table_unref);

    G_OBJECT_CLASS(example_plugin_parent_class)->finalize(object);
}

static void
example_plugin_class_init(ExamplePluginClass *klass)
{
    ClawtPluginClass *plugin_class = CLAWT_PLUGIN_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = example_plugin_finalize;

    plugin_class->get_name = example_plugin_get_name;
    plugin_class->get_version = example_plugin_get_version;
    plugin_class->get_description = example_plugin_get_description;
    plugin_class->configure = example_plugin_configure;
    plugin_class->activate = example_plugin_activate;
    plugin_class->deactivate = example_plugin_deactivate;
}

static void
example_plugin_init(ExamplePlugin *self)
{
    self->tally = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);
}

/*
 * The entry point.  Returning the GType rather than an instance lets the
 * manager construct as many as it needs and keeps the plugin free of
 * assumptions about its own lifetime.
 */
G_MODULE_EXPORT GType
clawt_plugin_register(void)
{
    return EXAMPLE_TYPE_PLUGIN;
}
