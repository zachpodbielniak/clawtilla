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
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 4, 4096, 64);

    mount = clawt_mount_new("/tmp", "/work");
    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);
    clawt_computer_add_mount(computer, mount);

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "<memoryBacking>"));
    g_assert_nonnull(strstr(xml, "access mode='shared'"));
    g_assert_nonnull(strstr(xml, "driver type='virtiofs'"));

    /*
     * The host directory by name, and the guest's side by *tag* -- which
     * is derived from the target rather than being it, because qemu
     * refuses a tag over 36 bytes and refuses the device with it.
     */
    g_assert_nonnull(strstr(xml, "<source dir='/tmp'/>"));
    {
        g_autofree gchar *tag = clawt_mount_tag("/work");

        g_assert_nonnull(strstr(xml, tag));
    }

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
    /*
     * The backend element, not the bare word: `host-passthrough` on the
     * CPU contains "passt", so a substring match here started failing
     * the moment the guest was given a decent processor.
     */
    g_assert_null(strstr(xml, "<backend type='passt'/>"));
    g_assert_null(strstr(xml, "<portForward"));
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
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 2, 1024, 16);

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
    clawt_vm_computer_set_resources(CLAWT_VM_COMPUTER(computer), 2, 1024, 16);

    mount = clawt_mount_new("/tmp", "/work");
    clawt_computer_add_mount(computer, mount);

    /*
     * A share on this backend is refused out loud, and the refusal is
     * the point of the assertion.
     *
     * `-chardev socket,path=...` connects as a client, and nothing here
     * ever started a virtiofsd to listen -- so qemu exited before the
     * guest existed. Emitting the device was worse than not having the
     * share, and it only stayed hidden while nothing had a mount by
     * default.
     */
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*qemu backend shares no directories*");

    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(computer),
                                             "/tmp/clawt-qmp.sock");

    g_test_assert_expected_messages();

    g_assert_cmpstr(argv[0], ==, "qemu-system-x86_64");

    for (i = 0; argv[i] != NULL; i++) {
        if (strstr(argv[i], "/tmp/clawt-qmp.sock") != NULL)
            saw_qmp = TRUE;
        if (strstr(argv[i], "memory-backend-memfd") != NULL)
            saw_memfd = TRUE;

        /* And no half-built device that qemu would refuse to start on. */
        g_assert_null(strstr(argv[i], "vhost-user-fs"));
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
    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

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

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

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

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_HOST);
}

/*
 * A `confine: none` host computer is refused here as well as at config
 * load, unless the fleet has said so too.
 *
 * Running unconfined on the real machine takes two deliberate acts --
 * the agent's confirm_host_control and the fleet's
 * daemon.allow_unconfined_host -- and only the first of the two was
 * checked in the factory.  Validation catches it in a config file; this
 * catches an agent constructed some other way, which is precisely the
 * case the confirmation's own double-check exists for, and it is the
 * stronger of the two grants.
 */
static void
test_factory_refuses_unconfined_without_the_fleet_gate(void)
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
        "        confine: none\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

    g_assert_null(computer);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "daemon.allow_unconfined_host"));
}

/*
 * And with both acts performed it builds, unconfined.
 *
 * Worth its own case rather than trusting the refusal above: the fleet
 * key has no agent-relative spelling, so a check that asked for it
 * through the agent's own getters would answer FALSE from the schema
 * whatever the file said -- refusing every properly-granted host while
 * looking exactly like a gate that works.
 */
static void
test_factory_builds_an_unconfined_host_when_both_agree(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
        "daemon:\n"
        "  allow_unconfined_host: true\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        confine: none\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(computer);
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

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
    g_assert_no_error(error);

    mounts = clawt_computer_get_mounts(computer);

    /*
     * Found by target rather than by position: every computer also gets
     * the agent's workspace, so the configured mount is not the only one
     * here and counting them tests the wrong thing.
     */
    {
        ClawtMount *work = NULL;
        guint i;

        for (i = 0; i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            if (g_strcmp0(clawt_mount_get_target(mount), "/work") == 0)
                work = mount;
        }

        g_assert_nonnull(work);
        g_assert_cmpint(clawt_mount_get_mount_type(work), ==,
                        CLAWT_MOUNT_VIRTIOFS);
    }
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

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
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

/*
 * A call loads the module it needs, rather than assuming somebody else
 * already did.
 *
 * clawt_pod_bridge_call_for() only ever looked the module up, and the
 * container backend loaded it in exactly one place -- container_provision().
 * Provisioning happens at an agent's *start*, so every other entry point
 * -- start, stop, exec and teardown -- depended on a side effect of a
 * function that may never have run in this daemon's lifetime.
 *
 * Removing a stopped agent therefore refused with "the podomation
 * 'container' module is not loaded" and left the container on the
 * machine under a name nothing refers to any more.  A daemon restart is
 * enough to reach it, which makes it the ordinary case rather than an
 * edge one: a libvirt domain and a podman container both outlive the
 * daemon.
 *
 * Loading on demand in call_for() rather than adding a load to each of
 * the five container call sites, because a new call site cannot forget
 * what it does not have to remember.  load_module_for() is idempotent
 * and takes exactly the arguments call_for() already holds.
 *
 * Hermetic: loading a .so and starting the container module's event
 * source needs no podman -- its start() spawns the event-stream thread
 * and returns TRUE unconditionally.  Only the *action* would need a
 * daemon, and this asserts on the loading.
 */
static void
test_a_call_loads_the_module_it_needs(void)
{
    g_autoptr(ClawtPodBridge) bridge = NULL;
    g_autoptr(GError) probe_error = NULL;
    g_autoptr(GHashTable) params = NULL;
    GHashTable *result;
    g_autoptr(GError) error = NULL;
    /*
     * The deps are built release whatever build type this is -- the
     * `deps` target pins DEBUG=0 -- so this is the right directory for
     * both trees rather than one that works only for `make test`.
     */
    g_autofree gchar *module_dir = g_build_filename(
        CLAWT_TEST_SRCDIR, "deps", "libreclaw", "build", "release",
        "modules", NULL);

    bridge = clawt_pod_bridge_new(module_dir);

    /*
     * Skip where the module was never built, rather than failing for a
     * reason that has nothing to do with the rule under test.
     */
    {
        g_autoptr(ClawtPodBridge) probe = clawt_pod_bridge_new(module_dir);

        if (!clawt_pod_bridge_load_module(probe, "container", &probe_error)) {
            g_test_skip(probe_error->message);
            return;
        }
    }

    /* Nothing has provisioned, so nothing has loaded it. */
    g_assert_false(clawt_pod_bridge_has_module(bridge, "container"));

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    result = clawt_pod_bridge_call_for(bridge, "container", NULL, "list",
                                       params, &error);

    /*
     * The action may well fail without a podman socket, and that is not
     * what this is about.  What must not happen is a refusal saying the
     * module is not loaded -- that is the bug, and it is the sentence
     * the operator saw.
     */
    if (result == NULL) {
        g_assert_nonnull(error);
        g_assert_false(g_str_has_suffix(error->message, "is not loaded"));
    } else {
        g_hash_table_unref(result);
    }

    /* Loaded on demand, by the call itself. */
    g_assert_true(clawt_pod_bridge_has_module(bridge, "container"));
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
/*
 * The state directory cannot be mounted into a computer -- as a *bind*
 * mount, which is the shape somebody would actually write.
 *
 * The refusal existed, was documented on clawt_mount_set_forbidden_sources()
 * and was populated by the daemon at start, and could not fire: the check
 * sat inside the CLAWT_MOUNT_TMPFS branch behind `source != NULL`, which
 * for a tmpfs is itself an error.  So it ran only in a case that was
 * refused anyway.  A test that built a tmpfs would have passed against
 * the broken code, which is the whole reason this one is a bind.
 */
static void
test_forbidden_source_is_refused_for_a_bind(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-forbid-XXXXXX", NULL);
    g_autofree gchar *state = g_build_filename(dir, "state", NULL);
    g_autofree gchar *parent = g_build_filename(state, "..", NULL);
    g_autofree gchar *inside = g_build_filename(state, "agents", NULL);
    g_autofree gchar *elsewhere = g_build_filename(dir, "notes", NULL);
    const gchar *forbidden[] = { NULL, NULL };
    g_autoptr(ClawtMount) bad = NULL;
    g_autoptr(ClawtMount) below = NULL;
    g_autoptr(ClawtMount) fine = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GError) below_error = NULL;

    g_mkdir_with_parents(inside, 0700);
    g_mkdir_with_parents(elsewhere, 0700);

    forbidden[0] = state;
    clawt_mount_set_forbidden_sources(forbidden);

    bad = clawt_mount_new(state, "/mnt/state");
    g_assert_false(clawt_mount_validate(bad, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_MOUNT);

    /* A directory below it is the same disclosure by a longer path. */
    below = clawt_mount_new(inside, "/mnt/agents");
    g_assert_false(clawt_mount_validate(below, &below_error));
    g_assert_error(below_error, CLAWT_ERROR, CLAWT_ERROR_MOUNT);

    /* And an unrelated source still passes, or the check is just a wall. */
    fine = clawt_mount_new(elsewhere, "/mnt/notes");
    g_assert_true(clawt_mount_validate(fine, NULL));

    forbidden[0] = NULL;
    clawt_mount_set_forbidden_sources(forbidden);
    clawt_test_remove_tree(dir);
}

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

/*
 * An agent's overlay is built on one base image, and qcow2 records which.
 * Provisioning again must not rebuild it: the guest's contents live in
 * that file, and throwing them away every start would make a VM agent
 * amnesiac.
 */
/* A tiny qcow2 to hang an overlay off; nothing ever boots these. */
static gboolean
make_blank_image(const gchar *path)
{
    g_autoptr(GSubprocess) process =
        g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                         G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                         "qemu-img", "create", "-f", "qcow2", path, "16M",
                         NULL);

    return process != NULL &&
           g_subprocess_wait_check(process, NULL, NULL);
}

static gchar *
image_backing_file(const gchar *overlay)
{
    g_autoptr(GSubprocess) process = NULL;
    g_autofree gchar *output = NULL;
    g_auto(GStrv) lines = NULL;
    gsize i;

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                               "qemu-img", "info", overlay, NULL);

    if (process == NULL ||
        !g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL,
                                       NULL))
        return NULL;

    lines = g_strsplit(output, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "backing file: "))
            return g_strdup(lines[i] + strlen("backing file: "));
    }

    return NULL;
}

