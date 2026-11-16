/*
 * test-pod-module.c - clawtilla as seen from a pod
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things here were found only by writing a pod and watching it fail,
 * which is a slow way to learn them twice: the DSL cannot parse a dot in
 * an event name, and podomation hands a handler its arguments as a
 * positional tuple rather than the dictionary the type signature
 * suggests.  Both are pinned here so the next change to either finds out
 * from a test instead.
 */

#include <clawtilla.h>

#include <string.h>

typedef struct {
    gchar      *action;
    GHashTable *params;    /* owned */
    gboolean    refuse;
} Recorder;

static gboolean
record_action(const gchar *action, GHashTable *params,
              GHashTable **out_result, gpointer user_data, GError **error)
{
    Recorder *recorder = user_data;

    g_free(recorder->action);
    recorder->action = g_strdup(action);

    g_clear_pointer(&recorder->params, g_hash_table_unref);
    recorder->params = g_hash_table_ref(params);

    if (recorder->refuse) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "no");
        return FALSE;
    }

    *out_result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
    g_hash_table_insert(*out_result, g_strdup("id"), g_strdup("task-1"));

    return TRUE;
}

static void
recorder_clear(Recorder *recorder)
{
    g_clear_pointer(&recorder->action, g_free);
    g_clear_pointer(&recorder->params, g_hash_table_unref);
}

/* ── Event names ─────────────────────────────────────────────────── */

/*
 * podomation's DSL cannot parse a dot in an event name, and every event
 * it ships is called `on_something`.  Neither is written down anywhere;
 * both took a pod that would not load.
 */
static void
test_every_event_is_spelled_the_way_the_dsl_can_read_it(void)
{
    const PodEventDataFieldInfo *fields;
    guint n_events = 0;
    guint i;

    fields = clawt_pod_module_events(&n_events);

    g_assert_cmpuint(n_events, >, 0);

    for (i = 0; i < n_events; i++) {
        g_assert_nonnull(fields[i].name);
        g_assert_null(strchr(fields[i].name, '.'));
        g_assert_true(g_str_has_prefix(fields[i].name, "on_"));

        /* And every one says what it is for. */
        g_assert_nonnull(fields[i].description);
        g_assert_cmpuint(strlen(fields[i].description), >, 0);
    }
}

static void
test_the_events_a_pod_may_bind_to(void)
{
    g_autoptr(ClawtPodModule) module = clawt_pod_module_new(NULL, NULL, NULL);
    const gchar *const *names;
    gboolean found_state = FALSE;
    gboolean found_message = FALSE;
    guint i;

    names = pod_event_source_get_supported_events(
        POD_EVENT_SOURCE(module));

    g_assert_nonnull(names);

    for (i = 0; names[i] != NULL; i++) {
        if (g_strcmp0(names[i], "on_agent_state") == 0)
            found_state = TRUE;

        if (g_strcmp0(names[i], "on_message") == 0)
            found_message = TRUE;
    }

    g_assert_true(found_state);
    g_assert_true(found_message);
}

/* ── Events reaching a pod ───────────────────────────────────────── */

typedef struct {
    gchar    *name;
    GVariant *data;
    guint     count;
} Fired;

static void
on_fired(PodEventSource *source, const gchar *name, GVariant *data,
         gpointer user_data)
{
    Fired *fired = user_data;

    (void)source;

    g_free(fired->name);
    fired->name = g_strdup(name);

    g_clear_pointer(&fired->data, g_variant_unref);
    fired->data = (data != NULL) ? g_variant_ref(data) : NULL;

    fired->count++;
}

static ClawtPodModule *
instance_for(ClawtPodModule *template, const gchar *first_agent, ...)
{
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    va_list rest;
    const gchar *agent = first_agent;

    va_start(rest, first_agent);

    while (agent != NULL) {
        g_ptr_array_add(args, g_strdup(agent));
        agent = va_arg(rest, const gchar *);
    }

    va_end(rest);

    return CLAWT_POD_MODULE(
        pod_module_create_instance(POD_MODULE(template), "new", args));
}

