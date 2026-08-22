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
#include <string.h>
#include <unistd.h>

#define MAX_OUTPUT_BYTES (256 * 1024)

struct _ClawtContainerComputer {
    ClawtComputer parent_instance;

    ClawtPodBridge *bridge;
    gchar          *image;
    gchar          *name;
    gchar          *network;
    gchar          *container_id;
    gchar          *connection;
    gchar          *command;
    gboolean        keep;
};

G_DEFINE_FINAL_TYPE(ClawtContainerComputer, clawt_container_computer,
                    CLAWT_TYPE_COMPUTER)

/*
 * Resolves which podman to talk to.
 *
 * podomation defaults to /run/podman/podman.sock -- the *root* socket --
 * so a rootless desktop got "Could not connect: Permission denied" for a
 * daemon it was never going to be allowed to reach.  Rootless is the
 * normal case: the socket lives under XDG_RUNTIME_DIR and is served by
 * the podman.socket user unit.
 *
 * The system socket is still the answer when running as root, and an
 * explicit `connection` beats both.
 */
static gchar *
default_connection(void)
{
    const gchar *runtime_dir = g_get_user_runtime_dir();

    if (runtime_dir != NULL && geteuid() != 0) {
        g_autofree gchar *path = g_build_filename(runtime_dir, "podman",
                                                  "podman.sock", NULL);

        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return g_strdup_printf("unix://%s", path);
    }

    return g_strdup("unix:///run/podman/podman.sock");
}

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
    self->connection = default_connection();

    /*
     * Something long-lived, because a container computer exists to be
     * exec'd into.  A plain base image's entrypoint exits immediately,
     * and podman then reports the container as Exited while clawtilla
     * still called it provisioned -- the first sign was an exec failing
     * for a reason that made no sense.
     */
    self->command = g_strdup("[\"sleep\", \"infinity\"]");

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
clawt_container_computer_set_connection(ClawtContainerComputer *self,
                                        const gchar            *connection)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    /*
     * "unix" on its own is the documented way of saying "the local
     * podman", not a URI -- it was the example in every config we ship.
     * Treating it as one produced a connection to nothing.
     */
    if (connection == NULL || *connection == '\0' ||
        g_strcmp0(connection, "unix") == 0)
        return;

    g_free(self->connection);

    /* A bare path is a socket path; anything with a scheme is passed on. */
    self->connection = (*connection == '/')
                       ? g_strdup_printf("unix://%s", connection)
                       : g_strdup(connection);
}

const gchar *
clawt_container_computer_get_connection(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->connection;
}

void
clawt_container_computer_set_command(ClawtContainerComputer *self,
                                     const gchar            *command)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    if (command == NULL || *command == '\0')
        return;

    g_free(self->command);

    /*
     * A JSON array is passed through; anything else is one argv element,
     * which is what somebody writing `command: "sleep infinity"` in YAML
     * means and would otherwise get silently rejected by podman.
     */
    if (*command == '[') {
        self->command = g_strdup(command);
    } else {
        g_auto(GStrv) words = g_strsplit(command, " ", -1);
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonGenerator) generator = json_generator_new();
        g_autoptr(JsonNode) root = NULL;
        gsize i;

        json_builder_begin_array(builder);

        for (i = 0; words[i] != NULL; i++) {
            if (*words[i] != '\0')
                json_builder_add_string_value(builder, words[i]);
        }

        json_builder_end_array(builder);
        root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        self->command = json_generator_to_data(generator, NULL);
    }
}

