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
#include <sys/stat.h>

struct _ClawtSandbox {
    GObject parent_instance;

    ClawtConfineMode  mode;
    gchar            *root;
    GPtrArray        *allow_paths;
    GPtrArray        *mount_paths;
    GArray           *mount_writable;   /* gboolean, parallel to mount_paths */
    GPtrArray        *deny_paths;
    gboolean          allow_network;
    gboolean          allow_sudo;
    gboolean          remote;

    /*
     * Set only for a confined agent's own process: an empty tmpfs over
     * this path, so the AI CLI has a $HOME to write dotfiles in that is
     * not the operator's.  NULL leaves the home directory absent, which
     * is what a clawtilla_computer_exec box has always had.
     */
    gchar            *private_home;
    gboolean          agent_confined;

    /* Symlinks to recreate inside: pairs of (link path, target). */
    GPtrArray        *symlinks;
};

G_DEFINE_FINAL_TYPE(ClawtSandbox, clawt_sandbox, G_TYPE_OBJECT)

/*
 * One spelling of "put this path in the form we compare in".
 *
 * Every stored path and every candidate goes through here, so the two
 * can never be normalised by different rules -- which is the whole way a
 * containment test comes to be more permissive than it reads.
 *
 * On this machine that is realpath(), which collapses symlinks and ".."
 * together.  On another machine there is nothing to call realpath()
 * against, and clawt_canonicalize_missing() would hand back the string
 * unchanged with its ".." intact; see clawt_sandbox_new_remote().
 */
static gchar *
sandbox_normalize(ClawtSandbox *self, const gchar *path)
{
    if (!self->remote)
        return clawt_canonicalize_missing(path);

    /*
     * A relative path is relative to the remote working directory, which
     * is the root.  Resolved here rather than left alone, or "../etc" is
     * compared against an absolute root, matches nothing, and is refused
     * for the wrong reason -- and "notes" is refused although it is
     * exactly where the agent was told to work.
     */
    if (path != NULL && path[0] != '/' && path[0] != '~' &&
        self->root != NULL) {
        g_autofree gchar *joined = g_build_filename(self->root, path, NULL);

        return clawt_normalize_path_lexically(joined);
    }

    /*
     * "~" is the remote account's home and this process has no way to
     * know it.  Left as written, so it falls outside every allowed root
     * and is refused -- the safe direction, and clawt_sandbox_describe()
     * tells the agent to use absolute paths.
     */
    return clawt_normalize_path_lexically(path);
}

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
    self->root = sandbox_normalize(self, root);

    return self;
}

ClawtSandbox *
clawt_sandbox_new_remote(ClawtConfineMode mode, const gchar *root)
{
    ClawtSandbox *self = g_object_new(CLAWT_TYPE_SANDBOX, NULL);

    /*
     * Set before the root is stored, which is why this is a constructor
     * and not a setter: the root is normalised on the way in, and a
     * sandbox told afterwards that it is remote would already be holding
     * a path resolved against the wrong machine.
     */
    self->remote = TRUE;
    self->mode = mode;
    self->root = sandbox_normalize(self, root);

    return self;
}

gboolean
clawt_sandbox_is_remote(ClawtSandbox *self)
{
    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), FALSE);

    return self->remote;
}

void
clawt_sandbox_add_mount_path(ClawtSandbox *self, const gchar *path,
                             gboolean writable)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));

    if (path == NULL)
        return;

    g_ptr_array_add(self->mount_paths, sandbox_normalize(self, path));
    g_array_append_val(self->mount_writable, writable);
}

/*
 * Every stored path is canonicalised, because the candidate always is.
 *
 * Comparing a realpath()-resolved candidate against an unresolved stored
 * path means a deny entry that is itself a symlink -- ~/.ssh in a
 * stow-managed home, say -- never matches its own real target, while the
 * broader allow entry still does.  The deny then silently loses.
 */
void
clawt_sandbox_add_allow_path(ClawtSandbox *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    g_return_if_fail(path != NULL);

    g_ptr_array_add(self->allow_paths, sandbox_normalize(self, path));
}

void
clawt_sandbox_add_deny_path(ClawtSandbox *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));
    g_return_if_fail(path != NULL);

    g_ptr_array_add(self->deny_paths, sandbox_normalize(self, path));
}

