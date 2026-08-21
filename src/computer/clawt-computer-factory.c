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
                                              "computer.vm.memory_mb"));

        {
            g_autofree gchar *key =
                clawt_agent_config_get_path_value(agent_config,
                                                  "computer.vm.ssh_key");

            clawt_vm_computer_set_ssh(
                CLAWT_VM_COMPUTER(computer),
                clawt_agent_config_get_string(agent_config,
                                              "computer.vm.ssh_user"),
                key, NULL);
        }

        clawt_vm_computer_set_snapshot_on_start(
            CLAWT_VM_COMPUTER(computer),
            clawt_agent_config_get_boolean(agent_config,
                                           "computer.vm.snapshot_on_start"));
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
    g_autofree gchar *socket_path = NULL;

    g_return_val_if_fail(agent_config != NULL, NULL);

    if (!clawt_agent_config_get_boolean(agent_config,
                                        "computer.desktop.enabled"))
        return NULL;

    backend = (ClawtDesktopBackend)
        clawt_agent_config_get_enum(agent_config, "computer.desktop.backend");
    socket_path = clawt_agent_config_get_path_value(agent_config,
                                                    "computer.desktop.socket");

    desktop = clawt_desktop_new(backend, socket_path);
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