/*
 * A VM agent with no disk image used to define, start, boot nothing and
 * show a black console -- while SSH to it answered "connection reset",
 * because the forwarded port reached a guest with no sshd. Three
 * symptoms, one missing setting, and nothing connecting them.
 */
/*
 * define_xml runs on every provision, so config changes reach the domain.
 * Without a UUID libvirt invents a fresh one each time and then refuses
 * the name it already holds -- so an agent could be provisioned once and
 * never started again.
 */
static void
test_vm_domain_xml_uuid_is_stable(void)
{
    g_autoptr(ClawtComputer) first =
        clawt_vm_computer_new("steady", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autoptr(ClawtComputer) second =
        clawt_vm_computer_new("steady", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *one = NULL;
    g_autofree gchar *two = NULL;

    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(first), "/tmp/a.qcow2");
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(second), "/tmp/a.qcow2");

    one = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(first));
    two = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(second));

    g_assert_nonnull(strstr(one, "<uuid>"));
    g_assert_cmpstr(one, ==, two);

    /* A different agent must not collide with it. */
    {
        g_autoptr(ClawtComputer) other =
            clawt_vm_computer_new("elsewhere", CLAWT_VM_BACKEND_LIBVIRT,
                                  NULL);
        g_autofree gchar *third = NULL;

        clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(other), "/tmp/a.qcow2");
        third = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(other));

        g_assert_cmpstr(one, !=, third);
    }
}

/*
 * A domain defined before clawtilla supplied a UUID has whatever libvirt
 * invented then, and libvirt will not take a different one for the same
 * name.  Provisioning adopts it rather than undefining somebody's domain.
 */
static void
test_vm_domain_xml_keeps_an_adopted_uuid(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("adopted", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), "/tmp/a.qcow2");
    clawt_vm_computer_set_uuid(CLAWT_VM_COMPUTER(computer),
                               "deadbeef-1234-4567-89ab-cdef01234567");

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml,
                            "<uuid>deadbeef-1234-4567-89ab-cdef01234567"
                            "</uuid>"));
}

static void
test_vm_without_an_image_is_refused(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("nodisk", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_computer_provision(computer, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION);

    /* The message has to name the setting, or it is just a refusal. */
    g_assert_nonnull(strstr(error->message, "computer.vm.image"));

    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_ERROR);
}

/*
 * The XML is what proves it: a domain built without a disk is one that
 * boots nothing, and it looked perfectly valid to libvirt.
 */
static void
test_vm_domain_xml_always_has_a_disk_to_boot(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("nodisk", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer),
                                "/tmp/base.qcow2");

    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(xml, "device='disk'"));
    g_assert_nonnull(strstr(xml, "<boot dev='hd'/>"));
}

/*
 * A qemu child already running must not be joined by a second one: two
 * writers on one qcow2 corrupt it.
 */
static void
test_vm_start_does_not_spawn_a_second_guest(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("twice", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(GError) error = NULL;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    g_assert_true(clawt_computer_start(computer, &error));
    g_assert_no_error(error);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);
}

/*
 * The worst thing this code ever did.
 *
 * qemu-img refuses an image a running guest holds -- `Failed to get
 * shared "write" lock` -- and that refusal was read as "the base is
 * something else", so provisioning deleted the disk of a running VM. The
 * guest carried on against the unlinked inode while the file on disk was
 * replaced by an empty one.
 *
 * Unknown is not different. An overlay whose base cannot be read is left
 * exactly as it is.
 */
static void
test_vm_overlay_is_kept_when_its_base_cannot_be_read(void)
{
    g_autofree gchar *base = NULL;
    g_autofree gchar *overlay = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    GStatBuf before;
    GStatBuf after;

    {
        g_autofree gchar *tool = g_find_program_in_path("qemu-img");

        if (tool == NULL) {
            g_test_skip("qemu-img is not installed here");
            return;
        }
    }

    base = g_build_filename(g_get_user_data_dir(), "unreadable-base.qcow2",
                            NULL);
    g_assert_true(make_blank_image(base));

    computer = clawt_vm_computer_new("unreadable", CLAWT_VM_BACKEND_QEMU,
                                     NULL);
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), base);
    clawt_vm_computer_set_cloud_init(CLAWT_VM_COMPUTER(computer), FALSE);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    overlay = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                               "unreadable", "overlay.qcow2", NULL);
    g_assert_cmpint(g_stat(overlay, &before), ==, 0);

    /*
     * Standing in for the lock a running guest holds: whatever the
     * reason, qemu-img cannot answer, and the answer to that is to leave
     * the disk alone.
     */
    g_assert_cmpint(g_chmod(overlay, 0), ==, 0);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_chmod(overlay, 0600), ==, 0);
    g_assert_cmpint(g_stat(overlay, &after), ==, 0);

    /* Same file, untouched. */
    g_assert_cmpint(before.st_ino, ==, after.st_ino);
    g_assert_cmpint(before.st_mtime, ==, after.st_mtime);
}

/*
 * A base that has gone is named, rather than surfacing later as a qemu
 * complaint about a backing file nobody chose by hand.
 */
static void
test_vm_missing_base_image_is_named(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("nobase", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(GError) error = NULL;

    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer),
                                "/nonexistent/never-here.qcow2");
    clawt_vm_computer_set_cloud_init(CLAWT_VM_COMPUTER(computer), FALSE);

    g_assert_false(clawt_computer_provision(computer, &error));
    g_assert_nonnull(strstr(error->message, "never-here.qcow2"));
}

static void
test_vm_overlay_survives_a_second_provision(void)
{
    g_autofree gchar *base = NULL;
    g_autofree gchar *overlay = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    GStatBuf before;
    GStatBuf after;

    {
        g_autofree gchar *tool = g_find_program_in_path("qemu-img");

        if (tool == NULL) {
            g_test_skip("qemu-img is not installed here");
            return;
        }
    }

    base = g_build_filename(g_get_user_data_dir(), "base-one.qcow2", NULL);
    g_assert_true(make_blank_image(base));

    computer = clawt_vm_computer_new("overlaytest", CLAWT_VM_BACKEND_QEMU,
                                     NULL);
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), base);
    clawt_vm_computer_set_cloud_init(CLAWT_VM_COMPUTER(computer), FALSE);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    overlay = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                               "overlaytest", "overlay.qcow2", NULL);
    g_assert_cmpint(g_stat(overlay, &before), ==, 0);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_stat(overlay, &after), ==, 0);
    g_assert_cmpint(before.st_ino, ==, after.st_ino);
}

/*
 * Changing the disk image has to take effect.
 *
 * The overlay is built on the old base, so keeping it would leave the VM
 * booting the old image for ever while the config said otherwise -- a
 * setting that silently does nothing, which is what picking one from the
 * client's list would have done.
 */
static void
test_vm_overlay_is_rebuilt_when_the_image_changes(void)
{
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    g_autofree gchar *overlay = NULL;
    g_autofree gchar *backing = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;

    {
        g_autofree gchar *tool = g_find_program_in_path("qemu-img");

        if (tool == NULL) {
            g_test_skip("qemu-img is not installed here");
            return;
        }
    }

    first = g_build_filename(g_get_user_data_dir(), "base-first.qcow2", NULL);
    second = g_build_filename(g_get_user_data_dir(), "base-second.qcow2",
                              NULL);
    g_assert_true(make_blank_image(first));
    g_assert_true(make_blank_image(second));

    computer = clawt_vm_computer_new("swaptest", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_vm_computer_set_cloud_init(CLAWT_VM_COMPUTER(computer), FALSE);
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), first);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), second);

    /* The rebuild says so out loud, because it costs the guest. */
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*disk image changed*");
    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);
    g_test_assert_expected_messages();

    overlay = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                               "swaptest", "overlay.qcow2", NULL);
    backing = image_backing_file(overlay);

    g_assert_cmpstr(backing, ==, second);
}

/*
 * The bug this exists for: teardown used to go through CALL_OR_TRUE, so
 * a backend that had not implemented it reported the computer as
 * destroyed and destroyed nothing.  Removing a VM agent said "removed"
 * and left the libvirt domain defined.
 *
 * A VM has a teardown now, so the assertion is that the *contract* holds
 * -- a computer that cannot destroy itself has to say so.
 */
