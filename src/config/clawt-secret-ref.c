/*
 * clawt-secret-ref.c - A reference to a secret, never the secret itself
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-secret-ref.h"
#include "config/clawt-secret-ref-private.h"

#include <yaml-glib.h>
#include <string.h>
#include <glib-unix.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

struct _ClawtSecretRef {
    ClawtSecretBackend  backend;
    gchar              *locator;
};

G_DEFINE_BOXED_TYPE(ClawtSecretRef, clawt_secret_ref,
                    clawt_secret_ref_copy, clawt_secret_ref_free)

ClawtSecretRef *
clawt_secret_ref_new(ClawtSecretBackend backend, const gchar *locator)
{
    ClawtSecretRef *self;

    g_return_val_if_fail(locator != NULL, NULL);

    self = g_new0(ClawtSecretRef, 1);
    self->backend = backend;
    self->locator = g_strdup(locator);

    return self;
}

ClawtSecretRef *
clawt_secret_ref_copy(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return clawt_secret_ref_new(self->backend, self->locator);
}

void
clawt_secret_ref_free(ClawtSecretRef *self)
{
    if (self == NULL)
        return;

    /*
     * The locator is not itself secret -- it is a path or a variable name --
     * but zeroing costs nothing and keeps the habit for the one place it
     * would matter if this type ever grew a cached value.
     */
    if (self->locator != NULL)
        memset(self->locator, 0, strlen(self->locator));

    g_free(self->locator);
    g_free(self);
}

ClawtSecretBackend
clawt_secret_ref_get_backend(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_SECRET_BACKEND_FILE);

    return self->backend;
}

const gchar *
clawt_secret_ref_get_locator(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->locator;
}

ClawtSecretRef *
clawt_secret_ref_parse(gpointer             spec,
                       ClawtSecretBackend   default_backend,
                       GError             **error)
{
    YamlNode *node = spec;
    YamlMapping *mapping;
    GList *members;
    GList *l;

    g_return_val_if_fail(node != NULL, NULL);

    /*
     * A bare string uses the default backend.  It exists so the common case
     * -- a file in the configured secrets directory -- does not need the
     * mapping form for every credential.
     */
    if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
        const gchar *value = yaml_node_get_string(node);

        if (value == NULL || *value == '\0') {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                                "secret reference is empty");
            return NULL;
        }

        return clawt_secret_ref_new(default_backend, value);
    }

    if (yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                            "secret reference must be a string or a "
                            "one-key mapping such as {env: NAME}");
        return NULL;
    }

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *key = l->data;
        YamlNode *value_node = yaml_mapping_get_member(mapping, key);
        const gchar *value;
        gint backend = -1;

        if (!clawt_enum_from_nick(CLAWT_TYPE_SECRET_BACKEND, key, &backend))
            continue;

        value = yaml_node_get_string(value_node);
        if (value == NULL || *value == '\0') {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "secret reference '%s' has no value", key);
            g_list_free(members);
            return NULL;
        }

        g_list_free(members);
        return clawt_secret_ref_new((ClawtSecretBackend)backend, value);
    }

    g_list_free(members);

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "secret reference names no known backend; "
                        "expected one of file, env or command");
    return NULL;
}

/*
 * Reads a file, trimming exactly one trailing newline.
 *
 * Trimming matters: `echo secret > file` is how everybody creates these, and
 * a token with a newline on the end fails authentication in a way that looks
 * like a wrong token rather than a wrong file.
 */
static gchar *
resolve_file(const gchar *locator, const gchar *base_dir, GError **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;

    if (g_path_is_absolute(locator) || locator[0] == '~')
        path = clawt_expand_path(locator);
    else if (base_dir != NULL)
        path = g_build_filename(base_dir, locator, NULL);
    else
        path = g_strdup(locator);

    if (!g_file_get_contents(path, &contents, &length, error)) {
        g_prefix_error(error, "secret file %s: ", path);
        return NULL;
    }

    while (length > 0 &&
           (contents[length - 1] == '\n' || contents[length - 1] == '\r'))
        length--;

    return g_strndup(contents, length);
}

static gchar *
resolve_env(const gchar *locator, GError **error)
{
    const gchar *value = g_getenv(locator);

    if (value == NULL || *value == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "environment variable %s is not set", locator);
        return NULL;
    }

    return g_strdup(value);
}

/*
 * How long a timed-out command may take to be reaped after its SIGKILL.
 * SIGKILL cannot be caught, but it is acted on only when the process next
 * leaves the kernel: one in uninterruptible sleep -- a hung NFS or FUSE
 * mount, a wedged socket on one -- is not reaped until that wait ends,
 * which may be never.  The deadline exists so that such a command cannot
 * hang startup, so the reap is bounded too.
 */
