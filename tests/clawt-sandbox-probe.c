/*
 * clawt-sandbox-probe.c - A hostile program run inside a clawtilla box
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Runs INSIDE a bubblewrap box built by clawt-sandbox.c -- the agent-CLI
 * box (`runtime.confine: bwrap`) or the `clawtilla_computer_exec` box
 * (`computer.host.confine: bwrap`) -- and tries what an agent that wanted
 * out would try.  tests/test-sandbox-probe.c lays out the fixture, builds
 * the box with the real builders, runs this in it and reads the report.
 *
 * Usage: clawt-sandbox-probe -c SPEC [--yaml]
 *
 * `-c SPEC` is the spelling the process runtime starts libreclaw with, so
 * the agent box is the runtime's own argv with this binary in libreclaw's
 * place.  SPEC is a GKeyFile, group [probe]; every key is optional and a
 * check whose key is missing is reported as skipped, never as passed.
 *
 * What the probe holds constant, so that no check can pass or fail for a
 * reason that is not the box:
 *
 *   - It never counts its own spec, its own binary or its own argv as a
 *     leak.  The spec names *where* things are and never what they hold:
 *     the secret's variable name is in it, the secret's value is not, so
 *     a probe that read its own spec could not find the value there.
 *   - It starts no listeners.  Every address it dials was opened by the
 *     test, outside the box, on an ephemeral port nothing else serves --
 *     so reaching one means the box shares the host's network, not that
 *     the harness bridged it in.
 *   - It writes nothing outside its own workspace.  A write test opens
 *     for writing and closes again, and removes anything it did create,
 *     so one run cannot change the next one's answer.
 *   - `report`, when the spec names it, is a file in the workspace the
 *     probe writes its own report to; it is never read back.
 *   - A file covered with /dev/null opens and reads empty.  "Read" here
 *     means at least one byte came back, which is what a leak is.
 *
 * Output is TAP by default and YAML with --yaml.  Exit status is 0 when
 * nothing leaked, 1 when something did, 2 when the spec could not be read.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define PROBE_GROUP         ("probe")
#define CONNECT_TIMEOUT_MS  (2000)

typedef enum {
	PROBE_HELD,     /* the box did what it should */
	PROBE_LEAKED,   /* the escape worked */
	PROBE_SKIPPED   /* not applicable to this box, with the reason */
} ProbeOutcome;

typedef struct {
	gchar        *name;
	ProbeOutcome  outcome;
	gchar        *detail;
} ProbeResult;

static void
probe_result_free(
	gpointer data
){
	ProbeResult *result;

	result = (ProbeResult *)data;
	g_free(result->name);
	g_free(result->detail);
	g_free(result);
}

/*
 * Records one check.  @detail is owned by the result from here on.
 */
static void
add_result(
	GPtrArray    *results,
	const gchar  *name,
	ProbeOutcome  outcome,
	gchar        *detail
){
	ProbeResult *result;

	result = g_new0(ProbeResult, 1);
	result->name = g_strdup(name);
	result->outcome = outcome;
	result->detail = detail;
	g_ptr_array_add(results, result);
}

static gchar *
spec_string(
	GKeyFile    *spec,
	const gchar *key
){
	return g_key_file_get_string(spec, PROBE_GROUP, key, NULL);
}

/* ── Reading and writing ─────────────────────────────────────────── */

/*
 * Whether @path gives up at least one byte.  A file covered with
 * /dev/null opens and reads nothing, and that is the deny working, so an
 * empty read is not a leak.  @why is set either way.
 */
static gboolean
reads_something(
	const gchar  *path,
	gchar       **why
){
	gint fd;
	gchar buffer[64];
	gssize got;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);

	if (fd < 0) {
		*why = g_strdup_printf("%s: %s", path, g_strerror(errno));
		return FALSE;
	}

	got = read(fd, buffer, sizeof(buffer));
	close(fd);

	if (got <= 0) {
		*why = g_strdup_printf("%s: opens and reads nothing", path);
		return FALSE;
	}

	*why = g_strdup_printf("%s: read %ld bytes", path, (glong)got);
	return TRUE;
}