void
clawt_sandbox_add_symlink(ClawtSandbox *self, const gchar *path)
{
    g_autofree gchar *target = NULL;
    g_autofree gchar *parent = NULL;
    g_autofree gchar *real_parent = NULL;
    g_autofree gchar *name = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_SANDBOX(self));

    if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        return;

    target = sandbox_normalize(self, path);

    /*
     * The link's directory canonicalised, its own name not: that is the
     * spelling a PATH lookup inside the box will try.
     */
    parent = g_path_get_dirname(path);
    real_parent = sandbox_normalize(self, parent);
    name = g_path_get_basename(path);

    for (i = 0; i < self->symlinks->len; i += 2) {
        g_autofree gchar *candidate =
            g_build_filename(real_parent, name, NULL);

        if (g_strcmp0(g_ptr_array_index(self->symlinks, i), candidate) == 0)
            return;
    }

    g_ptr_array_add(self->symlinks, g_build_filename(real_parent, name,
                                                     NULL));
    g_ptr_array_add(self->symlinks, g_steal_pointer(&target));
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

void
clawt_sandbox_set_private_home(ClawtSandbox *self, const gchar *home)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));

    g_clear_pointer(&self->private_home, g_free);

    /*
     * Normalised like every other stored path, so the ordering checks in
     * the builder compare like with like: a home reached through
     * /home -> /var/home would otherwise never be "within" itself.
     */
    if (home != NULL && *home != '\0')
        self->private_home = sandbox_normalize(self, home);
}

void
clawt_sandbox_set_agent_confined(ClawtSandbox *self, gboolean confined)
{
    g_return_if_fail(CLAWT_IS_SANDBOX(self));

    self->agent_confined = confined;
}

/*
 * The most entries one denied directory is walked for.  A deny is a
 * small thing -- ~/.ssh, a key file, a credentials directory -- and a
 * walk that would take longer than this is a deny that names the wrong
 * thing; it is refused rather than half-checked.
 */
#define LINK_WALK_LIMIT (20000)

/*
 * Whether @path is root's by construction: /usr and /etc are bound from
 * the system, and fs.protected_hardlinks stops an unprivileged agent
 * giving their files another name.  Skipping them is what keeps the
 * check from refusing every sandbox on a distribution that hardlinks its
 * own binaries.
 */
static gboolean
link_check_exempt(const gchar *path)
{
    return clawt_path_is_within(path, "/usr") ||
           clawt_path_is_within(path, "/etc");
}

static gboolean
refuse_linked_file(const gchar *path, const gchar *why, GError **error)
{
    struct stat st;

    if (link_check_exempt(path) || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink <= 1)
        return TRUE;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                "%s has %u hard links, so %s: another name for the same "
                "file reaches it around the bind. Find them with `find / "
                "-xdev -samefile %s` and remove the others, or copy the "
                "file so it has one name",
                path, (guint)st.st_nlink, why, path);

    return FALSE;
}

/*
 * Walks a denied directory for files with a second name.
 *
 * Covering the directory with a tmpfs removes its names from the box;
 * an inode with another name somewhere the box does bind is still
 * reachable through that one.  Symlinks are not followed: a symlink
 * inside a denied directory names something by path, and paths are what
 * the binds already decide.
 */
static gboolean
refuse_linked_tree(const gchar *root, guint *budget, GError **error)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    dir = g_dir_open(root, 0, NULL);

    if (dir == NULL)
        return TRUE;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *child = g_build_filename(root, name, NULL);
        struct stat st;

        if (*budget == 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                        "the denied directory %s holds more than %d "
                        "entries, too many to check for hard links; deny "
                        "something narrower", root, LINK_WALK_LIMIT);
            return FALSE;
        }

        (*budget)--;

        if (lstat(child, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (!refuse_linked_tree(child, budget, error))
                return FALSE;
            continue;
        }

        if (S_ISREG(st.st_mode) &&
            !refuse_linked_file(child, "denying its directory does not "
                                "hide it", error))
            return FALSE;
    }

    return TRUE;
}

static gboolean bwrap_path_is_visible(ClawtSandbox *self, const gchar *path);