#define CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS (2)

/*
 * Test only (clawt-secret-ref-private.h): when set, a timed-out command is
 * not killed, so the reap grace runs out exactly as it would for a child
 * the kernel cannot kill yet.
 */
static gint spare_on_timeout = 0;

/*
 * The state of one command resolution.  Everything that can hold a
 * descriptor -- the stdout pipe's source, the child watch's pidfd and the
 * deadline -- is attached to the private context that owns this, so the
 * calling thread releases all of it before resolve_command() returns.
 */
typedef struct {
	GMainContext *context;
	GPid          pid;
	gint          stdout_fd;
	GSource      *stdout_source;
	GSource      *child_source;
	GSource      *grace_source;
	GString      *output;
	gint          wait_status;
	gboolean      exited;
	gboolean      timed_out;
	gboolean      grace_expired;
	gint          read_errno;
} CommandWait;

void
clawt_secret_ref_test_set_spare_on_timeout(gboolean spare)
{
	g_atomic_int_set(&spare_on_timeout, spare ? 1 : 0);
}

/**
 * command_child_setup:
 * @user_data: unused
 *
 * Isolate the command and its children for deadline cleanup. Only
 * async-signal-safe operations are permitted between fork and exec.
 */
static void
command_child_setup(gpointer user_data)
{
	(void)user_data;
	if (setpgid(0, 0) != 0)
		_exit(127);
}

/**
 * command_close_stdout:
 * @wait: the resolution whose stdout pipe is finished with
 *
 * Detach the stdout source and close the read end.  Safe to call twice.
 */
static void
command_close_stdout(CommandWait *wait)
{
	if (wait->stdout_source != NULL) {
		g_source_destroy(wait->stdout_source);
		g_clear_pointer(&wait->stdout_source, g_source_unref);
	}
	if (wait->stdout_fd >= 0) {
		(void)close(wait->stdout_fd);
		wait->stdout_fd = -1;
	}
}

/**
 * on_command_stdout:
 * @fd: the non-blocking read end of the command's stdout
 * @condition: what the poll saw
 * @user_data: the #CommandWait
 *
 * Drain whatever is readable.  End of file closes the pipe here, on the
 * calling thread, so no descriptor outlives the resolution.
 *
 * Returns: %G_SOURCE_CONTINUE while the pipe is open
 */
