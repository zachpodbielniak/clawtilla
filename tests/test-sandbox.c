/*
 * test-sandbox.c - What a host command may touch
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One test per escape route, because this is the boundary between "an agent
 * that can help with your source tree" and "an agent that can read your
 * keys".  The last few tests are equally important in the other direction:
 * they pin down what confinement does NOT do, so nobody relies on more than
 * is there.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

typedef struct {
    gchar *root;
    gchar *outside;
    gchar *secret_dir;
} Paths;

static void
paths_setup(Paths *paths)
{
    g_autofree gchar *base = g_dir_make_tmp("clawt-sbx-XXXXXX", NULL);

    paths->root = g_build_filename(base, "workspace", NULL);
    paths->outside = g_build_filename(base, "elsewhere", NULL);
    paths->secret_dir = g_build_filename(paths->root, "secrets", NULL);

    g_mkdir_with_parents(paths->root, 0700);
    g_mkdir_with_parents(paths->outside, 0700);
    g_mkdir_with_parents(paths->secret_dir, 0700);
}

static void
paths_teardown(Paths *paths)
{
    g_autofree gchar *base = g_path_get_dirname(paths->root);

    g_rmdir(paths->secret_dir);
    g_rmdir(paths->root);
    g_rmdir(paths->outside);
    g_rmdir(base);

    g_clear_pointer(&paths->root, g_free);
    g_clear_pointer(&paths->outside, g_free);
    g_clear_pointer(&paths->secret_dir, g_free);
}

static ClawtSandbox *
workspace_sandbox(Paths *paths)
{
    return clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, paths->root);
}

/* Inside the workspace is fine; outside is not. */
static void
test_workspace_confines_to_root(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autofree gchar *inside = NULL;

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    inside = g_build_filename(paths.root, "notes.txt", NULL);

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, inside));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, paths.outside));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));

    paths_teardown(&paths);
}

/* ".." collapses under resolution, so this is not a way out. */
static void
test_dotdot_traversal_is_refused(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autofree gchar *escape = NULL;

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    escape = g_build_filename(paths.root, "..", "elsewhere", "loot", NULL);
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, escape));

    paths_teardown(&paths);
}

/*
 * Nor is a symlink.  This is the one that makes checking the literal string
 * insufficient: the path is inside the workspace and the file is not.
 */
static void
test_symlink_escape_is_refused(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autofree gchar *link_path = NULL;
    g_autofree gchar *through_link = NULL;

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    link_path = g_build_filename(paths.root, "shortcut", NULL);

    if (symlink(paths.outside, link_path) != 0) {
        g_test_skip("symlinks are not available here");
        paths_teardown(&paths);
        return;
    }

    through_link = g_build_filename(link_path, "loot", NULL);
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, through_link));

    g_unlink(link_path);
    paths_teardown(&paths);
}

/*
 * A prefix match is not containment.  "/home/zach/srcevil" begins with
 * "/home/zach/src" and is a different directory entirely.
 */
static void
test_prefix_is_not_containment(void)
{
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, "/tmp/clawt-prefix-root");

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox,
                                                "/tmp/clawt-prefix-root/x"));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox,
                                                 "/tmp/clawt-prefix-rootevil"));
}

/* Denials win over allows, so ~/.ssh stays out even when all of ~ is in. */
static void
test_deny_beats_allow(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autofree gchar *inside_secret = NULL;

    paths_setup(&paths);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_ALLOWLIST, paths.root);
    clawt_sandbox_add_allow_path(sandbox, paths.root);
    clawt_sandbox_add_deny_path(sandbox, paths.secret_dir);

    inside_secret = g_build_filename(paths.secret_dir, "key", NULL);

    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, inside_secret));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, paths.secret_dir));

    paths_teardown(&paths);
}

/* An allowlist widens the workspace, and only to what was listed. */
static void
test_allowlist_widens_but_not_indefinitely(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;

    paths_setup(&paths);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_ALLOWLIST, paths.root);
    clawt_sandbox_add_allow_path(sandbox, paths.outside);

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, paths.outside));
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, paths.root));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));

    paths_teardown(&paths);
}

/* A command naming a forbidden path is refused before it runs. */
static void
test_command_naming_a_forbidden_path_is_refused(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "cat", "/etc/shadow", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    g_assert_nonnull(strstr(error->message, "/etc/shadow"));

    paths_teardown(&paths);
}

/* A path inside an option, as in --file=/etc/shadow, is still a path. */
static void
test_path_inside_an_option_is_checked(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "tool", "--output=/etc/shadow", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));

    paths_teardown(&paths);
}