static void
test_teardown_is_never_a_silent_success(void)
{
    g_autoptr(ClawtComputer) vm =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtComputer) null_computer =
        clawt_null_computer_new("scribe");
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, "/tmp");
    g_autoptr(ClawtComputer) host =
        clawt_host_computer_new("scribe", sandbox);
    g_autoptr(ClawtComputer) box =
        clawt_distrobox_computer_new("scribe", NULL, NULL);
    ClawtComputerClass *klass;

    /*
     * Every backend states its answer rather than inheriting one.  A new
     * backend that forgets gets the base class's refusal, which is the
     * whole point -- so what is asserted is that none of these is relying
     * on it.
     */
    klass = CLAWT_COMPUTER_GET_CLASS(vm);
    g_assert_nonnull(klass->teardown);

    klass = CLAWT_COMPUTER_GET_CLASS(null_computer);
    g_assert_nonnull(klass->teardown);

    klass = CLAWT_COMPUTER_GET_CLASS(host);
    g_assert_nonnull(klass->teardown);

    klass = CLAWT_COMPUTER_GET_CLASS(box);
    g_assert_nonnull(klass->teardown);

    /* The two with genuinely nothing to destroy succeed. */
    g_assert_true(clawt_computer_teardown(null_computer, NULL));
    g_assert_true(clawt_computer_teardown(host, NULL));
}

/*
 * The config reaches the distrobox, including the setting that decides
 * what it can read.
 *
 * This is the gap this project keeps recording: two halves that each
 * work and no wire between them. clawt_distrobox_computer_resolve_home()
 * is tested above and the schema keys are generated -- neither notices
 * if the factory forgets to call the setter, and the symptom would be an
 * agent silently given the operator's entire home directory.
 *
 * The bridge is NULL, so this asserts what was *configured* rather than
 * what podman did; that is the half a machine without distrobox can
 * still check, and it is the half that decides the confinement.
 */
static void
test_the_factory_wires_a_distrobox_from_config(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *home = NULL;

    agent = agent_from_yaml(&config,
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: distrobox\n"
        "      distrobox:\n"
        "        home: /var/lib/boxes/chief\n");

    /*
     * A real bridge, which needs no podman and loads no module until
     * something is provisioned. Passing NULL here would make the factory
     * refuse before it built anything -- so the test would assert a
     * refusal message and nothing about the wiring it exists for.
     */
    computer = clawt_computer_factory_create(agent, NULL, bridge, &error);

    g_assert_no_error(error);
    g_assert_nonnull(computer);
    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_DISTROBOX);

    home = clawt_distrobox_computer_resolve_home(
        CLAWT_DISTROBOX_COMPUTER(computer));
    g_assert_cmpstr(home, ==, "/var/lib/boxes/chief");
}

/*
 * Without a bridge it refuses, and names the module it wanted.
 *
 * "not supported" on its own sends somebody to check whether their
 * clawtilla was built with distrobox in it, which is not a thing.
 */
static void
test_a_distrobox_without_a_bridge_names_the_module(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
        "agents:\n  - id: chief\n    computer:\n      type: distrobox\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);

    g_assert_null(computer);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "distrobox"));
}

/*
 * share_home reaches the computer, and is the only thing that makes it
 * take the operator's home.
 *
 * Asserted through the factory rather than only on the setter, because
 * a factory that read the wrong key -- or did not read it at all --
 * would leave the safe default in place and look correct, while the
 * person who asked to share their home would silently not be sharing
 * it. Wrong in the harmless direction, which is how it would survive.
 */
static void
test_share_home_reaches_the_computer_from_config(void)
{
    static const struct {
        const gchar *yaml;
        gboolean     shares;
    } cases[] = {
        { "agents:\n  - id: chief\n    computer:\n      type: distrobox\n",
          FALSE },
        { "agents:\n  - id: chief\n    computer:\n      type: distrobox\n"
          "      distrobox:\n        share_home: true\n", TRUE },
        { "agents:\n  - id: chief\n    computer:\n      type: distrobox\n"
          "      distrobox:\n        share_home: false\n"
          "        home: /var/lib/boxes/chief\n", FALSE }
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autoptr(ClawtConfig) config = NULL;
        g_autoptr(ClawtComputer) computer = NULL;
        g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
        g_autoptr(GError) error = NULL;
        ClawtAgentConfig *agent;
        g_autofree gchar *home = NULL;

        agent = agent_from_yaml(&config, cases[i].yaml);
        computer = clawt_computer_factory_create(agent, NULL, bridge, &error);

        g_assert_no_error(error);
        g_assert_nonnull(computer);

        home = clawt_distrobox_computer_resolve_home(
            CLAWT_DISTROBOX_COMPUTER(computer));

        if (cases[i].shares)
            g_assert_null(home);
        else
            g_assert_nonnull(home);
    }
}

/*
 * The type list both clients walk, and the two predicates they branch
 * on.
 *
 * There were five hand-written copies of this list between the clients,
 * so a type added to the fleet reached whichever of them somebody
 * remembered. What replaced them is only worth having if every type is
 * in it and every nick round-trips -- a type in the enum and missing
 * from the order table would be one no client could ever offer.
 */
static void
test_every_computer_type_is_offered_and_round_trips(void)
{
    static const ClawtComputerType every[] = {
        CLAWT_COMPUTER_NONE, CLAWT_COMPUTER_HOST, CLAWT_COMPUTER_CONTAINER,
        CLAWT_COMPUTER_DISTROBOX, CLAWT_COMPUTER_VM
    };
    guint i;
    guint j;

    g_assert_cmpuint(clawt_computer_type_count(), ==,
                     G_N_ELEMENTS(every));

    for (i = 0; i < G_N_ELEMENTS(every); i++) {
        gboolean found = FALSE;

        for (j = 0; j < clawt_computer_type_count(); j++) {
            const gchar *nick = clawt_computer_type_nth_nick(j);
            gint back = -1;

            g_assert_nonnull(nick);
            g_assert_nonnull(clawt_computer_type_nth_label(j));

            /* The nick a client stores has to come back as the type. */
            g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE,
                                               nick, &back));
            g_assert_cmpint(back, ==, clawt_computer_type_nth(j));

            if (clawt_computer_type_nth(j) == every[i])
                found = TRUE;
        }

        g_assert_true(found);
    }

    /* Least capable first, so the list reads as an escalation. */
    g_assert_cmpint(clawt_computer_type_nth(0), ==, CLAWT_COMPUTER_NONE);
    g_assert_cmpint(clawt_computer_type_nth(
                        clawt_computer_type_count() - 1), ==,
                    CLAWT_COMPUTER_HOST);
}

/*
 * The predicates that replaced five copies of `== "container"`.
 *
 * A distrobox takes an image and mounts exactly as a container does,
 * and that is the whole reason these exist -- adding the type without
 * them would have given it a form with no image chooser and no shared
 * folders, silently.
 */
static void
test_the_type_predicates_cover_every_type(void)
{
    g_assert_true(clawt_computer_type_takes_image(CLAWT_COMPUTER_CONTAINER));
    g_assert_true(clawt_computer_type_takes_image(CLAWT_COMPUTER_DISTROBOX));

    /* A VM takes a *disk* image, which is a different setting. */
    g_assert_false(clawt_computer_type_takes_image(CLAWT_COMPUTER_VM));
    g_assert_false(clawt_computer_type_takes_image(CLAWT_COMPUTER_NONE));
    g_assert_false(clawt_computer_type_takes_image(CLAWT_COMPUTER_HOST));

    g_assert_true(clawt_computer_type_takes_mounts(CLAWT_COMPUTER_CONTAINER));
    g_assert_true(clawt_computer_type_takes_mounts(CLAWT_COMPUTER_DISTROBOX));
    g_assert_true(clawt_computer_type_takes_mounts(CLAWT_COMPUTER_VM));

    /*
     * Not host: there the mount list is the confinement allowlist rather
     * than a set of kernel mounts, so a "Shared folders" editor would be
     * editing something else under a name that does not fit.
     */
    g_assert_false(clawt_computer_type_takes_mounts(CLAWT_COMPUTER_NONE));
    g_assert_false(clawt_computer_type_takes_mounts(CLAWT_COMPUTER_HOST));
}

/*
 * Whether a computer ended up with a mount at this path.
 *
 * Asked of the computer rather than of the config, because the whole
 * point of a fleet default is that it reaches an agent whose own block
 * never mentions it.
 */
static gboolean
has_target(GPtrArray *mounts, const gchar *target)
{
    guint i;

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        if (g_strcmp0(clawt_mount_get_target(g_ptr_array_index(mounts, i)),
                      target) == 0)
            return TRUE;
    }

    return FALSE;
}

/* ── Fleet-wide shared folders ────────────────────────────────────── */

/*
 * A default reaches an agent that has a computer.
 *
 * The whole feature is that this happens without the agent block
 * mentioning it, so the assertion has to be on the *computer* rather
 * than on the config -- reading the config back would only prove the
 * YAML was written.
 */
static void
test_a_default_mount_reaches_a_container(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(GPtrArray) defaults = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    GPtrArray *applied;

    agent = agent_from_yaml(&config,
        "defaults:\n"
        "  mounts:\n"
        "    - source: /tmp\n"
        "      target: /work/source\n"
        "      required: false\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: container\n");

    defaults = clawt_config_get_default_mounts(config);
    g_assert_cmpuint(defaults->len, ==, 1);

    computer = clawt_computer_factory_create(agent, defaults, bridge, &error);
    g_assert_no_error(error);

    applied = clawt_computer_get_mounts(computer);
    g_assert_true(has_target(applied, "/work/source"));
}

