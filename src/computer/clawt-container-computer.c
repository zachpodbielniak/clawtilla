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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * How long the `podman container inspect` an adoption runs may take.  A
 * local metadata read; this bounds a podman socket that has gone quiet,
 * as the limits' own podman calls are bounded.
 */
#define CONTAINER_ADOPT_INSPECT_SECONDS (60)

/*
 * Consecutive samples at the memory limit before a machine counts as
 * sitting there: one sample at 96% is a spike, two in a row is a build
 * that has nowhere left to go.
 */
#define PRESSURE_SAMPLES (2)

/*
 * How long after one OOM report another is held back. The kill reaches
 * clawtilla twice -- podman's event and the cgroup counter at the next
 * sample -- and both are the same kill.
 */
#define OOM_REPORT_QUIET_US ((gint64)90 * G_USEC_PER_SEC)


struct _ClawtContainerComputer {
    ClawtComputer parent_instance;

    ClawtPodBridge *bridge;
    gchar          *image;
    gchar          *name;
    gchar          *network;
    gchar          *container_id;
    gchar          *connection;
    gchar          *command;
    gchar          *env;
    gchar          *ports;
    gboolean        keep;

    /* NULL for podman's random pair; see ..._set_selinux_level(). */
    gchar          *selinux_level;

    /* podman's init as PID 1; see clawt_container_computer_set_init(). */
    gboolean        init;

    /*
     * @lock guards what a worker thread writes and the main loop reads:
     * the limits (replaced by a refresh while an apply may be running),
     * the last apply's outcome, and the usage the sampler leaves.
     */
    GMutex          lock;

    /* NULL for none; see clawt_container_computer_set_limits(). */
    ClawtContainerLimits *limits;

    /* The last apply that changed anything; see ..._get_limits_outcome(). */
    ClawtContainerLimitsOutcome limits_outcome;
    gchar          *limits_note;
    gint64          limits_applied_at;

    /* The sampler's; see clawt_container_computer_take_usage(). */
    ClawtContainerUsage *usage;
    guint           pressure_samples;
    gboolean        memory_pressure;
    gint64          oom_kills_seen;
    gint64          last_oom_report;
    gchar          *cgroup_path;

    /*
     * Adoption across a daemon restart; see container_adopt().
     *
     * @reuse is whether a container found under our name may be kept at
     * all (clawt_container_computer_set_reuse()).  @adoption_asked is
     * whether this object has asked podman for its name yet: once per
     * object, so an agent that never had a container costs one 404 and
     * not one per start.  @recreate_reason is why the container found
     * must be made again, set by the reconcile and consumed by the next
     * start -- the only place anything is removed.  @last_recreate is the
     * last recreate as the operator is told it, for the Fleet page.
     */
    gboolean        reuse;
    gboolean        adoption_asked;
    gchar          *recreate_reason;
    gchar          *last_recreate;
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
clawt_container_computer_set_env(ClawtContainerComputer *self,
                                 const gchar            *env_json)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    g_free(self->env);
    self->env = (env_json != NULL && *env_json != '\0') ? g_strdup(env_json)
                                                        : NULL;
}

void
clawt_container_computer_set_ports(ClawtContainerComputer *self,
                                   const gchar            *ports_json)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    g_free(self->ports);
    self->ports = (ports_json != NULL && *ports_json != '\0')
                  ? g_strdup(ports_json) : NULL;
}

const gchar *
clawt_container_computer_get_container_id(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->container_id;
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

void
clawt_container_computer_set_reuse(ClawtContainerComputer *self,
                                   gboolean                reuse)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    self->reuse = reuse;
}

const gchar *
clawt_container_computer_get_last_recreate(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->last_recreate;
}

void
clawt_container_computer_set_limits(ClawtContainerComputer     *self,
                                    const ClawtContainerLimits *limits)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    g_mutex_lock(&self->lock);

    g_clear_pointer(&self->limits, clawt_container_limits_free);

    if (!clawt_container_limits_is_empty(limits))
        self->limits = clawt_container_limits_copy(limits);

    g_mutex_unlock(&self->lock);
}

ClawtContainerLimitsOutcome
clawt_container_computer_get_limits_outcome(ClawtContainerComputer  *self,
                                            gchar                  **note,
                                            gint64                  *at)
{
    ClawtContainerLimitsOutcome outcome;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self),
                         CLAWT_CONTAINER_LIMITS_UNCHANGED);

    g_mutex_lock(&self->lock);

    outcome = self->limits_outcome;

    if (note != NULL)
        *note = g_strdup(self->limits_note);

    if (at != NULL)
        *at = self->limits_applied_at;

    g_mutex_unlock(&self->lock);

    return outcome;
}

ClawtContainerUsage *
clawt_container_computer_get_usage(ClawtContainerComputer *self)
{
    ClawtContainerUsage *usage;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    g_mutex_lock(&self->lock);
    usage = (self->usage != NULL) ? clawt_container_usage_copy(self->usage)
                                  : NULL;
    g_mutex_unlock(&self->lock);

    return usage;
}

gboolean
clawt_container_computer_get_memory_pressure(ClawtContainerComputer *self)
{
    gboolean pressure;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), FALSE);

    g_mutex_lock(&self->lock);
    pressure = self->memory_pressure;
    g_mutex_unlock(&self->lock);

    return pressure;
}

gchar *
clawt_container_computer_get_memory_limit(ClawtContainerComputer *self)
{
    gchar *memory;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    g_mutex_lock(&self->lock);
    memory = (self->limits != NULL)
        ? g_strdup(clawt_container_limits_get_memory(self->limits)) : NULL;
    g_mutex_unlock(&self->lock);

    return memory;
}

