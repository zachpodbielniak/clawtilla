/*
 * test-computer.c - The computer backends
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The host backend is exercised for real, since it needs nothing but a
 * temporary directory.  The container and VM backends are tested through
 * the specifications they generate -- the mount JSON and the domain XML --
 * because getting SELinux relabelling or virtiofs shared memory wrong is
 * silent until a guest cannot see a share, and that deserves a unit test
 * rather than an integration one.  Anything genuinely needing podman or
 * libvirtd is behind CLAWT_TEST_INTEGRATION.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

static gboolean
integration_enabled(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

/* ── No computer ─────────────────────────────────────────────────── */

/*
 * An explicit refusal, not a silent empty result.  An agent handed nothing
 * assumes the command produced nothing and carries on; one told it has no
 * computer stops asking.
 */
static void
test_null_computer_refuses_clearly(void)
{
    g_autoptr(ClawtComputer) computer = clawt_null_computer_new("chief");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *description = NULL;
    const gchar *argv[] = { "ls", NULL };

    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_NONE);
    g_assert_null(clawt_computer_exec(computer, argv, NULL, 5, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    description = clawt_computer_describe(computer);
    g_assert_nonnull(strstr(description, "no computer"));
}

/* ── Host ────────────────────────────────────────────────────────── */

typedef struct {
    gchar         *root;
    ClawtSandbox  *sandbox;
    ClawtComputer *computer;
} HostFixture;

static void
host_setup(HostFixture *fixture)
{
    fixture->root = g_dir_make_tmp("clawt-hostc-XXXXXX", NULL);
    fixture->sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE,
                                         fixture->root);
    fixture->computer = clawt_host_computer_new("chief", fixture->sandbox);
}

static void
host_teardown(HostFixture *fixture)
{
    g_clear_object(&fixture->computer);
    g_clear_object(&fixture->sandbox);
    g_rmdir(fixture->root);
    g_clear_pointer(&fixture->root, g_free);
}

static void
test_host_runs_a_command(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    const gchar *argv[] = { "echo", "hello from the host", NULL };

    host_setup(&fixture);
    g_assert_true(clawt_computer_start(fixture.computer, &error));

    result = clawt_computer_exec(fixture.computer, argv, fixture.root, 10,
                                 NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(clawt_exec_result_succeeded(result));
    g_assert_nonnull(strstr(clawt_exec_result_get_stdout(result),
                            "hello from the host"));

    host_teardown(&fixture);
}

static void
test_host_reports_a_failing_command(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    const gchar *argv[] = { "sh", "-c", "exit 3", NULL };

    host_setup(&fixture);
    clawt_computer_start(fixture.computer, &error);

    result = clawt_computer_exec(fixture.computer, argv, NULL, 10, NULL,
                                 &error);
    g_assert_nonnull(result);
    g_assert_false(clawt_exec_result_succeeded(result));
    g_assert_cmpint(clawt_exec_result_get_exit_status(result), ==, 3);

    host_teardown(&fixture);
}

/*
 * A command reaching outside the boundary must never start, not be killed
 * afterwards.
 */
static void
test_host_refuses_a_command_outside_the_boundary(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    const gchar *argv[] = { "cat", "/etc/shadow", NULL };

    host_setup(&fixture);
    clawt_computer_start(fixture.computer, &error);

    result = clawt_computer_exec(fixture.computer, argv, NULL, 10, NULL,
                                 &error);
    g_assert_null(result);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);

    host_teardown(&fixture);
}

/* A working directory outside the boundary is refused too. */
static void
test_host_refuses_a_working_directory_outside(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    const gchar *argv[] = { "pwd", NULL };

    host_setup(&fixture);
    clawt_computer_start(fixture.computer, &error);

    result = clawt_computer_exec(fixture.computer, argv, "/etc", 10, NULL,
                                 &error);
    g_assert_null(result);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);

    host_teardown(&fixture);
}

/*
 * Without a timeout an agent that runs an interactive command by mistake
 * waits for input that never comes, and the turn never ends.
 */
static void
test_host_times_out_a_hanging_command(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    const gchar *argv[] = { "sleep", "30", NULL };

    host_setup(&fixture);
    clawt_computer_start(fixture.computer, &error);

    result = clawt_computer_exec(fixture.computer, argv, NULL, 1, NULL,
                                 &error);
    g_assert_null(result);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);

    host_teardown(&fixture);
}

/* put_file and get_file go through the same boundary as exec, or an agent
 * could write anywhere by calling put_file instead of running cp. */