const gchar *
clawt_container_computer_get_command(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->command;
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

    if (!clawt_pod_bridge_load_module_for(self->bridge, "container",
                                          self->connection, error))
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

    if (self->command != NULL)
        g_hash_table_insert(params, g_strdup("command"),
                            g_strdup(self->command));

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "create",
                                   params, error);

    /*
     * A container we left behind holds the name.
     *
     * The name is deliberately predictable -- clawt-<agent> -- so a
     * container can be found again after a daemon restart.  The cost is
     * that a previous one still sitting there makes create fail, and it
     * failed permanently: every later start hit the same collision, so a
     * daemon killed at the wrong moment bricked the agent until somebody
     * ran podman by hand.  Removing ours and trying once more is what a
     * person would do, and it is safe because the name is ours.
     */
    if (result == NULL && error != NULL && *error != NULL &&
        strstr((*error)->message, "already in use") != NULL) {
        g_autoptr(GHashTable) removal =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_autoptr(GHashTable) removed = NULL;

        g_hash_table_insert(removal, g_strdup("target"), g_strdup(self->name));
        g_hash_table_insert(removal, g_strdup("force"), g_strdup("true"));

        removed = clawt_pod_bridge_call_for(self->bridge, "container",
                                            self->connection, "remove",
                                            removal, NULL);

        if (removed != NULL) {
            g_clear_error(error);
            result = clawt_pod_bridge_call_for(self->bridge, "container",
                                               self->connection, "create",
                                               params, error);
        }
    }

    if (result == NULL) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    /*
     * No id means no container, whatever the call reported.  podman
     * answers a create for an image it does not have with a 404, and an
     * older podomation read that as success -- the agent then came up
     * "running" with a container that had never existed, and the first
     * sign was an exec failing much later for a reason that made no
     * sense.
     */
    /*
     * "container_id" is what podomation calls it.  We asked for "id",
     * which no result has ever had, so self->container_id was empty on
     * every successful create too -- masked only because every later
     * call falls back to the container's name, which podman also
     * accepts.
     */
    identifier = g_hash_table_lookup(result, "container_id");

    if (identifier == NULL || *identifier == '\0') {
        const gchar *detail = g_hash_table_lookup(result, "message");

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "podman created no container for image '%s'%s%s. "
                    "If the image is not pulled, `podman pull %s` first.",
                    self->image,
                    (detail != NULL && *detail != '\0') ? ": " : "",
                    (detail != NULL && *detail != '\0') ? detail : "",
                    self->image);

        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    g_free(self->container_id);
    self->container_id = g_strdup(identifier);

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

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "start",
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

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "stop", params,
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

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "remove",
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
    gint exit_status = 0;
    gboolean truncated = FALSE;

    /*
     * The timeout is not enforced here: podomation's exec runs to
     * completion over the Podman API and offers no deadline to pass on.
     * Saying so beats pretending -- an agent that believes a timeout
     * applies will wait for one that never arrives.  A command that must
     * be bounded should carry its own `timeout` in the container.
     */
    (void)timeout_seconds;

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CANCELLED,
                            "the command was cancelled before it started");
        return NULL;
    }

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

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "exec", params,
                                   error);
    if (result == NULL)
        return NULL;

    /*
     * "stdout"/"stderr" is what the module returns; "output" was a key no
     * result has ever had, so every container exec came back empty even
     * when the command had run and printed something.
     */
    output = g_hash_table_lookup(result, "stdout");

    if (output == NULL)
        output = g_hash_table_lookup(result, "output");

    /*
     * The exit status comes from the module rather than being assumed.
     * It used to be hardcoded to 0, so every command run in a container
     * -- including one that failed outright -- was recorded as having
     * succeeded, in the exec result and in the audit trail with it.
     */
    {
        const gchar *status = g_hash_table_lookup(result, "exit_code");

        if (status == NULL)
            status = g_hash_table_lookup(result, "exit_status");

        if (status != NULL) {
            exit_status = (gint)g_ascii_strtoll(status, NULL, 10);
        } else {
            /*
             * Said out loud rather than assumed to be success: a module
             * that does not report one leaves us unable to tell a failure
             * from a success, and silently calling it 0 is how a failing
             * command looks fine.
             */
            g_warning("container %s: the module reported no exit status; "
                      "treating the command as failed", self->name);
            exit_status = -1;
        }
    }
    bounded = clawt_computer_truncate_output(output, MAX_OUTPUT_BYTES,
                                             &truncated);

    /*
     * stderr is kept separate rather than folded into stdout.  A command
     * that writes a diagnostic and still succeeds is common, and merging
     * the two makes that indistinguishable from output the caller asked
     * for.
     */
    {
        const gchar *errors = g_hash_table_lookup(result, "stderr");
        gboolean errors_truncated = FALSE;
        g_autofree gchar *bounded_errors =
            clawt_computer_truncate_output(errors, MAX_OUTPUT_BYTES,
                                           &errors_truncated);

        exec_result = clawt_exec_result_new(exit_status, bounded,
                                            bounded_errors != NULL
                                            ? bounded_errors : "");
        clawt_exec_result_set_truncated(exec_result,
                                        truncated || errors_truncated);
    }

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
    g_clear_pointer(&self->connection, g_free);
    g_clear_pointer(&self->command, g_free);

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