/*
 * A read check: held when the file in @key gives up nothing.
 */
static void
check_unreadable(
	GPtrArray   *results,
	GKeyFile    *spec,
	const gchar *name,
	const gchar *key
){
	g_autofree gchar *path = NULL;
	gchar *why;

	why = NULL;
	path = spec_string(spec, key);

	if (path == NULL) {
		add_result(results, name, PROBE_SKIPPED,
		           g_strdup_printf("the spec names no %s", key));
		return;
	}

	if (reads_something(path, &why))
		add_result(results, name, PROBE_LEAKED, why);
	else
		add_result(results, name, PROBE_HELD, why);
}

/*
 * The control: the agent's own workspace has to be writable, or every
 * other check is passing because the box is broken rather than because
 * it is tight.
 */
static void
check_workspace_writable(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *path = NULL;
	gint fd;

	workspace = spec_string(spec, "workspace");

	if (workspace == NULL) {
		add_result(results, "own_workspace_writable", PROBE_SKIPPED,
		           g_strdup("the spec names no workspace"));
		return;
	}

	path = g_strdup_printf("%s/.clawt-probe-%d", workspace, (gint)getpid());
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

	if (fd < 0) {
		/* Reported as a leak so the run fails: the box is unusable. */
		add_result(results, "own_workspace_writable", PROBE_LEAKED,
		           g_strdup_printf("%s: %s -- the box is broken, so no "
		                           "other answer here means anything",
		                           workspace, g_strerror(errno)));
		return;
	}

	close(fd);
	g_unlink(path);
	add_result(results, "own_workspace_writable", PROBE_HELD,
	           g_strdup(workspace));
}

/*
 * A read-only bind: a new name in the directory, or the existing file
 * opened for writing.  Nothing is written and anything created is
 * removed, so the next run starts from the same fixture.
 */