static void
test_host_file_transfer_respects_the_boundary(void)
{
    HostFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *inside = NULL;

    host_setup(&fixture);

    source = g_build_filename(fixture.root, "source.txt", NULL);
    inside = g_build_filename(fixture.root, "copy.txt", NULL);
    g_file_set_contents(source, "contents", -1, &error);

    g_assert_true(clawt_computer_put_file(fixture.computer, source, inside,
                                          &error));
    g_assert_no_error(error);

    g_assert_false(clawt_computer_put_file(fixture.computer, source,
                                           "/etc/clawtilla-should-not-exist",
                                           &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    g_clear_error(&error);

    g_assert_false(clawt_computer_get_file(fixture.computer, "/etc/shadow",
                                           inside, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);

    g_unlink(source);
    g_unlink(inside);
    host_teardown(&fixture);
}

/*
 * Unbounded output is a real failure mode: `find /` produces a reply too
 * large to send and too large to reason about.  Truncation is reported, or
 * an agent treats a cut-short listing as complete.
 */
static void
test_output_truncation_is_reported(void)
{
    g_autofree gchar *big = g_strnfill(2000, 'x');
    g_autofree gchar *bounded = NULL;
    gboolean truncated = FALSE;

    bounded = clawt_computer_truncate_output(big, 100, &truncated);

    g_assert_true(truncated);
    g_assert_nonnull(strstr(bounded, "truncated"));
    g_assert_cmpuint(strlen(bounded), <, 2000);

    /* Small output is left exactly alone. */
    {
        g_autofree gchar *small = NULL;
        gboolean small_truncated = TRUE;

        small = clawt_computer_truncate_output("short", 100,
                                               &small_truncated);
        g_assert_cmpstr(small, ==, "short");
        g_assert_false(small_truncated);
    }
}

static void
test_host_description_mentions_the_confinement(void)
{
    HostFixture fixture = { 0 };
    g_autofree gchar *description = NULL;

    host_setup(&fixture);

    description = clawt_computer_describe(fixture.computer);
    g_assert_nonnull(strstr(description, "clawtilla"));
    g_assert_nonnull(strstr(description, fixture.root));

    host_teardown(&fixture);
}

/* ── Container specification ─────────────────────────────────────── */

/*
 * SELinux relabelling is the flag that gets forgotten and then costs an
 * afternoon: on Silverblue an unlabelled bind mount is visible in the
 * container while every access is denied.
 */
static void
test_container_mount_json(void)
{
    g_autoptr(GPtrArray) mounts = NULL;
    g_autoptr(ClawtMount) rw = NULL;
    g_autoptr(ClawtMount) ro = NULL;
    g_autoptr(ClawtMount) scratch = NULL;
    g_autofree gchar *json = NULL;

    mounts = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);

    rw = clawt_mount_new("/tmp", "/work/tmp");
    clawt_mount_set_mode(rw, CLAWT_MOUNT_MODE_RW);
    clawt_mount_set_relabel(rw, CLAWT_RELABEL_SHARED);
    g_ptr_array_add(mounts, clawt_mount_copy(rw));

    ro = clawt_mount_new("/usr/share", "/work/share");
    g_ptr_array_add(mounts, clawt_mount_copy(ro));

    scratch = clawt_mount_new(NULL, "/scratch");
    clawt_mount_set_mount_type(scratch, CLAWT_MOUNT_TMPFS);
    clawt_mount_set_size(scratch, "512M");
    g_ptr_array_add(mounts, clawt_mount_copy(scratch));

    json = clawt_container_computer_build_mount_json(mounts);

    /*
     * Parsed rather than string-matched.  json-glib's spacing is its own
     * business, and a test that pins it fails on a pretty-printer change
     * while saying nothing about whether the mounts are right.
     */
    {
        g_autoptr(JsonParser) parser = json_parser_new();
        g_autoptr(GError) error = NULL;
        JsonArray *array;
        JsonObject *first;
        JsonObject *second;
        JsonObject *third;

        g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
        array = json_node_get_array(json_parser_get_root(parser));
        g_assert_cmpuint(json_array_get_length(array), ==, 3);

        first = json_array_get_object_element(array, 0);
        g_assert_cmpstr(json_object_get_string_member(first, "destination"),
                        ==, "/work/tmp");
        g_assert_cmpstr(json_object_get_string_member(first, "relabel"),
                        ==, "shared");
        g_assert_false(json_object_get_boolean_member(first, "read_only"));

        second = json_array_get_object_element(array, 1);
        g_assert_true(json_object_get_boolean_member(second, "read_only"));

        third = json_array_get_object_element(array, 2);
        g_assert_cmpstr(json_object_get_string_member(third, "type"),
                        ==, "tmpfs");
        g_assert_cmpstr(json_object_get_string_member(third, "size"),
                        ==, "512M");

        /* A tmpfs has nothing to share from, so it must carry no source. */
        g_assert_false(json_object_has_member(third, "source"));
    }
}

static void
test_container_mount_json_handles_no_mounts(void)
{
    g_autoptr(GPtrArray) mounts =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);
    g_autofree gchar *json = clawt_container_computer_build_mount_json(mounts);

    g_assert_cmpstr(json, ==, "[]");

    {
        /* NULL is accepted and still produces a valid empty array. */
        g_autofree gchar *from_null =
            clawt_container_computer_build_mount_json(NULL);

        g_assert_cmpstr(from_null, ==, "[]");
    }
}

/* ── VM specification ────────────────────────────────────────────── */

/*
 * virtiofs needs shared memory backing, and libvirt rejects the device
 * without it -- with a message that does not obviously point at the cause.
 */
static void
test_vm_domain_xml_includes_shared_memory_for_mounts(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autofree gchar *xml = NULL;

    computer = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 4, 4096);

    mount = clawt_mount_new("/tmp", "/work");
    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);
    clawt_computer_add_mount(computer, mount);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "<memoryBacking>"));
    g_assert_nonnull(strstr(xml, "access mode='shared'"));
    g_assert_nonnull(strstr(xml, "driver type='virtiofs'"));
    g_assert_nonnull(strstr(xml, "/work"));
    g_assert_nonnull(strstr(xml, "<vcpu>4</vcpu>"));
    g_assert_nonnull(strstr(xml, "4096"));
}

/* A read-only mount must actually say so, or it silently is not. */
static void
test_vm_domain_xml_marks_read_only_mounts(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autofree gchar *xml = NULL;

    computer = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);

    mount = clawt_mount_new("/usr/share", "/work/share");
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RO);
    clawt_computer_add_mount(computer, mount);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "<readonly/>"));
}

