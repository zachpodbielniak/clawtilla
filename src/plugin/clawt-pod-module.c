/*
 * clawt-pod-module.c - clawtilla as a podomation module
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "plugin/clawt-pod-module.h"

#include <string.h>

/* ── What a pod can hear about ───────────────────────────────────── */

/*
 * The hook points, and the one thing each is for.
 *
 * These are the daemon's own event kinds rather than a second
 * vocabulary invented for pods: anything clawtilla publishes is
 * something a pod can react to, and a kind added later needs no work
 * here beyond a line in this table to describe it.
 */
/*
 * The pod-visible name, and the daemon event kind it comes from.
 *
 * They differ twice over, and both were found by writing a pod and
 * watching it fail. The DSL cannot express a dot in an event name --
 * `researcher->agent.state` does not parse -- and every event podomation
 * ships is named `on_something`, which is the convention its bindings
 * are written around. The daemon's kinds keep their dots, since they are
 * the wire format a dozen other things already read.
 */
typedef struct {
    const gchar *pod_name;
    const gchar *bus_kind;
    const gchar *description;
} EventName;

static const EventName events[] = {
    { "on_agent_created", "agent.created",
      "An agent was added to the fleet" },
    { "on_agent_removed", "agent.removed", "An agent was removed" },
    { "on_agent_changed", "agent.changed",
      "An agent's configuration changed" },
    { "on_agent_started", "agent.started", "An agent was started" },
    { "on_agent_stopped", "agent.stopped", "An agent was stopped" },
    { "on_agent_state", "agent.state",
      "An agent changed state; `state` says which, `detail` says why" },
    { "on_agent_connected", "agent.connected",
      "An agent dialled in to the daemon" },
    { "on_agent_disconnected", "agent.disconnected",
      "An agent's link dropped" },
    { "on_agent_typing", "agent.typing",
      "A turn began or ended; `typing` says which" },
    { "on_message", "message",
      "A message was routed; `from`, `to`, `body` and `task`" },
    { "on_task_changed", "task.changed",
      "A delegated task changed state; `state` says which" },
    { "on_routine_ran", "routine.ran", "A scheduled routine started" },
    { "on_routine_changed", "routine.changed",
      "A routine was added, edited or removed" },
    { "on_integration_changed", "integration.changed",
      "An integration was added, edited or removed" },
    { "on_daemon_started", "daemon.started", "The daemon came up" },
    { "on_daemon_reloaded", "daemon.reloaded",
      "The configuration was reloaded" },
    { "on_daemon_stopped", "daemon.stopped", "The daemon is going away" }
};

/*
 * Exposed to podomation in its own shape.  Built once rather than kept
 * as a second table, so the descriptions cannot drift from the names.
 */
static const PodEventDataFieldInfo *
event_fields(void)
{
    static PodEventDataFieldInfo fields[G_N_ELEMENTS(events)];
    static gsize built = 0;

    if (g_once_init_enter(&built)) {
        gsize i;

        for (i = 0; i < G_N_ELEMENTS(events); i++) {
            fields[i].name = events[i].pod_name;
            fields[i].type_name = "event";
            fields[i].description = events[i].description;
        }

        g_once_init_leave(&built, 1);
    }

    return fields;
}

const PodEventDataFieldInfo *
clawt_pod_module_events(guint *n_events)
{
    if (n_events != NULL)
        *n_events = G_N_ELEMENTS(events);

    return event_fields();
}

/* ── What a pod can do ───────────────────────────────────────────── */

/*
 * The actions, as podomation parameter metadata.
 *
 * One flat table rather than a list per action, because podomation asks
 * for the parameters of one handler at a time and the natural shape is
 * "everything, filtered": the alternative is a table per action and a
 * switch to pick between them, which is the same information written
 * twice.
 */
typedef struct {
    const gchar               *name;
    const gchar               *summary;
    const PodHandlerParamInfo *params;
    guint                      n_params;
} Action;

static const PodHandlerParamInfo message_agent_params[] = {
    { "agent", "string", "Who to tell", TRUE },
    { "body", "string", "What to say", TRUE },
    { "priority", "string", "low, normal, high or urgent", FALSE }
};