static void
test_a_daemon_event_reaches_the_pod_under_its_own_name(void)
{
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autoptr(ClawtPodModule) template = clawt_pod_module_new(bus, NULL, NULL);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(ClawtEvent) event = NULL;
    g_autoptr(GError) error = NULL;
    Fired fired = { 0 };

    g_signal_connect(instance, "event-fired", G_CALLBACK(on_fired), &fired);
    g_assert_true(pod_event_source_start(POD_EVENT_SOURCE(instance), NULL,
                                         &error));
    g_assert_no_error(error);

    event = clawt_event_new("agent.state", "researcher");
    clawt_event_set_detail(event, "state", "error");
    clawt_event_set_detail(event, "detail", "it fell over");
    clawt_event_bus_publish(bus, event);

    g_assert_cmpuint(fired.count, ==, 1);
    g_assert_cmpstr(fired.name, ==, "on_agent_state");

    /* The details come across, so a pod can read `state`. */
    {
        g_autoptr(GVariant) state =
            g_variant_lookup_value(fired.data, "state", G_VARIANT_TYPE_STRING);
        g_autoptr(GVariant) agent =
            g_variant_lookup_value(fired.data, "agent", G_VARIANT_TYPE_STRING);

        g_assert_nonnull(state);
        g_assert_cmpstr(g_variant_get_string(state, NULL), ==, "error");
        g_assert_cmpstr(g_variant_get_string(agent, NULL), ==, "researcher");
    }

    g_free(fired.name);
    g_clear_pointer(&fired.data, g_variant_unref);
}

/*
 * Only the documented kinds.  A pod that could bind to anything the
 * daemon ever published would break the day an internal event was
 * renamed, and nothing would have told anybody it was load-bearing.
 */
static void
test_an_undocumented_event_is_not_forwarded(void)
{
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autoptr(ClawtPodModule) template = clawt_pod_module_new(bus, NULL, NULL);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(ClawtEvent) event = clawt_event_new("something.internal", "x");
    Fired fired = { 0 };

    g_signal_connect(instance, "event-fired", G_CALLBACK(on_fired), &fired);
    pod_event_source_start(POD_EVENT_SOURCE(instance), NULL, NULL);

    clawt_event_bus_publish(bus, event);

    g_assert_cmpuint(fired.count, ==, 0);

    g_free(fired.name);
}

/* ── Scope ───────────────────────────────────────────────────────── */

/*
 * The scope comes from the constructor and applies in both directions.
 * A pod named for one agent that could still hear about another would be
 * a per-agent automation with fleet-wide reach.
 */
static void
test_a_scoped_pod_hears_only_its_own_agents(void)
{
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autoptr(ClawtPodModule) template = clawt_pod_module_new(bus, NULL, NULL);
    g_autoptr(ClawtPodModule) scoped =
        instance_for(template, "researcher", NULL);
    g_autoptr(ClawtEvent) theirs = clawt_event_new("agent.state", "scribe");
    g_autoptr(ClawtEvent) ours = clawt_event_new("agent.state", "researcher");
    Fired fired = { 0 };

    g_signal_connect(scoped, "event-fired", G_CALLBACK(on_fired), &fired);
    pod_event_source_start(POD_EVENT_SOURCE(scoped), NULL, NULL);

    clawt_event_bus_publish(bus, theirs);
    g_assert_cmpuint(fired.count, ==, 0);

    clawt_event_bus_publish(bus, ours);
    g_assert_cmpuint(fired.count, ==, 1);

    g_free(fired.name);
    g_clear_pointer(&fired.data, g_variant_unref);
}

/*
 * An event with no agent reaches everybody.  There is nothing to filter
 * it against, and dropping it would silently lose every fleet-level hook
 * from a scoped pod.
 */
static void
test_a_fleet_event_reaches_a_scoped_pod(void)
{
    g_autoptr(ClawtPodModule) template = clawt_pod_module_new(NULL, NULL, NULL);
    g_autoptr(ClawtPodModule) scoped =
        instance_for(template, "researcher", NULL);
    g_autoptr(ClawtPodModule) group =
        instance_for(template, "researcher", "scribe", NULL);
    g_autoptr(ClawtPodModule) everyone = instance_for(template, NULL);

    g_assert_true(clawt_pod_module_covers(scoped, NULL));
    g_assert_true(clawt_pod_module_covers(scoped, "researcher"));
    g_assert_false(clawt_pod_module_covers(scoped, "scribe"));

    g_assert_true(clawt_pod_module_covers(group, "researcher"));
    g_assert_true(clawt_pod_module_covers(group, "scribe"));
    g_assert_false(clawt_pod_module_covers(group, "someone-else"));

    g_assert_true(clawt_pod_module_covers(everyone, "anybody"));
}

