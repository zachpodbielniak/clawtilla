/*
 * clawt-computer-factory.c - Building a computer from configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-computer-factory.h"
#include "computer/clawt-null-computer.h"
#include "computer/clawt-host-computer.h"
#include "computer/clawt-container-computer.h"
#include "computer/clawt-distrobox-computer.h"
#include "computer/clawt-vm-computer.h"
#include "computer/clawt-ssh-computer.h"

static void
apply_mounts(ClawtComputer    *computer,
             ClawtAgentConfig *agent_config,
             GPtrArray        *default_mounts)
{
    g_autoptr(GPtrArray) own = clawt_agent_config_get_mounts(agent_config);
    g_autoptr(GPtrArray) mounts = NULL;
    ClawtComputerType type = clawt_computer_get_computer_type(computer);
    guint i;

    /*
     * The fleet's shared folders, then the agent's own.
     *
     * Only where a mount is a kernel mount. On a host computer the
     * mount list *is* the confinement allowlist, so applying a fleet
     * default there would quietly widen what a host agent may reach --
     * which a convenience has no business doing.
     *
     * clawt_mount_merge_defaults() drops a default whose target the
     * agent has already claimed. Adding both would put two mounts at
     * one path, which validation refuses -- so an agent that customised
     * a single shared folder would stop starting, naming the path its
     * owner had deliberately chosen.
     */
    if (default_mounts != NULL &&
        clawt_computer_type_takes_mounts(type) &&
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.default_mounts")) {
        g_autoptr(GPtrArray) mine = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_mount_free);
        const gchar *agent_id = clawt_agent_config_get_id(agent_config);
        const gchar *team = clawt_agent_config_get_string(agent_config,
                                                          "team");
        guint d;

        /*
         * Narrowed to the ones that cover this agent before anything is
         * merged, so a folder scoped to a team reaches that team and
         * nobody else. Through clawt_mount_covers(), which is the same
         * rule integrations use -- "who gets this" has one answer in the
         * tree rather than two that differ on the case nobody tested.
         */
        for (d = 0; d < default_mounts->len; d++) {
            ClawtMount *candidate = g_ptr_array_index(default_mounts, d);

            if (clawt_mount_covers(candidate, agent_id, team))
                g_ptr_array_add(mine, clawt_mount_copy(candidate));
        }

        mounts = clawt_mount_merge_defaults(mine, own);
    } else {
        mounts = g_ptr_array_ref(own);
    }

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        /*
         * The mount type is filled in from the backend when the config did
         * not say, so a user writing source/target/mode gets a bind mount in
         * a container and a virtiofs share in a VM without having to know
         * that those are the right spellings.
         */
        if (clawt_mount_get_mount_type(mount) == CLAWT_MOUNT_BIND &&
            type == CLAWT_COMPUTER_VM)
            clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);

        clawt_computer_add_mount(computer, mount);
    }

    /*
     * And the agent's own workspace, unless it asked not to have it.
     *
     * It holds the persona, the notes and MEMORY.md, and until now it
     * lived only on the host -- so an agent read the files describing it
     * with tools that run out here, and worked in a machine that could
     * not see them. Anything it wrote inside went somewhere the host
     * never looked: a VM agent's screenshots were captured perfectly and
     * were unreachable, which is indistinguishable from a capture that
     * failed.
     *
     * Here rather than beside the exchange in the daemon, because it is
     * derivable from the config alone -- which is this function's whole
     * input, and what makes it testable without a hypervisor.
     *
     * Asked of clawt_computer_type_shares_host_paths() rather than
     * `type != none`, which is what it was. An ssh computer is a
     * different machine: there is no mount to make, so declaring the
     * workspace at /mnt/clawtilla/workspace over there would put a
     * directory that does not exist into the agent's allowlist and into
     * its prompt, and it would find out by reading nothing at a path it
     * had been told about.
     */
    if (clawt_computer_type_shares_host_paths(type) &&
        clawt_agent_config_get_boolean(agent_config, "computer.workspace")) {
        g_autofree gchar *workspace =
            clawt_agent_config_get_workspace(agent_config);

        if (workspace != NULL) {
            ClawtMount *mount = clawt_mount_new(workspace,
                                                CLAWT_WORKSPACE_MOUNT_POINT);

            /* The point is that it is the same file on both sides. */
            clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
            clawt_mount_set_create(mount, TRUE);
            clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);

            if (type == CLAWT_COMPUTER_VM)
                clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);

            clawt_computer_add_mount(computer, mount);
            clawt_mount_free(mount);
        }
    }
}