gint64
clawt_container_computer_take_usage(ClawtContainerComputer    *self,
                                    const ClawtContainerUsage *usage,
                                    gboolean                  *pressure_changed)
{
    gint64 killed = 0;
    gboolean at_limit;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), 0);

    if (pressure_changed != NULL)
        *pressure_changed = FALSE;

    g_mutex_lock(&self->lock);

    /* Gone from the sample: stopped, so nothing it says still holds. */
    if (usage == NULL) {
        g_clear_pointer(&self->usage, clawt_container_usage_free);

        if (self->memory_pressure && pressure_changed != NULL)
            *pressure_changed = TRUE;

        self->memory_pressure = FALSE;
        self->pressure_samples = 0;
        g_mutex_unlock(&self->lock);
        return 0;
    }

    /*
     * The OOM counter, against what this object last saw. The first
     * reading is a baseline, not news: a kill from before the daemon
     * started is not something it can say anything useful about. A new
     * container -- another id -- starts its counter again, so the
     * baseline goes with it.
     */
    if (self->usage != NULL &&
        g_strcmp0(self->usage->container_id, usage->container_id) != 0)
        self->oom_kills_seen = -1;

    if (usage->oom_kills >= 0) {
        if (self->oom_kills_seen >= 0 && usage->oom_kills > self->oom_kills_seen)
            killed = usage->oom_kills - self->oom_kills_seen;

        self->oom_kills_seen = usage->oom_kills;
    }

    at_limit = clawt_container_usage_at_memory_limit(
        usage, (self->limits != NULL)
               ? clawt_container_limits_get_memory(self->limits) : NULL);

    self->pressure_samples = at_limit ? self->pressure_samples + 1 : 0;

    if ((self->pressure_samples >= PRESSURE_SAMPLES) != self->memory_pressure) {
        self->memory_pressure = !self->memory_pressure;

        if (pressure_changed != NULL)
            *pressure_changed = TRUE;
    }

    g_clear_pointer(&self->usage, clawt_container_usage_free);
    self->usage = clawt_container_usage_copy(usage);

    g_mutex_unlock(&self->lock);

    return killed;
}

gboolean
clawt_container_computer_claim_oom_report(ClawtContainerComputer *self,
                                          gint64                  now)
{
    gboolean claimed;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), FALSE);

    g_mutex_lock(&self->lock);

    claimed = self->last_oom_report == 0 ||
              now - self->last_oom_report >= OOM_REPORT_QUIET_US;

    if (claimed)
        self->last_oom_report = now;

    g_mutex_unlock(&self->lock);

    return claimed;
}

gchar *
clawt_container_computer_get_cgroup_path(ClawtContainerComputer *self)
{
    gchar *path;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    g_mutex_lock(&self->lock);
    path = g_strdup(self->cgroup_path);
    g_mutex_unlock(&self->lock);

    return path;
}

void
clawt_container_computer_set_cgroup_path(ClawtContainerComputer *self,
                                         const gchar            *path)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    g_mutex_lock(&self->lock);
    g_free(self->cgroup_path);
    self->cgroup_path = g_strdup(path);
    g_mutex_unlock(&self->lock);
}

const ClawtContainerLimits *
clawt_container_computer_get_limits(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->limits;
}

const gchar *
clawt_container_computer_get_name(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->name;
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

void
clawt_container_computer_set_selinux_level(ClawtContainerComputer *self,
                                           const gchar            *level)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    /*
     * Checked here as well as in the config: podomation refuses a
     * malformed level at create, as an HTTP-shaped error a long way from
     * the key, and a container that will not be created is worse than
     * one with podman's random pair and a warning saying why.
     */
    if (level != NULL && !clawt_selinux_level_is_valid(level)) {
        g_warning("container %s: '%s' is not an SELinux MCS level; it is "
                  "created with podman's random pair instead",
                  self->name, level);
        level = NULL;
    }

    g_free(self->selinux_level);
    self->selinux_level = g_strdup(level);
}

const gchar *
clawt_container_computer_get_selinux_level(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    return self->selinux_level;
}

void
clawt_container_computer_set_init(ClawtContainerComputer *self,
                                  gboolean                init)
{
    g_return_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self));

    self->init = init;
}

gboolean
clawt_container_computer_get_init(ClawtContainerComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), FALSE);

    return self->init;
}

GHashTable *
clawt_container_computer_build_create_params(ClawtContainerComputer *self)
{
    GHashTable *params;
    g_autofree gchar *mounts_json = NULL;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"), g_strdup(self->image));
    g_hash_table_insert(params, g_strdup("image"), g_strdup(self->image));
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    mounts_json = clawt_container_computer_build_mount_json(
        clawt_computer_get_mounts(CLAWT_COMPUTER(self)));
    g_hash_table_insert(params, g_strdup("mounts"),
                        g_steal_pointer(&mounts_json));

    if (self->command != NULL)
        g_hash_table_insert(params, g_strdup("command"),
                            g_strdup(self->command));

    /*
     * Absent rather than empty for podman's random pair: an older
     * podomation does not know the parameter at all, and one that does
     * reads its absence as "leave it to podman", which is the meaning.
     */
    if (self->selinux_level != NULL)
        g_hash_table_insert(params, g_strdup("selinux_level"),
                            g_strdup(self->selinux_level));

    /*
     * Always said, either way, so what was asked for is in the request
     * rather than inferred from a key's absence.  podomation reads the
     * string as a boolean (v1.0.5: true writes "init": true into the
     * libpod spec, false leaves it out), and an older module, which does
     * not know the parameter, ignores it -- a container without an init,
     * as before, which the next adoption reports as a recreate reason
     * once the module does.
     */
    g_hash_table_insert(params, g_strdup("init"),
                        g_strdup(self->init ? "true" : "false"));

    /*
     * Only the broker's containers carry these.  An agent's computer has
     * neither: what it needs arrives by mount, and a published port on
     * an agent's machine would be a door nobody asked for.
     */
    if (self->env != NULL)
        g_hash_table_insert(params, g_strdup("env"), g_strdup(self->env));

    if (self->ports != NULL)
        g_hash_table_insert(params, g_strdup("ports"),
                            g_strdup(self->ports));

    return params;
}