/*
 * And does not reach one that has no computer to mount into, or a host
 * agent.
 *
 * The host case is the one that matters. There a mount is the
 * confinement allowlist rather than a kernel mount, so applying a fleet
 * default would quietly widen what a host agent may reach -- a
 * convenience silently loosening a security boundary, which is the
 * worst direction for this to be wrong in.
 */
static void
test_a_default_mount_does_not_widen_a_host_agent(void)
{
    static const gchar *const yamls[] = {
        "defaults:\n"
        "  mounts:\n"
        "    - source: /tmp\n"
        "      target: /work/source\n"
        "      required: false\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n",

        "defaults:\n"
        "  mounts:\n"
        "    - source: /tmp\n"
        "      target: /work/source\n"
        "      required: false\n"
        "agents:\n"
        "  - id: chief\n",
        NULL
    };
    guint i;

    for (i = 0; yamls[i] != NULL; i++) {
        g_autoptr(ClawtConfig) config = NULL;
        g_autoptr(ClawtComputer) computer = NULL;
        g_autoptr(GPtrArray) defaults = NULL;
        g_autoptr(GError) error = NULL;
        ClawtAgentConfig *agent;

        agent = agent_from_yaml(&config, yamls[i]);
        defaults = clawt_config_get_default_mounts(config);
        computer = clawt_computer_factory_create(agent, defaults, NULL,
                                                 &error);

        g_assert_no_error(error);
        g_assert_nonnull(computer);
        g_assert_false(has_target(clawt_computer_get_mounts(computer),
                                  "/work/source"));
    }
}

/*
 * An agent can decline the lot.
 *
 * Which is different from overriding one: the point of giving an agent
 * a container is sometimes that it sees only what it was given.
 */
static void
test_an_agent_can_decline_the_fleet_folders(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(GPtrArray) defaults = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
        "defaults:\n"
        "  mounts:\n"
        "    - source: /tmp\n"
        "      target: /work/source\n"
        "      required: false\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: container\n"
        "      default_mounts: false\n");

    defaults = clawt_config_get_default_mounts(config);
    computer = clawt_computer_factory_create(agent, defaults, bridge, &error);

    g_assert_no_error(error);
    g_assert_false(has_target(clawt_computer_get_mounts(computer),
                              "/work/source"));
}

/*
 * An agent's own folder at the same path wins, and the default is not
 * also applied.
 *
 * Adding both would put two mounts at one path, which validation
 * refuses -- so an agent that customised a single shared folder would
 * stop starting, naming the path its owner had deliberately chosen.
 * That is the failure this rule exists to prevent, and it needs a
 * container to reproduce, so it is asserted here instead.
 */
static void
test_an_agents_own_folder_replaces_the_default(void)
{
    g_autoptr(GPtrArray) defaults = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    g_autoptr(GPtrArray) own = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_mount_free);
    g_autoptr(GPtrArray) merged = NULL;
    ClawtMount *fleet;
    ClawtMount *mine;

    fleet = clawt_mount_new("/srv/fleet", "/work/source");
    g_ptr_array_add(defaults, fleet);
    g_ptr_array_add(defaults, clawt_mount_new("/srv/notes", "/work/notes"));

    mine = clawt_mount_new("/home/me/other", "/work/source");
    g_ptr_array_add(own, mine);

    merged = clawt_mount_merge_defaults(defaults, own);

    /* Three configured, three minus the overridden one. */
    g_assert_cmpuint(merged->len, ==, 2);

    /* The surviving default, then the agent's own. */
    g_assert_cmpstr(clawt_mount_get_target(g_ptr_array_index(merged, 0)),
                    ==, "/work/notes");
    g_assert_cmpstr(clawt_mount_get_target(g_ptr_array_index(merged, 1)),
                    ==, "/work/source");
    g_assert_cmpstr(clawt_mount_get_source(g_ptr_array_index(merged, 1)),
                    ==, "/home/me/other");
}

/*
 * The merge copies, and holds nothing belonging to its inputs.
 *
 * Both arrays own their elements, so returning borrowed pointers would
 * free each mount twice the moment either side went out of scope --
 * which is exactly the g_ptr_array_copy trap this project recorded
 * against the fstab renderer.
 */
static void
test_merging_folders_copies_rather_than_borrows(void)
{
    g_autoptr(GPtrArray) merged = NULL;

    {
        g_autoptr(GPtrArray) defaults = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_mount_free);
        g_autoptr(GPtrArray) own = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_mount_free);

        g_ptr_array_add(defaults, clawt_mount_new("/srv/a", "/work/a"));
        g_ptr_array_add(own, clawt_mount_new("/srv/b", "/work/b"));

        merged = clawt_mount_merge_defaults(defaults, own);
    }

    /* Both inputs are gone; the result is still readable. */
    g_assert_cmpuint(merged->len, ==, 2);
    g_assert_cmpstr(clawt_mount_get_source(g_ptr_array_index(merged, 0)),
                    ==, "/srv/a");
    g_assert_cmpstr(clawt_mount_get_source(g_ptr_array_index(merged, 1)),
                    ==, "/srv/b");
}

/*
 * Nothing configured is nothing applied, and neither array being there
 * is not a crash.
 */
static void
test_merging_folders_handles_nothing(void)
{
    g_autoptr(GPtrArray) empty = clawt_mount_merge_defaults(NULL, NULL);

    g_assert_cmpuint(empty->len, ==, 0);
}

/*
 * Who a shared folder reaches, over every combination.
 *
 * This is the rule integrations and shared folders now share. They were
 * going to be two implementations, and two would have differed exactly
 * once -- on an agent named directly *and* on a listed team, which is
 * the row nobody writes a test for.
 */
static void
test_scope_covers_agents_and_teams(void)
{
    static const gchar *const agents[] = { "scribe", NULL };
    static const gchar *const teams[] = { "backend", NULL };
    static const struct {
        ClawtScope           scope;
        const gchar *const  *agents;
        const gchar *const  *teams;
        const gchar         *agent;
        const gchar         *team;
        gboolean             covered;
    } cases[] = {
        /* all reaches everybody, named or not, teamed or not. */
        { CLAWT_SCOPE_ALL, NULL, NULL, "anyone", NULL, TRUE },
        { CLAWT_SCOPE_ALL, agents, teams, "anyone", "frontend", TRUE },

        /* none reaches nobody, however they are named. */
        { CLAWT_SCOPE_NONE, agents, teams, "scribe", "backend", FALSE },

        /* selected: by id, by team, by either, by neither. */
        { CLAWT_SCOPE_SELECTED, agents, NULL, "scribe", NULL, TRUE },
        { CLAWT_SCOPE_SELECTED, agents, NULL, "other", NULL, FALSE },
        { CLAWT_SCOPE_SELECTED, NULL, teams, "other", "backend", TRUE },
        { CLAWT_SCOPE_SELECTED, NULL, teams, "other", "frontend", FALSE },

        /*
         * Named directly *and* on a listed team. The row two
         * implementations would have disagreed about.
         */
        { CLAWT_SCOPE_SELECTED, agents, teams, "scribe", "backend", TRUE },

        /* Named directly while on an unlisted team still counts. */
        { CLAWT_SCOPE_SELECTED, agents, teams, "scribe", "frontend", TRUE },

        /*
         * A teamless agent matches no team entry. Both spellings of
         * absent -- no key at all, and the empty string an agent taken
         * off a team gets -- have to miss, which is a distinction this
         * tree has already paid for once in both clients' sidebars.
         */
        { CLAWT_SCOPE_SELECTED, NULL, teams, "other", NULL, FALSE },
        { CLAWT_SCOPE_SELECTED, NULL, teams, "other", "", FALSE },

        /* selected with no lists is nobody, not everybody. */
        { CLAWT_SCOPE_SELECTED, NULL, NULL, "scribe", "backend", FALSE }
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmpint(clawt_scope_covers(cases[i].scope, cases[i].agents,
                                           cases[i].teams, cases[i].agent,
                                           cases[i].team),
                        ==, cases[i].covered);
    }

    /* No agent at all is never covered, even by `all`. */
    g_assert_false(clawt_scope_covers(CLAWT_SCOPE_ALL, NULL, NULL, NULL,
                                      NULL));
}

/*
 * A folder scoped to a team reaches that team and nobody else, through
 * the factory.
 *
 * The rule is tested above; this is the wire. A scope that parsed
 * correctly and was never consulted when the computer was built would
 * be a setting reported as saved and applied to everybody.
 */
static void
test_a_team_scoped_folder_reaches_only_that_team(void)
{
    static const struct {
        const gchar *agent;
        const gchar *team_line;
        gboolean     gets_it;
    } cases[] = {
        { "one",   "    team: backend\n",  TRUE },
        { "two",   "    team: frontend\n", FALSE },
        { "three", "",                     FALSE }
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autoptr(ClawtConfig) config = NULL;
        g_autoptr(ClawtComputer) computer = NULL;
        g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
        g_autoptr(GPtrArray) defaults = NULL;
        g_autoptr(GError) error = NULL;
        g_autofree gchar *yaml = NULL;
        ClawtAgentConfig *agent;

        yaml = g_strdup_printf(
            "defaults:\n"
            "  mounts:\n"
            "    - source: /tmp\n"
            "      target: /work/backend\n"
            "      required: false\n"
            "      teams: [backend]\n"
            "agents:\n"
            "  - id: %s\n"
            "%s"
            "    computer:\n"
            "      type: container\n",
            cases[i].agent, cases[i].team_line);

        config = clawt_config_load_from_string(yaml, &error);
        g_assert_no_error(error);
        agent = clawt_config_get_agent(config, cases[i].agent);
        defaults = clawt_config_get_default_mounts(config);

        /*
         * Naming a team without saying `scope` means selected. Writing
         * a list and having it ignored because the scope defaulted to
         * ALL would be a rule that reads correctly and does the
         * opposite -- so that is asserted here rather than assumed.
         */
        g_assert_cmpint(clawt_mount_get_scope(g_ptr_array_index(defaults, 0)),
                        ==, CLAWT_SCOPE_SELECTED);

        computer = clawt_computer_factory_create(agent, defaults, bridge,
                                                 &error);
        g_assert_no_error(error);
        g_assert_cmpint(has_target(clawt_computer_get_mounts(computer),
                                   "/work/backend"),
                        ==, cases[i].gets_it);
    }
}

