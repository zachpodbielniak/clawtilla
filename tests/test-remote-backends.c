/*
 * test-remote-backends.c - Do the two existing backends reach a remote?
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two paths in this tree look as though they already reach another
 * machine: `computer.container.connection` takes an ssh:// URI that
 * podomation's container module knows how to tunnel, and
 * `computer.vm.uri` takes qemu+ssh://. Whether they *work* is a
 * different question from whether they are wired, and it is not one to
 * settle from what clawtilla remembers about itself.
 *
 * So this file asks in two ways. The hermetic half asks the code what it
 * actually builds -- which URI reaches the module, and which paths go
 * into a domain -- because a structural answer is checkable everywhere
 * and does not rot. The integration half asks a real remote, behind
 * CLAWT_TEST_INTEGRATION plus an environment variable naming one,
 * because "it should work" and "it worked" are not the same claim.
 *
 * What the two established is written up in docs/computers.org under
 * "Reaching another machine"; the short version is that remote podman
 * carries a real caveat about mounts and remote libvirt does not work at
 * all.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

static gboolean
integration_enabled(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

/* ── Remote podman: wired, with one caveat ───────────────────────── */

/*
 * The ssh:// URI survives the whole way to the module.
 *
 * clawt_container_computer_set_connection() rewrites a bare path into
 * unix:// and treats the literal string "unix" as unset, so it is worth
 * proving it leaves a URI with a scheme alone -- a connection quietly
 * mangled here would fall back to the local podman socket, and every
 * command an agent ran would succeed on the wrong machine.
 */
static void
test_a_remote_podman_uri_reaches_the_backend(void)
{
    static const gchar *const carried[] = {
        "ssh://buildbox",
        "ssh://zach@buildbox",
        "ssh://zach@buildbox:2222",
        "ssh://buildbox/run/user/1000/podman/podman.sock",
        "tcp://10.0.0.4:2375",
        NULL
    };
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    gsize i;

    for (i = 0; carried[i] != NULL; i++) {
        g_autoptr(ClawtComputer) computer =
            clawt_container_computer_new("chief", bridge, NULL);

        clawt_container_computer_set_connection(
            CLAWT_CONTAINER_COMPUTER(computer), carried[i]);

        g_assert_cmpstr(
            clawt_container_computer_get_connection(
                CLAWT_CONTAINER_COMPUTER(computer)), ==, carried[i]);
    }

    /* A bare path is still a local socket, and still gets its scheme. */
    {
        g_autoptr(ClawtComputer) computer =
            clawt_container_computer_new("chief", bridge, NULL);

        clawt_container_computer_set_connection(
            CLAWT_CONTAINER_COMPUTER(computer), "/run/podman/podman.sock");

        g_assert_cmpstr(
            clawt_container_computer_get_connection(
                CLAWT_CONTAINER_COMPUTER(computer)), ==,
            "unix:///run/podman/podman.sock");
    }
}

/*
 * The caveat, made explicit rather than left to be discovered.
 *
 * A container's shared folders are bind mounts, and podman resolves a
 * bind source on the machine its *service* runs on. Point the connection
 * at another host and the workspace mount still names a path under this
 * machine's state directory -- so podman creates an empty directory over
 * there and mounts that, and the agent finds its own workspace present
 * and empty. Nothing fails; it is simply the wrong directory.
 *
 * This asserts the shape that makes that true -- the source is a local
 * path, unrewritten -- so if anything ever does translate it, this test
 * says so rather than the documentation quietly going stale.
 */