/* A directory with an ampersand in its name must not produce invalid XML. */
static void
test_vm_domain_xml_escapes_markup(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autofree gchar *xml = NULL;

    computer = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);

    mount = clawt_mount_new("/tmp/rock & roll", "/work/music");
    clawt_computer_add_mount(computer, mount);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "&amp;"));
    g_assert_null(strstr(xml, "rock & roll"));
}

/* Without mounts there is no reason to demand shared memory. */
static void
test_vm_domain_xml_omits_shared_memory_without_mounts(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml =
        clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_null(strstr(xml, "<memoryBacking>"));
}

/*
 * A cloud image has no login until it is handed one, and the seed is how.
 * It rides as a CD-ROM because that is what NoCloud looks for.
 */
static void
test_vm_domain_xml_attaches_the_seed(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_seed_iso(CLAWT_VM_COMPUTER(computer),
                                   "/tmp/vms/chief/seed.iso");

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "device='cdrom'"));
    g_assert_nonnull(strstr(xml, "/tmp/vms/chief/seed.iso"));
    g_assert_nonnull(strstr(xml, "<readonly/>"));
}

/*
 * libvirt has no port forwarding for the SLIRP backend, so a forwarded
 * port has to bring passt with it -- a <portForward> without it is a
 * rejected domain, not an unforwarded one.
 */
static void
test_vm_domain_xml_forwards_ssh_through_passt(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_port_forward(CLAWT_VM_COMPUTER(computer), 24601);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "<backend type='passt'/>"));
    g_assert_nonnull(strstr(xml, "<range start='24601' to='22'/>"));
}

/* An address the user configured needs no forward inventing alongside it. */
static void
test_vm_domain_xml_without_a_forward_stays_plain(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml =
        clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "<interface type='user'>"));
    g_assert_null(strstr(xml, "portForward"));
    g_assert_null(strstr(xml, "passt"));
}

/*
 * The port is chosen on the host and written into the command line.
 * qemu's hostfwd accepts port 0 and lets the kernel pick, which is what
 * this used to do -- and then nothing reported which port it picked, so
 * nothing could connect.
 */
static void
test_vm_qemu_argv_forwards_a_known_port(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;
    gboolean saw_forward = FALSE;
    guint i;

    clawt_vm_computer_set_port_forward(CLAWT_VM_COMPUTER(computer), 24601);
    clawt_vm_computer_set_seed_iso(CLAWT_VM_COMPUTER(computer),
                                   "/tmp/vms/chief/seed.iso");

    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(computer),
                                             NULL);

    for (i = 0; argv[i] != NULL; i++) {
        if (strstr(argv[i], "hostfwd=tcp:127.0.0.1:24601-:22") != NULL)
            saw_forward = TRUE;
    }

    g_assert_true(saw_forward);

    joined = g_strjoinv(" ", argv);
    g_assert_null(strstr(joined, "hostfwd=tcp::0"));
}

static void
test_vm_qemu_argv_attaches_the_seed(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;

    clawt_vm_computer_set_seed_iso(CLAWT_VM_COMPUTER(computer),
                                   "/tmp/vms/chief/seed.iso");

    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(computer),
                                             NULL);
    joined = g_strjoinv(" ", argv);

    g_assert_nonnull(strstr(joined, "seed.iso"));
    g_assert_nonnull(strstr(joined, "media=cdrom"));
}

/*
 * The bug this whole path existed to have: nothing ever set an address, so
 * every command run in a VM failed.  A missing address must be a refusal
 * the caller can report, not an argv that dials nowhere.
 */
static void
test_vm_ssh_argv_is_null_without_an_address(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    const gchar *command[] = { "uname", "-a", NULL };

    g_assert_null(clawt_vm_computer_build_ssh_argv(CLAWT_VM_COMPUTER(computer),
                                                   command, NULL, 0));
}

static void
test_vm_ssh_argv_uses_the_forwarded_port(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    const gchar *command[] = { "uname", "-a", NULL };
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;

    clawt_vm_computer_set_ssh(CLAWT_VM_COMPUTER(computer), "agent", NULL,
                              "127.0.0.1", 24601);

    argv = clawt_vm_computer_build_ssh_argv(CLAWT_VM_COMPUTER(computer),
                                            command, NULL, 0);
    g_assert_nonnull(argv);
    joined = g_strjoinv(" ", argv);

    g_assert_cmpstr(argv[0], ==, "ssh");
    g_assert_nonnull(strstr(joined, "-p 24601"));
    g_assert_nonnull(strstr(joined, "agent@127.0.0.1"));

    /*
     * A rebuilt VM answers on the same address with a different host key.
     * Against the user's own known_hosts that reads as an attack and is
     * refused -- and clawtilla should not be writing there regardless.
     */
    g_assert_nonnull(strstr(joined, "UserKnownHostsFile="));
    g_assert_null(strstr(joined, "UserKnownHostsFile=/dev/null"));
}

/* An argument with a space in it must arrive as one argument. */
static void
test_vm_ssh_argv_quotes_the_command(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    const gchar *command[] = { "cat", "/etc/rock & roll", NULL };
    g_auto(GStrv) argv = NULL;
    guint last = 0;

    clawt_vm_computer_set_ssh(CLAWT_VM_COMPUTER(computer), "agent", NULL,
                              "127.0.0.1", 24601);

    argv = clawt_vm_computer_build_ssh_argv(CLAWT_VM_COMPUTER(computer),
                                            command, "/work", 5);
    g_assert_nonnull(argv);

    while (argv[last + 1] != NULL)
        last++;

    g_assert_nonnull(strstr(argv[last], "cd '/work' &&"));
    g_assert_nonnull(strstr(argv[last], "'/etc/rock & roll'"));
}