/*
 * An image reference with podman's implicit tag written out: "fedora" is
 * "fedora:latest".  A digest reference is left as it is.  Only the last
 * path component can carry the tag -- "localhost:5000/x" is a registry
 * port, not a tag.
 */
static gchar *
image_reference(const gchar *image)
{
    const gchar *slash;

    if (strchr(image, '@') != NULL)
        return g_strdup(image);

    slash = strrchr(image, '/');

    if (strchr(slash != NULL ? slash + 1 : image, ':') == NULL)
        return g_strconcat(image, ":latest", NULL);

    return g_strdup(image);
}

/*
 * Whether the image a container was made from is the one configured.
 *
 * By name, not by image id: a re-pulled tag is a new id, and recreating
 * every kept container because `podman pull` fetched a newer build of
 * the same tag would be this ticket's bug again with a better excuse.
 *
 * And loosely by name.  podman records the name it resolved, so the
 * configured "fedora:44" comes back as
 * "registry.fedoraproject.org/fedora:44"; one being the other with a
 * registry (or a path) in front, on a '/' boundary, is the same image.
 * A false "differs" here destroys a container on every start, and a
 * false "same" leaves one on an old image until it is rebuilt by hand --
 * the second is the one that can be undone.
 */
static gboolean
images_match(const gchar *wanted, const gchar *have)
{
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    gsize la;
    gsize lb;

    a = image_reference(wanted);
    b = image_reference(have);

    if (strcmp(a, b) == 0)
        return TRUE;

    la = strlen(a);
    lb = strlen(b);

    if (la < lb && b[lb - la - 1] == '/' && strcmp(b + lb - la, a) == 0)
        return TRUE;

    if (lb < la && a[la - lb - 1] == '/' && strcmp(a + la - lb, b) == 0)
        return TRUE;

    return FALSE;
}

/*
 * Whether two host paths name the same place: as written, cleaned up, or
 * once symlinks are resolved -- on Silverblue /home is a link to
 * /var/home, and the config and podman need not agree on which spelling
 * they keep.
 */
static gboolean
paths_match(const gchar *a, const gchar *b)
{
    g_autofree gchar *clean_a = NULL;
    g_autofree gchar *clean_b = NULL;
    gchar *real_a;
    gchar *real_b;
    gboolean same;

    if (g_strcmp0(a, b) == 0)
        return TRUE;

    if (a == NULL || b == NULL || !g_path_is_absolute(a) ||
        !g_path_is_absolute(b))
        return FALSE;

    clean_a = g_canonicalize_filename(a, NULL);
    clean_b = g_canonicalize_filename(b, NULL);

    if (strcmp(clean_a, clean_b) == 0)
        return TRUE;

    real_a = realpath(a, NULL);
    real_b = realpath(b, NULL);
    same = (real_a != NULL && real_b != NULL && strcmp(real_a, real_b) == 0);
    free(real_a);
    free(real_b);

    return same;
}

/*
 * The bind mounts that differ between the config and the container, as
 * "mounts changed (+/added, -/removed, ~/changed)", or NULL.
 *
 * Bind mounts only.  They are what a person adds and removes, they are
 * what podman lists under Mounts with a source and a mode, and they are
 * the case that matters: a folder added to the config and missing from
 * the machine.  A tmpfs is not in podman's Mounts at all, and a named
 * volume's Source is its mountpoint rather than the name configured, so
 * comparing either would answer "differs" for a container that does not
 * -- and a false "differs" destroys the container at every start.
 *
 * No Mounts member at all is "could not tell", not "every mount was
 * removed".
 */