gboolean
clawt_sandbox_check_links(ClawtSandbox *self, GError **error)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), FALSE);

    if (self->mode != CLAWT_CONFINE_BWRAP)
        return TRUE;

    /*
     * Read-only binds of a single file.  A directory bound read-only is
     * not walked: it is a CLI's install or a mount somebody declared,
     * often large, and the threat is a second name under a *writable*
     * bind -- which for a directory means the operator bound one inside
     * the other, a decision the mount list already shows.
     */
    for (i = 0; i < self->mount_paths->len; i++) {
        const gchar *path = g_ptr_array_index(self->mount_paths, i);
        gboolean writable = (i < self->mount_writable->len)
            ? g_array_index(self->mount_writable, gboolean, i) : FALSE;

        if (writable)
            continue;

        if (!refuse_linked_file(path, "binding it read-only does not "
                                "stop it being written", error))
            return FALSE;
    }

    /*
     * Denials, the ones the builder emits an operation for.  A denial of
     * something no bind brings in has nothing to leak through.
     */
    for (i = 0; i < self->deny_paths->len; i++) {
        const gchar *path = g_ptr_array_index(self->deny_paths, i);
        guint budget = LINK_WALK_LIMIT;

        if (!bwrap_path_is_visible(self, path))
            continue;

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            if (!refuse_linked_tree(path, &budget, error))
                return FALSE;
            continue;
        }

        if (!refuse_linked_file(path, "covering it with /dev/null does "
                                "not hide it", error))
            return FALSE;
    }

    return TRUE;
}

gboolean
clawt_sandbox_environment_is_owned(const gchar *name)
{
    if (name == NULL)
        return FALSE;

    return g_strcmp0(name, "TMPDIR") == 0 ||
           g_strcmp0(name, "HOME") == 0 ||
           g_str_has_prefix(name, "XDG_");
}

/*
 * The runtime directory a sandbox gives its occupant: the daemon's own
 * spelling, so a socket path written into the agent's config means the
 * same thing inside and out.  The directory itself is never bound in --
 * it holds the daemon's sockets and every agent's -- so with a private
 * home the builder makes an empty one there and binds back only the
 * sockets this agent needs.
 */
static const gchar *
sandbox_runtime_dir(void)
{
    return g_get_user_runtime_dir();
}

