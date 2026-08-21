/*
 * clawt-container-computer.c - A podman container per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-container-computer.h"

#include <json-glib/json-glib.h>

#define MAX_OUTPUT_BYTES (256 * 1024)

struct _ClawtContainerComputer {
    ClawtComputer parent_instance;

    ClawtPodBridge *bridge;
    gchar          *image;
    gchar          *name;
    gchar          *network;
    gchar          *container_id;
    gboolean        keep;
};

G_DEFINE_FINAL_TYPE(ClawtContainerComputer, clawt_container_computer,
                    CLAWT_TYPE_COMPUTER)

ClawtComputer *
clawt_container_computer_new(const gchar    *agent_id,
                             ClawtPodBridge *bridge,
                             const gchar    *image)
{
    ClawtContainerComputer *self;

    g_return_val_if_fail(CLAWT_IS_POD_BRIDGE(bridge), NULL);

    self = g_object_new(CLAWT_TYPE_CONTAINER_COMPUTER, NULL);
    self->bridge = g_object_ref(bridge);
    self->image = g_strdup(image);

    /* A predictable name, so a container left behind can be found again. */
    self->name = g_strdup_printf("clawt-%s",
                                 agent_id != NULL ? agent_id : "agent");

    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    return CLAWT_COMPUTER(self);
}

void
clawt_container_computer_set_name(ClawtContainerComputer *self,
                                  const gchar            *name)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    if (name == NULL)
        return;

    g_free(self->name);
    self->name = g_strdup(name);
}

void
clawt_container_computer_set_network(ClawtContainerComputer *self,
                                     const gchar            *network)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    g_free(self->network);
    self->network = g_strdup(network);
}

void
clawt_container_computer_set_keep(ClawtContainerComputer *self, gboolean keep)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    self->keep = keep;
}

gchar *
clawt_container_computer_build_mount_json(GPtrArray *mounts)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    guint i;

    json_builder_begin_array(builder);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        g_autofree gchar *source = clawt_mount_resolved_source(mount);
        ClawtMountType type = clawt_mount_get_mount_type(mount);

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "destination");
        json_builder_add_string_value(builder, clawt_mount_get_target(mount));

        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(
            builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_TYPE, type));

        if (source != NULL) {
            json_builder_set_member_name(builder, "source");
            json_builder_add_string_value(builder, source);
        }

        json_builder_set_member_name(builder, "read_only");
        json_builder_add_boolean_value(
            builder, clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RO);

        /*
         * The relabel flag is the one that gets forgotten and then costs an
         * afternoon: on Fedora Silverblue an unlabelled bind mount is
         * visible inside the container while every access is denied, which
         * reads like a permissions bug rather than a labelling one.
         */
        if (clawt_mount_get_relabel(mount) != CLAWT_RELABEL_NONE) {
            json_builder_set_member_name(builder, "relabel");
            json_builder_add_string_value(
                builder,
                clawt_enum_to_nick(CLAWT_TYPE_RELABEL,
                                   clawt_mount_get_relabel(mount)));
        }

        if (clawt_mount_get_size(mount) != NULL) {
            json_builder_set_member_name(builder, "size");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_size(mount));
        }

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);

    return json_generator_to_data(generator, NULL);
}

static gboolean
container_provision(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autofree gchar *mounts_json = NULL;
    const gchar *identifier;

    if (!clawt_pod_bridge_load_module(self->bridge, "container", error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"), g_strdup(self->image));
    g_hash_table_insert(params, g_strdup("image"), g_strdup(self->image));
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    mounts_json = clawt_container_computer_build_mount_json(
        clawt_computer_get_mounts(computer));
    g_hash_table_insert(params, g_strdup("mounts"),
                        g_steal_pointer(&mounts_json));

    result = clawt_pod_bridge_call(self->bridge, "container", "create",
                                   params, error);
    if (result == NULL) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    identifier = g_hash_table_lookup(result, "id");
    if (identifier != NULL && *identifier != '\0') {
        g_free(self->container_id);
        self->container_id = g_strdup(identifier);
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

static const gchar *
container_target(ClawtContainerComputer *self)
{
    /*
     * The id when we have it, the name otherwise.  A name is enough for
     * podman and is what survives a daemon restart, which is when the id we
     * remembered is gone.
     */
    return (self->container_id != NULL) ? self->container_id : self->name;
}

static gboolean
container_start(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_ABSENT &&
        !clawt_computer_provision(computer, error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STARTING, NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));

    result = clawt_pod_bridge_call(self->bridge, "container", "start",
                                   params, error);
    if (result == NULL) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    return TRUE;
}

static gboolean
container_stop(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPING, NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));

    result = clawt_pod_bridge_call(self->bridge, "container", "stop", params,
                                   error);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    if (result == NULL)
        return FALSE;

    if (!self->keep)
        return clawt_computer_teardown(computer, NULL);

    return TRUE;
}

static gboolean
container_teardown(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));

    result = clawt_pod_bridge_call(self->bridge, "container", "remove",
                                   params, error);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);
    g_clear_pointer(&self->container_id, g_free);

    return result != NULL;
}

