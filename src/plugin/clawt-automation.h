/*
 * clawt-automation.h - Running the pods that watch a fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A podomation engine, embedded in the daemon, with clawtilla itself
 * registered into it as a module.  That is what makes a pod able to say
 * "when the researcher breaks, restart it and tell me" in one file with
 * nothing else running.
 *
 * The module is registered rather than shipped upstream: podomation has
 * no business knowing what an agent is, and clawtilla has no business
 * waiting for somebody else's release to add a hook point.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "core/clawt-event-bus.h"
#include "plugin/clawt-pod-module.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AUTOMATION (clawt_automation_get_type())

G_DECLARE_FINAL_TYPE(ClawtAutomation, clawt_automation, CLAWT, AUTOMATION,
                     GObject)

/**
 * clawt_automation_new:
 * @bus: the daemon's event bus
 * @context: (nullable): the main context the daemon runs on, or %NULL
 *   for the thread-default
 * @action: (scope notified): how to carry out an action
 * @user_data: data for @action
 *
 * @context is not optional in the way a nullable argument usually is.
 * podomation attaches every pod's event source to it, and dispatches a
 * handler by blocking in a nested #GMainLoop on it -- so an engine given
 * a context nobody iterates does not degrade, it stops: the pods load,
 * they appear in the engine's own listing, and the first event to reach
 * one never returns.  An embedded daemon owns its own context, which is
 * exactly the case that breaks.
 *
 * Returns: (transfer full): a new #ClawtAutomation
 */
ClawtAutomation *clawt_automation_new(ClawtEventBus      *bus,
                                      GMainContext       *context,
                                      ClawtPodActionFunc  action,
                                      gpointer            user_data);

/**
 * clawt_automation_load:
 * @self: a #ClawtAutomation
 * @directory: where the pods live
 * @error: (out) (optional): return location for a #GError
 *
 * Reads every `*.pod` in @directory and starts what it finds.
 *
 * A file that does not parse disables *that file* with a warning naming
 * it, and the rest are loaded: one pod with a typo in it should not stop
 * the others, and a fleet whose automation silently all went away is
 * worse than one that lost a line of it loudly.
 *
 * Returns: %TRUE if the engine started, whatever individual files did
 */
gboolean clawt_automation_load(ClawtAutomation  *self,
                               const gchar      *directory,
                               GError          **error);

/**
 * clawt_automation_stop:
 * @self: a #ClawtAutomation
 *
 * Stops every pod.
 */
void clawt_automation_stop(ClawtAutomation *self);

/**
 * clawt_automation_list_pods:
 * @self: a #ClawtAutomation
 *
 * Returns: (transfer full) (array zero-terminated=1): the pods that
 *   loaded
 */
GStrv clawt_automation_list_pods(ClawtAutomation *self);

/**
 * clawt_automation_get_problems:
 * @self: a #ClawtAutomation
 *
 * What did not load, and why.
 *
 * Kept and reported rather than only logged, so a client can show that
 * a pod file is broken instead of leaving somebody to wonder why their
 * automation stopped happening.
 *
 * Returns: (transfer none) (element-type utf8): the problems
 */
GPtrArray *clawt_automation_get_problems(ClawtAutomation *self);

G_END_DECLS