static void
check_ro_bind(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *dir = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *created = NULL;
	gint fd;

	dir = spec_string(spec, "ro_dir");
	file = spec_string(spec, "ro_file");

	if (dir == NULL || file == NULL) {
		add_result(results, "ro_bind_write", PROBE_SKIPPED,
		           g_strdup("the spec names no read-only mount"));
		return;
	}

	created = g_strdup_printf("%s/.clawt-probe-%d", dir, (gint)getpid());
	fd = open(created, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

	if (fd >= 0) {
		close(fd);
		g_unlink(created);
		add_result(results, "ro_bind_write", PROBE_LEAKED,
		           g_strdup_printf("created a file in %s", dir));
		return;
	}

	fd = open(file, O_WRONLY | O_APPEND | O_CLOEXEC);

	if (fd >= 0) {
		close(fd);
		add_result(results, "ro_bind_write", PROBE_LEAKED,
		           g_strdup_printf("opened %s for writing", file));
		return;
	}

	add_result(results, "ro_bind_write", PROBE_HELD,
	           g_strdup_printf("%s: %s", file, g_strerror(errno)));
}

/*
 * A second name for a file the box binds read-only, sitting in the
 * writable workspace.  The workspace name always opens for writing --
 * it is the agent's own file -- so the leak is that it is the same inode
 * the read-only spelling shows.  When clawt_sandbox_check_links() works
 * the box is refused and this never runs at all.
 */
static void
check_hardlink_write(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *second = NULL;
	g_autofree gchar *protected_path = NULL;
	struct stat second_st;
	struct stat protected_st;
	gint fd;

	second = spec_string(spec, "hardlink_write");
	protected_path = spec_string(spec, "hardlink_write_ro");

	if (second == NULL || protected_path == NULL) {
		add_result(results, "hardlink_write", PROBE_SKIPPED,
		           g_strdup("the spec names no hard-linked read-only "
		                    "file"));
		return;
	}

	fd = open(second, O_WRONLY | O_APPEND | O_CLOEXEC);

	if (fd < 0) {
		add_result(results, "hardlink_write", PROBE_HELD,
		           g_strdup_printf("%s: %s", second, g_strerror(errno)));
		return;
	}

	if (fstat(fd, &second_st) != 0 || stat(protected_path, &protected_st) != 0) {
		close(fd);
		add_result(results, "hardlink_write", PROBE_HELD,
		           g_strdup_printf("%s is not visible", protected_path));
		return;
	}

	close(fd);

	if (second_st.st_ino == protected_st.st_ino &&
	    second_st.st_dev == protected_st.st_dev) {
		add_result(results, "hardlink_write", PROBE_LEAKED,
		           g_strdup_printf("%s is writable as %s", protected_path,
		                           second));
		return;
	}

	add_result(results, "hardlink_write", PROBE_HELD,
	           g_strdup("a different file"));
}

/*
 * A second name for a denied file.  The denied spelling is /dev/null in
 * the box; the inode number is in the spec (it is not a secret) so the
 * probe can tell the denied file's contents from any other.
 */
static void
check_hardlink_deny(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *second = NULL;
	gchar *why;
	guint64 inode;
	struct stat st;

	why = NULL;
	second = spec_string(spec, "hardlink_deny");
	inode = g_key_file_get_uint64(spec, PROBE_GROUP, "hardlink_deny_inode",
	                              NULL);

	if (second == NULL || inode == 0) {
		add_result(results, "hardlink_deny", PROBE_SKIPPED,
		           g_strdup("the spec names no hard-linked denied file"));
		return;
	}

	if (stat(second, &st) != 0 || (guint64)st.st_ino != inode) {
		add_result(results, "hardlink_deny", PROBE_HELD,
		           g_strdup_printf("%s is not the denied file", second));
		return;
	}

	if (reads_something(second, &why))
		add_result(results, "hardlink_deny", PROBE_LEAKED, why);
	else
		add_result(results, "hardlink_deny", PROBE_HELD, why);
}

/*
 * Every denied path in the spec's list: held only when none of them
 * gives up a byte.  A directory covered with a tmpfs has no files left;
 * a file covered with /dev/null reads empty.
 */
static void
check_denied(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_auto(GStrv) paths = NULL;
	g_autoptr(GString) seen = NULL;
	gsize i;

	paths = g_key_file_get_string_list(spec, PROBE_GROUP, "denied", NULL,
	                                   NULL);

	if (paths == NULL || paths[0] == NULL) {
		add_result(results, "denied_read", PROBE_SKIPPED,
		           g_strdup("the spec names no denied paths"));
		return;
	}

	seen = g_string_new(NULL);

	for (i = 0; paths[i] != NULL; i++) {
		gchar *why;

		why = NULL;

		if (reads_something(paths[i], &why)) {
			add_result(results, "denied_read", PROBE_LEAKED, why);
			return;
		}

		g_string_append_printf(seen, "%s%s", i > 0 ? "; " : "", why);
		g_free(why);
	}

	add_result(results, "denied_read", PROBE_HELD,
	           g_string_free(g_steal_pointer(&seen), FALSE));
}

/* ── Processes ───────────────────────────────────────────────────── */

/*
 * A process's start, in clock ticks since boot: field 22 of
 * /proc/<pid>/stat, counted after the command name's last ")".  A
 * process inside the box can have the same pid number as one outside;
 * it cannot have the same start as well.
 */
static guint64
process_start(
	gint pid
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) fields = NULL;
	const gchar *after;

	path = g_strdup_printf("/proc/%d/stat", pid);

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;

	after = strrchr(text, ')');

	if (after == NULL || after[1] == '\0')
		return 0;

	/* After ") " the fields start at the third, the state. */
	fields = g_strsplit(after + 2, " ", -1);

	if (g_strv_length(fields) < 20)
		return 0;

	return g_ascii_strtoull(fields[19], NULL, 10);
}

