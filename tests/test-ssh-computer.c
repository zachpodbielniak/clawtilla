/*
 * test-ssh-computer.c - The ssh backend
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Nothing here opens a socket.  Every decision this backend makes that
 * is worth checking -- what the command line is, what the allowlist
 * permits, what a failed probe means, which sentence comes back -- is a
 * pure function of the configuration, and that is deliberate: the
 * alternative is a suite that needs a second machine, which means a
 * suite nobody runs.
 *
 * The one thing that does need a real host is behind
 * CLAWT_TEST_INTEGRATION, in test-remote-backends.c.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "clawt-test-util.h"

/*
 * The five a fixture must pin.
 *
 * defaults.workspace_root is the one that matters most: it defaults to
 * ~/.clawtilla/agents, so a test that builds an agent without it
 * scaffolds one into the real fleet.
 */
#define FIXTURE_PREAMBLE \
    "daemon:\n" \
    "  state_dir: \"%s/state\"\n" \
    "  socket: \"%s/d.sock\"\n" \
    "  automation_dir: \"%s/automation\"\n" \
    "  tailscale: false\n" \
    "defaults:\n" \
    "  workspace_root: \"%s/agents\"\n"

/* Whether a NULL-terminated argv contains this exact word. */
static gboolean
argv_has(GStrv argv, const gchar *word)
{
    gsize i;

    for (i = 0; argv != NULL && argv[i] != NULL; i++) {
        if (g_strcmp0(argv[i], word) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * What the remote shell will actually be handed.
 *
 * The last word is the command line, quoted once for the *login* shell
 * that ssh gives it to -- ssh joins everything after the destination and
 * hands the result to that shell, whatever it is. So an assertion about
 * what runs over there has to unquote one layer first; asserting on the
 * quoted spelling instead would break the day the quoting improved,
 * while proving nothing about what the far end sees.
 */
static gchar *
remote_command(GStrv argv)
{
    gsize last = 0;
    gsize i;

    for (i = 0; argv != NULL && argv[i] != NULL; i++)
        last = i;

    return g_shell_unquote(argv[last], NULL);
}

/* Whether any word in an argv contains this text. */
static gboolean
argv_mentions(GStrv argv, const gchar *text)
{
    gsize i;

    for (i = 0; argv != NULL && argv[i] != NULL; i++) {
        if (strstr(argv[i], text) != NULL)
            return TRUE;
    }

    return FALSE;
}

/* ── The command line ────────────────────────────────────────────── */

/*
 * What ssh is actually asked to do.
 *
 * Asserted rather than eyeballed because every one of these is silent
 * when it is wrong: an unquoted argument redirects instead of being
 * printed, a missing BatchMode wedges on a password prompt nobody can
 * see, and a "--" in the wrong place asks the far end to run a program
 * called "--".
 */
static void
test_the_command_line_says_what_it_should(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_ssh_computer_new("chief", "buildbox");
    const gchar *argv[] = { "echo", "hi > /dev/console", NULL };
    g_auto(GStrv) built = NULL;

    clawt_ssh_computer_set_workspace(CLAWT_SSH_COMPUTER(computer),
                                     "/srv/work");

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer),
                                          argv, NULL);
    g_assert_nonnull(built);

    /* Nobody is watching a daemon, so nothing may prompt. */
    g_assert_true(argv_has(built, "BatchMode=yes"));

    /* A host that goes away must fail a turn rather than hang it. */
    g_assert_true(argv_mentions(built, "ConnectTimeout="));
    g_assert_true(argv_mentions(built, "ServerAliveInterval="));

    /* Otherwise every command pays a fresh handshake. */
    g_assert_true(argv_has(built, "ControlMaster=auto"));
    g_assert_true(argv_mentions(built, "ControlPersist="));

    /*
     * The negative one, and the reason this test exists at all.
     * Accepting an unknown host key is somebody's decision to make and
     * clawtilla must never make it for them -- and the failure mode of
     * getting this wrong is silence, because everything works.
     */
    g_assert_false(argv_mentions(built, "StrictHostKeyChecking"));

    /* The alias, and "--" before it rather than after. */
    {
        gsize i;
        gboolean found = FALSE;

        for (i = 0; built[i] != NULL; i++) {
            if (g_strcmp0(built[i], "--") == 0) {
                g_assert_cmpstr(built[i + 1], ==, "buildbox");
                found = TRUE;
                break;
            }
        }

        g_assert_true(found);
    }

    /*
     * The redirection arrives as text.  This is the failure the docs
     * call the bad kind: without the quoting the command exits 0 having
     * written a file, so nothing anywhere reports a problem.
     */
    {
        g_autofree gchar *line = remote_command(built);

        /*
         * g_shell_quote() encloses every word, so even "echo" arrives
         * quoted. That is the point: nothing in an agent's argv is ever
         * handed to the remote shell as syntax.
         */
        g_assert_nonnull(strstr(line, "'echo' 'hi > /dev/console'"));

        /* And with no directory named, the agent's own remote workspace. */
        g_assert_nonnull(strstr(line, "cd '/srv/work' &&"));
    }
}

/*
 * A working directory the caller named beats the configured one.
 */
static void
test_a_named_directory_wins(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_ssh_computer_new("chief", "buildbox");
    const gchar *argv[] = { "ls", NULL };
    g_auto(GStrv) built = NULL;

    clawt_ssh_computer_set_workspace(CLAWT_SSH_COMPUTER(computer),
                                     "/srv/work");

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer),
                                          argv, "/srv/other");

    {
        g_autofree gchar *line = remote_command(built);

        g_assert_nonnull(strstr(line, "cd '/srv/other' &&"));
        g_assert_null(strstr(line, "/srv/work"));
    }
}