static gchar *
mounts_changed(ClawtContainerComputer *self, JsonObject *inspected)
{
    GPtrArray *mounts = clawt_computer_get_mounts(CLAWT_COMPUTER(self));
    g_autoptr(GHashTable) have = NULL;
    g_autoptr(GHashTable) wanted = NULL;
    g_autoptr(GString) changes = NULL;
    JsonArray *listed;
    guint i;

    if (!json_object_has_member(inspected, "Mounts") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(inspected, "Mounts")))
        return NULL;

    listed = json_object_get_array_member(inspected, "Mounts");

    /* Destination to podman's object for it, binds only. */
    have = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < json_array_get_length(listed); i++) {
        JsonObject *one = json_array_get_object_element(listed, i);
        const gchar *type;
        const gchar *destination;

        if (one == NULL)
            continue;

        type = json_object_get_string_member_with_default(one, "Type", "");
        destination = json_object_get_string_member_with_default(
            one, "Destination", NULL);

        if (g_strcmp0(type, "bind") != 0 || destination == NULL ||
            !g_path_is_absolute(destination))
            continue;

        g_hash_table_insert(have, g_canonicalize_filename(destination, NULL),
                            one);
    }

    changes = g_string_new(NULL);
    wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Added or changed, in the config's order. */
    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);
        g_autofree gchar *destination = NULL;
        g_autofree gchar *source = NULL;
        JsonObject *found;

        if (clawt_mount_get_mount_type(mount) != CLAWT_MOUNT_BIND ||
            target == NULL || !g_path_is_absolute(target))
            continue;

        destination = g_canonicalize_filename(target, NULL);
        g_hash_table_add(wanted, g_strdup(destination));
        found = g_hash_table_lookup(have, destination);

        if (found == NULL) {
            g_string_append_printf(changes, "%s+%s",
                                   changes->len > 0 ? ", " : "",
                                   destination);
            continue;
        }

        source = clawt_mount_resolved_source(mount);

        if (!paths_match(source, json_object_get_string_member_with_default(
                                     found, "Source", NULL)) ||
            (clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RO) ==
                json_object_get_boolean_member_with_default(found, "RW",
                                                            TRUE))
            g_string_append_printf(changes, "%s~%s",
                                   changes->len > 0 ? ", " : "",
                                   destination);
    }

    /* Removed, in podman's order. */
    for (i = 0; i < json_array_get_length(listed); i++) {
        JsonObject *one = json_array_get_object_element(listed, i);
        const gchar *destination;
        g_autofree gchar *clean = NULL;

        if (one == NULL ||
            g_strcmp0(json_object_get_string_member_with_default(
                          one, "Type", ""), "bind") != 0)
            continue;

        destination = json_object_get_string_member_with_default(
            one, "Destination", NULL);

        if (destination == NULL || !g_path_is_absolute(destination))
            continue;

        clean = g_canonicalize_filename(destination, NULL);

        if (!g_hash_table_contains(wanted, clean))
            g_string_append_printf(changes, "%s-%s",
                                   changes->len > 0 ? ", " : "", clean);
    }

    if (changes->len == 0)
        return NULL;

    return g_strdup_printf("mounts changed (%s)", changes->str);
}

/*
 * Whether the container runs what the config says it should: our
 * command, a JSON array, against podman's Config.Cmd.  Unknown -- no
 * Cmd, or a command that is not an array of strings -- is "the same".
 */
static gboolean
command_matches(ClawtContainerComputer *self, JsonObject *inspected)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *config;
    JsonArray *have;
    JsonArray *want;
    guint i;

    if (self->command == NULL ||
        !json_object_has_member(inspected, "Config") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(inspected, "Config")))
        return TRUE;

    config = json_object_get_object_member(inspected, "Config");

    if (!json_object_has_member(config, "Cmd") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(config, "Cmd")))
        return TRUE;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, self->command, -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
        return TRUE;

    have = json_object_get_array_member(config, "Cmd");
    want = json_node_get_array(json_parser_get_root(parser));

    if (json_array_get_length(have) != json_array_get_length(want))
        return FALSE;

    for (i = 0; i < json_array_get_length(want); i++) {
        JsonNode *a = json_array_get_element(want, i);
        JsonNode *b = json_array_get_element(have, i);

        if (!JSON_NODE_HOLDS_VALUE(a) || !JSON_NODE_HOLDS_VALUE(b) ||
            g_strcmp0(json_node_get_string(a), json_node_get_string(b)) != 0)
            return FALSE;
    }

    return TRUE;
}

gboolean
clawt_container_computer_compare(ClawtContainerComputer  *self,
                                 const gchar             *inspected,
                                 gchar                  **reason,
                                 GError                 **error)
{
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GPtrArray) reasons = NULL;
    g_autoptr(GError) local = NULL;
    JsonNode *root;
    JsonObject *object = NULL;
    const gchar *image_name;
    g_autofree gchar *mounts = NULL;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), FALSE);
    g_return_val_if_fail(reason != NULL, FALSE);

    *reason = NULL;

    /*
     * `podman container inspect` prints an array of one; an object on
     * its own is accepted too, so a caller holding one need not wrap it.
     */
    parser = json_parser_new();

    if (inspected == NULL ||
        !json_parser_load_from_data(parser, inspected, -1, &local)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "podman's inspect of %s could not be read: %s",
                    self->name, local != NULL ? local->message
                                              : "it printed nothing");
        return FALSE;
    }

    root = json_parser_get_root(parser);

    if (root != NULL && JSON_NODE_HOLDS_ARRAY(root) &&
        json_array_get_length(json_node_get_array(root)) > 0)
        object = json_array_get_object_element(json_node_get_array(root), 0);
    else if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
        object = json_node_get_object(root);

    if (object == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "podman's inspect of %s describes no container",
                    self->name);
        return FALSE;
    }

    reasons = g_ptr_array_new_with_free_func(g_free);

    /*
     * Only what podman cannot change in place.  The limits are not here:
     * clawt_container_computer_apply_limits() gives an existing container
     * new ones with `podman update` at every start, so a changed limit
     * is never a reason to lose what the agent installed.
     */

    /* The image, by name (see images_match()). */
    image_name = json_object_get_string_member_with_default(object,
                                                            "ImageName", "");

    if (self->image != NULL && *image_name != '\0' &&
        !images_match(self->image, image_name))
        g_ptr_array_add(reasons,
                        g_strdup_printf("image changed (%s, the config "
                                        "wants %s)", image_name,
                                        self->image));

    /* The bind mounts (see mounts_changed()). */
    mounts = mounts_changed(self, object);

    if (mounts != NULL)
        g_ptr_array_add(reasons, g_steal_pointer(&mounts));

    /*
     * The SELinux level -- the one #13 sets at create and cannot be
     * changed afterwards.  Compared with the level this object would
     * create the container at, which the factory worked out from the
     * config the same way it does for a create.  No wanted level
     * (podman's random pair) accepts whatever pair the container has,
     * and a label with no level in it (a host without SELinux) cannot be
     * compared at all.
     */
    if (self->selinux_level != NULL) {
        g_autofree gchar *have = clawt_selinux_level_of_label(
            json_object_get_string_member_with_default(object,
                                                       "ProcessLabel",
                                                       NULL));

        if (have != NULL && strcmp(have, self->selinux_level) != 0)
            g_ptr_array_add(reasons,
                            g_strdup_printf("SELinux level changed (%s, the "
                                            "config wants %s)", have,
                                            self->selinux_level));
    }

    /* The command it runs, which podman fixes at create too. */
    if (!command_matches(self, object))
        g_ptr_array_add(reasons,
                        g_strdup_printf("command changed (the config wants "
                                        "%s)", self->command));

    /*
     * Whether podman's init runs as PID 1 (HostConfig.Init), which is
     * fixed at create like the rest.  Without it PID 1 is the command --
     * `sleep infinity` -- which never wait()s, so every orphan left by a
     * killed process tree stays a zombie for the life of the container.
     *
     * podman leaves Init out when it is false: libpod declares it
     * `json:"Init,omitempty"`, so a container made without an init -- every
     * one made before this setting -- has a HostConfig with no Init in it
     * at all.  So a readable HostConfig without Init *is* an answer: off.
     * Reading it as unknown would adopt every such container unchanged and
     * the init would never reach them.  Only a missing or unreadable
     * HostConfig, or an Init that is not a boolean, cannot be compared and
     * counts as unchanged, like every other unknown here.
     */
    if (json_object_has_member(object, "HostConfig") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "HostConfig"))) {
        JsonObject *host = json_object_get_object_member(object, "HostConfig");
        JsonNode *member = json_object_has_member(host, "Init")
                           ? json_object_get_member(host, "Init") : NULL;
        gboolean known = TRUE;
        gboolean have = FALSE;

        if (member != NULL) {
            known = JSON_NODE_HOLDS_VALUE(member) &&
                    json_node_get_value_type(member) == G_TYPE_BOOLEAN;
            have = known && json_node_get_boolean(member);
        }

        if (known && have != self->init)
            g_ptr_array_add(reasons,
                            g_strdup(self->init ? "init changed (now on)"
                                                : "init changed (now off)"));
    }

    if (reasons->len > 0) {
        g_ptr_array_add(reasons, NULL);
        *reason = g_strjoinv("; ", (gchar **)reasons->pdata);
    }

    return TRUE;
}