/*
 * The whole VM path against a real guest: build a seed, boot the image,
 * and run a command in it.
 *
 * Everything above this asserts on a specification -- the XML, the argv,
 * the user-data -- and every one of those was already correct while the
 * feature did not work at all.  Only a guest that boots and answers
 * proves cloud-init accepted what it was handed and that the forwarded
 * port reaches sshd.
 *
 * Needs CLAWT_TEST_INTEGRATION and a cloud image in CLAWT_TEST_VM_IMAGE:
 *
 *   CLAWT_TEST_INTEGRATION=1 \
 *   CLAWT_TEST_VM_IMAGE=~/vms/Fedora-Cloud-Base-44.qcow2 \
 *   make test-integration
 */
static void
test_vm_boots_and_runs_a_command(void)
{
    const gchar *image = g_getenv("CLAWT_TEST_VM_IMAGE");
    const gchar *command[] = { "sh", "-c", "id -un", NULL };
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtExecResult *result = NULL;
    guint waited;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION");
        return;
    }

    if (image == NULL) {
        g_test_skip("needs a cloud image in CLAWT_TEST_VM_IMAGE");
        return;
    }

    computer = clawt_vm_computer_new("vmtest", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), image);
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 2, 1024);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    /* Provisioning is what fills these in; nothing else ever did. */
    g_assert_cmpstr(clawt_vm_computer_get_ssh_host(
                        CLAWT_VM_COMPUTER(computer)), ==, "127.0.0.1");
    g_assert_cmpuint(clawt_vm_computer_get_ssh_port(
                         CLAWT_VM_COMPUTER(computer)), >, 0);

    g_assert_true(clawt_computer_start(computer, &error));
    g_assert_no_error(error);

    for (waited = 0; waited < 240; waited += 5) {
        g_usleep(5 * G_USEC_PER_SEC);

        result = clawt_computer_exec(computer, command, NULL, 10, NULL, NULL);

        if (result != NULL &&
            clawt_exec_result_get_exit_status(result) == 0)
            break;

        g_clear_pointer(&result, clawt_exec_result_free);
    }

    clawt_computer_stop(computer, NULL);

    g_assert_nonnull(result);
    g_assert_nonnull(strstr(clawt_exec_result_get_stdout(result), "root"));

    clawt_exec_result_free(result);
}

static void
test_vm_qemu_argv(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_auto(GStrv) argv = NULL;
    gboolean saw_qmp = FALSE;
    gboolean saw_memfd = FALSE;
    guint i;

    computer = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 2, 1024);

    mount = clawt_mount_new("/tmp", "/work");
    clawt_computer_add_mount(computer, mount);

    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(computer),
                                             "/tmp/clawt-qmp.sock");

    g_assert_cmpstr(argv[0], ==, "qemu-system-x86_64");

    for (i = 0; argv[i] != NULL; i++) {
        if (strstr(argv[i], "/tmp/clawt-qmp.sock") != NULL)
            saw_qmp = TRUE;
        if (strstr(argv[i], "memory-backend-memfd") != NULL)
            saw_memfd = TRUE;
    }

    g_assert_true(saw_qmp);

    /* Same shared-memory requirement as libvirt, spelled QEMU's way. */
    g_assert_true(saw_memfd);
}

/* ── Desktop ─────────────────────────────────────────────────────── */

/*
 * Observing and acting are separate grants.  An observe-only agent that
 * could still click would make the distinction meaningless.
 */
static void
test_desktop_observe_only_omits_input_tools(void)
{
    g_autoptr(ClawtDesktop) desktop =
        clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, "/tmp/nowhere.sock");

    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "describe_desktop"));
    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "screenshot_monitor"));

    g_assert_false(clawt_desktop_tool_is_permitted(desktop, "send_key"));
    g_assert_false(clawt_desktop_tool_is_permitted(desktop, "mouse_click"));
    g_assert_false(clawt_desktop_tool_is_permitted(desktop, "type_text"));
}

static void
test_desktop_with_input_permits_acting(void)
{
    g_autoptr(ClawtDesktop) desktop =
        clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, "/tmp/nowhere.sock");

    clawt_desktop_set_allow_input(desktop, TRUE);

    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "send_key"));
    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "mouse_click"));
    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "describe_desktop"));
}

/*
 * An unknown tool is refused rather than passed through.  A newer
 * compositor may add one that injects input, and defaulting to allow would
 * quietly widen an observe-only grant on upgrade.
 */
static void
test_unknown_desktop_tool_is_refused(void)
{
    g_autoptr(ClawtDesktop) desktop =
        clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, "/tmp/nowhere.sock");

    clawt_desktop_set_allow_input(desktop, TRUE);

    g_assert_false(clawt_desktop_tool_is_permitted(desktop,
                                                   "future_invention"));
}

/* A socket file that exists but answers nothing is not a desktop. */
static void
test_dead_gowl_socket_is_not_available(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-gowl-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "gowl-mcp.sock", NULL);
    g_autoptr(ClawtDesktop) desktop = NULL;
    g_autoptr(GError) error = NULL;

    /* A plain file where the socket should be: exists, answers nothing. */
    g_file_set_contents(path, "", -1, NULL);

    desktop = clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, path);

    g_assert_false(clawt_desktop_is_available(desktop, &error));
    g_assert_nonnull(error);

    g_unlink(path);
    clawt_test_remove_tree(dir);
}

