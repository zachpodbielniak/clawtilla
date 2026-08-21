/*
 * clawt-sandbox.c - Deciding what a host command may touch
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-sandbox.h"

#include <string.h>
#include <stdlib.h>

struct _ClawtSandbox {
    GObject parent_instance;

    ClawtConfineMode  mode;
    gchar            *root;
    GPtrArray        *allow_paths;
    GPtrArray        *mount_paths;
    GPtrArray        *deny_paths;
    gboolean          allow_network;
    gboolean          allow_sudo;
};

G_DEFINE_FINAL_TYPE(ClawtSandbox, clawt_sandbox, G_TYPE_OBJECT)

/*
 * Programs that hand an agent another user's authority.
 *
 * Matched on the basename, so /usr/bin/sudo and a bare sudo are both
 * caught, and checked inside shell one-liners too -- refusing `sudo id`
 * while allowing `sh -c 'sudo id'` would be security theatre.
 */
static const gchar *const escalation_commands[] = {
    "sudo", "pkexec", "doas", "run0", "machinectl", "su", "setpriv",
    NULL
};

/* Shells, whose -c argument is another command that needs the same checks. */
static const gchar *const shell_commands[] = {
    "sh", "bash", "dash", "zsh", "ksh", "fish", "csh", "tcsh",
    NULL
};

static gboolean
basename_matches(const gchar *path, const gchar *const *names)
{
    g_autofree gchar *base = g_path_get_basename(path);
    gsize i;

    for (i = 0; names[i] != NULL; i++) {
        if (g_strcmp0(base, names[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

ClawtSandbox *
clawt_sandbox_new(ClawtConfineMode mode, const gchar *root)
{
    ClawtSandbox *self = g_object_new(CLAWT_TYPE_SANDBOX, NULL);

    self->mode = mode;
    self->root = clawt_expand_path(root);

    return self;
}

void
clawt_sandbox_add_mount_path(ClawtSandbox *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));

    if (path == NULL)
        return;

    g_ptr_array_add(self->mount_paths, clawt_canonicalize_missing(path));
}

void
clawt_sandbox_add_allow_path(ClawtSandbox *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    g_return_if_fail(path != NULL);

    g_ptr_array_add(self->allow_paths, clawt_expand_path(path));
}

void
clawt_sandbox_add_deny_path(ClawtSandbox *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    g_return_if_fail(path != NULL);

    g_ptr_array_add(self->deny_paths, clawt_expand_path(path));
}

void
clawt_sandbox_set_allow_network(ClawtSandbox *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    self->allow_network = allow;
}

void
clawt_sandbox_set_allow_sudo(ClawtSandbox *self, gboolean allow)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    self->allow_sudo = allow;
}

ClawtConfineMode
clawt_sandbox_get_mode(ClawtSandbox *self)
{
    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), CLAWT_CONFINE_WORKSPACE);
    return self->mode;
}

gboolean
clawt_sandbox_is_available(ClawtSandbox *self, GError **error)
{
    g_autofree gchar *bwrap = NULL;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), FALSE);

    if (self->mode != CLAWT_CONFINE_BWRAP)
        return TRUE;

    bwrap = g_find_program_in_path("bwrap");

    /*
     * Missing bwrap is an error, not a downgrade.  Quietly falling back to
     * a weaker mode would leave an agent the user believes is sandboxed
     * running unsandboxed, which is the worst outcome available.
     */
    if (bwrap == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "confine: bwrap needs bubblewrap, which is not "
                            "installed; install it (Fedora: bubblewrap) or "
                            "choose another confinement mode");
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_sandbox_path_is_allowed(ClawtSandbox *self, const gchar *path)
{
    g_autofree gchar *resolved = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), FALSE);

    if (path == NULL)
        return TRUE;

    if (self->mode == CLAWT_CONFINE_NONE)
        return TRUE;

    resolved = clawt_canonicalize_missing(path);

    /*
     * Denials are checked first and win outright, so ~/.ssh stays out even
     * when all of ~ has been allowed.  Ordering it the other way would make
     * a broad allow silently defeat every specific deny.
     */
    for (i = 0; i < self->deny_paths->len; i++) {
        if (clawt_path_is_within(resolved, g_ptr_array_index(self->deny_paths, i)))
            return FALSE;
    }

    if (self->root != NULL && clawt_path_is_within(resolved, self->root))
        return TRUE;

    /*
     * Mounts are honoured in every mode.  Declaring one is an explicit
     * grant, and on a container the kernel would make it reachable --
     * refusing it here would mean the same config behaves differently
     * depending on the backend.
     */
    for (i = 0; i < self->mount_paths->len; i++) {
        if (clawt_path_is_within(resolved,
                                 g_ptr_array_index(self->mount_paths, i)))
            return TRUE;
    }

    if (self->mode == CLAWT_CONFINE_WORKSPACE)
        return FALSE;

    for (i = 0; i < self->allow_paths->len; i++) {
        if (clawt_path_is_within(resolved, g_ptr_array_index(self->allow_paths, i)))
            return TRUE;
    }

    return FALSE;
}