/*
 * `podman container inspect` of @target, the whole JSON.
 *
 * The CLI and not podomation's inspect, which answers the id, the
 * image's *id* and the state, and none of the mounts, the label or the
 * image name a recreate is decided on.  The limits already read and set
 * the same container through the same CLI, the same way: see
 * clawt_container_limits_apply().
 */
static gchar *
inspect_spec(ClawtContainerComputer  *self,
             const gchar             *target,
             GError                 **error)
{
    g_autofree gchar *podman = NULL;
    g_autofree gchar *out = NULL;
    g_autofree gchar *errors = NULL;
    g_auto(GStrv) prefix = NULL;
    g_autoptr(GPtrArray) argv = NULL;
    gint status = -1;
    guint i;

    podman = clawt_container_limits_find_podman();

    if (podman == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "the podman command is not on PATH or in "
                            "/usr/bin (Fedora: podman)");
        return NULL;
    }

    prefix = clawt_container_limits_podman_prefix(podman, self->connection);
    argv = g_ptr_array_new();

    for (i = 0; prefix[i] != NULL; i++)
        g_ptr_array_add(argv, prefix[i]);

    g_ptr_array_add(argv, (gpointer)"container");
    g_ptr_array_add(argv, (gpointer)"inspect");
    g_ptr_array_add(argv, (gpointer)target);
    g_ptr_array_add(argv, NULL);

    if (!clawt_run_argv_bounded((const gchar * const *)argv->pdata,
                                CONTAINER_ADOPT_INSPECT_SECONDS, &status,
                                &out, &errors, error))
        return NULL;

    if (status != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "`podman container inspect %s` exited with status %d: "
                    "%s", target, status,
                    (errors != NULL && *g_strstrip(errors) != '\0')
                    ? errors : "it gave no reason");
        return NULL;
    }

    return g_steal_pointer(&out);
}