static void
check_host_pids(
	GPtrArray *results,
	GKeyFile  *spec
){
	static const gchar *const roles[] = { "daemon", "canary", NULL };
	gboolean any;
	gsize i;

	any = FALSE;

	for (i = 0; roles[i] != NULL; i++) {
		g_autofree gchar *pid_key = g_strdup_printf("%s_pid", roles[i]);
		g_autofree gchar *start_key = g_strdup_printf("%s_start", roles[i]);
		gint pid;
		guint64 start;

		pid = g_key_file_get_integer(spec, PROBE_GROUP, pid_key, NULL);
		start = g_key_file_get_uint64(spec, PROBE_GROUP, start_key, NULL);

		if (pid <= 0 || start == 0)
			continue;

		any = TRUE;

		if (process_start(pid) == start) {
			add_result(results, "host_pids", PROBE_LEAKED,
			           g_strdup_printf("sees the %s, pid %d", roles[i],
			                           pid));
			return;
		}
	}

	if (!any) {
		add_result(results, "host_pids", PROBE_SKIPPED,
		           g_strdup("the spec names no host process"));
		return;
	}

	add_result(results, "host_pids", PROBE_HELD,
	           g_strdup("neither the daemon nor the canary is in /proc"));
}

/* ── Network ─────────────────────────────────────────────────────── */

/*
 * connect() with a deadline.  Returns 0 on a connection, else an errno.
 * A non-blocking connect that has not finished by the deadline is
 * ETIMEDOUT -- treated as held, since nothing answered.
 */
static gint
connect_with_timeout(
	gint                   fd,
	const struct sockaddr *address,
	socklen_t              length
){
	struct pollfd waiter;
	gint flags;
	gint error;
	socklen_t error_length;

	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (connect(fd, address, length) == 0)
		return 0;

	if (errno != EINPROGRESS)
		return errno;

	waiter.fd = fd;
	waiter.events = POLLOUT;
	waiter.revents = 0;

	if (poll(&waiter, 1, CONNECT_TIMEOUT_MS) <= 0)
		return ETIMEDOUT;

	error = 0;
	error_length = (socklen_t)sizeof(error);

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0)
		return errno;

	return error;
}

/*
 * Dials "address:port" (IPv4).  Returns 0 when it connected, an errno
 * otherwise.
 */
static gint
dial_inet(
	const gchar *target
){
	g_auto(GStrv) parts = NULL;
	struct sockaddr_in address;
	gint fd;
	gint result;

	parts = g_strsplit(target, ":", 2);

	if (g_strv_length(parts) != 2)
		return EINVAL;

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((guint16)g_ascii_strtoull(parts[1], NULL, 10));

	if (inet_pton(AF_INET, parts[0], &address.sin_addr) != 1)
		return EINVAL;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return errno;

	result = connect_with_timeout(fd, (const struct sockaddr *)&address,
	                              (socklen_t)sizeof(address));
	close(fd);

	return result;
}

/* An abstract unix socket: the namespace is the network namespace's. */
static gint
dial_abstract(
	const gchar *name
){
	struct sockaddr_un address;
	gsize length;
	gint fd;
	gint result;

	length = strlen(name);

	if (length + 1 > sizeof(address.sun_path))
		return EINVAL;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path + 1, name, length);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return errno;

	result = connect_with_timeout(fd, (const struct sockaddr *)&address,
	                              (socklen_t)(G_STRUCT_OFFSET(struct sockaddr_un,
	                                                          sun_path) +
	                                          1 + length));
	close(fd);

	return result;
}

/*
 * One reachability check.  In a box that has the network by design --
 * the agent box, until there is an egress gate -- the answer is reported
 * as a skip that says what happened, never as a pass: the check was not
 * one the box claims to make.
 *
 * An abstract unix socket is the exception: the namespace is the
 * network's, but clawt_sandbox_enter() closes it with a Landlock scope
 * whatever the network, on a kernel that has one.  The spec says
 * whether this one does (`abstract_scoped`).
 */
static void
check_reach(
	GPtrArray   *results,
	GKeyFile    *spec,
	const gchar *name,
	const gchar *key,
	gboolean     abstract
){
	g_autofree gchar *target = NULL;
	g_autofree gchar *network = NULL;
	gint error;

	target = spec_string(spec, key);
	network = spec_string(spec, "network");

	if (target == NULL || *target == '\0') {
		add_result(results, name, PROBE_SKIPPED,
		           g_strdup_printf("the spec names no %s target", key));
		return;
	}

	error = abstract ? dial_abstract(target) : dial_inet(target);

	if (g_strcmp0(network, "allowed") == 0 &&
	    !(abstract && g_key_file_get_boolean(spec, PROBE_GROUP,
	                                         "abstract_scoped", NULL))) {
		add_result(results, name, PROBE_SKIPPED,
		           g_strdup_printf("this box has the network by design "
		                           "(%s %s)", target,
		                           error == 0 ? "reached"
		                                      : g_strerror(error)));
		return;
	}

	if (error == 0)
		add_result(results, name, PROBE_LEAKED,
		           g_strdup_printf("connected to %s", target));
	else
		add_result(results, name, PROBE_HELD,
		           g_strdup_printf("%s: %s", target, g_strerror(error)));
}