/*
 * A scope that was written and cannot be read reaches nobody.
 *
 * The same asymmetry integrations record: a typo that hands somebody's
 * home directory to the whole fleet is far worse than one that hands it
 * to nothing and says so. Absent is still everybody -- a list under
 * `defaults` should mean what the word says.
 */
static void
test_an_unreadable_scope_reaches_nobody(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) defaults = NULL;
    g_autoptr(GError) error = NULL;

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*is not a scope; reaching nobody*");

    config = clawt_config_load_from_string(
        "defaults:\n"
        "  mounts:\n"
        "    - source: /tmp\n"
        "      target: /work/typo\n"
        "      scope: everyone\n"
        "    - source: /tmp\n"
        "      target: /work/silent\n", &error);
    g_assert_no_error(error);

    defaults = clawt_config_get_default_mounts(config);
    g_assert_cmpuint(defaults->len, ==, 2);

    g_test_assert_expected_messages();

    /* The typo reaches nobody... */
    g_assert_false(clawt_mount_covers(g_ptr_array_index(defaults, 0),
                                      "scribe", "backend"));

    /* ...and saying nothing still reaches everybody. */
    g_assert_true(clawt_mount_covers(g_ptr_array_index(defaults, 1),
                                     "scribe", NULL));
}

/*
 * A scope survives being written and read back.
 *
 * Only when it is not `all`, because writing `scope: all` into every
 * entry would be noise in a file people read -- so the round trip has
 * to prove the *absence* is read as ALL rather than as something else.
 */
static void
test_a_scope_round_trips_through_the_file(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *data = NULL;
    g_autoptr(ClawtConfig) back = NULL;
    g_autoptr(GPtrArray) mounts = NULL;
    ClawtMount *scoped;

    config = clawt_config_load_from_string("defaults: {}\n", &error);
    g_assert_no_error(error);

    scoped = clawt_mount_new("/srv/product", "/work/product");
    clawt_mount_set_scope(scoped, CLAWT_SCOPE_SELECTED);
    {
        static const gchar *const teams[] = { "backend", "platform", NULL };

        clawt_mount_set_teams(scoped, teams);
    }
    g_assert_true(clawt_config_add_default_mount(config, scoped));
    clawt_mount_free(scoped);

    g_assert_true(clawt_config_add_default_mount(
        config, (scoped = clawt_mount_new("/srv/all", "/work/all"))));
    clawt_mount_free(scoped);

    data = clawt_config_to_string(config);
    back = clawt_config_load_from_string(data, &error);
    g_assert_no_error(error);

    mounts = clawt_config_get_default_mounts(back);
    g_assert_cmpuint(mounts->len, ==, 2);

    g_assert_cmpint(clawt_mount_get_scope(g_ptr_array_index(mounts, 0)),
                    ==, CLAWT_SCOPE_SELECTED);
    g_assert_cmpstr(clawt_mount_get_teams(g_ptr_array_index(mounts, 0))[1],
                    ==, "platform");

    /* No scope written, and read back as everybody. */
    g_assert_null(strstr(data, "scope: all"));
    g_assert_cmpint(clawt_mount_get_scope(g_ptr_array_index(mounts, 1)),
                    ==, CLAWT_SCOPE_ALL);
}

/* ── distrobox ────────────────────────────────────────────────────── */

/*
 * The one decision that matters, asserted without distrobox installed.
 *
 * distrobox gives a box the invoking user's real home directory when
 * told nothing, and clawtilla's default is the opposite -- so this is
 * where an agent either has a home of its own or has the operator's ssh
 * keys. The two look identical from inside the box, which is exactly
 * why it should not be checked by running it and looking.
 */
static void
test_a_distrobox_does_not_share_a_home_by_accident(void)
{
    g_autoptr(ClawtComputer) box =
        clawt_distrobox_computer_new("scribe", NULL, NULL);
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(box);

    /* Untouched: a directory of its own, never the operator's. */
    {
        g_autofree gchar *home = clawt_distrobox_computer_resolve_home(self);

        g_assert_nonnull(home);
        g_assert_true(g_str_has_suffix(home, "/scribe/home"));
    }

    /* Named explicitly. */
    clawt_distrobox_computer_set_home(self, "/var/lib/boxes/scribe");

    {
        g_autofree gchar *home = clawt_distrobox_computer_resolve_home(self);

        g_assert_cmpstr(home, ==, "/var/lib/boxes/scribe");
    }

    /*
     * And the operator's own, which is reached only by asking. NULL is
     * "pass no --home", which is what makes distrobox use theirs.
     */
    clawt_distrobox_computer_set_share_home(self, TRUE);
    g_assert_null(clawt_distrobox_computer_resolve_home(self));

    /* share_home wins over an explicit home, rather than the two fighting. */
    clawt_distrobox_computer_set_home(self, "/var/lib/boxes/scribe");
    g_assert_null(clawt_distrobox_computer_resolve_home(self));
}

/*
 * An agent is told which of the two it got, in words.
 *
 * The describe text is the only place an agent learns this, and getting
 * it backwards is backwards in the dangerous direction -- the same
 * failure the guest desktop had, where an agent was told it was driving
 * the operator's real screen when it was not.
 */
static void
test_a_distrobox_says_whose_home_it_has(void)
{
    g_autoptr(ClawtComputer) box =
        clawt_distrobox_computer_new("scribe", NULL, NULL);
    ClawtDistroboxComputer *self = CLAWT_DISTROBOX_COMPUTER(box);

    {
        g_autofree gchar *said = clawt_computer_describe(box);

        g_assert_nonnull(strstr(said, "belongs to you"));
        g_assert_null(strstr(said, "operator's real home"));
    }

    clawt_distrobox_computer_set_share_home(self, TRUE);

    {
        g_autofree gchar *said = clawt_computer_describe(box);

        g_assert_nonnull(strstr(said, "operator's real home"));
        g_assert_null(strstr(said, "belongs to you"));
    }
}

/*
 * Mounts become --volume arguments with their mode and SELinux label
 * intact.
 *
 * A read-only mount silently arriving read-write is not noticed until
 * an agent overwrites something, and the relabel flag matters on
 * Silverblue, where an unlabelled bind is unreadable inside the box.
 */
static void
test_distrobox_volumes_keep_their_mode_and_label(void)
{
    g_autoptr(GPtrArray) mounts =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);
    g_autofree gchar *args = NULL;
    g_autofree gchar *none = NULL;
    ClawtMount *ro;
    ClawtMount *rw;

    ro = clawt_mount_new("/etc/passwd", "/mnt/passwd");
    clawt_mount_set_mode(ro, CLAWT_MOUNT_MODE_RO);
    g_ptr_array_add(mounts, ro);

    rw = clawt_mount_new("/tmp", "/mnt/scratch");
    clawt_mount_set_mode(rw, CLAWT_MOUNT_MODE_RW);
    clawt_mount_set_relabel(rw, CLAWT_RELABEL_SHARED);
    g_ptr_array_add(mounts, rw);

    args = clawt_distrobox_computer_build_volume_args(mounts);

    g_assert_nonnull(strstr(args, "/etc/passwd:/mnt/passwd:ro"));
    g_assert_nonnull(strstr(args, "/tmp:/mnt/scratch:z"));

    /* No mounts is no arguments, not a stray flag with nothing after it. */
    none = clawt_distrobox_computer_build_volume_args(NULL);
    g_assert_cmpstr(none, ==, "");
}

/*
 * Tearing down a VM removes the guest's disk, not just its definition.
 *
 * Keeping it would leave an agent's whole machine behind under a name
 * nothing refers to, and a later agent with the same id would silently
 * adopt it -- which is the confusion teardown is called to end.
 */
static void
test_vm_teardown_removes_the_disk(void)
{
    g_autoptr(ClawtComputer) vm = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *overlay = NULL;
    g_autofree gchar *key = NULL;
    g_autoptr(GError) error = NULL;

    vm = clawt_vm_computer_new("teardownee", CLAWT_VM_BACKEND_QEMU, NULL);

    state_dir = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                                 "teardownee", NULL);
    g_assert_true(clawt_ensure_dir(state_dir, 0700, &error));
    g_assert_no_error(error);

    overlay = g_build_filename(state_dir, "overlay.qcow2", NULL);
    key = g_build_filename(state_dir, "id_ed25519", NULL);

    g_assert_true(g_file_set_contents(overlay, "not really a disk", -1,
                                      NULL));
    g_assert_true(g_file_set_contents(key, "not really a key", -1, NULL));

    g_assert_true(clawt_computer_teardown(vm, &error));
    g_assert_no_error(error);

    g_assert_false(g_file_test(overlay, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(key, G_FILE_TEST_EXISTS));

    /* And the directory itself, once there is nothing left in it. */
    g_assert_false(g_file_test(state_dir, G_FILE_TEST_EXISTS));
}

