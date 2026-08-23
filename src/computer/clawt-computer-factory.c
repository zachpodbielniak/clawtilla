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
#include "computer/clawt-vm-computer.h"

static void
apply_mounts(ClawtComputer *computer, ClawtAgentConfig *agent_config)
{
    g_autoptr(GPtrArray) mounts = clawt_agent_config_get_mounts(agent_config);
    ClawtComputerType type = clawt_computer_get_computer_type(computer);
    guint i;

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
     */
    if (type != CLAWT_COMPUTER_NONE &&
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
            g_autofree gchar *image =
                clawt_agent_config_get_path_value(agent_config,
                                                  "computer.vm.image");

            clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), image);
        }

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
        apply_mounts(computer, agent_config);

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