/* ── Actions ─────────────────────────────────────────────────────── */

/*
 * The bug this pins: podomation hands a handler its arguments as a
 * *tuple*, in the order the module declared them, padded with empty
 * strings.  Read as the `a{sv}` the type signature suggests, every
 * argument was dropped -- so every action failed with "a notification
 * needs a title" while the pod, the binding and the dispatch were all
 * correct.
 */
static void
test_positional_arguments_arrive_named(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(GVariant) params = NULL;
    GVariant *result = NULL;

    params = g_variant_ref_sink(
        g_variant_new("(sss)", "researcher", "look at this", "high"));

    g_assert_true(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(instance), "message_agent", NULL, params,
        &result));

    g_assert_cmpstr(recorder.action, ==, "message_agent");
    g_assert_cmpstr(g_hash_table_lookup(recorder.params, "agent"), ==,
                    "researcher");
    g_assert_cmpstr(g_hash_table_lookup(recorder.params, "body"), ==,
                    "look at this");
    g_assert_cmpstr(g_hash_table_lookup(recorder.params, "priority"), ==,
                    "high");

    g_clear_pointer(&result, g_variant_unref);
    recorder_clear(&recorder);
}

/*
 * The padding is empty strings, and an empty string is not a value:
 * `notify(title: "x")` must leave `body` unset rather than set to "", or
 * a backend that renders an empty body shows a notification with nothing
 * in it.
 */
static void
test_the_padding_is_not_a_value(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(GVariant) params = NULL;
    GVariant *result = NULL;

    params = g_variant_ref_sink(g_variant_new("(sss)", "all done", "", ""));

    g_assert_true(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(instance), "notify", NULL, params, &result));

    g_assert_cmpstr(g_hash_table_lookup(recorder.params, "title"), ==,
                    "all done");
    g_assert_null(g_hash_table_lookup(recorder.params, "body"));
    g_assert_null(g_hash_table_lookup(recorder.params, "agent"));

    g_clear_pointer(&result, g_variant_unref);
    recorder_clear(&recorder);
}

/*
 * A dictionary works too, because that is what the type signature would
 * lead anybody to expect and a future podomation that sends one should
 * not break this.
 */
static void
test_a_dictionary_works_as_well(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(GVariantBuilder) builder =
        g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_autoptr(GVariant) params = NULL;
    GVariant *result = NULL;

    g_variant_builder_add(builder, "{sv}", "title",
                          g_variant_new_string("hello"));
    params = g_variant_ref_sink(g_variant_builder_end(builder));

    g_assert_true(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(instance), "notify", NULL, params, &result));

    g_assert_cmpstr(g_hash_table_lookup(recorder.params, "title"), ==,
                    "hello");

    g_clear_pointer(&result, g_variant_unref);
    recorder_clear(&recorder);
}

static void
test_the_scope_applies_to_actions_too(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) scoped =
        instance_for(template, "researcher", NULL);
    g_autoptr(GVariant) theirs = NULL;
    GVariant *result = NULL;

    theirs = g_variant_ref_sink(g_variant_new("(s)", "scribe"));

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*scoped to particular agents*");
    g_assert_false(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(scoped), "stop_agent", NULL, theirs, &result));
    g_test_assert_expected_messages();

    /* And the callback was never reached. */
    g_assert_null(recorder.action);

    recorder_clear(&recorder);
}

static void
test_an_unknown_action_is_refused(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    GVariant *result = NULL;

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*no action called 'delete_everything'*");
    g_assert_false(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(instance), "delete_everything", NULL, NULL,
        &result));
    g_test_assert_expected_messages();

    recorder_clear(&recorder);
}

/*
 * Every action a pod may call, with what it takes.  podomation reads
 * this to build its own introspection, so an action with no metadata is
 * one a person cannot discover.
 */