static ClawtSandbox *
build_sandbox(ClawtAgentConfig *agent_config)
{
    ClawtSandbox *sandbox;
    ClawtConfineMode mode;
    g_autofree gchar *root = NULL;
    g_auto(GStrv) allow = NULL;
    g_auto(GStrv) deny = NULL;
    gsize i;

    mode = (ClawtConfineMode)
        clawt_agent_config_get_enum(agent_config, "computer.host.confine");

    root = clawt_agent_config_get_path_value(agent_config,
                                             "computer.host.root");
    if (root == NULL)
        root = clawt_agent_config_get_workspace(agent_config);

    sandbox = clawt_sandbox_new(mode, root);

    allow = clawt_agent_config_get_string_list(agent_config,
                                               "computer.host.allow_paths");
    for (i = 0; allow != NULL && allow[i] != NULL; i++)
        clawt_sandbox_add_allow_path(sandbox, allow[i]);

    deny = clawt_agent_config_get_string_list(agent_config,
                                              "computer.host.deny_paths");
    for (i = 0; deny != NULL && deny[i] != NULL; i++)
        clawt_sandbox_add_deny_path(sandbox, deny[i]);

    /*
     * A host agent's mounts are its allowlist.  There is nothing to mount
     * on the host, but somebody writing mounts on a host computer plainly
     * means "these are the directories I want it to work with", and
     * ignoring them would be the wrong reading.
     */
    {
        g_autoptr(GPtrArray) mounts =
            clawt_agent_config_get_mounts(agent_config);
        guint index;

        for (index = 0; mounts != NULL && index < mounts->len; index++) {
            ClawtMount *mount = g_ptr_array_index(mounts, index);
            const gchar *source = clawt_mount_get_source(mount);

            if (source != NULL)
                clawt_sandbox_add_allow_path(sandbox, source);
        }
    }

    clawt_sandbox_set_allow_network(
        sandbox,
        clawt_agent_config_has_key(agent_config, "computer.host.allow_network")
        ? clawt_agent_config_get_boolean(agent_config,
                                         "computer.host.allow_network")
        : TRUE);

    clawt_sandbox_set_allow_sudo(
        sandbox,
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.host.allow_sudo"));

    return sandbox;
}

/*
 * The graphical session to build inside a VM, or %NULL for a headless one.
 *
 * Gated on computer.desktop.enabled rather than on a key of its own.  That
 * key is already the grant -- "this agent may see and drive a desktop" --
 * and a second switch meaning "and there is one to drive" is two ways to
 * say yes and one more way to end up with a desktop nobody asked for or an
 * agent staring at a machine that has none.
 */
static ClawtGuestDesktop *
build_guest_desktop(ClawtAgentConfig *agent_config)
{
    ClawtGuestDesktop *desktop;
    g_auto(GStrv) packages = NULL;
    g_autofree gchar *session_user = NULL;

    if (!clawt_agent_config_get_boolean(agent_config,
                                        "computer.desktop.enabled"))
        return NULL;

    session_user = clawt_guest_desktop_resolve_user(
        clawt_agent_config_get_string(agent_config,
                                      "computer.vm.desktop.user"),
        clawt_agent_config_get_string(agent_config, "computer.vm.ssh_user"));

    desktop = clawt_guest_desktop_new(session_user);

    packages = clawt_agent_config_get_string_list(
        agent_config, "computer.vm.desktop.packages");
    clawt_guest_desktop_set_packages(desktop,
                                     (const gchar * const *)packages);

    {
        g_autofree gchar *image =
            clawt_agent_config_get_path_value(agent_config,
                                              "computer.vm.image");
        const gchar *configured =
            clawt_agent_config_get_string(agent_config,
                                          "computer.vm.desktop.flavour");
        ClawtGuestFlavour flavour =
            clawt_guest_desktop_resolve_flavour(configured, image);

        /*
         * Said out loud rather than guessed at quietly.  Getting this
         * wrong installs Fedora package names into a Debian, which
         * cloud-init reports as a failed install somewhere in the
         * guest's log -- a long way from the config line that caused it,
         * and invisible from the host.
         */
        if (flavour == CLAWT_GUEST_FLAVOUR_AUTO) {
            g_warning("cannot tell which distribution '%s' is; installing "
                      "the desktop with Fedora package names. Set "
                      "computer.vm.desktop.flavour to fedora, enterprise "
                      "or debian to settle it",
                      image != NULL ? image : "(no image)");

            flavour = CLAWT_GUEST_FLAVOUR_FEDORA;
        }

        clawt_guest_desktop_set_flavour(desktop, flavour);
    }

    clawt_guest_desktop_set_autologin(
        desktop,
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.vm.desktop.autologin"));
    clawt_guest_desktop_set_install_mcp(
        desktop,
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.vm.desktop.mcp"));
    clawt_guest_desktop_set_mcp_repo(
        desktop,
        clawt_agent_config_get_string(agent_config,
                                      "computer.vm.desktop.mcp_repo"));

    return desktop;
}

ClawtComputer *
clawt_computer_factory_create(ClawtAgentConfig  *agent_config,
                              GPtrArray         *default_mounts,
                              ClawtPodBridge    *bridge,
                              GError           **error)
{
    ClawtComputerType type;
    const gchar *agent_id;
    ClawtComputer *computer = NULL;

    g_return_val_if_fail(agent_config != NULL, NULL);

    agent_id = clawt_agent_config_get_id(agent_config);
    type = (ClawtComputerType)
        clawt_agent_config_get_enum(agent_config, "computer.type");

    switch (type) {
    case CLAWT_COMPUTER_NONE:
        return clawt_null_computer_new(agent_id);

    case CLAWT_COMPUTER_HOST: {
        g_autoptr(ClawtSandbox) sandbox = build_sandbox(agent_config);

        /*
         * The confirmation is checked here as well as during config
         * validation.  Validation catches it at load; this catches an agent
         * constructed some other way, and the cost of checking twice is
         * nothing against the cost of missing it once.
         */
        if (!clawt_agent_config_get_boolean(
                agent_config, "computer.host.confirm_host_control")) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                        "agent '%s' asks for a host computer without "
                        "computer.host.confirm_host_control: true",
                        agent_id);
            return NULL;
        }

        /*
         * And the fleet's half of the same decision, for the same reason
         * and in the same place.
         *
         * Running unconfined on the real machine takes two deliberate
         * acts -- the agent's confirmation above and the fleet's
         * daemon.allow_unconfined_host -- and until now only the first
         * of the two was checked here.  Validation catches it at load,
         * which covers a config file; this catches an agent constructed
         * some other way, which is the case the first check exists for.
         * The stronger of the two grants is exactly the one that should
         * not rest on a single reader.
         *
         * Asked of the fleet config rather than through the agent's own
         * getters: `daemon.*` is not overridable per agent, so it has no
         * agent-relative spelling and an agent-relative lookup would
         * answer FALSE from the schema whatever the file says -- which
         * would refuse every properly-granted host as well.
         */
        if (clawt_agent_config_get_enum(agent_config,
                                        "computer.host.confine") ==
                CLAWT_CONFINE_NONE) {
            ClawtConfig *fleet = clawt_agent_config_get_config(agent_config);

            if (fleet == NULL ||
                !clawt_config_get_boolean(fleet,
                                          "daemon.allow_unconfined_host")) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                            "agent '%s' asks for computer.host.confine: none "
                            "without daemon.allow_unconfined_host: true -- "
                            "running unconfined on your real machine takes "
                            "two deliberate acts, not one. Set it, or pick a "
                            "confinement mode", agent_id);
                return NULL;
            }
        }

        if (!clawt_sandbox_is_available(sandbox, error))
            return NULL;

        computer = clawt_host_computer_new(agent_id, sandbox);
        {
            g_autoptr(GHashTable) env = clawt_agent_config_get_env(agent_config);

            clawt_host_computer_set_environment(CLAWT_HOST_COMPUTER(computer),
                                                env);
        }

        clawt_host_computer_set_nice(
            CLAWT_HOST_COMPUTER(computer),
            (gint)clawt_agent_config_get_int(agent_config,
                                             "computer.host.nice"));

        /*
         * The screen, so the computer can answer #ClawtObservable.
         *
         * Built from the same function every other caller uses rather
         * than assembled here: a second construction would be a second
         * set of grants, and the one that drifted would be the one
         * deciding whether an agent may click.
         */
        {
            g_autoptr(ClawtDesktop) desktop =
                clawt_computer_factory_create_desktop(agent_config);

            clawt_host_computer_set_desktop(CLAWT_HOST_COMPUTER(computer),
                                            desktop);
        }
        break;
    }

    case CLAWT_COMPUTER_CONTAINER: {
        const gchar *image;

        if (bridge == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                "container computers need podomation's "
                                "container module");
            return NULL;
        }

        image = clawt_agent_config_get_string(agent_config,
                                              "computer.container.image");
        computer = clawt_container_computer_new(agent_id, bridge, image);

        clawt_container_computer_set_name(
            CLAWT_CONTAINER_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.container.name"));
        clawt_container_computer_set_connection(
            CLAWT_CONTAINER_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.container.connection"));
        clawt_container_computer_set_command(
            CLAWT_CONTAINER_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.container.command"));
        clawt_container_computer_set_network(
            CLAWT_CONTAINER_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.container.network"));
        clawt_container_computer_set_keep(
            CLAWT_CONTAINER_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.container.keep"));
        break;
    }

    case CLAWT_COMPUTER_DISTROBOX: {
        const gchar *home;

        if (bridge == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                "distrobox computers need podomation's "
                                "distrobox module");
            return NULL;
        }

        computer = clawt_distrobox_computer_new(
            agent_id, bridge,
            clawt_agent_config_get_string(agent_config,
                                          "computer.distrobox.image"));

        clawt_distrobox_computer_set_name(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.distrobox.name"));

        /*
         * The home, which is the whole confinement question for a
         * distrobox: left to distrobox's own default the box gets the
         * operator's real home directory, ssh keys and all.
         *
         * So an unset value is not passed through as "distrobox
         * decides" -- it resolves to a directory of the agent's own
         * under the state dir, and sharing the operator's is
         * `share_home: true`, said out loud.  Same shape as the host
         * computer defaulting to `confine: workspace` rather than to
         * `none`.
         */
        home = clawt_agent_config_get_string(agent_config,
                                             "computer.distrobox.home");

        clawt_distrobox_computer_set_home(
            CLAWT_DISTROBOX_COMPUTER(computer), home);
        clawt_distrobox_computer_set_share_home(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.distrobox.share_home"));

        clawt_distrobox_computer_set_packages(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.distrobox.packages"));
        clawt_distrobox_computer_set_flags(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.distrobox.flags"));
        clawt_distrobox_computer_set_init(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.distrobox.init"));
        clawt_distrobox_computer_set_keep(
            CLAWT_DISTROBOX_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.distrobox.keep"));
        break;
    }

    case CLAWT_COMPUTER_SSH: {
        const gchar *host =
            clawt_agent_config_get_string(agent_config, "computer.ssh.host");

        /*
         * Validated here as well as at provision time. This catches a
         * typo when the agent is built, which is where the operator is
         * looking; provision catches an agent constructed some other
         * way. The cost of checking twice is nothing against handing a
         * destination beginning with "-" to ssh once.
         */
        if (!clawt_ssh_host_is_valid(host, error))
            return NULL;

        computer = clawt_ssh_computer_new(agent_id, host);

        /*
         * get_string(), not get_path_value(), although the schema calls
         * this a path. It is a path on *another* machine, and expanding
         * "~" or "$XDG_DATA_HOME" against this one would produce a
         * directory that exists here and means nothing over there.
         */
        clawt_ssh_computer_set_workspace(
            CLAWT_SSH_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.ssh.workspace"));
        clawt_ssh_computer_set_shell(
            CLAWT_SSH_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.ssh.shell"));
        clawt_ssh_computer_set_connect_timeout(
            CLAWT_SSH_COMPUTER(computer),
            (guint)clawt_agent_config_get_int(agent_config,
                                              "computer.ssh.connect_timeout"));
        clawt_ssh_computer_set_control_persist(
            CLAWT_SSH_COMPUTER(computer),
            (guint)clawt_agent_config_get_int(agent_config,
                                              "computer.ssh.control_persist"));

        {
            /*
             * Built with clawt_sandbox_new_remote(), not
             * clawt_sandbox_new(). Every path here names the other
             * machine, and the local constructor resolves paths with
             * realpath() against this one -- which for a wholly unknown
             * path leaves ".." in the string, so
             * "/srv/work/../../etc/shadow" would read as being inside
             * "/srv/work" while the remote kernel resolved it.
             *
             * allowlist rather than workspace, because the mount targets
             * are the grants and workspace mode ignores them.
             */
            g_autoptr(ClawtSandbox) sandbox = clawt_sandbox_new_remote(
                CLAWT_CONFINE_ALLOWLIST,
                clawt_agent_config_get_string(agent_config,
                                              "computer.ssh.workspace"));

            clawt_sandbox_set_allow_sudo(
                sandbox,
                clawt_agent_config_get_boolean(agent_config,
                                               "computer.ssh.allow_sudo"));

            clawt_ssh_computer_set_sandbox(CLAWT_SSH_COMPUTER(computer),
                                           sandbox);
        }
        break;
    }

    case CLAWT_COMPUTER_VM: {
        ClawtVmBackend backend = (ClawtVmBackend)
            clawt_agent_config_get_enum(agent_config, "computer.vm.backend");

        computer = clawt_vm_computer_new(agent_id, backend, bridge);

        clawt_vm_computer_set_domain(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config, "computer.vm.domain"));
        clawt_vm_computer_set_uri(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config, "computer.vm.uri"));

        {
            /*
             * Only when the config names one.  Unset leaves the path the
             * constructor resolved, which is the answer for every host
             * that keeps QEMU where its distribution puts it.
             */
            g_autofree gchar *emulator =
                clawt_agent_config_get_path_value(agent_config,
                                                  "computer.vm.emulator");

            if (emulator != NULL)
                clawt_vm_computer_set_emulator(CLAWT_VM_COMPUTER(computer),
                                               emulator);
        }

        {
            g_autofree gchar *image =
                clawt_agent_config_get_path_value(agent_config,
                                                  "computer.vm.image");

            clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), image);
        }

        clawt_vm_computer_set_resolution(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_string(agent_config,
                                          "computer.vm.resolution"));

        clawt_vm_computer_set_resources(
            CLAWT_VM_COMPUTER(computer),
            (guint)clawt_agent_config_get_int(agent_config, "computer.vm.cpus"),
            (guint)clawt_agent_config_get_int(agent_config,
                                              "computer.vm.memory_mb"),
            (guint)clawt_agent_config_get_int(agent_config,
                                              "computer.vm.disk_gb"));

        {
            g_autofree gchar *key =
                clawt_agent_config_get_path_value(agent_config,
                                                  "computer.vm.ssh_key");

            clawt_vm_computer_set_ssh(
                CLAWT_VM_COMPUTER(computer),
                clawt_agent_config_get_string(agent_config,
                                              "computer.vm.ssh_user"),
                key,
                clawt_agent_config_get_string(agent_config,
                                              "computer.vm.ssh_host"),
                (guint)clawt_agent_config_get_int(agent_config,
                                                  "computer.vm.ssh_port"));
        }

        {
            g_auto(GStrv) packages = clawt_agent_config_get_string_list(
                agent_config, "computer.vm.packages");

            clawt_vm_computer_set_packages(
                CLAWT_VM_COMPUTER(computer),
                (const gchar *const *)packages);
        }

        clawt_vm_computer_set_cloud_init(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.vm.cloud_init"));

        clawt_vm_computer_set_snapshot_on_start(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.vm.snapshot_on_start"));

        {
            g_autoptr(ClawtGuestDesktop) desktop =
                build_guest_desktop(agent_config);

            clawt_vm_computer_set_desktop(CLAWT_VM_COMPUTER(computer),
                                          desktop);
        }
        break;
    }

    default:
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "unknown computer type");
        return NULL;
    }

    if (computer != NULL)
        apply_mounts(computer, agent_config, default_mounts);

    return computer;
}

ClawtDesktop *
clawt_computer_factory_create_desktop(ClawtAgentConfig *agent_config)
{
    ClawtDesktop *desktop;
    ClawtDesktopBackend backend;
    ClawtComputerType type;
    g_autofree gchar *socket_path = NULL;
    gboolean has_guest;

    g_return_val_if_fail(agent_config != NULL, NULL);

    if (!clawt_agent_config_get_boolean(agent_config,
                                        "computer.desktop.enabled"))
        return NULL;

    backend = (ClawtDesktopBackend)
        clawt_agent_config_get_enum(agent_config, "computer.desktop.backend");
    socket_path = clawt_agent_config_get_path_value(agent_config,
                                                    "computer.desktop.socket");

    type = (ClawtComputerType)
        clawt_agent_config_get_enum(agent_config, "computer.type");
    has_guest = (type == CLAWT_COMPUTER_VM);

    /*
     * Asked for a guest desktop without a guest.  Refusing outright would
     * take the agent down over a setting it can simply not have, so it
     * falls back to probing the host -- loudly, because an agent quietly
     * driving the user's screen when it was told to drive its own VM is
     * the wrong way round to be wrong.
     */
    if (backend == CLAWT_DESKTOP_BACKEND_GUEST && !has_guest) {
        g_warning("agent %s: computer.desktop.backend is guest but "
                  "computer.type is %s, so there is no guest to drive. "
                  "Falling back to the host's desktop -- set "
                  "computer.type to vm, or name a different backend.",
                  clawt_agent_config_get_id(agent_config),
                  clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE, type));
        backend = CLAWT_DESKTOP_BACKEND_AUTO;
    }

    desktop = clawt_desktop_new(backend, socket_path);
    clawt_desktop_set_guest_available(desktop, has_guest);
    clawt_desktop_set_allow_spawn(
        desktop,
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.desktop.allow_spawn"));

    clawt_desktop_set_allow_input(
        desktop,
        clawt_agent_config_get_boolean(agent_config,
                                       "computer.desktop.allow_input"));

    return desktop;
}