static const PodHandlerParamInfo delegate_params[] = {
    { "agent", "string", "Who should do it", TRUE },
    { "prompt", "string", "What to ask for", TRUE }
};

static const PodHandlerParamInfo agent_params[] = {
    { "agent", "string", "Which agent", TRUE }
};

static const PodHandlerParamInfo exec_params[] = {
    { "agent", "string", "Whose computer", TRUE },
    { "command", "string",
      "The command. Not a shell line: redirects and pipes arrive as "
      "literal text, so wrap it in `bash -c` when you want shell "
      "behaviour", TRUE },
    { "timeout", "integer", "Seconds before giving up", FALSE }
};

static const PodHandlerParamInfo notify_params[] = {
    { "title", "string", "The line a lock screen shows", TRUE },
    { "body", "string", "The rest", FALSE },
    { "agent", "string", "Which agent it is about, if any", FALSE }
};

static const PodHandlerParamInfo routine_params[] = {
    { "routine", "string", "Which routine", TRUE }
};

static const PodHandlerParamInfo memory_params[] = {
    { "agent", "string", "Whose memory", TRUE },
    { "content", "string", "What to remember", TRUE },
    { "category", "string", "general, decision, fact, learning, ...", FALSE }
};

static const PodHandlerParamInfo room_params[] = {
    { "room", "string", "Which room", TRUE },
    { "body", "string", "What to say", TRUE }
};

static const Action actions[] = {
    { "on_message_agent", "Queue a message to an agent",
      message_agent_params, G_N_ELEMENTS(message_agent_params) },
    { "post_room", "Say something to every member of a room",
      room_params, G_N_ELEMENTS(room_params) },
    { "delegate", "Hand an agent a task; returns its id at once",
      delegate_params, G_N_ELEMENTS(delegate_params) },
    { "start_agent", "Start an agent",
      agent_params, G_N_ELEMENTS(agent_params) },
    { "stop_agent", "Stop an agent",
      agent_params, G_N_ELEMENTS(agent_params) },
    { "restart_agent", "Stop an agent and start it again",
      agent_params, G_N_ELEMENTS(agent_params) },
    { "computer_exec", "Run a command on an agent's computer",
      exec_params, G_N_ELEMENTS(exec_params) },
    { "notify", "Tell the operator something",
      notify_params, G_N_ELEMENTS(notify_params) },
    { "run_routine", "Run a routine now",
      routine_params, G_N_ELEMENTS(routine_params) },
    { "memory_add", "Write something into an agent's memory",
      memory_params, G_N_ELEMENTS(memory_params) }
};

const PodHandlerParamInfo *
clawt_pod_module_actions(guint *n_actions)
{
    if (n_actions != NULL)
        *n_actions = G_N_ELEMENTS(message_agent_params);

    return message_agent_params;
}

static const Action *
find_action(const gchar *name)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(actions); i++) {
        if (g_strcmp0(actions[i].name, name) == 0)
            return &actions[i];
    }

    return NULL;
}

/* ── The module ──────────────────────────────────────────────────── */

struct _ClawtPodModule {
    PodModule parent_instance;

    ClawtEventBus      *bus;          /* owned */
    gulong              bus_handler;

    ClawtPodActionFunc  action;
    gpointer            action_data;

    GStrv               agents;       /* NULL means all of them */
    gboolean            running;
};

static void event_source_iface_init(PodEventSourceInterface *iface);
static void event_handler_iface_init(PodEventHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtPodModule, clawt_pod_module, POD_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(POD_TYPE_EVENT_SOURCE, event_source_iface_init)
    G_IMPLEMENT_INTERFACE(POD_TYPE_EVENT_HANDLER, event_handler_iface_init))

gboolean
clawt_pod_module_covers(ClawtPodModule *self, const gchar *agent_id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_POD_MODULE(self), FALSE);

    if (self->agents == NULL || self->agents[0] == NULL)
        return TRUE;

    /*
     * An event with no agent reaches everybody.  There is nothing to
     * filter it against, and dropping it would silently lose every
     * fleet-level hook -- daemon.reloaded, a routine that failed before
     * it had an agent -- from a scoped pod.
     */
    if (agent_id == NULL)
        return TRUE;

    for (i = 0; self->agents[i] != NULL; i++) {
        if (g_strcmp0(self->agents[i], agent_id) == 0)
            return TRUE;
    }

    return FALSE;
}