static void
test_every_action_describes_itself(void)
{
    g_autoptr(ClawtPodModule) module = clawt_pod_module_new(NULL, NULL, NULL);
    const gchar *const *names;
    guint i;

    names = pod_event_handler_get_supported_handlers(
        POD_EVENT_HANDLER(module));

    g_assert_nonnull(names);
    g_assert_nonnull(names[0]);

    for (i = 0; names[i] != NULL; i++) {
        const PodHandlerParamInfo *params;
        guint n_params = 0;
        guint k;

        params = pod_event_handler_get_handler_params(
            POD_EVENT_HANDLER(module), names[i], &n_params);

        g_assert_nonnull(params);
        g_assert_cmpuint(n_params, >, 0);

        for (k = 0; k < n_params; k++) {
            g_assert_nonnull(params[k].name);
            g_assert_nonnull(params[k].description);
            g_assert_cmpuint(strlen(params[k].description), >, 0);
        }
    }
}

/*
 * The public getter names the actions table, not one action's
 * parameters.
 *
 * It returned `message_agent_params` -- three rows called `agent`, `body`
 * and `priority` -- under a doc comment promising every action.  Nothing
 * outside the module calls it, which is the only reason that survived, so
 * the assertion that matters is the count against podomation's own list
 * of handlers rather than anything about the first row.
 */
static void
test_the_actions_getter_returns_the_actions(void)
{
    g_autoptr(ClawtPodModule) module = clawt_pod_module_new(NULL, NULL, NULL);
    const PodHandlerParamInfo *listed;
    const gchar *const *handlers;
    guint n_actions = 0;
    guint n_handlers = 0;
    guint i;

    listed = clawt_pod_module_actions(&n_actions);
    handlers = pod_event_handler_get_supported_handlers(
        POD_EVENT_HANDLER(module));

    g_assert_nonnull(listed);
    g_assert_nonnull(handlers);

    for (n_handlers = 0; handlers[n_handlers] != NULL; n_handlers++)
        ;

    /* One row per action, and the same actions in the same order. */
    g_assert_cmpuint(n_actions, ==, n_handlers);

    for (i = 0; i < n_actions; i++) {
        g_assert_cmpstr(listed[i].name, ==, handlers[i]);

        /*
         * And each one says what it takes, because the flat row cannot
         * nest a parameter array and the doc comment promises it anyway.
         */
        g_assert_nonnull(listed[i].description);
        g_assert_nonnull(strstr(listed[i].description, "takes"));
    }

    /* Not a parameter list wearing the actions' name. */
    for (i = 0; i < n_actions; i++)
        g_assert_cmpstr(listed[i].name, !=, "body");
}

/*
 * The forwarder carries a fixed set of detail keys, so a field the daemon
 * adds to an event reaches a pod only when it is added to that list --
 * and nothing warns.  Three had gone missing that way: `peer` and `room`
 * on agent.typing, and `id` on message.  A pod could not tell an agent
 * busy on your question from one busy on a peer's, and could not
 * deduplicate the replay every new subscriber is sent.
 */
static void
test_every_published_detail_reaches_the_pod(void)
{
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autoptr(ClawtPodModule) template = clawt_pod_module_new(bus, NULL, NULL);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(ClawtEvent) typing = clawt_event_new("agent.typing",
                                                   "researcher");
    g_autoptr(ClawtEvent) message = clawt_event_new("message", "dm:user:x");
    Fired fired = { 0 };
    static const gchar *const on_typing[] = { "typing", "peer", "room", NULL };
    static const gchar *const on_message[] = { "id", "from", "to", "body",
                                               "task", NULL };
    guint i;

    g_signal_connect(instance, "event-fired", G_CALLBACK(on_fired), &fired);
    pod_event_source_start(POD_EVENT_SOURCE(instance), NULL, NULL);

    /* Exactly what src/core/clawt-daemon.c puts on an agent.typing. */
    clawt_event_set_detail(typing, "typing", "true");
    clawt_event_set_detail(typing, "peer", "scribe");
    clawt_event_set_detail(typing, "room", "dm:researcher:scribe");
    clawt_event_bus_publish(bus, typing);

    g_assert_cmpuint(fired.count, ==, 1);

    for (i = 0; on_typing[i] != NULL; i++) {
        g_autoptr(GVariant) value =
            g_variant_lookup_value(fired.data, on_typing[i],
                                   G_VARIANT_TYPE_STRING);

        if (value == NULL)
            g_error("on_agent_typing lost '%s'", on_typing[i]);
    }

    /* And what src/mailbox/clawt-mailbox-router.c puts on a message. */
    clawt_event_set_detail(message, "id", "msg-1");
    clawt_event_set_detail(message, "from", "researcher");
    clawt_event_set_detail(message, "to", "user");
    clawt_event_set_detail(message, "body", "done");
    clawt_event_set_detail(message, "task", "task-1");
    clawt_event_bus_publish(bus, message);

    g_assert_cmpuint(fired.count, ==, 2);

    for (i = 0; on_message[i] != NULL; i++) {
        g_autoptr(GVariant) value =
            g_variant_lookup_value(fired.data, on_message[i],
                                   G_VARIANT_TYPE_STRING);

        if (value == NULL)
            g_error("on_message lost '%s'", on_message[i]);
    }

    g_free(fired.name);
    g_clear_pointer(&fired.data, g_variant_unref);
}