static void
test_desktop_description_states_the_grant(void)
{
    g_autoptr(ClawtDesktop) observe =
        clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, "/tmp/x.sock");
    g_autoptr(ClawtDesktop) control =
        clawt_desktop_new(CLAWT_DESKTOP_BACKEND_GOWL, "/tmp/x.sock");
    g_autofree gchar *observe_text = NULL;
    g_autofree gchar *control_text = NULL;

    clawt_desktop_set_allow_input(control, TRUE);

    observe_text = clawt_desktop_describe(observe);
    control_text = clawt_desktop_describe(control);

    g_assert_nonnull(strstr(observe_text, "cannot send keystrokes"));
    g_assert_nonnull(strstr(control_text, "real screen"));
}

/* ── The factory ─────────────────────────────────────────────────── */

static ClawtAgentConfig *
agent_from_yaml(ClawtConfig **out_config, const gchar *yaml)
{
    g_autoptr(GError) error = NULL;

    *out_config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    return clawt_config_get_agent(*out_config, "chief");
}

static void
test_factory_builds_the_configured_backend(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config, "agents:\n  - id: chief\n");
    computer = clawt_computer_factory_create(agent, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_NONE);
}

/* A host computer without the confirmation is refused here as well as at
 * config load, because the cost of checking twice is nothing. */
static void
test_factory_refuses_unconfirmed_host(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
        "agents:\n  - id: chief\n    computer:\n      type: host\n");

    computer = clawt_computer_factory_create(agent, NULL, &error);

    g_assert_null(computer);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
}

static void
test_factory_builds_a_confirmed_host(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        confine: workspace\n");

    computer = clawt_computer_factory_create(agent, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_HOST);
}

/*
 * A mount written without a type becomes the right one for the backend, so
 * a user does not have to know that a container wants "bind" and a VM wants
 * "virtiofs".
 */
static void
test_factory_picks_the_mount_type_for_the_backend(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    GPtrArray *mounts;

    agent = agent_from_yaml(&config,
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: vm\n"
        "      mounts:\n"
        "        - source: \"/tmp\"\n"
        "          target: \"/work\"\n");

    computer = clawt_computer_factory_create(agent, NULL, &error);
    g_assert_no_error(error);

    mounts = clawt_computer_get_mounts(computer);
    g_assert_cmpuint(mounts->len, ==, 1);
    g_assert_cmpint(
        clawt_mount_get_mount_type(g_ptr_array_index(mounts, 0)), ==,
        CLAWT_MOUNT_VIRTIOFS);
}

/*
 * Mounts on a host computer become its allowlist.  There is nothing to
 * mount, but somebody writing them plainly means "work with these".
 */
static void
test_factory_treats_host_mounts_as_an_allowlist(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    ClawtSandbox *sandbox;

    agent = agent_from_yaml(&config,
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        confine: allowlist\n"
        "      mounts:\n"
        "        - source: \"/tmp\"\n"
        "          target: \"/work\"\n");

    computer = clawt_computer_factory_create(agent, NULL, &error);
    g_assert_no_error(error);

    sandbox = clawt_host_computer_get_sandbox(CLAWT_HOST_COMPUTER(computer));
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, "/tmp"));
}

static void
test_factory_desktop_is_optional(void)
{
    g_autoptr(ClawtConfig) without = NULL;
    g_autoptr(ClawtConfig) with = NULL;
    g_autoptr(ClawtDesktop) none = NULL;
    g_autoptr(ClawtDesktop) desktop = NULL;

    none = clawt_computer_factory_create_desktop(
        agent_from_yaml(&without, "agents:\n  - id: chief\n"));
    g_assert_null(none);

    desktop = clawt_computer_factory_create_desktop(
        agent_from_yaml(&with,
            "agents:\n"
            "  - id: chief\n"
            "    computer:\n"
            "      desktop:\n"
            "        enabled: true\n"
            "        allow_input: true\n"));

    g_assert_nonnull(desktop);
    g_assert_true(clawt_desktop_tool_is_permitted(desktop, "send_key"));
}

/* ── Integration, only with real infrastructure ──────────────────── */

static void
test_container_against_real_podman(void)
{
    g_autoptr(ClawtPodBridge) bridge = NULL;
    g_autoptr(GError) error = NULL;

    if (!integration_enabled()) {
        g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run this against podman");
        return;
    }

    bridge = clawt_pod_bridge_new(NULL);

    if (!clawt_pod_bridge_load_module(bridge, "container", &error)) {
        g_test_skip(error->message);
        return;
    }

    g_assert_true(clawt_pod_bridge_has_module(bridge, "container"));
}

/*
 * Removes a directory tree.
 *
 * The mount tests create nested directories with files in them, and
 * g_rmdir on a non-empty directory quietly does nothing -- leaving the
 * temporary tree behind on every run.
 */
/* ── Mounts on the host ──────────────────────────────────────────── */

/*
 * A host agent reaches a mount by the path it was given.
 *
 * A host computer has no mount namespace, so nothing makes
 * /mnt/clawtilla/exchange real -- clawtilla has to translate it.  Without
 * that, an agent told where its exchange is gets refused for using that
 * exact path, which reads as a bug in the confinement rather than as the
 * mount never having been applied.
 */