void
clawt_pod_module_set_agents(ClawtPodModule *self, const gchar *const *agents)
{
    g_return_if_fail(CLAWT_IS_POD_MODULE(self));

    g_clear_pointer(&self->agents, g_strfreev);

    if (agents != NULL && agents[0] != NULL)
        self->agents = g_strdupv((GStrv)agents);
}

/*
 * Which agent an event is about.
 *
 * The subject for the agent.* kinds, and a detail for the rest -- a
 * `message` is about a room and names its sender, and a pod scoped to
 * one agent wants that agent's messages rather than every room it
 * happens to be in.
 */
static const gchar *
event_agent(ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (g_str_has_prefix(kind, "agent."))
        return clawt_event_get_subject(event);

    if (g_strcmp0(kind, "message") == 0)
        return clawt_event_get_detail(event, "from");

    return clawt_event_get_detail(event, "agent");
}

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtPodModule *self = user_data;
    g_autoptr(GVariantBuilder) builder = NULL;
    const gchar *kind = clawt_event_get_kind(event);
    const gchar *subject;
    gsize i;
    gboolean known = FALSE;

    (void)bus;

    if (!self->running)
        return;

    for (i = 0; i < G_N_ELEMENTS(events); i++) {
        if (g_strcmp0(events[i].bus_kind, kind) == 0) {
            known = TRUE;
            kind = events[i].pod_name;
            break;
        }
    }

    /*
     * Only the documented kinds.  A pod that could bind to anything the
     * daemon ever published would break the day an internal event was
     * renamed, and nothing would have told anybody it was load-bearing.
     */
    if (!known)
        return;

    if (!clawt_pod_module_covers(self, event_agent(event)))
        return;

    builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    subject = clawt_event_get_subject(event);

    if (subject != NULL)
        g_variant_builder_add(builder, "{sv}", "subject",
                              g_variant_new_string(subject));

    if (event_agent(event) != NULL)
        g_variant_builder_add(builder, "{sv}", "agent",
                              g_variant_new_string(event_agent(event)));

    g_variant_builder_add(builder, "{sv}", "timestamp",
                          g_variant_new_int64(
                              clawt_event_get_timestamp(event)));

    /*
     * The details go across as they are.  A pod binding to `message`
     * wants `from`, `to` and `body`, and enumerating them per kind here
     * would be a third copy of what the event already carries.
     */
    {
        static const gchar *const carried[] = {
            "state", "detail", "from", "to", "body", "task", "typing",
            "error", "path", NULL
        };
        gsize k;

        for (k = 0; carried[k] != NULL; k++) {
            const gchar *value = clawt_event_get_detail(event, carried[k]);

            if (value != NULL)
                g_variant_builder_add(builder, "{sv}", carried[k],
                                      g_variant_new_string(value));
        }
    }

    /*
     * Emitted on the interface's own signal, which is what podomation's
     * bindings listen to.  The payload is floating, and g_signal_emit
     * does not sink it, so the reference is taken and released here
     * rather than leaked once per event -- which for a busy fleet is a
     * leak per message.
     */
    {
        g_autoptr(GVariant) payload =
            g_variant_ref_sink(g_variant_builder_end(builder));

        g_signal_emit_by_name(self, "event-fired", kind, payload);
    }
}

/* ── PodEventSource ──────────────────────────────────────────────── */

static gboolean
clawt_pod_source_start(PodEventSource *source, GMainContext *context,
                       GError **error)
{
    ClawtPodModule *self = CLAWT_POD_MODULE(source);

    (void)context;

    if (self->bus == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "this Clawtilla module has no daemon behind it");
        return FALSE;
    }

    if (self->bus_handler == 0)
        self->bus_handler = g_signal_connect(self->bus, "event",
                                             G_CALLBACK(on_bus_event), self);

    self->running = TRUE;

    return TRUE;
}