/*
 * The shell is named, so a host whose login shell is fish behaves the
 * same as one where it is bash.
 */
static void
test_the_shell_is_named(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_ssh_computer_new("chief", "buildbox");
    const gchar *argv[] = { "true", NULL };
    g_auto(GStrv) built = NULL;

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer),
                                          argv, NULL);
    g_assert_true(argv_has(built, "/bin/sh"));
    g_assert_true(argv_has(built, "-c"));

    clawt_ssh_computer_set_shell(CLAWT_SSH_COMPUTER(computer), "/bin/bash");
    g_clear_pointer(&built, g_strfreev);

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer),
                                          argv, NULL);
    g_assert_true(argv_has(built, "/bin/bash"));
}

/* ── The alias ───────────────────────────────────────────────────── */

/*
 * An alias, not a connection string.
 *
 * The leading hyphen is the one that is a hazard rather than an
 * inconvenience: ssh reads it as an option, so it never reaches a shell
 * to be quoted for and no amount of care further down would help.
 */
static void
test_the_alias_is_an_alias(void)
{
    static const gchar *const refused[] = {
        "", "-oProxyCommand=id", "user@host", "host:22", "a b",
        "host;id", "host$(id)", "ssh://host", NULL
    };
    static const gchar *const accepted[] = {
        "buildbox", "build-box", "build_box", "build.example.com",
        "box2", NULL
    };
    gsize i;

    for (i = 0; refused[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_ssh_host_is_valid(refused[i], &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
    }

    for (i = 0; accepted[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;

        g_assert_true(clawt_ssh_host_is_valid(accepted[i], &error));
        g_assert_no_error(error);
    }

    /*
     * NULL is refused with a message that says where the setting lives,
     * because "not configured" and "configured wrongly" want different
     * things done about them.
     */
    {
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_ssh_host_is_valid(NULL, &error));
        g_assert_nonnull(strstr(error->message, "computer.ssh.host"));
    }
}

/* ── The control socket ──────────────────────────────────────────── */

/*
 * The 108-byte limit, checked before anything tries to use it.
 *
 * An over-long path does not fail at bind time: ssh simply never creates
 * the master, every command pays a fresh handshake, and the only symptom
 * is the remote feeling slow -- which is a symptom of about forty other
 * things.
 */
static void
test_a_control_path_that_will_not_fit_is_refused(void)
{
    g_autofree gchar *long_agent = g_strnfill(200, 'a');
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *ok = NULL;

    path = clawt_ssh_control_path(long_agent, "buildbox", &error);

    g_assert_null(path);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);

    /*
     * And it says the number, rather than only that it was too long --
     * 107 usable bytes of the 108 in sun_path, the last being the NUL.
     */
    g_assert_nonnull(strstr(error->message, "107"));

    ok = clawt_ssh_control_path("chief", "buildbox", NULL);
    g_assert_nonnull(ok);
    g_assert_nonnull(strstr(ok, "chief@buildbox"));
}

/*
 * A path that will not fit turns multiplexing off rather than leaving it
 * on and broken -- and the command line then carries no ControlPath at
 * all, so ssh is not asked for something it cannot do.
 */
static void
test_multiplexing_is_off_when_the_socket_will_not_fit(void)
{
    g_autofree gchar *long_agent = g_strnfill(200, 'b');
    g_autoptr(ClawtComputer) computer = NULL;
    const gchar *argv[] = { "true", NULL };
    g_auto(GStrv) built = NULL;

    /*
     * And it is said out loud. Turning multiplexing off silently would
     * leave every command paying a handshake with nothing anywhere
     * mentioning a path.
     */
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*multiplexing is off*");
    computer = clawt_ssh_computer_new(long_agent, "buildbox");
    g_test_assert_expected_messages();

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer),
                                          argv, NULL);

    g_assert_false(argv_mentions(built, "ControlPath="));
    g_assert_false(argv_has(built, "ControlMaster=auto"));

    /* Everything that does not depend on a socket is still there. */
    g_assert_true(argv_has(built, "BatchMode=yes"));
}