GStrv
clawt_sandbox_wrap_environment(ClawtSandbox        *self,
                               const gchar * const *environment)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
    const gchar *home;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(self), NULL);

    for (i = 0; environment != NULL && environment[i] != NULL; i++) {
        const gchar *entry = environment[i];
        const gchar *equals = strchr(entry, '=');
        g_autofree gchar *name = (equals != NULL)
            ? g_strndup(entry, (gsize)(equals - entry)) : g_strdup(entry);

        /*
         * Dropped, not overridden later: getenv() returns the first
         * match, so appending the sandbox's value after the agent's
         * would leave the agent's in charge.  That ordering is exactly
         * how an agent's env: used to beat the allowlist.
         */
        if (self->mode == CLAWT_CONFINE_BWRAP &&
            clawt_sandbox_environment_is_owned(name))
            continue;

        g_ptr_array_add(out, g_strdup(entry));
    }

    if (self->mode == CLAWT_CONFINE_BWRAP) {
        const gchar *runtime = sandbox_runtime_dir();

        home = (self->private_home != NULL) ? self->private_home
                                            : g_get_home_dir();

        /*
         * The box's own /tmp, a tmpfs nothing else can see.  An agent
         * that could set this could aim every temporary file at a host
         * directory the box binds writable -- the exchange, a mount --
         * and have them outlive the box and reach other agents.
         */
        g_ptr_array_add(out, g_strdup("TMPDIR=/tmp"));
        g_ptr_array_add(out, g_strdup_printf("HOME=%s", home));

        /*
         * The daemon's own base directories, never the agent's.  Under a
         * private home they are inside the tmpfs and so empty and
         * private; the CLI's login directory is bound back over its own
         * spelling by whoever built the sandbox.
         */
        g_ptr_array_add(out, g_strdup_printf("XDG_CONFIG_HOME=%s",
                                             g_get_user_config_dir()));
        g_ptr_array_add(out, g_strdup_printf("XDG_DATA_HOME=%s",
                                             g_get_user_data_dir()));
        g_ptr_array_add(out, g_strdup_printf("XDG_CACHE_HOME=%s",
                                             g_get_user_cache_dir()));
        g_ptr_array_add(out, g_strdup_printf("XDG_STATE_HOME=%s",
                                             g_get_user_state_dir()));
        g_ptr_array_add(out, g_strdup_printf("XDG_RUNTIME_DIR=%s",
                                             runtime));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
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

    resolved = sandbox_normalize(self, path);

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

    /*
     * "." and ".." are paths with no separator in them.  Treating them as
     * ordinary words meant `tar -cf loot.tar ..` was never checked at
     * all: every argument was in bounds, and the command still archived
     * the parent of the confined directory.
     */
    if (g_strcmp0(argument, "..") == 0 || g_strcmp0(argument, ".") == 0)
        return TRUE;

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

/*
 * Whether this argument tells a shell that the next one is a command.
 *
 * An exact match on "-c" is not enough: a shell accepts bundled short
 * options, so `bash -lc 'sudo id'` runs the same command and used to
 * sail straight past this check -- with the payload never tokenised, the
 * top-level scan could not see the `sudo` inside it either.
 */
static gboolean
is_shell_command_flag(const gchar *argument)
{
    if (argument == NULL || argument[0] != '-' || argument[1] == '\0')
        return FALSE;

    /* A long option: only "--command" means the same thing. */
    if (argument[1] == '-')
        return g_strcmp0(argument, "--command") == 0;

    return strchr(argument + 1, 'c') != NULL;
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
        g_autofree gchar *resolved = NULL;

        /*
         * The name as written and the name it really resolves to are both
         * checked.  A symlink called "notsudo" pointing at /usr/bin/sudo
         * still executes the setuid binary, so matching only the spelling
         * in argv makes the block trivial to walk around.
         */
        resolved = sandbox_normalize(self, argv[i]);

        if (!basename_matches(argv[i], escalation_commands) &&
            !basename_matches(resolved, escalation_commands))
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

            if (!is_shell_command_flag(argv[i]) || argv[i + 1] == NULL)
                continue;

            if (!g_shell_parse_argv(argv[i + 1], NULL, &inner, NULL)) {
                /*
                 * A command string that cannot be tokenised is refused
                 * rather than skipped.  It used to be waved through, so
                 * anything with a quirk of quoting bypassed the scan
                 * entirely by being unparseable.
                 */
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "that shell command could not be read well "
                            "enough to check: %s", argv[i + 1]);
                return FALSE;
            }

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

/*
 * Whether a denied path is inside anything the sandbox binds.
 *
 * Only those need an operation: bwrap starts from nothing, so a path
 * outside every bind is already unreachable and covering it would mean
 * creating the mount point that makes it exist.
 */
static gboolean
bwrap_path_is_visible(ClawtSandbox *self, const gchar *path)
{
    guint i;

    if (path == NULL || *path == '\0')
        return FALSE;

    /*
     * /usr and /etc are bound read-only for every bwrap sandbox, so a
     * denial naming something under them is a denial of something that
     * is there.
     */
    if (clawt_path_is_within(path, "/usr") ||
        clawt_path_is_within(path, "/etc"))
        return TRUE;

    if (self->root != NULL && clawt_path_is_within(path, self->root))
        return TRUE;

    for (i = 0; i < self->allow_paths->len; i++) {
        if (clawt_path_is_within(path, g_ptr_array_index(self->allow_paths,
                                                         i)))
            return TRUE;
    }

    /*
     * Mounts too, now that they are bound: a denial under one names
     * something the sandbox really does contain, and skipping it as
     * "already absent" left it reachable.
     */
    for (i = 0; i < self->mount_paths->len; i++) {
        if (clawt_path_is_within(path, g_ptr_array_index(self->mount_paths,
                                                         i)))
            return TRUE;
    }

    return FALSE;
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

    /*
     * Name resolution, when the box has a network to resolve on.
     *
     * /etc is bound, but on a systemd-resolved host /etc/resolv.conf is
     * a symlink into /run, which is not -- so a networked box could
     * reach any address and look up none, and an AI CLI inside it
     * reported its provider as unreachable.  Only the one file the link
     * names is bound, read-only, and only when it is outside what is
     * already there; `-try` because a host with no resolver at all is
     * not a reason to refuse the box.
     */
    if (self->allow_network) {
        g_autofree gchar *resolv = clawt_canonicalize_missing(
            "/etc/resolv.conf");

        if (resolv != NULL && !clawt_path_is_within(resolv, "/etc") &&
            !clawt_path_is_within(resolv, "/usr") &&
            g_file_test(resolv, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(out, g_strdup("--ro-bind-try"));
            g_ptr_array_add(out, g_strdup(resolv));
            g_ptr_array_add(out, g_strdup(resolv));
        }
    }

    /*
     * A private home, before any bind, because bwrap applies operations
     * in order and a tmpfs mounted after a bind beneath it would cover
     * the bind: the workspace and the CLI's login directory usually live
     * under $HOME, and are brought back on top of this below.
     *
     * The runtime directory comes with it, empty and 0700, so the
     * sockets bound back into it have somewhere to be and the CLI has a
     * $XDG_RUNTIME_DIR it can write -- see
     * clawt_sandbox_wrap_environment(), which points it there.
     */
    if (self->private_home != NULL) {
        g_ptr_array_add(out, g_strdup("--tmpfs"));
        g_ptr_array_add(out, g_strdup(self->private_home));
        g_ptr_array_add(out, g_strdup("--perms"));
        g_ptr_array_add(out, g_strdup("0700"));
        g_ptr_array_add(out, g_strdup("--dir"));
        g_ptr_array_add(out, g_strdup(sandbox_runtime_dir()));
    }

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

    /*
     * And the mounts, which used to be bound nowhere.
     *
     * clawt_sandbox_path_is_allowed() honours a mount in every mode --
     * "declaring one is an explicit grant" -- and this wall was built
     * without a door for it, so under `confine: bwrap` the argument
     * check passed the path, clawt_computer_describe_mounts() told the
     * agent it was there, and the sandbox did not contain it at all.
     * `computer.exchange` is on by default, so that was every bwrap
     * agent ever handed a file: "No such file or directory", which
     * reads as the exchange being broken rather than as a mount that was
     * never applied.  The identical shape as the deny_paths gap below,
     * one list along.
     *
     * Read-only when the operator wrote `mode: ro`, because a grant
     * bound wider than it was declared is the one direction that costs
     * something.
     */
    for (i = 0; i < self->mount_paths->len; i++) {
        const gchar *path = g_ptr_array_index(self->mount_paths, i);
        gboolean writable = (i < self->mount_writable->len)
            ? g_array_index(self->mount_writable, gboolean, i) : FALSE;

        g_ptr_array_add(out, g_strdup(writable ? "--bind" : "--ro-bind"));
        g_ptr_array_add(out, g_strdup(path));
        g_ptr_array_add(out, g_strdup(path));
    }

    /*
     * The names programs are called by, after the binds that hold their
     * targets.  A link whose directory a bind already brought in is
     * there with it, and bwrap refuses to make a symlink over a file
     * that exists.
     */
    for (i = 0; i + 1 < self->symlinks->len; i += 2) {
        const gchar *link = g_ptr_array_index(self->symlinks, i);
        const gchar *target = g_ptr_array_index(self->symlinks, i + 1);
        g_autofree gchar *parent = g_path_get_dirname(link);

        if (bwrap_path_is_visible(self, parent))
            continue;

        g_ptr_array_add(out, g_strdup("--symlink"));
        g_ptr_array_add(out, g_strdup(target));
        g_ptr_array_add(out, g_strdup(link));
    }

    /*
     * Denials last, because bwrap applies its operations in order and a
     * deny is only ever about something an earlier bind brought in.
     *
     * They used to be emitted nowhere at all.  clawt_sandbox_check_argv()
     * returns early for this mode -- rightly, since scanning arguments
     * for paths is a guess and bwrap is a wall -- and the wall was built
     * without the denials in it, so `deny_paths` was inert in the one
     * mode documented as the strongest.  `allow_paths: [~]` with
     * `deny_paths: [~/.ssh]` bound the whole home directory read-write
     * and put nothing over the key, while clawt_sandbox_describe() told
     * the agent ~/.ssh was off-limits and the schema told the operator
     * the same.  The allowlist mode had honoured denials the whole time,
     * which is what made the gap invisible: the two modes were tested
     * with the same config and only one of them was lying.
     *
     * A directory is covered with an empty tmpfs and a file with
     * /dev/null, because the mount has to be something: bwrap cannot
     * unmount a subtree of a bind.  A denial naming a path that is not
     * in the sandbox at all needs no operation -- it is already absent,
     * which is the whole of what the denial asked for.
     */
    for (i = 0; i < self->deny_paths->len; i++) {
        const gchar *path = g_ptr_array_index(self->deny_paths, i);

        if (!bwrap_path_is_visible(self, path))
            continue;

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_ptr_array_add(out, g_strdup("--tmpfs"));
            g_ptr_array_add(out, g_strdup(path));
        } else {
            g_ptr_array_add(out, g_strdup("--ro-bind"));
            g_ptr_array_add(out, g_strdup("/dev/null"));
            g_ptr_array_add(out, g_strdup(path));
        }
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
            "Commands you run with clawtilla_computer_exec are confined to "
            "%s; one naming a path outside it is refused.",
            self->root != NULL ? self->root : "your workspace");
        break;

    case CLAWT_CONFINE_ALLOWLIST:
        g_string_append_printf(out,
            "Commands you run with clawtilla_computer_exec are confined to "
            "%s", self->root != NULL ? self->root : "your workspace");

        for (i = 0; i < self->allow_paths->len; i++)
            g_string_append_printf(out, ", %s",
                                   (const gchar *)
                                   g_ptr_array_index(self->allow_paths, i));

        g_string_append(out,
            ". One naming a path outside those is refused.");
        break;

    case CLAWT_CONFINE_BWRAP:
        g_string_append_printf(out,
            "Commands you run with clawtilla_computer_exec go through a "
            "bubblewrap sandbox. Writable there: %s",
            self->root != NULL ? self->root : "your workspace");

        for (i = 0; i < self->allow_paths->len; i++)
            g_string_append_printf(out, ", %s",
                                   (const gchar *)
                                   g_ptr_array_index(self->allow_paths, i));

        g_string_append(out, ". The rest of the filesystem is read-only or "
                             "absent inside it.");
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
     * Said out loud, because the header of this file says a confinement
     * mode people believe is stronger than it is, is worse than none --
     * and a remote one is weaker than the local one wearing the same
     * name.  Nothing here can follow a symlink on the other machine, so
     * the check is on the text of the path and stops there.
     */
    if (self->remote && self->mode != CLAWT_CONFINE_NONE) {
        g_string_append(out,
            " That check reads the paths in your command and nothing "
            "else: it is on another machine, so a symlink over there is "
            "not something clawtilla can follow. Write absolute paths -- "
            "\"~\" is your remote account's home and cannot be resolved "
            "from here, so a command naming one is refused.");
    }

    /*
     * Saying so plainly saves an agent three turns discovering it by trial,
     * and stops it reporting a policy refusal as a broken tool.
     */
    if (!self->allow_sudo)
        g_string_append(out, " You cannot use sudo or otherwise escalate "
                             "privilege.");

    /*
     * Every sentence above is about clawtilla_computer_exec, and it now
     * says so -- it used to open "You are running on the host inside a
     * bubblewrap sandbox", which is not true of the process reading it.
     * Unless runtime.confine says otherwise the agent's libreclaw child
     * is spawned unwrapped, so its own Bash, Read and Write reach the
     * whole filesystem as the daemon's user whatever this mode says.
     * That is the architecture (an agent's own tools run on the host;
     * only exec enters the computer), and the architecture being
     * deliberate is exactly why the description had to stop implying
     * otherwise: an agent told it is in a sandbox will believe a refusal
     * it never got, and an operator reading the same words will believe
     * deny_paths is protecting a key it is not.  When the agent *is*
     * confined (clawt_sandbox_set_agent_confined()) the sentence says
     * that instead, because the opposite would now be the lie.
     */
    if (self->mode != CLAWT_CONFINE_NONE && self->agent_confined)
        g_string_append(out,
            " This applies to clawtilla_computer_exec. Your own Bash, "
            "Read and Write tools are confined separately, in a "
            "bubblewrap sandbox of their own that holds your workspace "
            "and what your configuration grants; a path missing there "
            "is missing on purpose.");
    else if (self->mode != CLAWT_CONFINE_NONE)
        g_string_append(out,
            " This applies to clawtilla_computer_exec. Your own Bash, "
            "Read and Write tools run on the host outside it, so treat "
            "those paths as a rule you are asked to keep rather than one "
            "that will stop you.");

    return g_string_free(out, FALSE);
}

static void
clawt_sandbox_finalize(GObject *object)
{
    ClawtSandbox *self = CLAWT_SANDBOX(object);

    g_clear_pointer(&self->root, g_free);
    g_clear_pointer(&self->allow_paths, g_ptr_array_unref);
    g_clear_pointer(&self->mount_paths, g_ptr_array_unref);
    g_clear_pointer(&self->mount_writable, g_array_unref);
    g_clear_pointer(&self->deny_paths, g_ptr_array_unref);
    g_clear_pointer(&self->private_home, g_free);
    g_clear_pointer(&self->symlinks, g_ptr_array_unref);

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
    self->mount_writable = g_array_new(FALSE, FALSE, sizeof(gboolean));
    self->deny_paths = g_ptr_array_new_with_free_func(g_free);
    self->symlinks = g_ptr_array_new_with_free_func(g_free);
    self->allow_network = TRUE;
    self->allow_sudo = FALSE;
}