static void
clawt_pod_source_stop(PodEventSource *source)
{
    ClawtPodModule *self = CLAWT_POD_MODULE(source);

    self->running = FALSE;
}

static PodEventKind
clawt_pod_source_get_event_kind(PodEventSource *source)
{
    (void)source;

    return POD_EVENT_KIND_CUSTOM;
}

static const gchar *const *
clawt_pod_source_get_supported_events(PodEventSource *source)
{
    static const gchar *names[G_N_ELEMENTS(events) + 1];
    static gsize built = 0;

    (void)source;

    if (g_once_init_enter(&built)) {
        gsize i;

        for (i = 0; i < G_N_ELEMENTS(events); i++)
            names[i] = events[i].pod_name;

        names[G_N_ELEMENTS(events)] = NULL;
        g_once_init_leave(&built, 1);
    }

    return names;
}

static const PodEventDataFieldInfo *
clawt_pod_source_get_event_data_fields(PodEventSource *source,
                                       const gchar    *event_name,
                                       guint          *n_fields)
{
    gsize i;

    (void)source;

    for (i = 0; i < G_N_ELEMENTS(events); i++) {
        if (g_strcmp0(events[i].pod_name, event_name) != 0)
            continue;

        if (n_fields != NULL)
            *n_fields = 1;

        return &event_fields()[i];
    }

    if (n_fields != NULL)
        *n_fields = 0;

    return NULL;
}

static void
event_source_iface_init(PodEventSourceInterface *iface)
{
    iface->start = clawt_pod_source_start;
    iface->stop = clawt_pod_source_stop;
    iface->get_event_kind = clawt_pod_source_get_event_kind;
    iface->get_supported_events = clawt_pod_source_get_supported_events;
    iface->get_event_data_fields = clawt_pod_source_get_event_data_fields;
}

/* ── PodEventHandler ─────────────────────────────────────────────── */

/*
 * A GVariant of parameters, as a string table.
 *
 * Everything a pod passes is a string by the time it has been through
 * podomation's interpolation, so flattening here keeps the action
 * callback free of GVariant and therefore testable without one.
 */
static GHashTable *
params_to_hash(GVariant *params)
{
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    if (params == NULL || !g_variant_is_of_type(params,
                                                G_VARIANT_TYPE("a{sv}")))
        return out;

    g_variant_iter_init(&iter, params);

    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
            g_hash_table_insert(out, g_strdup(key),
                                g_variant_dup_string(value, NULL));
        else
            g_hash_table_insert(out, g_strdup(key),
                                g_variant_print(value, FALSE));

        g_variant_unref(value);
    }

    return out;
}

static GVariant *
hash_to_variant(GHashTable *values)
{
    g_autoptr(GVariantBuilder) builder =
        g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (values != NULL) {
        g_hash_table_iter_init(&iter, values);

        while (g_hash_table_iter_next(&iter, &key, &value))
            g_variant_builder_add(builder, "{sv}", (const gchar *)key,
                                  g_variant_new_string(
                                      value != NULL ? value : ""));
    }

    return g_variant_builder_end(builder);
}

static gboolean
clawt_pod_handle_event(PodEventHandler *handler, const gchar *event_name,
                       GVariant *event_data, GVariant *params,
                       GVariant **result)
{
    ClawtPodModule *self = CLAWT_POD_MODULE(handler);
    g_autoptr(GHashTable) arguments = NULL;
    g_autoptr(GHashTable) produced = NULL;
    g_autoptr(GError) error = NULL;
    const Action *action;
    const gchar *agent;

    (void)event_data;

    action = find_action(event_name);

    if (action == NULL) {
        g_warning("Clawtilla: there is no action called '%s'", event_name);
        return FALSE;
    }

    if (self->action == NULL) {
        g_warning("Clawtilla: this module has no daemon behind it, so "
                  "'%s' does nothing", event_name);
        return FALSE;
    }

    arguments = params_to_hash(params);
    agent = g_hash_table_lookup(arguments, "agent");

    /*
     * The scope applies to actions as well as events.  A pod scoped to
     * one agent that could still stop another would be a per-agent
     * automation with fleet-wide reach, which is exactly what somebody
     * writing `Clawtilla.New("researcher")` is asking not to have.
     */
    if (!clawt_pod_module_covers(self, agent)) {
        g_warning("Clawtilla: this pod is scoped to particular agents and "
                  "'%s' is not one of them", agent != NULL ? agent : "");
        return FALSE;
    }

    if (!self->action(event_name, arguments, &produced, self->action_data,
                      &error)) {
        g_warning("Clawtilla: %s: %s", event_name,
                  error != NULL ? error->message : "it did not work");
        return FALSE;
    }

    if (result != NULL)
        *result = hash_to_variant(produced);

    return TRUE;
}

