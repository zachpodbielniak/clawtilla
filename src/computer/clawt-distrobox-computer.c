/*
 * clawt-distrobox-computer.c - A distrobox per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-distrobox-computer.h"

struct _ClawtDistroboxComputer {
    ClawtComputer parent_instance;

    ClawtPodBridge *bridge;
    gchar          *image;
    gchar          *name;
    gchar          *home;
    gchar          *packages;
    gchar          *flags;
    gboolean        share_home;
    gboolean        init;
    gboolean        keep;
};

G_DEFINE_FINAL_TYPE(ClawtDistroboxComputer, clawt_distrobox_computer,
                    CLAWT_TYPE_COMPUTER)

/*
 * A box name that cannot collide with one somebody made by hand.
 *
 * The same shape as the container backend's, and for the same reason:
 * these machines already have boxes called `dev` and `util`, and an
 * agent called `dev` adopting one of those would be a fleet operation
 * reaching into somebody's development environment.
 */
static gchar *
default_name(const gchar *agent_id)
{
    return g_strdup_printf("clawt-%s", agent_id);
}

ClawtComputer *
clawt_distrobox_computer_new(const gchar    *agent_id,
                             ClawtPodBridge *bridge,
                             const gchar    *image)
{
    ClawtDistroboxComputer *self;

    g_return_val_if_fail(agent_id != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_DISTROBOX_COMPUTER, NULL);
    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    self->bridge = bridge != NULL ? g_object_ref(bridge) : NULL;
    self->image = g_strdup(image);
    self->name = default_name(agent_id);

    /*
     * Kept by default, unlike a plain container.  Making one pulls an
     * image and then runs a package install inside it, and it is the
     * kind of computer somebody chose precisely so that what an agent
     * installs survives -- so discarding it at every stop would mean
     * paying that cost on every start.
     */
    self->keep = TRUE;

    return CLAWT_COMPUTER(self);
}

void
clawt_distrobox_computer_set_name(ClawtDistroboxComputer *self,
                                  const gchar            *name)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    if (name == NULL || *name == '\0')
        return;

    g_free(self->name);
    self->name = g_strdup(name);
}

void
clawt_distrobox_computer_set_home(ClawtDistroboxComputer *self,
                                  const gchar            *home)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    g_free(self->home);
    self->home = (home != NULL && *home != '\0') ? clawt_expand_path(home)
                                                 : NULL;
}

void
clawt_distrobox_computer_set_share_home(ClawtDistroboxComputer *self,
                                        gboolean                share)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    self->share_home = share;
}

gchar *
clawt_distrobox_computer_resolve_home(ClawtDistroboxComputer *self)
{
    const gchar *agent_id;

    g_return_val_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self), NULL);

    /*
     * NULL means "pass no --home", which is distrobox's default and
     * therefore the operator's own home directory.  It is only ever
     * reached by asking for it.
     */
    if (self->share_home)
        return NULL;

    if (self->home != NULL)
        return g_strdup(self->home);

    /*
     * Derived here rather than handed in by the factory, the same way
     * the VM backend derives its own state directory: the thing that
     * owns the files is the thing that should know where they are, and
     * a second spelling elsewhere is how a path comes to name a
     * directory nothing creates.
     *
     * Deliberately not inside the workspace. That holds the persona and
     * the notes, is mounted into every computer, and is now sometimes a
     * git repository -- a home directory scattering dotfiles and caches
     * through it would be a mess in somebody's version control.
     */
    agent_id = clawt_computer_get_agent_id(CLAWT_COMPUTER(self));

    return g_build_filename(g_get_user_data_dir(), "clawtilla", "boxes",
                            agent_id != NULL ? agent_id : "agent", "home",
                            NULL);
}

void
clawt_distrobox_computer_set_packages(ClawtDistroboxComputer *self,
                                      const gchar            *packages)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    g_free(self->packages);
    self->packages = g_strdup(packages);
}

void
clawt_distrobox_computer_set_flags(ClawtDistroboxComputer *self,
                                   const gchar            *flags)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    g_free(self->flags);
    self->flags = g_strdup(flags);
}

void
clawt_distrobox_computer_set_init(ClawtDistroboxComputer *self, gboolean init)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    self->init = init;
}

void
clawt_distrobox_computer_set_keep(ClawtDistroboxComputer *self, gboolean keep)
{
    g_return_if_fail(CLAWT_IS_DISTROBOX_COMPUTER(self));

    self->keep = keep;
}