/*
 * ...but anything that was not clawtilla's is left alone.  g_rmdir only
 * removes an empty directory, so a file somebody put there by hand
 * survives rather than being swept up with the disk.
 */
static void
test_vm_teardown_leaves_what_it_did_not_make(void)
{
    g_autoptr(ClawtComputer) vm = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *overlay = NULL;
    g_autofree gchar *theirs = NULL;
    g_autoptr(GError) error = NULL;

    vm = clawt_vm_computer_new("keepsomething", CLAWT_VM_BACKEND_QEMU, NULL);

    state_dir = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                                 "keepsomething", NULL);
    g_assert_true(clawt_ensure_dir(state_dir, 0700, &error));

    overlay = g_build_filename(state_dir, "overlay.qcow2", NULL);
    theirs = g_build_filename(state_dir, "notes.txt", NULL);

    g_assert_true(g_file_set_contents(overlay, "disk", -1, NULL));
    g_assert_true(g_file_set_contents(theirs, "mine", -1, NULL));

    g_assert_true(clawt_computer_teardown(vm, &error));
    g_assert_no_error(error);

    g_assert_false(g_file_test(overlay, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(theirs, G_FILE_TEST_EXISTS));
}

/*
 * A guest with a desktop gets a GPU, and one without does not.
 *
 * The absence of this was invisible for a long time: the qemu backend
 * gets a VGA adapter from qemu by default, so the feature worked on the
 * backend it was tested on. On libvirt the guest had no /dev/dri, GDM
 * reported no primary GPU and started nothing, and every layer above --
 * cloud-init, gdm.service, the default target -- looked healthy.
 */
static void
test_a_guest_desktop_gets_a_screen(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autoptr(ClawtGuestDesktop) desktop = clawt_guest_desktop_new("clawt");
    g_autofree gchar *headless = NULL;
    g_autofree gchar *graphical = NULL;

    headless = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    /*
     * Even without a desktop.  A guest with no display device offers only
     * a serial console, which is the boot log for ever -- so somebody
     * looking at it cannot tell a broken guest from a working headless
     * one. A framebuffer nobody reads costs a few megabytes.
     */
    g_assert_nonnull(strstr(headless, "<video>"));
    g_assert_nonnull(strstr(headless, "<graphics"));

    clawt_vm_computer_set_desktop(CLAWT_VM_COMPUTER(computer), desktop);
    graphical =
        clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_nonnull(strstr(graphical, "<video>"));
    g_assert_nonnull(strstr(graphical, "type='virtio'"));

    /*
     * And a viewer, on loopback.  A VM's screen is a live view of what
     * the agent is doing; it does not go on the network because a default
     * was convenient.
     */
    g_assert_nonnull(strstr(graphical, "type='vnc'"));
    g_assert_nonnull(strstr(graphical, "listen='127.0.0.1'"));
}

/*
 * ...and the qemu backend says the same thing rather than inheriting it.
 */
static void
test_the_qemu_backend_names_its_gpu(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtGuestDesktop) desktop = clawt_guest_desktop_new("clawt");
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;

    (void)desktop;

    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(computer),
                                             "/tmp/qmp.sock");
    joined = g_strjoinv(" ", argv);

    g_assert_nonnull(strstr(joined, "virtio-gpu-pci"));

    /*
     * -vga none as well, or the guest gets two GPUs: qemu's default VGA
     * plus the one asked for.
     */
    g_assert_nonnull(strstr(joined, "-vga none"));
}

/*
 * Provisioning leaves the computer knowing which key reaches the guest.
 *
 * That knowledge used to live only inside the seed builder, and
 * provisioning skips seed-building entirely for a domain that is already
 * running -- correctly, since rebuilding a running guest's disk destroys
 * it. So a daemon restarted against a live VM held no key path, built an
 * ssh command with no -i at all, and every exec came back "Permission
 * denied (publickey)" for a key sitting in the state directory that
 * worked perfectly by hand.
 *
 * This covers the qemu backend, which is the one reachable without a
 * hypervisor. The libvirt early return that actually broke is verified
 * against a real domain.
 */
static void
test_provisioning_resolves_the_ssh_key(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autofree gchar *image = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *command[] = { "true", NULL };
    g_autofree gchar *qemu_img = g_find_program_in_path("qemu-img");

    if (qemu_img == NULL) {
        g_test_skip("qemu-img is not installed");
        return;
    }

    if (clawt_cloud_init_find_tool() == NULL) {
        g_test_skip("no ISO writer is installed");
        return;
    }

    image = g_build_filename(g_get_user_data_dir(), "pretend-base.qcow2",
                             NULL);
    {
        g_autoptr(GSubprocess) mk =
            g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                             G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                             "qemu-img", "create", "-f", "qcow2", image,
                             "1M", NULL);

        g_assert_nonnull(mk);
        g_assert_true(g_subprocess_wait_check(mk, NULL, NULL));
    }

    computer = clawt_vm_computer_new("keyed", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_vm_computer_set_image(CLAWT_VM_COMPUTER(computer), image);

    g_assert_true(clawt_computer_provision(computer, &error));
    g_assert_no_error(error);

    argv = clawt_vm_computer_build_ssh_argv(CLAWT_VM_COMPUTER(computer),
                                            command, NULL, 10);
    g_assert_nonnull(argv);
    joined = g_strjoinv(" ", argv);

    /*
     * The whole assertion. Without -i, ssh falls back to whatever keys
     * the daemon's user happens to have, which is never the one
     * cloud-init put in the guest.
     */
    g_assert_nonnull(strstr(joined, " -i "));
    g_assert_nonnull(strstr(joined, "id_ed25519"));
}


/*
 * A guest deleted behind clawtilla's back is built again.
 *
 * The state a computer carries is a belief about something another
 * program owns: a libvirt domain can be deleted in virt-manager and a
 * qcow2 with rm, neither of which tells clawtilla anything.  vm_start()
 * used to provision only when it remembered the machine as ABSENT --
 * and a stop leaves STOPPED -- so an agent restarted against a deleted
 * guest skipped provisioning and then asked for a machine that was not
 * there.
 *
 * Restarting the whole daemon happened to recover, because a freshly
 * built computer starts ABSENT.  Restarting just the agent did not,
 * which is a difference nobody should have to know about.
 *
 * The evidence here is the refusal: with no image set, provisioning
 * declines and names the setting.  Reaching that message at all means
 * provisioning was attempted, which before this it was not.
 */
static void
test_a_guest_that_vanished_is_built_again(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(GError) error = NULL;

    /* Exactly what a stop leaves behind. */
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    g_assert_false(clawt_computer_start(computer, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1, "computer.vm.image"));
}


/*
 * A share has two paths and the agent needs both.
 *
 * `read`, `write` and `bash` run on the host; only
 * clawtilla_computer_exec goes inside. This named only the path inside,
 * which is the one that is no use to the tools an agent reaches for
 * first -- so two of them looked for a shared file at the guest's path,
 * on the host, found nothing, and reported the share as never set up.
 */
static void
test_a_share_is_described_by_both_of_its_paths(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autofree gchar *described = NULL;

    computer = clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_LIBVIRT,
                                     NULL);

    mount = clawt_mount_new("/host/agents/scribe",
                            CLAWT_WORKSPACE_MOUNT_POINT);
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
    clawt_computer_add_mount(computer, mount);

    described = clawt_computer_describe(computer);

    g_assert_nonnull(strstr(described, "/host/agents/scribe"));
    g_assert_nonnull(strstr(described, CLAWT_WORKSPACE_MOUNT_POINT));

    /* And which of the two goes with which tool. */
    g_assert_nonnull(strstr(described, "run on the host"));
}

/*
 * A computer with nothing shared says so, rather than leaving an agent
 * to discover it by writing a file nobody can then find.
 */
static void
test_a_computer_with_no_shares_says_so(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *described = clawt_computer_describe(computer);

    g_assert_nonnull(strstr(described, "No host directories are shared"));
}


/*
 * Every computer gets the agent's own workspace.
 *
 * It holds the persona, the notes and MEMORY.md, and it used to live
 * only on the host -- so an agent read the files describing it with
 * tools that run out here and worked in a machine that could not see
 * them, and anything it wrote inside went somewhere the host never
 * looked. A VM agent's screenshots were the case that exposed it: the
 * capture was perfect and the file unreachable, which is
 * indistinguishable from a capture that failed.
 */