static const gchar *const *
clawt_pod_get_supported_handlers(PodEventHandler *handler)
{
    static const gchar *names[G_N_ELEMENTS(actions) + 1];
    static gsize built = 0;

    (void)handler;

    if (g_once_init_enter(&built)) {
        gsize i;

        for (i = 0; i < G_N_ELEMENTS(actions); i++)
            names[i] = actions[i].name;

        names[G_N_ELEMENTS(actions)] = NULL;
        g_once_init_leave(&built, 1);
    }

    return names;
}

static const PodHandlerParamInfo *
clawt_pod_get_handler_params(PodEventHandler *handler,
                             const gchar     *handler_name,
                             guint           *n_params)
{
    const Action *action;

    (void)handler;

    action = find_action(handler_name);

    if (action == NULL) {
        if (n_params != NULL)
            *n_params = 0;

        return NULL;
    }

    if (n_params != NULL)
        *n_params = action->n_params;

    return action->params;
}

static const PodHandlerReturnInfo return_fields[] = {
    { "ok", "boolean", "Whether it was done" },
    { "id", "string", "What was created, for the actions that create one" },
    { "detail", "string", "Anything worth saying about it" }
};

static const PodHandlerReturnInfo *
clawt_pod_get_handler_return_fields(PodEventHandler *handler,
                                    const gchar     *handler_name,
                                    guint           *n_fields)
{
    (void)handler;
    (void)handler_name;

    if (n_fields != NULL)
        *n_fields = G_N_ELEMENTS(return_fields);

    return return_fields;
}

static void
event_handler_iface_init(PodEventHandlerInterface *iface)
{
    iface->handle_event = clawt_pod_handle_event;
    iface->get_supported_handlers = clawt_pod_get_supported_handlers;
    iface->get_handler_params = clawt_pod_get_handler_params;
    iface->get_handler_return_fields = clawt_pod_get_handler_return_fields;
}

/* ── PodModule ───────────────────────────────────────────────────── */

/*
 * There is nothing to set up.
 *
 * The base class refuses by default, which is right for a module that
 * has to open something -- and wrong here, where the daemon is already
 * running and the bus is already connected. Without this, every pod
 * loaded and then failed to activate, with one line in the log and no
 * events ever delivered.
 */
static gboolean
clawt_pod_module_activate(PodModule *module)
{
    (void)module;

    return TRUE;
}

static void
clawt_pod_module_deactivate(PodModule *module)
{
    ClawtPodModule *self = CLAWT_POD_MODULE(module);

    self->running = FALSE;
}

static const gchar *
clawt_pod_module_get_name(PodModule *module)
{
    (void)module;

    return "clawtilla";
}

static const gchar *
clawt_pod_module_get_description(PodModule *module)
{
    (void)module;

    return "React to what a fleet of agents does, and act on it.";
}

static const PodConstructorParamInfo constructor_params[] = {
    { "agents", "string",
      "Agent ids this pod is scoped to. With none, every agent -- and "
      "the scope applies both ways, so a pod named for one agent neither "
      "hears about the others nor can act on them.",
      FALSE }
};

static const PodConstructorParamInfo *
clawt_pod_module_get_constructor_params(PodModule *module, guint *n_params)
{
    (void)module;

    if (n_params != NULL)
        *n_params = G_N_ELEMENTS(constructor_params);

    return constructor_params;
}

/*
 * podomation asks the registered module for one instance per pod, so
 * this is where the scope is applied: the constructor's arguments are
 * the agent ids, and the bus and the callback come from the template.
 */