static ClawtExecResult *
container_exec(ClawtComputer        *computer,
               const gchar * const  *argv,
               const gchar          *working_dir,
               guint                 timeout_seconds,
               GCancellable         *cancellable,
               GError              **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autofree gchar *command = NULL;
    g_autofree gchar *bounded = NULL;
    ClawtExecResult *exec_result;
    const gchar *output;
    gboolean truncated = FALSE;

    (void)timeout_seconds;
    (void)cancellable;

    /*
     * podomation's exec takes a command line rather than an argv, so the
     * arguments are quoted back into one.  g_shell_quote rather than plain
     * joining, or a filename with a space in it silently becomes two
     * arguments.
     */
    {
        g_autoptr(GString) joined = g_string_new(NULL);
        gsize i;

        if (working_dir != NULL) {
            g_autofree gchar *quoted_dir = g_shell_quote(working_dir);

            g_string_append_printf(joined, "cd %s && ", quoted_dir);
        }

        for (i = 0; argv[i] != NULL; i++) {
            g_autofree gchar *quoted = g_shell_quote(argv[i]);

            if (i > 0)
                g_string_append_c(joined, ' ');
            g_string_append(joined, quoted);
        }

        command = g_strdup(joined->str);
    }

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));
    g_hash_table_insert(params, g_strdup("command"), g_strdup(command));

    result = clawt_pod_bridge_call(self->bridge, "container", "exec", params,
                                   error);
    if (result == NULL)
        return NULL;

    output = g_hash_table_lookup(result, "output");
    bounded = clawt_computer_truncate_output(output, MAX_OUTPUT_BYTES,
                                             &truncated);

    exec_result = clawt_exec_result_new(0, bounded, "");
    clawt_exec_result_set_truncated(exec_result, truncated);

    return exec_result;
}

static gchar *
container_describe(ClawtComputer *computer)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    GPtrArray *mounts = clawt_computer_get_mounts(computer);
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    g_string_append_printf(out,
        "You have a container of your own, running %s. Anything you do "
        "inside it is isolated from the host.", self->image);

    if (mounts == NULL || mounts->len == 0) {
        g_string_append(out,
            " No host directories are shared with it, so you can only see "
            "what the image provides.");
        return g_string_free(g_steal_pointer(&out), FALSE);
    }

    g_string_append(out, " Shared from the host:");

    for (i = 0; i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        g_string_append_printf(out, "%s %s (%s)",
                               i > 0 ? "," : "",
                               clawt_mount_get_target(mount),
                               clawt_mount_get_mode(mount) ==
                                   CLAWT_MOUNT_MODE_RO
                               ? "read-only" : "read-write");
    }

    g_string_append_c(out, '.');

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static ClawtComputerType
container_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_CONTAINER;
}

static void
clawt_container_computer_dispose(GObject *object)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(object);

    g_clear_object(&self->bridge);

    G_OBJECT_CLASS(clawt_container_computer_parent_class)->dispose(object);
}

static void
clawt_container_computer_finalize(GObject *object)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(object);

    g_clear_pointer(&self->image, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->network, g_free);
    g_clear_pointer(&self->container_id, g_free);

    G_OBJECT_CLASS(clawt_container_computer_parent_class)->finalize(object);
}

static void
clawt_container_computer_class_init(ClawtContainerComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_container_computer_dispose;
    object_class->finalize = clawt_container_computer_finalize;

    computer_class->provision = container_provision;
    computer_class->start = container_start;
    computer_class->stop = container_stop;
    computer_class->teardown = container_teardown;
    computer_class->exec = container_exec;
    computer_class->describe = container_describe;
    computer_class->get_computer_type = container_get_computer_type;
}

static void
clawt_container_computer_init(ClawtContainerComputer *self)
{
    self->keep = FALSE;
}