/* Escalation is refused by name, and by every name. */
static void
test_escalation_is_refused(void)
{
    static const gchar *commands[] = {
        "sudo", "pkexec", "doas", "run0", "machinectl", "su", NULL
    };
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    gsize i;

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    for (i = 0; commands[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;
        const gchar *argv[] = { commands[i], "id", NULL };

        g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    }

    /* An absolute path to the same thing is the same thing. */
    {
        g_autoptr(GError) error = NULL;
        const gchar *argv[] = { "/usr/bin/sudo", "id", NULL };

        g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));
    }

    paths_teardown(&paths);
}

/*
 * And through a shell, or the check would be theatre: refusing `sudo id`
 * while allowing `sh -c 'sudo id'` protects nothing.
 */
static void
test_escalation_through_a_shell_is_refused(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "sh", "-c", "sudo id", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);

    paths_teardown(&paths);
}

/* A forbidden path inside a shell command is caught the same way. */
static void
test_forbidden_path_through_a_shell_is_refused(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "bash", "-c", "cat /etc/shadow", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    g_assert_false(clawt_sandbox_check_argv(sandbox, argv, &error));

    paths_teardown(&paths);
}

/* With allow_sudo on, it is permitted -- the setting has to mean something. */
static void
test_allow_sudo_permits_it(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "sudo", "id", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);
    clawt_sandbox_set_allow_sudo(sandbox, TRUE);

    g_assert_true(clawt_sandbox_check_argv(sandbox, argv, &error));
    g_assert_no_error(error);

    paths_teardown(&paths);
}

/* An ordinary command inside the workspace runs. */
static void
test_ordinary_command_is_permitted(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *inside = NULL;
    const gchar *argv[3];

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    inside = g_build_filename(paths.root, "notes.txt", NULL);
    argv[0] = "cat";
    argv[1] = inside;
    argv[2] = NULL;

    g_assert_true(clawt_sandbox_check_argv(sandbox, argv, &error));
    g_assert_no_error(error);

    paths_teardown(&paths);
}

/* Options that are not paths must not be mistaken for them. */
static void
test_plain_options_are_not_paths(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "ls", "-la", "--color=auto", NULL };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    g_assert_true(clawt_sandbox_check_argv(sandbox, argv, &error));

    paths_teardown(&paths);
}

/* confine: none really means none. */
static void
test_none_permits_everything(void)
{
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new(CLAWT_CONFINE_NONE, NULL);
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "cat", "/etc/shadow", NULL };

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));
    g_assert_true(clawt_sandbox_check_argv(sandbox, argv, &error));

    /* But escalation is still governed by its own setting. */
    {
        const gchar *sudo_argv[] = { "sudo", "id", NULL };
        g_autoptr(GError) sudo_error = NULL;

        g_assert_false(clawt_sandbox_check_argv(sandbox, sudo_argv,
                                                &sudo_error));
    }
}

/*
 * bwrap missing is an error, never a quiet downgrade: an agent the user
 * believes is sandboxed and is not, is the worst outcome available.
 */
static void
test_bwrap_availability_is_checked(void)
{
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new(CLAWT_CONFINE_BWRAP, "/tmp");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *bwrap = g_find_program_in_path("bwrap");

    if (bwrap != NULL) {
        g_assert_true(clawt_sandbox_is_available(sandbox, &error));
        g_assert_no_error(error);
    } else {
        g_assert_false(clawt_sandbox_is_available(sandbox, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
        g_assert_nonnull(strstr(error->message, "bubblewrap"));
    }

    /* The weaker modes are always available. */
    {
        g_autoptr(ClawtSandbox) workspace =
            clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, "/tmp");

        g_assert_true(clawt_sandbox_is_available(workspace, NULL));
    }
}

/* The bwrap command line starts from nothing and adds, which is the only
 * order that fails safe. */
static void
test_bwrap_wrapping(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_auto(GStrv) wrapped = NULL;
    const gchar *argv[] = { "ls", NULL };
    gboolean saw_die_with_parent = FALSE;
    gboolean saw_unshare_net = FALSE;
    gboolean saw_separator = FALSE;
    guint i;

    paths_setup(&paths);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_BWRAP, paths.root);
    clawt_sandbox_set_allow_network(sandbox, FALSE);

    wrapped = clawt_sandbox_wrap_argv(sandbox, argv);
    g_assert_cmpstr(wrapped[0], ==, "bwrap");

    for (i = 0; wrapped[i] != NULL; i++) {
        if (g_strcmp0(wrapped[i], "--die-with-parent") == 0)
            saw_die_with_parent = TRUE;
        if (g_strcmp0(wrapped[i], "--unshare-net") == 0)
            saw_unshare_net = TRUE;
        if (g_strcmp0(wrapped[i], "--") == 0)
            saw_separator = TRUE;
    }

    /* Without this a sandbox outlives a crashed daemon with nothing left to
     * stop it. */
    g_assert_true(saw_die_with_parent);
    g_assert_true(saw_unshare_net);
    g_assert_true(saw_separator);

    /* The real command is still at the end. */
    g_assert_cmpstr(wrapped[g_strv_length(wrapped) - 1], ==, "ls");

    paths_teardown(&paths);
}