/* ── Lifecycle: there is no machine here ─────────────────────────── */

/*
 * Refusing by name, rather than reporting success about somebody's
 * server.
 *
 * A missing vfunc answering TRUE is the shape this tree keeps finding.
 * Here the vfuncs exist precisely so the refusal can say the true thing
 * -- clawtilla never started it -- rather than the base class's "so
 * whatever it started is still running".
 */
static void
test_stop_and_teardown_refuse_by_name(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_ssh_computer_new("chief", "buildbox");

    g_assert_false(clawt_computer_type_has_machine(CLAWT_COMPUTER_SSH));

    {
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_computer_stop(computer, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
        g_assert_nonnull(strstr(error->message, "buildbox"));
        g_assert_nonnull(strstr(error->message, "Nothing was changed"));
    }

    {
        g_autoptr(GError) error = NULL;

        g_assert_false(clawt_computer_teardown(computer, &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
        g_assert_nonnull(strstr(error->message, "buildbox"));
    }
}

/* ── The allowlist ───────────────────────────────────────────────── */

static ClawtComputer *
confined_computer(void)
{
    ClawtComputer *computer = clawt_ssh_computer_new("chief", "buildbox");
    g_autoptr(ClawtSandbox) sandbox =
        clawt_sandbox_new_remote(CLAWT_CONFINE_ALLOWLIST, "/srv/work");

    clawt_ssh_computer_set_workspace(CLAWT_SSH_COMPUTER(computer),
                                     "/srv/work");
    clawt_ssh_computer_set_sandbox(CLAWT_SSH_COMPUTER(computer), sandbox);

    return computer;
}

/*
 * The wire, not the mechanism: a mount declared on the agent has to
 * *reach* the sandbox.
 *
 * Grepping for the caller rather than reading the implementation is the
 * lesson this tree has learned three times -- a correct allowlist that
 * nothing filled would refuse every path the operator granted and look
 * like confinement working.
 */
static void
test_a_mount_becomes_a_grant(void)
{
    g_autoptr(ClawtComputer) computer = confined_computer();
    ClawtSandbox *sandbox =
        clawt_ssh_computer_get_sandbox(CLAWT_SSH_COMPUTER(computer));
    ClawtMount *mount = clawt_mount_new("/ignored/on/this/machine",
                                        "/data/shared");

    g_assert_false(clawt_sandbox_path_is_allowed(sandbox,
                                                 "/data/shared/file"));

    clawt_computer_add_mount(computer, mount);
    clawt_mount_free(mount);

    clawt_ssh_computer_apply_mounts(CLAWT_SSH_COMPUTER(computer));

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox,
                                                "/data/shared/file"));

    /*
     * The target, not the source. The source is a path on the machine
     * the daemon runs on and granting it would be granting the wrong
     * computer's filesystem.
     */
    g_assert_false(clawt_sandbox_path_is_allowed(
        sandbox, "/ignored/on/this/machine/file"));
}

/*
 * The escapes the allowlist has to catch, and the one it cannot.
 *
 * ".." is the case that makes a remote sandbox a different thing from a
 * local one: clawt_canonicalize_missing() resolves a path as far as it
 * exists *here*, and for a path that exists only over there that is not
 * at all -- so the ".." survives, and "/srv/work/../../etc/shadow" is a
 * prefix of "/srv/work" with a separator after it. A local sandbox says
 * yes to that; this one must not.
 */
static void
test_the_allowlist_catches_what_it_can(void)
{
    g_autoptr(ClawtComputer) computer = confined_computer();
    ClawtSandbox *sandbox =
        clawt_ssh_computer_get_sandbox(CLAWT_SSH_COMPUTER(computer));

    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, "/srv/work"));
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, "/srv/work/notes"));

    /* Climbing out with "..", in three spellings. */
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox,
                                                 "/srv/work/../../etc/shadow"));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/srv/work/.."));
    g_assert_false(clawt_sandbox_path_is_allowed(
        sandbox, "/srv/work/a/b/../../../../etc"));

    /* A prefix of an allowed path is a different directory. */
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/srv/workshop"));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox,
                                                 "/srv/workshop/notes"));

    /* Somewhere else entirely. */
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));

    /*
     * A relative path is relative to the remote working directory, which
     * is the root -- so it is inside, and one that climbs out is not.
     */
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, "notes"));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "../etc"));

    /*
     * "~" is the *remote* account's home and this process cannot know
     * it. Refused rather than guessed at, which is the safe direction.
     */
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "~/.ssh/id_ed25519"));
}