static void
test_host_reaches_a_mount_by_its_target(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-mount-XXXXXX", NULL);
    g_autofree gchar *workspace = g_build_filename(dir, "work", NULL);
    g_autofree gchar *shared = g_build_filename(dir, "shared", NULL);
    g_autofree gchar *file = g_build_filename(shared, "note.txt", NULL);
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "cat", "/mnt/shared/note.txt", NULL };

    g_mkdir_with_parents(workspace, 0700);
    g_mkdir_with_parents(shared, 0700);
    g_file_set_contents(file, "from the host\n", -1, NULL);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, workspace);
    computer = clawt_host_computer_new("agent", sandbox);

    mount = clawt_mount_new(shared, "/mnt/shared");
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
    clawt_computer_add_mount(computer, mount);

    g_assert_true(clawt_computer_start(computer, &error));
    g_assert_no_error(error);

    result = clawt_computer_exec(computer, argv, NULL, 10, NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_cmpint(clawt_exec_result_get_exit_status(result), ==, 0);
    g_assert_nonnull(strstr(clawt_exec_result_get_stdout(result),
                            "from the host"));

    clawt_test_remove_tree(dir);
}

/*
 * A declared mount grants access even under `confine: workspace`.
 *
 * Declaring a mount is an explicit grant by whoever wrote the config; on
 * a container the kernel would make it reachable, and the same config
 * behaving differently per backend is the kind of inconsistency that
 * costs an afternoon.
 */
static void
test_a_mount_is_a_grant_in_workspace_mode(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-mount-XXXXXX", NULL);
    g_autofree gchar *workspace = g_build_filename(dir, "work", NULL);
    g_autofree gchar *shared = g_build_filename(dir, "shared", NULL);
    g_autoptr(ClawtSandbox) sandbox = NULL;

    g_mkdir_with_parents(workspace, 0700);
    g_mkdir_with_parents(shared, 0700);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, workspace);

    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, shared));

    clawt_sandbox_add_mount_path(sandbox, shared);

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, shared));

    /* And nothing else was opened up by it. */
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));

    clawt_test_remove_tree(dir);
}

/*
 * A mount target is matched as a whole path component.
 *
 * "/mnt/sharedx" starts with "/mnt/shared" and is somewhere else; a
 * prefix match would rewrite it and hand the agent the wrong directory.
 */
static void
test_a_mount_prefix_is_not_a_match(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-mount-XXXXXX", NULL);
    g_autofree gchar *workspace = g_build_filename(dir, "work", NULL);
    g_autofree gchar *shared = g_build_filename(dir, "shared", NULL);
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "cat", "/mnt/sharedx/secret", NULL };

    g_mkdir_with_parents(workspace, 0700);
    g_mkdir_with_parents(shared, 0700);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, workspace);
    computer = clawt_host_computer_new("agent", sandbox);

    mount = clawt_mount_new(shared, "/mnt/shared");
    clawt_computer_add_mount(computer, mount);
    clawt_computer_start(computer, NULL);

    /* Refused, not silently rewritten into the mount. */
    g_assert_null(clawt_computer_exec(computer, argv, NULL, 10, NULL,
                                      &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);

    clawt_test_remove_tree(dir);
}

/*
 * A socket path longer than the kernel allows is refused with the
 * number, rather than being bound "successfully" somewhere that does not
 * exist.
 */