gchar *
clawt_distrobox_computer_build_volume_args(GPtrArray *mounts)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        g_autofree gchar *source = clawt_mount_resolved_source(mount);
        g_autofree gchar *spec = NULL;
        g_autofree gchar *quoted = NULL;
        g_autoptr(GString) options = g_string_new(NULL);

        /*
         * A tmpfs has no source and distrobox has no way to ask for one
         * -- it takes --volume, which is podman's bind syntax.  Skipped
         * rather than rendered as a bind of nothing, which podman would
         * accept and mount the empty string over the target.
         */
        if (source == NULL || *source == '\0')
            continue;

        if (clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RO)
            g_string_append(options, ":ro");

        switch (clawt_mount_get_relabel(mount)) {
        case CLAWT_RELABEL_SHARED:
            g_string_append(options, ":z");
            break;

        case CLAWT_RELABEL_PRIVATE:
            g_string_append(options, ":Z");
            break;

        default:
            break;
        }

        spec = g_strdup_printf("%s:%s%s", source,
                               clawt_mount_get_target(mount), options->str);

        /*
         * Quoted, because the module splits this back into separate
         * arguments with g_shell_parse_argv -- a path with a space in
         * it would otherwise become two mounts that both fail.
         */
        quoted = g_shell_quote(spec);

        if (out->len > 0)
            g_string_append_c(out, ' ');

        g_string_append(out, quoted);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * Whether the box is already there.
 *
 * Asked of distrobox rather than remembered, for the reason the VM
 * backend records: a box is somebody else's object and can be deleted
 * from a terminal without telling us, so provisioning from our own
 * memory of it means an agent restarted against a box that has gone
 * skips creating one and then reports success about a machine that is
 * not there.
 */
static gboolean
distrobox_exists(ClawtDistroboxComputer *self, gboolean *exists)
{
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    const gchar *answer;

    *exists = FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    result = clawt_pod_bridge_call(self->bridge, "distrobox", "exists",
                                   params, NULL);

    if (result == NULL)
        return FALSE;

    answer = g_hash_table_lookup(result, "exists");
    *exists = (g_strcmp0(answer, "true") == 0);

    return TRUE;
}

static gboolean
distrobox_provision(ClawtComputer *computer, GError **error)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autofree gchar *volumes = NULL;
    gboolean exists = FALSE;

    if (!clawt_pod_bridge_load_module_for(self->bridge, "distrobox", NULL,
                                          error))
        return FALSE;

    /*
     * Idempotent, and asked rather than assumed.  `distrobox create` on
     * a name that already exists refuses, so a restart against a box
     * that survived would fail at provision -- which is the ordinary
     * case here, since keep defaults to TRUE.
     */
    if (distrobox_exists(self, &exists) && exists) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED,
                                 NULL);
        return TRUE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    volumes = clawt_distrobox_computer_build_volume_args(
        clawt_computer_get_mounts(computer));

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    if (self->image != NULL)
        g_hash_table_insert(params, g_strdup("image"), g_strdup(self->image));

    {
        g_autofree gchar *home = clawt_distrobox_computer_resolve_home(self);

        if (home != NULL) {
            /*
             * Created before distrobox is asked for it. distrobox will
             * make one, but as the *invoking* user in a directory that
             * may not exist yet -- and a home it cannot create is a box
             * that comes up with no writable home at all.
             */
            g_mkdir_with_parents(home, 0700);
            g_hash_table_insert(params, g_strdup("home"),
                                g_steal_pointer(&home));
        }
    }

    if (self->packages != NULL)
        g_hash_table_insert(params, g_strdup("packages"),
                            g_strdup(self->packages));

    if (volumes != NULL && *volumes != '\0')
        g_hash_table_insert(params, g_strdup("volumes"),
                            g_strdup(volumes));

    if (self->flags != NULL)
        g_hash_table_insert(params, g_strdup("flags"), g_strdup(self->flags));

    if (self->init)
        g_hash_table_insert(params, g_strdup("init"), g_strdup("true"));

    result = clawt_pod_bridge_call(self->bridge, "distrobox", "create",
                                   params, error);

    if (result == NULL) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

static gboolean
distrobox_start(ClawtComputer *computer, GError **error)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    if (!clawt_pod_bridge_load_module_for(self->bridge, "distrobox", NULL,
                                          error))
        return FALSE;

    /*
     * Always, not only from ABSENT.  A stop leaves STOPPED, and a box
     * deleted from a terminal in between leaves that memory describing
     * something that is gone -- the failure the VM backend already
     * records, where a computer reported as started had no machine
     * anywhere behind it.  Provisioning is idempotent, so the check
     * costs one `distrobox list`.
     */
    if (!distrobox_provision(computer, error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STARTING, NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    result = clawt_pod_bridge_call(self->bridge, "distrobox", "start",
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
distrobox_stop(ClawtComputer *computer, GError **error)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPING, NULL);

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    result = clawt_pod_bridge_call(self->bridge, "distrobox", "stop", params,
                                   error);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    if (result == NULL)
        return FALSE;

    if (!self->keep)
        return clawt_computer_teardown(computer, NULL);

    return TRUE;
}

static gboolean
distrobox_teardown(ClawtComputer *computer, GError **error)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    if (!clawt_pod_bridge_load_module_for(self->bridge, "distrobox", NULL,
                                          error))
        return FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));

    result = clawt_pod_bridge_call(self->bridge, "distrobox", "remove",
                                   params, error);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);

    return result != NULL;
}