/*
 * The limit, stated rather than implied.
 *
 * A symlink over there cannot be followed from here, so this check is on
 * the text of the path and stops. That is worth a test of its own,
 * because "a check finds the layer it looks at" and the failure of
 * saying nothing is that somebody reads a lexical check as a kernel one.
 *
 * The local sandbox is asserted alongside it, on a symlink that really
 * exists, so the two are shown to be genuinely different behaviours
 * rather than one of them being accidentally right.
 */
static void
test_the_remote_check_says_what_it_cannot_see(void)
{
    g_autoptr(ClawtComputer) computer = confined_computer();
    ClawtSandbox *remote =
        clawt_ssh_computer_get_sandbox(CLAWT_SSH_COMPUTER(computer));
    g_autofree gchar *described = clawt_sandbox_describe(remote);
    g_autofree gchar *root = g_dir_make_tmp("clawt-sshlink-XXXXXX", NULL);
    g_autofree gchar *link = NULL;
    g_autoptr(ClawtSandbox) local = NULL;

    g_assert_nonnull(strstr(described, "symlink"));
    g_assert_nonnull(strstr(described, "another machine"));

    /*
     * The control. A *local* sandbox resolves a real symlink and refuses
     * it, which is exactly what the remote one cannot do -- so if this
     * assertion ever fails, the distinction the remote mode exists for
     * has stopped being a distinction.
     */
    link = g_build_filename(root, "escape", NULL);
    g_assert_cmpint(symlink("/etc", link), ==, 0);

    local = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, root);

    g_assert_true(clawt_sandbox_path_is_allowed(local, root));
    g_assert_false(clawt_sandbox_path_is_allowed(local, link));

    g_unlink(link);
    clawt_test_remove_tree(root);
}

/*
 * Handing this backend a *local* sandbox is refused, not accepted.
 *
 * The two are indistinguishable from the outside and differ exactly on
 * the ".." case above, so silently taking the wrong one would produce a
 * confinement that reads correct and is not.
 */
static void
test_a_local_sandbox_is_refused(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_ssh_computer_new("chief", "buildbox");
    g_autoptr(ClawtSandbox) local =
        clawt_sandbox_new(CLAWT_CONFINE_ALLOWLIST, "/srv/work");

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*clawt_sandbox_new_remote*");
    clawt_ssh_computer_set_sandbox(CLAWT_SSH_COMPUTER(computer), local);
    g_test_assert_expected_messages();

    /* Refused means not taken -- not taken and used anyway. */
    g_assert_null(clawt_ssh_computer_get_sandbox(CLAWT_SSH_COMPUTER(computer)));
}

/*
 * A command that escalates privilege is refused before anything is
 * spawned, including when it is hiding inside a shell one-liner.
 *
 * Through the same ClawtSandbox the host backend uses, so there is one
 * implementation of this rather than two that would drift -- and the
 * refusal names the setting, so an agent told "no" does not try three
 * other ways.
 */