/*
 * Whether an argument looks like a path worth checking.
 *
 * Deliberately conservative: anything holding a "/" is treated as a path.
 * A false positive costs a refused command with a clear reason; a false
 * negative lets a path through unchecked, which is the failure that
 * matters.
 */
static gboolean
argument_looks_like_a_path(const gchar *argument)
{
    if (argument == NULL || *argument == '\0')
        return FALSE;

    /* Options are not paths, though "--file=/etc/x" contains one. */
    if (argument[0] == '-' && strchr(argument, '/') == NULL)
        return FALSE;

    return strchr(argument, '/') != NULL || argument[0] == '~';
}

/*
 * Pulls the path out of "--option=/some/path", so the check sees the path
 * rather than the whole argument.
 */
static const gchar *
path_part_of(const gchar *argument)
{
    const gchar *equals;

    if (argument[0] != '-')
        return argument;

    equals = strchr(argument, '=');

    return (equals != NULL) ? equals + 1 : argument;
}

static gboolean
check_escalation(ClawtSandbox        *self,
                 const gchar * const *argv,
                 GError             **error)
{
    gsize i;

    if (self->allow_sudo)
        return TRUE;

    for (i = 0; argv[i] != NULL; i++) {
        if (!basename_matches(argv[i], escalation_commands))
            continue;

        /*
         * Named rather than merely refused.  An agent told "permission
         * denied" will try three other ways to escalate; an agent told
         * sudo is off will do something else.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "'%s' is not permitted: this agent runs with "
                    "allow_sudo: false, so it cannot escalate privilege",
                    argv[i]);
        return FALSE;
    }

    return TRUE;
}

gboolean
clawt_sandbox_check_argv(ClawtSandbox        *self,
                         const gchar * const *argv,
                         GError             **error)
{
    gsize i;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), FALSE);
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, FALSE);

    if (!check_escalation(self, argv, error))
        return FALSE;

    /*
     * A shell's -c argument is another command, and it gets the same
     * treatment.  Refusing `sudo id` while allowing `sh -c 'sudo id'` would
     * be theatre.
     */
    if (basename_matches(argv[0], shell_commands)) {
        for (i = 1; argv[i] != NULL; i++) {
            g_auto(GStrv) inner = NULL;

            if (g_strcmp0(argv[i], "-c") != 0 || argv[i + 1] == NULL)
                continue;

            if (!g_shell_parse_argv(argv[i + 1], NULL, &inner, NULL))
                continue;

            if (!clawt_sandbox_check_argv(self,
                                          (const gchar * const *)inner,
                                          error))
                return FALSE;
        }
    }

    /*
     * bwrap constrains the process itself, so the argument scan would only
     * add false refusals on top of a real boundary.
     */
    if (self->mode == CLAWT_CONFINE_NONE || self->mode == CLAWT_CONFINE_BWRAP)
        return TRUE;

    for (i = 1; argv[i] != NULL; i++) {
        const gchar *candidate;

        if (!argument_looks_like_a_path(argv[i]))
            continue;

        candidate = path_part_of(argv[i]);

        if (clawt_sandbox_path_is_allowed(self, candidate))
            continue;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach", candidate);
        return FALSE;
    }

    return TRUE;
}

GStrv
clawt_sandbox_wrap_argv(ClawtSandbox *self, const gchar * const *argv)
{
    GPtrArray *out;
    guint i;
    gsize j;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), NULL);
    g_return_val_if_fail(argv != NULL, NULL);

    if (self->mode != CLAWT_CONFINE_BWRAP) {
        out = g_ptr_array_new_with_free_func(g_free);

        for (j = 0; argv[j] != NULL; j++)
            g_ptr_array_add(out, g_strdup(argv[j]));

        g_ptr_array_add(out, NULL);
        return (GStrv)g_ptr_array_free(out, FALSE);
    }

    out = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(out, g_strdup("bwrap"));

    /*
     * A minimal read-only system, then exactly what the agent was granted.
     * Starting from nothing and adding is the only order that fails safe:
     * starting from everything and removing means each new thing on the
     * host is reachable until somebody remembers to exclude it.
     */
    g_ptr_array_add(out, g_strdup("--ro-bind"));
    g_ptr_array_add(out, g_strdup("/usr"));
    g_ptr_array_add(out, g_strdup("/usr"));
    g_ptr_array_add(out, g_strdup("--ro-bind"));
    g_ptr_array_add(out, g_strdup("/etc"));
    g_ptr_array_add(out, g_strdup("/etc"));
    g_ptr_array_add(out, g_strdup("--symlink"));
    g_ptr_array_add(out, g_strdup("usr/bin"));
    g_ptr_array_add(out, g_strdup("/bin"));
    g_ptr_array_add(out, g_strdup("--symlink"));
    g_ptr_array_add(out, g_strdup("usr/lib"));
    g_ptr_array_add(out, g_strdup("/lib"));
    g_ptr_array_add(out, g_strdup("--symlink"));
    g_ptr_array_add(out, g_strdup("usr/lib64"));
    g_ptr_array_add(out, g_strdup("/lib64"));

    g_ptr_array_add(out, g_strdup("--proc"));
    g_ptr_array_add(out, g_strdup("/proc"));
    g_ptr_array_add(out, g_strdup("--dev"));
    g_ptr_array_add(out, g_strdup("/dev"));
    g_ptr_array_add(out, g_strdup("--tmpfs"));
    g_ptr_array_add(out, g_strdup("/tmp"));

    if (self->root != NULL) {
        g_ptr_array_add(out, g_strdup("--bind"));
        g_ptr_array_add(out, g_strdup(self->root));
        g_ptr_array_add(out, g_strdup(self->root));
        g_ptr_array_add(out, g_strdup("--chdir"));
        g_ptr_array_add(out, g_strdup(self->root));
    }

    for (i = 0; i < self->allow_paths->len; i++) {
        const gchar *path = g_ptr_array_index(self->allow_paths, i);

        g_ptr_array_add(out, g_strdup("--bind"));
        g_ptr_array_add(out, g_strdup(path));
        g_ptr_array_add(out, g_strdup(path));
    }

    if (!self->allow_network)
        g_ptr_array_add(out, g_strdup("--unshare-net"));

    /*
     * The sandbox dies with the daemon.  Without this a bwrap child
     * outlives a crashed daemon and keeps whatever it was doing, with
     * nothing left to stop it.
     */
    g_ptr_array_add(out, g_strdup("--die-with-parent"));
    g_ptr_array_add(out, g_strdup("--unshare-pid"));
    g_ptr_array_add(out, g_strdup("--new-session"));

    g_ptr_array_add(out, g_strdup("--"));

    for (j = 0; argv[j] != NULL; j++)
        g_ptr_array_add(out, g_strdup(argv[j]));

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