/* ── Environment and terminal ────────────────────────────────────── */

static gboolean
name_is_owned(
	const gchar *name
){
	return g_strcmp0(name, "TMPDIR") == 0 ||
	       g_strcmp0(name, "HOME") == 0 ||
	       g_str_has_prefix(name, "XDG_");
}

/*
 * TMPDIR, HOME and XDG_* are the box's: none may carry the path the
 * agent's env: tried to point them at, and TMPDIR is /tmp.
 */
static void
check_env_override(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *evil = NULL;
	g_auto(GStrv) environment = NULL;
	gsize i;

	evil = spec_string(spec, "env_evil");

	if (evil == NULL) {
		add_result(results, "env_override", PROBE_SKIPPED,
		           g_strdup("the spec names no overriding path"));
		return;
	}

	environment = g_get_environ();

	for (i = 0; environment[i] != NULL; i++) {
		const gchar *equals = strchr(environment[i], '=');
		g_autofree gchar *name = NULL;

		if (equals == NULL)
			continue;

		name = g_strndup(environment[i], (gsize)(equals - environment[i]));

		/*
		 * Every copy is looked at, not getenv()'s: a duplicate later in
		 * the block is harmless to getenv() and not to a program that
		 * walks environ itself.
		 */
		if (name_is_owned(name) && g_str_has_prefix(equals + 1, evil)) {
			add_result(results, "env_override", PROBE_LEAKED,
			           g_strdup_printf("%s=%s", name, equals + 1));
			return;
		}
	}

	if (g_strcmp0(g_getenv("TMPDIR"), "/tmp") != 0) {
		add_result(results, "env_override", PROBE_LEAKED,
		           g_strdup_printf("TMPDIR=%s", g_getenv("TMPDIR")));
		return;
	}

	add_result(results, "env_override", PROBE_HELD,
	           g_strdup("TMPDIR, HOME and XDG_* are the box's"));
}

/*
 * The daemon's environment stays outside: the secret's name is absent,
 * and nothing is here whose name the allowlist and the agent's own env:
 * did not put there.  Only the *name* is in the spec.
 */
static void
check_env_secret(
	GPtrArray *results,
	GKeyFile  *spec
){
	g_autofree gchar *secret = NULL;
	g_auto(GStrv) allowed = NULL;
	g_auto(GStrv) environment = NULL;
	gsize i;

	secret = spec_string(spec, "env_secret");
	allowed = g_key_file_get_string_list(spec, PROBE_GROUP, "env_allow",
	                                     NULL, NULL);

	if (secret == NULL || allowed == NULL) {
		add_result(results, "env_secret", PROBE_SKIPPED,
		           g_strdup("the spec names no secret"));
		return;
	}

	if (g_getenv(secret) != NULL) {
		add_result(results, "env_secret", PROBE_LEAKED,
		           g_strdup_printf("%s is set", secret));
		return;
	}

	environment = g_get_environ();

	for (i = 0; environment[i] != NULL; i++) {
		const gchar *equals = strchr(environment[i], '=');
		g_autofree gchar *name = NULL;

		if (equals == NULL)
			continue;

		name = g_strndup(environment[i], (gsize)(equals - environment[i]));

		/*
		 * PWD is bwrap's: --chdir sets it to the working directory it
		 * just changed to, which the box chose and the daemon did not
		 * pass.
		 */
		if (name_is_owned(name) || g_strcmp0(name, "PWD") == 0 ||
		    g_strv_contains((const gchar * const *)allowed, name))
			continue;

		add_result(results, "env_secret", PROBE_LEAKED,
		           g_strdup_printf("%s is outside the allowlist", name));
		return;
	}

	add_result(results, "env_secret", PROBE_HELD,
	           g_strdup("only allowlisted names"));
}