static void
test_escalation_is_refused_here_too(void)
{
    static const gchar *const escalators[] = {
        "sudo", "pkexec", "doas", "run0", NULL
    };
    g_autoptr(ClawtComputer) computer = confined_computer();
    gsize i;

    for (i = 0; escalators[i] != NULL; i++) {
        const gchar *plain[] = { NULL, "id", NULL };
        g_autofree gchar *one_liner = g_strdup_printf("%s id", escalators[i]);
        const gchar *shelled[] = { "sh", "-c", NULL, NULL };
        g_autoptr(GError) direct = NULL;
        g_autoptr(GError) hidden = NULL;

        plain[0] = escalators[i];
        shelled[2] = one_liner;

        g_assert_null(clawt_computer_exec(computer, plain, NULL, 5, NULL,
                                          &direct));
        g_assert_error(direct, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
        g_assert_nonnull(strstr(direct->message, "allow_sudo"));

        /* Refusing `sudo id` while allowing `sh -c 'sudo id'` is theatre. */
        g_assert_null(clawt_computer_exec(computer, shelled, NULL, 5, NULL,
                                          &hidden));
        g_assert_error(hidden, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    }
}

/*
 * A command naming a path outside the allowlist never reaches ssh.
 *
 * Asserted through clawt_computer_exec(), which is the path an agent
 * actually takes: a refusal proved only against the sandbox would not
 * show that the backend consults it.
 */
static void
test_a_path_outside_the_allowlist_never_runs(void)
{
    g_autoptr(ClawtComputer) computer = confined_computer();
    const gchar *argv[] = { "cat", "/srv/work/../../etc/shadow", NULL };
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_computer_exec(computer, argv, NULL, 5, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);

    /* And the working directory is checked as well as the arguments. */
    {
        const gchar *listing[] = { "ls", NULL };
        g_autoptr(GError) directory = NULL;

        g_assert_null(clawt_computer_exec(computer, listing, "/etc", 5, NULL,
                                          &directory));
        g_assert_error(directory, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    }
}

/* ── Missing is not broken ───────────────────────────────────────── */

/*
 * Absence has to be claimed; everything else is "I could not tell".
 *
 * Getting this backwards is how a provisioner sets about creating
 * something that already exists -- on a machine clawtilla does not own,
 * where that is somebody else's data.
 */
static void
test_missing_is_told_apart_from_broken(void)
{
    /* The shapes that genuinely mean not-there, in both wordings. */
    g_assert_cmpint(clawt_ssh_classify_probe(1, "no such file or directory: "
                                                "/srv/work"), ==,
                    CLAWT_SSH_PROBE_MISSING);
    g_assert_cmpint(clawt_ssh_classify_probe(2, "Error: no such container: "
                                                "clawt-chief"), ==,
                    CLAWT_SSH_PROBE_MISSING);
    g_assert_cmpint(clawt_ssh_classify_probe(125, "no such image"), ==,
                    CLAWT_SSH_PROBE_MISSING);

    /* Case is not a signal. */
    g_assert_cmpint(clawt_ssh_classify_probe(1, "No Such Object"), ==,
                    CLAWT_SSH_PROBE_MISSING);

    /*
     * And without the path, which is how probe_directory() phrases it
     * now.  It used to substitute the shell-quoted path a *second* time,
     * into a double-quoted `echo` -- where g_shell_quote()'s single
     * quotes protect nothing and a `"` in the path closed the string, so
     * the rest of it ran on the far machine as the ssh account.  The
     * name was never doing any work here: this function matches on the
     * phrase, and the caller already knows which directory it asked
     * about.
     */
    g_assert_cmpint(clawt_ssh_classify_probe(1, "no such file or directory"),
                    ==, CLAWT_SSH_PROBE_MISSING);

    /* Present is present. */
    g_assert_cmpint(clawt_ssh_classify_probe(0, NULL), ==,
                    CLAWT_SSH_PROBE_PRESENT);
    g_assert_cmpint(clawt_ssh_classify_probe(0, "a warning"), ==,
                    CLAWT_SSH_PROBE_PRESENT);

    /*
     * And everything else is a transport failure, whatever it exited
     * with. 255 especially: ssh reserves it for its own failures, so a
     * remote command that exits 255 is indistinguishable from a
     * connection that never arrived.
     */
    g_assert_cmpint(clawt_ssh_classify_probe(255, "Connection closed by "
                                                  "remote host"), ==,
                    CLAWT_SSH_PROBE_TRANSPORT);
    g_assert_cmpint(clawt_ssh_classify_probe(255, "no such file"), ==,
                    CLAWT_SSH_PROBE_TRANSPORT);
    g_assert_cmpint(clawt_ssh_classify_probe(1, "Permission denied"), ==,
                    CLAWT_SSH_PROBE_TRANSPORT);
    g_assert_cmpint(clawt_ssh_classify_probe(127, "sh: command not found"),
                    ==, CLAWT_SSH_PROBE_TRANSPORT);
    g_assert_cmpint(clawt_ssh_classify_probe(1, NULL), ==,
                    CLAWT_SSH_PROBE_TRANSPORT);
}

/* ── The status ladder ───────────────────────────────────────────── */

/*
 * Exactly one answer, and always the earliest cause.
 *
 * Walked down the ladder one rung at a time: with everything failing the
 * answer is the first rung, and each time one is satisfied the answer
 * moves down by exactly one. That is what "in order" means, and it
 * cannot be checked by arranging six real failures.
 */
static void
test_the_status_ladder_answers_once_and_in_order(void)
{
    static const ClawtSshStatus expected[] = {
        CLAWT_SSH_STATUS_NOT_CONFIGURED,
        CLAWT_SSH_STATUS_UNREACHABLE,
        CLAWT_SSH_STATUS_HOST_KEY,
        CLAWT_SSH_STATUS_AUTH_FAILED,
        CLAWT_SSH_STATUS_WORKSPACE_MISSING,
        CLAWT_SSH_STATUS_NOT_READY,
        CLAWT_SSH_STATUS_READY
    };
    gsize satisfied;

    for (satisfied = 0; satisfied < G_N_ELEMENTS(expected); satisfied++) {
        gboolean flags[6];
        gsize i;

        for (i = 0; i < G_N_ELEMENTS(flags); i++)
            flags[i] = (i < satisfied);

        g_assert_cmpint(clawt_ssh_status_resolve(flags[0], flags[1], flags[2],
                                                 flags[3], flags[4], flags[5]),
                        ==, expected[satisfied]);
    }

    /*
     * A later rung being satisfied does not promote a broken earlier
     * one. Everything true except reachability is still "unreachable",
     * not "ready".
     */
    g_assert_cmpint(clawt_ssh_status_resolve(TRUE, FALSE, TRUE, TRUE, TRUE,
                                             TRUE), ==,
                    CLAWT_SSH_STATUS_UNREACHABLE);
}

/*
 * Every rung has a sentence, and the one that matters names the remedy.
 */
static void
test_every_rung_has_something_to_say(void)
{
    static const ClawtSshStatus every[] = {
        CLAWT_SSH_STATUS_READY, CLAWT_SSH_STATUS_NOT_CONFIGURED,
        CLAWT_SSH_STATUS_UNREACHABLE, CLAWT_SSH_STATUS_HOST_KEY,
        CLAWT_SSH_STATUS_AUTH_FAILED, CLAWT_SSH_STATUS_WORKSPACE_MISSING,
        CLAWT_SSH_STATUS_NOT_READY
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(every); i++) {
        g_autofree gchar *message =
            clawt_ssh_status_message(every[i], "buildbox", "/srv/work");

        g_assert_nonnull(message);
        g_assert_cmpuint(strlen(message), >, 20);
    }

    /*
     * The host-key one is the reason the ladder has a rung of its own.
     * A person has to accept the key; the alternative would be clawtilla
     * making that decision for them, which is exactly what
     * StrictHostKeyChecking=no is.
     */
    {
        g_autofree gchar *message =
            clawt_ssh_status_message(CLAWT_SSH_STATUS_HOST_KEY, "buildbox",
                                     NULL);

        g_assert_nonnull(strstr(message, "ssh buildbox true"));
        g_assert_nonnull(strstr(message, "by hand"));
    }

    {
        g_autofree gchar *message =
            clawt_ssh_status_message(CLAWT_SSH_STATUS_WORKSPACE_MISSING,
                                     "buildbox", "/srv/work");

        g_assert_nonnull(strstr(message, "/srv/work"));
    }
}

/* ── Finding ssh ─────────────────────────────────────────────────── */

/*
 * The refusal names every place that was looked and the package, because
 * "ssh: not found" out of a daemon says nothing about which ssh was
 * wanted or where it was expected to be.
 */
static void
test_a_missing_binary_names_all_three_places(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *found = NULL;
    g_autofree gchar *missing = NULL;

    missing = clawt_ssh_resolve_binary("clawt-no-such-ssh-binary", &error);

    g_assert_null(missing);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "beside this binary"));
    g_assert_nonnull(strstr(error->message, "/usr/bin"));
    g_assert_nonnull(strstr(error->message, "PATH"));
    g_assert_nonnull(strstr(error->message, "openssh-clients"));

    /*
     * And it does find one that is there. Without this the assertion
     * above would pass against a function that always failed.
     */
    found = clawt_ssh_resolve_binary("sh", NULL);
    g_assert_nonnull(found);
    g_assert_true(g_file_test(found, G_FILE_TEST_IS_EXECUTABLE));
}

/* ── sftp ────────────────────────────────────────────────────────── */

/*
 * sftp's batch language quotes with '"' and has no escape inside quotes,
 * so three characters cannot be written at all. The newline is the one
 * that is a hazard: it would end the line and begin a second sftp
 * command of somebody else's choosing.
 */
static void
test_an_unquotable_path_is_refused_rather_than_mangled(void)
{
    g_autofree gchar *ok = NULL;

    g_assert_true(clawt_ssh_sftp_path_is_safe("/srv/work/notes.org"));
    g_assert_true(clawt_ssh_sftp_path_is_safe("/srv/work/a b/c"));

    g_assert_false(clawt_ssh_sftp_path_is_safe("/srv/\"work\""));
    g_assert_false(clawt_ssh_sftp_path_is_safe("/srv/work\\x"));
    g_assert_false(clawt_ssh_sftp_path_is_safe("/srv/work\nrm -rf /"));
    g_assert_false(clawt_ssh_sftp_path_is_safe(NULL));
    g_assert_false(clawt_ssh_sftp_path_is_safe(""));

    g_assert_null(clawt_ssh_build_sftp_batch("get", "/srv/w\nx", "/tmp/y"));
    g_assert_null(clawt_ssh_build_sftp_batch("put", "/tmp/y", "/srv/w\nx"));

    ok = clawt_ssh_build_sftp_batch("get", "/srv/work/a", "/tmp/b");
    g_assert_cmpstr(ok, ==, "get \"/srv/work/a\" \"/tmp/b\"\n");
}

/* ── What the agent is told ──────────────────────────────────────── */

/*
 * The sentence that costs the most turns when it is missing.
 *
 * On every backend an agent's own bash runs on the host; on this one the
 * two filesystems can look identical, so the agent has no way to notice
 * until something it wrote is not where it put it.
 */
static void
test_the_description_says_where_the_agents_own_tools_run(void)
{
    g_autoptr(ClawtComputer) computer = confined_computer();
    ClawtMount *mount = clawt_mount_new("/unused", "/data/shared");
    g_autofree gchar *described = NULL;

    clawt_computer_add_mount(computer, mount);
    clawt_mount_free(mount);

    described = clawt_computer_describe(computer);

    g_assert_nonnull(strstr(described, "buildbox"));
    g_assert_nonnull(strstr(described, "bash, read and write"));
    g_assert_nonnull(strstr(described, "clawtilla_computer_exec"));

    /* That the quoting is literal, so it is not learned by trial. */
    g_assert_nonnull(strstr(described, "literal text"));

    /* That clawtilla will not power it off. */
    g_assert_nonnull(strstr(described, "start it, stop it"));

    /* The grants, by name -- the list is the whole boundary here. */
    g_assert_nonnull(strstr(described, "/data/shared"));
    g_assert_nonnull(strstr(described, "/srv/work"));
}

/* ── The factory ─────────────────────────────────────────────────── */

/*
 * The wire from clawtilla.yaml to the backend.
 *
 * Every setting below was reachable, saved and echoed back before this
 * phase and read by nothing -- which is what CLAWT_SCHEMA_FLAG_INERT
 * meant. Clearing the flag is only half of implementing an option; this
 * asserts the other half.
 */
static void
test_the_factory_wires_an_ssh_computer(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-sshfac-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(root, "clawtilla.yaml", NULL);
    g_autofree gchar *text = g_strdup_printf(
        FIXTURE_PREAMBLE
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: ssh\n"
        "      ssh:\n"
        "        host: buildbox\n"
        "        workspace: /srv/work\n"
        "        shell: /bin/bash\n"
        "        connect_timeout: 4\n"
        "        control_persist: 30\n",
        root, root, root, root);
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    const gchar *argv[] = { "true", NULL };
    g_auto(GStrv) built = NULL;

    g_assert_true(g_file_set_contents(path, text, -1, NULL));

    config = clawt_config_load(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);

    agent = clawt_config_get_agent(config, "chief");
    g_assert_nonnull(agent);

    computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(computer);
    g_assert_true(CLAWT_IS_SSH_COMPUTER(computer));
    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_SSH);

    /* A remote sandbox, not a local one wearing the same name. */
    {
        ClawtSandbox *sandbox =
            clawt_ssh_computer_get_sandbox(CLAWT_SSH_COMPUTER(computer));

        g_assert_nonnull(sandbox);
        g_assert_true(clawt_sandbox_is_remote(sandbox));
        g_assert_false(clawt_sandbox_path_is_allowed(sandbox, "/etc/shadow"));
    }

    built = clawt_ssh_computer_build_argv(CLAWT_SSH_COMPUTER(computer), argv,
                                          NULL);

    g_assert_true(argv_has(built, "ConnectTimeout=4"));
    g_assert_true(argv_has(built, "ControlPersist=30"));
    g_assert_true(argv_has(built, "/bin/bash"));

    {
        g_autofree gchar *line = remote_command(built);

        g_assert_nonnull(strstr(line, "cd '/srv/work' &&"));
    }

    /*
     * And clawtilla's own directories are NOT declared, because there is
     * no mount to make them with. Promising a workspace at
     * /mnt/clawtilla/workspace over there would be a promise the agent
     * discovers is false a turn later.
     */
    {
        GPtrArray *mounts = clawt_computer_get_mounts(computer);
        guint i;

        for (i = 0; mounts != NULL && i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            g_assert_null(strstr(clawt_mount_get_target(mount),
                                 CLAWT_WORKSPACE_MOUNT_POINT));
        }
    }

    clawt_test_remove_tree(root);
}