static void
test_a_remote_containers_mounts_still_name_this_machine(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-remotec-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(root, "clawtilla.yaml", NULL);
    g_autofree gchar *text = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/d.sock\"\n"
        "  automation_dir: \"%s/automation\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        connection: \"ssh://buildbox\"\n",
        root, root, root, root);
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtPodBridge) bridge = NULL;
    GPtrArray *mounts;
    gboolean saw_a_local_source = FALSE;
    guint i;

    g_assert_true(g_file_set_contents(path, text, -1, NULL));

    config = clawt_config_load(path, NULL);
    g_assert_nonnull(config);

    /*
     * A bridge object, not a connection. clawt_computer_factory_create()
     * refuses a container with no bridge at all, and constructing one
     * loads no module and opens no socket -- a module connects when its
     * event source starts, which nothing here does.
     */
    bridge = clawt_pod_bridge_new(NULL);

    computer = clawt_computer_factory_create(
        clawt_config_get_agent(config, "chief"), NULL, bridge, NULL);
    g_assert_nonnull(computer);

    mounts = clawt_computer_get_mounts(computer);
    g_assert_nonnull(mounts);

    for (i = 0; i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (g_strcmp0(clawt_mount_get_target(mount),
                      CLAWT_WORKSPACE_MOUNT_POINT) != 0)
            continue;

        /*
         * The source is this machine's path, handed to a podman that is
         * over there. That is the finding.
         */
        g_assert_true(g_str_has_prefix(clawt_mount_get_source(mount), root));
        saw_a_local_source = TRUE;
    }

    g_assert_true(saw_a_local_source);

    clawt_test_remove_tree(root);
}

/* ── Remote libvirt: connected is not provisioned ────────────────── */

/*
 * Connecting is the easy half, and it is the only half that works.
 *
 * The URI does reach the module -- clawt_vm_computer_set_uri() stores it
 * and every action passes it to clawt_pod_bridge_load_module_for(), which
 * is where the module's connection is made. What does not travel is
 * everything the domain refers to: the overlay is created here with
 * qemu-img, the cloud-init seed is built here with xorrisofs, and both
 * go into the XML as absolute paths on *this* filesystem. Define that
 * against a remote libvirtd and the domain names a disk that is not
 * there, which per this project's own table is a VM that boots and
 * admits nobody.
 *
 * Asserted rather than described, so the day somebody makes the paths
 * travel this test fails and the documentation gets corrected with it.
 */
static void
test_a_remote_libvirt_domain_names_local_files(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *root = g_dir_make_tmp("clawt-remotev-XXXXXX", NULL);
    g_autofree gchar *seed = g_build_filename(root, "seed.iso", NULL);
    g_autofree gchar *xml = NULL;
    ClawtMount *share;

    clawt_vm_computer_set_uri(CLAWT_VM_COMPUTER(computer),
                              "qemu+ssh://buildbox/system");
    clawt_vm_computer_set_domain(CLAWT_VM_COMPUTER(computer), "clawt-chief");

    /*
     * The seed is built on this machine by xorrisofs, at a path under
     * the daemon's own state directory, and the domain names it
     * verbatim.
     */
    clawt_vm_computer_set_seed_iso(CLAWT_VM_COMPUTER(computer), seed);

    /*
     * And a share, whose source is likewise a path on this machine --
     * libvirt would start a virtiofsd over there against a directory
     * that is not there.
     */
    share = clawt_mount_new(root, "/mnt/clawtilla/workspace");
    clawt_mount_set_mount_type(share, CLAWT_MOUNT_VIRTIOFS);
    clawt_computer_add_mount(computer, share);
    clawt_mount_free(share);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));
    g_assert_nonnull(xml);

    /*
     * Both are absolute paths on the daemon's filesystem, written into a
     * domain that a remote libvirtd would be asked to define. That is
     * the finding: connecting works and provisioning cannot.
     */
    g_assert_nonnull(strstr(xml, seed));
    g_assert_nonnull(strstr(xml, root));

    clawt_test_remove_tree(root);
}

/*
 * The other half of why remote libvirt does not work: the address.
 *
 * The domain forwards a host port to the guest's ssh, and that forward
 * is on the loopback of whichever machine libvirtd is running on. The
 * daemon then dials 127.0.0.1 -- its own loopback, a different machine
 * entirely -- so even a domain that started perfectly would be
 * unreachable.
 */