static PodModule *
clawt_pod_module_create_instance(PodModule   *module,
                                 const gchar *constructor_name,
                                 GPtrArray   *constructor_args)
{
    ClawtPodModule *template = CLAWT_POD_MODULE(module);
    ClawtPodModule *instance = g_object_new(CLAWT_TYPE_POD_MODULE, NULL);
    guint i;

    (void)constructor_name;

    instance->bus = (template->bus != NULL)
        ? g_object_ref(template->bus) : NULL;
    instance->action = template->action;
    instance->action_data = template->action_data;

    if (constructor_args != NULL && constructor_args->len > 0) {
        g_autoptr(GPtrArray) ids = g_ptr_array_new();

        for (i = 0; i < constructor_args->len; i++) {
            const gchar *id = g_ptr_array_index(constructor_args, i);

            if (id != NULL && *id != '\0')
                g_ptr_array_add(ids, g_strdup(id));
        }

        g_ptr_array_add(ids, NULL);
        instance->agents =
            (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
    }

    return POD_MODULE(instance);
}

static const gchar *
clawt_pod_module_get_help(PodModule *module)
{
    (void)module;

    return
        "clawtilla -- react to a fleet of agents, and act on it.\n"
        "\n"
        "Scope comes from the constructor, and applies in both\n"
        "directions:\n"
        "\n"
        "  Clawtilla.New()                        every agent\n"
        "  Clawtilla.New(\"researcher\")             one agent\n"
        "  Clawtilla.New(\"researcher\", \"scribe\")   a group\n"
        "\n"
        "A pod scoped to one agent neither hears about the others nor\n"
        "can act on them.\n"
        "\n"
        "Event names follow podomation's own convention: on_agent_state,\n"
        "not agent.state. The DSL cannot parse a dot in an event name,\n"
        "and every event podomation ships is named on_something.\n"
        "\n"
        "Events carry `agent` and `timestamp`, plus whatever that kind\n"
        "has: `state` and `detail` for on_agent_state, `from`, `to`,\n"
        "`body` and `task` for on_message, `typing` for on_agent_typing.\n"
        "\n"
        "Actions take named parameters and return `ok`, and `id` for the\n"
        "ones that create something.\n"
        "\n"
        "Worth knowing: `computer_exec` takes a command and not a shell\n"
        "line, so a redirect arrives as literal text and the command\n"
        "reports success having done nothing. Wrap it in `bash -c`.";
}

static void
clawt_pod_module_finalize(GObject *object)
{
    ClawtPodModule *self = CLAWT_POD_MODULE(object);

    if (self->bus != NULL && self->bus_handler != 0)
        g_signal_handler_disconnect(self->bus, self->bus_handler);

    g_clear_object(&self->bus);
    g_clear_pointer(&self->agents, g_strfreev);

    G_OBJECT_CLASS(clawt_pod_module_parent_class)->finalize(object);
}

static void
clawt_pod_module_class_init(ClawtPodModuleClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    PodModuleClass *module_class = POD_MODULE_CLASS(klass);

    object_class->finalize = clawt_pod_module_finalize;

    module_class->activate = clawt_pod_module_activate;
    module_class->deactivate = clawt_pod_module_deactivate;
    module_class->get_name = clawt_pod_module_get_name;
    module_class->get_description = clawt_pod_module_get_description;
    module_class->get_help = clawt_pod_module_get_help;
    module_class->create_instance = clawt_pod_module_create_instance;
    module_class->get_constructor_params =
        clawt_pod_module_get_constructor_params;
}

static void
clawt_pod_module_init(ClawtPodModule *self)
{
    (void)self;
}

ClawtPodModule *
clawt_pod_module_new(ClawtEventBus      *bus,
                     ClawtPodActionFunc  action,
                     gpointer            user_data)
{
    ClawtPodModule *self = g_object_new(CLAWT_TYPE_POD_MODULE, NULL);

    self->bus = (bus != NULL) ? g_object_ref(bus) : NULL;
    self->action = action;
    self->action_data = user_data;

    return self;
}