/*
 * A bad alias is caught when the computer is built, not three turns
 * later when a command is run.
 */
static void
test_the_factory_refuses_a_bad_alias(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-sshbad-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(root, "clawtilla.yaml", NULL);
    g_autofree gchar *text = g_strdup_printf(
        FIXTURE_PREAMBLE
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: ssh\n"
        "      ssh:\n"
        "        host: \"-oProxyCommand=id\"\n",
        root, root, root, root);
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_set_contents(path, text, -1, NULL));

    config = clawt_config_load(path, NULL);
    g_assert_nonnull(config);

    computer = clawt_computer_factory_create(
        clawt_config_get_agent(config, "chief"), NULL, NULL, &error);

    g_assert_null(computer);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);

    clawt_test_remove_tree(root);
}

/* ── The lexical normaliser it all rests on ──────────────────────── */

/*
 * The function that makes a remote allowlist possible at all.
 *
 * Asserted directly as well as through the sandbox, because it is the
 * thing that is different -- realpath() is right for a path on this
 * machine and simply unavailable for one on another.
 */
static void
test_paths_are_normalised_by_text_alone(void)
{
    struct {
        const gchar *in;
        const gchar *out;
    } cases[] = {
        { "/srv/work/../../etc/shadow", "/etc/shadow" },
        { "/srv/work/./notes",          "/srv/work/notes" },
        { "/srv//work///notes",         "/srv/work/notes" },
        { "/srv/work/",                 "/srv/work" },
        { "/",                          "/" },
        { "/..",                        "/" },
        { "/../../..",                  "/" },
        { "a/b/../c",                   "a/c" },
        { "../etc",                     "../etc" },
        { "./x",                        "x" },
        { ".",                          "." }
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *got =
            clawt_normalize_path_lexically(cases[i].in);

        g_assert_cmpstr(got, ==, cases[i].out);
    }

    g_assert_null(clawt_normalize_path_lexically(NULL));

    /*
     * Never the empty string. A root that normalised to "" would make
     * every containment test against it succeed, which is the failure
     * this whole file is about.
     */
    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *got = clawt_normalize_path_lexically(cases[i].in);

        g_assert_cmpuint(strlen(got), >, 0);
    }
}