static gboolean
container_provision(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    const gchar *identifier;

    /*
     * Before the bridge is even loaded: a relabel podman cannot apply
     * fails at *start*, as an HTTP 500 naming a syscall and a container
     * id, so the one chance to name the mount and the remedy is here.
     */
    if (!clawt_mount_list_check_relabel(clawt_computer_get_mounts(computer),
                                        error))
        return FALSE;

    if (!clawt_pod_bridge_load_module_for(self->bridge, "container",
                                          self->connection, error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    /* Built where it can be tested; see ..._build_create_params(). */
    params = clawt_container_computer_build_create_params(self);

    if (self->selinux_level != NULL)
        g_message("container %s: creating it at SELinux level %s",
                  self->name, self->selinux_level);

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "create",
                                   params, error);

    /*
     * The image is not pulled yet.
     *
     * create does not pull -- deliberately, so a typo fails in a second
     * rather than after a download -- but a first run against any image
     * the user has not fetched by hand then fails, and the suggestion
     * list a client offers is a list of images that do not work. Pulling
     * once on the miss makes picking one from that list mean what it
     * looks like it means.
     */
    if (result == NULL && error != NULL && *error != NULL &&
        (strstr((*error)->message, "no such image") != NULL ||
         strstr((*error)->message, "image not known") != NULL)) {
        g_autoptr(GHashTable) pull =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_autoptr(GHashTable) pulled = NULL;
        g_autoptr(GError) pull_error = NULL;

        g_hash_table_insert(pull, g_strdup("target"), g_strdup(self->image));
        g_hash_table_insert(pull, g_strdup("image"), g_strdup(self->image));

        g_message("container %s: pulling %s", self->name, self->image);

        pulled = clawt_pod_bridge_call_for(self->bridge, "container",
                                           self->connection, "pull", pull,
                                           &pull_error);

        if (pulled != NULL) {
            g_clear_error(error);
            result = clawt_pod_bridge_call_for(self->bridge, "container",
                                               self->connection, "create",
                                               params, error);
        } else {
            /*
             * Both reasons, because they are different problems: the
             * image may not exist, or the registry may be unreachable,
             * and only the pull's own error distinguishes them.
             */
            g_prefix_error(error, "%s could not be pulled (%s); ",
                           self->image,
                           pull_error != NULL ? pull_error->message
                                              : "no reason given");
        }
    }

    /*
     * A container already holds the name.
     *
     * The name is deliberately predictable -- clawt-<agent> -- so a
     * container can be found again after a daemon restart, and since the
     * adoption in container_reconcile() it is: a container under our name
     * is adopted, or removed for a stated reason, before anything gets
     * here.  So this is reached only when the adoption could not ask
     * podman (the inspect failed, and then the create got through) or
     * something made the container between the two.
     *
     * It used to be the ordinary path, and it removed whatever held the
     * name and made a new one -- which is how every kept container was
     * destroyed at every daemon restart, silently, with everything the
     * agent had installed in it.
     *
     * A kept container is not removed here any more.  The start fails,
     * saying why, and the next start asks podman again and adopts it.
     * One failed start can be retried; a removed container cannot be
     * brought back.  A container that is not kept is still replaced,
     * which is what not keeping it means -- and said, like any other
     * recreate.
     */
    if (result == NULL && error != NULL && *error != NULL &&
        strstr((*error)->message, "already in use") != NULL &&
        self->keep && self->reuse) {
        g_autofree gchar *why = g_strdup((*error)->message);
        const gchar *agent_id = clawt_computer_get_agent_id(computer);

        g_clear_error(error);
        self->adoption_asked = FALSE;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "a container named %s already exists and could not be "
                    "adopted, so nothing was removed (%s). Start the agent "
                    "again to adopt it, or stop it and run `clawtilla "
                    "computer rebuild %s` to replace it", self->name, why,
                    agent_id != NULL ? agent_id : "<agent>");
        g_warning("container %s: %s", self->name, (*error)->message);
    } else if (result == NULL && error != NULL && *error != NULL &&
               strstr((*error)->message, "already in use") != NULL) {
        g_autoptr(GHashTable) removal =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_autoptr(GHashTable) removed = NULL;

        g_hash_table_insert(removal, g_strdup("target"), g_strdup(self->name));
        g_hash_table_insert(removal, g_strdup("force"), g_strdup("true"));

        g_message("recreating %s: name taken by a container we could not "
                  "adopt (%s)", self->name,
                  !self->keep ? "keep is false"
                              : "this machine is made afresh at each start");

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

/*
 * Does podman still have this container?
 *
 * The state was written and never read back, so `podman rm -f` behind the
 * daemon's back left every surface reporting `running` until the daemon
 * exited.  podomation has had an `inspect` handler all along; nothing
 * called it.
 *
 * Only a 404 is treated as "it is gone".  Any other failure -- podman not
 * running, the socket unreadable, a timeout -- means the question could
 * not be asked, and answering it as "absent" would provision a second
 * container for an agent that already has one.  That the two are
 * distinguished only by the HTTP status in the message is podomation's
 * interface, not a choice made here: every failure from the module
 * arrives as G_IO_ERROR_FAILED.
 */
/*
 * A container already under our name, found on this object's first
 * reconcile: adopted, or marked to be made again and why.
 *
 * Every computer object starts ABSENT, and nothing carries a container id
 * across a daemon restart -- nor across the rebuild of the object a
 * settings change causes.  The reconcile used to answer ABSENT with
 * "nothing has been created" without asking, so the start provisioned,
 * the create collided with the container that was there all along, and
 * the collision handler removed it and made a new one.  Every kept
 * container was destroyed at every daemon restart, and everything
 * installed in it with it (VENTURE #21).
 *
 * So the first reconcile asks podman for the name.  Nothing there is a
 * 404 and the object stays ABSENT.  A container there is adopted: its
 * id, and STOPPED or RUNNING from podman's state.  Before keeping it,
 * it is compared with what the config would create now -- see
 * clawt_container_computer_compare() -- and a difference podman cannot
 * fix in place becomes @recreate_reason, which the start acts on.  The
 * reconcile itself removes nothing.
 *
 * A container that is not kept (keep: false), or one whose owner said
 * it is made afresh at each start (clawt_container_computer_set_reuse()),
 * is found and marked to be replaced -- what it was before, said aloud.
 *
 * A comparison that cannot be made -- no podman CLI, an inspect that
 * fails -- keeps the container.  The cost of that is a container on an
 * old setting until somebody rebuilds it; the cost of the opposite is
 * the bug this exists to fix.
 */
static gboolean
container_adopt(ClawtContainerComputer *self, GError **error)
{
    ClawtComputer *computer = CLAWT_COMPUTER(self);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autoptr(GError) local = NULL;
    const gchar *identifier;
    const gchar *status;

    if (!clawt_pod_bridge_load_module_for(self->bridge, "container",
                                          self->connection, error))
        return FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"), g_strdup(self->name));

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "inspect", params,
                                       &local);

    /* Only a 404 is "nothing there"; see container_reconcile(). */
    if (result == NULL) {
        if (local != NULL && strstr(local->message, "HTTP 404") != NULL) {
            self->adoption_asked = TRUE;
            return TRUE;
        }

        g_propagate_error(error, g_steal_pointer(&local));
        return FALSE;
    }

    self->adoption_asked = TRUE;

    identifier = g_hash_table_lookup(result, "container_id");
    status = g_hash_table_lookup(result, "state");

    g_free(self->container_id);
    self->container_id = (identifier != NULL && *identifier != '\0')
                         ? g_strdup(identifier) : NULL;

    g_clear_pointer(&self->recreate_reason, g_free);

    if (!self->keep) {
        self->recreate_reason = g_strdup(
            "keep is false, so the container an earlier run left is not "
            "reused");
    } else if (!self->reuse) {
        self->recreate_reason = g_strdup(
            "this machine is made afresh at each start");
    } else {
        g_autofree gchar *inspected = NULL;
        g_autofree gchar *reason = NULL;
        g_autoptr(GError) unread = NULL;

        inspected = inspect_spec(self, container_target(self), &unread);

        if (inspected == NULL ||
            !clawt_container_computer_compare(self, inspected, &reason,
                                              &unread))
            g_warning("container %s: adopted without comparing it with the "
                      "config (%s); it is kept as it is. If its image, "
                      "mounts or SELinux level should have changed, stop "
                      "the agent and run `clawtilla computer rebuild`",
                      self->name,
                      unread != NULL ? unread->message : "no reason given");
        else
            self->recreate_reason = g_steal_pointer(&reason);
    }

    if (self->recreate_reason == NULL)
        g_message("container %s: adopted the existing container (%s); "
                  "nothing was recreated", self->name,
                  (status != NULL && *status != '\0') ? status : "state "
                                                               "unknown");

    clawt_computer_set_state(computer,
                             g_strcmp0(status, "running") == 0
                             ? CLAWT_COMPUTER_STATE_RUNNING
                             : CLAWT_COMPUTER_STATE_STOPPED,
                             NULL);

    return TRUE;
}

static gboolean
container_reconcile(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autoptr(GError) local = NULL;

    /*
     * Nothing has been created by this object -- but one may be there
     * under our name, left by the daemon before this one or by the
     * object this one replaced.  Asked once per object: after that an
     * ABSENT state is one this object reached itself, and asking again
     * would turn every start of an agent with no container into a round
     * trip that can only answer 404.
     */
    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_ABSENT) {
        if (self->adoption_asked)
            return TRUE;

        return container_adopt(self, error);
    }

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "inspect", params,
                                       &local);

    if (result != NULL)
        return TRUE;

    if (local == NULL || strstr(local->message, "HTTP 404") == NULL) {
        g_propagate_error(error, g_steal_pointer(&local));
        return FALSE;
    }

    /*
     * The remembered id goes with it.  container_target() prefers the id
     * over the name, so a recreated container would still be started and
     * exec'd at the dead one -- which is the 404 an agent saw from its
     * own commands long after the operator had recreated everything.
     * container_teardown() already clears it for the same reason.
     */
    g_clear_pointer(&self->container_id, g_free);
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);

    return TRUE;
}