/*
 * A controlling terminal.  With one, TIOCSTI (where the kernel still
 * allows it) types commands into the shell the daemon was started from;
 * --new-session is what takes it away.  Only meaningful when the harness
 * gave the box's parent a terminal, which the spec says.
 */
static void
check_tty(
	GPtrArray *results,
	GKeyFile  *spec
){
	gint fd;

	if (!g_key_file_get_boolean(spec, PROBE_GROUP, "tty", NULL)) {
		add_result(results, "controlling_tty", PROBE_SKIPPED,
		           g_strdup("the box was not started from a terminal"));
		return;
	}

	fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);

	if (fd >= 0) {
		close(fd);
		add_result(results, "controlling_tty", PROBE_LEAKED,
		           g_strdup("/dev/tty opens: the box shares the "
		                    "daemon's terminal"));
		return;
	}

	add_result(results, "controlling_tty", PROBE_HELD,
	           g_strdup_printf("/dev/tty: %s", g_strerror(errno)));
}

/* ── Reporting ───────────────────────────────────────────────────── */

static const gchar *
outcome_name(
	ProbeOutcome outcome
){
	switch (outcome) {
	case PROBE_HELD:
		return "held";
	case PROBE_LEAKED:
		return "leaked";
	case PROBE_SKIPPED:
	default:
		return "skipped";
	}
}

/*
 * A YAML double-quoted scalar.  Details carry paths and strerror() text,
 * which can hold a colon or a quote.
 */
static gchar *
yaml_quote(
	const gchar *text
){
	GString *out;
	const gchar *p;

	out = g_string_new("\"");

	for (p = text; p != NULL && *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');

		if (*p == '\n') {
			g_string_append(out, "\\n");
			continue;
		}

		g_string_append_c(out, *p);
	}

	g_string_append_c(out, '"');

	return g_string_free(out, FALSE);
}

static void
print_yaml(
	GString     *out,
	GPtrArray   *results,
	const gchar *box
){
	guint i;
	guint held;
	guint leaked;
	guint skipped;

	held = 0;
	leaked = 0;
	skipped = 0;

	g_string_append_printf(out, "probe: clawt-sandbox-probe\n");
	g_string_append_printf(out, "box: %s\n", box != NULL ? box : "unknown");
	g_string_append_printf(out, "checks:\n");

	for (i = 0; i < results->len; i++) {
		ProbeResult *result = g_ptr_array_index(results, i);
		g_autofree gchar *detail = yaml_quote(result->detail);

		g_string_append_printf(out, "  - name: %s\n", result->name);
		g_string_append_printf(out, "    result: %s\n", outcome_name(result->outcome));
		g_string_append_printf(out, "    detail: %s\n", detail);

		if (result->outcome == PROBE_HELD)
			held++;
		else if (result->outcome == PROBE_LEAKED)
			leaked++;
		else
			skipped++;
	}

	g_string_append_printf(out, "summary:\n");
	g_string_append_printf(out, "  total: %u\n", results->len);
	g_string_append_printf(out, "  held: %u\n", held);
	g_string_append_printf(out, "  leaked: %u\n", leaked);
	g_string_append_printf(out, "  skipped: %u\n", skipped);
}

/*
 * TAP: "ok N - name: detail", "not ok N - name: detail", and a skip as
 * "ok N - name # SKIP detail".  The test reads these lines back.
 */