static void
test_a_computer_is_given_the_agents_workspace(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    GPtrArray *mounts;
    gboolean saw_workspace = FALSE;
    guint i;

    agent = agent_from_yaml(&config,
                            "agents:\n"
                            "  - id: chief\n"
                            "    computer:\n"
                            "      type: host\n"
                            "      host:\n"
                            "        confirm_host_control: true\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(computer);

    mounts = clawt_computer_get_mounts(computer);
    g_assert_nonnull(mounts);

    for (i = 0; i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (g_strcmp0(clawt_mount_get_target(mount),
                      CLAWT_WORKSPACE_MOUNT_POINT) == 0) {
            saw_workspace = TRUE;

            /* Read-write: the point is that it is the same file. */
            g_assert_cmpint(clawt_mount_get_mode(mount), ==,
                            CLAWT_MOUNT_MODE_RW);
        }
    }

    g_assert_true(saw_workspace);
}

/*
 * A VM gets it as a virtiofs share rather than a bind mount, without
 * anybody having to write down which spelling their backend wants.
 */
static void
test_a_vm_workspace_share_is_virtiofs(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    GPtrArray *mounts;
    guint i;

    agent = agent_from_yaml(&config,
                            "agents:\n"
                            "  - id: chief\n"
                            "    computer:\n"
                            "      type: vm\n"
                            "      vm:\n"
                            "        image: /tmp/does-not-matter.qcow2\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(computer);

    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (g_strcmp0(clawt_mount_get_target(mount),
                      CLAWT_WORKSPACE_MOUNT_POINT) == 0)
            g_assert_cmpint(clawt_mount_get_mount_type(mount), ==,
                            CLAWT_MOUNT_VIRTIOFS);
    }
}

/* ...and an agent that asked not to have it does not get it. */
static void
test_the_workspace_share_can_be_turned_off(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    GPtrArray *mounts;
    guint i;

    agent = agent_from_yaml(&config,
                            "agents:\n"
                            "  - id: chief\n"
                            "    computer:\n"
                            "      type: host\n"
                            "      workspace: false\n"
                            "      host:\n"
                            "        confirm_host_control: true\n");

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(computer);

    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        g_assert_cmpstr(clawt_mount_get_target(mount), !=,
                        CLAWT_WORKSPACE_MOUNT_POINT);
    }
}


/* ── The name the guest mounts a share by ────────────────────────── */

/*
 * qemu refuses a virtiofs tag over 36 bytes, and refuses the *device*
 * with it: the domain does not start, and the error names a property
 * nobody set by hand.
 *
 * The tag used to be the target path.
 * `/mnt/clawtilla/exchange/ubuntu-tester` is 37 bytes, so the first VM
 * agent whose id reached thirteen characters would not boot -- and
 * deb-tester (34) and arch-tester (35) both fitted, which is why it took
 * a third name to surface.
 */
static void
test_a_tag_fits_in_what_qemu_accepts(void)
{
    static const gchar *const targets[] = {
        "/mnt/clawtilla/workspace",
        "/mnt/clawtilla/exchange",
        "/mnt/clawtilla/exchange/shared",
        "/mnt/clawtilla/exchange/ubuntu-tester",
        "/mnt/clawtilla/exchange/an-agent-with-a-really-long-name-indeed",
        "/work/projects",
        "/"
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(targets); i++) {
        g_autofree gchar *tag = clawt_mount_tag(targets[i]);

        g_assert_nonnull(tag);
        g_assert_cmpuint(strlen(tag), >, 0);
        g_assert_cmpuint(strlen(tag), <=, CLAWT_MOUNT_TAG_MAX);
    }
}

/*
 * Stable for ever, because it is written into the guest's fstab at first
 * boot and into the domain XML on every provision.  A tag that moved
 * would leave the guest mounting a device that is not there any more --
 * and `nofail` makes that silent.
 */
static void
test_a_tag_is_the_same_every_time(void)
{
    g_autofree gchar *once = clawt_mount_tag("/mnt/clawtilla/workspace");
    g_autofree gchar *again = clawt_mount_tag("/mnt/clawtilla/workspace");

    g_assert_cmpstr(once, ==, again);
}

/*
 * And two targets never share one.  The readable part is a lossy
 * transformation -- `/a/b` and `/a-b` reduce to the same letters -- so
 * the hash is always there rather than only when the name had to be cut.
 */
static void
test_two_targets_do_not_share_a_tag(void)
{
    g_autofree gchar *nested = clawt_mount_tag("/mnt/clawtilla/exchange/x");
    g_autofree gchar *dashed = clawt_mount_tag("/mnt/clawtilla/exchange-x");
    g_autofree gchar *long_a = clawt_mount_tag(
        "/mnt/clawtilla/exchange/an-agent-with-a-really-long-name-alpha");
    g_autofree gchar *long_b = clawt_mount_tag(
        "/mnt/clawtilla/exchange/an-agent-with-a-really-long-name-beta");

    g_assert_cmpstr(nested, !=, dashed);

    /* Even where the readable half was truncated to the same prefix. */
    g_assert_cmpstr(long_a, !=, long_b);
}

/*
 * The domain and the seed have to name the same tag.  Two spellings of
 * this would differ exactly once, and the share would simply not be
 * there -- with `nofail` keeping the guest quiet about it.
 */
static void
test_the_domain_and_the_seed_agree_on_the_tag(void)
{
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autofree gchar *xml = NULL;
    g_autofree gchar *seed = NULL;
    g_autofree gchar *tag = NULL;
    const gchar *target = "/mnt/clawtilla/exchange/ubuntu-tester";

    computer = clawt_vm_computer_new("ubuntu-tester",
                                     CLAWT_VM_BACKEND_LIBVIRT, NULL);

    mount = clawt_mount_new("/host/exchange/ubuntu-tester", target);
    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);
    clawt_computer_add_mount(computer, mount);

    tag = clawt_mount_tag(target);
    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));
    seed = clawt_cloud_init_build_user_data_full(
        "root", NULL, NULL, NULL, clawt_computer_get_mounts(computer));

    g_assert_nonnull(strstr(xml, tag));
    g_assert_nonnull(strstr(seed, tag));

    /* And neither carries the path where the tag belongs. */
    g_assert_null(strstr(xml, "<target dir='/mnt"));
}


/* ── The size of the screen ──────────────────────────────────────── */

/*
 * A screen size a person typed, read before anything can act on it.
 *
 * The hypervisor reports a malformed one as a domain that will not
 * define, which is an error about XML a long way from the line that
 * caused it.
 */
static void
test_a_resolution_is_read_or_refused(void)
{
    static const gchar *const good[] = {
        "1280x800", "1920x1080", "3840x2160", "800X600", "1024*768"
    };
    static const gchar *const bad[] = {
        NULL, "", "1920", "1920x", "x1080", "1920x1080x60", "-1920x1080",
        "1920 x 1080", "widthxheight", "320x240", "99999x99999", "0x0"
    };
    guint width = 0;
    guint height = 0;
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(good); i++)
        g_assert_true(clawt_vm_computer_parse_resolution(good[i], NULL,
                                                         NULL));

    for (i = 0; i < G_N_ELEMENTS(bad); i++)
        g_assert_false(clawt_vm_computer_parse_resolution(bad[i], NULL,
                                                          NULL));

    g_assert_true(clawt_vm_computer_parse_resolution("1920x1080", &width,
                                                     &height));
    g_assert_cmpuint(width, ==, 1920);
    g_assert_cmpuint(height, ==, 1080);
}

/*
 * And it reaches both backends, because a feature that works on one of
 * them is how the missing GPU stayed hidden for as long as it did.
 */
static void
test_the_resolution_reaches_both_backends(void)
{
    g_autoptr(ClawtComputer) libvirt = NULL;
    g_autoptr(ClawtComputer) qemu = NULL;
    g_autofree gchar *xml = NULL;
    g_auto(GStrv) argv = NULL;
    gboolean saw = FALSE;
    guint i;

    libvirt = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    clawt_vm_computer_set_resolution(CLAWT_VM_COMPUTER(libvirt),
                                     "1920x1080");
    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(libvirt));

    g_assert_nonnull(strstr(xml, "<resolution x='1920' y='1080'/>"));

    qemu = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    clawt_vm_computer_set_resolution(CLAWT_VM_COMPUTER(qemu), "1920x1080");
    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(qemu),
                                             "/tmp/clawt-qmp.sock");

    for (i = 0; argv[i] != NULL; i++) {
        if (strstr(argv[i], "virtio-gpu-pci") != NULL) {
            g_assert_nonnull(strstr(argv[i], "xres=1920"));
            g_assert_nonnull(strstr(argv[i], "yres=1080"));
            saw = TRUE;
        }
    }

    g_assert_true(saw);
}

/*
 * A size that will not parse leaves the hypervisor's own default rather
 * than putting nonsense in a domain.  It is refused at validation, so
 * arriving here with one means somebody went around that -- and a VM
 * with a screen beats no VM at all.
 */
static void
test_an_unreadable_resolution_leaves_the_default(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    g_autofree gchar *xml = NULL;

    clawt_vm_computer_set_resolution(CLAWT_VM_COMPUTER(computer), "huge");
    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(computer));

    g_assert_null(strstr(xml, "<resolution"));
    g_assert_nonnull(strstr(xml, "<model type='virtio'"));
}

/* ...and the config refuses it while somebody is still looking at it. */
static void
test_a_bad_resolution_is_refused_at_validation(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    agent = agent_from_yaml(&config,
                            "agents:\n"
                            "  - id: chief\n"
                            "    computer:\n"
                            "      type: vm\n"
                            "      vm:\n"
                            "        image: /tmp/x.qcow2\n"
                            "        resolution: \"1920\"\n");

    g_assert_false(clawt_agent_config_validate_computer(agent, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "WIDTHxHEIGHT"));
}


/*
 * A guest gets the host's CPU, because naming none is not neutral.
 *
 * libvirt fills in a default when the domain says nothing, and that
 * default is `qemu64` -- x86-64-v1, from 2003. Four of the five
 * distributions in the catalog boot on it happily. CentOS Stream 10 does
 * not: its own console says `Fatal glibc error: CPU does not support
 * x86-64-v2` and the kernel panics killing init.
 *
 * From outside that is a VM which is definitely running, a console
 * reading "Display output is not active", and ssh answering
 * `kex_exchange_identification: Connection reset by peer` -- the same
 * three symptoms as a VM with no disk at all, and nothing anywhere
 * naming a CPU.
 */