/* Weaker modes must not wrap anything. */
static void
test_non_bwrap_modes_do_not_wrap(void)
{
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, "/tmp");
    g_auto(GStrv) wrapped = NULL;
    const gchar *argv[] = { "ls", "-la", NULL };

    wrapped = clawt_sandbox_wrap_argv(sandbox, argv);

    g_assert_cmpstr(wrapped[0], ==, "ls");
    g_assert_cmpstr(wrapped[1], ==, "-la");
    g_assert_null(wrapped[2]);
}

/*
 * The description tells the agent what it can reach, so it does not spend
 * turns discovering the limits by trial or report a policy refusal as a
 * broken tool.
 */
static void
test_description_states_the_limits(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autofree gchar *description = NULL;

    paths_setup(&paths);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_ALLOWLIST, paths.root);
    clawt_sandbox_add_deny_path(sandbox, paths.secret_dir);

    description = clawt_sandbox_describe(sandbox);

    g_assert_nonnull(strstr(description, paths.root));
    g_assert_nonnull(strstr(description, "off-limits"));
    g_assert_nonnull(strstr(description, "sudo"));

    paths_teardown(&paths);
}

/*
 * The honest limitation, pinned down so nobody relies on more than is
 * there: argument inspection cannot see a path a program opens itself.
 * Only bwrap constrains that, because only bwrap involves the kernel.
 */
static void
test_argument_checking_cannot_see_runtime_opens(void)
{
    Paths paths = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    /*
     * The path is built at runtime, so no argument contains it.  A literal
     * "/etc/hostname" in the source WOULD be caught -- the scan is
     * deliberately blunt and treats anything containing a slash as a path --
     * but that is a coincidence of spelling, not a boundary, and an agent
     * that wanted around it would only have to write it this way.
     */
    const gchar *argv[] = {
        "python3", "-c",
        "print(open(chr(47)+'etc'+chr(47)+'hostname').read())", NULL
    };

    paths_setup(&paths);
    sandbox = workspace_sandbox(&paths);

    /*
     * Permitted, and correctly so: there is no path argument to inspect.
     * Only bwrap constrains what a running program opens, because only
     * bwrap involves the kernel.  Stated here, in docs/security.org and in
     * the confine option's own documentation, because a mode people believe
     * is stronger than it is, is worse than none.
     */
    g_assert_true(clawt_sandbox_check_argv(sandbox, argv, &error));

    paths_teardown(&paths);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sandbox/workspace-confines", test_workspace_confines_to_root);
    g_test_add_func("/sandbox/dotdot", test_dotdot_traversal_is_refused);
    g_test_add_func("/sandbox/symlink", test_symlink_escape_is_refused);
    g_test_add_func("/sandbox/prefix-not-containment",
                    test_prefix_is_not_containment);
    g_test_add_func("/sandbox/deny-beats-allow", test_deny_beats_allow);
    g_test_add_func("/sandbox/allowlist", test_allowlist_widens_but_not_indefinitely);
    g_test_add_func("/sandbox/forbidden-path-argv",
                    test_command_naming_a_forbidden_path_is_refused);
    g_test_add_func("/sandbox/path-in-option", test_path_inside_an_option_is_checked);
    g_test_add_func("/sandbox/escalation", test_escalation_is_refused);
    g_test_add_func("/sandbox/escalation-via-shell",
                    test_escalation_through_a_shell_is_refused);
    g_test_add_func("/sandbox/path-via-shell",
                    test_forbidden_path_through_a_shell_is_refused);
    g_test_add_func("/sandbox/allow-sudo", test_allow_sudo_permits_it);
    g_test_add_func("/sandbox/ordinary-command", test_ordinary_command_is_permitted);
    g_test_add_func("/sandbox/plain-options", test_plain_options_are_not_paths);
    g_test_add_func("/sandbox/none", test_none_permits_everything);
    g_test_add_func("/sandbox/bwrap-availability", test_bwrap_availability_is_checked);
    g_test_add_func("/sandbox/bwrap-wrapping", test_bwrap_wrapping);
    g_test_add_func("/sandbox/no-wrap-otherwise", test_non_bwrap_modes_do_not_wrap);
    g_test_add_func("/sandbox/description", test_description_states_the_limits);
    g_test_add_func("/sandbox/runtime-opens-not-seen",
                    test_argument_checking_cannot_see_runtime_opens);

    return g_test_run();
}