static ClawtExecResult *
distrobox_exec(ClawtComputer        *computer,
               const gchar * const  *argv,
               const gchar          *working_dir,
               guint                 timeout_seconds,
               GCancellable         *cancellable,
               GError              **error)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autofree gchar *command = NULL;
    ClawtExecResult *exec_result;
    const gchar *output;
    const gchar *code;

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CANCELLED,
                            "the command was cancelled before it started");
        return NULL;
    }

    /*
     * Each argument quoted and then joined, exactly as the container and
     * VM backends do.  That is what makes `>` and `|` reach the command
     * as literal text on every backend rather than redirecting on one of
     * them -- an agent should not have to know which kind of computer it
     * has to predict what its own arguments mean.
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
    g_hash_table_insert(params, g_strdup("name"), g_strdup(self->name));
    g_hash_table_insert(params, g_strdup("command"), g_strdup(command));

    /*
     * The deadline is held on this side.  The module runs distrobox to
     * completion and offers none to pass down, which used to be written
     * here as a reason not to have one -- leaving `timeout` accepted,
     * defaulted to 120 by the tool schema, and dropped.  The same wait
     * the container backend uses, from the same function, because two
     * of these would differ exactly once.
     */
    result = clawt_pod_bridge_call_bounded(self->bridge, "distrobox", NULL,
                                           "exec", params, timeout_seconds,
                                           error);

    if (result == NULL)
        return NULL;

    output = g_hash_table_lookup(result, "stdout");
    code = g_hash_table_lookup(result, "exit_code");

    exec_result = clawt_exec_result_new(
        code != NULL ? (gint)g_ascii_strtoll(code, NULL, 10) : 0,
        output,
        (const gchar *)g_hash_table_lookup(result, "stderr"));

    return exec_result;
}

static gchar *
distrobox_describe(ClawtComputer *computer)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(computer);
    g_autoptr(GString) out = g_string_new(NULL);

    g_string_append_printf(out,
        "You have a distrobox called %s%s%s. It is a container, so a "
        "package you install stays inside it -- but it is deliberately "
        "wired into the machine around it: you run as the same user as "
        "the operator, you can see their sockets and displays, and "
        "`distrobox-host-exec <command>` runs a command out on the host "
        "itself rather than in here.",
        self->name,
        self->image != NULL ? ", running " : "",
        self->image != NULL ? self->image : "");

    /*
     * The home is the thing an agent most needs to be told, because the
     * two cases look identical from inside and are completely different
     * in what they touch.  An agent that believes its home is its own
     * will write scratch files into somebody's real one.
     */
    {
        g_autofree gchar *home = clawt_distrobox_computer_resolve_home(self);

        if (home != NULL) {
            g_string_append_printf(out,
                "\n\nYour home directory is %s, which belongs to you "
                "alone. The operator's own home is not mounted.", home);
        } else {
            g_string_append(out,
                "\n\nYour home directory *is the operator's real home "
                "directory*, mounted through from the host. Everything "
                "you write there they will find in their own files, and "
                "everything of theirs is readable to you -- including "
                "credentials they never meant to hand to an agent. Treat "
                "it as their machine, because it is.");
        }
    }

    if (self->init) {
        g_string_append(out,
            "\n\nAn init system is running inside the box, so systemctl "
            "and user units work.");
    }

    clawt_computer_describe_mounts(computer, out);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static ClawtComputerType
distrobox_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_DISTROBOX;
}

static void
clawt_distrobox_computer_dispose(GObject *object)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(object);

    g_clear_object(&self->bridge);

    G_OBJECT_CLASS(clawt_distrobox_computer_parent_class)->dispose(object);
}

static void
clawt_distrobox_computer_finalize(GObject *object)
{
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(object);

    g_clear_pointer(&self->image, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->home, g_free);
    g_clear_pointer(&self->packages, g_free);
    g_clear_pointer(&self->flags, g_free);

    G_OBJECT_CLASS(clawt_distrobox_computer_parent_class)->finalize(object);
}

static void
clawt_distrobox_computer_class_init(ClawtDistroboxComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_distrobox_computer_dispose;
    object_class->finalize = clawt_distrobox_computer_finalize;

    computer_class->provision = distrobox_provision;
    computer_class->start = distrobox_start;
    computer_class->stop = distrobox_stop;
    computer_class->teardown = distrobox_teardown;
    computer_class->exec = distrobox_exec;
    computer_class->describe = distrobox_describe;
    computer_class->get_computer_type = distrobox_get_computer_type;
}

static void
clawt_distrobox_computer_init(ClawtDistroboxComputer *self)
{
    (void)self;
}
