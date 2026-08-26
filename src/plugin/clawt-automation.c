/*
 * clawt-automation.c - Running the pods that watch a fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "plugin/clawt-automation.h"

#include <glib/gstdio.h>
#include <podomation.h>

#include <string.h>

struct _ClawtAutomation {
    GObject parent_instance;

    PodEngine  *engine;
    GPtrArray  *problems;   /* gchar* */
    gboolean    running;
};

G_DEFINE_FINAL_TYPE(ClawtAutomation, clawt_automation, G_TYPE_OBJECT)

static void
clawt_automation_finalize(GObject *object)
{
    ClawtAutomation *self = CLAWT_AUTOMATION(object);

    clawt_automation_stop(self);

    g_clear_object(&self->engine);
    g_clear_pointer(&self->problems, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_automation_parent_class)->finalize(object);
}

static void
clawt_automation_class_init(ClawtAutomationClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_automation_finalize;
}

static void
clawt_automation_init(ClawtAutomation *self)
{
    self->problems = g_ptr_array_new_with_free_func(g_free);
}

ClawtAutomation *
clawt_automation_new(ClawtEventBus      *bus,
                     GMainContext       *context,
                     ClawtPodActionFunc  action,
                     gpointer            user_data)
{
    ClawtAutomation *self = g_object_new(CLAWT_TYPE_AUTOMATION, NULL);
    ClawtPodModule *module;

    self->engine = pod_engine_new();

    /*
     * pod_engine_new() captures the thread-default context, which is the
     * process default unless somebody pushed one -- so an embedded
     * daemon would get a context it never iterates, and every pod would
     * load, list, and never fire.  The same family as the timers and the
     * GTask already recorded in CLAUDE.md, one library along: anything
     * that captures "the current context" is wrong in an embedded daemon
     * unless it was told which one.
     */
    if (context != NULL)
        pod_engine_set_main_context(self->engine, context);

    /*
     * Registered, not loaded.  podomation lets an embedding application
     * hand it an already-instantiated module, which is exactly what this
     * is: there is no .so to find, and the module needs a live event bus
     * and a callback into a running daemon that a .so could never have.
     *
     * The manager takes the reference on success, so this must not be a
     * g_autoptr -- both would own the one reference.
     */
    module = clawt_pod_module_new(bus, action, user_data);

    if (!pod_module_manager_register(pod_engine_get_module_manager(self->engine),
                                     POD_MODULE(module))) {
        g_warning("automation: the clawtilla module could not be "
                  "registered; no pod will be able to see the fleet");
        g_object_unref(module);
    }

    return self;
}

gboolean
clawt_automation_load(ClawtAutomation *self, const gchar *directory,
                      GError **error)
{
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GError) local = NULL;
    const gchar *name;
    guint loaded = 0;

    g_return_val_if_fail(CLAWT_IS_AUTOMATION(self), FALSE);

    g_ptr_array_set_size(self->problems, 0);

    if (directory == NULL)
        return TRUE;

    /*
     * A missing directory is not a failure.  Most fleets have no
     * automation, and creating the directory to hold nothing would be a
     * file appearing in somebody's home for a feature they never used.
     */
    if (!g_file_test(directory, G_FILE_TEST_IS_DIR))
        return TRUE;

    dir = g_dir_open(directory, 0, &local);

    if (dir == NULL) {
        g_propagate_error(error, g_steal_pointer(&local));
        return FALSE;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *text = NULL;
        g_autoptr(GError) file_error = NULL;

        if (!g_str_has_suffix(name, ".pod"))
            continue;

        path = g_build_filename(directory, name, NULL);

        if (!g_file_get_contents(path, &text, NULL, &file_error)) {
            g_ptr_array_add(self->problems,
                            g_strdup_printf("%s: %s", name,
                                            file_error->message));
            continue;
        }

        /*
         * One bad file disables that file and nothing else.  A fleet
         * whose automation silently all went away is worse than one that
         * lost a line of it loudly, and the message names the file so
         * somebody can find it.
         */
        if (!pod_engine_parse_dsl(self->engine, text, &file_error)) {
            g_ptr_array_add(self->problems,
                            g_strdup_printf("%s: %s", name,
                                            file_error->message));
            g_warning("automation: %s could not be read: %s", path,
                      file_error->message);
            continue;
        }

        loaded++;
    }

    if (loaded == 0 && self->problems->len == 0)
        return TRUE;

    /*
     * Embedded, because the daemon owns the loop.  pod_engine_start()
     * installs signal handlers and runs a loop of its own, which in a
     * process that already has both is two programs fighting over
     * SIGTERM.
     */
    if (!pod_engine_start_embedded(self->engine, &local)) {
        g_propagate_error(error, g_steal_pointer(&local));
        return FALSE;
    }

    self->running = TRUE;

    g_message("automation: %u pod file%s loaded from %s", loaded,
              loaded == 1 ? "" : "s", directory);

    return TRUE;
}

void
clawt_automation_stop(ClawtAutomation *self)
{
    g_return_if_fail(CLAWT_IS_AUTOMATION(self));

    if (!self->running)
        return;

    pod_engine_stop(self->engine);
    self->running = FALSE;
}

GStrv
clawt_automation_list_pods(ClawtAutomation *self)
{
    g_autoptr(GPtrArray) names = NULL;
    GPtrArray *pods;
    guint i;

    g_return_val_if_fail(CLAWT_IS_AUTOMATION(self), NULL);

    names = g_ptr_array_new();
    pods = pod_engine_get_pods(self->engine);

    for (i = 0; pods != NULL && i < pods->len; i++) {
        PodPod *pod = g_ptr_array_index(pods, i);

        g_ptr_array_add(names, g_strdup(pod_pod_get_name(pod)));
    }

    g_ptr_array_add(names, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

GPtrArray *
clawt_automation_get_problems(ClawtAutomation *self)
{
    g_return_val_if_fail(CLAWT_IS_AUTOMATION(self), NULL);

    return self->problems;
}