static void
test_a_guest_is_given_a_cpu_worth_having(void)
{
    g_autoptr(ClawtComputer) libvirt = NULL;
    g_autoptr(ClawtComputer) qemu = NULL;
    g_autofree gchar *xml = NULL;
    g_auto(GStrv) argv = NULL;
    gboolean saw_cpu = FALSE;
    guint i;

    libvirt = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_LIBVIRT, NULL);
    xml = clawt_vm_computer_build_domain_xml(CLAWT_VM_COMPUTER(libvirt));

    g_assert_nonnull(strstr(xml, "mode='host-passthrough'"));

    /*
     * And never a named model. Picking one is picking a baseline to be
     * wrong about again the next time a distribution raises theirs.
     */
    g_assert_null(strstr(xml, "qemu64"));

    qemu = clawt_vm_computer_new("chief", CLAWT_VM_BACKEND_QEMU, NULL);
    argv = clawt_vm_computer_build_qemu_argv(CLAWT_VM_COMPUTER(qemu),
                                             "/tmp/clawt-qmp.sock");

    for (i = 0; argv[i] != NULL; i++) {
        if (g_strcmp0(argv[i], "-cpu") == 0) {
            /*
             * `max` rather than `host`: the machine line falls back to
             * TCG, where `-cpu host` is not valid at all.
             */
            g_assert_cmpstr(argv[i + 1], ==, "max");
            saw_cpu = TRUE;
        }
    }

    g_assert_true(saw_cpu);
}

int
main(int argc, char *argv[])
{
    g_autofree gchar *data_dir = g_dir_make_tmp("clawt-data-XXXXXX", NULL);
    gint status;
    /*
     * Before anything can ask for it: GLib caches the data directory on
     * first use, and the VM tests write overlays and seeds under it.
     * Left alone they would land in the real ~/.local/share -- a test
     * suite quietly filling the user's home with disk images.
     */
    g_setenv("XDG_DATA_HOME", data_dir, TRUE);

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

    g_test_add_func("/computer/cpu/host-passthrough",
                    test_a_guest_is_given_a_cpu_worth_having);
    g_test_add_func("/computer/resolution/read-or-refused",
                    test_a_resolution_is_read_or_refused);
    g_test_add_func("/computer/resolution/reaches-both-backends",
                    test_the_resolution_reaches_both_backends);
    g_test_add_func("/computer/resolution/unreadable-keeps-the-default",
                    test_an_unreadable_resolution_leaves_the_default);
    g_test_add_func("/computer/resolution/refused-at-validation",
                    test_a_bad_resolution_is_refused_at_validation);
    g_test_add_func("/computer/tag/fits-what-qemu-accepts",
                    test_a_tag_fits_in_what_qemu_accepts);
    g_test_add_func("/computer/tag/stable",
                    test_a_tag_is_the_same_every_time);
    g_test_add_func("/computer/tag/distinct",
                    test_two_targets_do_not_share_a_tag);
    g_test_add_func("/computer/tag/domain-and-seed-agree",
                    test_the_domain_and_the_seed_agree_on_the_tag);
    g_test_add_func("/computer/mounts/workspace-is-shared",
                    test_a_computer_is_given_the_agents_workspace);
    g_test_add_func("/computer/mounts/vm-workspace-is-virtiofs",
                    test_a_vm_workspace_share_is_virtiofs);
    g_test_add_func("/computer/mounts/workspace-can-be-off",
                    test_the_workspace_share_can_be_turned_off);
    g_test_add_func("/computer/mounts/described-by-both-paths",
                    test_a_share_is_described_by_both_of_its_paths);
    g_test_add_func("/computer/mounts/none-says-so",
                    test_a_computer_with_no_shares_says_so);
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
    g_test_add_func("/computer/vm/uuid-stable",
                    test_vm_domain_xml_uuid_is_stable);
    g_test_add_func("/computer/vm/uuid-adopted",
                    test_vm_domain_xml_keeps_an_adopted_uuid);
    g_test_add_func("/computer/vm/no-image-refused",
                    test_vm_without_an_image_is_refused);
    g_test_add_func("/computer/vm/xml-has-a-disk",
                    test_vm_domain_xml_always_has_a_disk_to_boot);
    g_test_add_func("/computer/vm/no-second-guest",
                    test_vm_start_does_not_spawn_a_second_guest);
    g_test_add_func("/computer/vm/overlay-kept-when-unreadable",
                    test_vm_overlay_is_kept_when_its_base_cannot_be_read);
    g_test_add_func("/computer/vm/missing-base-named",
                    test_vm_missing_base_image_is_named);
    g_test_add_func("/computer/vm/overlay-kept",
                    test_vm_overlay_survives_a_second_provision);
    g_test_add_func("/computer/vm/overlay-rebuilt",
                    test_vm_overlay_is_rebuilt_when_the_image_changes);

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
    g_test_add_func("/computer/factory/unconfined-needs-the-fleet-gate",
                    test_factory_refuses_unconfined_without_the_fleet_gate);
    g_test_add_func("/computer/factory/unconfined-with-both-acts-builds",
                    test_factory_builds_an_unconfined_host_when_both_agree);
    g_test_add_func("/computer/factory/mount-type",
                    test_factory_picks_the_mount_type_for_the_backend);
    g_test_add_func("/computer/factory/host-mounts-allowlist",
                    test_factory_treats_host_mounts_as_an_allowlist);
    g_test_add_func("/computer/factory/desktop", test_factory_desktop_is_optional);

    g_test_add_func("/computer/integration/container",
                    test_container_against_real_podman);
    g_test_add_func("/computer/a-call-loads-the-module-it-needs",
                    test_a_call_loads_the_module_it_needs);

    g_test_add_func("/computer/host-mount-target",
                    test_host_reaches_a_mount_by_its_target);
    g_test_add_func("/computer/mount-is-a-grant",
                    test_a_mount_is_a_grant_in_workspace_mode);
    g_test_add_func("/computer/mount-prefix-not-a-match",
                    test_a_mount_prefix_is_not_a_match);
    g_test_add_func("/computer/mounts/forbidden-source-refused-for-a-bind",
                    test_forbidden_source_is_refused_for_a_bind);
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
    g_test_add_func("/computer/vm/provision-resolves-the-key",
                    test_provisioning_resolves_the_ssh_key);
    g_test_add_func("/computer/vm/desktop-gets-a-screen",
                    test_a_guest_desktop_gets_a_screen);
    g_test_add_func("/computer/vm/qemu-names-its-gpu",
                    test_the_qemu_backend_names_its_gpu);
    g_test_add_func("/scope/covers-agents-and-teams",
                    test_scope_covers_agents_and_teams);
    g_test_add_func("/computer/folders/team-scope-reaches-that-team",
                    test_a_team_scoped_folder_reaches_only_that_team);
    g_test_add_func("/computer/folders/unreadable-scope-reaches-nobody",
                    test_an_unreadable_scope_reaches_nobody);
    g_test_add_func("/computer/folders/scope-round-trips",
                    test_a_scope_round_trips_through_the_file);
    g_test_add_func("/computer/folders/default-reaches-a-container",
                    test_a_default_mount_reaches_a_container);
    g_test_add_func("/computer/folders/default-does-not-widen-a-host",
                    test_a_default_mount_does_not_widen_a_host_agent);
    g_test_add_func("/computer/folders/an-agent-can-decline",
                    test_an_agent_can_decline_the_fleet_folders);
    g_test_add_func("/computer/folders/own-replaces-the-default",
                    test_an_agents_own_folder_replaces_the_default);
    g_test_add_func("/computer/folders/merge-copies",
                    test_merging_folders_copies_rather_than_borrows);
    g_test_add_func("/computer/folders/merge-handles-nothing",
                    test_merging_folders_handles_nothing);
    g_test_add_func("/computer/factory-wires-a-distrobox",
                    test_the_factory_wires_a_distrobox_from_config);
    g_test_add_func("/computer/distrobox-without-a-bridge-names-it",
                    test_a_distrobox_without_a_bridge_names_the_module);
    g_test_add_func("/computer/share-home-reaches-the-computer",
                    test_share_home_reaches_the_computer_from_config);
    g_test_add_func("/computer/every-type-is-offered",
                    test_every_computer_type_is_offered_and_round_trips);
    g_test_add_func("/computer/type-predicates-cover-every-type",
                    test_the_type_predicates_cover_every_type);
    g_test_add_func("/computer/distrobox/does-not-share-a-home",
                    test_a_distrobox_does_not_share_a_home_by_accident);
    g_test_add_func("/computer/distrobox/says-whose-home-it-has",
                    test_a_distrobox_says_whose_home_it_has);
    g_test_add_func("/computer/distrobox/volumes-keep-mode-and-label",
                    test_distrobox_volumes_keep_their_mode_and_label);
    g_test_add_func("/computer/teardown/never-a-silent-success",
                    test_teardown_is_never_a_silent_success);
    g_test_add_func("/computer/vm/vanished-guest-is-rebuilt",
                    test_a_guest_that_vanished_is_built_again);
    g_test_add_func("/computer/vm/teardown-removes-the-disk",
                    test_vm_teardown_removes_the_disk);
    g_test_add_func("/computer/vm/teardown-leaves-theirs",
                    test_vm_teardown_leaves_what_it_did_not_make);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