static gboolean
on_command_stdout(gint fd, GIOCondition condition, gpointer user_data)
{
	CommandWait *wait = user_data;
	gchar buffer[4096];
	gssize n;

	(void)condition;
	for (;;) {
		n = read(fd, buffer, sizeof(buffer));
		if (n > 0) {
			g_string_append_len(wait->output, buffer, n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return G_SOURCE_CONTINUE;
		break;
	}

	/* End of file, or a read error worth reporting. */
	if (n < 0)
		wait->read_errno = errno;
	/* Destroying the dispatching source is safe; the context holds a ref. */
	command_close_stdout(wait);
	return G_SOURCE_REMOVE;
}

/**
 * on_command_exited:
 * @pid: the command's process
 * @wait_status: its wait status
 * @user_data: the #CommandWait
 *
 * The child watch has reaped the command on the calling thread.
 */
static void
on_command_exited(GPid pid, gint wait_status, gpointer user_data)
{
	CommandWait *wait = user_data;

	wait->wait_status = wait_status;
	wait->exited = TRUE;
	g_spawn_close_pid(pid);
}

/**
 * on_command_reap_grace:
 * @user_data: the #CommandWait
 *
 * The killed command has not been reaped within
 * %CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS; stop waiting for it.
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean
on_command_reap_grace(gpointer user_data)
{
	CommandWait *wait = user_data;

	wait->grace_expired = TRUE;
	return G_SOURCE_REMOVE;
}

/**
 * command_kill:
 * @wait: the resolution to end
 *
 * SIGKILL the command's process group and the command itself, close stdout
 * rather than drain it (an escaped descendant may hold it open), and start
 * the reap grace on the private context.
 */
static void
command_kill(CommandWait *wait)
{
	/*
	 * The test hook leaves the command running, so the grace below runs out
	 * as it would for a child stuck in uninterruptible sleep.
	 */
	if (!g_atomic_int_get(&spare_on_timeout)) {
		/* Kill descendants too, including when the child already exited. */
		(void)kill(-wait->pid, SIGKILL);
		/*
		 * And the child itself, in case it left the group.  Until its watch
		 * has reaped it the pid cannot have been reused.
		 */
		if (!wait->exited)
			(void)kill(wait->pid, SIGKILL);
	}
	command_close_stdout(wait);

	if (wait->grace_source == NULL) {
		wait->grace_source = g_timeout_source_new(0);
		g_source_set_ready_time(wait->grace_source, g_get_monotonic_time() +
			(gint64)CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS * G_USEC_PER_SEC);
		g_source_set_callback(wait->grace_source, on_command_reap_grace,
			wait, NULL);
		g_source_attach(wait->grace_source, wait->context);
	}
}

/**
 * on_command_timeout:
 * @user_data: the #CommandWait
 *
 * The deadline passed: kill the command and stop reading.  The direct child
 * is then reaped by its watch before resolve_command() returns -- unless it
 * cannot be reaped within the grace, see resolve_command().
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean
on_command_timeout(gpointer user_data)
{
	CommandWait *wait = user_data;

	wait->timed_out = TRUE;
	command_kill(wait);
	return G_SOURCE_REMOVE;
}

/**
 * on_abandoned_command_exited:
 * @pid: a command resolve_command() stopped waiting for
 * @wait_status: unused
 * @user_data: unused
 *
 * Reap, on the default main context, a command whose reap grace ran out.
 */
static void
on_abandoned_command_exited(GPid pid, gint wait_status, gpointer user_data)
{
	(void)wait_status;
	(void)user_data;
	g_spawn_close_pid(pid);
}

/*
 * Runs a command and takes its stdout.
 *
 * The timeout is the whole point of doing this the long way: `pass show ...`
 * against a locked keyring waits for a passphrase that will never come on a
 * daemon with no terminal, and without a bound it would hang startup rather
 * than failing.
 *
 * It is spawned with g_spawn rather than GSubprocess because GSubprocess
 * reaps on GLib's worker thread: its child-watch pidfd, and the last
 * reference to any task holding this call's private context, are dropped
 * there some time after the call returns.  Here the child watch, the pipe
 * and the deadline are all sources on the private context, so every
 * descriptor the call opened is closed on the calling thread before it
 * returns.
 *
 * One exception.  A command killed at its deadline is normally reaped at
 * once, but a process in uninterruptible sleep is not reaped until that
 * sleep ends.  After %CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS this stops
 * waiting -- the timeout must still bound startup -- and hands the pid to
 * a child watch on the global default context, which reaps it whenever the
 * kernel lets it go.  Until then that watch owns the command's pidfd.
 */
static gchar *
resolve_command(const gchar *locator,
                guint        timeout_seconds,
                GError     **error)
{
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(GSource) timeout_source = NULL;
	g_autoptr(GString) output = NULL;
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) spawn_error = NULL;
	CommandWait wait;
	gsize length;

	if (!g_shell_parse_argv(locator, NULL, &argv, error)) {
		g_prefix_error(error, "secret command is not parseable: ");
		return NULL;
	}

	/*
	 * The same spawn GSubprocess made for us before: stdin from /dev/null,
	 * so a command that prompts reads end of file instead of the daemon's
	 * stdin; stderr silenced; PATH searched; and the pipe's read end
	 * close-on-exec, so no other child inherits it.
	 */
	wait.context = NULL;
	wait.pid = 0;
	wait.stdout_fd = -1;
	if (!g_spawn_async_with_pipes_and_fds(NULL, (const gchar * const *)argv,
			NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
			G_SPAWN_STDERR_TO_DEV_NULL |
			G_SPAWN_CLOEXEC_PIPES, command_child_setup, NULL,
			-1, -1, -1, NULL, NULL, 0,
			&wait.pid, NULL, &wait.stdout_fd, NULL, &spawn_error)) {
		g_propagate_prefixed_error(error, g_steal_pointer(&spawn_error),
			"secret command failed to start: ");
		return NULL;
	}

	output = g_string_new(NULL);
	wait.output = output;
	wait.stdout_source = NULL;
	wait.child_source = NULL;
	wait.grace_source = NULL;
	wait.wait_status = 0;
	wait.exited = FALSE;
	wait.timed_out = FALSE;
	wait.grace_expired = FALSE;
	wait.read_errno = 0;

	/*
	 * A private context, so nothing else the caller has attached runs while
	 * we wait -- the daemon may be inside a callback of the default one.
	 */
	context = g_main_context_new();
	wait.context = context;
	g_main_context_push_thread_default(context);

	if (!g_unix_set_fd_nonblocking(wait.stdout_fd, TRUE, NULL)) {
		/* A blocking read would ignore the deadline; never risk it. */
		wait.read_errno = errno != 0 ? errno : EIO;
		command_kill(&wait);
	} else {
		wait.stdout_source = g_unix_fd_source_new(wait.stdout_fd,
			G_IO_IN | G_IO_HUP | G_IO_ERR);
		g_source_set_callback(wait.stdout_source,
			G_SOURCE_FUNC(on_command_stdout), &wait, NULL);
		g_source_attach(wait.stdout_source, context);
	}

	wait.child_source = g_child_watch_source_new(wait.pid);
	g_source_set_callback(wait.child_source,
		G_SOURCE_FUNC(on_command_exited), &wait, NULL);
	g_source_attach(wait.child_source, context);

	/*
	 * Attached to the context we pushed, not added with
	 * g_timeout_add_seconds().  That helper attaches to the DEFAULT main
	 * context, which this loop is not iterating -- so the timeout would
	 * never fire, and a locked password manager would hang startup exactly
	 * as it would have with no timeout at all.
	 */
	if (timeout_seconds > 0) {
		/* Use an exact monotonic deadline without seconds-source coalescing. */
		timeout_source = g_timeout_source_new(0);
		g_source_set_ready_time(timeout_source,
			g_get_monotonic_time() + (gint64)timeout_seconds * G_USEC_PER_SEC);
		g_source_set_callback(timeout_source, on_command_timeout, &wait, NULL);
		g_source_attach(timeout_source, context);
	}

	/*
	 * Done when the child has been reaped and stdout is closed: at end of
	 * file normally, or by the deadline when a descendant holds it open.
	 * A killed child is reaped here too, unless the reap grace runs out.
	 */
	while ((!wait.exited || wait.stdout_fd >= 0) && !wait.grace_expired)
		g_main_context_iteration(context, TRUE);

	/* Release every source while the context is still ours to iterate. */
	if (timeout_source != NULL)
		g_source_destroy(timeout_source);
	if (wait.grace_source != NULL) {
		g_source_destroy(wait.grace_source);
		g_clear_pointer(&wait.grace_source, g_source_unref);
	}
	command_close_stdout(&wait);
	g_source_destroy(wait.child_source);
	g_clear_pointer(&wait.child_source, g_source_unref);
	g_main_context_pop_thread_default(context);

	/*
	 * Not reaped within the grace: the private watch is gone, so give the
	 * pid to one on the global default context rather than leave a zombie.
	 * The locator names it; the output is never logged.
	 */
	if (!wait.exited) {
		g_child_watch_add(wait.pid, on_abandoned_command_exited, NULL);
		g_warning("secret command (pid %" G_PID_FORMAT ") was not reaped "
		          "%d seconds after SIGKILL; the main loop will reap it "
		          "when it exits: %s", wait.pid,
		          CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS, locator);
	}

	if (wait.timed_out) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
		            "secret command did not finish within %u seconds: %s",
		            timeout_seconds, locator);
		return NULL;
	}

	if (wait.read_errno != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
		            "secret command failed: reading its output: %s: %s",
		            g_strerror(wait.read_errno), locator);
		return NULL;
	}

	if (!g_spawn_check_wait_status(wait.wait_status, NULL)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
		            "secret command exited non-zero: %s", locator);
		return NULL;
	}

	if (output->len == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
		            "secret command produced no output: %s", locator);
		return NULL;
	}

	/* An embedded NUL is invalid here too, so strlen() below is exact. */
	if (!g_utf8_validate_len(output->str, output->len, NULL)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
		            "secret command output is not valid UTF-8: %s", locator);
		return NULL;
	}

	length = output->len;
	while (length > 0 &&
	       (output->str[length - 1] == '\n' || output->str[length - 1] == '\r'))
		length--;

	if (length == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
		            "secret command produced only whitespace: %s", locator);
		return NULL;
	}

	return g_strndup(output->str, length);
}

gchar *
clawt_secret_ref_resolve(ClawtSecretRef  *self,
                         const gchar     *base_dir,
                         guint            command_timeout_seconds,
                         GError         **error)
{
    g_return_val_if_fail(self != NULL, NULL);

    switch (self->backend) {
    case CLAWT_SECRET_BACKEND_FILE:
        return resolve_file(self->locator, base_dir, error);

    case CLAWT_SECRET_BACKEND_ENV:
        return resolve_env(self->locator, error);

    case CLAWT_SECRET_BACKEND_COMMAND:
        return resolve_command(self->locator, command_timeout_seconds, error);

    default:
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                            "unknown secret backend");
        return NULL;
    }
}

gchar *
clawt_secret_ref_describe(ClawtSecretRef *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    /*
     * The locator is included because it is not the secret and it is what
     * makes an error actionable -- "env:ANTHROPIC_API_KEY is not set" tells
     * you what to do; "a secret could not be resolved" does not.
     */
    return g_strdup_printf("%s:%s",
                           clawt_enum_to_nick(CLAWT_TYPE_SECRET_BACKEND,
                                              self->backend),
                           self->locator);
}