static void
print_tap(
	GString   *out,
	GPtrArray *results
){
	guint i;

	g_string_append_printf(out, "TAP version 13\n");
	g_string_append_printf(out, "1..%u\n", results->len);

	for (i = 0; i < results->len; i++) {
		ProbeResult *result = g_ptr_array_index(results, i);
		g_autofree gchar *detail = g_strdup(result->detail);

		g_strdelimit(detail, "\n#", ' ');

		if (result->outcome == PROBE_SKIPPED)
			g_string_append_printf(out, "ok %u - %s # SKIP %s\n", i + 1, result->name, detail);
		else
			g_string_append_printf(out, "%s %u - %s: %s\n",
			       result->outcome == PROBE_LEAKED ? "not ok" : "ok",
			       i + 1, result->name, detail);
	}
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GKeyFile) spec = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *spec_path = NULL;
	g_autofree gchar *box = NULL;
	g_autofree gchar *report_path = NULL;
	g_autoptr(GString) report = NULL;
	gboolean yaml;
	gboolean license;
	guint i;
	gint status;
	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, NULL,
		  "The probe spec (a key file, group [probe])", "SPEC" },
		{ "yaml", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Report as YAML instead of TAP", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Print the license and exit", NULL },
		G_OPTION_ENTRY_NULL
	};

	yaml = FALSE;
	license = FALSE;
	entries[0].arg_data = &spec_path;
	entries[1].arg_data = &yaml;
	entries[2].arg_data = &license;

	context = g_option_context_new("- try to escape a clawtilla sandbox");
	g_option_context_add_main_entries(context, entries, NULL);
	g_option_context_set_description(context,
		"Runs inside a box built by clawt-sandbox.c and reports every\n"
		"escape it tried. tests/test-sandbox-probe.c writes the spec.\n"
		"\n"
		"Examples:\n"
		"  clawt-sandbox-probe -c spec.ini\n"
		"  clawt-sandbox-probe -c spec.ini --yaml\n");

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "clawt-sandbox-probe: %s\n", error->message);
		return 2;
	}

	if (license) {
		printf("clawt-sandbox-probe is part of clawtilla, licensed under "
		       "the GNU Affero General Public License v3 or later.\n");
		return 0;
	}

	if (spec_path == NULL) {
		fprintf(stderr, "clawt-sandbox-probe: -c SPEC is required\n");
		return 2;
	}

	spec = g_key_file_new();

	if (!g_key_file_load_from_file(spec, spec_path, G_KEY_FILE_NONE, &error)) {
		fprintf(stderr, "clawt-sandbox-probe: %s: %s\n", spec_path,
		        error->message);
		return 2;
	}

	box = spec_string(spec, "box");
	results = g_ptr_array_new_with_free_func(probe_result_free);

	/* The control first: a broken box makes every other answer moot. */
	check_workspace_writable(results, spec);

	/* Files the box must not show. */
	check_unreadable(results, spec, "daemon_state", "daemon_state");
	check_unreadable(results, spec, "other_workspace", "other_workspace");
	check_unreadable(results, spec, "home_ssh", "home_ssh");
	check_unreadable(results, spec, "home_documents", "home_documents");
	check_unreadable(results, spec, "host_tmp", "host_tmp");
	check_denied(results, spec);

	/* Writes the box must refuse. */
	check_ro_bind(results, spec);
	check_hardlink_write(results, spec);
	check_hardlink_deny(results, spec);

	/* Other processes, other machines, other sockets. */
	check_host_pids(results, spec);
	check_reach(results, spec, "loopback", "loopback", FALSE);
	check_reach(results, spec, "lan", "lan", FALSE);
	check_reach(results, spec, "abstract_unix", "abstract", TRUE);

	/* What the box was started with. */
	check_env_override(results, spec);
	check_env_secret(results, spec);
	check_tty(results, spec);

	report = g_string_new(NULL);

	if (yaml)
		print_yaml(report, results, box);
	else
		print_tap(report, results);

	fputs(report->str, stdout);
	fflush(stdout);

	/*
	 * And to a file, when the spec names one: the process runtime
	 * starts its child with a fixed argv and reads its stdout as a log,
	 * so a test going through clawt_agent_runtime_start() collects the
	 * report from here.  Written whole, so a reader never sees half.
	 */
	report_path = spec_string(spec, "report");

	if (report_path != NULL &&
	    !g_file_set_contents(report_path, report->str, -1, &error)) {
		fprintf(stderr, "clawt-sandbox-probe: %s\n", error->message);
		return 2;
	}

	status = 0;

	for (i = 0; i < results->len; i++) {
		ProbeResult *result = g_ptr_array_index(results, i);

		if (result->outcome == PROBE_LEAKED)
			status = 1;
	}

	return status;
}