gboolean
clawt_container_computer_apply_limits(ClawtContainerComputer  *self,
                                      GError                 **error)
{
    g_autoptr(ClawtContainerLimits) limits = NULL;
    g_autofree gchar *note = NULL;
    ClawtContainerLimitsOutcome outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

    g_return_val_if_fail(CLAWT_IS_CONTAINER_COMPUTER(self), FALSE);

    /*
     * A copy, taken under the lock: a refresh on the main loop replaces
     * the limits while this runs on a worker, and the old value would be
     * freed under the podman call reading it.
     */
    g_mutex_lock(&self->lock);
    limits = (self->limits != NULL) ? clawt_container_limits_copy(self->limits)
                                    : NULL;
    g_mutex_unlock(&self->lock);

    if (limits == NULL)
        return TRUE;

    if (!clawt_container_limits_apply(limits, self->connection,
                                      container_target(self), &outcome,
                                      &note, error))
        return FALSE;

    /*
     * Which path it took, in the log every time and on the object for
     * the Fleet page: "live" and "restart required" are different
     * answers to "did my change land".
     */
    switch (outcome) {
    case CLAWT_CONTAINER_LIMITS_UNCHANGED:
        return TRUE;
    case CLAWT_CONTAINER_LIMITS_APPLIED:
    case CLAWT_CONTAINER_LIMITS_APPLIED_LIVE:
        g_message("container %s: limits applied with podman update (%s): %s",
                  self->name, clawt_container_limits_outcome_nick(outcome),
                  note);
        break;
    case CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED:
        g_warning("container %s: %s", self->name, note);
        break;
    }

    g_mutex_lock(&self->lock);
    self->limits_outcome = outcome;
    g_free(self->limits_note);
    self->limits_note = g_steal_pointer(&note);
    self->limits_applied_at = g_get_real_time();
    g_mutex_unlock(&self->lock);

    return TRUE;
}

/*
 * Removes the adopted container @recreate_reason condemned, saying why.
 *
 * One g_message per recreate, naming the container and the reason, and
 * for a kept container the same sentence kept as @last_recreate, which
 * the daemon puts on the Fleet page: somebody whose agent just lost what
 * it installed is told what did it.  A removal that fails keeps the
 * reason for the next start, and creates nothing.
 */