gchar *
clawt_sandbox_describe(ClawtSandbox *self)
{
    GString *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), NULL);

    out = g_string_new(NULL);

    switch (self->mode) {
    case CLAWT_CONFINE_NONE:
        g_string_append(out,
            "You are running on the host with no restrictions.");
        break;

    case CLAWT_CONFINE_WORKSPACE:
        g_string_append_printf(out,
            "You are running on the host, confined to %s. Commands naming "
            "paths outside it will be refused.",
            self->root != NULL ? self->root : "your workspace");
        break;

    case CLAWT_CONFINE_ALLOWLIST:
        g_string_append_printf(out,
            "You are running on the host, confined to %s",
            self->root != NULL ? self->root : "your workspace");

        for (i = 0; i < self->allow_paths->len; i++)
            g_string_append_printf(out, ", %s",
                                   (const gchar *)
                                   g_ptr_array_index(self->allow_paths, i));

        g_string_append(out,
            ". Commands naming paths outside those will be refused.");
        break;

    case CLAWT_CONFINE_BWRAP:
        g_string_append_printf(out,
            "You are running on the host inside a bubblewrap sandbox. "
            "Writable: %s",
            self->root != NULL ? self->root : "your workspace");

        for (i = 0; i < self->allow_paths->len; i++)
            g_string_append_printf(out, ", %s",
                                   (const gchar *)
                                   g_ptr_array_index(self->allow_paths, i));

        g_string_append(out, ". The rest of the filesystem is read-only or "
                             "absent.");
        break;

    default:
        break;
    }

    if (self->deny_paths->len > 0) {
        g_string_append(out, " Explicitly off-limits:");

        for (i = 0; i < self->deny_paths->len; i++)
            g_string_append_printf(out, "%s %s", i > 0 ? "," : "",
                                   (const gchar *)
                                   g_ptr_array_index(self->deny_paths, i));

        g_string_append_c(out, '.');
    }

    if (!self->allow_network && self->mode == CLAWT_CONFINE_BWRAP)
        g_string_append(out, " You have no network access.");

    /*
     * Saying so plainly saves an agent three turns discovering it by trial,
     * and stops it reporting a policy refusal as a broken tool.
     */
    if (!self->allow_sudo)
        g_string_append(out, " You cannot use sudo or otherwise escalate "
                             "privilege.");

    return g_string_free(out, FALSE);
}

static void
clawt_sandbox_finalize(GObject *object)
{
    ClawtSandbox *self = CLAWT_SANDBOX(object);

    g_clear_pointer(&self->root, g_free);
    g_clear_pointer(&self->allow_paths, g_ptr_array_unref);
    g_clear_pointer(&self->mount_paths, g_ptr_array_unref);
    g_clear_pointer(&self->deny_paths, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_sandbox_parent_class)->finalize(object);
}

static void
clawt_sandbox_class_init(ClawtSandboxClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_sandbox_finalize;
}

static void
clawt_sandbox_init(ClawtSandbox *self)
{
    self->allow_paths = g_ptr_array_new_with_free_func(g_free);
    self->mount_paths = g_ptr_array_new_with_free_func(g_free);
    self->deny_paths = g_ptr_array_new_with_free_func(g_free);
    self->allow_network = TRUE;
    self->allow_sudo = FALSE;
}