int
main(int argc, char *argv[])
{
    g_autofree gchar *data_dir = NULL;
    int status;

    data_dir = g_dir_make_tmp("clawt-sshdata-XXXXXX", NULL);
    g_setenv("XDG_DATA_HOME", data_dir, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ssh/argv/says-what-it-should",
                    test_the_command_line_says_what_it_should);
    g_test_add_func("/ssh/argv/a-named-directory-wins",
                    test_a_named_directory_wins);
    g_test_add_func("/ssh/argv/the-shell-is-named",
                    test_the_shell_is_named);
    g_test_add_func("/ssh/host/is-an-alias",
                    test_the_alias_is_an_alias);
    g_test_add_func("/ssh/control/too-long-is-refused",
                    test_a_control_path_that_will_not_fit_is_refused);
    g_test_add_func("/ssh/control/off-when-it-will-not-fit",
                    test_multiplexing_is_off_when_the_socket_will_not_fit);
    g_test_add_func("/ssh/lifecycle/stop-and-teardown-refuse",
                    test_stop_and_teardown_refuse_by_name);
    g_test_add_func("/ssh/mounts/become-grants",
                    test_a_mount_becomes_a_grant);
    g_test_add_func("/ssh/allowlist/catches-what-it-can",
                    test_the_allowlist_catches_what_it_can);
    g_test_add_func("/ssh/allowlist/says-what-it-cannot-see",
                    test_the_remote_check_says_what_it_cannot_see);
    g_test_add_func("/ssh/allowlist/a-local-sandbox-is-refused",
                    test_a_local_sandbox_is_refused);
    g_test_add_func("/ssh/allowlist/escalation-is-refused",
                    test_escalation_is_refused_here_too);
    g_test_add_func("/ssh/allowlist/an-outside-path-never-runs",
                    test_a_path_outside_the_allowlist_never_runs);
    g_test_add_func("/ssh/probe/missing-is-not-broken",
                    test_missing_is_told_apart_from_broken);
    g_test_add_func("/ssh/status/one-answer-in-order",
                    test_the_status_ladder_answers_once_and_in_order);
    g_test_add_func("/ssh/status/every-rung-speaks",
                    test_every_rung_has_something_to_say);
    g_test_add_func("/ssh/binary/names-all-three-places",
                    test_a_missing_binary_names_all_three_places);
    g_test_add_func("/ssh/sftp/unquotable-is-refused",
                    test_an_unquotable_path_is_refused_rather_than_mangled);
    g_test_add_func("/ssh/describe/says-where-the-tools-run",
                    test_the_description_says_where_the_agents_own_tools_run);
    g_test_add_func("/ssh/factory/wires-a-computer",
                    test_the_factory_wires_an_ssh_computer);
    g_test_add_func("/ssh/factory/refuses-a-bad-alias",
                    test_the_factory_refuses_a_bad_alias);
    g_test_add_func("/ssh/paths/normalised-by-text",
                    test_paths_are_normalised_by_text_alone);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
