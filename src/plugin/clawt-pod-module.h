/*
 * clawt-pod-module.h - clawtilla as a podomation module
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * podomation lets an embedding application register its own modules, so
 * this lives here rather than upstream: podomation has no business
 * knowing what an agent is, and clawtilla has no business waiting for a
 * release of somebody else's library to add a hook point.
 *
 * It is both halves at once, which is the point.  A module that only
 * emitted events would need a second module to do anything about them,
 * and the pod between them would be the only place that knew they were
 * the same system.  So one module: `Clawtilla` fires when something
 * happens in the fleet, and `Clawtilla` is also what you call to make
 * something happen -- react and respond, in one line of a pod.
 *
 * Scope comes from the constructor, which is podomation's own way of
 * saying it:
 *
 *   Clawtilla.New()                        every agent
 *   Clawtilla.New("researcher")            one agent
 *   Clawtilla.New("researcher", "scribe")  a group
 *
 * The filter applies in both directions.  A pod scoped to one agent
 * neither hears about the others nor can act on them, so a per-agent
 * automation cannot reach across the fleet by accident.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <podomation.h>

#include "clawt-types.h"
#include "core/clawt-event-bus.h"

G_BEGIN_DECLS

/**
 * ClawtPodActionFunc:
 * @action: which action, such as `message_agent`
 * @params: (element-type utf8 utf8): what it was given
 * @out_result: (out) (element-type utf8 utf8) (transfer full): what to
 *   report back to the pod
 * @user_data: data passed to clawt_pod_module_new()
 * @error: (out) (optional): return location for a #GError
 *
 * Performs one action on the fleet.
 *
 * Everything a pod can do goes through here rather than through the
 * daemon's internals, so the module can be built and exercised without
 * one -- and so there is exactly one list of what a pod is allowed to
 * do.
 *
 * Returns: %TRUE if it was done
 */
typedef gboolean (*ClawtPodActionFunc)(const gchar  *action,
                                       GHashTable   *params,
                                       GHashTable  **out_result,
                                       gpointer      user_data,
                                       GError      **error);

#define CLAWT_TYPE_POD_MODULE (clawt_pod_module_get_type())

G_DECLARE_FINAL_TYPE(ClawtPodModule, clawt_pod_module, CLAWT, POD_MODULE,
                     PodModule)

/**
 * clawt_pod_module_new:
 * @bus: (nullable): the daemon's event bus, or %NULL for a module that
 *   only acts
 * @action: (scope notified) (nullable): how to carry out an action
 * @user_data: data for @action
 *
 * The template instance, registered with podomation's module manager.
 *
 * podomation asks a registered module for an instance per pod, so this
 * one is never started itself: it carries the bus and the callback, and
 * hands them to whatever create_instance() makes.
 *
 * Returns: (transfer full): a new #ClawtPodModule
 */
ClawtPodModule *clawt_pod_module_new(ClawtEventBus      *bus,
                                     ClawtPodActionFunc  action,
                                     gpointer            user_data);

/**
 * clawt_pod_module_set_agents:
 * @self: a #ClawtPodModule
 * @agents: (nullable) (array zero-terminated=1): the agents in scope, or
 *   %NULL for all of them
 *
 * Narrows what this instance hears about and may act on.
 */
void clawt_pod_module_set_agents(ClawtPodModule     *self,
                                 const gchar *const *agents);

/**
 * clawt_pod_module_covers:
 * @self: a #ClawtPodModule
 * @agent_id: (nullable): an agent
 *
 * Whether this instance is in scope for @agent_id.
 *
 * An event with no agent -- the daemon reloading, a routine that failed
 * before it started -- reaches every instance, because there is no agent
 * it could be filtered against and dropping it would silently lose the
 * fleet-level hooks.
 *
 * Returns: %TRUE if it is
 */
gboolean clawt_pod_module_covers(ClawtPodModule *self,
                                 const gchar    *agent_id);

/**
 * clawt_pod_module_events:
 * @n_events: (out) (optional): how many
 *
 * Every hook point, with what each carries.
 *
 * Returns: (transfer none) (array length=n_events): the events
 */
const PodEventDataFieldInfo *clawt_pod_module_events(guint *n_events);

/**
 * clawt_pod_module_actions:
 * @n_actions: (out) (optional): how many
 *
 * Every action, with what each takes.
 *
 * Returns: (transfer none) (array length=n_actions): the actions
 */
const PodHandlerParamInfo *clawt_pod_module_actions(guint *n_actions);

G_END_DECLS