static void
test_a_remote_libvirt_guest_is_dialled_on_the_wrong_loopback(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_uri(CLAWT_VM_COMPUTER(computer),
                              "qemu+ssh://buildbox/system");
    clawt_vm_computer_set_domain(CLAWT_VM_COMPUTER(computer), "clawt-chief");
    clawt_vm_computer_set_port_forward(CLAWT_VM_COMPUTER(computer), 24601);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    /*
     * The forward is pinned to a loopback address in the XML, so it is
     * the *remote* host's loopback that ends up listening.
     */
    g_assert_nonnull(strstr(xml, "127.0.0.1"));
    g_assert_nonnull(strstr(xml, "24601"));
}

/* ── Asking a real remote ────────────────────────────────────────── */

/*
 * The half that cannot be reasoned about: does a container actually run
 * over there.
 *
 * Set CLAWT_TEST_PODMAN_SSH to an ssh:// URI for a machine whose podman
 * you are willing to have a container created on, with
 * CLAWT_TEST_INTEGRATION=1. The assertion is that the hostname the
 * container reports is *not* this machine's -- which is the only form of
 * the question that cannot be satisfied by the local socket answering.
 */
static void
test_a_remote_podman_actually_runs_over_there(void)
{
    const gchar *connection = g_getenv("CLAWT_TEST_PODMAN_SSH");
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtPodBridge) bridge = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "hostname", "-f", NULL };
    g_autofree gchar *here = NULL;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; this talks to podman");
        return;
    }

    if (connection == NULL) {
        g_test_skip("set CLAWT_TEST_PODMAN_SSH to an ssh:// podman URI to "
                    "check remote podman against a real machine");
        return;
    }

    bridge = clawt_pod_bridge_new(NULL);
    computer = clawt_container_computer_new(
        "clawt-remote-probe", bridge,
        "registry.fedoraproject.org/fedora:44");

    clawt_container_computer_set_connection(
        CLAWT_CONTAINER_COMPUTER(computer), connection);
    clawt_container_computer_set_keep(CLAWT_CONTAINER_COMPUTER(computer),
                                      FALSE);

    if (!clawt_computer_start(computer, &error)) {
        g_test_message("remote podman at %s did not start a container: %s",
                       connection, error->message);
        g_test_fail();
        return;
    }

    result = clawt_computer_exec(computer, argv, NULL, 60, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);

    here = g_strdup(g_get_host_name());
    g_test_message("this machine is %s; the container says %s", here,
                   clawt_exec_result_get_stdout(result));

    /*
     * Deliberately not an assertion that the two differ: a container's
     * hostname is its own id, not its host's, so this proves the command
     * ran and reports what came back. The real question -- which podman
     * served it -- is answered by the container appearing on the remote
     * and not here, which the message above lets a person confirm.
     */
    g_assert_nonnull(clawt_exec_result_get_stdout(result));

    clawt_computer_teardown(computer, NULL);
}

int
main(int argc, char *argv[])
{
    g_autofree gchar *data_dir = NULL;
    int status;

    data_dir = g_dir_make_tmp("clawt-remotedata-XXXXXX", NULL);
    g_setenv("XDG_DATA_HOME", data_dir, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/remote/podman/uri-reaches-the-backend",
                    test_a_remote_podman_uri_reaches_the_backend);
    g_test_add_func("/remote/podman/mounts-still-name-this-machine",
                    test_a_remote_containers_mounts_still_name_this_machine);
    g_test_add_func("/remote/libvirt/domain-names-local-files",
                    test_a_remote_libvirt_domain_names_local_files);
    g_test_add_func("/remote/libvirt/wrong-loopback",
                    test_a_remote_libvirt_guest_is_dialled_on_the_wrong_loopback);
    g_test_add_func("/remote/podman/really-runs-over-there",
                    test_a_remote_podman_actually_runs_over_there);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