/*
 * Deliberately absent.  Every other action is a fleet operation the
 * daemon already owns; this one is arbitrary code on the machine, fired
 * by an event, with nobody watching.
 */
static void
test_computer_exec_says_why_it_will_not(void)
{
    Recorder recorder = { 0 };
    g_autoptr(ClawtPodModule) template =
        clawt_pod_module_new(NULL, record_action, &recorder);
    g_autoptr(ClawtPodModule) instance = instance_for(template, NULL);
    g_autoptr(GVariant) params = NULL;
    GVariant *result = NULL;

    /*
     * It is still declared, so a pod naming it gets a reason rather than
     * "no such action" -- which would read as a typo.
     */
    params = g_variant_ref_sink(g_variant_new("(sss)", "researcher",
                                              "uname -a", ""));

    /* The refusal happens in the daemon's callback, not here. */
    g_assert_true(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(instance), "computer_exec", NULL, params,
        &result));
    g_assert_cmpstr(recorder.action, ==, "computer_exec");

    g_clear_pointer(&result, g_variant_unref);
    recorder_clear(&recorder);
}

/*
 * A module with no daemon behind it says so rather than failing
 * silently.  It is the shape a test or a misregistration produces.
 */
static void
test_a_module_with_no_daemon_says_so(void)
{
    g_autoptr(ClawtPodModule) module = clawt_pod_module_new(NULL, NULL, NULL);
    g_autoptr(GError) error = NULL;
    GVariant *result = NULL;

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*no daemon behind it*");
    g_assert_false(pod_event_handler_handle_event(
        POD_EVENT_HANDLER(module), "notify", NULL, NULL, &result));
    g_test_assert_expected_messages();

    g_assert_false(pod_event_source_start(POD_EVENT_SOURCE(module), NULL,
                                          &error));
    g_assert_nonnull(error);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/pod-module/event-names",
                    test_every_event_is_spelled_the_way_the_dsl_can_read_it);
    g_test_add_func("/pod-module/supported-events",
                    test_the_events_a_pod_may_bind_to);
    g_test_add_func("/pod-module/event-reaches-pod",
                    test_a_daemon_event_reaches_the_pod_under_its_own_name);
    g_test_add_func("/pod-module/undocumented-event",
                    test_an_undocumented_event_is_not_forwarded);
    g_test_add_func("/pod-module/scope-events",
                    test_a_scoped_pod_hears_only_its_own_agents);
    g_test_add_func("/pod-module/scope-fleet-events",
                    test_a_fleet_event_reaches_a_scoped_pod);
    g_test_add_func("/pod-module/positional-args",
                    test_positional_arguments_arrive_named);
    g_test_add_func("/pod-module/padding", test_the_padding_is_not_a_value);
    g_test_add_func("/pod-module/dictionary-args",
                    test_a_dictionary_works_as_well);
    g_test_add_func("/pod-module/scope-actions",
                    test_the_scope_applies_to_actions_too);
    g_test_add_func("/pod-module/unknown-action",
                    test_an_unknown_action_is_refused);
    g_test_add_func("/pod-module/action-metadata",
                    test_every_action_describes_itself);
    g_test_add_func("/pod-module/actions-getter",
                    test_the_actions_getter_returns_the_actions);
    g_test_add_func("/pod-module/carried-details",
                    test_every_published_detail_reaches_the_pod);
    g_test_add_func("/pod-module/computer-exec",
                    test_computer_exec_says_why_it_will_not);
    g_test_add_func("/pod-module/no-daemon",
                    test_a_module_with_no_daemon_says_so);

    return g_test_run();
}