static gboolean
container_recreate(ClawtContainerComputer *self, GError **error)
{
    g_autofree gchar *reason = g_steal_pointer(&self->recreate_reason);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autoptr(GError) local = NULL;

    g_message("recreating %s: %s", self->name, reason);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("target"),
                        g_strdup(container_target(self)));
    g_hash_table_insert(params, g_strdup("force"), g_strdup("true"));

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "remove", params,
                                       &local);

    /* Gone already is as good as removed. */
    if (result == NULL &&
        (local == NULL || strstr(local->message, "HTTP 404") == NULL)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "%s must be recreated (%s), and removing it failed: %s. "
                    "Nothing was created; the next start tries again",
                    self->name, reason,
                    local != NULL ? local->message : "no reason given");
        self->recreate_reason = g_steal_pointer(&reason);
        return FALSE;
    }

    if (self->keep && self->reuse) {
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autofree gchar *when = g_date_time_format(now, "%F %T");

        g_free(self->last_recreate);
        self->last_recreate = g_strdup_printf("recreated %s at %s: %s",
                                              self->name, when, reason);
    }

    g_clear_pointer(&self->container_id, g_free);
    clawt_computer_set_state(CLAWT_COMPUTER(self),
                             CLAWT_COMPUTER_STATE_ABSENT, NULL);

    return TRUE;
}

static gboolean
container_start(ClawtComputer *computer, GError **error)
{
    ClawtContainerComputer *self = CLAWT_CONTAINER_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    /*
     * clawt_computer_start() has already reconciled by the time this
     * runs, so the state below is podman's answer rather than what the
     * last operation happened to write.  Without that a start found its
     * own stale RUNNING, skipped provisioning, and asked podman to start
     * a container that no longer existed -- so the one command an
     * operator reaches for to recover was the one that could not.
     */

    /*
     * An adopted container that must be made again goes first, so the
     * provision below makes the new one; see container_recreate().
     */
    if (self->recreate_reason != NULL && !container_recreate(self, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_ABSENT &&
        !clawt_computer_provision(computer, error))
        return FALSE;

    /*
     * The limits, before the start rather than after it, so a new
     * container never runs a moment without them -- and on every start,
     * so a changed limit reaches a kept container in place. Asking podman
     * what it has first means an unchanged limit costs one inspect and
     * no update.
     *
     * A failure fails the start. Somebody set a memory limit so that an
     * agent's build could not take the desktop down; starting it without
     * one and saying nothing would be the quiet version of exactly that.
     */
    if (!clawt_container_computer_apply_limits(self, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

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

    {
        g_autoptr(GError) local = NULL;

        result = clawt_pod_bridge_call_for(self->bridge, "container",
                                           self->connection, "stop", params,
                                           &local);

        if (result == NULL) {
            /*
             * A container that is not there is a container that is
             * stopped.  Podman answers 404 for it, and reporting that
             * as a failure to stop put an HTTP status and a URL in front
             * of somebody who had pressed Stop on a machine that had
             * already gone -- which is the ordinary case for an agent
             * that has never been started.
             *
             * The remembered id goes with it, for the reason
             * container_reconcile() clears it: container_target()
             * prefers the id, so a recreated container would still be
             * addressed at the dead one.
             */
            if (local != NULL && strstr(local->message, "HTTP 404") != NULL) {
                g_clear_pointer(&self->container_id, g_free);
                clawt_computer_set_state(computer,
                                         CLAWT_COMPUTER_STATE_ABSENT, NULL);
                return TRUE;
            }

            /*
             * Not STOPPED.  Setting it before reading the result -- which
             * is what this did -- reported the machine as stopped on the
             * one path where it demonstrably is not.
             */
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     local != NULL ? local->message : NULL);
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

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

    /*
     * Forced, because tearing a computer down is a decision that has
     * already been made.  podman refuses to remove a running container
     * without it, so this failed for exactly the case it exists to
     * handle -- an agent being deleted while its container is up.
     */
    g_hash_table_insert(params, g_strdup("force"), g_strdup("true"));

    result = clawt_pod_bridge_call_for(self->bridge, "container",
                                       self->connection, "remove",
                                   params, error);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);
    g_clear_pointer(&self->container_id, g_free);

    /* Nothing is left to recreate: the next start makes one afresh. */
    g_clear_pointer(&self->recreate_reason, g_free);

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

    /*
     * The deadline is held on this side; podomation's exec runs to
     * completion and has none to pass down.  Shared with the distrobox
     * backend, which has exactly the same problem -- two copies of a
     * wait-with-a-lock would differ once, in the case nobody looked at.
     */
    result = clawt_pod_bridge_call_bounded(self->bridge, "container",
                                           self->connection, "exec", params,
                                           timeout_seconds, error);

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
    bounded = clawt_computer_truncate_output(output, CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
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
            clawt_computer_truncate_output(errors, CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
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
    g_autoptr(GString) out = g_string_new(NULL);

    g_string_append_printf(out,
        "You have %s, running %s. Anything you do "
        "inside it is isolated from the host.",
        clawt_computer_is_shared(computer) ? "a container" : "a container of your own",
        self->image);

    clawt_computer_describe_mounts(computer, out);

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
    g_clear_pointer(&self->env, g_free);
    g_clear_pointer(&self->ports, g_free);
    g_clear_pointer(&self->selinux_level, g_free);
    g_clear_pointer(&self->limits, clawt_container_limits_free);
    g_clear_pointer(&self->limits_note, g_free);
    g_clear_pointer(&self->usage, clawt_container_usage_free);
    g_clear_pointer(&self->cgroup_path, g_free);
    g_mutex_clear(&self->lock);
    g_clear_pointer(&self->recreate_reason, g_free);
    g_clear_pointer(&self->last_recreate, g_free);

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
    computer_class->reconcile = container_reconcile;
}

static void
clawt_container_computer_init(ClawtContainerComputer *self)
{
    self->keep = FALSE;
    self->reuse = TRUE;
    self->oom_kills_seen = -1;
    g_mutex_init(&self->lock);
}
