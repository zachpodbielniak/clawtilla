/*
 * test-command-syntax.c - What a command line relies on and will not get
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla_computer_exec runs a program, not a shell line: the command
 * is lexed by g_shell_parse_argv() and spawned directly, so every shell
 * operator in it survives into argv as literal text.  Nothing errors,
 * the program exits 0, and the rest of the line comes back on stdout --
 * which reads as a command that ran and behaved oddly rather than as a
 * command that was never a command.
 *
 * There is no shell because confinement inspects the translated argv, so
 * the fix is to say so rather than to wrap it.  These tests pin both
 * halves: the constructs that are refused, and -- at least as important
 * -- the ones that are not, because a refusal for a command that works
 * today is a regression dressed as a safety feature.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

static void
assert_refused(const gchar *command, const gchar *naming)
{
    g_autofree gchar *refusal = clawt_command_shell_syntax_refusal(command);

    if (refusal == NULL)
        g_error("`%s` was accepted; it needs a shell and will not get one",
                command);

    /*
     * The construct is named, because a refusal that only says "no"
     * gets retried in the same shape.  Named in the failure too, for
     * the same reason one level up.
     */
    if (strstr(refusal, naming) == NULL)
        g_error("`%s` was refused without naming %s: %s", command, naming,
                refusal);

    /* And the way to run it anyway. */
    g_assert_nonnull(strstr(refusal, "bash -c"));
}

static void
assert_accepted(const gchar *command)
{
    g_autofree gchar *refusal = clawt_command_shell_syntax_refusal(command);

    if (refusal != NULL)
        g_error("`%s` was refused, but it runs correctly without a shell: %s",
                command, refusal);
}

/*
 * The operators that cost a real work cycle.  The reported case was
 * `echo "whoami=$(whoami)"; for p in ...; do ... done`, which came back
 * with the loop printed verbatim, $(whoami) unexpanded, and an exit
 * status of 0.
 */
static void
test_control_operators_are_refused(void)
{
    assert_refused("echo a; echo b", ";");
    assert_refused("echo a && echo b", "&");
    assert_refused("echo a || echo b", "pipe");
    assert_refused("ls | wc -l", "pipe");
    assert_refused("ls & ", "&");
}

static void
test_redirections_are_refused(void)
{
    assert_refused("echo hi > /dev/console", "redirection");
    assert_refused("cat < input", "redirection");
    assert_refused("make 2>&1", "redirection");
}

static void
test_substitutions_are_refused(void)
{
    assert_refused("echo $(whoami)", "substitution");
    assert_refused("echo `whoami`", "backquoted");
    assert_refused("echo $HOME", "$VAR");
    assert_refused("echo ${HOME}", "$VAR");
    assert_refused("echo $_underscore", "$VAR");
}

/*
 * A double quote stops word splitting and nothing else: bash expands
 * "$(whoami)" and so did the agent that wrote it.
 */
static void
test_double_quotes_do_not_protect_an_expansion(void)
{
    assert_refused("echo \"whoami=$(whoami)\"", "substitution");
    assert_refused("echo \"home=$HOME\"", "$VAR");
    assert_refused("echo \"now=`date`\"", "backquoted");
}

static void
test_a_line_break_is_refused(void)
{
    assert_refused("echo a\necho b", "line break");
}

/*
 * Single quotes make every one of them literal, and a program that is
 * handed `a|b` as one argument gets exactly what was meant.  This half
 * is why the check reads the raw string: after lexing, `grep 'a|b' f`
 * and `a | b` are the same argv and there is nothing left to tell them
 * apart.
 */
static void
test_single_quotes_are_left_alone(void)
{
    assert_accepted("grep 'a|b' file");
    assert_accepted("awk '{print $1}' file");
    assert_accepted("sed 's/a;b/c/' file");
    assert_accepted("find . -name '*.c' -print");
    assert_accepted("git log --format='%h %s'");
}

/*
 * The supported route out.  bash -c is inspected the same way anything
 * else is -- clawt_sandbox_check_argv() re-parses the nested command
 * line -- so refusing it would leave the refusal with nothing to
 * recommend.
 */
static void
test_the_remedy_is_itself_accepted(void)
{
    assert_accepted("bash -c \"echo hi > /dev/console\"");
    assert_accepted("sh -c 'a; b'");
    assert_accepted("bash -c 'for p in /mnt/a /mnt/b; do ls $p; done'");
}

/*
 * Globs are deliberately not flagged.  An unquoted `*.log` reaches the
 * program unchanged and that is sometimes exactly what the program
 * wanted, so refusing it would break commands that work today.
 */
static void
test_globs_are_not_refused(void)
{
    assert_accepted("ls *.c");
    assert_accepted("rm -f build/*.o");
    assert_accepted("ls file?.txt");
    assert_accepted("ls [ab]*.c");
}

/*
 * A bare $ is a regex anchor far more often than it is an expansion,
 * and it reaches the program either way.  Only the spellings that are
 * actually expansion syntax are refused.
 */
static void
test_a_bare_dollar_is_not_an_expansion(void)
{
    assert_accepted("grep -E foo$ file");
    assert_accepted("echo cost-in-$");
    assert_accepted("echo 100$");
}

static void
test_ordinary_commands_are_untouched(void)
{
    assert_accepted("ls -la /tmp");
    assert_accepted("podman ps --format {{.Names}}");
    assert_accepted("systemctl --user status clawtillad");
    assert_accepted("python3 -c 'print(1)'");
    assert_accepted("");
    assert_accepted(NULL);
}

/*
 * A backslash escapes the character after it everywhere except inside
 * single quotes, so an escaped operator is literal and reaches the
 * program as one argument.
 */
static void
test_an_escaped_operator_is_literal(void)
{
    assert_accepted("echo \\;");
    assert_accepted("echo \\$HOME");
    assert_accepted("find . -exec echo {} \\;");
}

/*
 * The first construct found is the one named, and the scan does not run
 * past the end of the string when an operator is the last character --
 * `$` at the end has nothing after it to classify.
 */
static void
test_a_trailing_operator_does_not_run_off_the_end(void)
{
    assert_accepted("echo $");
    assert_accepted("echo \\");
    assert_refused("echo &", "&");
    assert_accepted("echo 'unterminated");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/command-syntax/control-operators-refused",
                    test_control_operators_are_refused);
    g_test_add_func("/command-syntax/redirections-refused",
                    test_redirections_are_refused);
    g_test_add_func("/command-syntax/substitutions-refused",
                    test_substitutions_are_refused);
    g_test_add_func("/command-syntax/double-quotes-do-not-protect",
                    test_double_quotes_do_not_protect_an_expansion);
    g_test_add_func("/command-syntax/a-line-break-is-refused",
                    test_a_line_break_is_refused);
    g_test_add_func("/command-syntax/single-quotes-left-alone",
                    test_single_quotes_are_left_alone);
    g_test_add_func("/command-syntax/the-remedy-is-accepted",
                    test_the_remedy_is_itself_accepted);
    g_test_add_func("/command-syntax/globs-are-not-refused",
                    test_globs_are_not_refused);
    g_test_add_func("/command-syntax/a-bare-dollar-is-not-an-expansion",
                    test_a_bare_dollar_is_not_an_expansion);
    g_test_add_func("/command-syntax/ordinary-commands-untouched",
                    test_ordinary_commands_are_untouched);
    g_test_add_func("/command-syntax/an-escaped-operator-is-literal",
                    test_an_escaped_operator_is_literal);
    g_test_add_func("/command-syntax/a-trailing-operator-is-safe",
                    test_a_trailing_operator_does_not_run_off_the_end);

    return g_test_run();
}