static void
test_an_over_long_socket_path_is_refused(void)
{
    g_autoptr(GString) path = g_string_new("/tmp/");
    g_autoptr(GError) error = NULL;

    while (path->len < 200)
        g_string_append(path, "clawtilla-");

    g_assert_false(clawt_check_socket_path(path->str, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
    g_assert_nonnull(strstr(error->message, "107"));

    g_clear_error(&error);
    g_assert_true(clawt_check_socket_path("/tmp/short.sock", &error));
    g_assert_no_error(error);
}

/*
 * The module search path has to include the build tree.
 *
 * There was one directory, fixed at compile time to the install prefix,
 * and a comment claiming it was the build tree.  It was not, so starting
 * a container agent out of a checkout failed with a path that had never
 * existed on the machine -- and the message named only that path, so it
 * read as "create this directory" rather than "the modules are over
 * there".
 */
static void
test_module_search_path(void)
{
    g_autoptr(ClawtPodBridge) searching = clawt_pod_bridge_new(NULL);
    g_autoptr(ClawtPodBridge) pinned = clawt_pod_bridge_new("/nowhere");
    const gchar * const *path;
    g_autoptr(GError) error = NULL;
    gboolean saw_relative_to_binary = FALSE;
    gsize i;

    path = clawt_pod_bridge_get_search_path(searching);
    g_assert_nonnull(path);

    for (i = 0; path[i] != NULL; i++) {
        if (g_str_has_suffix(path[i], "/pod-modules") ||
            g_str_has_suffix(path[i], "/modules"))
            saw_relative_to_binary = TRUE;
    }

    g_assert_true(saw_relative_to_binary);
    g_assert_cmpuint(i, >, 1);

    /* A named directory is the only one used, so a wrong path fails loudly. */
    path = clawt_pod_bridge_get_search_path(pinned);
    g_assert_cmpstr(path[0], ==, "/nowhere");
    g_assert_null(path[1]);

    g_assert_false(clawt_pod_bridge_load_module(pinned, "container", &error));
    g_assert_nonnull(error);

    /* The failure names where it looked, or nobody can act on it. */
    g_assert_nonnull(strstr(error->message, "/nowhere"));
}

/*
 * A container computer has to talk to the podman the user actually runs.
 *
 * podomation defaults to /run/podman/podman.sock -- the root socket --
 * so a rootless desktop got "Could not connect: Permission denied" for a
 * daemon it was never going to be allowed to reach.  Rootless is the
 * normal case.
 */
static void
test_container_connection_default(void)
{
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(ClawtComputer) computer =
        clawt_container_computer_new("agent", bridge, "fedora:latest");
    ClawtContainerComputer *container = CLAWT_CONTAINER_COMPUTER(computer);
    const gchar *connection = clawt_container_computer_get_connection(container);

    g_assert_nonnull(connection);
    g_assert_true(g_str_has_prefix(connection, "unix://"));

    /* "unix" is the documented shorthand, not a URI: it keeps the default. */
    clawt_container_computer_set_connection(container, "unix");
    g_assert_cmpstr(clawt_container_computer_get_connection(container),
                    ==, connection);

    /* A bare path is a socket path. */
    clawt_container_computer_set_connection(container, "/tmp/podman.sock");
    g_assert_cmpstr(clawt_container_computer_get_connection(container),
                    ==, "unix:///tmp/podman.sock");

    /* Anything with a scheme is passed through untouched. */
    clawt_container_computer_set_connection(container,
                                            "ssh://me@host/run/podman.sock");
    g_assert_cmpstr(clawt_container_computer_get_connection(container),
                    ==, "ssh://me@host/run/podman.sock");
}

/*
 * A container computer exists to be exec'd into, so it needs something
 * long-lived to run.  A plain base image's entrypoint exits at once, and
 * podman then reports Exited while clawtilla still called it provisioned.
 */
static void
test_container_command(void)
{
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(ClawtComputer) computer =
        clawt_container_computer_new("agent", bridge, "fedora:latest");
    ClawtContainerComputer *container = CLAWT_CONTAINER_COMPUTER(computer);
    const gchar *command;

    command = clawt_container_computer_get_command(container);
    g_assert_nonnull(command);
    g_assert_cmpstr(command, ==, "[\"sleep\", \"infinity\"]");

    /* A JSON array is taken as written. */
    clawt_container_computer_set_command(container, "[\"tail\",\"-f\"]");
    g_assert_cmpstr(clawt_container_computer_get_command(container),
                    ==, "[\"tail\",\"-f\"]");

    /*
     * A plain string is split, because that is what somebody writing
     * `command: "sleep infinity"` in YAML means -- passing it as one argv
     * element gets it rejected by podman for no visible reason.
     */
    clawt_container_computer_set_command(container, "sleep 3600");
    g_assert_nonnull(strstr(clawt_container_computer_get_command(container),
                            "\"sleep\""));
    g_assert_nonnull(strstr(clawt_container_computer_get_command(container),
                            "\"3600\""));

    /* Empty keeps whatever was there rather than clearing it. */
    clawt_container_computer_set_command(container, "");
    g_assert_nonnull(clawt_container_computer_get_command(container));
}

/*
 * The image catalogue is a suggestion list, and the first entry is what
 * an agent gets when it names none.
 *
 * The default used to be a Debian slim image nobody had pulled, so a
 * first container agent failed with "no such image" against something
 * the user had never chosen.  Fedora because that is what clawtilla is
 * developed on.
 */
static void
test_image_catalog(void)
{
    const ClawtImageInfo *catalog;
    gsize n_images = 0;
    gsize i;

    catalog = clawt_image_catalog_get(&n_images);
    g_assert_nonnull(catalog);
    g_assert_cmpuint(n_images, >, 0);

    g_assert_cmpstr(clawt_image_catalog_default(), ==,
                    "registry.fedoraproject.org/fedora:44");
    g_assert_cmpstr(catalog[0].reference, ==, clawt_image_catalog_default());

    for (i = 0; i < n_images; i++) {
        g_assert_nonnull(catalog[i].reference);
        g_assert_nonnull(catalog[i].label);
        g_assert_nonnull(catalog[i].group);

        /*
         * Every reference names its registry.  A bare "fedora:44" is
         * resolved through podman's unqualified-search list, which is
         * per-machine, so the same config would pull different images on
         * two hosts -- and the difference surfaces much later as a
         * missing package.
         */
        g_assert_nonnull(strchr(catalog[i].reference, '/'));
        g_assert_nonnull(strchr(catalog[i].reference, ':'));
    }
}

/*
 * An agent that names no image inherits defaults.container_image, and
 * the schema default backs that up.  Without the inheritance the fleet
 * default was a documented setting that nothing read.
 */
static void
test_image_inherits_the_default(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "defaults:\n"
        "  container_image: \"registry.example.com/base:1\"\n"
        "agents:\n"
        "  - id: inherits\n"
        "    computer: {type: container}\n"
        "  - id: explicit\n"
        "    computer: {type: container, container: {image: \"other:2\"}}\n",
        &error);
    GPtrArray *agents;

    g_assert_no_error(error);
    g_assert_nonnull(config);

    agents = clawt_config_get_agents(config);
    g_assert_cmpuint(agents->len, ==, 2);

    g_assert_cmpstr(clawt_agent_config_get_string(
                        g_ptr_array_index(agents, 0),
                        "computer.container.image"),
                    ==, "registry.example.com/base:1");

    /* An agent that named one keeps it. */
    g_assert_cmpstr(clawt_agent_config_get_string(
                        g_ptr_array_index(agents, 1),
                        "computer.container.image"),
                    ==, "other:2");
}

/*
 * With no fleet default either, the schema's own default applies -- so a
 * config that says nothing about images still produces an agent with a
 * working one.
 */
static void
test_image_falls_back_to_the_schema(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: bare\n"
        "    computer: {type: container}\n",
        &error);
    GPtrArray *agents;

    g_assert_no_error(error);
    agents = clawt_config_get_agents(config);

    g_assert_cmpstr(clawt_agent_config_get_string(
                        g_ptr_array_index(agents, 0),
                        "computer.container.image"),
                    ==, clawt_image_catalog_default());
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/computer/null/refuses", test_null_computer_refuses_clearly);

    g_test_add_func("/computer/host/runs", test_host_runs_a_command);
    g_test_add_func("/computer/host/failure", test_host_reports_a_failing_command);
    g_test_add_func("/computer/host/outside-boundary",
                    test_host_refuses_a_command_outside_the_boundary);
    g_test_add_func("/computer/host/cwd-outside",
                    test_host_refuses_a_working_directory_outside);
    g_test_add_func("/computer/host/timeout", test_host_times_out_a_hanging_command);
    g_test_add_func("/computer/host/file-transfer",
                    test_host_file_transfer_respects_the_boundary);
    g_test_add_func("/computer/host/description",
                    test_host_description_mentions_the_confinement);
    g_test_add_func("/computer/truncation", test_output_truncation_is_reported);

    g_test_add_func("/computer/container/mount-json", test_container_mount_json);
    g_test_add_func("/computer/container/no-mounts",
                    test_container_mount_json_handles_no_mounts);

    g_test_add_func("/computer/vm/shared-memory",
                    test_vm_domain_xml_includes_shared_memory_for_mounts);
    g_test_add_func("/computer/vm/read-only",
                    test_vm_domain_xml_marks_read_only_mounts);
    g_test_add_func("/computer/vm/escapes-markup",
                    test_vm_domain_xml_escapes_markup);
    g_test_add_func("/computer/vm/no-shared-memory-without-mounts",
                    test_vm_domain_xml_omits_shared_memory_without_mounts);
    g_test_add_func("/computer/vm/qemu-argv", test_vm_qemu_argv);
    g_test_add_func("/computer/vm/seed-cdrom",
                    test_vm_domain_xml_attaches_the_seed);
    g_test_add_func("/computer/vm/passt-port-forward",
                    test_vm_domain_xml_forwards_ssh_through_passt);
    g_test_add_func("/computer/vm/no-forward-stays-plain",
                    test_vm_domain_xml_without_a_forward_stays_plain);
    g_test_add_func("/computer/vm/qemu-hostfwd",
                    test_vm_qemu_argv_forwards_a_known_port);
    g_test_add_func("/computer/vm/qemu-seed",
                    test_vm_qemu_argv_attaches_the_seed);
    g_test_add_func("/computer/vm/no-address-no-argv",
                    test_vm_ssh_argv_is_null_without_an_address);
    g_test_add_func("/computer/vm/ssh-argv-port",
                    test_vm_ssh_argv_uses_the_forwarded_port);
    g_test_add_func("/computer/vm/ssh-argv-quoting",
                    test_vm_ssh_argv_quotes_the_command);
    g_test_add_func("/computer/vm/boots-and-runs",
                    test_vm_boots_and_runs_a_command);

    g_test_add_func("/computer/desktop/observe-only",
                    test_desktop_observe_only_omits_input_tools);
    g_test_add_func("/computer/desktop/with-input",
                    test_desktop_with_input_permits_acting);
    g_test_add_func("/computer/desktop/unknown-tool",
                    test_unknown_desktop_tool_is_refused);
    g_test_add_func("/computer/desktop/dead-socket",
                    test_dead_gowl_socket_is_not_available);
    g_test_add_func("/computer/desktop/description",
                    test_desktop_description_states_the_grant);

    g_test_add_func("/computer/factory/default", test_factory_builds_the_configured_backend);
    g_test_add_func("/computer/factory/unconfirmed-host",
                    test_factory_refuses_unconfirmed_host);
    g_test_add_func("/computer/factory/confirmed-host",
                    test_factory_builds_a_confirmed_host);
    g_test_add_func("/computer/factory/mount-type",
                    test_factory_picks_the_mount_type_for_the_backend);
    g_test_add_func("/computer/factory/host-mounts-allowlist",
                    test_factory_treats_host_mounts_as_an_allowlist);
    g_test_add_func("/computer/factory/desktop", test_factory_desktop_is_optional);

    g_test_add_func("/computer/integration/container",
                    test_container_against_real_podman);

    g_test_add_func("/computer/host-mount-target",
                    test_host_reaches_a_mount_by_its_target);
    g_test_add_func("/computer/mount-is-a-grant",
                    test_a_mount_is_a_grant_in_workspace_mode);
    g_test_add_func("/computer/mount-prefix-not-a-match",
                    test_a_mount_prefix_is_not_a_match);
    g_test_add_func("/computer/module-search-path", test_module_search_path);
    g_test_add_func("/computer/container-connection-default",
                    test_container_connection_default);
    g_test_add_func("/computer/container-command", test_container_command);
    g_test_add_func("/computer/image-catalog", test_image_catalog);
    g_test_add_func("/computer/image-inherits-the-default",
                    test_image_inherits_the_default);
    g_test_add_func("/computer/image-falls-back-to-the-schema",
                    test_image_falls_back_to_the_schema);
    g_test_add_func("/computer/socket-path-length",
                    test_an_over_long_socket_path_is_refused);

    return g_test_run();
}
